"""Strict, idempotent application of reviewed mAirList 3.1 CSV source rows.

Only unambiguous External IDs can create draft SDBASE catalog identities.
All 20 raw source fields and cue conversion proposals remain attached to the
external identity for review.  No source path is treated as verified media,
and no imported cue or content version is approved automatically.
"""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
from uuid import NAMESPACE_URL, UUID, uuid5

from .identity import Principal, require
from .mairlist31_csv import preview_mairlist31_csv
from .storage import Conflict, Store, StoreError


_KEY = re.compile(r"[A-Za-z0-9_.:-]{1,128}\Z")
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_MAX_APPLY_BYTES = 1024 * 1024
_MAX_APPLY_ROWS = 1000


def _id(value: str, label: str) -> str:
    try:
        return str(UUID(str(value)))
    except (ValueError, TypeError, AttributeError) as exc:
        raise StoreError(f"{label} must be a UUID") from exc


def _stable(*parts: str) -> str:
    return str(uuid5(NAMESPACE_URL, "sdbase:mairlist31:" + json.dumps(parts)))


def _fingerprint(fields: dict) -> str:
    encoded = json.dumps(fields, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _source_namespace(station_id: str, namespace: str) -> str:
    if not isinstance(namespace, str) or not _KEY.fullmatch(namespace):
        raise StoreError("source_namespace must be 1..128 safe characters")
    return f"mairlist31:{station_id}:{namespace}"


def apply_mairlist31_csv(
    store: Store, principal: Principal, station_id: str,
    path: str | Path, *, source_namespace: str,
    expected_source_sha256: str, idempotency_key: str,
    encoding: str = "utf-8-sig",
) -> dict:
    """Atomically create draft catalog rows from an explicitly reviewed source.

    This conservative apply path rejects missing/repeated External IDs, repeated
    source paths, structural errors and changed previously imported identities.
    Such files remain available through the read-only preview for manual
    mapping.  The hash binds this call to the exact CSV bytes the user reviewed.
    """
    station = _id(station_id, "station_id")
    require(store, principal, "import", station)
    if store.get("stations", station) is None:
        raise StoreError("station is absent")
    if not isinstance(expected_source_sha256, str) or \
            not _SHA256.fullmatch(expected_source_sha256):
        raise StoreError("expected_source_sha256 must be a lowercase SHA-256")
    if not isinstance(idempotency_key, str) or not _KEY.fullmatch(idempotency_key):
        raise StoreError("idempotency_key must be 1..128 safe characters")
    namespace = _source_namespace(station, source_namespace)
    preview = preview_mairlist31_csv(path, source_namespace=source_namespace,
                                    encoding=encoding, max_rows=_MAX_APPLY_ROWS)
    if preview["source_sha256"] != expected_source_sha256:
        raise Conflict("CSV bytes changed since preview")
    if preview["source_bytes"] > _MAX_APPLY_BYTES or not preview["entries"]:
        raise StoreError("apply requires 1..1000 rows and at most 1 MiB")
    if preview["invalid_rows"] or preview["ambiguities"]:
        raise Conflict("CSV has invalid rows or ambiguous External IDs/paths; review mapping")
    for entry in preview["entries"]:
        fields = entry["raw_fields"]
        if fields["external_id"] in {"", "-"}:
            raise Conflict("every applied CSV row needs an explicit External ID")
        if fields["title"] in {"", "-"} or not fields["title"].strip():
            raise Conflict("every applied CSV row needs a title for a draft item")

    existing_jobs = store.lookup("import_jobs", "source_key", namespace, idempotency_key)
    if existing_jobs:
        job = existing_jobs[0]
        if job["source_sha256"] != expected_source_sha256 or \
                job.get("scope", {}).get("format") != "mairlist-3.1-csv-20":
            raise Conflict("idempotency key belongs to another CSV source")
        return {"replayed": True, "job_id": job["_id"],
                "source_sha256": expected_source_sha256, **job["summary"]}

    # The manual matches External ID updates through paths.  A path previously
    # seen under a different External ID is a conflict, never a silent merge.
    prior_paths: dict[str, str] = {}
    for mapping in store.scan("external_ids"):
        if mapping.get("source_namespace") != namespace:
            continue
        old_path = (mapping.get("source_snapshot") or {}).get("raw_fields", {}).get("file_path")
        if isinstance(old_path, str) and old_path:
            owner = prior_paths.setdefault(old_path, mapping["source_id"])
            if owner != mapping["source_id"]:
                raise Conflict("existing source path has ambiguous External IDs")
    for entry in preview["entries"]:
        fields = entry["raw_fields"]
        owner = prior_paths.get(fields["file_path"])
        if owner is not None and owner != fields["external_id"]:
            raise Conflict("source path was previously imported with another External ID")

    sequence = store.sequence
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    created = reused = cue_proposals = 0
    for entry in preview["entries"]:
        fields = entry["raw_fields"]
        source_id = fields["external_id"]
        fingerprint = _fingerprint(fields)
        cue_proposals += len(entry["proposals"]["cues"])
        mapped = store.lookup("external_ids", "source", namespace, source_id)
        if mapped:
            mapping = mapped[0]
            if mapping.get("target_collection") != "items" or \
                    mapping.get("source_fingerprint") != fingerprint or \
                    store.get("items", mapping["target_id"]) is None or \
                    store.get("versions", mapping.get("target_version_id", "")) is None or \
                    not store.lookup("station_items", "station_item", station,
                                     mapping["target_id"]):
                raise Conflict("source External ID changed or existing catalog link is invalid")
            tx.expect("external_ids", mapping["_id"], mapping["_rev"])
            reused += 1
            continue
        item_id = _stable("item", namespace, source_id)
        version_id = _stable("version", namespace, source_id)
        link_id = _stable("station_item", station, item_id)
        mapping_id = _stable("external_id", namespace, source_id)
        tx.put("items", item_id, {
            "kind": "other", "title": fields["title"].strip(),
            "lifecycle": "draft", "title_source": "mairlist31_csv",
            "created_at": now, "created_by": principal.user_id,
        })
        tx.put("versions", version_id, {
            "item_id": item_id, "version_no": 1,
            "label": "mAirList 3.1 CSV source", "status": "draft",
            "source_declared_duration_ms": entry["proposals"]["duration_ms"],
            "created_at": now, "created_by": principal.user_id,
        })
        tx.put("station_items", link_id, {
            "station_id": station, "item_id": item_id, "eligible": False,
            "created_at": now, "created_by": principal.user_id,
        })
        tx.put("external_ids", mapping_id, {
            "source_namespace": namespace, "source_id": source_id,
            "target_collection": "items", "target_id": item_id,
            "target_version_id": version_id,
            "source_fingerprint": fingerprint,
            "source_sha256": expected_source_sha256,
            "source_snapshot": {
                "row": entry["row"], "source_line": entry["source_line"],
                "raw_fields": fields, "field_states": entry["field_states"],
                "proposals": entry["proposals"],
                "losses": [loss for loss in preview["losses"]
                           if loss.get("row") == entry["row"]],
                "review_status": "review_required",
            },
            "imported_at": now, "imported_by": principal.user_id,
        })
        created += 1
    job_id = _stable("import_job", namespace, idempotency_key)
    summary = {"created_items": created, "reused_items": reused,
               "cue_proposals_review_required": cue_proposals,
               "loss_count": len(preview["losses"]),
               "all_content_requires_review": True,
               "audio_files_imported": 0, "radio_cues_approved": 0}
    tx.put("import_jobs", job_id, {
        "station_id": station, "source_namespace": namespace,
        "source_sha256": expected_source_sha256,
        "idempotency_key": idempotency_key, "status": "completed",
        "scope": {"format": "mairlist-3.1-csv-20", "source_namespace": source_namespace,
                  "row_count": preview["row_count"], "encoding": encoding},
        "summary": summary, "created_at": now, "created_by": principal.user_id,
    })
    tx.commit()
    return {"replayed": False, "job_id": job_id,
            "source_sha256": expected_source_sha256,
            "losses": preview["losses"], **summary}
