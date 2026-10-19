"""Version- and media-bound radio cues requiring explicit human approval."""

from __future__ import annotations

from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
from uuid import UUID, uuid4

from .identity import Principal, require
from .storage import Conflict, Store
from ._production_common import (ProductionError, _id, _body, _now,
                                 _version_in_station, _verified_media)


CUE_KINDS = frozenset({"audio_start", "intro_end", "vocal_start", "hook_start",
                       "hook_end", "outro_start", "segue_in", "segue_out",
                       "fade_start", "audio_end"})
CUE_UNITS = frozenset({"milliseconds", "seconds", "samples"})
CUE_SOURCES = frozenset({"manual", "rekordbox_xml", "media_analysis", "external"})


def _cue_position(value: str | int | float | Decimal, unit: str,
                  sample_rate_hz: int | None) -> tuple[str, int]:
    if unit not in CUE_UNITS or isinstance(value, bool):
        raise ProductionError("cue unit or position is invalid")
    if unit != "samples" and sample_rate_hz is not None:
        raise ProductionError("sample_rate_hz applies only to sample positions")
    try:
        number = Decimal(str(value))
    except (InvalidOperation, ValueError) as exc:
        raise ProductionError("cue position is not decimal") from exc
    if not number.is_finite() or number < 0:
        raise ProductionError("cue position must be finite and nonnegative")
    if unit == "samples":
        if number != number.to_integral_value() or isinstance(sample_rate_hz, bool) or \
                not isinstance(sample_rate_hz, int) or sample_rate_hz <= 0:
            raise ProductionError("sample cue needs integer samples and a sample rate")
        milliseconds = number * 1000 / sample_rate_hz
    elif unit == "seconds":
        milliseconds = number * 1000
    else:
        milliseconds = number
    if milliseconds > 7 * 24 * 60 * 60 * 1000:
        raise ProductionError("cue position exceeds seven days")
    return str(number), int(milliseconds.to_integral_value(rounding=ROUND_HALF_UP))


def create_radio_cue(
    store: Store, principal: Principal, station_id: str | UUID,
    version_id: str | UUID, *, cue_kind: str,
    position_value: str | int | float | Decimal, unit: str,
    source_kind: str = "manual", source_ref: str | None = None,
    source_raw: str | None = None, sample_rate_hz: int | None = None,
    review_reason: str | None = None, media_file_id: str | UUID | None = None,
    _analysis_dependency: tuple[str, int] | None = None,
) -> dict:
    """Create a draft cue; source DJ marks do not become approved radio cues."""
    station, version_id = _id(station_id, "station_id"), _id(version_id, "version_id")
    sequence = store.sequence
    require(store, principal, "edit", station)
    version, _ = _version_in_station(store, station, version_id)
    if cue_kind not in CUE_KINDS or source_kind not in CUE_SOURCES:
        raise ProductionError("unsupported cue kind or source")
    if source_kind != "manual" and (not isinstance(source_ref, str) or not source_ref.strip()):
        raise ProductionError("nonmanual cue must identify its source")
    if source_kind != "manual" and (not isinstance(review_reason, str) or
                                     not review_reason.strip()):
        raise ProductionError("source-derived cue needs explicit radio semantic justification")
    raw_position, position_ms = _cue_position(position_value, unit, sample_rate_hz)
    media_id = _id(media_file_id, "media_file_id") if media_file_id else None
    media = None
    if media_id:
        media = store.get("media_files", media_id)
        if media is None or media.get("version_id") != version_id:
            raise ProductionError("cue media file belongs to another content version")
    body = {"station_id": station, "version_id": version_id,
            "media_file_id": media_id, "cue_kind": cue_kind,
            "position_value": raw_position, "unit": unit,
            "position_ms": position_ms, "sample_rate_hz": sample_rate_hz,
            "source_kind": source_kind, "source_ref": source_ref,
            "source_raw": source_raw,
            "review_reason": review_reason.strip() if isinstance(review_reason, str) else None,
            "status": "draft", "created_by": principal.user_id,
            "created_at": _now()}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.expect("versions", version_id, version["_rev"])
    if media:
        tx.expect("media_files", media_id, media["_rev"])
    if _analysis_dependency:
        tx.expect("dj_analysis", _analysis_dependency[0], _analysis_dependency[1])
    return tx.put("radio_cues", str(uuid4()), body).commit()[0]


def propose_radio_cue_from_dj_mark(
    store: Store, principal: Principal, station_id: str | UUID,
    analysis_id: str | UUID, mark_ordinal: int, *, cue_kind: str,
    review_reason: str, mark_attribute: str = "Start",
) -> dict:
    """Copy one explicitly chosen DJ time as a draft, retaining its raw value."""
    station = _id(station_id, "station_id")
    require(store, principal, "edit", station)
    analysis = store.get("dj_analysis", _id(analysis_id, "analysis_id"))
    if analysis is None or not isinstance(mark_ordinal, int) or isinstance(mark_ordinal, bool) or \
            mark_ordinal < 0 or mark_ordinal >= len(analysis.get("marks", [])):
        raise ProductionError("DJ mark is absent")
    _version_in_station(store, station, analysis["version_id"])
    if mark_attribute not in {"Start", "End"}:
        raise ProductionError("only documented DJ mark time fields are accepted")
    mark = analysis["marks"][mark_ordinal]
    if mark.get("source_unit") != "seconds":
        raise ProductionError("DJ mark time unit is not verified as seconds")
    field = mark.get("attributes", {}).get(mark_attribute, {})
    if field.get("status") != "value":
        raise ProductionError("selected DJ mark time is not valid")
    return create_radio_cue(
        store, principal, station, analysis["version_id"],
        cue_kind=cue_kind, position_value=field["value"], unit="seconds",
        source_kind="rekordbox_xml",
        source_ref=f"dj_analysis:{analysis['_id']}:mark:{mark_ordinal}:{mark_attribute}",
        source_raw=field.get("raw"), review_reason=review_reason,
        _analysis_dependency=(analysis["_id"], analysis["_rev"]),
    )


def approve_radio_cue(
    store: Store, principal: Principal, station_id: str | UUID,
    cue_id: str | UUID, *, expected_revision: int,
    media_file_id: str | UUID | None = None,
) -> dict:
    station, cue_id = _id(station_id, "station_id"), _id(cue_id, "cue_id")
    sequence = store.sequence
    require(store, principal, "approve", station)
    cue = store.get("radio_cues", cue_id)
    if cue is None or cue["station_id"] != station:
        raise ProductionError("radio cue is outside station")
    if cue["_rev"] != expected_revision or cue["status"] != "draft":
        raise Conflict("only the current draft cue can be approved")
    version, _ = _version_in_station(store, station, cue["version_id"])
    if version.get("status") != "approved":
        raise Conflict("radio cue requires an approved content version")
    selected_id = _id(media_file_id, "media_file_id") if media_file_id else cue.get("media_file_id")
    media = _verified_media(store, version["_id"], selected_id)
    technical = media.get("technical") or {}
    duration = technical.get("duration_ms")
    if not isinstance(duration, int) or duration < 1 or cue["position_ms"] > duration:
        raise Conflict("cue time exceeds verified media duration or duration is missing")
    if cue["unit"] == "samples" and cue.get("sample_rate_hz") != technical.get("sample_rate_hz"):
        raise Conflict("sample cue rate differs from verified media")
    body = _body(cue) | {"status": "approved", "media_file_id": media["_id"],
                         "media_sha256": media["sha256"],
                         "media_duration_ms": duration,
                         "approved_by": principal.user_id, "approved_at": _now()}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.expect("versions", version["_id"], version["_rev"])
    tx.expect("media_files", media["_id"], media["_rev"])
    tx.put("radio_cues", cue_id, body, expected_revision=expected_revision)
    tx.commit()
    return store.get("radio_cues", cue_id)


def withdraw_radio_cue(store: Store, principal: Principal,
                       station_id: str | UUID, cue_id: str | UUID, *,
                       expected_revision: int, reason: str) -> dict:
    station, cue_id = _id(station_id, "station_id"), _id(cue_id, "cue_id")
    require(store, principal, "approve", station)
    cue = store.get("radio_cues", cue_id)
    if cue is None or cue["station_id"] != station:
        raise ProductionError("radio cue is outside station")
    if cue["_rev"] != expected_revision or cue["status"] == "withdrawn":
        raise Conflict("radio cue changed or was already withdrawn")
    if not isinstance(reason, str) or not reason.strip():
        raise ProductionError("withdrawal reason is required")
    return store.put("radio_cues", cue_id,
                     _body(cue) | {"status": "withdrawn", "withdrawn_at": _now(),
                                   "withdrawn_by": principal.user_id,
                                   "withdrawn_reason": reason.strip()},
                     expected_revision=expected_revision, actor=principal.user_id)


def approved_cues_for_version(store: Store, station_id: str | UUID,
                              version_id: str | UUID, media_file_id: str | UUID) -> list[dict]:
    """Return only cues approved against this exact version and media digest."""
    station, version, media_id = (_id(station_id, "station_id"),
                                  _id(version_id, "version_id"),
                                  _id(media_file_id, "media_file_id"))
    media = _verified_media(store, version, media_id)
    return [row for row in store.lookup("radio_cues", "version", version)
            if row["station_id"] == station and row["status"] == "approved" and
            row.get("media_file_id") == media_id and row.get("media_sha256") == media["sha256"]]


