#!/usr/bin/env python3
"""Real PCM snapshots, managed bytes, durable CAS/replay and tenant isolation."""
from copy import deepcopy
import hashlib
from http.client import HTTPConnection
import json
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "database/SDBASE/src"))
from sdbase import desktop_media as api
from sdbase.desktop_copyright import DEFAULT_POLICY
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.catalog import NotFound
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate, bootstrap, issue_endpoint_token
from sdbase.integrity import inspect_integrity
from sdbase.media import object_root
from sdbase.schema import open_radio_store
from sdbase.search import search_items
from sdbase.storage import Conflict, StoreError, Transaction


class DesktopMedia(unittest.TestCase):
    def assert_public_detail(self, actual, stored):
        self.assertEqual(actual, stored | {"copyright_policy": DEFAULT_POLICY,
                                          "copyright_policy_document_id": None,
                                          "copyright_policy_revision": 0})

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="desktop-media-", dir=ROOT / "build")
        self.root = Path(self.temp.name)
        self.imports = self.root / "imports"
        self.imports.mkdir(mode=0o700)
        self.db = self.root / "test.sdb"
        self.store = open_radio_store(self.db)
        self.store.desktop_import_root = self.imports
        self.identity = bootstrap(self.store, "admin", "MEDIA", "Media station")
        self.sid = self.identity["station_id"]
        self.principal = authenticate(self.store, self.identity["api_token"])
        self.path = self.imports / (str(uuid4()) + ".wav")
        samples = [round(16000 * math.sin(2 * math.pi * 400 * index / 8000)) for index in range(1600)]
        with wave.open(str(self.path), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(8000)
            output.writeframes(struct.pack("<1600h", *samples))
        self.path.chmod(0o600)
        normalized = [value / 32768 for value in samples]
        waveform = []
        for start in range(0, len(samples), 400):
            chunk = normalized[start:start + 400]
            waveform.extend((min(chunk), max(chunk)))
        self.analysis = {"sha256": hashlib.sha256(self.path.read_bytes()).hexdigest(),
                         "size_bytes": self.path.stat().st_size, "duration_ms": 200,
                         "sample_rate": 8000, "channels": 1, "codec": "pcm_s16le", "format": "wav",
                         "bitrate": 128000, "peak": max(map(abs, normalized)),
                         "rms": math.sqrt(sum(sample * sample for sample in normalized) / len(normalized)),
                         "clipped": False, "clipped_samples": 0, "sample_frames": 1600,
                         "waveform_frames_per_bin": 400, "waveform": waveform}
        self.metadata = {"title": "İstanbul Şarkısı", "kind": "music", "artist": "Örnek Sanatçı",
                         "album": "İlk Albüm", "genre": "Türkçe", "tags": ["yerli", "sabah"],
                         "year": "2026", "comment": "Measured fixture"}

    def tearDown(self):
        self.temp.cleanup()

    def args(self, **changes):
        args = {"station_id": self.sid, "import_path": self.path.name, "source_name": "song.wav",
                "idempotency_key": str(uuid4()), "metadata": deepcopy(self.metadata), "analysis": deepcopy(self.analysis)}
        args.update(changes)
        return args

    def load(self, **changes):
        return api.import_media(self.store, self.principal, **self.args(**changes))

    def markers(self, **changes):
        result = {"cue_in_ms": 0, "intro_ms": 30, "out_ms": 200, "fade_start_ms": 180,
                  "fade_end_ms": 200, "auto_next_ms": 190, "overlap_ms": 10, "automatic": False}
        result.update(changes)
        return result

    def test_staged_batch_has_no_fabricated_analysis_and_finalizes_atomically(self):
        args = self.args()
        args["probe"] = {key: args["analysis"][key] for key in (
            "sha256", "size_bytes", "duration_ms", "sample_rate", "channels", "codec", "format", "bitrate")}
        del args["analysis"]
        args["probe"]["duration_ms"] = 205  # header estimate; decode measures 200 ms
        args.update(source_path="/music/Albüm/Disc 1/song.wav", source_root="/music")
        staged = api.import_staged(self.store, self.principal, **args)
        sequence = self.store.sequence
        self.assertIsNone(staged["analysis"])
        self.assertEqual(self.store.scan("desktop_media_analysis"), [])
        self.assertEqual(staged["media"]["source_relative_path"], "Albüm/Disc 1/song.wav")
        self.assertEqual(api.import_staged(self.store, self.principal, **args), staged)
        self.assertEqual(sequence, self.store.sequence)
        measured = deepcopy(self.analysis)
        measured["rhythm"] = {"schema": 1, "algorithm": "radios-onset-grid-v3", "bpm": 0,
                              "first_beat_ms": 0, "confidence": 0, "analyzed_ms": 200}
        save = dict(station_id=self.sid, media_id=staged["media"]["_id"], version_id=staged["version"]["_id"],
                    expected_revision=1, expected_analysis_revision=0, idempotency_key=str(uuid4()), analysis=measured)
        result = api.save_analysis(self.store, self.principal, **save)
        self.assertEqual(result["version"]["duration_ms"], 200)
        self.assertEqual(result["analysis"]["waveform"], measured["waveform"])
        self.assertEqual(api.save_analysis(self.store, self.principal, **save), result)
        self.assert_public_detail(api.get_media(self.store, self.principal, self.sid, staged["media"]["_id"]), result)
        self.assertEqual(len(self.store.scan("desktop_media_analysis")), 1)

    def test_staged_import_rejects_forged_bytes_and_escaping_provenance(self):
        args = self.args()
        args["probe"] = {key: args["analysis"][key] for key in (
            "sha256", "size_bytes", "duration_ms", "sample_rate", "channels", "codec", "format", "bitrate")}
        del args["analysis"]
        with self.assertRaises(StoreError):
            api.import_staged(self.store, self.principal, **(args | {"source_path": "/other/song.wav", "source_root": "/music"}))
        args["probe"]["sha256"] = "0" * 64
        with self.assertRaises(StoreError):
            api.import_staged(self.store, self.principal, **args)
        self.assertEqual(self.store.scan("desktop_media_library"), [])

    def test_rhythm_reanalysis_cas_replay_and_reopen(self):
        imported = self.load()
        detail = api.get_media(self.store, self.principal, self.sid, imported["media"]["_id"])
        immutable_analysis = self.store.scan("desktop_media_analysis")
        analysis = deepcopy(detail["analysis"])
        analysis["rhythm"] = {"schema": 1, "algorithm": "radios-onset-grid-v1", "bpm": 0,
                              "first_beat_ms": 0, "confidence": 0, "analyzed_ms": 200}
        args = {"station_id": self.sid, "media_id": detail["media"]["_id"],
                "version_id": detail["version"]["_id"], "expected_revision": detail["media"]["_rev"],
                "expected_analysis_revision": detail["analysis_revision"], "idempotency_key": str(uuid4()), "analysis": analysis}
        saved = api.save_analysis(self.store, self.principal, **args)
        sequence = self.store.sequence
        self.assertEqual(api.save_analysis(self.store, self.principal, **args), saved)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(saved["analysis_revision"], detail["analysis_revision"] + 1)
        self.assertEqual(saved["version"], detail["version"])
        self.assertEqual(saved["media"], detail["media"])
        self.assertEqual(self.store.scan("desktop_media_analysis"), immutable_analysis)
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_media(reopened, self.principal, self.sid, detail["media"]["_id"])["analysis"], analysis)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        with self.assertRaises(Conflict):
            api.save_analysis(self.store, self.principal, **dict(args, idempotency_key=str(uuid4())))
        for field, value in (("bpm", 120), ("confidence", float("nan")), ("analyzed_ms", 201), ("schema", 2)):
            invalid = deepcopy(analysis)
            invalid["rhythm"][field] = value
            with self.assertRaises(StoreError):
                api.save_analysis(self.store, self.principal, **dict(args, analysis=invalid, idempotency_key=str(uuid4())))
        wrong_sha = deepcopy(analysis)
        wrong_sha["sha256"] = "a" * 64
        with self.assertRaises(StoreError):
            api.save_analysis(self.store, self.principal, **dict(args, expected_analysis_revision=saved["analysis_revision"], analysis=wrong_sha, idempotency_key=str(uuid4())))
        viewer = create_station_user(self.store, self.principal, self.sid, "rhythm-viewer", "Viewer", ["view"])
        with self.assertRaises(Forbidden):
            api.save_analysis(self.store, authenticate(self.store, viewer["token"]), **dict(args, idempotency_key=str(uuid4())))
        updated = api.save_analysis(self.store, self.principal, **dict(args, expected_analysis_revision=saved["analysis_revision"], idempotency_key=str(uuid4())))
        self.assertEqual(updated["analysis_revision"], saved["analysis_revision"] + 1)
        self.assertEqual(len(self.store.scan("desktop_media_rhythm")), 1)
        self.assertEqual(self.store.scan("desktop_media_analysis"), immutable_analysis)
        self.assertEqual(inspect_integrity(open_radio_store(self.db, read_only=True))["status"], "ok")

    def test_bar_phase_and_manual_grids_replace_legacy_rhythm(self):
        path = self.imports / (str(uuid4()) + ".wav")
        samples = [round(12000 * math.sin(2 * math.pi * 60 * index / 8000)) if index % 4000 < 400 else 0
                   for index in range(40000)]
        with wave.open(str(path), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(8000)
            output.writeframes(struct.pack("<40000h", *samples))
        path.chmod(0o600)
        normalized = [value / 32768 for value in samples]
        waveform = []
        for start in range(0, len(samples), 400):
            chunk = normalized[start:start + 400]
            waveform.extend((min(chunk), max(chunk)))
        analysis = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "size_bytes": path.stat().st_size,
                    "duration_ms": 5000, "sample_rate": 8000, "channels": 1, "codec": "pcm_s16le", "format": "wav",
                    "bitrate": 128000, "peak": max(map(abs, normalized)),
                    "rms": math.sqrt(sum(sample * sample for sample in normalized) / len(normalized)),
                    "clipped": False, "clipped_samples": 0, "sample_frames": 40000,
                    "waveform_frames_per_bin": 400, "waveform": waveform,
                    "rhythm": {"schema": 1, "algorithm": "radios-onset-grid-v1", "bpm": 120.0,
                               "first_beat_ms": 0.0, "confidence": 0.9, "analyzed_ms": 5000}}
        imported = self.load(import_path=path.name, analysis=analysis)
        detail = api.get_media(self.store, self.principal, self.sid, imported["media"]["_id"])
        self.assertEqual(detail["analysis"]["rhythm"]["algorithm"], "radios-onset-grid-v1")
        args = {"station_id": self.sid, "media_id": detail["media"]["_id"], "version_id": detail["version"]["_id"],
                "expected_revision": detail["media"]["_rev"], "expected_analysis_revision": 0}
        legacy = deepcopy(detail["analysis"])
        saved = api.save_analysis(self.store, self.principal, idempotency_key=str(uuid4()), analysis=legacy, **args)
        self.assertEqual(self.store.scan("desktop_media_rhythm")[0]["analyzer"], "radios-onset-grid-v1")
        automatic = deepcopy(legacy)
        automatic["rhythm"] = {"schema": 1, "algorithm": "radios-onset-grid-v2", "bpm": 120.0, "first_beat_ms": 20.0,
                               "confidence": 0.8, "analyzed_ms": 5000, "downbeat_ms": 1520.0}
        automatic["loudness"] = {"schema": 1, "algorithm": "radios-bs1770-v1", "integrated_lufs": -9.5}
        for loudness in ({"schema": 1, "algorithm": "radios-bs1770-v0", "integrated_lufs": -9.5},
                         {"schema": 1, "algorithm": "radios-bs1770-v1", "integrated_lufs": -80},
                         {"schema": 1, "algorithm": "radios-bs1770-v1"}):
            invalid = deepcopy(automatic)
            invalid["loudness"] = loudness
            with self.assertRaises(StoreError):
                api.save_analysis(self.store, self.principal, idempotency_key=str(uuid4()), analysis=invalid,
                                  **dict(args, expected_analysis_revision=saved["analysis_revision"]))
        args["expected_analysis_revision"] = saved["analysis_revision"]
        saved = api.save_analysis(self.store, self.principal, idempotency_key=str(uuid4()), analysis=automatic, **args)
        self.assertEqual(saved["analysis"]["rhythm"], automatic["rhythm"])
        self.assertEqual(saved["analysis"]["loudness"], automatic["loudness"])
        self.assertEqual(self.store.scan("desktop_media_rhythm")[0]["loudness"]["integrated_lufs"], -9.5)
        self.assertEqual(self.store.scan("desktop_media_rhythm")[0]["analyzer"], "radios-onset-grid-v2")
        manual = deepcopy(automatic)
        manual["rhythm"].update(algorithm="radios-manual-grid-v1", bpm=121.5, confidence=1.0, downbeat_ms=520.0)
        for changes in ({"algorithm": "radios-onset-grid-v1"}, {"bpm": 0, "first_beat_ms": 0, "confidence": 0},
                        {"downbeat_ms": 5000.0}, {"downbeat_ms": -1.0}, {"algorithm": "radios-onset-grid-v9"}):
            invalid = deepcopy(manual)
            invalid["rhythm"].update(changes)
            with self.assertRaises(StoreError):
                api.save_analysis(self.store, self.principal, idempotency_key=str(uuid4()), analysis=invalid,
                                  **dict(args, expected_analysis_revision=saved["analysis_revision"]))
        args["expected_analysis_revision"] = saved["analysis_revision"]
        saved = api.save_analysis(self.store, self.principal, idempotency_key=str(uuid4()), analysis=manual, **args)
        stored = self.store.scan("desktop_media_rhythm")
        self.assertEqual(len(stored), 1)
        self.assertEqual(stored[0]["analyzer"], "radios-manual-grid-v1")
        reopened = open_radio_store(self.db, read_only=True)
        reread = api.get_media(reopened, self.principal, self.sid, detail["media"]["_id"])["analysis"]
        self.assertEqual(reread["rhythm"], manual["rhythm"])
        self.assertEqual(reread["loudness"], automatic["loudness"])
        silent = deepcopy(manual)
        silent["loudness"]["integrated_lufs"] = None
        args["expected_analysis_revision"] = saved["analysis_revision"]
        saved = api.save_analysis(self.store, self.principal, idempotency_key=str(uuid4()), analysis=silent, **args)
        self.assertIsNone(saved["analysis"]["loudness"]["integrated_lufs"])
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_real_atomic_catalog_and_managed_bytes(self):
        before = self.store.sequence
        loaded = self.load()
        self.assertEqual(self.store.sequence, before + 1)
        library, version = loaded["media"], loaded["version"]
        media = self.store.get("media_files", library["media_file_id"])
        self.assertEqual(media["status"], "verified")
        self.assertEqual(version["status"], "draft")
        self.assertEqual(self.store.get("items", library["item_id"])["lifecycle"], "draft")
        association = self.store.lookup("station_items", "station_item", self.sid, library["item_id"])[0]
        self.assertFalse(association["eligible"])
        managed = Path(version["managed_path"])
        self.assertEqual(managed.read_bytes(), self.path.read_bytes())
        self.assertEqual(hashlib.sha256(managed.read_bytes()).hexdigest(), self.analysis["sha256"])
        self.assertEqual(media["technical"]["duration_ms"], 200)
        self.assertEqual(len(self.store.scan("people")), 1)
        self.assertEqual(self.store.lookup("contributions", "item", library["item_id"])[0]["role"], "primary_artist")
        self.assertEqual(len(self.store.scan("desktop_media_analysis")), 1)
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")
        self.assertFalse(self.store.scan("playback_marker_sets"))

    def test_durable_idempotent_import_after_source_removed(self):
        args = self.args()
        first = api.import_media(self.store, self.principal, **args)
        self.path.unlink()
        sequence = self.store.sequence
        reopened = open_radio_store(self.db)
        reopened.desktop_import_root = self.imports
        replay = api.import_media(reopened, self.principal, **args)
        self.assertEqual(replay, first)
        self.assertEqual(reopened.sequence, sequence)
        self.assertEqual(len(reopened.scan("items")), 1)
        args["metadata"]["title"] = "Changed"
        with self.assertRaises(Conflict):
            api.import_media(reopened, self.principal, **args)

    def test_failed_commit_preserves_only_reusable_object(self):
        args = self.args()
        before = self.store.sequence
        with patch.object(Transaction, "commit", side_effect=StoreError("injected journal failure")):
            with self.assertRaises(StoreError):
                api.import_media(self.store, self.principal, **args)
        self.assertEqual(self.store.sequence, before)
        for collection in ("items", "versions", "media_files", "people", "desktop_media_library", "desktop_mutations"):
            self.assertEqual(self.store.scan(collection), [])
        objects = [path for path in object_root(self.store).glob("*/*") if len(path.name) == 64]
        self.assertEqual(len(objects), 1)
        retried = api.import_media(self.store, self.principal, **args)
        self.assertEqual(len(self.store.scan("items")), 1)
        self.assertEqual(Path(retried["version"]["managed_path"]), objects[0])
        second = self.load(metadata={**self.metadata, "title": "Alternate catalog credit"})
        self.assertNotEqual(second["media"]["_id"], retried["media"]["_id"])
        self.assertEqual(second["version"]["managed_path"], retried["version"]["managed_path"])

    def test_source_confinement_and_byte_mismatch_no_catalog(self):
        symlink = self.imports / "linked.wav"
        symlink.symlink_to(self.path)
        fifo = self.imports / "pipe.wav"
        os.mkfifo(fifo)
        for name in (str(self.path), "../" + self.path.name, "folder/file.wav", "linked.wav", "pipe.wav"):
            with self.subTest(name=name), self.assertRaises(StoreError):
                self.load(import_path=name)
        for analysis in ({**self.analysis, "sha256": "0" * 64},
                         {**self.analysis, "size_bytes": self.analysis["size_bytes"] + 1}):
            with self.assertRaises(StoreError):
                self.load(analysis=analysis)
        self.assertFalse(self.store.scan("items"))
        self.assertFalse(self.store.scan("desktop_mutations"))
        del self.store.desktop_import_root
        with self.assertRaises(StoreError):
            self.load()

    def test_analysis_rejects_invalid_evidence_atomically(self):
        variants = [{"duration_ms": 250}, {"sample_rate": True}, {"channels": 33},
                    {"sample_rate": 384001}, {"peak": float("nan")}, {"rms": 1},
                    {"waveform": [0, 1]}, {"waveform": [1, -1] * 4},
                    {"clipped": True}, {"clipped_samples": 1601}, {"online": True}]
        before = self.store.sequence
        for changes in variants:
            with self.subTest(changes=changes), self.assertRaises(StoreError):
                self.load(analysis={**self.analysis, **changes})
        self.assertEqual(self.store.sequence, before)
        self.assertFalse(self.store.scan("items"))
        # Floating point WAV can exceed full scale: these level fields record
        # amplitude, not LUFS/true peak. Validation retains real clipping data.
        evidence = {**self.analysis, "peak": 1.25, "rms": 0.8, "clipped": True,
                    "clipped_samples": 2, "waveform": [-1.25, 1.25] * 4}
        self.assertEqual(api._analysis(evidence)["waveform"][1], 1.25)
        self.assertEqual(api.MAX_BYTES, 2 * 1024 * 1024 * 1024)

    def test_metadata_cas_replay_and_managed_catalog_artist(self):
        loaded = self.load()
        media = loaded["media"]
        args = {"station_id": self.sid, "media_id": media["_id"], "expected_revision": media["_rev"],
                "idempotency_key": str(uuid4()), "metadata": {**self.metadata, "title": "Yeni Başlık", "artist": "Yeni Sanatçı", "tags": ["gece"]}}
        updated = api.save_metadata(self.store, self.principal, **args)
        self.assertEqual(updated["media"]["_rev"], 2)
        self.assertEqual(updated["media"]["artist"], "Yeni Sanatçı")
        self.assertEqual(self.store.get("items", media["item_id"])["title"], "Yeni Başlık")
        self.assertEqual(self.store.get("people", updated["media"]["artist_id"])["display_name"], "Yeni Sanatçı")
        original_credit = media["primary_contribution_id"]
        self.assertEqual(updated["media"]["primary_contribution_id"], original_credit)
        credit = self.store.get("contributions", original_credit)
        self.assertEqual(credit["person_id"], updated["media"]["artist_id"])
        self.assertEqual(search_items(self.store, self.principal, self.sid, artist="Yeni Sanatçı")["items"][0]["id"], media["item_id"])
        self.assertFalse(search_items(self.store, self.principal, self.sid, artist=self.metadata["artist"])["items"])
        sequence = self.store.sequence
        self.assertEqual(api.save_metadata(self.store, self.principal, **args), updated)
        self.assertEqual(self.store.sequence, sequence)
        with self.assertRaises(Conflict):
            api.save_metadata(self.store, self.principal, **(args | {"idempotency_key": str(uuid4())}))
        cleared = api.save_metadata(self.store, self.principal, **(args | {"expected_revision": 2,
            "idempotency_key": str(uuid4()), "metadata": {**args["metadata"], "artist": ""}}))
        self.assertIsNone(cleared["media"]["artist_id"])
        self.assertIsNone(cleared["media"]["primary_contribution_id"])
        self.assertIsNone(self.store.get("contributions", original_credit))
        self.assertFalse(search_items(self.store, self.principal, self.sid, artist="Yeni Sanatçı")["items"])
        reopened = open_radio_store(self.db, read_only=True)
        self.assert_public_detail(api.get_media(reopened, self.principal, self.sid, media["_id"]), cleared)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        restored = api.save_metadata(self.store, self.principal, **(args | {"expected_revision": 3,
            "idempotency_key": str(uuid4()), "metadata": args["metadata"]}))
        self.assertNotEqual(restored["media"]["primary_contribution_id"], original_credit)
        self.assertEqual(search_items(self.store, self.principal, self.sid, artist="Yeni Sanatçı")["items"][0]["id"], media["item_id"])
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_artist_global_credit_permissions_approved_guard_and_unmanaged_history(self):
        loaded = self.load()
        native = loaded["media"]
        other = str(uuid4())
        self.store.put("stations", other, {"code": "SHARED", "name": "Shared", "timezone": "UTC", "status": "active"})
        self.store.put("station_items", str(uuid4()), {"station_id": other, "item_id": native["item_id"], "eligible": False})
        editor = create_station_user(self.store, self.principal, self.sid, "local", "Local editor", ["view", "edit"])
        local = authenticate(self.store, editor["token"])
        changed = self.metadata | {"artist": "Changed global artist"}
        before = self.store.sequence
        with self.assertRaises(Forbidden):
            api.save_metadata(self.store, local, self.sid, native["_id"], 1, str(uuid4()), changed)
        self.assertEqual(self.store.sequence, before)
        # Per-station notes remain editable without changing shared identity.
        saved = api.save_metadata(self.store, local, self.sid, native["_id"], 1, str(uuid4()), self.metadata | {"comment": "Local note"})
        version = self.store.get("versions", native["version_id"])
        version_body = api._data(version) | {"status": "approved", "approved_at": api._now(), "approved_by": self.principal.user_id}
        self.store.put("versions", version["_id"], version_body, expected_revision=version["_rev"])
        with self.assertRaises(Conflict):
            api.save_metadata(self.store, self.principal, self.sid, native["_id"], saved["media"]["_rev"], str(uuid4()), changed)
        unmanaged_id = str(uuid4())
        self.store.put("contributions", unmanaged_id, {"item_id": native["item_id"], "person_id": native["artist_id"], "role": "composer", "ordinal": 0})
        with self.assertRaises(StoreError):
            self.store.delete("contributions", unmanaged_id, expected_revision=1)
        self.assertIsNotNone(self.store.get("contributions", unmanaged_id))
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_markers_actual_media_bound_draft_cas_and_reopen(self):
        loaded = self.load()
        args = {"station_id": self.sid, "media_id": loaded["media"]["_id"], "expected_revision": 1,
                "expected_marker_revision": None, "idempotency_key": str(uuid4()), "markers": self.markers()}
        first = api.save_markers(self.store, self.principal, **args)
        self.assertEqual(first["markers"]["status"], "draft")
        self.assertEqual(first["markers"]["media_sha256"], self.analysis["sha256"])
        self.assertEqual(first["markers"]["media_file_id"], loaded["media"]["media_file_id"])
        self.assertEqual(first["media"]["_rev"], 2)
        self.assertEqual(api.save_markers(self.store, self.principal, **args), first)
        second = api.save_markers(self.store, self.principal, **(args | {"expected_revision": 2,
            "expected_marker_revision": 1, "idempotency_key": str(uuid4()), "markers": self.markers(intro_ms=40)}))
        self.assertEqual(second["markers"]["_id"], first["markers"]["_id"])
        self.assertEqual(second["markers"]["_rev"], 2)
        with self.assertRaises(Conflict):
            api.save_markers(self.store, self.principal, **(args | {"idempotency_key": str(uuid4())}))
        reopened = open_radio_store(self.db, read_only=True)
        self.assert_public_detail(api.get_media(reopened, self.principal, self.sid, args["media_id"]), second)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        self.assertFalse(reopened.scan("playback_marker_sets"))
        self.assertEqual(reopened.get("versions", second["version"]["_id"])["status"], "draft")

    def test_invalid_marker_and_metadata_do_not_partially_write(self):
        loaded = self.load()
        before = self.store.sequence
        for changes in ({"out_ms": 201}, {"intro_ms": 201}, {"fade_start_ms": 199, "fade_end_ms": 180},
                        {"auto_next_ms": 201}, {"overlap_ms": 201}, {"cue_in_ms": 200}):
            with self.subTest(changes=changes), self.assertRaises(StoreError):
                api.save_markers(self.store, self.principal, self.sid, loaded["media"]["_id"], 1, None, str(uuid4()), self.markers(**changes))
        for changes in ({"year": "26"}, {"tags": ["a", "a"]}, {"kind": "programme"}, {"online": True}):
            with self.subTest(changes=changes), self.assertRaises(StoreError):
                api.save_metadata(self.store, self.principal, self.sid, loaded["media"]["_id"], 1, str(uuid4()), self.metadata | changes)
        self.assertEqual(self.store.sequence, before)
        self.assertFalse(self.store.scan("desktop_media_markers"))

    def test_autodj_points_are_version_bound_keep_playback_tail_and_survive_legacy_edits(self):
        loaded = self.load()
        args = {"station_id": self.sid, "media_id": loaded["media"]["_id"], "expected_revision": 1,
                "expected_marker_revision": None, "idempotency_key": str(uuid4()),
                "markers": self.markers(autodj_in_ms=20, autodj_out_ms=160)}
        first = api.save_markers(self.store, self.principal, **args)
        self.assertEqual(first["markers"]["out_ms"], 200)
        self.assertEqual(first["markers"]["autodj_out_ms"], 160)
        self.assertEqual(first["markers"]["version_id"], loaded["version"]["_id"])
        self.assertEqual(api.save_markers(self.store, self.principal, **args), first)
        legacy = api.save_markers(self.store, self.principal, **(args | {
            "expected_revision": 2, "expected_marker_revision": 1, "idempotency_key": str(uuid4()),
            "markers": self.markers(intro_ms=40)}))
        self.assertEqual(legacy["markers"]["autodj_in_ms"], 20)
        self.assertEqual(legacy["markers"]["autodj_out_ms"], 160)
        reopened = open_radio_store(self.db, read_only=True)
        self.assert_public_detail(api.get_media(reopened, self.principal, self.sid, loaded["media"]["_id"]), legacy)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        cleared = api.save_markers(self.store, self.principal, **(args | {
            "expected_revision": 3, "expected_marker_revision": 2, "idempotency_key": str(uuid4()),
            "markers": self.markers(autodj_in_ms=None, autodj_out_ms=None)}))
        self.assertIsNone(cleared["markers"]["autodj_in_ms"])
        self.assertIsNone(cleared["markers"]["autodj_out_ms"])
        self.assertEqual(cleared["markers"]["out_ms"], 200)

    def test_autodj_point_bounds_single_points_and_cue_changes_are_atomic(self):
        loaded = self.load()
        for changes in ({"autodj_in_ms": -1}, {"autodj_out_ms": 201}, {"autodj_in_ms": True},
                        {"autodj_out_ms": 50.5}, {"autodj_in_ms": "50"}, {"autodj_in_ms": 200},
                        {"autodj_out_ms": 0}, {"autodj_in_ms": 100, "autodj_out_ms": 100},
                        {"autodj_in_ms": 150, "autodj_out_ms": 50}, {"cue_in_ms": 40, "autodj_in_ms": 20}):
            sequence = self.store.sequence
            with self.subTest(changes=changes), self.assertRaises(StoreError):
                api.save_markers(self.store, self.principal, self.sid, loaded["media"]["_id"], 1, None, str(uuid4()), self.markers(**changes))
            self.assertEqual(self.store.sequence, sequence)
        first = api.save_markers(self.store, self.principal, self.sid, loaded["media"]["_id"],
            1, None, str(uuid4()), self.markers(autodj_out_ms=160))
        self.assertNotIn("autodj_in_ms", first["markers"])
        sequence = self.store.sequence
        with self.assertRaises(StoreError):
            api.save_markers(self.store, self.principal, self.sid, loaded["media"]["_id"],
                2, 1, str(uuid4()), self.markers(out_ms=150, fade_start_ms=140, fade_end_ms=150,
                auto_next_ms=145))
        self.assertEqual(self.store.sequence, sequence)
        with self.assertRaises(Conflict):
            api.save_markers(self.store, self.principal, self.sid, loaded["media"]["_id"],
                1, None, str(uuid4()), self.markers(autodj_in_ms=20))

    def test_tenant_permission_scope_and_read_no_secrets(self):
        loaded = self.load()
        viewer = create_station_user(self.store, self.principal, self.sid, "viewer", "Viewer", ["view"])
        view_principal = authenticate(self.store, viewer["token"])
        self.assertEqual(len(api.list_media(self.store, view_principal, self.sid)["records"]), 1)
        with self.assertRaises(Forbidden):
            api.import_media(self.store, view_principal, **self.args())
        editor = create_station_user(self.store, self.principal, self.sid, "editor", "Editor", ["view", "edit"])
        with self.assertRaises(Forbidden):
            api.import_media(self.store, authenticate(self.store, editor["token"]), **self.args())
        endpoint_token = issue_endpoint_token(self.store, self.principal, self.sid, self.identity["endpoint_id"])
        with self.assertRaises(Forbidden):
            api.list_media(self.store, authenticate(self.store, endpoint_token), self.sid)
        other = str(uuid4())
        self.store.put("stations", other, {"code": "OTHER", "name": "Other", "timezone": "UTC", "status": "active"})
        with self.assertRaises(Forbidden):
            api.get_media(self.store, view_principal, other, loaded["media"]["_id"])
        with self.assertRaises(NotFound):
            api.get_media(self.store, self.principal, other, loaded["media"]["_id"])
        response = json.dumps(api.get_media(self.store, view_principal, self.sid, loaded["media"]["_id"]))
        for secret in (self.identity["api_token"], viewer["token"], "token_hash"):
            self.assertNotIn(secret, response)

    def test_search_fields_turkish_filter_and_bounded_list(self):
        one = self.load()
        two = self.load(metadata={**self.metadata, "title": "Haber", "kind": "news", "artist": "İstanbul"})
        self.assertEqual(api.list_media(self.store, self.principal, self.sid, query="istanbul", field="title")["records"][0]["media"]["_id"], one["media"]["_id"])
        self.assertEqual(api.list_media(self.store, self.principal, self.sid, query="istanbul", field="artist")["records"][0]["media"]["_id"], two["media"]["_id"])
        self.assertEqual(api.list_media(self.store, self.principal, self.sid, kind="news")["records"][0]["media"]["_id"], two["media"]["_id"])
        page = api.list_media(self.store, self.principal, self.sid, limit=1)
        self.assertEqual(page["total"], 2)
        self.assertEqual(page["next_offset"], 1)
        self.assertNotIn("waveform", page["records"][0]["analysis"])
        self.assertNotIn("managed_path", page["records"][0]["version"])
        last = api.list_media(self.store, self.principal, self.sid, limit=1, offset=page["next_offset"])
        self.assertIsNone(last["next_offset"])
        with self.assertRaises(StoreError):
            api.list_media(self.store, self.principal, self.sid, field="password")

    def test_tampered_or_missing_object_never_has_readable_path(self):
        loaded = self.load()
        path = Path(loaded["version"]["managed_path"])
        original = path.read_bytes()
        path.write_bytes(b"corrupt")
        damaged = api.get_media(self.store, self.principal, self.sid, loaded["media"]["_id"])
        self.assertIsNone(damaged["version"]["managed_path"])
        self.assertFalse(damaged["version"]["object_available"])
        self.assertEqual(damaged["analysis"]["sha256"], self.analysis["sha256"])
        path.write_bytes(original)
        self.assertTrue(api.get_media(self.store, self.principal, self.sid, loaded["media"]["_id"])["version"]["object_available"])
        path.unlink()
        path.symlink_to(self.path)
        self.assertIsNone(api.get_media(self.store, self.principal, self.sid, loaded["media"]["_id"])["version"]["managed_path"])
        path.unlink()
        path.write_bytes(original)
        prefix = path.parent
        moved = prefix.with_name(prefix.name + ".actual")
        prefix.rename(moved)
        prefix.symlink_to(moved, target_is_directory=True)
        self.assertIsNone(api.get_media(self.store, self.principal, self.sid, loaded["media"]["_id"])["version"]["managed_path"])

    def test_unicode_heavy_real_catalog_has_bounded_pages(self):
        # Seed many valid alternate catalog credits of the same measured object
        # in one fixture transaction. Every reference and SHA is real; this
        # avoids 110 separate import/audit/replay operations in a response test.
        loaded = self.load()
        original = loaded["media"]
        old_media = self.store.get("media_files", original["media_file_id"])
        old_item = self.store.get("items", original["item_id"])
        old_version = self.store.get("versions", original["version_id"])
        old_location = self.store.lookup("media_locations", "file", old_media["_id"])[0]
        old_analysis = self.store.lookup("desktop_media_analysis", "media", old_media["_id"])[0]
        data = lambda row: {key: deepcopy(value) for key, value in row.items() if not key.startswith("_")}
        tx = self.store.begin(actor=self.principal.user_id)
        for index in range(110):
            item_id, version_id, file_id, library_id, contribution_id = (str(uuid4()) for _ in range(5))
            tx.put("items", item_id, data(old_item) | {"title": "曲" * 200})
            tx.put("versions", version_id, data(old_version) | {"item_id": item_id})
            tx.put("station_items", str(uuid4()), {"station_id": self.sid, "item_id": item_id, "eligible": False})
            tx.put("media_files", file_id, data(old_media) | {"version_id": version_id})
            tx.put("media_locations", str(uuid4()), data(old_location) | {"media_file_id": file_id})
            binding = {"item_id": item_id, "version_id": version_id, "media_file_id": file_id}
            library = data(original)
            library.pop("title")
            library.pop("kind")
            library.update(binding | {"comment": "🎙" * 2000, "album": "🎶" * 200,
                                      "tags": ["🎼" * 78 + f"{tag:02d}" for tag in range(32)],
                                      "primary_contribution_id": contribution_id})
            tx.put("desktop_media_library", library_id, library)
            tx.put("desktop_media_analysis", str(uuid4()), data(old_analysis) | binding)
            tx.put("contributions", contribution_id, {"item_id": item_id, "person_id": library["artist_id"],
                   "role": "primary_artist", "ordinal": 0, "desktop_media_library_id": library_id})
        tx.commit()
        seen, offset, pages = set(), 0, 0
        while True:
            page = api.list_media(self.store, self.principal, self.sid, offset=offset)
            self.assertLess(len(json.dumps(page, ensure_ascii=False).encode()), 1600000)
            self.assertEqual(page["total"], 111)
            self.assertTrue(page["records"])
            for row in page["records"]:
                self.assertNotIn("waveform", row["analysis"])
                self.assertNotIn(row["media"]["_id"], seen)
                seen.add(row["media"]["_id"])
            pages += 1
            if page["next_offset"] is None:
                break
            self.assertGreater(page["next_offset"], offset)
            offset = page["next_offset"]
        self.assertGreater(pages, 1)
        self.assertEqual(len(seen), 111)

    def test_http_auth_404_409_422_and_durable_import(self):
        base = make_handler(self.store)
        class Handler(base):
            def log_message(self, *args):
                pass
        server = BoundedHTTPServer(("127.0.0.1", 0), Handler, request_timeout=3)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        def request(action, body, auth=True):
            connection = HTTPConnection("127.0.0.1", server.server_port, timeout=3)
            headers = {"Content-Type": "application/json", "Connection": "close"}
            if auth:
                headers["Authorization"] = "Bearer " + self.identity["api_token"]
            connection.request("POST", "/v1/workflows/desktop-media/" + action, json.dumps(body), headers)
            response = connection.getresponse()
            result = response.status, json.loads(response.read())
            connection.close()
            return result
        try:
            self.assertEqual(request("list", {"station_id": self.sid}, False)[0], 403)
            self.assertEqual(request("get", {"station_id": self.sid, "media_id": str(uuid4())})[0], 404)
            args = self.args()
            status, loaded = request("import", args)
            self.assertEqual(status, 200)
            self.assertEqual(request("import", args)[1], loaded)
            self.assertEqual(request("import", args | {"source_name": "different.wav"})[0], 409)
            metadata = {"station_id": self.sid, "media_id": loaded["media"]["_id"], "expected_revision": 1,
                        "idempotency_key": str(uuid4()), "metadata": self.metadata | {"comment": "HTTP edit"}}
            self.assertEqual(request("metadata-save", metadata)[0], 200)
            self.assertEqual(request("metadata-save", metadata | {"idempotency_key": str(uuid4())})[0], 409)
            self.assertEqual(request("import", self.args(import_path="../unsafe.wav"))[0], 422)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=4)


if __name__ == "__main__":
    unittest.main(verbosity=2)
