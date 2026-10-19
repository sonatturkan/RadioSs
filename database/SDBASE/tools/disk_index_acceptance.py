#!/usr/bin/env python3
"""Isolated persistent disk-index acceptance; creates and removes its own DBs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from uuid import uuid4

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from sdbase.disk_index import DiskIndexError, inspect_disk_indexes, rebuild_disk_indexes, recover_disk_index_builds
from sdbase.query import QueryBudgetExceeded
from sdbase.storage import Conflict, IndexSpec, Store, StoreError, _atomic_json, _json_bytes


_SPECS = {"rows": {"tenant_n": IndexSpec(("tenant", "n")), "tenant": IndexSpec(("tenant",))}}


def _without_engine(row):
    return {key: value for key, value in row.items() if key not in {"_id", "_rev"}}


def _expect(action, exception):
    try: action()
    except exception: return True
    raise AssertionError(f"expected {exception.__name__}")


def _worker(directory):
    path = Path(directory)
    if not (path.parent / "owned-fixture.json").is_file():
        raise StoreError("worker requires an isolated owned fixture")
    store = Store(path, indexes=_SPECS)
    try:
        result = rebuild_disk_indexes(store, collections=["rows"], sort_chunk_rows=256)
        print(json.dumps({"status": "published", "result": result}))
    except (Conflict, DiskIndexError, StoreError) as exc:
        print(json.dumps({"status": "rejected", "detail": str(exc)}))


def _launch(store):
    env = dict(os.environ); env["PYTHONDONTWRITEBYTECODE"] = "1"
    process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--worker", str(store.root)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    cache = store.journal.with_name(store.journal.name + ".indexes")
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if any(candidate.is_file() for candidate in cache.glob(".build-*/build.json")):
            return process
        if process.poll() is not None:
            raise AssertionError(f"worker exited before build stage: {process.communicate()}")
        time.sleep(0.005)
    process.kill(); process.wait()
    raise AssertionError("worker did not establish build stage")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, help="new JSON report file")
    parser.add_argument("--worker", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        _worker(args.worker); return
    checks = []; started = time.monotonic()
    def check(name, condition):
        checks.append({"name": name, "passed": bool(condition)})
        if not condition: raise AssertionError(name)
    with tempfile.TemporaryDirectory(prefix="sdb-disk-index-acceptance-") as directory:
        root = Path(directory); (root / "owned-fixture.json").write_text(json.dumps({"id": str(uuid4())}))
        empty = Store(root / "empty", indexes=_SPECS)
        rebuild_disk_indexes(empty, collections=["rows"])
        empty_sequence = empty.sequence
        empty_status = inspect_disk_indexes(empty)
        check("new empty collection has a valid zero-row disk index",
              empty_status["status"] == "valid" and
              all(row["status"] == "valid" and row["indexed_rows"] == 0 for row in empty_status["indexes"]))
        reopened_empty = Store(empty.journal, indexes=_SPECS, read_only=True)
        check("empty index remains valid after readonly restart without a semantic write",
              inspect_disk_indexes(reopened_empty)["status"] == "valid" and reopened_empty.sequence == empty_sequence)
        store = Store(root / "main", indexes=_SPECS)
        for start in range(0, 30000, 1000):
            tx = store.begin()
            for number in range(start, start + 1000):
                tx.put("rows", f"{number:06d}", {"tenant": "A" if number % 2 == 0 else "B", "n": number})
            tx.commit()
        before = store.sequence
        build = rebuild_disk_indexes(store, collections=["rows"], sort_chunk_rows=256)
        check("bounded multi-pass build leaves authoritative sequence unchanged", store.sequence == before and build["indexed_rows"] == 60000)
        cache = Path(build["cache_root"]); live_manifest = (cache / "manifest.json").read_bytes()
        check("published binary offsets/typed records survive process restart", inspect_disk_indexes(Store(store.journal, indexes=_SPECS))["status"] == "valid")
        arguments = dict(equal={"tenant": "A"}, ranges={"n": {"minimum": 1000, "maximum": 1030, "include_maximum": False}},
                         order_by=("n",), limit=3, max_scan=4, max_index_keys=64, max_candidates=64)
        with store.read_snapshot(["rows"]) as view:
            for descending in (False, True):
                result = view.query("rows", descending=descending, **arguments)
                values = [row["n"] for row in result["rows"]]
                while result["next_cursor"]:
                    result = view.query("rows", descending=descending, cursor=result["next_cursor"], **arguments)
                    values.extend(row["n"] for row in result["rows"])
                check(f"disk range ordered keyset descending={descending}", values == sorted(range(1000, 1030, 2), reverse=descending))
            check("disk selected-record budget rejects oversized range", _expect(lambda: view.query("rows",
                  **(arguments | {"max_index_keys": 5})), QueryBudgetExceeded))
            check("candidate budget rejects oversized materialization", _expect(lambda: view.query("rows",
                  **(arguments | {"max_candidates": 2})), QueryBudgetExceeded))
            result = view.query("rows", **arguments)
            path = next(path for path in result["explain"]["paths"] if path.get("backend") == "disk")
            check("mmap/bisect avoids sorted RAM key-array build", result["explain"]["ordered_keys_built"] == 0
                  and path["disk_work"]["key_records_read"] < 100 and not view._ordered_indices)
            result = view.query("rows", equal={"tenant": "A"}, order_by=("n",), limit=2)
            check("prefix equality uses disk ordering", [row["n"] for row in result["rows"]] == [0, 2]
                  and any(path.get("backend") == "disk" for path in result["explain"]["paths"]))
            result = view.query("rows", equal={"tenant": "A", "n": 1000})
            check("complete equality retains O(1) hash lookup", [row["n"] for row in result["rows"]] == [1000])
            row = store.get("rows", "001000"); body = _without_engine(row); body["n"] = 100000
            store.put("rows", row["_id"], body, expected_revision=row["_rev"])
            check("pinned old view continues old disk generation", [row["n"] for row in view.query("rows", **arguments)["rows"]] == [1000, 1002, 1004])
        with store.read_snapshot(["rows"]) as current:
            result = current.query("rows", equal={"tenant": "A"}, ranges={"n": {"minimum": 100000}}, order_by=("n",))
            check("stale cache fallback cannot drop newly committed row", [row["n"] for row in result["rows"]] == [100000]
                  and not any(path.get("backend") == "disk" for path in result["explain"]["paths"]))
        check("inspection exposes stale collection generation", inspect_disk_indexes(store)["status"] == "stale_or_invalid")
        rebuild_disk_indexes(store, collections=["rows"], sort_chunk_rows=256)
        live_manifest = (cache / "manifest.json").read_bytes()
        check("row-budget failure preserves published manifest", _expect(lambda: rebuild_disk_indexes(store, collections=["rows"], max_rows=1), DiskIndexError)
              and (cache / "manifest.json").read_bytes() == live_manifest)
        check("byte-budget failure preserves published manifest", _expect(lambda: rebuild_disk_indexes(store, collections=["rows"], max_bytes=1), DiskIndexError)
              and (cache / "manifest.json").read_bytes() == live_manifest)
        payload = json.loads(live_manifest)["payload"]
        group = next(group for group in payload["indexes"] if group["index"] == "tenant_n")
        for kind in ("data", "offsets"):
            path = cache / group[kind]["path"]; original = path.read_bytes()
            path.write_bytes(bytes([original[0] ^ 1]) + original[1:])
            try:
                with store.read_snapshot(["rows"]) as view:
                    result = view.query("rows", equal={"tenant": "B"}, ranges={"n": {"minimum": 1001, "maximum": 1010}}, order_by=("n",))
                    check(f"damaged {kind} checksum uses complete RAM fallback", [row["n"] for row in result["rows"]] == [1001, 1003, 1005, 1007, 1009]
                          and not any(path.get("backend") == "disk" for path in result["explain"]["paths"]))
            finally: path.write_bytes(original)
        # Forge internally matching hashes for an unsorted cache: layout
        # verification must still reject it, rather than silently miss a range.
        keys_path = cache / group["data"]["path"]; offsets_path = cache / group["offsets"]["path"]
        clean_keys = keys_path.read_bytes(); clean_offsets = offsets_path.read_bytes()
        lines = clean_keys.splitlines(keepends=True)
        lines[0], lines[-1] = lines[-1], lines[0]
        forged_keys = b"".join(lines)
        import struct
        offset = 0; positions = []
        for line in lines:
            positions.append(offset); offset += len(line)
        positions.append(offset)
        forged_offsets = clean_offsets[:24] + b"".join(struct.pack(">Q", position) for position in positions)
        forged = json.loads(live_manifest)
        forged_group = next(row for row in forged["payload"]["indexes"] if row["index"] == "tenant_n")
        forged_group["data"]["sha256"] = hashlib.sha256(forged_keys).hexdigest()
        forged_group["offsets"]["sha256"] = hashlib.sha256(forged_offsets).hexdigest()
        forged["sha256"] = hashlib.sha256(_json_bytes(forged["payload"])).hexdigest()
        keys_path.write_bytes(forged_keys); offsets_path.write_bytes(forged_offsets)
        (cache / "manifest.json").write_bytes(_json_bytes(forged))
        try:
            with store.read_snapshot(["rows"]) as view:
                result = view.query("rows", equal={"tenant": "B"}, ranges={"n": {"minimum": 1001, "maximum": 1010}}, order_by=("n",))
                check("recomputed hashes cannot hide unsorted cache", [row["n"] for row in result["rows"]] == [1001, 1003, 1005, 1007, 1009]
                      and not any(path.get("backend") == "disk" for path in result["explain"]["paths"]))
        finally:
            keys_path.write_bytes(clean_keys); offsets_path.write_bytes(clean_offsets)
            (cache / "manifest.json").write_bytes(live_manifest)
        value = json.loads(live_manifest); value["payload"]["indexes"][0]["collection_generation"] += 1
        (cache / "manifest.json").write_text(json.dumps(value))
        try:
            with store.read_snapshot(["rows"]) as view:
                result = view.query("rows", equal={"tenant": "B"}, ranges={"n": {"minimum": 1001, "maximum": 1010}}, order_by=("n",))
                check("damaged manifest checksum uses complete RAM fallback", [row["n"] for row in result["rows"]] == [1001, 1003, 1005, 1007, 1009])
            check("recovery preserves complete artifacts when active manifest is invalid", not recover_disk_index_builds(store, cleanup=True)["removed"])
        finally: (cache / "manifest.json").write_bytes(live_manifest)
        reverse_specs = {"rows": {"tenant_n": IndexSpec(("n", "tenant")), "tenant": IndexSpec(("tenant",))}}
        changed_spec = Store(store.journal, indexes=reverse_specs)
        with changed_spec.read_snapshot(["rows"]) as view:
            result = view.query("rows", ranges={"n": {"minimum": 1001, "maximum": 1010}}, order_by=("n", "tenant"))
            check("changed index specification rejects old disk ordering", [row["n"] for row in result["rows"]] == list(range(1001, 1011))
                  and not any(path.get("backend") == "disk" for path in result["explain"]["paths"]))

        process = _launch(store)
        try:
            row = store.get("rows", "000000"); body = _without_engine(row); body["n"] = 200000
            store.put("rows", row["_id"], body, expected_revision=row["_rev"])
            output, error = process.communicate(timeout=30)
            result = json.loads(output)
            check("concurrent write rejects online stale publication", result["status"] == "rejected"
                  and "changed during build" in result["detail"] and (cache / "manifest.json").read_bytes() == live_manifest)
        finally:
            if process.poll() is None: process.kill(); process.wait()
        process = _launch(store)
        try:
            check("active build recovery inspection sees job lock", recover_disk_index_builds(store)["active_build"])
            check("active build cleanup is rejected", _expect(lambda: recover_disk_index_builds(store, cleanup=True), DiskIndexError))
            process.send_signal(signal.SIGKILL); process.wait(timeout=5)
            recover = recover_disk_index_builds(store)
            check("killed builder leaves discoverable owned stage", not recover["active_build"] and any(name.startswith(".build-") for name in recover["eligible"]))
            recover = recover_disk_index_builds(store, cleanup=True)
            check("killed-build cleanup preserves authoritative data/current manifest", any(name.startswith(".build-") for name in recover["removed"])
                  and (cache / "manifest.json").read_bytes() == live_manifest and store.get("rows", "000000")["n"] == 200000)
        finally:
            if process.poll() is None: process.kill(); process.wait()
        unowned = cache / "unowned"; unowned.mkdir(); (unowned / "keep").write_text("user-owned")
        recover_disk_index_builds(store, cleanup=True)
        check("recovery preserves unmarked user directories", (unowned / "keep").read_text() == "user-owned")
        meta = store._meta(); original_meta = store.meta_file.read_bytes(); meta["node_role"] = "standby"
        _atomic_json(store.meta_file, meta)
        try: check("standby node cannot publish cache", _expect(lambda: rebuild_disk_indexes(store, collections=["rows"]), StoreError))
        finally: store.meta_file.write_bytes(original_meta)

        sparse_specs = {"rows": {"tenant": IndexSpec(("tenant",)), "tenant_n": IndexSpec(("tenant", "n")),
                        "tenant_n_optional": IndexSpec(("tenant", "n", "optional"))}}
        sparse = Store(root / "sparse", indexes=sparse_specs)
        tx = sparse.begin()
        for identity, value in (("neg", -3), ("float", 1.0), ("num", 1), ("truth", True), ("fraction", 1.25), ("str", "1")):
            tx.put("rows", identity, {"tenant": "T", "n": value, "optional": None if identity == "str" else "S"})
        tx.commit(); rebuild_disk_indexes(sparse, collections=["rows"])
        with sparse.read_snapshot(["rows"]) as view:
            result = view.query("rows", equal={"tenant": "T"}, order_by=("n",))
            check("typed numeric/string keys and ID ties keep engine semantics", [row["_id"] for row in result["rows"]] == ["neg", "float", "num", "truth", "fraction", "str"])
            check("nullable suffix never removes matching row", len(result["rows"]) == 6
                  and any(path.get("reason") == "index omits rows with unconstrained null fields" for path in result["explain"]["paths"]))
            check("query opens bounded disk reader resources", bool(view._disk_readers))
        check("disk reader resources cleared on close", not view._disk_readers)
        with sparse.read_snapshot(["rows"]) as view:
            result = view.query("rows", equal={"tenant": "T"}, ranges={"n": {"minimum": 1,
                                "maximum": 1, "include_minimum": False, "include_maximum": False}},
                                order_by=("n",))
            check("exclusive equal bounds form an empty disk window", not result["rows"]
                  and any(path.get("backend") == "disk" for path in result["explain"]["paths"]))

        # Same file hashes, generations, key counts and index specifications
        # across two collections must not share semantic verification credit.
        alias_specs = {name: {"v": IndexSpec(("v",))} for name in ("a", "b")}
        aliases = Store(root / "aliases", indexes=alias_specs)
        tx = aliases.begin()
        for collection in alias_specs:
            for value in range(10, 71, 10):
                tx.put(collection, f"common-{value}", {"v": value})
            tx.put(collection, "extra", {"v": 0 if collection == "a" else 65})
        tx.commit(); alias_build = rebuild_disk_indexes(aliases)
        alias_manifest = Path(alias_build["cache_root"]) / "manifest.json"
        clean_manifest = alias_manifest.read_bytes()
        with aliases.read_snapshot() as view:
            result = view.query("a", ranges={"v": {"minimum": 40, "maximum": 40}}, order_by=("v",))
            check("cold validation checks the authoritative collection coverage", result["rows"][0]["v"] == 40
                  and any(path.get("disk_work", {}).get("verification_records") == 8 for path in result["explain"]["paths"]))
        with aliases.read_snapshot() as view:
            result = view.query("a", ranges={"v": {"minimum": 40, "maximum": 40}}, order_by=("v",))
            check("warm same-source validation reuses a bounded certificate", result["rows"][0]["v"] == 40
                  and any(path.get("backend") == "disk" and path["disk_work"]["verification_records"] == 0
                          for path in result["explain"]["paths"]))
        forged = json.loads(clean_manifest)
        a_group = next(group for group in forged["payload"]["indexes"] if group["collection"] == "a")
        b_group = next(group for group in forged["payload"]["indexes"] if group["collection"] == "b")
        for kind in ("data", "offsets"):
            b_group[kind] = dict(a_group[kind])
        forged["sha256"] = hashlib.sha256(_json_bytes(forged["payload"])).hexdigest()
        alias_manifest.write_bytes(_json_bytes(forged))
        try:
            with aliases.read_snapshot() as view:
                result = view.query("b", ranges={"v": {"minimum": 65, "maximum": 65}}, order_by=("v",))
                check("cross-collection file alias cannot reuse semantic verification", [row["v"] for row in result["rows"]] == [65]
                      and not any(path.get("backend") == "disk" for path in result["explain"]["paths"]))
        finally:
            alias_manifest.write_bytes(clean_manifest)
        forged = json.loads(clean_manifest); forged["payload"]["indexes"] = [None]
        forged["sha256"] = hashlib.sha256(_json_bytes(forged["payload"])).hexdigest()
        alias_manifest.write_bytes(_json_bytes(forged))
        try:
            with aliases.read_snapshot() as view:
                result = view.query("b", ranges={"v": {"minimum": 65, "maximum": 65}}, order_by=("v",))
                check("valid-checksum malformed group uses complete RAM fallback", [row["v"] for row in result["rows"]] == [65]
                      and not any(path.get("backend") == "disk" for path in result["explain"]["paths"]))
        finally:
            alias_manifest.write_bytes(clean_manifest)
        shutil_root = sparse.journal.with_name(sparse.journal.name + ".indexes")
        import shutil
        shutil.rmtree(shutil_root)
        with sparse.read_snapshot(["rows"]) as view:
            check("backup-independent cache deletion restores ordinary query",
                  len(view.query("rows", equal={"tenant": "T"}, order_by=("n",))["rows"]) == 6)
    report = {"format": "sdbase-disk-index-acceptance-v1", "passed": all(check["passed"] for check in checks),
              "checks_passed": sum(check["passed"] for check in checks), "checks_total": len(checks),
              "elapsed_seconds": round(time.monotonic() - started, 3), "checks": checks,
              "scope": "owned temporary DBs; real child-process stale build/SIGKILL recovery; no whole-engine disk paging claim"}
    if args.report:
        with args.report.open("x", encoding="utf-8") as stream: json.dump(report, stream, indent=2); stream.write("\n")
    print(json.dumps(report))


if __name__ == "__main__": main()
