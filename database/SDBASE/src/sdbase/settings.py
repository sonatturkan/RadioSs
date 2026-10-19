"""Versioned station import profiles and per-user library view preferences."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
from typing import Any, Mapping
from uuid import UUID, uuid4

from .identity import Principal, require
from .rekordbox import PathMapRule
from .storage import Conflict, Store, StoreError


_POLICIES = {"fill_empty", "take_source", "preserve", "review_conflict"}
_CUE_POLICIES = {"ignore", "preserve_as_dj", "propose_radio_cues"}
_VIEWS = {"library", "archive", "on_air"}
_COLUMNS = {"title", "artist", "kind", "category", "duration_ms", "version_label",
            "lifecycle", "ready", "media_status", "last_played_at"}


def _id(value: str, name: str) -> str:
    try:
        return str(UUID(str(value)))
    except (ValueError, TypeError, AttributeError) as exc:
        raise StoreError(f"{name} must be a UUID") from exc


def validate_import_config(value: Mapping[str, Any]) -> dict:
    if not isinstance(value, Mapping):
        raise StoreError("import profile config must be an object")
    allowed = {"selected_node_paths", "selected_track_ids", "field_policy_overrides",
               "category_mapping", "path_rules", "allowed_roots", "cue_policy"}
    if set(value) - allowed:
        raise StoreError("unknown import profile option")
    config = deepcopy(dict(value))
    paths = config.get("selected_node_paths")
    if paths is not None and (not isinstance(paths, list) or any(
            not isinstance(path, list) or not path or any(
                isinstance(index, bool) or not isinstance(index, int) or index < 0
                for index in path) for path in paths)):
        raise StoreError("selected_node_paths must be lists of nonnegative ordinals")
    track_ids = config.get("selected_track_ids")
    if track_ids is not None and (not isinstance(track_ids, list) or any(
            not isinstance(track_id, (str, int)) or isinstance(track_id, bool)
            for track_id in track_ids)):
        raise StoreError("selected_track_ids must be a list of source IDs")
    policies = config.get("field_policy_overrides", {})
    if not isinstance(policies, dict) or any(not isinstance(key, str) or value not in _POLICIES
                                            for key, value in policies.items()):
        raise StoreError("invalid import field policies")
    categories = config.get("category_mapping", {})
    if not isinstance(categories, dict) or any(not isinstance(key, str) or not isinstance(val, str)
                                               for key, val in categories.items()):
        raise StoreError("invalid import category mapping")
    rules = config.get("path_rules", [])
    if not isinstance(rules, list):
        raise StoreError("path_rules must be a list")
    for rule in rules:
        if not isinstance(rule, dict) or set(rule) - {"source_root", "target_root", "case_sensitive"} or \
                not isinstance(rule.get("source_root"), str) or \
                not isinstance(rule.get("target_root"), str) or \
                (rule.get("case_sensitive") is not None and
                 not isinstance(rule.get("case_sensitive"), bool)):
            raise StoreError("invalid import path rule")
    roots = config.get("allowed_roots", [])
    if not isinstance(roots, list) or any(not isinstance(root, str) or not Path(root).is_absolute()
                                           for root in roots):
        raise StoreError("allowed_roots must contain absolute paths")
    if config.get("cue_policy", "preserve_as_dj") not in _CUE_POLICIES:
        raise StoreError("invalid cue import policy")
    return config


def put_import_profile(store: Store, principal: Principal, station_id: str, *,
                       name: str, library_key: str, config: Mapping[str, Any],
                       profile_id: str | None = None,
                       expected_revision: int | None = None,
                       status: str = "active") -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "manage", station_id)
    if store.get("stations", station_id) is None:
        raise StoreError("station not found")
    if not isinstance(name, str) or not name.strip() or \
            not isinstance(library_key, str) or not library_key.strip():
        raise StoreError("profile name and stable library key are required")
    if status not in {"active", "disabled"}:
        raise StoreError("invalid import profile status")
    config = validate_import_config(config)
    identifier = _id(profile_id, "profile_id") if profile_id else str(uuid4())
    existing = store.get("import_profiles", identifier)
    if existing and (existing["station_id"] != station_id or existing["_rev"] != expected_revision):
        raise Conflict("import profile changed or belongs to another station")
    if existing is None and expected_revision is not None:
        raise Conflict("import profile is absent at expected revision")
    body = {"station_id": station_id, "name": name.strip(),
            "library_key": library_key.strip(), "status": status,
            "config": config,
            "config_version": existing["config_version"] + 1 if existing else 1,
            "updated_by": principal.user_id}
    return store.put("import_profiles", identifier, body,
                     expected_revision=expected_revision, actor=principal.user_id)


def import_options(store: Store, principal: Principal, station_id: str,
                   profile_id: str) -> dict:
    station_id, profile_id = _id(station_id, "station_id"), _id(profile_id, "profile_id")
    require(store, principal, "import", station_id)
    row = store.get("import_profiles", profile_id)
    if row is None or row["station_id"] != station_id or row["status"] != "active":
        raise StoreError("active import profile not found in station")
    config = validate_import_config(row["config"])
    config["path_rules"] = [PathMapRule(**rule) for rule in config.get("path_rules", [])]
    config["allowed_roots"] = [Path(root) for root in config.get("allowed_roots", [])]
    return {"profile_id": profile_id, "config_version": row["config_version"],
            "library_key": row["library_key"], "options": config}


def put_view_profile(store: Store, principal: Principal, station_id: str, *,
                     name: str, view_kind: str, columns: list[str],
                     filters: Mapping[str, Any] | None = None,
                     profile_id: str | None = None,
                     expected_revision: int | None = None) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "view", station_id)
    if not isinstance(name, str) or not name.strip() or view_kind not in _VIEWS:
        raise StoreError("view name and valid view kind are required")
    if not isinstance(columns, list) or not columns or \
            any(not isinstance(column, str) or column not in _COLUMNS for column in columns) or \
            len(columns) != len(set(columns)):
        raise StoreError("view columns must be known and unique")
    if filters is not None and not isinstance(filters, Mapping):
        raise StoreError("view filters must be an object")
    filters = dict(filters or {})
    if set(filters) - {"kind", "category_id", "lifecycle", "ready"}:
        raise StoreError("unknown view filter")
    if "kind" in filters and filters["kind"] not in {
            "music", "advertisement", "jingle", "sweeper", "news", "program",
            "announcement", "effect", "voice_track", "other"}:
        raise StoreError("invalid view kind filter")
    if "lifecycle" in filters and filters["lifecycle"] not in {
            "draft", "active", "archived", "withdrawn", "trashed"}:
        raise StoreError("invalid view lifecycle filter")
    if "ready" in filters and not isinstance(filters["ready"], bool):
        raise StoreError("view ready filter must be Boolean")
    if "category_id" in filters and filters["category_id"] is not None:
        category = store.get("categories", _id(filters["category_id"], "category_id"))
        if category is None or category["station_id"] != station_id:
            raise StoreError("view category belongs to another station")
    identifier = _id(profile_id, "profile_id") if profile_id else str(uuid4())
    existing = store.get("view_profiles", identifier)
    if existing and (existing["station_id"] != station_id or
                     existing["owner_user_id"] != principal.user_id or
                     existing["_rev"] != expected_revision):
        raise Conflict("view profile changed or belongs to another user/station")
    if existing is None and expected_revision is not None:
        raise Conflict("view profile is absent at expected revision")
    body = {"station_id": station_id, "owner_user_id": principal.user_id,
            "name": name.strip(), "view_kind": view_kind,
            "columns": list(columns), "filters": filters,
            "config_version": existing["config_version"] + 1 if existing else 1}
    return store.put("view_profiles", identifier, body,
                     expected_revision=expected_revision, actor=principal.user_id)


def list_view_profiles(store: Store, principal: Principal, station_id: str) -> list[dict]:
    station_id = _id(station_id, "station_id")
    require(store, principal, "view", station_id)
    # A factory reset retires profiles (the store keeps every profile row).
    return [row for row in store.lookup("view_profiles", "owner_station",
                                        principal.user_id, station_id) if not row.get("retired_at")]


def list_import_profiles(store: Store, principal: Principal, station_id: str) -> list[dict]:
    station_id = _id(station_id, "station_id")
    require(store, principal, "import", station_id)
    return [row for row in store.lookup("import_profiles", "station", station_id) if not row.get("retired_at")]
