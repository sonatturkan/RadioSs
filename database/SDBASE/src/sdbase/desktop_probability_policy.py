"""Bounded immutable native automation probabilities and catalogue origin context.

Validation/hydration never draws a choice, mutates a policy, or grants playback.
Native C owns deterministic selection and actual-consumption checkpoints.
"""
from __future__ import annotations

import math
from zoneinfo import ZoneInfoNotFoundError

from . import _platform
from .storage import StoreError

NODE_KEYS = {"schema", "seed", "transitions", "jingles", "children"}
GROUP_KEYS = {"id", "scope", "ids", "probability", "policy"}
COMMON_KEYS = {"id", "probability", "frequency", "interval_minutes", "excluded_times", "timezone"}
TRANSITION_KEYS = COMMON_KEYS | {"mode", "overlap_ms", "early_seconds", "effect"}
JINGLE_KEYS = COMMON_KEYS | {"playlist_id", "player", "anchor", "seconds", "duck_db", "gain_db", "fade_ms"}


def _finite(value, name, lower, upper):
    if type(value) not in (int, float) or not lower <= value <= upper or not math.isfinite(value):
        raise StoreError(f"{name} must be finite in {lower}..{upper}")
    return value


def _catalogue_entry(identifier, station_id, state, *, playlist_id=None, active=False):
    from .desktop_api import _id, _reference
    identifier = _id(identifier, "probability catalogue entry_id")
    entry = state.get("desktop_playlist_entries", {}).get(identifier, {}).get("data")
    version = state.get("playlist_versions", {}).get(entry.get("playlist_version_id") if entry else None, {}).get("data")
    if not entry or entry.get("station_id") != station_id or not version:
        raise StoreError("probability track must reference a catalogue entry in this station")
    actual_playlist = version.get("playlist_id")
    if playlist_id is not None and actual_playlist != _id(playlist_id, "policy playlist_id"):
        raise StoreError("policy catalogue entry does not belong to its playlist")
    _reference(state, station_id, actual_playlist, "playlists", "probability playlist_id", active=active)
    playlist = state["playlists"][actual_playlist]["data"]
    if active and (entry.get("status") != "active" or version.get("status") != "draft" or
                   entry.get("playlist_version_id") != playlist.get("draft_version_id")):
        raise StoreError("probability track requires an active entry in the current playlist draft")
    return entry, actual_playlist


def validate_probability_policy(policy, station_id, state, *, persisted=False, previous=None, automation_ids=None):
    from .desktop_api import _id, _int, _list, _object, _reference
    active = not (persisted or policy == previous)
    identities, groups, rules = set(), 0, 0

    def identity(value, name):
        identifier = _id(value, name)
        if identifier in identities:
            raise StoreError("probability group and rule identities must be globally distinct")
        identities.add(identifier)
        return identifier

    def probability(value):
        return 0 if value is None else _finite(value, "probability", 0, 100)

    def common(rule):
        identity(rule["id"], "probability rule id")
        if "probability" in rule:
            probability(rule["probability"])
        if "frequency" in rule:
            _int(rule["frequency"], "probability frequency", 1, 1000)
        if "interval_minutes" in rule:
            _int(rule["interval_minutes"], "probability interval_minutes", 0, 10080)
        for window in _list(rule.get("excluded_times", []), "probability excluded_times", 16):
            _object(window, {"start_minute", "end_minute"}, {"start_minute", "end_minute"})
            _int(window["start_minute"], "exclusion start_minute", 0, 1439)
            _int(window["end_minute"], "exclusion end_minute", 0, 1439)
        if "timezone" in rule:
            # Excluded windows are station-local in this IANA zone; without it, UTC.
            zone = rule["timezone"]
            if not isinstance(zone, str) or not zone or len(zone.encode("utf-8")) > 64:
                raise StoreError("probability timezone must be a known IANA timezone")
            try:
                _platform.canonical_timezone(zone)   # exact case (Windows lookups ignore it)
            except (ZoneInfoNotFoundError, ValueError) as exc:
                raise StoreError("probability timezone must be a known IANA timezone") from exc

    def node(value, depth):
        nonlocal groups, rules
        if depth > 8:
            raise StoreError("probability policy exceeds depth 8")
        _object(value, NODE_KEYS, {"schema"})
        _int(value["schema"], "probability policy schema", 1, 1)
        if "seed" in value:
            seed = value["seed"]
            try:
                valid_seed = isinstance(seed, str) and len(seed.encode("utf-8")) <= 128
            except UnicodeError:
                valid_seed = False
            if not valid_seed or any(ord(c) < 32 or ord(c) == 127 for c in seed):
                raise StoreError("probability seed must be at most 128 UTF-8 bytes without controls")
        for name, fields, required in (("transitions", TRANSITION_KEYS, {"id", "mode"}),
                                        ("jingles", JINGLE_KEYS, {"id", "playlist_id", "player", "anchor", "seconds"})):
            total = 0
            for rule in _list(value.get(name, []), "probability " + name, 16):
                rules += 1
                if rules > 512:
                    raise StoreError("probability policy exceeds 512 rules")
                _object(rule, fields, required)
                common(rule)
                total += probability(rule.get("probability"))
                if name == "transitions":
                    mode = _int(rule["mode"], "probability transition mode", 0, 3)
                    overlap = _int(rule.get("overlap_ms", 0), "probability transition overlap_ms", 0, 30000)
                    _finite(rule.get("early_seconds", 0), "probability transition early_seconds", 0, 3600)
                    effect = rule.get("effect", "none")
                    if not isinstance(effect, str) or effect not in {"none", "lowpass"}:
                        raise StoreError("probability transition effect must be none or lowpass")
                    if mode == 0 and (overlap or effect != "none"):
                        raise StoreError("EOF probability transition requires zero overlap and no effect")
                    if (mode == 3 or effect == "lowpass") and not overlap:
                        raise StoreError("beatmatch/lowpass probability transition requires positive overlap")
                else:
                    _reference(state, station_id, rule["playlist_id"], "playlists", "probability jingle playlist_id", active=active)
                    _int(rule["player"], "probability jingle player", 0, 31)
                    if not isinstance(rule["anchor"], str) or rule["anchor"] not in {"after_start", "before_end"}:
                        raise StoreError("probability jingle anchor must be after_start or before_end")
                    _finite(rule["seconds"], "probability jingle seconds", 0, 86400)
                    _finite(rule.get("duck_db", -12), "probability jingle duck_db", -60, 0)
                    _finite(rule.get("gain_db", 0), "probability jingle gain_db", -60, 12)
                    _int(rule.get("fade_ms", 100), "probability jingle fade_ms", 0, 5000)
            if total > 100 + 1e-9:
                raise StoreError("comparable probability rules cannot total above 100 percent")
        seen, remainders, totals = {}, set(), {}
        for group in _list(value.get("children", []), "probability children", 256):
            groups += 1
            if groups > 256:
                raise StoreError("probability policy exceeds 256 groups")
            _object(group, GROUP_KEYS, {"id", "scope", "ids", "policy"})
            identity(group["id"], "probability group id")
            scope = group["scope"]
            if not isinstance(scope, str) or scope not in {"automation", "playlist", "track"}:
                raise StoreError("probability scope must be automation, playlist or track")
            selected = [_id(v, "probability selected id") for v in _list(group["ids"], "probability selected ids", 500)]
            if len(set(selected)) != len(selected) or set(selected).intersection(seen.get(scope, set())):
                raise StoreError("selected probability IDs must be disjoint within sibling scope")
            seen.setdefault(scope, set()).update(selected)
            if not selected:
                if scope in remainders:
                    raise StoreError("only one explicit remaining group is allowed per sibling scope")
                remainders.add(scope)
            for identifier in selected:
                if scope == "automation":
                    if automation_ids is not None and identifier not in automation_ids:
                        raise StoreError("probability automation id must name this calendar's rotation definition")
                elif scope == "playlist":
                    _reference(state, station_id, identifier, "playlists", "probability selected playlist_id", active=active)
                else:
                    _catalogue_entry(identifier, station_id, state, active=active)
            totals[scope] = totals.get(scope, 0) + probability(group.get("probability"))
            if totals[scope] > 100 + 1e-9:
                raise StoreError("comparable probability groups cannot total above 100 percent")
            node(group["policy"], depth + 1)
    node(policy, 0)


def validate_policy_context(context, row, station_id, state, *, persisted=False):
    """Validate original catalogue identity, separately from occurrence row.id."""
    from .desktop_api import _id, _object, _media_binding, MEDIA_BINDING_KEYS
    from . import desktop_playlist
    _object(context, {"automation_id", "playlist_id", "entry_id", "source_id"}, {"playlist_id", "entry_id"})
    for name, value in context.items():
        _id(value, "policy_context " + name)
    original, _playlist = _catalogue_entry(context["entry_id"], station_id, state,
        playlist_id=context["playlist_id"], active=not persisted)
    binding = {key: original[key] for key in MEDIA_BINDING_KEYS if key in original}
    if desktop_playlist.ONLINE_SOURCE_KEYS.intersection(original) or "analysis_media" in original:
        # The selected programme carries the real managed-media identities,
        # while a provider catalogue entry keeps them in analysis_media.
        # Validate that source and its exact binding before comparing them;
        # a provider reference alone cannot authorize a programme recording.
        fields = (desktop_playlist.METADATA | MEDIA_BINDING_KEYS |
                  desktop_playlist.ONLINE_SOURCE_KEYS | desktop_playlist.ANALYSIS_SOURCE_KEYS)
        source = desktop_playlist._metadata({name: original[name] for name in fields if name in original}, persisted=persisted)
        desktop_playlist._online_binding(source, station_id, state, persisted=persisted)
        binding = source.get("analysis_media", {})
    if binding:
        _media_binding(binding, station_id, state, persisted=persisted)
    for key in MEDIA_BINDING_KEYS:
        if binding.get(key) != row.get(key):
            raise StoreError("policy_context catalogue entry and programme media/version binding differ")


def jingle_playlist_ids(policy):
    """Iterate already validated references; there is no draw or mutation."""
    for rule in policy.get("jingles", []):
        yield rule["playlist_id"]
    for child in policy.get("children", []):
        yield from jingle_playlist_ids(child["policy"])
