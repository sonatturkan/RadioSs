"""Typed native automation timetable and ordered rotation drafts.

Calendar persistence uses desktop documents, including their station scope,
revision checks and idempotent receipts. Saved blocks describe preparation;
they do not authorize playback or claim that any programme has aired.
"""

from __future__ import annotations

import re
from zoneinfo import ZoneInfoNotFoundError

from . import _platform
from .storage import StoreError


EVENT_FIELDS = frozenset({"id", "title", "date", "start_minute", "end_minute",
                          "color", "rotations"})
ROTATION_FIELDS = frozenset({"id", "playlist_id", "frequency", "random",
                             "track_random", "advert_random", "sweeper_random"})
ROTATION_OPTIONAL_FIELDS = frozenset({"media_kind", "autodj"})
SOURCE_OPTIONAL_FIELDS = ROTATION_OPTIONAL_FIELDS | frozenset({
    "weight", "count", "start_minute", "end_minute", "interval_minutes", "settings",
    "track_selection_mode", "track_rules", "source_type", "live", "start_date", "end_date"})
SETTINGS_FIELDS = frozenset({"selection_mode", "track_gap_minutes", "artist_gap_minutes",
                             "album_gap_minutes", "transitions"})
# kind_transitions: AutoDJ boundary by content kind; autodj_players: rotation-
# wide AutoDJ channels by content kind (a source's own "autodj" map wins).
# soft_end_seconds (0..600, default 0 = hard block end) lets a last item end
# after the block; relax_gaps (default false) relaxes repeat gaps as a last
# resort instead of leaving silence. Both mirror the native C planner.
SETTINGS_OPTIONAL_FIELDS = frozenset({"transition_selection_mode", "kind_transitions", "autodj_players", "probability_policy", "automation_schema",
                                      "soft_end_seconds", "relax_gaps", "excluded_kinds", "autodj_presets", "autodj_automation",
                                      "automation_steps", "link_transitions", "programme_entry"})
# Schema flow (kılavuz §02-§04): automation_steps are the inner sequence records
# of a schema or of one playlist ("2 2 2": same order on all sides plus a kind
# fit), link_transitions the default transition of one source -> source link,
# programme_entry the order and transition used when the next programme starts
# (alternatives are the probability path). A record carries preset_id or
# transition, never both; without either it only takes part in the order match.
# Same contract as the native planner (src/core/automation_calendar_plan.c).
AUTOMATION_STEP_KINDS = frozenset({"music", "talk", "dj", "advertisement", "jingle", "sweeper"})
SCHEMA_TRANSITION_FIELDS = frozenset({"mode", "overlap_ms"})
# Live event step: a source row of source_type "live" (no playlist), or the
# "Live event" flag of a calendar programme. label selects the mixer role,
# channels the mixer channels (distinct 1..32), automation_id the special live
# event schema of this calendar ("" = none), end_rule how the step ends.
LIVE_LABELS = frozenset({"dj", "talk", "music"})
LIVE_END_RULES = frozenset({"manual", "duration"})
LIVE_FIELDS = frozenset({"label", "channels", "automation_id", "end_rule", "duration_ms"})
SOURCE_TYPES = frozenset({"playlist", "live"})
# autodj_presets / autodj_automation: designed AutoDJ transitions (curves and
# one mixer effect), their assignment per content kind with sequential or
# random rotation, and clock/day schedules. Same contract as the native
# planner (src/core/transition_preset.c); rotation level only.
TRANSITION_PRESET_FIELDS = frozenset({"id", "name", "duration_ms", "beat_sync", "out", "in", "effect"})
TRANSITION_SIDE_FIELDS = frozenset({"volume", "bass", "tone", "tone_kind"})
TRANSITION_EFFECT_FIELDS = frozenset({"type", "depth", "side", "mix", "bpm"})
TRANSITION_EFFECTS = frozenset({"none", "delay", "echo", "spiral", "reverb", "trans", "filter", "flanger", "phaser",
                                "robot", "melodic", "slip_roll", "roll", "rev_roll", "midi_lfo"})
TRANSITION_TONES = frozenset({"none", "lowpass", "highpass"})
TRANSITION_ROTATIONS = frozenset({"sequential", "random"})
TRANSITION_SCHEDULE_FIELDS = frozenset({"id", "name", "days", "start_minute", "end_minute", "kind", "presets", "rotation"})


def _unit(value, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= 1 or value != value:
        raise StoreError(f"{name} must be a number 0..1")
    return float(value)


def _curve(value, name):
    from .desktop_api import _list
    points = _list(value, name, 8)
    if len(points) < 2:
        raise StoreError(f"{name} needs at least two points")
    previous = None
    for point in points:
        if not isinstance(point, list) or len(point) != 2:
            raise StoreError(f"{name} points must be [t, v] pairs")
        t, v = _unit(point[0], name + " t"), _unit(point[1], name + " v")
        if previous is not None and t < previous:
            raise StoreError(f"{name} points must not go back in time")
        previous = t
    if points[0][0] != 0 or points[-1][0] != 1:
        raise StoreError(f"{name} must start at t=0 and end at t=1")
    return points


def _preset_name(value, name):
    from .desktop_api import _text
    text = _text(value, name, 60, True)
    if any(ord(c) < 32 for c in text):
        raise StoreError(f"{name} must not contain control characters")
    return text


def validate_transition_preset(preset, *, require_id=True):
    from .desktop_api import _bool, _id, _int, _object, _text
    _object(preset, TRANSITION_PRESET_FIELDS, {"name", "duration_ms", "out", "in", "effect"} | ({"id"} if require_id else set()))
    if "id" in preset:
        _id(preset["id"], "transition preset id")
    _preset_name(preset["name"], "transition preset name")
    _int(preset["duration_ms"], "transition preset duration_ms", 500, 30000)
    if "beat_sync" in preset:
        _bool(preset["beat_sync"], "transition preset beat_sync")
    for side in ("out", "in"):
        lane = _object(preset[side], TRANSITION_SIDE_FIELDS, TRANSITION_SIDE_FIELDS)
        for curve in ("volume", "bass", "tone"):
            _curve(lane[curve], f"transition preset {side} {curve}")
        if _text(lane["tone_kind"], "transition preset tone_kind", 20, True) not in TRANSITION_TONES:
            raise StoreError("transition preset tone_kind must be none, lowpass or highpass")
    effect = _object(preset["effect"], TRANSITION_EFFECT_FIELDS, {"type", "depth", "side", "mix"})
    if _text(effect["type"], "transition preset effect type", 20, True) not in TRANSITION_EFFECTS:
        raise StoreError("transition preset effect type is not a mixer effect")
    _int(effect["depth"], "transition preset effect depth", 0, 100)
    if _text(effect["side"], "transition preset effect side", 10, True) not in {"out", "in"}:
        raise StoreError("transition preset effect side must be out or in")
    _curve(effect["mix"], "transition preset effect mix")
    if "bpm" in effect:
        _int(effect["bpm"], "transition preset effect bpm", 30, 300)


def validate_transition_presets(value):
    from .desktop_api import _list
    presets = _list(value, "rotation autodj_presets", 32)
    ids = set()
    for preset in presets:
        validate_transition_preset(preset)
        if preset["id"] in ids:
            raise StoreError("transition preset ids must be distinct")
        ids.add(preset["id"])
    return ids


def _preset_kind_usable(automation, preset_id, kind):
    """A preset assigned to content kinds (AutoDJ automation kinds.<kind> or a
    schedule row of one kind) serves only those kinds; mirrors the planner's
    radios_automation_preset_kind_usable."""
    if not isinstance(automation, dict):
        return True
    wanted, typed = preset_id.lower(), False
    for name, entry in (automation.get("kinds") or {}).items():
        ids = [str(item).lower() for item in (entry or {}).get("presets", []) if isinstance(item, str)]
        if wanted in ids:
            if name == kind:
                return True
            typed = True
    for row in automation.get("schedule") or []:
        ids = [str(item).lower() for item in (row or {}).get("presets", []) if isinstance(item, str)]
        if wanted in ids:
            if row.get("kind") in ("all", kind):
                return True
            typed = True
    return not typed


def validate_transition_automation(value, preset_ids):
    from .desktop_api import _bool, _id, _int, _list, _object, _text

    def references(ids, name, require_any):
        chosen = _list(ids, name, 32)
        if require_any and not chosen:
            raise StoreError(f"{name} needs at least one preset")
        if len(set(map(str, chosen))) != len(chosen):
            raise StoreError(f"{name} must not repeat a preset")
        for identifier in chosen:
            if not isinstance(identifier, str) or identifier not in preset_ids:
                raise StoreError(f"{name} refers to an unknown transition preset")

    _object(value, {"enabled", "kinds", "schedule"}, {"enabled", "kinds"})
    _bool(value["enabled"], "AutoDJ automation enabled")
    kinds = _object(value["kinds"], AUTODJ_KINDS)
    for kind, entry in kinds.items():
        _object(entry, {"presets", "rotation"}, {"presets", "rotation"})
        references(entry["presets"], "AutoDJ automation " + kind + " presets", False)
        if _text(entry["rotation"], "AutoDJ automation rotation", 20, True) not in TRANSITION_ROTATIONS:
            raise StoreError("AutoDJ automation rotation must be sequential or random")
    if "schedule" in value:
        seen = set()
        for row in _list(value["schedule"], "AutoDJ automation schedule", 32):
            _object(row, TRANSITION_SCHEDULE_FIELDS, TRANSITION_SCHEDULE_FIELDS)
            identifier = _id(row["id"], "AutoDJ schedule id")
            if identifier in seen:
                raise StoreError("AutoDJ schedule ids must be distinct")
            seen.add(identifier)
            _preset_name(row["name"], "AutoDJ schedule name")
            days = _list(row["days"], "AutoDJ schedule days", 7)
            if not days or len(set(map(str, days))) != len(days) or any(isinstance(d, bool) or d not in range(1, 8) for d in days):
                raise StoreError("AutoDJ schedule days must be distinct ISO weekdays 1..7")
            start = _int(row["start_minute"], "AutoDJ schedule start_minute", 0, 1439)
            end = _int(row["end_minute"], "AutoDJ schedule end_minute", 1, 1440)
            if start == end:
                raise StoreError("AutoDJ schedule start and end must differ")
            if _text(row["kind"], "AutoDJ schedule kind", 20, True) not in AUTODJ_KINDS | {"all"}:
                raise StoreError("AutoDJ schedule kind must be all or a content kind")
            references(row["presets"], "AutoDJ schedule presets", True)
            if _text(row["rotation"], "AutoDJ schedule rotation", 20, True) not in TRANSITION_ROTATIONS:
                raise StoreError("AutoDJ schedule rotation must be sequential or random")


def _schema_transition(value, name):
    from .desktop_api import _int, _object
    _object(value, SCHEMA_TRANSITION_FIELDS, SCHEMA_TRANSITION_FIELDS)
    _int(value["mode"], name + " mode", 0, 3)
    _int(value["overlap_ms"], name + " overlap_ms", 0, 30000)


def _schema_choice(row, name, preset_ids):
    from .desktop_api import _id
    if "preset_id" in row and "transition" in row:
        raise StoreError(f"{name} carries either preset_id or transition, not both")
    if "preset_id" in row and _id(row["preset_id"], name + " preset_id") not in preset_ids:
        raise StoreError(f"{name} refers to an unknown transition preset of this rotation")
    if "transition" in row:
        _schema_transition(row["transition"], name + " transition")


def validate_automation_steps(value, preset_ids, name="automation steps"):
    from .desktop_api import _int, _list, _object, _text
    orders = set()
    for row in _list(value, name, 32):
        _object(row, {"order", "kind", "preset_id", "transition", "channels"}, {"order", "kind"})
        order = _int(row["order"], name + " order", 1, 99)
        if order in orders:
            raise StoreError(f"{name} orders must be distinct")
        orders.add(order)
        if _text(row["kind"], name + " kind", 20, True) not in AUTOMATION_STEP_KINDS:
            raise StoreError(f"{name} kind must be music, talk, dj, advertisement, jingle or sweeper")
        if "channels" in row:
            if row["kind"] not in {"music", "talk", "dj"}:
                raise StoreError(f"{name} mixer channels apply to music, talk or dj records")
            channels = [_int(channel, name + " channel", 1, 32)
                        for channel in _list(row["channels"], name + " channels", 32)]
            if len(channels) != len(set(channels)):
                raise StoreError(f"{name} mixer channels must be distinct")
        _schema_choice(row, name, preset_ids)


def validate_link_transitions(value, source_ids, preset_ids):
    from .desktop_api import _id, _list, _object
    pairs = set()
    for row in _list(value, "rotation link_transitions", 50):
        _object(row, {"from_source_id", "to_source_id", "preset_id", "transition"}, {"from_source_id", "to_source_id"})
        pair = (_id(row["from_source_id"], "link transition from_source_id"), _id(row["to_source_id"], "link transition to_source_id"))
        if pair[0] not in source_ids or pair[1] not in source_ids:
            raise StoreError("rotation link_transitions must name sources of this rotation")
        if pair in pairs:
            raise StoreError("rotation link_transitions pairs must be distinct")
        pairs.add(pair)
        _schema_choice(row, "rotation link transition", preset_ids)


def validate_programme_entry(value, preset_ids):
    from .desktop_api import _int, _list, _object
    _object(value, {"source_order", "preset_id", "transition", "alternatives"}, {"source_order"})
    _int(value["source_order"], "rotation programme_entry source_order", 1, 50)
    _schema_choice(value, "rotation programme_entry", preset_ids)
    total = 0
    for row in _list(value.get("alternatives", []), "rotation programme_entry alternatives", 8):
        _object(row, {"weight", "source_order", "preset_id", "transition"}, {"weight", "source_order"})
        total += _int(row["weight"], "programme_entry alternative weight", 0, 100)
        _int(row["source_order"], "programme_entry alternative source_order", 1, 50)
        _schema_choice(row, "rotation programme_entry alternative", preset_ids)
    if total > 100:
        raise StoreError("rotation programme_entry alternative weights must not exceed 100 in total")


def validate_live(value, name, *, schema_ids=None, title=False, end_rules=LIVE_END_RULES):
    """A live event step or flag. automation_id must name an automation schema
    of the same calendar when schema_ids is given; "" means none."""
    from .desktop_api import _id, _int, _list, _object, _text
    fields = LIVE_FIELDS | ({"title"} if title else set())
    _object(value, fields, LIVE_FIELDS)
    if "title" in value:
        _text(value["title"], name + " title", 160)
    if _text(value["label"], name + " label", 20, True) not in LIVE_LABELS:
        raise StoreError(f"{name} label must be dj, talk or music")
    channels = [_int(channel, name + " channel", 1, 32) for channel in _list(value["channels"], name + " channels", 32)]
    if len(set(channels)) != len(channels):
        raise StoreError(f"{name} channels must be distinct")
    automation = _text(value["automation_id"], name + " automation_id", 36)
    if automation and _id(automation, name + " automation_id") not in (schema_ids if schema_ids is not None else {_id(automation)}):
        raise StoreError(f"{name} automation_id must reference an automation schema of this calendar")
    rule = _text(value["end_rule"], name + " end_rule", 20, True)
    if rule not in end_rules:
        raise StoreError(f"{name} end_rule must be " + " or ".join(sorted(end_rules)))
    duration = _int(value["duration_ms"], name + " duration_ms", 0, 86400000)
    if rule == "duration" and not duration:
        raise StoreError(f"{name} duration end rule requires a positive duration_ms")


# excluded_kinds: AutoDJ content kinds the rotation (schema) never plays;
# rotation level only, like automation_schema. The C planner drops those rows.
AUTODJ_KINDS = frozenset({"music", "advertisement", "jingle", "sweeper"})
TRACK_RULE_FIELDS = frozenset({"entry_id", "weight", "order", "frequency", "max_plays",
                               "start_minute", "end_minute", "interval_minutes"})
ROTATION_MEDIA_KINDS = frozenset({"all", "music", "jingle", "advertisement", "sweeper"})


def validate_calendar(config, station_id, state, *, persisted=False, previous=None):
    """Validate the complete calendar without sorting or mutating its rows.

    Intervals are half open: adjacent blocks may share a boundary. Dates are
    explicit service dates in timezone (UTC when omitted), and 1440 is allowed
    only as a block end. All
    event and rotation UUIDs are distinct, so copying rows requires new IDs.
    Disabled playlist references remain readable and unchanged source rows can
    be saved again. New or changed rows require active playlists. Optional media_kind selects
    a canonical catalog kind; an omitted field preserves all playlist kinds.
    """
    from .desktop_api import _bool, _date, _id, _int, _list, _object, _reference, _text

    _object(config, {"schema", "events", "rotation_library", "timezone"}, {"schema", "events"})
    _int(config["schema"], "calendar schema", 1, 1)
    if "timezone" in config:
        zone = _text(config["timezone"], "calendar timezone", 100, True)
        try:
            # Exact-case IANA key: Windows tzdata lookups ignore case and trailing
            # dots/spaces, which would otherwise persist non-canonical keys.
            _platform.canonical_timezone(zone)
        except (ZoneInfoNotFoundError, ValueError) as exc:
            raise StoreError("calendar timezone must be a known IANA timezone") from exc
    previous = previous or {}
    previous_library = {_id(row["id"]): row for row in previous.get("rotation_library", [])}
    previous_events = {_id(row["id"]): row for row in previous.get("events", [])}
    automation_ids = {_id(row["id"]): row for row in _list(config.get("rotation_library", []), "rotation library", 200) if isinstance(row, dict)}
    from .desktop_probability_policy import validate_probability_policy
    identities = set()

    def add_identity(value, name):
        identifier = _id(value, name)
        if identifier in identities:
            raise StoreError("calendar event, assignment and rotation source identities must be distinct")
        identities.add(identifier)
        return identifier

    schema_ids = {identifier for identifier, row in automation_ids.items()
                  if isinstance(row.get("settings"), dict) and row["settings"].get("automation_schema") is True}

    def settings(value, *, override=False, previous_settings=None, preset_ids=frozenset()):
        fields = SETTINGS_FIELDS - {"selection_mode"} if override else SETTINGS_FIELDS
        optional = SETTINGS_OPTIONAL_FIELDS - {"automation_schema", "excluded_kinds", "link_transitions", "programme_entry"} if override else SETTINGS_OPTIONAL_FIELDS
        _object(value, fields | optional, set() if override else fields)
        if "automation_steps" in value:
            if not override:
                preset_ids = validate_transition_presets(value.get("autodj_presets", []))
            validate_automation_steps(value["automation_steps"], preset_ids,
                                      "playlist automation steps" if override else "rotation automation steps")
        if "link_transitions" in value or "programme_entry" in value:
            preset_ids = validate_transition_presets(value.get("autodj_presets", []))
            if "programme_entry" in value:
                validate_programme_entry(value["programme_entry"], preset_ids)
            # link_transitions also need this rotation's source ids: checked by the caller.
        if "automation_schema" in value:
            _bool(value["automation_schema"], "rotation automation_schema")
        if "excluded_kinds" in value:
            kinds = _list(value["excluded_kinds"], "rotation excluded_kinds", len(AUTODJ_KINDS))
            for kind in kinds:
                if _text(kind, "rotation excluded kind", 20, True) not in AUTODJ_KINDS:
                    raise StoreError("rotation excluded_kinds must name music, advertisement, jingle or sweeper")
            if len(set(kinds)) != len(kinds):
                raise StoreError("rotation excluded_kinds must be distinct")
        if "soft_end_seconds" in value:
            _int(value["soft_end_seconds"], "rotation soft_end_seconds", 0, 600)
        if "relax_gaps" in value:
            _bool(value["relax_gaps"], "rotation relax_gaps")
        if "probability_policy" in value:
            validate_probability_policy(value["probability_policy"], station_id, state, persisted=persisted,
                previous=(previous_settings or {}).get("probability_policy"), automation_ids=automation_ids)
        if not override and _text(value["selection_mode"], "rotation selection_mode", 20, True) not in {"linear", "weighted"}:
            raise StoreError("rotation selection_mode must be linear or weighted")
        if "transition_selection_mode" in value and _text(
                value["transition_selection_mode"], "rotation transition_selection_mode", 20, True) not in {"linear", "weighted"}:
            raise StoreError("rotation transition_selection_mode must be linear or weighted")
        for field in ("track_gap_minutes", "artist_gap_minutes", "album_gap_minutes"):
            if field in value:
                _int(value[field], "rotation " + field, 0, 10080)
        if "transitions" in value:
            # Entries without "kind" are the rotation's own distribution; entries
            # with "kind" give a content kind its percentage per transition type
            # or designed preset, and that kind's remainder resolves into the
            # rotation's distribution (contract 18.10 with the AutoDJ page).
            choices = _list(value["transitions"], "rotation transitions", 64)
            modes, total, base = set(), 0, 0
            kind_modes, kind_presets, kind_totals = set(), set(), {}

            def shape(choice, extra):
                _object(choice, {"mode", "weight", "overlap_ms", "effect"} | extra, {"mode", "weight", "overlap_ms"} | extra)
                mode = _int(choice["mode"], "rotation transition mode", 0, 3)
                overlap = _int(choice["overlap_ms"], "rotation transition overlap_ms", 0, 30000)
                effect = _text(choice.get("effect", "none"), "rotation transition effect", 20, True)
                if effect not in {"none", "lowpass"}:
                    raise StoreError("rotation transition effect must be none or lowpass")
                if mode == 0 and (overlap or effect != "none"):
                    raise StoreError("EOF rotation transition requires zero overlap_ms and no effect")
                if effect == "lowpass" and not overlap:
                    raise StoreError("lowpass rotation transition requires positive overlap_ms")
                if mode == 3 and not overlap:
                    raise StoreError("local beatmatch rotation transition requires positive overlap_ms")
                return mode

            for choice in choices:
                if not isinstance(choice, dict):
                    raise StoreError("rotation transition must be an object")
                if "kind" not in choice:
                    if "preset_id" in choice:
                        raise StoreError("rotation transition presets belong to a content kind")
                    mode = shape(choice, set())
                    if mode in modes:
                        raise StoreError("rotation transition modes must be distinct")
                    modes.add(mode)
                    base += 1
                    total += _int(choice["weight"], "rotation transition weight", 0, 100)
                    continue
                kind = _text(choice["kind"], "rotation transition kind", 20, True)
                if kind not in AUTODJ_KINDS:
                    raise StoreError("rotation transition kind must be music, advertisement, jingle or sweeper")
                weight = _int(choice.get("weight"), "rotation transition kind weight", 1, 100)
                kind_totals[kind] = kind_totals.get(kind, 0) + weight
                if "preset_id" in choice:
                    if override:
                        raise StoreError("transition presets belong to the rotation, not a playlist override")
                    _object(choice, {"kind", "preset_id", "mode", "overlap_ms", "weight"}, {"kind", "preset_id", "mode", "overlap_ms", "weight"})
                    preset_id = choice["preset_id"]
                    if (_int(choice["mode"], "rotation transition preset mode", 1, 1) != 1 or
                            _int(choice["overlap_ms"], "rotation transition preset overlap_ms", 0, 0) != 0):
                        raise StoreError("rotation transition preset entries use mode 1 and overlap_ms 0")
                    known = {str(row.get("id", "")).lower(): row for row in value.get("autodj_presets", []) if isinstance(row, dict)}
                    if not isinstance(preset_id, str) or preset_id.lower() not in known:
                        raise StoreError("rotation transition preset_id must name a preset of this rotation")
                    if not _preset_kind_usable(value.get("autodj_automation"), preset_id, kind):
                        raise StoreError("rotation transition preset is assigned to another content kind")
                    if (kind, preset_id.lower()) in kind_presets:
                        raise StoreError("rotation transition presets must be distinct per content kind")
                    kind_presets.add((kind, preset_id.lower()))
                else:
                    mode = shape(choice, {"kind"})
                    if (kind, mode) in kind_modes:
                        raise StoreError("rotation transition modes must be distinct per content kind")
                    kind_modes.add((kind, mode))
            if any(share > 100 for share in kind_totals.values()):
                raise StoreError("rotation transition shares of one content kind must not exceed 100")
            if not base or base > 4 or not total:
                raise StoreError("rotation transitions require at least one positive weight")
        if "kind_transitions" in value:
            _object(value["kind_transitions"], AUTODJ_KINDS, AUTODJ_KINDS)
            for kind in AUTODJ_KINDS:
                choice = value["kind_transitions"][kind]
                _object(choice, {"mode", "overlap_ms", "sync_percent"}, {"mode", "overlap_ms"})
                mode = _int(choice["mode"], "AutoDJ " + kind + " transition mode", 0, 3)
                overlap = _int(choice["overlap_ms"], "AutoDJ " + kind + " overlap_ms", 0, 30000)
                # SYNC beatmatch share of this kind's transitions; the rest crossfade.
                if "sync_percent" in choice:
                    _int(choice["sync_percent"], "AutoDJ " + kind + " sync_percent", 0, 100)
                    if mode != 3:
                        raise StoreError("AutoDJ sync_percent requires the SYNC beatmatch transition")
                if mode == 0 and overlap:
                    raise StoreError("EOF AutoDJ transition requires zero overlap_ms")
                if mode == 3 and not overlap:
                    raise StoreError("beatmatch AutoDJ transition requires positive overlap_ms")
        if "autodj_players" in value:
            autodj(value["autodj_players"])
        if "autodj_presets" in value:
            if override:
                raise StoreError("transition presets belong to the rotation, not a playlist override")
            validate_transition_presets(value["autodj_presets"])
        if "autodj_automation" in value:
            if override:
                raise StoreError("AutoDJ automation belongs to the rotation, not a playlist override")
            validate_transition_automation(value["autodj_automation"],
                validate_transition_presets(value.get("autodj_presets", [])))

    def autodj(value):
        _object(value, AUTODJ_KINDS, AUTODJ_KINDS)
        for kind in AUTODJ_KINDS:
            players = _list(value[kind], "AutoDJ " + kind + " players", 32)
            checked = [_int(player, "AutoDJ player id", 1, 32) for player in players]
            if not checked or len(checked) != len(set(checked)):
                raise StoreError("AutoDJ player pools must be nonempty with distinct IDs 1..32")

    def track_rules(rotation, *, unchanged=False):
        if "track_selection_mode" in rotation:
            mode = _text(rotation["track_selection_mode"], "track selection mode", 20, True)
            if mode not in {"linear", "weighted"}:
                raise StoreError("track selection mode must be linear or weighted")
        rows = _list(rotation.get("track_rules", []), "track rules", 500)
        seen = set()
        playlist_id = _id(rotation["playlist_id"], "track rules playlist_id")
        playlist = state.get("playlists", {}).get(playlist_id, {}).get("data")
        for rule in rows:
            _object(rule, TRACK_RULE_FIELDS, TRACK_RULE_FIELDS)
            entry_id = _id(rule["entry_id"], "track rule entry_id")
            if entry_id in seen:
                raise StoreError("track rules require distinct playlist entry identities")
            seen.add(entry_id)
            entry = state.get("desktop_playlist_entries", {}).get(entry_id, {}).get("data")
            version = state.get("playlist_versions", {}).get(entry.get("playlist_version_id") if entry else None, {}).get("data")
            if not entry or entry["station_id"] != station_id or not version or version["playlist_id"] != playlist_id:
                raise StoreError("track rule must reference an entry in this playlist and station")
            if not (persisted or unchanged) and (entry["status"] != "active" or not playlist or
                                  entry["playlist_version_id"] != playlist.get("draft_version_id")):
                raise StoreError("track rule requires an active entry in the current playlist draft")
            _int(rule["weight"], "track rule weight", 0, 100)
            _int(rule["order"], "track rule order", 0, 4999)
            _int(rule["frequency"], "track rule frequency", 1, 100)
            _int(rule["max_plays"], "track rule max_plays", 0, 200)
            begin = _int(rule["start_minute"], "track rule start_minute", 0, 1439)
            end = _int(rule["end_minute"], "track rule end_minute", 1, 1440)
            if begin >= end:
                raise StoreError("track rule end_minute must follow its start_minute")
            _int(rule["interval_minutes"], "track rule interval_minutes", 0, 10080)

    def source(rotation, *, named=False, previous_rows=None, preset_ids=frozenset()):
        optional = SOURCE_OPTIONAL_FIELDS if named else ROTATION_OPTIONAL_FIELDS
        live = named and isinstance(rotation, dict) and rotation.get("source_type") == "live"
        # A live step has no playlist; everything else of the row stays required.
        _object(rotation, ROTATION_FIELDS | optional, ROTATION_FIELDS - {"playlist_id"} if live else ROTATION_FIELDS)
        identifier = add_identity(rotation["id"], "calendar rotation source id")
        unchanged = rotation == (previous_rows or {}).get(identifier)
        if named:
            if _text(rotation.get("source_type", "playlist"), "rotation source_type", 20, True) not in SOURCE_TYPES:
                raise StoreError("rotation source_type must be playlist or live")
            if live:
                if "playlist_id" in rotation or "track_rules" in rotation or "track_selection_mode" in rotation or "live" not in rotation:
                    raise StoreError("a live rotation source carries a live object and no playlist")
                validate_live(rotation["live"], "rotation source live", schema_ids=schema_ids, title=True)
            elif "live" in rotation:
                raise StoreError("live belongs to a rotation source of source_type live")
            for field in ("start_date", "end_date"):
                if field in rotation:
                    _date(rotation[field], "rotation source " + field)
            if "start_date" in rotation and "end_date" in rotation and rotation["start_date"] > rotation["end_date"]:
                raise StoreError("rotation source end_date must not precede its start_date")
        if not live:
            _reference(state, station_id, rotation["playlist_id"], "playlists",
                       "calendar playlist_id (rotation " + identifier + ")",
                       active=not (persisted or unchanged))
        _int(rotation["frequency"], "calendar rotation frequency", 1, 100)
        media_kind = _text(rotation.get("media_kind", "all"), "calendar rotation media_kind", 20, True)
        if media_kind not in ROTATION_MEDIA_KINDS:
            raise StoreError("calendar rotation media_kind is unsupported")
        for field in ("random", "track_random", "advert_random", "sweeper_random"):
            _bool(rotation[field], "calendar rotation " + field)
        if "autodj" in rotation:
            autodj(rotation["autodj"])
        if named:
            for field in ("weight", "count"):
                if field in rotation:
                    _int(rotation[field], "rotation source " + field, 0 if field == "weight" else 1, 100)
            if not live:
                track_rules(rotation, unchanged=unchanged)
            begin = _int(rotation.get("start_minute", 0), "rotation source start_minute", 0, 1439)
            end = _int(rotation.get("end_minute", 1440), "rotation source end_minute", 1, 1440)
            if begin >= end:
                raise StoreError("rotation source end_minute must follow its start_minute")
            if "interval_minutes" in rotation:
                _int(rotation["interval_minutes"], "rotation source interval_minutes", 0, 10080)
            if "settings" in rotation:
                settings(rotation["settings"], override=True, preset_ids=preset_ids,
                         previous_settings=((previous_rows or {}).get(identifier) or {}).get("settings"))

    library = {}
    for definition in _list(config.get("rotation_library", []), "rotation library", 200):
        _object(definition, {"id", "name", "sources", "settings"}, {"id", "name", "sources", "settings"})
        identifier = add_identity(definition["id"], "rotation definition id")
        _text(definition["name"], "rotation definition name", 160, True)
        settings(definition["settings"], previous_settings=previous_library.get(identifier, {}).get("settings"))
        preset_ids = validate_transition_presets(definition["settings"].get("autodj_presets", []))
        rows = _list(definition["sources"], "rotation definition sources", 50)
        previous_rows = {_id(row["id"]): row for row in
                         previous_library.get(identifier, {}).get("sources", [])}
        for row in rows:
            source(row, named=True, previous_rows=previous_rows, preset_ids=preset_ids)
        if "link_transitions" in definition["settings"]:
            validate_link_transitions(definition["settings"]["link_transitions"],
                                      {_id(row["id"]) for row in rows}, preset_ids)
        library[identifier] = definition
    intervals = {}
    for event in _list(config["events"], "calendar events", 500):
        _object(event, EVENT_FIELDS | {"rotation_selection_mode", "probability_policy", "live"}, EVENT_FIELDS)
        if "live" in event:
            validate_live(event["live"], "calendar event live", schema_ids=schema_ids)
        if "probability_policy" in event:
            validate_probability_policy(event["probability_policy"], station_id, state, persisted=persisted,
                previous=previous_events.get(_id(event.get("id")), {}).get("probability_policy"), automation_ids=automation_ids)
        if "rotation_selection_mode" in event and _text(event["rotation_selection_mode"], "event rotation selection mode", 20, True) not in {"linear", "weighted"}:
            raise StoreError("event rotation selection mode must be linear or weighted")
        identifier = _id(event["id"], "calendar event id")
        if identifier in identities:
            raise StoreError("calendar event and rotation identities must be distinct")
        identities.add(identifier)
        _text(event["title"], "calendar event title", 160, True)
        day = _date(event["date"], "calendar event date")
        start = _int(event["start_minute"], "calendar start minute", 0, 1439)
        end = _int(event["end_minute"], "calendar end minute", 1, 1440)
        if start >= end:
            raise StoreError("calendar event must end after its start on the same day")
        _text(event["color"], "calendar event color", 7, True)
        if not re.fullmatch(r"#[0-9a-fA-F]{6}", event["color"]):
            raise StoreError("calendar event color must be #RRGGBB")
        intervals.setdefault(day, []).append((start, end))
        expanded = 0
        previous_rows = {_id(row["id"]): row for row in
                         previous_events.get(identifier, {}).get("rotations", [])}
        for rotation in _list(event["rotations"], "calendar rotations", 50):
            if isinstance(rotation, dict) and "rotation_id" in rotation:
                _object(rotation, {"id", "rotation_id", "weight"}, {"id", "rotation_id"})
                if "weight" in rotation:
                    _int(rotation["weight"], "calendar rotation assignment weight", 0, 100)
                assignment = add_identity(rotation["id"], "calendar rotation assignment id")
                reference = _id(rotation["rotation_id"], "calendar assigned rotation_id")
                if reference not in library:
                    raise StoreError("calendar rotation_id must reference this calendar's rotation library")
                if not persisted and rotation != previous_rows.get(assignment):
                    for row in library[reference]["sources"]:
                        if row.get("source_type") == "live":
                            continue
                        _reference(state, station_id, row["playlist_id"], "playlists",
                                   "calendar assigned playlist_id (event " + identifier + ")")
                expanded += len(library[reference]["sources"])
            else:
                source(rotation, previous_rows=previous_rows)
                expanded += 1
        if expanded > 50:
            raise StoreError("calendar event expands to more than 50 playlist sources")
    for day, ranges in intervals.items():
        ranges.sort()
        for previous, current in zip(ranges, ranges[1:]):
            if current[0] < previous[1]:
                raise StoreError(f"calendar events overlap on {day}")
