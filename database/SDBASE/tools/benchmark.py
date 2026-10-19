#!/usr/bin/env python3
"""Bounded synthetic SDBASE capacity probe; never a production acceptance result.

Run ``python3 tools/benchmark.py --items 100 --concurrency 2`` from SDBASE.
The default database lives in a disposable temporary directory.  Supplying
``--work-dir`` retains an absent directory for inspection; existing paths are
never overwritten. ``--temporary-parent`` selects the disk for an automatically
cleaned large fixture. No audio engine or real media is used. ``--http-requests``
uses an actual loopback HTTP server and station-scoped user bearer credentials.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from datetime import date, timedelta
import gc
from http.server import ThreadingHTTPServer
import json
import os
from pathlib import Path
import resource
import statistics
import sys
import tempfile
from threading import Thread
from time import perf_counter, sleep
from uuid import NAMESPACE_URL, uuid4, uuid5
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from sdbase.backup import create_backup, verify_backup  # noqa: E402
from sdbase.access_admin import create_station_user  # noqa: E402
from sdbase.http_server import make_handler  # noqa: E402
from sdbase.identity import authenticate, bootstrap  # noqa: E402
from sdbase.schema import open_radio_store  # noqa: E402
from sdbase.search import search_items  # noqa: E402


def _bounded_int(minimum: int, maximum: int):
    def parse(value: str) -> int:
        try:
            number = int(value)
        except ValueError as exc:
            raise argparse.ArgumentTypeError("expected an integer") from exc
        if not minimum <= number <= maximum:
            raise argparse.ArgumentTypeError(f"expected {minimum}..{maximum}")
        return number
    return parse


def _rss() -> dict[str, int | None]:
    current = None
    try:
        pages = int(Path("/proc/self/statm").read_text().split()[1])
        current = pages * os.sysconf("SC_PAGE_SIZE")
    except (OSError, ValueError, IndexError):
        pass
    # This probe runs on Linux, where ru_maxrss is KiB.
    return {"current_bytes": current,
            "peak_bytes": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * 1024}


def _latencies(values: list[float]) -> dict[str, float | int]:
    if not values:
        return {"count": 0}
    ordered = sorted(values)
    p95_index = max(0, (95 * len(ordered) + 99) // 100 - 1)
    return {"count": len(values), "p50_ms": round(statistics.median(ordered), 3),
            "p95_ms": round(ordered[p95_index], 3),
            "max_ms": round(ordered[-1], 3)}


def _elapsed_ms(start: float) -> float:
    return round((perf_counter() - start) * 1000, 3)


def _database_bytes(store) -> int:
    return sum(path.stat().st_size for path in
               (store.journal, store.meta_file, store.checkpoint_file) if path.exists())


def _seed(store, stations: list[dict], count: int,
           batch_size: int) -> tuple[list[tuple[str, str, str]], dict]:
    run_id = uuid4().hex
    item_ids: list[tuple[str, str, str]] = []
    samples: list[float] = []
    started = perf_counter()
    for number in range(count):
        station_id = stations[number % len(stations)]["station_id"]
        item_id = str(uuid5(NAMESPACE_URL, f"{run_id}/item/{number}"))
        version_id = str(uuid5(NAMESPACE_URL, f"{run_id}/version/{number}"))
        file_id = str(uuid5(NAMESPACE_URL, f"{run_id}/file/{number}"))
        association_id = str(uuid5(NAMESPACE_URL, f"{run_id}/association/{number}"))
        if number % batch_size == 0:
            tx = store.begin(actor="benchmark")
        tx.put("items", item_id, {"kind": "music", "title": f"Benchmark track {number:07d}",
                                  "lifecycle": "active"})
        tx.put("station_items", association_id, {
            "station_id": station_id, "item_id": item_id, "eligible": True})
        tx.put("versions", version_id, {"item_id": item_id, "version_no": 1,
                                        "label": "synthetic", "status": "draft"})
        tx.put("media_files", file_id, {"version_id": version_id,
                                        "status": "received",
                                        "technical": {"duration_ms": 180_000}})
        if (number+1) % batch_size == 0 or number+1 == count:
            operation_start = perf_counter()
            tx.commit()
            samples.append(_elapsed_ms(operation_start))
        item_ids.append((item_id, version_id, station_id))
    return item_ids, {"wall_ms": _elapsed_ms(started), "transactions": len(samples),
                      "items_per_batch": batch_size,
                      "documents_written": count * 4, "batch_latency": _latencies(samples),
                      "rss": _rss()}


def _station_setup(store, initial: dict, count: int) -> list[dict]:
    stations = [{"station_id": initial["station_id"],
                 "endpoint_id": initial["endpoint_id"]}]
    for number in range(1, count):
        station_id, endpoint_id = str(uuid4()), str(uuid4())
        tx = store.begin(actor="benchmark")
        tx.put("stations", station_id, {"code": f"BENCH{number}",
            "name": f"Synthetic station {number}", "timezone": "UTC", "status": "active"})
        tx.put("endpoints", endpoint_id, {"station_id": station_id,
            "code": "main", "name": "Synthetic endpoint", "status": "offline"})
        tx.commit()
        stations.append({"station_id": station_id, "endpoint_id": endpoint_id})
    return stations


def _operational_seed(store, stations: list[dict], items: list[tuple[str, str, str]],
                      count: int, years: int, batch_size: int) -> dict:
    """Write synthetic off-plan engine claims, not measured broadcasts.

    Each schema-validated transaction contains one completed attempt, its
    start/completion events and a self-report observation. All identities and
    timestamps are linked. This is a storage load fixture, not an engine test.
    """
    started = perf_counter()
    samples = []
    endpoint_by_station = {row["station_id"]: row["endpoint_id"] for row in stations}
    end = datetime(2026, 10, 1, tzinfo=timezone.utc)
    start = end.replace(year=end.year-years)
    span = (end-start).total_seconds() - 180
    for number in range(count):
        _item_id, version_id, station_id = items[number % len(items)]
        endpoint_id = endpoint_by_station[station_id]
        occurred = start + timedelta(seconds=span*number/max(1, count-1))
        completed = occurred + timedelta(seconds=180)
        at, done = occurred.isoformat(), completed.isoformat()
        attempt_id, first_id, last_id, observation_id = (str(uuid4()) for _ in range(4))
        attempt_key = f"synthetic:{number}"
        if number % batch_size == 0:
            tx = store.begin(actor="benchmark-synthetic-engine")
        tx.put("playout_attempts", attempt_id, {"station_id": station_id,
            "endpoint_id": endpoint_id, "version_id": version_id,
            "attempt_key": attempt_key, "attempt_no": 1, "state": "completed",
            "last_phase": "complete", "last_event_at": done,
            "created_at": at, "updated_at": done, "last_event_id": last_id,
            "plan_entry_id": None, "media_file_id": None})
        for identifier, phase, timestamp in ((first_id, "start", at),
                                               (last_id, "complete", done)):
            engine_id = f"synthetic:{number}:{phase}"
            tx.put("playout_events", identifier, {"station_id": station_id,
                "endpoint_id": endpoint_id, "version_id": version_id,
                "attempt_id": attempt_id, "event_key": f"{endpoint_id}:{engine_id}",
                "engine_event_id": engine_id, "phase": phase, "occurred_at": timestamp,
                "received_at": timestamp, "attempt_no": 1,
                "details": {"attempt_key": attempt_key, "synthetic": True},
                "plan_entry_id": None, "media_file_id": None,
                "accepted_snapshot_sha256": None})
        tx.put("output_observations", observation_id, {"station_id": station_id,
            "endpoint_id": endpoint_id, "observation_key": f"{endpoint_id}:synthetic:{number}",
            "kind": "audio_present", "occurred_at": done, "received_at": done,
            "event_id": last_id, "attempt_id": attempt_id,
            "observer_kind": "endpoint_self_report", "evidence_ref": None,
            "details": {"synthetic": True}})
        if (number+1) % batch_size == 0 or number+1 == count:
            sample = perf_counter()
            tx.commit()
            samples.append(_elapsed_ms(sample))
    return {"wall_ms": _elapsed_ms(started), "transactions": len(samples),
            "attempts_per_batch": batch_size,
            "attempts": count, "events": count*2, "observations": count,
            "documents_written": count*4,
            "timestamp_start": start.isoformat(), "timestamp_end": end.isoformat(),
            "simulated_years": years, "real_elapsed_years": 0,
            "batch_latency": _latencies(samples), "rss": _rss()}


def _searches(store, principal, stations: list[dict], count: int, concurrency: int) -> dict:
    def one(_number: int) -> tuple[float, int]:
        station_id = stations[_number % len(stations)]["station_id"]
        started = perf_counter()
        result = search_items(store, principal, station_id,
                              title="Benchmark track", min_duration_ms=1,
                              max_duration_ms=300_000, limit=25)
        if not result["items"]:
            raise RuntimeError("synthetic catalog query unexpectedly returned no items")
        return _elapsed_ms(started), len(result["items"])

    started = perf_counter()
    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        samples = [future.result() for future in as_completed(
            pool.submit(one, number) for number in range(count))]
    return {"wall_ms": _elapsed_ms(started), "queries": count,
            "concurrency": concurrency,
            "items_per_page": min(result_count for _latency, result_count in samples),
            "latency": _latencies([latency for latency, _count in samples]),
            "rss": _rss()}


def _updates(store, item_ids: list[tuple[str, str, str]], count: int, concurrency: int) -> dict:
    def one(item_id: str) -> float:
        started = perf_counter()
        current = store.get("items", item_id)
        if current is None:
            raise RuntimeError("synthetic item disappeared")
        body = {key: value for key, value in current.items()
                if key not in {"_id", "_rev"}}
        body["benchmark_update"] = True
        store.put("items", item_id, body, expected_revision=current["_rev"],
                  actor="benchmark")
        return _elapsed_ms(started)

    started = perf_counter()
    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        samples = [future.result() for future in as_completed(
            pool.submit(one, item_id) for item_id, _version_id, _station_id in item_ids[:count])]
    return {"wall_ms": _elapsed_ms(started), "transactions": count,
            "concurrency": concurrency,
            "latency": _latencies(samples) if samples else None,
            "rss": _rss()}


def _http_load(store, principal, stations: list[dict], items: list[tuple[str, str, str]],
                args: argparse.Namespace) -> dict:
    """Use real HTTP authentication, dispatch and optimistic preconditions.

    Waves stay within the server's normal IP/token limits. Conflicts are
    reported and never silently retried. The wave pause is outside operation
    latency and inside total wall time.
    """
    if not args.http_requests:
        return {"enabled": False}
    users: dict[str, list[str]] = {row["station_id"]: [] for row in stations}
    for number in range(args.api_users):
        sid = stations[number % len(stations)]["station_id"]
        user = create_station_user(store, principal, sid,
            f"benchmark-user-{number}", f"Synthetic API user {number}",
            ["view", "edit", "activate"])
        users[sid].append(user["token"])
    # Read each mutation's actual row revision before the timed concurrent wave.
    # The direct POST latency therefore measures writes; it does not hide a
    # rejected detail-read attempt or silently refresh a stale precondition.
    write_preconditions = {}
    for number in range(args.http_requests*args.http_rounds):
        if number % 3 == 1:
            item_id, _version_id, sid = items[number % len(items)]
            association = store.lookup("station_items", "station_item", sid, item_id)[0]
            write_preconditions[number] = (association["_rev"], not association["eligible"])
    handler = make_handler(store)
    handler.log_message = lambda *arguments: None
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"

    def request(path: str, token: str, body: dict | None = None) -> tuple[int, dict, float]:
        raw = json.dumps(body).encode() if body is not None else None
        req = Request(base+path, data=raw, headers={"Authorization": "Bearer "+token,
                      "Content-Type": "application/json"},
                      method="POST" if body is not None else "GET")
        started = perf_counter()
        try:
            with urlopen(req, timeout=60) as response:
                code, result = response.status, json.load(response)
        except HTTPError as error:
            code, result = error.code, json.load(error)
        if code not in {200, 409, 429}:
            raise RuntimeError(f"HTTP load request failed: {code} {result}")
        return code, result, _elapsed_ms(started)

    def one(number: int) -> dict:
        item_id, _version_id, station_id = items[number % len(items)]
        tokens = users[station_id]
        token = tokens[(number//len(stations)) % len(tokens)]
        kind = ("search", "optimistic_write", "create_plan_with_entry")[number % 3]
        calls: list[tuple[int, float]] = []
        started = perf_counter()
        if kind == "search":
            query = urlencode({"station_id": station_id, "title": "Benchmark track",
                               "min_duration_ms": 1, "max_duration_ms": 300_000,
                               "limit": 25})
            code, result, elapsed = request("/v1/items?"+query, token)
            calls.append((code, elapsed))
            if code == 200 and len(result["items"]) != 25:
                raise RuntimeError("HTTP search returned an unexpected page length")
        elif kind == "optimistic_write":
            revision, eligible = write_preconditions[number]
            code, result, elapsed = request(f"/v1/items/{item_id}/eligibility", token,
                {"station_id": station_id, "eligible": eligible,
                 "expected_revision": revision})
            calls.append((code, elapsed))
        else:
            day = (date(2030, 1, 1)+timedelta(days=number)).isoformat()
            code, result, elapsed = request("/v1/plans", token,
                    {"station_id": station_id, "service_date": day})
            calls.append((code, elapsed))
            if code == 200:
                code, result, elapsed = request(f"/v1/plans/{result['plan_id']}/entries", token,
                    {"station_id": station_id, "revision_id": result["draft_revision_id"],
                     "expected_plan_revision": result["plan_revision"],
                     "entry": {"ordinal": 0, "scheduled_at": day+"T10:00:00Z",
                               "event_kind": "wait", "planned_duration_ms": 1000,
                               "payload": {}}})
                calls.append((code, elapsed))
        return {"kind": kind, "status": code, "wall_ms": _elapsed_ms(started),
                "calls": calls}

    started = perf_counter()
    results = []
    try:
        for wave in range(args.http_rounds):
            if wave:
                sleep(12)
            with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
                results.extend(future.result() for future in as_completed(
                    pool.submit(one, wave*args.http_requests+number)
                    for number in range(args.http_requests)))
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    grouped = {}
    for kind in ("search", "optimistic_write", "create_plan_with_entry"):
        group = [result for result in results if result["kind"] == kind]
        grouped[kind] = {"operations": len(group),
            "statuses": {str(code): sum(result["status"] == code for result in group)
                         for code in sorted({result["status"] for result in group})},
            "successful_operation_latency": _latencies(
                [result["wall_ms"] for result in group if result["status"] == 200]),
            "all_operation_latency": _latencies([result["wall_ms"] for result in group])}
    calls = [call for result in results for call in result["calls"]]
    return {"enabled": True, "transport": "actual authenticated loopback HTTP",
            "wall_ms": _elapsed_ms(started), "users": args.api_users,
            "stations": len(stations), "concurrency": args.concurrency,
            "operations": len(results), "http_requests": len(calls),
            "waves": args.http_rounds, "wave_pause_seconds": 12,
            "statuses": {str(code): sum(status == code for status, _latency in calls)
                         for code in sorted({status for status, _latency in calls})},
            "request_latency": _latencies([latency for _code, latency in calls]),
            "operations_by_kind": grouped, "conflicts_retried": 0,
            "write_revision_source": "actual station-item._rev read before concurrent workload",
            "rss": _rss()}


def _run(root: Path, args: argparse.Namespace, *, retained: bool) -> dict:
    def progress(message: str) -> None:
        if args.progress:
            print(message, file=sys.stderr, flush=True)

    path = root / "store" / "RadioS.sdb"
    started = perf_counter()
    store = open_radio_store(path)
    initial = bootstrap(store, "benchmark-admin", "BENCH", "Synthetic benchmark")
    principal = authenticate(store, initial["api_token"])
    initial["api_token"] = "<redacted>"
    stations = _station_setup(store, initial, args.stations)
    progress("Seeding synthetic catalog")
    item_ids, seed = _seed(store, stations, args.items, args.seed_batch_size)
    operational = _operational_seed(store, stations, item_ids,
                                    args.operational_attempts, args.log_years, args.seed_batch_size)
    progress("Measuring catalog searches and point writes")
    searches = _searches(store, principal, stations,
                         args.searches, args.concurrency)
    updates = _updates(store, item_ids, args.updates, args.concurrency)
    progress("Measuring authenticated concurrent HTTP users")
    http_load = _http_load(store, principal, stations, item_ids, args)
    disk_indexes = {"enabled": False}
    if args.disk_indexes:
        from sdbase.disk_index import rebuild_disk_indexes
        progress("Building and measuring persistent station/item ordered index")
        phase = perf_counter()
        built = rebuild_disk_indexes(store, collections=["station_items"],
                                     index_names={"station_items": ["station_item"]})
        build_ms = _elapsed_ms(phase)
        station_id = stations[0]["station_id"]
        keys = sorted(item for item, _version, station in item_ids if station == station_id)
        with store.read_snapshot(["station_items"]) as view:
            phase = perf_counter()
            measured = view.query("station_items", equal={"station_id": station_id},
                ranges={"item_id": {"minimum": keys[0], "maximum": keys[min(19, len(keys)-1)]}},
                order_by=["item_id"], limit=20, max_index_keys=64, max_scan=64, max_candidates=64)
            query_ms = _elapsed_ms(phase)
            phase = perf_counter()
            warm = view.query("station_items", equal={"station_id": station_id},
                ranges={"item_id": {"minimum": keys[0], "maximum": keys[min(19, len(keys)-1)]}},
                order_by=["item_id"], limit=20, max_index_keys=64, max_scan=64, max_candidates=64)
            warm_ms = _elapsed_ms(phase)
            if warm["rows"] != measured["rows"]:
                raise RuntimeError("warm disk query differs from its captured cold query")
        if {row["item_id"] for row in measured["rows"]} != set(keys[:20]):
            raise RuntimeError("disk index range result does not match seeded station items")
        disk_indexes = {"enabled": True, "build_ms": build_ms, "build": built,
                        "query_ms": query_ms, "warm_query_ms": warm_ms,
                        "returned": len(measured["rows"]), "explain": measured["explain"],
                        "warm_explain": warm["explain"], "rss": _rss()}
    before_checkpoint_bytes = _database_bytes(store)
    progress("Checkpointing and reopening")
    phase = perf_counter()
    concurrent_maintenance_writes = []
    if args.online_checkpoint:
        import threading
        start_writers = threading.Event()
        def maintenance_writer(number):
            start_writers.wait()
            written = []
            for step in range(4):
                item_id = item_ids[number % len(item_ids)][0]
                row = store.get("items", item_id)
                begun = perf_counter()
                store.put("items", item_id,
                    {field: value for field, value in row.items() if field not in {"_id", "_rev"}} |
                    {"title": f"Online checkpoint writer {number}/{step}"}, expected_revision=row["_rev"])
                written.append(_elapsed_ms(begun))
            return written
        with ThreadPoolExecutor(max_workers=min(args.concurrency, len(item_ids))) as pool:
            futures = [pool.submit(maintenance_writer, number)
                       for number in range(min(args.concurrency, len(item_ids)))]
            start_writers.set()
            online_result = store.checkpoint_online()
            for future in futures:
                concurrent_maintenance_writes.extend(future.result())
        if online_result["status"] != "published":
            raise RuntimeError("online checkpoint was deferred under the selected workload")
        checkpoint_sequence = store.sequence
    else:
        online_result = None
        checkpoint_sequence = store.checkpoint()
    checkpoint = {"wall_ms": _elapsed_ms(phase), "sequence": checkpoint_sequence,
                  "database_bytes_before": before_checkpoint_bytes,
                  "database_bytes_after": _database_bytes(store), "rss": _rss(),
                  "mode": "online" if args.online_checkpoint else "exclusive",
                  "online_result": online_result,
                  "concurrent_writer_latency": _latencies(concurrent_maintenance_writes)}
    del store
    gc.collect()
    phase = perf_counter()
    reopened = open_radio_store(path)
    reopen = {"wall_ms": _elapsed_ms(phase), "sequence": reopened.sequence,
              "rss": _rss()}
    if reopened.sequence != checkpoint_sequence:
        raise RuntimeError("reopened sequence differs from checkpoint")
    progress("Creating and verifying the metadata-only backup")
    phase = perf_counter()
    backup = create_backup(reopened, root / "backup", checkpoint=False)
    backup_create = {"wall_ms": _elapsed_ms(phase),
                     "media_objects": backup["media_objects"], "rss": _rss()}
    phase = perf_counter()
    verified = verify_backup(root / "backup")
    backup_verify = {"wall_ms": _elapsed_ms(phase),
                     "files": verified["files"], "rss": _rss()}
    return {"format": "sdbase-synthetic-benchmark-v2",
            "production_result": False,
            "scope": "Synthetic catalog/media metadata and self-reported operational claims; "
                     "multiple stations and optional actual loopback HTTP users. "
                     "No audio bytes, verified media, physical output or real years-long run. "
                     "The result does not establish production capacity.",
            "database_file": str(path) if retained else "temporary .sdb (removed after run)",
            "parameters": {"items": args.items, "documents_per_item": 4,
                           "disk_indexes": args.disk_indexes, "online_checkpoint": args.online_checkpoint,
                           "seed_batch_size": args.seed_batch_size,
                           "stations": args.stations, "api_users": args.api_users,
                           "operational_attempts": args.operational_attempts,
                           "log_years": args.log_years,
                           "http_requests_per_wave": args.http_requests,
                           "http_rounds": args.http_rounds,
                           "searches": args.searches, "updates": args.updates,
                           "concurrency": args.concurrency},
            "measurements": {"seed_write": seed, "search": searches,
                             "operational_seed": operational, "http_load": http_load,
                             "concurrent_write": updates, "checkpoint": checkpoint,
                             "reopen": reopen, "backup_create": backup_create,
                             "backup_verify": backup_verify, "disk_indexes": disk_indexes,
                             "total_wall_ms": _elapsed_ms(started)},
            "rss_final": _rss(),
            "address_space_limit_bytes": resource.getrlimit(resource.RLIMIT_AS)[0],
            "created_at": datetime.now(timezone.utc).isoformat()}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--items", type=_bounded_int(1, 500_000), default=100)
    parser.add_argument("--searches", type=_bounded_int(1, 10_000), default=20)
    parser.add_argument("--updates", type=_bounded_int(0, 500_000), default=20)
    parser.add_argument("--concurrency", type=_bounded_int(1, 32), default=1)
    parser.add_argument("--stations", type=_bounded_int(1, 32), default=1)
    parser.add_argument("--api-users", type=_bounded_int(1, 64), default=8)
    parser.add_argument("--operational-attempts", type=_bounded_int(0, 500_000), default=0)
    parser.add_argument("--log-years", type=_bounded_int(1, 20), default=6)
    parser.add_argument("--seed-batch-size", type=_bounded_int(1, 1000), default=1,
                        help="synthetic fixture items/attempts per fsynced seed transaction")
    parser.add_argument("--http-requests", type=_bounded_int(0, 60), default=0,
                        help="mixed HTTP operations per wave; each sends 1 or 2 requests")
    parser.add_argument("--http-rounds", type=_bounded_int(1, 20), default=1)
    parser.add_argument("--temporary-parent", type=Path,
                        help="disk directory for the automatically removed fixture")
    parser.add_argument("--progress", action="store_true",
                        help="write phase progress to stderr without changing JSON stdout")
    parser.add_argument("--disk-indexes", action="store_true",
                        help="build and measure persistent station/item range index")
    parser.add_argument("--online-checkpoint", action="store_true",
                        help="prepare checkpoint while concurrent API-independent item writers run")
    parser.add_argument("--memory-limit-mib", type=_bounded_int(0, 65_536), default=0,
                        help="optional Linux process address-space bound; 0 keeps the current limit")
    parser.add_argument("--work-dir", type=Path,
                        help="retain the benchmark in a new, absent directory")
    args = parser.parse_args(argv)
    if args.updates > args.items:
        parser.error("--updates cannot exceed --items")
    if args.stations > args.items or args.http_requests and args.api_users < args.stations:
        parser.error("items and active API users must cover every station")
    if args.http_requests and args.items < 25*args.stations:
        parser.error("HTTP search probe needs at least 25 items per station")
    if args.temporary_parent is not None and not args.temporary_parent.is_dir():
        parser.error("--temporary-parent must name an existing directory")
    if args.memory_limit_mib:
        requested_limit = args.memory_limit_mib * 1024 * 1024
        _soft, hard_limit = resource.getrlimit(resource.RLIMIT_AS)
        if hard_limit != resource.RLIM_INFINITY:
            requested_limit = min(requested_limit, hard_limit)
        resource.setrlimit(resource.RLIMIT_AS, (requested_limit, requested_limit))
    if args.work_dir is None:
        with tempfile.TemporaryDirectory(prefix="sdbase-benchmark-",
                                         dir=args.temporary_parent) as temporary:
            result = _run(Path(temporary), args, retained=False)
    else:
        target = args.work_dir.expanduser().absolute()
        if target.exists() or target.is_symlink():
            parser.error("--work-dir must name an absent directory")
        if not target.parent.is_dir():
            parser.error("--work-dir parent must exist")
        target.mkdir(mode=0o700)
        result = _run(target, args, retained=True)
    json.dump(result, sys.stdout, ensure_ascii=False, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
