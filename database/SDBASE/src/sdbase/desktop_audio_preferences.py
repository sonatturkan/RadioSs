"""Typed scopes for the optional native LUFS normalization policy.

The legacy enable/target preferences remain separate. Selectors describe future
playback intent, so retired or currently unassigned automation UUIDs stay valid.
Scheduling uses local civil minutes in the persisted IANA timezone; an equal
start/end is a full selected day and overnight windows belong to their start day.
"""

from zoneinfo import ZoneInfo

from . import desktop_api as api
from .storage import StoreError


POLICY_FIELDS = frozenset({"schema_version", "players", "contexts", "phase",
                          "automation_mode", "automation_ids", "pairs_mode", "pairs",
                          "schedule_enabled", "timezone", "windows"})


def _choice(value, name, choices):
    if not isinstance(value, str) or value not in choices:
        raise StoreError(f"{name} must be one of {', '.join(sorted(choices))}")


def _distinct_integers(value, name, low, high):
    selected = set()
    for item in api._list(value, name, high - low + 1):
        item = api._int(item, name, low, high)
        if item in selected:
            raise StoreError(f"{name} must contain distinct integers")
        selected.add(item)


def validate_loudness_policy(policy):
    api._object(policy, POLICY_FIELDS, POLICY_FIELDS)
    api._int(policy["schema_version"], "loudness policy schema_version", 1, 1)
    _distinct_integers(policy["players"], "loudness policy players", 1, 32)
    contexts = set()
    for context in api._list(policy["contexts"], "loudness policy contexts", 3):
        _choice(context, "loudness policy context", {"manual", "automation", "autodj"})
        if context in contexts:
            raise StoreError("loudness policy contexts must be distinct")
        contexts.add(context)
    _choice(policy["phase"], "loudness policy phase", {"always", "transition", "steady"})
    _choice(policy["automation_mode"], "loudness policy automation_mode", {"all", "assigned", "selected"})
    automations = set()
    for identifier in api._list(policy["automation_ids"], "loudness policy automation_ids", 64):
        canonical = api._id(identifier, "loudness policy automation UUID")
        if identifier != canonical or canonical in automations:
            raise StoreError("loudness policy automation_ids must contain distinct canonical UUIDs")
        automations.add(canonical)
    _choice(policy["pairs_mode"], "loudness policy pairs_mode", {"all", "selected"})
    pairs = set()
    for pair in api._list(policy["pairs"], "loudness policy pairs", 32 * 31):
        api._object(pair, {"from", "to"}, {"from", "to"})
        source = api._int(pair["from"], "loudness policy pair from", 1, 32)
        target = api._int(pair["to"], "loudness policy pair to", 1, 32)
        if source == target or (source, target) in pairs:
            raise StoreError("loudness policy pairs must be distinct directed non-self player pairs")
        pairs.add((source, target))
    api._bool(policy["schedule_enabled"], "loudness policy schedule_enabled")
    zone = api._text(policy["timezone"], "loudness policy timezone", 127, True)
    try:
        if len(zone.encode("utf-8")) > 127:
            raise ValueError("timezone byte length")
        ZoneInfo(zone)
    except (ValueError, KeyError, UnicodeError) as exc:
        raise StoreError("loudness policy timezone must be an IANA identifier of at most 127 UTF-8 bytes") from exc
    for window in api._list(policy["windows"], "loudness policy windows", 16):
        fields = {"days", "start_minute", "end_minute"}
        api._object(window, fields, fields)
        _distinct_integers(window["days"], "loudness policy window days", 1, 7)
        api._int(window["start_minute"], "loudness policy window start_minute", 0, 1439)
        api._int(window["end_minute"], "loudness policy window end_minute", 0, 1439)
