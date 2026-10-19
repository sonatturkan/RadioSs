"""Typed, station-scoped native desktop configuration and draft traffic CRUD.

Configuration records describe operator preferences and preparation. They do
not attest to hardware, media verification, a running engine or a broadcast.
"""

from __future__ import annotations

from copy import deepcopy
from datetime import date, datetime, timedelta, timezone
import errno
import hashlib
import json
import math
import os
from pathlib import Path
import re
import secrets
from functools import wraps
from urllib.parse import urlsplit
from uuid import UUID, uuid4
from zoneinfo import ZoneInfo

from . import _platform
from .catalog import NotFound
from .identity import Forbidden, Principal, require
from .storage import Conflict, Store, StoreError


KINDS = frozenset({"programme", "channel", "sampler", "emergency", "transition", "markers",
                   "server", "web", "automation", "automation_calendar", "weekly_programme", "local_event", "advert_break", "audio_preferences", "audio_routing", "dj_mixer", "playout_history",
                   "advert_local_run", "advert_report", "advert_notifications",
                   "broadcast_stream", "broadcast_record", "broadcast_metadata", "broadcast_local_run", "admin_recovery", "admin_restore_fence",
                   "midi_mapping", "playout_policy",
                   "desktop_preferences", "live_players", "copyright_policy"})
MANAGED = frozenset({"server", "web", "audio_preferences", "audio_routing", "broadcast_stream", "broadcast_record", "broadcast_metadata",
                     "midi_mapping"})
# Spotify, Apple Music and YouTube Music support is retired: only local media
# plays. Documents of these kinds that an existing database already holds stay
# readable (they replay and pass the integrity check) but no kind below can be
# listed, saved or changed any more.
LEGACY_KINDS = frozenset({"online_services", "external_playlist"})
# MIDI controller maps: a control key ("note:CH:N", "cc:CH:N", "pitch:CH") runs
# one application action. Deck actions name a player 1..32.
_MIDI_CONTROL = re.compile(r"(?:(?:note|cc):(?:[1-9]|1[0-6]):(?:\d|[1-9]\d|1[01]\d|12[0-7])|pitch:(?:[1-9]|1[0-6]))\Z")
_MIDI_ACTION = re.compile(r"(?:deck(?:[1-9]|[12]\d|3[0-2])\.(?:play|pause|playpause|stop|next|cue|sync|volume)|master\.volume|microphone\.toggle)\Z")


def _validate_midi_mapping(config):
    _object(config, {"device", "active", "maps"}, {"device", "maps"})
    _text(config["device"], "MIDI device", 192)
    names = set()
    for item in _list(config["maps"], "MIDI maps", 32):
        _object(item, {"name", "device", "bindings"}, {"name", "device", "bindings"})
        name = _text(item["name"], "MIDI map name", 80, True).strip()
        if not name or name in names:
            raise StoreError("MIDI map names must be non-empty and unique")
        names.add(name)
        _text(item["device"], "MIDI map device", 192)
        controls = set()
        for binding in _list(item["bindings"], "MIDI bindings", 512):
            _object(binding, {"control", "action"}, {"control", "action"})
            control, action = binding["control"], binding["action"]
            if not isinstance(control, str) or not _MIDI_CONTROL.match(control) or control in controls:
                raise StoreError("MIDI bindings need distinct valid control keys")
            if not isinstance(action, str) or not _MIDI_ACTION.match(action):
                raise StoreError("unsupported MIDI action")
            controls.add(control)
    if "active" in config and config["active"] not in names:
        raise StoreError("active MIDI map must name a stored map")

# Workstation preferences mirrored from config/ui-preferences.ini and
# config/audio-panel.ini, so they survive a reinstall and a reset clears them.
def _validate_desktop_preferences(config):
    _object(config, {"schema_version", "ui", "audio_panel", "views", "active_view"}, {"schema_version"})
    _int(config["schema_version"], "schema_version", 1, 1)
    scale_keys = {"content_scale_percent", "mixer_scale_percent", "effector_scale_percent",
                  "mixer_height_percent", "effector_height_percent"}

    def preferences(value):
        ui = _object(value, {"text_size", "default_profile", "header_hidden_fields"} | scale_keys,
                     {"text_size", "default_profile", "header_hidden_fields"})
        _int(ui["text_size"], "text_size", 0, 2)
        _int(ui["default_profile"], "default_profile", 0, 8)
        _int(ui["header_hidden_fields"], "header_hidden_fields", 0, 7)
        for key in scale_keys:
            if key in ui:
                _int(ui[key], key, 50, 150)

    if "ui" in config:
        preferences(config["ui"])
    # Named views: complete ui sets under unique names of 1..40 characters
    # without surrounding (ASCII) space or control characters; active_view
    # is null or one of the names.
    names = set()
    for view in _list(config.get("views", []), "views", 16):
        _object(view, {"name", "ui"}, {"name", "ui"})
        name = _text(view["name"], "view name", 40, required=True)
        if name != name.strip(" \t\n\r\f\v") or any(ord(c) < 32 or 127 <= ord(c) < 160 for c in name):
            raise StoreError("view name must not have surrounding space or control characters")
        if name in names:
            raise StoreError("view names must be unique")
        names.add(name)
        preferences(view["ui"])
    active = config.get("active_view")
    if active is not None and (not isinstance(active, str) or active not in names):
        raise StoreError("active_view must be null or the name of a saved view")
    if "audio_panel" in config:
        keys = {"sample_rate", "buffer_frames", "click_sound", "click_volume"}
        panel = _object(config["audio_panel"], keys, keys)
        if type(panel["sample_rate"]) is not int or panel["sample_rate"] not in {44100, 48000}:
            raise StoreError("sample_rate must be 44100 or 48000")
        frames = _int(panel["buffer_frames"], "buffer_frames", 64, 4096)
        if frames & (frames - 1):
            raise StoreError("buffer_frames must be a power of two")
        _int(panel["click_sound"], "click_sound", 0, 2)
        _int(panel["click_volume"], "click_volume", 0, 2)


def _consistent_read(action):
    @wraps(action)
    def run(store, *args, **kwargs):
        with store._locked(exclusive=False):
            store._reload_locked()
            return action(store, *args, **kwargs)
    return run


def _id(value, name="id"):
    if not isinstance(value, str):
        raise StoreError(f"{name} must be a UUID")
    try:
        return str(UUID(value))
    except ValueError as exc:
        raise StoreError(f"{name} must be a UUID") from exc


def _text(value, name, limit=200, required=False):
    if not isinstance(value, str) or len(value) > limit or "\0" in value or (required and not value.strip()):
        raise StoreError(f"{name} must be text of at most {limit} characters")
    return value


def _int(value, name, low, high):
    if type(value) is not int or not low <= value <= high:
        raise StoreError(f"{name} must be an integer {low}..{high}")
    return value


def _number(value, name, low, high):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not low <= value <= high or not math.isfinite(value):
        raise StoreError(f"{name} must be a finite number {low}..{high}")
    return value


def _bool(value, name):
    if type(value) is not bool:
        raise StoreError(f"{name} must be boolean")
    return value


def _object(value, allowed, required=()):
    if not isinstance(value, dict) or set(value) - set(allowed) or set(required) - set(value):
        raise StoreError("configuration has missing or unsupported fields")
    return value


def _list(value, name, limit):
    if not isinstance(value, list) or len(value) > limit:
        raise StoreError(f"{name} must be a list with at most {limit} entries")
    return value


def _time(value, name, optional=False):
    _text(value, name, 5)
    if not (optional and value == "") and not re.fullmatch(r"(?:[01]\d|2[0-3]):[0-5]\d", value):
        raise StoreError(f"{name} must be HH:MM")
    return value


def _date(value, name):
    _text(value, name, 10, True)
    try:
        if date.fromisoformat(value).isoformat() != value:
            raise ValueError
    except ValueError as exc:
        raise StoreError(f"{name} must be YYYY-MM-DD") from exc
    return value


def _url(value, name, https=False, optional=False):
    _text(value, name, 500, not optional)
    if optional and not value:
        return value
    try:
        parsed = urlsplit(value)
        if (parsed.scheme not in ({"https"} if https else {"http", "https"}) or
                not parsed.hostname or parsed.username is not None or parsed.password is not None or
                any(char.isspace() for char in value) or (parsed.port is not None and not 1 <= parsed.port <= 65535)):
            raise ValueError
    except ValueError as exc:
        raise StoreError(f"{name} must be an HTTP(S) URL without credentials") from exc
    return value


def _reference(state, station_id, value, collection, name, optional=False, active=True):
    if optional and value is None:
        return
    key = _id(value, name)
    row = state.get(collection, {}).get(key)
    if not row or row["data"].get("station_id") != station_id or (active and row["data"].get("status") == "disabled"):
        raise StoreError(f"{name} must reference an active record in this station")


MEDIA_BINDING_KEYS = frozenset({"media_id", "version_id", "media_file_id", "media_sha256"})
# Retired: a programme event that streamed one online recording. Stored events
# keep these members and still replay; new ones are rejected.
ONLINE_EVENT_KEYS = frozenset({"online_provider", "online_id", "online_url"})
PROGRAMME_KINDS = frozenset({"music", "announcement", "advertisement", "jingle", "program",
                             "sweeper", "news", "effect", "voice_track", "other", "live"})
# Optional "Eventler": events stay the flat playable sequence, and each one names
# the block it is shown in. A block may be empty; its events are contiguous.
PROGRAMME_BLOCK_KEYS = frozenset({"id", "kind", "title", "start_time", "note"})


def _media_binding(binding, station_id, state, *, persisted=False):
    """A saved identity is not playback authorization or a trusted file path.

    New bindings must reference verified media in this station. Existing
    bindings remain readable if that media is subsequently disabled or loses
    verification; the native loader rechecks availability and frozen bytes.
    """
    _object(binding, MEDIA_BINDING_KEYS, MEDIA_BINDING_KEYS)
    media_id = _id(binding["media_id"], "media_id")
    version_id = _id(binding["version_id"], "version_id")
    file_id = _id(binding["media_file_id"], "media_file_id")
    digest = binding["media_sha256"]
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise StoreError("media_sha256 must be a lowercase SHA-256 digest")
    library = state.get("desktop_media_library", {}).get(media_id, {}).get("data")
    version = state.get("versions", {}).get(version_id, {}).get("data")
    media = state.get("media_files", {}).get(file_id, {}).get("data")
    if (not library or not version or not media or library.get("station_id") != station_id or
            library.get("version_id") != version_id or library.get("media_file_id") != file_id or
            version.get("item_id") != library.get("item_id") or media.get("version_id") != version_id or
            media.get("sha256") != digest or not any(
                row["data"].get("station_id") == station_id and
                row["data"].get("item_id") == library.get("item_id")
                for row in state.get("station_items", {}).values())):
        raise StoreError("media binding must reference one catalog version in this station")
    if not persisted and (library.get("status") != "active" or media.get("status") != "verified"):
        raise StoreError("new media binding requires active verified media")


def _dj_effect(effect):
    keys = {"type", "enabled", "depth", "send_db", "return_db", "monitor_path", "tempo_bpm"}
    _object(effect, keys, keys)
    _int(effect["type"], "effect type", 0, 13)
    _bool(effect["enabled"], "effect enabled")
    _number(effect["depth"], "effect depth", 0, 1)
    for key in ("send_db", "return_db"):
        _number(effect[key], key, -60, 6)
    _int(effect["monitor_path"], "monitor_path", 0, 2)
    _number(effect["tempo_bpm"], "tempo_bpm", 30, 300)


def _dj_input_text(value, name, limit):
    # The native mixer stores these as NUL-free UTF-8 of 1..limit bytes.
    try:
        size = len(value.encode("utf-8")) if isinstance(value, str) and "\0" not in value else 0
    except UnicodeEncodeError:
        size = 0
    if not 1 <= size <= limit:
        raise StoreError(f"{name} must be UTF-8 text of 1..{limit} bytes")
    return value


def _dj_input_url(value):
    # Same rule as radios_audio_channel_input_spec_validate: a whitelisted
    # scheme followed by an address, no ASCII space or control character.
    _dj_input_text(value, "network source url", 512)
    schemes = ("http://", "https://", "rtp://", "udp://", "rtsp://", "srt://")
    if (not any(value[:len(scheme)].lower() == scheme and len(value) > len(scheme) for scheme in schemes) or
            any(char in " \t\n\v\f\r" or ord(char) < 32 or ord(char) == 127 for char in value)):
        raise StoreError("network source url must use http, https, rtp, udp, rtsp or srt")
    return value


def _validate_dj_mixer(config):
    keys = {"channels", "master", "effects", "master_fx", "crossfader", "speech"}
    _object(config, keys | {"network_sources"}, keys)
    # Saved network sources offered by every channel's source menu (R5.1).
    for source in _list(config.get("network_sources", []), "network_sources", 32):
        _object(source, {"name", "url"}, {"name", "url"})
        _dj_input_text(source["name"], "network source name", 128)
        _dj_input_url(source["url"])
    channels = _list(config["channels"], "channels", 32)
    if len(channels) != 32:
        raise StoreError("DJ mixer requires 32 fixed channel identities")
    channel_keys = {"source_deck", "eq_low_db", "eq_mid_db", "eq_high_db", "pan", "gain_db",
                    "mute", "solo", "crossfade_group", "fx_slot", "fx_send_db", "fx_return_db"}
    route_keys = {"send_channel", "return_channel", "return_effect", "send_db", "return_db"}
    for index, channel in enumerate(channels):
        _object(channel, channel_keys | route_keys | {"loudness_bypass", "label", "input"}, channel_keys)
        _int(channel["source_deck"], "source_deck", -2, 31)
        # Broadcast role of the strip (R5.2) and its live input (R5.1).
        if "label" in channel and channel["label"] not in ("dj", "talk", "music"):
            raise StoreError("DJ channel label must be dj, talk or music")
        if "input" in channel:
            source = _object(channel["input"], {"kind", "device", "name"}, {"kind", "device", "name"})
            if source["kind"] not in ("capture", "network"):
                raise StoreError("DJ channel input kind must be capture or network")
            _dj_input_text(source["device"], "input device", 512)
            _dj_input_text(source["name"], "input name", 128)
            if source["kind"] == "network":
                _dj_input_url(source["device"])
            # A live input replaces the player; the two never add up.
            if channel["source_deck"] != -1:
                raise StoreError("DJ channel with a live input must have source_deck -1")
        for key in ("eq_low_db", "eq_mid_db", "eq_high_db"):
            _number(channel[key], key, -26, 6)
        _number(channel["pan"], "pan", -1, 1)
        for key in ("gain_db", "fx_send_db", "fx_return_db"):
            _number(channel[key], key, -60, 6)
        for key in ("mute", "solo"):
            _bool(channel[key], key)
        if "loudness_bypass" in channel:
            _bool(channel["loudness_bypass"], "loudness_bypass")
        _int(channel["crossfade_group"], "crossfade_group", 0, 2)
        _int(channel["fx_slot"], "fx_slot", -1, 4)
        _int(channel.get("return_effect", -1), "return_effect", -1, 4)
        for key in ("send_channel", "return_channel"):
            selected = channel.get(key, -1)
            _int(selected, key, -1, 31)
            if selected == index:
                raise StoreError("DJ channel cannot route to itself")
        for key in ("send_db", "return_db"):
            if key in channel:
                _number(channel[key], key, -60, 6)
    edges = {(source, target) for source in range(32) for target in range(32)
             if channels[source].get("send_channel", -1) == target
             or channels[target].get("return_channel", -1) == source}
    remaining = set(range(32))
    while remaining:
        ready = {target for target in remaining if not any(source in remaining and destination == target
                 for source, destination in edges)}
        if not ready:
            raise StoreError("DJ send/return routing cannot contain feedback cycles")
        remaining -= ready
    master_keys = {"eq_low_db", "eq_mid_db", "eq_high_db", "mute", "fx_slot", "fx_send_db", "fx_return_db"}
    master = _object(config["master"], master_keys | route_keys, master_keys)
    for key in ("eq_low_db", "eq_mid_db", "eq_high_db"):
        _number(master[key], key, -26, 6)
    _bool(master["mute"], "master mute")
    _int(master["fx_slot"], "master fx_slot", -1, 4)
    _int(master.get("return_effect", -1), "master return_effect", -1, 4)
    for key in ("fx_send_db", "fx_return_db"):
        _number(master[key], key, -60, 6)
    for key in ("send_channel", "return_channel"):
        _int(master.get(key, -1), "master " + key, -1, 31)
    for key in ("send_db", "return_db"):
        if key in master:
            _number(master[key], "master " + key, -60, 6)
    effects = _list(config["effects"], "effects", 5)
    if len(effects) != 5:
        raise StoreError("DJ mixer requires five fixed effect slots")
    for effect in effects:
        _dj_effect(effect)
    _dj_effect(config["master_fx"])
    _number(config["crossfader"], "crossfader", -1, 1)
    speech = _object(config["speech"], {"ducking", "bed_db", "fx_slot", "to_master", "channel"},
                     {"ducking", "bed_db"})
    _bool(speech["ducking"], "ducking")
    _number(speech["bed_db"], "bed_db", -36, 0)
    # Legacy records retain their original microphone path. These defaults are
    # validated without inserting new keys or changing the caller's saved wire.
    _bool(speech.get("to_master", True), "speech to_master")
    _int(speech.get("channel", -1), "speech channel", -1, 31)
    # Older mixers omitted this preference and keep the microphone effect off.
    # The native DJ exposes four pedals; its retired fifth slot is not selectable.
    _int(speech.get("fx_slot", -1), "speech fx_slot", -1, 3)


ROUTING_KINDS = ("mixer", "input", "microphone", "virtual", "main", "output", "recorder", "stream", "meter")
_ROUTING_IN = frozenset({"mixer", "virtual", "main", "output", "recorder", "stream", "meter"})
_ROUTING_OUT = frozenset({"mixer", "input", "microphone", "virtual", "main", "meter"})
_ROUTING_SETTINGS = {
    "mixer": {"channel"}, "input": {"device"}, "output": {"device"}, "microphone": {"device", "push_to_talk"},
    "virtual": {"as_microphone"}, "main": set(), "meter": set(), "recorder": {"folder", "prefix", "split_minutes"},
    "stream": {"protocol", "host", "port", "mount", "stream_id", "username", "bitrate_kbps", "public",
               "station", "genre", "description", "website"}}
_ROUTING_SETTINGS_OPTIONAL = {"stream": {"metadata_source", "metadata_player", "metadata_format", "metadata_manual",
                                         "password_saved", "password"}}
_ROUTING_LIMITS = {"input": 8, "microphone": 8, "output": 8, "virtual": 8, "recorder": 8, "stream": 4}


def _routing_text(value, name, limit):
    _text(value, name, limit)
    if len(value.encode("utf-8")) > limit or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise StoreError(f"{name} must be at most {limit} UTF-8 bytes without control characters")
    return value


def _validate_routing_patchbay(graph, *, persisted=False):
    """Schema 2 patchbay: multi-instance blocks; mirrors routing_graph.c.

    A recorder folder is a native absolute Windows folder (radios_path_is_absolute).
    Documents already stored by a Linux installation keep their POSIX folder text
    readable (the recorder treats it as missing); new saves need a Windows folder."""
    _object(graph, {"nodes", "edges"}, {"nodes", "edges"})
    nodes, counts = {}, {}
    for node in _list(graph["nodes"], "routing blocks", 48):
        _object(node, {"id", "kind", "name", "x", "y", "gain_db", "settings"},
                {"id", "kind", "name", "x", "y", "gain_db", "settings"})
        ident = _int(node["id"], "routing block id", 1, 65535)
        kind = node["kind"]
        if kind not in ROUTING_KINDS or ident in nodes:
            raise StoreError("routing blocks need distinct ids and a supported kind")
        _routing_text(node["name"], "routing block name", 160)
        _number(node["x"], "routing block x", 0, 3000)
        _number(node["y"], "routing block y", 0, 2000)
        _number(node["gain_db"], "routing block gain", -60, 12)
        settings = _object(node["settings"], _ROUTING_SETTINGS[kind] | _ROUTING_SETTINGS_OPTIONAL.get(kind, set()),
                           _ROUTING_SETTINGS[kind])
        if kind == "mixer":
            _int(settings["channel"], "mixer channel", 1, 32)
        elif kind in {"input", "output", "microphone"}:
            _routing_text(settings["device"], "routing device", 512)
            if kind != "microphone" and settings["device"] == "@talk":
                raise StoreError("the talk microphone belongs to microphone blocks")
            if kind == "microphone":
                _bool(settings["push_to_talk"], "push_to_talk")
        elif kind == "virtual":
            _bool(settings["as_microphone"], "as_microphone")
        elif kind == "recorder":
            folder = _routing_text(settings["folder"], "recorder folder", 512)
            if folder and not (_platform.is_absolute(folder) or
                               (persisted and _platform.stored_path_flavour(folder) == "legacy-posix")):
                raise StoreError("recorder folder must be empty or absolute")
            prefix = _routing_text(settings["prefix"], "recorder prefix", 80)
            if not prefix or prefix.startswith(".") or "/" in prefix or "\\" in prefix or \
                    (not persisted and not _platform.component_valid(prefix)):
                raise StoreError("recorder prefix must be one visible file name part")
            _int(settings["split_minutes"], "split_minutes", 0, 1440)
        elif kind == "stream":
            if settings["protocol"] not in {"icecast", "shoutcast1", "shoutcast2"}:
                raise StoreError("stream protocol must be icecast, shoutcast1 or shoutcast2")
            host = _routing_text(settings["host"], "stream host", 255)
            if any(c in host for c in " /\\?#@"):
                raise StoreError("stream host must be a host name or address")
            _int(settings["port"], "stream port", 1, 65535)
            mount = _routing_text(settings["mount"], "stream mount", 255)
            if mount and (not mount.startswith("/") or any(c in mount for c in " ?#\\")):
                raise StoreError("stream mount must start with / and contain no spaces, ? or #")
            _int(settings["stream_id"], "stream_id", 1, 999)
            if ":" in _routing_text(settings["username"], "stream username", 128):
                raise StoreError("stream username cannot contain a colon")
            if type(settings["bitrate_kbps"]) is not int or settings["bitrate_kbps"] not in {64, 96, 128, 160, 192, 256, 320}:
                raise StoreError("stream bitrate must be one of 64, 96, 128, 160, 192, 256, 320 kbps")
            _bool(settings["public"], "public")
            _routing_text(settings["station"], "station", 160)
            _routing_text(settings["genre"], "genre", 160)
            _routing_text(settings["description"], "description", 256)
            _routing_text(settings["website"], "website", 256)
            # A stored stream password is an explicit operator choice: the flag
            # and the value travel together (mirrors routing_graph.c).
            if "password_saved" in settings:
                _bool(settings["password_saved"], "password_saved")
            if ("password" in settings) != bool(settings.get("password_saved", False)):
                raise StoreError("stream password is stored only together with password_saved=true")
            if "password" in settings:
                _routing_text(settings["password"], "stream password", 1024)
            if "metadata_source" in settings:
                source = _routing_text(settings["metadata_source"], "metadata_source", 16)
                if source not in {"manual", "player", "automation"}:
                    raise StoreError("metadata_source must be manual, player or automation")
            if "metadata_player" in settings:
                _int(settings["metadata_player"], "metadata_player", 0, 31)
            if "metadata_format" in settings:
                _routing_text(settings["metadata_format"], "metadata_format", 255)
            if "metadata_manual" in settings:
                _routing_text(settings["metadata_manual"], "metadata_manual", 1024)
        nodes[ident] = node
        counts[kind] = counts.get(kind, 0) + 1
        if counts[kind] > _ROUTING_LIMITS.get(kind, 48):
            raise StoreError(f"too many {kind} routing blocks")
    edges = set()
    for edge in _list(graph["edges"], "routing cables", 128):
        _object(edge, {"from", "to"}, {"from", "to"})
        source = _int(edge["from"], "routing cable source", 1, 65535)
        target = _int(edge["to"], "routing cable target", 1, 65535)
        if source not in nodes or target not in nodes or source == target or (source, target) in edges or \
                nodes[source]["kind"] not in _ROUTING_OUT or nodes[target]["kind"] not in _ROUTING_IN:
            raise StoreError("routing cables must join an output to an input of existing blocks exactly once")
        edges.add((source, target))

    def implicit(a, b):
        # a's input reaches b's output through the mixer or a monitored device.
        if a is b:
            return False
        if a["kind"] in {"mixer", "main"} and b["kind"] == "main":
            return True
        if a["kind"] == "mixer" and b["kind"] == "mixer" and \
                a["settings"]["channel"] == b["settings"]["channel"]:
            return True
        if a["kind"] == "virtual":
            sink = f"radios_virtual_{a['id']}"
        elif a["kind"] == "output" and a["settings"]["device"]:
            sink = a["settings"]["device"]
            if b["kind"] == "virtual" and sink == f"radios_virtual_{b['id']}":
                return True
        else:
            return False
        if b["kind"] not in {"input", "microphone"}:
            return False
        device = b["settings"]["device"]
        return device == sink + ".monitor" or (a["kind"] == "virtual" and device == sink + "_mic")

    # Vertices are (block, False) inputs and (block, True) outputs.
    through = {"meter", "mixer", "main", "virtual"}

    def links(vertex):
        ident, output = vertex
        if output:
            return [(target, False) for source, target in edges if source == ident]
        result = [(ident, True)] if nodes[ident]["kind"] in through else []
        return result + [(other, True) for other in nodes if implicit(nodes[ident], nodes[other])]

    state = {}

    def cyclic(vertex):
        state[vertex] = 1
        for following in links(vertex):
            if state.get(following) == 1 or (following not in state and cyclic(following)):
                return True
        state[vertex] = 2
        return False

    for ident in nodes:
        for vertex in ((ident, False), (ident, True)):
            if vertex not in state and cyclic(vertex):
                raise StoreError("routing would feed audio back into its own input")


def _validate_audio_routing_v2(config, *, persisted=False):
    # Named patchbay configurations; never authority to start capture.
    _object(config, {"schema_version", "active", "configs"}, {"schema_version", "active", "configs"})
    names = set()
    for item in _list(config["configs"], "routing configurations", 32):
        _object(item, {"name", "graph"}, {"name", "graph"})
        name = _routing_text(item["name"], "routing configuration name", 80).strip()
        if not name or name in names or name != item["name"]:
            raise StoreError("routing configuration names must be distinct, trimmed and non-empty")
        names.add(name)
        _validate_routing_patchbay(item["graph"], persisted=persisted)
    if config["active"] not in names:
        raise StoreError("active routing configuration must name a stored configuration")


def _validate_routing_graph(graph):
    # A bounded saved topology, never authority to start capture or recording.
    _object(graph, {"nodes", "edges"}, {"nodes", "edges"})
    kinds = ("application", "microphone", "compressor", "output", "main", "recorder")
    nodes = set()
    for node in _list(graph["nodes"], "routing graph nodes", 6):
        _object(node, {"id", "kind", "x", "y"}, {"id", "kind", "x", "y"})
        ident = _int(node["id"], "routing node id", 1, 6)
        if node["kind"] != kinds[ident - 1] or ident in nodes:
            raise StoreError("routing node ids must be unique and match their stable kind")
        _number(node["x"], "routing node x", 0, 1400)
        _number(node["y"], "routing node y", 0, 700)
        nodes.add(ident)
    allowed = {(1, 4), (1, 5), (1, 6), (2, 3), (2, 6), (3, 6)}
    edges = set()
    for edge in _list(graph["edges"], "routing graph edges", 8):
        _object(edge, {"from", "to"}, {"from", "to"})
        source = _int(edge["from"], "routing source node", 1, 6)
        target = _int(edge["to"], "routing target node", 1, 6)
        pair = (source, target)
        if source not in nodes or target not in nodes or pair not in allowed or pair in edges:
            raise StoreError("routing connection must join existing supported ports exactly once")
        edges.add(pair)
    # The allowed forward-only ports cannot form a feedback cycle. Both
    # microphone paths together would duplicate the same captured signal.
    if {(2, 3), (3, 6), (2, 6)} <= edges:
        raise StoreError("microphone cannot reach the recorder twice")


# Optional event-list window of the native queue (manual A-7): how many
# blocks ("events") and rows ("contents") are prepared ahead and shown, and the
# list refresh period. Fetch counts never fall below the shown counts. Saved
# with the programme; "view_by_title" keeps one setting per programme title.
PROGRAMME_VIEW_LIMITS = {"event_fetch": (1, 500), "event_show": (1, 200), "content_fetch": (1, 500),
                         "content_show": (1, 500), "refresh_seconds": (1, 60)}
PROGRAMME_VIEW_KEYS = frozenset(PROGRAMME_VIEW_LIMITS)


def _programme_view(view, name="programme view"):
    _object(view, PROGRAMME_VIEW_KEYS, PROGRAMME_VIEW_KEYS)
    for key, (low, high) in PROGRAMME_VIEW_LIMITS.items():
        _int(view[key], f"{name} {key}", low, high)
    if view["event_fetch"] < view["event_show"] or view["content_fetch"] < view["content_show"]:
        raise StoreError("programme view fetch counts must not be below the shown counts")
    return view


def validate_configuration(kind, config, station_id, state, *, persisted=False, previous=None):
    if kind not in KINDS:
        raise StoreError("unsupported desktop configuration kind")
    if kind == "midi_mapping":
        _validate_midi_mapping(config)
    elif kind == "desktop_preferences":
        _validate_desktop_preferences(config)
    elif kind == "copyright_policy":
        from .desktop_copyright import validate_policy
        validate_policy(config)
    elif kind == "server":
        _object(config, {"architecture", "role", "target_name", "address", "database_profile", "offline_minutes"}, {"architecture", "role", "target_name", "address"})
        for key, choices in (("architecture", {"local", "cloud", "hybrid"}), ("role", {"primary", "backup", "management"})):
            if config[key] not in choices:
                raise StoreError(f"invalid {key}")
        _text(config["target_name"], "target_name", 100, True)
        _url(config["address"], "address")
        _text(config.get("database_profile", ""), "database_profile", 100)
        _int(config.get("offline_minutes", 0), "offline_minutes", 0, 100800)
    elif kind == "web":
        keys = {"management_url", "listener_url", "site_url", "api_path", "auth_mode", "session_minutes", "default_permission", "search", "requests", "adverts"}
        _object(config, keys, keys)
        _url(config["management_url"], "management_url", https=True)
        _url(config["listener_url"], "listener_url", optional=True)
        _url(config["site_url"], "site_url", https=True, optional=True)
        if not re.fullmatch(r"/(?!/)[^\s\\]{0,199}", config["api_path"]):
            raise StoreError("api_path must be an absolute relative URL path")
        _int(config["auth_mode"], "auth_mode", 0, 2)
        _int(config["session_minutes"], "session_minutes", 1, 1440)
        _int(config["default_permission"], "default_permission", 0, 2)
        for key in ("search", "requests", "adverts"):
            _bool(config[key], key)
        if not config["site_url"] and any(config[k] for k in ("search", "requests", "adverts")):
            raise StoreError("site features require a configured site URL")
    elif kind == "live_players":
        from .desktop_player_topology import validate_live_players
        validate_live_players(config, station_id, state, persisted=persisted, previous=previous)
    elif kind == "channel":
        _object(config, {"deck_count", "slots", "names"}, {"deck_count"})
        _int(config["deck_count"], "deck_count", 1, 32)
        if "slots" in config:
            slots = _list(config["slots"], "slots", 32)
            for slot in slots:
                _int(slot, "channel slot", 0, 31)
            if len(slots) != config["deck_count"] or len(set(slots)) != len(slots):
                raise StoreError("channel slots must be distinct and match deck_count")
        if "names" in config:
            # Labels use visible channel numbers: zero is MASTER, 1..32 are
            # the stable channel slots plus one. Older documents omit names.
            names = _object(config["names"], {str(number) for number in range(33)})
            for number, value in names.items():
                _text(value, f"channel {number} name", 64)
    elif kind == "dj_mixer":
        _validate_dj_mixer(config)
    elif kind == "audio_routing" and isinstance(config, dict) and config.get("schema_version") == 2:
        _validate_audio_routing_v2(config, persisted=persisted)
    elif kind == "audio_routing":
        # This is a saved selection, not authority to resume capture. Device
        # indices and running/recording gates belong to the native session.
        keys = {"schema_version", "application_name", "application_binary",
                "output_sink", "microphone_source", "application_enabled",
                "microphone_enabled", "application_gain_db", "microphone_gain_db",
                "compressor"}
        _object(config, keys | {"graph"}, keys)
        if "graph" in config:
            _validate_routing_graph(config["graph"])
        _int(config["schema_version"], "schema_version", 1, 1)
        for key, limit in (("application_name", 512), ("application_binary", 256),
                           ("output_sink", 512), ("microphone_source", 512)):
            value = _text(config[key], key, limit)
            try:
                byte_count = len(value.encode("utf-8"))
            except UnicodeError as exc:
                raise StoreError(f"{key} must be valid UTF-8 text") from exc
            if byte_count > limit or any(
                    ord(character) < 32 or ord(character) == 127 for character in value):
                raise StoreError(f"{key} must be at most {limit} UTF-8 bytes without control characters")
        for key in ("application_enabled", "microphone_enabled"):
            _bool(config[key], key)
        for key in ("application_gain_db", "microphone_gain_db"):
            _number(config[key], key, -60, 12)
        if not isinstance(config["compressor"], str) or config["compressor"] not in {"off", "voice", "music"}:
            raise StoreError("compressor must be off, voice or music")
    elif kind == "audio_preferences":
        keys = {"main_sink", "cue_sink", "monitor_sink", "microphone_source",
                "main_gain_db", "cue_gain_db", "monitor_gain_db", "microphone_gain_db",
                "cue_source", "monitor_source"}
        _object(config, keys | {"player_automation", "loudness_normalize", "loudness_target_lufs", "loudness_policy"}, keys)
        if "loudness_policy" in config:
            from .desktop_audio_preferences import validate_loudness_policy
            validate_loudness_policy(config["loudness_policy"])
        if "loudness_normalize" in config:
            _bool(config["loudness_normalize"], "loudness_normalize")
        if "loudness_target_lufs" in config:
            _number(config["loudness_target_lufs"], "loudness_target_lufs", -30, -5)
        for key in ("main_sink", "cue_sink", "monitor_sink", "microphone_source"):
            value = _text(config[key], key, 512)
            if any(ord(character) < 32 or ord(character) == 127 for character in value):
                raise StoreError("audio device names cannot contain control characters")
        for key in ("main_gain_db", "cue_gain_db", "monitor_gain_db"):
            _number(config[key], key, -60, 6)
        _number(config["microphone_gain_db"], "microphone_gain_db", -60, 30)
        for key in ("cue_source", "monitor_source"):
            _int(config[key], key, -1, 32)
        players, masters = set(), {}
        for row in _list(config.get("player_automation", []), "player_automation", 32):
            fields = {"player", "sync_master", "autodj_target"}
            _object(row, fields, fields)
            player = _int(row["player"], "player", 1, 32)
            if player in players:
                raise StoreError("player automation identities must be distinct")
            players.add(player)
            for field in ("sync_master", "autodj_target"):
                target = _int(row[field], field, 0, 32)
                if target == player:
                    raise StoreError("player automation cannot target itself")
            masters[player] = row["sync_master"]
        for player in masters:
            visited, current = set(), player
            while current in masters and masters[current]:
                if current in visited:
                    raise StoreError("SYNC master links cannot form a cycle")
                visited.add(current)
                current = masters[current]
    elif kind == "sampler":
        _object(config, {"pads"}, {"pads"})
        seen = set()
        for pad in _list(config["pads"], "pads", 132):
            _object(pad, {"key", "title", "file"} | MEDIA_BINDING_KEYS, {"key", "title", "file"})
            matched=re.fullmatch(r"channel-(0|[1-9]\d?)-pad-([1-4])",pad["key"]) if isinstance(pad["key"],str) else None
            if matched is None or int(matched.group(1)) > 32 or pad["key"] in seen:
                raise StoreError("sampler pad keys must be distinct channel/pad identities")
            seen.add(pad["key"])
            _text(pad["title"], "title", 160)
            _text(pad["file"], "file", 4096)
            if MEDIA_BINDING_KEYS.intersection(pad):
                _media_binding({key: pad[key] for key in MEDIA_BINDING_KEYS if key in pad},
                               station_id, state, persisted=persisted)
    elif kind == "programme":
        _object(config, {"title", "date", "note", "events", "timing", "blocks", "queue_player", "queue_transition", "queue_overlap", "autodj_players", "beat_sync", "view", "view_by_title"}, {"title", "date", "note", "events"})
        if "autodj_players" in config:
            pair = _list(config["autodj_players"], "autodj_players", 2)
            if len(pair) != 2 or _int(pair[0], "AutoDJ source", 1, 32) == _int(pair[1], "AutoDJ target", 1, 32) or "queue_player" in config:
                raise StoreError("AutoDJ requires two distinct players outside a single-player queue")
            _bool(config.get("beat_sync", True), "beat_sync")
        elif "beat_sync" in config:
            raise StoreError("beat_sync requires an explicit AutoDJ player pair")
        if "queue_player" in config:
            _int(config["queue_player"], "queue_player", 1, 32)
            _int(config.get("queue_transition", 0), "queue_transition", 0, 3)
            _number(config.get("queue_overlap", 0), "queue_overlap", 0, 0)
            if config.get("queue_transition", 0) == 1:
                raise StoreError("a single-player queue cannot crossfade between two decks")
        elif {"queue_transition", "queue_overlap"}.intersection(config):
            raise StoreError("queue transition settings require queue_player")
        if "view" in config:
            _programme_view(config["view"])
        if "view_by_title" in config:
            by_title = config["view_by_title"]
            if not isinstance(by_title, dict) or len(by_title) > 64:
                raise StoreError("view_by_title must map at most 64 programme titles to view settings")
            for title, view in by_title.items():
                _text(title, "view_by_title title", 160, True)
                _programme_view(view, "view_by_title view")
        _text(config["title"], "title", 160, True)
        _date(config["date"], "date")
        _text(config["note"], "note", 4000)
        from .desktop_local_event import validate_event_playback, validate_local_event
        previous_blocks = {block["id"]: block for block in (previous or {}).get("blocks", [])}
        blocks = {}
        for block in _list(config.get("blocks", []), "blocks", 5000):
            _object(block, PROGRAMME_BLOCK_KEYS | {"playlist_id", "playback", "local_event", "live"}, PROGRAMME_BLOCK_KEYS)
            key = _id(block["id"], "block id")
            if "playback" in block:
                validate_event_playback(block["playback"])
            if "live" in block:
                # Live event step of the broadcast flow: label dj|talk|music,
                # distinct mixer channels 1..32, automation_id uuid|"", end_rule
                # manual|duration|schema ("schema": the planner decides), duration_ms.
                from .desktop_automation_calendar import LIVE_END_RULES, validate_live
                validate_live(block["live"], "programme block live", end_rules=LIVE_END_RULES | {"schema"})
            if "local_event" in block:
                old_recipe = previous_blocks.get(block.get("id"), {}).get("local_event")
                validate_local_event(block["local_event"], station_id, state,
                                     persisted=persisted, previous=old_recipe)
            # Source provenance spans typed blocks of one imported playlist.
            # Keep it optional so saved legacy programmes need no rewrite.
            if "playlist_id" in block:
                _id(block["playlist_id"], "block playlist_id")
            if key in blocks:
                raise StoreError("programme block identities must be distinct")
            blocks[key] = len(blocks)
            if block["kind"] not in PROGRAMME_KINDS:
                raise StoreError("unsupported programme block kind")
            _text(block["title"], "title", 160, True)
            _time(block["start_time"], "start_time", True)
            _text(block["note"], "note", 2000)
        seen = set()
        block_order = 0
        for event in _list(config["events"], "events", 5000):
            keys = {"id", "title", "artist", "album", "kind", "start_time", "duration_ms", "version_label", "primary_file", "backup_file", "note"}
            _object(event, keys | MEDIA_BINDING_KEYS | ONLINE_EVENT_KEYS | {"start_policy", "max_wait_ms", "block_id", "probability_policy", "policy_context"}, keys)
            key = _id(event["id"], "event id")
            if key in seen or key in blocks:
                raise StoreError("programme event identities must be distinct")
            seen.add(key)
            if "blocks" in config:
                block = _id(event.get("block_id"), "event block_id")
                if block not in blocks or blocks[block] < block_order:
                    raise StoreError("programme events must follow the order of their programme blocks")
                block_order = blocks[block]
            elif "block_id" in event:
                raise StoreError("programme event block_id requires programme blocks")
            if event["kind"] not in PROGRAMME_KINDS:
                raise StoreError("unsupported programme event kind")
            for name, limit in (("title",160), ("artist",120), ("album",120), ("version_label",80), ("primary_file",4096), ("backup_file",4096), ("note",2000)):
                _text(event[name], name, limit, name == "title")
            _time(event["start_time"], "start_time", True)
            _int(event["duration_ms"], "duration_ms", 0, 86400000)
            if "probability_policy" in event or "policy_context" in event:
                from .desktop_probability_policy import validate_probability_policy, validate_policy_context
                if "probability_policy" in event:
                    validate_probability_policy(event["probability_policy"], station_id, state, persisted=persisted)
                if "policy_context" in event:
                    validate_policy_context(event["policy_context"], event, station_id, state, persisted=persisted)
            if MEDIA_BINDING_KEYS.intersection(event):
                _media_binding({name: event[name] for name in MEDIA_BINDING_KEYS if name in event},
                               station_id, state, persisted=persisted)
            if ONLINE_EVENT_KEYS.intersection(event):
                if not persisted:
                    raise StoreError("online programme events are no longer supported; use a local media file")
                # A stored legacy event only has to keep its shape.
                if (not ONLINE_EVENT_KEYS.issubset(event) or MEDIA_BINDING_KEYS.intersection(event) or
                        event["primary_file"] or event["backup_file"]):
                    raise StoreError("stored online programme event has an invalid shape")
                for name in sorted(ONLINE_EVENT_KEYS):
                    _text(event[name], name, 1024, True)
            if "start_policy" in event and event["start_policy"] not in {"sequential", "cut", "finish"}:
                raise StoreError("programme start_policy must be sequential, cut or finish")
            if "max_wait_ms" in event:
                _int(event["max_wait_ms"], "max_wait_ms", 0, 600000)
        if "timing" in config:
            timing = config["timing"]
            keys = {"event_id", "start_time", "target_time", "policy", "max_wait_ms"}
            _object(timing, keys, keys)
            if _id(timing["event_id"], "timing event_id") not in seen:
                raise StoreError("programme timing requires an event in this programme")
            _time(timing["start_time"], "start_time")
            _time(timing["target_time"], "target_time")
            if timing["policy"] not in {"cut", "finish"}:
                raise StoreError("programme timing policy must be cut or finish")
            _int(timing["max_wait_ms"], "max_wait_ms", 0, 600000)
    elif kind == "playout_policy":
        fields = {"schema", "mode", "overlap_ms", "beat_sync"}
        _object(config, fields, fields)
        _int(config["schema"], "playout policy schema", 1, 1)
        mode = _int(config["mode"], "playout transition mode", 0, 3)
        overlap = _int(config["overlap_ms"], "playout overlap_ms", 0, 30000)
        _bool(config["beat_sync"], "playout beat_sync")
        if mode in {0, 3} and (overlap or config["beat_sync"]):
            raise StoreError("EOF and manual transitions require zero overlap and no beatmatching")
        if config["beat_sync"] and (mode != 1 or not overlap):
            raise StoreError("beatmatching requires a positive crossfade overlap")
    elif kind == "playout_history":
        _object(config, {"entries", "kind", "rotation"}, {"entries", "kind"})
        if config["kind"] != "local_playback":
            raise StoreError("history records local playback observations only")
        if "rotation" in config:
            rotation = config["rotation"]
            rotation_keys = {"schema", "config_key", "source_index", "in_turn", "cursors",
                             "fallback_cursor", "jingle_cursor", "rng_hex", "complete_seen",
                             "jingle_phase", "jingle_due"}
            _object(rotation, rotation_keys, rotation_keys)
            _int(rotation["schema"], "rotation schema", 1, 1)
            for name, length in (("config_key", 64), ("rng_hex", 16)):
                if not isinstance(rotation[name], str) or re.fullmatch(r"[0-9a-f]{%d}" % length, rotation[name]) is None:
                    raise StoreError(f"rotation {name} must be {length} lowercase hexadecimal characters")
            _int(rotation["source_index"], "rotation source_index", 0, 49)
            _int(rotation["in_turn"], "rotation in_turn", 0, 500)
            _int(rotation["jingle_phase"], "rotation jingle_phase", 0, 100)
            _bool(rotation["jingle_due"], "rotation jingle_due")
            for cursor in _list(rotation["cursors"], "rotation cursors", 50):
                _int(cursor, "rotation cursor", 0, 2000)
            for name in ("fallback_cursor", "jingle_cursor"):
                _int(rotation[name], f"rotation {name}", 0, 2000)
            seen_entries = set()
            for entry_id in _list(rotation["complete_seen"], "rotation complete_seen", 500):
                if _id(entry_id, "rotation complete_seen entry") != entry_id or entry_id in seen_entries:
                    raise StoreError("rotation complete_seen requires distinct canonical entry UUIDs")
                seen_entries.add(entry_id)
        keys = {"media_id", "version_id", "artist", "album", "started_utc", "position_frames"}
        for entry in _list(config["entries"], "history entries", 2000):
            _object(entry, keys, keys)
            media_id = _id(entry["media_id"], "history media_id")
            version_id = _id(entry["version_id"], "history version_id")
            library = state.get("desktop_media_library", {}).get(media_id, {}).get("data")
            if not library or library["station_id"] != station_id or library["version_id"] != version_id:
                raise StoreError("history requires a catalog version in this station")
            _text(entry["artist"], "history artist", 200)
            _text(entry["album"], "history album", 200)
            _stamp(entry["started_utc"], "history started_utc")
            _int(entry["position_frames"], "history position_frames", 1, 4_147_200_000)
    elif kind in {"transition", "markers"}:
        allowed = {"values", "flag", "files", "preview_level"}
        if kind == "transition":
            allowed |= {"media_a", "media_b"}
        _object(config, allowed, {"values", "flag", "files", "preview_level"})
        for key in ("media_a", "media_b"):
            if key in config:
                _media_binding(config[key], station_id, state, persisted=persisted)
        values = _list(config["values"], "values", 8)
        files = _list(config["files"], "files", 2)
        if len(values) != 8 or (len(files) != 2 and not (kind == "markers" and not files)):
            raise StoreError("transition requires eight numeric fields and two file references")
        numeric = []
        for i, value in enumerate(values):
            required = i in {0, 1, 3, 7} if kind == "markers" else i != 2
            _text(value, "value", 32, required)
            try:
                minimum = 0.1 if i == 0 else -30 if kind == "transition" and i == 7 else 0
                maximum = 30 if kind == "transition" and i in {5,6} else 0 if kind == "transition" and i == 7 else 86400
                number = _number(float(value), "value", minimum, maximum) if value else 0
                numeric.append(number)
            except ValueError as exc:
                raise StoreError("transition values must be finite numeric text") from exc
        for value in files:
            _text(value, "file", 4096)
        _bool(config["flag"], "flag")
        _number(config["preview_level"], "preview_level", -36, 0)
        if numeric[3] <= numeric[1] or numeric[3] > numeric[0] or (values[2] and not numeric[1] <= numeric[2] <= numeric[3]):
            raise StoreError("cue and vocal markers must lie within duration")
        if kind == "markers":
            if any(values[i] and not numeric[1] <= numeric[i] <= numeric[3] for i in (4,5,6)):
                raise StoreError("fade and auto-next markers must lie within cue interval")
            if bool(values[4]) != bool(values[5]) or (values[4] and numeric[5] < numeric[4]) or (config["flag"] and not values[6]):
                raise StoreError("fade marker pair or automatic next marker is invalid")
        if numeric[7 if kind == "markers" else 6] > numeric[3] - numeric[1]:
            raise StoreError("overlap exceeds cue interval")
    elif kind == "emergency":
        _object(config, {"queues", "preparation"}, {"queues", "preparation"})
        queues = _list(config["queues"], "queues", 2)
        if len(queues) != 2:
            raise StoreError("emergency preparation requires two lists")
        seen = set()
        for queue in queues:
            for entry in _list(queue, "queue", 1000):
                _object(entry, {"kind", "values", "file", "id"} | MEDIA_BINDING_KEYS, {"kind", "values", "file"})
                if "id" in entry:
                    identifier = _id(entry["id"], "emergency entry id")
                    if identifier in seen:
                        raise StoreError("emergency entry identities must be distinct")
                    seen.add(identifier)
                if MEDIA_BINDING_KEYS.intersection(entry):
                    _media_binding({name: entry[name] for name in MEDIA_BINDING_KEYS if name in entry},
                                   station_id, state, persisted=persisted)
                _int(entry["kind"], "kind", 0, 4)
                values = _list(entry["values"], "values", 9)
                if len(values) != 9:
                    raise StoreError("emergency entry requires nine metadata fields")
                for value in values:
                    _text(value, "value", 2000)
                _text(entry["file"], "file", 4096)
        _validate_preparation(config["preparation"])
    elif kind == "automation":
        _validate_automation(config, station_id, state, persisted=persisted, previous=previous)
    elif kind == "automation_calendar":
        from .desktop_automation_calendar import validate_calendar
        validate_calendar(config, station_id, state, persisted=persisted, previous=previous)
    elif kind == "weekly_programme":
        from .desktop_weekly_programme import validate_weekly_programme
        validate_weekly_programme(config, station_id, state, persisted=persisted, previous=previous)
    elif kind == "local_event":
        from .desktop_local_event import validate_local_event
        validate_local_event(config, station_id, state, persisted=persisted, previous=previous)
    elif kind == "advert_break":
        from .desktop_advert import validate_break
        validate_break(config, station_id, state, persisted=persisted)
    elif kind in {"advert_local_run", "advert_report", "advert_notifications"}:
        from .desktop_advert import validate_owned_configuration
        validate_owned_configuration(kind, config, station_id, state)
    elif kind in {"broadcast_stream", "broadcast_record", "broadcast_metadata", "broadcast_local_run"}:
        from .desktop_broadcast import validate_configuration
        validate_configuration(kind, config, station_id, state, persisted=persisted)
    elif kind in {"admin_recovery", "admin_restore_fence"}:
        from .desktop_admin import validate_recovery
        validate_recovery(config, station_id, state)
    if len(json.dumps(config, ensure_ascii=False, allow_nan=False).encode()) > 1800000:
        raise StoreError("native configuration exceeds byte budget")
    return deepcopy(config)


def _validate_preparation(value):
    # Filled by the native emergency editor using its stable field labels.
    if not isinstance(value, dict) or len(value) > 500:
        raise StoreError("emergency preparation must have at most 500 typed fields")
    allowed = PREPARATION_FIELDS
    for key, text in value.items():
        base = key.split("/", 1)[-1]
        if base not in allowed:
            raise StoreError("unsupported emergency preparation field")
        if base in {"Test", "Gerçek"} and text not in {"0", "1"}:
            raise StoreError("emergency purpose flags must be 0 or 1")
        _text(key, "preparation key", 200)
        _text(text, "preparation value", 4000)


PREPARATION_FIELDS = frozenset({
    "Devralacak oynatıcı", "Devralma davranışı", "Acil liste adı", "Liste bitince", "Normale dönüş", "Yedek zincir anahtarı · GPO",
    "Eşik · dB", "Süre · sn", "2 · Sürerse · sn", "Ses dönünce eşik · dB", "Dönüş süresi · sn", "Fade · ms",
    "Oynatma RTO · sn", "Yayın günlüğü RPO · dk", "Katalog RPO · dk", "Ses arşivi RPO · sa",
    "Başlangıç", "Bitiş", "Dakika", "Gerekçe", "Metin dili", "Okunacak metin · TTS", "Ses dosyası / medya kimliği", "Bölge kodları", "Tekrar · dk",
    "Stüdyo", "İnternet", "Karasal", "Etiket", "Sahibi", "Devreden", "Devralan", "Son durum", "Ad", "Görev", "Telefon",
    "Tarih", "Senaryo", "Sonuç", "Algılama · sn", "Karar · sn", "Devralma · sn", "Toparlanma · sn", "Boşluk · ms",
    "Taslak durumu", "İptal gerekçesi", "mode", "actual-broadcast-state", "draft-log", "draft-kind", "Test", "Gerçek",
    "İzlenen nokta", "1 · Süre dolunca", "Sonra", "Sessiz sayılması", "Ölçüm", "Harici dedektör · GPI",
    "Sessizlikte otomatik devralma", "Kesme", "Kilitli mod", "Devralma yetkisi", "Ayar yetkisi", "Bu oturum",
    "İkinci yetkili", "Mühürlü kod yeri", "Yapılan işlem", "Kalan risk", "Mesai dışı", "Çift ses", "Not",
    "Yedek makinede", "Taşıma / destek dışı saat",
})


def _validate_automation(config, station_id, state, *, persisted=False, previous=None):
    keys = {"name", "seed", "planDate", "startTime", "windowStart", "windowEnd", "sourceMode", "trackMode", "transition", "boundaryTransition", "deckMode", "fallbackMode", "trackGap", "artistGap", "albumGap", "previewCount", "jingleEvery", "overlap", "boundaryOverlap", "includeProgramme", "jingleEnabled", "jingleDry", "requireMetadata", "jinglePlaylist", "fallbackPlaylist", "jingleDeck", "weekdays", "decks", "sources"}
    _object(config, keys | {"deckTransitions", "probability_policy", "probability_source_policies"}, keys)
    if "probability_policy" in config or "probability_source_policies" in config:
        from .desktop_probability_policy import validate_probability_policy
        if "probability_policy" in config:
            validate_probability_policy(config["probability_policy"], station_id, state, persisted=persisted,
                previous=(previous or {}).get("probability_policy"))
        for policy in _list(config.get("probability_source_policies", []), "probability source policies", 50):
            validate_probability_policy(policy, station_id, state, persisted=persisted)
    _text(config["name"], "name", 80, True)
    _text(config["seed"], "seed", 64, True)
    _date(config["planDate"], "planDate")
    for key in ("startTime", "windowStart", "windowEnd"):
        _time(config[key], key)
    for key, high in (("sourceMode",2), ("trackMode",2), ("transition",3), ("boundaryTransition",4), ("deckMode",1), ("fallbackMode",2)):
        _int(config[key], key, 0, high)
    for key in ("trackGap", "artistGap", "albumGap"):
        _int(config[key], key, 0, 10080)
    _int(config["previewCount"], "previewCount", 1, 200)
    _int(config["jingleEvery"], "jingleEvery", 1, 100)
    for key in ("overlap", "boundaryOverlap"):
        _number(config[key], key, 0, 30)
    for key in ("includeProgramme", "jingleEnabled", "jingleDry", "requireMetadata"):
        _bool(config[key], key)
    previous = previous or {}
    for key, mode, enabled in (("jinglePlaylist", "jingleEnabled", True),
                               ("fallbackPlaylist", "fallbackMode", 2)):
        unchanged = config[key] == previous.get(key) and (
            config[mode] == previous.get(mode) or config[mode] != enabled)
        _reference(state, station_id, config[key], "playlists", key, optional=True,
                   active=not (persisted or unchanged))
    if config["jingleEnabled"] and config["jinglePlaylist"] is None or config["fallbackMode"] == 2 and config["fallbackPlaylist"] is None:
        raise StoreError("enabled jingle/fallback requires a station playlist")
    if config["jingleDeck"] is not None:
        _text(config["jingleDeck"], "jingleDeck", 64, True)
    weekdays = _list(config["weekdays"], "weekdays", 7)
    if not weekdays or len(set(weekdays)) != len(weekdays):
        raise StoreError("weekdays must be nonempty and unique")
    for value in weekdays:
        _int(value, "weekday", 1, 7)
    decks = _list(config["decks"], "decks", 64)
    for value in decks:
        _text(value, "deck", 64, True)
    if len(set(decks)) != len(decks):
        raise StoreError("deck identities must be distinct")
    seen_decks = set()
    for policy in _list(config.get("deckTransitions", []), "deckTransitions", 32):
        _object(policy, {"deck_id", "transition", "overlap"}, {"deck_id", "transition", "overlap"})
        identifier = _text(policy["deck_id"], "deck_id", 2, True)
        if not re.fullmatch(r"(?:[1-9]|[12][0-9]|3[0-2])", identifier) or identifier not in decks:
            raise StoreError("AutoDJ transition requires a selected native deck 1..32")
        if identifier in seen_decks:
            raise StoreError("AutoDJ deck transition identities must be distinct")
        seen_decks.add(identifier)
        _int(policy["transition"], "deck transition", 0, 3)
        _number(policy["overlap"], "deck overlap", 0, 30)
        if policy["transition"] in {0, 3} and policy["overlap"] != 0:
            raise StoreError("EOF/manual AutoDJ deck transitions require zero overlap")
    previous_sources = {_id(row["source_id"], "source_id"): row
                        for row in previous.get("sources", [])}
    seen = set()
    for source in _list(config["sources"], "sources", 50):
        _object(source, {"source_id", "playlist_id", "weight", "count", "start", "end", "days"}, {"source_id", "playlist_id", "weight", "count", "start", "end", "days"})
        key = _id(source["source_id"], "source_id")
        if key in seen:
            raise StoreError("source identities must be distinct")
        seen.add(key)
        _reference(state, station_id, source["playlist_id"], "playlists", "playlist_id",
                   active=not (persisted or source == previous_sources.get(key)))
        _int(source["weight"], "weight", 1, 100)
        _int(source["count"], "count", 1, 100)
        _int(source["days"], "days", 0, 2)
        _time(source["start"], "start")
        _time(source["end"], "end")


def validate_document(data, state):
    _text(data["name"], "name", 160, True)
    if data["status"] not in {"active", "disabled"}:
        raise StoreError("invalid native configuration status")
    if data["kind"] in LEGACY_KINDS:
        # Retired online-service documents only have to stay readable.
        if not isinstance(data["config"], dict):
            raise StoreError("stored retired configuration must be an object")
    else:
        validate_configuration(data["kind"], data["config"], data["station_id"], state,persisted=True)
    for key in ("created_at", "updated_at"):
        _stamp(data[key], key)


def _stamp(value, name):
    try:
        stamp = datetime.fromisoformat(value)
        if stamp.tzinfo is None or stamp.utcoffset() is None:
            raise ValueError
    except (ValueError, TypeError) as exc:
        raise StoreError(f"{name} must be a timestamp with timezone") from exc


def validate_mutation(data):
    _id(data["idempotency_key"], "idempotency_key")
    if not isinstance(data["request_hash"], str) or not re.fullmatch(r"[0-9a-f]{64}", data["request_hash"]):
        raise StoreError("mutation requires SHA-256 request hash")
    _text(data["operation"], "operation", 100, True)
    if not isinstance(data["result"], dict) or len(json.dumps(data["result"], allow_nan=False).encode()) > 1900000:
        raise StoreError("mutation result must be a bounded object")
    _stamp(data["created_at"], "created_at")


def guard_document(before, after):
    if before and after and any(before.get(key) != after.get(key) for key in ("station_id", "kind", "created_by", "created_at")):
        raise StoreError("native configuration station, kind and creation identity are immutable")
    if before and before.get("kind") in {"advert_local_run", "advert_report", "advert_notifications"}:
        from .desktop_advert import guard_owned_document
        guard_owned_document(before, after)
    if before and before.get("kind") == "broadcast_local_run":
        from .desktop_broadcast import guard_run
        guard_run(before, after)
    if before and before.get("kind") in {"admin_recovery", "admin_restore_fence"}:
        from .desktop_admin import guard_recovery
        guard_recovery(before, after)


def operation_replay(store, principal, station_id, idempotency_key, operation, payload):
    key = _id(idempotency_key, "idempotency_key")
    try:
        encoded = json.dumps(payload, sort_keys=True, ensure_ascii=False,
                             separators=(",", ":"), allow_nan=False).encode()
    except (TypeError, ValueError) as exc:
        raise StoreError("request payload must contain valid JSON values") from exc
    request_hash = hashlib.sha256(encoded).hexdigest()
    found = store.lookup("desktop_mutations", "request", station_id, principal.user_id, key)
    if found:
        if found[0]["operation"] != operation or found[0]["request_hash"] != request_hash:
            raise Conflict("idempotency key was already used for a different request")
        return found[0]["_id"], request_hash, deepcopy(found[0]["result"])
    return str(uuid4()), request_hash, None


def add_operation(tx, principal, station_id, receipt_id, idempotency_key, operation, request_hash, result):
    tx.put("desktop_mutations", receipt_id, {"station_id": station_id, "user_id": principal.user_id,
        "idempotency_key": _id(idempotency_key), "operation": operation, "request_hash": request_hash,
        "result": deepcopy(result), "created_at": datetime.now(timezone.utc).isoformat()})


def _station(store, principal, station_id, permission="view"):
    station_id = _id(station_id, "station_id")
    require(store, principal, permission, station_id)
    station = store.get("stations", station_id)
    if station is None:
        raise NotFound("station not found")
    return station_id, station


@_consistent_read
def desktop_context(store: Store, principal: Principal, station_id: str):
    station_id, station = _station(store, principal, station_id)
    try:
        require(store, principal, "manage", station_id)
        manager = True
    except Forbidden:
        manager = False
    with store._locked(exclusive=False):
        store._reload_locked()
        user = store.get("users", principal.user_id)
        grants = [row for row in store.scan("permissions") if not row.get("revoked_at")]
        local_grants = [row for row in grants if row.get("station_id") in (None, station_id)]
        user_ids = {row["user_id"] for row in local_grants} if manager else {principal.user_id}
        users = [{key: row.get(key) for key in ("_id", "_rev", "username", "display_name", "status")} for row in store.scan("users") if row["_id"] in user_ids]
        permissions = [{key: row.get(key) for key in ("_id", "_rev", "user_id", "permission", "station_id")} for row in local_grants if row["user_id"] in user_ids]
        endpoints = store.lookup("endpoints", "station", station_id)
        outputs = [row for row in store.scan("station_outputs") if row["station_id"] == station_id]
        collections = {"endpoints":len(endpoints), "users":len(users), "outputs":len(outputs)}
        for collection in ("desktop_configurations", "playlists", "plans", "station_items"):
            collections[collection] = len([row for row in store.scan(collection) if row.get("station_id") == station_id])
        from .desktop_copyright import policy_snapshot
        copyright_policy, copyright_document = policy_snapshot(store, station_id)
        return {"station": station, "user": {key:user.get(key) for key in ("_id", "_rev", "username", "display_name", "status")}, "endpoints": endpoints, "users":users, "permissions":permissions, "outputs":outputs, "collections":collections, "sequence":store.sequence,
                "copyright_policy": copyright_policy,
                "copyright_policy_document_id": copyright_document["_id"] if copyright_document else None,
                "copyright_policy_revision": copyright_document["_rev"] if copyright_document else 0}


@_consistent_read
def list_documents(store, principal, station_id, kind):
    station_id, _ = _station(store, principal, station_id)
    if kind not in KINDS:
        raise StoreError("unsupported configuration kind")
    rows = [row for row in store.lookup("desktop_configurations", "station_kind", station_id, kind) if row["status"] == "active"]
    rows.sort(key=lambda row:(row["name"].casefold(), row["_id"]))
    return {"records":rows, "sequence":store.sequence}


@_consistent_read
def get_document(store, principal, station_id, kind, document_id):
    station_id, _ = _station(store, principal, station_id)
    row = store.get("desktop_configurations", _id(document_id, "document_id"))
    if row is None or row["station_id"] != station_id or row["kind"] != kind:
        raise NotFound("configuration not found in station")
    return row


def put_document(store, principal, station_id, kind, name, config, idempotency_key,
                 document_id=None, expected_revision=None):
    sequence = store.sequence
    station_id, _ = _station(store, principal, station_id, "manage" if kind in MANAGED else "edit")
    if kind in {"broadcast_local_run", "admin_recovery", "admin_restore_fence"}:
        raise StoreError("managed broadcast receipts require desktop-broadcast workflows")
    if kind in {"advert_local_run", "advert_report", "advert_notifications"} or (kind == "advert_break" and isinstance(config, dict) and "bridge" in config):
        raise StoreError("managed advertising records require desktop-advert workflows")
    _text(name, "name", 160, True)
    if kind not in KINDS:
        # LEGACY_KINDS (retired online services) stay readable but are never saved again.
        raise StoreError("unsupported configuration kind")
    payload = {"kind":kind, "name":name, "config":config, "document_id":document_id, "expected_revision":expected_revision}
    receipt, digest, replay = operation_replay(store, principal, station_id, idempotency_key, "desktop/put", payload)
    if replay is not None:
        return replay
    existing = None
    new_identifier = None
    if document_id is not None:
        if type(expected_revision) is int and expected_revision == 0:
            # A native operator can durably name a new document before its
            # first acknowledgement, so a queued follow-up has a real identity.
            new_identifier = _id(document_id, "document_id")
            if store.get("desktop_configurations", new_identifier) is not None:
                raise Conflict("new configuration identity already exists")
        else:
            existing = get_document(store, principal, station_id, kind, document_id)
            if kind == "advert_break" and "bridge" in existing["config"]:
                raise StoreError("bridged advertising breaks require desktop-advert workflows")
            if type(expected_revision) is not int or expected_revision < 1 or existing["_rev"] != expected_revision:
                raise Conflict("configuration changed")
    elif expected_revision is not None:
        raise Conflict("new configuration cannot carry an existing revision")
    if kind in {"playout_policy", "copyright_policy"}:
        if name.strip() != "main":
            raise StoreError(f"the {kind} must use its canonical main name")
        if any(row["status"] == "active" and row["name"] == "main" and
               row["_id"] != (existing["_id"] if existing else None)
               for row in store.lookup("desktop_configurations", "station_kind", station_id, kind)):
            label = "general playback" if kind == "playout_policy" else "copyright"
            raise Conflict(f"{label} policy already exists; reload its revision")
    config = validate_configuration(kind, config, station_id, store._state,
                                    previous=existing["config"] if existing else None)
    key = existing["_id"] if existing else new_identifier or str(uuid4())
    now = datetime.now(timezone.utc).isoformat()
    body = {"station_id":station_id, "kind":kind, "name":name.strip(), "config":config, "status":"active", "created_by":existing["created_by"] if existing else principal.user_id, "created_at":existing["created_at"] if existing else now, "updated_by":principal.user_id, "updated_at":now}
    result = body | {"_id":key, "_rev":existing["_rev"] + 1 if existing else 1}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("desktop_configurations", key, body, expected_revision=expected_revision if existing else None)
    add_operation(tx, principal, station_id, receipt, idempotency_key, "desktop/put", digest, result)
    tx.commit()
    return result


def delete_document(store, principal, station_id, kind, document_id, expected_revision, idempotency_key):
    sequence = store.sequence
    station_id, _ = _station(store, principal, station_id, "manage" if kind in MANAGED else "edit")
    if kind in {"advert_local_run", "advert_report", "advert_notifications"}:
        raise StoreError("managed advertising audit records cannot be deleted")
    if kind in {"broadcast_local_run", "admin_recovery", "admin_restore_fence"}:
        raise StoreError("managed broadcast receipts cannot be deleted")
    payload = {"kind":kind, "document_id":document_id, "expected_revision":expected_revision}
    receipt, digest, replay = operation_replay(store, principal, station_id, idempotency_key, "desktop/delete", payload)
    if replay is not None:
        return replay
    row = get_document(store, principal, station_id, kind, document_id)
    if kind == "advert_break" and "bridge" in row["config"]:
        raise StoreError("bridged advertising breaks require the cancel workflow")
    if type(expected_revision) is not int or row["_rev"] != expected_revision:
        raise Conflict("configuration changed")
    body = {key:value for key,value in row.items() if key not in {"_id", "_rev"}}
    body.update(status="disabled", updated_by=principal.user_id, updated_at=datetime.now(timezone.utc).isoformat())
    result = body | {"_id":row["_id"], "_rev":row["_rev"] + 1}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("desktop_configurations", row["_id"], body, expected_revision=expected_revision)
    add_operation(tx, principal, station_id, receipt, idempotency_key, "desktop/delete", digest, result)
    tx.commit()
    return result


CAMPAIGN_FIELDS = frozenset({"advertiser", "owner", "reference", "start_date", "end_date", "target_hint", "spot_title", "version_label", "duration_seconds", "planned_count", "price", "currency", "contact", "slot", "capacity_seconds", "intro_seconds", "outro_seconds", "announcement_seconds", "competition", "position", "fallback", "skip_empty"})


def _campaign_fields(fields):
    _object(fields, CAMPAIGN_FIELDS, CAMPAIGN_FIELDS)
    for key,limit in (("advertiser",120), ("owner",120), ("reference",120), ("target_hint",160), ("spot_title",160), ("version_label",40), ("contact",1000), ("competition",160)):
        _text(fields[key], key, limit, key in {"advertiser", "spot_title", "version_label"})
    start = _date(fields["start_date"], "start_date")
    end = _date(fields["end_date"], "end_date")
    if end < start:
        raise StoreError("campaign end date precedes start date")
    _time(fields["slot"], "slot", optional=True)
    for key, low, high in (("duration_seconds",1,3600), ("planned_count",0,100000), ("capacity_seconds",0,86400), ("intro_seconds",0,3600), ("outro_seconds",0,3600), ("announcement_seconds",0,3600), ("position",0,2), ("fallback",0,2)):
        _int(fields[key], key, low, high)
    _number(fields["price"], "price", 0, 1000000000)
    if fields["currency"] not in {"TRY", "EUR", "USD"}:
        raise StoreError("unsupported currency")
    _bool(fields["skip_empty"], "skip_empty")
    return deepcopy(fields)


def _campaign(store, station_id, campaign_id):
    row = store.get("campaigns", _id(campaign_id, "campaign_id"))
    if row is None or station_id not in (row.get("station_ids") or []):
        raise NotFound("campaign not found in station")
    return row


@_consistent_read
def list_campaigns(store, principal, station_id):
    station_id, _ = _station(store, principal, station_id)
    records = []
    for row in store.scan("campaigns"):
        if station_id in (row.get("station_ids") or []):
            advertiser = store.get("advertisers", row["advertiser_id"])
            records.append(row | {"advertiser_name": advertiser["name"] if advertiser else ""})
    records.sort(key=lambda row:(row["name"].casefold(), row["_id"]))
    return {"records":records, "sequence":store.sequence}


@_consistent_read
def get_campaign(store, principal, station_id, campaign_id):
    station_id, _ = _station(store, principal, station_id)
    row = _campaign(store, station_id, campaign_id)
    advertiser = store.get("advertisers", row["advertiser_id"])
    return row | {"advertiser_name":advertiser["name"] if advertiser else ""}


def save_campaign(store, principal, station_id, name, fields, idempotency_key,
                  campaign_id=None, expected_revision=None):
    sequence = store.sequence
    station_id, station = _station(store, principal, station_id, "edit")
    name = _text(name, "name", 120, True).strip()
    fields = _campaign_fields(fields)
    payload = {"name":name, "fields":fields, "campaign_id":campaign_id, "expected_revision":expected_revision}
    receipt,digest,replay = operation_replay(store,principal,station_id,idempotency_key,"desktop/campaign-save",payload)
    if replay is not None:
        return replay
    old = _campaign(store,station_id,campaign_id) if campaign_id else None
    if old:
        if old["status"] not in {"draft", "approved", "active", "paused"} or type(expected_revision) is not int or old["_rev"] != expected_revision:
            raise Conflict("only the current non-final native campaign can be edited")
        if any(row["campaign_id"] == old["_id"] and row["status"] == "approved" for row in store.scan("spots")) and any(
                fields[key] != old["desktop_fields"][key] for key in ("advertiser", "start_date", "end_date", "planned_count", "slot", "capacity_seconds", "competition", "position")):
            raise Conflict("approved advertiser, obligation windows/counts and spot constraints are retained; create a new campaign for changed delivery terms")
    elif expected_revision is not None:
        raise Conflict("new campaign cannot have a previous revision")
    advertiser = next((row for row in store.scan("advertisers") if row["name"].casefold() == fields["advertiser"].strip().casefold()),None)
    if advertiser is None:
        require(store,principal,"manage",station_id)
    now = datetime.now(timezone.utc).isoformat()
    zone = ZoneInfo(station["timezone"])
    start = datetime.combine(date.fromisoformat(fields["start_date"]), datetime.min.time(), zone)
    try:
        end = datetime.combine(date.fromisoformat(fields["end_date"]) + timedelta(days=1), datetime.min.time(),zone)
    except OverflowError as exc:
        raise StoreError("campaign end date exceeds supported calendar") from exc
    key = old["_id"] if old else str(uuid4())
    advertiser_id = advertiser["_id"] if advertiser else str(uuid4())
    item_id = old.get("desktop_item_id") if old else str(uuid4())
    version_id = old.get("desktop_version_id") if old else str(uuid4())
    if old and not (item_id and version_id):
        raise Conflict("existing campaign has no editable native spot draft")
    body = ({k:v for k,v in old.items() if k not in {"_id","_rev"}} if old else {"status":"draft","station_ids":[station_id],"region_ids":None,"output_ids":None,"created_by":principal.user_id,"created_at":now})
    body.update(advertiser_id=advertiser_id,name=name,starts_at=start.isoformat(),ends_at=end.isoformat(),desktop_fields=fields,desktop_item_id=item_id,desktop_version_id=version_id,updated_by=principal.user_id,updated_at=now)
    result = body | {"_id":key,"_rev":old["_rev"]+1 if old else 1,"advertiser_name":fields["advertiser"].strip()}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    if advertiser is None:
        tx.put("advertisers",advertiser_id,{"name":fields["advertiser"].strip(),"created_by":principal.user_id,"created_at":now})
    tx.put("campaigns",key,body,expected_revision=expected_revision)
    if old:
        item,version = store.get("items",item_id),store.get("versions",version_id)
        if old.get("desktop_binding"):
            # Sales metadata belongs to the campaign. Imported source titles
            # and approved version labels retain their catalog identities.
            pass
        elif item is None or version is None or item["lifecycle"] != "draft" or version["status"] != "draft":
            raise Conflict("native spot draft was changed or approved elsewhere")
        else:
            tx.put("items",item_id,{k:v for k,v in item.items() if k not in {"_id","_rev"}} | {"title":fields["spot_title"]},expected_revision=item["_rev"])
            tx.put("versions",version_id,{k:v for k,v in version.items() if k not in {"_id","_rev"}} | {"label":fields["version_label"]},expected_revision=version["_rev"])
    else:
        tx.put("items",item_id,{"kind":"advertisement","title":fields["spot_title"],"lifecycle":"draft","recording_id":None,"created_at":now})
        tx.put("station_items",str(uuid4()),{"station_id":station_id,"item_id":item_id,"eligible":False,"category_id":None,"rotation_code":None})
        tx.put("versions",version_id,{"item_id":item_id,"version_no":1,"label":fields["version_label"],"status":"draft","created_at":now})
    add_operation(tx,principal,station_id,receipt,idempotency_key,"desktop/campaign-save",digest,result)
    tx.commit()
    return result


def delete_campaign(store,principal,station_id,campaign_id,expected_revision,idempotency_key):
    sequence = store.sequence
    station_id,_ = _station(store,principal,station_id,"edit")
    payload = {"campaign_id":campaign_id,"expected_revision":expected_revision}
    receipt,digest,replay = operation_replay(store,principal,station_id,idempotency_key,"desktop/campaign-delete",payload)
    if replay is not None:
        return replay
    row = _campaign(store,station_id,campaign_id)
    if type(expected_revision) is not int or row["_rev"] != expected_revision or row["status"] not in {"draft","approved"}:
        raise Conflict("only an unchanged draft/approved campaign can be cancelled")
    body = {k:v for k,v in row.items() if k not in {"_id","_rev"}} | {"status":"cancelled"}
    result = body | {"_id":row["_id"],"_rev":row["_rev"]+1}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("campaigns",row["_id"],body,expected_revision=expected_revision)
    add_operation(tx,principal,station_id,receipt,idempotency_key,"desktop/campaign-delete",digest,result)
    tx.commit()
    return result


def create_user(store,principal,station_id,username,display_name,role,additional_permissions,
                idempotency_key,custom_role="",shift_start="",shift_end="",temporary=False):
    """Create station grants and retain the issued key privately before commit.

    Replays return the private key file identity, never the plaintext token.
    Advanced device/program and shift authorization is not part of this API.
    """
    from .access_admin import _USERNAME, _PERMISSIONS
    sequence = store.sequence
    station_id,_ = _station(store,principal,station_id,"manage")
    if not isinstance(username,str) or not _USERNAME.fullmatch(username):
        raise StoreError("username must contain 3..80 letters, numbers or ._@-")
    _text(display_name,"display_name",120,True)
    roles = {"operator":{"view","operate"},"dj":{"view","operate","edit"},"manager":set(_PERMISSIONS),"advert":{"view","edit","approve"},"custom":{"view"}}
    if not isinstance(role,str) or role not in roles:
        raise StoreError("unsupported station role")
    _text(custom_role,"custom_role",100)
    if shift_start or shift_end or temporary:
        raise StoreError("shift time authorization is not supported by the desktop station permissions")
    extras = _list(additional_permissions,"additional_permissions",9)
    if any(not isinstance(value,str) or value not in _PERMISSIONS for value in extras) or len(set(extras)) != len(extras):
        raise StoreError("additional permissions must be distinct named station permissions")
    permissions = sorted(roles[role] | set(extras))
    payload = {"username":username,"display_name":display_name,"role":role,"additional_permissions":extras,"custom_role":custom_role,"shift_start":shift_start,"shift_end":shift_end,"temporary":temporary}
    receipt,digest,replay = operation_replay(store,principal,station_id,idempotency_key,"desktop/user-create",payload)
    if replay is not None:
        return replay
    if store.lookup("users","username",username):
        raise Conflict("username already exists")
    user_id,token_id,token = str(uuid4()),str(uuid4()),secrets.token_urlsafe(32)
    now = datetime.now(timezone.utc)
    expires = (now+timedelta(days=30)).isoformat()
    directory = _private_key_directory(store)
    credential_file = directory/(user_id+".json")
    secret = {"user_id":user_id,"station_id":station_id,"token_id":token_id,"token":token,"expires_at":expires}
    # CREATE_NEW never follows an existing name; the file is born with the private
    # DACL (owner = this user, SYSTEM, Administrators), the Windows form of 0600.
    descriptor = _platform.open_fd(credential_file,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600,nofollow=True,private=True)
    try:
        try:
            stream = os.fdopen(descriptor,"w",encoding="utf-8",newline="\n")
        except BaseException:
            os.close(descriptor)
            raise
        with stream:
            json.dump(secret,stream,ensure_ascii=False,allow_nan=False);stream.write("\n");stream.flush()
            _platform.fsync(stream.fileno())
        # NTFS journals the directory entry; there is no directory flush on Windows.
        result = {"user_id":user_id,"token_id":token_id,"station_id":station_id,"expires_at":expires,"credential_file":str(credential_file),"permissions":permissions}
        tx = store.begin(actor=principal.user_id)
        tx.expect_sequence(sequence)
        tx.put("users",user_id,{"username":username,"display_name":display_name.strip(),"status":"active","created_by":principal.user_id,"created_at":now.isoformat(),"desktop_role":role,"desktop_custom_role":custom_role})
        for permission in permissions:
            tx.put("permissions",str(uuid4()),{"user_id":user_id,"permission":permission,"station_id":station_id,"station_scope":station_id,"granted_by":principal.user_id,"granted_at":now.isoformat()})
        tx.put("tokens",token_id,{"user_id":user_id,"scope":"user","token_hash":hashlib.sha256(token.encode()).hexdigest(),"expires_at":expires,"revoked_at":None})
        add_operation(tx,principal,station_id,receipt,idempotency_key,"desktop/user-create",digest,result)
        tx.commit()
        return result | {"token":token}
    except BaseException:
        # Keep the key after an acknowledged/ambiguous durable commit. Delete
        # only a preparation whose user definitively does not exist.
        try:
            if store.get("users",user_id) is None:
                _platform.unlink_quietly(credential_file)
        except (StoreError,OSError):
            pass
        raise


def _private_key_directory(store):
    """<database>\\private-user-keys: a real directory owned by this user with the
    private DACL (Linux: an owned 0700 directory). Volumes without persistent ACLs
    (FAT/exFAT) cannot keep a key private, so they fail closed."""
    directory = Path(store.root)/"private-user-keys"
    try:
        _platform.mkdir_private(directory,exist_ok=True)
    except (FileExistsError,PermissionError) as exc:
        raise StoreError("private user credential directory must be an owned real directory") from exc
    except OSError as exc:
        if exc.errno == errno.ELOOP:
            raise StoreError("private user credential directory must be an owned real directory") from exc
        raise
    # mkdir_private verified link, type and owner and re-applied the private DACL; a
    # volume without persistent ACLs has nothing it could verify or apply.
    if not _platform.acl_supported(directory):
        raise StoreError("private user credential directory requires an NTFS/ReFS volume")
    return directory


def create_campaign_version(store,principal,station_id,campaign_id,expected_revision,name,
                            duration_seconds,valid_from_time,valid_to_time,file_reference,idempotency_key):
    sequence=store.sequence
    station_id,_=_station(store,principal,station_id,"edit")
    _text(name,"name",80,True);_text(file_reference,"file_reference",4096)
    _int(duration_seconds,"duration_seconds",1,600)
    _time(valid_from_time,"valid_from_time",optional=True);_time(valid_to_time,"valid_to_time",optional=True)
    payload={"campaign_id":campaign_id,"expected_revision":expected_revision,"name":name,"duration_seconds":duration_seconds,"valid_from_time":valid_from_time,"valid_to_time":valid_to_time,"file_reference":file_reference}
    receipt,digest,replay=operation_replay(store,principal,station_id,idempotency_key,"desktop/campaign-version",payload)
    if replay is not None:return replay
    campaign=_campaign(store,station_id,campaign_id)
    if type(expected_revision) is not int or campaign["_rev"] != expected_revision or campaign["status"] != "draft":
        raise Conflict("only current draft campaign can receive a native spot draft version")
    item_id=campaign.get("desktop_item_id")
    if not item_id:raise Conflict("campaign has no native advertisement item")
    item=store.get("items",item_id)
    if not item or item["kind"] != "advertisement":raise Conflict("campaign advertisement item is absent")
    versions=store.lookup("versions","item",item_id)
    version_no=max((row["version_no"] for row in versions),default=0)+1
    version_id=str(uuid4());now=datetime.now(timezone.utc).isoformat()
    body={"item_id":item_id,"version_no":version_no,"label":name,"status":"draft","created_by":principal.user_id,"created_at":now,"desktop_metadata":{"duration_seconds":duration_seconds,"valid_from_time":valid_from_time,"valid_to_time":valid_to_time,"file_reference":file_reference,"media_state":"unverified"}}
    fields=deepcopy(campaign["desktop_fields"]);fields.update(version_label=name,duration_seconds=duration_seconds)
    campaign_body={k:v for k,v in campaign.items() if k not in {"_id","_rev"}}|{"desktop_version_id":version_id,"desktop_fields":fields}
    result=body|{"_id":version_id,"_rev":1,"campaign_id":campaign["_id"],"campaign_revision":campaign["_rev"]+1}
    tx=store.begin(actor=principal.user_id);tx.expect_sequence(sequence)
    tx.put("versions",version_id,body)
    tx.put("campaigns",campaign["_id"],campaign_body,expected_revision=expected_revision)
    add_operation(tx,principal,station_id,receipt,idempotency_key,"desktop/campaign-version",digest,result)
    tx.commit();return result
