"""Restore a verified portable data package into one new, fenced SDBASE store.

The source package is read only. This is a same-schema data restore, not a
third-party importer or a physical media restore. Source record IDs and
revisions are retained until explicit safety fencing changes live rows.
"""

from __future__ import annotations

from datetime import date, datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
from typing import Any
from uuid import UUID

from .backup import (BackupError, _commit_updates, _data, _destination,
                     _fence_restored_store, _fsync_tree, _issue_provision_admin,
                     _open_regular, _publish_absent, _verify_fenced_store)
from .identity import authenticate
from .portable_export import (PortableExportError, _canonical,
                              TOMBSTONE_FORMAT, verify_portable_export)
from .schema import (KINDS, REFERENCES, REQUIRED, SCHEMA_VERSION, STATUS,
                     open_radio_store)
from .storage import Store, StoreError, _FORMAT as STORE_FORMAT, _atomic_json


DATABASE = "RadioS.sdb"
_PERSISTENT_ENUMS = {
    "plan_revisions": ("status", {"draft", "approved", "superseded"}),
    "media_locations": ("status", {"present", "verified", "missing", "corrupt"}),
    "plan_entries": ("state", {"planned", "skipped", "replaced", "cancelled"}),
    "plan_distributions": ("status", {"pending", "sent", "accepted", "rejected", "expired"}),
    "engine_commands": ("state", {"pending", "accepted", "executed", "rejected", "expired"}),
    "emergency_alert_attempts": ("state", {"started", "completed", "failed", "interrupted"}),
    "live_overrides": ("state", {"pending", "accepted", "rejected", "expired",
                                   "execution_claimed", "failed", "evidenced"}),
}


class PortableImportError(StoreError):
    """A portable package cannot safely become a fresh SDBASE store."""


def _load_state(root: Path, manifest_sha256: str) -> tuple[dict, dict, dict]:
    """Load the exact verified JSONL bytes, guarding package changes after verify."""
    try:
        with os.fdopen(_open_regular(root / "manifest.json"), "rb") as stream:
            manifest_bytes = stream.read()
        if hashlib.sha256(manifest_bytes).hexdigest() != manifest_sha256:
            raise PortableImportError("portable manifest changed during restore")
        manifest = json.loads(manifest_bytes)
        if manifest_bytes != _canonical(manifest) + b"\n":
            raise PortableImportError("portable manifest changed during restore")
        if manifest["source"]["schema_version"] != SCHEMA_VERSION:
            raise PortableImportError("portable package schema differs from this SDBASE version")
        state: dict[str, dict[str, dict]] = {}
        for descriptor in manifest["collections"]:
            name = descriptor["name"]
            expected = descriptor["jsonl"]
            records: dict[str, dict] = {}
            digest = hashlib.sha256()
            size = 0
            with os.fdopen(_open_regular(root / expected["path"]), "rb") as stream:
                for raw in stream:
                    digest.update(raw)
                    size += len(raw)
                    row = json.loads(raw)
                    if raw != _canonical(row) + b"\n" or \
                            not isinstance(row, dict) or set(row) != {"_id", "_rev", "data"} or \
                            not isinstance(row["_id"], str) or not row["_id"] or \
                            type(row["_rev"]) is not int or row["_rev"] < 1 or \
                            not isinstance(row["data"], dict) or row["_id"] in records:
                        raise PortableImportError(f"portable collection changed: {name}")
                    records[row["_id"]] = {"revision": row["_rev"], "data": row["data"]}
            if digest.hexdigest() != expected["sha256"] or \
                    size != expected["size_bytes"] or len(records) != descriptor["rows"]:
                raise PortableImportError(f"portable collection changed: {name}")
            state[name] = records
        with os.fdopen(_open_regular(root / "tombstones.json"), "rb") as stream:
            tombstone_bytes = stream.read()
        expected = manifest["tombstones"]
        if hashlib.sha256(tombstone_bytes).hexdigest() != expected["sha256"] or \
                len(tombstone_bytes) != expected["size_bytes"]:
            raise PortableImportError("portable tombstones changed during restore")
        tombstone_doc = json.loads(tombstone_bytes)
        if tombstone_bytes != _canonical(tombstone_doc) + b"\n" or \
                tombstone_doc.get("format") != TOMBSTONE_FORMAT:
            raise PortableImportError("portable tombstones changed during restore")
        return manifest, state, tombstone_doc["collections"]
    except (BackupError, OSError, UnicodeError, ValueError, TypeError,
            KeyError, AttributeError) as exc:
        raise PortableImportError("portable package changed or cannot be read") from exc


def _validate_loaded_state(state: dict) -> None:
    """Check persistent schema facts without applying write-time transitions.

    Some RadioS validators deliberately require a draft revision when *writing*
    a plan entry. An approved entry remains valid historical data, so replaying
    that validator against a completed snapshot would reject a valid export.
    """
    for collection, records in state.items():
        if collection not in REQUIRED:
            continue  # Preserve separately defined extension collections opaquely.
        for identifier, envelope in records.items():
            data = envelope["data"]
            try:
                if str(UUID(identifier)) != identifier:
                    raise ValueError
            except (TypeError, ValueError, AttributeError) as exc:
                raise PortableImportError(f"{collection} has an invalid record identity") from exc
            if any(field not in data or (data[field] is None and not (
                    collection == "playback_marker_sets" and field in {"segue_in_ms", "segue_out_ms"}))
                   for field in REQUIRED[collection]):
                raise PortableImportError(f"{collection}/{identifier} lacks a required field")
            if collection == "retention_segments":
                raise PortableImportError("cold history requires physical backup restoration")
            if collection == "playback_marker_sets":
                from .playback_markers import validate_marker_set
                validate_marker_set(data, state)
            if collection == "plan_revisions" and data.get("execution_contract_version") == 2:
                from .schema import _validate_execution_revision
                _validate_execution_revision(data, state)
            if "status" in data and collection in STATUS and \
                    (not isinstance(data["status"], str) or
                     data["status"] not in STATUS[collection]):
                raise PortableImportError(f"{collection}/{identifier} has an invalid status")
            enum = _PERSISTENT_ENUMS.get(collection)
            if enum and (not isinstance(data.get(enum[0]), str) or
                         data[enum[0]] not in enum[1]):
                raise PortableImportError(f"{collection}/{identifier} has an invalid {enum[0]}")
            if collection == "items" and \
                    (data.get("kind") not in KINDS or
                     data.get("lifecycle") not in STATUS["items"]):
                raise PortableImportError(f"items/{identifier} has an invalid kind or lifecycle")
            if collection == "plan_entries" and \
                    (data.get("event_kind") not in {"audio", "live", "stream", "macro",
                                                    "wait", "marker", "ad_break", "chain"} or
                     type(data.get("ordinal")) is not int or data["ordinal"] < 0):
                raise PortableImportError(f"plan_entries/{identifier} has an invalid kind or ordinal")
            for field, target in REFERENCES.get(collection, ()):
                value = data.get(field)
                try:
                    target_id = str(UUID(str(value)))
                except (TypeError, ValueError, AttributeError) as exc:
                    raise PortableImportError(
                        f"{collection}/{identifier}.{field} is not a UUID") from exc
                if target_id not in state.get(target, {}):
                    raise PortableImportError(
                        f"{collection}/{identifier}.{field} has no {target} target")

    services: dict[tuple[str, str], dict] = {}
    outputs: set[tuple[str, str, str]] = set()
    for envelope in state.get("station_outputs", {}).values():
        row = envelope["data"]
        endpoint = state["endpoints"][row["endpoint_id"]]["data"]
        if endpoint["station_id"] != row["station_id"]:
            raise PortableImportError("station output and endpoint differ in station")
        outputs.add((row["station_id"], row["endpoint_id"], row["output_id"]))
    for envelope in state.get("broadcast_services", {}).values():
        row = envelope["data"]
        endpoint = state["endpoints"][row["endpoint_id"]]["data"]
        if endpoint["station_id"] != row["station_id"] or \
                (row["station_id"], row["endpoint_id"], row["output_id"]) not in outputs:
            raise PortableImportError("broadcast service endpoint/output scope differs")
        services[(row["station_id"], row["service_key"])] = row

    for envelope in state.get("plans", {}).values():
        row = envelope["data"]
        try:
            date.fromisoformat(row["service_date"])
        except (TypeError, ValueError) as exc:
            raise PortableImportError("plan service date is invalid") from exc
        if row["service_key"] != "default" and \
                (row["station_id"], row["service_key"]) not in services:
            raise PortableImportError("plan broadcast service is absent")
    for envelope in state.get("plan_entries", {}).values():
        row = envelope["data"]
        revision = state["plan_revisions"][row["plan_revision_id"]]["data"]
        plan = state["plans"][revision["plan_id"]]["data"]
        service = services.get((plan["station_id"], plan["service_key"]))
        payload = row.get("payload") or {}
        if not isinstance(payload, dict) or \
                (service and payload.get("output_id") not in (None, service["output_id"])) or \
                (service and row["event_kind"] in {"audio", "live", "stream"} and
                 payload.get("output_id") != service["output_id"]):
            raise PortableImportError("plan entry output differs from its service")
        version_id = payload.get("version_id") if row["event_kind"] == "audio" else None
        if version_id and version_id not in state.get("versions", {}):
            raise PortableImportError("plan audio entry has an absent version")
        try:
            instant = datetime.fromisoformat(row["scheduled_at"].replace("Z", "+00:00"))
            if instant.tzinfo is None or instant.utcoffset() is None:
                raise ValueError
        except (TypeError, ValueError, AttributeError) as exc:
            raise PortableImportError("plan entry schedule is invalid") from exc
    for envelope in state.get("plan_distributions", {}).values():
        row = envelope["data"]
        revision = state["plan_revisions"][row["revision_id"]]["data"]
        plan = state["plans"][revision["plan_id"]]["data"]
        endpoint = state["endpoints"][row["endpoint_id"]]["data"]
        if row["station_id"] != plan["station_id"] or \
                row["station_id"] != endpoint["station_id"]:
            raise PortableImportError("plan distribution scope differs")
    for envelope in state.get("recording_jobs", {}).values():
        row = envelope["data"]
        endpoint = state["endpoints"][row["endpoint_id"]]["data"]
        version = state["versions"][row["destination_version_id"]]["data"]
        associated = any(link["data"]["station_id"] == row["station_id"] and
                         link["data"]["item_id"] == version["item_id"]
                         for link in state.get("station_items", {}).values())
        if endpoint["station_id"] != row["station_id"] or not associated:
            raise PortableImportError("recording job station scope differs")


def _install_checkpoint(stage: Path, state: dict, tombstones: dict) -> Store:
    """Preserve imported IDs/revisions in a fresh store with its own identity."""
    store = Store(stage / DATABASE, schema_version=SCHEMA_VERSION)
    with store._locked(exclusive=True):
        store._reload_locked()
        if store._sequence != 0 or any(store._state.values()):
            raise PortableImportError("restore staging store is not empty")
        payload = {
            "format": STORE_FORMAT,
            # This store has no source _audit rows. Its first real transaction
            # must be sequence 1 so audit archives remain contiguous.
            "sequence": 0,
            "state": state,
            "tombstones": tombstones,
            "collection_sequences": {name: 0 for name in state.keys() | tombstones.keys()},
        }
        _atomic_json(store.checkpoint_file, {
            "payload": payload,
            "sha256": hashlib.sha256(_canonical(payload)).hexdigest(),
        })
        meta = store._meta()
        meta["generation"] += 1
        _atomic_json(store.meta_file, meta)
    return store


def _fence_portable_only(store: Store, admin_user_id: str) -> dict[str, int]:
    """Remove live authority and media availability absent from data packages."""
    now = datetime.now(timezone.utc).isoformat()
    counts: dict[str, int] = {}

    grants = []
    for row in store.scan("plan_distributions"):
        if row.get("offline_grant") is not None:
            changed = _data(row)
            changed.pop("offline_grant", None)
            changed["portable_restore_fenced_at"] = now
            grants.append((row, changed))
    counts["offline_grants_removed"] = _commit_updates(store, "plan_distributions", grants)

    media = []
    for row in store.scan("media_locations"):
        if row.get("status") in {"present", "verified"}:
            changed = _data(row) | {
                "status": "missing", "portable_restore_previous_status": row["status"],
                "portable_restore_fenced_at": now,
            }
            media.append((row, changed))
    counts["media_locations_missing"] = _commit_updates(store, "media_locations", media)

    overrides = []
    for row in store.scan("live_overrides"):
        if row.get("state") in {"pending", "accepted", "execution_claimed"}:
            changed = _data(row) | {
                "state": "expired" if row["state"] == "pending" else "failed",
                "error_code": "portable_restore_fenced",
                "portable_restore_previous_state": row["state"],
                "portable_restore_fenced_at": now,
            }
            overrides.append((row, changed))
    counts["live_overrides_fenced"] = _commit_updates(store, "live_overrides", overrides)

    now_playing = []
    for row in store.scan("now_playing_states"):
        if any(row.get(field) is not None for field in
               ("on_air", "next_intent", "talk_intent",
                "on_air_watermark", "next_watermark", "talk_watermark")):
            changed = _data(row) | {
                "on_air": None, "next_intent": None, "talk_intent": None,
                "on_air_watermark": None, "next_watermark": None,
                "talk_watermark": None, "updated_at": now,
                "portable_restore_fenced_at": now,
            }
            now_playing.append((row, changed))
    counts["now_playing_cleared"] = _commit_updates(store, "now_playing_states", now_playing)

    alerts = []
    for row in store.scan("emergency_alerts"):
        if row.get("status") == "active":
            changed = _data(row) | {
                "status": "cancelled", "cancelled_at": now,
                "cancelled_by": admin_user_id,
                "cancel_reason": "portable_restore_fenced",
                "portable_restore_fenced_at": now,
            }
            alerts.append((row, changed))
    counts["emergency_alerts_cancelled"] = _commit_updates(store, "emergency_alerts", alerts)

    jobs = []
    for row in store.scan("recording_jobs"):
        if row.get("status") == "active":
            changed = _data(row) | {
                "status": "disabled", "status_reason": "portable_restore_fenced",
                "status_changed_at": now, "status_changed_by": admin_user_id,
                "portable_restore_fenced_at": now,
            }
            jobs.append((row, changed))
    counts["recording_jobs_disabled"] = _commit_updates(store, "recording_jobs", jobs)
    return counts


def _verify_portable_fence(store: Store, allowed_token_hash: str) -> None:
    _verify_fenced_store(store, allowed_token_hash=allowed_token_hash)
    if any(row.get("offline_grant") is not None
           for row in store.scan("plan_distributions")):
        raise PortableImportError("offline plan authority remains in restored store")
    if any(row.get("status") in {"present", "verified"}
           for row in store.scan("media_locations")):
        raise PortableImportError("audio without restored bytes is marked available")
    if any(row.get("state") in {"pending", "accepted", "execution_claimed"}
           for row in store.scan("live_overrides")):
        raise PortableImportError("live override remains actionable")
    if any(any(row.get(field) is not None for field in
               ("on_air", "next_intent", "talk_intent"))
           for row in store.scan("now_playing_states")):
        raise PortableImportError("source now-playing state remains live")
    if any(row.get("status") == "active" for row in store.scan("emergency_alerts")):
        raise PortableImportError("source emergency alert remains active")
    if any(row.get("status") == "active" for row in store.scan("recording_jobs")):
        raise PortableImportError("source recording job remains active")


def restore_portable_export(package_dir: str | Path, target_dir: str | Path) -> dict[str, Any]:
    """Restore a same-version package into an absent directory, without audio.

    Returns one fresh, 30-day administrator bearer token. Callers must deliver
    it privately; only its hash is stored. The package/source is never written.
    """
    try:
        target = _destination(target_dir)
        verified = verify_portable_export(package_dir)
        source = Path(package_dir).expanduser().resolve(strict=True)
        if source in target.parents:
            raise PortableImportError("restore target cannot be inside the source package")
        manifest, state, tombstones = _load_state(source, verified["manifest_sha256"])
        try:
            _validate_loaded_state(state)
        except (KeyError, TypeError, ValueError, AttributeError) as exc:
            raise PortableImportError("portable records violate persistent schema") from exc
    except (BackupError, PortableExportError) as exc:
        raise PortableImportError(str(exc)) from exc

    stage = Path(tempfile.mkdtemp(prefix=f".{target.name}.", suffix=".restore",
                                  dir=target.parent))
    os.chmod(stage, 0o700)
    try:
        imported = _install_checkpoint(stage, state, tombstones)
        # Rebuild all RadioS indexes before any fencing commits. This rejects
        # duplicate scoped identities in a forged or incompatible package.
        indexed = open_radio_store(stage / DATABASE)
        if indexed.sequence != 0:
            raise PortableImportError("restored store did not start a new audit sequence")
        if indexed._meta()["store_id"] == manifest["source"]["store_id"]:
            raise PortableImportError("restore reused source store identity")
        if sum(len(indexed.scan(name)) for name in state) != manifest["record_count"]:
            raise PortableImportError("imported record count differs from package")

        counts = _fence_restored_store(imported)
        credential = _issue_provision_admin(imported)
        counts.update(_fence_portable_only(imported, credential["admin_user_id"]))
        final_sequence = imported.checkpoint()
        reopened = open_radio_store(stage / DATABASE)
        if reopened.sequence != final_sequence:
            raise PortableImportError("restored store failed to reopen")
        digest = hashlib.sha256(credential["admin_token"].encode()).hexdigest()
        _verify_portable_fence(reopened, digest)
        if authenticate(reopened, credential["admin_token"]).user_id != \
                credential["admin_user_id"]:
            raise PortableImportError("new administrator credential is unusable")
        restored_store_id = reopened._meta()["store_id"]
        _fsync_tree(stage)
        _publish_absent(stage, target)
        return {
            "path": str(target), "database": DATABASE,
            "store_id": restored_store_id,
            "source_store_id": manifest["source"]["store_id"],
            "source_sequence": manifest["source"]["sequence"],
            "restored_sequence": final_sequence,
            "imported_records": manifest["record_count"],
            "imported_tombstones": manifest["tombstone_count"],
            "audio_bytes_restored": False,
            "excluded_collections": manifest["excluded_collections"],
            **counts, **credential,
        }
    except BackupError as exc:
        raise PortableImportError(str(exc)) from exc
    finally:
        if stage.exists():
            shutil.rmtree(stage)
