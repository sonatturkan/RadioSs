"""Automatic service failover with reachable-file fencing and durable decisions.

No network partition is resolved by guessing. The old primary's exact local
database and its canonical advisory lock must remain reachable for promotion.
"""

from contextlib import contextmanager
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import hashlib
from http.client import HTTPConnection, HTTPSConnection
import json
from math import isfinite
import os
from pathlib import Path
import stat
import time
from urllib.parse import urlsplit
from uuid import uuid4

from . import _platform
from .backup import _open_regular, _fsync_dir
from .schema import SCHEMA_VERSION, open_radio_store
from .storage import Corruption, StoreError, _atomic_json, _json_bytes


class FailoverError(StoreError):
    pass


@dataclass(frozen=True)
class FailoverPolicy:
    failure_threshold: int = 3
    probe_timeout_seconds: float = 1.0
    probe_interval_seconds: float = 2.0
    cooldown_seconds: float = 300.0
    catchup_max_batches: int = 8
    catchup_max_records: int = 1000

    def __post_init__(self):
        for name, low, high in (("failure_threshold", 3, 100), ("catchup_max_batches", 1, 100),
                                ("catchup_max_records", 1, 10000)):
            value = getattr(self, name)
            if type(value) is not int or not low <= value <= high:
                raise FailoverError(f"{name} must be an integer from {low} to {high}")
        for name, low, high in (("probe_timeout_seconds", .1, 30), ("probe_interval_seconds", 1, 60),
                                ("cooldown_seconds", 30, 86400)):
            value = getattr(self, name)
            if isinstance(value, bool) or not isinstance(value, (float, int)) or \
                    not isfinite(value) or not low <= value <= high:
                raise FailoverError(f"{name} must be from {low} to {high} seconds")


def _path(value):
    raw = Path(value).expanduser().absolute()
    if any(_platform.is_link(path) for path in (raw, *raw.parents)):
        raise FailoverError("failover paths must not contain symlinks")
    return raw.resolve()


def _metadata(database):
    path = database.with_name(database.name + ".meta")
    with os.fdopen(_open_regular(path), "rb") as stream:
        raw = stream.read(1024 * 1024 + 1)
    if len(raw) > 1024 * 1024:
        raise FailoverError("failover metadata exceeds its byte budget")
    meta = json.loads(raw)
    if meta.get("schema_version") != SCHEMA_VERSION:
        raise FailoverError("failover needs current-schema databases")
    return meta


def _identity(meta):
    return {field: meta[field] for field in ("store_id", "node_id", "timeline_id", "epoch")}


class FailoverSupervisor:
    def __init__(self, source_db, standby_db, source_url, state_directory, *, policy=None):
        self.source, self.standby, self.root = map(_path, (source_db, standby_db, state_directory))
        if self.source.suffix.lower() != ".sdb" or self.standby.suffix.lower() != ".sdb" or \
                self.source == self.standby or \
                self.root.is_relative_to(self.source.parent) or self.root.is_relative_to(self.standby.parent):
            raise FailoverError("supervisor needs separate .sdb paths and an independent state directory")
        self.policy = policy or FailoverPolicy()
        self.url = source_url.rstrip("/")
        parsed = urlsplit(self.url)
        if parsed.scheme not in {"http", "https"} or not parsed.hostname or \
                parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise FailoverError("primary health URL must be HTTP(S) without credentials/query")
        self.root.parent.mkdir(parents=True, exist_ok=True)
        _platform.mkdir_private(self.root, exist_ok=True)
        self.file = self.root / "supervisor.json"
        self.lock = self.root / "supervisor.lock"
        self.config = {"source_database": str(self.source), "standby_database": str(self.standby),
                       "source_url": self.url, "policy": asdict(self.policy)}
        with self._job():
            if self.file.exists():
                if self._read()["configuration"] != self.config:
                    raise FailoverError("supervisor configuration differs from its durable job")
            else:
                source, target = _metadata(self.source), _metadata(self.standby)
                if source.get("node_role") != "primary" or target.get("node_role") != "standby" or \
                        source["store_id"] != target["store_id"] or \
                        source["timeline_id"] != target["timeline_id"] or source["epoch"] != target["epoch"] or \
                        target.get("replication", {}).get("source_node_id") != source["node_id"]:
                    raise FailoverError("initial failover pair is not a primary and its matching standby")
                now = time.time()
                self._save({"format": "sdbase-failover-supervisor-v1", "job_id": str(uuid4()),
                    "configuration": self.config, "source_identity": _identity(source),
                    "standby_identity": _identity(target), "state": "watching", "failures": 0,
                    "last_probe_at": None, "last_probe_time": None, "cooldown_until": now,
                    "active_database": str(self.source), "pending_bundle": None,
                    "promotion": None, "last_error": None, "health_confirmed": False,
                    "operator_required": False})

    @contextmanager
    def _job(self):
        fd = _platform.open_fd(self.lock, os.O_CREAT | os.O_RDWR, 0o600, nofollow=True)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise FailoverError("unsafe supervisor lock")
            try:
                _platform.flock(fd, _platform.LOCK_EX | _platform.LOCK_NB)
            except BlockingIOError as exc:
                raise FailoverError("another supervisor owns this job") from exc
            yield
        finally:
            _platform.unlock_and_close(fd)

    def _read(self):
        with os.fdopen(_open_regular(self.file), "rb") as stream:
            raw = stream.read(1024 * 1024 + 1)
        if len(raw) > 1024 * 1024:
            raise FailoverError("supervisor state exceeds its byte budget")
        envelope = json.loads(raw)
        state = envelope["payload"]
        if hashlib.sha256(_json_bytes(state)).hexdigest() != envelope["sha256"] or \
                state.get("format") != "sdbase-failover-supervisor-v1":
            raise FailoverError("supervisor state checksum or format mismatch")
        return state

    def _save(self, state):
        _atomic_json(self.file, {"payload": state, "sha256": hashlib.sha256(_json_bytes(state)).hexdigest()})

    def status(self):
        with self._job():
            return self._read()

    def _probe(self, identity):
        parsed = urlsplit(self.url)
        factory = HTTPSConnection if parsed.scheme == "https" else HTTPConnection
        host = parsed.hostname
        if parsed.scheme == "http" and host.lower() == "localhost":
            # Windows tries ::1 first and needs about 2 s to fall back to the
            # IPv4 listener, longer than the probe timeout; a healthy primary
            # would look absent.  Plain HTTP has no name to verify.
            host = "127.0.0.1"
        connection = factory(host, parsed.port, timeout=self.policy.probe_timeout_seconds)
        try:
            connection.request("GET", parsed.path.rstrip("/") + "/health", headers={"Connection": "close"})
            response = connection.getresponse()
            if response.status != 200:
                return {"result": "missing", "detail": f"HTTP {response.status}"}
            raw = response.read(65537)
            if len(raw) > 65536:
                return {"result": "foreign", "detail": "oversized health response"}
            health = json.loads(raw)
            if any(health.get(key) != value for key, value in identity.items()) or \
                    health.get("node_role") != "primary" or health.get("status") != "ok" or \
                    type(health.get("sequence")) is not int:
                return {"result": "foreign", "detail": "health identity or role differs from the pinned primary"}
            return {"result": "healthy", "sequence": health["sequence"]}
        except (OSError, TimeoutError) as exc:
            return {"result": "missing", "detail": type(exc).__name__}
        except Exception as exc:
            return {"result": "foreign", "detail": "invalid health response: " + type(exc).__name__}
        finally:
            connection.close()

    def _pair(self, state, *, promotion=False):
        for database in (self.source, self.standby):
            for path in (database, database.with_name(database.name + ".meta"),
                         database.with_name(database.name + ".lock")):
                if not path.is_file() or _platform.is_link(path):
                    raise FailoverError("known database components are unreachable; automatic fencing is refused")
        source_components = [self.source, self.source.with_name(self.source.name + ".meta"),
                             self.source.with_name(self.source.name + ".lock")]
        target_components = [self.standby, self.standby.with_name(self.standby.name + ".meta"),
                             self.standby.with_name(self.standby.name + ".lock")]
        if any(os.path.samefile(left, right) for left in source_components for right in target_components):
            raise FailoverError("primary and standby must not alias the same database files")
        source, target = _metadata(self.source), _metadata(self.standby)
        if _identity(source) != state["source_identity"] or \
                target["node_id"] != state["standby_identity"]["node_id"]:
            raise FailoverError("database identity changed since supervisor initialization")
        if not promotion and (_identity(target) != state["standby_identity"] or
                              source.get("node_role") != "primary" or target.get("node_role") != "standby"):
            raise FailoverError("automatic takeover requires its unchanged primary/standby pair")

    def _catchup(self, state):
        from .replication import (apply_shipment, create_shipment, recover_standby_replay,
                                  replication_status)
        source = open_radio_store(self.source)
        from .wal_archive import capture_archive
        archive = capture_archive(source)
        if not archive["enabled"] or archive["last_sequence"] != source.sequence:
            raise FailoverError("primary durable state and recovery archive do not have the same committed end")
        for _ in range(self.policy.catchup_max_batches):
            if state["pending_bundle"]:
                bundle = self.root / state["pending_bundle"]
                if bundle.parent != self.root or not bundle.name.startswith("shipment-") or \
                        _platform.is_link(bundle):
                    raise FailoverError("invalid durable supervisor shipment path")
                try:
                    target = open_radio_store(self.standby)
                    apply_shipment(target, bundle)
                except Corruption:
                    recover_standby_replay(self.standby, bundle)
                state["pending_bundle"] = None
                self._save(state)
                _platform.rmtree(bundle)
                _fsync_dir(self.root)
            target = open_radio_store(self.standby)
            status = replication_status(target)
            position = status["replication"]
            if source.sequence == position["last_replayed_sequence"] and not position.get("pending"):
                return source, target
            bundle = self.root / ("shipment-" + uuid4().hex)
            create_shipment(source, bundle, after_sequence=position["last_replayed_sequence"],
                            expected_previous_sha256=position["last_record_sha256"],
                            max_records=self.policy.catchup_max_records)
            state["pending_bundle"] = bundle.name
            self._save(state)
        # Keep the durable last bundle for the next tick or a restarted process.
        raise FailoverError("catch-up budget exhausted; retained shipment for the next tick")

    def _promote(self, state):
        from .replication import promote_standby
        self._pair(state, promotion=True)
        expected = state["promotion"]
        result = promote_standby(open_radio_store(self.source), open_radio_store(self.standby),
            expected_source_sequence=expected["source_sequence"],
            expected_source_timeline_id=expected["source_timeline_id"])
        state.update(state="promoted", active_database=str(self.standby), promotion_result=result,
                     last_error=None, cooldown_until=time.time() + self.policy.cooldown_seconds)
        self._save(state)
        return state

    def tick(self):
        with self._job():
            state = self._read()
            if state["state"] == "promoted":
                target = _metadata(self.standby)
                result = state["promotion_result"]
                if target.get("node_role") != "primary" or target.get("timeline_id") != result["timeline_id"] or \
                        target.get("epoch") != result["epoch"] or target.get("node_id") != state["standby_identity"]["node_id"]:
                    raise FailoverError("promoted routing target no longer owns the recorded writable timeline")
                return state
            if state.get("operator_required"):
                return state
            try:
                if state["state"] == "promotion_pending":
                    return self._promote(state)
                self._pair(state)
                now = time.time()
                if state["last_probe_time"] is not None and now - state["last_probe_time"] < self.policy.probe_interval_seconds:
                    return state
                state["last_probe_time"] = now
                state["last_probe_at"] = datetime.now(timezone.utc).isoformat()
                probe = self._probe(state["source_identity"])
                state["last_probe"] = probe
                if probe["result"] == "foreign":
                    state.update(state="blocked", failures=0, last_error=probe["detail"], operator_required=True)
                    self._save(state)
                    return state
                if probe["result"] == "healthy":
                    state.update(state="watching", failures=0, last_error=None, health_confirmed=True)
                    self._save(state)
                    return state
                if not state["health_confirmed"]:
                    state.update(state="blocked", failures=0,
                                 last_error="primary service identity has never been confirmed; start it before arming")
                    self._save(state)
                    return state
                state["failures"] += 1
                state.update(state="service_failed", last_error=probe["detail"])
                self._save(state)
                if state["failures"] < self.policy.failure_threshold or now < state["cooldown_until"]:
                    return state
                state["state"] = "catching_up"
                self._save(state)
                source, target = self._catchup(state)
                # A recovered service resets hysteresis; a foreign responder
                # never becomes evidence of a primary outage.
                probe = self._probe(state["source_identity"])
                if probe["result"] != "missing":
                    state.update(state="watching" if probe["result"] == "healthy" else "blocked",
                                 failures=0, last_probe=probe, last_error=probe.get("detail"),
                                 operator_required=probe["result"] == "foreign")
                    self._save(state)
                    return state
                self._pair(state)
                state["promotion"] = {"source_sequence": source.sequence,
                                      "source_timeline_id": state["source_identity"]["timeline_id"]}
                state["state"] = "promotion_pending"
                self._save(state)
                return self._promote(state)
            except Exception as exc:
                # Never overwrite a durable handover intent after source fencing.
                if state["state"] == "promotion_pending":
                    try:
                        source_meta, target_meta = _metadata(self.source), _metadata(self.standby)
                        if source_meta.get("node_role") == "primary" and not target_meta.get("promotion_pending"):
                            state.update(state="blocked", promotion=None, failures=0)
                    except Exception:
                        pass
                if state["state"] != "promotion_pending":
                    state["state"] = "blocked"
                state["last_error"] = type(exc).__name__ + ": " + str(exc)
                self._save(state)
                return state

    def reset(self):
        """Reset failed-probe hysteresis only; a completed/fenced handover is final."""
        with self._job():
            state = self._read()
            if state["state"] in {"promoted", "promotion_pending"} or state["pending_bundle"]:
                raise FailoverError("cannot reset a completed or pending handover/replay")
            self._pair(state)
            state.update(state="watching", failures=0, last_probe_time=None, last_error=None,
                         cooldown_until=time.time() + self.policy.cooldown_seconds,
                         health_confirmed=False, operator_required=False)
            self._save(state)
            return state

    @contextmanager
    def _runner(self):
        """Keep one monitoring process owner, including between probe ticks."""
        fd = _platform.open_fd(self.root / "runner.lock", os.O_CREAT | os.O_RDWR, 0o600, nofollow=True)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise FailoverError("unsafe supervisor runner lock")
            try:
                _platform.flock(fd, _platform.LOCK_EX | _platform.LOCK_NB)
            except BlockingIOError as exc:
                raise FailoverError("another monitor owns this supervisor job") from exc
            yield
        finally:
            _platform.unlock_and_close(fd)

    def watch(self, stop_event):
        """Yield durable decisions with one continuous monitor per job.

        Close the generator on early exit to release monitor ownership. A
        process crash releases the kernel lock, so restart needs no lock-file
        deletion. Individual tick/status/reset calls still use the decision
        lock and never run concurrently with another decision.
        """
        with self._runner():
            while not stop_event.is_set():
                yield self.tick()
                stop_event.wait(self.policy.probe_interval_seconds)

    def run(self, stop_event):
        """Run consecutive ticks until an explicit shutdown event is set."""
        for _state in self.watch(stop_event):
            pass
