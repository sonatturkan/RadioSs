"""Yönetim › Veritabanı durumu › "Tam denetim": the whole-store integrity scrub as a job.

The on-demand check behind "Durumu yenile" stops at the administration
overview's 64 MiB / 50 000 record budget, which a working station's journal,
checkpoint and managed media outgrow, and one HTTP request cannot read and
hash all of them within its time limit. A full check therefore sizes the scrub
budgets to the whole store before it starts and runs ``inspect_integrity`` in
a daemon thread of the data service; the desktop polls the job over HTTP. At
most one job runs at a time and the last few finished reports stay pollable.

The scrub reads the original database files under a shared lock (writes wait
while they are read) and then hashes every managed media object and cold
archive without holding it. It needs whole-store engine administration
(global ``manage``), exactly as ``GET /v1/operations/integrity`` does.
"""
from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from uuid import uuid4

from . import desktop_api as api
from .monitoring import require_engine_manager
from .storage import StoreError

MAX_FINISHED_JOBS = 8
# The scrub's own upper limits (integrity.inspect_integrity).
MAX_RECORDS = 10_000_000
MAX_BYTES = 8 * 1024 ** 4
# Head room for frames and rows committed between sizing and reading: 10 %,
# but never less than these floors on a small store.
_MARGIN_BYTES = 64 * 1024 * 1024
_MARGIN_RECORDS = 10_000

_LOCK = threading.Lock()
_JOBS: dict[str, "_Job"] = {}
_RUNNING: "_Job | None" = None


def _now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class _Job:
    def __init__(self, station_id, verify_media, max_bytes, max_records):
        self.job_id = str(uuid4())
        self.station_id = station_id
        self.verify_media = verify_media
        self.max_bytes = max_bytes
        self.max_records = max_records
        self.state = "running"
        self.started_at = _now()
        self.finished_at = None
        self.finished = None
        self.result = None
        self.message = None

    def finish(self, state, message, result=None):
        with _LOCK:
            self.state, self.message, self.result = state, message, result
            self.finished_at, self.finished = _now(), time.monotonic()
            _prune_locked()

    def view(self):
        with _LOCK:
            view = {"job_id": self.job_id, "state": self.state, "started_at": self.started_at}
            if self.finished_at is not None:
                view["finished_at"] = self.finished_at
            if self.result is not None:
                view["result"] = deepcopy(self.result)
            if self.message:
                view["message"] = self.message
            return view


def _prune_locked():
    finished = sorted((job for job in _JOBS.values() if job.state != "running"), key=lambda job: job.finished)
    for stale in finished[:max(0, len(finished) - MAX_FINISHED_JOBS)]:
        _JOBS.pop(stale.job_id, None)


def _running_locked():
    return _RUNNING is not None and _RUNNING.state == "running"


def _size(path):
    try:
        return path.stat().st_size
    except OSError:
        return 0


def _count(value):
    return value if type(value) is int and value > 0 else 0


def _budgets(store, verify_media):
    """Byte and record budgets that cover the whole store, read from one locked state.

    Database files and cold archives are always read; managed media objects
    only when they are verified. Damaged rows are not rejected here: sizing
    tolerates them and the scrub itself reports them."""
    media, cold_bytes, records = {}, 0, 0
    with store._locked(exclusive=False):
        store._reload_locked()
        state = store._state
        records = sum(len(rows) for rows in state.values() if isinstance(rows, dict))
        for envelope in state.get("media_locations", {}).values():
            row = envelope.get("data") if isinstance(envelope, dict) else None
            if isinstance(row, dict) and isinstance(row.get("sha256"), str):
                media[row["sha256"]] = max(media.get(row["sha256"], 0), _count(row.get("size_bytes")))
        for envelope in state.get("retention_segments", {}).values():
            data = envelope.get("data") if isinstance(envelope, dict) else None
            if not isinstance(data, dict):
                continue
            files = data.get("files") if isinstance(data.get("files"), list) else []
            cold_bytes += sum(_count(entry.get("size_bytes")) for entry in files if isinstance(entry, dict))
            counts = data.get("counts") if isinstance(data.get("counts"), dict) else {}
            records += sum(_count(value) for value in counts.values())
        database = sum(_size(path) for path in (store.journal, store.meta_file, store.checkpoint_file))
    total = database + cold_bytes + (sum(media.values()) if verify_media else 0)
    max_bytes = max(1, min(MAX_BYTES, total + max(total // 10, _MARGIN_BYTES)))
    max_records = max(1, min(MAX_RECORDS, records + max(records // 10, _MARGIN_RECORDS)))
    return max_bytes, max_records


# The service may import this package under an isolated alias
# (tools/radios_database_setup.backend_import); the child imports the same
# source directory under its real name.
_PACKAGE_DIR = Path(__file__).resolve().parent
_SOURCE_ROOT = _PACKAGE_DIR.parent
_SCAN_TIMEOUT = 4 * 3600


class _ScanError(RuntimeError):
    """The separate scrub process ended without a report."""


def _scan(store, job):
    """Scrub in a separate process with the package's own CLI.

    The replayed copy of the database, its memory and its CPU time stay out of
    the live service: no interpreter lock stall for other requests, and the
    memory returns when the child exits. The child reads the database files
    under the same shared store lock (writers wait while it reads them)."""
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONPATH=str(_SOURCE_ROOT))
    command = [sys.executable, "-m", _PACKAGE_DIR.name + ".cli", "--db", str(store.journal), "integrity",
               "--max-records", str(job.max_records), "--max-bytes", str(job.max_bytes)]
    if not job.verify_media:
        command.append("--skip-media")
    completed = subprocess.run(command, capture_output=True, text=True, env=environment,
                               timeout=_SCAN_TIMEOUT, check=False)
    # 0: sound; 2: the scrub ran and reports problems or stopped incomplete.
    if completed.returncode in (0, 2):
        return json.loads(completed.stdout)
    lines = completed.stderr.strip().splitlines()
    detail = lines[-1].removeprefix("sdbase: ") if lines else ""
    raise _ScanError(detail or f"denetim süreci {completed.returncode} koduyla sonlandı")


def _run(job, store):
    try:
        report = _scan(store, job)
    except subprocess.TimeoutExpired:
        job.finish("failed", "Tam denetim tamamlanamadı: süre aşıldı.")
    except _ScanError as exc:
        job.finish("failed", f"Tam denetim tamamlanamadı: {exc}")
    except MemoryError:
        job.finish("failed", "Tam denetim tamamlanamadı: bellek yetmedi.")
    except Exception as exc:  # noqa: BLE001 - the job must always reach a terminal state
        job.finish("failed", f"Tam denetim tamamlanamadı: {type(exc).__name__}: {exc}")
    else:
        job.finish("done", "Tam denetim tamamlandı.", report)


def integrity_start(store, principal, station_id, verify_media=True):
    """Start the whole-store scrub in the background; poll it with ``integrity_job``."""
    global _RUNNING
    require_engine_manager(store, principal)
    sid = api._id(station_id, "station_id")
    if type(verify_media) is not bool:
        raise StoreError("verify_media must be true or false")
    with _LOCK:
        if _running_locked():
            raise StoreError("Bir bütünlük denetimi zaten çalışıyor.")
    max_bytes, max_records = _budgets(store, verify_media)
    job = _Job(sid, verify_media, max_bytes, max_records)
    with _LOCK:
        if _running_locked():
            raise StoreError("Bir bütünlük denetimi zaten çalışıyor.")
        _JOBS[job.job_id] = job
        _RUNNING = job
        _prune_locked()
    started = job.view()
    try:
        threading.Thread(target=_run, args=(job, store), name="radios-integrity-" + job.job_id[:8],
                         daemon=True).start()
    except RuntimeError as exc:
        job.finish("failed", "Tam denetim başlatılamadı.")
        raise StoreError("Tam denetim başlatılamadı.") from exc
    return started


def integrity_job(store, principal, station_id, job_id):
    """The state of one full check; its scrub report once it is done."""
    require_engine_manager(store, principal)
    api._id(station_id, "station_id")
    key = api._id(job_id, "job_id")
    with _LOCK:
        job = _JOBS.get(key)
    if job is None:
        raise StoreError("Bütünlük denetimi bulunamadı.")
    return job.view()
