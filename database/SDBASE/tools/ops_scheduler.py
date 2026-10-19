#!/usr/bin/env python3
"""Run verified SDBASE backups and contiguous audit exports on a local schedule."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import signal
import sys
from threading import Event

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from sdbase.ops_scheduler import (ScheduleConfig, run_locked_loop,
                                  run_once)  # noqa: E402
from sdbase.storage import StoreError  # noqa: E402


def _positive(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected a positive whole number") from exc
    if parsed < 1:
        raise argparse.ArgumentTypeError("expected a positive whole number")
    return parsed


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", required=True, type=Path, help="existing .sdb database")
    parser.add_argument("--output-dir", required=True, type=Path,
                        help="existing mode-0700 directory for artifacts and state")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--once", action="store_true", help="run due jobs once and exit")
    mode.add_argument("--loop", action="store_true", help="run until SIGINT or SIGTERM")
    parser.add_argument("--force", action="store_true",
                        help="with --once, run both jobs even before their due time")
    parser.add_argument("--backup-interval-seconds", type=_positive, default=24 * 3600)
    parser.add_argument("--archive-interval-seconds", type=_positive, default=3600)
    parser.add_argument("--max-archive-records", type=_positive, default=10_000)
    parser.add_argument("--max-archive-bytes", type=_positive, default=64 * 1024 * 1024)
    parser.add_argument("--max-archive-segments-per-run", type=_positive, default=4)
    parser.add_argument("--poll-seconds", type=_positive, default=30,
                        help="loop wakeup interval; backlog may run at every wakeup")
    parser.add_argument("--station-id", action="append", default=[],
                        help="station UUID for optional delivery checks; repeatable")
    parser.add_argument("--token-env", help="environment variable holding a user API token")
    parser.add_argument("--delivery-policy", type=Path,
                        help="JSON DeliveryPolicy object for optional checks")
    parser.add_argument("--delivery-interval-seconds", type=_positive, default=300)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    if args.force and not args.once:
        parser.error("--force requires --once")
    if bool(args.station_id or args.token_env or args.delivery_policy) and \
            not (args.station_id and args.token_env and args.delivery_policy):
        parser.error("delivery checks require --station-id, --token-env and --delivery-policy")
    try:
        policy = json.loads(args.delivery_policy.read_text(encoding="utf-8")) \
            if args.delivery_policy else None
        if policy is not None and not isinstance(policy, dict):
            raise ValueError("delivery policy must be a JSON object")
    except (OSError, UnicodeError, ValueError) as exc:
        print(json.dumps({"error": f"cannot read delivery policy: {exc}"},
                         ensure_ascii=False), file=sys.stderr)
        return 2
    config = ScheduleConfig(
        database=args.db, output_dir=args.output_dir,
        backup_interval_seconds=args.backup_interval_seconds,
        archive_interval_seconds=args.archive_interval_seconds,
        max_archive_records=args.max_archive_records,
        max_archive_bytes=args.max_archive_bytes,
        max_archive_segments_per_run=args.max_archive_segments_per_run,
        station_ids=tuple(args.station_id), token_env=args.token_env,
        delivery_policy=policy,
        delivery_interval_seconds=args.delivery_interval_seconds)
    try:
        if args.once:
            report = run_once(config, force=args.force)
            print(json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2))
            return 2 if report["errors"] else 0
        stop = Event()
        previous = {}

        def request_stop(_signum, _frame):
            stop.set()

        for signum in (signal.SIGINT, signal.SIGTERM):
            previous[signum] = signal.getsignal(signum)
            signal.signal(signum, request_stop)
        try:
            run_locked_loop(
                config, stop, poll_seconds=args.poll_seconds,
                on_report=lambda result: print(json.dumps(
                    result, ensure_ascii=False, sort_keys=True), flush=True))
        finally:
            for signum, handler in previous.items():
                signal.signal(signum, handler)
        return 0
    except (StoreError, OSError, ValueError) as exc:
        print(json.dumps({"error": f"{type(exc).__name__}: {exc}"},
                         ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
