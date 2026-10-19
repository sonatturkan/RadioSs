"""Settings › Export / Import: the ``.st`` transfer package (guide §12).

A package is one ZIP (``allowZip64``) with three top folders. ``ayarlar/``
carries the station's configuration documents and playlists, ``medya/`` the
song files in their album layout and ``bilgiler/`` the manifest, the song table
and one analysis record per song. Export reads the database once under a
shared lock and copies managed object bytes without holding it. Import writes
the songs under the chosen media root, registers them exactly as the desktop
import does (managed object, library row, analysis, markers, rhythm), then
restores playlists and documents under their original identities with media
bindings rebound by SHA-256. Jobs run in a daemon thread of the data service
and are polled over HTTP; at most one job runs at a time.
"""
from __future__ import annotations

from copy import deepcopy
import configparser
from datetime import datetime, timezone
import hashlib
import json
import ntpath
import os
from pathlib import Path
import posixpath
import re
import stat
import threading
import time
import unicodedata
from uuid import NAMESPACE_URL, uuid4, uuid5
import zipfile

from . import __version__
from . import _platform
from . import desktop_api as api
from . import desktop_media
from . import desktop_playlist
from . import desktop_reset
from . import settings as station_settings
from . import media as media_service
from .catalog import NotFound
from .identity import Forbidden
from .runtime_version import DESKTOP_API_VERSION
from .storage import Conflict, StoreError

FORMAT = "radios-transfer-package"
VERSION = 1
MEDIA_VERSION = 1
PLAYLISTS = "playlists"
LOCAL_SETTINGS = "local_settings"
# Advertising campaigns (Kılavuz 18.10 s.21 "bütün radyo ayarları ve
# aralarındaki ilişkiler"): sales fields and the spot's audio by SHA-256.
# Approvals are legal acts of this station; a campaign arrives as a draft.
CAMPAIGNS = "campaigns"
_CAMPAIGN_OPEN = frozenset({"draft", "approved", "active", "paused"})
PROFILE_KINDS = frozenset({"view_profiles", "import_profiles"})
LOCAL_FILES = frozenset({"ui-preferences.ini", "audio-panel.ini", "media-archive.ini",
                         "library-analysis.ini", "storage.ini"})
# Display order and Turkish labels of the transferable groups.
LABELS = {
    "audio_preferences": "Ses tercihleri", "audio_routing": "Ses yönlendirme",
    "dj_mixer": "DJ mikseri", "channel": "DJ kanalları", "sampler": "DJ sampler",
    "live_players": "Canlı oynatıcılar", "automation": "Rotasyonlar ve şemalar",
    "automation_calendar": "Program takvimi", "weekly_programme": "Haftalık programlar",
    "programme": "Programlar", "local_event": "Yerel eventler", PLAYLISTS: "Playlistler",
    "playout_policy": "Yayın politikası", "transition": "Geçişler", "markers": "İşaretler",
    "emergency": "Acil yayın", "advert_break": "Reklam kuşakları", CAMPAIGNS: "Reklam kampanyaları",
    "broadcast_stream": "İnternet radyosu", "broadcast_record": "Kayıt",
    "broadcast_metadata": "Akış metadata", "server": "Sunucular", "web": "Web erişimi",
    "midi_mapping": "MIDI eşlemeleri", "copyright_policy": "Telif politikası",
    "desktop_preferences": "Masaüstü tercihleri",
    LOCAL_SETTINGS: "Yerel arşiv, analiz ve depolama ayarları",
    "view_profiles": "Görünüm profilleri", "import_profiles": "İçe aktarım profilleri",
    "admin_recovery": "Yönetim kurtarma", "admin_restore_fence": "Yönetim kurtarma",
}
# Configuration kinds a package carries: no retired online kinds, no run
# receipts or logs (they stay through a reset and are never transferred).
EXPORT_KINDS = frozenset(api.KINDS) - api.LEGACY_KINDS - desktop_reset.KEPT_KINDS
GROUPS = tuple(kind for kind in LABELS if kind in EXPORT_KINDS or kind in {PLAYLISTS, LOCAL_SETTINGS, CAMPAIGNS} | PROFILE_KINDS)
SINGLETON_KINDS = frozenset({"playout_policy", "copyright_policy"})
# Documents that reference other documents are restored after their targets.
_KIND_ORDER = ("automation", "automation_calendar", "weekly_programme", "local_event", "live_players", "programme")
_SENSITIVE = re.compile(r"password|passwd|token|secret|api[_-]?key|authorization|credential|private[_-]?key",
                        re.IGNORECASE)
# Project folders a package may never be written to or imported into, as path
# components (joined with the native separator, compared case-insensitively).
_PROTECTED = (("config",), ("src",), ("include",), ("tools",), ("tests",), ("docs",), ("design",),
              ("assets",), ("vendor",), ("packaging",), ("cmake",), ("database", "SDBASE"))
_PROJECT_ROOT = Path(__file__).resolve().parents[4]
_FILE_NAME = re.compile(r"[^/\\\x00-\x1f\x7f]{1,197}\.st\Z")
# Without LongPathsEnabled, Win32 programs (Explorer, players, the RadioS GUI) cannot
# open longer paths although the NT-path based shim could create them: MAX_PATH - 1
# for files, the CreateDirectoryW limit (MAX_PATH - 12 - 1) for folders, and the
# storage-folder rule of core spec 5.5 for chosen folders.
_MAX_FILE_PATH = 259
_MAX_DIRECTORY_PATH = 247
_MAX_CHOSEN_FOLDER = 240
_NO_ACL_WARNING = ("Hedef disk dosya izinlerini desteklemiyor; paket gizli değil "
                   "(kayıtlı akış parolaları içerebilir).")
_SHA = re.compile(r"[0-9a-f]{64}\Z")
_TURKISH = str.maketrans("çğıöşüÇĞİÖŞÜâîû", "cgiosuCGIOSUaiu")
_CHUNK = 1024 * 1024
_TRANSACTION_BYTES = 12 * 1024 * 1024
_FULL_CHECK_BYTES = 256 * 1024 * 1024
_SETTINGS_ALLOWANCE = 32 * 1024 * 1024
MAX_FINISHED_JOBS = 32
# Documents listed per group for a per-document export choice, and the most
# identities one partially selected group may carry.
MAX_GROUP_DOCUMENTS = 1000
_CANCEL_WAIT = 1.5
_ANALYSIS_STRIP = frozenset({"_id", "_rev", "station_id", "item_id", "version_id", "media_file_id",
                             "created_by", "updated_by"})
_DROP = object()


class _Cancelled(Exception):
    """The operator cancelled the running job."""


# --- jobs -------------------------------------------------------------------

_LOCK = threading.Lock()
_JOBS: dict[str, "_Job"] = {}
_RUNNING: "_Job | None" = None


class _Job:
    def __init__(self, kind, station_id, package_path):
        self.job_id = str(uuid4())
        self.kind = kind
        self.station_id = station_id
        self.package_path = package_path
        self.state = "running"
        self.progress = 0
        self.message = "Başlatılıyor…"
        self.result = None
        self.cancel = threading.Event()
        self.started = time.monotonic()
        self.finished = None
        self.thread = None

    def update(self, progress=None, message=None):
        with _LOCK:
            if progress is not None:
                self.progress = max(0, min(100, int(progress)))
            if message is not None:
                self.message = message

    def finish(self, state, message, result):
        with _LOCK:
            self.state, self.message, self.result = state, message, result
            self.progress = 100 if state == "done" else self.progress
            self.finished = time.monotonic()

    def view(self):
        with _LOCK:
            view = {"job_id": self.job_id, "kind": self.kind, "state": self.state,
                    "progress": self.progress, "message": self.message,
                    "package_path": self.package_path}
            if self.result is not None:
                view["result"] = deepcopy(self.result)
            return view

    def check(self):
        if self.cancel.is_set():
            raise _Cancelled()


def _start(job, worker):
    global _RUNNING
    with _LOCK:
        if _RUNNING is not None and _RUNNING.state == "running":
            raise StoreError("Bir aktarım işi zaten çalışıyor.")
        finished = sorted((row for row in _JOBS.values() if row.state != "running"),
                          key=lambda row: row.finished or 0)
        for stale in finished[:max(0, len(finished) - MAX_FINISHED_JOBS)]:
            _JOBS.pop(stale.job_id, None)
        _JOBS[job.job_id] = job
        _RUNNING = job

    def run():
        try:
            result = worker(job)
        except _Cancelled:
            job.finish("cancelled", "İptal edildi.", None)
        except (StoreError, Conflict, NotFound, Forbidden) as exc:
            job.finish("failed", f"Aktarım başarısız: {exc}", None)
        except OSError as exc:
            job.finish("failed", f"Dosya işlemi başarısız: {exc.strerror or exc}", None)
        except Exception as exc:  # noqa: BLE001 - the job must always reach a terminal state
            job.finish("failed", f"Beklenmeyen hata: {type(exc).__name__}: {exc}", None)
        else:
            job.finish("done", "Tamamlandı.", result)

    thread = threading.Thread(target=run, name="radios-transfer-" + job.job_id[:8], daemon=True)
    job.thread = thread
    try:
        thread.start()
    except BaseException as exc:
        # No worker (thread limit, interpreter shutdown): the job must not stay
        # "running" forever and block every later transfer.
        job.finish("failed", f"Aktarım işi başlatılamadı: {exc}", None)
        raise
    return job


def cancel_running(timeout=5.0):
    """Service stop path: cancel the running transfer job and wait up to ``timeout``
    seconds for it to remove its partial files.  True when no job is running any more."""
    with _LOCK:
        running = _RUNNING
    if running is None or running.state != "running":
        return True
    running.cancel.set()
    thread = running.thread
    if thread is not None and thread is not threading.current_thread():
        thread.join(max(0.0, float(timeout)))
    return running.state != "running"


def _find(store, principal, station_id, job_id):
    sid, _ = api._station(store, principal, station_id, "manage")
    key = api._id(job_id, "job_id")
    with _LOCK:
        job = _JOBS.get(key)
    if job is None or job.station_id != sid:
        raise StoreError("Aktarım işi bulunamadı.")
    return job


def job(store, principal, station_id, job_id):
    return _find(store, principal, station_id, job_id).view()


def job_cancel(store, principal, station_id, job_id):
    found = _find(store, principal, station_id, job_id)
    if found.state == "running":
        found.cancel.set()
        deadline = time.monotonic() + _CANCEL_WAIT
        while found.state == "running" and time.monotonic() < deadline:
            time.sleep(0.02)
    return {"job_id": found.job_id, "state": found.state}


# --- shared helpers ---------------------------------------------------------

def _now():
    return datetime.now(timezone.utc).isoformat()


def _json(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=1, allow_nan=False) + "\n").encode("utf-8")


def _utf16_units(text):
    return len(text) + sum(1 for char in text if ord(char) > 0xFFFF)


def _fits(path, limit):
    """True when Win32 programs can use ``path`` (always with LongPathsEnabled)."""
    return _platform.long_paths_enabled() or _utf16_units(str(path)) <= limit


def _protected(value):
    """``value`` is inside one of RadioS's own source or settings folders."""
    root = os.path.realpath(_PROJECT_ROOT)
    return any(_platform.is_inside(os.path.join(root, *parts), value) for parts in _PROTECTED)


def _directory(value, what):
    """An absolute, canonical, existing, writable, link-free folder outside the project
    sources: the storage-folder rules of core spec 5.5 (``storage_locations.c``)."""
    if not isinstance(value, str) or not value:
        raise StoreError(f"{what} klasörü seçilmedi.")
    if len(value.encode("utf-8", "surrogatepass")) > 4000 or \
            any(ord(char) < 0x20 or ord(char) == 0x7F for char in value):
        raise StoreError(f"{what} klasör yolu geçersiz.")
    if not _fits(value, _MAX_CHOSEN_FOLDER):
        raise StoreError(f"{what} klasör yolu çok uzun; Windows uzun yol desteği kapalıyken en çok "
                         f"{_MAX_CHOSEN_FOLDER} karakter olabilir.")
    if not _platform.is_absolute(value):
        raise StoreError(f"{what} klasör yolu mutlak olmalı (örn. D:\\RadioS\\Paketler).")
    if not _platform.is_canonical(value):
        raise StoreError(f"{what} klasör yolu sadeleştirilmiş olmalı (.., \\\\, / ve sondaki \\ olmadan, "
                         "sürücü harfi büyük).")
    if _platform.is_root(value):
        raise StoreError("Sürücü kökü seçilemez.")
    try:
        info = os.lstat(value)
    except FileNotFoundError:
        raise StoreError(f"Klasör bulunamadı: {value}") from None
    except OSError as exc:
        raise StoreError(f"Klasör incelenemedi: {value}") from exc
    if _platform.stat_is_link(info):
        raise StoreError(f"Klasör sembolik bağlantı veya bağlantı noktası (junction) olamaz: {value}")
    if not stat.S_ISDIR(info.st_mode):
        raise StoreError(f"Seçilen yol bir klasör değil: {value}")
    try:
        real = os.path.realpath(value)
    except (OSError, ValueError):
        real = None
    # A link, junction or mapped network drive on the way, or an 8.3 short name.
    if real is None or not _platform.path_equal(real, value):
        raise StoreError(f"Klasör yolunda bağlantı var; gerçek yolu seçin: {value}")
    if not _platform.dir_writable(value):
        raise StoreError(f"Klasöre yazma izni yok: {value}")
    if _protected(value):
        raise StoreError(f"RadioS'un kendi kaynak veya ayar klasörü seçilemez: {value}")
    return Path(value)


def _package(value):
    if not isinstance(value, str) or len(value) > 4000 or \
            any(ord(char) < 0x20 or ord(char) == 0x7F for char in value) or \
            not _platform.is_absolute(value) or not _platform.is_canonical(value):
        raise StoreError("Paket yolu mutlak ve sadeleştirilmiş olmalı.")
    # Explorer may show and keep the extension in capitals.
    if not value.lower().endswith(".st"):
        raise StoreError("Aktarım paketi .st uzantılı olmalı.")
    try:
        info = os.lstat(value)
    except FileNotFoundError:
        raise StoreError(f"Paket bulunamadı: {value}") from None
    except OSError as exc:
        raise StoreError(f"Paket incelenemedi: {value}") from exc
    if _platform.stat_is_link(info) or not stat.S_ISREG(info.st_mode):
        raise StoreError("Paket normal bir dosya olmalı; bağlantılar kabul edilmez.")
    if info.st_size == 0:
        raise StoreError("Paket dosyası boş.")
    # os.access ignores Windows ACLs: open the file to prove it can be read.
    try:
        with _platform.open_read(value, nofollow=True):
            pass
    except OSError:
        raise StoreError("Paket dosyası okunamıyor.") from None
    return Path(value)


def _slug(name):
    text = unicodedata.normalize("NFKD", str(name).translate(_TURKISH))
    text = "".join(char for char in text if char.isalnum() or char in " -_")
    text = re.sub(r"[\s_]+", "-", text.strip())
    return re.sub(r"-{2,}", "-", text).strip("-")[:40] or "istasyon"


def _safe_component(text, fallback):
    """A valid Windows file or folder name (at most 150 characters) for package text:
    control characters dropped, ``<>:"/\\|?*`` replaced by ``_``, surrounding dots and
    spaces stripped, reserved device names (``CON``, ``NUL.mp3``) prefixed."""
    if not _platform.component_valid(fallback):          # e.g. built from a container name
        fallback = _platform.sanitize_component(fallback, "_")
    return _platform.sanitize_component(text, fallback)


def _has_name(text):
    """Something of ``text`` survives _safe_component (not only controls, dots, spaces)."""
    return any(ord(char) >= 0x20 and ord(char) != 0x7F and char not in " ." and
               not 0xD800 <= ord(char) <= 0xDFFF for char in str(text))


def _redact(value, location=""):
    """Blank named secrets in a configuration; return (clean copy, blanked paths)."""
    fields = []
    if isinstance(value, dict):
        clean = {}
        for key, child in value.items():
            path = f"{location}.{key}" if location else str(key)
            if isinstance(key, str) and _SENSITIVE.search(key) and not isinstance(child, (dict, list)):
                clean[key] = "" if isinstance(child, str) else None
                fields.append(path)
            else:
                clean[key], nested = _redact(child, path)
                fields.extend(nested)
        return clean, fields
    if isinstance(value, list):
        clean = []
        for index, child in enumerate(value):
            item, nested = _redact(child, f"{location}[{index}]")
            clean.append(item)
            fields.extend(nested)
        return clean, fields
    return deepcopy(value), fields


def _local_settings(value):
    """Only application-selected INI bytes cross HTTP; never accept a filesystem target."""
    if value is None:
        return {}
    if not isinstance(value, dict) or set(value) - LOCAL_FILES:
        raise StoreError("Yerel ayar dosyası kapsamı geçersiz.")
    result = {}
    for name, contents in value.items():
        if contents is None:
            result[name] = None
            continue
        if not isinstance(contents, str) or "\0" in contents or len(contents.encode("utf-8")) > 65536:
            raise StoreError("Yerel ayar dosyası geçersiz veya çok büyük.")
        parser = configparser.ConfigParser(interpolation=None, strict=True)
        parser.optionxform = str
        try:
            parser.read_string(contents)
        except configparser.Error as exc:
            raise StoreError(f"Yerel ayar INI biçimi geçersiz: {name}") from exc
        result[name] = contents
    return result


def _media_rows(store, sid):
    """Active library songs with verified bytes, resolved like the desktop list."""
    rows = []
    for record in store.lookup("desktop_media_library", "station", sid):
        if record["status"] != "active":
            continue
        item = store.get("items", record["item_id"])
        version = store.get("versions", record["version_id"])
        media = store.get("media_files", record["media_file_id"])
        if not item or not version or not media or media.get("status") != "verified" or \
                not isinstance(media.get("sha256"), str) or not _SHA.fullmatch(media["sha256"]):
            continue
        rows.append({"library": record, "item": item, "version": version, "media": media})
    rows.sort(key=lambda row: (row["item"]["title"].casefold(), row["library"]["_id"]))
    return rows


def _station_campaigns(store, sid):
    """Open campaigns of the station kept by the desktop (sales fields present)."""
    return [row for row in store.scan("campaigns") if sid in (row.get("station_ids") or [])
            and row.get("status") in _CAMPAIGN_OPEN and isinstance(row.get("desktop_fields"), dict)]


def _group_counts(store, sid):
    counts = {kind: 0 for kind in GROUPS}
    counts[CAMPAIGNS] = len(_station_campaigns(store, sid))
    for row in store.lookup("desktop_configurations", "station", sid):
        if row["status"] == "active" and row["kind"] in counts and not _bridged(row):
            counts[row["kind"]] += 1
    counts[PLAYLISTS] = sum(1 for row in store.lookup("playlists", "station", sid) if row["status"] == "active")
    for kind in PROFILE_KINDS:
        counts[kind] = sum(1 for row in store.scan(kind) if row["station_id"] == sid and not row.get("retired_at"))
    return counts


def _bridged(row):
    return row["kind"] == "advert_break" and isinstance(row.get("config"), dict) and "bridge" in row["config"]


def _document_entry(row):
    name = row.get("name")
    return {"id": row["_id"], "name": name if isinstance(name, str) else ""}


def _group_documents(store, sid):
    """The documents an export of each group carries, by name, for a per-document choice."""
    documents = {kind: [] for kind in GROUPS}
    for row in store.lookup("desktop_configurations", "station", sid):
        if row["status"] == "active" and row["kind"] in documents and not _bridged(row):
            documents[row["kind"]].append(_document_entry(row))
    documents[PLAYLISTS] = [_document_entry(row) for row in store.lookup("playlists", "station", sid)
                            if row["status"] == "active"]
    documents[CAMPAIGNS] = [_document_entry(row) for row in _station_campaigns(store, sid)]
    for kind in PROFILE_KINDS:
        documents[kind] = [_document_entry(row) for row in store.scan(kind)
                           if row["station_id"] == sid and not row.get("retired_at")]
    # Workstation INI files travel as one unit; they are never chosen one by one.
    documents[LOCAL_SETTINGS] = []
    for rows in documents.values():
        rows.sort(key=lambda row: (row["name"].casefold(), row["id"]))
    return documents


def groups(store, principal, station_id):
    sid, _ = api._station(store, principal, station_id, "manage")
    with store._locked(exclusive=False):
        store._reload_locked()
        counts = _group_counts(store, sid)
        documents = _group_documents(store, sid)
        songs = _media_rows(store, sid)
    return {"groups": [{"id": kind, "label": LABELS[kind], "count": counts[kind],
                        "documents": documents[kind][:MAX_GROUP_DOCUMENTS]} for kind in GROUPS],
            "media": {"count": len(songs), "bytes": sum(int(row["media"].get("size_bytes") or 0) for row in songs)}}


# --- export -----------------------------------------------------------------

def _selection(settings, kinds):
    if settings not in {"all", "selected", "none"}:
        raise StoreError("settings değeri all, selected veya none olmalı.")
    if settings == "all":
        return list(GROUPS)
    if settings == "none":
        return []
    chosen = api._list(kinds, "kinds", len(GROUPS))
    if not chosen or any(not isinstance(kind, str) or kind not in GROUPS for kind in chosen) or len(set(chosen)) != len(chosen):
        raise StoreError("Seçili ayar grupları bilinen ve tekrarsız olmalı.")
    return [kind for kind in GROUPS if kind in chosen]


def _document_selection(settings, selected, documents):
    """Optional ``{kind: [id, ...]}``: only those documents of a selected group are exported.

    A selected group without an entry is exported whole; the workstation INI
    group is never split.
    """
    if documents is None:
        return {}
    if settings != "selected":
        raise StoreError("Belge seçimi yalnız seçili radyo ayarlarıyla birlikte gönderilebilir.")
    if not isinstance(documents, dict):
        raise StoreError("Belge seçimi, ayar grubundan belge kimliklerine giden bir nesne olmalı.")
    chosen = {}
    for kind, identities in documents.items():
        if kind == LOCAL_SETTINGS:
            raise StoreError("Yerel ayarlar grubunda belge seçimi yapılamaz.")
        if kind not in selected:
            raise StoreError(f"Belge seçimi yalnız seçili ayar grupları için yapılabilir: {str(kind)[:80]}")
        label = LABELS[kind]
        if not isinstance(identities, list) or not 1 <= len(identities) <= MAX_GROUP_DOCUMENTS:
            raise StoreError(f"{label}: seçili belgeler 1 ile {MAX_GROUP_DOCUMENTS} arasında kimlik içeren bir liste olmalı.")
        keys = []
        for identity in identities:
            try:
                keys.append(api._id(identity, "document id"))
            except StoreError:
                raise StoreError(f"{label}: seçili belge kimliği geçersiz.") from None
        if len(set(keys)) != len(keys):
            raise StoreError(f"{label}: seçili belge kimlikleri tekrarsız olmalı.")
        chosen[kind] = keys
    return chosen


def _missing_documents(kind, wanted, found, warnings):
    for identity in wanted:
        if identity not in found:
            warnings.append(f"{LABELS[kind]}: seçili kayıt bulunamadı veya etkin değil, paket dışı kaldı ({identity})")


def export_start(store, principal, station_id, destination_dir, settings="all", kinds=None, media=True, file_name=None,
                 local_settings=None, documents=None):
    sid, station = api._station(store, principal, station_id, "manage")
    selected = _selection(settings, kinds if kinds is not None else [])
    chosen = _document_selection(settings, selected, documents)
    api._bool(media, "media")
    if not selected and not media:
        raise StoreError("Dışa aktarılacak bir şey seçilmedi.")
    destination = _directory(destination_dir, "Hedef")
    if file_name is None:
        file_name = f"RadioS-{_slug(station['name'])}-{datetime.now().strftime('%Y%m%d-%H%M')}.st"
    api._text(file_name, "file_name", 200, True)
    if not _FILE_NAME.fullmatch(file_name) or file_name.startswith(".") or not _platform.component_valid(file_name):
        raise StoreError("Paket adı .st ile bitmeli; klasör ayracı, : * ? \" < > | karakterleri ve "
                         "Windows'a ayrılmış adlar (CON, NUL, COM1 …) içermemeli.")
    target = destination / file_name
    if not _fits(target, _MAX_FILE_PATH):
        raise StoreError(f"Paket yolu Windows için çok uzun ({_MAX_FILE_PATH} karakterden fazla); "
                         "daha kısa bir klasör veya ad seçin.")
    if os.path.lexists(target):
        raise StoreError(f"Aynı adda bir paket zaten var: {target}")
    media_bytes = 0
    if media:
        with store._locked(exclusive=False):
            store._reload_locked()
            media_bytes = sum(int(row["media"].get("size_bytes") or 0) for row in _media_rows(store, sid))
    desktop_reset.require_space([(destination, media_bytes + _SETTINGS_ALLOWANCE)], "Dışa aktarma")
    job = _Job("export", sid, str(target))
    plan = {"settings": settings, "kinds": selected, "media": media, "documents": chosen,
            "local_settings": _local_settings(local_settings) if LOCAL_SETTINGS in selected else {}}
    _start(job, lambda current: _run_export(current, store, principal, sid, station, target, plan))
    return {"job_id": job.job_id, "state": "running", "package_path": str(target)}


def _export_documents(store, sid, kinds, warnings, chosen=None):
    documents, redacted = {}, []
    chosen = chosen or {}
    for kind in kinds:
        if kind in {PLAYLISTS, LOCAL_SETTINGS, CAMPAIGNS} | PROFILE_KINDS:
            continue
        wanted = set(chosen[kind]) if kind in chosen else None
        rows, found = [], set()
        for row in store.lookup("desktop_configurations", "station_kind", sid, kind):
            if row["status"] != "active" or (wanted is not None and row["_id"] not in wanted):
                continue
            found.add(row["_id"])
            if _bridged(row):
                warnings.append(f"Reklam köprüsüne bağlı kuşak paket dışı bırakıldı: {row['name']}")
                continue
            # A local station transfer must round-trip all configured values,
            # including encoder credentials; these never enter progress text.
            config = deepcopy(row["config"])
            rows.append({"id": row["_id"], "name": row["name"], "revision": row["_rev"], "config": config})
        rows.sort(key=lambda row: (row["name"].casefold(), row["id"]))
        if wanted is not None:
            _missing_documents(kind, chosen[kind], found, warnings)
        if rows:
            documents[kind] = rows
    return documents, redacted


def _export_profiles(store, sid, kinds, chosen=None, warnings=None):
    result = {}
    chosen = chosen or {}
    for kind in PROFILE_KINDS.intersection(kinds):
        wanted = set(chosen[kind]) if kind in chosen else None
        rows = [{"id": row["_id"], "name": row["name"], "config": {
            key: value for key, value in row.items() if key not in {"_id", "_rev", "station_id", "owner_user_id"}}}
            for row in store.scan(kind) if row["station_id"] == sid and not row.get("retired_at")
            and (wanted is None or row["_id"] in wanted)]
        if wanted is not None and warnings is not None:
            _missing_documents(kind, chosen[kind], {row["id"] for row in rows}, warnings)
        for document in rows:
            config = document["config"]
            references = [config.get("filters", {}).get("category_id")] if kind == "view_profiles" else \
                list(config.get("config", {}).get("category_mapping", {}).values())
            document["categories"] = {}
            for identifier in references:
                category = store.get("categories", identifier) if isinstance(identifier, str) else None
                if category and category["station_id"] == sid:
                    document["categories"][identifier] = {key: value for key, value in category.items()
                                                          if key not in {"_id", "_rev", "station_id"}}
        if rows:
            result[kind] = rows
    return result


def _export_campaigns(store, sid, chosen=None, warnings=None):
    wanted = set(chosen) if chosen is not None else None
    rows = []
    for row in _station_campaigns(store, sid):
        if wanted is not None and row["_id"] not in wanted:
            continue
        binding = row.get("desktop_binding") if isinstance(row.get("desktop_binding"), dict) else {}
        digest = binding.get("media_sha256")
        rows.append({"id": row["_id"], "name": row["name"], "revision": row["_rev"],
                     "config": {"fields": deepcopy(row["desktop_fields"]), "status": row["status"],
                                "sha256": digest if isinstance(digest, str) and _SHA.fullmatch(digest) else None}})
    rows.sort(key=lambda row: (row["name"].casefold(), row["id"]))
    if chosen is not None and warnings is not None:
        _missing_documents(CAMPAIGNS, chosen, {row["id"] for row in rows}, warnings)
    return rows


def _export_playlists(store, sid, chosen=None, warnings=None):
    rows = []
    wanted = set(chosen) if chosen is not None else None
    for playlist in store.lookup("playlists", "station", sid):
        if playlist["status"] != "active" or (wanted is not None and playlist["_id"] not in wanted):
            continue
        version = store.get("playlist_versions", playlist["draft_version_id"]) if playlist.get("draft_version_id") else None
        entries = []
        if version and version["status"] == "draft":
            for track in desktop_playlist._tracks(store, version["_id"]):
                entry = {name: track[name] for name in desktop_playlist.METADATA}
                entry.update({name: track[name] for name in desktop_playlist.REKORDBOX_SOURCE if name in track})
                entry["entry_id"] = track["_id"]
                entry.update({key: track[key] for key in api.MEDIA_BINDING_KEYS if key in track})
                digest = track.get("media_sha256")
                entry["sha256"] = digest if isinstance(digest, str) and _SHA.fullmatch(digest) else None
                entries.append(entry)
        rows.append({"id": playlist["_id"], "name": playlist["name"], "revision": playlist["_rev"],
                     "config": {"purpose": playlist.get("purpose", "music"), "code": playlist["code"],
                                "version_id": version["_id"] if version else None, "entries": entries}})
    rows.sort(key=lambda row: (row["name"].casefold(), row["id"]))
    if chosen is not None and warnings is not None:
        _missing_documents(PLAYLISTS, chosen, {row["id"] for row in rows}, warnings)
    return rows


def _song_name(row):
    media, library, item = row["media"], row["library"], row["item"]
    source = media.get("source_name")
    if isinstance(source, str) and _has_name(source) and Path(source).name == source:
        return _safe_component(source, "parca")
    container = (media.get("technical") or {}).get("container") or "bin"
    base = f"{library['artist']} - {item['title']}" if library.get("artist") else item["title"]
    return _safe_component(f"{base}.{container}", f"parca.{container}")


def _assign_paths(rows):
    """Preserve each source tree; shared object bytes never collapse library identities."""
    taken = {}
    for row in rows:
        digest = row["media"]["sha256"]
        album = _safe_component(row["library"].get("album", ""), "Albümsüz")
        name = _song_name(row)
        relative = row["library"].get("source_relative_path") or row.get("source_relative_path")
        directory = ""
        if relative:
            relative = _relative_path(relative)
            directory, _, name = relative.rpartition("/")
        if not directory:
            # A single imported file carries no folder of its own (its root is
            # its own directory): keep it in its album folder.
            directory = album
        stem, suffix = (name.rsplit(".", 1) + [""])[:2] if "." in name else (name, "")
        prefix = "medya/" + (directory + "/" if directory else "")
        candidate, counter = prefix + name, 2
        while candidate in taken and taken[candidate] != digest:
            candidate = prefix + f"{stem} ({counter})" + (f".{suffix}" if suffix else "")
            counter += 1
        taken[candidate] = digest
        row["path"] = candidate
    return rows


def _relative_path(value):
    if not isinstance(value, str) or not value or len(value) > 4096 or value.startswith("/") or \
            "\\" in value or any(ord(char) < 32 or ord(char) == 127 for char in value) or \
            any(part in {"", ".", ".."} for part in value.split("/")):
        raise StoreError("Medya göreli yolu geçersiz.")
    return value


def _export_media(store, sid):
    rows = _media_rows(store, sid)
    # Older imports kept provenance in playlist rows rather than the library.
    # Locations are Windows paths (imports on this host) or legacy POSIX paths
    # (databases and packages made on Linux); each kind gets its own common folder.
    locations = {}
    for track in store.scan(desktop_playlist.COLLECTION):
        path = track.get("location")
        if track.get("station_id") == sid and track.get("status") == "active" and \
                isinstance(path, str) and "\0" not in path and _platform.stored_path_flavour(path):
            locations.setdefault(track.get("media_id"), path)
    grouped = {}
    for row in rows:
        path = locations.get(row["library"]["_id"])
        if path:
            grouped.setdefault(_platform.stored_path_flavour(path), []).append(path)
    commons = {}
    for flavour, paths in grouped.items():
        module = ntpath if flavour == "windows" else posixpath
        try:
            common = module.commonpath([module.dirname(path) for path in paths])
        except ValueError:          # different drives or shares: no shared folder
            continue
        if common and not (_platform.is_root(common) if flavour == "windows" else common == "/"):
            commons[flavour] = common
    for row in rows:
        path = locations.get(row["library"]["_id"])
        if not path or row["library"].get("source_relative_path"):
            continue
        flavour = _platform.stored_path_flavour(path)
        if flavour not in commons:
            continue
        try:
            if flavour == "windows":
                relative = ntpath.relpath(path, commons[flavour]).replace("\\", "/")
            else:
                relative = posixpath.relpath(path, commons[flavour])
            row["source_relative_path"] = _relative_path(relative)
        except (ValueError, StoreError):
            continue                # an unusable old location keeps the album layout
    rows = _assign_paths(rows)

    def strip(records):
        return {key: value for key, value in records[0].items() if key not in _ANALYSIS_STRIP} if records else None

    for row in rows:
        media = row["media"]
        row["records"] = {"analysis": strip(store.lookup("desktop_media_analysis", "media", media["_id"])),
                          "markers": strip(store.lookup("desktop_media_markers", "station_media", sid, media["_id"])),
                          "rhythm": strip(store.lookup("desktop_media_rhythm", "station_media", sid, media["_id"]))}
    return rows


def _song_entry(row, analysis_path):
    library, item, media = row["library"], row["item"], row["media"]
    technical = media.get("technical") or {}
    return {"sha256": media["sha256"], "size_bytes": media.get("size_bytes"), "path": row["path"],
            "file_name": row["path"].rsplit("/", 1)[-1], "media_file_id": media["_id"],
            "version_id": library["version_id"], "item_id": library["item_id"],
            "library_id": library["_id"], "title": item["title"], "kind": item["kind"],
            "artist": library.get("artist", ""), "album": library.get("album", ""),
            "genre": library.get("genre", ""), "year": library.get("year", ""),
            "tags": list(library.get("tags", [])), "comment": library.get("comment", ""),
            "duration_ms": technical.get("duration_ms"), "technical": deepcopy(technical),
            "library": {"revision": library["_rev"], "created_at": library.get("created_at"),
                        "updated_at": library.get("updated_at")},
            "analysis": analysis_path,
            "source_relative_path": row["path"][len("medya/"):]}


def _info(name, stamp, compress):
    info = zipfile.ZipInfo(name, date_time=stamp)
    info.compress_type = compress
    # Unix "made by" with mode 0644: packages made on Windows and on Linux carry the
    # same entry metadata (readers ignore it).
    info.create_system = 3
    info.external_attr = 0o644 << 16
    return info


def _stream_song(zf, info, source, digest, job):
    """Copy one managed object into the package while hashing it."""
    sha = hashlib.sha256()
    size = 0
    # Share-delete reader: object repair or retention may replace/delete the file meanwhile.
    with _platform.open_read(source, nofollow=True) as stream, zf.open(info, "w", force_zip64=True) as output:
        for chunk in iter(lambda: stream.read(_CHUNK), b""):
            job.check()
            sha.update(chunk)
            output.write(chunk)
            size += len(chunk)
    return sha.hexdigest(), size


def _run_export(job, store, principal, sid, station, target, plan):
    warnings = []
    with store._locked(exclusive=False):
        store._reload_locked()
        chosen = plan.get("documents") or {}
        documents, redacted = _export_documents(store, sid, plan["kinds"], warnings, chosen)
        documents.update(_export_profiles(store, sid, set(plan["kinds"]), chosen, warnings))
        if CAMPAIGNS in plan["kinds"]:
            campaigns = _export_campaigns(store, sid, chosen.get(CAMPAIGNS), warnings)
            if campaigns:
                documents[CAMPAIGNS] = campaigns
        playlists = _export_playlists(store, sid, chosen.get(PLAYLISTS), warnings) if PLAYLISTS in plan["kinds"] else None
        songs = _export_media(store, sid) if plan["media"] else []
        schema_version = store.schema_version
    job.check()
    created = datetime.now(timezone.utc)
    stamp = created.timetuple()[:6]
    temp = target.with_name(f".{target.name}.{uuid4().hex}.partial")
    total = len(songs) + 1
    done = 0
    written, media_bytes = {}, 0
    try:
        # The package may carry stream passwords: it is created with the private
        # owner-only ACL.  FAT/exFAT sticks keep no ACLs (Linux ignored mode 0600
        # there too): the package is written anyway and the operator is told.
        private = _platform.acl_supported(target.parent)
        if not private:
            warnings.append(_NO_ACL_WARNING)
        descriptor = _platform.open_fd(temp, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600, private=private)
        try:
            raw = os.fdopen(descriptor, "w+b")
        except BaseException:
            os.close(descriptor)
            raise
        # One handle from creation to close: the private ACL and the bytes stay ours.
        with raw, zipfile.ZipFile(raw, "w", allowZip64=True) as zf:
            job.update(0, "Ayarlar yazılıyor…")
            settings = []
            for kind, rows in documents.items():
                zf.writestr(_info(f"ayarlar/{kind}.json", stamp, zipfile.ZIP_DEFLATED),
                            _json({"kind": kind, "documents": rows}))
                settings.append({"kind": kind, "label": LABELS[kind], "count": len(rows)})
            if playlists:
                zf.writestr(_info(f"ayarlar/{PLAYLISTS}.json", stamp, zipfile.ZIP_DEFLATED),
                            _json({"kind": PLAYLISTS, "documents": playlists}))
                settings.append({"kind": PLAYLISTS, "label": LABELS[PLAYLISTS], "count": len(playlists)})
            if plan.get("local_settings"):
                local = {"id": str(uuid5(NAMESPACE_URL, "radios-local-settings")),
                         "name": LABELS[LOCAL_SETTINGS], "config": {"files": plan["local_settings"]}}
                zf.writestr(_info(f"ayarlar/{LOCAL_SETTINGS}.json", stamp, zipfile.ZIP_DEFLATED),
                            _json({"kind": LOCAL_SETTINGS, "documents": [local]}))
                settings.append({"kind": LOCAL_SETTINGS, "label": LABELS[LOCAL_SETTINGS], "count": len(plan["local_settings"])})
            done += 1
            table, analysis_written = [], set()
            for index, row in enumerate(songs, start=1):
                job.check()
                job.update(done * 100 // total, f"Medya kopyalanıyor ({index}/{len(songs)})…")
                media = row["media"]
                digest = media["sha256"]
                media_key = (digest, row["path"])
                if media_key not in written:
                    source = media_service.object_path(store, digest)
                    if _platform.is_link(source) or not source.is_file():
                        warnings.append(f"Medya dosyası eksik, paket dışı bırakıldı: {row['item']['title']}")
                        done += 1
                        continue
                    actual, size = _stream_song(zf, _info(row["path"], stamp, zipfile.ZIP_STORED), source, digest, job)
                    if actual != digest or size != media.get("size_bytes"):
                        warnings.append(f"Medya dosyası bozuk, paket dışı bırakıldı: {row['item']['title']}")
                        written[media_key] = None
                        done += 1
                        continue
                    written[media_key] = row["path"]
                    media_bytes += size
                elif written[media_key] is None:
                    done += 1
                    continue
                analysis_path = None
                if row["records"]["analysis"] or row["records"]["markers"] or row["records"]["rhythm"]:
                    analysis_path = f"bilgiler/analiz/{digest}.json"
                    if analysis_path in analysis_written:
                        analysis_path = f"bilgiler/analiz/{digest}-{row['library']['_id']}.json"
                    if analysis_path not in analysis_written:
                        analysis_written.add(analysis_path)
                        zf.writestr(_info(analysis_path, stamp, zipfile.ZIP_DEFLATED), _json(row["records"]))
                table.append(_song_entry(row, analysis_path))
                done += 1
            zf.writestr(_info("bilgiler/medya.json", stamp, zipfile.ZIP_DEFLATED),
                        _json({"version": MEDIA_VERSION, "songs": table}))
            manifest = {"format": FORMAT, "version": VERSION, "created_at": created.isoformat(),
                        "application_version": __version__, "desktop_api_version": DESKTOP_API_VERSION,
                        "schema_version": schema_version,
                        "station": {"id": sid, "name": station["name"]},
                        "scope": {"settings": plan["settings"], "kinds": plan["kinds"], "media": plan["media"],
                                  # Only partially chosen groups are listed; others travel whole.
                                  **({"documents": chosen} if chosen else {})},
                        "settings": settings, "media": {"count": len(table), "bytes": media_bytes},
                        "redacted": redacted}
            zf.writestr(_info("bilgiler/manifest.json", stamp, zipfile.ZIP_DEFLATED), _json(manifest))
        # Durable bytes before the name appears (NTFS journals the rename itself).
        _platform.fsync_file(temp)
        job.check()
        try:
            # Never overwrite a file that appeared under the name since export_start.
            _platform.rename_noreplace(temp, target)
        except FileExistsError:
            raise StoreError(f"Aynı adda bir paket zaten var: {target}") from None
    finally:
        _platform.unlink_quietly(temp)
    job.update(100, "Paket yazıldı.")
    return {"package_path": str(target), "bytes": target.stat().st_size, "settings": settings,
            "media": {"count": len(table), "bytes": media_bytes}, "redacted": redacted,
            "warnings": warnings}


# --- package reading --------------------------------------------------------

def _read_json(zf, name, what):
    try:
        return json.loads(zf.read(name).decode("utf-8"))
    except KeyError:
        raise StoreError(f"Pakette {what} yok ({name}).") from None
    except (ValueError, UnicodeDecodeError, zipfile.BadZipFile) as exc:
        raise StoreError(f"Paketteki {what} okunamadı ({name}).") from exc


class _PackageReader(zipfile.ZipFile):
    """ZipFile over a share-delete handle (the operator may move or delete the package
    in Explorer while an import reads it); closing the archive closes the handle."""

    def __init__(self, stream):
        self._stream = stream
        super().__init__(stream)

    def close(self):
        try:
            super().close()
        finally:
            self._stream.close()


def _open_package(path, *, full_check=True):
    """Validate the container and manifest; return (zipfile, manifest, songs, names)."""
    try:
        stream = _platform.open_read(path, nofollow=True)
    except OSError as exc:
        raise StoreError("Paket açılamadı; dosya okunamıyor.") from exc
    try:
        if not zipfile.is_zipfile(stream):
            raise StoreError("Dosya bir RadioS aktarım paketi değil.")
        stream.seek(0)
        zf = _PackageReader(stream)
    except (zipfile.BadZipFile, OSError) as exc:
        stream.close()
        raise StoreError("Paket açılamadı; dosya bozuk olabilir.") from exc
    except BaseException:
        stream.close()
        raise
    try:
        names = zf.namelist()
        for name in names:
            if name.startswith("/") or "\\" in name or ".." in name.split("/") or "\0" in name or \
                    name.split("/", 1)[0] not in {"ayarlar", "medya", "bilgiler"}:
                raise StoreError(f"Pakette beklenmeyen giriş var: {name}")
        if full_check:
            bad = zf.testzip()
            if bad is not None:
                raise StoreError(f"Paket bütünlük denetiminden geçemedi: {bad}")
        manifest = _read_json(zf, "bilgiler/manifest.json", "manifest")
        if not isinstance(manifest, dict) or manifest.get("format") != FORMAT:
            raise StoreError("Manifest biçimi RadioS aktarım paketi değil.")
        if manifest.get("version") != VERSION:
            raise StoreError(f"Paket sürümü ({manifest.get('version')}) bu RadioS tarafından desteklenmiyor.")
        for key in ("station", "scope", "settings", "media", "redacted"):
            if key not in manifest:
                raise StoreError(f"Manifestte {key} alanı eksik.")
        if not isinstance(manifest["station"], dict) or not isinstance(manifest["scope"], dict) or \
                not isinstance(manifest["settings"], list) or not isinstance(manifest["redacted"], list):
            raise StoreError("Manifest alanları geçersiz.")
        songs = []
        if "bilgiler/medya.json" in names:
            table = _read_json(zf, "bilgiler/medya.json", "medya tablosu")
            if not isinstance(table, dict) or table.get("version") != MEDIA_VERSION or not isinstance(table.get("songs"), list):
                raise StoreError("Paketteki medya tablosu geçersiz.")
            songs = table["songs"]
            for song in songs:
                if not isinstance(song, dict) or not isinstance(song.get("sha256"), str) or not _SHA.fullmatch(song["sha256"]) \
                        or not isinstance(song.get("path"), str) or not song["path"].startswith("medya/") \
                        or type(song.get("size_bytes")) is not int or song["size_bytes"] < 1:
                    raise StoreError("Paketteki medya tablosunda geçersiz satır var.")
    except BaseException:
        zf.close()
        raise
    return zf, manifest, songs, names


def _package_documents(zf, names, warnings):
    documents = {}
    for name in names:
        if not name.startswith("ayarlar/") or not name.endswith(".json") or name.count("/") != 1:
            continue
        kind = name[len("ayarlar/"):-len(".json")]
        payload = _read_json(zf, name, "ayar dosyası")
        if not isinstance(payload, dict) or payload.get("kind") != kind or not isinstance(payload.get("documents"), list):
            warnings.append(f"Ayar dosyası geçersiz, atlandı: {name}")
            continue
        if kind not in EXPORT_KINDS and kind not in {PLAYLISTS, LOCAL_SETTINGS, CAMPAIGNS} | PROFILE_KINDS:
            warnings.append(f"Bilinmeyen ayar grubu atlandı: {kind}")
            continue
        rows = [row for row in payload["documents"] if isinstance(row, dict) and isinstance(row.get("id"), str)
                and isinstance(row.get("name"), str) and isinstance(row.get("config"), dict)]
        if len(rows) != len(payload["documents"]):
            warnings.append(f"{LABELS[kind]}: biçimi bozuk {len(payload['documents']) - len(rows)} belge atlandı")
        documents[kind] = rows
        if kind == LOCAL_SETTINGS:
            for row in rows:
                row["config"] = {"files": _local_settings(row["config"].get("files"))}
    return documents


def inspect(store, principal, station_id, package_path):
    api._station(store, principal, station_id, "manage")
    path = _package(package_path)
    warnings = []
    full = path.stat().st_size <= _FULL_CHECK_BYTES
    if not full:
        warnings.append("Büyük paket: medya bütünlüğü içe aktarma sırasında doğrulanacak.")
    zf, manifest, songs, names = _open_package(path, full_check=full)
    with zf:
        documents = _package_documents(zf, names, warnings)
        for song in songs:
            if song["path"] not in names:
                warnings.append(f"Medya tablosundaki dosya pakette yok: {song['path']}")
    settings = [{"kind": kind, "label": LABELS[kind], "count": len(documents[kind])}
                for kind in GROUPS if kind in documents]
    if manifest.get("schema_version") not in (None, store.schema_version):
        warnings.append(f"Paket şema sürümü {manifest.get('schema_version')}, bu veritabanı {store.schema_version}; "
                        "doğrulamadan geçmeyen belgeler atlanır.")
    if manifest["redacted"]:
        warnings.append(f"{len(manifest['redacted'])} belgede gizli alanlar boşaltılmış; içe aktarma sonrası yeniden girin.")
    unique = {(song["sha256"], song["path"]): song for song in songs}
    return {"manifest": manifest, "settings": settings,
            "media": {"count": len(songs), "bytes": sum(song["size_bytes"] for song in unique.values())},
            "warnings": warnings, "package_path": str(path)}


# --- import -----------------------------------------------------------------

def import_start(store, principal, station_id, package_path, settings=True, kinds=None, media=False, media_root=None):
    sid, station = api._station(store, principal, station_id, "manage")
    api._bool(settings, "settings")
    api._bool(media, "media")
    if not settings and not media:
        raise StoreError("İçe aktarılacak bir şey seçilmedi.")
    path = _package(package_path)
    selected = None
    if kinds is not None:
        chosen = api._list(kinds, "kinds", len(GROUPS))
        if any(not isinstance(kind, str) or kind not in GROUPS for kind in chosen) or len(set(chosen)) != len(chosen):
            raise StoreError("Seçili ayar grupları bilinen ve tekrarsız olmalı.")
        selected = set(chosen)
    root = _directory(media_root, "Medya") if media else None
    zf, _manifest, songs, _names = _open_package(path, full_check=False)
    zf.close()
    if media:
        unique = {song["sha256"]: int(song["size_bytes"]) for song in songs}
        placed = sum(unique.values())
        staged = sum(size for digest, size in unique.items()
                     if not media_service.object_path(store, digest).is_file())
        desktop_reset.require_space([(root, placed), (Path(store.root), staged)], "İçe aktarma")
    job = _Job("import", sid, str(path))
    options = {"settings": settings, "kinds": selected, "media": media, "media_root": root}
    _start(job, lambda current: _run_import(current, store, principal, sid, station, path, options))
    return {"job_id": job.job_id, "state": "running", "package_path": str(path)}


def _place_song(zf, song, root, job, warnings):
    """Write ``root/<album>/<file>``; equal content is kept, a clash gets ' (2)'.

    Package folders and names come from any system (a Linux package may carry
    ``Live: 2020/CON.mp3``): every component is made a valid Windows name before it
    touches the disk, so nothing lands in an alternate data stream or a device."""
    parts = [_platform.sanitize_component(part, "_", limit=255)
             for part in _relative_path(song["path"][len("medya/"):]).split("/")]
    if len(parts) > 256:
        raise StoreError(f"Paketteki medya yolu geçersiz: {song['path']}")
    directory = root
    for part in parts[:-1]:
        directory = directory / part
        if not _fits(directory, _MAX_DIRECTORY_PATH):
            raise StoreError(f"Medya klasör yolu Windows için çok uzun: {directory}")
        directory.mkdir(exist_ok=True)
        # Checked after mkdir: an existing link (symlink or junction) is never entered.
        if _platform.is_link(directory):
            raise StoreError(f"Medya klasöründe sembolik bağlantı veya bağlantı noktası var: {directory}")
    name = parts[-1]
    stem, suffix = (name.rsplit(".", 1) + [""])[:2] if "." in name else (name, "")

    def candidates():
        yield directory / name
        counter = 2
        while True:
            yield directory / (f"{stem} ({counter})" + (f".{suffix}" if suffix else ""))
            counter += 1

    def same_content(path):
        if _platform.is_link(path) or not path.is_file():
            return False
        actual, size = media_service._digest(path)
        return actual == song["sha256"] and size == song["size_bytes"]

    names = candidates()
    candidate = next(names)
    while os.path.lexists(candidate):
        if same_content(candidate):
            return candidate, False
        candidate = next(names)
    if not _fits(candidate, _MAX_FILE_PATH):
        raise StoreError(f"Medya dosya yolu Windows için çok uzun: {candidate}")
    temp = directory / f".{name[:64]}.{uuid4().hex}.partial"
    sha = hashlib.sha256()
    size = 0
    try:
        descriptor = _platform.open_fd(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL)
        try:
            output = os.fdopen(descriptor, "wb")
        except BaseException:
            os.close(descriptor)
            raise
        with output, zf.open(song["path"]) as source:
            for chunk in iter(lambda: source.read(_CHUNK), b""):
                job.check()
                sha.update(chunk)
                output.write(chunk)
                size += len(chunk)
            output.flush()
            _platform.fsync(output)
        if sha.hexdigest() != song["sha256"] or size != song["size_bytes"]:
            raise StoreError(f"Paketteki medya dosyasının özeti uyuşmuyor: {song['path']}")
        while True:
            try:
                # Never overwrite a file that appeared under the name meanwhile.
                _platform.rename_noreplace(temp, candidate)
                return candidate, True
            except FileExistsError:
                if same_content(candidate):
                    return candidate, False
                candidate = next(names)
                if not _fits(candidate, _MAX_FILE_PATH):
                    raise StoreError(f"Medya dosya yolu Windows için çok uzun: {candidate}") from None
    finally:
        _platform.unlink_quietly(temp)


def _existing_songs(store, sid):
    """Library rows of this station by SHA-256 (active first, then disabled)."""
    index = {}
    for record in store.lookup("desktop_media_library", "station", sid):
        media = store.get("media_files", record["media_file_id"])
        digest = media.get("sha256") if media else None
        if not isinstance(digest, str) or media.get("status") != "verified":
            continue
        current = index.get(digest)
        if current is None or (current["library"]["status"] != "active" and record["status"] == "active"):
            index[digest] = {"library": record, "media": media}
    return index


def _binding(library, media):
    return {"media_id": library["_id"], "version_id": library["version_id"],
            "media_file_id": library["media_file_id"], "media_sha256": media["sha256"]}


def _lookup_binding(mapping, value):
    for field, prefix in (("media_id", "library:"), ("library_id", "library:"),
                          ("version_id", "version:"), ("media_file_id", "file:")):
        identity = value.get(field)
        if isinstance(identity, str) and prefix + identity in mapping:
            return mapping[prefix + identity]
    return mapping.get(value.get("media_sha256") or value.get("sha256"))


def _remember_binding(mapping, song, value):
    mapping.setdefault(song["sha256"], value)
    for field, prefix in (("library_id", "library:"), ("version_id", "version:"), ("media_file_id", "file:")):
        if isinstance(song.get(field), str):
            mapping[prefix + song[field]] = value


def _free(store, collection, key):
    return store.get(collection, key) is None and key not in store._tombstones.get(collection, set())


def _records(zf, song, names):
    path = song.get("analysis")
    if not isinstance(path, str) or path not in names:
        return {}
    records = _read_json(zf, path, "analiz kaydı")
    return records if isinstance(records, dict) else {}


def _extra_rows(records, sid, ids, digest, duration, user, now, warnings, title):
    """Analysis, markers and rhythm rows rebuilt from the package records."""
    rows = {}
    analysis = records.get("analysis")
    if isinstance(analysis, dict) and isinstance(analysis.get("analysis"), dict):
        try:
            rows["analysis"] = {**ids, "analysis": desktop_media._analysis(analysis["analysis"]),
                                "analyzer": analysis.get("analyzer") or "radios-native-decoder-v1",
                                "created_by": user, "created_at": analysis.get("created_at") or now}
            if rows["analysis"]["analysis"]["sha256"] != digest:
                raise StoreError("analysis digest differs")
        except StoreError:
            rows.pop("analysis", None)
            warnings.append(f"Analiz kaydı geçersiz, atlandı: {title}")
    markers = records.get("markers")
    if isinstance(markers, dict):
        try:
            fields = {key: markers[key] for key in desktop_media._MARKER_KEYS | desktop_media._AUTODJ_MARKER_KEYS | {"autodj_source"}
                      if key in markers}
            rows["markers"] = {**ids, "media_sha256": digest, "media_duration_ms": duration, "status": "draft",
                               **desktop_media._markers(fields, duration),
                               "created_by": user, "created_at": markers.get("created_at") or now,
                               "updated_by": user, "updated_at": now}
        except (StoreError, TypeError):
            warnings.append(f"İşaret kaydı geçersiz, atlandı: {title}")
    rhythm = records.get("rhythm")
    if isinstance(rhythm, dict) and isinstance(rhythm.get("rhythm"), dict):
        try:
            grid = desktop_media._rhythm(rhythm["rhythm"], duration)
            rows["rhythm"] = {**ids, "media_sha256": digest, "rhythm": grid, "analyzer": grid["algorithm"],
                              "created_by": user, "created_at": rhythm.get("created_at") or now,
                              "updated_by": user, "updated_at": now}
            if isinstance(rhythm.get("loudness"), dict):
                rows["rhythm"]["loudness"] = desktop_media._loudness(rhythm["loudness"])
        except (StoreError, TypeError):
            warnings.append(f"Ritim kaydı geçersiz, atlandı: {title}")
    return rows


def _reuse_song(store, principal, sid, existing, records, warnings, title, song=None):
    """Restore metadata on the same identity, and SHA-bound analysis records."""
    library, media = existing["library"], existing["media"]
    previous_analysis = store.lookup("desktop_media_analysis", "media", media["_id"])
    packaged_analysis = records.get("analysis")
    if previous_analysis and isinstance(packaged_analysis, dict) and \
            packaged_analysis.get("analysis") != previous_analysis[0].get("analysis"):
        # Measured analysis is immutable. Preserve both versions rather than
        # mutating the historical measurement or discarding the package data.
        raise Conflict("paket analizi için ayrı medya kimliği gerekiyor")
    if song is not None:
        metadata = {key: song.get(key, "" if key != "tags" else []) for key in desktop_media._METADATA_KEYS}
        desktop_media.save_metadata(store, principal, sid, library["_id"], library["_rev"], str(uuid4()), metadata)
        library = store.get("desktop_media_library", library["_id"])
    now = _now()
    user = principal.user_id
    ids = {"station_id": sid, "item_id": library["item_id"], "version_id": library["version_id"],
           "media_file_id": media["_id"]}
    duration = (media.get("technical") or {}).get("duration_ms") or 0
    extra = _extra_rows(records, sid, ids, media["sha256"], duration, user, now, warnings, title)
    have_analysis = store.lookup("desktop_media_analysis", "media", media["_id"])
    have_markers = store.lookup("desktop_media_markers", "station_media", sid, media["_id"])
    have_rhythm = store.lookup("desktop_media_rhythm", "station_media", sid, media["_id"])
    location = media_service._managed_location(store, media["_id"], media["sha256"])
    for attempt in ("full", "core"):
        sequence = store.sequence
        tx = store.begin(actor=user)
        tx.expect_sequence(sequence)
        writes = 0
        restored = desktop_media._data(library) | {"status": "active"}
        if song is not None and song.get("library_id"):
            restored["transfer_source_library_id"] = api._id(song["library_id"], "source library id")
        if song is not None and song.get("placed_root"):
            relative = _relative_path(song["source_relative_path"])
            restored.update(source_root=str(song["placed_root"]), source_relative_path=relative,
                            source_path=str(Path(song["placed_root"]) / relative))
        if restored != desktop_media._data(library):
            tx.put("desktop_media_library", library["_id"], restored | {
                   "updated_by": user, "updated_at": now}, expected_revision=library["_rev"])
            writes += 1
        if location.get("status") != "verified":
            tx.put("media_locations", location["_id"], media_service._data(location) | {"status": "verified",
                   "verified_at": now, "last_seen_at": now}, expected_revision=location["_rev"])
            writes += 1
        if attempt == "full":
            for key, collection, previous in (("analysis", "desktop_media_analysis", have_analysis),
                    ("markers", "desktop_media_markers", have_markers), ("rhythm", "desktop_media_rhythm", have_rhythm)):
                if key in extra:
                    if key == "analysis" and previous:
                        continue
                    values = dict(extra[key])
                    if previous:
                        values.update(created_by=previous[0]["created_by"], created_at=previous[0]["created_at"])
                    tx.put(collection, previous[0]["_id"] if previous else str(uuid4()), values,
                           expected_revision=previous[0]["_rev"] if previous else None)
                    writes += 1
        if not writes:
            break
        try:
            tx.commit()
            break
        except (StoreError, Conflict) as exc:
            if attempt == "full" and len(extra) > 0:
                warnings.append(f"Analiz/işaret kayıtları eklenemedi ({exc}): {title}")
                continue
            raise
    return _binding(library, media)


def _create_song(store, principal, sid, song, records, warnings):
    """Register new bytes as the desktop import does, keeping package identities when free."""
    now = _now()
    user = principal.user_id
    digest = song["sha256"]
    metadata = desktop_media._metadata({"title": song.get("title"), "kind": song.get("kind"),
                                        "artist": song.get("artist", ""), "album": song.get("album", ""),
                                        "genre": song.get("genre", ""), "tags": song.get("tags", []),
                                        "year": song.get("year", ""), "comment": song.get("comment", "")})
    technical = media_service._technical_metadata(song.get("technical") or {})
    source_name = song.get("file_name") if isinstance(song.get("file_name"), str) else ""
    source_name = api._text(_safe_component(source_name, "parca"), "source_name", 240, True)
    with store._locked(exclusive=False):
        store._reload_locked()
        item_id = song.get("item_id") if isinstance(song.get("item_id"), str) and _free(store, "items", song["item_id"]) else str(uuid4())
        version_id = song.get("version_id") if isinstance(song.get("version_id"), str) and _free(store, "versions", song["version_id"]) else str(uuid4())
        library_id = song.get("library_id") if isinstance(song.get("library_id"), str) and _free(store, "desktop_media_library", song["library_id"]) else str(uuid4())
        for name, key in (("item_id", item_id), ("version_id", version_id), ("library_id", library_id)):
            api._id(key, name)
        media_id = str(uuid5(NAMESPACE_URL, f"radios-media:{version_id}:{digest}"))
        location_id = str(uuid5(NAMESPACE_URL, f"radios-location:{media_id}:{digest}"))
        if not _free(store, "media_files", media_id):
            version_id = str(uuid4())
            media_id = str(uuid5(NAMESPACE_URL, f"radios-media:{version_id}:{digest}"))
            location_id = str(uuid5(NAMESPACE_URL, f"radios-location:{media_id}:{digest}"))
    ids = {"station_id": sid, "item_id": item_id, "version_id": version_id, "media_file_id": media_id}
    extra = _extra_rows(records, sid, ids, digest, technical["duration_ms"], user, now, warnings, metadata["title"])
    for attempt in ("full", "core"):
        sequence = store.sequence
        tx = store.begin(actor=user)
        tx.expect_sequence(sequence)
        person_id = desktop_media._person(store, tx, sid, metadata["artist"], now)
        contribution_id = str(uuid4()) if person_id else None
        tx.put("items", item_id, {"kind": metadata["kind"], "title": metadata["title"], "lifecycle": "draft", "created_at": now})
        tx.put("station_items", str(uuid4()), {"station_id": sid, "item_id": item_id, "eligible": False,
               "category_id": None, "rotation_policy": None, "created_at": now})
        tx.put("versions", version_id, {"item_id": item_id, "version_no": 1, "label": "Original", "status": "draft",
               "valid_from": None, "valid_to": None, "approved_at": None, "approved_by": None, "created_at": now})
        tx.put("media_files", media_id, {"version_id": version_id, "status": "verified", "sha256": digest,
               "size_bytes": song["size_bytes"], "source_name": source_name, "received_at": now,
               "technical": technical, "technical_reviewed_at": now, "technical_reviewed_by": user, "verified_at": now})
        tx.put("media_locations", location_id, {"media_file_id": media_id, "storage_uri": media_service._managed_uri(digest),
               "root_kind": "managed", "status": "verified", "sha256": digest, "size_bytes": song["size_bytes"],
               "last_seen_at": now, "verified_at": now})
        library = {**ids, **{key: metadata[key] for key in desktop_media._METADATA_KEYS - {"title", "kind"}},
                   "artist_id": person_id, "primary_contribution_id": contribution_id, "status": "active",
                   "created_by": user, "created_at": now, "updated_by": user, "updated_at": now}
        if song.get("library_id"):
            library["transfer_source_library_id"] = api._id(song["library_id"], "source library id")
        if song.get("source_relative_path") and song.get("placed_root"):
            relative = _relative_path(song["source_relative_path"])
            library.update(source_root=str(song["placed_root"]), source_relative_path=relative,
                           source_path=str(Path(song["placed_root"]) / relative))
        tx.put("desktop_media_library", library_id, library)
        if person_id:
            tx.put("contributions", contribution_id, {"item_id": item_id, "person_id": person_id, "role": "primary_artist",
                   "ordinal": 0, "desktop_media_library_id": library_id})
        if attempt == "full":
            for collection, key in (("desktop_media_analysis", "analysis"), ("desktop_media_markers", "markers"),
                                    ("desktop_media_rhythm", "rhythm")):
                if key in extra:
                    tx.put(collection, str(uuid4()), extra[key])
        try:
            tx.commit()
            break
        except (StoreError, Conflict) as exc:
            if attempt == "full" and extra:
                warnings.append(f"Analiz/işaret kayıtları eklenemedi ({exc}): {metadata['title']}")
                continue
            raise
    return {"media_id": library_id, "version_id": version_id, "media_file_id": media_id, "media_sha256": digest}


def _import_media(job, store, principal, sid, zf, songs, names, root, warnings, progress):
    """Place every song under the media root and register it in the library."""
    resolved, placed = {}, {}
    summary = {"placed": 0, "registered": 0, "reused": 0, "skipped": 0, "bytes": 0}
    existing = _existing_songs(store, sid)
    identities = {row["_id"]: row for row in store.lookup("desktop_media_library", "station", sid)}
    source_identities = {row.get("transfer_source_library_id"): row for row in identities.values()
                         if row.get("transfer_source_library_id")}
    used = set()
    for index, original_song in enumerate(songs, start=1):
        song = dict(original_song); digest = song["sha256"]
        job.check()
        progress(index - 1, len(songs), f"Medya yerleştiriliyor ({index}/{len(songs)})…")
        title = song.get("title") or song["path"]
        if song["path"] not in names:
            warnings.append(f"Medya dosyası pakette yok, atlandı: {song['path']}")
            summary["skipped"] += 1
            continue
        try:
            target, written = _place_song(zf, song, root, job, warnings)
            _remember_binding(placed, song, target)
            song["placed_root"] = str(root)
            song["source_relative_path"] = target.relative_to(root).as_posix()
            if written:
                summary["placed"] += 1
                summary["bytes"] += song["size_bytes"]
            staged, size, _object, _reused = media_service._stage_object(store, target)
            if staged != digest or size != song["size_bytes"]:
                raise StoreError("yerleştirilen dosyanın özeti uyuşmuyor")
            records = _records(zf, song, names)
            exact = identities.get(song.get("library_id")) or source_identities.get(song.get("library_id"))
            found = None
            if exact is not None:
                media = store.get("media_files", exact["media_file_id"])
                if media and media.get("sha256") == digest:
                    found = {"library": exact, "media": media}
            if found is None:
                candidate = existing.get(digest)
                if candidate and candidate["library"]["_id"] not in used:
                    found = candidate
            if found is not None:
                try:
                    binding = _reuse_song(store, principal, sid, found, records, warnings, title, song)
                    summary["reused"] += 1
                except Conflict:
                    binding = _create_song(store, principal, sid, song, records, warnings)
                    summary["registered"] += 1
            else:
                binding = _create_song(store, principal, sid, song, records, warnings)
                summary["registered"] += 1
            used.add(binding["media_id"])
            _remember_binding(resolved, song, binding)
        except (StoreError, Conflict, OSError) as exc:
            warnings.append(f"Medya kaydedilemedi ({exc}): {title}")
            summary["skipped"] += 1
    progress(len(songs), len(songs), "Medya tamamlandı.")
    return resolved, placed, summary


def _entry_metadata(entry):
    value = {name: entry.get(name) for name in desktop_playlist.METADATA}
    clean = desktop_playlist._metadata(value)
    clean.update({name: entry[name] for name in desktop_playlist.REKORDBOX_SOURCE
                  if name in entry and isinstance(entry[name], (str, int, float, list, dict, type(None)))})
    return clean


def _import_playlist(store, principal, sid, document, resolved, placed, warnings, counters, entry_counters):
    now = _now()
    user = principal.user_id
    config = document["config"]
    name = api._text(document["name"], "name", 120, True).strip()
    purpose = config.get("purpose", "music")
    if purpose not in desktop_playlist.PLAYLIST_PURPOSES:
        purpose = "music"
    entries = []
    for entry in config.get("entries", []) if isinstance(config.get("entries"), list) else []:
        if not isinstance(entry, dict):
            continue
        try:
            metadata = _entry_metadata(entry)
        except StoreError:
            warnings.append(f"Playlist satırı geçersiz, atlandı: {name}")
            continue
        digest = entry.get("sha256")
        binding = _lookup_binding(resolved, entry) if isinstance(digest, str) else None
        if binding:
            metadata.update(binding)
            target = _lookup_binding(placed, entry)
            if target:
                metadata["location"] = str(target)[:4096]
            entry_counters["bound"] += 1
        elif digest:
            entry_counters["unbound"] += 1
        entries.append((entry.get("entry_id") if isinstance(entry.get("entry_id"), str) else None, metadata))
    if len(entries) > 500:
        warnings.append(f"Playlist 500 satırla sınırlandı: {name}")
        entries = entries[:500]
    with store._locked(exclusive=False):
        store._reload_locked()
        playlist_id = api._id(document["id"], "playlist id")
        existing = store.get("playlists", playlist_id)
        if existing is not None and existing["station_id"] != sid:
            playlist_id = str(uuid5(NAMESPACE_URL, f"radios-transfer-playlist:{sid}:{playlist_id}"))
            existing = store.get("playlists", playlist_id)
            if existing and existing["station_id"] != sid:
                existing, playlist_id = None, str(uuid4())
        elif existing is None and not _free(store, "playlists", playlist_id):
            playlist_id = str(uuid4())
        version = None
        if existing and existing.get("draft_version_id"):
            version = store.get("playlist_versions", existing["draft_version_id"])
            if not version or version["status"] != "draft" or version["playlist_id"] != playlist_id:
                version = None
        current = {row["_id"]: row for row in desktop_playlist._tracks(store, version["_id"])} if version else {}
        version_id = version["_id"] if version else (config.get("version_id")
            if isinstance(config.get("version_id"), str) and _free(store, "playlist_versions", config.get("version_id")) else str(uuid4()))
        api._id(version_id, "version id")
        version_no = 1 if not existing else max([row["version_no"] for row in store.lookup("playlist_versions", "playlist", playlist_id)] + [0]) + 1
        entry_ids, used = [], set()
        for entry_id, _metadata in entries:
            candidate = entry_id
            try:
                if candidate is None or api._id(candidate) != candidate or candidate in used or \
                        not (candidate in current or _free(store, desktop_playlist.COLLECTION, candidate)):
                    candidate = str(uuid4())
            except StoreError:
                candidate = str(uuid4())
            used.add(candidate)
            entry_ids.append(candidate)
    sequence = store.sequence
    tx = store.begin(actor=user)
    tx.expect_sequence(sequence)
    body = {"station_id": sid, "code": existing["code"] if existing else "native-" + playlist_id,
            "name": name, "purpose": purpose, "status": "active",
            "current_version_id": existing.get("current_version_id") if existing else None,
            "draft_version_id": version_id,
            "created_by": existing["created_by"] if existing else user,
            "created_at": existing["created_at"] if existing else now}
    tx.put("playlists", playlist_id, body, expected_revision=existing["_rev"] if existing else None)
    if version is None:
        tx.put("playlist_versions", version_id, {"playlist_id": playlist_id, "version_no": version_no, "status": "draft",
               "created_by": user, "created_at": now})
    kept = set()
    for ordinal, ((_source_id, metadata), entry_id) in enumerate(zip(entries, entry_ids)):
        old = current.get(entry_id)
        kept.add(entry_id)
        tx.put(desktop_playlist.COLLECTION, entry_id, metadata | {"station_id": sid, "playlist_version_id": version_id,
               "ordinal": ordinal, "active_ordinal": ordinal, "status": "active", "media_state": "unverified",
               "created_by": old["created_by"] if old else user, "created_at": old["created_at"] if old else now,
               "updated_by": user, "updated_at": now}, expected_revision=old["_rev"] if old else None)
    for entry_id, row in current.items():
        if entry_id not in kept:
            tx.put(desktop_playlist.COLLECTION, entry_id, desktop_playlist._body(row) | {"status": "removed", "active_ordinal": None,
                   "updated_at": now, "updated_by": user}, expected_revision=row["_rev"])
    tx.commit()
    counters["updated" if existing else "created"] += 1
    return playlist_id


def _resolve_bindings(value, resolve, dropped):
    """Rebind every media binding by SHA-256; unresolved bindings disappear."""
    if isinstance(value, dict):
        if api.MEDIA_BINDING_KEYS <= set(value):
            binding = resolve(value)
            if set(value) == api.MEDIA_BINDING_KEYS:
                if binding is None:
                    dropped.append(1)
                    return _DROP
                return dict(binding)
            rest = {key: child for key, child in value.items() if key not in api.MEDIA_BINDING_KEYS}
            if binding is None:
                dropped.append(1)
            else:
                rest.update(binding)
            value = rest
        result = {}
        for key, child in value.items():
            clean = _resolve_bindings(child, resolve, dropped)
            if clean is not _DROP:
                result[key] = clean
        return result
    if isinstance(value, list):
        result = []
        for child in value:
            clean = _resolve_bindings(child, resolve, dropped)
            if clean is not _DROP:
                result.append(clean)
        return result
    return value


def _remap_identities(value, mapping):
    if isinstance(value, dict):
        return {key: _remap_identities(child, mapping) for key, child in value.items()}
    if isinstance(value, list):
        return [_remap_identities(child, mapping) for child in value]
    return mapping.get(value, value) if isinstance(value, str) else value


def _import_profiles(store, principal, sid, documents, warnings):
    counts = {"created": 0, "updated": 0, "skipped": 0}
    for kind, rows in documents.items():
        for document in rows:
            try:
                key = api._id(document["id"], "profile id")
                existing = store.get(kind, key)
                if existing and (existing["station_id"] != sid or
                        (kind == "view_profiles" and existing["owner_user_id"] != principal.user_id)):
                    key = str(uuid5(NAMESPACE_URL, f"radios-transfer-{kind}:{sid}:{principal.user_id}:{key}"))
                    existing = store.get(kind, key)
                name = document["name"]
                peers = [row for row in store.scan(kind) if row["station_id"] == sid and not row.get("retired_at") and
                         (kind != "view_profiles" or row["owner_user_id"] == principal.user_id)]
                if any(row["name"] == name and row["_id"] != key for row in peers):
                    name = f"{name} ({document['id'][:8]})"
                config = document["config"]
                categories = {}
                for identifier, values in document.get("categories", {}).items():
                    key_category = api._id(identifier, "category id")
                    old_category = store.get("categories", key_category)
                    peers_category = store.lookup("categories", "station_code", sid, values["code"])
                    if peers_category or old_category and old_category["station_id"] != sid:
                        key_category = peers_category[0]["_id"] if peers_category else str(uuid5(NAMESPACE_URL,
                            f"radios-transfer-category:{sid}:{identifier}"))
                        old_category = store.get("categories", key_category)
                    store.put("categories", key_category, dict(values) | {"station_id": sid},
                              expected_revision=old_category["_rev"] if old_category else None, actor=principal.user_id)
                    categories[identifier] = key_category
                config = _remap_identities(config, categories)
                if kind == "view_profiles":
                    station_settings.put_view_profile(store, principal, sid, name=name, view_kind=config["view_kind"],
                        columns=config["columns"], filters=config["filters"], profile_id=key,
                        expected_revision=existing["_rev"] if existing else None)
                else:
                    station_settings.put_import_profile(store, principal, sid, name=name, library_key=config["library_key"],
                        config=config["config"], status=config.get("status", "active"), profile_id=key,
                        expected_revision=existing["_rev"] if existing else None)
                counts["updated" if existing else "created"] += 1
            except (StoreError, Conflict, KeyError, TypeError) as exc:
                warnings.append(f"Profil geri yüklenemedi ({exc}): {document.get('name', kind)}")
                counts["skipped"] += 1
    return counts


def _import_campaigns(store, principal, sid, rows, resolved, warnings):
    """Same-named open campaigns are updated, others are created as drafts;
    the spot binds again when its audio is in this station's library."""
    from . import desktop_advert
    counts = {"created": 0, "updated": 0, "skipped": 0, "bound": 0}
    for document in rows:
        name = document["name"]
        try:
            config = document["config"]
            peers = [row for row in _station_campaigns(store, sid) if row["name"] == name.strip()]
            existing = peers[0] if peers else None
            key = str(uuid5(NAMESPACE_URL, f"radios-transfer-campaign:{sid}:{document['id']}:"
                                           f"{existing['_id'] if existing else ''}:{existing['_rev'] if existing else 0}"))
            saved = api.save_campaign(store, principal, sid, name, config["fields"], key,
                                      campaign_id=existing["_id"] if existing else None,
                                      expected_revision=existing["_rev"] if existing else None)
            counts["updated" if existing else "created"] += 1
            if not existing and config.get("status") != "draft":
                warnings.append(f"Kampanya taslak olarak alındı, onayı bu istasyonda yeniden verilmeli: {name}")
            digest = config.get("sha256")
            if isinstance(digest, str) and _SHA.fullmatch(digest) and not saved.get("desktop_binding"):
                value = resolved.get(digest)
                if value is None:
                    warnings.append(f"Kampanya spotunun sesi bu istasyonda yok, spot bağlanmadı: {name}")
                else:
                    desktop_advert.bind(store, principal, sid, saved["_id"], saved["_rev"], value["media_id"],
                                        value["version_id"], value["media_file_id"], value["media_sha256"],
                                        str(uuid5(NAMESPACE_URL, f"radios-transfer-campaign-bind:{sid}:{saved['_id']}:{saved['_rev']}")))
                    counts["bound"] += 1
        except (StoreError, Conflict, KeyError, TypeError, Forbidden, NotFound) as exc:
            warnings.append(f"Kampanya geri yüklenemedi ({exc}): {name}")
            counts["skipped"] += 1
    return counts


def _import_documents(store, principal, sid, documents, resolved, warnings, counters, identity_map=None):
    now = _now()
    user = principal.user_id
    identity_map = identity_map if identity_map is not None else {}

    def resolve(value):
        binding = _lookup_binding(resolved, value)
        return dict(binding) if binding else None

    pending = []  # [kind, key, body, expected_revision, label]
    with store._locked(exclusive=False):
        store._reload_locked()
        # Allocate all identities before rewriting references, including forward references.
        for kind, rows in documents.items():
            for document in rows:
                key = document["id"]
                old = store.get("desktop_configurations", key)
                if old and (old["station_id"] != sid or old["kind"] != kind):
                    identity_map[key] = str(uuid5(NAMESPACE_URL, f"radios-transfer-config:{sid}:{key}"))
                if kind in SINGLETON_KINDS:
                    canonical = [row for row in store.lookup("desktop_configurations", "station_kind", sid, kind)
                                 if row["status"] == "active" and row["name"] == "main"]
                    if canonical:
                        identity_map[key] = canonical[0]["_id"]
        order = {kind: index for index, kind in enumerate(_KIND_ORDER)}
        kinds = sorted(documents, key=lambda kind: (order.get(kind, len(order)), kind))
        for kind in kinds:
            for document in documents[kind]:
                label = f"{LABELS[kind]} › {document['name']}"
                try:
                    name = api._text(document["name"], "name", 160, True).strip()
                    key = api._id(identity_map.get(document["id"], document["id"]), "document id")
                except StoreError:
                    warnings.append(f"Belge adı veya kimliği geçersiz, atlandı: {label}")
                    counters["skipped"] += 1
                    continue
                dropped = []
                config = _remap_identities(_resolve_bindings(document["config"], resolve, dropped), identity_map)
                if dropped:
                    warnings.append(f"{len(dropped)} medya bağı çözülemedi ve kaldırıldı: {label}")
                existing = store.get("desktop_configurations", key)
                if kind in SINGLETON_KINDS:
                    name = "main"
                    canonical = [row for row in store.lookup("desktop_configurations", "station_kind", sid, kind)
                                 if row["status"] == "active" and row["name"] == "main"]
                    if canonical and canonical[0]["_id"] != key:
                        existing, key = canonical[0], canonical[0]["_id"]
                if existing is not None and (existing["station_id"] != sid or existing["kind"] != kind):
                    warnings.append(f"Belge kimliği başka istasyona veya türe ait, atlandı: {label}")
                    counters["skipped"] += 1
                    continue
                if existing is None and not _free(store, "desktop_configurations", key):
                    key = str(uuid4())
                body = {"station_id": sid, "kind": kind, "name": name, "config": config, "status": "active",
                        "created_by": existing["created_by"] if existing else user,
                        "created_at": existing["created_at"] if existing else now,
                        "updated_by": user, "updated_at": now}
                pending.append([kind, key, body, existing["_rev"] if existing else None, label])
        # Validate against the state as it will be once every pending document exists.
        while True:
            overlay = dict(store._state)
            docs = dict(overlay.get("desktop_configurations", {}))
            for _kind, key, body, revision, _label in pending:
                docs[key] = {"revision": (revision or 0) + 1, "data": body}
            overlay["desktop_configurations"] = docs
            failed = []
            for row in pending:
                kind, _key, body, _revision, label = row
                try:
                    body["config"] = api.validate_configuration(kind, body["config"], sid, overlay, persisted=True)
                except (StoreError, Conflict, NotFound, KeyError, TypeError, ValueError) as exc:
                    warnings.append(f"Belge doğrulamadan geçmedi ({exc}), atlandı: {label}")
                    failed.append(row)
            if not failed:
                break
            pending = [row for row in pending if row not in failed]
            counters["skipped"] += len(failed)
    while pending:
        chunk, size = [], 0
        for row in pending:
            size += len(json.dumps(row[2]["config"], ensure_ascii=False, allow_nan=False).encode()) + 1024
            if chunk and size > _TRANSACTION_BYTES:
                break
            chunk.append(row)
        pending = pending[len(chunk):]
        try:
            _commit_documents(store, user, chunk)
            for row in chunk:
                counters["updated" if row[3] else "created"] += 1
        except (StoreError, Conflict):
            for row in chunk:
                try:
                    _commit_documents(store, user, [row])
                    counters["updated" if row[3] else "created"] += 1
                except (StoreError, Conflict) as exc:
                    warnings.append(f"Belge yazılamadı ({exc}): {row[4]}")
                    counters["skipped"] += 1


def _commit_documents(store, user, rows):
    sequence = store.sequence
    tx = store.begin(actor=user)
    tx.expect_sequence(sequence)
    for _kind, key, body, revision, _label in rows:
        tx.put("desktop_configurations", key, body, expected_revision=revision)
    tx.commit()


def _run_import(job, store, principal, sid, station, path, options):
    warnings = []
    zf, manifest, songs, names = _open_package(path, full_check=False)
    with zf:
        documents = _package_documents(zf, names, warnings) if options["settings"] else {}
        if options["kinds"] is not None:
            documents = {kind: rows for kind, rows in documents.items() if kind in options["kinds"]}
        local_settings = {}
        for document in documents.pop(LOCAL_SETTINGS, []):
            local_settings.update(_local_settings(document["config"].get("files")))
        profile_documents = {kind: documents.pop(kind) for kind in PROFILE_KINDS if kind in documents}
        campaign_documents = documents.pop(CAMPAIGNS, [])
        if manifest.get("schema_version") not in (None, store.schema_version):
            warnings.append(f"Paket şema sürümü {manifest.get('schema_version')}, bu veritabanı {store.schema_version}.")
        media_total = len(songs) if options["media"] else 0
        document_total = sum(len(rows) for rows in documents.values())
        total = max(1, media_total + document_total)

        def progress(done, _count, message):
            job.update(done * 90 // total, message)

        resolved, placed, media_summary = {}, {}, {"placed": 0, "registered": 0, "reused": 0, "skipped": 0, "bytes": 0}
        if options["media"]:
            if not songs:
                warnings.append("Paket medya taşımıyor; yalnız ayarlar geri yüklendi.")
            else:
                resolved, placed, media_summary = _import_media(job, store, principal, sid, zf, songs, names,
                                                               options["media_root"], warnings, progress)
        job.check()
        playlist_counters = {"created": 0, "updated": 0, "skipped": 0}
        entry_counters = {"bound": 0, "unbound": 0}
        document_counters = {"created": 0, "updated": 0, "skipped": 0}
        campaign_counters = {"created": 0, "updated": 0, "skipped": 0, "bound": 0}
        identity_map = {manifest["station"].get("id"): sid}
        if documents or campaign_documents:
            # Songs the station already holds resolve bindings too; a song
            # placed by this job wins over an older row for the same bytes.
            with store._locked(exclusive=False):
                store._reload_locked()
                known = {}
                for library in store.lookup("desktop_media_library", "station", sid):
                    media = store.get("media_files", library["media_file_id"])
                    if library["status"] == "active" and media and media.get("status") == "verified":
                        _remember_binding(known, {"sha256": media["sha256"], "library_id": library["_id"],
                            "version_id": library["version_id"], "media_file_id": media["_id"]}, _binding(library, media))
            resolved = {**known, **resolved}
            job.update(media_total * 90 // total, "Playlistler geri yükleniyor…")
            for document in documents.get(PLAYLISTS, []):
                job.check()
                try:
                    identity_map[document["id"]] = _import_playlist(store, principal, sid, document, resolved, placed,
                                                                  warnings, playlist_counters, entry_counters)
                except (StoreError, Conflict, KeyError, TypeError) as exc:
                    warnings.append(f"Playlist geri yüklenemedi ({exc}): {document.get('name')}")
                    playlist_counters["skipped"] += 1
            job.update((media_total + len(documents.get(PLAYLISTS, []))) * 90 // total, "Ayarlar geri yükleniyor…")
            others = {kind: rows for kind, rows in documents.items() if kind != PLAYLISTS}
            if others:
                _import_documents(store, principal, sid, others, resolved, warnings, document_counters, identity_map)
            if campaign_documents:
                job.check()
                campaign_counters = _import_campaigns(store, principal, sid, campaign_documents, resolved, warnings)
        profiles = _import_profiles(store, principal, sid, profile_documents, warnings)
    job.update(100, "İçe aktarma tamamlandı.")
    return {"package_path": str(path), "station": {"id": sid, "name": station["name"]},
            "source_station": manifest.get("station"), "media_root": str(options["media_root"]) if options["media_root"] else None,
            "documents": document_counters, "playlists": playlist_counters, "entries": entry_counters,
            "media": media_summary, "warnings": warnings, "local_settings": local_settings, "profiles": profiles,
            "campaigns": campaign_counters}
