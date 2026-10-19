"""Shared validation helpers for reviewed radio production records."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
from typing import Any, Mapping
from uuid import UUID

from .storage import Conflict, Store, StoreError


class ProductionError(StoreError):
    """Invalid reviewed production decision or controller mapping."""


def _id(value: str | UUID, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise ProductionError(f"{field} must be a UUID") from exc


def _body(row: Mapping[str, Any]) -> dict:
    return {key: value for key, value in row.items() if key not in {"_id", "_rev"}}


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _sha(value: Any) -> str:
    raw = json.dumps(value, sort_keys=True, separators=(",", ":"),
                     ensure_ascii=False, allow_nan=False).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def _version_in_station(store: Store, station_id: str, version_id: str) -> tuple[dict, dict]:
    version = store.get("versions", version_id)
    if version is None:
        raise ProductionError("content version is absent")
    item = store.get("items", version["item_id"])
    if item is None or not store.lookup("station_items", "station_item", station_id, item["_id"]):
        raise ProductionError("content version is outside station")
    return version, item


def _verified_media(store: Store, version_id: str, media_file_id: str | None = None) -> dict:
    if media_file_id is not None:
        media = store.get("media_files", _id(media_file_id, "media_file_id"))
        if media is None or media.get("version_id") != version_id or media.get("status") != "verified":
            raise Conflict("selected media file is not verified for this content version")
        return media
    verified = [row for row in store.lookup("media_files", "version", version_id)
                if row.get("status") == "verified"]
    if len(verified) != 1:
        raise Conflict("choose exactly one verified media file for this version")
    return verified[0]


