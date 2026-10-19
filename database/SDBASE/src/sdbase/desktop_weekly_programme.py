"""Typed, station-scoped snapshots of bounded UTC weekly rotation programmes.

These are editable schedules, not formal approved daily programme revisions.
Saving them grants no media rights, playback gate, delivery or air evidence.
"""
from datetime import date, timedelta
from .storage import StoreError


def validate_weekly_programme(config, station_id, state, *, persisted=False, previous=None):
    from .desktop_api import _bool, _date, _int, _object
    from .desktop_automation_calendar import validate_calendar
    fields = {"schema", "week_anchor", "week_count", "repeat", "calendar"}
    _object(config, fields, fields)
    _int(config["schema"], "weekly programme schema", 1, 1)
    anchor = date.fromisoformat(_date(config["week_anchor"], "weekly programme week_anchor"))
    if anchor.weekday() != 0:
        raise StoreError("weekly programme week_anchor must be Monday in its calendar timezone")
    count = _int(config["week_count"], "weekly programme week_count", 1, 52)
    _bool(config["repeat"], "weekly programme repeat")
    try:
        end = anchor + timedelta(days=count * 7)
    except OverflowError as exc:
        raise StoreError("weekly programme period exceeds the supported date range") from exc
    validate_calendar(config["calendar"], station_id, state, persisted=persisted,
                      previous=previous.get("calendar") if previous else None)
    for event in config["calendar"]["events"]:
        if not anchor <= date.fromisoformat(event["date"]) < end:
            raise StoreError("weekly programme event lies outside its complete week period")
