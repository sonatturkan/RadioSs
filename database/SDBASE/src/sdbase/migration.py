"""Versioned forward migrations through a verified, fenced physical copy.

The source is opened read-only and never upgraded in place.  A failed stage is
removed; a successful stage is published only at a previously absent path.
The original directory remains the operator's rollback point.
"""

from __future__ import annotations

from contextlib import contextmanager
from datetime import date, datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile
from typing import Iterator
from uuid import UUID, uuid4

from . import backup
from .schema import REFERENCES, REQUIRED, SCHEMA_VERSION, STATUS, open_radio_store
from .storage import Store, StoreError, _atomic_json, _json_bytes


SUPPORTED_SOURCE_VERSIONS = (1, 2, 3)
_SERVICE_COLLECTIONS = ("plans", "schedule_series", "clock_assignments")
_BATCH_BYTES = 8 * 1024 * 1024
_BATCH_ROWS = 100
_STAGE_MARKER = ".sdb-migration-stage.json"
_STAGE_FORMAT = "sdbase-migration-stage-v1"


class MigrationError(StoreError):
    """The source cannot be safely migrated as an offline copy."""


def _target_identity(path: str | Path) -> Path:
    raw = Path(path).expanduser()
    if raw.name in {"", ".", ".."}:
        raise MigrationError("migration target must name a directory")
    try:
        parent = raw.parent.resolve(strict=True)
    except OSError as exc:
        raise MigrationError("migration target parent is absent") from exc
    if not parent.is_dir():
        raise MigrationError("migration target parent is not a directory")
    return parent / raw.name


@contextmanager
def _job_lock(target: Path) -> Iterator[Path]:
    """A dead process releases this lock; recovery never guesses from a PID."""
    path = target.with_name(f".{target.name}.sdb-migration.lock")
    descriptor = -1
    try:
        try:
            descriptor = os.open(path, os.O_CREAT | os.O_RDWR | getattr(os, "O_NOFOLLOW", 0) |
                                 getattr(os, "O_CLOEXEC", 0), 0o600)
        except OSError as exc:
            raise MigrationError("migration job lock is unreadable or unsafe") from exc
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise MigrationError("migration lock must be a regular, unlinked file")
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise MigrationError("migration target has an active migration or recovery job") from exc
        yield path
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _mark_stage(stage: Path, target: Path, *, stage_kind: str,
                source_identity: dict, run_id: str, phase: str = "staging") -> None:
    _atomic_json(stage / _STAGE_MARKER, {
        "format": _STAGE_FORMAT, "run_id": run_id, "target": str(target),
        "stage_path": str(stage), "stage_kind": stage_kind, "phase": phase,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "source": source_identity,
    })
    backup._fsync_dir(stage)


def recover_migration(target_dir: str | Path, *, cleanup: bool = False) -> dict:
    """Inspect or discard only idle, marked staging directories for this target.

    Partial migrations are never resumed.  Retry the original source after a
    cleanup. The completed target and all unmarked paths are preserved.
    """
    if type(cleanup) is not bool:
        raise MigrationError("cleanup must be a boolean")
    target = _target_identity(target_dir)
    candidates, skipped, removed = [], [], []
    with _job_lock(target) as lock:
        for stage in sorted(target.parent.iterdir()):
            if not stage.name.startswith(f".{target.name}.") or stage == lock:
                continue
            if stage.is_symlink() or not stage.is_dir():
                skipped.append({"path": str(stage), "reason": "not_a_real_stage_directory"})
                continue
            marker_path = stage / _STAGE_MARKER
            try:
                descriptor = backup._open_regular(marker_path)
                with os.fdopen(descriptor, "rb") as stream:
                    raw = stream.read(65537)
                if len(raw) > 65536:
                    raise ValueError("oversized stage marker")
                marker = json.loads(raw)
                fields = {"format", "run_id", "target", "stage_path", "stage_kind",
                          "phase", "created_at", "source"}
                if not isinstance(marker, dict) or set(marker) != fields or \
                        marker["format"] != _STAGE_FORMAT or marker["target"] != str(target) or \
                        marker["stage_path"] != str(stage) or \
                        marker["stage_kind"] not in {"snapshot", "database", "backup_source"} or \
                        marker["phase"] not in {"staging", "ready", "failed"} or \
                        not isinstance(marker["source"], dict):
                    raise ValueError("stage marker does not identify this job")
                UUID(marker["run_id"])
                created = datetime.fromisoformat(marker["created_at"].replace("Z", "+00:00"))
                if created.tzinfo is None or created.utcoffset() is None:
                    raise ValueError("stage marker needs an aware timestamp")
            except (StoreError, OSError, ValueError, TypeError, KeyError, AttributeError):
                skipped.append({"path": str(stage), "reason": "missing_or_invalid_ownership_marker"})
                continue
            info = stage.stat(follow_symlinks=False)
            candidates.append({"path": str(stage), "stage_kind": marker["stage_kind"],
                               "phase": marker["phase"], "run_id": marker["run_id"]})
            if cleanup:
                current = stage.stat(follow_symlinks=False)
                if not stat.S_ISDIR(current.st_mode) or \
                        (current.st_dev, current.st_ino) != (info.st_dev, info.st_ino) or \
                        marker_path.is_symlink() or marker_path.read_bytes() != raw:
                    raise MigrationError("migration stage changed during recovery")
                shutil.rmtree(stage)
                removed.append(str(stage))
        if removed:
            backup._fsync_dir(target.parent)
        return {"target": str(target), "target_exists": os.path.lexists(target),
                "cleanup": cleanup, "lock_file": str(lock), "eligible_stages": candidates,
                "removed_stages": removed, "skipped_paths": skipped,
                "recovery_policy": "discard idle owned stages; retry the untouched original source"}


def _source_database(path: str | Path) -> Path:
    raw = Path(path).expanduser().absolute()
    if raw.suffix != ".sdb" or not raw.is_file():
        raise MigrationError("migration source must be an existing .sdb file")
    if any(part.is_symlink() for part in (raw, *raw.parents)):
        raise MigrationError("migration source path must not contain symlinks")
    return raw.resolve(strict=True)


def _legacy_store(path: str | Path) -> Store:
    database = _source_database(path)
    metadata = database.with_name(database.name + ".meta")
    if metadata.is_symlink() or not metadata.is_file():
        raise MigrationError("migration metadata is absent or a symlink")
    try:
        version = json.loads(metadata.read_bytes())["schema_version"]
    except (ValueError, KeyError, OSError, TypeError) as exc:
        raise MigrationError("migration metadata is invalid") from exc
    if type(version) is not int or version not in SUPPORTED_SOURCE_VERSIONS:
        raise MigrationError("migration requires a schema v1, v2 or v3 source")
    return Store(database, schema_version=version, read_only=True)


def _service_changes(store: Store) -> dict[str, int]:
    counts: dict[str, int] = {}
    for collection in _SERVICE_COLLECTIONS:
        rows = store.scan(collection)
        for row in rows:
            key = row.get("service_key")
            if "service_key" in row and (not isinstance(key, str) or not key.strip()):
                raise MigrationError(f"{collection}/{row['_id']} has invalid service_key")
        counts[collection] = sum("service_key" not in row for row in rows)
    return counts


def preview_migration(source_db: str | Path) -> dict:
    """Read-only source census; migrate_copy performs full media verification."""
    source = _legacy_store(source_db)
    meta = source._meta()
    return {
        "source": str(source.journal), "source_schema_version": source.schema_version,
        "target_schema_version": SCHEMA_VERSION, "source_store_id": meta["store_id"],
        "source_sequence": source.sequence,
        "records": {name: len(rows) for name, rows in source._state.items()},
        "service_key_updates": _service_changes(source),
        "steps": [{"from_version": version, "to_version": version + 1}
                  for version in range(source.schema_version, SCHEMA_VERSION)],
        "legacy_approved_revisions": _execution_changes(source)["legacy_approved_revisions"],
        "legacy_execution_policy": "preserve history; fork and reapprove before new distribution",
        "media_verification": "performed by migrate-copy",
    }


def _migrate_service_keys(stage: Store) -> dict[str, int]:
    changed: dict[str, int] = {}
    for collection in _SERVICE_COLLECTIONS:
        rows = stage.scan(collection)
        changed[collection] = 0
        tx = stage.begin(actor="schema-migration-1-to-2")
        used_rows = used_bytes = 0
        for row in rows:
            if "service_key" in row:
                if not isinstance(row["service_key"], str) or not row["service_key"].strip():
                    raise MigrationError(f"{collection}/{row['_id']} has invalid service_key")
                continue
            data = {key: value for key, value in row.items() if key not in {"_id", "_rev"}}
            data["service_key"] = "default"
            size = len(_json_bytes(data)) * 2 + 1024
            if used_rows and (used_rows >= _BATCH_ROWS or used_bytes + size > _BATCH_BYTES):
                tx.commit()
                tx = stage.begin(actor="schema-migration-1-to-2")
                used_rows = used_bytes = 0
            tx.put(collection, row["_id"], data, expected_revision=row["_rev"])
            used_rows += 1
            used_bytes += size
            changed[collection] += 1
        if used_rows:
            tx.commit()
    return changed


def _validate_stored_state(store: Store) -> dict[str, int]:
    """Check persisted shape and references; index rebuilding checks uniqueness.

    Commit-time validators also enforce lifecycle transitions, so replaying
    them against completed historical rows would incorrectly reject approved
    plan entries.  This check uses persistent invariants instead.
    """
    state = store._state
    counts: dict[str, int] = {}
    for collection, rows in state.items():
        if collection != "_audit" and collection not in REQUIRED:
            raise MigrationError(f"unknown persisted collection: {collection}")
        if not isinstance(rows, dict):
            raise MigrationError(f"{collection} is not a record map")
        counts[collection] = len(rows)
        for key, envelope in rows.items():
            if not isinstance(key, str) or not key or not isinstance(envelope, dict) or \
                    type(envelope.get("revision")) is not int or envelope["revision"] < 1 or \
                    not isinstance(envelope.get("data"), dict):
                raise MigrationError(f"{collection}/{key} has invalid record envelope")
            data = envelope["data"]
            if any(name not in data or (data[name] is None and not (
                    collection == "playback_marker_sets" and name in {"segue_in_ms", "segue_out_ms"}))
                   for name in REQUIRED.get(collection, ())):
                raise MigrationError(f"{collection}/{key} is missing a required field")
            for field, target in REFERENCES.get(collection, ()):
                try:
                    reference = str(UUID(str(data[field])))
                except (KeyError, TypeError, ValueError) as exc:
                    raise MigrationError(f"{collection}/{key}.{field} is invalid") from exc
                if reference not in state.get(target, {}):
                    raise MigrationError(f"{collection}/{key}.{field} references absent {target}")
            if "status" in data and collection in STATUS and data["status"] not in STATUS[collection]:
                raise MigrationError(f"{collection}/{key} has invalid status")
            if collection in _SERVICE_COLLECTIONS + ("broadcast_services",):
                service_key = data.get("service_key")
                if not isinstance(service_key, str) or not service_key.strip():
                    raise MigrationError(f"{collection}/{key} has invalid service_key")
            if collection == "plans":
                if data["state"] not in STATUS["plans"]:
                    raise MigrationError(f"plans/{key} has invalid state")
                try:
                    date.fromisoformat(data["service_date"])
                except (TypeError, ValueError) as exc:
                    raise MigrationError(f"plans/{key} has invalid service date") from exc
            if collection == "broadcast_services":
                endpoint = state["endpoints"][str(UUID(data["endpoint_id"]))]["data"]
                if endpoint["station_id"] != data["station_id"]:
                    raise MigrationError(f"broadcast_services/{key} crosses stations")
            if collection == "playback_marker_sets":
                from .playback_markers import validate_marker_set
                validate_marker_set(data, state)
            if collection == "plan_revisions" and data.get("execution_contract_version") == 2:
                from .schema import _validate_execution_revision
                _validate_execution_revision(data, state)
    return counts


def _execution_changes(store: Store) -> dict:
    """Do not invent historical review, cue choices or output destinations."""
    return {"legacy_approved_revisions": sum(
        row["data"]["status"] in {"approved", "superseded"} and
        row["data"].get("execution_contract_version") != 2
        for row in store._state.get("plan_revisions", {}).values()),
        "legacy_deliveries": sum(row["data"].get("snapshot", {}).get("contract_version", 1) != 2
                                 for row in store._state.get("plan_distributions", {}).values()),
        "execution_policy": "legacy histories retained; fresh approval required"}


def _node_role_changes(store: Store) -> dict:
    meta = store._meta()
    return {"node_role": meta.get("node_role", "primary"),
            "node_id": meta["node_id"], "timeline_id": meta["timeline_id"],
            "epoch": meta["epoch"],
            "policy": "explicit primary/standby/fenced write gate; legacy writers incompatible"}


MIGRATIONS = {1: _migrate_service_keys, 2: _execution_changes, 3: _node_role_changes}


def _advance_schema(stage: Store, source_id: str, source_sequence: int) -> tuple[Store, list[dict]]:
    steps = []
    while stage.schema_version < SCHEMA_VERSION:
        version = stage.schema_version
        if version not in MIGRATIONS:
            raise MigrationError(f"no forward migration registered for schema {version}")
        changes = MIGRATIONS[version](stage)
        stage.checkpoint()
        entry = {"from_version": version, "to_version": version + 1,
                 "source_store_id": source_id, "source_sequence": source_sequence,
                 "prepared_at": datetime.now(timezone.utc).isoformat(), "changes": changes}
        with stage._locked(exclusive=True):
            meta = stage._meta()
            meta["schema_version"] = version + 1
            meta["migration_history"] = list(meta.get("migration_history", [])) + [entry]
            _atomic_json(stage.meta_file, meta)
        steps.append(entry)
        stage = Store(stage.journal, schema_version=version + 1)
    return stage, steps


def migrate_copy(source_db: str | Path, target_dir: str | Path) -> dict:
    """Migrate legacy data to an absent, offline, independently keyed current store.

    All media bytes are checked against their committed hashes.  A final source
    sequence check under a shared lock prevents publishing a stale snapshot if
    a writer committed during the copy.  Operators must still stop the old
    service before attaching any physical output to the new one.
    """
    target = backup._destination(target_dir)
    source_path = _source_database(source_db)
    if target.is_relative_to(source_path.parent):
        raise MigrationError("migration target must be outside the source directory")
    with _job_lock(target):
        return _migrate_copy_locked(source_path, target)


def _migrate_copy_locked(source_db: str | Path, target: Path) -> dict:
    source = _legacy_store(source_db)
    source_version = source.schema_version
    source_path = source.journal
    if target.is_relative_to(source.root):
        raise MigrationError("migration target must be outside the source directory")
    source_meta = source._meta()
    source_id = source_meta.get("store_id")
    source_sequence = source.sequence
    if not isinstance(source_id, str) or not source_id:
        raise MigrationError("source store identity is invalid")
    run_id = str(uuid4())
    source_identity = {"source_file": str(source_path), "store_id": source_id,
                       "sequence": source_sequence, "schema_version": source_version}
    with tempfile.TemporaryDirectory(prefix=f".{target.name}.migration-",
                                     dir=target.parent) as temporary:
        _mark_stage(Path(temporary), target, stage_kind="snapshot",
                    source_identity=source_identity, run_id=run_id)
        snapshot_dir = Path(temporary) / "snapshot"
        backup.create_backup(source, snapshot_dir, checkpoint=False)
        verified = backup.verify_backup(snapshot_dir)
        if verified["schema_version"] != source_version or \
                verified["store_id"] != source_id or verified["sequence"] != source_sequence:
            raise MigrationError("source snapshot identity changed before migration")
        manifest = backup._read_manifest(snapshot_dir)
        database, files, objects = backup._validate_manifest(manifest)
        stage = Path(tempfile.mkdtemp(prefix=f".{target.name}.", suffix=".migration",
                                           dir=target.parent))
        try:
            _mark_stage(stage, target, stage_kind="database",
                        source_identity=source_identity, run_id=run_id)
            for relative, entry in files.items():
                backup._copy_file(backup._safe_source(snapshot_dir, relative), stage / relative,
                                  expected_sha=entry["sha256"],
                                  expected_size=entry["size_bytes"])
            backup._prepare_staging_copy(stage / database)
            legacy = Store(stage / database, schema_version=source_version)
            if legacy.sequence != source_sequence or legacy._meta().get("store_id") != source_id:
                raise MigrationError("staged legacy snapshot identity differs")
            legacy, steps = _advance_schema(legacy, source_id, source_sequence)
            migrated = steps[0]["changes"] if source_version == 1 else {name: 0 for name in _SERVICE_COLLECTIONS}
            current = open_radio_store(stage / database)
            counts = _validate_stored_state(current)
            if backup._media_refs(current._state, database) != objects:
                raise MigrationError("staged media references differ from verified snapshot")
            # Final accepted deliveries are immutable to live writers.  The
            # isolated staging copy uses the restoration-only mutation path;
            # all indexes and structural invariants are checked again below.
            fencing = Store(stage / database, schema_version=SCHEMA_VERSION)
            fenced = backup._fence_restored_store(fencing)
            fenced["other_tokens_revoked"] = backup._revoke_other_tokens(fencing)
            credential = backup._issue_provision_admin(fencing)
            final_sequence = fencing.checkpoint()
            _old_id, target_id = backup._rotate_store_identity(fencing)
            reopened = open_radio_store(stage / database)
            if reopened.sequence != final_sequence or reopened._meta().get("store_id") != target_id:
                raise MigrationError("migrated store failed reopening")
            _validate_stored_state(reopened)
            backup._verify_fenced_store(
                reopened, allowed_token_hash=hashlib.sha256(
                    credential["admin_token"].encode()).hexdigest())
            from .identity import authenticate
            if authenticate(reopened, credential["admin_token"]).user_id != credential["admin_user_id"]:
                raise MigrationError("new migration administrator credential is unusable")
            if backup._media_refs(reopened._state, database) != objects:
                raise MigrationError("media references changed while fencing the migrated store")
            backup._fsync_tree(stage)
            _mark_stage(stage, target, stage_kind="database",
                        source_identity=source_identity, run_id=run_id, phase="ready")
            with source._locked(exclusive=False):
                source._reload_locked()
                if source._sequence != source_sequence or source._meta().get("store_id") != source_id:
                    raise MigrationError("source committed changes during migration; retry offline")
                backup._publish_absent(stage, target)
            return {
                "path": str(target), "database": database,
                "source_file": str(source_path), "source_store_id": source_id,
                "source_sequence": source_sequence, "store_id": target_id,
                "schema_version": SCHEMA_VERSION, "restored_sequence": final_sequence,
                "migration_steps": steps,
                "media_objects": len(objects), "records_before_fencing": counts,
                "service_key_updates": migrated, **fenced, **credential,
            }
        finally:
            if stage.exists():
                shutil.rmtree(stage)


def migrate_backup(backup_dir: str | Path, target_dir: str | Path) -> dict:
    """Convert a verified legacy backup when the original database is unavailable.

    A temporary source is extracted from the archive, then the same migration
    and publication checks as ``migrate_copy`` run.  No backup bytes change.
    """
    target = backup._destination(target_dir)
    try:
        backup_path = Path(backup_dir).expanduser().resolve(strict=True)
    except OSError as exc:
        raise MigrationError("migration source backup is absent") from exc
    if target.is_relative_to(backup_path):
        raise MigrationError("migration target must be outside the source backup")
    with _job_lock(target):
        return _migrate_backup_locked(backup_dir, target)


def _migrate_backup_locked(backup_dir: str | Path, target: Path) -> dict:
    verified = backup.verify_backup(backup_dir)
    if verified["schema_version"] not in SUPPORTED_SOURCE_VERSIONS:
        raise MigrationError("migrate-backup requires a v1 or v2 backup")
    source = Path(verified["path"])
    if target.is_relative_to(source):
        raise MigrationError("migration target must be outside the source backup")
    manifest = backup._read_manifest(source)
    database, files, _objects = backup._validate_manifest(manifest)
    with tempfile.TemporaryDirectory(prefix=f".{target.name}.source-",
                                     dir=target.parent) as temporary:
        _mark_stage(Path(temporary), target, stage_kind="backup_source", run_id=str(uuid4()),
                    source_identity={"source_backup": str(source), "store_id": verified["store_id"],
                                     "sequence": verified["sequence"],
                                     "schema_version": verified["schema_version"]})
        extracted = Path(temporary) / "source"
        extracted.mkdir()
        for relative, entry in files.items():
            backup._copy_file(backup._safe_source(source, relative), extracted / relative,
                              expected_sha=entry["sha256"],
                              expected_size=entry["size_bytes"])
        (extracted / (database + ".lock")).touch(exist_ok=False)
        result = _migrate_copy_locked(extracted / database, target)
    result["source_file"] = None
    result["source_backup"] = str(source)
    return result
