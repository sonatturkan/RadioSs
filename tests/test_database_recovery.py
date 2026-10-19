"""First-install recovery preserves the only plaintext bootstrap identity.

These tests interrupt actual SDBASE installation at publication boundaries.
All stores and credential files belong to temporary projects in RadioS/build.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import unittest
from unittest import mock


PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools"))
import radios_database_setup as setup


class DatabaseRecoveryTests(unittest.TestCase):
    def setUp(self):
        (PROJECT / "build").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="database-recovery-", dir=PROJECT / "build")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "RadioS Türkçe kaynak"
        (self.root / "config").mkdir(parents=True)
        shutil.copy2(PROJECT / "config/radios.example.ini", self.root / "config/radios.example.ini")
        shutil.copytree(PROJECT / "database/SDBASE/src", self.root / "database/SDBASE/src",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        self.cfg = setup.load_config(self.root)
        self.pending_file = self.cfg.runtime_dir / "setup-pending.json"

    def interrupted_install(self):
        original = setup._publish
        count = 0

        def interrupt(source, target, digest):
            nonlocal count
            count += 1
            if count == 3:
                raise OSError("simulated interruption after two published files")
            return original(source, target, digest)

        with setup.operation_lock(self.cfg), mock.patch.object(setup, "_publish", side_effect=interrupt):
            with self.assertRaises(setup.DatabaseError):
                setup.initialize(self.cfg)
        self.assertTrue(self.pending_file.is_file())
        pending = json.loads(self.pending_file.read_text(encoding="utf-8"))
        stage = self.cfg.runtime_dir / pending["stage_name"]
        staged_credentials = json.loads((stage / "credentials.json").read_text(encoding="utf-8"))
        self.assertFalse(self.cfg.credentials_file.exists())
        self.assertEqual(stat.S_IMODE((stage / "credentials.json").stat().st_mode), 0o600)
        return pending, stage, staged_credentials

    def live_files(self):
        return {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                for path in self.cfg.database_file.parent.glob(self.cfg.database_file.name + "*")
                if path.is_file()}

    def test_resume_retains_original_identity_and_tokens(self):
        _pending, stage, staged = self.interrupted_install()
        with setup.operation_lock(self.cfg):
            summary = setup.initialize(self.cfg)
            live = setup.load_credentials(self.cfg)
            repeated = setup.initialize(self.cfg)
        self.assertEqual(summary["status"], "initialized")
        self.assertEqual(repeated["status"], "already_initialized")
        for key in ("store_id", "user_id", "station_id", "endpoint_id"):
            self.assertEqual(live[key], staged[key])
            self.assertEqual(summary[key], staged[key])
        for key in ("api_token", "endpoint_token"):
            # Token values must never appear in assertion failure output.
            self.assertTrue(live[key] == staged[key], "Recovery replaced an original credential")
        backend = setup.backend_import(self.cfg)
        store = backend.open_radio_store(self.cfg.database_file, read_only=True)
        self.assertEqual(store.schema_version, 4)
        self.assertEqual(len(store.scan("users")), 1)
        self.assertEqual(len(store.scan("stations")), 1)
        self.assertEqual(len(store.scan("endpoints")), 1)
        self.assertEqual(len(store.scan("tokens")), 2)
        self.assertEqual(store.get("endpoints", staged["endpoint_id"])["status"], "offline")
        self.assertEqual(store.scan("items"), [])
        self.assertFalse(self.pending_file.exists())
        self.assertFalse(stage.exists())

    def test_conflicting_live_file_is_preserved_with_recoverable_identity(self):
        _pending, stage, staged = self.interrupted_install()
        sentinel = b"pre-existing unrelated journal must survive\n"
        self.cfg.database_file.write_bytes(sentinel)
        self.cfg.database_file.chmod(0o600)
        before = self.live_files()
        with setup.operation_lock(self.cfg):
            with self.assertRaises(setup.DatabaseError):
                setup.initialize(self.cfg)
        self.assertEqual(self.cfg.database_file.read_bytes(), sentinel)
        self.assertTrue(all(self.live_files().get(name) == digest for name, digest in before.items()))
        self.assertFalse(self.cfg.credentials_file.exists())
        self.assertTrue(self.pending_file.is_file())
        retained = json.loads((stage / "credentials.json").read_text(encoding="utf-8"))
        self.assertTrue(retained == staged, "A conflicting target changed the recoverable identity")

    def test_pending_target_mismatch_does_not_publish_or_discard_identity(self):
        pending, stage, staged = self.interrupted_install()
        before = self.live_files()
        pending["database_file"] = "data/database/unrelated.sdb"
        self.pending_file.write_text(json.dumps(pending), encoding="utf-8")
        with setup.operation_lock(self.cfg):
            with self.assertRaises(setup.DatabaseError):
                setup.initialize(self.cfg)
        self.assertEqual(self.live_files(), before)
        self.assertFalse((self.cfg.database_file.parent / "unrelated.sdb").exists())
        self.assertFalse(self.cfg.credentials_file.exists())
        retained = json.loads((stage / "credentials.json").read_text(encoding="utf-8"))
        self.assertTrue(retained == staged, "A mismatched target discarded the recoverable identity")
        self.assertTrue(self.pending_file.is_file())

    def test_failure_before_publication_leaves_no_unmanaged_database(self):
        backend = setup.backend_import(self.cfg)
        for exception in (backend.StoreError("simulated bootstrap commit failure"),
                          setup.DatabaseError("simulated preparation validation failure")):
            with self.subTest(exception_type=type(exception).__name__):
                with setup.operation_lock(self.cfg), mock.patch.object(
                        setup, "backend_import", return_value=backend), mock.patch.object(
                            backend, "bootstrap", side_effect=exception):
                    with self.assertRaises(setup.DatabaseError):
                        setup.initialize(self.cfg)
                self.assertFalse(self.cfg.database_file.exists())
                self.assertFalse(self.cfg.credentials_file.exists())
                self.assertFalse(self.pending_file.exists())
                self.assertEqual(list(self.cfg.runtime_dir.glob("setup-*")), [])
        # A failed first attempt leaves the project usable for a genuine retry.
        with setup.operation_lock(self.cfg):
            summary = setup.initialize(self.cfg)
        self.assertEqual(summary["status"], "initialized")


if __name__ == "__main__":
    unittest.main()
