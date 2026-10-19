"""Consistent, self-verifying backups for the independent SDBASE store.

The backup is a directory containing the exact ``.sdb`` database files,
referenced content-addressed media, and a detached-hash manifest.  Restoring
publishes a new directory only after verification and operational fencing.
The hash protects against accidental damage; it is not an authenticity proof.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import secrets
import stat
from typing import Any, BinaryIO, Mapping
from uuid import uuid4

from . import _platform
from .media import object_root
from .schema import open_radio_store
from .storage import Store, StoreError, _atomic_json


_FORMAT = "sdbase-backup-v1"
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_CHUNK = 1024 * 1024
_BATCH_BYTES = 8 * 1024 * 1024
_BATCH_ROWS = 100


class BackupError(StoreError):
    """The backup source, archive, or restoration target is unsuitable."""


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _json_bytes(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False).encode("utf-8")


def _fsync_dir(path: Path) -> None:
    """Directory durability point (NTFS journals the rename; see _platform)."""
    _platform.fsync_dir(path)


def _fsync_tree(root: Path) -> None:
    _platform.fsync_tree(root)


def _safe_source(root: Path, relative: str) -> Path:
    """Reject unsafe relative paths and links/junctions in every component.

    ``relative`` is a stored portable path ('/' separators).  The parser is
    host independent: a drive, a backslash, an alternate data stream, a
    reserved device name or a dot component is refused on every system.
    """
    try:
        parts = _platform.safe_relative_parts(relative)
    except ValueError as exc:
        raise BackupError("unsafe backup path") from exc
    current = root
    if _platform.is_link(current):
        raise BackupError("backup root must not be a symlink")
    for part in parts:
        current = current / part
        if _platform.is_link(current):
            raise BackupError("backup path must not contain symlinks")
    return current


def _open_regular(path: Path) -> int:
    """No-follow, share-delete, binary read descriptor of a regular file."""
    try:
        fd = _platform.open_fd(path, os.O_RDONLY, nofollow=True)
    except IsADirectoryError as exc:
        raise BackupError(f"backup component is not a regular file: {path}") from exc
    except OSError as exc:
        raise BackupError(f"cannot read backup component: {path}") from exc
    try:
        regular = stat.S_ISREG(os.fstat(fd).st_mode)
    except BaseException:
        os.close(fd)
        raise
    if not regular:
        os.close(fd)
        raise BackupError(f"backup component is not a regular file: {path}")
    return fd


def _create_private(path: Path) -> BinaryIO:
    """Create a new owner-only file (the Linux ``open("xb")`` + ``chmod 0600``).

    The handle is binary, non-inheritable and created atomically with the
    private security descriptor; an existing name raises FileExistsError and a
    link is never followed.
    """
    fd = _platform.open_fd(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600,
                           nofollow=True, private=True)
    try:
        return os.fdopen(fd, "wb")
    except BaseException:
        os.close(fd)
        raise


def _digest_file(path: Path) -> tuple[str, int]:
    fd = _open_regular(path)
    digest = hashlib.sha256()
    size = 0
    try:
        with os.fdopen(fd, "rb") as stream:
            for block in iter(lambda: stream.read(_CHUNK), b""):
                digest.update(block)
                size += len(block)
    except OSError as exc:
        raise BackupError(f"cannot hash backup component: {path}") from exc
    return digest.hexdigest(), size


def _copy_file(source: Path, target: Path, *, expected_sha: str | None = None,
               expected_size: int | None = None) -> dict:
    target.parent.mkdir(parents=True, exist_ok=True)
    source_fd = _open_regular(source)
    digest = hashlib.sha256()
    size = 0
    created = False
    try:
        with os.fdopen(source_fd, "rb") as input_stream:
            before = os.fstat(input_stream.fileno())
            with _create_private(target) as output_stream:
                created = True
                for block in iter(lambda: input_stream.read(_CHUNK), b""):
                    output_stream.write(block)
                    digest.update(block)
                    size += len(block)
                output_stream.flush()
                os.fsync(output_stream.fileno())
            after = os.fstat(input_stream.fileno())
        if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != \
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
            raise BackupError(f"backup source changed during copy: {source}")
    except BaseException:
        # Remove only what this call created (an existing target is never
        # deleted) and never mask the original error with a cleanup failure
        # (a scanner may still hold the new file); the stage owner removes it.
        if created:
            _platform.unlink_quietly(target)
        raise
    result = {"sha256": digest.hexdigest(), "size_bytes": size}
    if expected_sha is not None and result["sha256"] != expected_sha:
        _platform.unlink_quietly(target)
        raise BackupError(f"backup object hash mismatch: {source}")
    if expected_size is not None and size != expected_size:
        _platform.unlink_quietly(target)
        raise BackupError(f"backup object size mismatch: {source}")
    return result


def _destination(path: str | Path) -> Path:
    raw = Path(path).expanduser()
    if raw.name in {"", ".", ".."} or not _platform.valid_new_name(raw.name):
        # Windows would silently strip a trailing dot/space or open a device
        # (CON, NUL, ...) instead of creating the requested directory.
        raise BackupError("target must name a new directory")
    try:
        parent = raw.parent.resolve(strict=True)
    except OSError as exc:
        raise BackupError("target parent directory does not exist") from exc
    if not parent.is_dir():
        raise BackupError("target parent is not a directory")
    target = parent / raw.name
    if os.path.lexists(target):
        raise BackupError("target directory already exists")
    return target


def _publish_absent(stage: Path, target: Path) -> None:
    """Publish atomically without replacing a concurrently created target.

    A single no-replace rename (never overwrites; transient sharing violations
    from scanners are retried by the platform layer).
    """
    try:
        _platform.publish_dir_noreplace(stage, target)
    except FileExistsError as exc:
        raise BackupError("target directory appeared during publication") from exc
    except OSError as exc:
        raise BackupError(f"could not publish backup directory: {exc.strerror or exc}") from exc
    _fsync_dir(target.parent)


def _media_refs(state: Mapping[str, dict], database: str) -> list[dict]:
    """Derive object list exclusively from committed managed location records."""
    media = state.get("media_files", {})
    grouped: dict[str, dict] = {}
    for location_id, envelope in state.get("media_locations", {}).items():
        row = envelope["data"]
        media_id = row.get("media_file_id")
        if not isinstance(media_id, str):
            raise BackupError(f"managed location {location_id} has invalid media identity")
        file_row = media.get(media_id, {}).get("data")
        if file_row is None:
            raise BackupError(f"managed location {location_id} has no media file")
        digest = row.get("sha256")
        size = row.get("size_bytes")
        if not isinstance(digest, str) or not _SHA256.fullmatch(digest) or \
                isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise BackupError(f"managed location {location_id} has invalid object identity")
        if row.get("storage_uri") != "sdb-object:" + digest or \
                file_row.get("sha256") != digest or file_row.get("size_bytes") != size:
            raise BackupError(f"managed location {location_id} differs from its media file")
        obj = grouped.setdefault(digest, {
            "path": f"{database}.objects/{digest[:2]}/{digest}",
            "sha256": digest, "size_bytes": size,
            "media_file_ids": [], "location_ids": [],
        })
        if obj["size_bytes"] != size:
            raise BackupError("one content address has conflicting committed sizes")
        obj["media_file_ids"].append(media_id)
        obj["location_ids"].append(location_id)
    for obj in grouped.values():
        obj["media_file_ids"] = sorted(set(obj["media_file_ids"]))
        obj["location_ids"].sort()
    return [grouped[key] for key in sorted(grouped)]


def _write_manifest(stage: Path, manifest: dict) -> None:
    encoded = _json_bytes(manifest)
    for name, content in (("manifest.json", encoded),
                          ("manifest.sha256", (hashlib.sha256(encoded).hexdigest() + "\n").encode("ascii"))):
        path = stage / name
        with _create_private(path) as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())


def create_backup(store: Store, destination: str | Path, *, checkpoint: bool = True) -> dict:
    """Back up one committed SDBASE snapshot and its referenced media objects.

    By default a checkpoint is written first.  Setting ``checkpoint=False``
    captures the current journal and checkpoint without changing the source,
    as required by a physical provisioning copy.  A shared store lock holds
    the database files and committed object references stable while copied.
    Media copy checks every byte against its committed SHA-256 and size.
    """
    target = _destination(destination)
    stage = Path(_platform.mkdtemp_private(prefix=f".{target.name}.", suffix=".partial",
                                           dir=target.parent))
    database = store.journal.name
    try:
        if checkpoint:
            store.checkpoint()
        files: list[dict] = []
        with store._locked(exclusive=False):
            store._reload_locked()
            objects = _media_refs(store._state, database)
            from .retention import archive_file_refs
            cold_files = archive_file_refs(store._state, database)
            sequence = store._sequence
            metadata = store._meta()
            store_id = metadata.get("store_id")
            wal_anchor = None
            if metadata.get("wal_archive"):
                from .wal_archive import archive_anchor_stamp
                wal_anchor = {"archive_id": metadata["wal_archive"]["archive_id"],
                              **archive_anchor_stamp(store, sequence)}
            for source in (store.journal, store.meta_file, store.checkpoint_file):
                if not source.exists() and source == store.checkpoint_file:
                    continue
                relative = source.name
                info = _copy_file(_safe_source(store.root, relative), stage / relative)
                files.append({"path": relative, **info})
        for obj in objects:
            source = _safe_source(object_root(store),
                                  f"{obj['sha256'][:2]}/{obj['sha256']}")
            info = _copy_file(source, stage / obj["path"],
                              expected_sha=obj["sha256"],
                              expected_size=obj["size_bytes"])
            files.append({"path": obj["path"], **info})
        for entry in cold_files:
            info = _copy_file(_safe_source(store.root, entry["path"]), stage / entry["path"],
                              expected_sha=entry["sha256"], expected_size=entry["size_bytes"])
            files.append({"path": entry["path"], **info})
        manifest = {"format": _FORMAT, "created_at": _now(),
                    "database": database, "store_id": store_id,
                    "sequence": sequence, "schema_version": store.schema_version,
                    "files": sorted(files, key=lambda row: row["path"]),
                    "media_objects": objects, "cold_history_files": cold_files}
        if wal_anchor is not None:
            manifest["wal_anchor"] = wal_anchor
        _write_manifest(stage, manifest)
        _fsync_tree(stage)
        _publish_absent(stage, target)
        return {"path": str(target), "store_id": store_id, "sequence": sequence,
                "media_objects": len(objects), "files": len(files)}
    finally:
        if os.path.lexists(stage):
            _platform.rmtree(stage, ignore_errors=True)


def _read_manifest(root: Path) -> dict:
    def unique_keys(pairs: list[tuple[str, Any]]) -> dict:
        result: dict = {}
        for key, value in pairs:
            if key in result:
                raise BackupError("backup manifest contains duplicate keys")
            result[key] = value
        return result

    try:
        manifest_path = _safe_source(root, "manifest.json")
        hash_path = _safe_source(root, "manifest.sha256")
        with os.fdopen(_open_regular(manifest_path), "rb") as stream:
            encoded = stream.read()
        with os.fdopen(_open_regular(hash_path), "rb") as stream:
            expected = stream.read().decode("ascii").strip()
        if not _SHA256.fullmatch(expected) or hashlib.sha256(encoded).hexdigest() != expected:
            raise BackupError("backup manifest checksum mismatch")
        manifest = json.loads(encoded, object_pairs_hook=unique_keys)
    except (OSError, UnicodeError, ValueError, TypeError) as exc:
        raise BackupError("backup manifest is unreadable") from exc
    if not isinstance(manifest, dict) or manifest.get("format") != _FORMAT:
        raise BackupError("unsupported backup manifest")
    return manifest


def _validate_manifest(manifest: dict) -> tuple[str, dict[str, dict], list[dict]]:
    database = manifest.get("database")
    if not isinstance(database, str) or not database.endswith(".sdb") or \
            database in {".sdb", "..sdb"} or Path(database).name != database or \
            "/" in database or "\\" in database or "\x00" in database or \
            not _platform.component_valid(database):
        raise BackupError("unsafe database filename in manifest")
    sequence = manifest.get("sequence")
    if isinstance(sequence, bool) or not isinstance(sequence, int) or sequence < 0:
        raise BackupError("invalid backup sequence")
    schema_version = manifest.get("schema_version", 1)
    if type(schema_version) is not int or schema_version < 1:
        raise BackupError("invalid backup schema version")
    if not isinstance(manifest.get("store_id"), str) or not manifest["store_id"]:
        raise BackupError("invalid backup store identity")
    anchor = manifest.get("wal_anchor")
    if anchor is not None:
        from uuid import UUID
        try:
            valid = (isinstance(anchor, dict) and
                     set(anchor) in ({"archive_id", "sequence", "sha256"},
                                     {"archive_id", "sequence", "sha256", "effective_at"}) and
                     str(UUID(anchor["archive_id"])) == anchor["archive_id"] and
                     type(anchor["sequence"]) is int and anchor["sequence"] == sequence and
                     isinstance(anchor["sha256"], str) and _SHA256.fullmatch(anchor["sha256"]))
            if valid and "effective_at" in anchor:
                from .wal_archive import _instant
                _instant(anchor["effective_at"])
        except (KeyError, TypeError, ValueError, AttributeError, StoreError):
            valid = False
        if not valid:
            raise BackupError("invalid WAL base anchor")
    entries = manifest.get("files")
    objects = manifest.get("media_objects")
    if not isinstance(entries, list) or not isinstance(objects, list):
        raise BackupError("backup file inventory is absent")
    files: dict[str, dict] = {}
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {"path", "sha256", "size_bytes"}:
            raise BackupError("invalid backup file inventory")
        path, digest, size = entry["path"], entry["sha256"], entry["size_bytes"]
        if not isinstance(path, str) or path in files or \
                not isinstance(digest, str) or not _SHA256.fullmatch(digest) or \
                isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise BackupError("invalid or duplicated backup file")
        files[path] = entry
    required = {database, database + ".meta"}
    optional = {database + ".checkpoint"}
    if not required.issubset(files):
        raise BackupError("backup is missing database files")
    expected_paths = required | (optional & files.keys())
    seen_objects: set[str] = set()
    for obj in objects:
        if not isinstance(obj, dict) or set(obj) != {
                "path", "sha256", "size_bytes", "media_file_ids", "location_ids"}:
            raise BackupError("invalid managed object inventory")
        digest, size = obj["sha256"], obj["size_bytes"]
        if not isinstance(digest, str) or not _SHA256.fullmatch(digest) or \
                digest in seen_objects or isinstance(size, bool) or \
                not isinstance(size, int) or size < 0:
            raise BackupError("invalid or duplicated managed object")
        if obj["path"] != f"{database}.objects/{digest[:2]}/{digest}" or \
                not isinstance(obj["media_file_ids"], list) or \
                not isinstance(obj["location_ids"], list) or \
                not all(isinstance(value, str) for value in obj["media_file_ids"] + obj["location_ids"]):
            raise BackupError("invalid managed object reference")
        if obj["path"] not in files or files[obj["path"]]["sha256"] != digest or \
                files[obj["path"]]["size_bytes"] != size:
            raise BackupError("managed object differs from file inventory")
        seen_objects.add(digest)
        expected_paths.add(obj["path"])
    if set(files) != expected_paths:
        cold = manifest.get("cold_history_files", [])
        if not isinstance(cold, list):
            raise BackupError("invalid cold history inventory")
        for entry in cold:
            if not isinstance(entry, dict) or set(entry) != {"path", "sha256", "size_bytes"} or \
                    not isinstance(entry["path"], str) or entry["path"] not in files or \
                    files[entry["path"]] != entry or entry["path"] in expected_paths:
                raise BackupError("invalid or duplicated cold history file")
            try:
                parts = _platform.safe_relative_parts(entry["path"])
            except ValueError as exc:
                raise BackupError("unsafe cold history path") from exc
            if len(parts) != 3 or parts[0] != database + ".archives" or \
                    parts[2] not in {"records.jsonl", "manifest.json"}:
                raise BackupError("unsafe cold history path")
            try:
                from uuid import UUID
                if str(UUID(parts[1])) != parts[1]:
                    raise ValueError
            except ValueError as exc:
                raise BackupError("invalid cold segment identity") from exc
            expected_paths.add(entry["path"])
        if set(files) != expected_paths:
            raise BackupError("backup inventory contains unreferenced files")
    return database, files, objects


def _inventory(root: Path) -> tuple[set[str], set[str]]:
    """List actual archive contents without following a link or special file."""
    files: set[str] = set()
    directories: set[str] = set()
    stack = [root]
    while stack:
        directory = stack.pop()
        try:
            with os.scandir(directory) as entries:
                for entry in entries:
                    path = Path(entry.path)
                    relative = path.relative_to(root).as_posix()
                    if _platform.entry_is_link(entry):
                        raise BackupError("backup archive contains a symlink")
                    if entry.is_dir(follow_symlinks=False):
                        directories.add(relative)
                        stack.append(path)
                    elif entry.is_file(follow_symlinks=False):
                        files.add(relative)
                    else:
                        raise BackupError("backup archive contains a special file")
        except OSError as exc:
            raise BackupError("cannot inspect backup archive") from exc
    return files, directories


def verify_backup(backup_dir: str | Path) -> dict:
    """Hash every backed-up file and replay the exact committed SDBASE state."""
    raw = Path(backup_dir).expanduser()
    if _platform.is_link(raw) or not raw.is_dir():
        raise BackupError("backup directory is absent or a symlink")
    root = raw.resolve(strict=True)
    manifest = _read_manifest(root)
    database, files, objects = _validate_manifest(manifest)
    actual_files, actual_directories = _inventory(root)
    expected_files = set(files) | {"manifest.json", "manifest.sha256"}
    expected_directories = {
        parent.as_posix() for relative in expected_files
        for parent in list(PurePosixPath(relative).parents)[:-1]
    }
    if actual_files != expected_files or actual_directories != expected_directories:
        raise BackupError("backup archive contains missing or unreferenced files")
    for relative, entry in files.items():
        digest, size = _digest_file(_safe_source(root, relative))
        if digest != entry["sha256"] or size != entry["size_bytes"]:
            raise BackupError(f"backup file checksum/size mismatch: {relative}")
    with _platform.temporary_directory(prefix="sdbase-verify-") as temporary:
        scratch = Path(temporary)
        database_paths = {database, database + ".meta", database + ".checkpoint"}
        for relative in database_paths & files.keys():
            _copy_file(_safe_source(root, relative), scratch / relative,
                       expected_sha=files[relative]["sha256"],
                       expected_size=files[relative]["size_bytes"])
        (scratch / (database + ".lock")).touch(mode=0o600)
        replay = Store(scratch / database,
                       schema_version=manifest.get("schema_version", 1), read_only=True)
        if replay.sequence != manifest["sequence"] or \
                replay._meta().get("store_id") != manifest["store_id"]:
            raise BackupError("backup database identity or sequence differs from manifest")
        if manifest.get("wal_anchor") and replay._meta().get("wal_archive", {}).get("archive_id") != \
                manifest["wal_anchor"]["archive_id"]:
            raise BackupError("WAL base anchor differs from backed up metadata")
        # Store may repair an incomplete journal tail on opening.  It must not
        # have changed any copied database file during verification.
        for relative in database_paths & files.keys():
            digest, size = _digest_file(scratch / relative)
            if digest != files[relative]["sha256"] or size != files[relative]["size_bytes"]:
                raise BackupError("backup database needed crash-tail repair")
        if _media_refs(replay._state, database) != objects:
            raise BackupError("managed object inventory differs from committed records")
        from .retention import archive_file_refs
        if archive_file_refs(replay._state, database) != manifest.get("cold_history_files", []):
            raise BackupError("cold history inventory differs from committed records")
    return {"path": str(root), "database": database,
            "store_id": manifest["store_id"], "sequence": manifest["sequence"],
            "schema_version": manifest.get("schema_version", 1),
            "files": len(files), "media_objects": len(objects)}


def _data(row: Mapping[str, Any]) -> dict:
    return {key: value for key, value in row.items() if key not in {"_id", "_rev"}}


def _commit_updates(store: Store, collection: str, updates: list[tuple[dict, dict]]) -> int:
    count = 0
    tx = store.begin(actor="restore")
    bytes_used = 0
    rows_used = 0
    for original, changed in updates:
        size = len(_json_bytes(changed)) + 512
        if rows_used and (rows_used >= _BATCH_ROWS or bytes_used + size > _BATCH_BYTES):
            tx.commit()
            tx = store.begin(actor="restore")
            bytes_used = rows_used = 0
        tx.put(collection, original["_id"], changed,
               expected_revision=original["_rev"])
        count += 1
        bytes_used += size
        rows_used += 1
    if rows_used:
        tx.commit()
    return count


def _fence_restored_store(store: Store) -> dict[str, int]:
    """Prevent restored remote state from claiming prior live ownership."""
    now = _now()
    counts: dict[str, int] = {}
    endpoints = []
    for row in store.scan("endpoints"):
        if row.get("status") != "retired":
            changed = _data(row) | {"status": "offline", "restore_fenced_at": now}
            endpoints.append((row, changed))
    counts["endpoints_offline"] = _commit_updates(store, "endpoints", endpoints)

    copies = []
    for row in store.scan("endpoint_copies"):
        if row.get("status") != "deleted":
            changed = _data(row) | {
                "status": "pending", "verified_at": None,
                "restore_previous_status": row.get("status"),
                "restore_fenced_at": now,
            }
            copies.append((row, changed))
    counts["copies_pending"] = _commit_updates(store, "endpoint_copies", copies)

    acceptances = []
    for row in store.scan("active_acceptances"):
        changed = _data(row) | {
            "state": "inactive", "restore_previous_state": row.get("state", "active"),
            "restore_previous_engine_epoch": row["engine_epoch"],
            "restore_fenced_at": now,
        }
        acceptances.append((row, changed))
    counts["acceptances_inactive"] = _commit_updates(store, "active_acceptances", acceptances)

    distributions = []
    for row in store.scan("plan_distributions"):
        if row.get("status") in {"sent", "accepted"}:
            changed = _data(row) | {
                "status": "pending", "sent_at": None, "acknowledged_at": None,
                "restore_previous_status": row["status"], "restore_fenced_at": now,
            }
            distributions.append((row, changed))
    counts["distributions_pending"] = _commit_updates(store, "plan_distributions", distributions)

    commands = []
    for row in store.scan("engine_commands"):
        if row.get("state") in {"pending", "accepted"}:
            changed = _data(row) | {
                "state": "expired", "restore_previous_state": row["state"],
                "restore_fenced_at": now, "error_code": "restore_fenced",
            }
            commands.append((row, changed))
    counts["commands_expired"] = _commit_updates(store, "engine_commands", commands)

    tokens = []
    for row in store.scan("tokens"):
        if row.get("scope") == "endpoint" and not row.get("revoked_at"):
            tokens.append((row, _data(row) | {"revoked_at": now,
                                               "restore_fenced_at": now}))
    counts["endpoint_tokens_revoked"] = _commit_updates(store, "tokens", tokens)
    return counts


def _revoke_other_tokens(store: Store) -> int:
    """A separately provisioned copy must not inherit any source credentials."""
    now = _now()
    tokens = [(row, _data(row) | {"revoked_at": now, "provision_fenced_at": now})
              for row in store.scan("tokens") if not row.get("revoked_at")]
    return _commit_updates(store, "tokens", tokens)


def _issue_provision_admin(store: Store) -> dict[str, str]:
    """Issue one new copy-only credential; persist its hash, never its secret."""
    user_id = str(uuid4())
    username = "provision-admin-" + uuid4().hex
    token = secrets.token_urlsafe(32)
    now = datetime.now(timezone.utc)
    tx = store.begin(actor="provision")
    tx.put("users", user_id, {
        "username": username, "display_name": "Provision administrator",
        "status": "active", "created_at": now.isoformat(),
        "provisioned_at": now.isoformat(),
    })
    for permission in ("view", "edit", "bulk_edit", "import", "approve",
                       "activate", "operate", "repair", "manage"):
        tx.put("permissions", str(uuid4()), {
            "user_id": user_id, "permission": permission,
            "station_id": None, "station_scope": "*",
        })
    tx.put("tokens", str(uuid4()), {
        "user_id": user_id, "token_hash": hashlib.sha256(token.encode()).hexdigest(),
        "expires_at": (now + timedelta(days=30)).isoformat(),
        "scope": "user", "revoked_at": None,
    })
    tx.commit()
    return {"admin_user_id": user_id, "admin_username": username,
            "admin_token": token}


def _rotate_store_identity(store: Store) -> tuple[str, str]:
    """Change only staging metadata, after all copied credentials are fenced."""
    with store._locked(exclusive=True):
        store._reload_locked()
        meta = store._meta()
        old_id = meta.get("store_id")
        if not isinstance(old_id, str) or not old_id:
            raise BackupError("copied database has no store identity")
        new_id = str(uuid4())
        while new_id == old_id:
            new_id = str(uuid4())
        meta["store_id"] = new_id
        meta.update(node_role="primary", node_id=str(uuid4()), timeline_id=str(uuid4()), epoch=1)
        meta.pop("wal_archive", None)
        meta.pop("replication", None)
        meta.pop("promotion_pending", None)
        meta.pop("handover", None)
        _atomic_json(store.meta_file, meta)
        return old_id, new_id


def _prepare_staging_copy(database: Path) -> None:
    """Detach an unpublished restoration from its source writer and archive."""
    metadata = database.with_name(database.name + ".meta")
    with os.fdopen(_open_regular(metadata), "rb") as stream:
        meta = json.load(stream)
    meta["restoration_source"] = {field: meta.get(field) for field in
                                  ("store_id", "node_id", "timeline_id", "epoch", "node_role")}
    meta.update(node_role="primary", node_id=str(uuid4()), timeline_id=str(uuid4()), epoch=1)
    meta.pop("wal_archive", None)
    meta.pop("replication", None)
    meta.pop("promotion_pending", None)
    meta.pop("handover", None)
    _atomic_json(metadata, meta)


def _verify_fenced_store(store: Store, *, allowed_token_hash: str | None = None) -> None:
    """Refuse to publish a copy that retained live source ownership."""
    if any(row["status"] not in {"offline", "retired"} for row in store.scan("endpoints")):
        raise BackupError("copied endpoint is still online")
    if any(row["status"] not in {"pending", "deleted"}
           for row in store.scan("endpoint_copies")):
        raise BackupError("copied endpoint media has not been revalidated")
    if any(row.get("state") != "inactive" for row in store.scan("active_acceptances")):
        raise BackupError("copied endpoint still has an active acceptance")
    if any(row.get("state") in {"pending", "accepted"} for row in store.scan("engine_commands")):
        raise BackupError("copied engine command remains live")
    if any(row.get("status") in {"sent", "accepted"}
           for row in store.scan("plan_distributions")):
        raise BackupError("copied plan distribution remains live")
    if any(not row.get("revoked_at") and
           (row.get("scope") == "endpoint" or
            allowed_token_hash is not None and row.get("token_hash") != allowed_token_hash)
           for row in store.scan("tokens")):
        raise BackupError("copied credential remains active")


def restore_backup(backup_dir: str | Path, target_dir: str | Path, *,
                   provision_copy: bool = False) -> dict:
    """Verify, fence, and atomically publish a restored store at an absent path."""
    target = _destination(target_dir)
    verified = verify_backup(backup_dir)
    source = Path(verified["path"])
    if target.is_relative_to(source):
        raise BackupError("restore target cannot be inside its source backup")
    manifest = _read_manifest(source)
    database, files, _objects = _validate_manifest(manifest)
    from .schema import SCHEMA_VERSION
    if manifest.get("schema_version", 1) != SCHEMA_VERSION:
        raise BackupError("backup schema differs from this package; use migrate-backup for v1 data")
    stage = Path(_platform.mkdtemp_private(prefix=f".{target.name}.", suffix=".restore",
                                           dir=target.parent))
    try:
        for relative, entry in files.items():
            _copy_file(_safe_source(source, relative), stage / relative,
                       expected_sha=entry["sha256"],
                       expected_size=entry["size_bytes"])
        validated = open_radio_store(stage / database)
        if validated.sequence != manifest["sequence"] or \
                validated._meta().get("store_id") != manifest["store_id"]:
            raise BackupError("restored database changed during copy")
        # A restore must fence even a final accepted delivery. Normal live
        # transition guards deliberately forbid changing accepted rows, so
        # apply only the explicit restoration mutations in this isolated
        # staging copy, then reopen with the complete RadioS schema below.
        _prepare_staging_copy(stage / database)
        store = Store(stage / database, schema_version=SCHEMA_VERSION)
        counts = _fence_restored_store(store)
        credential: dict[str, str] = {}
        if provision_copy:
            counts["other_tokens_revoked"] = _revoke_other_tokens(store)
            credential = _issue_provision_admin(store)
        final_sequence = store.checkpoint()
        store_id = manifest["store_id"]
        if provision_copy:
            _source_id, store_id = _rotate_store_identity(store)
        reopened = open_radio_store(stage / database)
        if reopened.sequence != final_sequence or reopened._meta().get("store_id") != store_id:
            raise BackupError("staged database failed reopening after fencing")
        _verify_fenced_store(
            reopened, allowed_token_hash=(hashlib.sha256(credential["admin_token"].encode()).hexdigest()
                                          if provision_copy else None))
        if provision_copy:
            from .identity import authenticate
            if authenticate(reopened, credential["admin_token"]).user_id != credential["admin_user_id"]:
                raise BackupError("new provision administrator credential is unusable")
        _fsync_tree(stage)
        _publish_absent(stage, target)
        return {"path": str(target), "database": database,
                "store_id": store_id,
                "source_sequence": manifest["sequence"],
                "restored_sequence": final_sequence,
                "media_objects": verified["media_objects"], **counts,
                **credential,
                **({"source_store_id": manifest["store_id"]} if provision_copy else {})}
    finally:
        if os.path.lexists(stage):
            _platform.rmtree(stage, ignore_errors=True)
