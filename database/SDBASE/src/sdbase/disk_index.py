"""Rebuildable ordered disk index caches for immutable SDBASE read views.

The authoritative journal/checkpoint remains SDBASE's document store. Sidecars
contain sorted typed JSON key/ID pairs plus a checksummed binary offset table.
They provide mmap/bisect range reads without a RAM array of all sorted keys.
"""

from __future__ import annotations

from collections import OrderedDict
from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import hashlib
import heapq
import json
import math
import mmap
import os
from pathlib import Path
import shutil
import stat
import struct
import tempfile
from threading import Lock
import time
from uuid import UUID, uuid4

from .storage import Conflict, Store, StoreError, _atomic_json, _index_key, _json_bytes


_FORMAT = "sdbase-disk-index-v1"
_BUILD_FORMAT = "sdbase-disk-index-build-v1"
_MAGIC = b"SDBIDX01"
_HEADER = struct.Struct(">8sQQ")
_OFFSET = struct.Struct(">Q")
_MAX_LINE = 32 * 1024 * 1024
_MAX_MANIFEST = 4 * 1024 * 1024
_MAX_VERIFY_BYTES = 2 * 1024**3
_VERIFIED = OrderedDict()
_VERIFIED_LAYOUT = OrderedDict()
_VERIFY_LOCK = Lock()


class DiskIndexError(StoreError):
    pass


class DiskIndexUnavailable(DiskIndexError):
    """A missing, stale or damaged cache must fall back to authoritative rows."""


def _key(values):
    result = []
    for value in values:
        if isinstance(value, bool):
            value = int(value)
        elif isinstance(value, float):
            if not math.isfinite(value):
                raise DiskIndexError("disk index requires finite scalar keys")
            if value.is_integer():
                value = int(value)
        if isinstance(value, (int, float)):
            result.append((1, value))
        elif isinstance(value, str):
            result.append((2, value))
        else:
            raise DiskIndexError("disk index keys must contain non-null scalars")
    return tuple(result)


def _canonical(values):
    return [int(value) if isinstance(value, bool) or isinstance(value, float) and value.is_integer()
            else value for value in values]


def _spec(spec):
    return {"fields": list(spec.fields), "unique": spec.unique}


def _digest(value):
    return hashlib.sha256(_json_bytes(value)).hexdigest()


def _fsync_dir(path):
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _regular(path, *, writable=False, create=False):
    flags = (os.O_RDWR if writable else os.O_RDONLY) | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
    if create:
        flags |= os.O_CREAT
    try:
        fd = os.open(path, flags, 0o600)
    except OSError as exc:
        raise DiskIndexUnavailable("disk index component is absent or unsafe") from exc
    if not stat.S_ISREG(os.fstat(fd).st_mode):
        os.close(fd)
        raise DiskIndexUnavailable("disk index component is not a regular file")
    return fd


def _root(database):
    database = Path(database)
    if any(part.is_symlink() for part in (database, *database.parents)):
        raise DiskIndexUnavailable("disk index source must not contain symlinks")
    root = database.with_name(database.name + ".indexes")
    if root.is_symlink():
        raise DiskIndexUnavailable("disk index cache root must not be a symlink")
    return root


def _safe(root, relative):
    parts = Path(relative).parts
    if Path(relative).is_absolute() or not parts or any(part in {"", ".", ".."} for part in parts):
        raise DiskIndexUnavailable("unsafe disk index component path")
    current = root
    if current.is_symlink():
        raise DiskIndexUnavailable("unsafe disk index root")
    for part in parts:
        current = current / part
        if current.is_symlink():
            raise DiskIndexUnavailable("disk index path must not contain symlinks")
    return current


@contextmanager
def _source_lock(store, *, timeout):
    start = time.monotonic()
    if not store._mutex.acquire(timeout=timeout):
        raise DiskIndexError("disk index publication process lock timed out")
    fd = -1
    try:
        fd = _regular(store.lock_file)
        deadline = start + timeout
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise DiskIndexError("disk index publication file lock timed out")
                time.sleep(min(0.01, max(0, deadline - time.monotonic())))
        yield
    finally:
        if fd >= 0:
            os.close(fd)
        store._mutex.release()


@contextmanager
def _job_lock(root, *, create=False):
    fd = _regular(root / ".build.lock", writable=create, create=create)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise DiskIndexError("another disk index build/recovery job is active") from exc
        yield
    finally:
        os.close(fd)


class _Budget:
    def __init__(self, rows, size):
        self.limit_rows = rows
        self.limit_bytes = size
        self.rows = 0
        self.bytes = 0

    def row(self):
        if self.rows >= self.limit_rows:
            raise DiskIndexError("disk index build max_rows exhausted")
        self.rows += 1

    def add(self, size):
        if size > self.limit_bytes - self.bytes:
            raise DiskIndexError("disk index build max_bytes exhausted")
        self.bytes += size

    def write(self, stream, data):
        self.add(len(data))
        stream.write(data)


def _parse(line, arity=None):
    if not line or len(line) > _MAX_LINE or not line.endswith(b"\n"):
        raise DiskIndexUnavailable("invalid disk index record length")
    try:
        row = json.loads(line)
        if not isinstance(row, dict) or set(row) != {"key", "id"} or not isinstance(row["key"], list) or \
                not isinstance(row["id"], str) or not row["id"] or arity is not None and len(row["key"]) != arity:
            raise ValueError
        normalized = _key(row["key"])
    except (ValueError, TypeError, KeyError, DiskIndexError) as exc:
        raise DiskIndexUnavailable("invalid typed disk index record") from exc
    return normalized, tuple(row["key"]), row["id"]


def _merge_rows(paths):
    streams = []
    heap = []
    try:
        for index, path in enumerate(paths):
            stream = path.open("rb"); streams.append(stream)
            line = stream.readline(_MAX_LINE + 1)
            if line:
                normalized, _raw, identity = _parse(line)
                heap.append((normalized, identity, index, line))
        heapq.heapify(heap)
        while heap:
            normalized, identity, index, line = heapq.heappop(heap)
            yield normalized, identity, line
            following = streams[index].readline(_MAX_LINE + 1)
            if following:
                key, _raw, identity = _parse(following)
                heapq.heappush(heap, (key, identity, index, following))
    finally:
        for stream in streams:
            stream.close()


def _run(path, rows, budget):
    rows.sort(key=lambda row: (row[0], row[1]))
    with path.open("xb") as stream:
        for _keyvalue, _identity, line in rows:
            budget.write(stream, line)
        stream.flush(); os.fsync(stream.fileno())


def _build_one(view, collection, name, stage, budget, chunk_rows):
    spec = view.index_specs[collection][name]
    stem = hashlib.sha256(_json_bytes([collection, name])).hexdigest()
    runs = []; chunk = []; count = data_size = 0
    for identity, record in view._records.get(collection, {}).items():
        raw = _index_key(record["data"], spec)
        if raw is None:
            continue
        budget.row()
        line = _json_bytes({"key": _canonical(raw), "id": identity}) + b"\n"
        if len(line) > _MAX_LINE:
            raise DiskIndexError("one disk index record exceeds the record size limit")
        count += 1; data_size += len(line)
        chunk.append((_key(raw), identity, line))
        if len(chunk) >= chunk_rows:
            path = stage / f".{stem}.run-{len(runs)}"; _run(path, chunk, budget); runs.append(path); chunk = []
    if chunk:
        path = stage / f".{stem}.run-{len(runs)}"; _run(path, chunk, budget); runs.append(path)
    level = 0
    while len(runs) > 32:
        following = []
        for offset in range(0, len(runs), 32):
            path = stage / f".{stem}.merge-{level}-{offset}"
            with path.open("xb") as stream:
                for _normal, _identity, line in _merge_rows(runs[offset:offset + 32]):
                    budget.write(stream, line)
                stream.flush(); os.fsync(stream.fileno())
            following.append(path)
            for old in runs[offset:offset + 32]: old.unlink()
        runs = following; level += 1
    data_path = stage / f"{stem}.keys"
    offsets_path = stage / f"{stem}.offsets"
    data_hash = hashlib.sha256(); offsets_hash = hashlib.sha256()
    offset = written = key_count = 0; previous = None
    with data_path.open("xb") as data, offsets_path.open("xb") as offsets:
        header = _HEADER.pack(_MAGIC, count, data_size)
        budget.write(offsets, header); offsets_hash.update(header)
        for normalized, identity, line in _merge_rows(runs):
            encoded = _OFFSET.pack(offset); budget.write(offsets, encoded); offsets_hash.update(encoded)
            budget.write(data, line); data_hash.update(line)
            offset += len(line); written += 1
            if normalized != previous:
                key_count += 1; previous = normalized
        encoded = _OFFSET.pack(offset); budget.write(offsets, encoded); offsets_hash.update(encoded)
        for stream in (data, offsets): stream.flush(); os.fsync(stream.fileno())
    for path in runs: path.unlink()
    if written != count or offset != data_size or spec.unique and key_count != count:
        raise DiskIndexError("disk index build inventory/uniqueness differs from source")
    return {"collection": collection, "index": name, "spec": _spec(spec),
            "collection_generation": view.collection_generation((collection,)),
            "collection_rows": view.count(collection), "indexed_rows": count, "key_count": key_count,
            "data": {"path": f"{stage.name.removeprefix('.build-')}/{data_path.name}", "sha256": data_hash.hexdigest(), "size_bytes": data_size},
            "offsets": {"path": f"{stage.name.removeprefix('.build-')}/{offsets_path.name}", "sha256": offsets_hash.hexdigest(),
                        "size_bytes": _HEADER.size + _OFFSET.size * (count + 1)}}


def rebuild_disk_indexes(store: Store, *, collections=None, index_names=None,
                         max_rows=1000000, max_bytes=2 * 1024**3,
                         sort_chunk_rows=20000, lock_timeout=30.0) -> dict:
    """Build outside the writer lock; publish only the still-current source view.

    max_rows counts memberships across all requested indexes. max_bytes counts
    writes of temporary runs, final files and manifests, including merge passes.
    Existing published caches remain intact on any failure or stale build.
    """
    for label, value, low, high in (("max_rows", max_rows, 1, 10000000), ("max_bytes", max_bytes, 1, 8 * 1024**4),
                                   ("sort_chunk_rows", sort_chunk_rows, 256, 100000)):
        if type(value) is not int or not low <= value <= high:
            raise DiskIndexError(f"{label} must be an integer from {low} to {high}")
    if isinstance(lock_timeout, bool) or not isinstance(lock_timeout, (int, float)) or not math.isfinite(lock_timeout) or not 0 < lock_timeout <= 300:
        raise DiskIndexError("lock_timeout must be in (0, 300]")
    if isinstance(collections, str): collections = [collections]
    try:
        names = tuple(store.index_specs if collections is None else collections)
    except TypeError as exc:
        raise DiskIndexError("collections must be iterable declared collection names") from exc
    if not names or any(not isinstance(name, str) or name not in store.index_specs for name in names):
        raise DiskIndexError("requested collections must have declared indexes")
    selected = sorted(set(names))
    if index_names is not None and (not isinstance(index_names, dict) or set(index_names) - set(selected)):
        raise DiskIndexError("index_names must map selected collections to index-name lists")
    wanted = {}
    for collection in selected:
        names = list(store.index_specs[collection]) if index_names is None or collection not in index_names else index_names[collection]
        if not isinstance(names, (list, tuple)) or not names or any(not isinstance(name, str) or name not in store.index_specs[collection] for name in names):
            raise DiskIndexError("unknown or empty disk index selection")
        wanted[collection] = sorted(set(names))
    with _source_lock(store, timeout=lock_timeout):
        store._assert_writable_locked(); store._reload_locked()
        root = _root(store.journal); root.mkdir(mode=0o700, exist_ok=True)
    budget = _Budget(max_rows, max_bytes)
    run_id = str(uuid4()); stage = root / f".build-{run_id}"
    published = False
    with _job_lock(root, create=True), store.read_snapshot(selected) as view:
        stage.mkdir(mode=0o700)
        try:
            marker = {"format": _BUILD_FORMAT, "run_id": run_id, "store_id": view.store_id,
                      "schema_version": view.schema_version, "created_at": datetime.now(timezone.utc).isoformat(), "phase": "building"}
            budget.add(len(_json_bytes(marker))); _atomic_json(stage / "build.json", marker); _fsync_dir(root)
            groups = [_build_one(view, collection, name, stage, budget, sort_chunk_rows)
                      for collection in selected for name in wanted[collection]]
            payload = {"format": _FORMAT, "build_id": run_id, "store_id": view.store_id,
                       "schema_version": view.schema_version, "source_sequence": view.sequence,
                       "created_at": marker["created_at"], "indexes": groups}
            envelope = {"payload": payload, "sha256": _digest(payload)}
            encoded_size = len(_json_bytes(envelope))
            if encoded_size > _MAX_MANIFEST: raise DiskIndexError("disk index manifest exceeds limit")
            budget.add(encoded_size * 2); _atomic_json(stage / "index-manifest.json", envelope)
            marker["phase"] = "complete"; budget.add(len(_json_bytes(marker))); _atomic_json(stage / "build.json", marker)
            _fsync_dir(stage)
            with _source_lock(store, timeout=lock_timeout):
                store._assert_writable_locked(); store._reload_locked()
                meta = store._meta()
                if meta["store_id"] != view.store_id or meta.get("schema_version", 1) != view.schema_version or \
                        store.schema_version != view.schema_version:
                    raise Conflict("disk index source identity/schema changed during build")
                for group in groups:
                    collection, name = group["collection"], group["index"]
                    if store._collection_sequences.get(collection, 0) != group["collection_generation"] or \
                            _spec(store.index_specs[collection][name]) != group["spec"]:
                        raise Conflict("disk index source collection/spec changed during build")
                os.rename(stage, root / run_id); published = True
                _atomic_json(root / "manifest.json", envelope); _fsync_dir(root)
            return {"format": _FORMAT, "build_id": run_id, "store_id": view.store_id,
                    "source_sequence": view.sequence, "indexes": len(groups), "indexed_rows": budget.rows,
                    "bytes_written": budget.bytes, "manifest_sha256": envelope["sha256"],
                    "cache_root": str(root), "publication": "current primary collection generations verified"}
        finally:
            if not published and stage.exists(): shutil.rmtree(stage)


def _manifest(root):
    path = _safe(root, "manifest.json"); fd = _regular(path)
    with os.fdopen(fd, "rb") as stream:
        raw = stream.read(_MAX_MANIFEST + 1)
    try:
        if len(raw) > _MAX_MANIFEST: raise ValueError
        envelope = json.loads(raw); payload = envelope["payload"]
        if not isinstance(payload, dict) or envelope.get("sha256") != _digest(payload) or payload.get("format") != _FORMAT:
            raise ValueError
        if str(UUID(payload["build_id"])) != payload["build_id"] or not isinstance(payload["indexes"], list): raise ValueError
        if not isinstance(payload.get("store_id"), str) or str(UUID(payload["store_id"])) != payload["store_id"] or \
                type(payload.get("schema_version")) is not int or payload["schema_version"] < 1:
            raise ValueError
        seen = set()
        for group in payload["indexes"]:
            if not isinstance(group, dict):
                raise ValueError
            if any(not isinstance(group.get(field), str) or not group[field] for field in ("collection", "index")):
                raise ValueError
            identity = (group["collection"], group["index"])
            if identity in seen: raise ValueError
            seen.add(identity)
            for kind in ("data", "offsets"):
                entry = group[kind]
                if not isinstance(entry, dict) or set(entry) != {"path", "sha256", "size_bytes"} or \
                        type(entry["size_bytes"]) is not int or entry["size_bytes"] < 0 or \
                        not isinstance(entry["sha256"], str) or len(entry["sha256"]) != 64 or any(c not in "0123456789abcdef" for c in entry["sha256"]) or \
                        Path(entry["path"]).parts[0] != payload["build_id"]:
                    raise ValueError
                _safe(root, entry["path"])
            if any(type(group[field]) is not int or group[field] < 0 for field in
                    ("collection_generation", "collection_rows", "indexed_rows", "key_count")) or \
                    not 0 <= group["key_count"] <= group["indexed_rows"] <= group["collection_rows"]:
                raise ValueError
            if group["offsets"]["size_bytes"] != _HEADER.size + _OFFSET.size * (group["indexed_rows"] + 1): raise ValueError
        return payload, envelope["sha256"]
    except (ValueError, KeyError, TypeError, IndexError, AttributeError, DiskIndexError) as exc:
        raise DiskIndexUnavailable("disk index manifest failed checksum/shape validation") from exc


def _verify(fd, entry):
    info = os.fstat(fd)
    if info.st_size != entry["size_bytes"] or info.st_size > _MAX_VERIFY_BYTES:
        raise DiskIndexUnavailable("disk index file size differs or verification budget exceeded")
    identity = (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns, entry["sha256"])
    with _VERIFY_LOCK:
        if identity in _VERIFIED:
            _VERIFIED.move_to_end(identity); return 0
    digest = hashlib.sha256(); read = 0
    while read < info.st_size:
        chunk = os.pread(fd, min(1024 * 1024, info.st_size - read), read)
        if not chunk: raise DiskIndexUnavailable("disk index file became truncated")
        digest.update(chunk); read += len(chunk)
    final = os.fstat(fd)
    if (final.st_dev, final.st_ino, final.st_size, final.st_mtime_ns, final.st_ctime_ns) != identity[:5] or digest.hexdigest() != entry["sha256"]:
        raise DiskIndexUnavailable("disk index file checksum/identity differs")
    with _VERIFY_LOCK:
        _VERIFIED[identity] = True; _VERIFIED.move_to_end(identity)
        if len(_VERIFIED) > 256: _VERIFIED.popitem(last=False)
    return read


class DiskOrderedKeys:
    """Sequence facade that reads just binary-search probes from mapped files."""

    def __init__(self, root, group, view):
        self.group = group; self.view = view; self._closed = False
        self._data_fd = self._offset_fd = -1; self._data = self._offsets = None
        self.work = {"verification_bytes": 0, "verification_records": 0, "verification_key_bytes": 0,
                     "key_bytes_read": 0, "offset_probes": 0, "key_records_read": 0}
        try:
            self._data_fd = _regular(_safe(root, group["data"]["path"]))
            self._offset_fd = _regular(_safe(root, group["offsets"]["path"]))
            self.work["verification_bytes"] = _verify(self._data_fd, group["data"]) + _verify(self._offset_fd, group["offsets"])
            self._data_stamp = self._stamp(self._data_fd); self._offset_stamp = self._stamp(self._offset_fd)
            if group["data"]["size_bytes"]:
                self._data = mmap.mmap(self._data_fd, 0, access=mmap.ACCESS_READ)
            self._offsets = mmap.mmap(self._offset_fd, 0, access=mmap.ACCESS_READ)
            magic, count, size = _HEADER.unpack_from(self._offsets, 0)
            if magic != _MAGIC or count != group["indexed_rows"] or size != group["data"]["size_bytes"] or \
                    self._offset(0) != 0 or self._offset(count) != size:
                raise DiskIndexUnavailable("disk index binary offset header differs")
            self._validate_layout()
        except Exception:
            self.close(); raise

    def _validate_layout(self):
        """Stream-validate cache order/coverage once without retaining key arrays.

        Counts, strict key/ID order and authoritative membership prove a complete
        bijection: a valid checksum alone is insufficient for a forged or buggy
        cache with duplicated, missing or unsorted records.
        """
        identity = (self._data_stamp, self.group["data"]["sha256"], self._offset_stamp,
                    self.group["offsets"]["sha256"], self.view.store_id,
                    self.view.schema_version, self.view._source_pin,
                    self.group["collection"], self.group["index"],
                    self.group["collection_generation"], _digest(self.group["spec"]))
        with _VERIFY_LOCK:
            if identity in _VERIFIED_LAYOUT:
                _VERIFIED_LAYOUT.move_to_end(identity)
                return
        previous = None; previous_key = None; keys = 0
        buckets = self.view._indices.get((self.group["collection"], self.group["index"]), {})
        for position in range(len(self)):
            start = _OFFSET.unpack_from(self._offsets, _HEADER.size + position * _OFFSET.size)[0]
            stop = _OFFSET.unpack_from(self._offsets, _HEADER.size + (position + 1) * _OFFSET.size)[0]
            if not 0 <= start < stop <= self.group["data"]["size_bytes"] or stop - start > _MAX_LINE or self._data is None:
                raise DiskIndexUnavailable("disk index offset order/coverage is invalid")
            line = self._data[start:stop]
            self.work["verification_key_bytes"] += len(line)
            self.work["verification_records"] += 1
            normalized, raw, record_id = _parse(line, len(self.group["spec"]["fields"]))
            pair = normalized, record_id
            if previous is not None and pair <= previous:
                raise DiskIndexUnavailable("disk index key/ID order has a duplicate or inversion")
            if record_id not in buckets.get(raw, ()):
                raise DiskIndexUnavailable("disk index semantic membership differs from source")
            if normalized != previous_key:
                keys += 1; previous_key = normalized
            previous = pair
        if keys != self.group["key_count"]:
            raise DiskIndexUnavailable("disk index distinct-key inventory differs")
        self.assert_stable()
        with _VERIFY_LOCK:
            _VERIFIED_LAYOUT[identity] = True; _VERIFIED_LAYOUT.move_to_end(identity)
            if len(_VERIFIED_LAYOUT) > 256: _VERIFIED_LAYOUT.popitem(last=False)

    @staticmethod
    def _stamp(fd):
        info = os.fstat(fd)
        return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns

    def assert_stable(self):
        if self._closed or self._stamp(self._data_fd) != self._data_stamp or self._stamp(self._offset_fd) != self._offset_stamp:
            raise DiskIndexUnavailable("disk index changed while being read")

    def _offset(self, position):
        self.work["offset_probes"] += 1
        return _OFFSET.unpack_from(self._offsets, _HEADER.size + position * _OFFSET.size)[0]

    def __len__(self):
        return self.group["indexed_rows"]

    def record(self, position):
        self.assert_stable()
        if position < 0: position += len(self)
        if not 0 <= position < len(self): raise IndexError(position)
        start, stop = self._offset(position), self._offset(position + 1)
        if not 0 <= start < stop <= self.group["data"]["size_bytes"] or stop - start > _MAX_LINE or self._data is None:
            raise DiskIndexUnavailable("disk index offsets are invalid")
        line = self._data[start:stop]
        self.work["key_bytes_read"] += len(line); self.work["key_records_read"] += 1
        normalized, raw, identity = _parse(line, len(self.group["spec"]["fields"]))
        if identity not in self.view._indices.get((self.group["collection"], self.group["index"]), {}).get(raw, ()):
            raise DiskIndexUnavailable("disk index key/ID differs from captured authoritative membership")
        return normalized, raw, identity

    def __getitem__(self, position):
        if isinstance(position, slice):
            if position.step not in (None, 1): raise DiskIndexError("disk window only supports contiguous slices")
            start, stop, _step = position.indices(len(self))
            return DiskKeyWindow(self, start, stop)
        normalized, raw, _identity = self.record(position)
        return normalized, raw

    def close(self):
        for mapping in (self._data, self._offsets):
            if mapping is not None: mapping.close()
        for fd in (self._data_fd, self._offset_fd):
            if fd >= 0: os.close(fd)
        self._data = self._offsets = None; self._data_fd = self._offset_fd = -1; self._closed = True


class DiskKeyWindow:
    def __init__(self, source, start, stop):
        self.source = source; self.start = start; self.stop = max(start, stop)

    def __len__(self):
        return self.stop - self.start

    def records(self):
        for position in range(self.start, self.stop):
            yield self.source.record(position)
        self.source.assert_stable()

    def __iter__(self):
        previous = None
        for normalized, raw, _identity in self.records():
            if normalized != previous:
                yield normalized, raw; previous = normalized

    def __reversed__(self):
        previous = None
        for position in range(self.stop - 1, self.start - 1, -1):
            normalized, raw, _identity = self.source.record(position)
            if normalized != previous:
                yield normalized, raw; previous = normalized
        self.source.assert_stable()


def ordered_disk_keys(view, collection, name):
    """Return a valid generation reader or None; stale caches never win."""
    if not hasattr(view, "_disk_readers"):
        view._disk_readers = {}; view._disk_fallbacks = {}
    identity = (collection, name)
    if identity in getattr(view, "_disk_disabled", ()):
        return None
    if identity in view._disk_readers:
        reader = view._disk_readers[identity]
        try:
            reader.assert_stable(); return reader
        except (DiskIndexError, OSError):
            reader.close(); del view._disk_readers[identity]
    try:
        root = _root(view.database_path)
        payload, _sha = _manifest(root)
        if payload.get("store_id") != view.store_id or payload.get("schema_version") != view.schema_version:
            raise DiskIndexUnavailable("disk index store/schema identity is stale")
        group = next((row for row in payload["indexes"] if (row["collection"], row["index"]) == identity), None)
        if group is None: return None
        expected_rows = sum(len(ids) for ids in view._indices.get(identity, {}).values())
        if group["collection_generation"] != view.collection_generation((collection,)) or \
                group["spec"] != _spec(view.index_specs[collection][name]) or group["collection_rows"] != view.count(collection) or \
                group["indexed_rows"] != expected_rows or group["key_count"] != len(view._indices.get(identity, {})):
            raise DiskIndexUnavailable("disk index source generation/spec/coverage is stale")
        reader = DiskOrderedKeys(root, group, view); view._disk_readers[identity] = reader
        return reader
    except (DiskIndexError, OSError, ValueError, TypeError, KeyError) as exc:
        view._disk_fallbacks[identity] = str(exc)
        return None


def inspect_disk_indexes(store: Store) -> dict:
    """Read-only checksum/layout/coverage census of published current caches."""
    root = _root(store.journal)
    try:
        payload, sha = _manifest(root)
    except DiskIndexError as exc:
        return {"status": "unavailable", "cache_root": str(root), "detail": str(exc)}
    with store.read_snapshot({row["collection"] for row in payload["indexes"]}) as view:
        rows = []
        for group in payload["indexes"]:
            name, collection = group["index"], group["collection"]
            reader = ordered_disk_keys(view, collection, name)
            rows.append({"collection": collection, "index": name, "status": "valid" if reader is not None else "stale_or_invalid",
                         "indexed_rows": group["indexed_rows"], "key_count": group["key_count"],
                         "collection_generation": group["collection_generation"]})
        return {"status": "valid" if all(row["status"] == "valid" for row in rows) else "stale_or_invalid",
                "build_id": payload["build_id"], "manifest_sha256": sha, "cache_root": str(root), "indexes": rows}


def recover_disk_index_builds(store: Store, *, cleanup=False, lock_timeout=30.0) -> dict:
    """Inspect/remove idle marked stages and obsolete builds, never active files."""
    if type(cleanup) is not bool:
        raise DiskIndexError("cleanup must be a boolean")
    if isinstance(lock_timeout, bool) or not isinstance(lock_timeout, (int, float)) or not math.isfinite(lock_timeout) or not 0 < lock_timeout <= 300:
        raise DiskIndexError("lock_timeout must be in (0, 300]")
    root = _root(store.journal)
    if not root.exists(): return {"active_build": False, "eligible": [], "removed": [], "skipped": []}
    if cleanup:
        with _source_lock(store, timeout=lock_timeout): store._assert_writable_locked()
    try:
        job = _job_lock(root)
        job.__enter__()
    except DiskIndexError as exc:
        if cleanup: raise
        return {"active_build": True, "eligible": [], "removed": [], "skipped": [], "detail": str(exc)}
    try:
        active = None
        manifest_known = False
        try:
            active = _manifest(root)[0]["build_id"]
            manifest_known = True
        except DiskIndexError:
            pass
        with store._locked(exclusive=False):
            store._reload_locked(); owner = store._meta()["store_id"]
        eligible = []; removed = []; skipped = []
        for candidate in sorted(root.iterdir()):
            if candidate.name in {"manifest.json", ".build.lock", active}: continue
            if candidate.is_symlink() or not candidate.is_dir():
                skipped.append(candidate.name); continue
            try:
                fd = _regular(candidate / "build.json")
                with os.fdopen(fd, "rb") as stream: raw = stream.read(65537)
                marker = json.loads(raw)
                run_id = str(UUID(marker["run_id"]))
                if len(raw) > 65536 or marker.get("format") != _BUILD_FORMAT or marker.get("store_id") != owner or \
                        candidate.name not in {run_id, ".build-" + run_id} or marker.get("phase") not in {"building", "complete"}:
                    raise ValueError
            except (DiskIndexError, OSError, ValueError, TypeError, KeyError):
                skipped.append(candidate.name); continue
            if not manifest_known and not candidate.name.startswith(".build-"):
                skipped.append(candidate.name)
                continue
            eligible.append(candidate.name)
            if cleanup:
                shutil.rmtree(candidate); removed.append(candidate.name)
        if removed: _fsync_dir(root)
        return {"active_build": False, "active_build_id": active, "eligible": eligible, "removed": removed, "skipped": skipped}
    finally:
        job.__exit__(None, None, None)
