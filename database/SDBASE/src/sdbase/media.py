"""Managed RadioS media for the independent `.sdb` document store.

Bytes live in `{database filename}.objects/<sha-prefix>/<sha256>`. The `.sdb`
store records versions, media files, central locations, and endpoint reports.
Copying bytes and committing records are separate crash-safe steps: an object
left by a failed commit can be reused, and reconciliation reports it as orphaned.
No SQL engine, audio decoder, or remote filesystem probe is used here.
"""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import math
import os
from pathlib import Path
import re
import tempfile
from typing import Any, Mapping
from uuid import NAMESPACE_URL, UUID, uuid5

from .catalog import require_shared_item_permission
from .identity import Forbidden, Principal, require
from .storage import Conflict, Store, StoreError


_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_CHUNK = 1024 * 1024


class MediaError(StoreError):
    """Media input, object, or publication is invalid."""


class MediaPublicationError(MediaError):
    """Verified bytes were staged but the `.sdb` transaction did not commit."""

    def __init__(self, digest: str, path: Path, cause: Exception):
        super().__init__("verified object was staged but catalog publication failed")
        self.sha256 = digest
        self.path = path
        self.__cause__ = cause


def _id(value: str | UUID, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise MediaError(f"{field} must be a UUID") from exc


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _digest(path: Path) -> tuple[str, int]:
    sha = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(_CHUNK), b""):
            sha.update(chunk)
            size += len(chunk)
    return sha.hexdigest(), size


def _fsync_dir(path: Path) -> None:
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def object_root(store: Store) -> Path:
    """Sibling directory of the primary database file, e.g. RadioS.sdb.objects."""
    return store.journal.with_name(store.journal.name + ".objects")


def object_path(store: Store, sha256: str) -> Path:
    digest = str(sha256).lower()
    if not _SHA256.fullmatch(digest):
        raise MediaError("SHA-256 must be 64 hexadecimal digits")
    return object_root(store) / digest[:2] / digest


def _managed_uri(sha256: str) -> str:
    return "sdb-object:" + sha256


def _require_version_in_station(store: Store, station_id: str,
                                version_id: str, *, writable: bool) -> dict:
    version = store.get("versions", version_id)
    if version is None:
        raise MediaError("content version not found")
    item_id = _id(version.get("item_id"), "version.item_id")
    if not store.lookup("station_items", "station_item", station_id, item_id):
        raise Forbidden("content version is not attached to this station")
    if writable and version.get("status") in {"approved", "withdrawn"}:
        raise Conflict("approved or withdrawn version cannot gain a new media file")
    return version


def _media_in_station(store: Store, station_id: str, media_file_id: str) -> tuple[dict, dict]:
    media = store.get("media_files", media_file_id)
    if media is None:
        raise MediaError("media file not found")
    version = _require_version_in_station(
        store, station_id, _id(media.get("version_id"), "media.version_id"), writable=False)
    return media, version


def _managed_location(store: Store, media_file_id: str, sha256: str) -> dict:
    uri = _managed_uri(sha256)
    locations = store.lookup("media_locations", "file_uri", media_file_id, uri)
    if len(locations) != 1:
        raise MediaError("managed media location is absent or ambiguous")
    return locations[0]


def _validate_source(source: Path, allowed_roots: list[Path] | tuple[Path, ...]) -> Path:
    if not allowed_roots:
        raise MediaError("at least one allowed source root is required")
    try:
        resolved = Path(source).expanduser().resolve(strict=True)
        roots = tuple(Path(root).expanduser().resolve(strict=True) for root in allowed_roots)
    except OSError as exc:
        raise MediaError("source or allowed root does not exist") from exc
    if not resolved.is_file() or not any(resolved.is_relative_to(root) for root in roots):
        raise MediaError("source is outside configured roots or is not a file")
    return resolved


def _stage_object(store: Store, source: Path) -> tuple[str, int, Path, bool]:
    # Physical object publication must respect the same role/epoch fence as
    # transactions. Hold a shared lock during copy so handover cannot turn
    # this node into a standby midway through a managed-object replacement.
    with store._locked(exclusive=False):
        store._assert_writable_locked()
        return _stage_object_locked(store, source)


def _stage_object_locked(store: Store, source: Path) -> tuple[str, int, Path, bool]:
    root = object_root(store)
    staging = root / ".staging"
    new_root = not root.exists()
    staging.mkdir(parents=True, exist_ok=True)
    if new_root:
        _fsync_dir(root.parent)
    fd, name = tempfile.mkstemp(prefix="media-", suffix=".partial", dir=staging)
    temp = Path(name)
    sha = hashlib.sha256()
    size = 0
    try:
        try:
            input_stream = source.open("rb")
        except BaseException:
            os.close(fd)
            raise
        with input_stream, os.fdopen(fd, "wb") as output_stream:
            before = os.fstat(input_stream.fileno())
            for chunk in iter(lambda: input_stream.read(_CHUNK), b""):
                sha.update(chunk)
                output_stream.write(chunk)
                size += len(chunk)
            output_stream.flush()
            os.fsync(output_stream.fileno())
            after = os.fstat(input_stream.fileno())
        if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != \
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
            raise MediaError("source changed while it was copied")
        digest = sha.hexdigest()
        target = object_path(store, digest)
        new_prefix = not target.parent.exists()
        target.parent.mkdir(exist_ok=True)
        if new_prefix:
            _fsync_dir(target.parent.parent)
        if target.parent.is_symlink() or target.is_symlink():
            raise MediaError("managed object path must not be a symlink")
        if target.is_file():
            old_digest, old_size = _digest(target)
            if old_digest == digest and old_size == size:
                return digest, size, target, True
            # A corrupt object is replaced only with newly validated source
            # bytes for the same content address.
        os.replace(temp, target)
        _fsync_dir(target.parent)
        return digest, size, target, False
    finally:
        temp.unlink(missing_ok=True)


def stage_media(store: Store, principal: Principal, station_id: str | UUID,
                version_id: str | UUID, source: Path,
                allowed_source_roots: list[Path] | tuple[Path, ...]) -> dict:
    """Copy an allowed file, then atomically publish media and location records.

    Repeating the same version and content returns the existing media file.
    A failed `.sdb` commit leaves only a reusable, content-addressed object.
    """
    station = _id(station_id, "station_id")
    version = _id(version_id, "version_id")
    sequence = store.sequence
    require(store, principal, "edit", station)
    version_row = _require_version_in_station(store, station, version, writable=True)
    require_shared_item_permission(store, principal, version_row["item_id"], "edit")
    resolved = _validate_source(source, allowed_source_roots)
    digest, size, target, reused_object = _stage_object(store, resolved)
    existing = [row for row in store.lookup("media_files", "version", version)
                if row.get("sha256") == digest and row.get("size_bytes") == size and
                row.get("status") != "deleted"]
    if existing:
        if len(existing) != 1:
            raise Conflict("multiple media records have the same version and digest")
        location = _managed_location(store, existing[0]["_id"], digest)
        if location.get("status") in {"missing", "corrupt"}:
            # Physical repair does not silently approve technical metadata.
            tx = store.begin(actor=principal.user_id)
            tx.expect_sequence(sequence)
            tx.put("media_locations", location["_id"],
                   {**_data(location), "status": "present", "last_seen_at": _utc_now()},
                   expected_revision=location["_rev"])
            tx.commit()
        current_version = _require_version_in_station(store, station, version, writable=False)
        require_shared_item_permission(store, principal, current_version["item_id"], "edit")
        return {"media_file_id": existing[0]["_id"], "sha256": digest,
                "size_bytes": size, "storage_uri": _managed_uri(digest),
                "path": str(target), "replayed": True}
    media_id = str(uuid5(NAMESPACE_URL, f"radios-media:{version}:{digest}"))
    location_id = str(uuid5(NAMESPACE_URL, f"radios-location:{media_id}:{digest}"))
    now = _utc_now()
    media = {"version_id": version, "status": "received", "sha256": digest,
             "size_bytes": size, "received_at": now, "source_name": resolved.name}
    location = {"media_file_id": media_id, "storage_uri": _managed_uri(digest),
                "status": "present", "sha256": digest, "size_bytes": size,
                "last_seen_at": now, "root_kind": "managed"}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("media_files", media_id, media)
    tx.put("media_locations", location_id, location)
    try:
        tx.commit()
    except Conflict:
        # A concurrent identical publication may have won. Keep the durable
        # object and return the canonical record if it now exists.
        winner = store.get("media_files", media_id)
        if winner and winner.get("version_id") == version and \
                winner.get("sha256") == digest and winner.get("size_bytes") == size:
            current_version = _require_version_in_station(
                store, station, version, writable=False)
            require_shared_item_permission(store, principal, current_version["item_id"], "edit")
            return {"media_file_id": media_id, "sha256": digest,
                    "size_bytes": size, "storage_uri": _managed_uri(digest),
                    "path": str(target), "replayed": True}
        raise
    except Exception as exc:
        raise MediaPublicationError(digest, target, exc) from exc
    return {"media_file_id": media_id, "sha256": digest,
            "size_bytes": size, "storage_uri": _managed_uri(digest),
            "path": str(target), "replayed": False,
            "reused_object": reused_object}


def _data(row: Mapping[str, Any]) -> dict:
    return {key: value for key, value in row.items() if key not in ("_id", "_rev")}


def _technical_metadata(values: Mapping[str, Any]) -> dict:
    if not isinstance(values, Mapping):
        raise MediaError("technical metadata must be an object")
    allowed = {"codec", "container", "duration_ms", "sample_rate_hz", "channels",
               "bit_rate_bps", "bit_depth", "loudness_lufs", "true_peak_dbfs"}
    if any(key not in allowed for key in values):
        raise MediaError("unsupported technical metadata key")
    codec = values.get("codec")
    duration = values.get("duration_ms")
    rate = values.get("sample_rate_hz")
    channels = values.get("channels")
    if not isinstance(codec, str) or not codec.strip():
        raise MediaError("codec is required")
    for key, value, upper in (("duration_ms", duration, None),
                              ("sample_rate_hz", rate, 768000),
                              ("channels", channels, 64)):
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0 or \
                (upper is not None and value > upper):
            raise MediaError(f"{key} must be a positive plausible integer")
    clean: dict[str, Any] = {"codec": codec.strip(), "duration_ms": duration,
                             "sample_rate_hz": rate, "channels": channels}
    if "container" in values:
        if not isinstance(values["container"], str) or not values["container"].strip():
            raise MediaError("container must be nonempty")
        clean["container"] = values["container"].strip()
    for key in ("bit_rate_bps", "bit_depth"):
        if key in values:
            value = values[key]
            if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
                raise MediaError(f"{key} must be positive")
            clean[key] = value
    for key in ("loudness_lufs", "true_peak_dbfs"):
        if key in values:
            value = values[key]
            if isinstance(value, bool) or not isinstance(value, (int, float)) or \
                    not math.isfinite(value):
                raise MediaError(f"{key} must be finite")
            clean[key] = float(value)
    return clean


def verify_and_promote_media(store: Store, principal: Principal,
                             station_id: str | UUID, media_file_id: str | UUID,
                             technical_metadata: Mapping[str, Any],
                             *, expected_revision: int | None = None) -> dict:
    """Rehash stored bytes and record supplied technical review before approval."""
    station = _id(station_id, "station_id")
    media_id = _id(media_file_id, "media_file_id")
    sequence = store.sequence
    require(store, principal, "approve", station)
    media, version = _media_in_station(store, station, media_id)
    require_shared_item_permission(store, principal, version["item_id"], "approve")
    if media.get("status") in {"deleted", "rejected"}:
        raise Conflict("rejected or deleted media cannot be promoted")
    if expected_revision is not None and media["_rev"] != expected_revision:
        raise Conflict("media revision changed")
    technical = _technical_metadata(technical_metadata)
    digest = media.get("sha256")
    if not isinstance(digest, str) or not _SHA256.fullmatch(digest):
        raise MediaError("media lacks a valid SHA-256")
    location = _managed_location(store, media_id, digest)
    target = object_path(store, digest)
    if target.is_symlink() or not target.is_file():
        raise MediaError("managed media object is missing")
    actual_sha, actual_size = _digest(target)
    if actual_sha != digest or actual_size != media.get("size_bytes"):
        raise MediaError("managed object failed SHA-256 or size verification")
    if media.get("status") == "verified":
        if media.get("technical") != technical:
            raise Conflict("verified media technical metadata is immutable")
        current_version = _require_version_in_station(
            store, station, version["_id"], writable=False)
        require_shared_item_permission(store, principal, current_version["item_id"], "approve")
        return {"media_file_id": media_id, "sha256": digest, "verified": True,
                "replayed": True}
    now = _utc_now()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("media_files", media_id,
           {**_data(media), "status": "verified", "technical": technical,
            "technical_reviewed_at": now, "technical_reviewed_by": principal.user_id,
            "verified_at": now}, expected_revision=media["_rev"])
    tx.put("media_locations", location["_id"],
           {**_data(location), "status": "verified", "verified_at": now,
            "last_seen_at": now}, expected_revision=location["_rev"])
    tx.commit()
    return {"media_file_id": media_id, "sha256": digest,
            "verified": True, "replayed": False}


def verified_media_path(store: Store, principal: Principal,
                        station_id: str | UUID, endpoint_id: str | UUID,
                        media_file_id: str | UUID) -> Path:
    """Resolve bytes for an authenticated endpoint; caller provides transport."""
    station = _id(station_id, "station_id")
    endpoint = _id(endpoint_id, "endpoint_id")
    media_id = _id(media_file_id, "media_file_id")
    if principal.scope != "endpoint":
        raise Forbidden("endpoint credential required for media delivery")
    require(store, principal, "operate", station, endpoint_id=endpoint)
    media, _version = _media_in_station(store, station, media_id)
    if media.get("status") != "verified":
        raise Conflict("media has not passed central verification")
    digest = media.get("sha256")
    if not isinstance(digest, str) or not _SHA256.fullmatch(digest):
        raise MediaError("verified media digest is invalid")
    location = _managed_location(store, media_id, digest)
    if location.get("status") != "verified":
        raise Conflict("managed media location is not verified")
    target = object_path(store, digest)
    if target.is_symlink() or not target.is_file():
        raise MediaError("managed media object is missing")
    actual_sha, actual_size = _digest(target)
    if actual_sha != digest or actual_size != media.get("size_bytes"):
        raise MediaError("managed object failed SHA-256 or size verification")
    return target


def report_verified_endpoint_copy(store: Store, principal: Principal,
                                  station_id: str | UUID,
                                  endpoint_id: str | UUID,
                                  media_file_id: str | UUID,
                                  sha256: str, storage_uri: str) -> dict:
    """Record a hash report from that endpoint's credential, idempotently.

    The central process cannot independently inspect remote disk bytes. The
    endpoint agent verifies them before sending this attestation.
    """
    station = _id(station_id, "station_id")
    endpoint = _id(endpoint_id, "endpoint_id")
    media_id = _id(media_file_id, "media_file_id")
    if principal.scope != "endpoint":
        raise Forbidden("endpoint credential required for copy verification")
    require(store, principal, "operate", station, endpoint_id=endpoint)
    media, _version = _media_in_station(store, station, media_id)
    digest = str(sha256).lower()
    if media.get("status") != "verified" or digest != media.get("sha256"):
        raise Conflict("endpoint copy does not match verified central media")
    expected_uri = f"endpoint://{endpoint}/objects/{digest}"
    if storage_uri != expected_uri:
        raise MediaError("endpoint object URI does not match its identity and digest")
    matches = store.lookup("endpoint_copies", "endpoint_file", endpoint, media_id)
    if len(matches) > 1:
        raise Conflict("duplicate endpoint copy records")
    if matches:
        current = matches[0]
        if current.get("status") == "verified" and current.get("sha256") == digest and \
                current.get("storage_uri") == expected_uri:
            return {"endpoint_copy_id": current["_id"], "replayed": True,
                    "verified": True}
        copy_id, expected = current["_id"], current["_rev"]
        data = _data(current)
    else:
        copy_id = str(uuid5(NAMESPACE_URL, f"radios-copy:{endpoint}:{media_id}"))
        expected = None
        data = {"endpoint_id": endpoint, "media_file_id": media_id}
    now = _utc_now()
    data.update({"status": "verified", "sha256": digest,
                 "storage_uri": expected_uri, "verified_at": now,
                 "last_seen_at": now, "attested_by_endpoint": endpoint})
    store.put("endpoint_copies", copy_id, data,
              expected_revision=expected, actor=principal.user_id)
    return {"endpoint_copy_id": copy_id, "replayed": False,
            "verified": True}


def reconcile_media(store: Store, principal: Principal,
                    station_id: str | UUID) -> dict:
    """Mark missing/corrupt managed objects and list unreferenced objects.

    Healthy restored objects are marked present; technical approval is never
    silently restored. Orphans are reported and left untouched for recovery.
    """
    station = _id(station_id, "station_id")
    require(store, principal, "repair", station)
    scoped_items = {row["item_id"] for row in store.lookup("station_items", "station", station)}
    scoped_versions = {row["_id"] for row in store.scan("versions")
                       if row.get("item_id") in scoped_items}
    files = [row for row in store.scan("media_files")
             if row.get("version_id") in scoped_versions and row.get("sha256")]
    outcomes = {"verified": [], "missing": [], "corrupt": [], "orphans": []}
    referenced: set[str] = {
        row["sha256"] for row in store.scan("media_files")
        if isinstance(row.get("sha256"), str) and _SHA256.fullmatch(row["sha256"])
    }
    for media in files:
        media_id = media["_id"]
        digest = media["sha256"]
        if not isinstance(digest, str) or not _SHA256.fullmatch(digest):
            outcomes["corrupt"].append(media_id)
            continue
        target = object_path(store, digest)
        if target.is_symlink() or not target.is_file():
            status = "missing"
        else:
            actual_sha, actual_size = _digest(target)
            status = "verified" if (actual_sha == digest and
                                    actual_size == media.get("size_bytes")) else "corrupt"
        outcomes[status].append(media_id)
        try:
            location = _managed_location(store, media_id, digest)
        except MediaError:
            # A broken catalog relationship is reported without fabricating
            # an object status or deleting any bytes.
            outcomes[status].remove(media_id)
            outcomes["corrupt"].append(media_id)
            continue
        desired_location = ("missing" if status == "missing" else
                            "corrupt" if status == "corrupt" else
                            "verified" if media.get("status") == "verified" else "present")
        desired_media = ("error" if status != "verified" else
                         "received" if media.get("status") == "error" else media["status"])
        if location.get("status") != desired_location or media["status"] != desired_media:
            sequence = store.sequence
            version = _require_version_in_station(
                store, station, _id(media["version_id"], "media.version_id"),
                writable=False)
            require_shared_item_permission(store, principal, version["item_id"], "repair")
            tx = store.begin(actor=principal.user_id)
            tx.expect_sequence(sequence)
            if location.get("status") != desired_location:
                tx.put("media_locations", location["_id"],
                       {**_data(location), "status": desired_location,
                        "last_seen_at": _utc_now()}, expected_revision=location["_rev"])
            if media["status"] != desired_media:
                tx.put("media_files", media_id,
                       {**_data(media), "status": desired_media},
                       expected_revision=media["_rev"])
            tx.commit()
    root = object_root(store)
    if root.exists():
        for child in root.iterdir():
            if child.name == ".staging" or not child.is_dir() or child.is_symlink():
                continue
            for path in child.iterdir():
                if path.is_file() and not path.is_symlink() and \
                        _SHA256.fullmatch(path.name) and path.name not in referenced:
                    outcomes["orphans"].append(str(path))
    return {key: sorted(set(value)) for key, value in outcomes.items()}
