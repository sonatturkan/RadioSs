#!/usr/bin/env python3
"""Operational HTTP/resource/migration checks using only temporary databases."""

import fcntl
import hashlib
from http.client import HTTPConnection
import json
import multiprocessing
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))


def _hold_lock(path, ready, release):
    with open(path, "a+b") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        ready.set()
        release.wait(10)


def main():
    from sdbase import schema
    from sdbase.api_server import BoundedHTTPServer
    from sdbase.endpoint_protocol import TransportError
    from sdbase.http_server import make_handler
    from sdbase.identity import bootstrap, issue_endpoint_token, authenticate
    from sdbase.migration import migrate_copy
    from sdbase.storage import LockTimeout, _atomic_json
    from sdbase.transport import HTTPTransport
    from sdbase import wal_archive

    checks = []
    started = time.monotonic()
    def check(name, condition):
        if not condition:
            raise AssertionError(name)
        checks.append(name)

    with tempfile.TemporaryDirectory(prefix="sdbase-engine-acceptance-") as temporary:
        root = Path(temporary)
        store = schema.open_radio_store(root / "center.sdb", lock_timeout=0.15)
        identity = bootstrap(store, "admin", "RAD", "RadioS", "UTC")
        base = make_handler(store)
        class Handler(base):
            def log_message(self, *_args):
                pass
        server = BoundedHTTPServer(("127.0.0.1", 0), Handler,
                                   max_connections=2, request_timeout=1)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = f"http://127.0.0.1:{server.server_port}"
        def wait_slots(max_active):
            deadline = time.monotonic() + 2
            while server.stats()["active_connections"] > max_active:
                if time.monotonic() >= deadline:
                    raise AssertionError("server did not release a closed connection")
                time.sleep(0.005)
        transport = HTTPTransport(url, identity["api_token"], timeout=0.5)
        second = HTTPConnection("127.0.0.1", server.server_port, timeout=1)
        try:
            for _ in range(8):
                transport._json("GET", "/health")
            check("persistent endpoint transport reuses one connection",
                  transport.pool.stats()["created"] == 1 and transport.pool.stats()["reused"] == 7)
            metrics = transport._json("GET", "/v1/operations/metrics")
            check("global manager reads process/storage/role/server metrics",
                  metrics["node"]["node_role"] == "primary" and metrics["http_server"]["max_connections"] == 2)
            token = issue_endpoint_token(store, authenticate(store, identity["api_token"]),
                                         identity["station_id"], identity["endpoint_id"])
            transport.token = token
            try:
                transport._json("GET", "/v1/operations/metrics")
                raise AssertionError("endpoint must not read whole-store statistics")
            except TransportError as exc:
                check("endpoint credential cannot read whole-store metrics", "403" in str(exc))
            transport.token = identity["api_token"]
            transport._json("GET", "/health")
            second.request("GET", "/health")
            second.getresponse().read()
            third = HTTPConnection("127.0.0.1", server.server_port, timeout=1)
            try:
                third.request("GET", "/health")
                response = third.getresponse()
                check("idle keepalive sockets count against admission limit", response.status == 503)
                response.read()
            finally:
                third.close()
            check("listener records capacity rejection and bounded peak",
                  server.stats()["capacity_rejections"] >= 1 and server.stats()["peak_connections"] == 2)
            second.close()
            deadline = time.monotonic() + 2
            while server.stats()["active_connections"] > 1 and time.monotonic() < deadline:
                time.sleep(0.005)
            with HTTPTransport(url, identity["api_token"], timeout=0.1, max_connections=1) as single:
                held = single._open("GET", "/health")
                try:
                    single._open("GET", "/health")
                    raise AssertionError("pool checkout must be bounded")
                except TransportError:
                    check("client pool checkout timeout opens no extra connection", single.pool.stats()["created"] == 1)
                held.read()
                held.close()
                single._json("GET", "/health")
                check("consuming a held response releases its connection", single.pool.stats()["reused"] == 1)
                response = single._open("GET", "/health")
                response.read(3)
                response.close()
                wait_slots(1)
                single._json("GET", "/health")
                check("partial response close discards socket instead of reusing unread bytes",
                      single.pool.stats()["discarded"] >= 1 and single.pool.stats()["created"] == 2)
            deadline = time.monotonic() + 2
            while server.stats()["active_connections"] > 1 and time.monotonic() < deadline:
                time.sleep(0.005)
            raw = socket.create_connection(("127.0.0.1", server.server_port), timeout=1)
            try:
                raw.sendall(b"POST /v1/items HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n"
                            b"Content-Length: 2\r\n\r\n{}GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
                received = b""
                while True:
                    piece = raw.recv(4096)
                    if not piece:
                        break
                    received += piece
                check("duplicate body framing rejected and connection closed before queued request",
                      received.startswith(b"HTTP/1.1 422") and received.count(b"HTTP/1.1") == 1)
            finally:
                raw.close()
            wait_slots(1)
            before = store.sequence
            payload = json.dumps({"station_id": identity["station_id"],
                                  "kind": "music", "title": "short body"}).encode()
            raw = socket.create_connection(("127.0.0.1", server.server_port), timeout=1)
            try:
                headers = ("POST /v1/items HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer " +
                           identity["api_token"] + "\r\nContent-Length: " + str(len(payload) + 5) + "\r\n\r\n")
                raw.sendall(headers.encode() + payload)
                raw.shutdown(socket.SHUT_WR)
                received = b""
                while True:
                    piece = raw.recv(4096)
                    if not piece:
                        break
                    received += piece
                check("valid JSON with truncated HTTP body is refused without commit",
                      received.startswith(b"HTTP/1.1 422") and store.sequence == before)
            finally:
                raw.close()
            wait_slots(1)
            raw = socket.create_connection(("127.0.0.1", server.server_port), timeout=1)
            try:
                raw.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n"
                            b"{}GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
                received = b""
                while True:
                    piece = raw.recv(4096)
                    if not piece:
                        break
                    received += piece
                check("GET bodies cannot enter the next persistent request",
                      received.startswith(b"HTTP/1.1 422") and received.count(b"HTTP/1.1") == 1)
            finally:
                raw.close()
            wait_slots(1)
            wal_archive.enable_wal_archive(store)
            previous = store.sequence
            original_append = wal_archive._append_locked
            def fail_archive(*_args, **_kwargs):
                raise OSError("injected postcommit archive failure")
            wal_archive._append_locked = fail_archive
            try:
                response = transport._open("POST", "/v1/items", {
                    "station_id": identity["station_id"], "kind": "music", "title": "durable unack"})
                raise AssertionError("transport must surface non-2xx response")
            except TransportError as exc:
                check("durable archive failure is an HTTP 503 outcome", "503" in str(exc))
            finally:
                wal_archive._append_locked = original_append
            check("transport never automatically retries an ambiguous committed write",
                  store.sequence == previous + 1 and len(store.scan("items")) == 1)
            store.checkpoint()
            meta = store._meta()
            meta["node_role"] = "fenced"
            _atomic_json(store.meta_file, meta)
            transport._json("GET", "/health")
            try:
                transport._json("POST", "/v1/plans", {"station_id": identity["station_id"],
                                                        "service_date": "2026-10-02"})
                raise AssertionError("fenced HTTP writes must be refused")
            except TransportError as exc:
                check("fenced node remains readable and HTTP mutators return 503", "503" in str(exc))
        finally:
            second.close()
            transport.close()
            server.shutdown()
            server.server_close()
            thread.join(2)

        lock_store = schema.open_radio_store(root / "lock.sdb", lock_timeout=0.05)
        ready, release = multiprocessing.Event(), multiprocessing.Event()
        process = multiprocessing.Process(target=_hold_lock, args=(str(lock_store.lock_file), ready, release))
        process.start()
        try:
            if not ready.wait(2):
                raise AssertionError("child did not acquire database lock")
            before = lock_store.journal.stat().st_size
            try:
                lock_store.checkpoint()
                raise AssertionError("lock wait must time out")
            except LockTimeout:
                check("separate-process lock wait times out without storage mutation",
                      lock_store.journal.stat().st_size == before and not lock_store.checkpoint_file.exists())
        finally:
            release.set()
            process.join(2)
        lock_store.checkpoint()
        check("lock becomes usable after peer releases it", lock_store.metrics.snapshot()["counters"]["lock_timeouts"] == 1)

        for version in (1, 2, 3):
            original_schema = schema.SCHEMA_VERSION
            try:
                schema.SCHEMA_VERSION = version
                legacy = schema.open_radio_store(root / f"legacy{version}" / "RadioS.sdb")
                bootstrap(legacy, "legacy", "OLD", "Old radio", "UTC")
                legacy.checkpoint()
            finally:
                schema.SCHEMA_VERSION = original_schema
            meta = legacy._meta()
            for name in ("node_id", "node_role", "timeline_id", "epoch"):
                meta.pop(name, None)
            _atomic_json(legacy.meta_file, meta)
            paths = [legacy.journal, legacy.meta_file, legacy.checkpoint_file]
            before = [hashlib.sha256(path.read_bytes()).hexdigest() for path in paths]
            result = migrate_copy(legacy.journal, root / f"migrated{version}")
            current = schema.open_radio_store(Path(result["path"]) / result["database"], read_only=True)
            check(f"legacy schema {version} migrates to role-aware schema4 without source writes",
                  current.schema_version == 4 and current._meta()["node_role"] == "primary" and
                  before == [hashlib.sha256(path.read_bytes()).hexdigest() for path in paths])
    print(json.dumps({"format": "sdbase-engine-acceptance-v1", "passed": len(checks), "failed": 0,
                      "elapsed_seconds": round(time.monotonic() - started, 3), "checks": checks,
                      "scope": "temporary stores, loopback HTTP and real local lock process"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
