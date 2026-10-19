"""Read exported rekordbox XML without accessing its database or media files.

The output is a source snapshot for a separate, policy-controlled import step.
This module never writes to the source, creates RadioS records, or marks media
as ready for broadcast.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date
from decimal import Decimal, InvalidOperation
from hashlib import sha256
import os
from pathlib import Path
import re
import stat
from typing import Mapping, Sequence
from urllib.parse import unquote_to_bytes, urlsplit
import xml.etree.ElementTree as ET


READER_VERSION = "0.2.0"
SUPPORTED_XML_VERSIONS = frozenset({"1.0.0"})
DEFAULT_MAX_BYTES = 64_000_000


def reader_capabilities() -> dict[str, dict[str, object]]:
    """Describe verified XML/DeviceSQL scope and explicitly unsupported formats."""
    return {
        "exported_xml": {"status": "supported", "format_versions": ["1.0.0"],
                         "read_only": True, "direction": "rekordbox_to_radios"},
        "local_database": {"status": "unsupported", "reason": "schema and access method are unverified"},
        "device_library": {"status": "supported", "format": "DeviceSQL export.pdb",
                           "read_only": True, "tracklists": True, "anlz": False},
        "one_library": {"status": "unsupported", "reason": "device format is unverified"},
    }


class RekordboxError(Exception):
    """Base error for an unreadable or unsafe source snapshot."""


class RekordboxFormatError(RekordboxError):
    """The XML is malformed or cannot preserve required identities."""


class UnsupportedFormatError(RekordboxFormatError):
    """The source XML format version or playlist structure is not supported."""

    def __init__(self, message: str, *, losses: Sequence[LossRecord] = ()) -> None:
        super().__init__(message)
        self.losses = tuple(losses)


class SourceChangedError(RekordboxError):
    """The exported file changed while it was being read."""


class SourceTooLargeError(RekordboxError):
    """The exported file exceeds the configured byte limit."""


@dataclass(frozen=True)
class FieldSpec:
    source: str
    target: str
    kind: str = "text"
    unit: str | None = None
    default_policy: str = "review"


# Semantic target names are adapter contract names, not physical store fields.
# A service must apply its own per-field authorization, revision and approval rules.
TRACK_FIELD_SPECS: tuple[FieldSpec, ...] = (
    FieldSpec("TrackID", "external.track_id", "integer", default_policy="preserve"),
    FieldSpec("Name", "record.title", default_policy="fill_if_empty"),
    FieldSpec("Artist", "record.artist_credit", default_policy="fill_if_empty"),
    FieldSpec("Composer", "record.composer_credit", default_policy="fill_if_empty"),
    FieldSpec("Remixer", "record.remixer_credit", default_policy="fill_if_empty"),
    FieldSpec("Album", "record.album", default_policy="fill_if_empty"),
    FieldSpec("Grouping", "record.grouping", default_policy="review"),
    FieldSpec("Genre", "record.genre", default_policy="review"),
    FieldSpec("Year", "record.year", "integer", default_policy="fill_if_empty"),
    FieldSpec("Comments", "record.comments", default_policy="review"),
    FieldSpec("Rating", "dj.rating_raw", "integer", "0..255", "review"),
    FieldSpec("PlayCount", "dj.play_count", "integer", "plays", "preserve"),
    FieldSpec("LastPlayed", "dj.last_played", "date", default_policy="preserve"),
    FieldSpec("Location", "source.media_uri", default_policy="review"),
    FieldSpec("Kind", "file.source_kind", default_policy="review"),
    FieldSpec("Size", "file.reported_size", "integer", "bytes", "review"),
    FieldSpec("TotalTime", "file.reported_duration", "decimal", "seconds", "review"),
    FieldSpec("DiscNumber", "record.disc_number", "integer", default_policy="review"),
    FieldSpec("TrackNumber", "record.track_number", "integer", default_policy="review"),
    FieldSpec("AverageBpm", "dj.average_bpm", "decimal", "BPM", "review"),
    FieldSpec("DateModified", "source.date_modified", "date", default_policy="preserve"),
    FieldSpec("DateAdded", "source.date_added", "date", default_policy="preserve"),
    FieldSpec("BitRate", "file.reported_bitrate", "integer", "kbps", "review"),
    FieldSpec("SampleRate", "file.reported_sample_rate", "decimal", "Hz", "review"),
    FieldSpec("Tonality", "dj.tonality", default_policy="review"),
    FieldSpec("Label", "record.label", default_policy="review"),
    FieldSpec("Mix", "record.mix", default_policy="review"),
    FieldSpec("Colour", "dj.colour", default_policy="review"),
)

TEMPO_FIELD_SPECS: tuple[FieldSpec, ...] = (
    FieldSpec("Inizio", "dj.beatgrid.start", "decimal", "seconds"),
    FieldSpec("Bpm", "dj.beatgrid.bpm", "decimal", "BPM"),
    FieldSpec("Metro", "dj.beatgrid.meter"),
    FieldSpec("Battito", "dj.beatgrid.beat", "integer", "beat number"),
)

MARK_FIELD_SPECS: tuple[FieldSpec, ...] = (
    FieldSpec("Name", "dj.position_mark.name"),
    FieldSpec("Type", "dj.position_mark.type", "integer"),
    FieldSpec("Start", "dj.position_mark.start", "decimal", "seconds"),
    FieldSpec("End", "dj.position_mark.end", "decimal", "seconds"),
    # The official format table says integer, yet its notes list A/B/C hot cues.
    FieldSpec("Num", "dj.position_mark.number"),
)


@dataclass(frozen=True)
class SourceValue:
    """A field's value and presence are distinct; only 'value' is importable."""

    status: str  # value, empty, missing, invalid
    value: str | int | Decimal | date | None = None
    raw: str | None = None


@dataclass(frozen=True)
class LossRecord:
    """A source-to-snapshot limitation; no target field should be cleared for it."""

    kind: str  # reader_unavailable, unsupported, invalid, unresolved, count_mismatch
    object_key: str
    source_field: str
    detail: str


@dataclass(frozen=True)
class TempoPoint:
    attributes: Mapping[str, SourceValue]
    unmapped_attributes: Mapping[str, str]


@dataclass(frozen=True)
class PositionMark:
    attributes: Mapping[str, SourceValue]
    unmapped_attributes: Mapping[str, str]


@dataclass(frozen=True)
class Track:
    track_id: str
    fields: Mapping[str, SourceValue]
    source_path: str | None
    tempos: tuple[TempoPoint, ...]
    marks: tuple[PositionMark, ...]
    unmapped_attributes: Mapping[str, str]

    def mapped_values(self) -> dict[str, str | int | Decimal | date]:
        """Return source proposals, excluding absent, empty and invalid fields.

        This method does not apply update policies or approvals. In particular,
        ``dj.*`` values are never RadioS broadcast events or approved cues.
        """

        return {
            spec.target: field.value
            for spec in TRACK_FIELD_SPECS
            if (field := self.fields[spec.source]).status == "value"
            and field.value is not None
        }


@dataclass(frozen=True)
class PlaylistEntry:
    position: int  # zero-based, preserving duplicate entries and XML order
    source_key: str
    track_id: str | None


@dataclass(frozen=True)
class PlaylistNode:
    name: str
    kind: str  # folder or playlist
    path: tuple[int, ...]  # child ordinals; stable within one XML snapshot
    children: tuple[PlaylistNode, ...] = ()
    entries: tuple[PlaylistEntry, ...] = ()
    key_type: int | None = None  # playlist: 0=TrackID, 1=Location


@dataclass(frozen=True)
class RekordboxLibrary:
    format_version: str
    product_name: str | None
    product_version: str | None
    product_company: str | None
    source_sha256: str
    reader_version: str
    tracks: Mapping[str, Track]
    root_nodes: tuple[PlaylistNode, ...]
    losses: tuple[LossRecord, ...]

    @property
    def playlists(self) -> tuple[PlaylistNode, ...]:
        """All actual playlists, in source traversal order."""

        return tuple(node for root in self.root_nodes for node in _walk(root) if node.kind == "playlist")


@dataclass(frozen=True)
class SourceTrackKey:
    source_system: str
    source_library_id: str
    source_track_id: str


@dataclass(frozen=True)
class ImportPlan:
    source_sha256: str
    reader_version: str
    track_keys: tuple[SourceTrackKey, ...]
    selected_nodes: tuple[PlaylistNode, ...]
    selected_playlists: tuple[PlaylistNode, ...]
    partial: bool
    deletion_inference_allowed: bool = False


@dataclass(frozen=True)
class PathMapRule:
    """Map an explicit source media root to an allowed local target root."""

    source_root: str
    target_root: str
    case_sensitive: bool | None = None  # None: Windows drives/UNC insensitive


@dataclass(frozen=True)
class PathMapping:
    status: str  # mapped, unmapped, invalid
    source_path: str | None
    target_path: str | None
    detail: str


_INT_RE = re.compile(r"[+-]?\d+\Z")
_BAD_PERCENT_RE = re.compile(r"%(?![0-9A-Fa-f]{2})")
_DECLARATION_RE = re.compile(r"<!\s*(?:DOCTYPE|ENTITY)\b", re.IGNORECASE)
_XML_ENCODING_RE = re.compile(r"<\?xml\s+[^?]*encoding\s*=\s*['\"]([^'\"]+)['\"]", re.IGNORECASE)
_WINDOWS_DRIVE_RE = re.compile(r"^[A-Za-z]:/")


def _value(raw: str | None, kind: str) -> SourceValue:
    if raw is None:
        return SourceValue("missing")
    if raw == "":
        return SourceValue("empty", raw=raw)
    try:
        if kind == "integer":
            if not _INT_RE.fullmatch(raw):
                raise ValueError("not an integer")
            value: str | int | Decimal | date = int(raw)
        elif kind == "decimal":
            # The published format's note permits dot or comma decimal separators.
            if "," in raw and "." in raw:
                raise ValueError("mixed decimal separators")
            value = Decimal(raw.replace(",", "."))
            if not value.is_finite():
                raise ValueError("non-finite decimal")
        elif kind == "date":
            value = date.fromisoformat(raw)
        else:
            value = raw
    except (ValueError, InvalidOperation):
        return SourceValue("invalid", raw=raw)
    return SourceValue("value", value=value, raw=raw)


def _fields(
    attributes: Mapping[str, str],
    specs: tuple[FieldSpec, ...],
    object_key: str,
    losses: list[LossRecord],
) -> tuple[dict[str, SourceValue], dict[str, str]]:
    fields = {spec.source: _value(attributes.get(spec.source), spec.kind) for spec in specs}
    for name, field in fields.items():
        if field.status == "invalid":
            losses.append(LossRecord("invalid", object_key, name, "Field could not be converted; raw value retained."))
    known = set(fields)
    unknown = {name: raw for name, raw in attributes.items() if name not in known}
    for name in unknown:
        losses.append(LossRecord("unsupported", object_key, name, "Unmapped XML attribute retained for review."))
    return fields, unknown


def _source_path(uri: SourceValue, track_id: str, losses: list[LossRecord]) -> str | None:
    if uri.status != "value" or not isinstance(uri.value, str):
        return None
    raw = uri.value
    if _BAD_PERCENT_RE.search(raw):
        losses.append(LossRecord("invalid", track_id, "Location", "Invalid URI percent encoding."))
        return None
    try:
        parts = urlsplit(raw)
        if parts.scheme.lower() != "file" or parts.query or parts.fragment:
            raise ValueError("Only file URIs without query or fragment are supported")
        if not parts.path:
            raise ValueError("File URI has no path")
        path = unquote_to_bytes(parts.path).decode("utf-8", "strict")
        host = parts.netloc
        if host and host.lower() != "localhost":
            path = f"//{host}{path}"
        elif re.match(r"^/[A-Za-z]:/", path):
            path = path[1:]  # Windows drive in a file://localhost/C:/... URI
        return path
    except (ValueError, UnicodeDecodeError):
        losses.append(LossRecord("invalid", track_id, "Location", "URI is not a supported UTF-8 file URI."))
        return None


def _track(element: ET.Element, losses: list[LossRecord]) -> Track:
    identity = _value(element.get("TrackID"), "integer")
    if identity.status != "value" or not isinstance(identity.value, int):
        raise RekordboxFormatError("A COLLECTION TRACK has no valid TrackID; stable identity is required")
    track_id = str(identity.value)
    fields, unknown = _fields(element.attrib, TRACK_FIELD_SPECS, track_id, losses)
    tempos: list[TempoPoint] = []
    marks: list[PositionMark] = []
    for child in element:
        if child.tag == "TEMPO":
            attrs, unmapped = _fields(child.attrib, TEMPO_FIELD_SPECS, f"track:{track_id}:tempo:{len(tempos)}", losses)
            tempos.append(TempoPoint(attrs, unmapped))
        elif child.tag == "POSITION_MARK":
            attrs, unmapped = _fields(child.attrib, MARK_FIELD_SPECS, f"track:{track_id}:mark:{len(marks)}", losses)
            marks.append(PositionMark(attrs, unmapped))
        else:
            losses.append(LossRecord("unsupported", track_id, child.tag, "Unmapped TRACK child element; not imported."))
    return Track(track_id, fields, _source_path(fields["Location"], track_id, losses), tuple(tempos), tuple(marks), unknown)


def _count_check(element: ET.Element, attribute: str, actual: int, key: str, losses: list[LossRecord]) -> None:
    declared = _value(element.get(attribute), "integer")
    if declared.status == "invalid":
        losses.append(LossRecord("invalid", key, attribute, "Declared count is invalid."))
    elif declared.status == "value" and declared.value != actual:
        losses.append(LossRecord("count_mismatch", key, attribute, f"Declared {declared.value}; observed {actual}."))


def _node(
    element: ET.Element,
    path: tuple[int, ...],
    tracks: Mapping[str, Track],
    by_uri: Mapping[str, list[str]],
    by_path: Mapping[str, list[str]],
    losses: list[LossRecord],
) -> PlaylistNode:
    node_type = _value(element.get("Type"), "integer")
    if node_type.status != "value" or node_type.value not in (0, 1):
        raise UnsupportedFormatError(f"Unsupported playlist NODE Type at path {path}")
    name = element.get("Name", "")
    if not name:
        losses.append(LossRecord("empty", str(path), "NODE.Name", "Node name is absent or empty."))
    object_key = f"playlist:{path}"
    known_node_attributes = {"Type", "Name", "Count"} if node_type.value == 0 else {"Type", "Name", "Entries", "KeyType"}
    for attribute in element.attrib.keys() - known_node_attributes:
        losses.append(LossRecord("unsupported", object_key, attribute, "Unmapped NODE attribute."))
    if node_type.value == 0:
        children: list[PlaylistNode] = []
        for child in element:
            if child.tag == "NODE":
                children.append(_node(child, path + (len(children),), tracks, by_uri, by_path, losses))
            else:
                losses.append(LossRecord("unsupported", object_key, child.tag, "Unmapped folder child element."))
        _count_check(element, "Count", len(children), object_key, losses)
        return PlaylistNode(name, "folder", path, children=tuple(children))

    key_type = _value(element.get("KeyType"), "integer")
    if key_type.status != "value" or key_type.value not in (0, 1):
        raise UnsupportedFormatError(f"Unsupported playlist KeyType at path {path}")
    entries: list[PlaylistEntry] = []
    for child in element:
        if child.tag != "TRACK":
            losses.append(LossRecord("unsupported", object_key, child.tag, "Unmapped playlist child element."))
            continue
        key = child.get("Key", "")
        for attribute in child.attrib.keys() - {"Key"}:
            losses.append(LossRecord("unsupported", object_key, attribute, "Unmapped playlist TRACK attribute."))
        matched: str | None = None
        if key_type.value == 0:
            value = _value(key, "integer")
            if value.status == "value" and str(value.value) in tracks:
                matched = str(value.value)
        else:
            candidates = by_uri.get(key, [])
            if not candidates:
                location = _source_path(SourceValue("value", key, key), object_key, losses)
                candidates = by_path.get(location, []) if location is not None else []
            if len(candidates) == 1:
                matched = candidates[0]
        if matched is None:
            losses.append(LossRecord("unresolved", object_key, "TRACK.Key", f"Entry {len(entries)} does not resolve uniquely in COLLECTION."))
        entries.append(PlaylistEntry(len(entries), key, matched))
    _count_check(element, "Entries", len(entries), object_key, losses)
    return PlaylistNode(name, "playlist", path, entries=tuple(entries), key_type=int(key_type.value))


def _walk(node: PlaylistNode):
    yield node
    for child in node.children:
        yield from _walk(child)


def parse_rekordbox_xml(data: bytes, *, max_bytes: int = DEFAULT_MAX_BYTES) -> RekordboxLibrary:
    """Parse a bounded exported XML snapshot; do not read any referenced media.

    Unknown XML format versions fail closed. Unknown attributes remain visible in
    ``unmapped_attributes`` and ``losses`` rather than silently disappearing.
    """

    if not isinstance(data, bytes):
        raise TypeError("data must be bytes")
    if max_bytes < 1:
        raise ValueError("max_bytes must be positive")
    if len(data) > max_bytes:
        raise SourceTooLargeError(f"XML snapshot exceeds {max_bytes} bytes")
    try:
        decoded = data.decode("utf-8-sig", "strict")
    except UnicodeDecodeError as exc:
        raise RekordboxFormatError("Exported XML must be UTF-8") from exc
    if "\x00" in decoded:
        raise RekordboxFormatError("NUL bytes are not accepted in UTF-8 XML")
    declaration_end = decoded.find("?>")
    if decoded.startswith("<?xml") and (declaration_end < 0 or declaration_end > 4096):
        raise RekordboxFormatError("XML declaration is too long or incomplete")
    declared_encoding = _XML_ENCODING_RE.search(decoded[: declaration_end + 2] if declaration_end >= 0 else "")
    if declared_encoding is not None and declared_encoding.group(1).lower().replace("_", "-") != "utf-8":
        raise RekordboxFormatError("Exported XML must declare UTF-8 encoding")
    if _DECLARATION_RE.search(decoded):
        raise RekordboxFormatError("DTD and ENTITY declarations are not accepted")
    try:
        root = ET.fromstring(data)
    except ET.ParseError as exc:
        raise RekordboxFormatError(f"Malformed XML: {exc}") from exc
    if root.tag != "DJ_PLAYLISTS":
        raise RekordboxFormatError("Expected DJ_PLAYLISTS root element")
    version = root.get("Version", "")
    if version not in SUPPORTED_XML_VERSIONS:
        raise UnsupportedFormatError(
            f"Unsupported rekordbox XML format Version {version!r}",
            losses=(LossRecord("unsupported", "root", "Version", f"XML format Version {version!r} is not verified."),),
        )
    # Bound pathological nesting and element counts before recursive playlist work.
    stack: list[tuple[ET.Element, int]] = [(root, 0)]
    element_count = 0
    while stack:
        element, depth = stack.pop()
        element_count += 1
        if depth > 128 or element_count > 1_000_000:
            raise RekordboxFormatError("XML exceeds supported depth or element count")
        stack.extend((child, depth + 1) for child in element)
    losses: list[LossRecord] = []
    # These groups are not present in the documented 1.0.0 XML contract.
    for group in ("My Tag", "artwork", "waveform", "detailed analysis", "smart playlist rules"):
        losses.append(LossRecord("reader_unavailable", "collection", group, "Not provided by this XML reader; existing RadioS values must be preserved."))

    product = root.find("PRODUCT")
    collection = root.find("COLLECTION")
    if collection is None:
        raise RekordboxFormatError("COLLECTION element is required")
    tracks: dict[str, Track] = {}
    for element in collection:
        if element.tag != "TRACK":
            losses.append(LossRecord("unsupported", "collection", element.tag, "Unmapped COLLECTION child element."))
            continue
        parsed = _track(element, losses)
        if parsed.track_id in tracks:
            raise RekordboxFormatError(f"Duplicate TrackID {parsed.track_id}; import identity is ambiguous")
        tracks[parsed.track_id] = parsed
        location = parsed.fields["Location"]
        if location.status in {"missing", "empty"}:
            losses.append(LossRecord(location.status, parsed.track_id, "Location", "No media URI; source media cannot be located."))
    _count_check(collection, "Entries", len(tracks), "collection", losses)
    by_uri: dict[str, list[str]] = {}
    by_path: dict[str, list[str]] = {}
    for track_id, track in tracks.items():
        location = track.fields["Location"]
        if location.status == "value" and isinstance(location.value, str):
            by_uri.setdefault(location.value, []).append(track_id)
        if track.source_path is not None:
            by_path.setdefault(track.source_path, []).append(track_id)
    roots: list[PlaylistNode] = []
    playlists_element = root.find("PLAYLISTS")
    if playlists_element is not None:
        for child in playlists_element:
            if child.tag == "NODE":
                roots.append(_node(child, (len(roots),), tracks, by_uri, by_path, losses))
            else:
                losses.append(LossRecord("unsupported", "playlists", child.tag, "Unmapped PLAYLISTS child element."))
    for child in root:
        if child.tag not in {"PRODUCT", "COLLECTION", "PLAYLISTS"}:
            losses.append(LossRecord("unsupported", "root", child.tag, "Unmapped top-level element."))
    for attribute in root.attrib.keys() - {"Version"}:
        losses.append(LossRecord("unsupported", "root", attribute, "Unmapped root attribute."))
    for attribute in collection.attrib.keys() - {"Entries"}:
        losses.append(LossRecord("unsupported", "collection", attribute, "Unmapped COLLECTION attribute."))
    if product is not None:
        for attribute in product.attrib.keys() - {"Name", "Version", "Company"}:
            losses.append(LossRecord("unsupported", "product", attribute, "Unmapped PRODUCT attribute."))
    return RekordboxLibrary(
        version,
        product.get("Name") if product is not None else None,
        product.get("Version") if product is not None else None,
        product.get("Company") if product is not None else None,
        sha256(data).hexdigest(),
        READER_VERSION,
        tracks,
        tuple(roots),
        tuple(losses),
    )


def read_rekordbox_xml(path: str | os.PathLike[str], *, max_bytes: int = DEFAULT_MAX_BYTES) -> RekordboxLibrary:
    """Open a regular exported XML file read-only and detect concurrent edits.

    A source application should export to a stable file or an atomic snapshot.
    No ordinary live-database file copy is assumed to be consistent.
    """

    if max_bytes < 1:
        raise ValueError("max_bytes must be positive")
    source = Path(path)
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(source, flags)
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode):
            raise RekordboxFormatError("Source must be a regular exported XML file")
        if before.st_size > max_bytes:
            raise SourceTooLargeError(f"XML snapshot exceeds {max_bytes} bytes")
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            data = stream.read(max_bytes + 1)
        after = os.fstat(descriptor)
        if len(data) > max_bytes:
            raise SourceTooLargeError(f"XML snapshot exceeds {max_bytes} bytes")
        if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
            after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns
        ):
            raise SourceChangedError("Exported XML changed during read; retry from a stable snapshot")
    finally:
        os.close(descriptor)
    return parse_rekordbox_xml(data, max_bytes=max_bytes)


def plan_rekordbox_import(
    library: RekordboxLibrary,
    source_library_id: str,
    *,
    selected_node_paths: tuple[tuple[int, ...], ...] | None = None,
    selected_track_ids: tuple[str, ...] | None = None,
) -> ImportPlan:
    """Select source identities without mutating RadioS or inferring deletions.

    The stable ``source_library_id`` is assigned by the caller and must remain
    the same across repeat exports. A folder selection includes its descendants.
    """

    if not source_library_id or not source_library_id.strip():
        raise ValueError("source_library_id must be a stable nonempty value")
    all_nodes = tuple(node for root in library.root_nodes for node in _walk(root))
    node_by_path = {node.path: node for node in all_nodes}
    partial = selected_node_paths is not None or selected_track_ids is not None
    if selected_node_paths is None and not partial:
        selected_nodes = all_nodes
    elif selected_node_paths is None:
        selected_nodes = ()
    else:
        unknown = set(selected_node_paths) - node_by_path.keys()
        if unknown:
            raise ValueError(f"Unknown playlist node paths: {sorted(unknown)}")
        selected_nodes = tuple(
            node for node in all_nodes
            if any(
                node.path[: len(path)] == path or path[: len(node.path)] == node.path
                for path in selected_node_paths
            )
        )
    playlists = tuple(node for node in selected_nodes if node.kind == "playlist")
    selected_ids = {entry.track_id for node in playlists for entry in node.entries if entry.track_id is not None}
    if selected_track_ids is not None:
        for track_id in selected_track_ids:
            canonical = str(int(track_id)) if _INT_RE.fullmatch(track_id) else track_id
            if canonical not in library.tracks:
                raise ValueError(f"Unknown source TrackID: {track_id}")
            selected_ids.add(canonical)
    if not partial:
        selected_ids = set(library.tracks)
    keys = tuple(
        SourceTrackKey("rekordbox_xml", source_library_id, track_id)
        for track_id in library.tracks
        if track_id in selected_ids
    )
    return ImportPlan(library.source_sha256, library.reader_version, keys, selected_nodes, playlists, partial)


def _normalized_source_path(path: str) -> tuple[str, bool] | None:
    """Normalize separators only; refuse relative or traversing source paths."""

    if not path or "\x00" in path:
        return None
    normalized = path.replace("\\", "/")
    if normalized.startswith("//"):
        components = normalized[2:].split("/")
        if len(components) < 2 or not components[0] or not components[1]:
            return None
        windows_style = True
    elif _WINDOWS_DRIVE_RE.match(normalized):
        components = normalized[3:].split("/")
        windows_style = True
    elif normalized.startswith("/"):
        components = normalized[1:].split("/")
        windows_style = False
    else:
        return None
    if any(component in {"", ".", ".."} for component in components):
        return None
    return normalized.rstrip("/"), windows_style


def map_media_path(
    source_path: str | None,
    rules: Sequence[PathMapRule],
    *,
    allowed_roots: Sequence[str | os.PathLike[str]],
) -> PathMapping:
    """Calculate a destination path under an allowlist; never open media.

    Existing symlinks are resolved now for an initial containment check. The
    service opening the file must repeat containment after resolving it, because
    filesystem state can change between this proposal and the eventual open.
    """

    if source_path is None:
        return PathMapping("unmapped", None, None, "Source XML has no supported file URI.")
    normalized = _normalized_source_path(source_path)
    if normalized is None:
        return PathMapping("invalid", source_path, None, "Source path is relative or contains traversal/empty components.")
    source, windows_style = normalized
    permitted = tuple(Path(root).resolve() for root in allowed_roots)
    for rule in rules:
        root_pair = _normalized_source_path(rule.source_root)
        if root_pair is None:
            raise ValueError(f"Invalid source_root in path mapping rule: {rule.source_root!r}")
        source_root, root_windows_style = root_pair
        if root_windows_style != windows_style:
            continue
        sensitive = rule.case_sensitive if rule.case_sensitive is not None else not windows_style
        candidate = source if sensitive else source.casefold()
        prefix = source_root if sensitive else source_root.casefold()
        if candidate != prefix and not candidate.startswith(prefix + "/"):
            continue
        relative = source[len(source_root):].lstrip("/")
        target_root = Path(rule.target_root).resolve()
        target = target_root.joinpath(*relative.split("/")) if relative else target_root
        target = target.resolve()
        if not any(target.is_relative_to(allowed) for allowed in permitted):
            return PathMapping("invalid", source_path, None, "Mapped destination is outside allowed media roots.")
        return PathMapping("mapped", source_path, str(target), "Path mapped but media availability is unverified.")
    return PathMapping("unmapped", source_path, None, "No configured source-root mapping matches.")
