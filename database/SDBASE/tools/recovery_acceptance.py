#!/usr/bin/env python3
"""Exercise WAL/PITR crash recovery in disposable, same-host directories.

No production database path is accepted. Every source, backup, object, child
process and restored node belongs to one automatically removed temporary tree.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))


def _tree_digest(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


def run():
    from sdbase import wal_archive as wal
    from sdbase.backup import create_backup, verify_backup
    from sdbase.catalog import create_item, create_version
    from sdbase.identity import bootstrap, authenticate
    from sdbase.media import stage_media
    from sdbase.pitr import preview_recovery, recover_point_in_time
    from sdbase.retention import preview_retention, apply_retention, query_cold_history
    from sdbase.schema import open_radio_store
    from sdbase.storage import StoreError

    started, checks = time.monotonic(), []
    def check(name, condition=True):
        if not condition:
            raise AssertionError(name)
        checks.append({"name": name, "passed": True})
    def reject(name, action):
        try:
            action()
        except StoreError:
            check(name)
        else:
            raise AssertionError(name + " was accepted")

    with tempfile.TemporaryDirectory(prefix="sdbase-recovery-acceptance-") as directory:
        root = Path(directory)
        store = open_radio_store(root / "primary" / "RadioS.sdb")
        credentials = bootstrap(store, "acceptance", "R", "Recovery acceptance")
        principal = authenticate(store, credentials["api_token"])
        wal.enable_wal_archive(store)
        create_backup(store, root / "base", checkpoint=False)
        verify_backup(root / "base")
        check("verified base backup with WAL anchor")
        interrupted = open_radio_store(root / "enable-interrupted" / "RadioS.sdb")
        atomic_json = wal._atomic_json
        def attachment_failure(path, value):
            if path == interrupted.meta_file and "wal_archive" in value:
                raise OSError("acceptance fault after archive publication, before metadata attachment")
            return atomic_json(path, value)
        try:
            with patch("sdbase.wal_archive._atomic_json", side_effect=attachment_failure):
                wal.enable_wal_archive(interrupted)
        except OSError:
            pass
        check("interrupted enable preserves a published unattached archive",
              wal.archive_root(interrupted).is_dir() and "wal_archive" not in interrupted._meta())
        check("interrupted enable safely attaches its exact empty timeline",
              wal.enable_wal_archive(interrupted)["enabled"])
        item = create_item(store, principal, credentials["station_id"], kind="music", title="Before")
        version = create_version(store, principal, credentials["station_id"], item["_id"], label="Original")
        (root / "new.bin").write_bytes(b"managed media first introduced after the base backup")
        media = stage_media(store, principal, credentials["station_id"], version["_id"],
                            root / "new.bin", [root])
        media_sequence = store.sequence
        store.checkpoint()
        check("checkpoint truncates journal only after durable archive", not store.journal.stat().st_size and
              wal.archive_status(store)["last_sequence"] == media_sequence)
        current = store.get("items", item["_id"])
        store.put("items", item["_id"], {key: value for key, value in current.items()
            if key not in {"_id", "_rev"}} | {"title": "Later"}, expected_revision=current["_rev"])
        records = wal.read_archive_records(wal.archive_root(store))
        media_time = next(row["payload"]["effective_at"] for row in records
                          if row["payload"]["sequence"] == media_sequence)
        check("inclusive timestamp recovers its exact transaction", preview_recovery(
            root / "base", wal.archive_root(store), target_time=media_time)["target_sequence"] == media_sequence)
        check("exclusive timestamp stops before its exact transaction", preview_recovery(
            root / "base", wal.archive_root(store), target_time=media_time,
            inclusive=False)["target_sequence"] == media_sequence - 1)
        next_time = next(row["payload"]["effective_at"] for row in records
                         if row["payload"]["sequence"] == media_sequence + 1)
        midpoint = datetime.fromisoformat(media_time) + (datetime.fromisoformat(next_time) -
                                                        datetime.fromisoformat(media_time)) / 2
        check("timestamp between commits selects the previous committed state", preview_recovery(
            root / "base", wal.archive_root(store), target_time=midpoint.isoformat())["target_sequence"] == media_sequence)
        preview = preview_retention(store, principal, keep_days={"_audit": 1}, as_of=(
            datetime.now(timezone.utc) + timedelta(days=3)).isoformat())
        result = apply_retention(store, principal, preview)
        check("retention commits a full-state recovery barrier", result["purged_records"] > 0 and
              wal.read_archive_records(wal.archive_root(store))[-1]["payload"]["kind"] == "checkpoint")

        # Real child process termination after the original durable frame but
        # before the archiver can acknowledge it.
        before = store.sequence
        program = ("import os;from sdbase.schema import open_radio_store;"
                   "import sdbase.wal_archive as w;"
                   "w.archive_transaction_locked=lambda *a:os._exit(87);"
                   "s=open_radio_store(os.environ['SDB_ACCEPTANCE_DATABASE']);"
                   "s.put('recovery_acceptance','crash_commit',{'committed':True})")
        environment = os.environ | {"SDB_ACCEPTANCE_DATABASE": str(store.journal),
                                    "PYTHONDONTWRITEBYTECODE": "1",
                                    "PYTHONPATH": str(ROOT / "src")}
        child = subprocess.run([sys.executable, "-c", program], env=environment, timeout=30,
                               capture_output=True)
        store = open_radio_store(store.journal)
        check("process crash preserves the committed unacknowledged frame", child.returncode == 87 and
              store.sequence == before + 1 and wal.archive_status(store, verify=False)["unarchived_sequences"] == 1)
        store.checkpoint()
        check("checkpoint recovers a crash gap before retiring the journal",
              wal.archive_status(store)["last_sequence"] == store.sequence)

        atomic_json = wal._atomic_json
        def head_failure(path, value):
            if path.name == "head.json":
                raise OSError("acceptance fault before archive head publication")
            return atomic_json(path, value)
        try:
            with patch("sdbase.wal_archive._atomic_json", side_effect=head_failure):
                store.put("recovery_acceptance", "head_failure", {"committed": True})
        except wal.ArchiveAcknowledgementError as exc:
            check("archive acknowledgement failure reports its durable transaction",
                  exc.sequence == store.sequence and bool(exc.transaction_id))
        wal.capture_archive(store)
        check("sealed archive tail repairs a stale head",
              wal.archive_status(store)["last_sequence"] == store.sequence)
        import sdbase.storage as storage
        last_effective = datetime.fromisoformat(wal.archive_status(store)["last_effective_at"])
        class BackwardClock:
            @staticmethod
            def now(_timezone=None):
                return last_effective - timedelta(days=1)
        with patch.object(storage, "datetime", BackwardClock):
            store.put("recovery_acceptance", "backward_clock", {"committed": True})
        last_record = wal.read_archive_records(wal.archive_root(store))[-1]["payload"]
        check("backward source clock preserves monotonic effective commit time",
              datetime.fromisoformat(last_record["effective_at"]) > last_effective and
              datetime.fromisoformat(last_record["transaction"]["committed_at"]) < last_effective)

        # A second actual process crash at the checkpoint-only maintenance
        # commit point proves that retention is not missing from recovery.
        before = store.sequence
        program = ("import os;from datetime import datetime,timedelta,timezone;"
                   "from sdbase.schema import open_radio_store;"
                   "from sdbase.identity import authenticate;"
                   "from sdbase.retention import preview_retention,apply_retention;"
                   "import sdbase.wal_archive as w;"
                   "w.archive_checkpoint_locked=lambda *a:os._exit(88);"
                   "s=open_radio_store(os.environ['SDB_ACCEPTANCE_DATABASE']);"
                   "p=authenticate(s,os.environ['SDB_ACCEPTANCE_TOKEN']);"
                   "v=preview_retention(s,p,keep_days={'_audit':1},"
                   "as_of=(datetime.now(timezone.utc)+timedelta(days=5)).isoformat());"
                   "apply_retention(s,p,v)")
        child = subprocess.run([sys.executable, "-c", program], env=environment | {
            "SDB_ACCEPTANCE_TOKEN": credentials["api_token"]}, timeout=30, capture_output=True)
        store = open_radio_store(store.journal)
        check("maintenance process crash preserves its checkpoint commit", child.returncode == 88 and
              store.sequence == before + 1)
        wal.capture_archive(store)
        check("maintenance crash gap recreates its full-state recovery barrier",
              wal.read_archive_records(wal.archive_root(store))[-1]["payload"]["kind"] == "checkpoint")

        target_sequence = store.sequence
        for kind in ("truncated", "missing", "tampered", "missing-asset"):
            clone = root / ("damaged-" + kind)
            shutil.copytree(wal.archive_root(store), clone)
            path = sorted((clone / "records").glob("*.json"))[0]
            if kind == "truncated":
                path.write_bytes(path.read_bytes()[:-11])
            elif kind == "missing":
                path.unlink()
            elif kind == "tampered":
                damaged = json.loads(path.read_text())
                damaged["payload"]["transaction"]["actor"] = "tampered"
                path.write_text(json.dumps(damaged))
            else:
                next(path for path in (clone / "assets").rglob("*") if path.is_file()).unlink()
            reject(kind + " archive rejected", lambda: recover_point_in_time(
                root / "base", clone, root / ("invalid-" + kind), target_sequence=target_sequence))
            check(kind + " archive never publishes a recovery target", not (root / ("invalid-" + kind)).exists())
        reject("future sequence rejected", lambda: preview_recovery(root / "base", wal.archive_root(store),
                                                                    target_sequence=target_sequence + 1))
        reject("unproven future timestamp rejected", lambda: preview_recovery(root / "base", wal.archive_root(store),
            target_time=(datetime.now(timezone.utc) + timedelta(days=1)).isoformat()))
        orphan = wal.archive_root(store) / "records" / (".00000000000000009999.json." + "a" * 32 + ".partial")
        orphan.write_bytes(b"unsealed partial file left by a terminated archiver")
        wal.read_archive_records(wal.archive_root(store))
        check("unsealed crash orphan is excluded from committed recovery history")

        source_before = _tree_digest(store.root)
        restored = recover_point_in_time(root / "base", wal.archive_root(store), root / "restored-media",
                                         target_sequence=media_sequence)
        check("PITR leaves the source database and archive untouched", _tree_digest(store.root) == source_before)
        reopened = open_radio_store(root / "restored-media" / "RadioS.sdb")
        check("exact sequence restores the original content revision",
              restored["recovered_sequence"] == media_sequence and reopened.get("items", item["_id"])["title"] == "Before")
        check("new post-base media is recovered byte for byte", (root / "restored-media" /
              "RadioS.sdb.objects" / media["sha256"][:2] / media["sha256"]).read_bytes() == (root / "new.bin").read_bytes())
        reject("source credential revoked on the restored node", lambda: authenticate(reopened, credentials["api_token"]))
        check("restored node has independent identity and usable new administrator", restored["store_id"] !=
              restored["source_store_id"] and authenticate(reopened, restored["admin_token"]).user_id == restored["admin_user_id"])
        reject("existing restore target cannot be overwritten", lambda: recover_point_in_time(
            root / "base", wal.archive_root(store), root / "restored-media", target_sequence=media_sequence))

        shutil.copytree(wal.archive_root(store), root / "offsite-wal")
        shutil.rmtree(root / "primary")
        restored = recover_point_in_time(root / "base", root / "offsite-wal", root / "restored-cold",
                                         target_sequence=target_sequence)
        reopened = open_radio_store(root / "restored-cold" / "RadioS.sdb")
        principal = authenticate(reopened, restored["admin_token"])
        cold = query_cold_history(reopened, principal, "_audit")
        check("base and copied WAL recover after complete source loss", restored["recovered_sequence"] == target_sequence)
        check("checkpoint barriers recover verified cold history without the source", bool(cold["records"]) and
              restored["checkpoint_barriers"] == 2)
        check("durable unacknowledged transaction is included in exact recovery",
              reopened.get("recovery_acceptance", "crash_commit")["committed"])
    return {"format": "sdbase-recovery-acceptance-v1", "passed": len(checks), "failed": 0,
            "elapsed_seconds": round(time.monotonic() - started, 3), "checks": checks,
            "scope": "temporary directories and real child process crashes on one host; no second physical host"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="save the acceptance report as JSON")
    args = parser.parse_args()
    report = run()
    if args.output:
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
