"""Read-only storage and evidence coverage measurements for operators.

This measures committed metadata, journal/checkpoint footprint, disk space and
optional audit archive coverage.  It cannot measure audio output or promise
that a production workload meets latency targets.  Thresholds are installation
inputs; no universal hardware limit is encoded here.
"""

from __future__ import annotations

import os
from pathlib import Path
import stat
from typing import Any, Sequence

from .ops_archive import ArchiveError, verify_archive_chain
from .storage import Store, StoreError


_EVIDENCE = ("_audit", "playout_events", "output_observations",
             "diagnostic_events", "recording_events", "asrun_report_runs",
             "live_override_reports")


def _limit(value: int | None, name: str) -> int | None:
    if value is not None and (type(value) is not int or value < 0):
        raise StoreError(f"{name} must be a nonnegative whole number")
    return value


def _file_info(path: Path, *, optional: bool = False) -> dict:
    try:
        if path.is_symlink():
            return {"bytes": None, "status": "unsafe_type"}
        info = os.stat(path, follow_symlinks=False)
        if not stat.S_ISREG(info.st_mode):
            return {"bytes": None, "status": "unsafe_type"}
        return {"bytes": info.st_size, "status": "present"}
    except FileNotFoundError:
        return {"bytes": 0 if optional else None,
                "status": "absent_optional" if optional else "absent"}
    except OSError:
        return {"bytes": None, "status": "unreadable"}


def inspect_store_health(store: Store, *,
                         archives: Sequence[str | Path] | None = None,
                         max_audit_scan_rows: int = 100_000,
                         min_free_bytes: int | None = None,
                         critical_free_bytes: int | None = None,
                         max_journal_bytes: int | None = None,
                         max_archive_backlog_commits: int | None = None) -> dict:
    """Return observed capacity/evidence health without changing the database.

    ``archives`` must list contiguous segments beginning at sequence 1.  All
    supplied files are verified before coverage is credited.  An omitted
    threshold yields a raw measurement, not an implicit healthy verdict.
    """
    if type(max_audit_scan_rows) is not int or not 1 <= max_audit_scan_rows <= 1_000_000:
        raise StoreError("max_audit_scan_rows must be 1..1000000")
    min_free_bytes = _limit(min_free_bytes, "min_free_bytes")
    critical_free_bytes = _limit(critical_free_bytes, "critical_free_bytes")
    max_journal_bytes = _limit(max_journal_bytes, "max_journal_bytes")
    max_archive_backlog_commits = _limit(max_archive_backlog_commits,
                                         "max_archive_backlog_commits")
    if min_free_bytes is not None and critical_free_bytes is not None and \
            critical_free_bytes > min_free_bytes:
        raise StoreError("critical_free_bytes cannot exceed min_free_bytes")
    if archives is not None and (isinstance(archives, (str, bytes)) or
                                 not isinstance(archives, Sequence)):
        raise StoreError("archives must be an ordered path list")

    with store._locked(exclusive=False):
        store._reload_locked()
        meta = store._meta()
        sequence = store._sequence
        counts = {name: len(rows) for name, rows in store._state.items()}
        audit = store._state.get("_audit", {})
        cold_ranges = sorted((row["data"]["first_audit_sequence"], row["data"]["last_audit_sequence"])
                             for row in store._state.get("retention_segments", {}).values()
                             if row["data"]["first_audit_sequence"])
        cold_through = 0
        cold_ranges_valid = True
        for first, last in cold_ranges:
            if first != cold_through + 1 or last < first:
                cold_ranges_valid = False
            cold_through = last
        audit_count = len(audit)
        audit_scan_complete = audit_count <= max_audit_scan_rows
        audit_sequence_valid: bool | None = None
        if audit_scan_complete:
            numbers = {envelope["data"].get("sequence") for envelope in audit.values()}
            audit_sequence_valid = (len(numbers) == audit_count and
                                    all(type(value) is int and 1 <= value <= sequence
                                        for value in numbers) and
                                    (min(numbers) == cold_through + 1 and max(numbers) == sequence
                                     if numbers else sequence == cold_through))
        files = {
            "journal": _file_info(store.journal),
            "metadata": _file_info(store.meta_file),
            "checkpoint": _file_info(store.checkpoint_file, optional=True),
        }
        managed = store._state.get("media_locations", {})
        addresses = {(row["data"].get("sha256"), row["data"].get("size_bytes"))
                     for row in managed.values()
                     if isinstance(row.get("data"), dict)}
        expected_media_bytes = sum(size for _digest, size in addresses
                                   if type(size) is int and size >= 0)
        media_count = len(addresses)
        total_records = sum(counts.values())
        tombstones = sum(len(keys) for keys in store._tombstones.values())
        retired_unique_keys = sum(len(values) for indexes in store._retired_unique_keys.values()
                                  for values in indexes.values())

    try:
        filesystem = os.statvfs(store.root)
        free_bytes = filesystem.f_bavail * filesystem.f_frsize
        total_bytes = filesystem.f_blocks * filesystem.f_frsize
    except OSError:
        free_bytes = total_bytes = None

    issues: list[dict[str, Any]] = []

    def issue(code: str, severity: str, detail: str) -> None:
        issues.append({"code": code, "severity": severity, "detail": detail})

    if not cold_ranges_valid:
        issue("cold_audit_sequence_gap", "critical", "cold audit registry has a sequence gap or overlap")
    if audit_count + cold_through != sequence:
        issue("audit_count_mismatch", "critical",
              "hot plus archived audit row count differs from committed sequence")
    if audit_sequence_valid is False:
        issue("audit_sequence_gap", "critical",
              "audit sequence has a gap, duplicate or invalid value")
    elif not audit_scan_complete:
        issue("audit_scan_incomplete", "unknown",
              "audit row count exceeds configured scan budget")
    for name, entry in files.items():
        if entry["status"] not in {"present", "absent_optional"}:
            issue(f"{name}_file_{entry['status']}", "critical",
                  f"{name} file is absent, unreadable or unsafe")
    if free_bytes is None:
        issue("filesystem_probe_failed", "unknown", "filesystem free space is unavailable")
    elif critical_free_bytes is not None and free_bytes < critical_free_bytes:
        issue("free_space_critical", "critical", "filesystem free bytes below configured limit")
    elif min_free_bytes is not None and free_bytes < min_free_bytes:
        issue("free_space_warning", "warning", "filesystem free bytes below configured limit")
    journal_bytes = files["journal"]["bytes"]
    if max_journal_bytes is not None and journal_bytes is not None and \
            journal_bytes > max_journal_bytes:
        issue("journal_size_warning", "warning", "journal bytes above configured limit")

    archive_coverage = None
    if archives is not None:
        if not archives:
            archive_coverage = {"segments": 0, "last_sequence": 0,
                                "backlog_commits": sequence, "verified": False}
            issue("archive_absent", "warning", "no archive segment was supplied")
        else:
            try:
                verified = verify_archive_chain(list(archives))
                if verified["store_id"] != meta.get("store_id") or \
                        verified["last_sequence"] > sequence:
                    raise ArchiveError("archive belongs to another store or future sequence")
                backlog = sequence - verified["last_sequence"]
                archive_coverage = {"segments": verified["segments"],
                                    "last_sequence": verified["last_sequence"],
                                    "backlog_commits": backlog, "verified": True,
                                    "manifest_sha256": verified["manifest_sha256"]}
                if max_archive_backlog_commits is not None and \
                        backlog > max_archive_backlog_commits:
                    issue("archive_backlog_warning", "warning",
                          "unarchived commits exceed configured limit")
            except (ArchiveError, OSError, ValueError) as exc:
                archive_coverage = {"verified": False, "error": str(exc)}
                issue("archive_invalid", "critical", "supplied archive chain failed verification")

    severity = {entry["severity"] for entry in issues}
    status = ("critical" if "critical" in severity else
              "warning" if "warning" in severity else
              "unknown" if "unknown" in severity else "observed")
    return {
        "status": status, "store_id": meta.get("store_id"),
        "schema_version": meta.get("schema_version", 1),
        "sequence": sequence, "audit_rows": audit_count,
        "audit_archived_through": cold_through,
        "audit_sequence_valid": audit_sequence_valid,
        "audit_scan_complete": audit_scan_complete,
        "total_records": total_records, "tombstones": tombstones,
        "retired_unique_keys": retired_unique_keys,
        "evidence_rows": {name: counts.get(name, 0) for name in _EVIDENCE},
        "files": files, "database_bytes": (
            sum(entry["bytes"] for entry in files.values())
            if all(entry["bytes"] is not None for entry in files.values()) else None),
        "filesystem_free_bytes": free_bytes,
        "filesystem_total_bytes": total_bytes,
        "managed_media_objects_expected": media_count,
        "managed_media_bytes_expected": expected_media_bytes,
        "archive_coverage": archive_coverage,
        "thresholds": {"min_free_bytes": min_free_bytes,
                       "critical_free_bytes": critical_free_bytes,
                       "max_journal_bytes": max_journal_bytes,
                       "max_archive_backlog_commits": max_archive_backlog_commits,
                       "max_audit_scan_rows": max_audit_scan_rows},
        "issues": issues,
    }
