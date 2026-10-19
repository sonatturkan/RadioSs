"""Administration CLI for the standalone RadioS `.sdb` database."""

from __future__ import annotations

import argparse
from contextlib import closing, contextmanager
import json
import os
from pathlib import Path
import sys

from . import _platform
from .identity import authenticate, bootstrap, issue_endpoint_token
from .schema import open_radio_store
from .storage import StoreError
from .endpoint_journal import EndpointError


_MAX_MAP_BYTES = 2 * 1024 * 1024
_WSAEACCES = 10013
_WSAEADDRINUSE = 10048


def _json_map_file(path: Path) -> dict:
    """Load a bounded operator mapping without silently dropping duplicate keys."""
    if path.stat().st_size > _MAX_MAP_BYTES:
        raise StoreError("mapping JSON exceeds 2 MiB")
    # Bounded share-delete read: a file that grows after the size check cannot
    # exhaust memory, and the reader never blocks the editor saving the file.
    with _platform.open_read(path) as stream:
        raw = stream.read(_MAX_MAP_BYTES + 1)
    if len(raw) > _MAX_MAP_BYTES:
        raise StoreError("mapping JSON exceeds 2 MiB")
    def pairs(values):
        result = {}
        for key, value in values:
            if key in result:
                raise StoreError("mapping JSON contains a duplicate key")
            result[key] = value
        return result
    try:
        # utf-8-sig: Windows PowerShell 5.1 (Out-File/Set-Content -Encoding utf8) writes a BOM.
        value = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=pairs)
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise StoreError("mapping file is not valid UTF-8 JSON") from exc
    if not isinstance(value, dict):
        raise StoreError("mapping JSON root must be an object")
    return value


def _environment_secret(name):
    import re
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        raise StoreError("invalid secret environment variable name")
    value = os.environ.get(name)
    if not value or len(value) < 32:
        raise StoreError("replication secret environment variable needs at least 32 characters")
    return value


@contextmanager
def _listener(host, port):
    """Report Windows bind failures of a listening command as operator messages."""
    try:
        yield
    except OSError as exc:
        address = f"{host}:{port}"
        if getattr(exc, "winerror", None) == _WSAEADDRINUSE:
            raise StoreError(f"{address} kullanımda.") from exc
        if getattr(exc, "winerror", None) == _WSAEACCES:
            raise StoreError(
                f"{address} Windows tarafından ayrılmış veya başka bir uygulamaya özel; "
                "`netsh int ipv4 show excludedportrange protocol=tcp` ile denetleyin ve "
                "--port ile başka bir port seçin.") from exc
        raise


def _print_listening(address) -> None:
    """Readiness line for supervisors and tests (``--port 0`` picks a free port)."""
    host, port = address[:2]
    print(json.dumps({"listening": {"host": host, "port": port}}), flush=True)


def main(argv: list[str] | None = None) -> int:
    # No critical-error dialogs from a windowless process (empty removable drives),
    # 8 MiB thread stacks, and UTF-8 JSON on stdout even without -X utf8.
    _platform.configure_process()
    _platform.configure_stdio_utf8()
    parser = argparse.ArgumentParser(prog="sdbase", description="RadioS custom database")
    parser.add_argument("--db", type=Path, default=Path("RadioS.sdb"),
                        help="primary .sdb database file")
    parser.add_argument("--lock-timeout", type=float, default=30.0)
    sub = parser.add_subparsers(dest="command", required=True)
    init = sub.add_parser("init", help="create the first station, user and endpoint")
    init.add_argument("--username", required=True)
    init.add_argument("--station-code", required=True)
    init.add_argument("--station-name", required=True)
    init.add_argument("--timezone", default="UTC")
    server = sub.add_parser("serve", help="serve authenticated HTTP API")
    server.add_argument("--host", default="127.0.0.1")
    server.add_argument("--port", type=int, default=8000)
    server.add_argument("--max-connections", type=int, default=32)
    server.add_argument("--request-timeout", type=float, default=30.0)
    sub.add_parser("status", help="show database sequence and collection counts")
    sub.add_parser("metrics", help="show local engine, storage and role statistics")
    integrity = sub.add_parser("integrity", help="read-only bounded database and asset scrub")
    integrity.add_argument("--max-records", type=int, default=100000)
    integrity.add_argument("--max-bytes", type=int, default=1073741824)
    integrity.add_argument("--skip-media", action="store_true")
    integrity.add_argument("--skip-cold", action="store_true")
    sub.add_parser("wal-enable", help="archive future commits; then take a new base backup")
    sub.add_parser("wal-capture", help="seal committed journal frames missing from the archive")
    sub.add_parser("wal-status", help="verify and report the active WAL archive")
    for command in ("pitr-preview", "pitr-restore"):
        recovery = sub.add_parser(command, help="verified sequence/time recovery from base backup and WAL")
        recovery.add_argument("--backup", type=Path, required=True)
        recovery.add_argument("--wal-dir", type=Path, required=True)
        selector = recovery.add_mutually_exclusive_group(required=True)
        selector.add_argument("--target-sequence", type=int)
        selector.add_argument("--target-time")
        recovery.add_argument("--exclusive", action="store_true")
        if command == "pitr-restore":
            recovery.add_argument("--target-dir", type=Path, required=True)
    replica = sub.add_parser("replica-create", help="create a verified read-only standby")
    replica.add_argument("--target-dir", type=Path, required=True)
    sub.add_parser("replica-status", help="show persisted standby progress and source identity")
    catchup = sub.add_parser("replica-catchup", help="apply verified local primary WAL to a standby")
    catchup.add_argument("--standby-db", type=Path, required=True)
    catchup.add_argument("--max-records", type=int, default=10000)
    shipment = sub.add_parser("replica-shipment", help="publish a verified incremental transfer bundle")
    shipment.add_argument("--destination", type=Path, required=True)
    shipment.add_argument("--after-sequence", type=int, required=True)
    shipment.add_argument("--previous-sha256", required=True)
    shipment.add_argument("--through-sequence", type=int)
    shipment.add_argument("--max-records", type=int, default=10000)
    replay = sub.add_parser("replica-apply", help="apply an incremental bundle to the --db standby")
    replay.add_argument("--bundle", type=Path, required=True)
    replay_recover = sub.add_parser("replica-recover", help="resume an interrupted standby replay")
    replay_recover.add_argument("--bundle", type=Path, required=True)
    promotion = sub.add_parser("replica-promote", help="fence reachable source and hand over to caught-up standby")
    promotion.add_argument("--standby-db", type=Path, required=True)
    promotion.add_argument("--expected-sequence", type=int, required=True)
    promotion.add_argument("--expected-timeline", required=True)
    query = sub.add_parser("query", help="local bounded document query; no SQL parser")
    query.add_argument("--collection", required=True)
    query.add_argument("--query-json", type=Path, required=True)
    query.add_argument("--explain", action="store_true")
    sub.add_parser("checkpoint", help="write a checksummed recovery checkpoint")
    for command in ("checkpoint-online", "maintenance-loop"):
        maintenance = sub.add_parser(command, help="prepare a checkpoint while writes continue")
        maintenance.add_argument("--max-tail-bytes", type=int, default=64 * 1024**2)
        maintenance.add_argument("--max-tail-frames", type=int, default=10000)
        if command == "maintenance-loop":
            maintenance.add_argument("--interval", type=float, default=60)
            maintenance.add_argument("--journal-threshold-bytes", type=int, default=64 * 1024**2)
    maintenance_recover = sub.add_parser("maintenance-recover", help="inspect owned abandoned preparation jobs")
    maintenance_recover.add_argument("--cleanup", action="store_true")
    disk_build = sub.add_parser("index-rebuild", help="online build of generation-bound ordered disk indexes")
    disk_build.add_argument("--collection", action="append")
    disk_build.add_argument("--max-rows", type=int, default=1000000)
    disk_build.add_argument("--max-bytes", type=int, default=2 * 1024**3)
    disk_build.add_argument("--sort-chunk-rows", type=int, default=20000)
    sub.add_parser("index-status", help="inspect persistent ordered index generations")
    disk_recover = sub.add_parser("index-recover", help="inspect owned abandoned index build jobs")
    disk_recover.add_argument("--cleanup", action="store_true")
    replication_server = sub.add_parser("replication-serve", help="dedicated authenticated TLS replication listener")
    replication_server.add_argument("--host", default="127.0.0.1")
    replication_server.add_argument("--port", type=int, default=8443)
    replication_server.add_argument("--token-env", required=True)
    replication_server.add_argument("--cert", type=Path)
    replication_server.add_argument("--key", type=Path)
    replication_server.add_argument("--allow-loopback-http", action="store_true")
    replication_server.add_argument("--max-records", type=int, default=1000)
    replication_server.add_argument("--max-batch-bytes", type=int, default=64 * 1024**2)
    replication_server.add_argument("--max-cache-bytes", type=int, default=512 * 1024**2)
    replication_server.add_argument("--max-cached-batches", type=int, default=4)
    replication_server.add_argument("--ttl-seconds", type=float, default=120)
    for command in ("replication-pull", "replication-follow"):
        follower = sub.add_parser(command, help="resume verified network WAL/media transfer to a standby")
        follower.add_argument("--source-url", required=True)
        follower.add_argument("--token-env", required=True)
        follower.add_argument("--ca-file", type=Path)
        follower.add_argument("--allow-loopback-http", action="store_true")
        follower.add_argument("--max-records", type=int, default=1000)
        follower.add_argument("--max-batch-bytes", type=int, default=64 * 1024**2)
        follower.add_argument("--timeout", type=float, default=15)
        if command == "replication-follow":
            follower.add_argument("--poll-interval", type=float, default=1)
    for command in ("failover-tick", "failover-status", "failover-reset", "failover-watch", "failover-serve"):
        failover = sub.add_parser(command, help="supervised automatic handover with reachable-source fencing")
        failover.add_argument("--standby-db", type=Path, required=True)
        failover.add_argument("--source-url", required=True)
        failover.add_argument("--state-dir", type=Path, required=True)
        failover.add_argument("--failure-threshold", type=int, default=3)
        failover.add_argument("--probe-timeout", type=float, default=1)
        failover.add_argument("--probe-interval", type=float, default=2)
        failover.add_argument("--cooldown", type=float, default=300)
        if command == "failover-serve":
            failover.add_argument("--host", default="127.0.0.1")
            failover.add_argument("--port", type=int, default=8000)
            failover.add_argument("--max-connections", type=int, default=32)
            failover.add_argument("--request-timeout", type=float, default=30)
    retention_preview = sub.add_parser("retention-preview", help="preview dependency-safe cold history maintenance")
    retention_preview.add_argument("--token", required=True)
    retention_preview.add_argument("--policy-json", type=Path)
    retention_preview.add_argument("--as-of")
    retention_preview.add_argument("--max-records", type=int, default=10000)
    retention_apply = sub.add_parser("retention-apply", help="apply an exact reviewed retention preview")
    retention_apply.add_argument("--token", required=True)
    retention_apply.add_argument("--preview-json", type=Path, required=True)
    retention_query = sub.add_parser("retention-query", help="read verified archived records")
    retention_query.add_argument("--token", required=True)
    retention_query.add_argument("--collection", required=True)
    retention_query.add_argument("--record-id")
    retention_query.add_argument("--segment-id")
    retention_query.add_argument("--limit", type=int, default=100)
    retention_query.add_argument("--offset", type=int, default=0)
    sub.add_parser("retention-verify", help="verify registered cold history hashes and audit continuity")
    endpoint_token = sub.add_parser("endpoint-token", help="issue endpoint-bound credential")
    endpoint_token.add_argument("--token", required=True, help="administrator bearer token")
    endpoint_token.add_argument("--station-id", required=True)
    endpoint_token.add_argument("--endpoint-id", required=True)
    endpoint_token.add_argument("--days", type=int, default=30)
    backup_create = sub.add_parser("backup-create", help="copy one consistent database/media snapshot")
    backup_create.add_argument("--destination", type=Path, required=True)
    backup_verify = sub.add_parser("backup-verify", help="verify a backup manifest and hashes")
    backup_verify.add_argument("--backup", type=Path, required=True)
    backup_restore = sub.add_parser("backup-restore", help="restore into an absent target directory")
    backup_restore.add_argument("--backup", type=Path, required=True)
    backup_restore.add_argument("--target-dir", type=Path, required=True)
    provision_copy = sub.add_parser("provision-copy", help="create an independent offline .sdb copy")
    provision_copy.add_argument("--target-dir", type=Path, required=True)
    portable_export = sub.add_parser("portable-export", help="write a JSONL/CSV data package without audio bytes")
    portable_export.add_argument("--token", required=True)
    portable_export.add_argument("--destination", type=Path, required=True)
    portable_verify = sub.add_parser("portable-verify", help="verify a portable JSONL/CSV data package")
    portable_verify.add_argument("--package", type=Path, required=True)
    portable_restore = sub.add_parser("portable-restore", help="restore a portable package into a new fenced .sdb store")
    portable_restore.add_argument("--package", type=Path, required=True)
    portable_restore.add_argument("--target-dir", type=Path, required=True)
    sub.add_parser("migrate-preview", help="inspect a legacy v1/v2/v3 .sdb without writing to it")
    migrate_copy = sub.add_parser("migrate-copy", help="migrate legacy v1/v2/v3 into a new fenced v4 directory")
    migrate_copy.add_argument("--target-dir", type=Path, required=True)
    migrate_backup = sub.add_parser("migrate-backup", help="migrate a verified legacy backup into a new fenced v4 directory")
    migrate_backup.add_argument("--backup", type=Path, required=True)
    migrate_backup.add_argument("--target-dir", type=Path, required=True)
    migrate_recover = sub.add_parser("migrate-recover", help="inspect or discard idle owned migration staging directories")
    migrate_recover.add_argument("--target-dir", type=Path, required=True)
    migrate_recover.add_argument("--cleanup", action="store_true")
    mairlist_preview = sub.add_parser("mairlist31-preview", help="read-only historical 20-column CSV preview")
    mairlist_preview.add_argument("--csv", type=Path, required=True)
    mairlist_preview.add_argument("--namespace", required=True)
    mairlist_preview.add_argument("--encoding", default="utf-8-sig")
    mairlist_apply = sub.add_parser("mairlist31-apply", help="import unambiguous CSV rows as review drafts")
    mairlist_apply.add_argument("--csv", type=Path, required=True)
    mairlist_apply.add_argument("--namespace", required=True)
    mairlist_apply.add_argument("--encoding", default="utf-8-sig")
    mairlist_apply.add_argument("--token", required=True)
    mairlist_apply.add_argument("--station-id", required=True)
    mairlist_apply.add_argument("--expected-sha256", required=True)
    mairlist_apply.add_argument("--idempotency-key", required=True)
    radio_preview = sub.add_parser("radioboss-preview", help="read-only plain M3U8 preview")
    radio_preview.add_argument("--playlist", type=Path, required=True)
    radio_apply = sub.add_parser("radioboss-apply", help="create a draft from reviewed M3U8")
    radio_apply.add_argument("--playlist", type=Path, required=True)
    radio_apply.add_argument("--token", required=True)
    radio_apply.add_argument("--station-id", required=True)
    radio_apply.add_argument("--namespace", required=True)
    radio_apply.add_argument("--expected-sha256", required=True)
    radio_apply.add_argument("--idempotency-key", required=True)
    radio_apply.add_argument("--service-date", required=True)
    radio_apply.add_argument("--start-at", required=True)
    radio_apply.add_argument("--bindings-json", type=Path, required=True)
    radio_apply.add_argument("--service-key", default="default")
    radio_apply.add_argument("--territory", default="*")
    radio_export = sub.add_parser("radioboss-export", help="loss-reported M3U8 text from approved plan")
    radio_export.add_argument("--token", required=True)
    radio_export.add_argument("--station-id", required=True)
    radio_export.add_argument("--plan-id", required=True)
    radio_export.add_argument("--revision-id", required=True)
    radio_export.add_argument("--paths-json", type=Path, required=True)
    libre_project = sub.add_parser("libretime-project", help="project an approved plan for LibreTime playout")
    libre_project.add_argument("--token", required=True)
    libre_project.add_argument("--station-id", required=True)
    libre_project.add_argument("--endpoint-id", required=True)
    libre_project.add_argument("--revision-id", required=True)
    libre_project.add_argument("--media-urls-json", type=Path, required=True)
    libre_project.add_argument("--show-name", default="RadioS")
    audio_render = sub.add_parser("audio-render", help="render an accepted endpoint plan to a new PCM WAV")
    audio_render.add_argument("--endpoint-dir", type=Path, required=True)
    audio_render.add_argument("--output", type=Path, required=True)
    audio_render.add_argument("--max-bytes", type=int, default=1_000_000_000)
    args = parser.parse_args(argv)
    try:
        if args.command.startswith("failover-"):
            from .failover import FailoverSupervisor, FailoverPolicy
            supervisor = FailoverSupervisor(args.db, args.standby_db, args.source_url, args.state_dir,
                policy=FailoverPolicy(failure_threshold=args.failure_threshold,
                    probe_timeout_seconds=args.probe_timeout, probe_interval_seconds=args.probe_interval,
                    cooldown_seconds=args.cooldown))
            if args.command == "failover-status":
                result = supervisor.status()
            elif args.command == "failover-reset":
                result = supervisor.reset()
            elif args.command == "failover-tick":
                result = supervisor.tick()
            elif args.command == "failover-serve":
                from .failover_server import serve_failover
                with _listener(args.host, args.port):
                    serve_failover(supervisor, args.host, args.port, max_connections=args.max_connections,
                                   request_timeout=args.request_timeout)
                return 0
            else:
                # Graceful stop: Local\RadioS-stop-<pid> (request_stop), Ctrl+C/Ctrl+Break or
                # console close.  closing() releases the monitor ownership on every exit.
                with _platform.StopRequest() as stop:
                    try:
                        last = None
                        with closing(supervisor.watch(stop.event)) as statuses:
                            for status in statuses:
                                signature = (status["state"], status.get("failures"),
                                             status.get("last_error"), status["active_database"])
                                if signature != last:
                                    print(json.dumps(status, ensure_ascii=False), flush=True)
                                    last = signature
                    finally:
                        stop.event.set()
                return 0
        elif args.command in {"replication-pull", "replication-follow"}:
            from .replication_stream import follow_once, follow_forever
            options = {"token": _environment_secret(args.token_env), "ca_file": args.ca_file,
                       "allow_loopback_http": args.allow_loopback_http, "max_records": args.max_records,
                       "max_batch_bytes": args.max_batch_bytes, "timeout": args.timeout}
            if args.command == "replication-pull":
                result = follow_once(args.db, args.source_url, **options)
            else:
                with _platform.StopRequest() as stop:
                    result = follow_forever(args.db, args.source_url, poll_interval=args.poll_interval,
                        stop_event=stop.event,
                        on_progress=lambda value: print(json.dumps(value, ensure_ascii=False), flush=True),
                        **options)
        elif args.command in {"pitr-preview", "pitr-restore"}:
            from .pitr import preview_recovery, recover_point_in_time
            targets = {"target_sequence": args.target_sequence, "target_time": args.target_time,
                       "inclusive": not args.exclusive}
            result = preview_recovery(args.backup, args.wal_dir, **targets) if args.command == "pitr-preview" \
                else recover_point_in_time(args.backup, args.wal_dir, args.target_dir, **targets)
        elif args.command == "replica-recover":
            from .replication import recover_standby_replay
            result = recover_standby_replay(args.db, args.bundle)
        elif args.command == "integrity":
            from .integrity import inspect_integrity
            result = inspect_integrity(args.db, max_records=args.max_records, max_bytes=args.max_bytes,
                                       verify_media=not args.skip_media, verify_cold=not args.skip_cold)
        elif args.command == "audio-render":
            from .audio_playout import render_active_plan_wav
            from .endpoint import EndpointAgent
            with EndpointAgent.open(args.endpoint_dir) as agent:
                result = render_active_plan_wav(agent, args.output,
                                               max_output_bytes=args.max_bytes)
        elif args.command == "backup-restore":
            from .backup import restore_backup
            result = restore_backup(args.backup, args.target_dir)
        elif args.command == "backup-verify":
            from .backup import verify_backup
            result = verify_backup(args.backup)
        elif args.command == "portable-verify":
            from .portable_export import verify_portable_export
            result = verify_portable_export(args.package)
        elif args.command == "portable-restore":
            from .portable_import import restore_portable_export
            result = restore_portable_export(args.package, args.target_dir)
        elif args.command == "migrate-preview":
            from .migration import preview_migration
            result = preview_migration(args.db)
        elif args.command == "migrate-copy":
            from .migration import migrate_copy
            result = migrate_copy(args.db, args.target_dir)
        elif args.command == "migrate-backup":
            from .migration import migrate_backup
            result = migrate_backup(args.backup, args.target_dir)
        elif args.command == "migrate-recover":
            from .migration import recover_migration
            result = recover_migration(args.target_dir, cleanup=args.cleanup)
        elif args.command == "mairlist31-preview":
            from .mairlist31_csv import preview_mairlist31_csv
            result = preview_mairlist31_csv(
                args.csv, source_namespace=args.namespace, encoding=args.encoding)
        elif args.command == "radioboss-preview":
            from .radioboss_m3u8 import preview_radioboss_m3u8
            result = preview_radioboss_m3u8(args.playlist)
        else:
            if args.db.suffix != ".sdb":
                raise StoreError("database filename must end in .sdb")
            metadata = args.db.with_name(args.db.name + ".meta")
            # Links include junctions and mount points (name-surrogate reparse points).
            if _platform.is_link(args.db) or _platform.is_link(metadata):
                raise StoreError("database path and metadata must not be symlinks")
            if args.command != "init" and (
                    not args.db.is_file() or not metadata.is_file()):
                raise StoreError("an existing .sdb file and its .meta file are required")
            store = open_radio_store(args.db, lock_timeout=args.lock_timeout,
                read_only=args.command in {"status", "metrics", "wal-status", "replica-status", "query", "index-status"})
            if args.command == "init":
                result = bootstrap(store, args.username, args.station_code,
                                   args.station_name, args.timezone)
            elif args.command == "serve":
                from .http_server import serve
                # serve() returns after a graceful stop request (named stop event,
                # Ctrl+C/Ctrl+Break, console close) once in-flight requests finished.
                with _listener(args.host, args.port):
                    serve(store, args.host, args.port, max_connections=args.max_connections,
                          request_timeout=args.request_timeout, ready=_print_listening)
                return 0
            elif args.command == "status":
                with store._locked(exclusive=False):
                    store._reload_locked()
                    result = {"file": str(store.journal), "sequence": store._sequence,
                              "node_role": store._meta().get("node_role"),
                              "collections": {name: len(rows) for name, rows in store._state.items()}}
            elif args.command == "metrics":
                from .monitoring import engine_status
                result = engine_status(store)
            elif args.command in {"wal-enable", "wal-capture", "wal-status"}:
                from .wal_archive import enable_wal_archive, capture_archive, archive_status
                result = {"wal-enable": enable_wal_archive, "wal-capture": capture_archive,
                          "wal-status": archive_status}[args.command](store)
            elif args.command == "replica-create":
                from .replication import create_standby
                result = create_standby(store, args.target_dir)
            elif args.command == "replica-status":
                from .replication import replication_status
                result = replication_status(store)
            elif args.command == "replica-catchup":
                from .replication import catch_up
                result = catch_up(store, open_radio_store(args.standby_db,
                    lock_timeout=args.lock_timeout), max_records=args.max_records)
            elif args.command == "replica-shipment":
                from .replication import create_shipment
                result = create_shipment(store, args.destination, after_sequence=args.after_sequence,
                    expected_previous_sha256=args.previous_sha256,
                    through_sequence=args.through_sequence, max_records=args.max_records)
            elif args.command == "replica-apply":
                from .replication import apply_shipment
                result = apply_shipment(store, args.bundle)
            elif args.command == "replica-promote":
                from .replication import promote_standby
                result = promote_standby(store, open_radio_store(args.standby_db,
                    lock_timeout=args.lock_timeout), expected_source_sequence=args.expected_sequence,
                    expected_source_timeline_id=args.expected_timeline)
            elif args.command == "query":
                with store.read_snapshot([args.collection]) as view:
                    result = (view.explain if args.explain else view.query)(
                        args.collection, **_json_map_file(args.query_json))
            elif args.command == "checkpoint":
                result = {"checkpoint_sequence": store.checkpoint()}
            elif args.command == "checkpoint-online":
                result = store.checkpoint_online(max_tail_bytes=args.max_tail_bytes,
                                                 max_tail_frames=args.max_tail_frames)
            elif args.command == "maintenance-recover":
                from .online_maintenance import recover_online_jobs
                result = recover_online_jobs(store, cleanup=args.cleanup)
            elif args.command == "maintenance-loop":
                from .online_maintenance import run_online_maintenance
                with _platform.StopRequest() as stop:
                    try:
                        with closing(run_online_maintenance(store, stop.event, interval_seconds=args.interval,
                                journal_threshold_bytes=args.journal_threshold_bytes,
                                max_tail_bytes=args.max_tail_bytes,
                                max_tail_frames=args.max_tail_frames)) as values:
                            for value in values:
                                print(json.dumps(value, ensure_ascii=False), flush=True)
                    finally:
                        stop.event.set()
                return 0
            elif args.command == "index-rebuild":
                from .disk_index import rebuild_disk_indexes
                result = rebuild_disk_indexes(store, collections=args.collection, max_rows=args.max_rows,
                    max_bytes=args.max_bytes, sort_chunk_rows=args.sort_chunk_rows, lock_timeout=args.lock_timeout)
            elif args.command == "index-status":
                from .disk_index import inspect_disk_indexes
                result = inspect_disk_indexes(store)
            elif args.command == "index-recover":
                from .disk_index import recover_disk_index_builds
                result = recover_disk_index_builds(store, cleanup=args.cleanup)
            elif args.command == "replication-serve":
                from .replication_stream import serve_replication
                with _listener(args.host, args.port):
                    serve_replication(store, host=args.host, port=args.port,
                        token=_environment_secret(args.token_env),
                        certfile=args.cert, keyfile=args.key, allow_loopback_http=args.allow_loopback_http,
                        max_records=args.max_records, max_batch_bytes=args.max_batch_bytes,
                        max_cache_bytes=args.max_cache_bytes, max_cached_batches=args.max_cached_batches,
                        ttl_seconds=args.ttl_seconds)
                return 0
            elif args.command == "retention-preview":
                from .retention import preview_retention
                result = preview_retention(store, authenticate(store, args.token),
                    keep_days=_json_map_file(args.policy_json) if args.policy_json else None,
                    as_of=args.as_of, max_records=args.max_records)
            elif args.command == "retention-apply":
                from .retention import apply_retention
                result = apply_retention(store, authenticate(store, args.token),
                                         _json_map_file(args.preview_json))
            elif args.command == "retention-query":
                from .retention import query_cold_history
                result = query_cold_history(store, authenticate(store, args.token), args.collection,
                    record_id=args.record_id, segment_id=args.segment_id, limit=args.limit, offset=args.offset)
            elif args.command == "retention-verify":
                from .retention import verify_cold_history
                result = verify_cold_history(store)
            elif args.command == "endpoint-token":
                principal = authenticate(store, args.token)
                result = {"endpoint_token": issue_endpoint_token(
                    store, principal, args.station_id, args.endpoint_id, days=args.days)}
            elif args.command == "backup-create":
                from .backup import create_backup
                result = create_backup(store, args.destination)
            elif args.command == "provision-copy":
                from .store_provision import provision_store_copy
                result = provision_store_copy(store, args.target_dir)
            elif args.command == "portable-export":
                from .portable_export import create_portable_export
                principal = authenticate(store, args.token)
                result = create_portable_export(store, principal, args.destination)
            elif args.command == "mairlist31-apply":
                from .mairlist31_import import apply_mairlist31_csv
                principal = authenticate(store, args.token)
                result = apply_mairlist31_csv(
                    store, principal, args.station_id, args.csv,
                    source_namespace=args.namespace, encoding=args.encoding,
                    expected_source_sha256=args.expected_sha256,
                    idempotency_key=args.idempotency_key)
            elif args.command == "radioboss-apply":
                from .radioboss_m3u8_import import apply_radioboss_m3u8
                principal = authenticate(store, args.token)
                result = apply_radioboss_m3u8(
                    store, principal, args.station_id, args.playlist,
                    source_namespace=args.namespace,
                    expected_source_sha256=args.expected_sha256,
                    idempotency_key=args.idempotency_key,
                    service_date=args.service_date, start_at=args.start_at,
                    path_bindings=_json_map_file(args.bindings_json),
                    service_key=args.service_key, territory=args.territory)
            elif args.command == "radioboss-export":
                from .radioboss_m3u8_export import export_radioboss_m3u8
                principal = authenticate(store, args.token)
                result = export_radioboss_m3u8(
                    store, principal, args.station_id,
                    args.plan_id, args.revision_id,
                    _json_map_file(args.paths_json))
            elif args.command == "libretime-project":
                from .libretime_adapter import project_plan_for_libretime
                principal = authenticate(store, args.token)
                result = project_plan_for_libretime(
                    store, principal, args.station_id, args.endpoint_id,
                    args.revision_id, media_urls=_json_map_file(args.media_urls_json),
                    show_name=args.show_name)
            else:
                raise StoreError("unsupported command")
        print(json.dumps(result, ensure_ascii=False, default=str, indent=2))
        if args.command == "integrity" and result["status"] != "ok":
            return 2
        return 0
    except (StoreError, EndpointError, OSError, ValueError) as exc:
        print(f"sdbase: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
