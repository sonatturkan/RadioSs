"""Local RadioS database identity installation, without external dependencies.

Callers serialize operations with ``operation_lock``. A first installation is
prepared privately and its identity is durably saved before any live database
file appears. The pending manifest allows the next init to finish a publication
interrupted between the journal, metadata, checkpoint and credential files.
"""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
import configparser
import fcntl
import hashlib
import importlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tempfile
import time
from types import SimpleNamespace
from urllib.parse import urlsplit
from uuid import UUID, uuid4
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError


class DatabaseError(RuntimeError):
    """An installation operation could not safely complete."""


@dataclass(frozen=True)
class LocalConfig:
    project_root: Path
    config_file: Path
    database_file: Path
    source_dir: Path
    endpoint_root: Path
    log_root: Path
    backup_root: Path
    service_url: str
    host: str
    port: int
    python_command: str
    token_environment: str
    connect_timeout_seconds: int
    request_timeout_seconds: int
    credentials_file: Path
    runtime_dir: Path
    import_root: Path


def _contained(root: Path, value: str | Path, *, reject_link: bool = False) -> Path:
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = root / candidate
    if reject_link and candidate.is_symlink():
        raise DatabaseError("Sembolik dosya bağlantısı kullanılamaz.")
    try:
        resolved = candidate.resolve(strict=False)
        resolved.relative_to(root)
    except (OSError, RuntimeError, ValueError) as exc:
        raise DatabaseError("Yol RadioS proje klasörünün dışına çıkıyor.") from exc
    return resolved


def _regular(path: Path, *, optional: bool = False, private: bool = False) -> bool:
    try:
        info = path.lstat()
    except FileNotFoundError:
        if optional:
            return False
        raise DatabaseError("Gerekli yerel dosya bulunamadı.") from None
    if not stat.S_ISREG(info.st_mode):
        raise DatabaseError("Yerel dosya normal dosya olmalı; bağlantı kullanılamaz.")
    if private and (stat.S_IMODE(info.st_mode) != 0o600 or info.st_uid != os.getuid()):
        raise DatabaseError("Kimlik veya servis dosyasının sahibi ve 0600 izni geçerli olmalı.")
    return True


def _read_bytes(path: Path, limit: int, *, private: bool = False) -> bytes:
    _regular(path, private=private)
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd, "rb") as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size > limit:
                raise DatabaseError("Yerel dosyanın türü veya boyutu geçersiz.")
            if private and (stat.S_IMODE(info.st_mode) != 0o600 or info.st_uid != os.getuid()):
                raise DatabaseError("Kimlik dosyası özel 0600 iznine sahip olmalı.")
            body = stream.read(limit + 1)
            if len(body) > limit:
                raise DatabaseError("Yerel dosya boyut sınırını aşıyor.")
            return body
    except OSError as exc:
        raise DatabaseError("Yerel dosya güvenli biçimde okunamadı.") from exc


def _json_read(path: Path, *, private: bool = True) -> dict:
    def pairs(values):
        result = {}
        for key, value in values:
            if key in result:
                raise DatabaseError("JSON içinde yinelenen alan bulundu.")
            result[key] = value
        return result
    def invalid_constant(_value):
        raise DatabaseError("JSON içinde standart dışı sayısal değer bulundu.")
    try:
        value = json.loads(_read_bytes(path, 64 * 1024, private=private).decode("utf-8"),
                           object_pairs_hook=pairs, parse_constant=invalid_constant)
    except (UnicodeError, ValueError) as exc:
        raise DatabaseError("Yerel JSON dosyası okunamadı.") from exc
    if not isinstance(value, dict):
        raise DatabaseError("Yerel JSON kökü bir nesne olmalı.")
    return value


def load_config(project_root: Path, config_path: str | None = None) -> LocalConfig:
    try:
        root = Path(project_root).resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise DatabaseError("RadioS proje kökü bulunamadı.") from exc
    if not root.is_dir():
        raise DatabaseError("RadioS proje kökü bir klasör olmalı.")
    if config_path is None:
        local = root / "config/radios.ini"
        config_file = local if local.exists() or local.is_symlink() else root / "config/radios.example.ini"
    else:
        config_file = _contained(root, config_path)
    config_file = _contained(root, config_file)
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    try:
        parser.read_string(_read_bytes(config_file, 256 * 1024).decode("utf-8"))
        def required(section, name):
            value = parser.get(section, name).strip()
            if not value or "\x00" in value:
                raise DatabaseError("Yapılandırmada gerekli alan boş veya geçersiz.")
            return value
        def path(section, name, reject_link=False):
            return _contained(root, required(section, name), reject_link=reject_link)
        database_file = path("paths", "database_file", True)
        if database_file.suffix != ".sdb":
            raise DatabaseError("Veritabanı yolu .sdb uzantılı bir dosya olmalı.")
        source_dir = path("database", "source_dir")
        endpoint_root = path("paths", "endpoint_root")
        log_root = path("paths", "log_root")
        backup_root = path("paths", "backup_root")
        import_root = path("paths", "import_root", True)
        service_url = required("database", "service_url")
        parsed = urlsplit(service_url)
        port = parsed.port
        if (parsed.scheme != "http" or parsed.hostname != "127.0.0.1" or
                parsed.username is not None or parsed.password is not None or
                parsed.path or parsed.query or parsed.fragment or port is None or
                not 1 <= port <= 65535 or parsed.netloc != f"127.0.0.1:{port}"):
            raise DatabaseError("Yerel servis adresi http://127.0.0.1:PORT biçiminde olmalı.")
        python_command = required("database", "python_command")
        if not re.fullmatch(r"[A-Za-z0-9_./+ -]+", python_command) or any(
                char.isspace() for char in python_command):
            raise DatabaseError("python_command tek bir çalıştırılabilir dosya adı veya yolu olmalı.")
        token_environment = required("database", "token_environment")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", token_environment):
            raise DatabaseError("Token ortam değişkeninin adı geçersiz.")
        connect = parser.getint("database", "connect_timeout_seconds")
        request = parser.getint("database", "request_timeout_seconds")
        if not 1 <= connect <= 300 or not connect <= request <= 3600:
            raise DatabaseError("Bağlantı ve istek süre sınırları geçersiz.")
    except (configparser.Error, ValueError, UnicodeError) as exc:
        raise DatabaseError("RadioS yapılandırması geçersiz.") from exc
    credentials = _contained(root, "config/credentials.json", reject_link=True)
    runtime = _contained(root, "data/database-service")
    return LocalConfig(root, config_file, database_file, source_dir, endpoint_root,
                       log_root, backup_root, service_url, "127.0.0.1", port,
                       python_command, token_environment, connect, request,
                       credentials, runtime, import_root)


def backend_source_digest(cfg: LocalConfig) -> str:
    """Match the running service's sorted basename/NUL/bytes/NUL SHA256."""
    package = _contained(cfg.project_root, cfg.source_dir / "src" / "sdbase")
    sources = sorted(package.glob("*.py"))
    if not sources:
        raise DatabaseError("SDBASE kaynak dosyaları bulunamadı.")
    digest = hashlib.sha256()
    for source in sources:
        source = _contained(cfg.project_root, source, reject_link=True)
        digest.update(source.name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(_read_bytes(source, 16 * 1024 * 1024))
        digest.update(b"\0")
    return digest.hexdigest()


def backend_import(cfg: LocalConfig):
    """Import this configured source tree under an isolated package name."""
    # Runtime administration must not populate the imported source tree with
    # generated bytecode, including modules the service imports later.
    sys.dont_write_bytecode = True
    src = _contained(cfg.project_root, cfg.source_dir / "src")
    if not src.is_dir():
        raise DatabaseError("SDBASE kaynak klasörü bulunamadı.")
    for name in ("__init__.py", "schema.py", "identity.py", "storage.py"):
        _regular(_contained(cfg.project_root, src / "sdbase" / name, reject_link=True))
    package_name = "_radios_sdbase_" + hashlib.sha256(os.fsencode(src)).hexdigest()[:16]
    if str(src) not in sys.path:
        sys.path.insert(0, str(src))
    try:
        if package_name not in sys.modules:
            spec = importlib.util.spec_from_file_location(
                package_name, src / "sdbase/__init__.py",
                submodule_search_locations=[str(src / "sdbase")])
            if spec is None or spec.loader is None:
                raise DatabaseError("SDBASE paket yükleyicisi bulunamadı.")
            package = importlib.util.module_from_spec(spec)
            sys.modules[package_name] = package
            try:
                spec.loader.exec_module(package)
            except Exception:
                sys.modules.pop(package_name, None)
                raise
        schema = importlib.import_module(package_name + ".schema")
        identity = importlib.import_module(package_name + ".identity")
        storage = importlib.import_module(package_name + ".storage")
        package = sys.modules[package_name]
    except (ImportError, OSError, SyntaxError) as exc:
        raise DatabaseError("SDBASE kaynakları yüklenemedi.") from exc
    return SimpleNamespace(package_name=package_name, schema=schema, identity=identity,
                           storage=storage, open_radio_store=schema.open_radio_store,
                           bootstrap=identity.bootstrap, authenticate=identity.authenticate,
                           require=identity.require, issue_endpoint_token=identity.issue_endpoint_token,
                           SCHEMA_VERSION=schema.SCHEMA_VERSION, StoreError=storage.StoreError,
                           __version__=package.__version__)


def _directory(cfg: LocalConfig, path: Path, *, private: bool = False):
    actual = _contained(cfg.project_root, path)
    try:
        actual.mkdir(parents=True, exist_ok=True, mode=0o700 if private else 0o755)
        if not actual.is_dir():
            raise DatabaseError("Çalışma yolu bir klasör olmalı.")
        if private:
            if actual.stat().st_uid != os.getuid():
                raise DatabaseError("Özel çalışma klasörünün sahibi geçersiz.")
            actual.chmod(0o700)
    except OSError as exc:
        raise DatabaseError("Yerel çalışma klasörü hazırlanamadı.") from exc


@contextmanager
def operation_lock(cfg: LocalConfig):
    """Bounded advisory lock shared by RadioS init/start/status/stop commands."""
    _directory(cfg, cfg.runtime_dir, private=True)
    lock_path = _contained(cfg.project_root, cfg.runtime_dir / "operation.lock", reject_link=True)
    try:
        fd = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    except OSError as exc:
        raise DatabaseError("Veritabanı işlem kilidi açılamadı.") from exc
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
            raise DatabaseError("Veritabanı işlem kilidi geçersiz.")
        os.fchmod(fd, 0o600)
        deadline = time.monotonic() + cfg.request_timeout_seconds
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise DatabaseError("Başka bir veritabanı işleminin bitmesi beklenirken süre doldu.")
                time.sleep(0.05)
        yield
    finally:
        os.close(fd)


def _sync_dir(path: Path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _json_write(path: Path, value: dict):
    body = (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(body)
        stream.flush()
        os.fsync(stream.fileno())
    _sync_dir(path.parent)


def _uuid(value, label: str):
    if not isinstance(value, str):
        raise DatabaseError(f"Kimlik alanı geçersiz: {label}.")
    try:
        if str(UUID(value)) != value:
            raise ValueError
    except (ValueError, AttributeError) as exc:
        raise DatabaseError(f"Kimlik alanı geçersiz: {label}.") from exc


def _instant(value, label: str):
    if not isinstance(value, str):
        raise DatabaseError(f"Kimlik zamanı geçersiz: {label}.")
    try:
        parsed = datetime.fromisoformat(value)
        if parsed.tzinfo is None or parsed.utcoffset() is None:
            raise ValueError
    except ValueError as exc:
        raise DatabaseError(f"Kimlik zamanı geçersiz: {label}.") from exc


def _store(cfg: LocalConfig, backend, database_file: Path | None = None):
    path = database_file or cfg.database_file
    _contained(cfg.project_root, path, reject_link=True)
    for suffix in ("", ".meta", ".lock", ".checkpoint"):
        sidecar = path.with_name(path.name + suffix)
        _contained(cfg.project_root, sidecar, reject_link=True)
        _regular(sidecar, optional=suffix == ".checkpoint")
    try:
        return backend.open_radio_store(path, read_only=True,
                                        lock_timeout=cfg.connect_timeout_seconds)
    except (backend.StoreError, OSError, ValueError) as exc:
        raise DatabaseError("SDBASE veritabanı açılamadı; mevcut dosyalar korunuyor.") from exc


def _credentials_validate(cfg: LocalConfig, value: dict, backend, store, authenticate_tokens: bool):
    if type(value.get("schema_version")) is not int or value["schema_version"] != 1:
        raise DatabaseError("Kimlik dosyasının şeması desteklenmiyor.")
    for label in ("store_id", "user_id", "station_id", "endpoint_id"):
        _uuid(value.get(label), label)
    for label in ("created_at", "api_token_expires_at", "endpoint_token_expires_at"):
        _instant(value.get(label), label)
    for label in ("username", "station_code", "station_name", "timezone"):
        field = value.get(label)
        if not isinstance(field, str) or not field.strip() or len(field) > 256 or "\x00" in field:
            raise DatabaseError("Kimlik dosyasındaki açıklayıcı alan geçersiz.")
    if value.get("database_file") != str(cfg.database_file.relative_to(cfg.project_root)):
        raise DatabaseError("Kimlik dosyasının veritabanı yolu eşleşmiyor.")
    for label in ("api_token", "endpoint_token"):
        field = value.get(label)
        if not isinstance(field, str) or not re.fullmatch(r"[A-Za-z0-9_-]{20,256}", field):
            raise DatabaseError("Kimlik dosyasındaki erişim anahtarı geçersiz.")
    if value["api_token"] == value["endpoint_token"]:
        raise DatabaseError("Kullanıcı ve uç erişim anahtarları ayrı olmalı.")
    if store._meta().get("store_id") != value["store_id"]:
        raise DatabaseError("Kimlik dosyası bu veritabanına ait değil.")
    user = store.get("users", value["user_id"])
    station = store.get("stations", value["station_id"])
    endpoint = store.get("endpoints", value["endpoint_id"])
    if (not user or not station or not endpoint or user.get("username") != value["username"] or
            station.get("code") != value["station_code"] or
            station.get("name") != value["station_name"] or
            station.get("timezone") != value["timezone"] or
            endpoint.get("station_id") != value["station_id"]):
        raise DatabaseError("Veritabanındaki kullanıcı, istasyon ve uç kimliği eşleşmiyor.")
    for label, scope in (("api_token", "user"), ("endpoint_token", "endpoint")):
        digest = hashlib.sha256(value[label].encode("ascii")).hexdigest()
        matches = store.lookup("tokens", "hash", digest)
        if (len(matches) != 1 or matches[0].get("user_id") != value["user_id"] or
                matches[0].get("scope") != scope or
                matches[0].get("expires_at") != value[label + "_expires_at"] or
                (scope == "endpoint" and matches[0].get("endpoint_id") != value["endpoint_id"])):
            raise DatabaseError("Erişim anahtarının kimliği, kapsamı veya süre bilgisi eşleşmiyor.")
    if authenticate_tokens:
        try:
            admin = backend.authenticate(store, value["api_token"])
            agent = backend.authenticate(store, value["endpoint_token"])
            backend.require(store, admin, "manage", value["station_id"])
            backend.require(store, agent, "operate", value["station_id"], endpoint_id=value["endpoint_id"])
        except (backend.StoreError, ValueError) as exc:
            raise DatabaseError("Kimlik anahtarlarının süresi dolmuş, iptal edilmiş veya yetkisi geçersiz.") from exc
        if (admin.scope != "user" or admin.user_id != value["user_id"] or
                agent.scope != "endpoint" or agent.endpoint_id != value["endpoint_id"]):
            raise DatabaseError("Erişim anahtarlarının kapsamı geçersiz.")
    return value


def load_credentials(cfg: LocalConfig, *, authenticate_tokens: bool = True) -> dict:
    backend = backend_import(cfg)
    value = _json_read(_contained(cfg.project_root, cfg.credentials_file, reject_link=True))
    return _credentials_validate(cfg, value, backend, _store(cfg, backend), authenticate_tokens)


def _digest(path: Path) -> str:
    _regular(path)
    value = hashlib.sha256()
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        while block := stream.read(1024 * 1024):
            value.update(block)
    return value.hexdigest()


def _publish(source: Path, target: Path, digest: str):
    """Publish without replacement, including across filesystems."""
    if target.exists() or target.is_symlink():
        _regular(target, private=True)
        if _digest(target) != digest:
            raise DatabaseError("Bekleyen kurulumun hedef dosyası farklı; hiçbir dosyanın üzerine yazılmadı.")
        return
    fd, temporary_name = tempfile.mkstemp(prefix=".radios-setup-", dir=target.parent)
    temporary = Path(temporary_name)
    try:
        source_fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(source_fd, "rb") as incoming, os.fdopen(fd, "wb") as outgoing:
            shutil.copyfileobj(incoming, outgoing, length=1024 * 1024)
            outgoing.flush()
            os.fsync(outgoing.fileno())
        if _digest(temporary) != digest:
            raise DatabaseError("Bekleyen kurulum dosyasının içeriği değişmiş.")
        os.link(temporary, target, follow_symlinks=False)
        _sync_dir(target.parent)
    finally:
        temporary.unlink(missing_ok=True)


def _summary(cfg: LocalConfig, value: dict, status: str, backend) -> dict:
    result = {key: value[key] for key in ("store_id", "user_id", "station_id", "endpoint_id",
              "station_code", "station_name", "username", "api_token_expires_at", "endpoint_token_expires_at")}
    store = _store(cfg, backend)
    result.update(status=status, database_file=str(cfg.database_file),
                  credentials_file=str(cfg.credentials_file), schema_version=backend.SCHEMA_VERSION,
                  endpoint_status=store.get("endpoints", value["endpoint_id"])["status"])
    return result


def _finish_pending(cfg: LocalConfig, backend) -> dict:
    pending_file = cfg.runtime_dir / "setup-pending.json"
    pending = _json_read(pending_file)
    expected_database = str(cfg.database_file.relative_to(cfg.project_root))
    stage_name = pending.get("stage_name")
    if (type(pending.get("schema_version")) is not int or pending["schema_version"] != 1 or
            pending.get("database_file") != expected_database or
            not isinstance(stage_name, str) or not re.fullmatch(r"setup-[0-9a-f]{32}", stage_name)):
        raise DatabaseError("Bekleyen kurulum günlüğü geçersiz; özel kimlik dosyaları korunuyor.")
    stage = _contained(cfg.project_root, cfg.runtime_dir / stage_name, reject_link=True)
    if not stage.is_dir() or stat.S_IMODE(stage.stat().st_mode) != 0o700:
        raise DatabaseError("Bekleyen kurulumun özel hazırlık klasörü geçersiz.")
    staged_db = stage / cfg.database_file.name
    credentials = _json_read(stage / "credentials.json")
    _credentials_validate(cfg, credentials, backend, _store(cfg, backend, staged_db), True)
    files = pending.get("files")
    allowed = {cfg.database_file.name + suffix for suffix in ("", ".meta", ".lock", ".checkpoint")}
    if (not isinstance(files, dict) or set(files) != allowed | {"credentials.json"} or
            pending.get("store_id") != credentials["store_id"]):
        raise DatabaseError("Bekleyen kurulumun dosya veya veritabanı kimliği geçersiz.")
    for name, digest in files.items():
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise DatabaseError("Bekleyen kurulumun doğrulama özeti geçersiz.")
        source = _contained(cfg.project_root, stage / name, reject_link=True)
        _regular(source, private=True)
        if _digest(source) != digest:
            raise DatabaseError("Bekleyen kurulum dosyaları değişmiş; kimlikler korunuyor.")
    _directory(cfg, cfg.database_file.parent)
    _directory(cfg, cfg.credentials_file.parent)
    for suffix in (".lock", ".meta", ".checkpoint", ""):
        name = cfg.database_file.name + suffix
        _publish(stage / name, cfg.database_file.with_name(name), files[name])
    _publish(stage / "credentials.json", cfg.credentials_file, files["credentials.json"])
    value = load_credentials(cfg)
    # Remove the marker only after the complete live identity validates. A crash
    # after this unlink leaves a harmless private staging directory, not a DB
    # whose administrator's original plaintext credential was discarded.
    pending_file.unlink()
    _sync_dir(cfg.runtime_dir)
    try:
        shutil.rmtree(stage)
        _sync_dir(cfg.runtime_dir)
    except OSError:
        # The validated live installation is already committed. An unremoved
        # private preparation directory must not turn success into a failure.
        pass
    return _summary(cfg, value, "initialized", backend)


def initialize(cfg: LocalConfig, *, username: str = "admin", station_code: str = "RADIOS",
               station_name: str = "RadioS", timezone_name: str = "UTC") -> dict:
    """Initialize once or verify a previously managed installation.

    Existing stores without this tool's credential file and pending manifest
    are never adopted or overwritten, even if they contain no users yet.
    """
    pending_restore = cfg.runtime_dir / "restore-pending.json"
    if pending_restore.exists() or pending_restore.is_symlink():
        raise DatabaseError("Kesilmiş geri yükleme var; önce offline recover komutunu çalıştırın.")
    backend = backend_import(cfg)
    _directory(cfg, cfg.runtime_dir, private=True)
    pending_file = cfg.runtime_dir / "setup-pending.json"
    if pending_file.exists() or pending_file.is_symlink():
        return _finish_pending(cfg, backend)
    if cfg.credentials_file.exists() or cfg.credentials_file.is_symlink():
        value = load_credentials(cfg)
        return _summary(cfg, value, "already_initialized", backend)
    for suffix in ("", ".meta", ".lock", ".checkpoint"):
        path = cfg.database_file.with_name(cfg.database_file.name + suffix)
        if path.exists() or path.is_symlink():
            raise DatabaseError("Mevcut veritabanı RadioS tarafından yönetilmiyor; üzerine yazılmadı.")
    for value in (username, station_code, station_name, timezone_name):
        if not isinstance(value, str) or not value.strip() or len(value) > 256 or "\x00" in value:
            raise DatabaseError("İlk kullanıcı ve istasyon alanları boş veya geçersiz.")
    try:
        ZoneInfo(timezone_name)
    except (ZoneInfoNotFoundError, ValueError) as exc:
        raise DatabaseError("İstasyon saat dilimi geçersiz.") from exc
    stage = cfg.runtime_dir / ("setup-" + uuid4().hex)
    stage.mkdir(mode=0o700)
    try:
        staged_db = stage / cfg.database_file.name
        store = backend.open_radio_store(staged_db, lock_timeout=cfg.connect_timeout_seconds)
        bootstrap = backend.bootstrap(store, username, station_code, station_name, timezone_name)
        admin = backend.authenticate(store, bootstrap["api_token"])
        endpoint_token = backend.issue_endpoint_token(
            store, admin, bootstrap["station_id"], bootstrap["endpoint_id"], days=30)
        credentials = {
            "schema_version": 1, "store_id": store._meta()["store_id"],
            "user_id": bootstrap["user_id"], "station_id": bootstrap["station_id"],
            "endpoint_id": bootstrap["endpoint_id"], "api_token": bootstrap["api_token"],
            "endpoint_token": endpoint_token, "username": username, "station_code": station_code,
            "station_name": station_name, "timezone": timezone_name,
            "database_file": str(cfg.database_file.relative_to(cfg.project_root)),
            "created_at": datetime.now(timezone.utc).isoformat(),
        }
        for label in ("api_token", "endpoint_token"):
            digest = hashlib.sha256(credentials[label].encode("ascii")).hexdigest()
            credentials[label + "_expires_at"] = store.lookup("tokens", "hash", digest)[0]["expires_at"]
        store.checkpoint()
        for path in stage.iterdir():
            _regular(path)
            path.chmod(0o600)
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        _json_write(stage / "credentials.json", credentials)
        _credentials_validate(cfg, credentials, backend, _store(cfg, backend, staged_db), True)
        _json_write(pending_file, {
            "schema_version": 1, "stage_name": stage.name, "store_id": credentials["store_id"],
            "database_file": credentials["database_file"],
            "files": {path.name: _digest(path) for path in stage.iterdir()},
        })
        return _finish_pending(cfg, backend)
    except (backend.StoreError, DatabaseError, OSError, ValueError) as exc:
        # A pending installation is preserved with its plaintext credentials.
        # Before publication, an incomplete preparation can be discarded.
        if not pending_file.exists():
            shutil.rmtree(stage, ignore_errors=True)
        if isinstance(exc, DatabaseError):
            raise
        raise DatabaseError("Veritabanı kurulumu tamamlanamadı; mevcut veriler korunuyor.") from exc
