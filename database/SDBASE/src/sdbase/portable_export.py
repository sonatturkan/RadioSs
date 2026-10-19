"""Deterministic, credential-free JSONL/CSV export of a committed SDBASE view.

This is a portable *data* package, not a physical backup or an import protocol.
The source store is only read. Database credentials and the engine audit log
(which embeds old credential rows) are deliberately excluded. Every included
record keeps its ID, revision, complete JSON data and typed CSV cell values.
"""

from __future__ import annotations

from collections import defaultdict
from copy import deepcopy
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import tempfile
from threading import Lock
from typing import Any, Mapping

from .backup import BackupError, _destination, _publish_absent
from .identity import Forbidden, Principal
from .schema import REFERENCES, REQUIRED
from .storage import Store, StoreError


FORMAT = "sdbase-portable-export-v2"
MEDIA_FORMAT = "sdbase-media-references-v1"
TOMBSTONE_FORMAT = "sdbase-tombstones-v1"
EXCLUDED = frozenset({"tokens", "_audit"})
_NAME = re.compile(r"[A-Za-z][A-Za-z0-9_]*\Z")
_SHA = re.compile(r"[0-9a-f]{64}\Z")
_SENSITIVE = re.compile(
    r"token|password|secret|authorization|credential|api[_-]?key|private[_-]?key|bearer",
    re.IGNORECASE,
)
_BEARER_VALUE = re.compile(r"\bBearer\s+[A-Za-z0-9._~+/=-]{12,}", re.IGNORECASE)
_URL_USERINFO = re.compile(r"[A-Za-z][A-Za-z0-9+.-]*://[^\s/@:]+:[^\s/@]+@")
_CSV_LOCK = Lock()


class PortableExportError(StoreError):
    """A source, destination or package violates the portable export contract."""


def _canonical(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False).encode("utf-8")


def _fsync_dir(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _digest(path: Path) -> dict:
    result = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
            size += len(block)
    return {"sha256": result.hexdigest(), "size_bytes": size}


def _regular(path: Path) -> None:
    try:
        mode = path.lstat().st_mode
    except OSError as exc:
        raise PortableExportError(f"package component is absent: {path.name}") from exc
    if not stat.S_ISREG(mode):
        raise PortableExportError(f"package component is not a regular file: {path.name}")


def _sensitive_value(value: Any, location: str) -> None:
    """Fail closed on named secrets instead of silently dropping user data."""
    if isinstance(value, Mapping):
        for key, child in value.items():
            if not isinstance(key, str) or _SENSITIVE.search(key):
                raise PortableExportError(f"sensitive or invalid field at {location}")
            _sensitive_value(child, f"{location}.{key}")
    elif isinstance(value, (list, tuple)):
        for index, child in enumerate(value):
            _sensitive_value(child, f"{location}[{index}]")
    elif isinstance(value, str) and (_BEARER_VALUE.search(value) or _URL_USERINFO.search(value)):
        raise PortableExportError(f"credential-like text at {location}")


def _snapshot(store: Store, principal: Principal) -> tuple[dict, dict, dict]:
    if principal.scope != "user":
        raise Forbidden("portable export requires a user credential")
    # Take one short shared-lock snapshot. File generation never holds the DB
    # lock; later commits cannot alter these copied records.
    with store._locked(exclusive=False):
        store._reload_locked()
        user = store._state.get("users", {}).get(principal.user_id)
        if user is None or user["data"].get("status") != "active":
            raise Forbidden("portable export user is inactive")
        station_ids = set(store._state.get("stations", {}))
        if not station_ids:
            raise Forbidden("portable export requires a managed station")
        permissions = [row["data"] for row in store._state.get("permissions", {}).values()
                       if not row["data"].get("revoked_at") and row["data"].get("user_id") == principal.user_id and
                       row["data"].get("permission") == "manage"]
        if any(not any(permission.get("station_id") in (None, station_id)
                       for permission in permissions) for station_id in station_ids):
            raise Forbidden("manage permission is required for every station")
        meta = store._meta()
        if store._state.get("retention_segments"):
            raise PortableExportError("cold history requires a physical backup; portable data export cannot carry archived evidence")
        state = {name: deepcopy(records) for name, records in store._state.items()
                 if name not in EXCLUDED}
        tombstones = {name: set(ids) for name, ids in store._tombstones.items()
                      if name not in EXCLUDED}
        source = {"store_id": meta.get("store_id"),
                  "schema_version": meta.get("schema_version", 1),
                  "sequence": store._sequence}
    if not isinstance(source["store_id"], str):
        raise PortableExportError("source store identity is invalid")
    for name, records in state.items():
        if not _NAME.fullmatch(name) or _SENSITIVE.search(name):
            raise PortableExportError("source contains a sensitive or unsafe collection name")
        for identifier, envelope in records.items():
            if not isinstance(identifier, str) or not isinstance(envelope, dict) or \
                    type(envelope.get("revision")) is not int or envelope["revision"] < 1 or \
                    not isinstance(envelope.get("data"), dict):
                raise PortableExportError("source contains a malformed record")
            _sensitive_value(envelope["data"], f"{name}/{identifier}")
            if name == "field_definitions" and _SENSITIVE.search(
                    str(envelope["data"].get("key", ""))):
                raise PortableExportError("sensitive metadata field cannot be exported losslessly")
    for name, ids in tombstones.items():
        if not _NAME.fullmatch(name) or _SENSITIVE.search(name) or \
                any(not isinstance(identifier, str) or not identifier or
                    identifier in state.get(name, {}) for identifier in ids):
            raise PortableExportError("source contains an invalid tombstone")
    return source, state, tombstones


def _write_bytes(path: Path, value: bytes) -> dict:
    with path.open("xb") as stream:
        os.chmod(path, 0o600)
        stream.write(value)
        stream.flush()
        os.fsync(stream.fileno())
    return _digest(path)


def _write_collection(stage: Path, name: str, records: dict) -> dict:
    entries = [(identifier, records[identifier]) for identifier in sorted(records)]
    fields = sorted({field for _identifier, envelope in entries
                     for field in envelope["data"]})
    if any(not isinstance(field, str) for field in fields):
        raise PortableExportError("record field names must be text")
    json_path = stage / "collections" / f"{name}.jsonl"
    with json_path.open("xb") as stream:
        os.chmod(json_path, 0o600)
        for identifier, envelope in entries:
            line = _canonical({"_id": identifier, "_rev": envelope["revision"],
                               "data": envelope["data"]}) + b"\n"
            stream.write(line)
        stream.flush()
        os.fsync(stream.fileno())
    csv_path = stage / "collections" / f"{name}.csv"
    with csv_path.open("x", encoding="utf-8", newline="") as stream:
        os.chmod(csv_path, 0o600)
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(["_id", "_rev", *(f"field:{field}" for field in fields)])
        for identifier, envelope in entries:
            data = envelope["data"]
            writer.writerow([identifier, envelope["revision"], *(
                _canonical(data[field]).decode("utf-8") if field in data else ""
                for field in fields)])
        stream.flush()
        os.fsync(stream.fileno())
    return {"name": name, "rows": len(entries), "fields": fields,
            "jsonl": {"path": f"collections/{name}.jsonl", **_digest(json_path)},
            "csv": {"path": f"collections/{name}.csv", **_digest(csv_path)}}


def _media_references(state: Mapping[str, dict]) -> dict:
    media = state.get("media_files", {})
    locations: dict[str, list[dict]] = defaultdict(list)
    for identifier, envelope in state.get("media_locations", {}).items():
        row = envelope["data"]
        media_id = row.get("media_file_id")
        if not isinstance(media_id, str) or media_id not in media:
            raise PortableExportError("media location refers to absent media")
        file_row = media[media_id]["data"]
        digest, size = row.get("sha256"), row.get("size_bytes")
        if not isinstance(digest, str) or not _SHA.fullmatch(digest) or \
                type(size) is not int or size < 0 or \
                row.get("storage_uri") != "sdb-object:" + digest or \
                file_row.get("sha256") != digest or file_row.get("size_bytes") != size:
            raise PortableExportError("media location differs from its content address")
        locations[media_id].append({
            "location_id": identifier, "status": row.get("status"),
            "storage_uri": row.get("storage_uri"),
            "sha256": row.get("sha256"), "size_bytes": row.get("size_bytes"),
        })
    entries = []
    for identifier in sorted(media):
        row = media[identifier]["data"]
        entries.append({
            "media_file_id": identifier, "version_id": row.get("version_id"),
            "status": row.get("status"), "sha256": row.get("sha256"),
            "size_bytes": row.get("size_bytes"),
            "locations": sorted(locations.get(identifier, []),
                                key=lambda value: value["location_id"]),
        })
    return {"format": MEDIA_FORMAT, "entries": entries,
            "audio_bytes_included": False}


def _relations() -> list[dict]:
    return sorted(({"from": collection, "field": field, "to": target}
                   for collection, specs in REFERENCES.items() if collection not in EXCLUDED
                   for field, target in specs if target not in EXCLUDED),
                  key=lambda row: (row["from"], row["field"], row["to"]))


def create_portable_export(store: Store, principal: Principal,
                           destination: str | Path) -> dict:
    """Write a deterministic package and atomically publish a previously absent dir."""
    source, state, tombstones = _snapshot(store, principal)
    try:
        target = _destination(destination)
    except BackupError as exc:
        raise PortableExportError(str(exc)) from exc
    stage = Path(tempfile.mkdtemp(prefix=f".{target.name}.", suffix=".partial",
                                  dir=target.parent))
    os.chmod(stage, 0o700)
    try:
        collection_dir = stage / "collections"
        collection_dir.mkdir(mode=0o700)
        collections = []
        for name in sorted((set(REQUIRED) | set(state) | set(tombstones)) - EXCLUDED):
            if not _NAME.fullmatch(name):
                raise PortableExportError("collection name is unsafe for a portable filename")
            collections.append(_write_collection(stage, name, state.get(name, {})))
        media_info = _write_bytes(stage / "media_manifest.json",
                                  _canonical(_media_references(state)) + b"\n")
        tombstone_doc = {"format": TOMBSTONE_FORMAT,
                         "collections": {name: sorted(tombstones.get(name, set()))
                                         for name in sorted(tombstones)}}
        tombstone_info = _write_bytes(stage / "tombstones.json",
                                      _canonical(tombstone_doc) + b"\n")
        manifest = {
            "format": FORMAT, "source": source,
            "excluded_collections": sorted(EXCLUDED),
            "collections": collections,
            "record_count": sum(entry["rows"] for entry in collections),
            "relations": _relations(),
            "media_manifest": {"path": "media_manifest.json", **media_info},
            "tombstones": {"path": "tombstones.json", **tombstone_info},
            "tombstone_count": sum(len(ids) for ids in tombstones.values()),
            "csv_encoding": "UTF-8; field cells are canonical JSON; empty cell means absent",
            "audio_bytes_included": False,
        }
        content = _canonical(manifest) + b"\n"
        _write_bytes(stage / "manifest.json", content)
        _write_bytes(stage / "manifest.sha256",
                     (hashlib.sha256(content).hexdigest() + "\n").encode("ascii"))
        _fsync_dir(collection_dir)
        _fsync_dir(stage)
        try:
            _publish_absent(stage, target)
        except BackupError as exc:
            raise PortableExportError(str(exc)) from exc
        return {"destination": str(target), "source_sequence": source["sequence"],
                "collection_count": len(collections),
                "record_count": manifest["record_count"],
                "tombstone_count": manifest["tombstone_count"],
                "manifest_sha256": hashlib.sha256(content).hexdigest(),
                "excluded_collections": sorted(EXCLUDED),
                "audio_bytes_included": False}
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def _package_files(root: Path) -> set[str]:
    if root.is_symlink() or not root.is_dir():
        raise PortableExportError("portable export root is not a regular directory")
    files: set[str] = set()
    directories: set[str] = set()
    for current, subdirs, names in os.walk(root, followlinks=False):
        for name in subdirs:
            path = Path(current) / name
            if path.is_symlink():
                raise PortableExportError("package contains a symlink directory")
            directories.add(path.relative_to(root).as_posix())
        for name in names:
            path = Path(current) / name
            _regular(path)
            files.add(path.relative_to(root).as_posix())
    if directories != {"collections"}:
        raise PortableExportError("package contains unexpected directories")
    return files


def _checked_file(root: Path, descriptor: Mapping[str, Any], expected: str) -> Path:
    if not isinstance(descriptor, Mapping) or descriptor.get("path") != expected or \
            not isinstance(descriptor.get("sha256"), str) or not \
            _SHA.fullmatch(descriptor["sha256"]) or \
            type(descriptor.get("size_bytes")) is not int or descriptor["size_bytes"] < 0:
        raise PortableExportError("manifest file descriptor is invalid")
    path = root / expected
    _regular(path)
    if _digest(path) != {"sha256": descriptor["sha256"],
                         "size_bytes": descriptor["size_bytes"]}:
        raise PortableExportError(f"package file digest differs: {expected}")
    return path


def _read_collection(root: Path, entry: Mapping[str, Any]) -> tuple[set[str], list[tuple[str, dict]]]:
    name = entry.get("name")
    fields = entry.get("fields")
    count = entry.get("rows")
    if not isinstance(name, str) or not _NAME.fullmatch(name) or \
            not isinstance(fields, list) or \
            any(not isinstance(field, str) for field in fields) or \
            fields != sorted(set(fields)) or \
            type(count) is not int or count < 0:
        raise PortableExportError("collection descriptor is invalid")
    json_path = _checked_file(root, entry.get("jsonl"), f"collections/{name}.jsonl")
    csv_path = _checked_file(root, entry.get("csv"), f"collections/{name}.csv")
    identifiers: set[str] = set()
    references: list[tuple[str, dict]] = []
    previous = None
    with _CSV_LOCK:
        prior_limit = csv.field_size_limit()
        csv.field_size_limit(64 * 1024 * 1024)
        try:
            with json_path.open("r", encoding="utf-8", newline="") as json_stream, \
                    csv_path.open("r", encoding="utf-8", newline="") as csv_stream:
                reader = csv.reader(csv_stream, strict=True)
                if next(reader, None) != ["_id", "_rev", *(f"field:{field}" for field in fields)]:
                    raise PortableExportError(f"CSV header differs: {name}")
                for line in json_stream:
                    if not line.endswith("\n"):
                        raise PortableExportError(f"JSONL line has no terminator: {name}")
                    try:
                        row = json.loads(line)
                    except (ValueError, TypeError) as exc:
                        raise PortableExportError(f"JSONL row is malformed: {name}") from exc
                    if not isinstance(row, dict) or set(row) != {"_id", "_rev", "data"} or \
                            not isinstance(row["_id"], str) or not row["_id"] or \
                            type(row["_rev"]) is not int or row["_rev"] < 1 or \
                            not isinstance(row["data"], dict) or \
                            line.encode("utf-8") != _canonical(row) + b"\n":
                        raise PortableExportError(f"JSONL row is not canonical: {name}")
                    if previous is not None and row["_id"] <= previous:
                        raise PortableExportError(f"collection IDs are not strictly sorted: {name}")
                    previous = row["_id"]
                    cells = next(reader, None)
                    if cells is None or len(cells) != len(fields) + 2 or \
                            cells[0] != row["_id"] or cells[1] != str(row["_rev"]):
                        raise PortableExportError(f"CSV row differs from JSONL: {name}")
                    reconstructed = {}
                    for field, cell in zip(fields, cells[2:]):
                        if cell:
                            try:
                                value = json.loads(cell)
                            except ValueError as exc:
                                raise PortableExportError(f"CSV field is invalid JSON: {name}") from exc
                            if cell.encode("utf-8") != _canonical(value):
                                raise PortableExportError(f"CSV field is not canonical: {name}")
                            reconstructed[field] = value
                    if reconstructed != row["data"]:
                        raise PortableExportError(f"CSV values differ from JSONL: {name}")
                    _sensitive_value(row["data"], f"{name}/{row['_id']}")
                    if name == "field_definitions" and _SENSITIVE.search(
                            str(row["data"].get("key", ""))):
                        raise PortableExportError("package contains a sensitive metadata field")
                    identifiers.add(row["_id"])
                    references.append((row["_id"], row["data"]))
                if next(reader, None) is not None:
                    raise PortableExportError(f"CSV has more rows than JSONL: {name}")
        except (UnicodeError, OSError, csv.Error) as exc:
            raise PortableExportError(f"collection cannot be parsed: {name}") from exc
        finally:
            csv.field_size_limit(prior_limit)
    if len(identifiers) != count:
        raise PortableExportError(f"collection row count differs: {name}")
    if fields != sorted({field for _identifier, data in references for field in data}):
        raise PortableExportError(f"CSV field inventory differs from JSONL: {name}")
    return identifiers, references


def verify_portable_export(directory: str | Path) -> dict:
    """Read back every file, JSON/CSV row and declared relationship, without writes."""
    root = Path(directory).expanduser().absolute()
    actual_files = _package_files(root)
    for fixed in ("manifest.json", "manifest.sha256"):
        _regular(root / fixed)
    manifest_bytes = (root / "manifest.json").read_bytes()
    digest_text = (root / "manifest.sha256").read_bytes()
    if digest_text != (hashlib.sha256(manifest_bytes).hexdigest() + "\n").encode("ascii"):
        raise PortableExportError("manifest SHA-256 differs")
    try:
        manifest = json.loads(manifest_bytes)
    except (UnicodeError, ValueError) as exc:
        raise PortableExportError("manifest JSON is invalid") from exc
    if not isinstance(manifest, dict) or manifest.get("format") != FORMAT or \
            manifest_bytes != _canonical(manifest) + b"\n" or \
            manifest.get("excluded_collections") != sorted(EXCLUDED) or \
            manifest.get("audio_bytes_included") is not False or \
            not isinstance(manifest.get("collections"), list) or \
            not isinstance(manifest.get("relations"), list) or \
            type(manifest.get("record_count")) is not int or \
            manifest["record_count"] < 0 or \
            type(manifest.get("tombstone_count")) is not int or \
            manifest["tombstone_count"] < 0 or \
            manifest.get("csv_encoding") != \
            "UTF-8; field cells are canonical JSON; empty cell means absent":
        raise PortableExportError("manifest contract is invalid")
    source = manifest.get("source")
    if not isinstance(source, dict) or not isinstance(source.get("store_id"), str) or \
            not source["store_id"] or \
            type(source.get("schema_version")) is not int or source["schema_version"] < 1 or \
            type(source.get("sequence")) is not int or source["sequence"] < 0:
        raise PortableExportError("manifest source identity is invalid")
    names = [entry.get("name") for entry in manifest["collections"] if isinstance(entry, dict)]
    if len(names) != len(manifest["collections"]) or \
            any(not isinstance(name, str) for name in names) or \
            names != sorted(set(names)) or \
            not (set(REQUIRED) - EXCLUDED).issubset(names) or \
            any(name in EXCLUDED or _SENSITIVE.search(name) for name in names):
        raise PortableExportError("manifest collection list is invalid")
    expected_files = {"manifest.json", "manifest.sha256", "media_manifest.json",
                      "tombstones.json"}
    for name in names:
        if not isinstance(name, str) or not _NAME.fullmatch(name):
            raise PortableExportError("manifest collection name is unsafe")
        expected_files.update((f"collections/{name}.jsonl", f"collections/{name}.csv"))
    if actual_files != expected_files:
        raise PortableExportError("package has missing or unexpected files")
    media_path = _checked_file(root, manifest.get("media_manifest"), "media_manifest.json")
    try:
        media_bytes = media_path.read_bytes()
        media_doc = json.loads(media_bytes)
    except (UnicodeError, ValueError, OSError) as exc:
        raise PortableExportError("media manifest cannot be read") from exc
    if media_bytes != _canonical(media_doc) + b"\n":
        raise PortableExportError("media manifest is not canonical")
    identities = {}
    rows_by_collection = {}
    for entry in manifest["collections"]:
        ids, rows = _read_collection(root, entry)
        identities[entry["name"]] = ids
        rows_by_collection[entry["name"]] = rows
    if sum(len(ids) for ids in identities.values()) != manifest.get("record_count"):
        raise PortableExportError("manifest record count differs")
    tombstone_path = _checked_file(root, manifest.get("tombstones"), "tombstones.json")
    try:
        tombstone_bytes = tombstone_path.read_bytes()
        tombstone_doc = json.loads(tombstone_bytes)
    except (UnicodeError, ValueError, OSError) as exc:
        raise PortableExportError("tombstone inventory cannot be read") from exc
    if tombstone_bytes != _canonical(tombstone_doc) + b"\n" or \
            not isinstance(tombstone_doc, dict) or \
            tombstone_doc.get("format") != TOMBSTONE_FORMAT or \
            not isinstance(tombstone_doc.get("collections"), dict) or \
            any(name not in identities or not isinstance(ids, list) or \
                any(not isinstance(identifier, str) for identifier in ids) or \
                ids != sorted(set(ids)) or \
                any(not isinstance(identifier, str) or not identifier or \
                    identifier in identities[name] for identifier in ids)
                for name, ids in tombstone_doc["collections"].items()) or \
            sum(len(ids) for ids in tombstone_doc["collections"].values()) != \
            manifest["tombstone_count"]:
        raise PortableExportError("tombstone inventory is invalid")
    relations = manifest["relations"]
    if relations != _relations():
        raise PortableExportError("relationship dictionary differs from this schema")
    if any(not isinstance(relation, dict) or
           set(relation) != {"from", "field", "to"} or
           any(not isinstance(relation[key], str) or not relation[key]
               for key in ("from", "field", "to")) or
           relation["from"] not in identities or relation["to"] not in identities
           for relation in relations) or \
            relations != sorted(relations,
                                key=lambda row: (row["from"], row["field"], row["to"])) or \
            len({(row["from"], row["field"], row["to"]) for row in relations}) != len(relations):
        raise PortableExportError("relationship dictionary is invalid")
    for relation in relations:
        source_collection, field, target = relation["from"], relation["field"], relation["to"]
        target_ids = identities.get(target, set())
        for identifier, data in rows_by_collection.get(source_collection, []):
            value = data.get(field)
            if value is not None and value not in target_ids:
                raise PortableExportError(
                    f"broken relationship {source_collection}/{identifier}.{field}")
    for collection in ("external_ids", "metadata_values", "metadata_suggestions"):
        for identifier, data in rows_by_collection.get(collection, []):
            target = data.get("target_collection")
            value = data.get("target_id")
            if target is not None and value is not None and value not in identities.get(target, set()):
                raise PortableExportError(f"broken polymorphic relationship {collection}/{identifier}")
    expected_media = _media_references({
        name: {identifier: {"data": data} for identifier, data in rows}
        for name, rows in rows_by_collection.items() if name in {"media_files", "media_locations"}
    })
    if media_doc != expected_media:
        raise PortableExportError("media manifest differs from exported media records")
    return {"valid": True, "format": FORMAT,
            "source_sequence": source["sequence"],
            "collection_count": len(names),
            "record_count": manifest["record_count"],
            "tombstone_count": manifest["tombstone_count"],
            "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
            "audio_bytes_included": False,
            "excluded_collections": sorted(EXCLUDED)}
