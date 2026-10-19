"""Persisted cart/cut selection for draft radio plans.

The selected version is frozen in a draft entry. Cart configuration, rotation
state, the entry, and the plan token use one Store compare-and-swap transaction.
"""

from __future__ import annotations

from datetime import datetime, time, timedelta, timezone
from typing import Any
from uuid import UUID, uuid4

from .identity import Principal, require
from .planning import select_cart_cut, validate_entry
from .readiness import readiness_at
from .storage import Conflict, Store, StoreError


def _id(value: Any, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _instant(value: Any, field: str) -> datetime:
    try:
        parsed = value if isinstance(value, datetime) else datetime.fromisoformat(
            str(value).replace("Z", "+00:00"))
    except ValueError as exc:
        raise StoreError(f"{field} must be an ISO instant") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise StoreError(f"{field} needs a timezone offset")
    return parsed.astimezone(timezone.utc)


def _data(row: dict) -> dict:
    return {key: value for key, value in row.items() if key not in ("_id", "_rev")}


def _integer(value: Any, field: str, *, minimum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise StoreError(f"{field} must be an integer >= {minimum}")
    return value


def _cart(store: Store, station_id: str, cart_id: str) -> dict:
    row = store.get("carts", cart_id)
    if row is None or row.get("station_id") != station_id:
        raise StoreError("cart does not belong to station")
    return row


def _clock(value: Any, field: str) -> str | None:
    if value is None:
        return None
    try:
        parsed = value if isinstance(value, time) else time.fromisoformat(str(value))
    except ValueError as exc:
        raise StoreError(f"{field} must be an ISO local time") from exc
    if parsed.tzinfo is not None:
        raise StoreError(f"{field} must not have a timezone offset")
    return parsed.isoformat()


def create_cart(store: Store, principal: Principal, station_id: str, *,
                code: str, title: str, selection_mode: str = "sequential") -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "manage", station_id)
    if store.get("stations", station_id) is None:
        raise StoreError("station not found")
    if not isinstance(code, str) or not code.strip() or not isinstance(title, str) or not title.strip():
        raise StoreError("cart code and title must be nonempty")
    if not isinstance(selection_mode, str) or selection_mode not in {"sequential", "weighted"}:
        raise StoreError("invalid cart selection mode")
    cart_id = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.put("carts", cart_id, {"station_id": station_id, "code": code.strip(),
                              "title": title.strip(), "selection_mode": selection_mode,
                              "status": "active"})
    tx.put("cart_states", cart_id, {"cart_id": cart_id, "last_cut_id": None})
    tx.commit()
    return store.get("carts", cart_id)


def set_cart_status(store: Store, principal: Principal, station_id: str,
                    cart_id: str, status: str, *, expected_cart_revision: int) -> dict:
    station_id, cart_id = _id(station_id, "station_id"), _id(cart_id, "cart_id")
    require(store, principal, "manage", station_id)
    cart = _cart(store, station_id, cart_id)
    if not isinstance(status, str) or status not in {"active", "disabled"}:
        raise StoreError("invalid cart status")
    if cart["_rev"] != expected_cart_revision:
        raise Conflict("cart changed")
    return store.put("carts", cart_id, _data(cart) | {"status": status},
                     expected_revision=cart["_rev"], actor=principal.user_id)


def put_cut(store: Store, principal: Principal, station_id: str, cart_id: str, *,
            version_id: str, ordinal: int, expected_cart_revision: int,
            cut_id: str | None = None, expected_cut_revision: int | None = None,
            weight: int = 1, is_fallback: bool = False, status: str = "active",
            valid_from: str | datetime | None = None,
            valid_to: str | datetime | None = None,
            weekdays: list[int] | None = None,
            local_start: str | time | None = None,
            local_end: str | time | None = None) -> dict:
    """Create or replace a cut; bump the cart revision to fence selectors."""
    station_id, cart_id, version_id = (_id(station_id, "station_id"),
                                       _id(cart_id, "cart_id"),
                                       _id(version_id, "version_id"))
    require(store, principal, "manage", station_id)
    cart = _cart(store, station_id, cart_id)
    if cart["_rev"] != expected_cart_revision:
        raise Conflict("cart changed")
    if store.get("versions", version_id) is None:
        raise StoreError("cut version not found")
    _integer(ordinal, "ordinal", minimum=0)
    _integer(weight, "weight", minimum=1)
    if not isinstance(is_fallback, bool) or not isinstance(status, str) or status not in {"active", "disabled"}:
        raise StoreError("invalid fallback or cut status")
    lower = _instant(valid_from, "valid_from").isoformat() if valid_from is not None else None
    upper = _instant(valid_to, "valid_to").isoformat() if valid_to is not None else None
    if lower is not None and upper is not None and upper <= lower:
        raise StoreError("cut validity interval is empty")
    weekdays = list(range(1, 8)) if weekdays is None else weekdays
    if not isinstance(weekdays, list) or not weekdays or \
            any(isinstance(day, bool) or not isinstance(day, int) or day not in range(1, 8)
                for day in weekdays) or len(set(weekdays)) != len(weekdays):
        raise StoreError("weekdays must contain unique ISO weekdays 1..7")
    first, last = _clock(local_start, "local_start"), _clock(local_end, "local_end")
    if (first is None) != (last is None) or (first is not None and first == last):
        raise StoreError("local time window needs distinct start and end")
    identifier = _id(cut_id, "cut_id") if cut_id else str(uuid4())
    existing = store.get("cart_cuts", identifier)
    if existing is not None and (existing["cart_id"] != cart_id or
                                 expected_cut_revision != existing["_rev"]):
        raise Conflict("cut changed or belongs to another cart")
    if existing is not None and existing["version_id"] != version_id:
        raise Conflict("a cut version is immutable; create another cut")
    if existing is None and expected_cut_revision is not None:
        raise Conflict("cut is absent at expected revision")
    row = {"cart_id": cart_id, "version_id": version_id, "ordinal": ordinal,
           "weight": weight, "is_fallback": is_fallback, "status": status,
           "valid_from": lower, "valid_to": upper, "weekdays": weekdays,
           "local_start": first, "local_end": last}
    tx = store.begin(actor=principal.user_id)
    tx.put("cart_cuts", identifier, row,
           expected_revision=existing["_rev"] if existing else None)
    tx.put("carts", cart_id, _data(cart), expected_revision=cart["_rev"])
    tx.commit()
    return store.get("cart_cuts", identifier)


def list_cuts(store: Store, principal: Principal, station_id: str,
              cart_id: str) -> list[dict]:
    station_id, cart_id = _id(station_id, "station_id"), _id(cart_id, "cart_id")
    require(store, principal, "view", station_id)
    _cart(store, station_id, cart_id)
    return sorted(store.lookup("cart_cuts", "cart", cart_id),
                  key=lambda row: (row["ordinal"], row["_id"]))


def _selection(store: Store, station_id: str, cart: dict,
               start: datetime, end: datetime, *, seed: str) -> dict:
    if cart["status"] != "active":
        raise Conflict("cart is disabled")
    station = store.get("stations", station_id)
    state = store.get("cart_states", cart["_id"])
    if state is None:
        raise StoreError("cart selection state is missing")
    cuts = store.lookup("cart_cuts", "cart", cart["_id"])
    prepared: list[dict] = []
    readiness_by_cut: dict[str, dict] = {}
    for cut in cuts:
        version = store.get("versions", cut["version_id"])
        item_id = version["item_id"] if version else None
        station_item = (store.lookup("station_items", "station_item", station_id, item_id)
                        if item_id else [])
        readiness = (readiness_at(store, station_id, cut["version_id"],
                                  slot_start=start, slot_end=end,
                                  require_endpoint_copy=False)
                     if version else {"ready": False,
                                      "blocked_reasons": ["version_missing"],
                                      "media_file_id": None})
        candidate = _data(cut) | {
            "id": cut["_id"],
            "approved": version is not None and version.get("status") == "approved",
            "station_eligible": bool(station_item and station_item[0].get("eligible")),
            "central_media_verified": readiness.get("media_file_id") is not None,
        }
        if not readiness["ready"]:
            candidate["status"] = "disabled"
        prepared.append(candidate)
        readiness_by_cut[cut["_id"]] = readiness
    result = select_cart_cut(prepared, at=start, slot_end_at=end,
                             station_timezone=station["timezone"],
                             mode=cart["selection_mode"],
                             last_cut_id=state.get("last_cut_id"), seed=seed)
    for decision in result["decisions"]:
        if decision["cut_id"] in readiness_by_cut:
            decision["readiness_reasons"] = readiness_by_cut[decision["cut_id"]]["blocked_reasons"]
    result["cart_id"] = cart["_id"]
    result["cart_revision"] = cart["_rev"]
    result["state_revision"] = state["_rev"]
    result["selected_version_id"] = (result["selected"]["version_id"]
                                     if result["selected"] else None)
    return result


def preview_cart_cut(store: Store, principal: Principal, station_id: str,
                     cart_id: str, scheduled_at: str | datetime, *,
                     planned_duration_ms: int | None = None,
                     seed: str = "preview") -> dict:
    station_id, cart_id = _id(station_id, "station_id"), _id(cart_id, "cart_id")
    require(store, principal, "view", station_id)
    cart = _cart(store, station_id, cart_id)
    start = _instant(scheduled_at, "scheduled_at")
    duration = (_integer(planned_duration_ms, "planned_duration_ms", minimum=1)
                if planned_duration_ms is not None else 1)
    end = start + timedelta(milliseconds=duration)
    return _selection(store, station_id, cart, start, end, seed=seed)


def resolve_draft_audio_entry(store: Store, principal: Principal,
                              station_id: str, plan_id: str, entry_id: str, *,
                              expected_plan_revision: int,
                              expected_entry_revision: int,
                              expected_cart_revision: int | None = None) -> dict:
    """Freeze an eligible cut/version and rotate cart state in one transaction."""
    station_id, plan_id, entry_id = (_id(station_id, "station_id"),
                                     _id(plan_id, "plan_id"),
                                     _id(entry_id, "entry_id"))
    require(store, principal, "edit", station_id)
    plan = store.get("plans", plan_id)
    entry = store.get("plan_entries", entry_id)
    if plan is None or plan.get("station_id") != station_id or plan["_rev"] != expected_plan_revision:
        raise Conflict("plan changed or belongs to another station")
    if entry is None or entry["_rev"] != expected_entry_revision or entry.get("event_kind") != "audio":
        raise Conflict("draft audio entry changed or is absent")
    revision_id = entry["plan_revision_id"]
    revision = store.get("plan_revisions", revision_id)
    if revision is None or revision.get("status") != "draft" or revision.get("plan_id") != plan_id or \
            plan.get("draft_revision_id") != revision_id:
        raise Conflict("entry is not in the current draft revision")
    payload_before = entry.get("payload") or {}
    if not isinstance(payload_before, dict):
        raise StoreError("audio entry payload must be an object")
    if payload_before.get("version_id") or payload_before.get("cut_id"):
        raise Conflict("entry has already frozen a cut/version")
    cart_id = _id(payload_before.get("cart_id"), "payload.cart_id")
    cart = _cart(store, station_id, cart_id)
    if expected_cart_revision is not None and cart["_rev"] != expected_cart_revision:
        raise Conflict("cart changed")
    start = _instant(entry["scheduled_at"], "scheduled_at")
    duration = entry.get("planned_duration_ms")
    end = start + timedelta(milliseconds=duration if isinstance(duration, int) and
                            not isinstance(duration, bool) and duration > 0 else 1)
    snapshot_sequence = store.sequence
    result = _selection(store, station_id, cart, start, end,
                        seed=f"{plan_id}:{revision_id}:{entry_id}:{entry['scheduled_at']}")
    if result["selected"] is None:
        raise Conflict("cart has no distributable cut: " +
                       ", ".join(error["code"] for error in result["errors"]))
    cut = store.get("cart_cuts", result["selected"]["id"])
    version = store.get("versions", cut["version_id"])
    state = store.get("cart_states", cart_id)
    payload = dict(payload_before) | {
        "cart_id": cart_id, "cut_id": cut["_id"],
        "version_id": version["_id"], "item_id": version["item_id"]}
    body = _data(entry) | {"payload": payload}
    station = store.get("stations", station_id)
    validation = validate_entry(body | {"id": entry_id}, station["timezone"], plan["service_date"])
    if not validation["valid"] or validation["requires_resolution"]:
        raise StoreError("resolved cart entry violates plan entry rules")
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(snapshot_sequence)
    tx.put("plan_entries", entry_id, body, expected_revision=entry["_rev"])
    tx.put("cart_states", cart_id, _data(state) | {"last_cut_id": cut["_id"]},
           expected_revision=state["_rev"])
    tx.put("carts", cart_id, _data(cart), expected_revision=cart["_rev"])
    tx.put("plans", plan_id, _data(plan), expected_revision=plan["_rev"])
    tx.commit()
    return {"plan_entry_id": entry_id, "cut_id": cut["_id"],
            "version_id": version["_id"], "item_id": version["item_id"],
            "cart_id": cart_id, "plan_revision": expected_plan_revision + 1,
            "entry_revision": expected_entry_revision + 1,
            "used_fallback": result["used_fallback"],
            "decisions": result["decisions"]}
