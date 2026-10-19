"""Saved, ordered local Event references, separate from live programme state.

A local Event is an operator-owned recipe. Saving or reopening it never edits
its source playlists/schedules or creates playback, planning or history rows.
The native player resolves copied sources only after an explicit insert action.
"""
from __future__ import annotations

from .storage import StoreError

SOURCE_TYPES = frozenset({"playlist", "automation_calendar", "weekly_programme"})
MAX_SOURCES = 100


def validate_event_playback(playback):
    """Saved selection only; current deck membership is checked before playback."""
    from .desktop_api import _id, _int, _object, _text

    _object(playback, {"player", "next_cluster_id"}, {"player", "next_cluster_id"})
    _int(playback["player"], "event playback player", 0, 32)
    cluster = _text(playback["next_cluster_id"], "event playback next_cluster_id", 36)
    if cluster and _id(cluster, "event playback next_cluster_id") != cluster:
        raise StoreError("event playback next_cluster_id must be a canonical UUID")


def validate_local_event(config, station_id, state, *, persisted=False, previous=None):
    from .desktop_api import PROGRAMME_KINDS, _id, _int, _list, _object, _text

    fields = {"schema", "title", "kind", "sources"}
    _object(config, fields | {"playback"}, fields)
    if "playback" in config:
        validate_event_playback(config["playback"])
    _int(config["schema"], "local event schema", 1, 1)
    title = _text(config["title"], "local event title", 160, True)
    if title != title.strip():
        raise StoreError("local event title must be trimmed")
    if not isinstance(config["kind"], str) or config["kind"] not in PROGRAMME_KINDS:
        raise StoreError("local event kind must be a programme event kind")
    previous = previous if isinstance(previous, dict) else {}
    previous_sources = {source["id"]: source for source in previous.get("sources", [])}
    seen = set()
    for source in _list(config["sources"], "local event sources", MAX_SOURCES):
        _object(source, {"id", "type", "source_id"}, {"id", "type", "source_id"})
        occurrence = _id(source["id"], "local event source occurrence id")
        if occurrence != source["id"] or occurrence in seen:
            raise StoreError("local event source occurrence ids must be distinct canonical UUIDs")
        seen.add(occurrence)
        kind = source["type"]
        if not isinstance(kind, str) or kind not in SOURCE_TYPES:
            raise StoreError("local event source type is unsupported")
        identifier = _id(source["source_id"], "local event source_id")
        if identifier != source["source_id"]:
            raise StoreError("local event source_id must be a canonical UUID")
        collection = "playlists" if kind == "playlist" else "desktop_configurations"
        row = state.get(collection, {}).get(identifier, {}).get("data")
        if (not row or row.get("station_id") != station_id or
                (kind != "playlist" and row.get("kind") != kind)):
            raise StoreError("local event sources must reference playlists or schedules in this station")
        # Disabled sources remain inspectable without reviving or modifying
        # them. Existing unchanged occurrences may be reordered or renamed;
        # adding a disabled source under a new occurrence still fails.
        unchanged = source == previous_sources.get(occurrence)
        if row.get("status") != "active" and not (persisted or unchanged):
            raise StoreError("local event sources must reference active records")
