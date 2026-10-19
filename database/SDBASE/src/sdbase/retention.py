"""Verified cold history and dependency-safe removal from the hot checkpoint.

Only a global manager can perform maintenance. Ordinary append-only and
non-deletion contracts remain enforced by Transaction. A maintenance commit
publishes one atomic checkpoint after the cold segment has been fsynced and
verified; a crash before publication leaves the original database intact.
"""

from __future__ import annotations

from collections import Counter
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
from typing import Any, Mapping
from uuid import UUID, uuid4

from .backup import (_copy_file, _digest_file, _fsync_tree, _publish_absent,
                     _safe_source)
from .identity import Forbidden, Principal
from .storage import (Conflict, Store, StoreError, _FORMAT, _atomic_json,
                      _index_key, _json_bytes, _unique_key_digest)


FORMAT = "sdbase-retention-v1"
DEFAULT_KEEP_DAYS = {"_audit": 365, "playout_events": 730,
                     "playout_attempts": 730, "output_observations": 180,
                     "listener_samples": 180, "plan_revisions": 730,
                     "plan_entries": 730}
_TIMES = {"_audit": "committed_at", "playout_events": "occurred_at",
          "playout_attempts": "updated_at", "output_observations": "occurred_at",
          "listener_samples": "observed_at", "plan_revisions": "approved_at"}
_MAX_RECORDS = 100_000


def _instant(value: str | datetime) -> datetime:
    try:
        result = datetime.fromisoformat(value.replace("Z", "+00:00")) if isinstance(value, str) else value
        if not isinstance(result, datetime) or result.tzinfo is None or result.utcoffset() is None:
            raise ValueError
        return result.astimezone(timezone.utc)
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError("retention needs an aware timestamp") from exc


def _sha(value: Any) -> str:
    return hashlib.sha256(_json_bytes(value)).hexdigest()


def _digest(value: Any) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def validate_policy(keep_days: Mapping[str, int]) -> None:
    if not isinstance(keep_days, dict) or not keep_days or set(keep_days) - DEFAULT_KEEP_DAYS.keys():
        raise StoreError("unsupported retention collections")
    if any(type(days) is not int or not 1 <= days <= 36500 for days in keep_days.values()):
        raise StoreError("retention keep_days must be 1..36500; omitted collections stay hot")


def validate_segment(data: Mapping[str, Any]) -> None:
    try:
        UUID(data["source_store_id"])
        _instant(data["created_at"])
        if type(data["source_sequence"]) is not int or data["source_sequence"] < 1:
            raise ValueError
        if not _digest(data["policy_sha256"]) or not _digest(data["records_sha256"]):
            raise ValueError
        if not isinstance(data["counts"], dict) or not data["counts"] or \
                set(data["counts"]) - DEFAULT_KEEP_DAYS.keys() or \
                any(type(count) is not int or count < 1 for count in data["counts"].values()):
            raise ValueError
        first, last = data["first_audit_sequence"], data["last_audit_sequence"]
        if type(first) is not int or type(last) is not int or first < 0 or last < first or \
                last > data["source_sequence"] or bool(first) != bool(last) or \
                data["counts"].get("_audit", 0) != (last - first + 1 if first else 0):
            raise ValueError
        files = data["files"]
        if not isinstance(files, list) or len(files) != 2 or \
                {Path(row["path"]).name for row in files} != {"records.jsonl", "manifest.json"}:
            raise ValueError
        for row in files:
            if set(row) != {"path", "sha256", "size_bytes"} or not _digest(row["sha256"]) or \
                    type(row["size_bytes"]) is not int or row["size_bytes"] < 1:
                raise ValueError
    except (ValueError, TypeError, KeyError, AttributeError) as exc:
        raise StoreError("invalid retention segment") from exc


def _manage(store: Store, principal: Principal) -> None:
    if principal.scope != "user":
        raise Forbidden("retention requires a global user manager")
    user = store._state.get("users", {}).get(principal.user_id)
    if user is None or user["data"].get("status") != "active":
        raise Forbidden("retention manager is inactive")
    stations = list(store._state.get("stations", {}))
    if not stations:
        raise StoreError("retention requires a managed station")
    for station in stations:
        if not any(not row["data"].get("revoked_at") and row["data"].get("user_id") == principal.user_id and
                   row["data"].get("permission") == "manage" and
                   row["data"].get("station_id") in {None, station}
                   for row in store._state.get("permissions", {}).values()):
            raise Forbidden("retention requires manage permission in every station")


def put_policy(store: Store, principal: Principal, keep_days: dict, *,
               expected_revision: int | None = None) -> dict:
    validate_policy(keep_days)
    sequence = store.sequence
    _manage(store, principal)
    rows = store.lookup("retention_policies", "code", "operational")
    row = rows[0] if rows else None
    if (row["_rev"] if row else None) != expected_revision:
        raise Conflict("retention policy changed")
    identifier = row["_id"] if row else str(uuid4())
    body = {"code": "operational", "keep_days": dict(keep_days),
            "created_by": row["created_by"] if row else principal.user_id,
            "updated_at": datetime.now(timezone.utc).isoformat()}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("retention_policies", identifier, body, expected_revision=expected_revision)
    tx.commit()
    return store.get("retention_policies", identifier)


def get_policy(store: Store, principal: Principal) -> dict:
    store.refresh()
    _manage(store, principal)
    rows = store.lookup("retention_policies", "code", "operational")
    return rows[0] if rows else {"code": "operational", "keep_days": DEFAULT_KEEP_DAYS.copy(),
                              "persisted": False}


def _values(value: Any):
    if isinstance(value, str):
        yield value
        try:
            canonical = str(UUID(value))
        except (ValueError, AttributeError):
            canonical = value
        if canonical != value:
            yield canonical
    elif isinstance(value, Mapping):
        # Several domain dictionaries use entry UUIDs as their keys.
        for key, child in value.items():
            yield from _values(key)
            yield from _values(child)
    elif isinstance(value, list):
        for child in value:
            yield from _values(child)


def archive_file_refs(state: Mapping, database: str) -> list[dict]:
    """Derive the physical cold-file inventory from committed segment rows."""
    result = []
    for identifier, row in state.get("retention_segments", {}).items():
        data = row["data"]
        validate_segment(data)
        for entry in data["files"]:
            expected = f"{database}.archives/{identifier}/{Path(entry['path']).name}"
            if entry["path"] != expected:
                raise StoreError("retention file is outside its managed segment")
            result.append(dict(entry))
    return sorted(result, key=lambda row: row["path"])


def _select(store: Store, keep_days: dict, observed: datetime, max_records: int):
    selected: dict[tuple[str, str], dict] = {}
    candidates = []
    for collection, days in keep_days.items():
        if collection in {"_audit", "plan_entries"}:
            continue
        cutoff = observed - timedelta(days=days)
        for key, row in store._state.get(collection, {}).items():
            body = row["data"]
            if collection == "plan_revisions" and body.get("status") != "superseded":
                continue
            if collection == "playout_attempts" and body.get("state") not in {
                    "completed", "failed", "skipped", "cancelled"}:
                continue
            value = body.get(_TIMES[collection])
            if value is None:
                continue
            stamp = _instant(value)
            if stamp < cutoff:
                candidates.append((stamp, collection, key, row))
    for _stamp, collection, key, row in sorted(candidates)[:max_records]:
        selected[(collection, key)] = row
    if "plan_entries" in keep_days:
        cutoff = observed - timedelta(days=keep_days["plan_entries"])
        for key, row in store._state.get("plan_entries", {}).items():
            revision = row["data"]["plan_revision_id"]
            if ("plan_revisions", revision) in selected and \
                    _instant(row["data"]["scheduled_at"]) < cutoff and len(selected) < max_records:
                selected[("plan_entries", key)] = row
    # A retained hot reference pins its target; pinning propagates recursively.
    # Audit before/after bodies and segment manifests are historical evidence,
    # not live foreign keys. Everything else (including nested report links)
    # participates, conservatively, to avoid destroying working references.
    pinned = set()
    while True:
        by_id = {}
        for identity in selected:
            by_id.setdefault(identity[1], set()).add(identity)
        newly = set()
        for collection, rows in store._state.items():
            if collection in {"_audit", "retention_segments"}:
                continue
            for key, row in rows.items():
                if (collection, key) in selected:
                    continue
                for value in _values(row["data"]):
                    newly.update(by_id.get(value, ()))
        if not newly:
            break
        pinned.update(newly)
        for identity in newly:
            selected.pop(identity)
    first = last = 0
    if "_audit" in keep_days:
        cutoff = observed - timedelta(days=keep_days["_audit"])
        previous = max((row["data"]["last_audit_sequence"] for row in
                        store._state.get("retention_segments", {}).values()), default=0)
        audit = sorted((row["data"]["sequence"], key, row) for key, row in
                       store._state.get("_audit", {}).items())
        for number, key, row in audit:
            if number != (last or previous) + 1:
                raise StoreError("hot audit has a sequence gap before retention")
            if _instant(row["data"]["committed_at"]) >= cutoff or len(selected) >= max_records:
                break
            selected[("_audit", key)] = row
            first = first or number
            last = number
    return selected, pinned, first, last


def _preview_locked(store: Store, keep_days: dict, observed: datetime, max_records: int):
    validate_policy(keep_days)
    if type(max_records) is not int or not 1 <= max_records <= _MAX_RECORDS:
        raise StoreError("retention max_records must be 1..100000")
    selected, pinned, first, last = _select(store, keep_days, observed, max_records)
    identities = [(collection, key, row["revision"]) for (collection, key), row in sorted(selected.items())]
    result = {"source_store_id": store._meta()["store_id"], "sequence": store._sequence,
              "as_of": observed.isoformat(), "keep_days": keep_days, "max_records": max_records,
              "counts": dict(Counter(collection for collection, _key in selected)),
              "pinned_counts": dict(Counter(collection for collection, _key in pinned)),
              "selected_records": len(selected), "first_audit_sequence": first,
              "last_audit_sequence": last, "selection_sha256": _sha(identities)}
    result["preview_sha256"] = _sha(result)
    return result, selected


def preview_retention(store: Store, principal: Principal, *, keep_days: dict | None = None,
                      as_of: str | datetime | None = None, max_records: int = 10000) -> dict:
    observed = _instant(as_of or datetime.now(timezone.utc))
    with store._locked(exclusive=False):
        store._reload_locked()
        _manage(store, principal)
        policy = keep_days if keep_days is not None else get_policy(store, principal)["keep_days"]
        result, _selected = _preview_locked(store, dict(policy), observed, max_records)
        return result


def _verify_segment(root: Path, data: Mapping, *, expected_records=None):
    validate_segment(data)
    for entry in data["files"]:
        path = _safe_source(root, entry["path"])
        digest, size = _digest_file(path)
        if digest != entry["sha256"] or size != entry["size_bytes"]:
            raise StoreError("retention archive checksum or size differs")
    record_ref = next(row for row in data["files"] if row["path"].endswith("/records.jsonl"))
    manifest_ref = next(row for row in data["files"] if row["path"].endswith("/manifest.json"))
    manifest = json.loads(_safe_source(root, manifest_ref["path"]).read_bytes())
    expected_manifest = {key: data[key] for key in data if key != "files"} | {"format": FORMAT}
    if manifest != expected_manifest or record_ref["sha256"] != data["records_sha256"]:
        raise StoreError("retention manifest differs from committed registry")
    counts = Counter()
    audit_numbers = []
    seen = set()
    with _safe_source(root, record_ref["path"]).open("rb") as stream:
        for line in stream:
            if len(line) > 32 * 1024 * 1024:
                raise StoreError("retention record exceeds bound")
            record = json.loads(line)
            if set(record) != {"collection", "key", "envelope"}:
                raise StoreError("invalid retention record fields")
            identity = (record["collection"], record["key"])
            row = record["envelope"]
            if identity in seen or identity[0] not in DEFAULT_KEEP_DAYS or \
                    not isinstance(row, dict) or set(row) != {"revision", "data"} or \
                    type(row["revision"]) is not int or row["revision"] < 1:
                raise StoreError("invalid or duplicate retention record")
            seen.add(identity)
            counts[identity[0]] += 1
            if identity[0] == "_audit":
                audit_numbers.append(row["data"]["sequence"])
            if expected_records is not None and expected_records.get(identity) != row:
                raise StoreError("retention record differs from hot source")
    if dict(counts) != data["counts"] or expected_records is not None and seen != set(expected_records):
        raise StoreError("retention archive record inventory differs")
    first, last = data["first_audit_sequence"], data["last_audit_sequence"]
    expected_audit = list(range(first, last + 1)) if first else []
    if sorted(audit_numbers) != expected_audit:
        raise StoreError("retention archive audit range differs")
    return record_ref


def apply_retention(store: Store, principal: Principal, preview: dict) -> dict:
    """Require an exact preview; publish cold files before the atomic hot commit."""
    if store.read_only:
        raise StoreError("read-only database cannot apply retention")
    required = {"source_store_id", "sequence", "as_of", "keep_days", "max_records",
                "counts", "pinned_counts", "selected_records", "first_audit_sequence",
                "last_audit_sequence", "selection_sha256", "preview_sha256"}
    if not isinstance(preview, dict) or set(preview) != required:
        raise StoreError("retention apply requires the exact preview object")
    with store._locked(exclusive=True):
        store._assert_writable_locked()
        store._reload_locked(repair_tail=True)
        from .wal_archive import archive_pending_locked
        archive_pending_locked(store)
        _manage(store, principal)
        fresh, selected = _preview_locked(store, dict(preview["keep_days"]),
                                          _instant(preview["as_of"]), preview["max_records"])
        if fresh != preview:
            raise Conflict("database or retention selection changed since preview")
        if not selected:
            return {"purged_records": 0, "sequence": store._sequence, "archive": None}
        identifier = str(uuid4())
        archive_root = store.root / (store.journal.name + ".archives")
        if archive_root.is_symlink():
            raise StoreError("retention archive root must not be a symlink")
        archive_root.mkdir(mode=0o700, exist_ok=True)
        target = archive_root / identifier
        stage = Path(tempfile.mkdtemp(prefix=".retention-", dir=archive_root))
        published = False
        try:
            records = stage / "records.jsonl"
            with records.open("xb") as stream:
                os.chmod(records, 0o600)
                for (collection, key), row in sorted(selected.items()):
                    stream.write(_json_bytes({"collection": collection, "key": key, "envelope": row}) + b"\n")
                stream.flush()
                os.fsync(stream.fileno())
            digest, size = _digest_file(records)
            data = {"source_store_id": fresh["source_store_id"], "source_sequence": fresh["sequence"],
                    "created_at": datetime.now(timezone.utc).isoformat(), "counts": fresh["counts"],
                    "first_audit_sequence": fresh["first_audit_sequence"],
                    "last_audit_sequence": fresh["last_audit_sequence"],
                    "policy_sha256": _sha(fresh["keep_days"]), "records_sha256": digest}
            _atomic_json(stage / "manifest.json", data | {"format": FORMAT})
            manifest_digest, manifest_size = _digest_file(stage / "manifest.json")
            prefix = f"{store.journal.name}.archives/{identifier}"
            data["files"] = [{"path": prefix + "/records.jsonl", "sha256": digest, "size_bytes": size},
                             {"path": prefix + "/manifest.json", "sha256": manifest_digest,
                              "size_bytes": manifest_size}]
            _fsync_tree(stage)
            _publish_absent(stage, target)
            published = True
            _verify_segment(store.root, data, expected_records=selected)
            _commit_compaction(store, selected, identifier, data, principal.user_id)
            return {"purged_records": len(selected), "counts": fresh["counts"],
                    "pinned_counts": fresh["pinned_counts"], "sequence": store._sequence,
                    "segment_id": identifier, "archive": str(target),
                    "records_sha256": digest, "cold_bytes": size + manifest_size}
        finally:
            if stage.exists():
                shutil.rmtree(stage)
            # An unreferenced published segment is a safe orphan after a crash.
            # Never delete it after checkpoint publication could have succeeded.
            if published and identifier not in store._state.get("retention_segments", {}):
                pass


def _commit_compaction(store: Store, selected: dict, identifier: str, segment: dict, actor: str):
    """Private maintenance path; called under the exclusive lock after verification."""
    state = {name: dict(rows) for name, rows in store._state.items()}
    tombstones = {name: set(ids) for name, ids in store._tombstones.items()}
    retired = {collection: {name: set(values) for name, values in indexes.items()}
               for collection, indexes in store._retired_unique_keys.items()}
    generations = dict(store._collection_sequences)
    sequence = store._sequence + 1
    for (collection, key), expected in selected.items():
        if state[collection].get(key) != expected:
            raise Conflict("retention source changed")
        del state[collection][key]
        tombstones.setdefault(collection, set()).add(key)
        for name, spec in store.index_specs.get(collection, {}).items():
            value = _index_key(expected["data"], spec)
            if spec.unique and value is not None:
                retired.setdefault(collection, {}).setdefault(name, set()).add(_unique_key_digest(value))
        generations[collection] = sequence
    state.setdefault("retention_segments", {})[identifier] = {"revision": 1, "data": segment}
    audit_id = str(uuid4())
    state.setdefault("_audit", {})[audit_id] = {"revision": 1, "data": {
        "sequence": sequence, "actor": actor, "committed_at": segment["created_at"],
        "changes": [{"collection": "retention_segments", "key": identifier,
                     "before_revision": None, "before": None, "after_revision": 1, "after": segment}]}}
    generations["_audit"] = generations["retention_segments"] = sequence
    payload = {"format": _FORMAT, "sequence": sequence, "state": state,
               "tombstones": {name: sorted(ids) for name, ids in tombstones.items()},
               "retired_unique_keys": {collection: {name: sorted(values) for name, values in indexes.items()}
                                       for collection, indexes in retired.items()},
               "collection_sequences": generations}
    # Replacing the checkpoint is the commit point. Even if truncation or the
    # generation write fails, replay ignores journal frames already included.
    # _reload_locked also detects checkpoint replacement, for existing readers.
    _atomic_json(store.checkpoint_file, {"payload": payload, "sha256": _sha(payload)})
    store._checkpoint_stamp = None
    store._reload_locked()
    from .wal_archive import archive_checkpoint_locked
    archive_checkpoint_locked(store, payload, segment["created_at"])
    with store.journal.open("r+b") as stream:
        stream.truncate(0)
        stream.flush()
        os.fsync(stream.fileno())
    meta = store._meta()
    meta["generation"] += 1
    _atomic_json(store.meta_file, meta)
    store._generation = meta["generation"]
    store._offset = 0


def query_cold_history(store: Store, principal: Principal, collection: str, *,
                       record_id: str | None = None, segment_id: str | None = None,
                       limit: int = 100, offset: int = 0) -> dict:
    if collection not in DEFAULT_KEEP_DAYS or type(limit) is not int or not 1 <= limit <= 1000 or \
            type(offset) is not int or offset < 0:
        raise StoreError("invalid cold history query bounds")
    sequence = store.sequence
    _manage(store, principal)
    rows = sorted(store.scan("retention_segments"), key=lambda row: (row["created_at"], row["_id"]))
    if segment_id:
        rows = [row for row in rows if row["_id"] == str(UUID(segment_id))]
        if not rows:
            raise StoreError("cold segment is absent")
    results = []
    seen = 0
    for row in rows:
        data = {key: value for key, value in row.items() if key not in {"_id", "_rev"}}
        record_ref = _verify_segment(store.root, data)
        with _safe_source(store.root, record_ref["path"]).open("rb") as stream:
            for line in stream:
                record = json.loads(line)
                if record["collection"] != collection or record_id and record["key"] != record_id:
                    continue
                if seen >= offset and len(results) < limit:
                    envelope = record["envelope"]
                    results.append(dict(envelope["data"]) | {"_id": record["key"],
                        "_rev": envelope["revision"], "_segment_id": row["_id"]})
                seen += 1
    if store.sequence != sequence:
        raise Conflict("database changed during cold history query")
    return {"collection": collection, "sequence": sequence, "records": results,
            "matched": seen, "next_offset": offset + len(results) if offset + len(results) < seen else None}


def verify_cold_history(store: Store) -> dict:
    """Administrative integrity probe; checks each registered immutable file."""
    rows = store.scan("retention_segments")
    ranges = []
    for row in rows:
        data = {key: value for key, value in row.items() if key not in {"_id", "_rev"}}
        _verify_segment(store.root, data)
        if data["first_audit_sequence"]:
            ranges.append((data["first_audit_sequence"], data["last_audit_sequence"]))
    through = 0
    for first, last in sorted(ranges):
        if first != through + 1:
            raise StoreError("cold audit segments have a sequence gap or overlap")
        through = last
    return {"segments": len(rows), "audit_archived_through": through,
            "files": len(archive_file_refs(store._state, store.journal.name))}
