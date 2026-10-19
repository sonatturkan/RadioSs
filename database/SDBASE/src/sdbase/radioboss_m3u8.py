"""Bounded preview of RadioBOSS's documented plain UTF-8 M3U8 playlist.

This deliberately accepts only the absolute-file-path subset shown in the
RadioBOSS manual.  RadioBOSS commands, variables, containers, URLs, and other
directives must be handled by a separately documented adapter.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import re
from typing import Any

from .storage import StoreError


FORMAT = "radioboss-plain-m3u8-v1"
MAX_BYTES = 2 * 1024 * 1024
MAX_TRACKS = 10_000
MAX_LINE = 8192
MAX_SECONDS = 7 * 24 * 60 * 60
_EXTINF = re.compile(r"#EXTINF:([0-9]+),(.+)\Z")
_WINDOWS_PATH = re.compile(r"(?:[A-Za-z]:[\\/]|\\\\[^\\/]+[\\/][^\\/]+[\\/]).+\Z")
_LINE_BREAKS = "\r\n\x0b\x0c\x1c\x1d\x1e\x85\u2028\u2029"


class RadioBOSSPlaylistError(StoreError):
    """The source cannot be safely parsed as the documented playlist subset."""


def absolute_media_path(value: Any) -> bool:
    """Allow explicit POSIX, drive-rooted Windows, or UNC media paths only."""
    if not isinstance(value, str) or not value or value.endswith(("/", "\\")) or any(
            ord(char) < 32 or ord(char) == 127 or char in _LINE_BREAKS
            for char in value):
        return False
    return (value.startswith("/") and not value.startswith("//")) or bool(
        _WINDOWS_PATH.fullmatch(value))


def _read(path: str | Path) -> bytes:
    source = Path(path).expanduser()
    if source.suffix.lower() != ".m3u8":
        raise RadioBOSSPlaylistError("source must have a .m3u8 extension")
    try:
        if not source.is_file() or source.stat().st_size > MAX_BYTES:
            raise RadioBOSSPlaylistError("source is absent or exceeds 2 MiB")
        raw = source.read_bytes()
    except OSError as exc:
        raise RadioBOSSPlaylistError("playlist cannot be read") from exc
    if len(raw) > MAX_BYTES or b"\x00" in raw:
        raise RadioBOSSPlaylistError("playlist exceeds 2 MiB or contains NUL")
    return raw


def preview_radioboss_m3u8(path: str | Path) -> dict:
    """Return exact-byte identity, row diagnostics and explicit model losses.

    `#EXTINF` seconds and display text are source hints, not measured media
    facts. No referenced audio is opened and no SDBASE data is written.
    """
    raw = _read(path)
    try:
        decoded = raw.decode("utf-8-sig", errors="strict")
    except UnicodeError as exc:
        raise RadioBOSSPlaylistError("M3U8 must be UTF-8") from exc
    lines = decoded.splitlines()
    if any(len(line) > MAX_LINE for line in lines):
        raise RadioBOSSPlaylistError("playlist line exceeds 8192 characters")
    issues: list[dict] = []
    losses: list[dict] = []
    entries: list[dict] = []
    pending: tuple[int, str, int | None, str | None] | None = None
    header_seen = False
    for line_no, line in enumerate(lines, 1):
        if not line.strip():
            continue
        if not header_seen:
            header_seen = True
            if line != "#EXTM3U":
                issues.append({"line": line_no, "code": "missing_extm3u_header"})
            if line == "#EXTM3U":
                continue
        if line == "#EXTM3U":
            issues.append({"line": line_no, "code": "duplicate_extm3u_header"})
            continue
        if line.startswith("#EXTINF:"):
            if pending is not None:
                issues.append({"line": pending[0], "code": "extinf_without_path"})
            match = _EXTINF.fullmatch(line)
            if match is None:
                issues.append({"line": line_no, "code": "unsupported_extinf_syntax",
                               "raw": line})
                pending = (line_no, line, None, None)
                continue
            seconds = int(match.group(1))
            if seconds > MAX_SECONDS:
                issues.append({"line": line_no, "code": "extinf_duration_out_of_range"})
            pending = (line_no, line, seconds if seconds <= MAX_SECONDS else None,
                       match.group(2))
            continue
        if line.startswith("#"):
            issues.append({"line": line_no, "code": "unsupported_directive",
                           "raw": line})
            continue
        if len(entries) >= MAX_TRACKS:
            raise RadioBOSSPlaylistError("playlist exceeds 10000 tracks")
        path_errors = []
        if not absolute_media_path(line):
            path_errors.append("absolute_local_path_required")
        if "?" in line or "%" in line:
            path_errors.append("dynamic_path_or_macro_unsupported")
        if pending is None:
            losses.append({"row": len(entries) + 1, "code": "extinf_absent"})
        elif pending[2] is None:
            path_errors.append("extinf_invalid")
        entries.append({
            "row": len(entries) + 1, "source_line": line_no,
            "source_path": line, "extinf_line": pending[0] if pending else None,
            "extinf_raw": pending[1] if pending else None,
            "declared_duration_seconds": pending[2] if pending else None,
            "display_title": pending[3] if pending else None,
            "status": "invalid" if path_errors else "review_required",
            "errors": path_errors,
        })
        pending = None
    if pending is not None:
        issues.append({"line": pending[0], "code": "extinf_without_path"})
    if not header_seen:
        issues.append({"line": 0, "code": "empty_playlist"})
    if not entries:
        issues.append({"line": 0, "code": "no_media_paths"})
    return {
        "format": FORMAT, "source_sha256": hashlib.sha256(raw).hexdigest(),
        "source_bytes": len(raw), "encoding": "utf-8-sig", "row_count": len(entries),
        "entries": entries, "issues": issues, "losses": losses,
        "can_apply_as_draft": not issues and all(not row["errors"] for row in entries),
        "requires_human_review": True,
        "model_losses": [
            "M3U8 has no service date or absolute broadcast start times.",
            "M3U8 has no SDBASE item/version/media identity or approvals.",
            "M3U8 has no rights, output scope, campaign, cue or audit history.",
            "EXTINF display text and duration are unverified source hints.",
        ],
        "limits": [
            "Only RadioBOSS's documented plain #EXTM3U/#EXTINF absolute-file subset.",
            "Commands, dynamic variables, URLs, relative paths and unknown directives are not applied.",
            "No referenced audio is read or imported.",
        ],
    }
