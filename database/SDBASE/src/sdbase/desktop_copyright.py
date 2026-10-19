"""Operator-entered copyright dates for native playlists and automation.

These station preferences filter individual candidates without deleting their
playlist entries. They do not create or approve formal catalog rights grants.
"""

from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timezone

from . import desktop_api as api
from .search import turkish_key
from .storage import Conflict, StoreError


DEFAULT_POLICY = {"schema": 1, "restrict": False, "unregistered": "allow", "rules": []}
MAX_RULES = 1000
FIELDS = ("artist", "album", "title")


def utc_now():
    return datetime.now(timezone.utc)


def _instant(value, name):
    api._text(value, name, 80, True)
    api._stamp(value, name)
    try:
        return datetime.fromisoformat(value).astimezone(timezone.utc)
    except (OverflowError, ValueError) as exc:
        raise StoreError(f"{name} must be a representable timestamp with timezone") from exc


def validate_policy(config):
    fields = {"schema", "restrict", "unregistered", "rules"}
    api._object(config, fields, fields)
    api._int(config["schema"], "copyright policy schema", 1, 1)
    api._bool(config["restrict"], "copyright restrict")
    if not isinstance(config["unregistered"], str) or config["unregistered"] not in {"allow", "deny"}:
        raise StoreError("copyright unregistered must be allow or deny")
    identifiers = set()
    for rule in api._list(config["rules"], "copyright rules", MAX_RULES):
        fields = {"id", *FIELDS, "starts_at", "ends_at", "note", "enabled"}
        api._object(rule, fields, fields)
        identifier = api._id(rule["id"], "copyright rule id")
        if identifier != rule["id"] or identifier in identifiers:
            raise StoreError("copyright rules must have distinct canonical UUID identities")
        identifiers.add(identifier)
        for name in FIELDS:
            api._text(rule[name], "copyright " + name, 200)
        if not any(turkish_key(rule[name]) for name in FIELDS):
            raise StoreError("copyright rule requires an artist, album or track title")
        api._text(rule["note"], "copyright note", 2000)
        api._bool(rule["enabled"], "copyright enabled")
        start, end = (_instant(rule[name], "copyright " + name)
                      for name in ("starts_at", "ends_at"))
        if start >= end:
            raise StoreError("copyright validity interval must be nonempty")
    return deepcopy(config)


def policy_document(store, station_id):
    rows = [row for row in store.lookup("desktop_configurations", "station_kind", station_id,
                                       "copyright_policy")
            if row["status"] == "active" and row["name"] == "main"]
    if len(rows) > 1:
        raise Conflict("multiple active copyright policies require resolution")
    return rows[0] if rows else None


def policy_snapshot(store, station_id):
    document = policy_document(store, station_id)
    return (validate_policy(document["config"]) if document else deepcopy(DEFAULT_POLICY)), document


def evaluate(policy, metadata, *, at=None):
    """Evaluate one instant against every matching rule's date intersection.

    Empty selector fields are wildcards; fields within one rule combine with
    AND. Matching uses the same strict Turkish comparison as catalog search.
    The end is exclusive, so an item expires at its stated end timestamp.
    """
    at = utc_now() if at is None else at
    if not isinstance(at, datetime) or at.tzinfo is None or at.utcoffset() is None:
        raise StoreError("copyright evaluation requires a timestamp with timezone")
    at = at.astimezone(timezone.utc)
    result = {"allowed": True, "reasons": [], "matching_rule_ids": [],
              "checked_at": at.isoformat(), "valid_from": None, "valid_until": None}
    if not policy["restrict"]:
        return result
    keys = {name: turkish_key(metadata.get(name, "") or "") for name in FIELDS}
    matches = [rule for rule in policy["rules"] if rule["enabled"] and all(
        not turkish_key(rule[name]) or turkish_key(rule[name]) == keys[name] for name in FIELDS)]
    result["matching_rule_ids"] = [rule["id"] for rule in matches]
    if not matches:
        if policy["unregistered"] == "deny":
            result.update(allowed=False, reasons=["copyright_unregistered"])
        return result
    start = max(_instant(rule["starts_at"], "copyright starts_at") for rule in matches)
    end = min(_instant(rule["ends_at"], "copyright ends_at") for rule in matches)
    result.update(valid_from=start.isoformat(), valid_until=end.isoformat())
    if at < start:
        result["reasons"].append("copyright_not_started")
    if at >= end:
        result["reasons"].append("copyright_expired")
    result["allowed"] = not result["reasons"]
    return result
