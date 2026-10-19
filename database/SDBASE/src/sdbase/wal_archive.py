"""Durable, hash-chained recovery records for SDBASE's own journal.

This is an independently written implementation. PostgreSQL's archive-before-
recycle rule and SQLite's checksummed commit/checkpoint barriers informed the
design; neither database engine nor its on-disk format is used here.
"""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import struct
from typing import Any, Iterator
from uuid import UUID, uuid4

from .storage import Store, StoreError, _atomic_json, _frame, _json_bytes, _MAX_FRAME


HEADER_FORMAT = "sdbase-wal-archive-v1"
RECORD_FORMAT = "sdbase-wal-record-v1"
_SHA = re.compile(r"^[0-9a-f]{64}$")
_RECORD = re.compile(r"^(\d{20})\.json$")


class WalArchiveError(StoreError):
    pass


class WalRangeTooLarge(WalArchiveError):
    """One sealed record exceeds the receiver's bounded processing budget."""


_STAMP_FIELDS = {"format", "source_store_id", "timeline_id", "epoch", "schema_version",
                 "sequence", "effective_at"}
_JSON_MARKERS = re.compile(rb'["\\{}\[\]:,]')


class _StampScanner:
    """Extract bounded root scalars while skipping arbitrarily large JSON values."""
    def __init__(self):
        self.depth, self.quoted, self.escaped = 0, False, False
        self.mode, self.key, self.buffer = None, None, bytearray()
        self.fields = {}

    def _capture(self, part):
        if self.mode is not None:
            if len(self.buffer) + len(part) > 2048:
                raise WalArchiveError("WAL anchor scalar exceeds its metadata budget")
            self.buffer.extend(part)

    def _finish_value(self):
        if self.mode == "value":
            if self.key in self.fields:
                raise WalArchiveError("duplicate WAL anchor scalar")
            self.fields[self.key] = json.loads(bytes(self.buffer))
        self.buffer.clear()

    def feed(self, block):
        position = 0
        for match in _JSON_MARKERS.finditer(block):
            start, end = match.span()
            self._capture(block[position:start])
            if self.quoted and self.escaped and start > position:
                self.escaped = False  # Escape consumed a non-marker byte.
            token = block[start:end]
            if self.quoted:
                self._capture(token)
                if self.escaped:
                    self.escaped = False
                elif token == b"\\":
                    self.escaped = True
                elif token == b'"':
                    self.quoted = False
            elif token == b":" and self.depth == 1:
                self.key = json.loads(bytes(self.buffer))
                if not isinstance(self.key, str):
                    raise WalArchiveError("invalid WAL anchor key")
                self.buffer.clear()
                self.mode = "value" if self.key in _STAMP_FIELDS else None
            elif token == b"," and self.depth == 1:
                self._finish_value()
                self.mode = "key"
            elif token == b"}" and self.depth == 1:
                self._finish_value()
                self.mode, self.depth = None, 0
            else:
                self._capture(token)
                if token == b'"':
                    self.quoted = True
                elif token in {b"{", b"["}:
                    self.depth += 1
                    if self.depth == 1:
                        self.mode = "key"
                elif token in {b"}", b"]"}:
                    self.depth -= 1
                    if self.depth < 0:
                        raise WalArchiveError("invalid WAL anchor nesting")
            position = end
        self._capture(block[position:])
        if self.quoted and self.escaped and position < len(block):
            self.escaped = False


def _anchor_footer(stream, expected_sha256=None):
    """Read a canonical envelope's fixed-size suffix, without loading its payload."""
    prefix = b'{"payload":'
    suffix_size = len(b',"sha256":"') + 64 + len(b'"}')
    size = os.fstat(stream.fileno()).st_size
    if size <= len(prefix) + suffix_size:
        raise WalArchiveError("truncated WAL anchor envelope")
    stream.seek(0)
    if stream.read(len(prefix)) != prefix:
        raise WalArchiveError("WAL anchor envelope is not canonical")
    stream.seek(size - suffix_size)
    suffix = stream.read(suffix_size)
    match = re.fullmatch(rb',"sha256":"([0-9a-f]{64})"}', suffix)
    if not match:
        raise WalArchiveError("invalid WAL anchor suffix")
    digest = match.group(1).decode("ascii")
    if expected_sha256 is not None and digest != expected_sha256:
        raise WalArchiveError("pinned predecessor differs from the receiver's applied archive anchor")
    return digest, len(prefix), size - len(prefix) - suffix_size


def archive_anchor_stamp(store: Store, sequence: int, *, expected_sha256: str | None = None,
                         max_scan_bytes: int = 64 * 1024 * 1024 * 1024) -> dict:
    """One-time, 256 KiB-memory SHA/timestamp extraction for a physical base.

    Replay then persists this timestamp beside the known digest. Old 0.7
    standbys can obtain the same stamp once through their authenticated source.
    No checkpoint/document tree or directory listing is allocated here.
    """
    from .backup import _open_regular
    if type(sequence) is not int or sequence < 0 or \
            expected_sha256 is not None and (not isinstance(expected_sha256, str) or not _SHA.fullmatch(expected_sha256)) or \
            type(max_scan_bytes) is not int or max_scan_bytes < 65536:
        raise WalArchiveError("invalid WAL anchor stamp request")
    with store._locked(exclusive=False):
        store._reload_locked()
        identity = _identity_locked(store)
        if identity is None:
            raise WalArchiveError("WAL archival is not enabled")
        root, header = identity
        if not header["payload"]["start_sequence"] <= sequence <= store._sequence:
            raise WalArchiveError("WAL anchor is outside the durable source timeline")
    if sequence == header["payload"]["start_sequence"]:
        if expected_sha256 is not None and expected_sha256 != header["sha256"]:
            raise WalArchiveError("WAL header differs from the pinned anchor")
        return {"sequence": sequence, "sha256": header["sha256"], "effective_at": header["payload"]["created_at"]}
    try:
        with os.fdopen(_open_regular(_safe(root, f"records/{sequence:020d}.json")), "rb") as stream:
            digest, offset, remaining = _anchor_footer(stream, expected_sha256)
            if remaining > max_scan_bytes:
                raise WalRangeTooLarge("one-time WAL anchor stamp exceeds the bounded scan budget")
            stream.seek(offset)
            hashed, scanner = hashlib.sha256(), _StampScanner()
            while remaining:
                block = stream.read(min(256 * 1024, remaining))
                if not block:
                    raise WalArchiveError("WAL anchor changed during stamp extraction")
                hashed.update(block)
                scanner.feed(block)
                remaining -= len(block)
        fields = scanner.fields
        if scanner.depth or scanner.quoted or set(fields) != _STAMP_FIELDS or hashed.hexdigest() != digest or \
                fields["format"] != RECORD_FORMAT or type(fields["sequence"]) is not int or fields["sequence"] != sequence or \
                type(fields["epoch"]) is not int or type(fields["schema_version"]) is not int or \
                any(fields[key] != header["payload"][key] for key in ("source_store_id", "timeline_id", "epoch", "schema_version")):
            raise WalArchiveError("WAL anchor stamp checksum, identity or sequence mismatch")
        _instant(fields["effective_at"])
        return {"sequence": sequence, "sha256": digest, "effective_at": fields["effective_at"]}
    except (OSError, ValueError, UnicodeError, TypeError) as exc:
        raise WalArchiveError("unreadable WAL anchor stamp") from exc


def _bounded_envelope(path: Path, max_bytes: int) -> dict:
    from .backup import _open_regular
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise WalArchiveError("duplicate WAL JSON key")
            result[key] = value
        return result
    try:
        with os.fdopen(_open_regular(path), "rb") as stream:
            if os.fstat(stream.fileno()).st_size > max_bytes:
                raise WalRangeTooLarge("sealed WAL record exceeds the range byte budget; increase limits or rebase")
            encoded = stream.read(max_bytes + 1)
        if len(encoded) > max_bytes:
            raise WalRangeTooLarge("sealed WAL record exceeds the range byte budget; increase limits or rebase")
        record = json.loads(encoded, object_pairs_hook=pairs)
        if not isinstance(record, dict) or set(record) != {"payload", "sha256"} or \
                not isinstance(record["payload"], dict) or record["sha256"] != _digest(record["payload"]):
            raise WalArchiveError("WAL archive checksum mismatch")
        return record
    except (OSError, ValueError, UnicodeError) as exc:
        raise WalArchiveError("unreadable selected WAL archive record") from exc


def iter_pinned_archive_records(store: Store, *, after_sequence: int,
        through_sequence: int, expected_previous_sha256: str,
        expected_previous_effective_at: str | None = None,
        max_records: int = 10000, max_record_bytes: int = 256 * 1024 * 1024,
        verify_assets: bool = True) -> Iterator[dict]:
    """Read only a pinned predecessor and selected contiguous immutable records.

    A physical base or previous applied batch establishes hash AND timestamp.
    This does not certify the older global archive history: keep full archive
    verification/PITR for that audit. Every selected record is hash/identity/
    timestamp chained; selected objects can be verified here or by a receiver's
    subsequent verified copy. No record-directory enumeration is performed.
    """
    if type(after_sequence) is not int or type(through_sequence) is not int or \
            not 0 <= after_sequence <= through_sequence or type(max_records) is not int or \
            not 1 <= max_records <= 10000 or through_sequence - after_sequence > max_records or \
            type(max_record_bytes) is not int or not 65536 <= max_record_bytes <= 256 * 1024 * 1024 or \
            not isinstance(expected_previous_sha256, str) or not _SHA.fullmatch(expected_previous_sha256) or \
            type(verify_assets) is not bool:
        raise WalArchiveError("invalid bounded pinned WAL range")
    with store._locked(exclusive=False):
        store._reload_locked()
        identity = _identity_locked(store)
        if identity is None:
            raise WalArchiveError("WAL archival is not enabled")
        root, header = identity
        if after_sequence < header["payload"]["start_sequence"] or through_sequence > store._sequence:
            raise WalArchiveError("selected WAL range is outside the durable source timeline")
    previous = header["sha256"]
    instant = _instant(header["payload"]["created_at"])
    if after_sequence != header["payload"]["start_sequence"]:
        if expected_previous_effective_at is None:
            stamp = archive_anchor_stamp(store, after_sequence, expected_sha256=expected_previous_sha256)
            previous, instant = stamp["sha256"], _instant(stamp["effective_at"])
        else:
            from .backup import _open_regular
            with os.fdopen(_open_regular(_safe(root, f"records/{after_sequence:020d}.json")), "rb") as stream:
                previous, _offset, _size = _anchor_footer(stream, expected_previous_sha256)
            instant = _instant(expected_previous_effective_at)
            if instant <= _instant(header["payload"]["created_at"]):
                raise WalArchiveError("pinned predecessor timestamp precedes the archive")
    elif expected_previous_effective_at is not None and _instant(expected_previous_effective_at) != instant:
        raise WalArchiveError("pinned header timestamp differs from the archive")
    if previous != expected_previous_sha256:
        raise WalArchiveError("pinned predecessor differs from the receiver's applied archive anchor")
    checked = set()
    for sequence in range(after_sequence + 1, through_sequence + 1):
        record = _bounded_envelope(_safe(root, f"records/{sequence:020d}.json"), max_record_bytes)
        row = validate_archive_record(record, header)
        if row["sequence"] != sequence or row["previous_sha256"] != previous or _instant(row["effective_at"]) <= instant:
            raise WalArchiveError("selected WAL range contains a gap, fork or unordered timestamp")
        if verify_assets:
            for asset in row["assets"]:
                token = (asset["sha256"], asset["size_bytes"])
                if token not in checked:
                    _verify_asset(root, asset)
                    checked.add(token)
        previous, instant = record["sha256"], _instant(row["effective_at"])
        yield record


class ArchiveAcknowledgementError(WalArchiveError):
    """The original commit is durable, but its recovery archive was not acknowledged."""

    def __init__(self, sequence: int, transaction_id: str):
        self.sequence = sequence
        self.transaction_id = transaction_id
        super().__init__(f"transaction {transaction_id} committed durably at sequence {sequence}; "
                         "WAL archive acknowledgement failed; read its result before retrying")


def _digest(value: Any) -> str:
    return hashlib.sha256(_json_bytes(value)).hexdigest()


def _instant(value: str) -> datetime:
    try:
        result = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if result.tzinfo is None:
            raise ValueError
        return result.astimezone(timezone.utc)
    except (ValueError, TypeError, AttributeError) as exc:
        raise WalArchiveError("WAL timestamp must contain an explicit timezone") from exc


def _uuid(value: Any) -> bool:
    try:
        return isinstance(value, str) and str(UUID(value)) == value
    except ValueError:
        return False


def archive_root(store: Store) -> Path:
    return store.root / (store.journal.name + ".wal")


def _safe(root: Path, relative: str) -> Path:
    from .backup import _safe_source
    return _safe_source(root, relative)


def _read_json(path: Path) -> dict:
    from .backup import _open_regular
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise WalArchiveError("duplicate WAL JSON key")
            result[key] = value
        return result
    try:
        with os.fdopen(_open_regular(path), "rb") as stream:
            value = json.load(stream, object_pairs_hook=pairs)
    except (OSError, ValueError, UnicodeError) as exc:
        raise WalArchiveError("unreadable WAL archive component") from exc
    if not isinstance(value, dict):
        raise WalArchiveError("WAL archive component must be an object")
    return value


def _envelope(path: Path) -> dict:
    value = _read_json(path)
    if set(value) != {"payload", "sha256"} or not isinstance(value["payload"], dict) or \
            not isinstance(value["sha256"], str) or not _SHA.fullmatch(value["sha256"]) or \
            _digest(value["payload"]) != value["sha256"]:
        raise WalArchiveError("WAL archive checksum mismatch")
    return value


def _header(root: Path) -> dict:
    envelope = _envelope(_safe(root, "header.json"))
    row = envelope["payload"]
    required = {"format", "archive_id", "source_store_id", "timeline_id", "epoch",
                "schema_version", "database", "start_sequence", "created_at"}
    if set(row) != required or row["format"] != HEADER_FORMAT or \
            not all(_uuid(row[key]) for key in ("archive_id", "source_store_id", "timeline_id")) or \
            type(row["epoch"]) is not int or row["epoch"] < 1 or \
            type(row["schema_version"]) is not int or row["schema_version"] < 1 or \
            type(row["start_sequence"]) is not int or row["start_sequence"] < 0 or \
            not isinstance(row["database"], str) or \
            Path(row["database"]).name != row["database"] or not row["database"].endswith(".sdb"):
        raise WalArchiveError("invalid WAL archive header")
    _instant(row["created_at"])
    return envelope


def _root(path: str | Path) -> Path:
    raw = Path(path)
    if raw.is_symlink() or not raw.is_dir():
        raise WalArchiveError("WAL archive root is absent or a symlink")
    return raw.resolve(strict=True)


def _record_paths(root: Path) -> list[tuple[int, Path]]:
    records = _safe(root, "records")
    if not records.is_dir():
        raise WalArchiveError("WAL record directory is missing")
    result = []
    for item in records.iterdir():
        if item.is_symlink() or not item.is_file():
            raise WalArchiveError("invalid WAL record filename or component")
        if re.fullmatch(r"\.\d{20}\.json\.[0-9a-f]{32}\.partial", item.name):
            # A crash before sealing may leave an uncommitted temporary file.
            # It is not a segment and is never considered a recovery record.
            continue
        if not (match := _RECORD.fullmatch(item.name)):
            raise WalArchiveError("invalid WAL record filename")
        result.append((int(match.group(1)), item))
    return sorted(result)


def _asset_path(digest: str) -> str:
    return f"assets/{digest[:2]}/{digest}"


def _validate_assets(assets: Any, database: str) -> None:
    from .backup import _SHA256
    if not isinstance(assets, list):
        raise WalArchiveError("invalid WAL asset inventory")
    seen = set()
    for asset in assets:
        if not isinstance(asset, dict) or set(asset) != {"path", "sha256", "size_bytes"} or \
                not isinstance(asset["path"], str) or asset["path"] in seen or \
                not isinstance(asset["sha256"], str) or not _SHA256.fullmatch(asset["sha256"]) or \
                type(asset["size_bytes"]) is not int or asset["size_bytes"] < 0:
            raise WalArchiveError("invalid or duplicated WAL asset")
        seen.add(asset["path"])
        parts = Path(asset["path"]).parts
        media = len(parts) == 3 and parts[0] == database + ".objects" and \
            parts[1] == asset["sha256"][:2] and parts[2] == asset["sha256"]
        cold = len(parts) == 3 and parts[0] == database + ".archives" and \
            _uuid(parts[1]) and parts[2] in {"manifest.json", "records.jsonl"}
        if not (media or cold):
            raise WalArchiveError("unsafe or unsupported WAL asset path")


def validate_archive_record(record: dict, header: dict | None = None) -> dict:
    """Validate a sealed record, independently usable by a replica receiver."""
    if not isinstance(record, dict) or set(record) != {"payload", "sha256"} or \
            not isinstance(record["payload"], dict) or record["sha256"] != _digest(record["payload"]):
        raise WalArchiveError("invalid WAL record checksum")
    row = record["payload"]
    common = {"format", "source_store_id", "timeline_id", "epoch", "schema_version",
              "sequence", "previous_sha256", "effective_at", "kind", "assets"}
    specific = {"transaction"} if row.get("kind") == "transaction" else {"checkpoint"}
    if set(row) != common | specific or row["format"] != RECORD_FORMAT or \
            row["kind"] not in {"transaction", "checkpoint"} or \
            type(row["sequence"]) is not int or row["sequence"] < 1 or \
            not all(_uuid(row[key]) for key in ("source_store_id", "timeline_id")) or \
            type(row["epoch"]) is not int or row["epoch"] < 1 or \
            type(row["schema_version"]) is not int or row["schema_version"] < 1 or \
            not isinstance(row["previous_sha256"], str) or not _SHA.fullmatch(row["previous_sha256"]):
        raise WalArchiveError("invalid WAL record identity")
    _instant(row["effective_at"])
    if header:
        identity = header["payload"] if "payload" in header else header
        for key in ("source_store_id", "timeline_id", "epoch", "schema_version"):
            if row[key] != identity[key]:
                raise WalArchiveError("WAL source timeline, epoch, or schema mismatch")
        database = identity["database"]
    else:
        # A replica verifies paths against its source database separately.
        if not isinstance(row["assets"], list):
            raise WalArchiveError("invalid WAL asset inventory")
        roots = {Path(a.get("path", "")).parts[0] for a in row["assets"]
                 if isinstance(a, dict) and Path(a.get("path", "")).parts}
        names = {name.removesuffix(".objects").removesuffix(".archives") for name in roots}
        if len(names) > 1:
            raise WalArchiveError("WAL assets name more than one database")
        database = next(iter(names), "RadioS.sdb")
    _validate_assets(row["assets"], database)
    value = row[next(iter(specific))]
    if not isinstance(value, dict) or value.get("sequence") != row["sequence"] or \
            value.get("format") != "sdbase-journal-v1":
        raise WalArchiveError("WAL body differs from its record sequence or format")
    if row["kind"] == "transaction":
        _frame(value)  # Includes the engine's maximum transaction bound.
        _instant(value.get("committed_at"))
        if not isinstance(value.get("operations"), list) or not value["operations"]:
            raise WalArchiveError("WAL transaction has no operations")
        seen = set()
        for operation in value["operations"]:
            if not isinstance(operation, dict) or operation.get("kind") not in {"put", "delete"} or \
                    not isinstance(operation.get("collection"), str) or not operation["collection"] or \
                    not isinstance(operation.get("key"), str) or not operation["key"]:
                raise WalArchiveError("invalid WAL transaction operation")
            token = (operation["collection"], operation["key"])
            if token in seen:
                raise WalArchiveError("duplicate WAL transaction operation")
            seen.add(token)
            if operation["kind"] == "put":
                record = operation.get("record")
                if not isinstance(record, dict) or type(record.get("revision")) is not int or \
                        record["revision"] < 1 or not isinstance(record.get("data"), dict):
                    raise WalArchiveError("invalid WAL transaction revision or data")
    elif not isinstance(value.get("state"), dict) or not isinstance(value.get("tombstones"), dict):
        raise WalArchiveError("invalid WAL checkpoint body")
    return row


def _verify_asset(root: Path, asset: dict) -> None:
    from .backup import _digest_file
    digest, size = _digest_file(_safe(root, _asset_path(asset["sha256"])))
    if digest != asset["sha256"] or size != asset["size_bytes"]:
        raise WalArchiveError("WAL media or cold history object is damaged")


def read_archive_records(path: str | Path, *, after_sequence: int = 0,
                         through_sequence: int | None = None,
                         source_store_id: str | None = None, timeline_id: str | None = None,
                         epoch: int | None = None) -> list[dict]:
    """Verify the complete chain and all objects; return the requested range.

    A missing middle segment and a truncated tail segment are errors. A copied
    archive may legitimately end at its sealed head; callers must require their
    intended recovery target rather than treating the available end as success.
    """
    root = _root(path)
    header = _header(root)
    identity = header["payload"]
    for key, requested in (("source_store_id", source_store_id), ("timeline_id", timeline_id),
                           ("epoch", epoch)):
        if requested is not None and requested != identity[key]:
            raise WalArchiveError("requested WAL source identity differs from archive")
    if type(after_sequence) is not int or after_sequence < 0 or \
            through_sequence is not None and (type(through_sequence) is not int or
                                             through_sequence < after_sequence):
        raise WalArchiveError("invalid WAL sequence range")
    previous, sequence = header["sha256"], identity["start_sequence"]
    instant = _instant(identity["created_at"])
    result, checked = [], set()
    for number, file in _record_paths(root):
        record = _envelope(file)
        row = validate_archive_record(record, header)
        if number != sequence + 1 or row["sequence"] != number or \
                row["previous_sha256"] != previous or _instant(row["effective_at"]) <= instant:
            raise WalArchiveError("WAL chain contains a gap, fork, or unordered timestamp")
        for asset in row["assets"]:
            token = (asset["sha256"], asset["size_bytes"])
            if token not in checked:
                _verify_asset(root, asset)
                checked.add(token)
        if number > after_sequence and (through_sequence is None or number <= through_sequence):
            result.append(record)
        previous, sequence, instant = record["sha256"], number, _instant(row["effective_at"])
    head = _read_json(_safe(root, "head.json"))
    if head != {"sequence": sequence, "sha256": previous, "effective_at": instant.isoformat()}:
        raise WalArchiveError("WAL head does not match its sealed chain")
    if through_sequence is not None and sequence < through_sequence:
        raise WalArchiveError("WAL archive ends before the requested target")
    return result


def _identity_locked(store: Store) -> tuple[Path, dict] | None:
    meta = store._meta()
    config = meta.get("wal_archive")
    if config is None:
        return None
    if not isinstance(config, dict) or set(config) != {"archive_id", "start_sequence"}:
        raise WalArchiveError("invalid WAL archive configuration")
    root = _root(archive_root(store))
    header = _header(root)
    body = header["payload"]
    if body["archive_id"] != config["archive_id"] or body["start_sequence"] != config["start_sequence"] or \
            body["source_store_id"] != meta.get("store_id") or \
            body["timeline_id"] != meta.get("timeline_id") or body["epoch"] != meta.get("epoch") or \
            body["schema_version"] != store.schema_version or body["database"] != store.journal.name:
        raise WalArchiveError("WAL archive is attached to another store or timeline")
    return root, header


def enable_wal_archive(store: Store) -> dict:
    """Start archiving future commits; take a base backup before relying on PITR."""
    if store.read_only:
        raise WalArchiveError("cannot enable WAL archival on a read-only store")
    from .backup import _destination, _fsync_tree, _publish_absent
    import shutil
    import tempfile
    with store._locked(exclusive=True):
        store._assert_writable_locked()
        store._reload_locked(repair_tail=True)
        if _identity_locked(store):
            archive_pending_locked(store)
            return _status_locked(store, verify=True)
        meta = store._meta()
        if meta.get("node_role", "primary") != "primary":
            raise WalArchiveError("only a primary may start a WAL timeline")
        if not _uuid(meta.get("timeline_id")) or type(meta.get("epoch")) is not int or meta["epoch"] < 1:
            raise WalArchiveError("store needs a valid primary timeline and epoch before WAL archival")
        existing = archive_root(store)
        if os.path.lexists(existing):
            # Publication precedes the metadata attachment. A process can die
            # between those two fsynced operations without losing any commits.
            header = _header(_root(existing))
            body = header["payload"]
            expected = {"source_store_id": meta["store_id"], "timeline_id": meta["timeline_id"],
                        "epoch": meta["epoch"], "schema_version": store.schema_version,
                        "database": store.journal.name, "start_sequence": store._sequence}
            if any(body[key] != value for key, value in expected.items()) or read_archive_records(existing):
                raise WalArchiveError("unattached WAL directory is not a recoverable enable operation")
            meta["wal_archive"] = {"archive_id": body["archive_id"], "start_sequence": body["start_sequence"]}
            _atomic_json(store.meta_file, meta)
            return _status_locked(store, verify=True)
        target = _destination(archive_root(store))
        stage = Path(tempfile.mkdtemp(prefix=".wal-enable-", dir=store.root))
        try:
            body = {"format": HEADER_FORMAT, "archive_id": str(uuid4()),
                    "source_store_id": meta["store_id"], "timeline_id": meta["timeline_id"],
                    "epoch": meta["epoch"], "schema_version": store.schema_version,
                    "database": store.journal.name, "start_sequence": store._sequence,
                    "created_at": datetime.now(timezone.utc).isoformat()}
            envelope = {"payload": body, "sha256": _digest(body)}
            (stage / "records").mkdir(mode=0o700)
            (stage / "assets").mkdir(mode=0o700)
            _atomic_json(stage / "header.json", envelope)
            _atomic_json(stage / "head.json", {"sequence": store._sequence,
                         "sha256": envelope["sha256"], "effective_at": body["created_at"]})
            _fsync_tree(stage)
            _publish_absent(stage, target)
            meta["wal_archive"] = {"archive_id": body["archive_id"], "start_sequence": store._sequence}
            _atomic_json(store.meta_file, meta)
        finally:
            if stage.exists():
                shutil.rmtree(stage)
        return _status_locked(store, verify=True)


def _head_locked(root: Path, header: dict) -> dict:
    """Repair only a sealed extra record caused by a crash before head update."""
    head = _read_json(_safe(root, "head.json"))
    if set(head) != {"sequence", "sha256", "effective_at"} or \
            type(head["sequence"]) is not int or head["sequence"] < header["payload"]["start_sequence"] or \
            not isinstance(head["sha256"], str) or not _SHA.fullmatch(head["sha256"]):
        raise WalArchiveError("invalid WAL archive head")
    _instant(head["effective_at"])
    if head["sequence"] == header["payload"]["start_sequence"]:
        if head["sha256"] != header["sha256"] or head["effective_at"] != header["payload"]["created_at"]:
            raise WalArchiveError("invalid WAL archive starting anchor")
    else:
        prior = _envelope(_safe(root, f"records/{head['sequence']:020d}.json"))
        validate_archive_record(prior, header)
        if prior["sha256"] != head["sha256"] or prior["payload"]["effective_at"] != head["effective_at"]:
            raise WalArchiveError("WAL archive head anchor differs from its record")
    while (next_path := root / "records" / f"{head['sequence'] + 1:020d}.json").exists():
        record = _envelope(_safe(root, next_path.relative_to(root).as_posix()))
        row = validate_archive_record(record, header)
        if row["sequence"] != head["sequence"] + 1 or row["previous_sha256"] != head["sha256"] or \
                _instant(row["effective_at"]) <= _instant(head["effective_at"]):
            raise WalArchiveError("WAL sealed tail forks from its head")
        for asset in row["assets"]:
            _verify_asset(root, asset)
        head = {"sequence": row["sequence"], "sha256": record["sha256"],
                "effective_at": row["effective_at"]}
        _atomic_json(root / "head.json", head)
    return head


def _assets_from_state(state: dict, database: str) -> list[dict]:
    from .backup import _media_refs
    from .retention import archive_file_refs
    assets = [{key: obj[key] for key in ("path", "sha256", "size_bytes")}
              for obj in _media_refs(state, database)]
    assets.extend(archive_file_refs(state, database))
    return sorted(assets, key=lambda item: item["path"])


def _transaction_assets(store: Store, txn: dict) -> list[dict]:
    # Only objects referenced by changed rows are newly required. Older objects
    # are already in the base backup or an earlier recovery record.
    state = {"media_files": dict(store._state.get("media_files", {})),
             "media_locations": {}, "retention_segments": {}}
    for operation in txn["operations"]:
        collection = operation["collection"]
        if operation["kind"] == "put" and collection in state:
            state[collection][operation["key"]] = operation["record"]
    return _assets_from_state(state, store.journal.name)


def _capture_assets(root: Path, store: Store, assets: list[dict]) -> None:
    from .backup import _copy_file, _fsync_dir
    for asset in assets:
        relative = _asset_path(asset["sha256"])
        target = _safe(root, relative)
        if target.exists():
            _verify_asset(root, asset)
            continue
        target.parent.mkdir(mode=0o700, exist_ok=True)
        temporary = target.with_name("." + target.name + "." + uuid4().hex + ".partial")
        try:
            _copy_file(_safe(store.root, asset["path"]), temporary,
                       expected_sha=asset["sha256"], expected_size=asset["size_bytes"])
            os.link(temporary, target, follow_symlinks=False)
            _fsync_dir(target.parent)
            _fsync_dir(target.parent.parent)
        finally:
            temporary.unlink(missing_ok=True)


def _append_locked(store: Store, *, kind: str, value: dict, committed_at: str) -> None:
    identity = _identity_locked(store)
    if identity is None:
        return
    root, header = identity
    head = _head_locked(root, header)
    sequence = value["sequence"]
    if sequence <= head["sequence"]:
        prior = _envelope(_safe(root, f"records/{sequence:020d}.json"))
        if prior["payload"].get(kind) != value:
            raise WalArchiveError("archived sequence contains another committed state")
        return
    if sequence != head["sequence"] + 1:
        raise WalArchiveError("cannot archive a sequence gap")
    from datetime import timedelta
    effective = max(_instant(committed_at), _instant(head["effective_at"]) + timedelta(microseconds=1))
    assets = (_transaction_assets(store, value) if kind == "transaction" else
              _assets_from_state(value["state"], store.journal.name))
    _capture_assets(root, store, assets)
    body = {"format": RECORD_FORMAT,
            **{key: header["payload"][key] for key in
               ("source_store_id", "timeline_id", "epoch", "schema_version")},
            "sequence": sequence, "previous_sha256": head["sha256"],
            "effective_at": effective.isoformat(), "kind": kind, kind: value, "assets": assets}
    record = {"payload": body, "sha256": _digest(body)}
    validate_archive_record(record, header)
    # Exclusive database locks serialize archive writers. Publish sealed record
    # atomically and never replace an existing record at the same sequence.
    from .backup import _fsync_dir
    path = _safe(root, f"records/{sequence:020d}.json")
    temporary = path.with_name("." + path.name + "." + uuid4().hex + ".partial")
    try:
        with temporary.open("xb") as stream:
            os.chmod(temporary, 0o600)
            stream.write(_json_bytes(record))
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, path, follow_symlinks=False)
        _fsync_dir(path.parent)
    finally:
        temporary.unlink(missing_ok=True)
    _atomic_json(root / "head.json", {"sequence": sequence, "sha256": record["sha256"],
                                     "effective_at": body["effective_at"]})


def journal_transactions(path: Path) -> Iterator[dict]:
    """Strictly parse complete original frames; never repair an archive source."""
    from .backup import _open_regular
    with os.fdopen(_open_regular(path), "rb") as stream:
        while prefix := stream.read(4):
            if len(prefix) != 4:
                raise WalArchiveError("incomplete source journal frame")
            size = struct.unpack(">I", prefix)[0]
            if not 0 < size <= _MAX_FRAME:
                raise WalArchiveError("invalid source journal frame size")
            encoded, checksum = stream.read(size), stream.read(32)
            if len(encoded) != size or len(checksum) != 32 or hashlib.sha256(encoded).digest() != checksum:
                raise WalArchiveError("incomplete or damaged source journal frame")
            try:
                value = json.loads(encoded)
            except ValueError as exc:
                raise WalArchiveError("invalid source journal JSON") from exc
            if not isinstance(value, dict):
                raise WalArchiveError("invalid source transaction")
            yield value


def archive_pending_locked(store: Store) -> None:
    """Catch up a crash gap before a writer may retire journal records.

    Caller holds the exclusive database lock and has reloaded/repaired its
    journal. A durable maintenance checkpoint can supply its missing full-state
    barrier, but a normal checkpoint cannot conceal an unarchived transaction.
    """
    identity = _identity_locked(store)
    if identity is None:
        return
    root, header = identity
    head = _head_locked(root, header)
    if head["sequence"] > store._sequence:
        raise WalArchiveError("archive contains an uncommitted future sequence")
    if head["sequence"] == store._sequence:
        return
    for txn in journal_transactions(store.journal):
        if txn.get("sequence", 0) > head["sequence"]:
            _append_locked(store, kind="transaction", value=txn, committed_at=txn["committed_at"])
            head = _head_locked(root, header)
    if head["sequence"] < store._sequence:
        payload = _envelope(store.checkpoint_file)["payload"]
        candidates = [row["data"] for row in payload.get("state", {}).get("_audit", {}).values()
                      if row.get("data", {}).get("sequence") == payload["sequence"]]
        is_maintenance = any(any(change.get("collection") == "retention_segments"
                            for change in row.get("changes", [])) for row in candidates)
        if not is_maintenance or payload["sequence"] != head["sequence"] + 1:
            raise WalArchiveError("unarchived committed journal was retired; a new base is required")
        _append_locked(store, kind="checkpoint", value=payload,
                       committed_at=candidates[0]["committed_at"])


def archive_transaction_locked(store: Store, txn: dict) -> None:
    """Commit hook: call after journal fsync, before acknowledging the commit."""
    try:
        identity = _identity_locked(store)
        if identity is None:
            return
        # The writer catches up any previous failed acknowledgement before adding
        # this frame. Avoid rescanning an ever-growing journal on every commit.
        _append_locked(store, kind="transaction", value=txn, committed_at=txn["committed_at"])
    except Exception as exc:
        raise ArchiveAcknowledgementError(txn["sequence"], txn["transaction_id"]) from exc


def archive_checkpoint_locked(store: Store, payload: dict, committed_at: str) -> None:
    """Maintenance hook, after checkpoint fsync and before journal truncation."""
    try:
        identity = _identity_locked(store)
        if identity is None:
            return
        root, header = identity
        head = _head_locked(root, header)
        for txn in journal_transactions(store.journal):
            if head["sequence"] < txn.get("sequence", 0) < payload["sequence"]:
                _append_locked(store, kind="transaction", value=txn, committed_at=txn["committed_at"])
                head = _head_locked(root, header)
        _append_locked(store, kind="checkpoint", value=payload, committed_at=committed_at)
    except Exception as exc:
        transaction_id = next((key for key, record in payload.get("state", {}).get("_audit", {}).items()
                               if record["data"].get("sequence") == payload["sequence"]), "maintenance")
        raise ArchiveAcknowledgementError(payload["sequence"], transaction_id) from exc


def _status_locked(store: Store, *, verify: bool) -> dict:
    identity = _identity_locked(store)
    if identity is None:
        return {"enabled": False, "store_sequence": store._sequence}
    root, header = identity
    if verify:
        read_archive_records(root)
    head = _read_json(_safe(root, "head.json"))
    return {"enabled": True, "path": str(root), **header["payload"],
            "last_sequence": head["sequence"], "last_record_sha256": head["sha256"],
            "last_effective_at": head["effective_at"], "store_sequence": store._sequence,
            "unarchived_sequences": max(0, store._sequence - head["sequence"])}


def archive_status(store: Store, *, verify: bool = True) -> dict:
    with store._locked(exclusive=False):
        store._reload_locked()
        return _status_locked(store, verify=verify)


def archive_anchor(store: Store, sequence: int) -> str:
    """Return the exact immutable chain anchor for a physical base snapshot."""
    with store._locked(exclusive=False):
        store._reload_locked()
        identity = _identity_locked(store)
        if identity is None:
            raise WalArchiveError("WAL archival is not enabled")
        root, header = identity
        start = header["payload"]["start_sequence"]
        if type(sequence) is not int or sequence < start or sequence > store._sequence:
            raise WalArchiveError("base sequence is outside this archive timeline")
        if sequence == start:
            return header["sha256"]
        record = _envelope(_safe(root, f"records/{sequence:020d}.json"))
        validate_archive_record(record, header)
        if record["payload"]["sequence"] != sequence:
            raise WalArchiveError("WAL base anchor sequence differs")
        return record["sha256"]


def read_verified_archive(path: str | Path) -> tuple[dict, list[dict]]:
    root = _root(path)
    header = _header(root)
    return header, read_archive_records(root)


def capture_archive(store: Store) -> dict:
    if store.read_only:
        raise WalArchiveError("cannot capture a read-only store")
    with store._locked(exclusive=True):
        store._assert_writable_locked()
        store._reload_locked(repair_tail=True)
        archive_pending_locked(store)
        return _status_locked(store, verify=True)


def hydrate_assets(path: str | Path, record: dict, destination_root: str | Path) -> None:
    """Copy verified immutable media/cold history objects into a staged copy."""
    from .backup import _copy_file, _digest_file
    root, target = _root(path), Path(destination_root)
    header = _header(root)
    row = validate_archive_record(record, header)
    for asset in row["assets"]:
        source = _safe(root, _asset_path(asset["sha256"]))
        destination = _safe(target, asset["path"])
        if destination.exists():
            digest, size = _digest_file(destination)
            if digest != asset["sha256"] or size != asset["size_bytes"]:
                raise WalArchiveError("recovery destination has conflicting object bytes")
        else:
            _copy_file(source, destination, expected_sha=asset["sha256"],
                       expected_size=asset["size_bytes"])


def apply_archive_record(store: Store, record: dict) -> None:
    """Apply one verified record to an isolated, caller-controlled staged store.

    The receiver must verify the chain, identity, and objects before calling.
    This deliberately bypasses workflow validators just as journal replay does;
    it is not a live client write API and does not acknowledge endpoint ownership.
    """
    row = validate_archive_record(record)
    if store._sequence + 1 != row["sequence"]:
        raise WalArchiveError("recovery application sequence gap")
    if row["kind"] == "transaction":
        value = row["transaction"]
        frame = _frame(value)
        with store.journal.open("ab") as stream:
            stream.write(frame)
            stream.flush()
            os.fsync(stream.fileno())
        store._apply(value)
        store._offset += len(frame)
    else:
        payload = row["checkpoint"]
        _atomic_json(store.checkpoint_file, {"payload": payload, "sha256": _digest(payload)})
        store._checkpoint_stamp = None
        store._reload_locked()
        with store.journal.open("r+b") as stream:
            stream.truncate(0)
            stream.flush()
            os.fsync(stream.fileno())
        store._offset = 0
