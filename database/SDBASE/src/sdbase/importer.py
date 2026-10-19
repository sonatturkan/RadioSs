"""Read-only Rekordbox source preview and policy-controlled `.sdb` import.

The public profile adapter reads exported XML 1.0.0 bytes. The desktop bridge
can supply a separately parsed DeviceSQL snapshot to the same import planner.
Neither path opens a Rekordbox application database or referenced audio file.
Source fields are proposals; DJ marks/beatgrid stay source-only and cannot approve radio
cues, media, or playout history. All Store changes for one import are committed
in a single optimistic transaction; a reused idempotency key returns its
recorded result only for the same source, selection and policy fingerprint.
"""

from __future__ import annotations

from datetime import date, datetime, timezone
from decimal import Decimal
import hashlib
import json
from pathlib import Path
from typing import Any, Mapping, Sequence
from uuid import NAMESPACE_URL, UUID, uuid5

from .catalog import require_shared_item_permission
from .identity import Principal, require
from .metadata import _validate_value
from .rekordbox import (
    PathMapRule, PlaylistNode, RekordboxLibrary, SourceValue, Track,
    TRACK_FIELD_SPECS, map_media_path, parse_rekordbox_xml, plan_rekordbox_import,
)
from .storage import Conflict, Store, StoreError


SOURCE_KIND = "rekordbox_xml"
FIELD_TARGETS = {
    "Artist": ("artist_credit", "item", "string"),
    "Composer": ("composer_credit", "item", "string"),
    "Remixer": ("remixer_credit", "item", "string"),
    "Album": ("album", "item", "string"),
    "Genre": ("genre", "item", "string"),
    "Year": ("release_year", "item", "integer"),
    "Comments": ("comment", "item", "string"),
    "AverageBpm": ("bpm", "version", "decimal"),
    "Location": ("source_location", "version", "string"),
}
SOURCE_ONLY_FIELDS = frozenset(
    spec.source for spec in TRACK_FIELD_SPECS
    if spec.source not in {"TrackID", "Name", *FIELD_TARGETS}
)
DEFAULT_ACTIONS = {spec.source: {
    "fill_if_empty": "fill_empty", "review": "review_conflict",
    "preserve": "preserve",
}.get(spec.default_policy, "preserve") for spec in TRACK_FIELD_SPECS}
VALID_ACTIONS = frozenset({"fill_empty", "take_source", "preserve", "review_conflict"})


class ImportError(StoreError):
    """Invalid XML import selection, profile, or source binding."""


def _id(value: str | UUID, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise ImportError(f"{field} must be a UUID") from exc


def _jsonable(value: Any) -> Any:
    if isinstance(value, (date, datetime, UUID)):
        return value.isoformat() if isinstance(value, (date, datetime)) else str(value)
    if isinstance(value, Decimal):
        return str(value)  # Decimal precision and a raw source value are retained.
    if isinstance(value, Mapping):
        return {str(key): _jsonable(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_jsonable(item) for item in value]
    return value


def _canonical(value: Any) -> str:
    return json.dumps(_jsonable(value), sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False, allow_nan=False)


def _hash(value: Any) -> str:
    return hashlib.sha256(_canonical(value).encode("utf-8")).hexdigest()


def _source_value(value: SourceValue) -> dict[str, Any]:
    result: dict[str, Any] = {"status": value.status}
    if value.raw is not None:
        result["raw"] = value.raw
    if value.status == "value":
        result["value"] = _jsonable(value.value)
    return result


def _track_snapshot(track: Track) -> dict:
    return {
        "track_id": track.track_id,
        "fields": {name: _source_value(value) for name, value in track.fields.items()},
        "source_path": track.source_path,
        "unmapped_attributes": dict(track.unmapped_attributes),
        "tempos": [{"attributes": {key: _source_value(value)
                                     for key, value in point.attributes.items()},
                    "unmapped_attributes": dict(point.unmapped_attributes)}
                   for point in track.tempos],
        "marks": [{"attributes": {key: _source_value(value)
                                    for key, value in mark.attributes.items()},
                   "unmapped_attributes": dict(mark.unmapped_attributes)}
                  for mark in track.marks],
    }


def _analysis(track: Track, namespace: str, version_id: str) -> dict:
    """Keep source units/raw spelling; never infer approved radio cue meaning."""
    marks = []
    for ordinal, mark in enumerate(track.marks):
        marks.append({
            "ordinal": ordinal, "source_unit": "seconds",
            "attributes": {name: _source_value(value) for name, value in mark.attributes.items()},
            "unmapped_attributes": dict(mark.unmapped_attributes),
        })
    tempos = []
    for ordinal, point in enumerate(track.tempos):
        tempos.append({
            "ordinal": ordinal, "start_unit": "seconds", "bpm_unit": "BPM",
            "attributes": {name: _source_value(value) for name, value in point.attributes.items()},
            "unmapped_attributes": dict(point.unmapped_attributes),
        })
    return {"version_id": version_id, "source_namespace": namespace,
            "source_track_id": track.track_id, "marks": marks, "tempos": tempos,
            "status": "source_only", "radio_cues_created": 0}


def decide_field(source: SourceValue, target: Any, *, target_exists: bool,
                 action: str, protected: bool = False) -> tuple[str, str]:
    """Choose set/preserve/suggest/conflict without treating absence as clear."""
    if action not in VALID_ACTIONS:
        raise ImportError(f"unsupported field action: {action}")
    if source.status != "value" or source.value is None:
        return "preserve", f"source_{source.status}"
    proposed = _jsonable(source.value)
    if target_exists and target == proposed:
        return "preserve", "unchanged"
    if protected:
        return (("conflict", "approved_or_editorial_target") if target_exists
                else ("suggest", "approval_required"))
    if action == "preserve":
        return "preserve", "policy_preserve"
    if action == "review_conflict":
        return (("conflict", "review_conflict") if target_exists
                else ("suggest", "review_required"))
    if action == "fill_empty":
        return (("conflict", "target_has_value") if target_exists
                else ("set", "empty_target"))
    return "set", "take_source"


def _data(row: Mapping[str, Any]) -> dict:
    return {key: value for key, value in row.items() if key not in {"_id", "_rev"}}


def _namespace(library_key: str, source_kind: str = SOURCE_KIND) -> str:
    if not isinstance(library_key, str) or not library_key.strip() or len(library_key) > 128:
        raise ImportError("stable library_key must contain 1..128 characters")
    return f"{source_kind}:{library_key.strip()}"


def _paths(value: Sequence[Sequence[int]] | None) -> tuple[tuple[int, ...], ...] | None:
    if value is None:
        return None
    if isinstance(value, (str, bytes)):
        raise ImportError("selected_node_paths must be a list of ordinal paths")
    try:
        result = tuple(tuple(path) for path in value)
    except TypeError as exc:
        raise ImportError("selected_node_paths must be a list of ordinal paths") from exc
    if any(not path or any(type(part) is not int or part < 0 for part in path)
           for path in result):
        raise ImportError("playlist paths must be nonempty nonnegative ordinal paths")
    return result


def _track_ids(value: Sequence[str | int] | None) -> tuple[str, ...] | None:
    if value is None:
        return None
    if isinstance(value, (str, bytes)):
        raise ImportError("selected_track_ids must be a list")
    return tuple(str(part) for part in value)


def _policies(overrides: Mapping[str, str] | None) -> dict[str, str]:
    if overrides is None:
        return {}
    if not isinstance(overrides, Mapping) or any(
        key not in {"Name", *FIELD_TARGETS} or action not in VALID_ACTIONS
        for key, action in overrides.items()
    ):
        raise ImportError("field policy overrides contain unsupported fields or actions")
    return dict(overrides)


def _definition(store: Store, key: str, scope: str, data_type: str) -> dict | None:
    matches = store.lookup("field_definitions", "key", key)
    if not matches:
        return None
    field = matches[0]
    if field.get("scope") != scope or field.get("data_type") != data_type:
        return None
    return field


def _mapping(store: Store, namespace: str, track_id: str) -> dict | None:
    matches = store.lookup("external_ids", "source", namespace, track_id)
    if len(matches) > 1:
        raise Conflict("source TrackID maps to multiple target identities")
    return matches[0] if matches else None


def _mapped_category(store: Store, station_id: str, genre: SourceValue,
                     mapping: Mapping[str, str]) -> str | None:
    if genre.status != "value" or not isinstance(genre.value, str):
        return None
    configured = mapping.get(genre.value)
    if configured is None:
        return None
    candidates = [row for row in store.lookup("categories", "station_code", station_id, configured)
                  if row.get("archived_at") is None]
    if not candidates:
        try:
            category_id = _id(configured, "category_id")
        except ImportError as exc:
            raise ImportError(f"category mapping target is not in station: {configured}") from exc
        candidate = store.get("categories", category_id)
        if candidate and candidate.get("station_id") == station_id and not candidate.get("archived_at"):
            return category_id
        raise ImportError(f"category mapping target is not in station: {configured}")
    return candidates[0]["_id"]


def _playlist_path(node: PlaylistNode) -> str:
    return "/".join(str(part) for part in node.path)


def _media_proposal(track: Track, rules: Sequence[PathMapRule], roots: Sequence[str | Path]) -> dict:
    if not rules:
        return {"status": "unmapped", "source_path": track.source_path,
                "target_path": None, "detail": "No path mapping is configured."}
    try:
        mapping = map_media_path(track.source_path, rules, allowed_roots=roots)
    except ValueError as exc:
        raise ImportError(str(exc)) from exc
    return {"status": mapping.status, "source_path": mapping.source_path,
            "target_path": mapping.target_path, "detail": mapping.detail}


def _prepare(store: Store, principal: Principal, station_id: str | UUID,
             library_key: str, xml_bytes: bytes, *,
             selected_node_paths: Sequence[Sequence[int]] | None,
             selected_track_ids: Sequence[str | int] | None,
             field_policy_overrides: Mapping[str, str] | None,
             category_mapping: Mapping[str, str] | None,
             path_rules: Sequence[PathMapRule], allowed_roots: Sequence[str | Path],
             cue_policy: str, source_library: RekordboxLibrary | None = None,
             source_kind: str = SOURCE_KIND) -> dict:
    sequence = store.sequence
    station = _id(station_id, "station_id")
    require(store, principal, "import", station)
    if not isinstance(xml_bytes, bytes):
        raise ImportError("XML snapshot must be bytes")
    if source_kind not in {SOURCE_KIND, "rekordbox_device"}:
        raise ImportError("unsupported source reader")
    namespace = _namespace(library_key, source_kind)
    paths = _paths(selected_node_paths)
    ids = _track_ids(selected_track_ids)
    overrides = _policies(field_policy_overrides)
    if cue_policy not in {"ignore", "preserve_as_dj", "propose_radio_cues"}:
        raise ImportError("unsupported cue policy")
    if not isinstance(category_mapping, (Mapping, type(None))):
        raise ImportError("category_mapping must be an object")
    categories = dict(category_mapping or {})
    if any(not isinstance(key, str) or not isinstance(value, str)
           for key, value in categories.items()):
        raise ImportError("category mapping keys and values must be strings")
    library = source_library if source_library is not None else parse_rekordbox_xml(xml_bytes)
    try:
        plan = plan_rekordbox_import(library, namespace,
                                    selected_node_paths=paths,
                                    selected_track_ids=ids)
    except ValueError as exc:
        raise ImportError(str(exc)) from exc
    for track_key in plan.track_keys:
        mapping = _mapping(store, namespace, track_key.source_track_id)
        if mapping and mapping.get("target_collection") == "items" and \
                store.get("items", mapping["target_id"]) is not None:
            require_shared_item_permission(store, principal, mapping["target_id"], "import")
    definitions = {source: _definition(store, target, scope, kind)
                   for source, (target, scope, kind) in FIELD_TARGETS.items()}
    request = {"station_id": station, "source_namespace": namespace,
               "source_sha256": library.source_sha256,
               "reader_version": library.reader_version,
               "selected_node_paths": [list(path) for path in paths] if paths is not None else None,
               "selected_track_ids": list(ids) if ids is not None else None,
               "field_policy_overrides": overrides, "category_mapping": categories,
               "path_rules": [_jsonable(rule.__dict__) for rule in path_rules],
               "allowed_roots": [str(root) for root in allowed_roots],
               "cue_policy": cue_policy}
    return {"station_id": station, "namespace": namespace, "library": library, "source_kind": source_kind,
            "sequence": sequence,
            "plan": plan, "overrides": overrides, "definitions": definitions,
            "categories": categories, "path_rules": path_rules,
            "allowed_roots": allowed_roots, "cue_policy": cue_policy,
            "request": request, "request_fingerprint": _hash(request)}


def _track_decisions(store: Store, prepared: dict, track: Track,
                     losses: list[dict]) -> dict:
    station = prepared["station_id"]
    namespace = prepared["namespace"]
    mapping = _mapping(store, namespace, track.track_id)
    item = store.get("items", mapping["target_id"]) if mapping else None
    version = store.get("versions", mapping.get("target_version_id")) if mapping and mapping.get("target_version_id") else None
    identity_conflict = None
    if mapping and (mapping.get("target_collection") != "items" or item is None or
                    version is None or version.get("item_id") != item["_id"]):
        identity_conflict = "source_identity_target_is_missing_or_incompatible"
    elif item and (item.get("kind") != "music" or item.get("lifecycle") in {"trashed", "withdrawn"}):
        identity_conflict = "mapped_item_is_not_editable_music"
    item_id = item["_id"] if item else None
    version_id = version["_id"] if version else None
    approved = bool(item and (item.get("lifecycle") in {"active", "archived"} or
                              any(row.get("status") == "approved"
                                  for row in store.lookup("versions", "item", item_id))))
    decisions: dict[str, dict] = {}
    title = track.fields["Name"]
    current_title = item.get("title") if item else None
    fallback = bool(item and item.get("title_source") == "rekordbox_fallback")
    title_action = prepared["overrides"].get("Name", "fill_empty")
    if item is None:
        title_decision = ("set", "new_item") if title.status == "value" and str(title.value).strip() else ("preserve", f"source_{title.status}")
    else:
        title_decision = decide_field(title, current_title,
                                      target_exists=bool(current_title) and not fallback,
                                      action=title_action, protected=approved)
    decisions["Name"] = {"target": "items.title", "source": _source_value(title),
                         "current": current_title, "decision": title_decision[0],
                         "reason": title_decision[1],
                         "target_id": item_id, "target_revision": item["_rev"] if item else None}
    for source in sorted(SOURCE_ONLY_FIELDS):
        source_value = track.fields[source]
        if source_value.status == "value":
            losses.append({"kind": "source_only", "object_key": f"track:{track.track_id}",
                           "source_field": source,
                           "detail": "XML value is retained in the source snapshot; no compatible approved target mapping is configured."})
    for source, (target_key, scope, _kind) in FIELD_TARGETS.items():
        source_value = track.fields[source]
        field = prepared["definitions"][source]
        if field is None:
            if source_value.status == "value":
                losses.append({"kind": "unsupported", "object_key": f"track:{track.track_id}",
                               "source_field": source,
                               "detail": f"Compatible target field {target_key} is not configured; source value retained."})
            decisions[source] = {"target": target_key, "source": _source_value(source_value),
                                 "current": None, "decision": "preserve",
                                 "reason": "target_definition_missing", "field_id": None,
                                 "value_id": None, "value_revision": None}
            continue
        target_id = item_id if scope == "item" else version_id
        collection = "items" if scope == "item" else "versions"
        matches = (store.lookup("metadata_values", "field_target", field["_id"],
                                collection, target_id, "*") if target_id else [])
        current = matches[0] if matches else None
        target_exists = current is not None and (current.get("is_cleared") or
                                                  current.get("value") is not None)
        protected = bool(
            approved or (version and version.get("status") == "approved") or
            (current and (current.get("approval_status") == "approved" or
                          current.get("is_cleared"))) or
            field.get("metadata_class") in {"editorial", "commercial", "operational"}
        )
        action = prepared["overrides"].get(
            source, field.get("source_policies", {}).get(prepared["source_kind"],
                    DEFAULT_ACTIONS.get(source, "preserve")))
        if action not in VALID_ACTIONS:
            raise ImportError(f"invalid configured source policy for {target_key}")
        decision, reason = decide_field(source_value, current.get("value") if current else None,
                                        target_exists=target_exists, action=action,
                                        protected=protected)
        if source_value.status == "value" and decision in {"set", "suggest"}:
            try:
                _validate_value(field, _jsonable(source_value.value))
            except StoreError as exc:
                decision, reason = "conflict", "target_validation_failed"
                losses.append({"kind": "invalid", "object_key": f"track:{track.track_id}",
                               "source_field": source, "detail": str(exc)})
        decisions[source] = {"target": target_key, "source": _source_value(source_value),
                             "current": current.get("value") if current else None,
                             "decision": decision, "reason": reason,
                             "field_id": field["_id"], "field_revision": field["_rev"],
                             "value_id": current["_id"] if current else None,
                             "value_revision": current["_rev"] if current else None,
                             "target_collection": collection, "target_id": target_id}
    genre = track.fields["Genre"]
    category = _mapped_category(store, station, genre, prepared["categories"])
    membership = (store.lookup("station_items", "station_item", station, item_id)[0]
                  if item_id and store.lookup("station_items", "station_item", station, item_id)
                  else None)
    category_decision = None
    if category:
        current = membership.get("category_id") if membership else None
        if approved and current is None:
            category_decision = "conflict"
        elif current is None:
            category_decision = "set"
        elif current == category:
            category_decision = "preserve"
        else:
            category_decision = "conflict"
        decisions["category"] = {"target": "station_items.category_id",
                                 "source": category, "current": current,
                                 "decision": category_decision,
                                 "reason": "category_target_has_value" if category_decision == "conflict" else
                                           "empty_station_category" if category_decision == "set" else "unchanged"}
    if identity_conflict:
        action = "conflict"
    elif mapping is None:
        action = "create"
    elif any(value["decision"] == "conflict" for value in decisions.values()):
        action = "conflict"
    elif any(value["decision"] == "set" for value in decisions.values()) or membership is None:
        action = "update"
    else:
        action = "preserve"
    media = _media_proposal(track, prepared["path_rules"], prepared["allowed_roots"])
    if media["status"] != "mapped":
        losses.append({"kind": "unresolved", "object_key": f"track:{track.track_id}",
                       "source_field": "Location", "detail": media["detail"]})
    cue_status = ("ignored" if prepared["cue_policy"] == "ignore" else
                  "awaiting_review" if prepared["cue_policy"] == "propose_radio_cues" and track.marks
                  else "source_only")
    if cue_status == "awaiting_review":
        losses.append({"kind": "requires_review", "object_key": f"track:{track.track_id}",
                       "source_field": "POSITION_MARK",
                       "detail": "DJ marks have no automatic radio cue meaning."})
    return {"source_id": track.track_id, "source_fingerprint": _hash(_track_snapshot(track)),
            "action": action, "identity_conflict": identity_conflict,
            "target_item_id": item_id, "target_version_id": version_id,
            "mapping_id": mapping["_id"] if mapping else None,
            "mapping_revision": mapping["_rev"] if mapping else None,
            "item_revision": item["_rev"] if item else None,
            "version_revision": version["_rev"] if version else None,
            "station_item_id": membership["_id"] if membership else None,
            "station_item_revision": membership["_rev"] if membership else None,
            "approved_target": approved, "category_id": category,
            "fields": decisions,
            "source_only_fields": {source: _source_value(track.fields[source])
                                   for source in sorted(SOURCE_ONLY_FIELDS)},
            "media_path": media,
            "cue_review": {"policy": prepared["cue_policy"], "status": cue_status,
                           "source_marks": len(track.marks),
                           "source_tempos": len(track.tempos),
                           "radio_cues_created": 0}}


def _preview_prepared(store: Store, prepared: dict) -> dict:
    library: RekordboxLibrary = prepared["library"]
    plan = prepared["plan"]
    losses = [_jsonable(loss.__dict__) for loss in library.losses]
    losses.append({"kind": "identity_limit", "object_key": "playlists",
                   "source_field": "NODE.path",
                   "detail": "Playlist rows are scoped to the source snapshot and its folder traversal order."})
    entries = []
    counts = {"create": 0, "update": 0, "preserve": 0, "conflict": 0}
    for source_key in plan.track_keys:
        track = library.tracks[source_key.source_track_id]
        decision = _track_decisions(store, prepared, track, losses)
        entries.append(decision)
        counts[decision["action"]] += 1
    # A source may contain two TrackIDs already mapped to one item. Applying
    # both in one transaction would make field ownership ambiguous.
    owners: dict[str, str] = {}
    for entry in entries:
        item_id = entry["target_item_id"]
        if item_id and item_id in owners:
            counts[entry["action"]] -= 1
            entry["action"] = "conflict"
            entry["identity_conflict"] = "multiple_source_tracks_share_one_target_item"
            counts["conflict"] += 1
            losses.append({"kind": "identity_conflict", "object_key": f"track:{entry['source_id']}",
                           "source_field": "TrackID",
                           "detail": f"Also mapped from source TrackID {owners[item_id]}."})
        elif item_id:
            owners[item_id] = entry["source_id"]
    nodes = [{"path": _playlist_path(node), "kind": node.kind,
              "name": node.name,
              "parent_path": "/".join(map(str, node.path[:-1])) if len(node.path) > 1 else None,
              "entries": [{"ordinal": entry.position, "source_key": entry.source_key,
                           "source_track_id": entry.track_id}
                          for entry in node.entries] if node.kind == "playlist" else []}
             for node in plan.selected_nodes]
    scope = {"node_paths": [list(node.path) for node in plan.selected_nodes],
             "selected_track_ids": [key.source_track_id for key in plan.track_keys],
             "partial": plan.partial}
    preview_hash = _hash({"request_fingerprint": prepared["request_fingerprint"],
                          "entries": entries,
                          "definitions": {key: field["_rev"] if field else None
                                          for key, field in prepared["definitions"].items()},
                          "nodes": nodes})
    if store.sequence != prepared["sequence"]:
        raise Conflict("database changed during import preview; retry")
    return {"station_id": prepared["station_id"],
            "source_namespace": prepared["namespace"],
            "source_sha256": library.source_sha256,
            "reader_version": library.reader_version,
            "format_version": library.format_version,
            "request_fingerprint": prepared["request_fingerprint"],
            "preview_hash": preview_hash, "store_sequence": prepared["sequence"],
            "partial": plan.partial, "deletion_inference_allowed": False,
            "scope": scope, "counts": counts, "entries": entries,
            "playlist_nodes": nodes, "losses": losses}


def preview_xml_import(
    store: Store, principal: Principal, station_id: str | UUID,
    library_key: str, xml_bytes: bytes, *,
    selected_node_paths: Sequence[Sequence[int]] | None = None,
    selected_track_ids: Sequence[str | int] | None = None,
    field_policy_overrides: Mapping[str, str] | None = None,
    category_mapping: Mapping[str, str] | None = None,
    path_rules: Sequence[PathMapRule] = (),
    allowed_roots: Sequence[str | Path] = (),
    cue_policy: str = "preserve_as_dj",
) -> dict:
    """Preview every proposed change and loss without writing to the store."""
    prepared = _prepare(
        store, principal, station_id, library_key, xml_bytes,
        selected_node_paths=selected_node_paths,
        selected_track_ids=selected_track_ids,
        field_policy_overrides=field_policy_overrides,
        category_mapping=category_mapping, path_rules=path_rules,
        allowed_roots=allowed_roots, cue_policy=cue_policy,
    )
    return _preview_prepared(store, prepared)


def _stable_id(*parts: object) -> str:
    return str(uuid5(NAMESPACE_URL, "sdbase:rekordbox:" + _canonical(parts)))


def _put_changed(tx: Any, store: Store, collection: str, key: str,
                 body: dict, counters: dict[str, int], counter: str) -> bool:
    current = store.get(collection, key)
    if current and _data(current) == body:
        tx.expect(collection, key, current["_rev"])
        return False
    tx.put(collection, key, body,
           expected_revision=current["_rev"] if current else None)
    counters[counter] += 1
    return True


def _check_preview_reads(tx: Any, prepared: dict, preview: dict) -> None:
    """Record the decisions' direct dependencies as well as the global fence."""
    tx.expect_sequence(preview["store_sequence"])
    for field in prepared["definitions"].values():
        if field:
            tx.expect("field_definitions", field["_id"], field["_rev"])
    for entry in preview["entries"]:
        for collection, key_name, revision_name in (
            ("external_ids", "mapping_id", "mapping_revision"),
            ("items", "target_item_id", "item_revision"),
            ("versions", "target_version_id", "version_revision"),
            ("station_items", "station_item_id", "station_item_revision"),
        ):
            if entry[key_name]:
                tx.expect(collection, entry[key_name], entry[revision_name])
        for field in entry["fields"].values():
            if field.get("value_id"):
                tx.expect("metadata_values", field["value_id"], field["value_revision"])


def _track_writes(tx: Any, store: Store, principal: Principal, prepared: dict,
                  entry: dict, track: Track, counters: dict[str, int],
                  resolved: dict[str, dict]) -> None:
    if entry["identity_conflict"]:
        counters["identity_conflicts"] += 1
        return
    namespace = prepared["namespace"]
    station_id = prepared["station_id"]
    source_id = track.track_id
    item_id = entry["target_item_id"] or _stable_id("item", namespace, source_id)
    version_id = entry["target_version_id"] or _stable_id("version", namespace, source_id)
    now = datetime.now(timezone.utc).isoformat()
    snapshot = _track_snapshot(track)
    if entry["target_item_id"] is None:
        name = track.fields["Name"]
        usable = name.status == "value" and isinstance(name.value, str) and bool(name.value.strip())
        title = name.value.strip() if usable else f"Rekordbox Track {source_id}"
        tx.put("items", item_id, {"kind": "music", "title": title,
                                  "title_source": prepared["source_kind"] if usable else "rekordbox_fallback",
                                  "lifecycle": "draft", "created_at": now,
                                  "created_by": principal.user_id})
        tx.put("versions", version_id, {"item_id": item_id, "version_no": 1,
                                          "label": "Imported Rekordbox source", "status": "draft",
                                          "created_at": now, "created_by": principal.user_id})
        counters["items_created"] += 1
        counters["versions_created"] += 1
    elif entry["fields"]["Name"]["decision"] == "set":
        current = store.get("items", item_id)
        if current is None:
            raise Conflict("mapped item disappeared before import")
        body = _data(current)
        body["title"] = str(track.fields["Name"].value).strip()
        body["title_source"] = prepared["source_kind"]
        body["updated_at"] = now
        body["updated_by"] = principal.user_id
        tx.put("items", item_id, body, expected_revision=current["_rev"])
        counters["items_updated"] += 1
    membership_id = entry["station_item_id"] or _stable_id("station_item", station_id, item_id)
    category_field = entry["fields"].get("category")
    if entry["station_item_id"] is None:
        body = {"station_id": station_id, "item_id": item_id, "eligible": False,
                "created_at": now, "created_by": principal.user_id}
        if category_field and category_field["decision"] == "set":
            body["category_id"] = entry["category_id"]
        tx.put("station_items", membership_id, body)
        counters["station_links_created"] += 1
    elif category_field and category_field["decision"] == "set":
        membership = store.get("station_items", membership_id)
        body = _data(membership)
        body["category_id"] = entry["category_id"]
        tx.put("station_items", membership_id, body,
               expected_revision=membership["_rev"])
        counters["station_links_updated"] += 1
    identity_id = entry["mapping_id"] or _stable_id("external_id", namespace, source_id)
    previous = store.get("external_ids", identity_id)
    identity = _data(previous) if previous else {
        "source_namespace": namespace, "source_id": source_id,
        "target_collection": "items", "target_id": item_id,
        "target_version_id": version_id,
    }
    identity.update({"source_fingerprint": entry["source_fingerprint"],
                     "source_snapshot": snapshot,
                     "source_sha256": prepared["library"].source_sha256})
    _put_changed(tx, store, "external_ids", identity_id, identity, counters,
                 "external_id_rows_written")
    for source in FIELD_TARGETS:
        field_decision = entry["fields"][source]
        decision = field_decision["decision"]
        if decision not in {"set", "suggest"}:
            continue
        field = prepared["definitions"][source]
        if field is None:
            continue
        collection = field_decision["target_collection"]
        target_id = item_id if collection == "items" else version_id
        value = _jsonable(track.fields[source].value)
        source_ref = f"{namespace}:track:{source_id}:{source}"
        if decision == "set":
            value_id = field_decision["value_id"] or _stable_id(
                "metadata_value", field["_id"], collection, target_id, "*")
            body = {"field_id": field["_id"], "target_collection": collection,
                    "target_id": target_id, "station_scope": "*", "value": value,
                    "is_cleared": False, "source_kind": prepared["source_kind"],
                    "source_ref": source_ref, "approval_status": "unreviewed",
                    "updated_by": principal.user_id, "updated_at": now}
            _put_changed(tx, store, "metadata_values", value_id, body,
                         counters, "metadata_values_written")
        else:
            suggestion_id = _stable_id("metadata_suggestion", namespace, source_id,
                                       field["_id"], collection, target_id, value)
            existing = store.get("metadata_suggestions", suggestion_id)
            if existing:
                tx.expect("metadata_suggestions", suggestion_id, existing["_rev"])
                continue
            tx.put("metadata_suggestions", suggestion_id, {
                "field_id": field["_id"], "target_collection": collection,
                "target_id": target_id, "proposed_value": value,
                "source_kind": prepared["source_kind"], "source_ref": source_ref,
                "status": "pending", "created_at": now,
                "created_by": principal.user_id,
            })
            counters["metadata_suggestions_created"] += 1
    if prepared["cue_policy"] != "ignore":
        analysis_id = _stable_id("dj_analysis", namespace, source_id, version_id)
        analysis = _analysis(track, namespace, version_id)
        analysis["source_sha256"] = prepared["library"].source_sha256
        analysis["review_status"] = (
            "awaiting_review" if prepared["cue_policy"] == "propose_radio_cues" and track.marks
            else "source_only")
        _put_changed(tx, store, "dj_analysis", analysis_id, analysis,
                     counters, "dj_analysis_written")
    resolved[source_id] = {"item_id": item_id, "version_id": version_id}


def _playlist_writes(tx: Any, store: Store, prepared: dict, preview: dict,
                     resolved: dict[str, dict], counters: dict[str, int]) -> None:
    namespace = prepared["namespace"]
    station_id = prepared["station_id"]
    source_sha = prepared["library"].source_sha256
    for node in preview["playlist_nodes"]:
        path = node["path"]
        node_id = _stable_id("playlist_node", station_id, namespace, source_sha, path)
        body = {"station_id": station_id, "source_namespace": namespace,
                "source_sha256": source_sha, "path": path,
                "kind": node["kind"], "name": node["name"],
                "parent_path": node["parent_path"]}
        _put_changed(tx, store, "import_playlist_nodes", node_id, body,
                     counters, "playlist_nodes_written")
        for source_entry in node["entries"]:
            ordinal = source_entry["ordinal"]
            track_id = source_entry["source_track_id"]
            matched = resolved.get(track_id) if track_id else None
            entry_id = _stable_id("playlist_entry", node_id, ordinal)
            entry_body = {"node_id": node_id, "ordinal": ordinal,
                          "source_key": source_entry["source_key"],
                          "source_track_id": track_id or "",
                          "resolved": matched is not None}
            if matched:
                entry_body.update(matched)
            _put_changed(tx, store, "import_playlist_entries", entry_id,
                         entry_body, counters, "playlist_entries_written")


def apply_xml_import(
    store: Store, principal: Principal, station_id: str | UUID,
    library_key: str, xml_bytes: bytes, idempotency_key: str, *,
    expected_preview_hash: str | None = None,
    selected_node_paths: Sequence[Sequence[int]] | None = None,
    selected_track_ids: Sequence[str | int] | None = None,
    field_policy_overrides: Mapping[str, str] | None = None,
    category_mapping: Mapping[str, str] | None = None,
    path_rules: Sequence[PathMapRule] = (),
    allowed_roots: Sequence[str | Path] = (),
    cue_policy: str = "preserve_as_dj",
) -> dict:
    """Commit a previewed XML selection once, with source and revision fencing.

    The XML remains the sole read source. Referenced media paths are proposals;
    neither this function nor the parser opens an audio file. The final job and
    every catalog/import row are one durable Store transaction.
    """
    if not isinstance(idempotency_key, str) or not idempotency_key.strip() or len(idempotency_key) > 128:
        raise ImportError("idempotency_key must contain 1..128 characters")
    prepared = _prepare(
        store, principal, station_id, library_key, xml_bytes,
        selected_node_paths=selected_node_paths,
        selected_track_ids=selected_track_ids,
        field_policy_overrides=field_policy_overrides,
        category_mapping=category_mapping, path_rules=path_rules,
        allowed_roots=allowed_roots, cue_policy=cue_policy,
    )
    namespace = prepared["namespace"]
    key = idempotency_key.strip()
    existing_jobs = store.lookup("import_jobs", "source_key", namespace, key)
    if existing_jobs:
        previous = existing_jobs[0]
        if (previous["station_id"] != prepared["station_id"] or
                previous["source_sha256"] != prepared["library"].source_sha256 or
                previous.get("request_fingerprint") != prepared["request_fingerprint"]):
            raise Conflict("idempotency key is already bound to a different import request")
        if previous["status"] != "completed":
            raise Conflict("idempotency key belongs to an unfinished import")
        return dict(previous["summary"], replayed=True)
    preview = _preview_prepared(store, prepared)
    if expected_preview_hash is not None and expected_preview_hash != preview["preview_hash"]:
        raise Conflict("import preview changed; refresh and review it again")
    counters = {name: 0 for name in (
        "items_created", "items_updated", "versions_created",
        "station_links_created", "station_links_updated", "external_id_rows_written",
        "metadata_values_written", "metadata_suggestions_created",
        "dj_analysis_written", "playlist_nodes_written", "playlist_entries_written",
        "identity_conflicts",
    )}
    job_id = _stable_id("import_job", namespace, key)
    tx = store.begin(actor=principal.user_id)
    _check_preview_reads(tx, prepared, preview)
    resolved: dict[str, dict] = {}
    for entry in preview["entries"]:
        track = prepared["library"].tracks[entry["source_id"]]
        _track_writes(tx, store, principal, prepared, entry, track, counters, resolved)
    _playlist_writes(tx, store, prepared, preview, resolved, counters)
    summary = {"job_id": job_id, "station_id": prepared["station_id"],
               "source_namespace": namespace,
               "source_sha256": prepared["library"].source_sha256,
               "request_fingerprint": prepared["request_fingerprint"],
               "preview_hash": preview["preview_hash"],
               "scope": preview["scope"], "counts": counters,
               "track_decisions": preview["counts"],
               "losses": preview["losses"],
               "deletion_inference_allowed": False,
               "media_verified": 0, "radio_cues_created": 0}
    tx.put("import_jobs", job_id, {
        "station_id": prepared["station_id"], "source_namespace": namespace,
        "source_sha256": prepared["library"].source_sha256,
        "idempotency_key": key, "request_fingerprint": prepared["request_fingerprint"],
        "status": "completed", "scope": preview["scope"], "summary": summary,
        "completed_at": datetime.now(timezone.utc).isoformat(),
        "created_by": principal.user_id,
    })
    tx.commit()
    return dict(summary, replayed=False)
