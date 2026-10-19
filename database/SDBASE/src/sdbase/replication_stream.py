"""Authenticated, bounded HTTP transport for SDBASE's verified WAL shipments.

The follower applies one complete verified batch at a time and resumes partial
file transfers. It never promotes a standby or changes its upstream identity.
TLS certificate/hostname verification is mandatory outside explicitly enabled
loopback acceptance. This is independently written transport, not SQL or a
PostgreSQL/MariaDB replication protocol implementation.
"""

from __future__ import annotations

from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import shutil
import ssl
import tempfile
import threading
import time
from urllib.error import HTTPError, URLError
from urllib.parse import quote, unquote, urlsplit
from urllib.request import (HTTPRedirectHandler, HTTPSHandler, Request,
                            build_opener)

from .api_server import BoundedHTTPServer
from .backup import (_digest_file, _fsync_dir, _open_regular, _safe_source)
from .replication import (_asset_path, _identity, _json_unique,
                          _read_shipment, _sha, apply_shipment, create_shipment,
                          recover_standby_replay, replication_status)
from .schema import open_radio_store
from .storage import Conflict, Store, StoreError, _atomic_json, _json_bytes


PROTOCOL = "sdbase-replication-http-v1"
_HEX = re.compile(r"^[0-9a-f]{64}$")
_MIB = 1024 * 1024
_MANIFEST_MAX = 4 * _MIB
_BODY_MAX = 4096
_CHUNK = 256 * 1024


class StreamProtocolError(StoreError):
    """Permanent mismatch, unsafe transport, corrupt bytes or invalid limits."""


class StreamUnavailable(StoreError):
    """A transient outage, timeout or bounded-capacity rejection."""


def _token(value: str) -> str:
    if not isinstance(value, str) or not 32 <= len(value) <= 512 or any(
            not 33 <= ord(char) <= 126 for char in value):
        raise StreamProtocolError("replication bearer token must be 32..512 printable non-space ASCII characters")
    return value


def _limit(value, minimum, maximum, label):
    if type(value) is not int or not minimum <= value <= maximum:
        raise StreamProtocolError(f"{label} must be {minimum}..{maximum}")
    return value


def _seconds(value, maximum, label):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not 0 < value <= maximum:
        raise StreamProtocolError(f"{label} must be positive and at most {maximum} seconds")
    return float(value)


def _loopback(host: str) -> bool:
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def _source_identity(store: Store) -> dict:
    with store._locked(exclusive=False):
        store._reload_locked()
        meta = store._meta()
        if meta.get("node_role") != "primary":
            raise StreamUnavailable("upstream is no longer the writable primary")
        identifier, timeline, epoch = _identity(meta)
        return {"source_store_id": identifier, "source_node_id": meta["node_id"],
                "source_timeline_id": timeline, "source_epoch": epoch,
                "schema_version": store.schema_version, "database": store.journal.name}


def _bound_identity(standby: Store) -> dict:
    status = replication_status(standby)
    source = status.get("replication")
    if standby.read_only or status["node_role"] != "standby" or not isinstance(source, dict):
        raise StreamProtocolError("continuous follower requires an initialized writable-maintenance standby handle")
    return {"source_store_id": source["source_store_id"], "source_node_id": source["source_node_id"],
            "source_timeline_id": source["source_timeline_id"], "source_epoch": source["source_epoch"],
            "schema_version": standby.schema_version, "database": standby.journal.name}


def _current_source(store: Store, expected: dict) -> dict:
    current = _source_identity(store)
    if current != expected:
        raise StreamUnavailable("upstream identity changed; restart listener and provision the matching replica")
    return current


def _private_directory(path: Path, marker: dict) -> Path:
    if path.is_symlink():
        raise StreamProtocolError("replication transport directory must not be a symlink")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not path.is_dir():
        raise StreamProtocolError("replication transport path is not a directory")
    marker_path = _safe_source(path, "owner.json")
    if marker_path.exists():
        if _json_unique(marker_path.read_bytes()) != marker:
            raise StreamProtocolError("replication transport directory belongs to a different node")
    elif any(path.iterdir()):
        raise StreamProtocolError("existing transport directory has no SDBASE owner marker")
    else:
        _atomic_json(marker_path, marker)
        os.chmod(marker_path, 0o600)
    os.chmod(path, 0o700)
    return path


class _BatchCache:
    def __init__(self, store: Store, *, max_records: int, max_batch_bytes: int,
                 max_cache_bytes: int, max_cached_batches: int, ttl_seconds: float):
        self.store = store
        self.identity = _source_identity(store)
        self.max_records = _limit(max_records, 1, 10000, "max_records")
        self.max_batch_bytes = _limit(max_batch_bytes, 65536, 4 * 1024 * _MIB, "max_batch_bytes")
        self.max_cache_bytes = _limit(max_cache_bytes, self.max_batch_bytes, 64 * 1024 * _MIB, "max_cache_bytes")
        self.max_cached_batches = _limit(max_cached_batches, 1, 128, "max_cached_batches")
        self.ttl = _seconds(ttl_seconds, 86400, "ttl_seconds")
        self.root = _private_directory(store.root / (store.journal.name + ".replication-stream-cache"),
            {"format": PROTOCOL + "-sender-cache", "store_id": self.identity["source_store_id"],
             "node_id": self.identity["source_node_id"]})
        self.lease_fd = os.open(_safe_source(self.root, "server.lock"),
            os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0), 0o600)
        try:
            fcntl.flock(self.lease_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            os.close(self.lease_fd)
            self.lease_fd = None
            raise StreamUnavailable("another replication listener owns this source cache") from exc
        self.lock = threading.RLock()
        self.build_slot = threading.BoundedSemaphore(1)
        self.entries: dict[str, dict] = {}
        self.building: set[str] = set()
        self.closed = False
        try:
            self._load_and_sweep()
        except BaseException:
            fcntl.flock(self.lease_fd, fcntl.LOCK_UN)
            os.close(self.lease_fd)
            self.lease_fd = None
            raise

    def _load_and_sweep(self):
        for path in self.root.iterdir():
            if path.name in {"owner.json", "server.lock"}:
                continue
            if path.is_symlink() or not path.is_dir():
                raise StreamProtocolError("unexpected or linked component in sender cache")
            if _HEX.fullmatch(path.name):
                try:
                    _root, manifest, _records = _read_shipment(path)
                    size = sum(row["size_bytes"] for row in manifest["files"]) + len(_json_bytes(manifest)) + 65
                    created = datetime.fromisoformat(manifest["created_at"]).timestamp()
                    if any(manifest.get(key) != value for key, value in self.identity.items()) or \
                            size > self.max_batch_bytes or time.time() - created >= self.ttl:
                        shutil.rmtree(path)
                        continue
                    self.entries[path.name] = {"path": path, "manifest": manifest, "size": size,
                                               "touched": time.monotonic() - max(0, time.time() - created),
                                               "active": 0, "delete": False}
                except (StoreError, OSError, ValueError, KeyError):
                    # Every UUID/hash directory under this verified owner marker
                    # is a transport cache artifact, never the database itself.
                    shutil.rmtree(path)
            elif re.fullmatch(r"\.[0-9a-f]{64}\.[A-Za-z0-9_-]+\.partial", path.name):
                shutil.rmtree(path)  # Killed builder, no sealed batch to serve.
            else:
                raise StreamProtocolError("unrecognized sender cache directory")
        self.sweep()

    def _remove(self, identifier: str):
        entry = self.entries.get(identifier)
        if entry is None:
            return
        if entry["active"]:
            entry["delete"] = True
            return
        shutil.rmtree(entry["path"])
        self.entries.pop(identifier, None)

    def sweep(self):
        with self.lock:
            now = time.monotonic()
            for identifier, entry in list(self.entries.items()):
                if entry["delete"] or now - entry["touched"] >= self.ttl:
                    self._remove(identifier)
            while len(self.entries) > self.max_cached_batches or sum(row["size"] for row in self.entries.values()) > self.max_cache_bytes:
                victims = [(row["touched"], identifier) for identifier, row in self.entries.items() if not row["active"]]
                if not victims:
                    break
                self._remove(min(victims)[1])

    def status(self) -> dict:
        _current_source(self.store, self.identity)
        self.sweep()
        with self.lock:
            return {"protocol": PROTOCOL, **self.identity, "node_role": "primary",
                    "sequence": self.store.sequence,
                    "limits": {"max_records": self.max_records, "max_batch_bytes": self.max_batch_bytes,
                               "max_cache_bytes": self.max_cache_bytes, "max_cached_batches": self.max_cached_batches,
                               "ttl_seconds": self.ttl},
                    "cache": {"batches": len(self.entries), "bytes": sum(row["size"] for row in self.entries.values())}}

    def anchor(self, request: dict) -> dict:
        """Upgrade a legacy receiver once, with its existing digest pinned."""
        _current_source(self.store, self.identity)
        required = set(self.identity) | {"sequence", "sha256"}
        if not isinstance(request, dict) or set(request) != required or \
                any(request[key] != value for key, value in self.identity.items()):
            raise StreamProtocolError("anchor upgrade requires the pinned source identity")
        if not self.build_slot.acquire(blocking=False):
            raise StreamUnavailable("replication anchor scanner is busy")
        try:
            from .wal_archive import archive_anchor_stamp
            stamp = archive_anchor_stamp(self.store, request["sequence"], expected_sha256=request["sha256"])
            _current_source(self.store, self.identity)
            return {**self.identity, **stamp}
        finally:
            self.build_slot.release()

    def build(self, request: dict) -> dict:
        if self.closed:
            raise StreamUnavailable("replication listener is closing")
        _current_source(self.store, self.identity)
        required = set(self.identity) | {"after_sequence", "previous_sha256", "previous_effective_at", "max_records", "max_bytes"}
        if not isinstance(request, dict) or set(request) != required or any(request[key] != value for key, value in self.identity.items()):
            raise StreamProtocolError("batch request is bound to a different source identity")
        after = _limit(request["after_sequence"], 0, 2**63 - 1, "after_sequence")
        count = _limit(request["max_records"], 1, self.max_records, "requested max_records")
        budget = _limit(request["max_bytes"], 65536, self.max_batch_bytes, "requested max_bytes")
        if not _sha(request["previous_sha256"]):
            raise StreamProtocolError("batch request requires a previous archive SHA-256")
        from .wal_archive import _instant
        _instant(request["previous_effective_at"])
        identifier = hashlib.sha256(_json_bytes(request)).hexdigest()
        self.sweep()
        with self.lock:
            cached = self.entries.get(identifier)
            if cached and not cached["delete"]:
                cached["touched"] = time.monotonic()
                return {"batch_id": identifier, "manifest": cached["manifest"],
                        "manifest_sha256": hashlib.sha256(_json_bytes(cached["manifest"])).hexdigest()}
        if not self.build_slot.acquire(blocking=False):
            raise StreamUnavailable("replication builder is busy; retry after backpressure")
        target = self.root / identifier
        try:
            with self.lock:
                self.building.add(identifier)
            source_sequence = self.store.sequence
            if after > source_sequence:
                raise Conflict("follower is ahead of primary")
            from .wal_archive import iter_pinned_archive_records, WalRangeTooLarge
            records = iter_pinned_archive_records(self.store, after_sequence=after,
                through_sequence=min(source_sequence, after + count),
                expected_previous_sha256=request["previous_sha256"], max_records=count,
                expected_previous_effective_at=request["previous_effective_at"],
                max_record_bytes=min(budget, 256 * _MIB), verify_assets=False)
            through, json_size, assets = after, 2, {}
            while True:
                try:
                    record = next(records)
                except StopIteration:
                    break
                except WalRangeTooLarge:
                    if through == after:
                        raise
                    break
                if record["payload"]["sequence"] != through + 1:
                    raise StreamProtocolError("source WAL archive has an incremental sequence gap")
                candidate_assets = dict(assets)
                for asset in record["payload"]["assets"]:
                    _asset_path(self.identity["database"], asset)
                    if candidate_assets.setdefault(asset["path"], asset) != asset:
                        raise StreamProtocolError("source asset path has conflicting immutable versions")
                candidate_json = json_size + len(_json_bytes(record)) + (1 if through > after else 0)
                estimated = candidate_json + sum(row["size_bytes"] for row in candidate_assets.values()) + 32768 + 256 * len(candidate_assets)
                if estimated > budget:
                    if through == after:
                        raise StreamProtocolError("next complete record and its media exceed the batch byte budget; increase limits or rebase")
                    break
                through, json_size, assets = record["payload"]["sequence"], candidate_json, candidate_assets
            if source_sequence > after and through == after:
                raise StreamUnavailable("committed source records have not yet reached the WAL archive")
            estimated = json_size + sum(row["size_bytes"] for row in assets.values()) + 32768 + 256 * len(assets)
            with self.lock:
                while len(self.entries) >= self.max_cached_batches or \
                        sum(row["size"] for row in self.entries.values()) + estimated > self.max_cache_bytes:
                    victims = [(row["touched"], key) for key, row in self.entries.items() if not row["active"]]
                    if not victims:
                        raise StreamUnavailable("replication transfer cache is at its bounded capacity")
                    self._remove(min(victims)[1])
            create_shipment(self.store, target, after_sequence=after,
                expected_previous_sha256=request["previous_sha256"], through_sequence=through,
                expected_previous_effective_at=request["previous_effective_at"],
                max_records=count, verify_full_archive=False)
            _current_source(self.store, self.identity)
            if self.closed:
                raise StreamUnavailable("replication listener closed while preparing the bounded batch")
            _root, manifest, _records = _read_shipment(target)
            actual_size = sum(row["size_bytes"] for row in manifest["files"]) + len(_json_bytes(manifest)) + 65
            if actual_size > budget:
                shutil.rmtree(target)
                raise StreamProtocolError("actual shipment exceeds its total byte budget")
            with self.lock:
                self.entries[identifier] = {"path": target, "manifest": manifest, "size": actual_size,
                                            "touched": time.monotonic(), "active": 0, "delete": False}
            return {"batch_id": identifier, "manifest": manifest,
                    "manifest_sha256": hashlib.sha256(_json_bytes(manifest)).hexdigest()}
        finally:
            with self.lock:
                self.building.discard(identifier)
                if target.is_dir() and identifier not in self.entries:
                    shutil.rmtree(target)
            self.build_slot.release()

    @contextmanager
    def file(self, identifier: str, relative: str):
        _current_source(self.store, self.identity)
        self.sweep()
        with self.lock:
            entry = self.entries.get(identifier)
            if not entry or entry["delete"]:
                raise FileNotFoundError("batch expired; request its range again")
            files = {row["path"]: row for row in entry["manifest"]["files"]}
            if relative not in files:
                raise FileNotFoundError("file is not in the sealed batch manifest")
            entry["active"] += 1
            entry["touched"] = time.monotonic()
            source = _safe_source(entry["path"], relative)
        try:
            with os.fdopen(_open_regular(source), "rb") as stream:
                yield stream, files[relative]
        finally:
            with self.lock:
                entry["active"] -= 1
            self.sweep()

    def ack(self, identifier: str):
        with self.lock:
            if identifier in self.entries:
                self._remove(identifier)

    def close(self):
        self.closed = True
        with self.lock:
            for identifier in list(self.entries):
                self._remove(identifier)
            if self.lease_fd is not None:
                fcntl.flock(self.lease_fd, fcntl.LOCK_UN)
                os.close(self.lease_fd)
                self.lease_fd = None


def _handler(cache: _BatchCache, token: str):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_args):
            pass  # Never record authorization headers, request bodies or tokens.

        def setup(self):
            self.request.settimeout(self.server.request_timeout)
            super().setup()

        def _send(self, status, value):
            encoded = _json_bytes(value)
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            if status in {429, 503}:
                self.send_header("Retry-After", "1")
            self.end_headers()
            self.wfile.write(encoded)
            self.close_connection = True

        def _run(self, method):
            try:
                authorization = self.headers.get("Authorization", "")
                if len(authorization) > 520 or not hmac.compare_digest(authorization, "Bearer " + token):
                    return self._send(401, {"error": "replication_unauthorized"})
                path = urlsplit(self.path)
                if path.query or path.fragment:
                    return self._send(400, {"error": "unexpected_replication_query"})
                if method == "GET" and path.path == "/replication/v1/status":
                    return self._send(200, cache.status())
                if method == "POST" and path.path in {"/replication/v1/batches", "/replication/v1/anchor"}:
                    raw_length = self.headers.get("Content-Length", "")
                    if len(self.headers.get_all("Content-Length", [])) != 1 or self.headers.get("Transfer-Encoding") or \
                            not raw_length.isdecimal() or not 1 <= int(raw_length) <= _BODY_MAX:
                        return self._send(413, {"error": "replication_body_budget"})
                    body = self.rfile.read(int(raw_length))
                    if len(body) != int(raw_length):
                        return self._send(400, {"error": "replication_incomplete_body"})
                    request = _json_unique(body)
                    return self._send(200, cache.anchor(request) if path.path.endswith("/anchor") else cache.build(request))
                match = re.fullmatch(r"/replication/v1/batches/([0-9a-f]{64})(?:/files/(.+))?", path.path)
                if match and method == "DELETE" and match.group(2) is None:
                    cache.ack(match.group(1))
                    return self._send(200, {"released": True})
                if match and method == "GET" and match.group(2):
                    relative = unquote(match.group(2), errors="strict")
                    with cache.file(match.group(1), relative) as (stream, file):
                        offset = 0
                        requested_range = self.headers.get("Range")
                        if requested_range:
                            selected = re.fullmatch(r"bytes=(\d+)-", requested_range)
                            if not selected or not 0 <= int(selected.group(1)) < file["size_bytes"]:
                                return self._send(416, {"error": "unsupported_replication_range"})
                            offset = int(selected.group(1))
                        self.send_response(206 if requested_range else 200)
                        self.send_header("Content-Type", "application/octet-stream")
                        self.send_header("Content-Length", str(file["size_bytes"] - offset))
                        self.send_header("ETag", '"' + file["sha256"] + '"')
                        self.send_header("Accept-Ranges", "bytes")
                        self.send_header("Cache-Control", "no-store")
                        self.send_header("Connection", "close")
                        if requested_range:
                            self.send_header("Content-Range", f"bytes {offset}-{file['size_bytes']-1}/{file['size_bytes']}")
                        self.end_headers()
                        stream.seek(offset)
                        remaining = file["size_bytes"] - offset
                        while remaining:
                            block = stream.read(min(_CHUNK, remaining))
                            if not block:
                                raise StreamProtocolError("sealed batch file changed during transfer")
                            self.wfile.write(block)
                            remaining -= len(block)
                    self.close_connection = True
                    return
                return self._send(404, {"error": "replication_not_found"})
            except FileNotFoundError:
                self._send(404, {"error": "replication_batch_expired"})
            except (StreamUnavailable, TimeoutError):
                self._send(503, {"error": "replication_backpressure_or_upstream_unavailable"})
            except Conflict as exc:
                self._send(409, {"error": "replication_history_conflict", "detail": str(exc)})
            except (StoreError, OSError, ValueError, KeyError, TypeError) as exc:
                if isinstance(exc, (BrokenPipeError, ConnectionResetError)):
                    self.close_connection = True
                else:
                    self._send(422, {"error": "replication_invalid_request", "detail": str(exc)})

        def do_GET(self):
            self._run("GET")

        def do_POST(self):
            self._run("POST")

        def do_DELETE(self):
            self._run("DELETE")
    return Handler


class ReplicationStreamServer(BoundedHTTPServer):
    def __init__(self, address, cache, token, context, *, max_connections, request_timeout):
        self.cache = cache
        self.tls_context = context
        super().__init__(address, _handler(cache, token), max_connections=max_connections,
                         request_timeout=request_timeout)

    def process_request_thread(self, request, client_address):
        if self.tls_context is not None:
            try:
                request.settimeout(self.request_timeout)
                request = self.tls_context.wrap_socket(request, server_side=True)
            except (OSError, ssl.SSLError):
                self.shutdown_request(request)
                self._release_slot()
                return
        super().process_request_thread(request, client_address)

    def service_actions(self):
        self.cache.sweep()

    def server_close(self):
        super().server_close()
        self.cache.close()


def make_replication_server(source: Store, host: str = "127.0.0.1", port: int = 8443, *,
        token: str, certfile: str | Path | None = None, keyfile: str | Path | None = None,
        allow_loopback_http: bool = False, max_connections: int = 8,
        request_timeout: float = 15, max_records: int = 1000,
        max_batch_bytes: int = 64 * _MIB, max_cache_bytes: int = 512 * _MIB,
        max_cached_batches: int = 4, ttl_seconds: float = 120) -> ReplicationStreamServer:
    token = _token(token)
    if type(allow_loopback_http) is not bool:
        raise StreamProtocolError("allow_loopback_http must be an explicit boolean")
    context = None
    if certfile is not None and keyfile is not None:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(str(certfile), str(keyfile))
    elif certfile is not None or keyfile is not None:
        raise StreamProtocolError("replication TLS requires both certificate and private key")
    elif not allow_loopback_http or not _loopback(host):
        raise StreamProtocolError("replication listener requires TLS; plaintext is permitted only on explicit literal loopback")
    from .wal_archive import archive_status
    if not archive_status(source, verify=False).get("enabled"):
        raise StreamProtocolError("enable WAL archival and create the matching standby before starting replication transport")
    cache = _BatchCache(source, max_records=max_records, max_batch_bytes=max_batch_bytes,
        max_cache_bytes=max_cache_bytes, max_cached_batches=max_cached_batches, ttl_seconds=ttl_seconds)
    try:
        return ReplicationStreamServer((host, port), cache, token, context,
            max_connections=max_connections, request_timeout=request_timeout)
    except BaseException:
        cache.close()
        raise


def serve_replication(source: Store, **options) -> None:
    server = make_replication_server(source, **options)
    try:
        server.serve_forever(poll_interval=0.25)
    finally:
        server.server_close()


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None  # Never forward the privileged bearer token to another URL.


class _Client:
    def __init__(self, url: str, token: str, *, ca_file=None, allow_loopback_http=False, timeout=15):
        if type(allow_loopback_http) is not bool:
            raise StreamProtocolError("allow_loopback_http must be an explicit boolean")
        parts = urlsplit(url)
        if parts.username or parts.password or parts.query or parts.fragment or parts.path not in {"", "/"} or \
                not parts.hostname or parts.scheme not in {"https", "http"}:
            raise StreamProtocolError("upstream must be an HTTPS origin without credentials, path, query or fragment")
        if parts.scheme != "https" and not (allow_loopback_http and _loopback(parts.hostname)):
            raise StreamProtocolError("upstream TLS is mandatory outside explicitly enabled literal loopback")
        self.url = url.rstrip("/")
        self.token = _token(token)
        self.timeout = _seconds(timeout, 3600, "network timeout")
        context = ssl.create_default_context(cafile=str(ca_file) if ca_file is not None else None)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        self.opener = build_opener(_NoRedirect(), HTTPSHandler(context=context))

    def open(self, path, *, method="GET", body=None, headers=None):
        request = Request(self.url + path, method=method,
            data=None if body is None else _json_bytes(body),
            headers={"Authorization": "Bearer " + self.token, "Content-Type": "application/json",
                     "Accept-Encoding": "identity", **(headers or {})})
        try:
            return self.opener.open(request, timeout=self.timeout)
        except HTTPError as exc:
            if exc.code in {404, 408, 429, 500, 502, 503, 504}:
                raise StreamUnavailable(f"replication upstream returned retryable HTTP {exc.code}") from exc
            raise StreamProtocolError(f"replication upstream refused request with HTTP {exc.code}") from exc
        except (URLError, TimeoutError, OSError) as exc:
            if isinstance(getattr(exc, "reason", None), ssl.SSLCertVerificationError):
                raise StreamProtocolError("replication upstream TLS certificate/hostname verification failed") from exc
            raise StreamUnavailable("replication upstream network or TLS connection unavailable") from exc

    def json(self, path, *, method="GET", body=None, max_bytes=_MANIFEST_MAX):
        with self.open(path, method=method, body=body) as response:
            length = response.headers.get("Content-Length", "")
            if not length.isdecimal() or int(length) > max_bytes or response.headers.get("Content-Encoding", "identity") != "identity":
                raise StreamProtocolError("replication JSON response exceeds bounds or lacks exact length")
            encoded = response.read(int(length) + 1)
            if len(encoded) != int(length):
                raise StreamUnavailable("replication JSON response was interrupted")
        return _json_unique(encoded)


def _database_path(value: Store | str | Path) -> Path:
    if isinstance(value, Store):
        return value.journal
    path = Path(value).expanduser()
    return path.resolve() if path.suffix == ".sdb" else (path / "RadioS.sdb").resolve()


def _workspace(database: Path) -> Path:
    meta = _json_unique(_safe_source(database.parent, database.name + ".meta").read_bytes())
    identifier, _timeline, _epoch = _identity(meta)
    if meta.get("node_role") != "standby" or not isinstance(meta.get("replication"), dict):
        raise StreamProtocolError("replication follower path must name an initialized standby")
    return _private_directory(database.parent / (database.name + ".replication-stream"),
        {"format": PROTOCOL + "-follower-cache", "store_id": identifier, "node_id": meta["node_id"]})


@contextmanager
def _follower_lock(workspace: Path, name="follower.lock"):
    lock_path = _safe_source(workspace, name)
    fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise StreamUnavailable("another continuous follower owns this standby") from exc
        yield
    finally:
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)


def _open_or_resume(value: Store | Path, workspace: Path) -> Store:
    if isinstance(value, Store):
        store = value
    else:
        try:
            store = open_radio_store(value)
        except StoreError:
            incoming = _safe_source(workspace, "incoming")
            if not incoming.is_dir():
                raise
            recover_standby_replay(value, incoming)
            store = open_radio_store(value)
    status = replication_status(store)
    if status["pending_replay"]:
        incoming = _safe_source(workspace, "incoming")
        if not incoming.is_dir():
            raise StreamProtocolError("standby has pending replay without its verified cached incoming batch")
        apply_shipment(store, incoming)
    return store


def _validate_offer(offer: dict, identity: dict, after: int, previous: str, max_bytes: int,
                    previous_effective_at: str) -> tuple[dict, str]:
    if not isinstance(offer, dict) or set(offer) != {"batch_id", "manifest", "manifest_sha256"} or \
            not isinstance(offer["batch_id"], str) or not _HEX.fullmatch(offer["batch_id"]):
        raise StreamProtocolError("invalid replication batch offer")
    manifest = offer["manifest"]
    encoded = _json_bytes(manifest)
    if len(encoded) > _MANIFEST_MAX or hashlib.sha256(encoded).hexdigest() != offer["manifest_sha256"] or \
            any(manifest.get(key) != value for key, value in identity.items()) or \
            manifest.get("from_sequence") != after or manifest.get("previous_sha256") != previous or \
            manifest.get("previous_effective_at") != previous_effective_at:
        raise StreamProtocolError("offered batch differs from the pinned source or resume anchor")
    try:
        first, through = manifest["from_sequence"], manifest["through_sequence"]
        if type(first) is not int or type(through) is not int or not first <= through <= first + 10000 or \
                type(manifest["records_count"]) is not int or manifest["records_count"] != through - first:
            raise ValueError
        seen = set()
        for row in manifest["files"]:
            if set(row) != {"path", "sha256", "size_bytes"} or row["path"] in seen or \
                    not _sha(row["sha256"]) or type(row["size_bytes"]) is not int or row["size_bytes"] < 0:
                raise ValueError
            seen.add(row["path"])
            if row["path"] != "records.json":
                _asset_path(identity["database"], row)
        if "records.json" not in seen or sum(row["size_bytes"] for row in manifest["files"]) + len(encoded) + 65 > max_bytes:
            raise ValueError
    except (KeyError, TypeError, ValueError) as exc:
        raise StreamProtocolError("offered manifest has unsafe files or exceeds the complete batch byte budget") from exc
    return manifest, offer["batch_id"]


def _download(client: _Client, identifier: str, incoming: Path, file: dict) -> dict:
    target = _safe_source(incoming, file["path"])
    if target.exists():
        if _digest_file(target) == (file["sha256"], file["size_bytes"]):
            return {"downloaded_bytes": 0, "resumed_bytes": file["size_bytes"]}
        target.unlink()  # Only this owned incoming workspace is changed.
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = _safe_source(incoming, file["path"] + ".part")
    offset = partial.stat().st_size if partial.exists() else 0
    if offset > file["size_bytes"]:
        partial.unlink()
        offset = 0
    path = f"/replication/v1/batches/{identifier}/files/{quote(file['path'], safe='')}"
    downloaded = 0
    if offset < file["size_bytes"]:
        headers = {"Range": f"bytes={offset}-"} if offset else {}
        with client.open(path, headers=headers) as response:
            expected = file["size_bytes"] - offset
            if response.status != (206 if offset else 200) or response.headers.get("Content-Length") != str(expected) or \
                    response.headers.get("ETag") != '"' + file["sha256"] + '"' or \
                    response.headers.get("Content-Encoding", "identity") != "identity" or \
                    offset and response.headers.get("Content-Range") != f"bytes {offset}-{file['size_bytes']-1}/{file['size_bytes']}":
                raise StreamProtocolError("replication file response has an invalid length, ETag or resume range")
            fd = os.open(partial, os.O_WRONLY | os.O_CREAT | os.O_APPEND | getattr(os, "O_NOFOLLOW", 0), 0o600)
            with os.fdopen(fd, "ab") as output:
                try:
                    while downloaded < expected:
                        block = response.read(min(_CHUNK, expected - downloaded))
                        if not block:
                            raise StreamUnavailable("replication file transfer was interrupted")
                        output.write(block)
                        downloaded += len(block)
                except (OSError, TimeoutError) as exc:
                    raise StreamUnavailable("replication file transfer failed; cached prefix retained for resume") from exc
                finally:
                    output.flush()
                    os.fsync(output.fileno())
    elif not partial.exists():
        partial.touch(mode=0o600)  # A committed zero-byte managed object.
    if _digest_file(partial) != (file["sha256"], file["size_bytes"]):
        partial.unlink()
        raise StreamProtocolError("downloaded replication file SHA-256 or size differs from its sealed manifest")
    os.replace(partial, target)
    _fsync_dir(target.parent)
    return {"downloaded_bytes": downloaded, "resumed_bytes": offset}


def follow_once(standby: Store | str | Path, url: str, *, token: str,
                ca_file: str | Path | None = None, allow_loopback_http: bool = False,
                max_records: int = 1000, max_batch_bytes: int = 64 * _MIB,
                timeout: float = 15) -> dict:
    client = _Client(url, token, ca_file=ca_file, allow_loopback_http=allow_loopback_http, timeout=timeout)
    database = _database_path(standby)
    workspace = _workspace(database)
    with _follower_lock(workspace):
        store = _open_or_resume(standby if isinstance(standby, Store) else database, workspace)
        identity = _bound_identity(store)
        source_status = client.json("/replication/v1/status", max_bytes=16384)
        if source_status.get("protocol") != PROTOCOL or source_status.get("node_role") != "primary" or \
                any(source_status.get(key) != value for key, value in identity.items()):
            raise StreamProtocolError("upstream is not the standby's pinned primary source/timeline/epoch")
        count = min(_limit(max_records, 1, 10000, "max_records"), source_status["limits"]["max_records"])
        budget = min(_limit(max_batch_bytes, 65536, 4 * 1024 * _MIB, "max_batch_bytes"), source_status["limits"]["max_batch_bytes"])
        progress = replication_status(store)["replication"]
        after, previous = progress["last_replayed_sequence"], progress["last_record_sha256"]
        previous_time = progress.get("last_effective_at")
        if previous_time is None:
            # A 0.7 receiver has already verified the digest, but did not keep
            # its timestamp. The source scans that exact payload once with
            # bounded memory; do not weaken the timestamp lower bound.
            stamp = client.json("/replication/v1/anchor", method="POST",
                                body={**identity, "sequence": after, "sha256": previous})
            from .wal_archive import _instant
            if set(stamp) != set(identity) | {"sequence", "sha256", "effective_at"} or \
                    any(stamp.get(key) != value for key, value in identity.items()) or \
                    stamp["sequence"] != after or stamp["sha256"] != previous:
                raise StreamProtocolError("legacy anchor upgrade differs from the durable source/digest")
            _instant(stamp["effective_at"])
            with store._locked(exclusive=True):
                store._reload_locked()
                meta = store._meta()
                current = meta.get("replication", {})
                if meta.get("node_role") != "standby" or \
                        any(current.get(key) != identity[target] for key, target in (
                            ("source_store_id", "source_store_id"), ("source_node_id", "source_node_id"),
                            ("source_timeline_id", "source_timeline_id"), ("source_epoch", "source_epoch"))) or \
                        current.get("last_replayed_sequence") != after or current.get("last_record_sha256") != previous or \
                        current.get("pending"):
                    raise StreamProtocolError("standby ownership or replay position changed during anchor upgrade")
                current["last_effective_at"] = stamp["effective_at"]
                _atomic_json(store.meta_file, meta)
            previous_time = stamp["effective_at"]
        if type(source_status.get("sequence")) is not int or source_status["sequence"] < after:
            raise StreamProtocolError("upstream sequence regressed behind the standby")
        if source_status["sequence"] == after:
            return {"applied": 0, "sequence": after, "lag_transactions": 0, "caught_up": True,
                    "downloaded_bytes": 0, "resumed_bytes": 0}
        offer = client.json("/replication/v1/batches", method="POST",
            body={**identity, "after_sequence": after, "previous_sha256": previous,
                  "previous_effective_at": previous_time,
                  "max_records": count, "max_bytes": budget})
        manifest, identifier = _validate_offer(offer, identity, after, previous, budget, previous_time)
        incoming = _safe_source(workspace, "incoming")
        state_path = _safe_source(workspace, "state.json")
        existing = _json_unique(state_path.read_bytes()) if state_path.exists() else None
        transfer = {"format": PROTOCOL, "batch_id": identifier, "manifest_sha256": offer["manifest_sha256"],
                    "source_url": client.url, "after_sequence": after}
        if existing != transfer:
            if incoming.exists():
                shutil.rmtree(incoming)
            incoming.mkdir(mode=0o700)
            _atomic_json(state_path, transfer)
        elif not incoming.is_dir():
            raise StreamProtocolError("cached transfer metadata has no incoming directory")
        encoded = _json_bytes(manifest)
        _atomic_json(incoming / "manifest.json", manifest)
        hash_path = _safe_source(incoming, "manifest.sha256")
        with hash_path.open("w", encoding="ascii") as stream:
            os.chmod(hash_path, 0o600)
            stream.write(hashlib.sha256(encoded).hexdigest() + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        totals = {"downloaded_bytes": 0, "resumed_bytes": 0}
        for file in manifest["files"]:
            result = _download(client, identifier, incoming, file)
            for key in totals:
                totals[key] += result[key]
        _read_shipment(incoming)  # Full inventory, frame/identity/asset verification.
        result = apply_shipment(store, incoming)
        # The apply position is durable before the sender's cache is released.
        try:
            client.json(f"/replication/v1/batches/{identifier}", method="DELETE", max_bytes=16384)
        except StreamUnavailable:
            pass  # TTL will release the cache; replay is already durable.
        shutil.rmtree(incoming)
        state_path.unlink(missing_ok=True)
        result.update(totals)
        result["lag_transactions"] = max(0, source_status["sequence"] - result["sequence"])
        result["caught_up"] = result["lag_transactions"] == 0
        return result


def follow_forever(standby: Store | str | Path, url: str, *, token: str,
                   poll_interval: float = 1, stop_event: threading.Event | None = None,
                   on_progress=None, max_iterations: int | None = None, **options) -> dict:
    """Back off on temporary failures, persist progress, stop on any mismatch."""
    interval = _seconds(poll_interval, 60, "poll_interval")
    if max_iterations is not None:
        _limit(max_iterations, 1, 2**31 - 1, "max_iterations")
    stopper = stop_event or threading.Event()
    iterations, failures, total_applied = 0, 0, 0
    workspace = _workspace(_database_path(standby))
    with _follower_lock(workspace, "runner.lock"):
        while not stopper.is_set() and (max_iterations is None or iterations < max_iterations):
            iterations += 1
            try:
                result = follow_once(standby, url, token=token, **options)
                failures = 0
                total_applied += result["applied"]
                result["state"] = "caught_up" if result["caught_up"] else "catching_up"
                delay = interval if result["caught_up"] else 0
            except StreamUnavailable as exc:
                failures += 1
                result = {"state": "retrying", "detail": str(exc), "consecutive_failures": failures}
                delay = min(30, interval * 2 ** min(failures - 1, 10))
            if on_progress is not None:
                on_progress(result)
            if delay:
                stopper.wait(delay)
    return {"stopped": stopper.is_set(), "iterations": iterations, "applied": total_applied}
