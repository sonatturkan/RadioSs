"""Read-only preview of the mAirList 3.1 manual's 20-field library CSV.

Source: ``mAirList-3.1-User-Manual.pdf``, PDF pages 127–128, figure 11.1.
This is the documented 2011 layout, not a promise about current mAirList.
No function here opens referenced audio, writes SDBASE records, or approves cues.
"""

from __future__ import annotations

import csv
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
import hashlib
import io
from pathlib import Path
import re
from typing import Any

from .storage import StoreError


class Mairlist31CSVError(StoreError):
    """The historical CSV source cannot be safely previewed."""


FIELDS = (
    "internal_id", "external_id", "file_path", "title", "artists",
    "ending", "duration", "comment", "cue_in", "ramp_1", "ramp_2",
    "ramp_3", "hook_in", "hook_fade", "hook_out", "outro",
    "start_next", "fade_out", "cue_out", "anchor",
)
CUE_FIELDS = FIELDS[8:]
_MAX_BYTES = 16 * 1024 * 1024
_MAX_ROWS = 50_000
_MAX_CELL = 8192
_MAX_MILLISECONDS = 7 * 24 * 60 * 60 * 1000
_NAMESPACE = re.compile(r"[A-Za-z0-9_.:-]{1,128}\Z")

# A proposed semantic translation is deliberately separate from numeric unit
# conversion. Ramp 1/2/3, Hook Fade and Anchor need a station-specific choice.
CUE_KIND_CANDIDATES = {
    "cue_in": "audio_start",
    "ramp_1": None, "ramp_2": None, "ramp_3": None,
    "hook_in": "hook_start", "hook_fade": None,
    "hook_out": "hook_end", "outro": "outro_start",
    "start_next": "segue_out", "fade_out": "fade_start",
    "cue_out": "audio_end", "anchor": None,
}


def _cell(value: str) -> dict[str, str]:
    if value == "":
        return {"status": "blank", "raw": value}
    if value == "-":
        return {"status": "keep_existing", "raw": value}
    return {"status": "value", "raw": value}


def _seconds(value: str, field: str, row_number: int,
             errors: list[dict], losses: list[dict]) -> dict[str, Any] | None:
    if value in {"", "-"}:
        return None
    try:
        seconds = Decimal(value)
    except InvalidOperation:
        errors.append({"row": row_number, "field": field,
                       "code": "invalid_seconds", "raw": value})
        return None
    if not seconds.is_finite() or seconds < 0 or seconds * 1000 > _MAX_MILLISECONDS:
        errors.append({"row": row_number, "field": field,
                       "code": "seconds_out_of_range", "raw": value})
        return None
    exact = seconds * 1000
    milliseconds = int(exact.to_integral_value(rounding=ROUND_HALF_UP))
    if exact != milliseconds:
        losses.append({"row": row_number, "field": field,
                       "code": "submillisecond_rounded",
                       "raw": value, "proposed_ms": milliseconds})
    return {"source_raw": value, "source_unit": "seconds",
            "seconds": str(seconds), "milliseconds": milliseconds}


def preview_mairlist31_csv(path: str | Path, *, source_namespace: str,
                           encoding: str = "utf-8-sig",
                           max_rows: int = _MAX_ROWS) -> dict:
    """Parse a 20-column import file and report proposed mappings and losses.

    ``-`` is the manual's update-only "keep existing" sentinel.  Duplicate
    External IDs are legal in mAirList but ambiguous as SDBASE identity keys.
    This function always returns ``can_apply=False`` and performs no writes.
    """
    if not isinstance(source_namespace, str) or not _NAMESPACE.fullmatch(source_namespace):
        raise Mairlist31CSVError("source_namespace must be 1..128 safe characters")
    if type(max_rows) is not int or not 1 <= max_rows <= _MAX_ROWS:
        raise Mairlist31CSVError("max_rows must be 1..50000")
    source = Path(path).expanduser()
    try:
        if not source.is_file() or source.stat().st_size > _MAX_BYTES:
            raise Mairlist31CSVError("CSV source is absent or exceeds 16 MiB")
        raw = source.read_bytes()
    except OSError as exc:
        raise Mairlist31CSVError("CSV source cannot be read") from exc
    if len(raw) > _MAX_BYTES or b"\x00" in raw:
        raise Mairlist31CSVError("CSV source exceeds 16 MiB or contains NUL")
    try:
        text = raw.decode(encoding, errors="strict")
    except (LookupError, UnicodeError) as exc:
        raise Mairlist31CSVError("CSV source encoding is invalid; specify it explicitly") from exc

    entries: list[dict] = []
    losses: list[dict] = []
    external_occurrences: dict[str, list[int]] = {}
    path_occurrences: dict[str, list[int]] = {}
    try:
        reader = csv.reader(io.StringIO(text, newline=""), strict=True)
        for row_number, values in enumerate(reader, 1):
            if row_number > max_rows:
                raise Mairlist31CSVError("CSV exceeds the configured row limit")
            errors: list[dict] = []
            if len(values) != len(FIELDS):
                errors.append({"row": row_number, "field": None,
                               "code": "field_count", "expected": len(FIELDS),
                               "actual": len(values)})
                losses.append({"row": row_number, "field": None,
                               "code": "unsupported_extra_columns" if len(values) > len(FIELDS)
                               else "missing_documented_columns",
                               "raw_values": values[len(FIELDS):] if len(values) > len(FIELDS)
                               else values})
                entries.append({"row": row_number, "source_line": reader.line_num,
                                "raw_values": values, "status": "invalid",
                                "errors": errors, "proposals": None})
                continue
            if any(len(value) > _MAX_CELL for value in values):
                errors.append({"row": row_number, "field": None,
                               "code": "cell_too_long"})
            fields = dict(zip(FIELDS, values))
            if fields["internal_id"] != "":
                errors.append({"row": row_number, "field": "internal_id",
                               "code": "import_internal_id_must_be_blank"})
            if fields["file_path"] in {"", "-"}:
                errors.append({"row": row_number, "field": "file_path",
                               "code": "required_file_path"})
            if fields["external_id"] not in {"", "-"}:
                external_occurrences.setdefault(fields["external_id"], []).append(row_number)
            if fields["file_path"] not in {"", "-"}:
                path_occurrences.setdefault(fields["file_path"], []).append(row_number)
            duration = _seconds(fields["duration"], "duration", row_number, errors, losses)
            cues = []
            for field in CUE_FIELDS:
                converted = _seconds(fields[field], field, row_number, errors, losses)
                if converted is None:
                    continue
                candidate = CUE_KIND_CANDIDATES[field]
                cues.append({"source_field": field, **converted,
                             "relative_to": "file_start",
                             "candidate_sdb_kind": candidate,
                             "status": "review_required"})
                if candidate is None:
                    losses.append({"row": row_number, "field": field,
                                   "code": "cue_semantic_mapping_required",
                                   "raw": fields[field]})
            if duration is not None:
                for cue in cues:
                    if cue["milliseconds"] > duration["milliseconds"]:
                        errors.append({"row": row_number,
                                       "field": cue["source_field"],
                                       "code": "cue_exceeds_declared_duration"})
            for field in ("ending", "comment"):
                if fields[field] not in {"", "-"}:
                    losses.append({"row": row_number, "field": field,
                                   "code": "no_canonical_sdb_field",
                                   "raw": fields[field]})
            entry = {"row": row_number, "source_line": reader.line_num,
                     "raw_fields": fields,
                     "field_states": {name: _cell(value) for name, value in fields.items()},
                     "status": "invalid" if errors else "review_required",
                     "errors": errors,
                     "identity_resolution": "manual",
                     "proposals": {
                         "source_path": fields["file_path"],
                         "external_id": fields["external_id"]
                         if fields["external_id"] not in {"", "-"} else None,
                         "title": fields["title"] if fields["title"] not in {"", "-"} else None,
                         "artist_text": fields["artists"]
                         if fields["artists"] not in {"", "-"} else None,
                         "duration_ms": duration["milliseconds"] if duration else None,
                         "duration_source": duration,
                         "cues": cues,
                     }}
            entries.append(entry)
    except (csv.Error, UnicodeError) as exc:
        raise Mairlist31CSVError("malformed CSV quoting or record structure") from exc

    ambiguities: list[dict] = []
    for field, occurrences in (("external_id", external_occurrences),
                               ("file_path", path_occurrences)):
        for value, rows in occurrences.items():
            if len(rows) > 1:
                ambiguities.append({"field": field, "value": value,
                                    "rows": rows, "code": "duplicate_identity_candidate"})
                for row in rows:
                    if entries[row - 1]["status"] != "invalid":
                        entries[row - 1]["status"] = "ambiguous"
    return {"format": "mairlist-3.1-csv-20-preview-v1",
            "manual_pdf_pages": [127, 128],
            "source_namespace": source_namespace,
            "source_sha256": hashlib.sha256(raw).hexdigest(),
            "source_bytes": len(raw), "encoding": encoding, "row_count": len(entries),
            "valid_for_review": sum(entry["status"] == "review_required" for entry in entries),
            "invalid_rows": sum(entry["status"] == "invalid" for entry in entries),
            "can_apply": False, "requires_human_review": True,
            "entries": entries, "ambiguities": ambiguities,
            "losses": losses,
            "limits": ["Historical mAirList 3.1 layout only.",
                       "No audio files are read or imported.",
                       "Source paths are evidence, not SDBASE identities.",
                       "Cues remain unapproved review proposals."]}
