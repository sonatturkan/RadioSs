#!/usr/bin/env python3
"""Inspect storage health and create, verify or read sealed audit archives.

All commands are local operator actions.  Archived audit rows contain original
before/after data, including sensitive metadata; protect archive directories.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from sdbase.ops_archive import (create_archive, query_archive,
                                reconstruct_record, verify_archive,
                                verify_archive_chain)  # noqa: E402
from sdbase.schema import open_radio_store  # noqa: E402
from sdbase.storage import StoreError  # noqa: E402
from sdbase.store_health import inspect_store_health  # noqa: E402


def _open_existing_db(path: Path):
    """A mistyped operator path must never bootstrap an empty database."""
    metadata = path.with_name(path.name + ".meta")
    if path.suffix != ".sdb" or path.is_symlink() or not path.is_file() or \
            metadata.is_symlink() or not metadata.is_file():
        raise StoreError("an existing .sdb file and its .meta file are required")
    return open_radio_store(path)


def _positive(value: str) -> int:
    try:
        result = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected a whole number") from exc
    if result < 1:
        raise argparse.ArgumentTypeError("expected a positive whole number")
    return result


def _nonnegative(value: str) -> int:
    try:
        result = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected a whole number") from exc
    if result < 0:
        raise argparse.ArgumentTypeError("expected a nonnegative whole number")
    return result


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    create = commands.add_parser("create", help="write the next immutable audit segment")
    create.add_argument("--db", required=True, type=Path, help="source .sdb path")
    create.add_argument("--destination", required=True, type=Path,
                        help="new, absent archive directory")
    create.add_argument("--previous", type=Path, help="prior segment for incremental export")
    create.add_argument("--through-sequence", type=_positive)
    create.add_argument("--max-records", type=_positive, default=10_000)
    create.add_argument("--max-bytes", type=_positive, default=64 * 1024 * 1024)

    verify = commands.add_parser("verify", help="verify one archive segment")
    verify.add_argument("--archive", required=True, type=Path)
    verify.add_argument("--previous", type=Path)

    chain = commands.add_parser("verify-chain", help="verify ordered segments from sequence 1")
    chain.add_argument("archives", nargs="+", type=Path)

    query = commands.add_parser("query", help="read changes after whole-file verification")
    query.add_argument("--archive", required=True, type=Path)
    query.add_argument("--previous", type=Path)
    query.add_argument("--collection", required=True)
    query.add_argument("--record-id")
    query.add_argument("--first-sequence", type=_positive)
    query.add_argument("--last-sequence", type=_positive)
    query.add_argument("--limit", type=_positive, default=100)

    reconstruct = commands.add_parser("reconstruct", help="rebuild one historical document")
    reconstruct.add_argument("--archives", nargs="+", required=True, type=Path,
                             help="all archive segments from sequence 1")
    reconstruct.add_argument("--collection", required=True)
    reconstruct.add_argument("--record-id", required=True)
    reconstruct.add_argument("--through-sequence", type=_positive)

    health = commands.add_parser("health", help="read storage and evidence coverage metrics")
    health.add_argument("--db", required=True, type=Path, help="source .sdb path")
    health.add_argument("--archives", nargs="*", type=Path,
                        help="all archive segments in sequence order")
    health.add_argument("--max-audit-scan-rows", type=_positive, default=100_000)
    health.add_argument("--min-free-bytes", type=_nonnegative)
    health.add_argument("--critical-free-bytes", type=_nonnegative)
    health.add_argument("--max-journal-bytes", type=_nonnegative)
    health.add_argument("--max-archive-backlog-commits", type=_nonnegative)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "create":
            result = create_archive(
                _open_existing_db(args.db), args.destination,
                previous_archive=args.previous,
                through_sequence=args.through_sequence,
                max_records=args.max_records, max_bytes=args.max_bytes)
        elif args.command == "verify":
            result = verify_archive(args.archive, previous_archive=args.previous)
        elif args.command == "verify-chain":
            result = verify_archive_chain(args.archives)
        elif args.command == "query":
            result = query_archive(
                args.archive, previous_archive=args.previous,
                collection=args.collection, record_id=args.record_id,
                first_sequence=args.first_sequence,
                last_sequence=args.last_sequence, limit=args.limit)
        elif args.command == "reconstruct":
            result = reconstruct_record(
                args.archives, collection=args.collection,
                record_id=args.record_id,
                through_sequence=args.through_sequence)
        else:
            result = inspect_store_health(
                _open_existing_db(args.db), archives=args.archives,
                max_audit_scan_rows=args.max_audit_scan_rows,
                min_free_bytes=args.min_free_bytes,
                critical_free_bytes=args.critical_free_bytes,
                max_journal_bytes=args.max_journal_bytes,
                max_archive_backlog_commits=args.max_archive_backlog_commits)
    except (StoreError, OSError, ValueError) as exc:
        print(json.dumps({"error": str(exc)}, ensure_ascii=False), file=sys.stderr)
        return 2
    print(json.dumps(result, ensure_ascii=False, sort_keys=True, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
