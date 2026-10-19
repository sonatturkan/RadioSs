"""Bounded USB DeviceSQL and legacy XML-to-native-playlist bridge.

Uses the existing importer's exact catalog decisions, source snapshots and DJ
analysis writers. Native playlist rows, importer rows and the immutable replay
receipt commit together. Source snapshots never open or verify audio files.
The original profile-based HTTP importer remains unchanged.
"""

from base64 import b64decode
from binascii import Error as Base64Error
from datetime import datetime, timezone
import hashlib
import json
import zlib
from uuid import uuid4

from . import desktop_playlist as playlists, importer
from .desktop_api import add_operation, operation_replay
from .identity import require
from .storage import Conflict, StoreError
from .rekordbox_device import PDB_LIMIT, parse_device_library


XML_LIMIT = 1024 * 1024
PLAYLIST_LIMIT = 500
ENTRY_LIMIT = 500
COMPRESSED_LIMIT = 4 * 1024 * 1024


def _digest(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                    separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def _xml(encoded):
    if not isinstance(encoded, str) or len(encoded) > ((XML_LIMIT + 2) // 3) * 4:
        raise StoreError("native Rekordbox XML must be at most 1 MiB")
    try:
        contents = b64decode(encoded, validate=True)
    except (Base64Error, ValueError) as exc:
        raise StoreError("invalid Rekordbox XML base64") from exc
    if not contents or len(contents) > XML_LIMIT:
        raise StoreError("native Rekordbox XML must be 1 byte..1 MiB")
    return contents


def _source(xml_base64, pdb_zlib_base64, usb_root):
    if (xml_base64 is None) == (pdb_zlib_base64 is None):
        raise StoreError("choose one Rekordbox source: USB Device Library or legacy XML")
    if pdb_zlib_base64 is None:
        if usb_root is not None:
            raise StoreError("usb_root belongs only to a USB Device Library source")
        contents = _xml(xml_base64)
        return contents, importer.parse_rekordbox_xml(contents), importer.SOURCE_KIND
    if not isinstance(pdb_zlib_base64, str) or len(pdb_zlib_base64) > ((COMPRESSED_LIMIT + 2) // 3) * 4:
        raise StoreError("compressed Device Library exceeds 4 MiB")
    try:
        compressed = b64decode(pdb_zlib_base64, validate=True)
        if not compressed or len(compressed) > COMPRESSED_LIMIT:
            raise StoreError("compressed Device Library must be 1 byte..4 MiB")
        decoder = zlib.decompressobj()
        contents = decoder.decompress(compressed, PDB_LIMIT + 1)
        if len(contents) > PDB_LIMIT or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
            raise StoreError("Device Library is oversized, truncated or contains trailing data")
    except (Base64Error, ValueError, zlib.error) as exc:
        raise StoreError("invalid compressed Device Library snapshot") from exc
    return contents, parse_device_library(contents, usb_root), "rekordbox_device"


def _options(library_key, selected_node_paths, include_collection, name_policy,
             duplicate_policy, cue_policy):
    library_key = playlists._text(library_key, "stable library key", 120, True)
    if type(include_collection) is not bool:
        raise StoreError("include_collection must be boolean")
    if not isinstance(name_policy, str) or name_policy not in {"append", "copy"}:
        raise StoreError("name_policy must be append or copy")
    if not isinstance(duplicate_policy, str) or duplicate_policy not in {"skip", "keep"}:
        raise StoreError("duplicate_policy must be skip or keep")
    if not isinstance(cue_policy, str) or cue_policy not in {"preserve_as_dj", "propose_radio_cues", "ignore"}:
        raise StoreError("unsupported cue policy")
    if selected_node_paths is not None:
        if not isinstance(selected_node_paths, list) or len(selected_node_paths) > PLAYLIST_LIMIT:
            raise StoreError("selected_node_paths must contain at most 500 playlist paths")
        for path in selected_node_paths:
            if not isinstance(path, list) or not path or len(path) > 64 or any(
                    type(index) is not int or index < 0 for index in path):
                raise StoreError("playlist paths must contain nonnegative integer ordinals")
        if len({tuple(path) for path in selected_node_paths}) != len(selected_node_paths):
            raise StoreError("playlist paths must be distinct")
    return {"library_key": library_key, "selected_node_paths": selected_node_paths,
            "include_collection": include_collection, "name_policy": name_policy,
            "duplicate_policy": duplicate_policy, "cue_policy": cue_policy}


def _field(track, name, fallback=""):
    value = track.fields[name]
    return value.value if value.status == "value" and value.value is not None else fallback


def _metadata(track, losses):
    values = {"title": str(_field(track, "Name", "Rekordbox Track " + track.track_id)),
              "artist": str(_field(track, "Artist")), "album": str(_field(track, "Album")),
              "genre": str(_field(track, "Genre")), "key": str(_field(track, "Tonality")),
              "location": track.source_path or "", "comment": str(_field(track, "Comments"))}
    if len(values["location"]) > 4096:
        raise StoreError("source media location exceeds the native 4096 character limit")
    for name, limit in (("title", 160), ("artist", 120), ("album", 120),
                        ("genre", 120), ("key", 120), ("comment", 2000)):
        if len(values[name]) > limit:
            losses.append({"source_id": track.track_id, "field": name,
                           "detail": "Native display shortened; full value remains in import source snapshot."})
            values[name] = values[name][:limit]
    if not values["title"].strip():
        values["title"] = "Rekordbox Track " + track.track_id
    seconds = _field(track, "TotalTime", 0)
    # Compare Decimal proposals before converting, so a source exponent cannot
    # create a huge Python integer or an infinite native BPM value.
    duration = int(seconds * 1000) if 0 < seconds <= 86400 else 0
    values["duration_ms"] = duration if duration > 0 else None
    source_bpm = _field(track, "AverageBpm", 0)
    values["bpm"] = float(source_bpm) if 0 <= source_bpm <= 1000 else 0
    for field, out_of_range in (("duration_ms", seconds < 0 or seconds > 86400),
                                ("bpm", source_bpm < 0 or source_bpm > 1000)):
        if out_of_range:
            losses.append({"source_id": track.track_id, "field": field,
                           "detail": "Native value unavailable outside its supported range; source proposal retained."})
    return playlists._metadata(values)


def _duplicate_key(row):
    # Source URI decoding is done by the existing strict XML reader. A textual
    # path is only a duplicate membership key, never an audio authorization.
    return ("path", row["location"]) if row["location"] else (
        "source", row.get("rekordbox_namespace", ""), row.get("rekordbox_track_id", ""))


def _prepare(store, principal, station_id, source, options):
    station_id = playlists._id(station_id, "station_id")
    require(store, principal, "import", station_id)
    require(store, principal, "edit", station_id)
    if store.get("stations", station_id) is None:
        raise StoreError("station not found")
    sequence = store.sequence
    contents, library, source_kind = source
    if len(library.playlists) > PLAYLIST_LIMIT or len(library.tracks) > (50000 if source_kind == "rekordbox_device" else 5000):
        raise StoreError("native source exceeds its playlist or collection track limit")
    paths = options["selected_node_paths"]
    if paths is None:
        paths = [] if source_kind == "rekordbox_device" and not options["include_collection"] else [list(node.path) for node in library.playlists]
    actual = {tuple(node.path) for node in library.playlists}
    if any(tuple(path) not in actual for path in paths):
        raise StoreError("selected path must identify an actual playlist in this source snapshot")
    # Native library names are station-local. The original importer's global
    # source namespace remains available through its preserved profile API.
    source_key = "native:" + station_id + ":" + hashlib.sha256(options["library_key"].encode()).hexdigest()
    prepared = importer._prepare(store, principal, station_id, source_key, contents,
        selected_node_paths=paths,
        selected_track_ids=list(library.tracks) if options["include_collection"] else [],
        field_policy_overrides=None, category_mapping=None, path_rules=(), allowed_roots=(),
        cue_policy=options["cue_policy"], source_library=library, source_kind=source_kind)
    domain = importer._preview_prepared(store, prepared)
    targets = [row for row in store.lookup("playlists", "station", station_id)
               if row["status"] == "active"]
    names = {}
    for row in targets:
        names.setdefault(row["name"].casefold(), []).append(row)
    losses = []
    sources = [(list(node.path), node.name or "Adsız playlist",
                [entry.track_id for entry in node.entries]) for node in prepared["plan"].selected_playlists]
    if options["include_collection"]:
        sources.append((None, "Rekordbox koleksiyonu", list(library.tracks)))
    planned = []
    # Share a destination plan when the XML has multiple equally named nodes.
    destinations = {}
    reserved = {row["name"].casefold() for row in targets}
    for path, source_name, track_ids in sources:
        name = source_name[:120].strip() or "Adsız playlist"
        canonical = name.casefold()
        destination = destinations.get(canonical) if options["name_policy"] == "append" else None
        if destination is None:
            existing = names.get(canonical, []) if options["name_policy"] == "append" else []
            if len(existing) > 1:
                raise Conflict(f"multiple native playlists have the name {name!r}; choose new copy")
            if existing:
                row, version = playlists._playlist(store, station_id, existing[0]["_id"])
                tracks = playlists._tracks(store, version["_id"])
                destination = {"playlist_id": row["_id"], "version_id": version["_id"],
                               "expected_revision": row["_rev"], "version_revision": version["_rev"],
                               "name": row["name"], "existing_count": len(tracks),
                               "existing": tracks, "entries": [], "skipped": 0}
            else:
                if options["name_policy"] == "copy":
                    suffix = 2
                    while name.casefold() in reserved:
                        tail = f" ({suffix})"
                        name = (source_name[:120 - len(tail)].strip() or "Playlist") + tail
                        suffix += 1
                reserved.add(name.casefold())
                destination = {"playlist_id": None, "version_id": None, "expected_revision": None,
                               "version_revision": None, "name": name, "existing_count": 0,
                               "existing": [], "entries": [], "skipped": 0}
            planned.append(destination)
            destinations[canonical] = destination
        seen = {_duplicate_key(row) for row in destination["existing"] + destination["entries"]}
        for ordinal, source_id in enumerate(track_ids):
            if source_id is None or source_id not in library.tracks:
                losses.append({"source_id": source_id, "field": "playlist entry",
                               "detail": "Unresolved source reference is omitted from native playlist."})
                continue
            track = library.tracks[source_id]
            metadata = _metadata(track, losses)
            metadata.update(rekordbox_namespace=prepared["namespace"], rekordbox_track_id=source_id,
                            rekordbox_sha256=library.source_sha256,
                            rekordbox_playlist_path=path, rekordbox_source_ordinal=ordinal,
                            rekordbox_cue_policy=options["cue_policy"], rekordbox_cue_marks=len(track.marks))
            duplicate = _duplicate_key(metadata)
            if options["duplicate_policy"] == "skip" and duplicate in seen:
                destination["skipped"] += 1
                continue
            seen.add(duplicate)
            destination["entries"].append(metadata)
        if destination["existing_count"] + len(destination["entries"]) > ENTRY_LIMIT:
            raise StoreError(f"native playlist {destination['name']!r} would exceed 500 entries")
    if sum(len(row["entries"]) for row in planned) > 5000:
        raise StoreError("native import accepts at most 5000 new playlist memberships per operation")
    if store.sequence != sequence or domain["store_sequence"] != sequence:
        raise Conflict("database changed during native import preview; retry")
    return prepared, domain, planned, losses


def _public(prepared, domain, planned, losses, options):
    nodes = [{"path": list(node.path), "name": node.name or "Adsız playlist",
              "tracks": len(node.entries)} for node in prepared["library"].playlists]
    destinations = [{key: row[key] for key in ("playlist_id", "expected_revision", "name", "existing_count", "skipped")}
                    | {"added_count": len(row["entries"])} for row in planned]
    native_hash = _digest({"domain_preview_hash": domain["preview_hash"], "options": options,
                           "destinations": planned})
    labels = {}
    def remember(node, ancestors=()):
        labels[node.path] = " / ".join(ancestors + (node.name,))
        for child in node.children:
            remember(child, ancestors + (node.name,))
    for root in prepared["library"].root_nodes:
        remember(root)
    for node in nodes:
        node["display_path"] = labels[tuple(node["path"])]
    return {"station_id": prepared["station_id"], "source_sha256": domain["source_sha256"],
            "source_kind": prepared["source_kind"], "reader_version": prepared["library"].reader_version,
            "preview_hash": native_hash, "store_sequence": domain["store_sequence"],
            "playlist_nodes": nodes, "destinations": destinations,
            "collection_tracks": len(prepared["library"].tracks),
            "selected_tracks": len(domain["entries"]), "catalog_counts": domain["counts"],
            "cue_marks": sum(len(prepared["library"].tracks[row["source_id"]].marks) for row in domain["entries"]),
            "cue_policy": options["cue_policy"], "projection_losses": losses[:100],
            "projection_loss_count": len(losses), "domain_loss_count": len(domain["losses"]),
            "domain_losses": domain["losses"][:100],
            "media_verified": 0, "radio_cues_created": 0}


def preview_import(store, principal, station_id, xml_base64=None, *, pdb_zlib_base64=None, usb_root=None,
                   library_key="native-rekordbox",
                   selected_node_paths=None, include_collection=False, name_policy="append",
                   duplicate_policy="skip", cue_policy="preserve_as_dj"):
    options = _options(library_key, selected_node_paths, include_collection,
                       name_policy, duplicate_policy, cue_policy)
    require(store, principal, "import", station_id)
    require(store, principal, "edit", station_id)
    source = _source(xml_base64, pdb_zlib_base64, usb_root)
    prepared, domain, planned, losses = _prepare(store, principal, station_id, source, options)
    return _public(prepared, domain, planned, losses, options)


def apply_import(store, principal, station_id, xml_base64=None, expected_preview_hash=None, idempotency_key=None, *,
                 pdb_zlib_base64=None, usb_root=None,
                 library_key="native-rekordbox", selected_node_paths=None, include_collection=False,
                 name_policy="append", duplicate_policy="skip", cue_policy="preserve_as_dj"):
    options = _options(library_key, selected_node_paths, include_collection,
                       name_policy, duplicate_policy, cue_policy)
    station_id = playlists._id(station_id, "station_id")
    require(store, principal, "import", station_id)
    require(store, principal, "edit", station_id)
    if selected_node_paths is None or not selected_node_paths and not include_collection:
        raise StoreError("apply requires explicit playlist selection or collection")
    if not isinstance(expected_preview_hash, str) or len(expected_preview_hash) != 64 or any(
            char not in "0123456789abcdef" for char in expected_preview_hash):
        raise StoreError("expected_preview_hash must be SHA-256")
    source = _source(xml_base64, pdb_zlib_base64, usb_root)
    payload = {"source_sha256": source[1].source_sha256, "options": options,
               "expected_preview_hash": expected_preview_hash}
    if source[2] == "rekordbox_device":
        payload.update(source_kind=source[2], usb_root=usb_root)
    receipt_id, request_hash, replay = operation_replay(store, principal, station_id,
        idempotency_key, "desktop-rekordbox/apply", payload)
    if replay is not None:
        return replay
    prepared, domain, planned, losses = _prepare(store, principal, station_id, source, options)
    preview = _public(prepared, domain, planned, losses, options)
    if expected_preview_hash != preview["preview_hash"]:
        raise Conflict("native import preview changed; refresh and review before applying")
    # The established importer writers own catalog policy, identities and cues;
    # the native rows are merely editable unverified playlist metadata.
    counters = {name: 0 for name in ("items_created", "items_updated", "versions_created",
        "station_links_created", "station_links_updated", "external_id_rows_written",
        "metadata_values_written", "metadata_suggestions_created", "dj_analysis_written",
        "playlist_nodes_written", "playlist_entries_written", "identity_conflicts")}
    tx = store.begin(actor=principal.user_id)
    importer._check_preview_reads(tx, prepared, domain)
    resolved = {}
    for decision in domain["entries"]:
        importer._track_writes(tx, store, principal, prepared, decision,
            prepared["library"].tracks[decision["source_id"]], counters, resolved)
    importer._playlist_writes(tx, store, prepared, domain, resolved, counters)
    now = datetime.now(timezone.utc).isoformat()
    results = []
    for destination in planned:
        playlist_id = destination["playlist_id"] or str(uuid4())
        version_id = destination["version_id"] or str(uuid4())
        if destination["playlist_id"]:
            row = store.get("playlists", playlist_id)
            tx.expect("playlist_versions", version_id, destination["version_revision"])
            tx.put("playlists", playlist_id, playlists._body(row),
                   expected_revision=destination["expected_revision"])
        else:
            tx.put("playlists", playlist_id, {"station_id": station_id, "code": "native-" + playlist_id,
                "name": destination["name"], "status": "active", "current_version_id": None,
                "draft_version_id": version_id, "created_by": principal.user_id, "created_at": now})
            tx.put("playlist_versions", version_id, {"playlist_id": playlist_id, "version_no": 1,
                "status": "draft", "created_by": principal.user_id, "created_at": now})
        for index, metadata in enumerate(destination["entries"]):
            ordinal = destination["existing_count"] + index
            tx.put(playlists.COLLECTION, str(uuid4()), metadata | {
                "station_id": station_id, "playlist_version_id": version_id, "ordinal": ordinal,
                "active_ordinal": ordinal, "status": "active", "media_state": "unverified",
                "created_at": now, "created_by": principal.user_id,
                "updated_at": now, "updated_by": principal.user_id})
        results.append({"playlist_id": playlist_id, "name": destination["name"],
                        "added_count": len(destination["entries"]), "skipped": destination["skipped"]})
    job_id = importer._stable_id("native_import_job", station_id, principal.user_id, idempotency_key)
    result = {"job_id": job_id, "station_id": station_id, "source_sha256": preview["source_sha256"],
              "preview_hash": preview["preview_hash"], "playlists": results,
              "catalog_counts": counters, "cue_policy": cue_policy,
              "projection_losses": preview["projection_losses"],
              "projection_loss_count": preview["projection_loss_count"],
              "media_verified": 0, "radio_cues_created": 0}
    tx.put("import_jobs", job_id, {
        "station_id": station_id, "source_namespace": prepared["namespace"],
        "source_sha256": prepared["library"].source_sha256,
        "idempotency_key": "native:" + principal.user_id + ":" + idempotency_key,
        "request_fingerprint": request_hash, "status": "completed",
        "scope": domain["scope"], "summary": result,
        "completed_at": now, "created_by": principal.user_id})
    add_operation(tx, principal, station_id, receipt_id, idempotency_key,
                  "desktop-rekordbox/apply", request_hash, result)
    tx.commit()
    return result
