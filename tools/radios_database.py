#!/usr/bin/env python3
"""RadioS local SDBASE installation and bounded, owned service lifecycle.

This manages the existing data engine; the desktop GUI and future data client
remain C. Secrets are read from the private installation file, never arguments.
"""

from __future__ import annotations

import argparse
import configparser
import dataclasses
import fcntl
import http.client
import importlib
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
from uuid import UUID, uuid4

from radios_database_setup import (
    DatabaseError, LocalConfig, _contained, _credentials_validate, _json_read, _regular, backend_import,
    backend_source_digest, initialize, load_config, operation_lock,
)
from radios_database_setup import load_credentials as _load_credentials_in_project


def _read_json(path: Path) -> dict | None:
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        return None
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > 16384:
            raise DatabaseError("Servis durum dosyası geçerli bir küçük dosya olmalı.")
        if info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise DatabaseError("Servis durum dosyası yalnız kullanıcıya açık olmalı (0600).")
        value = json.loads(stream.read(16385))
    if not isinstance(value, dict):
        raise DatabaseError("Servis durum dosyası geçersiz.")
    return value


def _write_json(path: Path, value: dict) -> None:
    fd, name = tempfile.mkstemp(prefix=".service-", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=False, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        if path.is_symlink():
            raise DatabaseError("Servis durumu sembolik bağlantı olamaz.")
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def _process_identity(pid: int) -> tuple[str, str, list[str]] | None:
    if type(pid) is not int or pid <= 1:
        return None
    try:
        process = Path("/proc") / str(pid)
        fields = (process / "stat").read_text().rsplit(")", 1)[1].split()
        if fields[0] == "Z":
            return None
        boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        args = (process / "cmdline").read_bytes().decode().rstrip("\0").split("\0")
        return boot, fields[19], args
    except (OSError, UnicodeError, IndexError):
        return None


SERVICE_START_TIMEOUT_SECONDS = 60.0

# config/storage.ini (Ayarlar › Medya arşivi › Radyo verileri) mirrors
# src/core/storage_locations.c: the same file, keys, path rule and marker
# rule, so the C application and this service open the same database file.
STORAGE_FILE = "storage.ini"
STORAGE_MARKER = ".radios-storage.json"
STORAGE_MARKER_FORMAT = "radios-storage-v1"
STORAGE_ROLES = {"database_dir": "database", "recording_root": "recordings", "backup_root": "backups"}
_STORAGE_PROTECTED = ("config", "src", "include", "tools", "tests", "docs", "design", "assets", "vendor",
                      "packaging", "cmake", "database/SDBASE")
_STORAGE_LAST = "storage-last.json"


def _keyfile_unescape(value: str) -> str:
    """The escapes g_key_file_set_string writes into a value."""
    result = []
    index = 0
    while index < len(value):
        char = value[index]
        if char == "\\" and index + 1 < len(value):
            index += 1
            char = {"s": " ", "n": "\n", "t": "\t", "r": "\r", "\\": "\\"}.get(value[index], "\\" + value[index])
        result.append(char)
        index += 1
    return "".join(result)


def storage_path_check(path: str) -> None:
    """radios_storage_path_check_syntax: absolute, simplified text, not the root."""
    if not path:
        raise DatabaseError("Klasör yolu boş.")
    if len(path) > 4000:
        raise DatabaseError("Klasör yolu çok uzun.")
    if any(ord(char) < 0x20 or ord(char) == 0x7F for char in path):
        raise DatabaseError("Klasör yolu denetim karakteri içeremez.")
    if not path.startswith("/"):
        raise DatabaseError("Klasör yolu mutlak olmalı (/ ile başlamalı).")
    if "//" in path or os.path.normpath(path) != path:
        raise DatabaseError("Klasör yolu sadeleştirilmiş olmalı (.. , // veya sondaki / içermemeli).")
    if path == "/":
        raise DatabaseError("Kök dizin seçilemez.")


def _inside(root: str, path: str) -> bool:
    return path == root or path.startswith(root.rstrip("/") + "/")


def storage_marker_check(directory: Path, role: str) -> None:
    marker = directory / STORAGE_MARKER
    try:
        info = marker.lstat()
    except OSError:
        raise DatabaseError(f"{directory} klasöründe RadioS işareti yok. Disk takılı olmayabilir veya "
                            "klasör RadioS ile hazırlanmamış.") from None
    value = None
    if stat.S_ISREG(info.st_mode) and 0 < info.st_size <= 4096:
        try:
            value = json.loads(marker.read_bytes().decode("utf-8"))
        except (OSError, UnicodeError, ValueError):
            value = None
    if not isinstance(value, dict) or value.get("format") != STORAGE_MARKER_FORMAT:
        raise DatabaseError(f"{directory} klasöründeki RadioS işareti okunamadı veya geçersiz.")
    if value.get("role") != role:
        raise DatabaseError(f"Bu klasör RadioS tarafından başka amaçla ({value.get('role', '?')}) kullanılıyor.")


def storage_directory_check(project_root: Path, role: str, directory: str) -> Path:
    """radios_storage_directory_check with the role marker required."""
    storage_path_check(directory)
    try:
        info = os.lstat(directory)
    except FileNotFoundError:
        raise DatabaseError(f"Klasör bulunamadı: {directory}") from None
    except OSError as exc:
        raise DatabaseError(f"Klasör incelenemedi: {directory}") from exc
    if stat.S_ISLNK(info.st_mode):
        raise DatabaseError(f"Klasör sembolik bağlantı olamaz: {directory}")
    if not stat.S_ISDIR(info.st_mode):
        raise DatabaseError(f"Seçilen yol bir klasör değil: {directory}")
    if os.path.realpath(directory) != directory:
        raise DatabaseError(f"Klasör yolunda sembolik bağlantı var; gerçek yolu seçin: {directory}")
    if not os.access(directory, os.R_OK | os.W_OK | os.X_OK):
        raise DatabaseError(f"Klasöre yazma izni yok: {directory}")
    root = os.path.realpath(project_root)
    if any(_inside(os.path.join(root, name), directory) for name in _STORAGE_PROTECTED):
        raise DatabaseError(f"RadioS'un kendi kaynak veya ayar klasörü seçilemez: {directory}")
    storage_marker_check(Path(directory), role)
    return Path(directory)


def load_storage_settings(project_root: Path) -> dict[str, str]:
    """{key: path} from config/storage.ini; a missing file is empty, a link is rejected."""
    path = Path(project_root) / "config" / STORAGE_FILE
    try:
        info = path.lstat()
    except FileNotFoundError:
        return {}
    except OSError as exc:
        raise DatabaseError("Depolama ayarları (storage.ini) incelenemedi.") from exc
    if not stat.S_ISREG(info.st_mode) or info.st_size > 16384:
        raise DatabaseError("storage.ini normal dosya olmalı ve 16 KB sınırını aşmamalı; bağlantılar kabul edilmez.")
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    settings = {}
    try:
        parser.read_string(path.read_bytes().decode("utf-8"))
        if parser.getint("storage", "version") != 1:
            raise DatabaseError("storage.ini version=1 olmalı.")
        for key in STORAGE_ROLES:
            value = _keyfile_unescape(parser.get("storage", key, fallback=""))
            if value:
                storage_path_check(value)
                settings[key] = value
    except (configparser.Error, ValueError, UnicodeError, OSError) as exc:
        raise DatabaseError(f"Depolama ayarları (storage.ini) okunamadı: {exc}") from exc
    return settings


def effective_backup_root(cfg: LocalConfig) -> Path:
    """The backup folder right now: storage.ini's when usable, else the configured default."""
    try:
        value = load_storage_settings(cfg.project_root).get("backup_root")
        return storage_directory_check(cfg.project_root, "backups", value) if value else cfg.backup_root
    except DatabaseError:
        return cfg.backup_root


def effective_config(cfg: LocalConfig) -> LocalConfig:
    """Applies storage.ini like radios_storage_apply: an unusable database folder is an
    error, because a stale copy must not be opened; a backup folder falls back."""
    settings = load_storage_settings(cfg.project_root)
    database_file = cfg.database_file
    if settings.get("database_dir"):
        try:
            directory = storage_directory_check(cfg.project_root, "database", settings["database_dir"])
        except DatabaseError as exc:
            raise DatabaseError(f"Veritabanı klasörü kullanılamıyor: {exc} Eski bir kopyayla başlamamak için "
                                "servis açılmadı; diski takın ya da config/storage.ini dosyasını kaldırın.") from exc
        database_file = directory / cfg.database_file.name
    return dataclasses.replace(cfg, database_file=database_file, backup_root=effective_backup_root(cfg))


def _effective_or_base(cfg: LocalConfig) -> LocalConfig:
    try:
        return effective_config(cfg)
    except DatabaseError:
        return cfg


def _database_in_project(cfg: LocalConfig) -> bool:
    try:
        cfg.database_file.relative_to(cfg.project_root)
        return True
    except ValueError:
        return False


def load_credentials(cfg: LocalConfig, *, authenticate_tokens: bool = True) -> dict:
    """The installation's credentials, validated against the effective database.

    credentials.json keeps the project-relative path of the installation; a
    database moved by storage.ini is matched by its file name, as
    src/core/data_client.c does, and the moved store itself is opened."""
    if _database_in_project(cfg):
        return _load_credentials_in_project(cfg, authenticate_tokens=authenticate_tokens)
    backend = backend_import(cfg)
    value = _json_read(_contained(cfg.project_root, cfg.credentials_file, reject_link=True))
    stored = value.get("database_file")
    if not isinstance(stored, str) or Path(stored).is_absolute() or Path(stored).name != cfg.database_file.name:
        raise DatabaseError("Kimlik dosyasının veritabanı yolu eşleşmiyor.")
    for suffix in ("", ".meta", ".lock", ".checkpoint"):
        sidecar = cfg.database_file.with_name(cfg.database_file.name + suffix)
        if sidecar.is_symlink():
            raise DatabaseError("Veritabanı dosyaları sembolik bağlantı olamaz.")
        _regular(sidecar, optional=suffix == ".checkpoint")
    try:
        store = backend.open_radio_store(cfg.database_file, read_only=True,
                                         lock_timeout=cfg.connect_timeout_seconds)
    except (backend.StoreError, OSError, ValueError) as exc:
        raise DatabaseError("SDBASE veritabanı açılamadı; mevcut dosyalar korunuyor.") from exc
    stored_view = dataclasses.replace(cfg, database_file=cfg.project_root / stored)
    return _credentials_validate(stored_view, value, backend, store, authenticate_tokens)


def _last_database(cfg: LocalConfig) -> Path | None:
    state = _read_json(cfg.runtime_dir / _STORAGE_LAST)
    value = state.get("database_file") if state else None
    return Path(value) if isinstance(value, str) and value.startswith("/") else None


def _remember_database(cfg: LocalConfig) -> None:
    cfg.runtime_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    _write_json(cfg.runtime_dir / _STORAGE_LAST, {"schema_version": 1, "database_file": str(cfg.database_file)})


def _copy_fsync(source: Path, target: Path) -> None:
    shutil.copy2(source, target, follow_symlinks=False)
    fd = os.open(target, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _migrate_database(cfg: LocalConfig, source: Path) -> None:
    """One-time copy of a database with its sidecars and managed media objects
    into the folder storage.ini now names. The journal is published last, so an
    interrupted copy is repeated instead of being opened. The source stays."""
    target = cfg.database_file
    if source == target or target.exists() or target.is_symlink():
        return
    for suffix in ("", ".meta"):
        origin = source.with_name(source.name + suffix)
        if origin.is_symlink() or not origin.is_file():
            raise DatabaseError(f"Taşınacak veritabanı dosyası bulunamadı: {origin}")
    objects = source.with_name(source.name + ".objects")
    if objects.is_dir() and not objects.is_symlink():
        shutil.copytree(objects, target.with_name(target.name + ".objects"), symlinks=False, dirs_exist_ok=True)
    for suffix in (".meta", ".checkpoint", ".lock"):
        origin = source.with_name(source.name + suffix)
        if origin.is_file() and not origin.is_symlink():
            _copy_fsync(origin, target.with_name(target.name + suffix))
    lock = target.with_name(target.name + ".lock")
    if not lock.exists() and not lock.is_symlink():
        lock.touch(mode=0o600)
    stage = target.with_name("." + target.name + "-" + uuid4().hex + ".tmp")
    try:
        _copy_fsync(source, stage)
        os.replace(stage, target)
    finally:
        stage.unlink(missing_ok=True)
    directory = os.open(target.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    os.sync()


def _prepare_database(base: LocalConfig, cfg: LocalConfig) -> None:
    """A folder newly chosen in Ayarlar has no database yet: copy the one used
    last (or the project default) into it before the service opens it. An own
    service still writing the old copy is stopped first."""
    if cfg.database_file.exists() or cfg.database_file.is_symlink():
        return
    last = _last_database(cfg)
    candidates = [last] if last else []
    candidates.append(base.database_file)
    source = next((path for path in candidates if path != cfg.database_file and path.is_file()), None)
    if source is None:
        raise DatabaseError(f"Veritabanı dosyası yok: {cfg.database_file}. Önce diski takın ya da "
                            "config/storage.ini dosyasını kaldırın.")
    if _owned(cfg, _read_json(_state_path(cfg)), same_database=False):
        stopped = stop_service(cfg)
        if stopped["state"] != "stopped":
            raise DatabaseError("Veritabanı taşınmadan önce eski servis kapatılamadı.")
    _migrate_database(cfg, source)


def _state_path(cfg: LocalConfig) -> Path:
    return cfg.runtime_dir / "service.json"


def _owned(cfg: LocalConfig, state: dict | None, *, same_database: bool = True) -> bool:
    if not state or state.get("schema_version") != 1:
        return False
    if (state.get("project_root") != str(cfg.project_root) or
            (same_database and state.get("database_file") != str(cfg.database_file)) or
            state.get("service_url") != cfg.service_url):
        return False
    identity = _process_identity(state.get("pid"))
    if identity is None:
        return False
    boot, started, args = identity
    try:
        instance = str(UUID(state["instance"]))
    except (ValueError, KeyError, TypeError, AttributeError):
        return False
    script = str(cfg.project_root / "tools" / "radios_database.py")
    expected = [script, "--project-root", str(cfg.project_root), "--config",
                str(cfg.config_file), "serve", "--instance", instance]
    return (boot == state.get("boot_id") and started == state.get("start_ticks")
            and len(args) >= len(expected) and args[-len(expected):] == expected)


def _request(cfg: LocalConfig, path: str, token: str | None = None,
             *, timeout: float | None = None) -> dict:
    # http.client bypasses proxy environment variables for this loopback service.
    connection = http.client.HTTPConnection(cfg.host, cfg.port,
        timeout=timeout or cfg.request_timeout_seconds)
    headers = {"Accept": "application/json", "Connection": "close"}
    if token is not None:
        headers["Authorization"] = "Bearer " + token
    try:
        connection.request("GET", path, headers=headers)
        response = connection.getresponse()
        body = response.read(2 * 1024 * 1024 + 1)
        if response.status != 200:
            raise DatabaseError(f"Veri servisi {path} için HTTP {response.status} döndürdü.")
        if len(body) > 2 * 1024 * 1024:
            raise DatabaseError("Veri servisi yanıt sınırını aştı.")
        result = json.loads(body)
        if not isinstance(result, dict):
            raise DatabaseError("Veri servisi JSON nesnesi döndürmedi.")
        return result
    finally:
        connection.close()


def _occupied(cfg: LocalConfig) -> bool:
    try:
        with socket.create_connection((cfg.host, cfg.port), timeout=0.25):
            return True
    except OSError:
        return False


def service_status(cfg: LocalConfig, credentials: dict | None = None) -> dict:
    cfg = _effective_or_base(cfg)
    credentials = credentials or load_credentials(cfg, authenticate_tokens=False)
    state = _read_json(_state_path(cfg))
    result = {"service_url": cfg.service_url, "store_id": credentials["store_id"],
              "database_file": str(cfg.database_file)}
    # The installation's own service on a database folder storage.ini no longer
    # names is still ours: start replaces it, stop closes it.
    if _owned(cfg, state, same_database=False):
        result["pid"] = state["pid"]
        result["stale_database"] = state.get("database_file") != str(cfg.database_file)
        try:
            health = _request(cfg, "/health", timeout=min(cfg.connect_timeout_seconds, 2))
            if health.get("status") != "ok" or health.get("store_id") != credentials["store_id"]:
                return {**result, "state": "foreign"}
            return {**result, "state": "running", "sequence": health["sequence"]}
        except (OSError, ValueError, http.client.HTTPException, DatabaseError):
            return {**result, "state": "unhealthy"}
    return {**result, "state": "foreign" if _occupied(cfg) else "stopped"}


def start_service(cfg: LocalConfig) -> dict:
    base, cfg = cfg, effective_config(cfg)
    pending_restore = cfg.runtime_dir / "restore-pending.json"
    if pending_restore.exists() or pending_restore.is_symlink():
        raise DatabaseError("Kesilmiş geri yükleme var; önce offline recover komutunu çalıştırın.")
    _prepare_database(base, cfg)
    credentials = load_credentials(cfg)
    expected_digest = backend_source_digest(cfg)
    current = service_status(cfg, credentials)
    restarted = False
    if current["state"] == "running":
        capabilities = _request(cfg, "/v1/capabilities", credentials["api_token"])
        if (capabilities.get("source_digest") == expected_digest and
                capabilities.get("desktop_api_version") == 1 and not current.get("stale_database")):
            _remember_database(cfg)
            return {**current, "already_running": True, "restarted": False,
                    "source_digest": expected_digest}
        # service_status has checked process ownership and store identity;
        # stop_service pins and rechecks that process before signaling it.
        stopped = stop_service(cfg)
        if stopped["state"] != "stopped":
            raise DatabaseError("Eski veri servisi güvenle kapatılamadı; yeniden başlatılmadı.")
        restarted = stopped["stopped"]
        current = service_status(cfg, credentials)
    if current["state"] != "stopped":
        raise DatabaseError("Servis başlatılamadı: port kullanımda veya önceki süreç sağlıksız. "
                            "Başka servise müdahale edilmedi.")
    log = cfg.log_root / "database-service.log"
    cfg.log_root.mkdir(parents=True, exist_ok=True)
    log_fd = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
    info = os.fstat(log_fd)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
        os.close(log_fd)
        raise DatabaseError("Servis günlüğü kullanıcıya ait normal dosya olmalı.")
    os.fchmod(log_fd, 0o600)
    instance = str(uuid4())
    args = shlex.split(cfg.python_command) + [
        str(cfg.project_root / "tools" / "radios_database.py"),
        "--project-root", str(cfg.project_root), "--config", str(cfg.config_file),
        "serve", "--instance", instance,
    ]
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    try:
        child = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log_fd,
            stderr=log_fd, cwd=cfg.project_root, env=environment, start_new_session=True,
            close_fds=True)
    finally:
        os.close(log_fd)
    # Store open time grows with the checkpoint (290 MB took ~14 s on 2026-10-17);
    # the connect timeout only bounds one request. A dead child still fails fast.
    deadline = time.monotonic() + max(cfg.connect_timeout_seconds + 3,
                                      SERVICE_START_TIMEOUT_SECONDS)
    try:
        while time.monotonic() < deadline:
            if child.poll() is not None:
                raise DatabaseError("Veri servisi açılamadı; data/logs/database-service.log dosyasını kontrol edin.")
            state = _read_json(_state_path(cfg))
            if state and state.get("instance") == instance and _owned(cfg, state):
                current = service_status(cfg, credentials)
                if current["state"] == "running":
                    capabilities = _request(cfg, "/v1/capabilities", credentials["api_token"])
                    if capabilities.get("source_digest") != expected_digest:
                        raise DatabaseError("Veri servisi açılırken kaynak kod değişti; yeniden başlatın.")
                    _remember_database(cfg)
                    return {**current, "already_running": False, "restarted": restarted,
                            "source_digest": expected_digest}
            time.sleep(0.05)
        raise DatabaseError("Veri servisi başlangıç süresini aştı.")
    except BaseException:
        # This is our own Popen handle, never a PID taken from an arbitrary file.
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=3)
        raise


def stop_service(cfg: LocalConfig) -> dict:
    cfg = _effective_or_base(cfg)
    state = _read_json(_state_path(cfg))
    if not _owned(cfg, state, same_database=False):
        return {"state": "foreign" if _occupied(cfg) else "stopped",
                "stopped": False, "service_url": cfg.service_url}
    # Linux pidfd pins the checked process even if its numeric PID is recycled.
    try:
        descriptor = os.pidfd_open(state["pid"])
    except ProcessLookupError:
        return {"state": "stopped", "stopped": False, "service_url": cfg.service_url}
    try:
        if not _owned(cfg, state, same_database=False):
            raise DatabaseError("Servis süreç kimliği değişti; kapatma uygulanmadı.")
        signal.pidfd_send_signal(descriptor, signal.SIGTERM)
        deadline = time.monotonic() + min(cfg.request_timeout_seconds + 3, 60)
        while time.monotonic() < deadline:
            if not _owned(cfg, state, same_database=False):
                return {"state": "stopped", "stopped": True,
                        "service_url": cfg.service_url}
            time.sleep(0.05)
        raise DatabaseError("Servis kapanışı sürüyor; açık isteklerin bitmesi bekleniyor.")
    finally:
        os.close(descriptor)


def check_service(cfg: LocalConfig) -> dict:
    cfg = effective_config(cfg)
    credentials = load_credentials(cfg)
    current = service_status(cfg, credentials)
    if current["state"] != "running":
        raise DatabaseError("Veritabanı kontrolü için bu RadioS kurulumunun servisi çalışmalı.")
    token = credentials["api_token"]
    capabilities = _request(cfg, "/v1/capabilities", token)
    if (capabilities.get("source_digest") != backend_source_digest(cfg) or
            capabilities.get("desktop_api_version") != 1):
        raise DatabaseError("Veri servisi eski kaynak kodla çalışıyor; start komutuyla güncelleyin.")
    metrics = _request(cfg, "/v1/operations/metrics", token)
    integrity = _request(cfg, "/v1/operations/integrity", token)
    if (capabilities.get("schema_version") != 4 or
            metrics.get("store_id") != credentials["store_id"] or
            integrity.get("store_id") != credentials["store_id"] or
            integrity.get("status") != "ok" or integrity.get("complete") is not True):
        raise DatabaseError("Veritabanı şema, kimlik veya bütünlük kabulü başarısız.")
    return {**current, "status": "ok", "schema_version": capabilities["schema_version"],
            "source_digest": capabilities["source_digest"],
            "engine_version": capabilities["api_version"], "integrity": "ok",
            "collections": metrics["collections"],
            "station_id": credentials["station_id"], "endpoint_id": credentials["endpoint_id"]}


def initial_backup(cfg: LocalConfig) -> dict:
    cfg = effective_config(cfg)
    credentials = load_credentials(cfg, authenticate_tokens=False)
    backend = backend_import(cfg)
    backup = importlib.import_module(backend.package_name + ".backup")
    cfg.backup_root.mkdir(parents=True, exist_ok=True)
    destination = cfg.backup_root / "initial-installation"
    if destination.is_symlink():
        raise DatabaseError("İlk kurulum yedeği sembolik bağlantı olamaz.")
    if destination.exists():
        verified = backup.verify_backup(destination)
        if verified.get("store_id") != credentials["store_id"]:
            raise DatabaseError("Mevcut ilk kurulum yedeği başka depoya ait.")
        return {"status": "verified", "path": str(destination), "store_id": credentials["store_id"]}
    store = backend.open_radio_store(cfg.database_file, read_only=True)
    backup.create_backup(store, destination, checkpoint=False)
    verified = backup.verify_backup(destination)
    if verified.get("store_id") != credentials["store_id"]:
        raise DatabaseError("İlk kurulum yedeğinin depo kimliği uyuşmadı.")
    return {"status": "created", "path": str(destination), "store_id": credentials["store_id"]}


def serve(cfg: LocalConfig, instance: str) -> None:
    base, cfg = cfg, effective_config(cfg)
    pending_restore = cfg.runtime_dir / "restore-pending.json"
    if pending_restore.exists() or pending_restore.is_symlink():
        raise DatabaseError("Kesilmiş geri yükleme var; servis başlatılmadı.")
    instance = str(UUID(instance))
    credentials = load_credentials(cfg)
    backend = backend_import(cfg)
    http = importlib.import_module(backend.package_name + ".http_server")
    server_module = importlib.import_module(backend.package_name + ".api_server")
    fd = os.open(cfg.runtime_dir / "server.lock",
                 os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    server = None
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise DatabaseError("Servis kilidi normal dosya olmalı.")
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if pending_restore.exists() or pending_restore.is_symlink():
            raise DatabaseError("Kesilmiş geri yükleme var; servis başlatılmadı.")
        store = backend.open_radio_store(cfg.database_file)
        store.desktop_import_root = cfg.import_root
        # Re-read per backup, so a folder chosen in Ayarlar applies at once and
        # an unplugged backup disk falls back to the project default.
        store.desktop_backup_root = lambda: effective_backup_root(base)
        handler = http.make_handler(store)
        server = server_module.BoundedHTTPServer((cfg.host, cfg.port), handler,
            max_connections=32, request_timeout=cfg.request_timeout_seconds)
        # Finish accepted requests before closing; each request has a deadline.
        server.daemon_threads = False
        server.block_on_close = True
        server.timeout = 0.2
        stopping = threading.Event()
        signal.signal(signal.SIGTERM, lambda _signum, _frame: stopping.set())
        signal.signal(signal.SIGINT, lambda _signum, _frame: stopping.set())
        identity = _process_identity(os.getpid())
        if identity is None:
            raise DatabaseError("Linux servis süreç kimliği okunamadı.")
        boot, started, _args = identity
        state = {"schema_version": 1, "pid": os.getpid(), "boot_id": boot,
                 "start_ticks": started, "instance": instance,
                 "project_root": str(cfg.project_root), "database_file": str(cfg.database_file),
                 "service_url": cfg.service_url, "store_id": credentials["store_id"],
                 "source_digest": handler.source_digest}
        _write_json(_state_path(cfg), state)
        print(json.dumps({"event": "started", "service_url": cfg.service_url,
                          "store_id": credentials["store_id"]}), flush=True)
        while not stopping.is_set():
            server.handle_request()
    finally:
        if server is not None:
            server.server_close()
        saved = _read_json(_state_path(cfg))
        if saved and saved.get("instance") == instance:
            _state_path(cfg).unlink(missing_ok=True)
        os.close(fd)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="RadioS yerel veritabanı kurulum ve servis yönetimi")
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--config")
    commands = parser.add_subparsers(dest="command", required=True)
    init = commands.add_parser("init", help="İlk kimlik/veritabanını kur; var olan kurulumu koru")
    init.add_argument("--username", default="admin")
    init.add_argument("--station-code", default="RADIOS")
    init.add_argument("--station-name", default="RadioS")
    init.add_argument("--timezone", default="UTC")
    descriptions = {"start": "Yerel servisi başlat", "status": "Bu kurulumun servis durumunu göster",
                    "check": "Kimlik, şema ve bütünlüğü doğrula", "stop": "Bu kurulumun servisini kapat",
                    "backup": "İlk kurulum yedeğini oluştur veya doğrula"}
    for name, description in descriptions.items():
        commands.add_parser(name, help=description)
    foreground = commands.add_parser("serve", help="start tarafından çağrılan servis yürütücüsü")
    foreground.add_argument("--instance", required=True)
    args = parser.parse_args(argv)
    os.umask(0o077)
    try:
        cfg = load_config(args.project_root, args.config)
        if args.command == "serve":
            serve(cfg, args.instance)
            return 0
        with operation_lock(cfg):
            if args.command == "init":
                result = initialize(cfg, username=args.username, station_code=args.station_code,
                                    station_name=args.station_name, timezone_name=args.timezone)
            else:
                action = {"start": start_service, "status": service_status, "check": check_service,
                          "stop": stop_service, "backup": initial_backup}[args.command]
                result = action(cfg)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (RuntimeError, OSError, ValueError, http.client.HTTPException) as exc:
        print(f"RadioS veritabanı: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
