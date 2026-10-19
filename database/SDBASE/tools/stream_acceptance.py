#!/usr/bin/env python3
"""Isolated HTTP/TLS replication, outage, byte-resume and follower acceptance.

All databases, keys, bearer credentials and worker processes are temporary.
The public runner accepts only a report path, never a production database.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from uuid import uuid4


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
TOKEN_ENV = "SDBASE_STREAM_ACCEPTANCE_BEARER"


def worker(kind, config_path):
    from sdbase.schema import open_radio_store
    from sdbase.storage import _atomic_json
    from sdbase.replication_stream import make_replication_server, follow_forever
    config = json.loads(Path(config_path).read_bytes())
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_args: stop.set())
    if kind == "follower":
        follow_forever(Path(config["standby"]), config["url"], token=os.environ[TOKEN_ENV],
            ca_file=config.get("ca_file"), allow_loopback_http=config.get("allow_http", False),
            max_records=config.get("max_records", 3), poll_interval=0.05, timeout=0.3,
            stop_event=stop, on_progress=lambda value: _atomic_json(Path(config["progress"]), value))
        return
    server = make_replication_server(open_radio_store(config["source"]),
        host="127.0.0.1", port=config.get("port", 0), token=os.environ[TOKEN_ENV],
        certfile=config.get("certfile"), keyfile=config.get("keyfile"),
        allow_loopback_http=config.get("allow_http", False), max_records=3,
        max_cached_batches=config.get("max_cached_batches", 4),
        ttl_seconds=config.get("ttl_seconds", 120), request_timeout=1)
    import sdbase.wal_archive as wal
    def forbidden_history_scan(*_args):
        raise AssertionError("stream range must not enumerate or read complete WAL history")
    wal._record_paths = forbidden_history_scan
    if config.get("forbid_predecessor"):
        sequence = config["forbid_predecessor"]
        original_bounded, original_stamp = wal._bounded_envelope, wal.archive_anchor_stamp
        def bounded(path, max_bytes):
            if path.name == f"{sequence:020d}.json":
                raise AssertionError("pinned predecessor must not allocate its JSON document")
            return original_bounded(path, max_bytes)
        def stamp(store, number, **options):
            if number == sequence:
                raise AssertionError("durable timestamp pin must avoid rescanning its predecessor")
            return original_stamp(store, number, **options)
        wal._bounded_envelope, wal.archive_anchor_stamp = bounded, stamp
    if config.get("cut_once"):
        marker = Path(config["cut_once"])
        original = server.RequestHandlerClass
        class CutWriter:
            def __init__(self, writer, request):
                self.writer, self.request = writer, request
            def __getattr__(self, name):
                return getattr(self.writer, name)
            def write(self, block):
                if not marker.exists():
                    self.writer.write(block[:65536])
                    self.writer.flush()
                    _atomic_json(marker, {"cut_bytes": min(65536, len(block))})
                    self.request.shutdown(socket.SHUT_RDWR)
                    raise ConnectionResetError("acceptance cut after body prefix")
                return self.writer.write(block)
        class CutHandler(original):
            def end_headers(self):
                super().end_headers()
                if "/files/" in self.path and not marker.exists():
                    self.wfile = CutWriter(self.wfile, self.connection)
        server.RequestHandlerClass = CutHandler
    if config.get("hold_builder"):
        server.cache.build_slot.acquire()
    thread = threading.Thread(target=server.serve_forever,
                              kwargs={"poll_interval": 0.05}, daemon=True)
    thread.start()
    scheme = "https" if config.get("certfile") else "http"
    _atomic_json(Path(config["ready"]), {"url": f"{scheme}://127.0.0.1:{server.server_port}"})
    stop.wait()
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)


class Run:
    def __init__(self, root):
        self.root = Path(root)
        self.token = secrets.token_urlsafe(40)
        self.checks = []
        self.workers = []
        self.counter = 0

    def check(self, label, value=True):
        if not value:
            raise AssertionError(label)
        self.checks.append(label)

    def denied(self, label, call, expected=None):
        from sdbase.storage import StoreError
        try:
            call()
        except expected or StoreError:
            self.check(label)
        else:
            raise AssertionError(label)

    def start(self, kind, config):
        from sdbase.storage import _atomic_json
        self.counter += 1
        path = self.root / f"worker-{self.counter}.json"
        _atomic_json(path, config)
        os.chmod(path, 0o600)
        process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
            "--worker", kind, "--config", str(path)], stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, env=os.environ | {TOKEN_ENV: self.token, "PYTHONDONTWRITEBYTECODE": "1"})
        self.workers.append(process)
        return process

    def source_server(self, source, **options):
        ready = self.root / f"ready-{self.counter}.json"
        process = self.start("server", {"source": str(source), "ready": str(ready), **options})
        deadline = time.monotonic() + 10
        while not ready.exists():
            if process.poll() is not None:
                raise AssertionError("source worker failed: " + process.stderr.read().decode())
            if time.monotonic() > deadline:
                raise AssertionError("source worker did not become ready")
            time.sleep(0.02)
        return process, json.loads(ready.read_bytes())["url"]

    @staticmethod
    def stop(process, *, kill=False):
        if process.poll() is None:
            process.kill() if kill else process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)

    def await_check(self, label, call):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if call():
                self.check(label)
                return
            time.sleep(0.03)
        raise AssertionError(label)

    def close(self):
        for process in self.workers:
            self.stop(process)
            if process.stderr:
                process.stderr.close()

    def execute(self):
        from sdbase.schema import open_radio_store
        from sdbase.identity import bootstrap, authenticate
        from sdbase.catalog import create_item, create_version
        from sdbase.media import stage_media, object_path
        from sdbase.replication import create_standby, replication_status
        from sdbase.retention import preview_retention, apply_retention, verify_cold_history
        from sdbase.replication_stream import (_Client, _bound_identity, _follower_lock, _workspace,
            StreamProtocolError, StreamUnavailable, make_replication_server, follow_once)

        primary = open_radio_store(self.root / "source" / "RadioS.sdb")
        identity = bootstrap(primary, "stream-acceptance", "ST", "Stream acceptance")
        principal = authenticate(primary, identity["api_token"])
        create_standby(primary, self.root / "replica")
        database = self.root / "replica" / "RadioS.sdb"
        replica = open_radio_store(database)
        self.denied("listener requires TLS by default", lambda: make_replication_server(primary, port=0, token=self.token))
        self.denied("plaintext listener rejects non-loopback binding", lambda: make_replication_server(primary,
            host="0.0.0.0", port=0, token=self.token, allow_loopback_http=True))
        self.denied("plaintext upstream rejects non-loopback origin", lambda: follow_once(database,
            "http://192.0.2.1:8443", token=self.token, allow_loopback_http=True))
        self.denied("weak bearer token rejected", lambda: make_replication_server(primary, port=0,
            token="weak", allow_loopback_http=True))
        marker = self.root / "cut-once.json"
        server, url = self.source_server(primary.journal, allow_http=True, cut_once=str(marker))
        client = _Client(url, self.token, allow_loopback_http=True, timeout=2)
        self.denied("wrong bearer token rejected", lambda: _Client(url, "w" * 48,
            allow_loopback_http=True).json("/replication/v1/status"), StreamProtocolError)
        self.denied("second source listener lease rejected", lambda: make_replication_server(primary,
            port=0, token=self.token, allow_loopback_http=True), StreamUnavailable)
        self.check("authenticated HTTP source identity pinned", client.json("/replication/v1/status")["source_node_id"] == primary._meta()["node_id"])
        item = create_item(primary, principal, identity["station_id"], kind="music", title="streamed media")
        version = create_version(primary, principal, identity["station_id"], item["_id"], label="source")
        source_bytes = self.root / "audio.bin"
        source_bytes.write_bytes(b"stream-media-integrity" * 50000)
        media = stage_media(primary, principal, identity["station_id"], version["_id"], source_bytes, [self.root])
        before = replica.sequence
        self.denied("interrupted body transfer is retryable", lambda: follow_once(database, url,
            token=self.token, max_records=3, allow_loopback_http=True), StreamUnavailable)
        self.check("interrupted transfer never advances replay", replica.sequence == before)
        partials = list((database.parent / (database.name + ".replication-stream") / "incoming").rglob("*.part"))
        self.check("partial media prefix retained for resume", bool(partials) and sum(path.stat().st_size for path in partials) >= 65536)
        port = int(url.rsplit(":", 1)[1])
        self.stop(server, kill=True)
        server, same_url = self.source_server(primary.journal, allow_http=True, port=port)
        self.check("source process restart preserves origin", same_url == url)
        result = follow_once(database, url, token=self.token, max_records=3, allow_loopback_http=True)
        self.check("HTTP Range resumes cached prefix after source restart", result["resumed_bytes"] >= 65536)
        self.check("post-resume verified replay catches up", replica.sequence == primary.sequence)
        self.check("incremental transport works with full-history enumeration forbidden")
        self.check("streamed managed media bytes equal original", object_path(replica, media["sha256"]).read_bytes() == source_bytes.read_bytes())
        self.check("sender ACK releases sealed batch", client.json("/replication/v1/status")["cache"]["batches"] == 0)
        self.check("standby remains read-only role", replica._meta()["node_role"] == "standby")
        for index in range(3):
            primary.put("items", str(uuid4()), {"kind": "music", "title": f"bounded {index}", "lifecycle": "draft"})
        result = follow_once(database, url, token=self.token, max_records=1, allow_loopback_http=True)
        self.check("requested record budget enforced", result["applied"] == 1 and result["lag_transactions"] == 2)
        while replica.sequence < primary.sequence:
            follow_once(database, url, token=self.token, max_records=1, allow_loopback_http=True)
        from sdbase.wal_archive import archive_root, read_archive_records, WalArchiveError
        older = archive_root(primary) / "records" / f"{2:020d}.json"
        saved_older = older.read_bytes()
        try:
            older.write_bytes(saved_older + b"broken-older-history")
            primary.put("items", str(uuid4()), {"kind": "music", "title": "bounded historical range", "lifecycle": "draft"})
            result = follow_once(database, url, token=self.token, allow_loopback_http=True)
            self.check("pinned new range does not reread unrelated older record", result["applied"] == 1)
            self.denied("global archive verification still detects older damage", lambda: read_archive_records(archive_root(primary)), WalArchiveError)
        finally:
            older.write_bytes(saved_older)
        before_gap = replica.sequence
        for index in range(3):
            primary.put("items", str(uuid4()), {"kind": "music", "title": f"range gap {index}", "lifecycle": "draft"})
        middle = archive_root(primary) / "records" / f"{before_gap+2:020d}.json"
        withheld = middle.with_name(middle.name + ".withheld")
        middle.rename(withheld)
        try:
            self.denied("selected range missing middle frame rejected", lambda: follow_once(database, url,
                token=self.token, max_records=3, allow_loopback_http=True), StreamProtocolError)
            self.check("selected gap leaves replay position unchanged", replica.sequence == before_gap)
        finally:
            withheld.rename(middle)
        follow_once(database, url, token=self.token, max_records=3, allow_loopback_http=True)
        preview = preview_retention(primary, principal, keep_days={"_audit": 1},
            as_of=datetime.now(timezone.utc) + timedelta(days=3))
        apply_retention(primary, principal, preview)
        follow_once(database, url, token=self.token, allow_loopback_http=True)
        self.check("retention checkpoint and cold bytes streamed", verify_cold_history(replica)["segments"] == 1)
        pinned = _bound_identity(replica)
        progress = replication_status(replica)["replication"]
        request = {**pinned, "after_sequence": progress["last_replayed_sequence"],
            "previous_sha256": progress["last_record_sha256"],
            "previous_effective_at": progress["last_effective_at"], "max_records": 1, "max_bytes": 64 * 1024 * 1024}
        foreign = dict(request, source_node_id=str(uuid4()))
        self.denied("foreign source binding refused before shipping", lambda: client.json("/replication/v1/batches",
            method="POST", body=foreign), StreamProtocolError)
        bad_anchor = dict(request, previous_sha256="0" * 64)
        self.denied("resume hash divergence refused", lambda: client.json("/replication/v1/batches",
            method="POST", body=bad_anchor), StreamProtocolError)
        with _follower_lock(_workspace(database)):
            self.denied("simultaneous follower step rejected", lambda: follow_once(database, url,
                token=self.token, allow_loopback_http=True), StreamUnavailable)
        primary.put("items", str(uuid4()), {"kind": "music", "title": "tamper test", "lifecycle": "draft"})
        offer = client.json("/replication/v1/batches", method="POST", body=request)
        cache = primary.root / (primary.journal.name + ".replication-stream-cache") / offer["batch_id"]
        frame_file = cache / "records.json"
        original = frame_file.read_bytes()
        changed = bytearray(original)
        changed[len(changed) // 2] ^= 1
        frame_file.write_bytes(changed)
        before = replica.sequence
        self.denied("same-length streamed tamper rejected by SHA", lambda: follow_once(database, url,
            token=self.token, max_records=1, allow_loopback_http=True), StreamProtocolError)
        self.check("tampered stream never advances standby", replica.sequence == before)
        frame_file.write_bytes(original)
        follow_once(database, url, token=self.token, max_records=1, allow_loopback_http=True)
        self.stop(server)
        cert, key = self.root / "tls-cert.pem", self.root / "tls-key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost",
            "-keyout", str(key), "-out", str(cert)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.chmod(key, 0o600)
        server, tls_url = self.source_server(primary.journal, certfile=str(cert), keyfile=str(key))
        self.denied("untrusted TLS certificate rejected", lambda: follow_once(database, tls_url,
            token=self.token), StreamProtocolError)
        primary.put("items", str(uuid4()), {"kind": "music", "title": "TLS bytes", "lifecycle": "draft"})
        result = follow_once(database, tls_url, token=self.token, ca_file=cert)
        self.check("real verified TLS transfer applies delta", result["applied"] == 1 and replica.sequence == primary.sequence)
        progress_path = self.root / "follower-progress.json"
        follower = self.start("follower", {"standby": str(database), "url": tls_url,
            "ca_file": str(cert), "progress": str(progress_path)})
        self.await_check("continuous follower starts caught up", lambda: progress_path.exists() and
            json.loads(progress_path.read_bytes()).get("state") == "caught_up")
        self.stop(server, kill=True)
        primary.put("items", str(uuid4()), {"kind": "music", "title": "while disconnected", "lifecycle": "draft"})
        self.await_check("continuous follower retries real source outage", lambda: progress_path.exists() and
            json.loads(progress_path.read_bytes()).get("state") == "retrying")
        self.check("outage never auto-promotes replica", replica._meta()["node_role"] == "standby")
        tls_port = int(tls_url.rsplit(":", 1)[1])
        server, returned_url = self.source_server(primary.journal, certfile=str(cert), keyfile=str(key), port=tls_port)
        self.check("TLS source restarts at pinned origin", returned_url == tls_url)
        self.await_check("continuous follower resumes after source restart", lambda: replica.sequence == primary.sequence)
        self.stop(follower)
        self.check("continuous follower exits cleanly on signal", follower.returncode == 0)
        primary.put("items", str(uuid4()), {"kind": "music", "title": "follower restarted", "lifecycle": "draft"})
        restarted = self.start("follower", {"standby": str(database), "url": tls_url,
            "ca_file": str(cert), "progress": str(progress_path)})
        self.await_check("new follower process resumes durable replay position", lambda: replica.sequence == primary.sequence)
        self.stop(restarted)
        self.check("restarted follower preserves standby role and epoch", replica._meta()["node_role"] == "standby" and replica._meta()["epoch"] == 1)
        primary.put("items", str(uuid4()), {"kind": "music", "title": "b" * 70000, "lifecycle": "draft"})
        before = replica.sequence
        self.denied("single oversized record cannot bypass byte budget", lambda: follow_once(database,
            tls_url, token=self.token, ca_file=cert, max_batch_bytes=65536), StreamProtocolError)
        self.check("byte-budget refusal leaves durable replay unchanged", replica.sequence == before)
        follow_once(database, tls_url, token=self.token, ca_file=cert)
        self.stop(server)
        server, tls_url = self.source_server(primary.journal, certfile=str(cert), keyfile=str(key),
            max_cached_batches=1, ttl_seconds=0.3)
        secure = _Client(tls_url, self.token, ca_file=cert)
        progress = replication_status(replica)["replication"]
        request.update(after_sequence=progress["last_replayed_sequence"], previous_sha256=progress["last_record_sha256"],
                       previous_effective_at=progress["last_effective_at"])
        one = secure.json("/replication/v1/batches", method="POST", body=request)
        secure.json("/replication/v1/batches", method="POST", body=dict(request, max_records=2))
        self.check("sender cache count bounded with eviction", secure.json("/replication/v1/status")["cache"]["batches"] == 1)
        self.denied("evicted batch cannot serve files", lambda: secure.open(
            f"/replication/v1/batches/{one['batch_id']}/files/records.json"), StreamUnavailable)
        time.sleep(0.5)
        self.check("TTL removes unacknowledged cached transfers", secure.json("/replication/v1/status")["cache"]["batches"] == 0)
        status = secure.json("/replication/v1/status")
        self.check("cache bytes remain within configured budget", status["cache"]["bytes"] <= status["limits"]["max_cache_bytes"])
        self.stop(server)
        server, tls_url = self.source_server(primary.journal, certfile=str(cert), keyfile=str(key), hold_builder=True)
        secure = _Client(tls_url, self.token, ca_file=cert)
        self.denied("busy bounded builder returns retryable backpressure", lambda: secure.json(
            "/replication/v1/batches", method="POST", body=dict(request, max_records=3)), StreamUnavailable)
        self.stop(server)
        server, url = self.source_server(primary.journal, allow_http=True)
        legacy = replica._meta()
        legacy["replication"].pop("last_effective_at")
        from sdbase.storage import _atomic_json
        _atomic_json(replica.meta_file, legacy)
        before = replica.sequence
        follow_once(replica, url, token=self.token, allow_loopback_http=True)
        self.check("0.7 standby gains exact timestamp pin without semantic sequence change",
            replica.sequence == before and "last_effective_at" in replica._meta()["replication"])
        upgraded = _Client(url, self.token, allow_loopback_http=True)
        self.denied("legacy anchor upgrade rejects divergent digest", lambda: upgraded.json(
            "/replication/v1/anchor", method="POST", body={**pinned, "sequence": before, "sha256": "0" * 64}), StreamProtocolError)
        self.denied("batch without timestamp pin is refused", lambda: upgraded.json(
            "/replication/v1/batches", method="POST",
            body={key: value for key, value in request.items() if key != "previous_effective_at"}), StreamProtocolError)
        self.stop(server)
        from sdbase.storage import Conflict, _json_bytes
        from sdbase.replication import create_shipment, _read_shipment, _digest, apply_shipment
        from sdbase.backup import _write_manifest
        import hashlib
        progress = replica._meta()["replication"]
        primary.put("items", str(uuid4()), {"kind": "music", "title": "manual timestamp invariant", "lifecycle": "draft"})
        manual = self.root / "legacy-manual-shipment"
        create_shipment(primary, manual, after_sequence=replica.sequence,
                        expected_previous_sha256=progress["last_record_sha256"])
        directory, manifest, records = _read_shipment(manual)
        records[0]["payload"]["effective_at"] = progress["last_effective_at"]
        records[0]["sha256"] = _digest(records[0]["payload"])
        encoded = _json_bytes(records)
        (directory / "records.json").write_bytes(encoded)
        manifest["last_record_sha256"] = records[0]["sha256"]
        next(row for row in manifest["files"] if row["path"] == "records.json").update(
            sha256=hashlib.sha256(encoded).hexdigest(), size_bytes=len(encoded))
        (directory / "manifest.json").unlink()
        (directory / "manifest.sha256").unlink()
        _write_manifest(directory, manifest)
        self.denied("old manual manifest cannot regress a known durable timestamp pin",
            lambda: apply_shipment(replica, manual), Conflict)
        self.large_anchor()

    def large_anchor(self):
        """Exercise >256 MiB canonical checkpoint fixtures, not a scale benchmark."""
        import hashlib
        import tracemalloc
        from sdbase.schema import open_radio_store
        from sdbase.storage import _atomic_json, _json_bytes
        from sdbase.replication import create_standby
        from sdbase.replication_stream import follow_once
        from sdbase.wal_archive import enable_wal_archive, archive_anchor_stamp

        primary = open_radio_store(self.root / "large-source" / "RadioS.sdb")
        enable_wal_archive(primary)
        primary.put("items", str(uuid4()), {"kind": "music", "title": "large anchor base", "lifecycle": "draft"})
        primary.checkpoint()
        checkpoint = json.loads(primary.checkpoint_file.read_bytes())["payload"]
        archive = primary.root / (primary.journal.name + ".wal")
        path = archive / "records" / f"{primary.sequence:020d}.json"
        row = json.loads(path.read_bytes())["payload"]
        # The engine permits extra checkpoint metadata. A deterministic padding
        # field exercises physical checkpoint size without constructing hundreds
        # of millions of live records; domain state stays native and valid.
        marker = "stream-acceptance-large-envelope-padding"
        checkpoint["acceptance_padding"] = marker
        row.pop("transaction")
        row.update(kind="checkpoint", checkpoint=checkpoint)
        padding_bytes = 260 * 1024 * 1024
        block = b"x" * (256 * 1024)
        def seal(destination, payload):
            before, after = _json_bytes(payload).split(_json_bytes(marker), 1)
            digest = hashlib.sha256()
            with destination.open("wb") as output:
                output.write(b'{"payload":')
                for part in (before, b'"'):
                    digest.update(part)
                    output.write(part)
                remaining = padding_bytes
                while remaining:
                    part = block[:min(remaining, len(block))]
                    digest.update(part)
                    output.write(part)
                    remaining -= len(part)
                for part in (b'"', after):
                    digest.update(part)
                    output.write(part)
                checksum = digest.hexdigest()
                output.write(b',"sha256":"' + checksum.encode() + b'"}')
                output.flush()
                os.fsync(output.fileno())
            return checksum
        checksum = seal(path, row)
        seal(primary.checkpoint_file, checkpoint)
        _atomic_json(archive / "head.json", {"sequence": primary.sequence,
            "sha256": checksum, "effective_at": row["effective_at"]})
        primary.sequence  # Load physical recovery image outside measured helper.
        tracemalloc.start()
        stamp = archive_anchor_stamp(primary, primary.sequence, expected_sha256=checksum)
        _current, peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        self.check("260 MiB predecessor stamp verifies exact digest and timestamp",
            path.stat().st_size > 256 * 1024 * 1024 and stamp["sha256"] == checksum and stamp["effective_at"] == row["effective_at"])
        self.check("one-time giant anchor extraction uses less than 4 MiB traced allocations", peak < 4 * 1024 * 1024)
        create_standby(primary, self.root / "large-replica")
        replica = open_radio_store(self.root / "large-replica" / "RadioS.sdb")
        self.check("physical rebase persists giant predecessor exact timestamp",
            replica._meta()["replication"]["last_effective_at"] == stamp["effective_at"])
        old_sequence = primary.sequence
        primary.put("items", str(uuid4()), {"kind": "music", "title": "tiny delta after giant checkpoint", "lifecycle": "draft"})
        server, url = self.source_server(primary.journal, allow_http=True, forbid_predecessor=old_sequence)
        result = follow_once(replica, url, token=self.token, max_batch_bytes=65536, allow_loopback_http=True)
        self.check("64 KiB HTTP batch follows 260 MiB predecessor without JSON load or rescan",
            result["applied"] == 1 and replica.sequence == primary.sequence)
        self.stop(server)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--worker", choices=["server", "follower"], help=argparse.SUPPRESS)
    parser.add_argument("--config", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        worker(args.worker, args.config)
        return
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="sdbase-stream-acceptance-") as temporary:
        run = Run(temporary)
        try:
            run.execute()
            result = {"ok": True, "passed": len(run.checks), "checks": run.checks,
                "real_worker_processes": run.counter, "seconds": round(time.monotonic() - started, 3),
                "scope": "same_host_real_http_tls_transport_continuous_follower",
                "excluded": ["second_physical_host", "network_quorum", "automatic_network_failover"]}
        finally:
            run.close()
    encoded = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")


if __name__ == "__main__":
    main()
