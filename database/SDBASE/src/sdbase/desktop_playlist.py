"""Native desktop playlist drafts, separate from formal publication.

Playlist/version identities use the existing domain collections. Editable rows
are typed, audited desktop_playlist_entries. Optional verified catalog bindings
support local playback; metadata cannot publish content, approve versions or
claim that a user-entered file path is accessible.
"""

from datetime import datetime, timezone
import math
from uuid import UUID, uuid4

from .identity import require
from .storage import Conflict, StoreError

COLLECTION = "desktop_playlist_entries"
PLAYLIST_PURPOSES = frozenset({"music", "jingle", "advert", "announcement", "programme", "other", "sweeper"})
METADATA = {"title", "artist", "album", "duration_ms", "bpm", "genre", "key", "location", "comment"}
# Retired: Spotify, Apple Music and YouTube Music playlist sources. A new row
# may only carry display metadata or a verified local media binding; rows that
# an existing database already holds keep their shape and stay readable.
ONLINE_REFERENCE_KEYS = frozenset({"provider", "external_id", "url"})
ONLINE_SOURCE_KEYS = ONLINE_REFERENCE_KEYS | {"online_id"}
ANALYSIS_SOURCE_KEYS = frozenset({"analysis_media"})
RETIRED_SOURCE_MESSAGE = "online playlist entries are no longer supported; use a local media file"
REKORDBOX_SOURCE = {"rekordbox_namespace", "rekordbox_track_id", "rekordbox_sha256",
                   "rekordbox_playlist_path", "rekordbox_source_ordinal",
                   "rekordbox_cue_policy", "rekordbox_cue_marks"}


def _id(value, name):
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{name} must be a UUID") from exc


def _integer(value, name, low, high):
    if type(value) is not int or not low <= value <= high:
        raise StoreError(f"{name} must be an integer in {low}..{high}")
    return value


def _text(value, name, maximum, required=False):
    if not isinstance(value, str) or len(value) > maximum or "\0" in value or (required and not value.strip()):
        raise StoreError(f"{name} must be {'nonempty ' if required else ''}text of at most {maximum} characters")
    return value.strip() if required else value


def _metadata(value, *, persisted=False):
    from .desktop_api import MEDIA_BINDING_KEYS
    if not isinstance(value, dict) or set(value) - (METADATA | MEDIA_BINDING_KEYS | ONLINE_SOURCE_KEYS | ANALYSIS_SOURCE_KEYS) or METADATA - set(value):
        raise StoreError("playlist metadata has missing or unsupported fields")
    result = {name: _text(value[name], name, limit, name == "title") for name, limit in
              (("title", 160), ("artist", 120), ("album", 120), ("genre", 120),
               ("key", 120), ("location", 4096), ("comment", 2000))}
    duration = value["duration_ms"]
    result["duration_ms"] = None if duration is None else _integer(duration, "duration_ms", 1, 86_400_000)
    bpm = value["bpm"]
    if isinstance(bpm, bool) or not isinstance(bpm, (int, float)) or not 0 <= bpm <= 1000 or not math.isfinite(bpm):
        raise StoreError("bpm must be finite and in 0..1000")
    result["bpm"] = bpm
    present = MEDIA_BINDING_KEYS.intersection(value)
    online = ONLINE_SOURCE_KEYS.intersection(value)
    if (online or "analysis_media" in value) and not persisted:
        raise StoreError(RETIRED_SOURCE_MESSAGE)
    if present and online:
        raise StoreError("playlist entry cannot combine local and online source bindings")
    if present:
        if present != MEDIA_BINDING_KEYS:
            raise StoreError("playlist media binding requires all four identities")
        result.update({key: value[key] for key in MEDIA_BINDING_KEYS})
    if online:
        # A stored legacy provider row only has to keep its shape.
        if not ONLINE_REFERENCE_KEYS.issubset(value):
            raise StoreError("stored online playlist entry has an invalid shape")
        for name in sorted(ONLINE_REFERENCE_KEYS):
            if not isinstance(value[name], str) or not value[name] or len(value[name]) > 1024 or "\0" in value[name]:
                raise StoreError("stored online playlist entry has an invalid shape")
            result[name] = value[name]
        if "online_id" in value:
            result["online_id"] = _id(value["online_id"], "online_id")
    if "analysis_media" in value:
        if not ONLINE_REFERENCE_KEYS.issubset(result):
            raise StoreError("analysis_media requires an online playback source")
        binding = value["analysis_media"]
        if not isinstance(binding, dict) or set(binding) != MEDIA_BINDING_KEYS:
            raise StoreError("analysis_media requires exactly four verified media identities")
        digest = binding["media_sha256"]
        if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            raise StoreError("analysis_media media_sha256 must be a lowercase SHA-256 digest")
        result["analysis_media"] = {key: _id(binding[key], "analysis_media " + key)
                                    for key in MEDIA_BINDING_KEYS - {"media_sha256"}}
        result["analysis_media"]["media_sha256"] = digest
    return result


def _online_binding(metadata, station_id, state, *, persisted=False):
    """Validate an optional saved reference without fetching or authorizing audio."""
    if "online_id" not in metadata:
        return
    row = state.get("desktop_configurations", {}).get(metadata["online_id"])
    source = row["data"] if row else None
    if not source or source["station_id"] != station_id or source["kind"] != "external_playlist":
        raise StoreError("playlist online_id must reference an external playlist in this station")
    if not persisted and source["status"] != "active":
        raise StoreError("playlist online source is disabled")
    config = source["config"]
    if config["provider"] != metadata["provider"] or not any(
            item["provider_id"] == metadata["external_id"] and item["url"] == metadata["url"]
            for item in config["items"]):
        raise StoreError("playlist online source must match the saved provider recording and URL")


def validate_entry(data, state):
    from .desktop_api import MEDIA_BINDING_KEYS, _media_binding
    metadata = _metadata({name: data[name] for name in METADATA | MEDIA_BINDING_KEYS | ONLINE_SOURCE_KEYS | ANALYSIS_SOURCE_KEYS if name in data},
                         persisted=True)
    _online_binding(metadata, data["station_id"], state, persisted=True)
    if "analysis_media" in metadata:
        _media_binding(metadata["analysis_media"], data["station_id"], state, persisted=True)
    if MEDIA_BINDING_KEYS.intersection(data):
        _media_binding({key: data[key] for key in MEDIA_BINDING_KEYS if key in data},
                       data["station_id"], state, persisted=True)
    if data["media_state"] != "unverified" or data["status"] not in {"active", "removed"}:
        raise StoreError("desktop entries remain unverified metadata drafts")
    _integer(data["ordinal"], "ordinal", 0, 499)
    if (data["status"] == "active" and type(data["active_ordinal"]) is not int) or \
            data["active_ordinal"] != (data["ordinal"] if data["status"] == "active" else None):
        raise StoreError("desktop entry active ordinal mismatch")
    version = state["playlist_versions"][_id(data["playlist_version_id"], "playlist_version_id")]["data"]
    playlist = state["playlists"][_id(version["playlist_id"], "playlist_id")]["data"]
    if playlist["station_id"] != data["station_id"] or version["status"] != "draft":
        raise StoreError("desktop metadata requires a draft version in its station")
    for field in ("created_at", "updated_at"):
        stamp = datetime.fromisoformat(data[field])
        if stamp.tzinfo is None:
            raise StoreError("desktop entry timestamps need a timezone")


def guard_entry(before, after):
    if after is None:
        raise StoreError("desktop playlist entries are retired, not physically deleted")
    if before and any(before.get(field) != after.get(field) for field in
                      ("station_id", "playlist_version_id", "created_by", "created_at", "media_state")):
        raise StoreError("desktop playlist entry identity is immutable")
    if before and before["status"] == "removed":
        raise StoreError("removed desktop playlist entry is immutable")


def _body(row):
    return {key: value for key, value in row.items() if key not in {"_id", "_rev"}}


def _playlist(store, station_id, playlist_id, revision=None):
    row = store.get("playlists", _id(playlist_id, "playlist_id"))
    if row is None or row["station_id"] != station_id or row["status"] != "active":
        raise StoreError("playlist is absent, disabled or outside station")
    if revision is not None and (type(revision) is not int or row["_rev"] != revision):
        raise Conflict("playlist changed; reload before saving")
    version = store.get("playlist_versions", row["draft_version_id"]) if row.get("draft_version_id") else None
    if not version or version["status"] != "draft" or version["playlist_id"] != row["_id"]:
        raise Conflict("only a current playlist draft is editable")
    return row, version


def _tracks(store, version_id):
    return sorted((row for row in store.lookup(COLLECTION, "version", version_id) if row["status"] == "active"),
                  key=lambda row: (row["ordinal"], row["_id"]))


def _snapshot(store, station_id, playlist_id):
    with store.read_snapshot(["playlists", "playlist_versions", COLLECTION]) as snapshot:
        row = snapshot.get("playlists", playlist_id)
        if row is None or row["station_id"] != station_id:
            raise StoreError("playlist is outside station")
        version = snapshot.get("playlist_versions", row["draft_version_id"]) if row.get("draft_version_id") else None
        tracks = _tracks(snapshot, version["_id"]) if version else []
        return row | {"purpose": row.get("purpose", "music"),
                      "version_id": version["_id"] if version else None,
                      "version_revision": version["_rev"] if version else None,
                      "tracks": tracks, "media_state": "unverified"}


def list_playlists(store, principal, station_id):
    station_id = _id(station_id, "station_id")
    require(store, principal, "view", station_id)
    sequence = store.sequence
    rows = store.lookup("playlists", "station", station_id)
    result = [_snapshot(store, station_id, row["_id"]) for row in rows if row["status"] == "active"]
    result.sort(key=lambda row: (row["name"].casefold(), row["_id"]))
    if store.sequence != sequence:
        raise Conflict("playlists changed during listing")
    return {"playlists": result, "sequence": sequence}


def _operation(store, principal, station_id, idempotency_key, operation, payload, permission="edit"):
    from .desktop_api import operation_replay
    station_id = _id(station_id, "station_id")
    require(store, principal, permission, station_id)
    return station_id, operation_replay(store, principal, station_id, idempotency_key, operation, payload)


def _replay(store, station_id, result):
    return result if result.get("deleted") else _snapshot(store, station_id, result["playlist_id"])


def _receipt(tx, principal, station_id, idempotency_key, operation, receipt, result):
    from .desktop_api import add_operation
    receipt_id, request_hash, _replayed = receipt
    add_operation(tx, principal, station_id, receipt_id, idempotency_key, operation, request_hash, result)


def create_playlist(store, principal, station_id, name, *, idempotency_key, entries=None, purpose="music"):
    """Create one draft, optionally with selected metadata rows atomically.

    Omitting entries retains the existing empty-create request identity. Rows
    with catalog identities use the same binding validation as entry-save;
    display metadata alone remains an unverified draft. Retired online provider
    references are rejected before any write.
    """
    sequence = store.sequence
    name = _text(name, "name", 120, True)
    if not isinstance(purpose, str) or purpose not in PLAYLIST_PURPOSES:
        raise StoreError("playlist purpose must be music, jingle, advert, announcement, programme, other or sweeper")
    payload = {"name": name}
    # Explicit music and omitted purpose retain the legacy empty-create hash.
    if purpose != "music":
        payload["purpose"] = purpose
    if entries is not None:
        if not isinstance(entries, list) or not 1 <= len(entries) <= 500:
            raise StoreError("entries must be a nonempty list of at most 500 metadata rows")
        entries = [_metadata(entry) for entry in entries]
        payload["entries"] = entries
    station_id, receipt = _operation(store, principal, station_id, idempotency_key, "desktop-playlists/create", payload)
    if receipt[2] is not None:
        return _replay(store, station_id, receipt[2])
    from .desktop_api import MEDIA_BINDING_KEYS, _media_binding
    for entry in entries or []:
        _online_binding(entry, station_id, store._state)
        if "analysis_media" in entry:
            _media_binding(entry["analysis_media"], station_id, store._state)
        if MEDIA_BINDING_KEYS.intersection(entry):
            _media_binding({key: entry[key] for key in MEDIA_BINDING_KEYS}, station_id, store._state)
    playlist_id, version_id = str(uuid4()), str(uuid4())
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("playlists", playlist_id, {"station_id": station_id, "code": "native-" + playlist_id,
        "name": name, "purpose": purpose, "status": "active", "current_version_id": None, "draft_version_id": version_id,
        "created_by": principal.user_id, "created_at": now})
    tx.put("playlist_versions", version_id, {"playlist_id": playlist_id, "version_no": 1,
        "status": "draft", "created_by": principal.user_id, "created_at": now})
    for ordinal, entry in enumerate(entries or []):
        tx.put(COLLECTION, str(uuid4()), entry | {"station_id": station_id,
            "playlist_version_id": version_id, "ordinal": ordinal, "active_ordinal": ordinal,
            "status": "active", "media_state": "unverified", "created_by": principal.user_id,
            "created_at": now, "updated_by": principal.user_id, "updated_at": now})
    _receipt(tx, principal, station_id, idempotency_key, "desktop-playlists/create", receipt, {"playlist_id": playlist_id})
    tx.commit()
    return _snapshot(store, station_id, playlist_id)


def rename_playlist(store, principal, station_id, playlist_id, name, *, expected_revision, idempotency_key):
    sequence = store.sequence
    name = _text(name, "name", 120, True)
    payload = {"playlist_id": playlist_id, "name": name, "expected_revision": expected_revision}
    station_id, receipt = _operation(store, principal, station_id, idempotency_key, "desktop-playlists/rename", payload)
    if receipt[2] is not None:
        return _replay(store, station_id, receipt[2])
    playlist, _version = _playlist(store, station_id, playlist_id, expected_revision)
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("playlists", playlist["_id"], _body(playlist) | {"name": name}, expected_revision=playlist["_rev"])
    _receipt(tx, principal, station_id, idempotency_key, "desktop-playlists/rename", receipt, {"playlist_id": playlist["_id"]})
    tx.commit()
    return _snapshot(store, station_id, playlist["_id"])


def _configuration_uses(store, station_id, playlist_id):
    """Describe schedule references without preventing an unrelated save."""
    uses = []
    for document in store.scan("desktop_configurations"):
        if document["station_id"] != station_id or document["status"] != "active":
            continue
        kind, config = document["kind"], document["config"]
        locations = []
        if kind == "automation":
            locations.extend(key for key in ("jinglePlaylist", "fallbackPlaylist")
                             if config.get(key) == playlist_id)
            locations.extend("source " + row["source_id"] for row in config["sources"]
                             if row["playlist_id"] == playlist_id)
        elif kind in {"automation_calendar", "weekly_programme"}:
            calendar = config["calendar"] if kind == "weekly_programme" else config
            for definition in calendar.get("rotation_library", []):
                if any(row["playlist_id"] == playlist_id for row in definition["sources"]):
                    locations.append("rotation: " + definition["name"])
            for event in calendar["events"]:
                if any(row.get("playlist_id") == playlist_id for row in event["rotations"]):
                    locations.append(event["date"] + " · " + event["title"])
        if locations:
            uses.append({"document_id": document["_id"], "kind": kind,
                         "name": document["name"], "locations": locations})
    return uses


def delete_playlist(store, principal, station_id, playlist_id, *, expected_revision, idempotency_key):
    sequence = store.sequence
    payload = {"playlist_id": playlist_id, "expected_revision": expected_revision}
    station_id, receipt = _operation(store, principal, station_id, idempotency_key, "desktop-playlists/delete", payload, "manage")
    if receipt[2] is not None:
        return receipt[2]
    playlist, _version = _playlist(store, station_id, playlist_id, expected_revision)
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("playlists", playlist["_id"], _body(playlist) | {"status": "disabled"}, expected_revision=playlist["_rev"])
    uses = _configuration_uses(store, station_id, playlist["_id"])
    result = {"playlist_id": playlist["_id"], "deleted": True, "configuration_uses": uses}
    if uses:
        result["warning"] = ("Playlist silindi; şu programlarda kullanılmaya devam ediyor: " +
                             ", ".join(use["name"] for use in uses) +
                             ". Bu kaynak satırını değiştirmek için etkin bir playlist seçin.")
    _receipt(tx, principal, station_id, idempotency_key, "desktop-playlists/delete", receipt, result)
    tx.commit()
    return result


def save_entry(store, principal, station_id, playlist_id, metadata, *, expected_revision,
               idempotency_key, entry_id=None, expected_entry_revision=None):
    sequence = store.sequence
    metadata = _metadata(metadata)
    payload = {"playlist_id": playlist_id, "metadata": metadata, "expected_revision": expected_revision,
               "entry_id": entry_id, "expected_entry_revision": expected_entry_revision}
    station_id, receipt = _operation(store, principal, station_id, idempotency_key, "desktop-playlists/entry-save", payload)
    if receipt[2] is not None:
        return _replay(store, station_id, receipt[2])
    playlist, version = _playlist(store, station_id, playlist_id, expected_revision)
    _online_binding(metadata, station_id, store._state)
    from .desktop_api import MEDIA_BINDING_KEYS, _media_binding
    if "analysis_media" in metadata:
        _media_binding(metadata["analysis_media"], station_id, store._state)
    if MEDIA_BINDING_KEYS.intersection(metadata):
        _media_binding({key: metadata[key] for key in MEDIA_BINDING_KEYS}, station_id, store._state)
    tracks = _tracks(store, version["_id"])
    existing = store.get(COLLECTION, _id(entry_id, "entry_id")) if entry_id else None
    if entry_id and (not existing or existing["playlist_version_id"] != version["_id"] or
                     existing["status"] != "active" or type(expected_entry_revision) is not int or
                     existing["_rev"] != expected_entry_revision):
        raise Conflict("playlist entry changed or is absent")
    if not entry_id and expected_entry_revision is not None:
        raise Conflict("new playlist entry cannot have an expected revision")
    if existing is None and len(tracks) >= 500:
        raise StoreError("playlist accepts at most 500 metadata entries")
    now = datetime.now(timezone.utc).isoformat()
    identifier = existing["_id"] if existing else str(uuid4())
    ordinal = existing["ordinal"] if existing else len(tracks)
    body = metadata | {"station_id": station_id, "playlist_version_id": version["_id"], "ordinal": ordinal,
        "active_ordinal": ordinal, "status": "active", "media_state": "unverified",
        "created_at": existing["created_at"] if existing else now,
        "created_by": existing["created_by"] if existing else principal.user_id,
        "updated_at": now, "updated_by": principal.user_id}
    if existing:
        # Editing display metadata does not replace the immutable source
        # identity used by XML duplicate handling and source/cue inspection.
        body.update({key: existing[key] for key in REKORDBOX_SOURCE if key in existing})
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put(COLLECTION, identifier, body, expected_revision=existing["_rev"] if existing else None)
    tx.put("playlists", playlist["_id"], _body(playlist), expected_revision=playlist["_rev"])
    _receipt(tx, principal, station_id, idempotency_key, "desktop-playlists/entry-save", receipt, {"playlist_id": playlist["_id"]})
    tx.commit()
    return _snapshot(store, station_id, playlist["_id"])


def append_entries(store, principal, station_id, playlist_id, entries, *, expected_revision, idempotency_key):
    """Append an ordered selected batch with one revision and durable receipt.

    Validate all rows and source bindings before opening a transaction. Replays
    return the current snapshot without repeating validation or appending rows.
    """
    sequence = store.sequence
    if not isinstance(entries, list) or not 1 <= len(entries) <= 500:
        raise StoreError("entries must be a nonempty list of at most 500 metadata rows")
    entries = [_metadata(entry) for entry in entries]
    _integer(expected_revision, "expected_revision", 1, 2**63 - 1)
    payload = {"playlist_id": playlist_id, "entries": entries, "expected_revision": expected_revision}
    operation = "desktop-playlists/entries-append"
    station_id, receipt = _operation(store, principal, station_id, idempotency_key, operation, payload)
    if receipt[2] is not None:
        return _replay(store, station_id, receipt[2])
    playlist, version = _playlist(store, station_id, playlist_id, expected_revision)
    tracks = _tracks(store, version["_id"])
    if len(tracks) + len(entries) > 500:
        raise StoreError("playlist accepts at most 500 metadata entries")
    from .desktop_api import MEDIA_BINDING_KEYS, _media_binding
    for metadata in entries:
        _online_binding(metadata, station_id, store._state)
        if "analysis_media" in metadata:
            _media_binding(metadata["analysis_media"], station_id, store._state)
        if MEDIA_BINDING_KEYS.intersection(metadata):
            _media_binding({key: metadata[key] for key in MEDIA_BINDING_KEYS}, station_id, store._state)
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    for ordinal, metadata in enumerate(entries, start=len(tracks)):
        tx.put(COLLECTION, str(uuid4()), metadata | {"station_id": station_id,
            "playlist_version_id": version["_id"], "ordinal": ordinal, "active_ordinal": ordinal,
            "status": "active", "media_state": "unverified", "created_by": principal.user_id,
            "created_at": now, "updated_by": principal.user_id, "updated_at": now})
    tx.put("playlists", playlist["_id"], _body(playlist), expected_revision=playlist["_rev"])
    _receipt(tx, principal, station_id, idempotency_key, operation, receipt, {"playlist_id": playlist["_id"]})
    tx.commit()
    return _snapshot(store, station_id, playlist["_id"])


def remove_entries(store, principal, station_id, playlist_id, entries, *, expected_revision, idempotency_key):
    sequence = store.sequence
    payload = {"playlist_id": playlist_id, "entries": entries, "expected_revision": expected_revision}
    station_id, receipt = _operation(store, principal, station_id, idempotency_key, "desktop-playlists/entry-remove", payload)
    if receipt[2] is not None:
        return _replay(store, station_id, receipt[2])
    playlist, version = _playlist(store, station_id, playlist_id, expected_revision)
    if not isinstance(entries, list) or not 1 <= len(entries) <= 500:
        raise StoreError("entries must be a nonempty list of at most 500 identities/revisions")
    removed = {}
    for selection in entries:
        if not isinstance(selection, dict) or set(selection) != {"entry_id", "expected_revision"}:
            raise StoreError("removed entries need entry_id and expected_revision")
        identifier = _id(selection["entry_id"], "entry_id")
        row = store.get(COLLECTION, identifier)
        if identifier in removed or not row or row["playlist_version_id"] != version["_id"] or row["status"] != "active" or \
                type(selection["expected_revision"]) is not int or row["_rev"] != selection["expected_revision"]:
            raise Conflict("one of the selected playlist entries changed")
        removed[identifier] = row
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    now = datetime.now(timezone.utc).isoformat()
    for row in removed.values():
        tx.put(COLLECTION, row["_id"], _body(row) | {"status": "removed", "active_ordinal": None,
            "updated_at": now, "updated_by": principal.user_id}, expected_revision=row["_rev"])
    remaining = [row for row in _tracks(store, version["_id"]) if row["_id"] not in removed]
    for ordinal, row in enumerate(remaining):
        if row["ordinal"] != ordinal:
            tx.put(COLLECTION, row["_id"], _body(row) | {"ordinal": ordinal, "active_ordinal": ordinal,
                "updated_at": now, "updated_by": principal.user_id}, expected_revision=row["_rev"])
    tx.put("playlists", playlist["_id"], _body(playlist), expected_revision=playlist["_rev"])
    _receipt(tx, principal, station_id, idempotency_key, "desktop-playlists/entry-remove", receipt, {"playlist_id": playlist["_id"]})
    tx.commit()
    return _snapshot(store, station_id, playlist["_id"])


def reorder_entries(store, principal, station_id, playlist_id, entries, *, expected_revision, idempotency_key):
    sequence = store.sequence
    payload = {"playlist_id": playlist_id, "entries": entries, "expected_revision": expected_revision}
    station_id, receipt = _operation(store, principal, station_id, idempotency_key, "desktop-playlists/reorder", payload)
    if receipt[2] is not None:
        return _replay(store, station_id, receipt[2])
    playlist, version = _playlist(store, station_id, playlist_id, expected_revision)
    tracks = {row["_id"]: row for row in _tracks(store, version["_id"])}
    if not isinstance(entries, list) or any(not isinstance(item, str) for item in entries) or \
            len(entries) != len(tracks) or len(set(entries)) != len(entries) or set(entries) != set(tracks):
        raise Conflict("reorder must contain every active entry exactly once")
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    now = datetime.now(timezone.utc).isoformat()
    for ordinal, identifier in enumerate(entries):
        row = tracks[identifier]
        if row["ordinal"] != ordinal:
            tx.put(COLLECTION, identifier, _body(row) | {"ordinal": ordinal, "active_ordinal": ordinal,
                "updated_at": now, "updated_by": principal.user_id}, expected_revision=row["_rev"])
    tx.put("playlists", playlist["_id"], _body(playlist), expected_revision=playlist["_rev"])
    _receipt(tx, principal, station_id, idempotency_key, "desktop-playlists/reorder", receipt, {"playlist_id": playlist["_id"]})
    tx.commit()
    return _snapshot(store, station_id, playlist["_id"])
