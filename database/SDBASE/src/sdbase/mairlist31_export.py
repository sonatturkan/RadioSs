"""Read-only export of an explicitly selected SDBASE view to 2011 mAirList CSV.

Only the 20 fields documented in the mAirList 3.1 manual (PDF pages 127–128)
are emitted.  The result includes a loss report; it does not claim current
mAirList compatibility or copy audio to the export destination.
"""

from __future__ import annotations

import csv
from collections import defaultdict
from decimal import Decimal
import io
import ntpath
from typing import Any, Mapping, Sequence
from uuid import UUID

from .identity import Principal, require
from .mairlist31_csv import FIELDS
from .storage import Conflict, Store, StoreError


_MAX_ROWS = 50_000
_MAX_OUTPUT_BYTES = 16 * 1024 * 1024
_WATCHED = frozenset({"items", "station_items", "versions", "media_files",
                      "people", "contributions", "radio_cues"})
_EXPORT_CUE_FIELDS = {
    "audio_start": "cue_in", "hook_start": "hook_in",
    "hook_end": "hook_out", "outro_start": "outro",
    "segue_out": "start_next", "fade_start": "fade_out",
    "audio_end": "cue_out",
}
_ARTIST_ROLES = frozenset({"artist", "performer", "primary_artist",
                           "main_artist", "featured_artist"})


def _id(value: Any, name: str) -> str:
    try:
        return str(UUID(str(value)))
    except (ValueError, TypeError, AttributeError) as exc:
        raise StoreError(f"{name} must be a UUID") from exc


def _seconds(milliseconds: int) -> str:
    return format(Decimal(milliseconds) / Decimal(1000), "f")


def _absolute_path(value: Any) -> bool:
    """A destination path for the mAirList (Windows) machine: drive-absolute
    (``C:\\x``, ``C:/x``) or UNC.  Rooted (``\\x``) and drive-relative (``C:x``) paths
    are rejected, as are control characters (never valid in Windows paths; a CR/LF
    would also split the CSV record in mAirList)."""
    return isinstance(value, str) and bool(value) and \
        not any(ord(character) < 32 or ord(character) == 127 for character in value) and \
        ntpath.isabs(value)


def _duration_ms(media: dict) -> int | None:
    technical = media.get("technical")
    value = technical.get("duration_ms") if isinstance(technical, dict) else None
    if value is None:
        value = media.get("duration_ms")
    return value if type(value) is int and value >= 0 else None


def _artists(contributions: Mapping[str, list[dict]], people: Mapping[str, dict],
             item_id: str, losses: list[dict]) -> str:
    names: list[str] = []
    other_roles: set[str] = set()
    for contribution in contributions.get(item_id, []):
        role = contribution.get("role")
        if role not in _ARTIST_ROLES:
            other_roles.add(str(role))
            continue
        person = people.get(str(contribution.get("person_id")))
        if person and isinstance(person.get("display_name"), str):
            names.append(person["display_name"])
    unique = list(dict.fromkeys(names))
    if len(unique) > 1:
        losses.append({"item_id": item_id, "code": "artist_structure_flattened",
                       "detail": "Multiple artist identities become one plain text field."})
    if other_roles:
        losses.append({"item_id": item_id, "code": "contribution_roles_omitted",
                       "roles": sorted(other_roles)})
    return "; ".join(unique)


def _cues(store: Store, station_id: str, version_id: str, media: dict,
          item_id: str, losses: list[dict]) -> dict[str, str]:
    values: dict[str, str] = {}
    collisions: set[str] = set()
    for cue in store.lookup("radio_cues", "version", version_id):
        if cue.get("station_id") != station_id:
            continue
        if cue.get("status") != "approved" or cue.get("media_file_id") != media["_id"] or \
                cue.get("media_sha256") != media.get("sha256"):
            losses.append({"item_id": item_id, "code": "cue_not_exported",
                           "cue_id": cue["_id"],
                           "detail": "Cue is unapproved or bound to another media revision."})
            continue
        field = _EXPORT_CUE_FIELDS.get(cue["cue_kind"])
        if field is None:
            losses.append({"item_id": item_id, "code": "cue_semantic_mapping_required",
                           "cue_id": cue["_id"], "cue_kind": cue["cue_kind"]})
            continue
        milliseconds = cue.get("position_ms")
        if type(milliseconds) is not int or milliseconds < 0:
            losses.append({"item_id": item_id, "code": "invalid_cue_position",
                           "cue_id": cue["_id"]})
            continue
        if field in values:
            collisions.add(field)
        else:
            values[field] = _seconds(milliseconds)
    for field in collisions:
        values.pop(field, None)
        losses.append({"item_id": item_id, "code": "multiple_cues_for_one_csv_field",
                       "field": field})
    return values


def export_mairlist31_csv(store: Store, principal: Principal, station_id: str,
                          view_rows: Sequence[Mapping[str, Any]],
                          path_by_media_file: Mapping[str, str]) -> dict:
    """Export selected search result rows, with explicit path and loss review.

    ``view_rows`` should be the ``items`` array from ``search_items``.  Every
    selected version/media ID is rechecked against the current store.  A row
    without an approved version, verified media or supplied absolute path is
    skipped and reported; no arbitrary version is silently substituted.
    """
    station = _id(station_id, "station_id")
    require(store, principal, "view", station)
    if not isinstance(view_rows, (list, tuple)) or len(view_rows) > _MAX_ROWS or \
            not isinstance(path_by_media_file, Mapping):
        raise StoreError("export needs at most 50000 selected rows and a media path map")
    generation = store.collection_generation(_WATCHED)
    contributions: dict[str, list[dict]] = defaultdict(list)
    for row in store.scan("contributions"):
        contributions[str(row.get("item_id"))].append(row)
    people = {row["_id"]: row for row in store.scan("people")}
    output = io.StringIO(newline="")
    writer = csv.writer(output, lineterminator="\r\n")
    losses: list[dict] = []
    skipped: list[dict] = []
    emitted: list[str] = []
    seen: set[str] = set()
    for ordinal, selected in enumerate(view_rows, 1):
        if not isinstance(selected, Mapping):
            raise StoreError("selected view rows must be objects")
        item_id = _id(selected.get("id"), "view.id")
        if item_id in seen:
            skipped.append({"row": ordinal, "item_id": item_id,
                            "code": "duplicate_selected_item"})
            continue
        seen.add(item_id)
        item = store.get("items", item_id)
        if item is None or item.get("lifecycle") in {"trashed", "withdrawn"} or \
                not store.lookup("station_items", "station_item", station, item_id):
            skipped.append({"row": ordinal, "item_id": item_id,
                            "code": "item_not_available_in_station"})
            continue
        if selected.get("station_id") != station or selected.get("revision") != item["_rev"]:
            skipped.append({"row": ordinal, "item_id": item_id,
                            "code": "selected_catalog_view_is_stale"})
            continue
        try:
            version_id = _id(selected.get("version_id"), "view.version_id")
            media_id = _id(selected.get("media_file_id"), "view.media_file_id")
        except StoreError:
            skipped.append({"row": ordinal, "item_id": item_id,
                            "code": "selected_version_or_media_missing"})
            continue
        version = store.get("versions", version_id)
        media = store.get("media_files", media_id)
        if version is None or version.get("item_id") != item_id or \
                version.get("status") != "approved" or media is None or \
                media.get("version_id") != version_id or media.get("status") != "verified":
            skipped.append({"row": ordinal, "item_id": item_id,
                            "code": "selected_version_or_media_not_approved_verified"})
            continue
        path = path_by_media_file.get(media_id)
        if not _absolute_path(path):
            skipped.append({"row": ordinal, "item_id": item_id,
                            "code": "explicit_absolute_media_path_required"})
            continue
        duration = _duration_ms(media)
        fields = {name: "" for name in FIELDS}
        fields.update({"external_id": item_id, "file_path": path,
                       "title": str(item["title"]),
                       "artists": _artists(contributions, people, item_id, losses),
                       "duration": _seconds(duration) if duration is not None else ""})
        if duration is None:
            losses.append({"item_id": item_id, "code": "duration_missing"})
        fields.update(_cues(store, station, version_id, media, item_id, losses))
        writer.writerow([fields[name] for name in FIELDS])
        emitted.append(item_id)
        losses.append({"item_id": item_id, "code": "csv_model_is_narrower",
                       "omitted_sdb_concepts": [
                           "content_kind_and_lifecycle", "version_identity_and_approval",
                           "media_hash_and_verification", "station_membership_and_category",
                           "rights_and_readiness", "source_provenance_and_audit"]})
    if store.collection_generation(_WATCHED) != generation:
        raise Conflict("catalog changed during mAirList CSV export")
    csv_text = output.getvalue()
    if len(csv_text.encode("utf-8")) > _MAX_OUTPUT_BYTES:
        raise StoreError("mAirList CSV export exceeds 16 MiB")
    return {"format": "mairlist-3.1-csv-20-export-v1",
            "manual_pdf_pages": [127, 128], "station_id": station,
            "catalog_generation": generation,
            "csv_text": csv_text, "row_count": len(emitted),
            "exported_item_ids": emitted, "skipped": skipped,
            "losses": losses, "requires_human_review": True,
            "limits": ["Historical mAirList 3.1 layout only.",
                       "Audio files are not copied or path-verified.",
                       "External ID is the SDBASE item UUID; mAirList may allow duplicates.",
                       "Only approved cues for the selected exact media are candidates."]}
