"""Transport contract for a RadioS endpoint, independent of a wire protocol.

The central adapter is responsible for endpoint authentication, authorization,
and returning only that endpoint's approved distribution and media streams.
Media and event integrity are checked again by the local endpoint agent.
"""

from __future__ import annotations

from contextlib import AbstractContextManager
from typing import Any, BinaryIO, Mapping, Protocol


class TransportError(RuntimeError):
    """Central service is unavailable or rejected an operation."""


class EndpointTransport(Protocol):
    def plan_state(self, endpoint_id: str) -> Mapping[str, Any]: ...

    def fetch_snapshot(self, station_id: str, endpoint_id: str,
                       revision_id: str) -> Mapping[str, Any]: ...

    def open_media(self, station_id: str, endpoint_id: str,
                   media_file_id: str) -> AbstractContextManager[BinaryIO]: ...

    def report_verified_media(self, station_id: str, endpoint_id: str,
                              media_file_id: str, sha256: str,
                              storage_uri: str) -> Mapping[str, Any]: ...

    def acknowledge_delivery(self, station_id: str, endpoint_id: str,
                             revision_id: str,
                             snapshot_sha256: str) -> Mapping[str, Any]: ...

    def accept_plan(self, station_id: str, endpoint_id: str, revision_id: str,
                    engine_instance: str,
                    engine_epoch: int) -> Mapping[str, Any]: ...

    def heartbeat_acceptance(self, station_id: str, endpoint_id: str,
                             revision_id: str, engine_instance: str,
                             engine_epoch: int) -> Mapping[str, Any]: ...

    def poll_commands(self, station_id: str, endpoint_id: str,
                      engine_instance: str,
                      engine_epoch: int) -> list[Mapping[str, Any]]: ...

    def acknowledge_command(self, station_id: str, endpoint_id: str,
                            command_id: str, engine_instance: str,
                            engine_epoch: int,
                            executed: bool) -> Mapping[str, Any]: ...

    def record_playout_event(self, payload: Mapping[str, Any]) -> Mapping[str, Any]: ...
