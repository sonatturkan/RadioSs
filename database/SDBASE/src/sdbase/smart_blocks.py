"""Versioned, deterministic catalog selection for radio smart blocks.

Rules are finite JSON trees. A preview is bound to one database sequence; its
audio choices can be committed to a draft plan only while that sequence still
exists. No SQL engine or external playlist service is involved.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
from decimal import Decimal, InvalidOperation
from hashlib import sha256
import json
import math
from typing import Any, Mapping
from uuid import UUID, uuid4
from zoneinfo import ZoneInfo

from .broadcast_services import check_entry_scope, require_service, service_key as checked_service_key
from .identity import Principal, require
from .planning import validate_entry
from .readiness import readiness_at
from .storage import Conflict, Store, StoreError


_FIELDS = {"kind", "category_id", "rotation_code", "item_id", "version_id",
           "title", "artist", "album", "genre", "bpm", "duration_ms"}
_MAX_NODES = 64
_MAX_DEPTH = 8
_MAX_CANDIDATES = 50_000


def _id(value: Any, name: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{name} must be a UUID") from exc


def _body(row: dict) -> dict:
    return {key: value for key, value in row.items() if key not in {"_id", "_rev"}}


def _text(value: Any, name: str, limit: int = 200) -> str:
    if not isinstance(value, str) or not value.strip() or len(value.strip()) > limit:
        raise StoreError(f"{name} must be 1..{limit} characters")
    return value.strip()


def _integer(value: Any, name: str, minimum: int, maximum: int) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise StoreError(f"{name} must be an integer in {minimum}..{maximum}")
    return value


def _instant(value: Any, name: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00")) if isinstance(value, str) else value
    except ValueError as exc:
        raise StoreError(f"{name} must be an offset-aware ISO timestamp") from exc
    if not isinstance(parsed, datetime) or parsed.tzinfo is None or parsed.utcoffset() is None:
        raise StoreError(f"{name} must be an offset-aware ISO timestamp")
    return parsed.astimezone(timezone.utc)


def _json_copy(value: Any, name: str) -> Any:
    try:
        return json.loads(json.dumps(value, ensure_ascii=False, allow_nan=False))
    except (TypeError, ValueError, OverflowError) as exc:
        raise StoreError(f"{name} must be finite JSON data") from exc


def validate_rule_tree(rule_tree: Any) -> dict:
    """Return a normalized finite tree or raise StoreError.

    Nodes are ``{"all": [node, ...]}``, ``{"any": [node, ...]}``,
    ``{"not": node}``, or a leaf with exactly field/op/value. Equality and
    membership compare catalog scalars, title uses casefolded substring, and
    duration uses an inclusive integer interval.
    """
    root = _json_copy(rule_tree, "rule_tree")
    count = 0

    def walk(node: Any, depth: int) -> None:
        nonlocal count
        count += 1
        if depth > _MAX_DEPTH or count > _MAX_NODES or not isinstance(node, dict):
            raise StoreError("smart block rule tree exceeds depth/node limits or has a non-object node")
        if set(node) in ({"all"}, {"any"}):
            children = next(iter(node.values()))
            if not isinstance(children, list) or not 1 <= len(children) <= 32:
                raise StoreError("all/any rule needs 1..32 child rules")
            for child in children:
                walk(child, depth + 1)
            return
        if set(node) == {"not"}:
            walk(node["not"], depth + 1)
            return
        if set(node) != {"field", "op", "value"}:
            raise StoreError("smart block leaf needs exactly field, op and value")
        field, op, value = node["field"], node["op"], node["value"]
        if not isinstance(field, str) or field not in _FIELDS:
            raise StoreError("unsupported smart block rule field")
        if field == "duration_ms":
            if op != "between" or not isinstance(value, list) or len(value) != 2 or \
                    any(type(v) is not int or not 1 <= v <= 86_400_000 for v in value) or \
                    value[0] > value[1]:
                raise StoreError("duration_ms rule needs between and two ascending positive bounds")
        elif field == "bpm":
            if op != "between" or not isinstance(value, list) or len(value) != 2 or \
                    any(isinstance(v, bool) or not isinstance(v, (int, float)) or
                        not math.isfinite(v) or not 1 <= v <= 400 for v in value) or \
                    value[0] > value[1]:
                raise StoreError("bpm rule needs between and two ascending numeric bounds")
        elif op == "contains" and field in {"title", "artist", "album", "genre"}:
            if not isinstance(value, str) or not value.strip() or len(value) > 200:
                raise StoreError("contains rule needs a nonempty text value")
        elif op == "eq":
            if not isinstance(value, str) or not value.strip() or len(value) > 200:
                raise StoreError("eq rule needs a nonempty scalar value")
            if field in {"item_id", "version_id", "category_id"}:
                node["value"] = _id(value, field)
        elif op == "in":
            if not isinstance(value, list) or not 1 <= len(value) <= 100 or \
                    any(not isinstance(v, str) or not v.strip() or len(v) > 200 for v in value) or \
                    len(set(value)) != len(value):
                raise StoreError("in rule needs 1..100 distinct nonempty scalar values")
            if field in {"item_id", "version_id", "category_id"}:
                normalized = [_id(v, field) for v in value]
                if len(set(normalized)) != len(normalized):
                    raise StoreError("in rule UUID values must be distinct after normalization")
                node["value"] = normalized
        else:
            raise StoreError("unsupported smart block rule operator")

    walk(root, 1)
    return root


def _matches(rule: dict, candidate: dict) -> bool:
    if "all" in rule:
        return all(_matches(child, candidate) for child in rule["all"])
    if "any" in rule:
        return any(_matches(child, candidate) for child in rule["any"])
    if "not" in rule:
        return not _matches(rule["not"], candidate)
    value = candidate.get(rule["field"])
    if rule["op"] == "between":
        return isinstance(value, (int, float)) and not isinstance(value, bool) and \
            rule["value"][0] <= value <= rule["value"][1]
    if rule["op"] == "contains":
        values = value if isinstance(value, list) else [value]
        return any(isinstance(v, str) and rule["value"].casefold() in v.casefold()
                   for v in values)
    if rule["op"] == "in":
        return any(v in rule["value"] for v in value) if isinstance(value, list) else value in rule["value"]
    return rule["value"] in value if isinstance(value, list) else value == rule["value"]


def _rule_fields(rule: dict) -> set[str]:
    if "all" in rule or "any" in rule:
        return set().union(*(_rule_fields(child) for child in rule.get("all", rule.get("any", []))))
    if "not" in rule:
        return _rule_fields(rule["not"])
    return {rule["field"]}


def _approved_metadata(store: Store, key: str, item_id: str,
                       version_id: str, station_id: str) -> Any:
    from .metadata import value_for
    definitions = store.lookup("field_definitions", "key", key)
    if not definitions:
        return None
    target_id = item_id if definitions[0]["scope"] == "item" else version_id
    try:
        row = value_for(store, key, target_id, station_id=station_id)
    except StoreError:
        return None
    return row.get("value") if row and row.get("approval_status") == "approved" and \
        not row.get("is_cleared") else None


def _bpm(store: Store, item_id: str, version_id: str,
         station_id: str) -> float | None:
    for key in ("dj.average_bpm", "dj.beatgrid.bpm"):
        value = _approved_metadata(store, key, item_id, version_id, station_id)
        try:
            number = Decimal(str(value))
        except InvalidOperation:
            continue
        if number.is_finite() and 1 <= number <= 400:
            return float(number)
    return None


def _block(store: Store, station_id: str, block_id: str) -> dict:
    row = store.get("smart_blocks", _id(block_id, "block_id"))
    if row is None or row["station_id"] != station_id:
        raise StoreError("smart block is outside station")
    return row


def _version(store: Store, station_id: str, version_id: str) -> tuple[dict, dict]:
    row = store.get("smart_block_versions", _id(version_id, "version_id"))
    if row is None:
        raise StoreError("smart block version does not exist")
    return row, _block(store, station_id, row["block_id"])


def _configuration(rule_tree: Any, target_duration_ms: int,
                   max_overshoot_ms: int, max_items: int,
                   selection_order: str, selection_mode: str) -> dict:
    if not isinstance(selection_order, str) or selection_order not in {"ordered", "shuffle"}:
        raise StoreError("selection_order must be ordered or shuffle")
    if not isinstance(selection_mode, str) or selection_mode not in {"dynamic", "static"}:
        raise StoreError("selection_mode must be dynamic or static")
    target = _integer(target_duration_ms, "target_duration_ms", 1, 86_400_000)
    overshoot = _integer(max_overshoot_ms, "max_overshoot_ms", 0, 86_400_000)
    if target + overshoot > 86_400_000:
        raise StoreError("target plus overshoot exceeds one day")
    return {"rule_tree": validate_rule_tree(rule_tree),
            "target_duration_ms": target, "max_overshoot_ms": overshoot,
            "max_items": _integer(max_items, "max_items", 1, 200),
            "selection_order": selection_order, "selection_mode": selection_mode}


def create_smart_block(store: Store, principal: Principal, station_id: str, *,
                       code: str, name: str, rule_tree: dict,
                       target_duration_ms: int, max_overshoot_ms: int = 300_000,
                       max_items: int = 20, selection_order: str = "ordered",
                       selection_mode: str = "dynamic") -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "edit", station_id)
    if store.get("stations", station_id) is None:
        raise StoreError("station does not exist")
    config = _configuration(rule_tree, target_duration_ms, max_overshoot_ms,
                            max_items, selection_order, selection_mode)
    block_id, version_id = str(uuid4()), str(uuid4())
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("smart_blocks", block_id, {"station_id": station_id,
            "code": _text(code, "code", 100), "name": _text(name, "name"),
            "status": "active", "current_version_id": None,
            "draft_version_id": version_id, "created_by": principal.user_id,
            "created_at": now})
    tx.put("smart_block_versions", version_id, {"block_id": block_id,
            "version_no": 1, "status": "draft", **config,
            "created_by": principal.user_id, "created_at": now})
    tx.commit()
    return {"block_id": block_id, "draft_version_id": version_id, "version_no": 1}


def list_smart_blocks(store: Store, principal: Principal, station_id: str, *,
                      include_disabled: bool = False) -> list[dict]:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "view", station_id)
    if type(include_disabled) is not bool:
        raise StoreError("include_disabled must be boolean")
    rows = [row for row in store.lookup("smart_blocks", "station", station_id)
            if include_disabled or row["status"] == "active"]
    rows.sort(key=lambda row: (row["code"].casefold(), row["_id"]))
    if store.sequence != sequence:
        raise Conflict("smart blocks changed during listing")
    return rows


def list_smart_block_versions(store: Store, principal: Principal, station_id: str,
                              block_id: str) -> list[dict]:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "view", station_id)
    block = _block(store, station_id, block_id)
    versions = store.lookup("smart_block_versions", "block", block["_id"])
    versions.sort(key=lambda row: (row["version_no"], row["_id"]))
    if store.sequence != sequence:
        raise Conflict("smart block changed during version listing")
    return versions


def set_smart_block_status(store: Store, principal: Principal, station_id: str,
                           block_id: str, status: str, *,
                           expected_revision: int) -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "manage", station_id)
    block = _block(store, station_id, block_id)
    if not isinstance(status, str) or status not in {"active", "disabled"}:
        raise StoreError("smart block status must be active or disabled")
    if block["_rev"] != expected_revision:
        raise Conflict("smart block changed")
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("smart_blocks", block["_id"], _body(block) | {
        "status": status, "status_changed_by": principal.user_id,
        "status_changed_at": datetime.now(timezone.utc).isoformat()},
        expected_revision=block["_rev"])
    return tx.commit()[0]


def put_smart_block_draft(store: Store, principal: Principal, station_id: str,
                          version_id: str, *, expected_version_revision: int,
                          rule_tree: dict, target_duration_ms: int,
                          max_overshoot_ms: int = 300_000, max_items: int = 20,
                          selection_order: str = "ordered",
                          selection_mode: str = "dynamic") -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "edit", station_id)
    version, block = _version(store, station_id, version_id)
    if version["status"] != "draft" or block.get("draft_version_id") != version["_id"] or \
            version["_rev"] != expected_version_revision:
        raise Conflict("smart block draft changed")
    config = _configuration(rule_tree, target_duration_ms, max_overshoot_ms,
                            max_items, selection_order, selection_mode)
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("smart_block_versions", version["_id"],
           _body(version) | config | {"updated_by": principal.user_id,
                                     "updated_at": datetime.now(timezone.utc).isoformat()},
           expected_revision=version["_rev"])
    return tx.commit()[0]


def publish_smart_block_version(store: Store, principal: Principal, station_id: str,
                                 version_id: str, *, expected_version_revision: int,
                                 expected_block_revision: int) -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "approve", station_id)
    version, block = _version(store, station_id, version_id)
    if version["status"] != "draft" or block.get("draft_version_id") != version["_id"] or \
            version["_rev"] != expected_version_revision or block["_rev"] != expected_block_revision:
        raise Conflict("smart block draft or parent changed")
    config = _configuration(version["rule_tree"], version["target_duration_ms"],
                            version["max_overshoot_ms"], version["max_items"],
                            version["selection_order"], version.get("selection_mode", "dynamic"))
    static_ids = None
    if config["selection_mode"] == "static":
        candidates = _candidates(store, station_id, config["rule_tree"])
        _sort_candidates(candidates, version["selection_order"], version["_id"])
        selected, total = _fit_candidates(candidates, config["target_duration_ms"],
                                          config["max_overshoot_ms"], config["max_items"])
        if total < config["target_duration_ms"]:
            raise Conflict("static smart block cannot reach its target duration")
        static_ids = [row["version_id"] for row in selected]
        config["static_version_ids"] = static_ids
    digest = sha256(json.dumps(config, sort_keys=True, separators=(",", ":"),
                               ensure_ascii=False).encode()).hexdigest()
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("smart_block_versions", version["_id"],
           _body(version) | {"status": "published", "fingerprint": digest,
                             **({"static_version_ids": static_ids} if static_ids is not None else {}),
                             "published_by": principal.user_id, "published_at": now},
           expected_revision=version["_rev"])
    tx.put("smart_blocks", block["_id"], _body(block) |
           {"current_version_id": version["_id"], "draft_version_id": None},
           expected_revision=block["_rev"])
    tx.commit()
    return {"block_id": block["_id"], "published_version_id": version["_id"],
            "fingerprint": digest}


def fork_smart_block_draft(store: Store, principal: Principal, station_id: str,
                            block_id: str, *, expected_block_revision: int) -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "edit", station_id)
    block = _block(store, station_id, block_id)
    if block["_rev"] != expected_block_revision or block.get("draft_version_id"):
        raise Conflict("smart block changed or already has a draft")
    current = store.get("smart_block_versions", block.get("current_version_id")) if block.get("current_version_id") else None
    if current is None or current["status"] != "published":
        raise Conflict("smart block has no published version")
    new_id = str(uuid4())
    body = {key: current[key] for key in ("rule_tree", "target_duration_ms",
                                          "max_overshoot_ms", "max_items",
                                          "selection_order", "selection_mode")}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("smart_block_versions", new_id, {"block_id": block["_id"],
            "version_no": current["version_no"] + 1, "status": "draft",
            "based_on_id": current["_id"], **body,
            "created_by": principal.user_id,
            "created_at": datetime.now(timezone.utc).isoformat()})
    tx.put("smart_blocks", block["_id"], _body(block) | {"draft_version_id": new_id},
           expected_revision=block["_rev"])
    tx.commit()
    return {"block_id": block["_id"], "draft_version_id": new_id,
            "version_no": current["version_no"] + 1}


def _candidates(store: Store, station_id: str, rule: dict | None,
                version_ids: list[str] | None = None) -> list[dict]:
    versions = ([row for identifier in version_ids
                 if (row := store.get("versions", identifier)) is not None]
                if version_ids is not None else store.scan("versions"))
    if len(versions) > _MAX_CANDIDATES:
        raise Conflict("catalog exceeds smart block preview candidate limit")
    fields = _rule_fields(rule) if rule is not None else set()
    wanted = set(version_ids) if version_ids is not None else None
    result = []
    for version in versions:
        if version["status"] != "approved" or wanted is not None and version["_id"] not in wanted:
            continue
        item = store.get("items", version["item_id"])
        if item is None or item["lifecycle"] != "active":
            continue
        links = store.lookup("station_items", "station_item", station_id, item["_id"])
        association = links[0] if links else None
        if association is None or association.get("eligible") is not True:
            continue
        playable_media = next((media for media in
            store.lookup("media_files", "version", version["_id"])
            if media.get("status") == "verified" and media.get("sha256") and
            isinstance(media.get("technical"), dict) and
            type(media["technical"].get("duration_ms")) is int and
            0 < media["technical"]["duration_ms"] <= 86_400_000 and
            any(location["status"] in {"present", "verified"} for location in
                store.lookup("media_locations", "file", media["_id"]))), None)
        if playable_media is None:
            continue
        candidate = {"version_id": version["_id"], "item_id": item["_id"],
                     "title": item["title"], "kind": item["kind"],
                     "category_id": association.get("category_id"),
                     "rotation_code": association.get("rotation_code"),
                     "duration_ms": playable_media["technical"]["duration_ms"],
                     "media_file_id": playable_media["_id"],
                     "version_no": version["version_no"]}
        if "artist" in fields:
            candidate["artist"] = sorted({person["display_name"] for link in
                store.lookup("contributions", "item", item["_id"])
                if link["role"] in {"primary_artist", "joint_artist", "guest_artist"}
                for person in [store.get("people", link["person_id"])]
                if person is not None})
        if "album" in fields:
            candidate["album"] = _approved_metadata(store, "record.album", item["_id"],
                                                      version["_id"], station_id)
        if "genre" in fields:
            candidate["genre"] = _approved_metadata(store, "record.genre", item["_id"],
                                                      version["_id"], station_id)
        if "bpm" in fields:
            candidate["bpm"] = _bpm(store, item["_id"], version["_id"], station_id)
        if rule is None or _matches(rule, candidate):
            result.append(candidate)
    return result


def _sort_candidates(candidates: list[dict], order: str, seed: str) -> None:
    if order == "shuffle":
        candidates.sort(key=lambda row: (sha256((seed + row["version_id"]).encode()).hexdigest(),
                                         row["version_id"]))
    else:
        candidates.sort(key=lambda row: (row["title"].casefold(), row["version_no"],
                                         row["version_id"]))


def _fit_candidates(candidates: list[dict], target: int,
                    overshoot: int, max_items: int) -> tuple[list[dict], int]:
    selected, elapsed = [], 0
    for row in candidates:
        if elapsed >= target or len(selected) >= max_items:
            break
        if elapsed + row["duration_ms"] <= target + overshoot:
            selected.append(row)
            elapsed += row["duration_ms"]
    return selected, elapsed


def _select_entries(store: Store, station_id: str, version: dict,
                    start: datetime, *, station_timezone: str,
                    territory: str, output_id: str, ordinal_base: int,
                    binding: dict | None) -> dict:
    rule = validate_rule_tree(version["rule_tree"])
    mode = version.get("selection_mode", "dynamic")
    unavailable = []
    if mode == "static":
        fixed_ids = version.get("static_version_ids")
        if not isinstance(fixed_ids, list) or not fixed_ids:
            raise Conflict("published static smart block lacks frozen selections")
        by_id = {row["version_id"]: row for row in _candidates(store, station_id, None, fixed_ids)}
        candidates = [by_id[identifier] for identifier in fixed_ids if identifier in by_id]
        unavailable = [{"version_id": identifier,
                        "reasons": ["frozen_version_unavailable"]}
                       for identifier in fixed_ids if identifier not in by_id]
    else:
        candidates = _candidates(store, station_id, rule)
        _sort_candidates(candidates, version["selection_order"],
                         f"{version['_id']}:{start.isoformat()}")
    target = version["target_duration_ms"]
    ceiling = target + version["max_overshoot_ms"]
    elapsed, entries, rejected = 0, [], list(unavailable)
    day = start.astimezone(ZoneInfo(station_timezone)).date()
    for candidate in candidates:
        if elapsed >= target or len(entries) >= version["max_items"]:
            break
        duration = candidate["duration_ms"]
        if elapsed + duration > ceiling:
            continue
        at = start + timedelta(milliseconds=elapsed)
        ready = readiness_at(store, station_id, candidate["version_id"],
                             slot_start=at, slot_end=at + timedelta(milliseconds=duration),
                             require_endpoint_copy=False,
                             territory=territory, output_id=output_id)
        if not ready["ready"]:
            rejected.append({"version_id": candidate["version_id"],
                             "reasons": ready["blocked_reasons"]})
            continue
        if ready["media_file_id"] != candidate["media_file_id"]:
            rejected.append({"version_id": candidate["version_id"],
                             "reasons": ["selected_media_duration_changed"]})
            continue
        payload = {"version_id": candidate["version_id"]}
        if territory != "*":
            payload["territory"] = territory
        if output_id != "*":
            payload["output_id"] = output_id
        entry = {"event_kind": "audio", "state": "planned",
                 "ordinal": ordinal_base + len(entries),
                 "scheduled_at": at.isoformat(), "payload": payload,
                 "duration_kind": "fixed", "planned_duration_ms": duration,
                 "smart_block_version_id": version["_id"]}
        validated = validate_entry(entry, station_timezone, day)
        if not validated["valid"]:
            rejected.append({"version_id": candidate["version_id"],
                             "reasons": [issue["code"] for issue in validated["errors"]]})
            continue
        body = {key: value for key, value in validated["normalized"].items()
                if key not in {"id", "nominal_end_at", "requires_resolution"}}
        check_entry_scope(body, binding)
        entries.append({"entry": body, "version_id": candidate["version_id"],
                        "duration_ms": duration})
        elapsed += duration
    complete = elapsed >= target and (mode != "static" or
                                      len(entries) == len(fixed_ids) and not rejected)
    return {"status": "ready" if complete else "needs_resolution",
            "entries": entries, "selected_duration_ms": elapsed,
            "target_duration_ms": target, "max_overshoot_ms": version["max_overshoot_ms"],
            "rejected": rejected[:50], "candidate_count": len(candidates),
            "next_at": (start + timedelta(milliseconds=elapsed)).isoformat()}


def preview_smart_block(store: Store, principal: Principal, station_id: str,
                        version_id: str, start_at: str, *,
                        service_key: str = "default", territory: str = "*",
                        output_id: str = "*", ordinal_base: int = 0) -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "view", station_id)
    version, block = _version(store, station_id, version_id)
    if version["status"] != "published" or block["status"] != "active":
        raise Conflict("smart block must be active and published")
    binding = require_service(store, station_id, checked_service_key(service_key))
    if binding is not None:
        if output_id not in {"*", binding["output_id"]}:
            raise Conflict("smart block output differs from broadcast service")
        output_id = binding["output_id"]
    if any(not isinstance(v, str) or not v.strip() for v in (territory, output_id)):
        raise StoreError("territory and output_id must be nonempty strings")
    base = _integer(ordinal_base, "ordinal_base", 0, 2_000_000_000)
    start = _instant(start_at, "start_at")
    station = store.get("stations", station_id)
    if station is None:
        raise StoreError("station does not exist")
    result = _select_entries(store, station_id, version, start,
                             station_timezone=station["timezone"],
                             territory=territory, output_id=output_id,
                             ordinal_base=base, binding=binding)
    if store.sequence != sequence:
        raise Conflict("smart block changed during preview")
    digest = sha256(json.dumps({"version_id": version["_id"],
        "start_at": start.isoformat(), "territory": territory,
        "output_id": output_id, "entries": result["entries"]},
        sort_keys=True, ensure_ascii=False, separators=(",", ":")).encode()).hexdigest()
    return result | {"block_id": block["_id"], "block_version_id": version["_id"],
                     "station_id": station_id, "service_key": service_key,
                     "start_at": start.isoformat(), "sequence": sequence,
                     "fingerprint": digest}


def materialize_smart_block(store: Store, principal: Principal, station_id: str,
                            version_id: str, start_at: str, *,
                            expected_sequence: int, service_key: str = "default",
                            territory: str = "*", output_id: str = "*",
                            ordinal_base: int = 0) -> dict:
    """Atomically append a resolved block to the matching station-day draft."""
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    preview = preview_smart_block(store, principal, station_id, version_id, start_at,
                                  service_key=service_key, territory=territory,
                                  output_id=output_id, ordinal_base=ordinal_base)
    if preview["status"] != "ready":
        raise Conflict("smart block has not reached its target duration")
    from .playlists import _materialize_entries
    return _materialize_entries(store, principal, station_id,
                                preview["entries"], preview["start_at"],
                                service_key=service_key,
                                expected_sequence=expected_sequence,
                                source={"smart_block_version_id": preview["block_version_id"],
                                        "smart_block_fingerprint": preview["fingerprint"]})
