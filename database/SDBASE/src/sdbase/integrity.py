"""Read-only, budgeted scrub of durable SDBASE records and managed objects.

This reads original files under a shared source lock and reuses the engine's
in-memory replay/index routines. It never opens the journal for writing,
repairs a tail, creates a lock, checkpoints, or changes stored metadata.
"""

from __future__ import annotations

from collections import Counter
from datetime import date, datetime
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
from uuid import UUID

from . import _platform
from .backup import _open_regular, _safe_source
from .schema import INDEXES, KINDS, REFERENCES, REQUIRED, SCHEMA_VERSION, STATUS
from .storage import Corruption, Store, StoreError, _FORMAT, _MAX_FRAME, _index_key, _json_bytes, _unique_key_digest


class _Incomplete(Exception):
    pass


class _Work:
    def __init__(self, max_bytes, max_records):
        self.max_bytes = max_bytes
        self.max_records = max_records
        self.bytes = 0
        self.records = 0
        self.hot_records = 0
        self.cold_records = 0
        self.files = 0
        self.frames = 0

    def reserve(self, size):
        if size > self.max_bytes - self.bytes:
            raise _Incomplete("byte budget exhausted before reading the next complete file")

    def read(self, stream, size):
        available = max(0, os.fstat(stream.fileno()).st_size - stream.tell())
        if min(size, available) > self.max_bytes - self.bytes:
            raise _Incomplete("byte budget exhausted")
        data = stream.read(size)
        self.bytes += len(data)
        return data

    def record(self, *, cold=False):
        if self.records >= self.max_records:
            raise _Incomplete("record validation budget exhausted")
        self.records += 1
        if cold:
            self.cold_records += 1
        else:
            self.hot_records += 1


def _whole(path, work):
    fd = _open_regular(path)
    with os.fdopen(fd, "rb") as stream:
        size = os.fstat(stream.fileno()).st_size
        work.reserve(size)
        data = work.read(stream, size)
        if len(data) != size or os.fstat(stream.fileno()).st_size != size:
            raise Corruption("file size changed during scrub")
    work.files += 1
    return data


def _uuid(value):
    return str(UUID(str(value)))


def _load(database, work):
    """Checksummed, non-repairing replay into an independent private state."""
    root = database.parent
    lock_path = database.with_name(database.name + ".lock")
    lock_fd = _open_regular(_safe_source(root, lock_path.name))
    try:
        try:
            _platform.lock(lock_fd, exclusive=False, timeout=30)
        except TimeoutError:
            raise _Incomplete("source lock wait exceeded 30 seconds") from None
        meta_path = _safe_source(root, database.name + ".meta")
        checkpoint_path = _safe_source(root, database.name + ".checkpoint")
        journal_path = _safe_source(root, database.name)
        paths = [meta_path, journal_path] + ([checkpoint_path] if checkpoint_path.exists() else [])
        for path in paths:
            info = path.stat(follow_symlinks=False)
            if not stat.S_ISREG(info.st_mode):
                raise Corruption("database component is not a regular file")
        work.reserve(sum(path.stat().st_size for path in paths))
        meta = json.loads(_whole(meta_path, work))
        if not isinstance(meta, dict) or meta.get("format") != _FORMAT or \
                type(meta.get("generation")) is not int or meta["generation"] < 0 or \
                type(meta.get("schema_version", 1)) is not int or meta.get("schema_version", 1) != SCHEMA_VERSION:
            raise Corruption("invalid or unsupported database metadata")
        if _uuid(meta["store_id"]) != meta["store_id"]:
            raise Corruption("invalid store identity")
        if SCHEMA_VERSION >= 4:
            if meta.get("node_role") not in {"primary", "standby", "fenced"} or \
                    type(meta.get("epoch")) is not int or meta["epoch"] < 1:
                raise Corruption("invalid metadata role/epoch")
            for field in ("node_id", "timeline_id"):
                if _uuid(meta[field]) != meta[field]:
                    raise Corruption("invalid metadata node/timeline identity")
        replay = object.__new__(Store)
        replay.index_specs = INDEXES
        replay._indices = {}
        replay._state = {}
        replay._tombstones = {}
        replay._retired_unique_keys = {}
        replay._collection_sequences = {}
        replay._sequence = 0
        if checkpoint_path.exists():
            envelope = json.loads(_whole(checkpoint_path, work))
            payload = envelope["payload"]
            if hashlib.sha256(_json_bytes(payload)).hexdigest() != envelope["sha256"]:
                raise Corruption("checkpoint checksum mismatch")
            if payload.get("format") != _FORMAT or type(payload.get("sequence")) is not int or payload["sequence"] < 0:
                raise Corruption("invalid checkpoint format or sequence")
            if not isinstance(payload.get("state"), dict) or not isinstance(payload.get("tombstones"), dict):
                raise Corruption("checkpoint state/tombstones must be record maps")
            replay._sequence = payload["sequence"]
            replay._state = payload["state"]
            for collection, keys in payload["tombstones"].items():
                if not isinstance(collection, str) or not isinstance(keys, list) or \
                        any(not isinstance(key, str) or not key for key in keys) or len(set(keys)) != len(keys):
                    raise Corruption("invalid checkpoint tombstones")
                replay._tombstones[collection] = set(keys)
            generations = payload.get("collection_sequences", {name: replay._sequence for name in
                                                                  replay._state.keys() | replay._tombstones.keys()})
            if not isinstance(generations, dict) or any(not isinstance(name, str) or not name or
                    type(value) is not int or not 0 <= value <= replay._sequence for name, value in generations.items()):
                raise Corruption("invalid checkpoint collection generations")
            replay._collection_sequences = dict(generations)
            retired = payload.get("retired_unique_keys", {})
            if not isinstance(retired, dict):
                raise Corruption("invalid checkpoint retired unique identities")
            for collection, indexes in retired.items():
                if not isinstance(collection, str) or not isinstance(indexes, dict):
                    raise Corruption("invalid retired unique index map")
                replay._retired_unique_keys[collection] = {}
                for name, values in indexes.items():
                    if not isinstance(name, str) or not isinstance(values, list) or any(
                            not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdef" for c in value)
                            for value in values):
                        raise Corruption("invalid retired unique digests")
                    replay._retired_unique_keys[collection][name] = set(values)
            replay._rebuild_indexes()
        fd = _open_regular(journal_path)
        with os.fdopen(fd, "rb") as stream:
            while True:
                header = work.read(stream, 4)
                if not header:
                    break
                if len(header) != 4:
                    raise Corruption("incomplete journal tail")
                length = struct.unpack(">I", header)[0]
                if not 1 <= length <= _MAX_FRAME:
                    raise Corruption("invalid journal frame length")
                data = work.read(stream, length)
                digest = work.read(stream, 32)
                if len(data) != length or len(digest) != 32:
                    raise Corruption("incomplete journal tail")
                if hashlib.sha256(data).digest() != digest:
                    raise Corruption("journal checksum mismatch")
                txn = json.loads(data)
                if not isinstance(txn, dict) or txn.get("format") != _FORMAT or type(txn.get("sequence")) is not int:
                    raise Corruption("invalid journal transaction envelope")
                work.frames += 1
                if txn["sequence"] > replay._sequence:
                    replay._apply(txn)
        work.files += 1
        return replay, meta
    finally:
        _platform.unlock_and_close(lock_fd)


def _references(collection, key, data, replay):
    state = replay._state
    for field, target in REFERENCES.get(collection, ()):
        reference = _uuid(data[field])
        if reference not in state.get(target, {}):
            raise StoreError(f"{field} references absent {target}")
        if data.get("station_id") and target not in {"stations", "items", "versions", "media_files", "users", "campaigns"}:
            target_station = state[target][reference]["data"].get("station_id")
            if target_station is not None and target_station != data["station_id"]:
                raise StoreError(f"{field} crosses station scope")
    if collection in {"tokens", "permissions"}:
        if data.get("station_id") and _uuid(data["station_id"]) not in state.get("stations", {}):
            raise StoreError("station_id references absent station")
        if collection == "tokens" and data.get("endpoint_id") and _uuid(data["endpoint_id"]) not in state.get("endpoints", {}):
            raise StoreError("token references absent endpoint")
        if collection == "permissions" and data.get("station_scope") != (data.get("station_id") or "*"):
            raise StoreError("permission station_scope differs from station_id")
        if collection == "permissions" and data.get("revoked_at") is not None:
            stamp = datetime.fromisoformat(data["revoked_at"])
            if stamp.tzinfo is None or not data.get("revoked_by") or _uuid(data["revoked_by"]) not in state.get("users", {}):
                raise StoreError("permission revocation timestamp/operator is invalid")
    if collection == "station_items" and data.get("category_id"):
        category = state.get("categories", {}).get(_uuid(data["category_id"]))
        if category is None or category["data"]["station_id"] != data["station_id"]:
            raise StoreError("category references absent/outside-station category")
    if collection == "plans":
        for field in ("current_revision_id", "draft_revision_id"):
            if data.get(field):
                row = state.get("plan_revisions", {}).get(_uuid(data[field]))
                if row is None or row["data"]["plan_id"] != key:
                    raise StoreError("plan revision pointer has absent/wrong parent")
    if collection == "plan_entries" and data.get("event_kind") == "audio":
        version_id = _uuid(data["payload"]["version_id"])
        version = state.get("versions", {}).get(version_id)
        if version is None:
            raise StoreError("audio entry references absent version")
        revision = state["plan_revisions"][_uuid(data["plan_revision_id"])]["data"]
        station_id = state["plans"][_uuid(revision["plan_id"])]["data"]["station_id"]
        if not replay._indices.get(("station_items", "station_item"), {}).get(
                (station_id, version["data"]["item_id"])):
            raise StoreError("audio entry version is outside station")


def _record(collection, key, row, replay):
    if collection != "_audit" and collection not in REQUIRED:
        raise StoreError("unknown persisted domain collection")
    if not isinstance(key, str) or not key or not isinstance(row, dict) or \
            type(row.get("revision")) is not int or row["revision"] < 1 or not isinstance(row.get("data"), dict):
        raise StoreError("invalid persisted record envelope")
    if key in replay._tombstones.get(collection, ()):
        raise StoreError("live ID also exists in tombstones")
    data = row["data"]
    if any(field not in data or data[field] is None and not (
            collection == "playback_marker_sets" and field in {"segue_in_ms", "segue_out_ms"})
           for field in REQUIRED.get(collection, ())):
        raise StoreError("required field is missing")
    if collection in STATUS and "status" in data and data["status"] not in STATUS[collection]:
        raise StoreError("invalid persisted status")
    if collection in {"desktop_media_library", "desktop_media_analysis", "desktop_media_markers", "desktop_media_rhythm"}:
        from .desktop_media import validate_library, validate_analysis, validate_marker_draft, validate_rhythm
        {"desktop_media_library": validate_library, "desktop_media_analysis": validate_analysis,
         "desktop_media_markers": validate_marker_draft, "desktop_media_rhythm": validate_rhythm}[collection](data, replay._state)
    if collection == "contributions":
        from .desktop_media import validate_managed_contribution
        validate_managed_contribution(data, replay._state)
    if collection == "desktop_configurations":
        from .desktop_api import validate_document
        validate_document(data, replay._state)
    if collection == "desktop_mutations":
        from .desktop_api import validate_mutation
        validate_mutation(data)
    if collection == "desktop_playlist_entries":
        from .desktop_playlist import validate_entry
        validate_entry(data, replay._state)
    if collection == "items" and (data["kind"] not in KINDS or data["lifecycle"] not in STATUS["items"] or
                                   not isinstance(data["title"], str) or not data["title"].strip()):
        raise StoreError("invalid item kind/lifecycle/title")
    _references(collection, key, data, replay)
    if collection in {"plans", "schedule_series", "clock_assignments", "broadcast_services"} and (
            not isinstance(data.get("service_key"), str) or not data["service_key"].strip()):
        raise StoreError("invalid broadcast service key")
    if collection == "plans":
        if data["state"] not in STATUS["plans"]:
            raise StoreError("invalid plan state")
        date.fromisoformat(data["service_date"])
    if collection == "playback_marker_sets":
        from .playback_markers import validate_marker_set
        projected = dict(replay._state)
        version = replay._state["versions"][data["version_id"]]["data"]
        ids = replay._indices.get(("station_items", "station_item"), {}).get(
            (data["station_id"], version["item_id"]), ())
        projected["station_items"] = {identifier: replay._state["station_items"][identifier] for identifier in ids}
        validate_marker_set(data, projected)
    if collection == "plan_revisions" and data.get("execution_contract_version") == 2:
        from .schema import _validate_execution_revision
        projected = dict(replay._state)
        projected["plan_revisions"] = {key: row}
        ids = replay._indices.get(("plan_entries", "revision"), {}).get((key,), ())
        projected["plan_entries"] = {identifier: replay._state["plan_entries"][identifier] for identifier in ids}
        _validate_execution_revision(data, projected)
    if collection == "plan_distributions":
        from .snapshot_contract import validate_snapshot_contract
        validate_snapshot_contract(data["snapshot"], allow_legacy=True)
        if hashlib.sha256(_json_bytes(data["snapshot"])).hexdigest() != data["payload_hash"]:
            raise StoreError("distribution snapshot payload hash differs")
        if data["snapshot"].get("station_id") != data["station_id"] or \
                data["snapshot"].get("plan_revision_id") != data["revision_id"]:
            raise StoreError("distribution snapshot scope differs from registry")
    if collection == "retention_segments":
        from .retention import validate_segment
        validate_segment(data)
    if collection == "_audit":
        if type(data.get("sequence")) is not int or not 1 <= data["sequence"] <= replay._sequence:
            raise StoreError("invalid audit sequence")
        if not isinstance(data.get("changes"), list) or not isinstance(data.get("actor"), str):
            raise StoreError("invalid audit evidence fields")
        instant = datetime.fromisoformat(data["committed_at"].replace("Z", "+00:00"))
        if instant.tzinfo is None or instant.utcoffset() is None:
            raise StoreError("audit timestamp needs a timezone")
    for name, spec in replay.index_specs.get(collection, {}).items():
        value = _index_key(data, spec)
        if value is not None:
            if key not in replay._indices.get((collection, name), {}).get(value, ()):
                raise StoreError("replayed index membership differs from stored record")
            if spec.unique and _unique_key_digest(value) in replay._retired_unique_keys.get(collection, {}).get(name, ()):
                raise StoreError("live unique value also exists in retired identities")


def _hash_ref(root, entry, work, *, parse_lines=False):
    path = _safe_source(root, entry["path"])
    fd = _open_regular(path)
    hasher = hashlib.sha256()
    size = 0
    counts = Counter()
    seen = set()
    audit_numbers = []
    with os.fdopen(fd, "rb") as stream:
        initial = os.fstat(stream.fileno())
        if initial.st_size != entry["size_bytes"]:
            raise StoreError("managed file size differs from committed inventory")
        work.reserve(initial.st_size)
        if parse_lines:
            while True:
                # Reading one byte beyond a bounded line would itself consume
                # budget; read capped blocks through the budgeted line helper.
                line = _line(stream, work)
                if not line:
                    break
                hasher.update(line);size += len(line)
                work.record(cold=True)
                record = json.loads(line)
                if not isinstance(record, dict) or set(record) != {"collection", "key", "envelope"}:
                    raise StoreError("invalid cold record fields")
                identity = (record["collection"], record["key"])
                from .retention import DEFAULT_KEEP_DAYS
                if identity in seen or identity[0] not in DEFAULT_KEEP_DAYS or not isinstance(identity[1], str):
                    raise StoreError("duplicate or invalid cold record identity")
                seen.add(identity)
                envelope = record["envelope"]
                if not isinstance(envelope, dict) or set(envelope) != {"revision", "data"} or \
                        type(envelope["revision"]) is not int or envelope["revision"] < 1 or not isinstance(envelope["data"], dict):
                    raise StoreError("invalid cold record envelope")
                counts[identity[0]] += 1
                if identity[0] == "_audit":
                    audit_numbers.append(envelope["data"]["sequence"])
        else:
            while size < initial.st_size:
                chunk = work.read(stream, min(1024 * 1024, initial.st_size - size))
                if not chunk:
                    raise StoreError("managed file became truncated")
                size += len(chunk);hasher.update(chunk)
        final = os.fstat(stream.fileno())
        if (initial.st_dev, initial.st_ino, initial.st_size, initial.st_mtime_ns) != \
                (final.st_dev, final.st_ino, final.st_size, final.st_mtime_ns):
            raise StoreError("managed file changed during scrub")
    work.files += 1
    if size != entry["size_bytes"] or hasher.hexdigest() != entry["sha256"]:
        raise StoreError("managed file checksum differs from committed inventory")
    return dict(counts), audit_numbers


def _line(stream, work):
    remaining = work.max_bytes - work.bytes
    if remaining <= 0:
        if stream.tell() == os.fstat(stream.fileno()).st_size:
            return b""
        raise _Incomplete("byte budget exhausted")
    line = stream.readline(min(_MAX_FRAME + 1, remaining))
    work.bytes += len(line)
    if len(line) > _MAX_FRAME or line and not line.endswith(b"\n"):
        raise StoreError("invalid or oversized cold record line")
    return line


def inspect_integrity(store_or_path: Store | str | Path, *, max_records: int = 100000,
                      max_bytes: int = 1073741824, verify_media: bool = True,
                      verify_cold: bool = True, max_issues: int = 100) -> dict:
    """Return an honest ``ok``, ``invalid`` or ``incomplete`` scrub report.

    Record budget covers persistent hot invariant checks plus cold JSONL rows.
    Byte budget covers all database bytes decoded and all object bytes hashed.
    Metadata/JSON replay may decode more rows than the invariant-check budget;
    callers should also size max_bytes for the RAM available on their host.
    Authorization belongs to the administrative API/CLI caller.
    """
    for name, value, upper in (("max_records", max_records, 10000000),
                              ("max_bytes", max_bytes, 8 * 1024**4), ("max_issues", max_issues, 10000)):
        if type(value) is not int or not 1 <= value <= upper:
            raise StoreError(f"{name} must be an integer from 1 to {upper}")
    if type(verify_media) is not bool or type(verify_cold) is not bool:
        raise StoreError("verification switches must be booleans")
    requested = store_or_path.journal if isinstance(store_or_path, Store) else Path(store_or_path).expanduser()
    requested = Path(requested).absolute()
    database = requested if requested.suffix.lower() == ".sdb" else requested / "RadioS.sdb"
    work = _Work(max_bytes, max_records)
    checks = {name: {"status": "pending"} for name in
              ("storage", "records", "indexes", "managed_media", "cold_history")}
    issues = []
    issue_count = 0
    complete = True
    sequence = store_id = schema_version = None
    phase = "storage"
    def issue(code, detail, *, collection=None, key=None):
        nonlocal issue_count
        issue_count += 1
        if len(issues) < max_issues:
            issues.append({"code": code, "detail": str(detail), **({"collection": collection, "key": key}
                           if collection is not None else {})})
    try:
        if any(_platform.is_link(path) for path in (database, *database.parents)):
            raise StoreError("database source path must not contain symlinks")
        replay, meta = _load(database, work)
        sequence, store_id, schema_version = replay._sequence, meta["store_id"], meta.get("schema_version", 1)
        checks["storage"] = {"status": "passed", "frames": work.frames, "repair_tail": False}
        checks["indexes"] = {"status": "passed", "indexes": len(replay._indices),
                             "unique_indexes": sum(INDEXES[collection][name].unique
                                for collection, name in replay._indices)}
        phase = "records"
        checked = 0
        for collection, rows in sorted(replay._state.items()):
            if not isinstance(rows, dict):
                issue("record_map_invalid", "collection is not a record map", collection=collection)
                continue
            for key, row in rows.items():
                work.record();checked += 1
                try:
                    _record(collection, key, row, replay)
                except (StoreError, ValueError, TypeError, KeyError, AttributeError) as exc:
                    issue("persistent_record_invalid", exc, collection=collection, key=key)
        audit_numbers = [row["data"].get("sequence") for row in replay._state.get("_audit", {}).values()]
        cold_ranges = sorted((row["data"].get("first_audit_sequence"), row["data"].get("last_audit_sequence"))
                             for row in replay._state.get("retention_segments", {}).values()
                             if row["data"].get("first_audit_sequence"))
        through = 0
        for first, last in cold_ranges:
            if type(first) is not int or type(last) is not int or first != through + 1 or last < first:
                issue("audit_coverage_invalid", "cold audit registry has gap or overlap")
                break
            through = last
        if len(set(audit_numbers)) != len(audit_numbers) or len(audit_numbers) != sequence - through or any(
                type(number) is not int or not through < number <= sequence for number in audit_numbers):
            issue("audit_coverage_invalid", "hot audit sequences differ from committed coverage")
        checks["records"] = {"status": "failed" if issue_count else "passed", "checked": checked,
                              "audit_archived_through": through}
        phase = "managed_media"
        if verify_media:
            from .backup import _media_refs
            objects = _media_refs(replay._state, database.name)
            verified = 0
            for entry in objects:
                try:
                    _hash_ref(database.parent, entry, work);verified += 1
                except (StoreError, OSError, ValueError, TypeError, KeyError) as exc:
                    issue("managed_media_invalid", f"{entry['path']}: {exc}")
            checks[phase] = {"status": "passed" if verified == len(objects) else "failed",
                             "expected_objects": len(objects), "verified_objects": verified}
        else:
            checks[phase] = {"status": "skipped", "reason": "verification disabled"};complete = False
        phase = "cold_history"
        if verify_cold:
            from .retention import archive_file_refs
            archive_file_refs(replay._state, database.name)
            segments = replay._state.get("retention_segments", {})
            verified = 0
            for identifier, row in segments.items():
                data = row["data"]
                try:
                    manifest_ref = next(entry for entry in data["files"] if entry["path"].endswith("/manifest.json"))
                    raw = _whole(_safe_source(database.parent, manifest_ref["path"]), work)
                    if len(raw) != manifest_ref["size_bytes"] or hashlib.sha256(raw).hexdigest() != manifest_ref["sha256"]:
                        raise StoreError("cold manifest checksum or size differs")
                    expected = {key: value for key, value in data.items() if key != "files"} | {"format": "sdbase-retention-v1"}
                    if json.loads(raw) != expected:
                        raise StoreError("cold manifest differs from registered segment")
                    record_ref = next(entry for entry in data["files"] if entry["path"].endswith("/records.jsonl"))
                    if record_ref["sha256"] != data["records_sha256"]:
                        raise StoreError("cold record digest differs from registry")
                    counts, numbers = _hash_ref(database.parent, record_ref, work, parse_lines=True)
                    first, last = data["first_audit_sequence"], data["last_audit_sequence"]
                    if counts != data["counts"] or len(set(numbers)) != len(numbers) or (
                            len(numbers) != last - first + 1 or any(type(number) is not int or not first <= number <= last
                                for number in numbers) if first else bool(numbers)):
                        raise StoreError("cold record inventory/audit range differs")
                    verified += 1
                except (StoreError, OSError, ValueError, TypeError, KeyError, StopIteration) as exc:
                    issue("cold_history_invalid", f"{identifier}: {exc}")
            checks[phase] = {"status": "passed" if verified == len(segments) else "failed",
                             "expected_segments": len(segments), "verified_segments": verified}
        else:
            checks[phase] = {"status": "skipped", "reason": "verification disabled"};complete = False
    except _Incomplete as exc:
        complete = False
        checks[phase] = {"status": "incomplete", "reason": str(exc)}
    except (StoreError, OSError, ValueError, TypeError, KeyError, AttributeError) as exc:
        checks[phase] = {"status": "failed"}
        issue(f"{phase}_invalid", exc)
    for entry in checks.values():
        if entry["status"] == "pending":
            entry.update(status="not_checked", reason="earlier phase stopped")
            complete = False
    return {"status": "invalid" if issue_count else ("ok" if complete else "incomplete"),
            "complete": complete, "store_id": store_id, "sequence": sequence,
            "schema_version": schema_version, "source": str(database), "checks": checks,
            "work": {"bytes_read": work.bytes, "records_checked": work.records,
                     "hot_records_checked": work.hot_records, "cold_records_checked": work.cold_records,
                     "files_read": work.files, "journal_frames": work.frames},
            "limits": {"max_bytes": max_bytes, "max_records": max_records, "max_issues": max_issues,
                       "verify_media": verify_media, "verify_cold": verify_cold},
            "issue_count": issue_count, "issues": issues, "issues_truncated": issue_count > len(issues)}
