#!/usr/bin/env python3
"""config/storage.ini in the data service: the database and backup folders chosen in
Ayarlar › Medya arşivi are honoured with the same file, keys and marker rule as
src/core/storage_locations.c. Every test uses a private project root."""
from __future__ import annotations

import dataclasses
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from uuid import uuid4

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "database/SDBASE/src"))
import radios_database as service  # noqa: E402
import radios_database_setup as setup  # noqa: E402
from sdbase import desktop_reset  # noqa: E402

MARKER = {"format": "radios-storage-v1"}


class StorageLocations(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="radios-storage-")
        self.addCleanup(self.temporary.cleanup)
        base = Path(self.temporary.name).resolve()
        self.root = base / "Türkçe kaynak kodlar RadioS"
        for name in ("config", "src", "database/SDBASE", "data/database", "data/backups", "data/logs",
                     "data/database-service"):
            (self.root / name).mkdir(parents=True)
        self.disk = base / "disk"
        self.disk.mkdir()
        self.cfg = setup.LocalConfig(
            project_root=self.root, config_file=self.root / "config/radios.example.ini",
            database_file=self.root / "data/database/RadioS.sdb", source_dir=self.root / "database/SDBASE",
            endpoint_root=self.root / "data/endpoints", log_root=self.root / "data/logs",
            backup_root=self.root / "data/backups", service_url="http://127.0.0.1:9", host="127.0.0.1", port=9,
            python_command="python3", token_environment="RADIOS_SDBASE_TOKEN", connect_timeout_seconds=1,
            request_timeout_seconds=3, credentials_file=self.root / "config/credentials.json",
            runtime_dir=self.root / "data/database-service", import_root=self.root / "data/imports")

    def prepare(self, directory: Path, role: str) -> Path:
        directory.mkdir(parents=True, exist_ok=True)
        (directory / service.STORAGE_MARKER).write_text(json.dumps({**MARKER, "role": role}), encoding="utf-8")
        return directory

    def write_ini(self, **values):
        lines = ["[storage]", "version=1"] + [f"{key}={value}" for key, value in values.items()]
        (self.root / "config" / service.STORAGE_FILE).write_text("\n".join(lines) + "\n", encoding="utf-8")

    def install(self, database_file: Path) -> dict:
        """A managed installation whose database lives at database_file, like tools init."""
        shutil.copytree(ROOT / "database/SDBASE/src", self.root / "database/SDBASE/src",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        backend = setup.backend_import(self.cfg)
        store = backend.open_radio_store(database_file, lock_timeout=5)
        boot = backend.bootstrap(store, "admin", "RADIOS", "RadioS", "UTC")
        admin = backend.authenticate(store, boot["api_token"])
        endpoint_token = backend.issue_endpoint_token(store, admin, boot["station_id"], boot["endpoint_id"], days=30)
        credentials = {
            "schema_version": 1, "store_id": store._meta()["store_id"], "user_id": boot["user_id"],
            "station_id": boot["station_id"], "endpoint_id": boot["endpoint_id"], "api_token": boot["api_token"],
            "endpoint_token": endpoint_token, "username": "admin", "station_code": "RADIOS",
            "station_name": "RadioS", "timezone": "UTC", "database_file": "data/database/RadioS.sdb",
            "created_at": datetime.now(timezone.utc).isoformat(),
        }
        for label in ("api_token", "endpoint_token"):
            digest = hashlib.sha256(credentials[label].encode("ascii")).hexdigest()
            credentials[label + "_expires_at"] = store.lookup("tokens", "hash", digest)[0]["expires_at"]
        store.checkpoint()
        setup._json_write(self.cfg.credentials_file, credentials)
        return credentials

    def test_missing_file_keeps_the_project_defaults(self):
        self.assertEqual(service.load_storage_settings(self.root), {})
        cfg = service.effective_config(self.cfg)
        self.assertEqual((cfg.database_file, cfg.backup_root), (self.cfg.database_file, self.cfg.backup_root))
        self.assertTrue(service._database_in_project(cfg))

    def test_path_rule_matches_the_c_reader(self):
        for bad in ("", "relative", "/a/../b", "/a//b", "/a/", "/", "/a\tb", "/a/./b"):
            with self.assertRaises(setup.DatabaseError):
                service.storage_path_check(bad)
        service.storage_path_check("/mnt/Türkçe disk/arşiv")
        self.write_ini(database_dir="relative/path")
        with self.assertRaises(setup.DatabaseError):
            service.load_storage_settings(self.root)

    def test_key_file_escapes_version_and_links(self):
        self.write_ini(recording_root="/mnt/kay\\sıt", backup_root="")
        self.assertEqual(service.load_storage_settings(self.root), {"recording_root": "/mnt/kay ıt"})
        ini = self.root / "config" / service.STORAGE_FILE
        ini.write_text("[storage]\nversion=2\n", encoding="utf-8")
        with self.assertRaises(setup.DatabaseError):
            service.load_storage_settings(self.root)
        ini.unlink()
        ini.symlink_to(self.disk)
        with self.assertRaises(setup.DatabaseError):
            service.load_storage_settings(self.root)

    def test_database_folder_needs_the_marker_and_a_real_path(self):
        folder = self.disk / "db"
        folder.mkdir()
        self.write_ini(database_dir=str(folder))
        with self.assertRaisesRegex(setup.DatabaseError, "işareti yok"):
            service.effective_config(self.cfg)
        self.prepare(folder, "recordings")
        with self.assertRaisesRegex(setup.DatabaseError, "başka amaçla"):
            service.effective_config(self.cfg)
        self.prepare(folder, "database")
        cfg = service.effective_config(self.cfg)
        self.assertEqual(cfg.database_file, folder / "RadioS.sdb")
        self.assertFalse(service._database_in_project(cfg))
        self.assertEqual(service.effective_config(cfg).database_file, cfg.database_file)
        link = self.disk / "link"
        link.symlink_to(folder)
        self.write_ini(database_dir=str(link))
        with self.assertRaisesRegex(setup.DatabaseError, "sembolik"):
            service.effective_config(self.cfg)
        inside = self.prepare(self.root / "src" / "db", "database")
        self.write_ini(database_dir=str(inside))
        with self.assertRaisesRegex(setup.DatabaseError, "kaynak veya ayar"):
            service.effective_config(self.cfg)
        self.write_ini(database_dir=str(self.disk / "yok"))
        with self.assertRaisesRegex(setup.DatabaseError, "bulunamadı"):
            service.effective_config(self.cfg)
        self.assertIs(service._effective_or_base(self.cfg), self.cfg)

    def test_backup_folder_falls_back_without_a_marker(self):
        folder = self.disk / "yedek"
        folder.mkdir()
        self.write_ini(backup_root=str(folder))
        self.assertEqual(service.effective_backup_root(self.cfg), self.cfg.backup_root)
        self.assertEqual(service.effective_config(self.cfg).backup_root, self.cfg.backup_root)
        self.prepare(folder, "backups")
        self.assertEqual(service.effective_backup_root(self.cfg), folder)
        self.assertEqual(service.effective_config(self.cfg).backup_root, folder)

    def test_desktop_reset_backs_up_into_the_service_folder(self):
        folder = self.prepare(self.disk / "yedek", "backups")
        targets = []

        def fake_backup(store, target):
            targets.append(Path(target))
            return {"path": str(target)}

        store = SimpleNamespace(root=str(self.root / "data/database"), desktop_backup_root=lambda: folder)
        # The free-space check (desktop_reset.require_space) sizes a real store;
        # these stand-ins only name folders, so the backup writes nothing.
        with patch("sdbase.backup.create_backup", side_effect=fake_backup), \
             patch.object(desktop_reset, "_backup_bytes", return_value=0):
            path = desktop_reset._backup(store, ["media"])
            desktop_reset._backup(SimpleNamespace(root=str(self.root / "data/database")), ["settings"])
        self.assertEqual(path, str(targets[0]))
        self.assertEqual(targets[0].parent, folder)
        self.assertTrue(targets[0].name.startswith("sifirlama-"))
        self.assertTrue(targets[0].name.endswith("-media"))
        self.assertEqual(targets[1].parent, self.root / "data/backups")

    def test_migration_copies_the_database_once_and_keeps_the_source(self):
        source = self.cfg.database_file
        source.write_bytes(b"journal")
        source.with_name("RadioS.sdb.meta").write_text("{}", encoding="utf-8")
        objects = source.with_name("RadioS.sdb.objects") / "ab"
        objects.mkdir(parents=True)
        (objects / "abcd").write_bytes(b"audio")
        folder = self.prepare(self.disk / "db", "database")
        self.write_ini(database_dir=str(folder))
        cfg = service.effective_config(self.cfg)
        with patch.object(service, "_owned", return_value=False):
            service._prepare_database(self.cfg, cfg)
        self.assertEqual((folder / "RadioS.sdb").read_bytes(), b"journal")
        self.assertEqual((folder / "RadioS.sdb.meta").read_text(encoding="utf-8"), "{}")
        self.assertEqual((folder / "RadioS.sdb.objects/ab/abcd").read_bytes(), b"audio")
        self.assertTrue((folder / "RadioS.sdb.lock").is_file())
        self.assertEqual(source.read_bytes(), b"journal")
        self.assertEqual(sorted(path.name for path in folder.iterdir() if path.name.startswith(".")),
                         [service.STORAGE_MARKER])
        # An existing database in the chosen folder is never replaced.
        (folder / "RadioS.sdb").write_bytes(b"newer")
        service._prepare_database(self.cfg, cfg)
        self.assertEqual((folder / "RadioS.sdb").read_bytes(), b"newer")
        # The folder used last is the source for the next move, not the project default.
        service._remember_database(cfg)
        self.assertEqual(service._last_database(cfg), folder / "RadioS.sdb")
        other = self.prepare(self.disk / "db2", "database")
        self.write_ini(database_dir=str(other))
        moved = service.effective_config(self.cfg)
        with patch.object(service, "_owned", return_value=False):
            service._prepare_database(self.cfg, moved)
        self.assertEqual((other / "RadioS.sdb").read_bytes(), b"newer")

    def test_missing_database_without_a_source_is_an_error(self):
        folder = self.prepare(self.disk / "db", "database")
        self.write_ini(database_dir=str(folder))
        cfg = service.effective_config(self.cfg)
        with self.assertRaisesRegex(setup.DatabaseError, "Veritabanı dosyası yok"):
            service._prepare_database(self.cfg, cfg)
        self.assertFalse((folder / "RadioS.sdb").exists())

    def test_own_service_on_another_database_is_stale_not_foreign(self):
        cfg = self.cfg
        instance = str(uuid4())
        expected = [str(cfg.project_root / "tools" / "radios_database.py"), "--project-root", str(cfg.project_root),
                    "--config", str(cfg.config_file), "serve", "--instance", instance]
        state = {"schema_version": 1, "project_root": str(cfg.project_root),
                 "database_file": str(self.disk / "eski" / "RadioS.sdb"), "service_url": cfg.service_url,
                 "pid": 4242, "boot_id": "boot", "start_ticks": "77", "instance": instance}
        with patch.object(service, "_process_identity", return_value=("boot", "77", expected)):
            self.assertFalse(service._owned(cfg, state))
            self.assertTrue(service._owned(cfg, state, same_database=False))
            service._write_json(service._state_path(cfg), state)
            result = service.service_status(cfg, {"store_id": "store"})
        self.assertEqual((result["state"], result["pid"], result["stale_database"]), ("unhealthy", 4242, True))
        self.assertEqual(result["database_file"], str(cfg.database_file))

    def test_credentials_validate_against_the_moved_store(self):
        folder = self.prepare(self.disk / "db", "database")
        credentials = self.install(folder / "RadioS.sdb")
        self.write_ini(database_dir=str(folder))
        cfg = service.effective_config(self.cfg)
        value = service.load_credentials(cfg)
        self.assertEqual(value["store_id"], credentials["store_id"])
        with self.assertRaisesRegex(setup.DatabaseError, "eşleşmiyor"):
            service.load_credentials(dataclasses.replace(cfg, database_file=folder / "Other.sdb"))
        (self.root / "config" / service.STORAGE_FILE).unlink()
        with self.assertRaises(setup.DatabaseError):
            service.load_credentials(service.effective_config(self.cfg))


if __name__ == "__main__":
    unittest.main()
