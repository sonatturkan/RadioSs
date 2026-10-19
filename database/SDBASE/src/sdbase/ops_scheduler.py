"""Crash-recoverable local schedule for verified SDBASE backups and audit copies.

This module never prunes database rows, media objects, backups or archives.
The output directory must be private and on a filesystem with working flock,
fsync and atomic rename semantics. A digest detects accidental state damage;
it does not authenticate a hostile operator or host.
"""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import asdict, dataclass
from datetime import datetime, timedelta, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile
from typing import Any, Iterator, Mapping
from uuid import UUID, uuid4

from .backup import create_backup, verify_backup
from .capacity_monitor import _delivery_policy, evaluate_delivery_health
from .identity import authenticate
from .ops_archive import create_archive, verify_archive, verify_archive_chain
from .schema import open_radio_store
from .storage import Store, StoreError


_FORMAT = "sdbase-ops-scheduler-v1"
_STATE = "scheduler-state.json"
_LOCK = "scheduler.lock"
_MAX_STATE_BYTES = 64 * 1024 * 1024
_ARTIFACT_NAME = re.compile(r"(?:backup|archive)-[0-9]{8}T[0-9]{12}Z-[0-9a-f]{12}\Z")
_ENV_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")


class SchedulerError(StoreError):
    """The schedule, persistent progress, or an output is unsafe."""


@dataclass(frozen=True)
class ScheduleConfig:
    database: Path
    output_dir: Path
    backup_interval_seconds: int = 24 * 3600
    archive_interval_seconds: int = 3600
    max_archive_records: int = 10_000
    max_archive_bytes: int = 64 * 1024 * 1024
    max_archive_segments_per_run: int = 4
    station_ids: tuple[str, ...] = ()
    token_env: str | None = None
    delivery_policy: Mapping[str, Any] | None = None
    delivery_interval_seconds: int = 300


def _canonical(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False).encode("utf-8")


def _unique_keys(pairs: list[tuple[str, Any]]) -> dict:
    result: dict = {}
    for key, value in pairs:
        if key in result:
            raise SchedulerError("scheduler state has duplicate JSON keys")
        result[key] = value
    return result


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _stamp(value: datetime) -> str:
    return value.astimezone(timezone.utc).isoformat()


def _parse_stamp(value: Any) -> datetime:
    if not isinstance(value, str):
        raise SchedulerError("scheduler state has an invalid timestamp")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise SchedulerError("scheduler state has an invalid timestamp") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise SchedulerError("scheduler state timestamps need an offset")
    return parsed.astimezone(timezone.utc)


def _positive(value: Any, name: str, maximum: int) -> None:
    if type(value) is not int or not 1 <= value <= maximum:
        raise SchedulerError(f"{name} must be 1..{maximum}")


def _validate_config(config: ScheduleConfig) -> tuple[Path, Path]:
    if not isinstance(config, ScheduleConfig):
        raise SchedulerError("a ScheduleConfig is required")
    for name in ("backup_interval_seconds", "archive_interval_seconds"):
        _positive(getattr(config, name), name, 366 * 24 * 3600)
    _positive(config.max_archive_records, "max_archive_records", 100_000)
    if type(config.max_archive_bytes) is not int or not 1024 <= config.max_archive_bytes <= 2 * 1024**3:
        raise SchedulerError("max_archive_bytes must be 1024..2147483648")
    _positive(config.max_archive_segments_per_run, "max_archive_segments_per_run", 100)
    _positive(config.delivery_interval_seconds, "delivery_interval_seconds", 366 * 24 * 3600)
    delivery_enabled = bool(config.station_ids or config.token_env or config.delivery_policy)
    if delivery_enabled:
        if not config.station_ids or not config.token_env or config.delivery_policy is None:
            raise SchedulerError("delivery checks require station_ids, token_env and delivery_policy")
        if not isinstance(config.station_ids, tuple) or len(config.station_ids) > 100 or \
                not isinstance(config.token_env, str) or not _ENV_NAME.fullmatch(config.token_env) or \
                not isinstance(config.delivery_policy, Mapping):
            raise SchedulerError("delivery schedule settings are invalid")
        canonical_ids = []
        for station_id in config.station_ids:
            try:
                canonical_ids.append(str(UUID(station_id)))
            except (ValueError, TypeError, AttributeError) as exc:
                raise SchedulerError("delivery station IDs must be UUIDs") from exc
        if len(set(canonical_ids)) != len(canonical_ids) or \
                canonical_ids != list(config.station_ids):
            raise SchedulerError("delivery station IDs must be distinct canonical UUIDs")
    raw_database = Path(config.database).expanduser()
    if raw_database.suffix != ".sdb" or raw_database.is_symlink() or not raw_database.is_file():
        raise SchedulerError("an existing regular .sdb database is required")
    database = raw_database.resolve(strict=True)
    for suffix in (".meta", ".lock"):
        component = database.with_name(database.name + suffix)
        if component.is_symlink() or not component.is_file():
            raise SchedulerError("existing .sdb metadata and lock files are required")
    root = Path(config.output_dir).expanduser()
    if root.is_symlink() or not root.is_dir():
        raise SchedulerError("output_dir must be an existing private directory")
    root = root.resolve(strict=True)
    mode = root.stat().st_mode
    if not stat.S_ISDIR(mode) or mode & 0o077:
        raise SchedulerError("output_dir must be accessible only by its owner (mode 0700)")
    if root == database.parent:
        raise SchedulerError("output_dir must be separate from the database directory")
    return database, root


@contextmanager
def scheduler_lock(root: Path) -> Iterator[None]:
    """Hold one nonblocking process lock for the whole once/loop invocation."""
    path = root / _LOCK
    flags = os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags, 0o600)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise SchedulerError("scheduler lock is not a regular file")
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise SchedulerError("another scheduler owns this output directory") from exc
        yield
    finally:
        try:
            fcntl.flock(fd, fcntl.LOCK_UN)
        finally:
            os.close(fd)


def _read_state(root: Path) -> dict | None:
    path = root / _STATE
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    except FileNotFoundError:
        return None
    except OSError as exc:
        raise SchedulerError("cannot read scheduler state") from exc
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise SchedulerError("scheduler state is not a regular file")
        with os.fdopen(fd, "rb") as stream:
            fd = -1
            encoded = stream.read(_MAX_STATE_BYTES + 1)
    finally:
        if fd != -1:
            os.close(fd)
    if len(encoded) > _MAX_STATE_BYTES:
        raise SchedulerError("scheduler state exceeds its size bound")
    try:
        envelope = json.loads(encoded, object_pairs_hook=_unique_keys)
        if (not isinstance(envelope, dict) or set(envelope) != {"payload", "sha256"} or
                not isinstance(envelope["payload"], dict) or
                envelope["sha256"] != hashlib.sha256(_canonical(envelope["payload"])).hexdigest()):
            raise SchedulerError("scheduler state checksum mismatch")
    except (UnicodeError, ValueError, TypeError, KeyError) as exc:
        raise SchedulerError("scheduler state is invalid JSON") from exc
    return envelope["payload"]


def _write_state(root: Path, state: dict) -> None:
    encoded = _canonical({"payload": state,
                          "sha256": hashlib.sha256(_canonical(state)).hexdigest()})
    if len(encoded) > _MAX_STATE_BYTES:
        raise SchedulerError("scheduler state exceeds its size bound")
    path = root / _STATE
    if path.is_symlink():
        raise SchedulerError("scheduler state path became a symlink")
    fd, temporary = tempfile.mkstemp(prefix=".scheduler-state-", dir=root)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        dir_fd = os.open(root, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(dir_fd)
        finally:
            os.close(dir_fd)
    finally:
        if os.path.lexists(temporary):
            os.unlink(temporary)


def _check_state(state: dict, database: Path, root: Path, store_id: str) -> None:
    required = {"format", "database", "output_dir", "store_id", "archives", "last_backup",
                "last_backup_attempt_at", "last_archive_attempt_at", "archive_backlog",
                "pending", "last_errors", "delivery"}
    if set(state) != required or state["format"] != _FORMAT or \
            state["database"] != str(database) or state["output_dir"] != str(root) or \
            state["store_id"] != store_id:
        raise SchedulerError("scheduler state belongs to another database or output directory")
    if not isinstance(state["archives"], list) or not isinstance(state["pending"], dict) or \
            set(state["pending"]) != {"backup", "archive"} or \
            not isinstance(state["last_errors"], dict) or \
            set(state["last_errors"]) != {"backup", "archive"} or \
            type(state["archive_backlog"]) is not bool:
        raise SchedulerError("scheduler state has invalid progress fields")
    for name in ("last_backup_attempt_at", "last_archive_attempt_at"):
        if state[name] is not None:
            _parse_stamp(state[name])
    for name in ("backup", "archive"):
        pending = state["pending"][name]
        if pending is not None:
            if not isinstance(pending, dict) or set(pending) != {"path", "started_at"}:
                raise SchedulerError("scheduler state has invalid pending work")
            _artifact_path(root, pending["path"], name)
            _parse_stamp(pending["started_at"])
        if state["last_errors"][name] is not None and \
                not isinstance(state["last_errors"][name], str):
            raise SchedulerError("scheduler state has invalid error fields")
    last = 0
    paths: set[str] = set()
    for row in state["archives"]:
        if not isinstance(row, dict) or set(row) != {"path", "first_sequence", "last_sequence",
                                                  "manifest_sha256", "chain_sha256"}:
            raise SchedulerError("scheduler state has an invalid archive entry")
        _artifact_path(root, row["path"], "archive")
        if row["path"] in paths or type(row["first_sequence"]) is not int or \
                type(row["last_sequence"]) is not int or row["first_sequence"] != last + 1 or \
                row["last_sequence"] < row["first_sequence"]:
            raise SchedulerError("scheduler archive sequence is not contiguous")
        for name in ("manifest_sha256", "chain_sha256"):
            if not isinstance(row[name], str) or len(row[name]) != 64:
                raise SchedulerError("scheduler archive digest is invalid")
        paths.add(row["path"])
        last = row["last_sequence"]
    backup = state["last_backup"]
    if backup is not None:
        if not isinstance(backup, dict) or set(backup) != {"path", "sequence", "verified_at"}:
            raise SchedulerError("scheduler state has an invalid backup entry")
        _artifact_path(root, backup["path"], "backup")
        if type(backup["sequence"]) is not int or backup["sequence"] < 0:
            raise SchedulerError("scheduler backup sequence is invalid")
        _parse_stamp(backup["verified_at"])
    if not isinstance(state["delivery"], dict):
        raise SchedulerError("scheduler delivery state is invalid")
    for station_id, row in state["delivery"].items():
        try:
            if str(UUID(station_id)) != station_id:
                raise ValueError
        except (ValueError, TypeError, AttributeError) as exc:
            raise SchedulerError("scheduler delivery station ID is invalid") from exc
        if not isinstance(row, dict) or set(row) != {"last_attempt_at", "pending", "last_error"}:
            raise SchedulerError("scheduler delivery progress is invalid")
        if row["last_attempt_at"] is not None:
            _parse_stamp(row["last_attempt_at"])
        if row["last_error"] is not None and not isinstance(row["last_error"], str):
            raise SchedulerError("scheduler delivery error is invalid")
        if row["pending"] is not None:
            pending = row["pending"]
            if not isinstance(pending, dict) or set(pending) != {"observation_key", "policy"} or \
                    not isinstance(pending["observation_key"], str) or \
                    not isinstance(pending["policy"], dict):
                raise SchedulerError("scheduler pending delivery observation is invalid")


def _artifact_path(root: Path, name: Any, kind: str) -> Path:
    if not isinstance(name, str) or not name.startswith(kind + "-") or \
            not _ARTIFACT_NAME.fullmatch(name):
        raise SchedulerError("scheduler artifact path is invalid")
    return root / name


def _new_path(root: Path, kind: str) -> Path:
    return root / (f"{kind}-{_now().strftime('%Y%m%dT%H%M%S%fZ')}-{uuid4().hex[:12]}")


def _open_existing(database: Path) -> Store:
    return open_radio_store(database)


def _new_state(database: Path, root: Path, store_id: str) -> dict:
    if any(entry.name.startswith(("backup-", "archive-")) for entry in root.iterdir()):
        raise SchedulerError("managed artifacts exist without scheduler state; recover state before continuing")
    return {"format": _FORMAT, "database": str(database), "output_dir": str(root),
            "store_id": store_id, "archives": [], "last_backup": None,
            "last_backup_attempt_at": None, "last_archive_attempt_at": None,
            "archive_backlog": False, "pending": {"backup": None, "archive": None},
            "last_errors": {"backup": None, "archive": None}, "delivery": {}}


def _archive_entry(result: dict) -> dict:
    return {"path": Path(result["path"]).name,
            **{key: result[key] for key in ("first_sequence", "last_sequence",
                                            "manifest_sha256", "chain_sha256")}}


def _same_archive(verified: dict, created: dict) -> None:
    for key in ("store_id", "first_sequence", "last_sequence", "manifest_sha256",
                "chain_sha256"):
        if verified[key] != created[key]:
            raise SchedulerError("published archive differs from its verified result")


def _finish_backup(state: dict, root: Path, verified: dict, at: datetime) -> None:
    if verified["store_id"] != state["store_id"]:
        raise SchedulerError("backup belongs to another store")
    state["last_backup"] = {"path": Path(verified["path"]).name,
                            "sequence": verified["sequence"], "verified_at": _stamp(at)}
    state["pending"]["backup"] = None
    state["last_errors"]["backup"] = None
    _write_state(root, state)


def _finish_archive(state: dict, root: Path, verified: dict) -> None:
    if verified["store_id"] != state["store_id"]:
        raise SchedulerError("archive belongs to another store")
    expected = state["archives"][-1]["last_sequence"] + 1 if state["archives"] else 1
    if verified["first_sequence"] != expected:
        raise SchedulerError("archive does not continue scheduler state")
    state["archives"].append(_archive_entry(verified))
    state["pending"]["archive"] = None
    state["last_errors"]["archive"] = None
    _write_state(root, state)


def _verify_known_chain(state: dict, root: Path) -> None:
    if not state["archives"]:
        return
    paths = [_artifact_path(root, row["path"], "archive") for row in state["archives"]]
    chain = verify_archive_chain(paths)
    final = state["archives"][-1]
    if (chain["store_id"] != state["store_id"] or
            any(chain[key] != final[key] for key in ("last_sequence", "manifest_sha256",
                                                  "chain_sha256"))):
        raise SchedulerError("archive chain differs from scheduler state")


def _recover_pending(state: dict, root: Path, kind: str) -> str | None:
    pending = state["pending"][kind]
    if pending is None:
        return None
    path = _artifact_path(root, pending["path"], kind)
    if not os.path.lexists(path):
        state["pending"][kind] = None
        state[f"last_{kind}_attempt_at"] = None
        state["last_errors"][kind] = "interrupted before publication; no artifact exists"
        _write_state(root, state)
        return "absent"
    if kind == "backup":
        verified = verify_backup(path)
        _finish_backup(state, root, verified, _now())
    else:
        _verify_known_chain(state, root)
        prior = (_artifact_path(root, state["archives"][-1]["path"], "archive")
                 if state["archives"] else None)
        verified = verify_archive(path, previous_archive=prior)
        _finish_archive(state, root, verified)
        state["archive_backlog"] = True
        _write_state(root, state)
    return "verified"


def _due(last: str | None, seconds: int, now: datetime) -> bool:
    if last is None:
        return True
    observed = _parse_stamp(last)
    if observed > now + timedelta(minutes=5):
        raise SchedulerError("system clock moved backwards beyond five minutes")
    return now >= observed + timedelta(seconds=seconds)


def _run_backup(store: Store, state: dict, root: Path, at: datetime) -> dict:
    path = _new_path(root, "backup")
    state["pending"]["backup"] = {"path": path.name, "started_at": _stamp(at)}
    state["last_backup_attempt_at"] = _stamp(at)
    _write_state(root, state)
    try:
        created = create_backup(store, path, checkpoint=False)
        verified = verify_backup(path)
        if created["store_id"] != verified["store_id"] or \
                created["sequence"] != verified["sequence"]:
            raise SchedulerError("published backup differs from its verified result")
        _finish_backup(state, root, verified, _now())
        return {"status": "verified", "path": str(path), "sequence": verified["sequence"]}
    except Exception:
        if not os.path.lexists(path):
            state["pending"]["backup"] = None
            _write_state(root, state)
        raise


def _run_archive(store: Store, state: dict, root: Path, at: datetime,
                 config: ScheduleConfig) -> dict:
    _verify_known_chain(state, root)
    target_sequence = store.sequence
    last = state["archives"][-1]["last_sequence"] if state["archives"] else 0
    state["last_archive_attempt_at"] = _stamp(at)
    if target_sequence <= last:
        state["archive_backlog"] = False
        _write_state(root, state)
        return {"status": "current", "through_sequence": last, "segments": []}
    segments: list[dict] = []
    for _ in range(config.max_archive_segments_per_run):
        path = _new_path(root, "archive")
        state["pending"]["archive"] = {"path": path.name, "started_at": _stamp(_now())}
        _write_state(root, state)
        try:
            prior = (_artifact_path(root, state["archives"][-1]["path"], "archive")
                     if state["archives"] else None)
            created = create_archive(store, path, previous_archive=prior,
                                     through_sequence=target_sequence,
                                     max_records=config.max_archive_records,
                                     max_bytes=config.max_archive_bytes)
            verified = verify_archive(path, previous_archive=prior)
            _same_archive(verified, created)
            _finish_archive(state, root, verified)
            segments.append(_archive_entry(verified))
            if verified["last_sequence"] >= target_sequence:
                break
        except Exception:
            if not os.path.lexists(path):
                state["pending"]["archive"] = None
                _write_state(root, state)
            raise
    last = state["archives"][-1]["last_sequence"]
    state["archive_backlog"] = last < target_sequence
    _write_state(root, state)
    return {"status": "verified", "through_sequence": target_sequence,
            "last_sequence": last, "has_more": state["archive_backlog"],
            "segments": segments}


def _run_delivery(store: Store, state: dict, root: Path, at: datetime,
                  config: ScheduleConfig, station_id: str, *, force: bool) -> dict:
    row = state["delivery"].setdefault(station_id, {
        "last_attempt_at": None, "pending": None, "last_error": None})
    due = force or _due(row["last_attempt_at"], config.delivery_interval_seconds, at)
    if not due:
        return {"status": "retry_wait" if row["pending"] else "not_due",
                "last_error": row["last_error"]}
    selected_policy = _delivery_policy(config.delivery_policy or {})
    if row["pending"] is not None:
        try:
            _delivery_policy(row["pending"]["policy"])
        except StoreError:
            # Validation runs before any diagnostic write. An invalid policy
            # from an older scheduler build can therefore be replaced safely.
            row["pending"] = None
    if row["pending"] is None:
        policy = asdict(selected_policy)
        row["pending"] = {"observation_key": uuid4().hex, "policy": policy}
    row["last_attempt_at"] = _stamp(at)
    _write_state(root, state)
    token = os.environ.get(config.token_env or "", "")
    principal = authenticate(store, token)
    observation = row["pending"]
    result = evaluate_delivery_health(
        store, principal, station_id, observation["policy"],
        observation_key=observation["observation_key"])
    row["pending"] = None
    row["last_error"] = None
    _write_state(root, state)
    counts: dict[str, int] = {}
    for check in result["checks"]:
        counts[check["state"]] = counts.get(check["state"], 0) + 1
    return {"status": "evaluated", "station_id": station_id,
            "scan_complete": result["metrics"]["scan_complete"],
            "check_states": counts, "new_diagnostics": len(result["diagnostics"]),
            "source_sequence": result["metrics"]["sequence"]}


def _prepare(config: ScheduleConfig) -> tuple[Path, Path, Store, dict]:
    database, root = _validate_config(config)
    store = _open_existing(database)
    with store._locked(exclusive=False):
        store._reload_locked()
        store_id = store._meta().get("store_id")
    if not isinstance(store_id, str) or not store_id:
        raise SchedulerError("database has no store identity")
    state = _read_state(root)
    if state is None:
        state = _new_state(database, root, store_id)
        _write_state(root, state)
    else:
        _check_state(state, database, root, store_id)
    return database, root, store, state


def _run_prepared(store: Store, state: dict, root: Path,
                  config: ScheduleConfig, *, force: bool = False) -> dict:
    at = _now()
    report: dict[str, Any] = {"observed_at": _stamp(at), "store_id": state["store_id"],
                              "backup": {"status": "not_due"},
                              "archive": {"status": "not_due"},
                              "delivery": {}, "errors": {}}
    for station_id in config.station_ids:
        try:
            result = _run_delivery(store, state, root, at, config, station_id, force=force)
            report["delivery"][station_id] = result
            if result["status"] == "retry_wait" and result["last_error"]:
                report["errors"]["delivery:" + station_id] = result["last_error"]
        except Exception as exc:
            message = f"{type(exc).__name__}: {exc}"[:1000]
            row = state["delivery"].setdefault(station_id, {
                "last_attempt_at": _stamp(at), "pending": None, "last_error": None})
            if row["last_attempt_at"] is None:
                row["last_attempt_at"] = _stamp(at)
            row["last_error"] = message
            _write_state(root, state)
            report["delivery"][station_id] = {"status": "error", "error": message}
            report["errors"]["delivery:" + station_id] = message
    for kind in ("backup", "archive"):
        interval = (config.backup_interval_seconds if kind == "backup"
                    else config.archive_interval_seconds)
        previous_error = state["last_errors"][kind]
        if state["pending"][kind] is not None and previous_error and not force and \
                not _due(state[f"last_{kind}_attempt_at"], interval, at):
            report[kind] = {"status": "retry_wait", "error": previous_error}
            report["errors"][kind] = previous_error
            continue
        try:
            recovered = _recover_pending(state, root, kind)
            if recovered is not None:
                report[kind] = {"status": "recovered_" + recovered}
        except Exception as exc:
            message = f"{type(exc).__name__}: {exc}"[:1000]
            state["last_errors"][kind] = message
            state[f"last_{kind}_attempt_at"] = _stamp(at)
            _write_state(root, state)
            report[kind] = {"status": "error", "error": message}
            report["errors"][kind] = message
    for kind in ("backup", "archive"):
        if state["pending"][kind] is not None or kind in report["errors"]:
            continue
        try:
            interval = (config.backup_interval_seconds if kind == "backup"
                        else config.archive_interval_seconds)
            last = state[f"last_{kind}_attempt_at"]
            due = force or (kind == "archive" and state["archive_backlog"]) or \
                _due(last, interval, at)
            if not due:
                if state["last_errors"][kind]:
                    message = state["last_errors"][kind]
                    report[kind] = {"status": "retry_wait", "error": message}
                    report["errors"][kind] = message
                continue
            if kind == "backup":
                report[kind] = _run_backup(store, state, root, at)
            else:
                report[kind] = _run_archive(store, state, root, at, config)
            state["last_errors"][kind] = None
            _write_state(root, state)
        except Exception as exc:
            message = f"{type(exc).__name__}: {exc}"[:1000]
            state["last_errors"][kind] = message
            state[f"last_{kind}_attempt_at"] = _stamp(at)
            if kind == "archive":
                state["archive_backlog"] = False
            _write_state(root, state)
            report[kind] = {"status": "error", "error": message}
            report["errors"][kind] = message
    report["archived_through_sequence"] = (state["archives"][-1]["last_sequence"]
                                            if state["archives"] else 0)
    report["source_sequence"] = store.sequence
    return report


def run_once(config: ScheduleConfig, *, force: bool = False) -> dict:
    """Run due jobs once, with a process lock and durable crash recovery."""
    database, root = _validate_config(config)
    with scheduler_lock(root):
        _database, _root, store, state = _prepare(config)
        return _run_prepared(store, state, root, config, force=force)


def run_locked_loop(config: ScheduleConfig, stop, *, poll_seconds: int = 30,
                    on_report=None) -> None:
    """Hold one process lock until ``stop`` is set; caller installs signals."""
    _positive(poll_seconds, "poll_seconds", 3600)
    database, root = _validate_config(config)
    with scheduler_lock(root):
        _database, _root, store, state = _prepare(config)
        while not stop.is_set():
            result = _run_prepared(store, state, root, config)
            if on_report is not None:
                on_report(result)
            stop.wait(poll_seconds)
