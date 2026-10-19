"""Read-only DeviceSQL USB/SD export.pdb track and playlist reader."""

from hashlib import sha256
from pathlib import PureWindowsPath
import re
import struct

from . import _platform
from .rekordbox import (LossRecord, PlaylistEntry, PlaylistNode, RekordboxLibrary,
                       SourceValue, TRACK_FIELD_SPECS, Track, _value)
from .storage import StoreError


PDB_LIMIT = 16 * 1024 * 1024
READER_VERSION = "devicesql-pdb-1"


def _number(data, offset, size=4):
    if offset < 0 or offset + size > len(data):
        raise StoreError("Device Library contains an out-of-bounds row or pointer")
    return struct.unpack_from("<I" if size == 4 else "<H", data, offset)[0]


def _string(row, offset):
    if offset < 0 or offset >= len(row):
        raise StoreError("Device Library string pointer is outside its row")
    kind = row[offset]
    if kind in (0x40, 0x90):
        length = _number(row, offset + 1, 2)
        header = 4
        encoding = "utf-16-le" if kind == 0x90 else "ascii"
    elif kind & 1:
        length = kind >> 1
        header = 1
        encoding = "ascii"
    else:
        raise StoreError("Unsupported DeviceSQL string encoding")
    if length < header or offset + length > len(row):
        raise StoreError("Device Library string length is outside its row")
    try:
        text = bytes(row[offset + header:offset + length]).decode(encoding)
    except UnicodeError as exc:
        raise StoreError("Device Library contains invalid text") from exc
    if "\x00" in text:
        raise StoreError("Device Library contains a NUL in text")
    return text


def _rows(contents):
    if not isinstance(contents, bytes) or not 28 <= len(contents) <= PDB_LIMIT:
        raise StoreError("Device Library must be a nonempty export.pdb of at most 16 MiB")
    page_size = _number(contents, 4)
    table_count = _number(contents, 8)
    if _number(contents, 0) or _number(contents, 24) or page_size < 512 or page_size > 65536 or \
            page_size & (page_size - 1) or len(contents) % page_size or \
            table_count > 64 or 28 + table_count * 16 > page_size:
        raise StoreError("Invalid DeviceSQL export.pdb header")
    tables = {}
    visited = set()
    count = 0
    for table in range(table_count):
        offset = 28 + table * 16
        kind = _number(contents, offset)
        first = _number(contents, offset + 8)
        last = _number(contents, offset + 12)
        if kind in tables:
            raise StoreError("Device Library contains duplicate table identities")
        tables[kind] = []
        if not first and not last:
            continue
        current = first
        while True:
            if current < 1 or current >= len(contents) // page_size or current in visited:
                raise StoreError("Device Library has a broken or cyclic page chain")
            visited.add(current)
            page = memoryview(contents)[current * page_size:(current + 1) * page_size]
            if _number(page, 0) or _number(page, 4) != current or _number(page, 8) != kind:
                raise StoreError("Device Library page does not match its table")
            if not page[27] & 0x40 and kind in (0, 1, 2, 3, 4, 5, 7, 8):
                slots = int.from_bytes(page[24:27], "little") & 0x1fff
                remainder = slots % 16
                index_size = slots // 16 * 36 + (4 + remainder * 2 if remainder else 0)
                heap_end = page_size - index_size
                if heap_end < 40:
                    raise StoreError("Device Library row index exceeds its page")
                offsets = []
                for slot in range(slots):
                    base = page_size - slot // 16 * 36
                    flags = _number(page, base - 4, 2)
                    if flags & (1 << (slot % 16)):
                        start = 40 + _number(page, base - 6 - slot % 16 * 2, 2)
                        if not 40 <= start < heap_end or start in offsets:
                            raise StoreError("Device Library row offset is invalid")
                        offsets.append(start)
                boundaries = sorted(offsets) + [heap_end]
                ends = dict(zip(boundaries, boundaries[1:]))
                for start in offsets:
                    tables[kind].append(page[start:ends[start]])
                count += len(offsets)
                if count > 200000:
                    raise StoreError("Device Library has too many rows")
            if current == last:
                break
            current = _number(page, 12)
    if 0 not in tables:
        raise StoreError("Device Library has no track table")
    return tables


def _names(rows, kind):
    result = {}
    for row in rows:
        if kind in (2, 3):
            identifier = _number(row, 4 if kind == 2 else 12)
            near = 9 if kind == 2 else 21
            if near >= len(row):
                raise StoreError("Device Library name row is truncated")
            offset = _number(row, near + 1, 2) if _number(row, 0, 2) & 4 else row[near]
        else:
            identifier = _number(row, 0)
            offset = 8 if kind == 5 else 4
        if identifier in result:
            raise StoreError("Device Library contains duplicate metadata identities")
        result[identifier] = _string(row, offset)
    return result


def _media_path(root, path, track_id, losses):
    """The Windows path of a DeviceSQL track location (``/Contents/...``) below usb_root.

    A backslash, a ``..`` part or a drive in the first part escapes the stick and
    fails the whole library (unchanged). A name that cannot exist on a Windows
    volume (``:``, ``*``, ``CON``, trailing dot ...) only costs that track its
    location, reported as a loss."""
    if not path:
        return None
    raw_parts = path.split("/")
    if "\\" in path or any(part == ".." for part in raw_parts) or ":" in raw_parts[0]:
        raise StoreError("Device Library media path escapes its USB root")
    parts = [part for part in raw_parts if part and part != "."]
    if not parts or not all(_platform.component_valid(part) for part in parts):
        losses.append(LossRecord("unsupported", track_id, "Location",
                                 "Device path is not representable on Windows"))
        return None
    return str(PureWindowsPath(root).joinpath(*parts))


def _checked_usb_root(usb_root):
    """usb_root is the folder holding PIONEER\\ on the stick: a canonical absolute Windows
    path, a drive root such as ``E:\\`` or a UNC share included."""
    if not isinstance(usb_root, str) or len(usb_root) > 4096 or \
            any(ord(character) < 32 for character in usb_root) or \
            not _platform.is_absolute(usb_root) or \
            any(part in (".", "..") for part in re.split(r"[\\/]", usb_root)) or \
            not _platform.is_canonical(usb_root):
        raise StoreError("usb_root must be an absolute USB/SD folder such as E:\\")
    return usb_root


def parse_device_library(contents, usb_root):
    usb_root = _checked_usb_root(usb_root)
    tables = _rows(contents)
    names = {kind: _names(tables.get(kind, []), kind) for kind in (1, 2, 3, 4, 5)}
    tracks = {}
    losses = [LossRecord("reader_unavailable", "device", "ANLZ",
                         "Tracklists and metadata read from export.pdb; waveform, beatgrid and hot cues in ANLZ are not read.")]
    for row in tables[0]:
        if len(row) < 136:
            raise StoreError("Device Library track row is truncated")
        identifier = str(_number(row, 72))
        if identifier in tracks or len(tracks) >= 50000:
            raise StoreError("Device Library track identities are duplicated or exceed 50000")
        strings = {position: _string(row, _number(row, 94 + position * 2, 2)) for position in (10, 14, 16, 17, 20)}
        source_path = _media_path(usb_root, strings[20], identifier, losses)
        attributes = {"TrackID": identifier, "Name": strings[17], "Comments": strings[16],
                      "Artist": names[2].get(_number(row, 68), ""),
                      "Composer": names[2].get(_number(row, 12), ""),
                      "Remixer": names[2].get(_number(row, 44), ""),
                      "Album": names[3].get(_number(row, 64), ""),
                      "Genre": names[1].get(_number(row, 60), ""),
                      "Label": names[4].get(_number(row, 40), ""),
                      "Tonality": names[5].get(_number(row, 32), ""),
                      "AverageBpm": str(_number(row, 56) / 100),
                      "TotalTime": str(_number(row, 84, 2)),
                      "Year": str(_number(row, 80, 2)), "TrackNumber": str(_number(row, 52)),
                      "Size": str(_number(row, 16)), "BitRate": str(_number(row, 48)),
                      "SampleRate": str(_number(row, 8)), "DateAdded": strings[10]}
        fields = {spec.source: _value(attributes.get(spec.source), spec.kind) for spec in TRACK_FIELD_SPECS}
        if source_path:
            fields["Location"] = SourceValue("value", source_path, strings[20])
        elif strings[20]:
            fields["Location"] = SourceValue("invalid", raw=strings[20])
        else:
            fields["Location"] = SourceValue("empty", raw="")
        tracks[identifier] = Track(identifier, fields, source_path, (), (),
                                   {"device_path": strings[20], "analysis_path": strings[14]})
    tree = {}
    children = {}
    entries = {}
    for row in tables.get(7, []):
        identifier = _number(row, 12)
        if not identifier or identifier in tree or len(tree) >= 10000:
            raise StoreError("Device Library playlist identities are invalid or exceed 10000")
        parent = _number(row, 0)
        tree[identifier] = (parent, _number(row, 8), bool(_number(row, 16)), _string(row, 20))
        children.setdefault(parent, []).append(identifier)
    for identifier, (parent, order, folder, name) in tree.items():
        if parent and (parent not in tree or not tree[parent][2]):
            raise StoreError("Device Library playlist has a missing or non-folder parent")
    for row in tables.get(8, []):
        position, track, playlist = (_number(row, offset) for offset in (0, 4, 8))
        if playlist not in tree or tree[playlist][2] or str(track) not in tracks:
            raise StoreError("Device Library playlist references a missing track or playlist")
        target = entries.setdefault(playlist, {})
        if position in target:
            raise StoreError("Device Library playlist contains duplicate positions")
        target[position] = str(track)
    seen = set()

    def node(identifier, path):
        if identifier in seen or len(path) > 64:
            raise StoreError("Device Library playlist hierarchy is cyclic or too deep")
        seen.add(identifier)
        parent, order, folder, name = tree[identifier]
        ordered = sorted(children.get(identifier, []), key=lambda child: (tree[child][1], child))
        members = tuple(PlaylistEntry(index, track, track) for index, (position, track) in
                        enumerate(sorted(entries.get(identifier, {}).items())))
        return PlaylistNode(name, "folder" if folder else "playlist", path,
                            tuple(node(child, path + (index,)) for index, child in enumerate(ordered)), members, 0)

    roots = sorted(children.get(0, []), key=lambda child: (tree[child][1], child))
    root_nodes = tuple(node(identifier, (index,)) for index, identifier in enumerate(roots))
    if len(seen) != len(tree):
        raise StoreError("Device Library contains an unreachable or cyclic playlist hierarchy")
    return RekordboxLibrary("DeviceSQL", "rekordbox USB/SD", None, "AlphaTheta",
                            sha256(contents).hexdigest(), READER_VERSION, tracks, root_nodes, tuple(losses))
