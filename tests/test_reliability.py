"""Actual private-installation archives, PCM media, offline swaps and recovery.

Every store lives below RadioS/build; production service, credentials and media
are never imported or changed by these tests.
"""
from __future__ import annotations

import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4
import wave

PROJECT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(PROJECT / "tools"))
import radios_database_setup as setup
import radios_database as service
import radios_reliability as reliability


class SimulatedCrash(BaseException):
    pass


class ReliabilityTests(unittest.TestCase):
    def setUp(self):
        (PROJECT / "build").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="reliability-", dir=PROJECT / "build")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "RadioS özel"
        (self.root / "config").mkdir(parents=True)
        shutil.copy2(PROJECT / "config/radios.example.ini", self.root / "config/radios.example.ini")
        shutil.copytree(PROJECT / "database/SDBASE/src", self.root / "database/SDBASE/src",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        (self.root / "tools").mkdir()
        for name in ("radios_database.py", "radios_database_setup.py", "radios_reliability.py"):
            shutil.copy2(PROJECT / "tools" / name, self.root / "tools" / name)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        config = self.root / "config/radios.example.ini"
        config.write_text(config.read_text().replace("127.0.0.1:8000", f"127.0.0.1:{self.port}"))
        self.cfg = setup.load_config(self.root)
        self.addCleanup(lambda: service.stop_service(self.cfg))
        with setup.operation_lock(self.cfg):
            setup.initialize(self.cfg)
        self.credentials = setup.load_credentials(self.cfg)
        self.backend, self.engine = reliability._modules(self.cfg)
        self.store = self.backend.open_radio_store(self.cfg.database_file)
        self.preferences = self.root / "config/ui-preferences.ini"
        self.preferences.write_text("[ui]\nversion=1\ntext_size=1\ndefault_profile=2\nlast_profile=1\n")
        self.preferences.chmod(0o600)

    def media(self):
        self.cfg.import_root.mkdir(mode=0o700, parents=True, exist_ok=True)
        source = self.cfg.import_root / (uuid4().hex + ".wav")
        samples = [round(16000 * math.sin(2 * math.pi * 400 * index / 8000)) for index in range(1600)]
        with wave.open(str(source), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(8000)
            output.writeframes(struct.pack("<1600h", *samples))
        source.chmod(0o600)
        normalized = [value / 32768 for value in samples]
        bins = []
        for start in range(0, len(samples), 400):
            bins.extend((min(normalized[start:start + 400]), max(normalized[start:start + 400])))
        analysis = {"sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                    "size_bytes": source.stat().st_size, "duration_ms": 200,
                    "sample_rate": 8000, "channels": 1, "codec": "pcm_s16le", "format": "wav",
                    "bitrate": 128000, "peak": max(map(abs, normalized)),
                    "rms": math.sqrt(sum(sample * sample for sample in normalized) / len(normalized)),
                    "clipped": False, "clipped_samples": 0, "sample_frames": 1600,
                    "waveform_frames_per_bin": 400, "waveform": bins}
        module = importlib.import_module(self.backend.package_name + ".desktop_media")
        self.store.desktop_import_root = self.cfg.import_root
        principal = self.backend.authenticate(self.store, self.credentials["api_token"])
        result = module.import_media(self.store, principal, station_id=self.credentials["station_id"],
            import_path=source.name, source_name="Türkçe.wav", idempotency_key=str(uuid4()),
            metadata={"title": "Yedeklenen gerçek PCM", "kind": "music", "artist": "Sanatçı",
                      "album": "Albüm", "genre": "Türkçe", "tags": [], "year": "2026", "comment": ""},
            analysis=analysis)
        return result, source.read_bytes()

    def snapshot(self):
        with setup.operation_lock(self.cfg):
            return reliability.create_backup(self.cfg)

    def live_hashes(self):
        result = {}
        for path in (*self.cfg.database_file.parent.rglob("*"), self.cfg.credentials_file,
                     self.cfg.config_file, self.preferences):
            if path.is_file() and not path.name.endswith(".lock"):
                result[path.relative_to(self.root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
        return result

    def test_full_archive_media_credentials_preferences_without_live_mutation(self):
        media, pcm = self.media()
        before = self.live_hashes()
        sequence = self.store.sequence
        archive = self.snapshot()
        self.assertEqual(archive["media_objects"], 1)
        self.assertEqual(archive["sequence"], sequence)
        self.assertEqual(self.live_hashes(), before)
        self.assertEqual(reliability.verify_backup(self.cfg, archive["name"])["status"], "verified")
        self.assertEqual(reliability.list_backups(self.cfg)["backups"][0]["name"], archive["name"])
        path = Path(archive["path"])
        for file in path.rglob("*"):
            if file.is_file():
                self.assertEqual(stat.S_IMODE(file.stat().st_mode), 0o600)
        restored = reliability.restore_stage(self.cfg, archive["name"])
        self.assertFalse(restored["activated"])
        stage = Path(restored["restored_path"])
        copy = self.backend.open_radio_store(stage / self.cfg.database_file.name, read_only=True)
        self.assertEqual(len(copy.scan("items")), 1)
        self.assertEqual(copy.get("endpoints", self.credentials["endpoint_id"])["status"], "offline")
        media_row = copy.get("media_files", media["media"]["media_file_id"])
        self.assertEqual((stage / (self.cfg.database_file.name + ".objects") / media_row["sha256"][:2] / media_row["sha256"]).read_bytes(), pcm)
        copied = setup._json_read(stage / "settings/credentials.json")
        self.assertTrue(copied["api_token"] == self.credentials["api_token"], "Archive changed private bootstrap credential")
        self.assertEqual(self.live_hashes(), before)
        self.assertNotIn(self.credentials["api_token"], json.dumps(archive))
        self.assertNotIn(self.credentials["endpoint_token"], json.dumps(restored))

    def test_actual_offline_restore_rewinds_catalog_media_and_ui_settings(self):
        first_media, first_pcm = self.media()
        archive = self.snapshot()
        self.media()
        self.preferences.write_text("[ui]\nversion=1\ntext_size=2\ndefault_profile=6\nlast_profile=5\n")
        with setup.operation_lock(self.cfg):
            result = reliability.restore(self.cfg, archive["name"], self.credentials["store_id"], True)
        self.assertEqual(result["status"], "restored")
        self.assertFalse(result["service_started"])
        credentials = setup.load_credentials(self.cfg)
        self.assertEqual(credentials["store_id"], self.credentials["store_id"])
        self.assertTrue(credentials["api_token"] == self.credentials["api_token"], "Restore replaced private bootstrap token")
        self.assertTrue(credentials["endpoint_token"] != self.credentials["endpoint_token"], "Restore reused a fenced endpoint token")
        live = self.backend.open_radio_store(self.cfg.database_file, read_only=True)
        self.assertEqual(len(live.scan("items")), 1)
        media_row = live.get("media_files", first_media["media"]["media_file_id"])
        media_path = self.cfg.database_file.with_name(self.cfg.database_file.name + ".objects") / media_row["sha256"][:2] / media_row["sha256"]
        self.assertEqual(media_path.read_bytes(), first_pcm)
        self.assertIn("text_size=1", self.preferences.read_text())
        self.assertFalse((self.cfg.runtime_dir / reliability.JOURNAL).exists())
        self.assertEqual(list(self.cfg.runtime_dir.glob("restore-transaction-*")), [])
        old_principal = self.backend.authenticate(live, credentials["api_token"])
        with self.assertRaises(self.backend.StoreError):
            self.backend.issue_endpoint_token(self.store, old_principal, credentials["station_id"], credentials["endpoint_id"])
        # Exercise the actual owned HTTP service with the renewed staged token,
        # not just a direct Python object, after the entire installation swap.
        start = subprocess.run([sys.executable, str(self.root / "tools/radios_database.py"),
                                "--project-root", str(self.root), "start"], capture_output=True, text=True, timeout=30)
        self.assertEqual(start.returncode, 0, start.stdout + start.stderr)
        started = json.loads(start.stdout)
        self.assertEqual(started["state"], "running")
        self.assertEqual(service.check_service(self.cfg)["integrity"], "ok")
        self.assertTrue(service.stop_service(self.cfg)["stopped"])

    def test_snapshot_while_an_independent_engine_commits_is_consistent(self):
        self.media()
        writer = self.backend.open_radio_store(self.cfg.database_file)
        principal = self.backend.authenticate(writer, self.credentials["api_token"])
        started = threading.Event()
        failures = []
        def commit():
            try:
                started.set()
                for _ in range(20):
                    self.backend.issue_endpoint_token(writer, principal, self.credentials["station_id"],
                                                      self.credentials["endpoint_id"], days=1)
            except BaseException as exc:
                failures.append(type(exc).__name__)
        thread = threading.Thread(target=commit)
        thread.start()
        self.assertTrue(started.wait(2))
        archive = self.snapshot()
        thread.join(5)
        self.assertFalse(thread.is_alive())
        self.assertEqual(failures, [])
        self.assertLessEqual(archive["sequence"], writer.sequence)
        self.assertEqual(reliability.verify_backup(self.cfg, archive["name"])["integrity"], "ok")
        self.assertEqual(archive["media_objects"], 1)

    def outbox(self):
        user = self.root / "data/recovery" / self.credentials["store_id"] / self.credentials["user_id"]
        user.mkdir(mode=0o700, parents=True, exist_ok=True)
        for parent in (user, user.parent, user.parent.parent):
            parent.chmod(0o700)
        lock = user / ".lock"
        lock.touch(mode=0o600)
        key = str(uuid4())
        path = user / (key + ".json")
        body = {"version": 1, "created_us": 1791288000000000,
                **{key: self.credentials[key] for key in reliability.IDENTITIES}, "state": "pending",
                "method": "POST", "path": "/v1/workflows/desktop/put",
                "body": {"station_id": self.credentials["station_id"], "idempotency_key": key,
                         "kind": "broadcast_metadata", "name": "Bekleyen", "config": {"title": "Başlık", "automatic": False}}}
        path.write_text(json.dumps(body))
        path.chmod(0o600)
        return user.parent, path.read_bytes(), path.name

    def test_restore_quarantines_current_outbox_without_archiving_or_replaying(self):
        store_dir, envelope, leaf = self.outbox()
        archive = self.snapshot()
        archive_files = [path.relative_to(Path(archive["path"])).as_posix() for path in Path(archive["path"]).rglob("*")]
        self.assertFalse(any("recovery/" in path or leaf in path for path in archive_files))
        # A safe preview neither acknowledges nor moves the pending operation.
        preview = reliability.restore_stage(self.cfg, archive["name"])
        self.assertFalse(preview["outbox_included"])
        self.assertTrue(store_dir.exists())
        before = self.live_hashes()
        result = reliability.restore(self.cfg, archive["name"], self.credentials["store_id"], True)
        self.assertFalse(store_dir.exists())
        quarantine = Path(result["outbox_quarantine"])
        self.assertTrue(quarantine.is_relative_to(self.cfg.runtime_dir))
        self.assertEqual((quarantine / self.credentials["user_id"] / leaf).read_bytes(), envelope)
        self.assertNotIn("body", result)

    def test_crash_after_outbox_move_rolls_back_exact_pending_envelopes(self):
        store_dir, envelope, leaf = self.outbox()
        before = self.interrupted("outbox")
        self.assertFalse(store_dir.exists())
        result = reliability.recover(self.cfg, self.credentials["store_id"], True)
        self.assertEqual(result["action"], "rolled_back")
        self.assertEqual(self.live_hashes(), before)
        self.assertEqual((store_dir / self.credentials["user_id"] / leaf).read_bytes(), envelope)

    def test_interruption_after_durable_commit_finalizes_and_preserves_outbox(self):
        store_dir, envelope, leaf = self.outbox()
        archive = self.snapshot()
        write = reliability._write_json
        def interrupt(path, value):
            write(path, value)
            if path.name == reliability.JOURNAL and value.get("state") == "committed":
                raise SimulatedCrash()
        with patch.object(reliability, "_write_json", side_effect=interrupt), self.assertRaises(SimulatedCrash):
            reliability.restore(self.cfg, archive["name"], self.credentials["store_id"], True)
        result = reliability.recover(self.cfg, self.credentials["store_id"], True)
        self.assertEqual(result["action"], "committed")
        self.assertFalse(store_dir.exists())
        self.assertEqual((Path(result["outbox_quarantine"]) / self.credentials["user_id"] / leaf).read_bytes(), envelope)
        self.assertEqual(setup.load_credentials(self.cfg)["store_id"], self.credentials["store_id"])

    def test_backup_and_restore_preview_refuse_without_free_space(self):
        """A full copy that would leave the disk without the margin is refused before writing."""
        reset = importlib.import_module(self.backend.package_name + ".desktop_reset")
        archive = self.snapshot()
        margin = reset.SPACE_MARGIN_BYTES
        self.addCleanup(setattr, reset, "SPACE_MARGIN_BYTES", margin)
        reset.SPACE_MARGIN_BYTES = 2 ** 62
        before = sorted(path.name for path in self.cfg.backup_root.iterdir())
        with self.assertRaisesRegex(setup.DatabaseError, "Yedek için yeterli boş alan yok"):
            self.snapshot()
        self.assertEqual(sorted(path.name for path in self.cfg.backup_root.iterdir()), before)
        with self.assertRaisesRegex(setup.DatabaseError, "Geri yükleme denemesi için yeterli boş alan yok"):
            reliability.restore_stage(self.cfg, archive["name"])
        self.assertFalse(any(path.name.startswith("restore-preview-") for path in self.cfg.runtime_dir.iterdir()))

    def test_missing_confirmation_or_foreign_identity_never_overwrites(self):
        archive = self.snapshot()
        before = self.live_hashes()
        for confirm, offline in ((None, False), (self.credentials["store_id"], False), (str(uuid4()), True)):
            with self.subTest(offline=offline), self.assertRaises(setup.DatabaseError):
                reliability.restore(self.cfg, archive["name"], confirm, offline)
        self.assertEqual(self.live_hashes(), before)
        meta = self.cfg.database_file.with_name(self.cfg.database_file.name + ".meta")
        original = meta.read_bytes()
        changed = json.loads(original)
        changed["store_id"] = str(uuid4())
        meta.write_text(json.dumps(changed))
        changed_before = self.live_hashes()
        with self.assertRaises(setup.DatabaseError):
            reliability.restore(self.cfg, archive["name"], self.credentials["store_id"], True)
        self.assertEqual(self.live_hashes(), changed_before)
        meta.write_bytes(original)

    def test_foreign_port_pid_gui_and_engine_lock_are_rejected(self):
        archive = self.snapshot()
        before = self.live_hashes()
        with socket.socket() as foreign:
            foreign.bind(("127.0.0.1", self.port))
            foreign.listen()
            with self.assertRaises(setup.DatabaseError):
                reliability.restore(self.cfg, archive["name"], self.credentials["store_id"], True)
        service._write_json(service._state_path(self.cfg), {"pid": os.getpid(), "schema_version": 1})
        with self.assertRaises(setup.DatabaseError):
            reliability.restore(self.cfg, archive["name"], self.credentials["store_id"], True)
        service._state_path(self.cfg).unlink()
        with patch.object(reliability, "_gui_open", return_value=True), self.assertRaises(setup.DatabaseError):
            reliability.restore(self.cfg, archive["name"], self.credentials["store_id"], True)
        # Hold server lock: unlike the short-lived engine lock it remains
        # available while archive verification prepares its independent copy.
        lock = self.cfg.runtime_dir / "server.lock"
        with lock.open("a+b") as held:
            import fcntl
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(setup.DatabaseError):
                reliability.restore(self.cfg, archive["name"], self.credentials["store_id"], True)
        with (self.cfg.runtime_dir / "gui.lock").open("a+b") as held:
            import fcntl
            fcntl.flock(held, fcntl.LOCK_SH | fcntl.LOCK_NB)
            with self.assertRaises(setup.DatabaseError):
                reliability.restore(self.cfg, archive["name"], self.credentials["store_id"], True)
        self.assertEqual(self.live_hashes(), before)

    def test_corruption_unlisted_symlink_and_traversal_archives_rejected(self):
        self.media()
        archive = self.snapshot()
        root = Path(archive["path"])
        before = self.live_hashes()
        for unsafe in ("../foreign", "/tmp/foreign", "radios-20261006T120000Z-12345678/../foreign"):
            with self.assertRaises(setup.DatabaseError):
                reliability.verify_backup(self.cfg, unsafe)
        settings = root / "settings/application.ini"
        original = settings.read_bytes()
        settings.write_bytes(original + b"damage")
        with self.assertRaises(setup.DatabaseError):
            reliability.verify_backup(self.cfg, archive["name"])
        settings.write_bytes(original)
        rogue = root / "unlisted"
        rogue.write_text("not inventoried")
        with self.assertRaises(setup.DatabaseError):
            reliability.verify_backup(self.cfg, archive["name"])
        rogue.unlink()
        settings.unlink()
        settings.symlink_to(self.cfg.config_file)
        with self.assertRaises((setup.DatabaseError, self.backend.StoreError)):
            reliability.verify_backup(self.cfg, archive["name"])
        settings.unlink()
        settings.write_bytes(original)
        settings.chmod(0o600)
        manifest = reliability._read_manifest(root)
        manifest["database_file"] = "../outside.sdb"
        (root / "manifest.json").unlink()
        (root / "manifest.sha256").unlink()
        reliability._write_manifest(root, manifest)
        with self.assertRaises(setup.DatabaseError):
            reliability.verify_backup(self.cfg, archive["name"])
        self.assertEqual(reliability.list_backups(self.cfg)["backups"][0]["status"], "invalid")
        self.assertEqual(self.live_hashes(), before)

    def test_foreign_credentials_even_with_recomputed_hash_are_rejected(self):
        archive = self.snapshot()
        root = Path(archive["path"])
        credentials = setup._json_read(root / "settings/credentials.json")
        credentials["station_id"] = str(uuid4())
        (root / "settings/credentials.json").write_text(json.dumps(credentials))
        manifest = reliability._read_manifest(root)
        digest, size = self.engine._digest_file(root / "settings/credentials.json")
        oldsize = manifest["settings"]["credentials.json"]["size_bytes"]
        manifest["settings"]["credentials.json"] = {"sha256": digest, "size_bytes": size}
        manifest["size_bytes"] += size - oldsize
        (root / "manifest.json").unlink()
        (root / "manifest.sha256").unlink()
        reliability._write_manifest(root, manifest)
        with self.assertRaises(setup.DatabaseError):
            reliability.verify_backup(self.cfg, archive["name"])

    def interrupted(self, missing_key):
        archive = self.snapshot()
        self.media()
        before = self.live_hashes()
        move = reliability._move
        target = reliability._targets(self.cfg)[missing_key]
        def interrupt(source, destination):
            move(source, destination)
            if source == target:
                raise SimulatedCrash()
        with patch.object(reliability, "_move", side_effect=interrupt), self.assertRaises(SimulatedCrash):
            reliability.restore(self.cfg, archive["name"], self.credentials["store_id"], True)
        self.assertTrue((self.cfg.runtime_dir / reliability.JOURNAL).exists())
        self.assertFalse(target.exists())
        return before

    def test_crash_between_credentials_swaps_recovers_original_identity(self):
        before = self.interrupted("credentials")
        with self.assertRaises(setup.DatabaseError):
            service.start_service(self.cfg)
        with self.assertRaises(setup.DatabaseError):
            setup.initialize(self.cfg)
        result = reliability.recover(self.cfg, self.credentials["store_id"], True)
        self.assertEqual(result["action"], "rolled_back")
        self.assertEqual(self.live_hashes(), before)
        live = setup.load_credentials(self.cfg)
        self.assertTrue(live["api_token"] == self.credentials["api_token"], "Recovery lost original private credential")

    def test_crash_between_config_swaps_recovers_via_actual_cli(self):
        before = self.interrupted("config")
        command = [sys.executable, str(PROJECT / "tools/radios_reliability.py"), "--project-root", str(self.root),
                   "recover", "--confirm-store-id", self.credentials["store_id"], "--offline"]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["action"], "rolled_back")
        self.assertEqual(self.live_hashes(), before)

    def test_crash_missing_local_config_recovers_without_example_fallback(self):
        local = self.root / "config/radios.ini"
        shutil.copy2(self.cfg.config_file, local)
        self.cfg = setup.load_config(self.root)
        self.assertEqual(self.cfg.config_file, local)
        before = self.interrupted("config")
        self.assertFalse(local.exists())
        self.assertTrue((self.root / "config/radios.example.ini").exists())
        command = [sys.executable, str(PROJECT / "tools/radios_reliability.py"), "--project-root", str(self.root),
                   "recover", "--confirm-store-id", self.credentials["store_id"], "--offline"]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["action"], "rolled_back")
        self.assertTrue(local.exists())
        self.assertEqual(self.live_hashes(), before)

    def test_io_error_rolls_back_and_post_crash_unrelated_edit_is_preserved(self):
        archive = self.snapshot()
        self.media()
        before = self.live_hashes()
        move = reliability._move
        count = 0
        def fail_once(source, destination):
            nonlocal count
            count += 1
            if count == 4:
                raise OSError("simulated disk operation failure")
            move(source, destination)
        with patch.object(reliability, "_move", side_effect=fail_once), self.assertRaises(OSError):
            reliability.restore(self.cfg, archive["name"], self.credentials["store_id"], True)
        self.assertEqual(self.live_hashes(), before)
        self.assertFalse((self.cfg.runtime_dir / reliability.JOURNAL).exists())
        self.interrupted("credentials")
        self.cfg.database_file.write_bytes(b"unrelated post-crash bytes must survive")
        changed = self.live_hashes()
        with self.assertRaises(setup.DatabaseError):
            reliability.recover(self.cfg, self.credentials["store_id"], True)
        self.assertEqual(self.live_hashes(), changed)
        self.assertTrue((self.cfg.runtime_dir / reliability.JOURNAL).exists())

    def test_json_cli_output_never_contains_tokens(self):
        commands = ["create", "list"]
        for command in commands:
            result = subprocess.run([sys.executable, str(PROJECT / "tools/radios_reliability.py"),
                                     "--project-root", str(self.root), command], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIsInstance(json.loads(result.stdout), dict)
            self.assertNotIn(self.credentials["api_token"], result.stdout + result.stderr)
            self.assertNotIn(self.credentials["endpoint_token"], result.stdout + result.stderr)

    def test_restored_runtime_is_fenced_with_original_observed_counters(self):
        module = importlib.import_module(self.backend.package_name + ".desktop_broadcast")
        admin = importlib.import_module(self.backend.package_name + ".desktop_admin")
        principal = self.backend.authenticate(self.store, self.credentials["api_token"])
        sid = self.credentials["station_id"]
        profile = module.profile_save(self.store, principal, sid, "record", "Yerel", {"filename": "main.wav"}, str(uuid4()))
        run_key = str(uuid4())
        run = module.run_start(self.store, principal, sid, "record", profile["_id"], profile["_rev"], run_key)
        module.run_event(self.store, principal, sid, run["run_id"], run["revision"], "active", 123456, 789000,
                         "Actual observed counters; future tail unknown", str(uuid4()))
        before = self.store.get("desktop_configurations", run["run_id"])
        archive = self.snapshot()
        restored = reliability.restore_stage(self.cfg, archive["name"])
        self.assertEqual(restored["closed_runtime_runs"], 1)
        self.assertEqual(restored["fenced_runtime_runs"], 1)
        copy = self.backend.open_radio_store(Path(restored["restored_path"]) / self.cfg.database_file.name)
        original = copy.get("desktop_configurations", run["run_id"])
        self.assertEqual(original, before)
        closure = admin.recovery_for_run(copy, sid, run["run_id"])
        self.assertTrue(closure["config"]["unknown_tail"])
        self.assertFalse(closure["config"]["delivery_proven"])
        with self.assertRaises(self.backend.StoreError):
            module.run_event(copy, principal, sid, run["run_id"], original["_rev"], "completed", 999999, 999999,
                             "A new observation cannot describe old media", str(uuid4()))
        with self.assertRaises(self.backend.StoreError):
            module.run_start(copy, principal, sid, "record", profile["_id"], profile["_rev"], run_key)

    def test_media_audit_protects_committed_pending_young_and_unsafe_files(self):
        media, _ = self.media()
        store_dir, _, envelope_leaf = self.outbox()
        outbox = store_dir / self.credentials["user_id"] / envelope_leaf
        private = self.cfg.import_root / (str(uuid4()) + ".wav")
        pending = self.cfg.import_root / (str(uuid4()) + ".wav")
        young = self.cfg.import_root / (str(uuid4()) + ".wav")
        unsafe = self.cfg.import_root / (str(uuid4()) + ".wav")
        for path in (private, pending, young):
            path.write_bytes(b"retained recoverable source bytes")
            path.chmod(0o600)
        unsafe.symlink_to(private)
        body = json.loads(outbox.read_text())
        body["path"] = "/v1/workflows/desktop-media/import"
        body["body"]["import_path"] = pending.name
        outbox.write_text(json.dumps(body))
        preview = self.root / "data/cache/transition-preview/transition-Ab12CD"
        preview.parent.mkdir(mode=0o700, parents=True)
        preview.write_bytes(b"rendered private CUE preview")
        preview.chmod(0o600)
        objects = self.cfg.database_file.with_name(self.cfg.database_file.name + ".objects")
        orphan_bytes = b"unreferenced content address left after rejected transaction"
        orphan_sha = hashlib.sha256(orphan_bytes).hexdigest()
        orphan = objects / orphan_sha[:2] / orphan_sha
        orphan.parent.mkdir(mode=0o700, exist_ok=True)
        orphan.write_bytes(orphan_bytes)
        orphan.chmod(0o600)
        future = reliability.time.time_ns() + 72 * 3600 * 1000000000
        os.utime(young, ns=(future, future))
        with patch.object(reliability.time, "time_ns", return_value=future):
            audit = reliability._media_audit(self.cfg)
            self.assertEqual({row["kind"] for row in audit["candidates"]}, {"import", "cue_preview", "managed_orphan"})
            self.assertEqual(len(audit["candidates"]), 3)
            self.assertNotIn(pending.name, {Path(row["path"]).name for row in audit["candidates"]})
            with self.assertRaises(setup.DatabaseError):
                reliability.media_cleanup(self.cfg, "0" * 64, self.credentials["store_id"], True)
            result = reliability.media_cleanup(self.cfg, audit["audit_id"], self.credentials["store_id"], True)
        self.assertEqual(result["files"], 3)
        self.assertFalse(result["deletes_bytes"])
        self.assertTrue(pending.is_file())
        self.assertTrue(young.is_file())
        self.assertTrue(unsafe.is_symlink())
        self.assertFalse(private.exists())
        self.assertFalse(orphan.exists())
        self.assertFalse(preview.exists())
        copies = [path for path in Path(result["quarantine_path"]).iterdir() if path.name != "quarantine.json"]
        self.assertEqual(len(copies), 3)
        self.assertIn(orphan_bytes, [path.read_bytes() for path in copies])
        managed = self.store.get("media_files", media["media"]["media_file_id"])
        self.assertTrue((objects / managed["sha256"][:2] / managed["sha256"]).is_file())

    def test_media_cleanup_rejects_changed_review_and_invalid_outbox(self):
        self.cfg.import_root.mkdir(mode=0o700, parents=True)
        path = self.cfg.import_root / (str(uuid4()) + ".wav")
        path.write_bytes(b"original review bytes")
        path.chmod(0o600)
        future = reliability.time.time_ns() + 72 * 3600 * 1000000000
        with patch.object(reliability.time, "time_ns", return_value=future):
            audit = reliability._media_audit(self.cfg)
            path.write_bytes(b"changed since review")
            with self.assertRaises(setup.DatabaseError):
                reliability.media_cleanup(self.cfg, audit["audit_id"], self.credentials["store_id"], True)
            self.assertEqual(path.read_bytes(), b"changed since review")
            store_dir, _, leaf = self.outbox()
            (store_dir / self.credentials["user_id"] / leaf).write_text("not JSON")
            with self.assertRaises(ValueError):
                reliability._media_audit(self.cfg)
            self.assertEqual(path.read_bytes(), b"changed since review")


if __name__ == "__main__":
    unittest.main()
