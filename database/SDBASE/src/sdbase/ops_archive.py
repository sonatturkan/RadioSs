"""Portable, verifiable segments of the SDBASE transaction audit history.

The source ``_audit`` collection is append-only and contains every committed
transaction's before/after values.  These JSONL segments provide an independent
read path for that evidence.  Export never deletes or compacts source records;
it is not a substitute for a media-inclusive backup.  SHA-256 detects accidental
change, but a party able to rewrite both data and manifests can forge a chain.
Operators should preserve the published manifest digest outside the host.
"""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import stat
from typing import Any, BinaryIO, Callable, Mapping
from uuid import UUID

from . import _platform
from .backup import BackupError, _destination, _fsync_dir, _publish_absent
from .storage import Store, StoreError


FORMAT = "sdbase-audit-archive-v1"
_ZERO = "0" * 64
_SHA = re.compile(r"[0-9a-f]{64}\Z")
_FILES = frozenset({"audit.jsonl", "manifest.json", "manifest.sha256"})
_MAX_LINE = 64 * 1024 * 1024


class ArchiveError(StoreError):
    """An audit archive is absent, incomplete, inconsistent or unsafe."""


def _json_bytes(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False).encode("utf-8")


def _object_pairs(pairs: list[tuple[str, Any]]) -> dict:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ArchiveError("archive JSON contains duplicate keys")
        result[key] = value
    return result


def _regular(path: Path) -> BinaryIO:
    # No-follow, binary, share-delete reader: verifying an archive never blocks
    # an operator (or antivirus) from moving or deleting it.
    try:
        descriptor = _platform.os_open(path, os.O_RDONLY, nofollow=True)
    except OSError as exc:
        raise ArchiveError(f"cannot read archive component: {path.name}") from exc
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise ArchiveError(f"archive component is not a regular file: {path.name}")
        return os.fdopen(descriptor, "rb")
    except OSError as exc:
        os.close(descriptor)
        raise ArchiveError(f"cannot read archive component: {path.name}") from exc
    except BaseException:
        os.close(descriptor)
        raise


def _create_private(path: Path) -> BinaryIO:
    """Exclusive create of a private (owner-only DACL) binary file; LF bytes as given."""
    descriptor = _platform.os_open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600,
                                   nofollow=True, private=True)
    try:
        return os.fdopen(descriptor, "wb")
    except BaseException:
        os.close(descriptor)
        raise


def _read_small(path: Path, limit: int = 64 * 1024) -> bytes:
    with _regular(path) as stream:
        raw = stream.read(limit + 1)
    if len(raw) > limit:
        raise ArchiveError(f"archive component is too large: {path.name}")
    return raw


def _archive_dir(value: str | Path) -> Path:
    raw = Path(value).expanduser()
    if _platform.is_link_like(raw) or not raw.is_dir():
        raise ArchiveError("archive directory is absent or a symlink")
    root = raw.resolve(strict=True)
    try:
        contents = {entry.name for entry in root.iterdir()}
    except OSError as exc:
        raise ArchiveError("archive directory cannot be listed") from exc
    if contents != _FILES:
        raise ArchiveError("archive contains missing or unreferenced files")
    return root


def _manifest(root: Path) -> tuple[dict, str]:
    encoded = _read_small(root / "manifest.json")
    checksum = _read_small(root / "manifest.sha256")
    digest = hashlib.sha256(encoded).hexdigest()
    if checksum != (digest + "\n").encode("ascii"):
        raise ArchiveError("archive manifest checksum mismatch")
    try:
        body = json.loads(encoded, object_pairs_hook=_object_pairs)
    except (UnicodeError, ValueError, TypeError) as exc:
        raise ArchiveError("archive manifest is invalid JSON") from exc
    if not isinstance(body, dict) or body.get("format") != FORMAT:
        raise ArchiveError("unsupported archive manifest")
    expected = {"format", "store_id", "created_at", "source_sequence",
                "requested_through_sequence", "first_sequence", "last_sequence",
                "record_count", "audit_sha256", "audit_bytes",
                "previous_manifest_sha256", "previous_chain_sha256",
                "chain_sha256"}
    if set(body) != expected:
        raise ArchiveError("archive manifest fields are invalid")
    try:
        if str(UUID(body["store_id"])) != body["store_id"]:
            raise ValueError
        observed = datetime.fromisoformat(body["created_at"].replace("Z", "+00:00"))
        if observed.tzinfo is None or observed.utcoffset() is None:
            raise ValueError
    except (ValueError, TypeError, AttributeError) as exc:
        raise ArchiveError("archive identity or creation time is invalid") from exc
    for field in ("source_sequence", "requested_through_sequence", "first_sequence",
                  "last_sequence", "record_count", "audit_bytes"):
        if type(body[field]) is not int or body[field] < (0 if field == "audit_bytes" else 1):
            raise ArchiveError(f"archive {field} is invalid")
    first, last = body["first_sequence"], body["last_sequence"]
    if (first > last or body["record_count"] != last - first + 1 or
            last > body["requested_through_sequence"] or
            body["requested_through_sequence"] > body["source_sequence"]):
        raise ArchiveError("archive sequence bounds are inconsistent")
    for field in ("audit_sha256", "previous_manifest_sha256",
                  "previous_chain_sha256", "chain_sha256"):
        if not isinstance(body[field], str) or not _SHA.fullmatch(body[field]):
            raise ArchiveError(f"archive {field} is invalid")
    if first == 1 and (body["previous_manifest_sha256"] != _ZERO or
                       body["previous_chain_sha256"] != _ZERO):
        raise ArchiveError("first archive has a predecessor")
    if first > 1 and body["previous_manifest_sha256"] == _ZERO:
        raise ArchiveError("later archive has no predecessor digest")
    return body, digest


def _audit_snapshot(store: Store, first: int, through: int | None,
                    max_records: int) -> tuple[str, int, int, list[dict]]:
    # Audit envelopes are immutable.  Retaining their references after this
    # short read lock is safe within the Store API and avoids duplicating large
    # before/after bodies in memory during the disk write.
    with store._locked(exclusive=False):
        store._reload_locked()
        source_sequence = store._sequence
        store_id = store._meta().get("store_id")
        try:
            store_id = str(UUID(store_id))
        except (ValueError, TypeError, AttributeError) as exc:
            raise ArchiveError("source store identity is invalid") from exc
        if first > source_sequence:
            raise ArchiveError("no new committed audit records to archive")
        requested = source_sequence if through is None else through
        if type(requested) is not int or not first <= requested <= source_sequence:
            raise ArchiveError("through_sequence is outside committed audit history")
        audit = store._state.get("_audit", {})
        cold_through = max((row["data"]["last_audit_sequence"] for row in
                            store._state.get("retention_segments", {}).values()), default=0)
        if len(audit) + cold_through != source_sequence:
            raise ArchiveError("source audit count differs from committed sequence")
        upper = min(requested, first + max_records - 1)
        candidates: list[tuple[int, str, dict]] = []
        for transaction_id, envelope in audit.items():
            sequence = envelope["data"].get("sequence")
            if type(sequence) is not int or not 1 <= sequence <= source_sequence:
                raise ArchiveError("source audit has an invalid sequence")
            if first <= sequence <= upper:
                candidates.append((sequence, transaction_id, envelope))
        candidates.sort(key=lambda value: value[0])
        if first <= cold_through:
            from .retention import _verify_segment
            from .backup import _safe_source
            for segment in store._state.get("retention_segments", {}).values():
                data = segment["data"]
                if not data["first_audit_sequence"] or data["last_audit_sequence"] < first or \
                        data["first_audit_sequence"] > upper:
                    continue
                reference = _verify_segment(store.root, data)
                with _platform.open_read(_safe_source(store.root, reference["path"]),
                                         nofollow=True) as stream:
                    for line in stream:
                        record = json.loads(line)
                        if record["collection"] != "_audit":
                            continue
                        number = record["envelope"]["data"]["sequence"]
                        if first <= number <= upper:
                            candidates.append((number, record["key"], record["envelope"]))
            candidates.sort(key=lambda value: value[0])
        selected = candidates
        if len(selected) != upper - first + 1:
            raise ArchiveError("source audit range has a missing or duplicate sequence")
        for expected, (sequence, _id, _envelope) in enumerate(selected, first):
            if expected != sequence:
                raise ArchiveError("source audit range has a sequence gap or duplicate")
        records = [envelope["data"] | {"_id": transaction_id,
                                          "_rev": envelope["revision"]}
                   for _sequence, transaction_id, envelope in selected]
    return store_id, source_sequence, requested, records


def _write(stage: Path, records: list[dict], max_bytes: int,
           prior_chain: str) -> tuple[int, int, str, str]:
    digest = hashlib.sha256()
    chain = prior_chain
    count = byte_count = 0
    path = stage / "audit.jsonl"
    with _create_private(path) as stream:
        for row in records:
            line = _json_bytes(row) + b"\n"
            if len(line) > _MAX_LINE:
                raise ArchiveError("one audit record exceeds the archive line bound")
            if byte_count + len(line) > max_bytes:
                if count == 0:
                    raise ArchiveError("one audit record exceeds max_bytes")
                break
            stream.write(line)
            digest.update(line)
            chain = hashlib.sha256(bytes.fromhex(chain) + b"\n" + line).hexdigest()
            count += 1
            byte_count += len(line)
        stream.flush()
        os.fsync(stream.fileno())
    return count, byte_count, digest.hexdigest(), chain


def _write_manifest(stage: Path, body: dict) -> str:
    encoded = _json_bytes(body)
    digest = hashlib.sha256(encoded).hexdigest()
    for name, data in (("manifest.json", encoded),
                       ("manifest.sha256", (digest + "\n").encode("ascii"))):
        with _create_private(stage / name) as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    return digest


def create_archive(store: Store, destination: str | Path, *,
                   previous_archive: str | Path | None = None,
                   through_sequence: int | None = None,
                   max_records: int = 10_000,
                   max_bytes: int = 64 * 1024 * 1024) -> dict:
    """Seal a new contiguous audit segment in a previously absent directory.

    The first segment begins at sequence 1.  Later segments require the prior
    segment to be supplied and verified.  A finite row/byte bound makes a run
    predictable; the returned ``has_more`` flag calls for another segment.
    """
    if type(max_records) is not int or not 1 <= max_records <= 100_000:
        raise ArchiveError("max_records must be 1..100000")
    if type(max_bytes) is not int or not 1024 <= max_bytes <= 2 * 1024**3:
        raise ArchiveError("max_bytes must be 1024..2147483648")
    prior = verify_archive(previous_archive) if previous_archive is not None else None
    first = prior["last_sequence"] + 1 if prior else 1
    store_id, source_sequence, requested, records = _audit_snapshot(
        store, first, through_sequence, max_records)
    if prior and prior["store_id"] != store_id:
        raise ArchiveError("previous archive belongs to another store")
    prior_manifest = prior["manifest_sha256"] if prior else _ZERO
    prior_chain = prior["chain_sha256"] if prior else _ZERO
    try:
        target = _destination(destination)
    except BackupError as exc:
        raise ArchiveError(str(exc)) from exc
    stage = Path(_platform.mkdtemp_private(prefix=f".{target.name}.", dir=target.parent))
    try:
        count, byte_count, audit_sha, chain = _write(stage, records, max_bytes,
                                                     prior_chain)
        last = first + count - 1
        manifest = {
            "format": FORMAT, "store_id": store_id,
            "created_at": datetime.now(timezone.utc).isoformat(),
            "source_sequence": source_sequence,
            "requested_through_sequence": requested,
            "first_sequence": first, "last_sequence": last,
            "record_count": count, "audit_sha256": audit_sha,
            "audit_bytes": byte_count,
            "previous_manifest_sha256": prior_manifest,
            "previous_chain_sha256": prior_chain,
            "chain_sha256": chain,
        }
        digest = _write_manifest(stage, manifest)
        _fsync_dir(stage)
        try:
            _publish_absent(stage, target)
        except BackupError as exc:
            raise ArchiveError(str(exc)) from exc
    finally:
        # Cleanup must not mask the original error; a leftover ".<name>.<hex>"
        # stage holds no published evidence and can be deleted by the operator.
        if stage.exists():
            _platform.rmtree(stage, ignore_errors=True)
    return {"path": str(target), "store_id": store_id,
            "first_sequence": first, "last_sequence": last,
            "source_sequence": source_sequence, "record_count": count,
            "manifest_sha256": digest, "chain_sha256": chain,
            "has_more": last < requested}


def _verify_segment(path: str | Path, prior: Mapping[str, Any] | None = None,
                    selector: Callable[[dict], None] | None = None) -> dict:
    root = _archive_dir(path)
    body, manifest_digest = _manifest(root)
    first = body["first_sequence"]
    if prior is not None:
        if (body["store_id"] != prior["store_id"] or
                first != prior["last_sequence"] + 1 or
                body["previous_manifest_sha256"] != prior["manifest_sha256"] or
                body["previous_chain_sha256"] != prior["chain_sha256"]):
            raise ArchiveError("archive does not continue the supplied previous segment")
    elif first != 1:
        # The individual file can be checked without its predecessor, but
        # continuity back to transaction 1 has not been established.
        pass
    expected = first
    byte_count = 0
    digest = hashlib.sha256()
    chain = body["previous_chain_sha256"]
    with _regular(root / "audit.jsonl") as stream:
        while True:
            line = stream.readline(_MAX_LINE + 1)
            if not line:
                break
            if len(line) > _MAX_LINE or not line.endswith(b"\n"):
                raise ArchiveError("audit archive has an oversized or partial line")
            byte_count += len(line)
            if byte_count > body["audit_bytes"]:
                raise ArchiveError("audit archive exceeds its manifest size")
            digest.update(line)
            chain = hashlib.sha256(bytes.fromhex(chain) + b"\n" + line).hexdigest()
            try:
                row = json.loads(line, object_pairs_hook=_object_pairs)
            except (ValueError, UnicodeError, TypeError) as exc:
                raise ArchiveError("audit archive line is invalid JSON") from exc
            if (not isinstance(row, dict) or
                    set(row) != {"_id", "_rev", "sequence", "actor",
                                 "committed_at", "changes"} or
                    type(row["sequence"]) is not int or row["sequence"] != expected or
                    type(row["_rev"]) is not int or row["_rev"] != 1 or
                    not isinstance(row["actor"], str) or
                    not isinstance(row["changes"], list)):
                raise ArchiveError("audit archive record shape or sequence is invalid")
            for change in row["changes"]:
                if (not isinstance(change, dict) or
                        set(change) != {"collection", "key", "before_revision",
                                        "before", "after_revision", "after"} or
                        not isinstance(change["collection"], str) or
                        not change["collection"] or
                        not isinstance(change["key"], str) or not change["key"] or
                        any(value is not None and (type(value) is not int or value < 1)
                            for value in (change["before_revision"],
                                          change["after_revision"])) or
                        (change["before"] is None) !=
                        (change["before_revision"] is None) or
                        (change["after"] is None) !=
                        (change["after_revision"] is None) or
                        (change["after_revision"] is None and
                         change["before_revision"] is None) or
                        (change["after_revision"] is not None and
                         change["after_revision"] !=
                         (change["before_revision"] or 0) + 1) or
                        any(value is not None and not isinstance(value, dict)
                            for value in (change["before"], change["after"]))):
                    raise ArchiveError("audit archive change has an invalid shape")
            try:
                UUID(row["_id"])
                observed = datetime.fromisoformat(row["committed_at"].replace("Z", "+00:00"))
                if observed.tzinfo is None or observed.utcoffset() is None:
                    raise ValueError
            except (ValueError, TypeError, AttributeError) as exc:
                raise ArchiveError("audit archive transaction identity or time is invalid") from exc
            if line != _json_bytes(row) + b"\n":
                raise ArchiveError("audit archive record is not canonical JSON")
            if selector is not None:
                selector(row)
            expected += 1
    if (expected - 1 != body["last_sequence"] or
            byte_count != body["audit_bytes"] or
            digest.hexdigest() != body["audit_sha256"] or
            chain != body["chain_sha256"]):
        raise ArchiveError("audit archive count, digest or chain is invalid")
    return {"path": str(root), "store_id": body["store_id"],
            "first_sequence": first, "last_sequence": body["last_sequence"],
            "record_count": body["record_count"],
            "source_sequence": body["source_sequence"],
            "manifest_sha256": manifest_digest, "chain_sha256": chain,
            "chain_to_genesis_verified": first == 1 or bool(
                prior and prior["chain_to_genesis_verified"])}


def verify_archive(path: str | Path, *,
                   previous_archive: str | Path | None = None) -> dict:
    """Verify file/record hashes and, if supplied, the immediate chain link."""
    prior = _verify_segment(previous_archive) if previous_archive is not None else None
    return _verify_segment(path, prior)


def verify_archive_chain(paths: list[str | Path]) -> dict:
    """Verify every adjacent segment from transaction 1 through the last."""
    if not paths:
        raise ArchiveError("archive chain is empty")
    prior = None
    for path in paths:
        prior = _verify_segment(path, prior)
    assert prior is not None
    if not prior["chain_to_genesis_verified"]:
        raise ArchiveError("archive chain does not begin at sequence 1")
    return {"store_id": prior["store_id"],
            "last_sequence": prior["last_sequence"],
            "manifest_sha256": prior["manifest_sha256"],
            "chain_sha256": prior["chain_sha256"],
            "segments": len(paths), "chain_to_genesis_verified": True}


def query_archive(path: str | Path, *, collection: str,
                  record_id: str | None = None, first_sequence: int | None = None,
                  last_sequence: int | None = None, limit: int = 100,
                  previous_archive: str | Path | None = None) -> dict:
    """Read matching committed changes only after verifying the whole segment."""
    if not isinstance(collection, str) or not collection or len(collection) > 100:
        raise ArchiveError("collection must be a short nonempty name")
    if record_id is not None and (not isinstance(record_id, str) or not record_id or
                                  len(record_id) > 256):
        raise ArchiveError("record_id is invalid")
    if type(limit) is not int or not 1 <= limit <= 10_000:
        raise ArchiveError("limit must be 1..10000")
    if first_sequence is not None and (type(first_sequence) is not int or first_sequence < 1):
        raise ArchiveError("first_sequence is invalid")
    if last_sequence is not None and (type(last_sequence) is not int or last_sequence < 1):
        raise ArchiveError("last_sequence is invalid")
    if first_sequence and last_sequence and first_sequence > last_sequence:
        raise ArchiveError("sequence range is reversed")
    matches: list[dict] = []
    total = 0

    def collect(row: dict) -> None:
        nonlocal total
        if ((first_sequence is not None and row["sequence"] < first_sequence) or
                (last_sequence is not None and row["sequence"] > last_sequence)):
            return
        if collection == "_audit":
            if record_id is None or row["_id"] == record_id:
                total += 1
                if len(matches) < limit:
                    matches.append(row)
            return
        for change in row["changes"]:
            if (not isinstance(change, dict) or
                    change.get("collection") != collection or
                    (record_id is not None and change.get("key") != record_id)):
                continue
            total += 1
            if len(matches) < limit:
                matches.append({"sequence": row["sequence"],
                                "transaction_id": row["_id"],
                                "actor": row["actor"],
                                "committed_at": row["committed_at"],
                                "change": change})

    prior = _verify_segment(previous_archive) if previous_archive is not None else None
    verified = _verify_segment(path, prior, collect)
    return {"archive": verified, "total_matches": total,
            "truncated": total > len(matches), "items": matches}


def reconstruct_record(paths: list[str | Path], *, collection: str,
                       record_id: str, through_sequence: int | None = None) -> dict:
    """Rebuild one document from a complete archive chain through a sequence.

    Every before value is checked against the prior archived after value.  The
    result is historical data, not a write or a restored live collection.
    """
    if not paths:
        raise ArchiveError("archive chain is empty")
    if not isinstance(collection, str) or not collection or len(collection) > 100 or \
            collection == "_audit":
        raise ArchiveError("collection must name a non-audit document collection")
    if not isinstance(record_id, str) or not record_id or len(record_id) > 256:
        raise ArchiveError("record_id is invalid")
    if through_sequence is not None and (type(through_sequence) is not int or
                                         through_sequence < 1):
        raise ArchiveError("through_sequence is invalid")
    state = None
    revision = None
    last_changed = None

    def apply(row: dict) -> None:
        nonlocal state, revision, last_changed
        if through_sequence is not None and row["sequence"] > through_sequence:
            return
        for change in row["changes"]:
            if change["collection"] != collection or change["key"] != record_id:
                continue
            if change["before_revision"] != revision or change["before"] != state:
                raise ArchiveError("document history does not match its preceding version")
            state = change["after"]
            revision = change["after_revision"]
            last_changed = row["sequence"]

    prior = None
    for path in paths:
        prior = _verify_segment(path, prior, apply)
    assert prior is not None
    if not prior["chain_to_genesis_verified"]:
        raise ArchiveError("archive chain does not begin at sequence 1")
    if through_sequence is not None and through_sequence > prior["last_sequence"]:
        raise ArchiveError("requested sequence is not archived")
    return {"store_id": prior["store_id"], "collection": collection,
            "record_id": record_id,
            "as_of_sequence": through_sequence or prior["last_sequence"],
            "last_changed_sequence": last_changed,
            "exists": state is not None, "revision": revision, "data": state,
            "chain_to_genesis_verified": True}
