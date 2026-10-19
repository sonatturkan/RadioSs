"""Small durable document engine with atomic transactions and revision checks.

Records live in an append-only, checksummed journal. A checkpoint contains a
complete point-in-time image; every journal frame after it has a monotonic
sequence number. Linux advisory locks serialize processes, and fsync makes an
acknowledged commit durable on a filesystem that honors fsync. This is SDBASE's
own storage format and query API; it does not execute SQL.
"""

from __future__ import annotations

from collections.abc import Mapping as MappingABC
from contextlib import contextmanager
from copy import deepcopy
from dataclasses import dataclass
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import struct
import threading
import time
from typing import Any, Callable, Iterable, Iterator, Mapping
from uuid import UUID, uuid4


_MAX_FRAME = 32 * 1024 * 1024
_FORMAT = "sdbase-journal-v1"
ANY = object()
_DELETED = object()
_MISSING = object()


@dataclass(frozen=True)
class IndexSpec:
    fields: tuple[str, ...]
    unique: bool = False


def _index_key(data: dict, spec: IndexSpec) -> tuple | None:
    values = tuple(data.get(field) for field in spec.fields)
    if any(value is None for value in values):
        return None
    if any(not isinstance(value, (str, int, float, bool)) for value in values):
        raise StoreError("indexed fields must contain scalar values")
    return values


def _unique_key_digest(value: tuple) -> str:
    """Match Python index equality: True, 1 and 1.0 name the same key."""
    normalized = tuple(int(part) if isinstance(part, bool) or
                       isinstance(part, float) and part.is_integer() else part for part in value)
    return hashlib.sha256(_json_bytes(normalized)).hexdigest()


class StoreError(RuntimeError):
    pass


class Conflict(StoreError):
    pass


class Corruption(StoreError):
    pass


class WriteRejected(StoreError):
    """A standby, fenced node or obsolete writer handle cannot commit."""


class LockTimeout(StoreError):
    """A bounded lock wait elapsed without acquiring the database lock."""


class _OverlayRecords(MappingABC[str, dict]):
    """A transaction's proposed rows without copying its entire collection.

    Validators may inspect cross-record references and iterate a collection.
    The base remains untouched until the journal frame is durable; changed
    keys shadow it and deletions are invisible in this read-only view.
    """

    def __init__(self, base: Mapping[str, dict], changes: Mapping[str, dict | object]):
        self.base = base
        self.changes = changes

    def __getitem__(self, key: str) -> dict:
        value = self.changes.get(key, _MISSING)
        if value is _DELETED:
            raise KeyError(key)
        if value is not _MISSING:
            return value
        return self.base[key]

    def __iter__(self) -> Iterator[str]:
        for key in self.base:
            if self.changes.get(key, _MISSING) is not _DELETED:
                yield key
        for key, value in self.changes.items():
            if key not in self.base and value is not _DELETED:
                yield key

    def __len__(self) -> int:
        size = len(self.base)
        for key, value in self.changes.items():
            if key in self.base:
                size -= value is _DELETED
            else:
                size += value is not _DELETED
        return size


def _json_bytes(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False).encode("utf-8")


def _atomic_json(path: Path, value: Any) -> None:
    temp = path.with_name(f".{path.name}.{uuid4().hex}.tmp")
    try:
        with temp.open("xb") as stream:
            stream.write(_json_bytes(value))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
        directory_fd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        temp.unlink(missing_ok=True)


def _frame(value: Mapping[str, Any]) -> bytes:
    data = _json_bytes(value)
    if len(data) > _MAX_FRAME:
        raise StoreError("transaction exceeds maximum journal frame")
    return struct.pack(">I", len(data)) + data + hashlib.sha256(data).digest()


class Store:
    """Process-safe JSON document store.

    ``expected_revision=None`` means create only; an integer means compare and
    swap; ``ANY`` explicitly opts out of revision protection. Deleted IDs are
    tombstoned permanently. ``immutable_collections`` forbid update/delete.
    """

    def __init__(self, root: str | Path, *, immutable_collections: set[str] | None = None,
                 validators: Mapping[str, Callable[[dict, Mapping[str, dict]], None]] | None = None,
                 indexes: Mapping[str, Mapping[str, IndexSpec]] | None = None,
                 transition_guards: Mapping[str, Callable[[dict | None, dict | None], None]] | None = None,
                 non_deletable_collections: set[str] | None = None,
                 schema_version: int = 1, read_only: bool = False,
                 lock_timeout: float | None = 30.0):
        requested = Path(root).resolve()
        if requested.suffix == ".sdb":
            self.root = requested.parent
            self.journal = requested
        else:
            self.root = requested
            self.journal = self.root / "RadioS.sdb"
        self.read_only = read_only
        if lock_timeout is not None and (isinstance(lock_timeout, bool) or
                not isinstance(lock_timeout, (int, float)) or not 0 < lock_timeout <= 3600):
            raise StoreError("lock_timeout must be positive and at most 3600 seconds")
        self.lock_timeout = lock_timeout
        self._write_identity = None
        from .engine_metrics import EngineMetrics
        self.metrics = EngineMetrics()
        if not read_only:
            self.root.mkdir(parents=True, exist_ok=True)
        self.checkpoint_file = self.journal.with_name(self.journal.name + ".checkpoint")
        self.meta_file = self.journal.with_name(self.journal.name + ".meta")
        self.lock_file = self.journal.with_name(self.journal.name + ".lock")
        self.immutable_collections = frozenset(immutable_collections or ()) | {"_audit"}
        self.validators = dict(validators or {})
        self.transition_guards = dict(transition_guards or {})
        self.non_deletable_collections = frozenset(non_deletable_collections or ())
        if isinstance(schema_version, bool) or not isinstance(schema_version, int) or schema_version < 1:
            raise StoreError("schema version must be a positive integer")
        self.schema_version = schema_version
        self.index_specs = {collection: dict(specs) for collection, specs in (indexes or {}).items()}
        self._indices: dict[tuple[str, str], dict[tuple, set[str]]] = {}
        self._mutex = threading.RLock()
        self._state: dict[str, dict[str, dict]] = {}
        self._tombstones: dict[str, set[str]] = {}
        self._retired_unique_keys: dict[str, dict[str, set[str]]] = {}
        self._collection_sequences: dict[str, int] = {}
        self._sequence = 0
        self._offset = 0
        self._generation = -1
        self._checkpoint_stamp = None
        if read_only:
            if not self.journal.is_file() or not self.meta_file.is_file() or \
                    not self.lock_file.is_file() or any(path.is_symlink() for path in
                                                       (self.journal, self.meta_file,
                                                        self.checkpoint_file, self.lock_file)):
                raise StoreError("read-only database needs existing journal, metadata and lock")
            with self._locked(exclusive=False):
                self._reload_locked()
        else:
            self.lock_file.touch(exist_ok=True)
            with self._locked(exclusive=True):
                if not self.meta_file.exists():
                    _atomic_json(self.meta_file, {"format": _FORMAT, "store_id": str(uuid4()),
                                                  "generation": 0,
                                                  "schema_version": schema_version,
                                                  "node_role": "primary", "node_id": str(uuid4()),
                                                  "timeline_id": str(uuid4()), "epoch": 1})
                meta = self._meta()
                if "node_role" not in meta and schema_version < 4:
                    meta.update(node_role="primary", node_id=str(uuid4()),
                                timeline_id=str(uuid4()), epoch=1)
                    _atomic_json(self.meta_file, meta)
                    self._write_identity = tuple(meta[field] for field in ("node_id", "timeline_id", "epoch"))
                if meta.get("node_role", "primary") == "primary":
                    self.journal.touch(exist_ok=True)
                elif not self.journal.is_file():
                    raise Corruption("standby journal is absent")
                self._reload_locked(repair_tail=meta.get("node_role", "primary") == "primary")

    @contextmanager
    def _locked(self, *, exclusive: bool) -> Iterator[None]:
        start = time.monotonic()
        acquired = self._mutex.acquire() if self.lock_timeout is None else \
            self._mutex.acquire(timeout=self.lock_timeout)
        if not acquired:
            self.metrics.increment("lock_timeouts")
            raise LockTimeout("database process lock wait timed out")
        try:
            with self.lock_file.open("rb") as lock:
                mode = fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH
                if self.lock_timeout is None:
                    fcntl.flock(lock.fileno(), mode)
                else:
                    deadline = start + self.lock_timeout
                    while True:
                        try:
                            fcntl.flock(lock.fileno(), mode | fcntl.LOCK_NB)
                            break
                        except BlockingIOError:
                            remaining = deadline - time.monotonic()
                            if remaining <= 0:
                                self.metrics.increment("lock_timeouts")
                                raise LockTimeout("database file lock wait timed out")
                            time.sleep(min(0.01, remaining))
                self.metrics.observe("lock_wait_ms", (time.monotonic() - start) * 1000)
                try:
                    yield
                finally:
                    fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
        finally:
            self._mutex.release()

    def _meta(self) -> dict:
        try:
            meta = json.loads(self.meta_file.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise Corruption("store metadata is unreadable") from exc
        if not isinstance(meta, dict) or meta.get("format") != _FORMAT or \
                type(meta.get("generation")) is not int or meta["generation"] < 0:
            raise Corruption("unsupported store metadata")
        try:
            if str(UUID(meta["store_id"])) != meta["store_id"]:
                raise ValueError
        except (KeyError, TypeError, ValueError, AttributeError) as exc:
            raise Corruption("invalid store identity") from exc
        installed_schema = meta.get("schema_version", 1)
        if type(installed_schema) is not int or installed_schema < 1:
            raise Corruption("invalid database schema version")
        if installed_schema != self.schema_version:
            raise StoreError(f"database schema version {installed_schema} requires a compatible package")
        if installed_schema >= 4 or "node_role" in meta:
            if meta.get("node_role") not in {"primary", "standby", "fenced"} or \
                    type(meta.get("epoch")) is not int or meta["epoch"] < 1:
                raise Corruption("invalid database role or epoch")
            for field in ("node_id", "timeline_id"):
                try:
                    if str(UUID(meta[field])) != meta[field]:
                        raise ValueError
                except (KeyError, TypeError, ValueError, AttributeError) as exc:
                    raise Corruption(f"invalid database {field}") from exc
        if self._write_identity is None:
            self._write_identity = tuple(meta.get(field) for field in ("node_id", "timeline_id", "epoch"))
        return meta

    def _assert_writable_locked(self) -> None:
        if self.read_only:
            raise WriteRejected("read-only database cannot write")
        meta = self._meta()
        if meta.get("node_role", "primary") != "primary":
            self.metrics.increment("writes_rejected")
            raise WriteRejected("database node is not the writable primary")
        if tuple(meta.get(field) for field in ("node_id", "timeline_id", "epoch")) != self._write_identity:
            self.metrics.increment("writes_rejected")
            raise WriteRejected("writer timeline or epoch changed; reopen the database")

    def _read_checkpoint(self) -> tuple[int, dict, dict, dict[str, int], dict]:
        if not self.checkpoint_file.exists():
            return 0, {}, {}, {}, {}
        try:
            envelope = json.loads(self.checkpoint_file.read_text(encoding="utf-8"))
            payload = envelope["payload"]
            if hashlib.sha256(_json_bytes(payload)).hexdigest() != envelope["sha256"]:
                raise Corruption("checkpoint checksum mismatch")
            if payload["format"] != _FORMAT:
                raise Corruption("unsupported checkpoint format")
            sequence = payload["sequence"]
            if type(sequence) is not int or sequence < 0:
                raise Corruption("invalid checkpoint sequence")
            state = payload["state"]
            tombstones = {
                collection: set(keys) for collection, keys in payload["tombstones"].items()
            }
            retired = payload.get("retired_unique_keys", {})
            if not isinstance(retired, dict) or any(
                    not isinstance(collection, str) or not isinstance(indexes, dict) or any(
                        not isinstance(name, str) or not isinstance(digests, list) or any(
                            not isinstance(digest, str) or len(digest) != 64 or any(
                                char not in "0123456789abcdef" for char in digest)
                            for digest in digests) for name, digests in indexes.items())
                    for collection, indexes in retired.items()):
                raise Corruption("invalid retired unique index keys")
            retired = {collection: {name: set(digests) for name, digests in indexes.items()}
                       for collection, indexes in retired.items()}
            generations = payload.get("collection_sequences")
            if generations is None:
                # Checkpoints from before per-collection generations existed
                # cannot reveal the precise last write.  Conservatively mark
                # every collection that ever held an ID as changed at the
                # checkpoint sequence; later checkpoints record exact values.
                generations = {name: sequence for name in state.keys() | tombstones.keys()}
            elif not isinstance(generations, dict) or any(
                    not isinstance(name, str) or not name or
                    type(value) is not int or value < 0 or value > sequence
                    for name, value in generations.items()):
                raise Corruption("invalid checkpoint collection generation")
            return sequence, state, tombstones, generations, retired
        except (OSError, ValueError, KeyError, TypeError) as exc:
            raise Corruption("checkpoint is unreadable") from exc

    def _apply(self, txn: dict) -> None:
        seq = txn.get("sequence")
        if type(seq) is not int or seq != self._sequence + 1:
            raise Corruption("journal sequence gap or duplicate")
        operations = txn.get("operations")
        if not isinstance(operations, list) or not operations:
            raise Corruption("journal transaction has no operations")
        seen: set[tuple[str, str]] = set()
        for operation in operations:
            if not isinstance(operation, dict):
                raise Corruption("invalid journal operation")
            collection = operation.get("collection")
            key = operation.get("key")
            kind = operation.get("kind")
            if not isinstance(collection, str) or not collection or \
                    not isinstance(key, str) or not key or \
                    kind not in {"put", "delete"} or (collection, key) in seen:
                raise Corruption("invalid or duplicated journal operation")
            if kind == "put" and (not isinstance(operation.get("record"), dict) or
                                  not isinstance(operation["record"].get("data"), dict)):
                raise Corruption("invalid journal record")
            seen.add((collection, key))
        # Remove all previous index keys before adding replacements.  A single
        # valid transaction may exchange two unique values between documents.
        for operation in operations:
            collection = operation["collection"]
            key = operation["key"]
            prior = self._state.get(collection, {}).get(key)
            if prior is not None:
                self._index_remove(collection, key, prior["data"])
        for operation in operations:
            collection = operation["collection"]
            key = operation["key"]
            if operation["kind"] == "put":
                self._state.setdefault(collection, {})[key] = operation["record"]
                self._index_add(collection, key, operation["record"]["data"])
            else:
                self._state.setdefault(collection, {}).pop(key, None)
                self._tombstones.setdefault(collection, set()).add(key)
            self._collection_sequences[collection] = seq
        self._sequence = seq

    def _index_remove(self, collection: str, key: str, data: dict) -> None:
        for name, spec in self.index_specs.get(collection, {}).items():
            value = _index_key(data, spec)
            if value is not None:
                bucket = self._indices.get((collection, name), {}).get(value)
                if bucket:
                    bucket.discard(key)
                    if not bucket:
                        self._indices[(collection, name)].pop(value, None)

    def _index_add(self, collection: str, key: str, data: dict) -> None:
        for name, spec in self.index_specs.get(collection, {}).items():
            value = _index_key(data, spec)
            if value is not None:
                bucket = self._indices.setdefault((collection, name), {}).setdefault(value, set())
                if spec.unique and any(other != key for other in bucket):
                    raise Corruption(f"unique index {collection}.{name} has duplicate stored values")
                bucket.add(key)

    def _rebuild_indexes(self) -> None:
        self._indices = {}
        for collection, records in self._state.items():
            for key, record in records.items():
                self._index_add(collection, key, record["data"])

    def _reload_locked(self, *, repair_tail: bool = False) -> None:
        meta = self._meta()
        if repair_tail:
            self._assert_writable_locked()
        journal_size = self.journal.stat().st_size
        checkpoint_stat = self.checkpoint_file.stat() if self.checkpoint_file.exists() else None
        checkpoint_stamp = ((checkpoint_stat.st_ino, checkpoint_stat.st_size,
                             checkpoint_stat.st_mtime_ns) if checkpoint_stat else None)
        if meta["generation"] != self._generation or journal_size < self._offset or \
                checkpoint_stamp != self._checkpoint_stamp:
            (self._sequence, self._state, self._tombstones,
             self._collection_sequences, self._retired_unique_keys) = self._read_checkpoint()
            self._offset = 0
            self._generation = meta["generation"]
            self._checkpoint_stamp = checkpoint_stamp
            self._rebuild_indexes()
        with self.journal.open("r+b" if repair_tail else "rb") as stream:
            stream.seek(self._offset)
            while True:
                frame_start = stream.tell()
                header = stream.read(4)
                if not header:
                    break
                if len(header) < 4:
                    if repair_tail:
                        stream.truncate(frame_start)
                        stream.flush()
                        os.fsync(stream.fileno())
                        break
                    raise Corruption("incomplete journal tail")
                length = struct.unpack(">I", header)[0]
                if length <= 0 or length > _MAX_FRAME:
                    raise Corruption("invalid journal frame size")
                data = stream.read(length)
                checksum = stream.read(32)
                if len(data) != length or len(checksum) != 32:
                    if repair_tail:
                        stream.truncate(frame_start)
                        stream.flush()
                        os.fsync(stream.fileno())
                        break
                    raise Corruption("incomplete journal tail")
                if hashlib.sha256(data).digest() != checksum:
                    raise Corruption("journal checksum mismatch")
                try:
                    txn = json.loads(data)
                except ValueError as exc:
                    raise Corruption("journal frame is invalid JSON") from exc
                if txn.get("sequence", 0) <= self._sequence:
                    # A crash after checkpoint replacement but before journal
                    # truncation leaves old frames in place. They are already
                    # represented by the checkpoint.
                    self._offset = stream.tell()
                    continue
                self._apply(txn)
                self._offset = stream.tell()

    def refresh(self) -> None:
        with self._locked(exclusive=False):
            self._reload_locked()

    @property
    def sequence(self) -> int:
        with self._locked(exclusive=False):
            self._reload_locked()
            return self._sequence

    def collection_generation(self, collection_names: Iterable[str]) -> int:
        """Last committed write to any named collection, independent of other writes."""
        if isinstance(collection_names, str):
            collection_names = (collection_names,)
        names = tuple(collection_names)
        if any(not isinstance(name, str) or not name for name in names):
            raise StoreError("collection names must be nonempty strings")
        with self._locked(exclusive=False):
            self._reload_locked()
            return max((self._collection_sequences.get(name, 0) for name in names), default=0)

    def get(self, collection: str, key: str) -> dict | None:
        with self._locked(exclusive=False):
            self._reload_locked()
            record = self._state.get(collection, {}).get(str(key))
            return deepcopy(record["data"] | {"_id": str(key), "_rev": record["revision"]}) if record else None

    def scan(self, collection: str) -> list[dict]:
        with self._locked(exclusive=False):
            self._reload_locked()
            return [deepcopy(record["data"] | {"_id": key, "_rev": record["revision"]})
                    for key, record in self._state.get(collection, {}).items()]

    def lookup(self, collection: str, index_name: str, *values: str | int | float | bool) -> list[dict]:
        """Return an exact-match indexed result in stable ID order."""
        with self._locked(exclusive=False):
            self._reload_locked()
            spec = self.index_specs.get(collection, {}).get(index_name)
            if spec is None or len(values) != len(spec.fields):
                raise StoreError("unknown index or wrong key arity")
            ids = self._indices.get((collection, index_name), {}).get(tuple(values), set())
            return [deepcopy(self._state[collection][key]["data"] |
                             {"_id": key, "_rev": self._state[collection][key]["revision"]})
                    for key in sorted(ids)]

    def begin(self, *, actor: str = "system") -> Transaction:
        return Transaction(self, actor)

    def read_snapshot(self, collections: Iterable[str] | None = None):
        from .read_snapshot import ReadSnapshot
        return ReadSnapshot.capture(self, collections)

    def put(self, collection: str, key: str, data: dict, *, expected_revision: int | None | object = None,
            actor: str = "system") -> dict:
        tx = self.begin(actor=actor)
        tx.put(collection, key, data, expected_revision=expected_revision)
        return tx.commit()[0]

    def delete(self, collection: str, key: str, *, expected_revision: int,
               actor: str = "system") -> None:
        tx = self.begin(actor=actor)
        tx.delete(collection, key, expected_revision=expected_revision)
        tx.commit()

    def checkpoint(self) -> int:
        """Atomically replace the recovery image and retire old journal frames."""
        started = time.monotonic()
        try:
            return self._checkpoint()
        finally:
            self.metrics.observe("checkpoint_ms", (time.monotonic() - started) * 1000)

    def checkpoint_online(self, **limits) -> dict:
        from .online_maintenance import checkpoint_online
        return checkpoint_online(self, **limits)

    def _checkpoint(self) -> int:
        if self.read_only:
            raise StoreError("read-only database cannot checkpoint")
        with self._locked(exclusive=True):
            self._assert_writable_locked()
            self._reload_locked(repair_tail=True)
            from .wal_archive import archive_pending_locked
            archive_pending_locked(self)
            payload = {"format": _FORMAT, "sequence": self._sequence,
                       "state": self._state,
                       "tombstones": {name: sorted(ids) for name, ids in self._tombstones.items()},
                       "retired_unique_keys": {collection: {name: sorted(values) for name, values in indexes.items()}
                                               for collection, indexes in self._retired_unique_keys.items()},
                       "collection_sequences": self._collection_sequences}
            _atomic_json(self.checkpoint_file, {
                "payload": payload, "sha256": hashlib.sha256(_json_bytes(payload)).hexdigest()})
            with self.journal.open("r+b") as stream:
                stream.truncate(0)
                stream.flush()
                os.fsync(stream.fileno())
            meta = self._meta()
            meta["generation"] += 1
            _atomic_json(self.meta_file, meta)
            self._generation = meta["generation"]
            self._offset = 0
            checkpoint_stat = self.checkpoint_file.stat()
            self._checkpoint_stamp = (checkpoint_stat.st_ino, checkpoint_stat.st_size,
                                      checkpoint_stat.st_mtime_ns)
            self.metrics.increment("checkpoints")
            return self._sequence


class Transaction:
    def __init__(self, store: Store, actor: str):
        self.store = store
        self.actor = actor
        self.operations: list[tuple[str, str, str, dict | None, int | None | object]] = []
        self.read_checks: list[tuple[str, str, int | None]] = []
        self.sequence_check: int | None = None
        self._done = False

    def expect(self, collection: str, key: str, revision: int | None) -> Transaction:
        """Fail commit if a read-only dependency changed meanwhile."""
        if self._done:
            raise StoreError("transaction already committed")
        self.read_checks.append((collection, str(key), revision))
        return self

    def expect_sequence(self, sequence: int) -> Transaction:
        """Bind a prepared operation to the exact database snapshot it saw."""
        if self._done or isinstance(sequence, bool) or not isinstance(sequence, int) or sequence < 0:
            raise StoreError("invalid transaction sequence precondition")
        self.sequence_check = sequence
        return self

    def put(self, collection: str, key: str, data: dict, *,
            expected_revision: int | None | object = None) -> Transaction:
        if self._done:
            raise StoreError("transaction already committed")
        if not isinstance(data, dict) or not collection or not key:
            raise StoreError("collection, key and object data are required")
        if collection == "_audit":
            raise StoreError("audit collection is managed by the engine")
        if any(name in data for name in ("_id", "_rev")):
            raise StoreError("_id and _rev are managed by the engine")
        _json_bytes(data)  # Reject unserializable values before taking the write lock.
        self.operations.append(("put", collection, str(key), deepcopy(data), expected_revision))
        return self

    def delete(self, collection: str, key: str, *, expected_revision: int) -> Transaction:
        if self._done:
            raise StoreError("transaction already committed")
        if collection == "_audit":
            raise StoreError("audit collection is managed by the engine")
        self.operations.append(("delete", collection, str(key), None, expected_revision))
        return self

    def commit(self) -> list[dict]:
        started = time.monotonic()
        try:
            return self._commit()
        except Conflict:
            self.store.metrics.increment("conflicts")
            raise
        finally:
            self.store.metrics.observe("commit_ms", (time.monotonic() - started) * 1000)

    def _commit(self) -> list[dict]:
        if self._done:
            raise StoreError("transaction already committed")
        if not self.operations:
            raise StoreError("empty transaction")
        store = self.store
        if store.read_only:
            raise StoreError("read-only database cannot commit")
        with store._locked(exclusive=True):
            store._assert_writable_locked()
            store._reload_locked(repair_tail=True)
            from .wal_archive import archive_pending_locked
            archive_pending_locked(store)
            if self.sequence_check is not None and store._sequence != self.sequence_check:
                raise Conflict("database changed since preview")
            for collection, key, revision in self.read_checks:
                current = store._state.get(collection, {}).get(key)
                actual = current["revision"] if current else None
                if actual != revision:
                    raise Conflict(f"{collection}/{key} changed before commit")
            seen: set[tuple[str, str]] = set()
            prepared: list[dict] = []
            outputs: list[dict] = []
            for kind, collection, key, data, expected in self.operations:
                identity = (collection, key)
                if identity in seen:
                    raise StoreError("one transaction may change a document only once")
                seen.add(identity)
                prior = store._state.get(collection, {}).get(key)
                if expected is not ANY:
                    if expected is None and prior is not None:
                        raise Conflict(f"{collection}/{key} already exists")
                    if isinstance(expected, int) and not isinstance(expected, bool) and (prior is None or prior["revision"] != expected):
                        raise Conflict(f"{collection}/{key} revision changed")
                    if expected is not None and (not isinstance(expected, int) or isinstance(expected, bool)):
                        raise StoreError("expected_revision must be None, an integer or ANY")
                if prior is None and key in store._tombstones.get(collection, set()):
                    raise Conflict(f"{collection}/{key} is tombstoned")
                if collection in store.immutable_collections and (kind == "delete" or prior is not None):
                    raise Conflict(f"{collection} is append-only")
                if kind == "delete" and collection in store.non_deletable_collections:
                    raise Conflict(f"{collection} must be archived rather than deleted")
                if guard := store.transition_guards.get(collection):
                    guard(prior["data"] if prior else None, data if kind == "put" else None)
                if kind == "delete":
                    if prior is None:
                        raise Conflict(f"{collection}/{key} is absent")
                    prepared.append({"kind": kind, "collection": collection, "key": key})
                    continue
                assert data is not None
                revision = 1 if prior is None else prior["revision"] + 1
                record = {"revision": revision, "data": data}
                prepared.append({"kind": kind, "collection": collection,
                                 "key": key, "record": record})
                outputs.append(deepcopy(data | {"_id": key, "_rev": revision}))
            prospective = dict(store._state)
            changes_by_collection: dict[str, dict[str, dict | object]] = {}
            for op in prepared:
                collection, key = op["collection"], op["key"]
                changes_by_collection.setdefault(collection, {})[key] = (
                    op["record"] if op["kind"] == "put" else _DELETED)
            for collection, changes in changes_by_collection.items():
                prospective[collection] = _OverlayRecords(
                    store._state.get(collection, {}), changes)
            for op in prepared:
                if op["kind"] == "put" and (validator := store.validators.get(op["collection"])):
                    validator(op["record"]["data"], prospective)
            # Validate every index key before appending the journal frame.
            # _apply() also builds non-unique indexes; a malformed value there
            # would otherwise make an acknowledged journal impossible to replay.
            # Check uniqueness against current documents not changed in this
            # transaction, and against all new values in the same transaction.
            changed = {(op["collection"], op["key"]) for op in prepared}
            occupied: dict[tuple[str, str, tuple], str] = {}
            for op in prepared:
                if op["kind"] != "put":
                    continue
                collection, key = op["collection"], op["key"]
                for name, spec in store.index_specs.get(collection, {}).items():
                    value = _index_key(op["record"]["data"], spec)
                    if value is None or not spec.unique:
                        continue
                    retired = store._retired_unique_keys.get(collection, {}).get(name, set())
                    if _unique_key_digest(value) in retired:
                        raise Conflict(f"unique index {collection}.{name} belongs to archived history; query cold history")
                    token = (collection, name, value)
                    existing = store._indices.get((collection, name), {}).get(value, set())
                    if any((collection, other) not in changed for other in existing):
                        raise Conflict(f"unique index {collection}.{name} already has {value}")
                    if token in occupied and occupied[token] != key:
                        raise Conflict(f"unique index {collection}.{name} duplicated in transaction")
                    occupied[token] = key
            transaction_id = str(uuid4())
            committed_at = datetime.now(timezone.utc).isoformat()
            changes = []
            for op in prepared:
                prior = store._state.get(op["collection"], {}).get(op["key"])
                changes.append({"collection": op["collection"], "key": op["key"],
                                "before_revision": prior["revision"] if prior else None,
                                "before": prior["data"] if prior else None,
                                "after_revision": op["record"]["revision"] if op["kind"] == "put" else None,
                                "after": op["record"]["data"] if op["kind"] == "put" else None})
            prepared.append({"kind": "put", "collection": "_audit", "key": transaction_id,
                             "record": {"revision": 1, "data": {
                                 "sequence": store._sequence + 1, "actor": self.actor,
                                 "committed_at": committed_at, "changes": changes}}})
            txn = {"format": _FORMAT, "sequence": store._sequence + 1,
                   "transaction_id": transaction_id, "actor": self.actor,
                   "committed_at": committed_at, "operations": prepared}
            frame = _frame(txn)
            with store.journal.open("ab") as stream:
                stream.write(frame)
                stream.flush()
                os.fsync(stream.fileno())
            store._apply(txn)
            store._offset += len(frame)
            self._done = True
            store.metrics.increment("commits")
            store.metrics.increment("journal_bytes_written", len(frame))
            from .wal_archive import archive_transaction_locked
            archive_transaction_locked(store, txn)
            return outputs
