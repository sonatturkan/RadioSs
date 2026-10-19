"""Authenticated HTTP transport for a remote SDBASE endpoint agent."""

from __future__ import annotations

from contextlib import contextmanager
import json
from typing import Any, BinaryIO, Iterator, Mapping
from http.client import HTTPException
from urllib.parse import urlencode
from uuid import UUID

from .endpoint_protocol import TransportError
from .http_pool import HTTPConnectionPool


class HTTPTransport:
    def __init__(self, base_url: str, bearer_token: str, *, timeout: float = 20.0,
                 max_connections: int = 4):
        if not base_url.startswith(("https://", "http://")) or not bearer_token:
            raise ValueError("HTTP(S) URL and endpoint bearer token are required")
        self.base_url = base_url.rstrip("/")
        self.token = bearer_token
        self.timeout = timeout
        self.pool = HTTPConnectionPool(self.base_url, timeout=timeout,
                                       max_connections=max_connections)

    def close(self):
        self.pool.close()

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.close()

    def _open(self, method: str, path: str, body: Mapping[str, Any] | None = None,
              query: Mapping[str, Any] | None = None):
        request_path = path
        if query:
            request_path += "?" + urlencode(query)
        data = json.dumps(body, sort_keys=True, separators=(",", ":"),
                          ensure_ascii=False).encode("utf-8") if body is not None else None
        try:
            response = self.pool.request(method, request_path, body=data,
                         headers={"Authorization": f"Bearer {self.token}",
                                  "Content-Type": "application/json"})
            if response.status >= 400:
                status = response.status
                response.close()
                raise TransportError(f"central service rejected {path}: HTTP {status}")
            return response
        except (HTTPException, TimeoutError, OSError) as exc:
            raise TransportError(f"central service unavailable for {path}") from exc

    def _json(self, method: str, path: str, body: Mapping[str, Any] | None = None,
              query: Mapping[str, Any] | None = None) -> Any:
        with self._open(method, path, body, query) as response:
            try:
                return json.load(response)
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                raise TransportError("central service returned invalid JSON") from exc
            except (HTTPException, TimeoutError, OSError) as exc:
                raise TransportError("central service response was interrupted") from exc

    def plan_state(self, endpoint_id: str) -> Mapping[str, Any]:
        return self._json("GET", f"/v1/endpoints/{UUID(endpoint_id)}/state")

    def fetch_snapshot(self, station_id: str, endpoint_id: str,
                       revision_id: str) -> Mapping[str, Any]:
        return self._json("GET", f"/v1/endpoints/{UUID(endpoint_id)}/snapshots/{UUID(revision_id)}",
                          query={"station_id": station_id})

    @contextmanager
    def open_media(self, station_id: str, endpoint_id: str,
                   media_file_id: str) -> Iterator[BinaryIO]:
        path = f"/v1/endpoints/{UUID(endpoint_id)}/media/{UUID(media_file_id)}"
        with self._open("GET", path, query={"station_id": station_id}) as response:
            yield response

    def report_verified_media(self, station_id: str, endpoint_id: str,
                              media_file_id: str, sha256: str,
                              storage_uri: str) -> Mapping[str, Any]:
        return self._json("POST", f"/v1/endpoints/{UUID(endpoint_id)}/media/{UUID(media_file_id)}/verify",
                          {"station_id": station_id, "sha256": sha256,
                           "storage_uri": storage_uri})

    def acknowledge_delivery(self, station_id: str, endpoint_id: str,
                             revision_id: str, snapshot_sha256: str) -> Mapping[str, Any]:
        return self._json("POST", f"/v1/endpoints/{UUID(endpoint_id)}/deliveries/{UUID(revision_id)}/ack",
                          {"station_id": station_id, "snapshot_sha256": snapshot_sha256})

    def accept_plan(self, station_id: str, endpoint_id: str, revision_id: str,
                    engine_instance: str, engine_epoch: int) -> Mapping[str, Any]:
        return self._json("POST", f"/v1/endpoints/{UUID(endpoint_id)}/plans/{UUID(revision_id)}/accept",
                          {"station_id": station_id, "engine_instance": engine_instance,
                           "engine_epoch": engine_epoch})

    def heartbeat_acceptance(self, station_id: str, endpoint_id: str,
                             revision_id: str, engine_instance: str,
                             engine_epoch: int) -> Mapping[str, Any]:
        return self._json("POST", f"/v1/endpoints/{UUID(endpoint_id)}/plans/{UUID(revision_id)}/heartbeat",
                          {"station_id": station_id, "engine_instance": engine_instance,
                           "engine_epoch": engine_epoch})

    def poll_commands(self, station_id: str, endpoint_id: str,
                      engine_instance: str, engine_epoch: int) -> list[Mapping[str, Any]]:
        return self._json("GET", f"/v1/endpoints/{UUID(endpoint_id)}/commands",
                          query={"station_id": station_id,
                                 "engine_instance": engine_instance,
                                 "engine_epoch": engine_epoch})

    def acknowledge_command(self, station_id: str, endpoint_id: str,
                            command_id: str, engine_instance: str,
                            engine_epoch: int, executed: bool) -> Mapping[str, Any]:
        return self._json("POST", f"/v1/endpoints/{UUID(endpoint_id)}/commands/{UUID(command_id)}/ack",
                          {"station_id": station_id, "engine_instance": engine_instance,
                           "engine_epoch": engine_epoch, "executed": executed})

    def record_playout_event(self, payload: Mapping[str, Any]) -> Mapping[str, Any]:
        return self._json("POST", "/v1/playout/events", payload)
