"""Atomic content-addressed endpoint media staging and re-verification."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import tempfile
from typing import Any, BinaryIO
from uuid import UUID

from . import _platform
from .endpoint_journal import EndpointError, EndpointJournal


class MediaIntegrityError(EndpointError):
    """Source bytes do not match the snapshot's size and SHA-256."""


def _media_uuid(value: str | UUID) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise EndpointError("media_file_id must be a UUID") from exc


def _digest(path: Path) -> str:
    value = hashlib.sha256()
    # Share-delete reader: hashing never blocks a concurrent replace of the object.
    with _platform.open_read(path) as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


class EndpointMediaStore:
    def __init__(self, root: Path, allowed_source_roots: tuple[Path, ...],
                 journal: EndpointJournal):
        self.root = root
        self.objects = root / "objects"
        self.staging = root / "staging"
        self.objects.mkdir(parents=True, exist_ok=True)
        self.staging.mkdir(parents=True, exist_ok=True)
        self.allowed_source_roots = allowed_source_roots
        self.journal = journal

    @property
    def _state(self) -> dict[str, Any]:
        return self.journal.state

    def _object_path(self, sha256: str) -> Path:
        return self.objects / sha256[:2] / sha256

    def _publish_stream(self, media_id: str, stream: BinaryIO) -> dict[str, Any]:
        spec = self._state["media"].get(media_id)
        if spec is None:
            raise EndpointError("media is not required by an installed snapshot")
        target = self._object_path(spec["sha256"])
        target.parent.mkdir(exist_ok=True)
        if _platform.is_link(target) or _platform.is_link(target.parent):
            raise EndpointError("local media path must not be a symlink or junction")
        if (target.is_file() and target.stat().st_size == spec["size_bytes"] and
                _digest(target) == spec["sha256"]):
            if spec["status"] != "verified":
                self.journal.append("media_status", {"media_file_id": media_id,
                                                     "status": "verified"})
            return {"media_file_id": media_id, "path": str(target),
                    "sha256": spec["sha256"], "replayed": True}
        fd, name = tempfile.mkstemp(prefix="media-", suffix=".partial", dir=self.staging)
        temp = Path(name)
        digest = hashlib.sha256()
        size = 0
        try:
            with os.fdopen(fd, "wb") as output:
                while True:
                    chunk = stream.read(1024 * 1024)
                    if not chunk:
                        break
                    size += len(chunk)
                    if size > spec["size_bytes"]:
                        raise MediaIntegrityError("stream exceeds expected media size")
                    digest.update(chunk)
                    output.write(chunk)
                output.flush()
                _platform.fsync(output)
            if size != spec["size_bytes"] or digest.hexdigest() != spec["sha256"]:
                raise MediaIntegrityError("media size or SHA-256 differs from snapshot")
            # The external playout process may still be reading a corrupt object
            # being re-staged: POSIX-semantics replace with the sharing retry budget.
            # NTFS journals the rename itself; there is no directory flush on Windows.
            _platform.replace(temp, target)
            self.journal.append("media_status", {"media_file_id": media_id,
                                                 "status": "verified"})
            return {"media_file_id": media_id, "path": str(target),
                    "sha256": spec["sha256"], "replayed": False}
        finally:
            _platform.unlink_quietly(temp)

    def stage_media_stream(self, media_file_id: str | UUID,
                           stream: BinaryIO) -> dict[str, Any]:
        """Publish already authenticated source bytes after manifest checking."""
        return self._publish_stream(_media_uuid(media_file_id), stream)

    def stage_media(self, media_file_id: str | UUID, source: Path) -> dict[str, Any]:
        """Copy only from caller-configured local source roots."""
        if not self.allowed_source_roots:
            raise EndpointError("no allowed source roots configured")
        resolved = Path(source).expanduser().resolve(strict=True)
        if not resolved.is_file() or not any(resolved.is_relative_to(root)
                                              for root in self.allowed_source_roots):
            raise EndpointError("source is outside configured roots")
        with _platform.open_read(resolved) as stream:
            before = os.fstat(stream.fileno())
            result = self._publish_stream(_media_uuid(media_file_id), stream)
            after = os.fstat(stream.fileno())
        if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != \
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
            raise MediaIntegrityError("source changed during copy")
        return result

    def verify_media(self) -> dict[str, int]:
        """Re-hash local objects; report missing or altered bytes in the journal."""
        counts = {"verified": 0, "missing": 0, "corrupt": 0}
        for media_id in list(self._state["media"]):
            counts[self.verify_media_file(media_id)] += 1
        return counts

    def verify_media_file(self, media_file_id: str | UUID) -> str:
        """Check one selected object before a start command is handed off."""
        media_id = _media_uuid(media_file_id)
        spec = self._state["media"].get(media_id)
        if spec is None:
            raise EndpointError("media is absent from installed snapshots")
        path = self._object_path(spec["sha256"])
        if _platform.is_link(path) or _platform.is_link(path.parent):
            status = "corrupt"
        elif not path.is_file():
            status = "missing"
        elif path.stat().st_size != spec["size_bytes"] or _digest(path) != spec["sha256"]:
            status = "corrupt"
        else:
            status = "verified"
        if spec["status"] != status:
            self.journal.append("media_status", {"media_file_id": media_id,
                                                 "status": status})
        return status
