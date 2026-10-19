"""Versioned, lossless execution contract shared by the center and endpoint.

The contract contains the approved entry's execution fields verbatim.  A
receiver validates the same tagged event rules used when the draft was saved;
unknown contract versions and incomplete entries are rejected before staging.
"""

from __future__ import annotations

from copy import deepcopy
import re
from typing import Any, Mapping
from uuid import UUID

from .planning import validate_entry, validate_plan
from .playback_markers import validate_frozen_markers
from .storage import StoreError


CONTRACT_VERSION = 2
EXECUTION_FIELDS = ("ordinal", "scheduled_at", "state", "payload",
                    "duration_kind", "planned_duration_ms", "timing",
                    "transition", "ad_break_id")
ENTRY_FIELDS = frozenset(("plan_entry_id", "entry_kind", "version_id",
                          "media_file_id", "playback_markers", *EXECUTION_FIELDS))
_LEGACY_ENTRY_FIELDS = ENTRY_FIELDS - {"playback_markers"}
_CODE = re.compile(r"[A-Za-z0-9_.:-]{1,100}\Z")


class SnapshotContractError(ValueError):
    """A plan snapshot cannot be executed without losing approved intent."""


def _validate_destination(snapshot: Mapping[str, Any]) -> None:
    binding = snapshot.get("execution_binding")
    fields = {"source", "service_key", "endpoint_id", "output_id",
              "station_output_id", "broadcast_service_id"}
    if not isinstance(binding, Mapping) or set(binding) != fields:
        raise SnapshotContractError("snapshot needs the exact frozen execution destination")
    for field in ("station_id", "endpoint_id"):
        try:
            UUID(str(snapshot.get(field)))
        except (TypeError, ValueError, AttributeError) as exc:
            raise SnapshotContractError(f"snapshot {field} must be a UUID") from exc
    for field in ("endpoint_id", "station_output_id"):
        try:
            UUID(str(binding[field]))
        except (TypeError, ValueError, AttributeError) as exc:
            raise SnapshotContractError(f"execution {field} must be a UUID") from exc
    if binding["endpoint_id"] != snapshot["endpoint_id"] or \
            binding["service_key"] != snapshot.get("service_key"):
        raise SnapshotContractError("execution destination differs from snapshot scope")
    for field in ("service_key", "output_id"):
        if not isinstance(binding[field], str) or not _CODE.fullmatch(binding[field]):
            raise SnapshotContractError(f"execution {field} must be a concrete short code")
    if binding["source"] == "broadcast_service":
        try:
            UUID(str(binding["broadcast_service_id"]))
        except (TypeError, ValueError, AttributeError) as exc:
            raise SnapshotContractError("bound service needs its frozen UUID") from exc
    elif binding["source"] != "unique_default" or binding["service_key"] != "default" or \
            binding["broadcast_service_id"] is not None:
        raise SnapshotContractError("unsupported frozen execution destination source")


def freeze_entry(row: Mapping[str, Any], media_file_id: str | None = None,
                  playback_markers: Mapping[str, Any] | None = None) -> dict:
    """Copy all approved execution fields into one immutable snapshot entry."""
    result = {"plan_entry_id": str(row["_id"]),
              "entry_kind": row["event_kind"]}
    for field in EXECUTION_FIELDS:
        if field not in row:
            raise SnapshotContractError(f"approved entry lacks {field}")
        result[field] = deepcopy(row[field])
    if result["entry_kind"] == "audio":
        version_id = result["payload"].get("version_id")
        if not version_id or not media_file_id or playback_markers is None:
            raise SnapshotContractError("audio needs a resolved version, media file and frozen playback markers")
        result["version_id"] = version_id
        result["media_file_id"] = media_file_id
        result["playback_markers"] = deepcopy(dict(playback_markers))
        try:
            validate_frozen_markers(result["playback_markers"], version_id=version_id,
                                    media_file_id=media_file_id, payload=result["payload"],
                                    duration_kind=result["duration_kind"],
                                    planned_duration_ms=result["planned_duration_ms"])
        except StoreError as exc:
            raise SnapshotContractError(str(exc)) from exc
    elif media_file_id is not None:
        raise SnapshotContractError("non-audio entry cannot name a media file")
    elif playback_markers is not None:
        raise SnapshotContractError("non-audio entry cannot name playback markers")
    return result


def validate_frozen_entry(entry: Mapping[str, Any], station_timezone: str,
                          service_date: str, *, legacy: bool = False) -> None:
    """Reject loss, unknown fields, or changed meaning in a frozen entry."""
    if not isinstance(entry, Mapping):
        raise SnapshotContractError("snapshot entry must be an object")
    kind = entry.get("entry_kind")
    fields = _LEGACY_ENTRY_FIELDS if legacy else ENTRY_FIELDS
    expected = fields if kind == "audio" else fields - {"version_id", "media_file_id", "playback_markers"}
    if set(entry) != expected:
        missing = sorted(expected - set(entry))
        extra = sorted(set(entry) - expected)
        raise SnapshotContractError(f"snapshot entry fields differ: missing={missing}, extra={extra}")
    try:
        UUID(str(entry["plan_entry_id"]))
    except (TypeError, ValueError, AttributeError) as exc:
        raise SnapshotContractError("plan_entry_id must be a UUID") from exc
    candidate = {field: entry[field] for field in EXECUTION_FIELDS}
    candidate["event_kind"] = kind
    candidate["id"] = entry["plan_entry_id"]
    validated = validate_entry(candidate, station_timezone, service_date)
    if not validated["valid"] or validated["normalized"].get("requires_resolution"):
        codes = [issue["code"] for issue in validated["errors"]]
        raise SnapshotContractError("invalid execution entry: " + ", ".join(codes))
    normalized = validated["normalized"]
    if any(entry[field] != normalized[field] for field in EXECUTION_FIELDS):
        raise SnapshotContractError("snapshot entry is not in canonical execution form")
    if kind == "audio":
        try:
            UUID(str(entry["version_id"]))
            UUID(str(entry["media_file_id"]))
        except (TypeError, ValueError, AttributeError) as exc:
            raise SnapshotContractError("audio version and media IDs must be UUIDs") from exc
        if entry["payload"].get("version_id") != entry["version_id"]:
            raise SnapshotContractError("audio version differs from payload")
        if not legacy:
            try:
                validate_frozen_markers(entry["playback_markers"],
                                        version_id=entry["version_id"],
                                        media_file_id=entry["media_file_id"],
                                        payload=entry["payload"],
                                        duration_kind=entry["duration_kind"],
                                        planned_duration_ms=entry["planned_duration_ms"])
            except StoreError as exc:
                raise SnapshotContractError(str(exc)) from exc


def validate_snapshot_contract(snapshot: Mapping[str, Any], *, allow_legacy: bool = False) -> None:
    """Validate execution; v1 may be read explicitly for historical inspection only."""
    if not isinstance(snapshot, Mapping) or type(snapshot.get("contract_version")) is not int:
        raise SnapshotContractError("unsupported or missing plan contract version")
    version = snapshot["contract_version"]
    if version != CONTRACT_VERSION and not (version == 1 and allow_legacy):
        raise SnapshotContractError("unsupported plan contract; v1 snapshots need a new reviewed approval")
    legacy = version == 1
    if not legacy:
        _validate_destination(snapshot)
    entries = snapshot.get("entries")
    if not isinstance(entries, list):
        raise SnapshotContractError("snapshot entries must be a list")
    for entry in entries:
        validate_frozen_entry(entry, snapshot.get("station_timezone"),
                              snapshot.get("service_date"), legacy=legacy)
    if not legacy:
        assets = snapshot.get("media")
        if not isinstance(assets, list) or any(not isinstance(asset, Mapping) for asset in assets):
            raise SnapshotContractError("snapshot media must be a list of manifest objects")
        for asset in assets:
            if set(asset) != {"media_file_id", "sha256", "size_bytes"}:
                raise SnapshotContractError("media manifest fields differ")
            try:
                UUID(str(asset["media_file_id"]))
            except (TypeError, ValueError, AttributeError) as exc:
                raise SnapshotContractError("media manifest ID must be a UUID") from exc
            if not isinstance(asset["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", asset["sha256"]) or \
                    type(asset["size_bytes"]) is not int or asset["size_bytes"] < 0:
                raise SnapshotContractError("media manifest SHA or size is invalid")
        media = {asset["media_file_id"]: asset for asset in assets}
        if len(media) != len(assets):
            raise SnapshotContractError("snapshot media IDs must be unique")
        for entry in entries:
            if entry["entry_kind"] in {"audio", "live", "stream"} and \
                    entry["payload"].get("output_id") != snapshot["execution_binding"]["output_id"]:
                raise SnapshotContractError("sound event differs from its approved concrete output")
            if entry["entry_kind"] != "audio":
                continue
            asset = media.get(entry["media_file_id"])
            if asset is None:
                raise SnapshotContractError("playback marker source is missing from media manifest")
            try:
                validate_frozen_markers(entry["playback_markers"],
                                        station_id=snapshot.get("station_id"),
                                        media_sha256=asset.get("sha256"))
            except StoreError as exc:
                raise SnapshotContractError(str(exc)) from exc
    plan_entries = [{field: entry[field] for field in EXECUTION_FIELDS} |
                    {"id": entry["plan_entry_id"],
                     "event_kind": entry["entry_kind"]}
                    for entry in entries]
    result = validate_plan(plan_entries, snapshot.get("station_timezone"),
                           snapshot.get("service_date"))
    if not result["valid"] or not result["approvable"]:
        codes = [issue["code"] for issue in result.get("conflicts", [])]
        raise SnapshotContractError("invalid execution plan: " + ", ".join(codes))
