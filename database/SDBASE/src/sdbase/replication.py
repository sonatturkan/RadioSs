"""Verified physical standby shipping and a fenced, local handover.

This is asynchronous replication of SDBASE's own document journal. A standby
has the same store identity and may serve reads; ordinary transactions cannot
write it. Promotion requires access to the old primary's lock and fences that
primary before publishing a new writable timeline. There is no automatic
network failover or quorum protocol here.
"""

from __future__ import annotations

from contextlib import ExitStack
from copy import deepcopy
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
import time
from typing import Any
from uuid import UUID, uuid4

from .backup import (_copy_file, _destination, _digest_file, _fsync_dir, _fsync_tree,
                     _inventory, _open_regular, _publish_absent, _read_manifest, _safe_source,
                     _write_manifest, create_backup, verify_backup)
from .schema import open_radio_store
from .storage import (Conflict, Corruption, Store, StoreError, _FORMAT,
                      _MAX_FRAME, _atomic_json, _frame, _json_bytes)


FORMAT = "sdbase-replication-bundle-v1"
_MAX_RECORDS = 10_000
_MAX_BUNDLE_BYTES = 256 * 1024 * 1024


class ReplicationError(StoreError):
    pass


def _read_regular(path: Path, *, max_bytes: int = _MAX_BUNDLE_BYTES) -> bytes:
    with os.fdopen(_open_regular(path), "rb") as stream:
        if os.fstat(stream.fileno()).st_size > max_bytes:
            raise ReplicationError("replication component exceeds its byte budget; split or rebase")
        result = stream.read(max_bytes + 1)
    if len(result) > max_bytes:
        raise ReplicationError("replication component exceeds its byte budget; split or rebase")
    return result


def _json_unique(encoded: bytes | str) -> Any:
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ReplicationError("replication JSON contains a duplicate key")
            result[key] = value
        return result
    return json.loads(encoded, object_pairs_hook=pairs)


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _digest(value: Any) -> str:
    return hashlib.sha256(_json_bytes(value)).hexdigest()


def _sha(value: Any) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(
        char in "0123456789abcdef" for char in value)


def _identity(meta: dict) -> tuple[str, str, int]:
    try:
        store_id = str(UUID(meta["store_id"]))
        timeline = str(UUID(meta["timeline_id"]))
        epoch = meta["epoch"]
        if type(epoch) is not int or epoch < 1:
            raise ValueError
    except (KeyError, TypeError, ValueError, AttributeError) as exc:
        raise ReplicationError("replication requires initialized schema-v4 node identity") from exc
    return store_id, timeline, epoch


def _archive_root(store: Store) -> Path:
    return store.journal.with_name(store.journal.name + ".wal")


def _archive_records(store: Store, *, after_sequence: int = 0,
                     through_sequence: int | None = None) -> list[dict]:
    from .wal_archive import read_archive_records
    meta = store._meta()
    identifier, timeline, epoch = _identity(meta)
    return read_archive_records(_archive_root(store), after_sequence=after_sequence,
                                through_sequence=through_sequence,
                                source_store_id=identifier, timeline_id=timeline,
                                epoch=epoch)


def _anchor(store: Store, sequence: int) -> str:
    """Resolve the exact archived record that establishes a base snapshot."""
    from .wal_archive import archive_anchor
    return archive_anchor(store, sequence)


def _anchor_locked(store: Store, sequence: int) -> str:
    # Do not acquire another flock descriptor while already holding the source
    # lock: an independent shared lock can deadlock against our exclusive one.
    from .wal_archive import read_verified_archive
    header, records = read_verified_archive(_archive_root(store))
    identity = _identity(store._meta())
    body = header["payload"]
    if (body["source_store_id"], body["timeline_id"], body["epoch"]) != identity:
        raise ReplicationError("archive belongs to a different source timeline")
    if sequence == body["start_sequence"]:
        return header["sha256"]
    for record in records:
        if record["payload"]["sequence"] == sequence:
            return record["sha256"]
    raise ReplicationError("source sequence has no archived handover anchor")


def replication_status(store: Store) -> dict:
    with store._locked(exclusive=False):
        store._reload_locked()
        meta = store._meta()
        identifier, timeline, epoch = _identity(meta)
        replication = deepcopy(meta.get("replication"))
        return {"node_role": meta.get("node_role"), "node_id": meta.get("node_id"),
                "store_id": identifier, "timeline_id": timeline, "epoch": epoch,
                "sequence": store._sequence, "replication": replication,
                "pending_replay": bool(replication and replication.get("pending")),
                "mode": "asynchronous_manual_shipping"}


def create_standby(source: Store, destination: str | Path) -> dict:
    """Publish a new read-only physical replica, including media/cold history.

    The immutable WAL archive is enabled before taking the base backup, so a
    writer may continue committing while the backup's large objects are copied.
    No existing destination is overwritten and source live ownership is not
    copied into a writable service.
    """
    from .wal_archive import enable_wal_archive
    with source._locked(exclusive=True):
        source._reload_locked()
        source._assert_writable_locked()
    enable_wal_archive(source)
    target = _destination(destination)
    stage = Path(tempfile.mkdtemp(prefix=f".{target.name}.", suffix=".partial",
                                  dir=target.parent))
    backup_path = stage / "base"
    replica_path = stage / "replica"
    replica_path.mkdir(mode=0o700)
    try:
        create_backup(source, backup_path, checkpoint=False)
        checked = verify_backup(backup_path)
        manifest = _read_manifest(backup_path)
        for entry in manifest["files"]:
            _copy_file(_safe_source(backup_path, entry["path"]),
                       replica_path / entry["path"], expected_sha=entry["sha256"],
                       expected_size=entry["size_bytes"])
        database = checked["database"]
        meta_path = replica_path / (database + ".meta")
        meta = json.loads(meta_path.read_bytes())
        source_id, timeline, epoch = _identity(meta)
        captured_anchor = manifest.get("wal_anchor")
        if not isinstance(captured_anchor, dict) or captured_anchor.get("sequence") != checked["sequence"] or \
                not _sha(captured_anchor.get("sha256")) or \
                captured_anchor.get("archive_id") != meta.get("wal_archive", {}).get("archive_id"):
            raise ReplicationError("verified standby backup has no exact WAL snapshot anchor")
        anchor = captured_anchor["sha256"]
        anchor_time = captured_anchor.get("effective_at")
        if anchor_time is None:
            from .wal_archive import archive_anchor_stamp
            anchor_time = archive_anchor_stamp(source, checked["sequence"], expected_sha256=anchor)["effective_at"]
        meta["node_role"] = "standby"
        meta["node_id"] = str(uuid4())
        meta["generation"] += 1
        # A replica consumes shipped records; it must not capture a second
        # archive under its upstream timeline, even if its backup included one.
        meta.pop("wal_archive", None)
        meta["replication"] = {
            "source_store_id": source_id, "source_node_id": meta.get("source_node_id", ""),
            "source_timeline_id": timeline, "source_epoch": epoch,
            "base_sequence": checked["sequence"],
            "last_replayed_sequence": checked["sequence"],
            "last_record_sha256": anchor, "last_effective_at": anchor_time, "created_at": _now(),
        }
        # source node identity is read from the captured metadata, not from a
        # possibly promoted source after the backup snapshot was established.
        captured_meta = json.loads((backup_path / (database + ".meta")).read_bytes())
        meta["replication"]["source_node_id"] = captured_meta["node_id"]
        _atomic_json(meta_path, meta)
        (replica_path / (database + ".lock")).touch(mode=0o600)
        replica = open_radio_store(replica_path / database, read_only=True)
        if replica.sequence != checked["sequence"]:
            raise ReplicationError("standby base sequence differs from verified backup")
        _fsync_tree(replica_path)
        _publish_absent(replica_path, target)
        return {"path": str(target), "database": database, "store_id": source_id,
                "node_role": "standby", "timeline_id": timeline, "epoch": epoch,
                "sequence": checked["sequence"], "last_record_sha256": anchor,
                "media_objects": checked["media_objects"]}
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def _asset_path(database: str, entry: dict) -> str:
    relative = entry.get("path")
    if not isinstance(relative, str):
        raise ReplicationError("replication asset has no managed path")
    parts = Path(relative).parts
    if Path(relative).is_absolute() or any(part in {"", ".", ".."} for part in parts):
        raise ReplicationError("replication asset path is unsafe")
    if not _sha(entry.get("sha256")) or \
            type(entry.get("size_bytes")) is not int or entry["size_bytes"] < 0:
        raise ReplicationError("replication asset is outside the managed object/history roots")
    media = len(parts) == 3 and parts[0] == database + ".objects" and \
        parts[1] == entry["sha256"][:2] and parts[2] == entry["sha256"]
    try:
        cold = len(parts) == 3 and parts[0] == database + ".archives" and \
            str(UUID(parts[1])) == parts[1] and parts[2] in {"manifest.json", "records.jsonl"}
    except ValueError:
        cold = False
    if not (media or cold):
        raise ReplicationError("replication asset is outside the managed object/history roots")
    return relative


def create_shipment(source: Store, destination: str | Path, *, after_sequence: int,
                    expected_previous_sha256: str, through_sequence: int | None = None,
                    max_records: int = _MAX_RECORDS, verify_full_archive: bool = True,
                    expected_previous_effective_at: str | None = None) -> dict:
    """Package an ordered archive range without changing the replica.

    The caller supplies the replica's last committed archive hash. A checksum
    detects damage, not hostile replacement: transfer the directory over an
    authenticated channel and trust the source operator.
    """
    if type(after_sequence) is not int or after_sequence < 0 or type(verify_full_archive) is not bool or \
            not _sha(expected_previous_sha256) or type(max_records) is not int or \
            not 1 <= max_records <= _MAX_RECORDS:
        raise ReplicationError("invalid replication range or predecessor hash")
    with source._locked(exclusive=False):
        source._reload_locked()
        meta = source._meta()
        identifier, timeline, epoch = _identity(meta)
        if meta.get("node_role") != "primary":
            raise ReplicationError("only a current primary may issue a shipment")
        source_sequence = source._sequence
        if through_sequence is None:
            through_sequence = min(source_sequence, after_sequence + max_records)
        if type(through_sequence) is not int or not after_sequence <= through_sequence <= source_sequence or \
                through_sequence - after_sequence > max_records:
            raise ReplicationError("requested shipment is ahead of source or exceeds bounded range")
        database = source.journal.name
    if verify_full_archive:
        anchor = _anchor(source, after_sequence)
        if anchor != expected_previous_sha256:
            raise Conflict("replica predecessor differs from primary archive; refusing divergent history")
        records = _archive_records(source, after_sequence=after_sequence,
                                   through_sequence=through_sequence)
    else:
        from .wal_archive import iter_pinned_archive_records
        anchor = expected_previous_sha256
        records = list(iter_pinned_archive_records(source, after_sequence=after_sequence,
            through_sequence=through_sequence, expected_previous_sha256=anchor,
            expected_previous_effective_at=expected_previous_effective_at,
            max_records=max_records, verify_assets=False))
    if len(records) != through_sequence - after_sequence:
        raise ReplicationError("WAL archive has a replication sequence gap")
    previous = anchor
    assets: dict[str, dict] = {}
    for number, record in enumerate(records, after_sequence + 1):
        payload = record["payload"]
        if payload["sequence"] != number or payload["previous_sha256"] != previous:
            raise ReplicationError("WAL archive range is not contiguous")
        previous = record["sha256"]
        for entry in payload.get("assets", []):
            relative = _asset_path(database, entry)
            existing = assets.setdefault(relative, dict(entry))
            if existing != entry:
                raise ReplicationError("one managed path has divergent archived assets")
    target = _destination(destination)
    stage = Path(tempfile.mkdtemp(prefix=f".{target.name}.", suffix=".partial",
                                  dir=target.parent))
    try:
        encoded = _json_bytes(records)
        if len(encoded) > _MAX_BUNDLE_BYTES:
            raise ReplicationError("replication records exceed the 256 MiB bundle budget; split or rebase")
        record_path = stage / "records.json"
        with record_path.open("xb") as stream:
            os.chmod(record_path, 0o600)
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        files = [{"path": "records.json", "sha256": hashlib.sha256(encoded).hexdigest(),
                  "size_bytes": len(encoded)}]
        # The immutable archive pool retains the exact bytes referenced at
        # each commit, even after a later retention operation changes history.
        archive_root = _archive_root(source)
        for relative, entry in sorted(assets.items()):
            digest = entry["sha256"]
            archived_object = _safe_source(archive_root, f"assets/{digest[:2]}/{digest}")
            info = _copy_file(archived_object, stage / relative,
                              expected_sha=digest, expected_size=entry["size_bytes"])
            files.append({"path": relative, **info})
        manifest = {"format": FORMAT, "bundle_id": str(uuid4()), "created_at": _now(),
                    "database": database, "source_store_id": identifier,
                    "source_node_id": meta["node_id"], "source_timeline_id": timeline,
                    "source_epoch": epoch, "schema_version": source.schema_version,
                    "from_sequence": after_sequence, "through_sequence": through_sequence,
                    "previous_sha256": anchor, "last_record_sha256": previous,
                    "records_count": len(records), "files": sorted(files, key=lambda row: row["path"])}
        if expected_previous_effective_at is not None:
            manifest["previous_effective_at"] = expected_previous_effective_at
        _write_manifest(stage, manifest)
        _fsync_tree(stage)
        _publish_absent(stage, target)
        return {"path": str(target), "from_sequence": after_sequence,
                "through_sequence": through_sequence, "records": len(records),
                "last_record_sha256": previous}
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def _read_shipment(path: str | Path) -> tuple[Path, dict, list[dict]]:
    raw = Path(path).expanduser()
    if raw.is_symlink() or not raw.is_dir():
        raise ReplicationError("replication bundle directory is absent or a symlink")
    root = raw.resolve(strict=True)
    # Backup's reader pins the backup format; replication has its own strict
    # detached manifest reader instead of pretending to be a backup directory.
    encoded = _read_regular(_safe_source(root, "manifest.json"), max_bytes=4 * 1024 * 1024)
    expected = _read_regular(_safe_source(root, "manifest.sha256"), max_bytes=128).decode("ascii").strip()
    if not _sha(expected) or hashlib.sha256(encoded).hexdigest() != expected:
        raise ReplicationError("replication manifest checksum mismatch")
    try:
        manifest = _json_unique(encoded)
        if manifest["format"] != FORMAT:
            raise ValueError
        UUID(manifest["bundle_id"])
        UUID(manifest["source_node_id"])
        _identity({"store_id": manifest["source_store_id"],
                   "timeline_id": manifest["source_timeline_id"], "epoch": manifest["source_epoch"]})
        first, last = manifest["from_sequence"], manifest["through_sequence"]
        if type(first) is not int or type(last) is not int or not 0 <= first <= last or \
                last - first > _MAX_RECORDS or type(manifest["records_count"]) is not int or \
                manifest["records_count"] != last - first or \
                not _sha(manifest["previous_sha256"]) or not _sha(manifest["last_record_sha256"]) or \
                type(manifest["schema_version"]) is not int or manifest["schema_version"] < 1:
            raise ValueError
        database = manifest["database"]
        if not isinstance(database, str) or Path(database).name != database or not database.endswith(".sdb"):
            raise ValueError
        files = {}
        for entry in manifest["files"]:
            relative = entry["path"]
            if relative != "records.json":
                _asset_path(database, entry)
            if relative in files or set(entry) != {"path", "sha256", "size_bytes"} or \
                    not _sha(entry["sha256"]) or type(entry["size_bytes"]) is not int or entry["size_bytes"] < 0:
                raise ValueError
            files[relative] = entry
        if "records.json" not in files:
            raise ValueError
        if files["records.json"]["size_bytes"] > _MAX_BUNDLE_BYTES:
            raise ReplicationError("replication records exceed the receiver's 256 MiB byte budget")
    except (KeyError, TypeError, ValueError, AttributeError) as exc:
        raise ReplicationError("invalid replication bundle manifest") from exc
    actual_files, actual_directories = _inventory(root)
    expected_files = set(files) | {"manifest.json", "manifest.sha256"}
    expected_dirs = {parent.as_posix() for relative in expected_files
                     for parent in list(Path(relative).parents)[:-1]}
    if actual_files != expected_files or actual_directories != expected_dirs:
        raise ReplicationError("replication bundle has missing or unreferenced files")
    for relative, entry in files.items():
        digest, size = _digest_file(_safe_source(root, relative))
        if (digest, size) != (entry["sha256"], entry["size_bytes"]):
            raise ReplicationError("replication bundle asset checksum/size mismatch")
    try:
        encoded_records = _read_regular(root / "records.json")
        if hashlib.sha256(encoded_records).hexdigest() != files["records.json"]["sha256"] or \
                len(encoded_records) != files["records.json"]["size_bytes"]:
            raise ReplicationError("replication records changed after bundle verification")
        records = _json_unique(encoded_records)
        if not isinstance(records, list) or len(records) != last - first:
            raise ValueError
        previous = manifest["previous_sha256"]
        from .wal_archive import _instant
        previous_time = _instant(manifest["previous_effective_at"]) if "previous_effective_at" in manifest else None
        assets = {}
        from .wal_archive import validate_archive_record
        for number, record in enumerate(records, first + 1):
            payload = validate_archive_record(record)
            if set(record) != {"payload", "sha256"} or _digest(payload) != record["sha256"] or \
                    payload["format"] != "sdbase-wal-record-v1" or payload["sequence"] != number or \
                    payload["source_store_id"] != manifest["source_store_id"] or \
                    payload["timeline_id"] != manifest["source_timeline_id"] or \
                    payload["epoch"] != manifest["source_epoch"] or \
                    payload["schema_version"] != manifest["schema_version"] or \
                    payload["previous_sha256"] != previous or payload["kind"] not in {"transaction", "checkpoint"}:
                raise ValueError
            state = payload["transaction"] if payload["kind"] == "transaction" else payload["checkpoint"]
            instant = _instant(payload["effective_at"])
            if previous_time is not None and instant <= previous_time:
                raise ValueError("unordered shipment timestamp")
            previous_time = instant
            if state["sequence"] != number or state["format"] != _FORMAT:
                raise ValueError
            if payload["kind"] == "transaction":
                _frame(state)  # bounded and JSON serializable before touching the replica
            previous = record["sha256"]
            for entry in payload["assets"]:
                relative = _asset_path(database, entry)
                simple = {key: entry[key] for key in ("path", "sha256", "size_bytes")}
                if files.get(relative) != simple or assets.setdefault(relative, simple) != simple:
                    raise ValueError
        if previous != manifest["last_record_sha256"] or set(files) != {"records.json"} | assets.keys():
            raise ValueError
    except (KeyError, TypeError, ValueError, AttributeError) as exc:
        raise ReplicationError("invalid or noncontiguous replication records") from exc
    return root, manifest, records


def _checkpoint_locked(store: Store, payload: dict) -> None:
    """Trusted recovery publication. Caller holds the lock and verifies role."""
    _atomic_json(store.checkpoint_file, {"payload": payload, "sha256": _digest(payload)})
    with store.journal.open("r+b") as stream:
        stream.truncate(0)
        stream.flush()
        os.fsync(stream.fileno())
    meta = store._meta()
    meta["generation"] += 1
    _atomic_json(store.meta_file, meta)
    store._reload_locked()


def _journal_contains(store: Store, transaction: dict) -> bool:
    expected = _frame(transaction)
    with store.journal.open("rb") as stream:
        while True:
            header = stream.read(4)
            if not header:
                return False
            if len(header) != 4:
                raise Corruption("incomplete standby journal during replay recovery")
            size = struct.unpack(">I", header)[0]
            if not 0 < size <= _MAX_FRAME:
                raise Corruption("invalid standby journal frame size")
            body = stream.read(size + 32)
            if len(body) != size + 32 or hashlib.sha256(body[:-32]).digest() != body[-32:]:
                raise Corruption("invalid standby journal frame during replay recovery")
            if header + body == expected:
                return True


def _recover_pending_locked(store: Store, record: dict | None = None) -> None:
    meta = store._meta()
    replication = meta.get("replication", {})
    pending = replication.get("pending")
    if not pending:
        return
    if store._sequence == replication["last_replayed_sequence"]:
        # Metadata was prepared but no durable state was installed. The same
        # verified bundle can start this record again.
        return
    if store._sequence != pending["sequence"] or record is None or record["sha256"] != pending["sha256"]:
        raise ReplicationError("standby has an interrupted replay; reapply its exact pending bundle")
    payload = record["payload"]
    if payload["kind"] == "transaction":
        installed = _journal_contains(store, payload["transaction"])
    else:
        installed = _digest(json.loads(store.checkpoint_file.read_bytes())["payload"]) == _digest(payload["checkpoint"])
    if not installed:
        raise ReplicationError("pending replay does not match durable standby state")
    replication["last_replayed_sequence"] = pending["sequence"]
    replication["last_record_sha256"] = pending["sha256"]
    replication["last_effective_at"] = payload["effective_at"]
    replication["last_replayed_at"] = _now()
    replication.pop("pending", None)
    _atomic_json(store.meta_file, meta)


def _preflight_locked(store: Store, records: list[dict]) -> None:
    """Replay in a separate in-memory state before any receiver file changes."""
    from .migration import _validate_stored_state
    trial = object.__new__(Store)
    trial._sequence = store._sequence
    trial._state = {name: dict(rows) for name, rows in store._state.items()}
    trial._tombstones = {name: set(ids) for name, ids in store._tombstones.items()}
    trial._retired_unique_keys = deepcopy(store._retired_unique_keys)
    trial._collection_sequences = dict(store._collection_sequences)
    trial.index_specs = store.index_specs
    trial._indices = deepcopy(store._indices)
    for record in records:
        payload = record["payload"]
        if payload["sequence"] <= trial._sequence:
            continue
        if payload["sequence"] != trial._sequence + 1:
            raise ReplicationError("receiver preflight found a sequence gap")
        if payload["kind"] == "transaction":
            trial._apply(payload["transaction"])
        else:
            checkpoint = payload["checkpoint"]
            try:
                tombstones = checkpoint["tombstones"]
                retired = checkpoint.get("retired_unique_keys", {})
                if not isinstance(tombstones, dict) or any(
                        not isinstance(name, str) or not name or not isinstance(ids, list) or
                        any(not isinstance(identifier, str) or not identifier for identifier in ids)
                        for name, ids in tombstones.items()) or not isinstance(retired, dict) or any(
                            not isinstance(collection, str) or not isinstance(indexes, dict) or any(
                                not isinstance(name, str) or not isinstance(values, list) or
                                any(not _sha(value) for value in values)
                                for name, values in indexes.items())
                            for collection, indexes in retired.items()):
                    raise ValueError
                trial._state = checkpoint["state"]
                trial._tombstones = {name: set(ids) for name, ids in tombstones.items()}
                trial._retired_unique_keys = {collection: {name: set(values) for name, values in indexes.items()}
                                             for collection, indexes in retired.items()}
                trial._collection_sequences = dict(checkpoint["collection_sequences"])
                trial._sequence = checkpoint["sequence"]
                if any(type(seq) is not int or not 0 <= seq <= trial._sequence
                       for seq in trial._collection_sequences.values()) or any(
                           not isinstance(name, str) or not name for name in trial._collection_sequences) or any(
                               set(trial._state.get(name, {})) & ids for name, ids in trial._tombstones.items()):
                    raise ValueError
                _validate_stored_state(trial)
                trial._rebuild_indexes()
            except (KeyError, TypeError, ValueError, AttributeError) as exc:
                raise ReplicationError("invalid receiver checkpoint during preflight") from exc
    _validate_stored_state(trial)


def _hydrate_asset_atomic(source: Path, target: Path, entry: dict) -> None:
    """Publish complete verified bytes, so a killed copy cannot poison retry."""
    target.parent.mkdir(parents=True, exist_ok=True)
    stage = target.with_name(f".{target.name}.{uuid4().hex}.replica-partial")
    try:
        _copy_file(source, stage, expected_sha=entry["sha256"], expected_size=entry["size_bytes"])
        try:
            os.link(stage, target, follow_symlinks=False)
        except FileExistsError:
            if _digest_file(target) != (entry["sha256"], entry["size_bytes"]):
                raise ReplicationError("standby immutable object appeared with conflicting bytes")
        _fsync_dir(target.parent)
    finally:
        stage.unlink(missing_ok=True)
        _fsync_dir(target.parent)


def apply_shipment(standby: Store, bundle: str | Path) -> dict:
    """Validate a complete bundle, hydrate assets, then replay durable records.

    Each record has a persisted replay intent. A crash after its journal write
    can be recovered by submitting the exact same bundle; different bytes are
    refused. Successful duplicate bundle delivery is idempotent.
    """
    if standby.read_only:
        raise ReplicationError("receiver maintenance needs an explicitly writable store handle")
    root, manifest, records = _read_shipment(bundle)
    with standby._locked(exclusive=True):
        standby._reload_locked()
        meta = standby._meta()
        identifier, timeline, epoch = _identity(meta)
        replication = meta.get("replication")
        if meta.get("node_role") != "standby" or not isinstance(replication, dict):
            raise ReplicationError("shipping may write only an initialized standby")
        if (identifier, timeline, epoch) != (manifest["source_store_id"], manifest["source_timeline_id"],
                                            manifest["source_epoch"]) or \
                replication.get("source_node_id") != manifest["source_node_id"] or \
                manifest["schema_version"] != standby.schema_version or \
                manifest["database"] != standby.journal.name:
            raise ReplicationError("replication bundle has a foreign source, timeline, epoch or schema")
        pending = replication.get("pending")
        if pending:
            match = next((row for row in records if row["payload"]["sequence"] == pending["sequence"]), None)
            _recover_pending_locked(standby, match)
            meta = standby._meta()
            replication = meta["replication"]
        last = replication["last_replayed_sequence"]
        if standby._sequence != last:
            raise ReplicationError("standby sequence differs from its durable replay position")
        if manifest["through_sequence"] == last and manifest["last_record_sha256"] == replication["last_record_sha256"]:
            return {"applied": 0, "duplicate": True, "sequence": last,
                    "last_record_sha256": replication["last_record_sha256"]}
        if not manifest["from_sequence"] <= last < manifest["through_sequence"]:
            raise ReplicationError("replication bundle is stale, out of order or has a sequence gap")
        anchor = (manifest["previous_sha256"] if last == manifest["from_sequence"] else
                  next(row["sha256"] for row in records if row["payload"]["sequence"] == last))
        if anchor != replication["last_record_sha256"]:
            raise Conflict("replication bundle forks the standby's durable archive chain")
        if last == manifest["from_sequence"] and "previous_effective_at" in manifest:
            from .wal_archive import _instant
            pinned_time = replication.get("last_effective_at")
            if pinned_time is None or _instant(pinned_time) != _instant(manifest["previous_effective_at"]):
                raise Conflict("replication bundle timestamp differs from the standby's durable anchor")
        if replication.get("last_effective_at") is not None:
            from .wal_archive import _instant
            first_unapplied = next(record for record in records if record["payload"]["sequence"] > last)
            if _instant(first_unapplied["payload"]["effective_at"]) <= _instant(replication["last_effective_at"]):
                raise Conflict("replication successor timestamp does not advance the standby's durable anchor")
        _preflight_locked(standby, records)
        for entry in manifest["files"]:
            if entry["path"] == "records.json":
                continue
            target = _safe_source(standby.root, entry["path"])
            if target.exists():
                if _digest_file(target) != (entry["sha256"], entry["size_bytes"]):
                    raise ReplicationError("existing standby object differs from shipped immutable bytes")
            else:
                _hydrate_asset_atomic(_safe_source(root, entry["path"]), target, entry)
        _fsync_tree(standby.root)
        applied = 0
        for record in records:
            payload = record["payload"]
            if payload["sequence"] <= last:
                continue
            meta = standby._meta()
            replication = meta["replication"]
            replication["pending"] = {"sequence": payload["sequence"], "sha256": record["sha256"],
                                       "bundle_id": manifest["bundle_id"]}
            _atomic_json(standby.meta_file, meta)
            if payload["kind"] == "transaction":
                txn = payload["transaction"]
                frame = _frame(txn)
                with standby.journal.open("ab") as stream:
                    stream.write(frame)
                    stream.flush()
                    os.fsync(stream.fileno())
                standby._reload_locked()
            else:
                _checkpoint_locked(standby, payload["checkpoint"])
            _recover_pending_locked(standby, record)
            last = payload["sequence"]
            applied += 1
        return {"applied": applied, "duplicate": False, "sequence": last,
                "last_record_sha256": standby._meta()["replication"]["last_record_sha256"]}


def catch_up(source: Store, standby: Store, *, max_records: int = _MAX_RECORDS) -> dict:
    """Perform one bounded local shipping batch. Repeat until lag is zero."""
    status = replication_status(standby)
    replication = status.get("replication")
    if status["node_role"] != "standby" or not replication:
        raise ReplicationError("catch-up target is not a standby")
    with tempfile.TemporaryDirectory(prefix="sdbase-shipping-", dir=standby.root.parent) as temporary:
        bundle = Path(temporary) / "bundle"
        shipped = create_shipment(source, bundle,
                                  after_sequence=replication["last_replayed_sequence"],
                                  expected_previous_sha256=replication["last_record_sha256"],
                                  max_records=max_records)
        result = apply_shipment(standby, bundle)
    result["source_sequence"] = source.sequence
    result["lag_transactions"] = result["source_sequence"] - result["sequence"]
    return result


def recover_standby_replay(database: str | Path, bundle: str | Path) -> dict:
    """Repair only an exact verified pending-frame prefix after a torn write.

    Ordinary standby startup never truncates journal bytes. This explicit
    maintenance operation needs the original verified shipment, its persisted
    replay intent and the store lock. A checksum failure in a complete frame,
    a different prefix or an unrelated incomplete tail is refused.
    """
    _bundle_root, manifest, records = _read_shipment(bundle)
    raw = Path(database).expanduser()
    if raw.suffix != ".sdb":
        raw = raw / "RadioS.sdb"
    if raw.is_symlink() or raw.parent.is_symlink():
        raise ReplicationError("standby recovery path must not contain symlinks")
    journal = raw.resolve(strict=True)
    root = journal.parent
    meta_path = _safe_source(root, journal.name + ".meta")
    lock_path = _safe_source(root, journal.name + ".lock")
    repaired = 0
    lock_fd = _open_regular(lock_path)
    try:
        deadline = time.monotonic() + 30
        while True:
            try:
                fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise ReplicationError("standby replay recovery lock timed out")
                time.sleep(0.01)
        meta = _json_unique(_read_regular(meta_path, max_bytes=4 * 1024 * 1024))
        replication = meta.get("replication", {})
        pending = replication.get("pending")
        if meta.get("format") != _FORMAT or type(meta.get("generation")) is not int or \
                meta["generation"] < 0 or meta.get("node_role") != "standby" or not pending or \
                _identity(meta) != (manifest["source_store_id"], manifest["source_timeline_id"], manifest["source_epoch"]) or \
                replication.get("source_node_id") != manifest["source_node_id"] or \
                meta.get("schema_version") != manifest["schema_version"] or journal.name != manifest["database"]:
            raise ReplicationError("torn-write repair requires the matching standby replay intent")
        record = next((row for row in records if row["payload"]["sequence"] == pending["sequence"]), None)
        if record is None or record["sha256"] != pending["sha256"]:
            raise ReplicationError("recovery bundle differs from pending standby replay")
        checkpoint_path = _safe_source(root, journal.name + ".checkpoint")
        sequence = 0
        if checkpoint_path.exists():
            envelope = _json_unique(_read_regular(checkpoint_path))
            if _digest(envelope["payload"]) != envelope["sha256"]:
                raise ReplicationError("standby checkpoint is damaged")
            sequence = envelope["payload"]["sequence"]
        fd = os.open(journal, os.O_RDWR | getattr(os, "O_NOFOLLOW", 0))
        with os.fdopen(fd, "r+b") as stream:
            while True:
                start = stream.tell()
                header = stream.read(4)
                if not header:
                    break
                if len(header) == 4:
                    length = struct.unpack(">I", header)[0]
                    if not 0 < length <= _MAX_FRAME:
                        raise ReplicationError("standby recovery found an invalid complete frame header")
                    body = stream.read(length + 32)
                else:
                    body = b""
                    length = 0
                if len(header) < 4 or len(body) < length + 32:
                    partial = header + body
                    payload = record["payload"]
                    if payload["kind"] != "transaction" or sequence != replication["last_replayed_sequence"] or \
                            pending["sequence"] != sequence + 1 or not _frame(payload["transaction"]).startswith(partial):
                        raise ReplicationError("incomplete standby tail is not the verified pending transaction prefix")
                    repaired = len(partial)
                    stream.truncate(start)
                    stream.flush()
                    os.fsync(stream.fileno())
                    break
                if hashlib.sha256(body[:-32]).digest() != body[-32:]:
                    raise ReplicationError("complete standby frame checksum failure cannot be repaired as a torn tail")
                transaction = _json_unique(body[:-32])
                number = transaction.get("sequence")
                if type(number) is not int or number > sequence and number != sequence + 1:
                    raise ReplicationError("standby recovery found a sequence gap")
                sequence = max(sequence, number)
    finally:
        fcntl.flock(lock_fd, fcntl.LOCK_UN)
        os.close(lock_fd)
    result = apply_shipment(open_radio_store(journal), bundle)
    result["repaired_tail_bytes"] = repaired
    return result


def _promotion_payload(store: Store) -> tuple[dict, dict]:
    """Make one compact fenced ownership checkpoint without normal writes."""
    state = deepcopy(store._state)
    now = _now()
    counts: dict[str, int] = {}
    touched = set()
    audit_changes = []

    def update(collection: str, changes):
        count = 0
        for identifier, record in state.get(collection, {}).items():
            body = record["data"]
            extra = changes(body)
            if extra is not None:
                before_revision = record["revision"]
                record["data"] = body | extra | {"promotion_fenced_at": now}
                record["revision"] += 1
                audit_changes.append({"collection": collection, "key": identifier,
                                      "before_revision": before_revision, "before": body,
                                      "after_revision": record["revision"], "after": record["data"]})
                count += 1
        if count:
            counts[collection] = count
            touched.add(collection)

    update("endpoints", lambda row: None if row.get("status") == "retired" else {"status": "offline"})
    update("endpoint_copies", lambda row: None if row.get("status") == "deleted" else
           {"status": "pending", "verified_at": None, "promotion_previous_status": row.get("status")})
    update("active_acceptances", lambda row: {"state": "inactive", "promotion_previous_engine_epoch": row.get("engine_epoch")})
    update("plan_distributions", lambda row: {"status": "pending", "sent_at": None, "acknowledged_at": None}
           if row.get("status") in {"sent", "accepted"} else None)
    update("engine_commands", lambda row: {"state": "expired", "error_code": "promotion_fenced"}
           if row.get("state") in {"pending", "accepted"} else None)
    update("tokens", lambda row: {"revoked_at": now} if row.get("scope") == "endpoint" and not row.get("revoked_at") else None)
    sequence = store._sequence + 1
    audit_id = str(uuid4())
    state.setdefault("_audit", {})[audit_id] = {"revision": 1, "data": {
        "sequence": sequence, "actor": "replication-promotion", "committed_at": now,
        "changes": audit_changes}}
    generations = dict(store._collection_sequences)
    for collection in touched | {"_audit"}:
        generations[collection] = sequence
    payload = {"format": _FORMAT, "sequence": sequence, "state": state,
               "tombstones": {name: sorted(ids) for name, ids in store._tombstones.items()},
               "retired_unique_keys": {collection: {name: sorted(values) for name, values in indexes.items()}
                                       for collection, indexes in store._retired_unique_keys.items()},
               "collection_sequences": generations}
    return payload, counts


def _read_promotion_intent(store: Store, pending: dict) -> dict:
    relative = pending.get("checkpoint_path", "")
    parts = Path(relative).parts
    if len(parts) != 2 or parts[0] != store.journal.name + ".replication" or \
            not parts[1].startswith("promotion-") or not parts[1].endswith(".json"):
        raise ReplicationError("invalid promotion recovery intent path")
    try:
        envelope = json.loads(_safe_source(store.root, relative).read_bytes())
        payload = envelope["payload"]
        if envelope["sha256"] != pending["checkpoint_sha256"] or _digest(payload) != envelope["sha256"] or \
                payload["sequence"] != pending["sequence"]:
            raise ValueError
    except (OSError, KeyError, ValueError, TypeError) as exc:
        raise ReplicationError("promotion recovery intent is missing or damaged") from exc
    return payload


def promote_standby(source: Store, standby: Store, *, expected_source_sequence: int,
                    expected_source_timeline_id: str) -> dict:
    """Controlled local handover. The old primary is fenced before publication.

    There is deliberately no ``force=True`` path: both store locks and exact
    replay equality are required. After a crash between fencing and promotion,
    retrying this exact command can finish a recorded handover. It never makes
    the old primary writable again. Reopen the promoted store for new writes.
    """
    if source.read_only or standby.read_only:
        raise ReplicationError("handover maintenance needs explicitly writable store handles")
    if source.journal == standby.journal or type(expected_source_sequence) is not int or expected_source_sequence < 0:
        raise ReplicationError("promotion requires separate source and standby stores and an exact sequence")
    try:
        expected_source_timeline_id = str(UUID(expected_source_timeline_id))
    except (ValueError, TypeError, AttributeError) as exc:
        raise ReplicationError("promotion requires an exact source timeline UUID") from exc
    stores = sorted((source, standby), key=lambda row: str(row.lock_file))
    with ExitStack() as stack:
        for store in stores:
            stack.enter_context(store._locked(exclusive=True))
            store._reload_locked()
        source_meta, target_meta = source._meta(), standby._meta()
        source_identity = _identity(source_meta)
        target_identity = _identity(target_meta)
        replication = target_meta.get("replication", {})
        existing = source_meta.get("handover")
        matching_handover = bool(existing and source_meta.get("node_role") == "fenced" and
                                existing.get("target_node_id") == target_meta["node_id"] and
                                existing.get("source_sequence") == expected_source_sequence and
                                existing.get("source_timeline_id") == expected_source_timeline_id)
        if target_meta.get("node_role") == "primary" and matching_handover and \
                target_meta.get("handover") == existing and target_identity == (
                    source_identity[0], existing["target_timeline_id"], existing["target_epoch"]):
            return {"source_role": "fenced", "target_role": "primary", "already_promoted": True,
                    "sequence": standby._sequence, "timeline_id": target_identity[1],
                    "epoch": target_identity[2], "reopen_required": True}
        if target_meta.get("node_role") != "standby":
            raise ReplicationError("promotion target is not a standby")
        if source_identity != target_identity or source_identity[1] != expected_source_timeline_id or \
                replication.get("source_node_id") != source_meta.get("node_id"):
            raise ReplicationError("promotion source and standby do not share the requested history")
        pending = target_meta.get("promotion_pending")
        recovering_checkpoint = bool(pending and matching_handover and
                                     pending.get("sequence") == expected_source_sequence + 1 and
                                     standby._sequence == expected_source_sequence + 1)
        if source._sequence != expected_source_sequence or \
                (standby._sequence != expected_source_sequence and not recovering_checkpoint) or \
                replication.get("last_replayed_sequence") != expected_source_sequence or replication.get("pending"):
            raise Conflict("promotion requires exact caught-up positions and no pending replay")
        anchor = _anchor_locked(source, expected_source_sequence)
        if anchor != replication.get("last_record_sha256"):
            raise ReplicationError("promotion standby archive anchor differs from source")
        if not recovering_checkpoint:
            # Full logical equality also catches a replaced checkpoint retaining
            # the same source identifiers and replication progress metadata.
            source_state = {"state": source._state, "tombstones": {k: sorted(v) for k, v in source._tombstones.items()},
                            "retired": {c: {n: sorted(v) for n, v in ix.items()} for c, ix in source._retired_unique_keys.items()},
                            "collections": source._collection_sequences}
            target_state = {"state": standby._state, "tombstones": {k: sorted(v) for k, v in standby._tombstones.items()},
                            "retired": {c: {n: sorted(v) for n, v in ix.items()} for c, ix in standby._retired_unique_keys.items()},
                            "collections": standby._collection_sequences}
            if _digest(source_state) != _digest(target_state):
                raise ReplicationError("promotion requires byte-equivalent logical source and replica state")
        if source_meta.get("node_role") == "fenced":
            if not matching_handover:
                raise ReplicationError("source was fenced for a different handover")
            new_timeline, new_epoch = existing["target_timeline_id"], existing["target_epoch"]
        elif source_meta.get("node_role") == "primary":
            source._assert_writable_locked()
            if pending:
                raise ReplicationError("standby has a promotion intent from a different handover")
            new_timeline, new_epoch = str(uuid4()), source_identity[2] + 1
            existing = {"target_node_id": target_meta["node_id"], "target_timeline_id": new_timeline,
                        "target_epoch": new_epoch, "source_sequence": expected_source_sequence,
                        "source_timeline_id": source_identity[1], "created_at": _now()}
            source_meta["node_role"] = "fenced"
            source_meta["handover"] = existing
            source_meta["generation"] += 1
            _atomic_json(source.meta_file, source_meta)
        else:
            raise ReplicationError("handover source must be current primary or its matching fenced predecessor")
        if pending:
            payload = _read_promotion_intent(standby, pending)
            counts = pending["ownership_fenced"]
        else:
            payload, counts = _promotion_payload(standby)
            relative = standby.journal.name + f".replication/promotion-{uuid4().hex}.json"
            intent_path = _safe_source(standby.root, relative)
            intent_path.parent.mkdir(mode=0o700, exist_ok=True)
            _atomic_json(intent_path, {"payload": payload, "sha256": _digest(payload)})
            target_meta = standby._meta()
            pending = {"checkpoint_path": relative, "checkpoint_sha256": _digest(payload),
                       "sequence": payload["sequence"], "ownership_fenced": counts}
            target_meta["promotion_pending"] = pending
            _atomic_json(standby.meta_file, target_meta)
        if recovering_checkpoint:
            installed = json.loads(standby.checkpoint_file.read_bytes())
            if installed.get("sha256") != pending["checkpoint_sha256"] or _digest(installed.get("payload")) != pending["checkpoint_sha256"]:
                raise ReplicationError("interrupted promotion checkpoint differs from recorded intent")
        else:
            _checkpoint_locked(standby, payload)
        target_meta = standby._meta()
        target_meta["node_role"] = "primary"
        target_meta["timeline_id"] = new_timeline
        target_meta["epoch"] = new_epoch
        target_meta["generation"] += 1
        target_meta.pop("wal_archive", None)
        target_meta.pop("promotion_pending", None)
        target_meta["replication"]["promoted_at"] = _now()
        target_meta["replication"]["promoted_from_timeline_id"] = source_identity[1]
        target_meta["replication"]["promotion_source_sequence"] = expected_source_sequence
        target_meta["replication"]["last_replayed_sequence"] = expected_source_sequence
        target_meta["handover"] = existing
        _atomic_json(standby.meta_file, target_meta)
        retained_intent = None
        try:
            _safe_source(standby.root, pending["checkpoint_path"]).unlink(missing_ok=True)
        except OSError:
            # Publication is already durable. A cleanup error must not imply
            # that the old primary should be made writable again.
            retained_intent = pending["checkpoint_path"]
        result = {"source_role": "fenced", "target_role": "primary", "target_path": str(standby.journal),
                "source_sequence": expected_source_sequence, "sequence": payload["sequence"],
                "store_id": source_identity[0], "timeline_id": new_timeline, "epoch": new_epoch,
                "ownership_fenced": counts, "reopen_required": True,
                "wal_archive": "enable_again_for_new_timeline"}
        if retained_intent:
            result["retained_recovery_intent"] = retained_intent
        return result
