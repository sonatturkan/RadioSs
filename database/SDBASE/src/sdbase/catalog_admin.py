"""Station-local categories and item assignments for clock planning."""

from __future__ import annotations

import re
from uuid import UUID, uuid4

from .identity import Principal, require
from .storage import Conflict, Store, StoreError


_CODE = re.compile(r"[A-Za-z0-9_.:-]{1,64}\Z")


def _id(value: str, name: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{name} must be a UUID") from exc


def create_category(store: Store, principal: Principal, station_id: str,
                    code: str, name: str) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "manage", station_id)
    if store.get("stations", station_id) is None:
        raise StoreError("station is absent")
    if not isinstance(code, str) or not _CODE.fullmatch(code) or \
            not isinstance(name, str) or not name.strip() or len(name) > 120:
        raise StoreError("category code or name is invalid")
    if store.lookup("categories", "station_code", station_id, code):
        raise Conflict("category code already exists in station")
    return store.put("categories", str(uuid4()),
                     {"station_id": station_id, "code": code,
                      "name": name.strip(), "created_by": principal.user_id},
                     actor=principal.user_id)


def update_category(store: Store, principal: Principal, station_id: str,
                    category_id: str, *, expected_revision: int,
                    name: str) -> dict:
    station_id, category_id = _id(station_id, "station_id"), _id(category_id, "category_id")
    require(store, principal, "manage", station_id)
    current = store.get("categories", category_id)
    if current is None or current["station_id"] != station_id:
        raise Conflict("category is outside station")
    if type(expected_revision) is not int or current["_rev"] != expected_revision:
        raise Conflict("category changed")
    if not isinstance(name, str) or not name.strip() or len(name) > 120:
        raise StoreError("category name is invalid")
    body = {key: value for key, value in current.items() if key not in {"_id", "_rev"}}
    body["name"] = name.strip()
    return store.put("categories", category_id, body,
                     expected_revision=expected_revision, actor=principal.user_id)


def assign_item_category(store: Store, principal: Principal, station_id: str,
                         item_id: str, category_id: str | None, *,
                         expected_revision: int) -> dict:
    station_id, item_id = _id(station_id, "station_id"), _id(item_id, "item_id")
    require(store, principal, "edit", station_id)
    association = store.lookup("station_items", "station_item", station_id, item_id)
    if not association:
        raise Conflict("item is outside station")
    current = association[0]
    if type(expected_revision) is not int or current["_rev"] != expected_revision:
        raise Conflict("station item changed")
    if category_id is not None:
        category_id = _id(category_id, "category_id")
        category = store.get("categories", category_id)
        if category is None or category["station_id"] != station_id:
            raise Conflict("category is outside station")
    body = {key: value for key, value in current.items() if key not in {"_id", "_rev"}}
    body["category_id"] = category_id
    return store.put("station_items", current["_id"], body,
                     expected_revision=expected_revision, actor=principal.user_id)
