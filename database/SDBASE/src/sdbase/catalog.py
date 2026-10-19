"""Catalog identity, approved versions, people and station-specific metadata."""

from __future__ import annotations

from datetime import datetime, timezone
from uuid import UUID, uuid4

from .identity import Forbidden, Principal, require
from .schema import KINDS
from .storage import Conflict, Store, StoreError


class NotFound(StoreError):
    pass


def _id(value: str) -> str:
    return str(UUID(str(value)))


def _put_fenced(store: Store, principal: Principal, sequence: int,
                collection: str, identifier: str, data: dict, *,
                expected_revision: int | None = None) -> dict:
    """Commit after permission/association reads only if they stayed current."""
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put(collection, identifier, data, expected_revision=expected_revision)
    return tx.commit()[0]


def create_item(store: Store, principal: Principal, station_id: str, *,
                kind: str, title: str, recording_id: str | None = None) -> dict:
    sequence = store.sequence
    station_id = _id(station_id)
    require(store, principal, "edit", station_id)
    if kind not in KINDS or not isinstance(title, str) or not title.strip():
        raise StoreError("valid content kind and nonempty title required")
    item_id = str(uuid4())
    association_id = str(uuid4())
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("items", item_id, {"kind": kind, "title": title.strip(),
                               "lifecycle": "draft", "recording_id": recording_id,
                               "created_at": now})
    tx.put("station_items", association_id, {"station_id": station_id,
                                               "item_id": item_id,
                                               "category_id": None,
                                               "eligible": False,
                                               "rotation_code": None})
    tx.commit()
    return store.get("items", item_id)


def share_item(store: Store, principal: Principal, station_id: str, item_id: str) -> dict:
    sequence = store.sequence
    station_id, item_id = _id(station_id), _id(item_id)
    require(store, principal, "edit", station_id)
    if store.get("items", item_id) is None:
        raise NotFound("item not found")
    require_shared_item_permission(store, principal, item_id, "edit")
    if store.lookup("station_items", "station_item", station_id, item_id):
        raise Conflict("item already belongs to station")
    return _put_fenced(store, principal, sequence, "station_items", str(uuid4()), {
        "station_id": station_id, "item_id": item_id, "eligible": False,
        "category_id": None, "rotation_code": None})


def _item_in_station(store: Store, station_id: str, item_id: str) -> dict:
    matches = store.lookup("station_items", "station_item", station_id, item_id)
    if not matches:
        raise NotFound("item not in station")
    return matches[0]


def require_shared_item_permission(store: Store, principal: Principal,
                                   item_id: str, permission: str) -> None:
    """A global item edit needs authorization in every station sharing it."""
    associations = store.lookup("station_items", "item", item_id)
    if not associations:
        raise NotFound("item has no station association")
    for association in associations:
        require(store, principal, permission, association["station_id"])


def create_version(store: Store, principal: Principal, station_id: str, item_id: str,
                   *, label: str, valid_from: str | None = None,
                   valid_to: str | None = None) -> dict:
    sequence = store.sequence
    station_id, item_id = _id(station_id), _id(item_id)
    require(store, principal, "edit", station_id)
    _item_in_station(store, station_id, item_id)
    require_shared_item_permission(store, principal, item_id, "edit")
    if not isinstance(label, str) or not label.strip():
        raise StoreError("version label must be nonempty")
    numbers = [row["version_no"] for row in store.lookup("versions", "item", item_id)]
    version_id = str(uuid4())
    data = {"item_id": item_id, "version_no": max(numbers, default=0) + 1,
            "label": label.strip(), "status": "draft", "valid_from": valid_from,
            "valid_to": valid_to, "approved_at": None, "approved_by": None,
            "created_at": datetime.now(timezone.utc).isoformat()}
    return _put_fenced(store, principal, sequence, "versions", version_id, data)


def approve_version(store: Store, principal: Principal, station_id: str,
                    version_id: str, *, expected_revision: int) -> dict:
    sequence = store.sequence
    station_id, version_id = _id(station_id), _id(version_id)
    require(store, principal, "approve", station_id)
    version = store.get("versions", version_id)
    if version is None:
        raise NotFound("version not found")
    _item_in_station(store, station_id, version["item_id"])
    require_shared_item_permission(store, principal, version["item_id"], "approve")
    if version["status"] not in ("draft", "review"):
        raise Conflict("only a draft or review version may be approved")
    body = {key: value for key, value in version.items() if not key.startswith("_")}
    body.update(status="approved", approved_at=datetime.now(timezone.utc).isoformat(),
                approved_by=principal.user_id)
    return _put_fenced(store, principal, sequence, "versions", version_id, body,
                       expected_revision=expected_revision)


def set_station_eligibility(store: Store, principal: Principal, station_id: str,
                            item_id: str, eligible: bool, *, expected_revision: int) -> dict:
    station_id, item_id = _id(station_id), _id(item_id)
    # This decision depends on one association and the actual granting rows.
    # A shared read lock captures those rows consistently; unrelated item or
    # plan commits need not invalidate the update. The commit still fences a
    # revoked grant, disabled user, changed station and stale association.
    with store._locked(exclusive=False):
        store._reload_locked()
        require(store, principal, "activate", station_id)
        user = store.get("users", principal.user_id)
        if user is None or user.get("status") != "active":
            raise Forbidden("user is disabled")
        station = store.get("stations", station_id)
        if station is None:
            raise NotFound("station not found")
        grants = [row for row in store.lookup("permissions", "user", principal.user_id)
                  if not row.get("revoked_at") and row["permission"] == "activate" and
                  row.get("station_id") in (None, station_id)]
        if not grants:
            raise Forbidden("permission activate required")
        association = _item_in_station(store, station_id, item_id)
        body = {key: value for key, value in association.items() if not key.startswith("_")}
        body["eligible"] = bool(eligible)
        tx = store.begin(actor=principal.user_id)
        tx.expect("users", user["_id"], user["_rev"])
        tx.expect("stations", station["_id"], station["_rev"])
        tx.expect("permissions", grants[0]["_id"], grants[0]["_rev"])
        tx.put("station_items", association["_id"], body,
               expected_revision=expected_revision)
    return tx.commit()[0]


def set_lifecycle(store: Store, principal: Principal, station_id: str,
                  item_id: str, target: str, *, expected_revision: int) -> dict:
    sequence = store.sequence
    station_id, item_id = _id(station_id), _id(item_id)
    require(store, principal, "edit" if target in ("draft", "archived") else "activate", station_id)
    _item_in_station(store, station_id, item_id)
    require_shared_item_permission(store, principal, item_id,
                                   "edit" if target in ("draft", "archived") else "activate")
    item = store.get("items", item_id)
    if item is None:
        raise NotFound("item not found")
    allowed = {"draft": {"active", "archived", "trashed"},
               "active": {"archived", "withdrawn"},
               "archived": {"active", "trashed"},
               "withdrawn": {"active", "archived"},
               "trashed": {"draft"}}
    if target not in allowed.get(item["lifecycle"], set()):
        raise Conflict("invalid item lifecycle transition")
    body = {key: value for key, value in item.items() if not key.startswith("_")}
    body["lifecycle"] = target
    body["updated_at"] = datetime.now(timezone.utc).isoformat()
    return _put_fenced(store, principal, sequence, "items", item_id, body,
                       expected_revision=expected_revision)


def create_person(store: Store, principal: Principal, station_id: str,
                  display_name: str, kind: str = "person") -> dict:
    sequence = store.sequence
    require(store, principal, "edit", _id(station_id))
    if kind not in ("person", "group", "organization") or not display_name.strip():
        raise StoreError("invalid person or group")
    return _put_fenced(store, principal, sequence, "people", str(uuid4()), {
        "display_name": display_name.strip(), "kind": kind,
        "aliases": [], "created_at": datetime.now(timezone.utc).isoformat(),
    })


def add_contribution(store: Store, principal: Principal, station_id: str,
                     item_id: str, person_id: str, role: str, *, ordinal: int = 0) -> dict:
    sequence = store.sequence
    station_id, item_id, person_id = _id(station_id), _id(item_id), _id(person_id)
    require(store, principal, "edit", station_id)
    _item_in_station(store, station_id, item_id)
    require_shared_item_permission(store, principal, item_id, "edit")
    if role not in {"primary_artist", "joint_artist", "guest_artist", "composer",
                    "producer", "remixer", "host", "speaker", "other"}:
        raise StoreError("invalid contribution role")
    return _put_fenced(store, principal, sequence, "contributions", str(uuid4()), {
        "item_id": item_id, "person_id": person_id, "role": role,
        "ordinal": ordinal})


def add_group_membership(store: Store, principal: Principal, station_id: str,
                         person_id: str, group_id: str,
                         valid_from: str | None = None, valid_to: str | None = None) -> dict:
    sequence = store.sequence
    station_id, person_id, group_id = _id(station_id), _id(person_id), _id(group_id)
    require(store, principal, "edit", station_id)
    # Membership affects rotation wherever this person appears as an artist.
    # Check every linked item and fence those reads against a concurrent share.
    item_ids = {row["item_id"] for row in
                store.lookup("contributions", "person", person_id)}
    if item_ids and not any(store.lookup("station_items", "station_item",
                                      station_id, item_id) for item_id in item_ids):
        raise Forbidden("person has no contribution in station")
    for item_id in item_ids:
        require_shared_item_permission(store, principal, item_id, "edit")
    return _put_fenced(store, principal, sequence, "person_memberships", str(uuid4()), {
        "person_id": person_id, "group_id": group_id,
        "valid_from": valid_from, "valid_to": valid_to})
