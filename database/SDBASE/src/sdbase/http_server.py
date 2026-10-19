"""Small authenticated JSON and media API for the independent SDBASE engine."""

from __future__ import annotations

import base64
import binascii
import hashlib
from http.server import BaseHTTPRequestHandler
import json
import os
from pathlib import Path
import re
import tempfile
from time import monotonic
from urllib.parse import parse_qs, urlsplit
from uuid import UUID

from . import __version__
from .catalog import NotFound
from .identity import Forbidden, authenticate
from .http_limits import RequestLimiter
from .runtime_version import DESKTOP_API_VERSION, source_digest
from .storage import Conflict, LockTimeout, Store, StoreError, WriteRejected
from .wal_archive import ArchiveAcknowledgementError


_UUID = r"[0-9a-fA-F-]{36}"


def make_handler(store: Store):
    limits = RequestLimiter()
    # Capture once: hashing on each request would describe updated disk files,
    # while this process could still be executing modules imported before them.
    runtime_digest = source_digest()

    class Handler(BaseHTTPRequestHandler):
        source_digest = runtime_digest
        server_version = "RadioS-SDBASE/" + __version__
        protocol_version = "HTTP/1.1"

        def setup(self) -> None:
            self.request.settimeout(getattr(self.server, "request_timeout", 30))
            super().setup()

        def _send(self, status: int, value: object, *, retry_after: int = 0) -> None:
            self.connection.settimeout(getattr(self.server, "request_timeout", 30))
            store.metrics.increment(f"http_status_{status}")
            # A rejected request may still have unread body bytes. Closing keeps
            # them from being interpreted as another HTTP/1.1 request.
            if status >= 400 or (self.command == "GET" and
                                 self.headers.get("Content-Length", "0") != "0"):
                self.close_connection = True
            data = json.dumps(value, ensure_ascii=False, default=str,
                              separators=(",", ":")).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            if self.close_connection:
                self.send_header("Connection", "close")
            if retry_after:
                self.send_header("Retry-After", str(retry_after))
            self.end_headers()
            self.wfile.write(data)

        def _body(self, *, max_bytes: int = 2 * 1024 * 1024) -> dict:
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError as exc:
                raise StoreError("invalid Content-Length") from exc
            if length < 1 or length > max_bytes:
                raise StoreError(f"JSON body must be 1..{max_bytes} bytes")
            deadline = monotonic() + getattr(self.server, "request_timeout", 30)
            chunks = []
            remaining = length
            while remaining:
                chunk = self._read_body_chunk(remaining, deadline)
                chunks.append(chunk)
                remaining -= len(chunk)
            encoded = b"".join(chunks)
            try:
                parsed = json.loads(encoded)
            except (ValueError, UnicodeDecodeError) as exc:
                raise StoreError("invalid JSON body") from exc
            if not isinstance(parsed, dict):
                raise StoreError("JSON body must be an object")
            return parsed

        def _read_body_chunk(self, remaining: int, deadline: float) -> bytes:
            available = deadline - monotonic()
            if available <= 0:
                raise StoreError("request body exceeded its total time limit")
            self.connection.settimeout(min(60, available))
            try:
                chunk = self.rfile.read1(min(64 * 1024, remaining))
            except OSError as exc:
                raise StoreError("request body transfer interrupted") from exc
            if not chunk:
                raise StoreError("request body ended before Content-Length")
            return chunk

        def _upload_media(self, media, principal, station_id: str,
                          version_id: str) -> dict:
            """Stream bytes to private staging before publishing a media object."""
            from .identity import require
            require(store, principal, "edit", station_id)
            media._require_version_in_station(store, station_id, version_id,
                                              writable=True)
            if self.headers.get("Content-Type", "").split(";", 1)[0].strip() != \
                    "application/octet-stream":
                raise StoreError("media upload needs application/octet-stream")
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError as exc:
                raise StoreError("invalid Content-Length") from exc
            if length < 1 or length > 512 * 1024 * 1024:
                raise StoreError("media upload must be 1..536870912 bytes")
            staging = Path(store.root) / ".uploads"
            staging.mkdir(mode=0o700, exist_ok=True)
            if staging.is_symlink():
                raise StoreError("upload staging must not be a symlink")
            temporary: Path | None = None
            try:
                with tempfile.NamedTemporaryFile(mode="wb", prefix="upload-",
                                                 dir=staging, delete=False) as stream:
                    temporary = Path(stream.name)
                    remaining = length
                    deadline = monotonic() + 300
                    while remaining:
                        chunk = self._read_body_chunk(remaining, deadline)
                        stream.write(chunk)
                        remaining -= len(chunk)
                    stream.flush()
                    os.fsync(stream.fileno())
                result = media.stage_media(store, principal, station_id, version_id,
                                           temporary, [staging])
                return {key: value for key, value in result.items() if key != "path"}
            except OSError as exc:
                raise StoreError("media upload failed") from exc
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)

        def _auth(self):
            header = self.headers.get("Authorization", "")
            if not header.startswith("Bearer "):
                raise Forbidden("bearer token required")
            return authenticate(store, header[7:])

        def _query_bool(self, query: dict, key: str) -> bool | None:
            value = query.get(key)
            if value is None:
                return None
            if value not in {"true", "false"}:
                raise StoreError(f"{key} must be true or false")
            return value == "true"

        def _run(self, method: str) -> None:
            started = monotonic()
            self.connection.settimeout(getattr(self.server, "request_timeout", 30))
            parts = urlsplit(self.path)
            query = {key: values[0] for key, values in parse_qs(parts.query).items()}
            path = parts.path
            try:
                if self.headers.get("Transfer-Encoding") or len(self.headers.get_all("Content-Length", [])) > 1:
                    raise StoreError("chunked transfer and duplicate Content-Length are unsupported")
                if method == "GET" and int(self.headers.get("Content-Length", "0")) != 0:
                    raise StoreError("GET request bodies are unsupported")
                retry = limits.retry_after("ip:" + self.client_address[0],
                                           capacity=120, refill_per_second=10)
                if retry:
                    return self._send(429, {"error": "rate_limited"}, retry_after=retry)
                auth_header = self.headers.get("Authorization", "")
                if auth_header.startswith("Bearer "):
                    digest = hashlib.sha256(auth_header[7:].encode()).hexdigest()
                    retry = limits.retry_after("token:" + digest,
                                               capacity=120, refill_per_second=10)
                    if retry:
                        return self._send(429, {"error": "rate_limited"},
                                          retry_after=retry)
                if method == "GET" and path == "/health":
                    with store._locked(exclusive=False):
                        store._reload_locked()
                        metadata = store._meta()
                        health = {"status": "ok", "sequence": store._sequence,
                                  "format": "sdbase-journal-v1",
                                  **{field: metadata.get(field) for field in
                                     ("store_id", "node_role", "node_id", "timeline_id", "epoch")}}
                    return self._send(200, health)
                principal = self._auth()
                if principal.scope == "endpoint" or path.startswith("/v1/endpoints/"):
                    with store._locked(exclusive=False):
                        if store._meta().get("node_role", "primary") != "primary":
                            raise WriteRejected("endpoint operation requires the current primary")
                if method == "GET" and path == "/v1/operations/metrics":
                    from .monitoring import read_engine_status
                    status = read_engine_status(store, principal)
                    if hasattr(self.server, "stats"):
                        status["http_server"] = self.server.stats()
                    return self._send(200, status)
                if method == "GET" and path == "/v1/operations/integrity":
                    from .monitoring import read_integrity
                    return self._send(200, read_integrity(store, principal,
                        max_records=int(query.get("max_records", "100000")),
                        max_bytes=int(query.get("max_bytes", "1073741824")),
                        verify_media=self._query_bool(query, "verify_media") is not False,
                        verify_cold=self._query_bool(query, "verify_cold") is not False))
                if method != "GET":
                    with store._locked(exclusive=False):
                        store._assert_writable_locked()
                if method == "GET" and path == "/v1/openapi.json":
                    from .openapi import openapi_document
                    return self._send(200, openapi_document())
                if method == "GET" and path == "/v1/capabilities":
                    from .desktop_api import KINDS
                    return self._send(200, {"api_version": __version__,
                        "source_digest": runtime_digest,
                        "desktop_api_version": DESKTOP_API_VERSION,
                        "desktop_configuration_kinds": sorted(KINDS),
                        "database_format": ".sdb", "storage_engine": "RadioS custom journal",
                        "readers": ["rekordbox_xml_1.0.0", "rekordbox_devicesql_export_pdb"],
                        "offline_cli_adapters": ["mairlist_csv_3.1_20_column",
                                                 "radioboss_plain_m3u8", "libretime_playout_v2_projection"],
                        "workflow_features": ["versioned_playlists", "static_dynamic_smart_blocks",
                                              "rss_atom_podcasts", "listener_mount_time_series",
                                              "frozen_output_binding", "approved_playback_markers",
                                              "detail_revision_reads", "verified_cold_retention",
                                              "consistent_read_snapshots", "indexed_query_explain",
                                              "wal_point_in_time_recovery", "fenced_standby_handover",
                                              "bounded_http_connections", "engine_metrics_integrity",
                                              "authenticated_network_replication",
                                              "reachable_source_automatic_failover",
                                              "online_checkpoint_preparation",
                                              "generation_bound_ordered_disk_indexes",
                                              "kind_transition_distribution"],
                        "schema_version": store.schema_version,
                        "audio_executors": ["accepted_plan_pcm16_wav_render"],
                        "unsupported_readers": ["rekordbox_local", "rekordbox_device_anlz", "rekordbox_onelibrary"]})
                result = self._dispatch(method, path, query, principal)
                if result is None:
                    self._send(404, {"error": "not_found"})
                elif result is not False:
                    self._send(200, result)
            except Forbidden as exc:
                self._send(403, {"error": "forbidden", "detail": str(exc)})
            except Conflict as exc:
                self._send(409, {"error": "conflict", "detail": str(exc)})
            except NotFound:
                self._send(404, {"error": "not_found"})
            except ArchiveAcknowledgementError as exc:
                self._send(503, {"error": "archive_acknowledgement_failed", "committed": True,
                    "sequence": exc.sequence, "transaction_id": exc.transaction_id,
                    "detail": str(exc)})
            except (WriteRejected, LockTimeout) as exc:
                self._send(503, {"error": "temporarily_unavailable", "detail": str(exc)}, retry_after=1)
            except (StoreError, ValueError, KeyError, TypeError) as exc:
                self._send(422, {"error": "invalid_request", "detail": str(exc)})
            except (BrokenPipeError, ConnectionResetError):
                self.close_connection = True
            except Exception:
                self._send(500, {"error": "internal_error"})
            finally:
                store.metrics.observe("http_request_ms", (monotonic() - started) * 1000)

        def _dispatch(self, method: str, path: str, query: dict, principal):
            from . import catalog, importer, media, plans, playout, search, settings
            from . import access_admin
            if method == "GET":
                from .read_api import dispatch_read
                detail = dispatch_read(store, principal, path, query)
                if detail is not None:
                    return detail
            if method == "POST" and path == "/v1/access/users":
                body = self._body()
                return access_admin.create_station_user(
                    store, principal, body["station_id"], body["username"],
                    body["display_name"], body["permissions"],
                    days=body.get("days", 30))
            match = re.fullmatch(rf"/v1/access/users/({_UUID})/tokens", path)
            if method == "POST" and match:
                body = self._body()
                return access_admin.issue_user_token(
                    store, principal, match.group(1), days=body.get("days", 30))
            match = re.fullmatch(rf"/v1/access/tokens/({_UUID})/revoke", path)
            if method == "POST" and match:
                body = self._body()
                return access_admin.revoke_token(
                    store, principal, match.group(1),
                    expected_revision=body["expected_revision"])
            match = re.fullmatch(rf"/v1/access/tokens/({_UUID})", path)
            if method == "GET" and match:
                return access_admin.get_token(store, principal, match.group(1))
            if method == "GET" and path == "/v1/import-profiles":
                return settings.list_import_profiles(store, principal, query["station_id"])
            if method == "POST" and path == "/v1/import-profiles":
                body = self._body()
                return settings.put_import_profile(store, principal, body["station_id"],
                    name=body["name"], library_key=body["library_key"],
                    config=body.get("config", {}), status=body.get("status", "active"))
            match = re.fullmatch(rf"/v1/import-profiles/({_UUID})", path)
            if method == "POST" and match:
                body = self._body()
                return settings.put_import_profile(store, principal, body["station_id"],
                    name=body["name"], library_key=body["library_key"],
                    config=body.get("config", {}), profile_id=match.group(1),
                    expected_revision=body["expected_revision"],
                    status=body.get("status", "active"))
            if method == "GET" and path == "/v1/views":
                return settings.list_view_profiles(store, principal, query["station_id"])
            if method == "POST" and path == "/v1/views":
                body = self._body()
                return settings.put_view_profile(store, principal, body["station_id"],
                    name=body["name"], view_kind=body["view_kind"],
                    columns=body["columns"], filters=body.get("filters"),
                    profile_id=body.get("profile_id"),
                    expected_revision=body.get("expected_revision"))
            if method == "POST" and path in {
                    "/v1/imports/rekordbox/preview", "/v1/imports/rekordbox/apply"}:
                body = self._body(max_bytes=90_000_000)
                profile = settings.import_options(store, principal,
                                                  body["station_id"], body["profile_id"])
                encoded = body["xml_base64"]
                if not isinstance(encoded, str) or len(encoded) > 86_000_000:
                    raise StoreError("XML base64 is too large or invalid")
                try:
                    xml = base64.b64decode(encoded, validate=True)
                except (binascii.Error, ValueError) as exc:
                    raise StoreError("invalid XML base64") from exc
                options = profile["options"]
                if path.endswith("/preview"):
                    result = importer.preview_xml_import(store, principal,
                        body["station_id"], profile["library_key"], xml, **options)
                else:
                    result = importer.apply_xml_import(store, principal,
                        body["station_id"], profile["library_key"], xml,
                        body["idempotency_key"],
                        expected_preview_hash=body["expected_preview_hash"],
                        **options)
                return {"profile_id": profile["profile_id"],
                        "profile_version": profile["config_version"], **result}
            if method == "GET" and path == "/v1/items":
                return search.search_items(store, principal, query["station_id"],
                    title=query.get("title"), artist=query.get("artist"),
                    contribution_role=query.get("contribution_role"),
                    category_id=query.get("category_id"), category=query.get("category"),
                    version_label=query.get("version_label"), kind=query.get("kind"),
                    min_duration_ms=(int(query["min_duration_ms"])
                                     if "min_duration_ms" in query else None),
                    max_duration_ms=(int(query["max_duration_ms"])
                                     if "max_duration_ms" in query else None),
                    endpoint_id=query.get("endpoint_id"),
                    ready=self._query_bool(query, "ready"),
                    slot_start=query.get("slot_start"), slot_end=query.get("slot_end"),
                    tolerant=self._query_bool(query, "tolerant") or False,
                    limit=int(query.get("limit", "50")), cursor=query.get("cursor"))
            if method == "POST" and path == "/v1/items":
                body = self._body()
                return catalog.create_item(store, principal, body["station_id"],
                                           kind=body["kind"], title=body["title"],
                                           recording_id=body.get("recording_id"))
            match = re.fullmatch(rf"/v1/items/({_UUID})/versions", path)
            if method == "POST" and match:
                body = self._body()
                return catalog.create_version(store, principal, body["station_id"],
                                              match.group(1), label=body["label"],
                                              valid_from=body.get("valid_from"),
                                              valid_to=body.get("valid_to"))
            match = re.fullmatch(rf"/v1/items/({_UUID})/share", path)
            if method == "POST" and match:
                body = self._body()
                return catalog.share_item(store, principal, body["station_id"],
                                          match.group(1))
            match = re.fullmatch(rf"/v1/items/({_UUID})/eligibility", path)
            if method == "POST" and match:
                body = self._body()
                if not isinstance(body.get("eligible"), bool):
                    raise StoreError("eligible must be boolean")
                return catalog.set_station_eligibility(
                    store, principal, body["station_id"], match.group(1),
                    body["eligible"], expected_revision=body["expected_revision"])
            match = re.fullmatch(rf"/v1/items/({_UUID})/lifecycle", path)
            if method == "POST" and match:
                body = self._body()
                return catalog.set_lifecycle(
                    store, principal, body["station_id"], match.group(1),
                    body["target"], expected_revision=body["expected_revision"])
            match = re.fullmatch(rf"/v1/versions/({_UUID})/approve", path)
            if method == "POST" and match:
                body = self._body()
                return catalog.approve_version(
                    store, principal, body["station_id"], match.group(1),
                    expected_revision=body["expected_revision"])
            match = re.fullmatch(rf"/v1/versions/({_UUID})/media", path)
            if method == "POST" and match:
                return self._upload_media(media, principal, query["station_id"],
                                          match.group(1))
            match = re.fullmatch(rf"/v1/media/({_UUID})/verify", path)
            if method == "POST" and match:
                body = self._body()
                return media.verify_and_promote_media(
                    store, principal, body["station_id"], match.group(1),
                    body["technical_metadata"],
                    expected_revision=body.get("expected_revision"))
            if method == "POST" and path == "/v1/rights":
                from . import rights
                body = self._body()
                return rights.grant(store, principal, body["station_id"],
                    body["version_id"], territory=body["territory"],
                    output_id=body["output_id"], starts_at=body["starts_at"],
                    ends_at=body["ends_at"], decision=body.get("decision", "allow"),
                    source_ref=body.get("source_ref"))
            if method == "POST" and path == "/v1/plans":
                body = self._body()
                return plans.create_plan(store, principal, body["station_id"],
                                         body["service_date"],
                                         service_key=body.get("service_key", "default"))
            match = re.fullmatch(rf"/v1/plans/({_UUID})/entries", path)
            if method == "POST" and match:
                body = self._body()
                return plans.put_entry(store, principal, body["station_id"], match.group(1),
                                       body["revision_id"], body["entry"],
                                       expected_plan_revision=body["expected_plan_revision"],
                                       entry_id=body.get("entry_id"),
                                       expected_entry_revision=body.get("expected_entry_revision"))
            match = re.fullmatch(rf"/v1/plans/({_UUID})/approve", path)
            if method == "POST" and match:
                body = self._body()
                return plans.approve_plan(store, principal, body["station_id"], match.group(1),
                                          body["revision_id"],
                                          expected_plan_revision=body["expected_plan_revision"])
            match = re.fullmatch(rf"/v1/plans/({_UUID})/distribute", path)
            if method == "POST" and match:
                body = self._body()
                snapshot = plans.build_snapshot(store, principal, body["station_id"],
                                                body["endpoint_id"], body["revision_id"])
                return playout.prepare_distribution(store, principal, body["station_id"],
                    body["endpoint_id"], body["revision_id"], snapshot)
            if method == "POST" and path == "/v1/engine/commands":
                body = self._body()
                return playout.queue_command(store, principal, body["station_id"],
                    body["endpoint_id"], body["command_kind"], body.get("payload", {}),
                    body["command_key"], ttl_seconds=body.get("ttl_seconds", 30))
            if method == "POST" and path == "/v1/playout/events":
                return playout.record_playout_event(store, principal, self._body())
            if method == "POST" and path == "/v1/monitors/tokens":
                from .identity import issue_monitor_token
                body = self._body()
                return issue_monitor_token(store, principal, body["station_id"],
                                           body["endpoint_id"], days=body.get("days", 30))
            match = re.fullmatch(rf"/v1/endpoints/({_UUID})/tokens", path)
            if method == "POST" and match:
                from .identity import issue_endpoint_token
                body = self._body()
                token = issue_endpoint_token(
                    store, principal, body["station_id"], match.group(1),
                    days=body.get("days", 30))
                record = store.lookup("tokens", "hash",
                                      hashlib.sha256(token.encode()).hexdigest())[0]
                return {"endpoint_id": match.group(1),
                        "token_id": record["_id"], "token": token}
            if method == "POST" and path == "/v1/observations":
                body = self._body()
                return playout.record_output_observation(
                    store, principal, body["station_id"], body["endpoint_id"],
                    body["observation_key"], body["kind"], body["occurred_at"],
                    event_id=body.get("event_id"),
                    observer_kind=body.get("observer_kind", "endpoint_self_report"),
                    evidence_ref=body.get("evidence_ref"),
                    details=body.get("details"))
            match = re.fullmatch(rf"/v1/endpoints/({_UUID})/state", path)
            if method == "GET" and match:
                endpoint = store.get("endpoints", str(UUID(match.group(1))))
                if endpoint is None:
                    raise StoreError("endpoint not found")
                return playout.plan_state(store, principal, endpoint["station_id"],
                                          match.group(1))
            match = re.fullmatch(rf"/v1/endpoints/({_UUID})/snapshots/({_UUID})", path)
            if method == "GET" and match:
                return playout.fetch_snapshot(store, principal, query["station_id"],
                                              match.group(1), match.group(2))
            match = re.fullmatch(rf"/v1/endpoints/({_UUID})/media/({_UUID})", path)
            if method == "GET" and match:
                source = media.verified_media_path(store, principal, query["station_id"],
                                                   match.group(1), match.group(2))
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(source.stat().st_size))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                with source.open("rb") as stream:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        self.wfile.write(chunk)
                return False
            match = re.fullmatch(rf"/v1/endpoints/({_UUID})/media/({_UUID})/verify", path)
            if method == "POST" and match:
                body = self._body()
                return media.report_verified_endpoint_copy(store, principal,
                    body["station_id"], match.group(1), match.group(2),
                    body["sha256"], body["storage_uri"])
            match = re.fullmatch(rf"/v1/endpoints/({_UUID})/deliveries/({_UUID})/ack", path)
            if method == "POST" and match:
                body = self._body()
                return playout.acknowledge_delivery(store, principal,
                    body["station_id"], match.group(1), match.group(2), body["snapshot_sha256"])
            match = re.fullmatch(rf"/v1/endpoints/({_UUID})/plans/({_UUID})/accept", path)
            if method == "POST" and match:
                body = self._body()
                return playout.accept_plan(store, principal, body["station_id"],
                    match.group(1), match.group(2), body["engine_instance"], body["engine_epoch"])
            match = re.fullmatch(rf"/v1/endpoints/({_UUID})/plans/({_UUID})/heartbeat", path)
            if method == "POST" and match:
                body = self._body()
                return playout.heartbeat_acceptance(
                    store, principal, body["station_id"], match.group(1),
                    match.group(2), body["engine_instance"], body["engine_epoch"])
            match = re.fullmatch(rf"/v1/endpoints/({_UUID})/commands", path)
            if method == "GET" and match:
                return playout.poll_commands(store, principal, query["station_id"],
                    match.group(1), query["engine_instance"], int(query["engine_epoch"]))
            match = re.fullmatch(rf"/v1/endpoints/({_UUID})/commands/({_UUID})/ack", path)
            if method == "POST" and match:
                body = self._body()
                return playout.acknowledge_command(store, principal, body["station_id"],
                    match.group(1), match.group(2), body["engine_instance"],
                    body["engine_epoch"], body["executed"])
            from .workflow_api import dispatch_workflow
            if path in {"/v1/workflows/desktop-rekordbox/preview", "/v1/workflows/desktop-rekordbox/apply"}:
                return dispatch_workflow(store, principal, method, path,
                                         lambda: self._body(max_bytes=6 * 1024 * 1024))
            return dispatch_workflow(store, principal, method, path, self._body)

        def do_GET(self) -> None:
            self._run("GET")

        def do_POST(self) -> None:
            self._run("POST")

    return Handler


def serve(store: Store, host: str = "127.0.0.1", port: int = 8000, *,
          max_connections: int = 32, request_timeout: float = 30.0) -> None:
    from .api_server import BoundedHTTPServer
    server = BoundedHTTPServer((host, port), make_handler(store),
                              max_connections=max_connections, request_timeout=request_timeout)
    try:
        server.serve_forever()
    finally:
        server.server_close()
