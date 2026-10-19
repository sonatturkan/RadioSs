"""Station resets for the desktop Settings > Sıfırlama page.

A reset retires rows exactly as their own delete workflows do: configuration
documents and playlists become ``disabled``, library songs ``disabled`` and
their playlist rows ``removed``. Nothing is physically erased, and by default
a verified backup of the whole store is written first, so every reset can be
restored. Audit, run and history records are never touched.
"""
from __future__ import annotations

from datetime import datetime, timezone
import os
from pathlib import Path
import shutil

from . import _platform
from . import desktop_api as api
from .identity import require
from .storage import Conflict, StoreError

SCOPES = ("settings", "playlists", "media", "mixer", "automation", "factory")
SETTINGS_KINDS = frozenset({"audio_preferences", "server", "web", "midi_mapping", "audio_routing", "online_services",
                            "broadcast_stream", "broadcast_record", "broadcast_metadata", "desktop_preferences", "live_players"})
PLAYLIST_KINDS = frozenset({"external_playlist"})
MIXER_KINDS = frozenset({"dj_mixer", "channel", "sampler"})
AUTOMATION_KINDS = frozenset({"automation", "automation_calendar", "weekly_programme", "transition", "playout_policy",
                              "local_event"})
# Receipts and logs stay through every reset, including factory.
KEPT_KINDS = frozenset({"playout_history", "advert_local_run", "advert_report", "advert_notifications",
                        "broadcast_local_run", "admin_recovery", "admin_restore_fence"})
CONFIRMATION = "SIFIRLA"
OPERATION = "desktop-reset/apply"


def _now():
    return datetime.now(timezone.utc).isoformat()


def _body(row):
    return {key: value for key, value in row.items() if key not in {"_id", "_rev"}}


def _scopes(value):
    if not isinstance(value, list) or not value or len(value) > len(SCOPES) or \
            any(not isinstance(scope, str) or scope not in SCOPES for scope in value) or len(set(value)) != len(value):
        raise StoreError("reset scopes must be a non-empty list of distinct known scopes")
    return sorted(value, key=SCOPES.index)


def _kinds(scopes):
    if "factory" in scopes:
        return frozenset(api.KINDS) - KEPT_KINDS
    kinds = set()
    for scope, group in (("settings", SETTINGS_KINDS), ("playlists", PLAYLIST_KINDS),
                         ("mixer", MIXER_KINDS), ("automation", AUTOMATION_KINDS)):
        if scope in scopes:
            kinds |= group
    return frozenset(kinds - KEPT_KINDS)


def _plan(store, sid, scopes):
    """Rows to retire, read from one consistent store state."""
    kinds = _kinds(scopes)
    everything = "factory" in scopes
    documents, skipped = [], []
    for row in store.lookup("desktop_configurations", "station", sid):
        if row["status"] != "active" or row["kind"] not in kinds:
            continue
        if row["kind"] == "advert_break" and "bridge" in row["config"]:
            skipped.append({"kind": row["kind"], "name": row["name"],
                            "reason": "Reklam köprüsüne bağlı arası; Reklam sayfasından iptal edin."})
            continue
        documents.append(row)
    playlists = [row for row in store.lookup("playlists", "station", sid) if row["status"] == "active"] \
        if everything or "playlists" in scopes else []
    library = [row for row in store.lookup("desktop_media_library", "station", sid) if row["status"] == "active"] \
        if everything or "media" in scopes else []
    removed_ids = {row["_id"] for row in library}
    profiles = [(kind, row) for kind in ("view_profiles", "import_profiles") for row in store.scan(kind)
                if row["station_id"] == sid and not row.get("retired_at")] if everything or "settings" in scopes else []
    retired = {row["_id"] for row in playlists}
    entries, renumber, touched = [], [], {}
    if removed_ids:
        from .desktop_playlist import _tracks
        for playlist in store.lookup("playlists", "station", sid):
            if playlist["status"] != "active" or playlist["_id"] in retired or not playlist.get("draft_version_id"):
                continue
            version = store.get("playlist_versions", playlist["draft_version_id"])
            if not version or version["status"] != "draft":
                continue
            tracks = _tracks(store, version["_id"])
            gone = [row for row in tracks if row.get("media_id") in removed_ids]
            if not gone:
                continue
            entries.extend(gone)
            remaining = [row for row in tracks if row.get("media_id") not in removed_ids]
            renumber.extend((row, ordinal) for ordinal, row in enumerate(remaining) if row["ordinal"] != ordinal)
            touched[playlist["_id"]] = playlist
    return {"documents": documents, "skipped": skipped, "playlists": playlists, "library": library, "profiles": profiles,
            "entries": entries, "renumber": renumber, "touched": list(touched.values())}


def _summary(plan):
    kinds = {}
    for row in plan["documents"]:
        kinds[row["kind"]] = kinds.get(row["kind"], 0) + 1
    return {"documents": dict(sorted(kinds.items())), "document_count": len(plan["documents"]),
            "playlists": len(plan["playlists"]), "media": len(plan["library"]),
            "playlist_entries": len(plan["entries"]), "skipped": plan["skipped"], "profiles": len(plan["profiles"])}


def _retired_profile(kind, row, now, principal):
    """Profiles cannot be deleted (the store archives them): mark the row
    retired, free its unique name and keep the old name for the record."""
    body = _body(row)
    body.update(retired_at=now, retired_by=principal.user_id, retired_name=body["name"],
                name=f"{body['name']} · sıfırlandı {row['_id'][:8]}",
                config_version=body["config_version"] + 1)
    if kind == "import_profiles":
        body["status"] = "disabled"
    return body


def preview(store, principal, station_id, scopes):
    sid = api._id(station_id, "station_id")
    require(store, principal, "manage", sid)
    scopes = _scopes(scopes)
    with store._locked(exclusive=False):
        store._reload_locked()
        plan = _plan(store, sid, scopes)
        sequence = store.sequence
    return {"scopes": scopes, "sequence": sequence, **_summary(plan),
            "kept": sorted(KEPT_KINDS)}


# Copies (backups, transfer packages) must leave the disk usable: the live
# service journals on the same disk and a full filesystem rejects its writes.
SPACE_MARGIN_BYTES = 512 * 1024 * 1024


def _size_text(value):
    for unit, scale in (("GB", 1024 ** 3), ("MB", 1024 ** 2), ("KB", 1024)):
        if value >= scale:
            return f"{value / scale:.1f} {unit}".replace(".", ",")
    return f"{value} bayt"


def require_space(needs, what):
    """Refuse before writing when a filesystem would keep less than the margin free.

    ``needs`` pairs an existing folder with the bytes the operation writes
    there; folders on the same filesystem add up."""
    totals = {}
    for folder, size in needs:
        entry = totals.setdefault(os.stat(folder).st_dev, [folder, 0])
        entry[1] += max(0, int(size))
    for folder, size in totals.values():
        free = shutil.disk_usage(folder).free
        if size + SPACE_MARGIN_BYTES > free:
            raise StoreError(f"{what} için yeterli boş alan yok: yaklaşık {_size_text(size)} yazılacak, "
                             f"{folder} diskinde {_size_text(free)} boş; en az {_size_text(SPACE_MARGIN_BYTES)} boş kalmalı.")


def _backup_bytes(store):
    """Bytes a whole-store backup writes: database files and every referenced object."""
    from .backup import _media_refs
    from .retention import archive_file_refs
    with store._locked(exclusive=False):
        store._reload_locked()
        objects = _media_refs(store._state, store.journal.name)
        cold = archive_file_refs(store._state, store.journal.name)
    total = sum(obj["size_bytes"] for obj in objects)
    total += sum(entry["size_bytes"] for entry in cold if type(entry.get("size_bytes")) is int)
    for source in (store.journal, store.meta_file, store.checkpoint_file):
        try:
            total += source.stat().st_size
        except OSError:
            pass
    return total


def _backup_folder(store, name):
    """A verified whole-store backup in a fresh folder called ``name``."""
    from .backup import create_backup
    # The service may name the backup folder (config/storage.ini, read per
    # backup by tools/radios_database.py); the default stays beside the data.
    root = getattr(store, "desktop_backup_root", None)
    if callable(root):
        root = root()
    if root:
        # A storage.ini folder (often another disk): a folder created here is
        # private like the default, an existing one is used as configured.
        root = Path(root)
        if not root.is_dir():
            try:
                _platform.mkdir_private(root, parents=True)
            except FileExistsError:
                if not root.is_dir():
                    raise
    else:
        # data\backups beside the database: owner-only (Linux 0700), never a link.
        root = Path(store.root).parent / "backups"
        _platform.mkdir_private(root, parents=True, exist_ok=True)
    require_space([(root, _backup_bytes(store))], "Yedek")
    target = root / name
    suffix = 1
    while target.exists():
        suffix += 1
        target = root / f"{name}-{suffix}"
    return create_backup(store, target)


def _stamp():
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def _backup(store, scopes):
    return _backup_folder(store, f"sifirlama-{_stamp()}-{'-'.join(scopes)}")["path"]


def backup(store, principal, station_id, idempotency_key=None):
    """Settings › Sıfırlama "Şimdi yedek al": a verified backup with no reset."""
    api._station(store, principal, station_id, "manage")
    if idempotency_key is not None:
        api._id(idempotency_key, "idempotency_key")
    created = _backup_folder(store, f"yedek-{_stamp()}")
    return {"path": created["path"], "created_at": _now(), "store_id": created["store_id"],
            "sequence": created["sequence"], "files": created["files"], "media_objects": created["media_objects"]}


def apply(store, principal, station_id, scopes, confirm, idempotency_key, backup=True):
    sid = api._id(station_id, "station_id")
    require(store, principal, "manage", sid)
    scopes = _scopes(scopes)
    if confirm != CONFIRMATION:
        raise StoreError(f"reset requires the confirmation text {CONFIRMATION}")
    if type(backup) is not bool:
        raise StoreError("backup must be boolean")
    payload = {"scopes": scopes, "backup": backup}
    receipt, digest, replay = api.operation_replay(store, principal, sid, idempotency_key, OPERATION, payload)
    if replay is not None:
        return replay
    backup_path = _backup(store, scopes) if backup else None
    with store._locked(exclusive=False):
        store._reload_locked()
        sequence = store.sequence
        plan = _plan(store, sid, scopes)
    now = _now()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    for row in plan["documents"]:
        tx.put("desktop_configurations", row["_id"], _body(row) | {"status": "disabled", "updated_by": principal.user_id,
               "updated_at": now}, expected_revision=row["_rev"])
    for row in plan["playlists"]:
        tx.put("playlists", row["_id"], _body(row) | {"status": "disabled"}, expected_revision=row["_rev"])
    from .desktop_playlist import COLLECTION
    for row in plan["entries"]:
        tx.put(COLLECTION, row["_id"], _body(row) | {"status": "removed", "active_ordinal": None,
               "updated_at": now, "updated_by": principal.user_id}, expected_revision=row["_rev"])
    for row, ordinal in plan["renumber"]:
        tx.put(COLLECTION, row["_id"], _body(row) | {"ordinal": ordinal, "active_ordinal": ordinal,
               "updated_at": now, "updated_by": principal.user_id}, expected_revision=row["_rev"])
    for row in plan["touched"]:
        tx.put("playlists", row["_id"], _body(row), expected_revision=row["_rev"])
    for row in plan["library"]:
        tx.put("desktop_media_library", row["_id"], _body(row) | {"status": "disabled", "updated_by": principal.user_id,
               "updated_at": now}, expected_revision=row["_rev"])
    for kind, row in plan["profiles"]:
        tx.put(kind, row["_id"], _retired_profile(kind, row, now, principal), expected_revision=row["_rev"])
    result = {"scopes": scopes, "backup": backup_path, **_summary(plan), "kept": sorted(KEPT_KINDS),
              "completed_at": now}
    api.add_operation(tx, principal, sid, receipt, idempotency_key, OPERATION, digest, result)
    try:
        tx.commit()
    except Conflict as exc:
        raise Conflict("station data changed while resetting; nothing was reset, try again") from exc
    return result
