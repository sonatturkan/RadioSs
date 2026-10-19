"""Station-scoped details and permission checks from one committed read view.

The returned ``_rev`` belongs to its row. Writers send that revision in their
optimistic precondition. Unrelated commits no longer invalidate a detail read.
"""

from __future__ import annotations

import base64
import binascii
import json
from functools import wraps
import re
from uuid import UUID

from .catalog import NotFound
from .identity import Forbidden, Principal, require
from .read_snapshot import ReadSnapshot
from .storage import Conflict, Store, StoreError


READ_ROUTES = (
    "/v1/items/{item_id}",
    "/v1/items/{item_id}/versions",
    "/v1/versions/{version_id}",
    "/v1/plans/{plan_id}",
    "/v1/plans/{plan_id}/entries",
    "/v1/plans/{plan_id}/entries/{entry_id}",
    "/v1/plans/{plan_id}/revisions",
    "/v1/plans/{plan_id}/revisions/{revision_id}",
    "/v1/plans/{plan_id}/revisions/{revision_id}/entries",
    "/v1/stations/{station_id}/items",
    "/v1/stations/{station_id}/items/{item_id}",
)


_DETAIL_COLLECTIONS = ("users", "permissions", "tokens", "stations", "station_items", "items",
                       "versions", "plans", "plan_revisions", "plan_entries")


def _snapshot_read(action):
    @wraps(action)
    def read(store, *args, **kwargs):
        if isinstance(store, ReadSnapshot):
            return action(store, *args, **kwargs)
        with ReadSnapshot.capture(store, _DETAIL_COLLECTIONS) as view:
            return action(view, *args, **kwargs)
    return read


def _uuid(value: str, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (ValueError, TypeError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _start(store: Store, principal: Principal, station_id: str) -> tuple[int, str]:
    sequence = store.sequence
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "view", station_id)
    user = store.get("users", principal.user_id)
    if user is None or user["status"] != "active":
        raise Forbidden("user is disabled")
    if store.get("stations", station_id) is None:
        raise NotFound("resource not found in station")
    return sequence, station_id


def _finish(store: Store, sequence: int, **records: object) -> dict:
    return {"sequence": sequence, **records}


def _item(store: Store, station_id: str, item_id: str) -> tuple[dict, dict]:
    item_id = _uuid(item_id, "item_id")
    associations = store.lookup("station_items", "station_item", station_id, item_id)
    # Missing and outside-station IDs intentionally return the same response.
    if not associations:
        raise NotFound("resource not found in station")
    item = store.get("items", item_id)
    if item is None:
        raise NotFound("resource not found in station")
    return item, associations[0]


def _plan(store: Store, station_id: str, plan_id: str) -> dict:
    plan = store.get("plans", _uuid(plan_id, "plan_id"))
    if plan is None or plan["station_id"] != station_id:
        raise NotFound("resource not found in station")
    return plan


def _revision(store: Store, plan: dict, revision_id: str | None) -> dict:
    if revision_id is None:
        revision_id = plan.get("draft_revision_id") or plan.get("current_revision_id")
    if revision_id is None:
        raise NotFound("resource not found in station")
    revision = store.get("plan_revisions", _uuid(revision_id, "revision_id"))
    if revision is None or revision["plan_id"] != plan["_id"]:
        raise NotFound("resource not found in station")
    return revision


@_snapshot_read
def get_item(store: Store, principal: Principal, station_id: str, item_id: str) -> dict:
    """Read an item and only the requested station's association and revisions."""
    sequence, station_id = _start(store, principal, station_id)
    item, association = _item(store, station_id, item_id)
    return _finish(store, sequence, item=item, station_item=association)


@_snapshot_read
def list_item_versions(store: Store, principal: Principal, station_id: str,
                       item_id: str) -> dict:
    sequence, station_id = _start(store, principal, station_id)
    item, association = _item(store, station_id, item_id)
    versions = sorted(store.lookup("versions", "item", item["_id"]),
                      key=lambda row: (row["version_no"], row["_id"]))
    return _finish(store, sequence, item=item, station_item=association,
                   versions=versions)


@_snapshot_read
def get_version(store: Store, principal: Principal, station_id: str,
                version_id: str) -> dict:
    sequence, station_id = _start(store, principal, station_id)
    version = store.get("versions", _uuid(version_id, "version_id"))
    if version is None:
        raise NotFound("resource not found in station")
    item, association = _item(store, station_id, version["item_id"])
    return _finish(store, sequence, version=version, item=item,
                   station_item=association)


@_snapshot_read
def get_plan(store: Store, principal: Principal, station_id: str, plan_id: str) -> dict:
    sequence, station_id = _start(store, principal, station_id)
    plan = _plan(store, station_id, plan_id)
    draft = (_revision(store, plan, plan["draft_revision_id"])
             if plan.get("draft_revision_id") else None)
    current = (_revision(store, plan, plan["current_revision_id"])
               if plan.get("current_revision_id") else None)
    return _finish(store, sequence, plan=plan, draft_revision=draft,
                   current_revision=current)


@_snapshot_read
def list_plan_revisions(store: Store, principal: Principal, station_id: str,
                        plan_id: str) -> dict:
    sequence, station_id = _start(store, principal, station_id)
    plan = _plan(store, station_id, plan_id)
    revisions = sorted(store.lookup("plan_revisions", "plan", plan["_id"]),
                       key=lambda row: (row["revision_no"], row["_id"]))
    return _finish(store, sequence, plan=plan, revisions=revisions)


@_snapshot_read
def get_plan_revision(store: Store, principal: Principal, station_id: str,
                      plan_id: str, revision_id: str) -> dict:
    sequence, station_id = _start(store, principal, station_id)
    plan = _plan(store, station_id, plan_id)
    revision = _revision(store, plan, revision_id)
    return _finish(store, sequence, plan=plan, revision=revision)


@_snapshot_read
def list_plan_entries(store: Store, principal: Principal, station_id: str,
                       plan_id: str, revision_id: str | None = None) -> dict:
    """Read one explicit revision, or the current draft then current approval."""
    sequence, station_id = _start(store, principal, station_id)
    plan = _plan(store, station_id, plan_id)
    revision = _revision(store, plan, revision_id)
    entries = sorted(store.lookup("plan_entries", "revision", revision["_id"]),
                     key=lambda row: (row["ordinal"], row["_id"]))
    return _finish(store, sequence, plan=plan, revision=revision, entries=entries)


@_snapshot_read
def get_plan_entry(store: Store, principal: Principal, station_id: str,
                    plan_id: str, entry_id: str) -> dict:
    sequence, station_id = _start(store, principal, station_id)
    plan = _plan(store, station_id, plan_id)
    entry = store.get("plan_entries", _uuid(entry_id, "entry_id"))
    if entry is None:
        raise NotFound("resource not found in station")
    revision = _revision(store, plan, entry["plan_revision_id"])
    return _finish(store, sequence, plan=plan, revision=revision, entry=entry)


@_snapshot_read
def get_station_item(store: Store, principal: Principal, station_id: str,
                      item_id: str) -> dict:
    return get_item(store, principal, station_id, item_id)


@_snapshot_read
def list_station_items(store: Store, principal: Principal, station_id: str,
                        *, limit: int = 100, cursor: str | None = None) -> dict:
    """Page station associations without exposing other stations sharing items."""
    sequence, station_id = _start(store, principal, station_id)
    if type(limit) is not int or not 1 <= limit <= 1000:
        raise StoreError("limit must be an integer from 1 to 1000")
    generation = store.collection_generation(("station_items",))
    after = ""
    if cursor is not None:
        try:
            if not isinstance(cursor, str) or not 1 <= len(cursor) <= 1024:
                raise ValueError("invalid cursor")
            raw = base64.b64decode(cursor + "=" * (-len(cursor) % 4),
                                  altchars=b"-_", validate=True)
            claim = json.loads(raw)
            if (not isinstance(claim, dict) or claim.get("v") != 1 or
                    claim.get("station_id") != station_id or
                    type(claim.get("generation")) is not int):
                raise ValueError("invalid cursor")
            after = _uuid(claim["after_id"], "cursor.after_id")
        except (ValueError, TypeError, KeyError, binascii.Error, UnicodeError) as exc:
            raise StoreError("invalid station-item cursor") from exc
        if claim["generation"] != generation:
            raise Conflict("station associations changed; start a new page sequence")
    associations = [row for row in store.lookup("station_items", "station", station_id)
                    if row["_id"] > after]
    selected = associations[:limit]
    next_cursor = None
    if len(associations) > limit:
        claim = {"v": 1, "station_id": station_id, "generation": generation,
                 "after_id": selected[-1]["_id"]}
        next_cursor = base64.urlsafe_b64encode(json.dumps(
            claim, sort_keys=True, separators=(",", ":")).encode()).rstrip(b"=").decode()
    return _finish(store, sequence, station_id=station_id, station_items=selected,
                   next_cursor=next_cursor)


def dispatch_read(store: Store, principal: Principal, path: str, query: dict) -> dict | None:
    """Dispatch exact public GET paths; unknown paths remain a HTTP 404."""
    uuid_pattern = r"([0-9a-fA-F-]{36})"
    routes = (
        (rf"/v1/items/{uuid_pattern}", get_item),
        (rf"/v1/items/{uuid_pattern}/versions", list_item_versions),
        (rf"/v1/versions/{uuid_pattern}", get_version),
        (rf"/v1/plans/{uuid_pattern}", get_plan),
        (rf"/v1/plans/{uuid_pattern}/entries", list_plan_entries),
        (rf"/v1/plans/{uuid_pattern}/entries/{uuid_pattern}", get_plan_entry),
        (rf"/v1/plans/{uuid_pattern}/revisions", list_plan_revisions),
        (rf"/v1/plans/{uuid_pattern}/revisions/{uuid_pattern}", get_plan_revision),
        (rf"/v1/plans/{uuid_pattern}/revisions/{uuid_pattern}/entries", list_plan_entries),
    )
    for pattern, action in routes:
        match = re.fullmatch(pattern, path)
        if match:
            arguments = {}
            if action is list_plan_entries and len(match.groups()) == 1:
                arguments["revision_id"] = query.get("revision_id")
            return action(store, principal, query["station_id"], *match.groups(),
                          **arguments)
    match = re.fullmatch(rf"/v1/stations/{uuid_pattern}/items(?:/{uuid_pattern})?", path)
    if match:
        if "station_id" in query and _uuid(query["station_id"], "station_id") != \
                _uuid(match.group(1), "station_id"):
            raise StoreError("query station_id differs from path station_id")
        if match.group(2):
            return get_station_item(store, principal, match.group(1), match.group(2))
        return list_station_items(store, principal, match.group(1),
                                  limit=int(query.get("limit", "100")),
                                  cursor=query.get("cursor"))
    return None
