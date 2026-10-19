#!/usr/bin/env python3
"""Private RadioS installation snapshots and explicitly offline restoration.

The SDBASE engine owns snapshot consistency and operational restoration fences.
This wrapper binds its archive to the installation's credentials and settings.
Directory archives are deliberately used: no tar/zip extraction is performed.
Detached hashes detect damage, not a malicious archive author or authenticity.
"""
from __future__ import annotations

import argparse
import configparser
from contextlib import contextmanager
from dataclasses import replace
from datetime import datetime, timezone
import fcntl
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import tempfile
import time
from uuid import UUID, uuid4

import radios_database as service
import radios_database_setup as setup


FORMAT = "radios-installation-backup-v1"
NAME = re.compile(r"^radios-[0-9]{8}T[0-9]{6}Z-[0-9a-f]{8}$")
JOURNAL = "restore-pending.json"
IDENTITIES = ("store_id", "user_id", "station_id", "endpoint_id")


def _modules(cfg):
    backend = setup.backend_import(cfg)
    return backend, importlib.import_module(backend.package_name + ".backup")


def _safe(root: Path, path: Path, *, missing=False) -> Path:
    """Check every lexical component before resolve; never accept links."""
    path = Path(path)
    if not path.is_absolute():
        path = root / path
    try:
        relative = path.relative_to(root)
    except ValueError as exc:
        raise setup.DatabaseError("Yol RadioS klasörünün dışında.") from exc
    current = root
    for part in relative.parts:
        if part in {"", ".", ".."}:
            raise setup.DatabaseError("Güvensiz yerel yol.")
        current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            if missing:
                continue
            raise setup.DatabaseError("Gerekli yerel dosya veya klasör bulunamadı.") from None
        if stat.S_ISLNK(info.st_mode):
            raise setup.DatabaseError("Yedek veya geri yükleme yolu bağlantı içeremez.")
    return path


def _directory(cfg, path):
    _safe(cfg.project_root, path, missing=True)
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not path.is_dir() or path.stat().st_uid != os.getuid():
        raise setup.DatabaseError("Özel çalışma klasörünün sahibi veya türü geçersiz.")
    path.chmod(0o700)


def _write_json(path, value):
    service._write_json(path, value)


def _pending(cfg):
    path = cfg.runtime_dir / JOURNAL
    if path.exists() or path.is_symlink():
        raise setup.DatabaseError("Kesilmiş geri yükleme var; önce offline recover komutunu çalıştırın.")


def _archive(cfg, name):
    if not isinstance(name, str) or not NAME.fullmatch(name):
        raise setup.DatabaseError("Yedek adı geçersiz; list komutundaki adı kullanın.")
    return _safe(cfg.project_root, cfg.backup_root / name)


def _read_manifest(root):
    encoded = setup._read_bytes(root / "manifest.json", 1024 * 1024, private=True)
    expected = setup._read_bytes(root / "manifest.sha256", 65, private=True).decode("ascii").strip()
    if not re.fullmatch(r"[0-9a-f]{64}", expected) or hashlib.sha256(encoded).hexdigest() != expected:
        raise setup.DatabaseError("Yedek manifest özeti uyuşmuyor.")
    value = setup._json_read(root / "manifest.json")
    if value.get("format") != FORMAT:
        raise setup.DatabaseError("RadioS yedek biçimi geçersiz.")
    return value


def _write_manifest(root, value):
    encoded = json.dumps(value, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":"), allow_nan=False).encode()
    for name, body in (("manifest.json", encoded),
                       ("manifest.sha256", (hashlib.sha256(encoded).hexdigest() + "\n").encode())):
        with (root / name).open("xb") as output:
            os.chmod(root / name, 0o600)
            output.write(body)
            output.flush()
            os.fsync(output.fileno())


def _configuration_matches(cfg, config_file):
    archived = setup.load_config(cfg.project_root, str(config_file))
    for key in ("database_file", "source_dir", "backup_root", "runtime_dir", "import_root", "service_url"):
        if getattr(archived, key) != getattr(cfg, key):
            raise setup.DatabaseError("Yedek yapılandırmasının kurulum yolları veya servis adresi farklı.")


def _verify(cfg, root, *, bind=True):
    backend, backup = _modules(cfg)
    _safe(cfg.project_root, root)
    if not root.is_dir():
        raise setup.DatabaseError("Yedek bir klasör olmalı.")
    manifest = _read_manifest(root)
    expected_keys = {"format", "created_at", "identity", "database_file", "config_file",
                     "settings", "sequence", "media_objects", "size_bytes", "engine_files"}
    if set(manifest) != expected_keys:
        raise setup.DatabaseError("Yedek manifest alanları geçersiz.")
    setup._instant(manifest["created_at"], "created_at")
    for key in ("sequence", "media_objects", "engine_files", "size_bytes"):
        if type(manifest[key]) is not int or manifest[key] < 0:
            raise setup.DatabaseError("Yedek sayısal envanteri geçersiz.")
    identity = manifest.get("identity")
    if not isinstance(identity, dict) or set(identity) != set(IDENTITIES):
        raise setup.DatabaseError("Yedek kurulum kimliği geçersiz.")
    for key in IDENTITIES:
        setup._uuid(identity[key], key)
    if (manifest["database_file"] != cfg.database_file.relative_to(cfg.project_root).as_posix() or
            manifest["config_file"] != cfg.config_file.relative_to(cfg.project_root).as_posix()):
        raise setup.DatabaseError("Yedek bu kurulumun dosya yollarına ait değil.")
    settings = manifest["settings"]
    allowed = {"credentials.json", "application.ini", "ui-preferences.ini"}
    if not isinstance(settings, dict) or not {"credentials.json", "application.ini"} <= set(settings) <= allowed:
        raise setup.DatabaseError("Yedek ayar listesi geçersiz.")
    for name, entry in settings.items():
        if not isinstance(entry, dict) or set(entry) != {"sha256", "size_bytes"}:
            raise setup.DatabaseError("Yedek ayar özeti geçersiz.")
        digest, size = backup._digest_file(_safe(root, root / "settings" / name))
        setup._regular(root / "settings" / name, private=True)
        if digest != entry["sha256"] or size != entry["size_bytes"]:
            raise setup.DatabaseError("Yedek ayar dosyası bozulmuş.")
    verified = backup.verify_backup(root / "database")
    engine_files, engine_dirs = backup._inventory(root / "database")
    files, directories = backup._inventory(root)
    expected_files = {"manifest.json", "manifest.sha256"} | {"settings/" + name for name in settings}
    expected_files |= {"database/" + name for name in engine_files}
    expected_dirs = {"settings", "database"} | {"database/" + name for name in engine_dirs}
    if files != expected_files or directories != expected_dirs:
        raise setup.DatabaseError("Yedekte listelenmeyen veya eksik dosya var.")
    if any(verified[key] != manifest[key] for key in ("sequence", "media_objects")) or \
            verified["store_id"] != identity["store_id"] or verified["files"] != manifest["engine_files"]:
        raise setup.DatabaseError("Yedek veritabanı kimliği veya envanteri uyuşmuyor.")
    size = sum((root / path).stat().st_size for path in files if path not in {"manifest.json", "manifest.sha256"})
    if type(manifest["size_bytes"]) is not int or size != manifest["size_bytes"]:
        raise setup.DatabaseError("Yedek toplam boyutu uyuşmuyor.")
    _configuration_matches(cfg, root / "settings/application.ini")
    credentials = setup._json_read(root / "settings/credentials.json")
    # Engine verification used a private scratch copy. Credential validation
    # needs another lock-bearing scratch copy, never a lock added to the archive.
    with tempfile.TemporaryDirectory(prefix="verify-", dir=cfg.runtime_dir) as scratch_name:
        scratch = Path(scratch_name)
        engine_manifest = backup._read_manifest(root / "database")
        dbname, engine_entries, _ = backup._validate_manifest(engine_manifest)
        for name, entry in engine_entries.items():
            backup._copy_file(root / "database" / name, scratch / name,
                              expected_sha=entry["sha256"], expected_size=entry["size_bytes"])
        (scratch / (dbname + ".lock")).touch(mode=0o600)
        store = backend.open_radio_store(scratch / dbname, read_only=True)
        setup._credentials_validate(cfg, credentials, backend, store, False)
        integrity = importlib.import_module(backend.package_name + ".integrity").inspect_integrity(
            store, max_records=10000000, max_bytes=min(8 * 1024**4, max(1024**3, size * 2 + 64 * 1024**2)))
        if integrity["status"] != "ok" or not integrity["complete"]:
            raise setup.DatabaseError("Yedek şema, ilişki veya medya bütünlüğü doğrulanamadı.")
    if any(credentials[key] != identity[key] for key in IDENTITIES):
        raise setup.DatabaseError("Yedek kimlik dosyası farklı kurulumdan.")
    if bind:
        live = setup._json_read(cfg.credentials_file)
        if any(live.get(key) != identity[key] for key in IDENTITIES):
            raise setup.DatabaseError("Yedek başka RadioS kurulumuna ait; mevcut kurulum korunuyor.")
    return {"name": root.name, "created_at": manifest["created_at"], **identity,
            "sequence": verified["sequence"], "media_objects": verified["media_objects"],
            "size_bytes": size, "engine_files": verified["files"], "integrity": "ok"}


def _require_space(cfg, folder, needed, what):
    """Copies must leave the disk usable: the live service journals on it."""
    backend, _ = _modules(cfg)
    reset = importlib.import_module(backend.package_name + ".desktop_reset")
    try:
        reset.require_space([(folder, needed)], what)
    except RuntimeError as exc:
        raise setup.DatabaseError(str(exc)) from None


def create_backup(cfg):
    _pending(cfg)
    _directory(cfg, cfg.backup_root)
    credentials = setup.load_credentials(cfg, authenticate_tokens=False)
    backend, backup = _modules(cfg)
    store = backend.open_radio_store(cfg.database_file, read_only=True)
    reset = importlib.import_module(backend.package_name + ".desktop_reset")
    _require_space(cfg, cfg.backup_root, reset._backup_bytes(store), "Yedek")
    name = "radios-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid4().hex[:8]
    temporary = Path(tempfile.mkdtemp(prefix=".radios-backup-", dir=cfg.backup_root))
    try:
        (temporary / "settings").mkdir(mode=0o700)
        settings = {}
        paths = {"credentials.json": cfg.credentials_file, "application.ini": cfg.config_file}
        preferences = cfg.project_root / "config/ui-preferences.ini"
        if preferences.exists() or preferences.is_symlink():
            paths["ui-preferences.ini"] = preferences
        for leaf, source in paths.items():
            settings[leaf] = backup._copy_file(_safe(cfg.project_root, source), temporary / "settings" / leaf)
        snapshot = backup.create_backup(store, temporary / "database", checkpoint=False)
        files, _ = backup._inventory(temporary)
        manifest = {"format": FORMAT, "created_at": datetime.now(timezone.utc).isoformat(),
                    "identity": {key: credentials[key] for key in IDENTITIES},
                    "database_file": cfg.database_file.relative_to(cfg.project_root).as_posix(),
                    "config_file": cfg.config_file.relative_to(cfg.project_root).as_posix(),
                    "settings": settings, "sequence": snapshot["sequence"],
                    "media_objects": snapshot["media_objects"], "engine_files": snapshot["files"],
                    "size_bytes": sum((temporary / path).stat().st_size for path in files)}
        _write_manifest(temporary, manifest)
        result = _verify(cfg, temporary)
        backup._fsync_tree(temporary)
        backup._publish_absent(temporary, cfg.backup_root / name)
        return {**result, "name": name, "path": str(cfg.backup_root / name), "status": "created"}
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


def list_backups(cfg):
    if not cfg.backup_root.exists():
        return {"status": "ok", "backups": []}
    _safe(cfg.project_root, cfg.backup_root)
    rows = []
    for path in sorted(cfg.backup_root.iterdir(), reverse=True):
        if not NAME.fullmatch(path.name):
            continue
        try:
            rows.append({**_verify(cfg, path), "status": "verified"})
        except (RuntimeError, OSError, ValueError):
            rows.append({"name": path.name, "status": "invalid", "error": "Yedek doğrulanamadı."})
    return {"status": "ok", "backups": rows}


def verify_backup(cfg, name):
    return {**_verify(cfg, _archive(cfg, name)), "status": "verified"}


def restore_plan(cfg, name):
    verified = verify_backup(cfg, name)
    command = [cfg.python_command, str(cfg.project_root / "tools/radios_reliability.py"),
               "--project-root", str(cfg.project_root), "--config", str(cfg.config_file),
               "restore", "--backup", name, "--confirm-store-id", verified["store_id"], "--offline"]
    return {**verified, "status": "planned", "offline_required": True, "activated": False,
            "command": command, "note": "Uygulama ve veri servisi kapatılmadan uygulanamaz; mevcut kurulum aynı kimlikle geri yüklenir."}


def _restore_stage(cfg, name, target):
    verified = verify_backup(cfg, name)
    root = _archive(cfg, name)
    _require_space(cfg, target.parent, int(_read_manifest(root).get("size_bytes") or 0), "Geri yükleme denemesi")
    backend, backup = _modules(cfg)
    result = backup.restore_backup(root / "database", target)
    for leaf in _read_manifest(root)["settings"]:
        backup._copy_file(root / "settings" / leaf, target / "settings" / leaf)
    store = backend.open_radio_store(target / cfg.database_file.name)
    credentials = setup._json_read(target / "settings/credentials.json")
    setup._credentials_validate(cfg, credentials, backend, store, False)
    # Engine restoration revokes every old endpoint lease. Renew only this
    # installation's local endpoint credential; remote endpoints stay fenced.
    try:
        admin = backend.authenticate(store, credentials["api_token"])
        endpoint_token = backend.issue_endpoint_token(store, admin, credentials["station_id"],
                                                     credentials["endpoint_id"], days=30)
    except backend.StoreError as exc:
        raise setup.DatabaseError("Yedekteki yönetici anahtarı artık geçerli değil; bu kopya etkinleştirilemez.") from exc
    credentials["endpoint_token"] = endpoint_token
    digest = hashlib.sha256(endpoint_token.encode()).hexdigest()
    credentials["endpoint_token_expires_at"] = store.lookup("tokens", "hash", digest)[0]["expires_at"]
    _write_json(target / "settings/credentials.json", credentials)
    setup._credentials_validate(cfg, credentials, backend, store, True)
    admin_module = importlib.import_module(backend.package_name + ".desktop_admin")
    recovery = admin_module.fence_restored_runtime(store, actor_user_id=credentials["user_id"])
    restored_sequence = store.checkpoint()
    integrity = importlib.import_module(backend.package_name + ".integrity").inspect_integrity(store)
    if integrity["status"] != "ok" or not integrity["complete"]:
        raise setup.DatabaseError("Geri yükleme kopyasının bütünlük kontrolü tamamlanmadı.")
    backup._fsync_tree(target)
    return {**verified, "status": "staged", "restored_path": str(target),
            "source_sequence": result["source_sequence"], "restored_sequence": restored_sequence,
            "activated": False, "offline_required": True, "integrity": "ok", "endpoint_token_rotated": True,
            "closed_runtime_runs": recovery["closed_runs"], "fenced_runtime_runs": recovery["fenced_runs"],
            "outbox_included": False}


def restore_stage(cfg, name):
    _pending(cfg)
    target = cfg.runtime_dir / ("restore-preview-" + uuid4().hex)
    try:
        return _restore_stage(cfg, name, target)
    except BaseException:
        if target.exists() and not target.is_symlink():
            shutil.rmtree(target)
        raise


def _gui_open(cfg):
    for process in Path("/proc").iterdir():
        if not process.name.isdigit() or int(process.name) == os.getpid():
            continue
        try:
            executable = os.readlink(process / "exe").removesuffix(" (deleted)")
            if Path(executable).name != "radios" or not Path(executable).is_relative_to(cfg.project_root):
                continue
            args = (process / "cmdline").read_bytes().decode().rstrip("\0").split("\0")
            if str(cfg.project_root) in args or Path(os.readlink(process / "cwd")) == cfg.project_root:
                return True
        except (OSError, UnicodeError):
            continue
    return False


@contextmanager
def _offline_locks(cfg, confirm, offline, *, recovering=False):
    if not offline or confirm is None:
        raise setup.DatabaseError("Geri yükleme --offline ve --confirm-store-id gerektirir.")
    setup._uuid(confirm, "confirm-store-id")
    if cfg.credentials_file.exists() or cfg.credentials_file.is_symlink():
        credentials = setup._json_read(cfg.credentials_file)
    elif recovering:
        journal, stage = _read_journal(cfg, confirm)
        candidate = stage / "rollback/credentials"
        if not candidate.exists():
            candidate = stage / "prepared/settings/credentials.json"
        credentials = setup._json_read(_safe(cfg.project_root, candidate))
    else:
        raise setup.DatabaseError("Kurulum kimlik dosyası bulunamadı.")
    if confirm != credentials.get("store_id"):
        raise setup.DatabaseError("Yazılan depo kimliği mevcut kurulumla uyuşmuyor.")
    state = service._read_json(service._state_path(cfg))
    if state and service._process_identity(state.get("pid")) is not None:
        raise setup.DatabaseError("Servis durumundaki süreç açık; yabancı sürece müdahale edilmedi.")
    if service._occupied(cfg) or _gui_open(cfg):
        raise setup.DatabaseError("Uygulama ve veri servisi tamamen kapatılmalı; port kullanımda olamaz.")
    descriptors = []
    try:
        for path in (cfg.runtime_dir / "server.lock", cfg.runtime_dir / "gui.lock",
                     cfg.database_file.with_name(cfg.database_file.name + ".lock")):
            _safe(cfg.project_root, path, missing=True)
            fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
            descriptors.append(fd)
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
                raise setup.DatabaseError("Geri yükleme kilidinin sahibi veya türü geçersiz.")
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise setup.DatabaseError("Servis veya veritabanı kilidi kullanımda; geri yükleme uygulanmadı.") from exc
        outbox = cfg.project_root / "data/recovery" / confirm
        if outbox.exists() or outbox.is_symlink():
            _safe(cfg.project_root, outbox)
            if not outbox.is_dir():
                raise setup.DatabaseError("Dayanıklı işlem kuyruğu klasör olmalı.")
            for user_dir in sorted(outbox.iterdir()):
                setup._uuid(user_dir.name, "outbox-user-id")
                _safe(cfg.project_root, user_dir)
                if not user_dir.is_dir() or user_dir.stat().st_uid != os.getuid() or user_dir.stat().st_mode & 0o077:
                    raise setup.DatabaseError("Dayanıklı işlem kuyruğu özel kullanıcı klasörü olmalı.")
                fd = os.open(user_dir / ".lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
                descriptors.append(fd)
                info = os.fstat(fd)
                if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
                    raise setup.DatabaseError("Dayanıklı işlem kuyruğu kilidi geçersiz.")
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError as exc:
                    raise setup.DatabaseError("Dayanıklı işlem kuyruğu kullanımda; geri yükleme uygulanmadı.") from exc
        if service._occupied(cfg) or _gui_open(cfg):
            raise setup.DatabaseError("Geri yükleme öncesinde uygulama veya port yeniden açıldı.")
        yield credentials
    finally:
        for fd in descriptors:
            os.close(fd)


def _fingerprint(path, backup):
    if not path.exists() and not path.is_symlink():
        return None
    if path.is_symlink():
        raise setup.DatabaseError("Geri yükleme hedefi bağlantı olamaz.")
    if path.is_file():
        digest, size = backup._digest_file(path)
        return {"kind": "file", "sha256": digest, "size_bytes": size}
    if not path.is_dir():
        raise setup.DatabaseError("Geri yükleme hedefinin türü geçersiz.")
    files, directories = backup._inventory(path)
    rows = {name: backup._digest_file(path / name) for name in sorted(files)}
    encoded = json.dumps({"files": rows, "directories": sorted(directories)}, sort_keys=True).encode()
    return {"kind": "directory", "sha256": hashlib.sha256(encoded).hexdigest(), "files": len(files)}


def _targets(cfg, store_id=None):
    store_id = store_id or setup._json_read(cfg.credentials_file)["store_id"]
    setup._uuid(store_id, "store_id")
    return {**{suffix or "journal": cfg.database_file.with_name(cfg.database_file.name + suffix)
               for suffix in ("", ".meta", ".checkpoint", ".objects", ".archives")},
            "credentials": cfg.credentials_file, "config": cfg.config_file,
            "preferences": cfg.project_root / "config/ui-preferences.ini",
            "outbox": cfg.project_root / "data/recovery" / store_id}


def _move(source, target):
    os.replace(source, target)
    setup._sync_dir(source.parent)
    if source.parent != target.parent:
        setup._sync_dir(target.parent)


def _read_journal(cfg, confirm):
    journal = setup._json_read(cfg.runtime_dir / JOURNAL)
    if (journal.get("format") != "radios-restore-v1" or journal.get("project_root") != str(cfg.project_root) or
            journal.get("database_file") != str(cfg.database_file) or journal.get("store_id") != confirm or
            journal.get("state") not in {"applying", "committed"} or
            not isinstance(journal.get("stage"), str) or not re.fullmatch(r"restore-transaction-[0-9a-f]{32}", journal["stage"])):
        raise setup.DatabaseError("Kesilmiş geri yükleme manifest kimliği geçersiz.")
    targets = _targets(cfg, confirm)
    if not isinstance(journal.get("entries"), dict) or set(journal["entries"]) != set(targets):
        raise setup.DatabaseError("Geri yükleme hedef envanteri geçersiz.")
    stage = _safe(cfg.project_root, cfg.runtime_dir / journal["stage"])
    _, backup = _modules(cfg)
    backup._inventory(stage)  # reject links/special files in rollback too
    for key, entry in journal["entries"].items():
        if not isinstance(entry, dict) or set(entry) != {"old", "new", "target"} or entry["target"] != str(targets[key]):
            raise setup.DatabaseError("Geri yükleme hedefi değişmiş.")
        _safe(cfg.project_root, targets[key], missing=True)
    return journal, stage


def _recover_locked(cfg, confirm):
    _, backup = _modules(cfg)
    journal, stage = _read_journal(cfg, confirm)
    targets = _targets(cfg, confirm)
    # Validate ALL paths before modifying ANY target. An unrelated post-crash
    # edit is never silently removed, even when other components are recoverable.
    for key, entry in journal["entries"].items():
        actual = _fingerprint(targets[key], backup)
        old = _fingerprint(stage / "rollback" / key, backup)
        if journal["state"] == "committed":
            if actual != entry["new"]:
                raise setup.DatabaseError("Tamamlanmış geri yüklemenin hedefi sonradan değişmiş.")
        elif actual not in (None, entry["old"], entry["new"]) or (old is not None and old != entry["old"]):
            raise setup.DatabaseError("Kesilmiş geri yüklemeden sonra hedef değişmiş; dosyalar korunuyor.")
        elif entry["old"] is not None and old is None and actual != entry["old"]:
            raise setup.DatabaseError("Geri dönüş için özgün dosya eksik; dosyalar korunuyor.")
    if journal["state"] == "applying":
        (stage / "discard").mkdir(mode=0o700, exist_ok=True)
        for key, entry in journal["entries"].items():
            rollback = stage / "rollback" / key
            if rollback.exists():
                if targets[key].exists():
                    _move(targets[key], stage / "discard" / key)
                _move(rollback, targets[key])
            elif entry["old"] is None and targets[key].exists():
                _move(targets[key], stage / "discard" / key)
    quarantined = None
    if journal["state"] == "committed" and journal["entries"]["outbox"]["old"] is not None:
        quarantine = cfg.runtime_dir / ("restore-outbox-" + journal["stage"].removeprefix("restore-transaction-"))
        if (stage / "rollback/outbox").exists():
            if quarantine.exists() or quarantine.is_symlink():
                raise setup.DatabaseError("Kurtarma kuyruğu saklama hedefi kullanımda; özgün kuyruk korunuyor.")
            _move(stage / "rollback/outbox", quarantine)
        if _fingerprint(quarantine, backup) != journal["entries"]["outbox"]["old"]:
            raise setup.DatabaseError("Saklanan dayanıklı işlem kuyruğunun özeti değişmiş.")
        quarantined = str(quarantine)
    (cfg.runtime_dir / JOURNAL).unlink()
    setup._sync_dir(cfg.runtime_dir)
    shutil.rmtree(stage)
    return {"status": "recovered", "store_id": confirm, "outbox_quarantine": quarantined,
            "action": "committed" if journal["state"] == "committed" else "rolled_back"}


def recover(cfg, confirm, offline):
    with _offline_locks(cfg, confirm, offline, recovering=True):
        return _recover_locked(cfg, confirm)


def _load_config_for_recovery(project_root, config_path):
    """Recover even when interruption happened between the two config renames."""
    root = Path(project_root).resolve(strict=True)
    journal = setup._json_read(_safe(root, root / "data/database-service" / JOURNAL))
    if journal.get("project_root") != str(root) or not isinstance(journal.get("stage"), str) or \
            not re.fullmatch(r"restore-transaction-[0-9a-f]{32}", journal["stage"]):
        raise setup.DatabaseError("Geri yükleme yapılandırma kurtarma kimliği geçersiz.")
    try:
        target = Path(journal["entries"]["config"]["target"])
    except (KeyError, TypeError) as exc:
        raise setup.DatabaseError("Kurtarma yapılandırma hedefi eksik.") from exc
    _safe(root, target, missing=True)
    if config_path and _safe(root, Path(config_path), missing=True) != target:
        raise setup.DatabaseError("Kurtarma yapılandırma yolu farklı.")
    try:
        loaded = setup.load_config(root, config_path)
        if loaded.config_file == target:
            return loaded
    except setup.DatabaseError:
        pass
    # If a missing radios.ini caused load_config to fall back to the example,
    # that successful parse still describes the wrong restoration target.
    stage = _safe(root, root / "data/database-service" / journal["stage"])
    candidate = stage / "rollback/config"
    if not candidate.exists():
        candidate = stage / "prepared/settings/application.ini"
    loaded = setup.load_config(root, str(_safe(root, candidate)))
    return replace(loaded, config_file=target)


def restore(cfg, name, confirm, offline):
    _pending(cfg)
    verified = verify_backup(cfg, name)
    if verified["store_id"] != confirm:
        raise setup.DatabaseError("Yedek ve onaylanan depo kimliği uyuşmuyor.")
    # Expensive validation/copy runs before the exclusive live DB lock.
    stage = cfg.runtime_dir / ("restore-transaction-" + uuid4().hex)
    stage.mkdir(mode=0o700)
    try:
        staged_result = _restore_stage(cfg, name, stage / "prepared")
        (stage / "rollback").mkdir(mode=0o700)
        _, backup = _modules(cfg)
        targets = _targets(cfg, confirm)
        sources = {**{suffix or "journal": stage / "prepared" / (cfg.database_file.name + suffix)
                      for suffix in ("", ".meta", ".checkpoint", ".objects", ".archives")},
                   "credentials": stage / "prepared/settings/credentials.json",
                   "config": stage / "prepared/settings/application.ini",
                   "preferences": stage / "prepared/settings/ui-preferences.ini",
                   "outbox": stage / "absent-outbox"}
        with _offline_locks(cfg, confirm, offline):
            # Reading metadata needs no second flock while we hold LOCK_EX.
            meta = setup._json_read(cfg.database_file.with_name(cfg.database_file.name + ".meta"), private=False)
            if meta.get("store_id") != confirm:
                raise setup.DatabaseError("Canlı veritabanının depo kimliği uyuşmuyor.")
            entries = {}
            for key, target in targets.items():
                _safe(cfg.project_root, target, missing=True)
                entries[key] = {"target": str(target), "old": _fingerprint(target, backup),
                                "new": _fingerprint(sources[key], backup)}
            journal = {"format": "radios-restore-v1", "project_root": str(cfg.project_root),
                       "database_file": str(cfg.database_file), "store_id": confirm,
                       "stage": stage.name, "state": "applying", "entries": entries}
            backup._fsync_tree(stage)
            _write_json(cfg.runtime_dir / JOURNAL, journal)
            try:
                for key, target in targets.items():
                    if entries[key]["old"] is not None:
                        _move(target, stage / "rollback" / key)
                    if entries[key]["new"] is not None:
                        _move(sources[key], target)
                journal["state"] = "committed"
                _write_json(cfg.runtime_dir / JOURNAL, journal)
            except Exception:
                _recover_locked(cfg, confirm)
                raise
            recovered = _recover_locked(cfg, confirm)
        return {**verified, "status": "restored", "activated": True,
                "restored_sequence": staged_result["restored_sequence"],
                "closed_runtime_runs": staged_result["closed_runtime_runs"],
                "fenced_runtime_runs": staged_result["fenced_runtime_runs"], "endpoint_token_rotated": True,
                "outbox_quarantine": recovered["outbox_quarantine"], "outbox_included": False,
                "service_started": False, "note": "Servis ve uygulama otomatik başlatılmadı; yeni çalışma oturumu gerekir."}
    finally:
        # A killed/interrupted process leaves this stage and its durable journal
        # intact. The next recover rolls back instead of discarding identity.
        if stage.exists() and not (cfg.runtime_dir / JOURNAL).exists():
            shutil.rmtree(stage)


def _pending_imports(cfg, store_id, *, locked=False):
    """Fail closed if any current-store outbox cannot be inspected exactly."""
    root = cfg.project_root / "data/recovery" / store_id
    protected = set()
    if not root.exists() and not root.is_symlink():
        return protected
    _safe(cfg.project_root, root)
    _, backup = _modules(cfg)
    backup._inventory(root)
    for user in sorted(root.iterdir()):
        setup._uuid(user.name, "outbox-user-id")
        if not user.is_dir():
            raise setup.DatabaseError("Dayanıklı işlem kuyruğu kullanıcı klasörü geçersiz.")
        fd = os.open(user / ".lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
                raise setup.DatabaseError("Dayanıklı işlem kuyruğu kilidi geçersiz.")
            if not locked:
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError as exc:
                    raise setup.DatabaseError("Dayanıklı işlem kuyruğu kullanımda; medya envanteri ertelendi.") from exc
            entries = list(user.glob("*.json"))
            if len(entries) > 512:
                raise setup.DatabaseError("Dayanıklı işlem kuyruğu 512 işlem sınırını aşıyor; hiçbir medya taşınmadı.")
            def unique_pairs(pairs):
                result = {}
                for key, value in pairs:
                    if key in result:
                        raise setup.DatabaseError("Dayanıklı işlem kuyruğu yinelenen JSON alanı içeriyor.")
                    result[key] = value
                return result
            def invalid_constant(_value):
                raise setup.DatabaseError("Dayanıklı işlem kuyruğu geçersiz JSON sayısı içeriyor.")
            for path in entries:
                encoded = setup._read_bytes(path, 2 * 1024 * 1024, private=True)
                value = json.loads(encoded, object_pairs_hook=unique_pairs, parse_constant=invalid_constant)
                if (not isinstance(value, dict) or type(value.get("version")) is not int or value.get("version") != 1 or value.get("store_id") != store_id or
                        value.get("user_id") != user.name or value.get("state") not in {"pending", "blocked"} or
                        not isinstance(value.get("body"), dict) or value.get("method") != "POST"):
                    raise setup.DatabaseError("Dayanıklı işlem kuyruğu kimliği geçersiz; hiçbir medya taşınmadı.")
                body = value["body"]
                operation_id = body.get("idempotency_key") or body.get("operation_key")
                setup._uuid(operation_id, "outbox-operation-id")
                if path.name != operation_id + ".json":
                    raise setup.DatabaseError("Dayanıklı işlem kuyruğu dosyası ve işlem kimliği farklı.")
                if value.get("path") == "/v1/workflows/desktop-media/import":
                    leaf = body.get("import_path")
                    if not isinstance(leaf, str) or Path(leaf).name != leaf or "/" in leaf or "\\" in leaf:
                        raise setup.DatabaseError("Bekleyen import yolu geçersiz; hiçbir medya taşınmadı.")
                    protected.add(leaf)
        finally:
            os.close(fd)
    return protected


def _media_audit(cfg, minimum_age_hours=24, *, offline_snapshot=None):
    if type(minimum_age_hours) is not int or not 24 <= minimum_age_hours <= 87600:
        raise setup.DatabaseError("Medya saklama sınırı 24..87600 saat olmalı.")
    credentials = (setup.load_credentials(cfg, authenticate_tokens=False) if offline_snapshot is None
                   else setup._json_read(cfg.credentials_file))
    backend, backup = _modules(cfg)
    if offline_snapshot is None:
        store = backend.open_radio_store(cfg.database_file, read_only=True)
        with store._locked(exclusive=False):
            store._reload_locked()
            referenced = {row["data"].get("sha256") for row in store._state.get("media_files", {}).values()}
            sequence = store._sequence
    else:
        referenced, sequence = offline_snapshot
    pending = _pending_imports(cfg, credentials["store_id"], locked=offline_snapshot is not None)
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.read_string(setup._read_bytes(cfg.config_file, 256 * 1024).decode())
    preview_root = _safe(cfg.project_root, setup._contained(cfg.project_root, parser.get("paths", "cache_root")) / "transition-preview", missing=True)
    paths = []
    if cfg.import_root.exists() or cfg.import_root.is_symlink():
        _safe(cfg.project_root, cfg.import_root)
        for path in cfg.import_root.iterdir():
            if re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\.(wav|mp3|flac|ogg|oga|m4a|mp4|aac|audio)", path.name):
                paths.append((path, "import", path.name in pending))
    if preview_root.exists():
        for path in preview_root.iterdir():
            if re.fullmatch(r"transition-[A-Za-z0-9]{6}", path.name):
                paths.append((path, "cue_preview", False))
    objects = cfg.database_file.with_name(cfg.database_file.name + ".objects")
    if objects.exists() or objects.is_symlink():
        _safe(cfg.project_root, objects)
        for prefix in objects.iterdir():
            if re.fullmatch(r"[0-9a-f]{2}", prefix.name) and prefix.is_dir() and not prefix.is_symlink():
                for path in prefix.iterdir():
                    if re.fullmatch(r"[0-9a-f]{64}", path.name) and path.name[:2] == prefix.name:
                        paths.append((path, "managed_orphan", path.name in referenced))
    cutoff = time.time_ns() - minimum_age_hours * 3600 * 1000000000
    rows, protected = [], 0
    for path, kind, retained in sorted(paths):
        info = path.lstat()
        if (retained or not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                info.st_mode & 0o077 or max(info.st_mtime_ns, info.st_ctime_ns) > cutoff):
            protected += 1
            continue
        digest, size = backup._digest_file(path)
        if kind == "managed_orphan" and digest != path.name:
            protected += 1
            continue
        rows.append({"path": path.relative_to(cfg.project_root).as_posix(), "kind": kind,
                     "sha256": digest, "size_bytes": size, "device": info.st_dev, "inode": info.st_ino,
                     "mtime_ns": info.st_mtime_ns, "ctime_ns": info.st_ctime_ns})
    evidence = {"store_id": credentials["store_id"], "sequence": sequence, "minimum_age_hours": minimum_age_hours,
                "candidates": rows, "pending_imports": sorted(pending)}
    digest = hashlib.sha256(json.dumps(evidence, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return {"status": "audited", "audit_id": digest, "store_id": credentials["store_id"], "sequence": sequence,
            "minimum_age_hours": minimum_age_hours, "candidates": rows, "protected_files": protected,
            "size_bytes": sum(row["size_bytes"] for row in rows), "deletes_bytes": False}


def media_cleanup(cfg, audit_id, confirm, offline, minimum_age_hours=24):
    _pending(cfg)
    # Capture committed references BEFORE exclusive DB lock; then check exact
    # metadata/journal fingerprints under the lock rather than nested flock.
    setup.load_credentials(cfg, authenticate_tokens=False)
    backend, backup = _modules(cfg)
    store = backend.open_radio_store(cfg.database_file, read_only=True)
    with store._locked(exclusive=False):
        store._reload_locked()
        referenced = {row["data"].get("sha256") for row in store._state.get("media_files", {}).values()}
        sequence = store._sequence
        stamps = {suffix: _fingerprint(cfg.database_file.with_name(cfg.database_file.name + suffix), backup)
                  for suffix in ("", ".meta", ".checkpoint")}
    with _offline_locks(cfg, confirm, offline):
        if any(_fingerprint(cfg.database_file.with_name(cfg.database_file.name + suffix), backup) != digest
               for suffix, digest in stamps.items()):
            raise setup.DatabaseError("Medya envanteri hazırlanırken veritabanı değişti; hiçbir medya taşınmadı.")
        audit = _media_audit(cfg, minimum_age_hours, offline_snapshot=(referenced, sequence))
        if not isinstance(audit_id, str) or audit_id != audit["audit_id"]:
            raise setup.DatabaseError("İncelenen medya envanteri değişmiş; yeni media-audit gerekli.")
        target = cfg.runtime_dir / ("media-quarantine-" + uuid4().hex)
        target.mkdir(mode=0o700)
        setup._sync_dir(cfg.runtime_dir)
        journal = {**audit, "status": "moving", "created_at": datetime.now(timezone.utc).isoformat()}
        _write_json(target / "quarantine.json", journal)
        for index, row in enumerate(audit["candidates"]):
            path = _safe(cfg.project_root, cfg.project_root / row["path"])
            info = path.stat()
            digest, size = backup._digest_file(path)
            if ((info.st_dev, info.st_ino, info.st_mtime_ns, info.st_ctime_ns) !=
                    (row["device"], row["inode"], row["mtime_ns"], row["ctime_ns"]) or digest != row["sha256"] or size != row["size_bytes"]):
                raise setup.DatabaseError("Medya dosyası temizlik öncesinde değişti; taşınan kopyalar saklama klasöründe.")
            _move(path, target / f"{index:06d}-{path.name}")
        journal["status"] = "quarantined"
        _write_json(target / "quarantine.json", journal)
        return {"status": "quarantined", "store_id": confirm, "files": len(audit["candidates"]),
                "quarantine_path": str(target), "size_bytes": audit["size_bytes"], "deletes_bytes": False}


def main(argv=None):
    parser = argparse.ArgumentParser(description="RadioS özel yedek ve offline geri yükleme")
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--config")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list")
    commands.add_parser("create")
    audit = commands.add_parser("media-audit")
    audit.add_argument("--minimum-age-hours", type=int, default=24)
    cleanup = commands.add_parser("media-cleanup")
    cleanup.add_argument("--audit-id", required=True)
    cleanup.add_argument("--confirm-store-id")
    cleanup.add_argument("--offline", action="store_true")
    cleanup.add_argument("--minimum-age-hours", type=int, default=24)
    for name in ("verify", "restore-plan", "restore-stage", "restore"):
        command = commands.add_parser(name)
        command.add_argument("--backup", required=True)
        if name == "restore":
            command.add_argument("--confirm-store-id")
            command.add_argument("--offline", action="store_true")
    command = commands.add_parser("recover")
    command.add_argument("--confirm-store-id")
    command.add_argument("--offline", action="store_true")
    args = parser.parse_args(argv)
    os.umask(0o077)
    try:
        cfg = (_load_config_for_recovery(args.project_root, args.config) if args.command == "recover"
               else setup.load_config(args.project_root, args.config))
        with setup.operation_lock(cfg):
            _safe(cfg.project_root, cfg.runtime_dir)
            if args.command == "recover":
                result = recover(cfg, args.confirm_store_id, args.offline)
            else:
                _pending(cfg)
                if args.command == "list":
                    result = list_backups(cfg)
                elif args.command == "create":
                    result = create_backup(cfg)
                elif args.command == "restore":
                    result = restore(cfg, args.backup, args.confirm_store_id, args.offline)
                elif args.command == "media-audit":
                    result = _media_audit(cfg, args.minimum_age_hours)
                elif args.command == "media-cleanup":
                    result = media_cleanup(cfg, args.audit_id, args.confirm_store_id, args.offline, args.minimum_age_hours)
                else:
                    result = {"verify": verify_backup, "restore-plan": restore_plan,
                              "restore-stage": restore_stage}[args.command](cfg, args.backup)
        print(json.dumps(result, ensure_ascii=False, allow_nan=False))
        return 0
    except (RuntimeError, OSError, ValueError) as exc:
        # Native/backend error strings can contain file paths but never token
        # values. Avoid exposing raw unexpected parser/decoder exception bodies.
        message = str(exc) if isinstance(exc, setup.DatabaseError) else "Yedek veya geri yükleme güvenli biçimde tamamlanamadı."
        print(json.dumps({"status": "error", "error": message}, ensure_ascii=False))
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
