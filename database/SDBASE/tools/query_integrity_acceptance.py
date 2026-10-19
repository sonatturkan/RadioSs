#!/usr/bin/env python3
"""Run isolated read-snapshot/query/scrub acceptance without a production path.

All databases, source audio and deliberately corrupted copies live in an
owned TemporaryDirectory and are removed on exit. No hardware or network
service is required. An optional JSON report contains no bearer credentials.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
import sys
import tempfile
from threading import Thread
import time
from uuid import uuid4
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from sdbase.catalog import NotFound, create_item, create_version
from sdbase.identity import Forbidden, Principal, bootstrap
from sdbase.integrity import inspect_integrity
from sdbase.media import object_path, stage_media
from sdbase.query import QueryBudgetExceeded
from sdbase.read_api import get_item, get_station_item
from sdbase.retention import DEFAULT_KEEP_DAYS, apply_retention, preview_retention
from sdbase.schema import open_radio_store
from sdbase.search import search_items
from sdbase.storage import Conflict, IndexSpec, Store, StoreError, _json_bytes


def _without_engine(row):
    return {key: value for key, value in row.items() if key not in {"_id", "_rev"}}


def _expect_error(action, exception):
    try:
        action()
    except exception:
        return True
    raise AssertionError(f"expected {exception.__name__}")


def _snapshot_checks(root, check):
    store = Store(root / "generic", indexes={"rows": {
        "tenant_n": IndexSpec(("tenant", "n")), "tenant": IndexSpec(("tenant",)),
        "flag": IndexSpec(("flag",))}})
    tx = store.begin()
    for number in range(100):
        tx.put("rows", f"{number:03d}", {"tenant": "A" if number % 2 == 0 else "B",
               "n": number, "flag": number % 3 == 0, "nested": {"v": [number]}})
    tx.commit()
    view = store.read_snapshot(["rows"])
    for descending in (False, True):
        arguments = dict(equal={"tenant": "A"}, ranges={"n": {
            "minimum": 10, "maximum": 30, "include_maximum": False}},
            order_by=("n",), limit=3, max_scan=4, descending=descending)
        result = view.query("rows", **arguments)
        values = [row["n"] for row in result["rows"]]
        while result["next_cursor"]:
            result = view.query("rows", cursor=result["next_cursor"], **arguments)
            values.extend(row["n"] for row in result["rows"])
            assert result["explain"]["rows_examined"] <= 4
        check(f"indexed range/keyset descending={descending}", values == sorted(range(10, 30, 2), reverse=descending))
    returned = view.get("rows", "000"); returned["nested"]["v"][0] = 900
    check("nested result mutation isolated", view.get("rows", "000")["nested"]["v"] == [0])
    store.put("rows", "000", {"tenant": "B", "n": 1000, "flag": False}, expected_revision=1)
    store.delete("rows", "002", expected_revision=1); store.checkpoint()
    check("old rows/index survive update delete checkpoint", view.get("rows", "000")["nested"]["v"] == [0]
          and view.get("rows", "002")["n"] == 2 and len(view.lookup("rows", "tenant", "A")) == 50)
    check("stale snapshot CAS rejected", _expect_error(lambda: store.put("rows", "000", _without_engine(returned),
          expected_revision=returned["_rev"]), Conflict))
    result = view.query("rows", equal={"tenant": "A", "flag": True}, limit=100)
    check("AND index intersection", result["explain"]["access"] == "index_intersection"
          and [row["n"] for row in result["rows"]] == list(range(0, 100, 6)))
    check("unindexed implicit scan rejected", _expect_error(lambda: view.query("rows", ranges={"n": {"minimum": 10}}), QueryBudgetExceeded))
    check("explicit work budgets enforced", _expect_error(lambda: view.query("rows", equal={"tenant": "A"},
          ranges={"n": {"minimum": 10}}, max_scan=4, max_index_keys=5), QueryBudgetExceeded))
    cursor = view.query("rows", equal={"tenant": "A"}, limit=5)["next_cursor"]
    with store.read_snapshot(["rows"]) as current:
        check("new collection generation fences cursor", _expect_error(lambda: current.query("rows", equal={"tenant": "A"},
              limit=5, cursor=cursor), Conflict))
    check("uncaptured collection rejected", _expect_error(lambda: view.get("uncaptured", "x"), StoreError))
    view.close()
    check("closed view rejected", _expect_error(lambda: view.scan("rows"), StoreError))

    nullable = Store(root / "nullable", indexes={"episodes": {"feed": IndexSpec(("feed",)),
                     "feed_guid": IndexSpec(("feed", "guid"))}, "entries": {"version": IndexSpec(("version",)),
                     "version_ordinal": IndexSpec(("version", "ordinal"))}})
    tx = nullable.begin()
    tx.put("episodes", "no-guid", {"feed": "F", "guid": None}); tx.put("episodes", "with-guid", {"feed": "F", "guid": "G"})
    tx.put("entries", "removed", {"version": "V", "ordinal": None}); tx.put("entries", "active", {"version": "V", "ordinal": 1})
    tx.commit()
    with nullable.read_snapshot(["episodes", "entries"]) as captured:
        check("nullable composite prefix preserves guidless episode", {row["_id"] for row in captured.query("episodes", equal={"feed": "F"})["rows"]} == {"no-guid", "with-guid"})
        check("nullable composite prefix preserves removed entry", {row["_id"] for row in captured.query("entries", equal={"version": "V"})["rows"]} == {"removed", "active"})
        check("nonnull range can safely select sparse composite index", [row["_id"] for row in captured.query("episodes", equal={"feed": "F"}, ranges={"guid": {"minimum": "A"}})["rows"]] == ["with-guid"])

    pairs = Store(root / "pairs", indexes={"pairs": {"group": IndexSpec(("group",))}})
    tx = pairs.begin(); tx.put("pairs", "left", {"group": "x", "value": 0}); tx.put("pairs", "right", {"group": "x", "value": 0}); tx.commit()
    writer_store = Store(pairs.journal, indexes=pairs.index_specs)
    failures = []
    def writer():
        try:
            for number in range(1, 81):
                tx = writer_store.begin()
                for key in ("left", "right"):
                    row = writer_store.get("pairs", key)
                    tx.put("pairs", key, {"group": "x", "value": number}, expected_revision=row["_rev"])
                tx.commit()
                if number % 20 == 0: writer_store.checkpoint()
        except Exception as exc:
            failures.append(str(exc))
    thread = Thread(target=writer); thread.start(); views = []
    try:
        for _ in range(120):
            captured = pairs.read_snapshot(["pairs"]); rows = captured.lookup("pairs", "group", "x")
            assert rows[0]["value"] == rows[1]["value"]
            views.append((captured, rows[0]["value"]))
        thread.join(30)
        assert not thread.is_alive() and not failures
        check("120 views coherent through 80 commits/four checkpoints", all(
              [row["value"] for row in captured.scan("pairs")] == [value, value] for captured, value in views))
    finally:
        thread.join(30)
        for captured, _ in views: captured.close()


def _domain_and_integrity_checks(root, check):
    store = open_radio_store(root / "radio")
    account = bootstrap(store, "operator", "R", "Radio")
    principal = Principal(account["user_id"], "user"); station = account["station_id"]
    item = create_item(store, principal, station, kind="music", title="Işık")
    version = create_version(store, principal, station, item["_id"], label="Original")
    source = root / "source.wav"
    with wave.open(str(source), "wb") as audio:
        audio.setnchannels(1); audio.setsampwidth(2); audio.setframerate(8000); audio.writeframes(b"\x00\x00" * 80)
    media = stage_media(store, principal, station, version["_id"], source, [root])
    other_station = str(uuid4())
    store.put("stations", other_station, {"code": "OTHER", "name": "Other", "timezone": "UTC", "status": "active"})
    check("detail read uses station association", get_item(store, principal, station, item["_id"])["item"]["title"] == "Işık")
    check("cross-station association hidden", _expect_error(lambda: get_station_item(store, principal, other_station, item["_id"]), NotFound))
    check("endpoint credential denied detail read", _expect_error(lambda: get_item(store,
          Principal(account["user_id"], "endpoint", account["endpoint_id"]), station, item["_id"]), Forbidden))
    captured = store.read_snapshot(["users", "permissions", "stations", "station_items", "items", "versions", "plans",
                "plan_revisions", "plan_entries", "categories", "people", "contributions", "media_files"])
    row = store.get("items", item["_id"]); body = _without_engine(row); body["title"] = "Yeni"
    store.put("items", item["_id"], body, expected_revision=row["_rev"])
    permission = next(row for row in store.lookup("permissions", "user", account["user_id"]) if row["permission"] == "view")
    body = _without_engine(permission); body.update(station_id=other_station, station_scope=other_station)
    store.put("permissions", permission["_id"], body, expected_revision=permission["_rev"])
    check("old permission and catalog view remain coherent", get_item(captured, principal, station, item["_id"])["item"]["title"] == "Işık"
          and search_items(captured, principal, station, title="ışık")["items"][0]["title"] == "Işık")
    check("new view honors revoked station grant", _expect_error(lambda: get_item(store, principal, station, item["_id"]), Forbidden))
    captured.close()
    audit = store.read_snapshot(["_audit"]); ids = [row["_id"] for row in audit.scan("_audit")]
    preview = preview_retention(store, principal, keep_days={key: 1 for key in DEFAULT_KEEP_DAYS},
                               as_of=datetime.now(timezone.utc) + timedelta(days=30))
    applied = apply_retention(store, principal, preview)
    check("old view survives real cold retention purge", applied["purged_records"] > 0
          and all(audit.get("_audit", key) for key in ids) and len(store.scan("_audit")) < len(ids))
    audit.close(); store.checkpoint()
    result = inspect_integrity(store)
    check("clean checkpoint media cold-history scrub", result["status"] == "ok" and result["complete"]
          and result["checks"]["managed_media"]["verified_objects"] == 1 and result["checks"]["cold_history"]["verified_segments"] == 1)
    check("one-byte budget yields incomplete", inspect_integrity(store, max_bytes=1)["status"] == "incomplete")
    check("record budget yields incomplete", inspect_integrity(store, max_records=1)["status"] == "incomplete")
    check("exact file-byte budget succeeds", inspect_integrity(store, max_bytes=result["work"]["bytes_read"])["status"] == "ok")
    check("disabled verification is never full healthy verdict", inspect_integrity(store, verify_media=False)["status"] == "incomplete")
    database = store.journal
    original = database.read_bytes(); database.write_bytes(original + b"\x00")
    try:
        check("partial tail detected without repair", inspect_integrity(store)["status"] == "invalid"
              and database.read_bytes() == original + b"\x00")
    finally: database.write_bytes(original)
    checkpoint = store.checkpoint_file; clean = checkpoint.read_bytes()
    value = json.loads(clean); value["payload"]["state"]["items"][item["_id"]]["data"]["title"] = "changed without checksum"
    checkpoint.write_bytes(_json_bytes(value))
    try: check("checkpoint checksum detects mutation", inspect_integrity(store)["status"] == "invalid")
    finally: checkpoint.write_bytes(clean)
    value = json.loads(clean); association = next(iter(value["payload"]["state"]["station_items"].values()))
    association["data"]["item_id"] = str(uuid4()); value["sha256"] = hashlib.sha256(_json_bytes(value["payload"])).hexdigest()
    checkpoint.write_bytes(_json_bytes(value))
    try: check("valid checksum cannot hide orphan reference", inspect_integrity(store)["status"] == "invalid")
    finally: checkpoint.write_bytes(clean)
    value = json.loads(clean); existing_user = next(iter(value["payload"]["state"]["users"].values()))
    value["payload"]["state"]["users"][str(uuid4())] = existing_user
    value["sha256"] = hashlib.sha256(_json_bytes(value["payload"])).hexdigest(); checkpoint.write_bytes(_json_bytes(value))
    try: check("valid checksum cannot hide duplicate unique key", inspect_integrity(store)["status"] == "invalid")
    finally: checkpoint.write_bytes(clean)
    managed = object_path(store, media["sha256"]); clean_audio = managed.read_bytes()
    managed.write_bytes(bytes([clean_audio[0] ^ 1]) + clean_audio[1:])
    try: check("managed media hash detects same-size corruption", inspect_integrity(store)["checks"]["managed_media"]["status"] == "failed")
    finally: managed.write_bytes(clean_audio)
    segment = next(iter(store.scan("retention_segments")))
    entry = next(entry for entry in segment["files"] if entry["path"].endswith("/records.jsonl"))
    cold = store.root / entry["path"]; clean_cold = cold.read_bytes(); cold.write_bytes(clean_cold.replace(b'"actor"', b'"actox"', 1))
    try: check("cold record file hash detects same-size corruption", inspect_integrity(store)["checks"]["cold_history"]["status"] == "failed")
    finally: cold.write_bytes(clean_cold)
    check("restoring original bytes yields clean scrub", inspect_integrity(store)["status"] == "ok")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, help="new JSON report file (existing files are preserved)")
    arguments = parser.parse_args()
    checks = []; start = time.monotonic()
    def check(name, passed):
        checks.append({"name": name, "passed": bool(passed)})
        if not passed: raise AssertionError(name)
    with tempfile.TemporaryDirectory(prefix="sdb-query-integrity-") as directory:
        root = Path(directory)
        _snapshot_checks(root, check)
        _domain_and_integrity_checks(root, check)
    report = {"format": "sdbase-query-integrity-acceptance-v1", "passed": all(row["passed"] for row in checks),
              "checks_passed": sum(row["passed"] for row in checks), "checks_total": len(checks),
              "elapsed_seconds": round(time.monotonic() - start, 3), "checks": checks,
              "scope": "isolated local temporary databases; two Store connections; no hardware or physical second host"}
    if arguments.report:
        with arguments.report.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2); stream.write("\n")
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    main()
