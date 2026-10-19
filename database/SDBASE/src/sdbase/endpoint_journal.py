"""Fsynced, hash-chained, single-writer endpoint journal (.sdb file)."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
from typing import Any

from . import _platform

_ZERO_HASH = "0" * 64


class EndpointError(RuntimeError):
    """Invalid local state, snapshot, command, or evidence."""


def _json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _fsync_directory(path: Path) -> None:
    """Directory flush after creating a name (kept for endpoint_media).

    NTFS journals the new directory entry; the shim keeps the POSIX error
    behaviour for a missing or non-directory path.
    """
    _platform.fsync_dir(path)


class EndpointJournal:
    """Single-writer record journal with crash-tail repair and hash checking."""

    def __init__(self, root: Path):
        self.root = root
        self.fd = -1
        self.lock_fd = -1
        root.mkdir(parents=True, exist_ok=True)
        # Binary, non-inheritable, no-follow; LockFileEx on the shared RadioS
        # lock region.  A dead owner releases the lock with its handle.
        lock_fd = _platform.os_open(root / "owner.lock", os.O_CREAT | os.O_RDWR, 0o600, nofollow=True)
        try:
            _platform.flock(lock_fd, _platform.LOCK_EX | _platform.LOCK_NB)
        except BlockingIOError as exc:
            os.close(lock_fd)
            raise EndpointError("endpoint state directory is already open") from exc
        except BaseException:
            os.close(lock_fd)
            raise
        self.lock_fd = lock_fd
        try:
            self.path = root / "Endpoint.sdb"
            existed = self.path.exists()
            # O_BINARY through the shim: records end in LF exactly as on Linux
            # (a text-mode CRT descriptor would write CR LF).
            self.fd = _platform.os_open(self.path, os.O_CREAT | os.O_RDWR | os.O_APPEND, 0o600,
                                        nofollow=True)
            if not existed:
                _fsync_directory(root)
        except BaseException:
            self.close()
            raise
        self.seq = 0
        self.last_hash = _ZERO_HASH
        self.state: dict[str, Any] = {
            "identity": None, "snapshots": {}, "media": {}, "commands": {},
            "acks": {}, "events": {}, "offline_grants": {},
        }
        try:
            self._replay()
        except BaseException:
            self.close()
            raise

    def close(self) -> None:
        try:
            if self.fd >= 0:
                fd, self.fd = self.fd, -1
                os.close(fd)
        finally:
            if self.lock_fd >= 0:
                lock_fd, self.lock_fd = self.lock_fd, -1
                _platform.unlock_and_close(lock_fd)

    def _replay(self) -> None:
        offset = 0
        with _platform.open_read(self.path, nofollow=True) as stream:
            for line in stream:
                if not line.endswith(b"\n"):
                    # A process died mid-write. Only the incomplete final line
                    # is discarded; complete malformed records remain errors.
                    os.ftruncate(self.fd, offset)
                    os.fsync(self.fd)
                    break
                try:
                    record = json.loads(line)
                    claimed = record.pop("hash")
                    material = _json(record).encode("utf-8")
                    expected = hashlib.sha256(
                        bytes.fromhex(self.last_hash) + material).hexdigest()
                    if (record["seq"] != self.seq + 1 or
                        record["prev_hash"] != self.last_hash or claimed != expected):
                        raise EndpointError("endpoint journal chain is damaged")
                    self._apply(record["kind"], record["payload"])
                    self.seq = record["seq"]
                    self.last_hash = claimed
                except (AttributeError, KeyError, TypeError, ValueError,
                        UnicodeDecodeError, json.JSONDecodeError) as exc:
                    raise EndpointError("endpoint journal contains an invalid complete record") from exc
                offset += len(line)

    def append(self, kind: str, payload: dict[str, Any]) -> None:
        body = {"seq": self.seq + 1, "prev_hash": self.last_hash,
                "kind": kind, "payload": payload}
        material = _json(body).encode("utf-8")
        claimed = hashlib.sha256(bytes.fromhex(self.last_hash) + material).hexdigest()
        encoded = (_json({**body, "hash": claimed}) + "\n").encode("utf-8")
        remaining = memoryview(encoded)
        while remaining:
            written = os.write(self.fd, remaining)
            if written <= 0:
                raise OSError("endpoint journal write made no progress")
            remaining = remaining[written:]
        os.fsync(self.fd)
        self._apply(kind, payload)
        self.seq += 1
        self.last_hash = claimed

    def _apply(self, kind: str, data: dict[str, Any]) -> None:
        state = self.state
        if kind == "initialize":
            if state["identity"] is not None:
                raise EndpointError("duplicate endpoint identity in journal")
            state["identity"] = {**data, "engine_epoch": 0,
                                 "active_revision_id": None, "offline_grant_id": None,
                                 "fenced": False}
        elif kind == "snapshot":
            revision = data["revision_id"]
            state["snapshots"][revision] = {"body": data["body"],
                                             "sha256": data["sha256"],
                                             "status": "staged"}
            for item in data["body"]["media"]:
                media_id = str(item["media_file_id"])
                state["media"].setdefault(media_id, {
                    "sha256": item["sha256"].lower(),
                    "size_bytes": item["size_bytes"], "status": "missing",
                })
        elif kind == "media_status":
            state["media"][data["media_file_id"]]["status"] = data["status"]
        elif kind == "offline_grant":
            grant = data["grant"]
            revision = grant["plan_revision_id"]
            existing = state["offline_grants"].get(revision)
            if existing is not None and existing != grant:
                raise EndpointError("offline grant changed for a staged revision")
            state["offline_grants"][revision] = grant
        elif kind == "activate":
            identity = state["identity"]
            identity["active_revision_id"] = data["revision_id"]
            identity["engine_epoch"] = data["engine_epoch"]
            grant_id = data.get("offline_grant_id")
            if grant_id is not None:
                grant = state["offline_grants"].get(data["revision_id"])
                if grant is None or grant["grant_id"] != grant_id:
                    raise EndpointError("offline activation lacks a staged grant")
            identity["offline_grant_id"] = grant_id
            identity["fenced"] = False
            for snapshot in state["snapshots"].values():
                snapshot["status"] = "staged"
            state["snapshots"][data["revision_id"]]["status"] = "active"
        elif kind == "fence":
            state["identity"]["fenced"] = data["fenced"]
        elif kind == "command":
            state["commands"][data["command_id"]] = {**data, "status": "pending"}
        elif kind == "command_status":
            state["commands"][data["command_id"]]["status"] = data["status"]
        elif kind == "ack_queued":
            state["acks"][(data["command_id"], data["executed"])] = {
                **data, "status": "queued"}
        elif kind == "ack_status":
            state["acks"][(data["command_id"], data["executed"])]["status"] = data["status"]
        elif kind == "event":
            state["events"][data["engine_event_id"]] = {
                **data, "central_event_id": None, "attempts": 0}
        elif kind == "event_attempt":
            state["events"][data["engine_event_id"]]["attempts"] += 1
        elif kind == "event_delivered":
            state["events"][data["engine_event_id"]]["central_event_id"] = data["central_event_id"]
        else:
            raise EndpointError(f"unsupported endpoint journal record: {kind}")
