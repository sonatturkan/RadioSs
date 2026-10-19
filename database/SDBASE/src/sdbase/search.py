"""Station-scoped catalog search for the independent SDBASE store.

Search keys affect matching only. They never alter titles, people, aliases or
stable item/version IDs. Results use keyset pagination by item ID. A cursor
fixes the relevant catalog generation and filter set, so unrelated playout
events do not interrupt paging while catalog changes cannot mix result views.
"""

from __future__ import annotations

import base64
import binascii
from bisect import bisect_right
from datetime import datetime, timezone
import hashlib
import json
import unicodedata
from typing import Any, Iterator, Mapping
from uuid import UUID

from .identity import Forbidden, Principal, require
from .read_snapshot import ReadSnapshot
from .readiness import readiness_at
from .storage import Conflict, Store, StoreError


_TOLERANT_TRANSLATION = str.maketrans({
    "ç": "c", "ğ": "g", "ı": "i", "ö": "o", "ş": "s", "ü": "u",
})


def turkish_key(value: str, *, tolerant: bool = False) -> str:
    """Return a Turkish-aware comparison key, optionally folding accents.

    Strict mode equates ``Işık`` with ``ışık`` and ``İlhan`` with ``ilhan``.
    Tolerant mode also equates ``Şebnem`` with ``Sebnem`` and ``İstanbul``
    with ``Istanbul``. Whitespace differences are ignored in both modes.
    """
    if not isinstance(value, str):
        raise StoreError("search text must be a string")
    key = unicodedata.normalize("NFC", value)
    key = key.replace("I", "ı").replace("İ", "i").casefold()
    if tolerant:
        key = key.translate(_TOLERANT_TRANSLATION)
        key = "".join(char for char in unicodedata.normalize("NFKD", key)
                      if unicodedata.category(char) != "Mn")
    return " ".join(unicodedata.normalize("NFC", key).split())


def _contains(value: str | None, query: str | None, *, tolerant: bool) -> bool:
    if query is None:
        return True
    return turkish_key(query, tolerant=tolerant) in turkish_key(value or "", tolerant=tolerant)


def _uuid(value: str, label: str) -> str:
    try:
        return str(UUID(str(value)))
    except (ValueError, TypeError, AttributeError) as exc:
        raise StoreError(f"{label} must be a UUID") from exc


def _instant(value: str | datetime, label: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00")) if isinstance(value, str) else value
    except ValueError as exc:
        raise StoreError(f"{label} must be an ISO timestamp") from exc
    if not isinstance(parsed, datetime) or parsed.tzinfo is None or parsed.utcoffset() is None:
        raise StoreError(f"{label} must include a timezone")
    return parsed.astimezone(timezone.utc)


def _duration_bound(value: int | None, label: str) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise StoreError(f"{label} must be a nonnegative integer")
    return value


def _filter_digest(filters: Mapping[str, Any]) -> str:
    raw = json.dumps(filters, ensure_ascii=False, sort_keys=True,
                     separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def _encode_cursor(station_id: str, filter_digest: str, generation: int,
                   after_id: str) -> str:
    payload = {"v": 2, "station": station_id, "filters": filter_digest,
               "generation": generation, "after": after_id}
    raw = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def _decode_cursor(cursor: str, station_id: str, filter_digest: str) -> tuple[int, str]:
    if not isinstance(cursor, str) or len(cursor) > 1024 or not cursor:
        raise StoreError("invalid search cursor")
    try:
        raw = base64.urlsafe_b64decode(cursor + "=" * (-len(cursor) % 4))
        payload = json.loads(raw)
        if (payload.get("v") != 2 or payload.get("station") != station_id
                or payload.get("filters") != filter_digest
                or isinstance(payload.get("generation"), bool)
                or not isinstance(payload.get("generation"), int)
                or payload["generation"] < 0):
            raise ValueError("cursor does not match search")
        return payload["generation"], _uuid(payload["after"], "cursor.after")
    except (ValueError, KeyError, TypeError, UnicodeError, binascii.Error) as exc:
        raise StoreError("invalid or mismatched search cursor") from exc


def _aliases(person: Mapping[str, Any]) -> list[str]:
    values = [person.get("display_name", "")]
    aliases = person.get("aliases")
    for alias in aliases if isinstance(aliases, (list, tuple)) else []:
        if isinstance(alias, str):
            values.append(alias)
        elif isinstance(alias, Mapping):
            text = alias.get("alias", alias.get("name"))
            if isinstance(text, str):
                values.append(text)
    return [value for value in values if isinstance(value, str)]


def _artist_matches(store: Store | ReadSnapshot, item_id: str, artist: str | None,
                    role: str | None, *, tolerant: bool) -> bool:
    if artist is None and role is None:
        return True
    for contribution in store.lookup("contributions", "item", item_id):
        if role is not None and contribution.get("role") != role:
            continue
        if artist is None:
            return True
        person = store.get("people", str(contribution.get("person_id")))
        if person and any(_contains(name, artist, tolerant=tolerant)
                          for name in _aliases(person)):
            return True
    return False


def _preferred_file(files: list[dict]) -> dict | None:
    available = [row for row in files if row.get("status") != "deleted"]
    if not available:
        return None
    # Created times are UTC ISO values in the catalog. ID breaks ties.
    available.sort(key=lambda row: (row.get("status") == "verified",
                                    str(row.get("created_at", "")), row["_id"]),
                   reverse=True)
    return available[0]


def _duration_matches(file: dict | None, minimum: int | None,
                      maximum: int | None) -> bool:
    if minimum is None and maximum is None:
        return True
    value = _media_duration_ms(file)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        return False
    return (minimum is None or value >= minimum) and (maximum is None or value <= maximum)


def _media_duration_ms(file: dict | None) -> int | None:
    """Read verified media's canonical technical duration, with legacy fallback."""
    if file is None:
        return None
    technical = file.get("technical")
    if isinstance(technical, dict) and "duration_ms" in technical:
        value = technical["duration_ms"]
    else:
        value = file.get("duration_ms")
    return value if isinstance(value, int) and not isinstance(value, bool) and value >= 0 else None


_CATALOG_COLLECTIONS = frozenset({
    "station_items", "items", "categories", "people", "contributions",
    "versions", "media_files",
})
_READINESS_COLLECTIONS = frozenset({
    "endpoints", "field_definitions", "metadata_values", "rights_grants",
    "spots", "campaigns", "media_locations", "endpoint_copies",
})

def _station_associations(store: Store | ReadSnapshot, station_id: str,
                          after_id: str | None) -> Iterator[dict]:
    ordered = sorted((row["item_id"], row["_id"]) for row in
                     store.lookup("station_items", "station", station_id))
    # Association IDs are UUIDs, so the maximum Unicode character places the
    # cursor after every association for that item without a linear scan.
    start = bisect_right(ordered, (after_id, "\U0010ffff")) if after_id else 0
    for position in range(start, len(ordered)):
        item_id, association_id = ordered[position]
        association = store.get("station_items", association_id)
        if (association is not None and association.get("station_id") == station_id
                and association.get("item_id") == item_id):
            yield association


def _version_choice(store: Store | ReadSnapshot, station_id: str, versions: list[dict],
                    *, version_label: str | None, tolerant: bool,
                    minimum: int | None, maximum: int | None,
                    endpoint_id: str | None, slot_start: datetime | None,
                    slot_end: datetime | None) -> tuple[dict | None, dict | None, dict | None]:
    ordered = [version for version in versions
               if _contains(version.get("label"), version_label, tolerant=tolerant)]
    ordered.sort(key=lambda row: (row.get("status") == "approved",
                                  row.get("version_no", 0), row["_id"]), reverse=True)
    fallback: tuple[dict, dict | None, dict | None] | None = None
    for version in ordered:
        # This condition is deliberately on the returned version itself. An
        # item with a clean and an explicit version cannot return the explicit
        # version for a clean-label query.
        readiness = None
        if endpoint_id is not None:
            assert slot_start is not None and slot_end is not None
            readiness = readiness_at(store, station_id, version["_id"],
                                     slot_start=slot_start, slot_end=slot_end,
                                     endpoint_id=endpoint_id,
                                     require_endpoint_copy=True)
            selected_file = (store.get("media_files", str(readiness["media_file_id"]))
                             if readiness.get("media_file_id") else None)
            if selected_file is None:
                selected_file = _preferred_file(
                    store.lookup("media_files", "version", version["_id"]))
        else:
            selected_file = _preferred_file(
                store.lookup("media_files", "version", version["_id"]))
        if not _duration_matches(selected_file, minimum, maximum):
            continue
        candidate = (version, selected_file, readiness)
        if endpoint_id is None or (readiness and readiness["ready"]):
            return candidate
        if fallback is None:
            fallback = candidate
    return fallback if fallback is not None else (None, None, None)


def _search_view(store: Store | ReadSnapshot, station_id: str, *, title: str | None,
                 artist: str | None, contribution_role: str | None,
                 category_id: str | None, category: str | None,
                 version_label: str | None, kind: str | None,
                 minimum: int | None, maximum: int | None,
                 endpoint_id: str | None, ready: bool | None,
                 slot_start: datetime | None, slot_end: datetime | None,
                 tolerant: bool, limit: int, after_id: str | None) -> tuple[list[dict], bool]:
    results: list[dict] = []
    for association in _station_associations(store, station_id, after_id):
        item_id = str(association["item_id"])
        item = store.get("items", item_id)
        if item is None or item.get("lifecycle") == "trashed":
            continue
        if kind is not None and item.get("kind") != kind:
            continue
        if not _contains(item.get("title"), title, tolerant=tolerant):
            continue
        if not _artist_matches(store, item_id, artist, contribution_role,
                               tolerant=tolerant):
            continue
        actual_category_id = association.get("category_id")
        if category_id is not None and actual_category_id != category_id:
            continue
        category_record = store.get("categories", str(actual_category_id)) if actual_category_id else None
        if category_record is not None and category_record.get("station_id") != station_id:
            category_record = None
        if category is not None and (category_record is None or not any(
                _contains(category_record.get(field), category, tolerant=tolerant)
                for field in ("name", "code"))):
            continue

        version, file, readiness = _version_choice(
            store, station_id, store.lookup("versions", "item", item_id),
            version_label=version_label, tolerant=tolerant, minimum=minimum,
            maximum=maximum, endpoint_id=endpoint_id, slot_start=slot_start,
            slot_end=slot_end)
        if version_label is not None and version is None:
            continue
        if (minimum is not None or maximum is not None) and version is None:
            continue
        if ready is True and not (readiness and readiness["ready"]):
            continue
        if ready is False and readiness and readiness["ready"]:
            continue
        results.append({
            "id": item_id, "station_id": station_id,
            "title": item["title"], "kind": item["kind"],
            "lifecycle": item["lifecycle"], "revision": item["_rev"],
            "category_id": actual_category_id,
            "category_name": category_record["name"] if category_record else None,
            "eligible": bool(association.get("eligible")),
            "version_id": version["_id"] if version else None,
            "version_no": version["version_no"] if version else None,
            "version_label": version["label"] if version else None,
            "version_status": version["status"] if version else None,
            "media_file_id": file["_id"] if file else None,
            "duration_ms": _media_duration_ms(file),
            "media_status": file.get("status") if file else None,
            "endpoint_ready": readiness["ready"] if readiness else None,
            "blocked_reasons": readiness["blocked_reasons"] if readiness else [],
        })
        if len(results) > limit:
            return results[:limit], True
    return results, False


def _search_items_snapshot(store: ReadSnapshot, principal: Principal, station_id: str, *,
                 title: str | None = None, artist: str | None = None,
                 contribution_role: str | None = None,
                 category_id: str | None = None, category: str | None = None,
                 version_label: str | None = None, kind: str | None = None,
                 min_duration_ms: int | None = None,
                 max_duration_ms: int | None = None,
                 endpoint_id: str | None = None, ready: bool | None = None,
                 slot_start: str | datetime | None = None,
                 slot_end: str | datetime | None = None,
                 tolerant: bool = False, limit: int = 50,
                 cursor: str | None = None) -> dict:
    """Search one station, returning ``items`` and a stable ``next_cursor``.

    Readiness is evaluated for the supplied future slot, never implicitly at
    the current clock time. ``endpoint_id`` and both slot instants are required
    when readiness annotation or filtering is requested.
    """
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "view", station_id)
    user = store.get("users", principal.user_id)
    if user is None or user["status"] != "active":
        raise Forbidden("user is disabled")
    if isinstance(limit, bool) or not isinstance(limit, int) or not 1 <= limit <= 200:
        raise StoreError("limit must be in 1..200")
    if not isinstance(tolerant, bool) or (ready is not None and not isinstance(ready, bool)):
        raise StoreError("tolerant and ready must be booleans")
    for label, value in (("title", title), ("artist", artist),
                         ("contribution_role", contribution_role),
                         ("category", category), ("version_label", version_label),
                         ("kind", kind)):
        if value is not None and not isinstance(value, str):
            raise StoreError(f"{label} must be a string")
    category_id = _uuid(category_id, "category_id") if category_id is not None else None
    minimum = _duration_bound(min_duration_ms, "min_duration_ms")
    maximum = _duration_bound(max_duration_ms, "max_duration_ms")
    if minimum is not None and maximum is not None and maximum < minimum:
        raise StoreError("maximum duration precedes minimum")

    parsed_start = parsed_end = None
    if endpoint_id is not None:
        endpoint_id = _uuid(endpoint_id, "endpoint_id")
        endpoint = store.get("endpoints", endpoint_id)
        if endpoint is None or endpoint.get("station_id") != station_id:
            raise StoreError("endpoint is outside station")
        if slot_start is None or slot_end is None:
            raise StoreError("endpoint readiness requires slot_start and slot_end")
        parsed_start = _instant(slot_start, "slot_start")
        parsed_end = _instant(slot_end, "slot_end")
        if parsed_end <= parsed_start:
            raise StoreError("slot_end must be later than slot_start")
    elif ready is not None or slot_start is not None or slot_end is not None:
        raise StoreError("readiness slot requires endpoint_id")

    filters = {"title": title, "artist": artist,
               "contribution_role": contribution_role,
               "category_id": category_id, "category": category,
               "version_label": version_label, "kind": kind,
               "min_duration_ms": minimum, "max_duration_ms": maximum,
               "endpoint_id": endpoint_id, "ready": ready,
               "slot_start": parsed_start.isoformat() if parsed_start else None,
               "slot_end": parsed_end.isoformat() if parsed_end else None,
               "tolerant": tolerant}
    digest = _filter_digest(filters)
    cursor_generation = None
    after_id = None
    if cursor is not None:
        cursor_generation, after_id = _decode_cursor(cursor, station_id, digest)

    watched = _CATALOG_COLLECTIONS | (_READINESS_COLLECTIONS if endpoint_id else frozenset())
    generation = store.collection_generation(watched)
    if cursor_generation is not None and generation != cursor_generation:
        raise Conflict("search cursor expired because catalog changed")
    rows, has_more = _search_view(
        store, station_id, title=title, artist=artist,
        contribution_role=contribution_role, category_id=category_id,
        category=category, version_label=version_label, kind=kind,
        minimum=minimum, maximum=maximum, endpoint_id=endpoint_id,
        ready=ready, slot_start=parsed_start, slot_end=parsed_end,
        tolerant=tolerant, limit=limit, after_id=after_id)
    next_cursor = (_encode_cursor(station_id, digest, generation, rows[-1]["id"])
                   if has_more and rows else None)
    return {"items": rows, "next_cursor": next_cursor, "sequence": store.sequence,
            "catalog_generation": generation}


def search_items(store: Store, principal: Principal, station_id: str, *,
                 title: str | None = None, artist: str | None = None,
                 contribution_role: str | None = None,
                 category_id: str | None = None, category: str | None = None,
                 version_label: str | None = None, kind: str | None = None,
                 min_duration_ms: int | None = None,
                 max_duration_ms: int | None = None,
                 endpoint_id: str | None = None, ready: bool | None = None,
                 slot_start: str | datetime | None = None,
                 slot_end: str | datetime | None = None,
                 tolerant: bool = False, limit: int = 50,
                 cursor: str | None = None) -> dict:
    """Search and annotate readiness within one immutable committed view."""
    collections = _CATALOG_COLLECTIONS | {"users", "permissions", "tokens"}
    if endpoint_id is not None:
        collections |= _READINESS_COLLECTIONS
    arguments = dict(title=title, artist=artist, contribution_role=contribution_role,
                     category_id=category_id, category=category, version_label=version_label,
                     kind=kind, min_duration_ms=min_duration_ms, max_duration_ms=max_duration_ms,
                     endpoint_id=endpoint_id, ready=ready, slot_start=slot_start, slot_end=slot_end,
                     tolerant=tolerant, limit=limit, cursor=cursor)
    if isinstance(store, ReadSnapshot):
        return _search_items_snapshot(store, principal, station_id, **arguments)
    with ReadSnapshot.capture(store, collections) as view:
        return _search_items_snapshot(view, principal, station_id, **arguments)
