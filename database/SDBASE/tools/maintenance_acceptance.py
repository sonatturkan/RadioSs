#!/usr/bin/env python3
"""Real writes/crashes around online checkpoint publication, in disposable stores.

No production database path is accepted. Temporary fixtures and child process
artifacts are removed automatically; an optional report contains no credentials.
"""

import argparse
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from threading import Event
from time import monotonic
from unittest.mock import patch
from uuid import uuid4

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))


def put_item(store, title, key=None):
    current = store.get("items", key) if key else None
    return store.put("items", key or str(uuid4()),
                     {"kind": "music", "title": title, "lifecycle": "draft"},
                     expected_revision=current["_rev"] if current else None)


def worker(config):
    from sdbase.schema import open_radio_store
    from sdbase import online_maintenance as maintenance
    values = json.loads(Path(config).read_bytes())
    store = open_radio_store(values["db"])
    if values["kind"] == "write":
        records = [put_item(store, f"concurrent {number}") for number in range(4)]
        put_item(store, "Updated while preparing", values["update_id"])
        print(json.dumps({"records": records, "sequence": store.sequence}))
        return
    def crash(phase):
        if phase == "prepared":
            put_item(open_radio_store(values["db"]), "durable tail", values["tail_id"])
        if phase == values["phase"]:
            os._exit(71)
    maintenance._after_publish = crash
    store.checkpoint_online()
    raise AssertionError("crash boundary not reached")


def run():
    from sdbase import online_maintenance as maintenance
    from sdbase import wal_archive as wal
    from sdbase.backup import create_backup
    from sdbase.identity import bootstrap, authenticate
    from sdbase.pitr import recover_point_in_time
    from sdbase.replication import create_standby, catch_up, promote_standby
    from sdbase.retention import preview_retention, apply_retention, query_cold_history
    from sdbase.schema import open_radio_store
    from sdbase.storage import StoreError, Corruption, _atomic_json

    started, checks, workers = monotonic(), [], 0
    def check(name, condition=True):
        if not condition:
            raise AssertionError(name)
        checks.append({"name": name, "passed": True})
    def denied(name, action, error=StoreError):
        try:
            action()
        except error:
            check(name)
        else:
            raise AssertionError(name + " unexpectedly succeeded")

    with tempfile.TemporaryDirectory(prefix="sdbase-maintenance-acceptance-") as directory:
        root = Path(directory)
        def child(values):
            nonlocal workers
            workers += 1
            config = root / f"worker-{workers}.json"
            _atomic_json(config, values)
            return subprocess.run([sys.executable, str(Path(__file__).resolve()),
                                   "--worker", str(config)], capture_output=True, text=True,
                                  env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"), timeout=30)
        store = open_radio_store(root / "primary" / "RadioS.sdb")
        identity = bootstrap(store, "acceptance", "M", "Online maintenance")
        original = put_item(store, "Before capture")
        wal.enable_wal_archive(store)
        create_backup(store, root / "base", checkpoint=False)
        create_standby(store, root / "standby")
        standby = open_radio_store(root / "standby" / "RadioS.sdb")
        reader = open_radio_store(store.journal, read_only=True)
        captured_sequence = store.sequence
        writer = {}
        with store.read_snapshot(["items"]) as view:
            def writes(phase):
                if phase == "prepared":
                    process = child({"kind": "write", "db": str(store.journal),
                                     "update_id": original["_id"]})
                    check("independent writer commits while checkpoint is prepared",
                          process.returncode == 0)
                    writer.update(json.loads(process.stdout))
            with patch.object(maintenance, "_after_publish", side_effect=writes):
                result = store.checkpoint_online()
            check("published image has original captured transaction sequence",
                  result["status"] == "published" and result["checkpoint_sequence"] == captured_sequence)
            check("all five concurrent transactions survive in journal suffix",
                  result["retained_transactions"] == 5 and result["current_sequence"] == writer["sequence"])
            check("open immutable read view remains at old title and sequence",
                  view.sequence == captured_sequence and view.get("items", original["_id"])["title"] == "Before capture")
        latest = open_radio_store(store.journal)
        check("restart reads updated record and all new records",
              latest.get("items", original["_id"])["title"] == "Updated while preparing" and
              all(latest.get("items", row["_id"]) == row for row in writer["records"]))
        check("preexisting readonly handle notices new checkpoint and preserves tail",
              reader.sequence == writer["sequence"] and len(reader.scan("items")) == 5)
        checkpoint = json.loads(latest.checkpoint_file.read_bytes())
        import hashlib
        from sdbase.storage import _json_bytes
        check("published checkpoint has valid payload checksum",
              hashlib.sha256(_json_bytes(checkpoint["payload"])).hexdigest() == checkpoint["sha256"])
        check("online maintenance does not allocate a semantic transaction",
              store.sequence == writer["sequence"])
        check("published work reports lock/prepare timing and shortened journal",
              result["journal_bytes_after"] < result["journal_bytes_before"] and
              all(number >= 0 for number in result["timings_ms"].values()))
        check("WAL remains sealed at latest concurrent commit",
              wal.archive_status(latest)["last_sequence"] == writer["sequence"])
        catch_up(latest, standby)
        check("standby consumes WAL across online checkpoint boundary",
              standby.sequence == latest.sequence and standby.scan("items") == latest.scan("items"))
        recovered = recover_point_in_time(root / "base", wal.archive_root(latest), root / "recovered",
                                           target_sequence=captured_sequence + 2)
        pitr = open_radio_store(root / "recovered" / "RadioS.sdb")
        check("PITR restores precise transaction before remaining tail commits",
              recovered["recovered_sequence"] == captured_sequence + 2 and
              pitr.get("items", original["_id"])["title"] == "Before capture" and
              pitr.get("items", writer["records"][1]["_id"]) is not None and
              pitr.get("items", writer["records"][2]["_id"]) is None)
        result = latest.checkpoint_online()
        check("second online checkpoint retires entire already captured suffix",
              result["status"] == "published" and result["retained_transactions"] == 0 and
              latest.journal.stat().st_size == 0)

        for boundary in ("prepared", "checkpoint_published", "journal_published"):
            crashing = open_radio_store(root / boundary / "RadioS.sdb")
            before = put_item(crashing, "before crash")
            crashing.checkpoint()
            tail_id = str(uuid4())
            process = child({"kind": "crash", "db": str(crashing.journal),
                             "phase": boundary, "tail_id": tail_id})
            check(boundary + ": actual process exits at requested boundary", process.returncode == 71)
            reopened = open_radio_store(crashing.journal)
            check(boundary + ": restart retains checkpoint and durable tail",
                  reopened.get("items", before["_id"]) is not None and
                  reopened.get("items", tail_id)["title"] == "durable tail" and reopened.sequence == 2)
            check(boundary + ": existing handle reloads despite metadata publication not finishing",
                  crashing.get("items", tail_id)["title"] == "durable tail")
            inspect = maintenance.recover_online_jobs(reopened)
            check(boundary + ": abandoned owned job is inspectable", len(inspect["jobs"]) == 1)
            cleanup = maintenance.recover_online_jobs(reopened, cleanup=True)
            check(boundary + ": cleanup changes no database sequence or data",
                  cleanup["jobs"][0]["removed"] and reopened.sequence == 2 and
                  maintenance.recover_online_jobs(reopened)["jobs"] == [])
            reopened.checkpoint_online()

        for budget, reason in (({"max_tail_bytes": 1}, "tail_byte_budget"),
                               ({"max_tail_frames": 1}, "tail_frame_budget")):
            bounded = open_radio_store(root / reason / "RadioS.sdb")
            put_item(bounded, "initial")
            bounded.checkpoint()
            checkpoint_before = bounded.checkpoint_file.read_bytes()
            def add_tail(phase):
                if phase == "prepared":
                    other = open_radio_store(bounded.journal)
                    put_item(other, "one")
                    put_item(other, "two")
            with patch.object(maintenance, "_after_publish", side_effect=add_tail):
                deferred = bounded.checkpoint_online(**budget)
            check(reason + ": budget safely defers without replacing checkpoint",
                  deferred["status"] == "deferred" and deferred["reason"] == reason and
                  bounded.checkpoint_file.read_bytes() == checkpoint_before)
            check(reason + ": all committed records survive deferred work",
                  len(open_radio_store(bounded.journal).scan("items")) == 3)

        retired = open_radio_store(root / "retention" / "RadioS.sdb")
        credentials = bootstrap(retired, "retention", "R", "Retention")
        put_item(retired, "keep item")
        retired.checkpoint()
        principal = authenticate(retired, credentials["api_token"])
        old_audit_ids = {row["_id"] for row in retired.scan("_audit")}
        def compact(phase):
            if phase == "prepared":
                preview = preview_retention(retired, principal, keep_days={"_audit": 1},
                    as_of=datetime.now(timezone.utc) + timedelta(days=3))
                apply_retention(retired, principal, preview)
        with patch.object(maintenance, "_after_publish", side_effect=compact):
            deferred = retired.checkpoint_online()
        check("concurrent retention checkpoint supersedes prepared image",
              deferred["status"] == "deferred" and deferred["reason"] == "checkpoint_replaced")
        check("obsolete online image never resurrects retired audit records",
              not old_audit_ids.intersection(row["_id"] for row in retired.scan("_audit")))
        cold = query_cold_history(retired, principal, "_audit")
        check("retired audit is still readable from verified cold history",
              old_audit_ids.issubset({row["_id"] for row in cold["records"]}))
        retired.checkpoint_online()

        def overlap(phase):
            if phase == "prepared":
                denied("simultaneous maintenance job is rejected without blocking writers",
                       lambda: open_radio_store(retired.journal).checkpoint_online())
                put_item(open_radio_store(retired.journal), "writer during rejected job")
        with patch.object(maintenance, "_after_publish", side_effect=overlap):
            result = retired.checkpoint_online()
        check("first job still publishes concurrent write after rejecting another job",
              result["status"] == "published" and result["retained_transactions"] == 1)

        stages = maintenance._root(retired)
        foreign = stages / str(uuid4())
        foreign.mkdir()
        (foreign / "keep.txt").write_text("not an owned job")
        malformed = stages / str(uuid4())
        malformed.mkdir()
        _atomic_json(malformed / "job.json", {"format": maintenance._JOB_FORMAT,
            "job_id": malformed.name, "database": str(retired.journal), "phase": "prepared"})
        link = stages / str(uuid4())
        link.symlink_to(foreign, target_is_directory=True)
        maintenance.recover_online_jobs(retired, cleanup=True)
        check("recovery ignores foreign/malformed/symlink staging paths",
              (foreign / "keep.txt").exists() and malformed.exists() and link.is_symlink())
        denied("readonly handle cannot publish checkpoint",
               lambda: open_radio_store(retired.journal, read_only=True).checkpoint_online())
        denied("invalid tail budgets are rejected", lambda: retired.checkpoint_online(max_tail_frames=True))

        corrupt = open_radio_store(root / "corruption" / "RadioS.sdb")
        put_item(corrupt, "safe")
        corrupt.checkpoint()
        checkpoint_before = corrupt.checkpoint_file.read_bytes()
        def corrupt_tail(phase):
            if phase == "prepared":
                with corrupt.journal.open("ab") as stream:
                    stream.write(b"bad")
                    stream.flush()
                    os.fsync(stream.fileno())
        with patch.object(maintenance, "_after_publish", side_effect=corrupt_tail):
            denied("online publication rejects torn journal instead of repairing it",
                   corrupt.checkpoint_online, Corruption)
        check("rejected corrupt tail and prior checkpoint remain untouched",
              corrupt.journal.read_bytes() == b"bad" and corrupt.checkpoint_file.read_bytes() == checkpoint_before)

        stop = Event()
        loop = maintenance.run_online_maintenance(retired, stop, interval_seconds=.05,
                                                   journal_threshold_bytes=1)
        put_item(retired, "loop trigger")
        check("maintenance loop triggers a bounded checkpoint", next(loop)["status"] == "published")
        stop.set()
        check("maintenance loop stops when its owner requests stop", list(loop) == [])

        denied("standby cannot publish a primary checkpoint", standby.checkpoint_online)
        promote_standby(latest, standby, expected_source_sequence=latest.sequence,
                        expected_source_timeline_id=latest._meta()["timeline_id"])
        denied("fenced former primary cannot publish prepared maintenance", latest.checkpoint_online)

    from sdbase import __version__
    return {"format": "sdbase-online-maintenance-acceptance-v1", "version": __version__,
            "passed": True, "checks": len(checks), "workers": workers,
            "elapsed_seconds": round(monotonic() - started, 3),
            "scope": "same-host independent writes, actual process crashes, WAL/PITR/standby and retention",
            "results": checks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--worker", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        worker(args.worker)
        return
    result = run()
    encoded = json.dumps(result, indent=2, ensure_ascii=False)
    if args.report:
        args.report.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)


if __name__ == "__main__":
    main()
