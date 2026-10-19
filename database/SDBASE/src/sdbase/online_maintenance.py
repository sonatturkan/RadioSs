"""Prepare recovery images while reads and writes continue, then publish briefly.

The checkpoint describes a captured sequence. Later durable journal frames
are retained verbatim. A newer maintenance checkpoint, changed timeline, or
excessive tail defers publication; no old snapshot may resurrect retired rows.
No external database or SQL format is involved.
"""

from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
import stat
import struct
from time import monotonic
from uuid import UUID, uuid4

from . import _platform
from .backup import _fsync_dir, _open_regular, _safe_source
from .storage import Corruption, Store, StoreError, WriteRejected, _atomic_json, _FORMAT, _MAX_FRAME


_JOB_FORMAT = "sdbase-online-checkpoint-job-v1"


def _root(store):
    return store.journal.with_name(store.journal.name + ".maintenance")


@contextmanager
def _job_lock(store):
    if store.read_only:
        raise WriteRejected("read-only handle cannot prepare maintenance")
    with store._locked(exclusive=False):
        store._assert_writable_locked()
        root = _root(store)
        if _platform.is_link(root):
            raise StoreError("maintenance root must not be a symlink")
        _platform.mkdir_private(root, exist_ok=True)
    fd = _platform.open_fd(root / "maintenance.lock", os.O_RDWR | os.O_CREAT, 0o600, nofollow=True)
    try:
        lock_stat = os.fstat(fd)
        if not stat.S_ISREG(lock_stat.st_mode) or lock_stat.st_nlink != 1:
            raise StoreError("maintenance lock must be a singly linked regular file")
        try:
            _platform.flock(fd, _platform.LOCK_EX | _platform.LOCK_NB)
        except BlockingIOError as exc:
            raise StoreError("another online maintenance job is active") from exc
        yield root
    finally:
        _platform.unlock_and_close(fd)


def _identity(meta):
    return tuple(meta[field] for field in ("store_id", "node_id", "timeline_id", "epoch", "schema_version"))


def _capture(store):
    with store._locked(exclusive=False):
        store._assert_writable_locked()
        store._reload_locked()
        meta = store._meta()
        journal_stat = store.journal.stat()
        payload = {"format": _FORMAT, "sequence": store._sequence,
                   "state": {name: dict(rows) for name, rows in store._state.items()},
                   "tombstones": {name: sorted(ids) for name, ids in store._tombstones.items()},
                   "retired_unique_keys": {collection: {name: sorted(values) for name, values in indexes.items()}
                                           for collection, indexes in store._retired_unique_keys.items()},
                   "collection_sequences": dict(store._collection_sequences)}
        return payload, {"identity": _identity(meta), "generation": meta["generation"],
                         "checkpoint_stamp": store._checkpoint_stamp, "offset": store._offset,
                         "journal_inode": journal_stat.st_ino}


def _prepare_checkpoint(path, payload):
    """Stream canonical JSON and its checksum without one giant encoded buffer."""
    encoder = json.JSONEncoder(ensure_ascii=False, sort_keys=True,
                              separators=(",", ":"), allow_nan=False)
    digest = hashlib.sha256()
    # The job stage is a private directory: the file inherits its owner-only DACL.
    with path.open("xb") as stream:
        stream.write(b'{"payload":')
        for chunk in encoder.iterencode(payload):
            encoded = chunk.encode("utf-8")
            digest.update(encoded)
            stream.write(encoded)
        stream.write(b',"sha256":"' + digest.hexdigest().encode("ascii") + b'"}')
        stream.flush()
        os.fsync(stream.fileno())
    return digest.hexdigest()


def _after_publish(phase):
    """Fault-injection seam used by the isolated operational acceptance runner."""


def _journal_tail(store, path, base_sequence, offset, max_bytes, max_frames):
    """Copy only a contiguous checksummed suffix under the publication lock."""
    fd = _open_regular(store.journal)
    written = frames = 0
    expected = base_sequence + 1
    with os.fdopen(fd, "rb") as source, path.open("xb") as output:
        source.seek(offset)
        while header := source.read(4):
            if len(header) != 4:
                raise Corruption("incomplete journal prefix during online checkpoint")
            size = struct.unpack(">I", header)[0]
            if not 0 < size <= _MAX_FRAME:
                raise Corruption("invalid journal frame during online checkpoint")
            encoded, checksum = source.read(size), source.read(32)
            if len(encoded) != size or len(checksum) != 32 or hashlib.sha256(encoded).digest() != checksum:
                raise Corruption("incomplete or corrupt journal during online checkpoint")
            txn = json.loads(encoded)
            sequence = txn.get("sequence")
            if type(sequence) is not int:
                raise Corruption("invalid journal sequence during online checkpoint")
            if sequence <= base_sequence:
                continue
            if sequence != expected:
                raise Corruption("journal tail has a sequence gap")
            expected += 1
            frame = header + encoded + checksum
            frames += 1
            written += len(frame)
            if written > max_bytes or frames > max_frames:
                raise StoreError("online checkpoint tail exceeded the configured work budget")
            output.write(frame)
        if expected - 1 != store._sequence:
            raise Corruption("journal cannot recover the latest captured state")
        output.flush()
        os.fsync(output.fileno())
    return written, frames


def checkpoint_online(store: Store, *, max_tail_bytes: int = 64 * 1024**2,
                      max_tail_frames: int = 10000) -> dict:
    """Publish a captured checkpoint and keep all later transactions.

    Capture briefly holds a shared lock. Serialization and fsync of the large
    image happen without the store lock. Only bounded tail verification/copy,
    pending WAL assurance and atomic publication hold the final write lock.
    Return ``deferred`` if a newer checkpoint superseded the image or the
    bounded tail is too large; callers may retry from a fresh capture.
    """
    if type(max_tail_bytes) is not int or not 1 <= max_tail_bytes <= 2 * 1024**3 or \
            type(max_tail_frames) is not int or not 1 <= max_tail_frames <= 100000:
        raise StoreError("invalid online checkpoint tail limits")
    started = monotonic()
    with _job_lock(store) as root:
        job_id = str(uuid4())
        stage = root / job_id
        _platform.mkdir_private(stage)
        try:
            capture_started = monotonic()
            payload, captured = _capture(store)
            capture_ms = (monotonic() - capture_started) * 1000
            sequence = payload["sequence"]
            job = {"format": _JOB_FORMAT, "job_id": job_id, "database": str(store.journal),
                   "identity": list(captured["identity"]), "sequence": sequence,
                   "created_at": datetime.now(timezone.utc).isoformat(), "phase": "preparing"}
            _atomic_json(stage / "job.json", job)
            prepare_started = monotonic()
            digest = _prepare_checkpoint(stage / "checkpoint.pending", payload)
            del payload
            prepare_ms = (monotonic() - prepare_started) * 1000
            job.update(phase="prepared", payload_sha256=digest)
            _atomic_json(stage / "job.json", job)
            _after_publish("prepared")
            lock_requested = monotonic()
            with store._locked(exclusive=True):
                publish_started = monotonic()
                store._assert_writable_locked()
                meta = store._meta()
                if _identity(meta) != captured["identity"]:
                    raise WriteRejected("online checkpoint belongs to an obsolete writer timeline")
                # Avoid an unbounded reload/copy under the write lock. The fast
                # path can start at the original journal offset only when both
                # inode and checkpoint generation still identify that prefix.
                cp_stat = store.checkpoint_file.stat() if store.checkpoint_file.exists() else None
                cp_stamp = (cp_stat.st_ino, cp_stat.st_size, cp_stat.st_mtime_ns) if cp_stat else None
                if cp_stamp != captured["checkpoint_stamp"]:
                    return {"status": "deferred", "reason": "checkpoint_replaced",
                            "captured_sequence": sequence}
                journal_stat = store.journal.stat()
                fast = (journal_stat.st_ino == captured["journal_inode"] and
                        cp_stamp == captured["checkpoint_stamp"] and
                        meta["generation"] == captured["generation"] and
                        journal_stat.st_size >= captured["offset"])
                scan_offset = captured["offset"] if fast else 0
                if journal_stat.st_size - scan_offset > max_tail_bytes:
                    return {"status": "deferred", "reason": "tail_byte_budget",
                            "captured_sequence": sequence, "journal_scan_bytes": journal_stat.st_size - scan_offset}
                store._reload_locked()
                if store._sequence < sequence:
                    raise Corruption("store sequence moved backwards during online checkpoint")
                if store._sequence - sequence > max_tail_frames:
                    return {"status": "deferred", "reason": "tail_frame_budget", "captured_sequence": sequence}
                from .wal_archive import archive_pending_locked, _status_locked
                wal_status = _status_locked(store, verify=False)
                if wal_status.get("enabled") and wal_status["last_sequence"] != store._sequence:
                    return {"status": "deferred", "reason": "wal_archive_not_sealed",
                            "captured_sequence": sequence, "current_sequence": store._sequence}
                archive_pending_locked(store)
                tail_bytes, tail_frames = _journal_tail(store, stage / "journal.pending", sequence,
                                                        scan_offset, max_tail_bytes, max_tail_frames)
                journal_before = journal_stat.st_size
                job.update(phase="publishing", current_sequence=store._sequence,
                           tail_bytes=tail_bytes, tail_frames=tail_frames)
                _atomic_json(stage / "job.json", job)
                # POSIX-semantics replace: a share-delete reader (or a scanner
                # that allows it) keeps the old bytes; sharing violations retry.
                _platform.replace(stage / "checkpoint.pending", store.checkpoint_file)
                _fsync_dir(store.root)
                _after_publish("checkpoint_published")
                # A crash here leaves the new checkpoint and the complete old
                # journal. Normal replay skips the already represented prefix.
                _platform.replace(stage / "journal.pending", store.journal)
                _fsync_dir(store.root)
                _after_publish("journal_published")
                meta = store._meta()
                meta["generation"] += 1
                _atomic_json(store.meta_file, meta)
                checkpoint_stat = store.checkpoint_file.stat()
                store._generation = meta["generation"]
                store._checkpoint_stamp = (checkpoint_stat.st_ino, checkpoint_stat.st_size,
                                           checkpoint_stat.st_mtime_ns)
                store._offset = tail_bytes
                store.metrics.increment("online_checkpoints")
                publish_ms = (monotonic() - publish_started) * 1000
                result = {"status": "published", "checkpoint_sequence": sequence,
                          "current_sequence": store._sequence, "retained_transactions": tail_frames,
                          "journal_bytes_before": journal_before, "journal_bytes_after": tail_bytes,
                          "checkpoint_payload_sha256": digest,
                          "timings_ms": {"capture": round(capture_ms, 3), "prepare": round(prepare_ms, 3),
                                         "publish_lock_wait": round((publish_started - lock_requested)*1000, 3),
                                         "publish_lock_held": round(publish_ms, 3),
                                         "total": round((monotonic()-started)*1000, 3)}}
            store.metrics.observe("online_checkpoint_publish_ms", publish_ms)
            return result
        finally:
            # Never mask the job's own error; recover_online_jobs removes a
            # stage that a scanner kept busy past the retry budget.
            _platform.rmtree(stage, ignore_errors=True)


def recover_online_jobs(store: Store, *, cleanup: bool = False) -> dict:
    """Inspect/remove only exact owned abandoned checkpoint staging jobs.

    The DB itself is never repaired or rolled back. Recovery is already safe
    after either publication boundary; staging is disposable preparation data.
    """
    if type(cleanup) is not bool:
        raise StoreError("cleanup must be a boolean")
    with _job_lock(store) as root:
        recovered = []
        for stage in sorted(root.iterdir()):
            if stage.name == "maintenance.lock":
                continue
            if _platform.is_link(stage) or not stage.is_dir():
                continue
            try:
                if str(UUID(stage.name)) != stage.name:
                    continue
                marker = _safe_source(stage, "job.json")
                if marker.stat().st_size > 65536:
                    continue
                with os.fdopen(_open_regular(marker), "rb") as stream:
                    job = json.load(stream)
                if job.get("format") != _JOB_FORMAT or job.get("job_id") != stage.name or \
                        not isinstance(job.get("database"), str) or \
                        not _platform.path_equal(job["database"], str(store.journal)) or \
                        job.get("phase") not in {"preparing", "prepared", "publishing"} or \
                        type(job.get("sequence")) is not int or job["sequence"] < 0 or \
                        not isinstance(job.get("identity"), list) or len(job["identity"]) != 5 or \
                        any(_platform.is_link(path) or not path.is_file() or path.name not in
                            {"job.json", "checkpoint.pending", "journal.pending"} for path in stage.iterdir()):
                    continue
            except (OSError, StoreError, ValueError, TypeError, AttributeError):
                continue
            recovered.append({"job_id": stage.name, "phase": job["phase"],
                              "captured_sequence": job["sequence"], "removed": cleanup})
            if cleanup:
                _platform.rmtree(stage)
        if cleanup:
            _fsync_dir(root)
        return {"jobs": recovered, "cleanup": cleanup, "database_unchanged": True}


def run_online_maintenance(store: Store, stop_event, *, interval_seconds: float = 60,
                           journal_threshold_bytes: int = 64 * 1024**2,
                           max_tail_bytes: int = 64 * 1024**2, max_tail_frames: int = 10000):
    """Bounded checkpoint loop; caller owns lifecycle and result persistence."""
    if isinstance(interval_seconds, bool) or not isinstance(interval_seconds, (int, float)) or \
            not 0.05 <= interval_seconds <= 86400 or type(journal_threshold_bytes) is not int or \
            journal_threshold_bytes < 1:
        raise StoreError("invalid maintenance interval/threshold")
    while not stop_event.is_set():
        if store.journal.stat().st_size >= journal_threshold_bytes:
            yield checkpoint_online(store, max_tail_bytes=max_tail_bytes, max_tail_frames=max_tail_frames)
        stop_event.wait(interval_seconds)
