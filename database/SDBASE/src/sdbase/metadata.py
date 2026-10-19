"""Typed radio metadata, source ownership policies and review suggestions."""

from __future__ import annotations

from datetime import date, datetime, timezone
from decimal import Decimal, InvalidOperation
from uuid import UUID, uuid4

from .catalog import require_shared_item_permission
from .identity import Principal, require
from .storage import Conflict, Store, StoreError


DATA_TYPES = {"string", "integer", "decimal", "boolean", "date", "timestamp",
              "array", "object"}
POLICIES = {"take_source", "fill_empty", "preserve", "review_conflict"}


def _field(store: Store, key: str) -> dict:
    rows = store.lookup("field_definitions", "key", key)
    if not rows:
        raise StoreError(f"unknown metadata field: {key}")
    return rows[0]


def _validate_value(field: dict, value: object) -> None:
    kind = field["data_type"]
    valid = {
        "string": isinstance(value, str),
        "integer": isinstance(value, int) and not isinstance(value, bool),
        "decimal": isinstance(value, (int, float, str)) and not isinstance(value, bool),
        "boolean": isinstance(value, bool),
        "date": isinstance(value, str),
        "timestamp": isinstance(value, str),
        "array": isinstance(value, list),
        "object": isinstance(value, dict),
    }.get(kind, False)
    if not valid:
        raise StoreError(f"wrong value type for {field['key']}")
    if kind == "decimal":
        try:
            parsed_decimal = Decimal(str(value))
        except InvalidOperation as exc:
            raise StoreError("decimal value is invalid") from exc
        if not parsed_decimal.is_finite():
            raise StoreError("decimal value must be finite")
    if kind == "date":
        try:
            date.fromisoformat(value)
        except ValueError as exc:
            raise StoreError("date value must use ISO format") from exc
    if kind == "timestamp":
        try:
            parsed_time = datetime.fromisoformat(value.replace("Z", "+00:00"))
        except ValueError as exc:
            raise StoreError("timestamp value must use ISO format") from exc
        if parsed_time.tzinfo is None or parsed_time.utcoffset() is None:
            raise StoreError("timestamp value needs a timezone offset")
    rules = field.get("validation", {})
    if "enum" in rules and value not in rules["enum"]:
        raise StoreError(f"value is outside allowed set for {field['key']}")
    if isinstance(value, (str, list)):
        if "min_length" in rules and len(value) < rules["min_length"]:
            raise StoreError("metadata value is too short")
        if "max_length" in rules and len(value) > rules["max_length"]:
            raise StoreError("metadata value is too long")
    if kind in ("integer", "decimal"):
        numeric = Decimal(str(value))
        if "min" in rules and numeric < Decimal(str(rules["min"])):
            raise StoreError("metadata value is below minimum")
        if "max" in rules and numeric > Decimal(str(rules["max"])):
            raise StoreError("metadata value is above maximum")


def define_field(store: Store, principal: Principal, station_id: str, *,
                 key: str, label: str, scope: str, data_type: str,
                 metadata_class: str, validation: dict | None = None,
                 required_for: list[dict] | None = None,
                 source_policies: dict[str, str] | None = None) -> dict:
    require(store, principal, "manage", str(UUID(station_id)))
    for station in store.scan("stations"):
        require(store, principal, "manage", station["_id"])
    if not key or data_type not in DATA_TYPES or scope not in ("item", "version"):
        raise StoreError("field key, type and item/version scope required")
    if metadata_class not in {"descriptive", "editorial", "technical", "commercial", "operational"}:
        raise StoreError("invalid metadata class")
    policies = source_policies or {}
    if any(action not in POLICIES for action in policies.values()):
        raise StoreError("unsupported source policy")
    return store.put("field_definitions", str(uuid4()), {
        "key": key, "label": label, "scope": scope, "data_type": data_type,
        "metadata_class": metadata_class, "validation": validation or {},
        "required_for": required_for or [], "source_policies": policies,
        "created_by": principal.user_id,
    }, actor=principal.user_id)


def _target(store: Store, field: dict, target_id: str, station_id: str) -> tuple[str, str]:
    collection = "items" if field["scope"] == "item" else "versions"
    target_id = str(UUID(target_id))
    target = store.get(collection, target_id)
    if target is None:
        raise StoreError("metadata target not found")
    item_id = target_id if collection == "items" else target["item_id"]
    if not store.lookup("station_items", "station_item", station_id, item_id):
        raise StoreError("metadata target is outside station")
    return collection, target_id


def value_for(store: Store, field_key: str, target_id: str, *,
              station_id: str | None = None) -> dict | None:
    field = _field(store, field_key)
    collection = "items" if field["scope"] == "item" else "versions"
    if station_id is not None:
        local = store.lookup("metadata_values", "field_target", field["_id"], collection,
                             str(UUID(target_id)), str(UUID(station_id)))
        if local:
            return local[0]
    global_rows = store.lookup("metadata_values", "field_target", field["_id"], collection,
                               str(UUID(target_id)), "*")
    return global_rows[0] if global_rows else None


def set_value(store: Store, principal: Principal, station_id: str, field_key: str,
              target_id: str, value: object | None, *, station_override: bool = False,
              expected_revision: int | None = None, source_kind: str = "manual",
              approval_status: str = "unreviewed", clear: bool = False,
              _permission: str = "edit") -> dict:
    sequence = store.sequence
    station_id = str(UUID(station_id))
    require(store, principal, _permission, station_id)
    if _permission == "edit" and source_kind != "manual":
        raise StoreError("manual metadata edits cannot claim an import source")
    field = _field(store, field_key)
    collection, target_id = _target(store, field, target_id, station_id)
    if not station_override:
        item_id = target_id if collection == "items" else store.get("versions", target_id)["item_id"]
        require_shared_item_permission(store, principal, item_id, _permission)
    if clear and value is not None:
        raise StoreError("clear must not carry a value")
    if not clear:
        _validate_value(field, value)
    if approval_status != "unreviewed":
        raise StoreError("metadata changes must be reviewed by a separate approval action")
    scope = station_id if station_override else "*"
    matches = store.lookup("metadata_values", "field_target", field["_id"],
                           collection, target_id, scope)
    existing = matches[0] if matches else None
    if existing is None and expected_revision is not None:
        raise StoreError("metadata value does not exist at expected revision")
    if existing is not None and expected_revision is None:
        raise StoreError("expected_revision is required to change an existing value")
    now = datetime.now(timezone.utc).isoformat()
    body = {"field_id": field["_id"], "target_collection": collection,
            "target_id": target_id, "station_scope": scope,
            "value": None if clear else value, "is_cleared": clear,
            "source_kind": source_kind, "approval_status": approval_status,
            "updated_by": principal.user_id, "updated_at": now}
    # The global value may affect every station sharing the item. Fence the
    # association and permission reads against a concurrent new share/grant.
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("metadata_values", existing["_id"] if existing else str(uuid4()), body,
           expected_revision=expected_revision)
    return tx.commit()[0]


def review_value(store: Store, principal: Principal, station_id: str,
                 value_id: str, *, decision: str, expected_revision: int,
                 reason: str | None = None) -> dict:
    """Approve or reject one unchanged draft with scoped approve permission."""
    sequence = store.sequence
    station_id, value_id = str(UUID(station_id)), str(UUID(value_id))
    if decision not in {"approved", "rejected"}:
        raise StoreError("review decision must be approved or rejected")
    if isinstance(expected_revision, bool) or not isinstance(expected_revision, int) or expected_revision < 1:
        raise StoreError("expected_revision must be a positive integer")
    if reason is not None and (not isinstance(reason, str) or not reason.strip()):
        raise StoreError("review reason must be nonempty text")
    if decision == "rejected" and reason is None:
        raise StoreError("rejected metadata needs a reason")
    require(store, principal, "approve", station_id)
    current = store.get("metadata_values", value_id)
    if current is None:
        raise StoreError("metadata value not found")
    if current["_rev"] != expected_revision:
        raise Conflict("metadata value changed before review")
    if current.get("approval_status", "unreviewed") != "unreviewed":
        raise Conflict("only an unreviewed value can be reviewed")
    scope = current["station_scope"]
    if scope != "*" and scope != station_id:
        raise StoreError("metadata value belongs to another station")
    field = store.get("field_definitions", current["field_id"])
    if field is None:
        raise StoreError("metadata field is absent")
    collection, target_id = _target(store, field, current["target_id"], station_id)
    if collection != current["target_collection"] or target_id != current["target_id"]:
        raise StoreError("metadata target differs from field scope")
    if scope == "*":
        item_id = target_id if collection == "items" else store.get("versions", target_id)["item_id"]
        require_shared_item_permission(store, principal, item_id, "approve")
    if decision == "approved" and (current.get("is_cleared") or current.get("value") is None):
        raise StoreError("a cleared metadata value cannot be approved")
    now = datetime.now(timezone.utc).isoformat()
    body = {key: value for key, value in current.items() if key not in {"_id", "_rev"}}
    body.update({"approval_status": decision, "reviewed_by": principal.user_id,
                 "reviewed_at": now, "review_reason": reason.strip() if reason else None})
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("metadata_values", value_id, body, expected_revision=expected_revision)
    return tx.commit()[0]


def apply_source(store: Store, principal: Principal, station_id: str, field_key: str,
                 target_id: str, proposed_value: object, *, source_kind: str,
                 source_ref: str | None = None, station_override: bool = False) -> dict:
    """Apply configured source policy without turning absence into a clear."""
    sequence = store.sequence
    station_id = str(UUID(station_id))
    require(store, principal, "import", station_id)
    field = _field(store, field_key)
    collection, target_id = _target(store, field, target_id, station_id)
    if not station_override:
        item_id = target_id if collection == "items" else store.get("versions", target_id)["item_id"]
        require_shared_item_permission(store, principal, item_id, "import")
    _validate_value(field, proposed_value)
    policy = field.get("source_policies", {}).get(source_kind, "review_conflict")
    current = value_for(store, field_key, target_id,
                        station_id=station_id if station_override else None)
    if policy == "preserve" or (policy == "fill_empty" and current and
                                 not current["is_cleared"] and current.get("value") is not None):
        return {"action": "preserved", "field": field_key,
                "current_value_id": current["_id"] if current else None}
    if policy == "review_conflict" and current and current.get("value") != proposed_value:
        tx = store.begin(actor=principal.user_id)
        tx.expect_sequence(sequence)
        suggestion = tx.put("metadata_suggestions", str(uuid4()), {
            "field_id": field["_id"], "target_collection": collection,
            "target_id": target_id, "proposed_value": proposed_value,
            "source_kind": source_kind, "source_ref": source_ref,
            "status": "pending", "created_at": datetime.now(timezone.utc).isoformat(),
        }).commit()[0]
        return {"action": "review_required", "suggestion_id": suggestion["_id"]}
    result = set_value(store, principal, station_id, field_key, target_id,
                       proposed_value, station_override=station_override,
                       expected_revision=current["_rev"] if current else None,
                       source_kind=source_kind, _permission="import")
    return {"action": "updated", "value_id": result["_id"], "revision": result["_rev"]}


def missing_required(store: Store, station_id: str, item_id: str, version_id: str | None,
                     *, stage: str) -> list[str]:
    item = store.get("items", str(UUID(item_id)))
    if item is None:
        raise StoreError("item not found")
    missing = []
    for field in store.scan("field_definitions"):
        requirements = field.get("required_for", [])
        if not any(rule.get("kind") in ("*", item["kind"]) and
                   rule.get("stage") == stage for rule in requirements):
            continue
        target = item_id if field["scope"] == "item" else version_id
        if target is None:
            missing.append(field["key"])
            continue
        current = value_for(store, field["key"], target, station_id=station_id)
        if (current is None or current["is_cleared"] or current.get("value") is None
                or (stage == "activate" and current.get("approval_status") != "approved")):
            missing.append(field["key"])
    return missing
