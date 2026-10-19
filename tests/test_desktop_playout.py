"""Isolated real-byte preparation, binding security and local-history contracts."""

import copy
from http.client import HTTPConnection
import json
from pathlib import Path
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4

import test_desktop_api as fixtures
from sdbase import desktop_api as api, desktop_media, desktop_playlist as playlists, desktop_playout as playback
from sdbase.desktop_probability_policy import validate_policy_context
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.catalog import NotFound
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError


class DesktopPlayout(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal

    def prepare(self, kind="playlist", **fields):
        return playback.prepare(self.store, self.principal, self.sid, kind, **fields)

    def create(self, name="Yerel liste"):
        return playlists.create_playlist(self.store, self.principal, self.sid, name, idempotency_key=str(uuid4()))

    @staticmethod
    def metadata(binding=None, title="Kullanıcı taslağı"):
        return {"title": title, "artist": "Metadata sanatçısı", "album": "Metadata albümü",
                "duration_ms": 999999, "bpm": 123.5, "genre": "Pop", "key": "Am",
                "location": "/external/path/not-audio.wav", "comment": "Yerel taslak", **(binding or {})}

    @staticmethod
    def online(provider="youtube", binding=None):
        external_id, url = {
            "youtube": ("dQw4w9WgXcQ", "https://music.youtube.com/watch?v=dQw4w9WgXcQ"),
            "spotify": ("4uLU6hMCjMI75M1A2tKUQC", "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"),
            "apple_music": ("1594677759", "https://music.apple.com/tr/song/1594677759")}[provider]
        return {"provider": provider, "external_id": external_id, "url": url,
                **({"analysis_media": binding} if binding is not None else {})}

    def save(self, playlist, binding=None, **fields):
        return playlists.save_entry(self.store, self.principal, self.sid, playlist["_id"],
                                    self.metadata(binding), expected_revision=playlist["_rev"],
                                    idempotency_key=str(uuid4()), **fields)

    @staticmethod
    def programme(binding=None, **extra):
        event = {"id": str(uuid4()), "title": "Yerel öğe", "artist": "", "album": "", "kind": "music",
                 "start_time": "08:30", "duration_ms": 500, "version_label": "v1",
                 "primary_file": "/user/unverified.wav", "backup_file": "", "note": "", **(binding or {}), **extra}
        return {"title": "Mevcut program", "date": "2026-10-06", "note": "", "events": [event]}

    @staticmethod
    def automation(playlist_id):
        return {"name": "Yerel rotasyon", "seed": "deterministic-test", "planDate": "2026-10-06",
                "startTime": "23:30", "windowStart": "22:00", "windowEnd": "06:00",
                "sourceMode": 0, "trackMode": 1, "transition": 2, "boundaryTransition": 3,
                "deckMode": 0, "fallbackMode": 2, "trackGap": 60, "artistGap": 30, "albumGap": 10,
                "previewCount": 20, "jingleEvery": 3, "overlap": 3, "boundaryOverlap": 1,
                "includeProgramme": True, "jingleEnabled": True, "jingleDry": True,
                "requireMetadata": False, "jinglePlaylist": playlist_id, "fallbackPlaylist": playlist_id,
                "jingleDeck": "32", "weekdays": [1, 2, 3, 4, 5, 6, 7], "decks": ["1", "2"],
                "sources": [{"source_id": str(uuid4()), "playlist_id": playlist_id, "weight": 10,
                             "count": 2, "start": "23:00", "end": "05:00", "days": 0}]}

    def put(self, kind, name, config, **extra):
        return api.put_document(self.store, self.principal, self.sid, kind, name, config, str(uuid4()), **extra)

    def test_unbound_drafts_selection_order_and_no_side_effects(self):
        playlist = self.create()
        for _ in range(3):
            playlist = self.save(playlist)
        ids = [row["_id"] for row in playlist["tracks"]]
        sequence = self.store.sequence
        result = self.prepare(playlist_id=playlist["_id"], entry_ids=[ids[2], ids[0]])
        self.assertEqual([row["entry_id"] for row in result["tracks"]], [ids[0], ids[2]])
        self.assertEqual([row["ordinal"] for row in result["tracks"]], [0, 2])
        self.assertTrue(all(not row["eligible"] and row["reasons"] == ["media_unbound"] and row["detail"] is None
                            for row in result["tracks"]))
        self.assertEqual(result["playback_scope"], "local_draft")
        self.assertEqual(result["sequence"], sequence)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.prepare(playlist_id=playlist["_id"], entry_ids=[])["tracks"], [])
        for selection in ([str(uuid4())], [ids[0], ids[0]], "wrong"):
            with self.assertRaises(StoreError):
                self.prepare(playlist_id=playlist["_id"], entry_ids=selection)
        self.assertFalse(self.store.scan("playout_events"))
        self.assertFalse(self.store.scan("plan_revisions"))

    def test_bound_entry_cas_replay_identity_and_reopen_remain_draft(self):
        binding = self.fixture.verified_binding()
        playlist = self.create()
        payload = {"expected_revision": playlist["_rev"], "idempotency_key": str(uuid4())}
        playlist = playlists.save_entry(self.store, self.principal, self.sid, playlist["_id"], self.metadata(binding), **payload)
        sequence = self.store.sequence
        self.assertEqual(playlists.save_entry(self.store, self.principal, self.sid, playlist["_id"], self.metadata(binding), **payload), playlist)
        self.assertEqual(self.store.sequence, sequence)
        entry = playlist["tracks"][0]
        changed = self.save(playlist, binding, entry_id=entry["_id"], expected_entry_revision=entry["_rev"])
        self.assertEqual(changed["tracks"][0]["_id"], entry["_id"])
        with self.assertRaises(Conflict):
            self.save(playlist, binding, entry_id=entry["_id"], expected_entry_revision=entry["_rev"])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        prepared = playback.prepare(reopened, self.principal, self.sid, "playlist", playlist_id=playlist["_id"])
        self.assertTrue(prepared["tracks"][0]["eligible"])
        self.assertEqual(changed["tracks"][0]["media_state"], "unverified")
        self.assertEqual(reopened.get("versions", binding["version_id"])["status"], "draft")
        self.assertFalse(reopened.lookup("station_items", "station", self.sid)[0]["eligible"])
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_partial_mismatched_foreign_and_forged_bindings_are_rejected(self):
        binding = self.fixture.verified_binding()
        playlist = self.create()
        other = str(uuid4())
        self.store.put("stations", other, {"code": "OTHER", "name": "Other", "timezone": "UTC", "status": "active"})
        wrongs = [dict(binding, version_id=str(uuid4())), dict(binding, media_file_id=str(uuid4())),
                  dict(binding, media_sha256="F" * 64), {"media_id": binding["media_id"]}]
        for wrong in wrongs:
            sequence = self.store.sequence
            with self.assertRaises(StoreError):
                self.save(playlist, wrong)
            with self.assertRaises(StoreError):
                self.put("programme", "Mevcut program", self.programme(wrong))
            self.assertEqual(self.store.sequence, sequence)
        with self.assertRaises(StoreError):
            api.put_document(self.store, self.principal, other, "programme", "Foreign", self.programme(binding), str(uuid4()))
        with self.assertRaises(StoreError):
            playback.prepare(self.store, self.principal, other, "programme", config=self.programme(binding))

    def test_real_detail_measurement_markers_and_waveform_budget(self):
        binding = self.fixture.verified_binding()
        playlist = self.save(self.create(), binding)
        original = desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"])
        markers = {"cue_in_ms": 20, "intro_ms": 40, "out_ms": 180, "fade_start_ms": 160,
                   "fade_end_ms": 180, "auto_next_ms": 160, "overlap_ms": 20, "automatic": True}
        desktop_media.save_markers(self.store, self.principal, self.sid, binding["media_id"],
                                  original["media"]["_rev"], None, str(uuid4()), markers)
        result = self.prepare(playlist_id=playlist["_id"])
        row, = result["tracks"]
        self.assertTrue(row["eligible"])
        self.assertEqual(row["metadata"]["duration_ms"], 200)
        self.assertEqual(row["metadata"]["title"], "Gerçek DJ sampler")
        self.assertEqual(row["metadata"]["kind"], "jingle")
        self.assertEqual(row["metadata"]["bpm"], 123.5)
        self.assertEqual(row["detail"]["analysis"]["sample_frames"], 1600)
        self.assertNotIn("waveform", row["detail"]["analysis"])
        self.assertEqual({key: row["detail"]["markers"][key] for key in markers}, markers)
        self.assertTrue(Path(row["detail"]["version"]["managed_path"]).is_file())
        self.assertNotEqual(row["detail"]["version"]["managed_path"], row["metadata"]["location"])
        self.assertIn("waveform", desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"])["analysis"])

    def test_retired_online_sources_are_rejected_before_any_write(self):
        binding = self.fixture.verified_binding()
        sequence, state, stored_bytes = self.store.sequence, copy.deepcopy(self.store._state), self.fixture.db.read_bytes()
        for entries in ([self.metadata(binding), self.metadata(self.online())],
                        [self.metadata(self.online("spotify", binding))],
                        [self.metadata({"analysis_media": binding})]):
            with self.subTest(entries=entries), self.assertRaisesRegex(StoreError, "no longer supported"):
                playlists.create_playlist(self.store, self.principal, self.sid, "Sağlayıcı kaynağı",
                                          idempotency_key=str(uuid4()), entries=entries)
        source = self.online()
        config = self.programme(online_provider=source["provider"], online_id=source["external_id"],
                                online_url=source["url"], primary_file="", backup_file="")
        with self.assertRaisesRegex(StoreError, "no longer supported"):
            self.put("programme", "Sağlayıcı programı", config)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store._state, state)
        self.assertEqual(self.fixture.db.read_bytes(), stored_bytes)
        self.assertFalse(self.store.scan("desktop_playlist_entries"))

    def test_prepare_reads_real_detail_in_mixed_local_playlist_and_automation_read_only(self):
        binding = self.fixture.verified_binding()
        entries = [self.metadata(binding), self.metadata(), self.metadata(binding, "İkinci doğrulanmış")]
        playlist = playlists.create_playlist(self.store, self.principal, self.sid,
            "Karışık yerel kaynaklar", idempotency_key=str(uuid4()), entries=entries)
        viewer = create_station_user(self.store, self.principal, self.sid,
                                     "provider-viewer", "Viewer", ["view"])
        principal = authenticate(self.store, viewer["token"])
        sequence, state, stored_bytes = self.store.sequence, copy.deepcopy(self.store._state), self.fixture.db.read_bytes()
        reopened = open_radio_store(self.fixture.db, read_only=True)
        for kind, fields in (("playlist", {"playlist_id": playlist["_id"]}),
                             ("automation", {"config": self.automation(playlist["_id"])})):
            with self.subTest(kind=kind):
                result = playback.prepare(reopened, principal, self.sid, kind, **fields)
                sources = [result] if kind == "playlist" else result["sources"]
                for source in sources:
                    self.assertEqual([row["entry_id"] for row in source["tracks"]],
                                     [row["_id"] for row in playlist["tracks"]])
                    self.assertEqual([row["eligible"] for row in source["tracks"]], [True, False, True])
                    self.assertEqual(source["tracks"][1]["reasons"], ["media_unbound"])
                    self.assertIsNone(source["tracks"][1]["detail"])
                    for row in (source["tracks"][0], source["tracks"][2]):
                        self.assertTrue(row["media_eligible"])
                        self.assertEqual(row["reasons"], [])
                        self.assertEqual(row["detail"]["media"]["_id"], binding["media_id"])
                        self.assertEqual(row["detail"]["version"]["_id"], binding["version_id"])
                        self.assertEqual(row["detail"]["version"]["sha256"], binding["media_sha256"])
                        self.assertEqual(row["detail"]["analysis"]["sample_frames"], 1600)
                        self.assertNotIn("waveform", row["detail"]["analysis"])
                        self.assertTrue(Path(row["detail"]["version"]["managed_path"]).is_file())
                        self.assertEqual(row["metadata"]["duration_ms"], 200)
                self.assertEqual(result["sequence"], sequence)
        with self.assertRaises(Forbidden):
            playback.prepare(reopened, principal, str(uuid4()), "playlist", playlist_id=playlist["_id"])
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store._state, state)
        self.assertEqual(self.fixture.db.read_bytes(), stored_bytes)
        self.assertEqual(playlists.list_playlists(reopened, principal, self.sid)["playlists"], [playlist])
        self.assertEqual(reopened.get("versions", binding["version_id"])["status"], "draft")
        self.assertFalse(reopened.lookup("station_items", "station", self.sid)[0]["eligible"])
        self.assertFalse(reopened.scan("playout_events"))
        self.assertFalse(reopened.scan("plan_revisions"))
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_online_programme_id_remains_an_unbound_provider_event(self):
        source = self.online()
        config = self.programme(online_provider=source["provider"], online_id=source["external_id"],
                                online_url=source["url"], primary_file="", backup_file="")
        original = copy.deepcopy(config)
        sequence, state, stored_bytes = self.store.sequence, copy.deepcopy(self.store._state), self.fixture.db.read_bytes()
        prepared = self.prepare("programme", config=config)
        row, = prepared["tracks"]
        self.assertEqual(row["entry_id"], config["events"][0]["id"])
        self.assertFalse(row["eligible"])
        self.assertFalse(row["media_eligible"])
        self.assertEqual(row["reasons"], ["media_unbound"])
        self.assertIsNone(row["detail"])
        self.assertEqual(prepared["config"], original)
        self.assertEqual(config, original)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store._state, state)
        self.assertEqual(self.fixture.db.read_bytes(), stored_bytes)

    def test_policy_context_prepares_and_reopens_without_mutation(self):
        binding = self.fixture.verified_binding()
        playlist = self.save(self.create("Kaynak playlist"), binding)
        origin = playlist["tracks"][0]["_id"]
        context = {"automation_id": str(uuid4()), "playlist_id": playlist["_id"],
                   "entry_id": origin, "source_id": str(uuid4())}
        policy = {"schema": 1, "transitions": [{"id": str(uuid4()), "mode": 3,
            "overlap_ms": 500, "early_seconds": .1, "effect": "lowpass", "probability": 100}]}
        config = self.programme(binding, policy_context=context, probability_policy=policy)
        original = copy.deepcopy(config)
        sequence, state, stored_bytes = self.store.sequence, copy.deepcopy(self.store._state), self.fixture.db.read_bytes()
        prepared = self.prepare("programme", config=config)
        row, = prepared["tracks"]
        self.assertTrue(row["eligible"])
        self.assertEqual(row["entry_id"], config["events"][0]["id"])
        self.assertNotEqual(row["entry_id"], origin)
        self.assertEqual(row["policy_context"], context)
        self.assertEqual(row["probability_policy"], policy)
        self.assertEqual(row["detail"]["version"]["sha256"], binding["media_sha256"])
        self.assertEqual(row["detail"]["analysis"]["sample_frames"], 1600)
        self.assertTrue(Path(row["detail"]["version"]["managed_path"]).is_file())
        self.assertEqual(config, original)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store._state, state)
        self.assertEqual(self.fixture.db.read_bytes(), stored_bytes)
        saved = self.put("programme", "Seçilen program", config)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        sequence, stored_bytes = reopened.sequence, self.fixture.db.read_bytes()
        result = playback.prepare(reopened, self.principal, self.sid, "programme", config=saved["config"])
        self.assertTrue(result["tracks"][0]["eligible"])
        self.assertEqual(result["tracks"][0]["policy_context"], context)
        self.assertEqual(reopened.sequence, sequence)
        self.assertEqual(self.fixture.db.read_bytes(), stored_bytes)
        stored_playlist = next(value for value in playlists.list_playlists(reopened, self.principal, self.sid)["playlists"]
                               if value["_id"] == playlist["_id"])
        self.assertEqual(stored_playlist, playlist)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_policy_context_keeps_binding_and_tenant_fences(self):
        binding = self.fixture.verified_binding()
        other_binding = self.fixture.verified_binding()
        playlist = self.save(self.create("Bağlam sınırları"), binding)
        origin = playlist["tracks"][0]
        context = {"playlist_id": playlist["_id"], "entry_id": origin["_id"]}
        config = self.programme(binding, policy_context=context)
        sequence, state, stored_bytes = self.store.sequence, copy.deepcopy(self.store._state), self.fixture.db.read_bytes()
        invalid_configs = []
        for key in api.MEDIA_BINDING_KEYS:
            wrong = copy.deepcopy(config)
            wrong["events"][0][key] = "0" * 64 if key == "media_sha256" else other_binding[key]
            invalid_configs.append(wrong)
        for key in ("playlist_id", "entry_id"):
            wrong = copy.deepcopy(config); wrong["events"][0]["policy_context"][key] = str(uuid4())
            invalid_configs.append(wrong)
        wrong = copy.deepcopy(config); wrong["events"][0]["policy_context"]["extra"] = str(uuid4())
        invalid_configs.append(wrong)
        for wrong in invalid_configs:
            with self.subTest(config=wrong), self.assertRaises(StoreError):
                self.prepare("programme", config=wrong)
        online = {"provider": "youtube", "external_id": "dQw4w9WgXcQ",
                  "url": "https://music.youtube.com/watch?v=dQw4w9WgXcQ"}
        # Both fixtures hold identical bytes, so only the identities differ; the
        # digest is covered by an explicit mismatch below.
        bad_sources = [dict(origin, **{key: other_binding[key]}) for key in api.MEDIA_BINDING_KEYS - {"media_sha256"}]
        bad_sources.extend((dict(origin, media_sha256="0" * 64), dict(origin, **online),
                            {key: value for key, value in origin.items() if key != "media_id"},
                            dict({key: value for key, value in origin.items() if key not in binding}, **online,
                                 analysis_media=dict(binding, media_sha256="0" * 64))))
        for bad_source in bad_sources:
            altered = copy.deepcopy(state)
            altered["desktop_playlist_entries"][origin["_id"]]["data"] = bad_source
            for persisted in (False, True):
                with self.subTest(source=bad_source, persisted=persisted), self.assertRaises(StoreError):
                    validate_policy_context(context, config["events"][0], self.sid, altered, persisted=persisted)
        # A legacy provider row whose analysis bytes match keeps working for stored
        # documents, while a new document can no longer point at it.
        legacy = dict({key: value for key, value in origin.items() if key not in binding}, **online, analysis_media=binding)
        altered = copy.deepcopy(state)
        altered["desktop_playlist_entries"][origin["_id"]]["data"] = legacy
        with self.assertRaisesRegex(StoreError, "no longer supported"):
            validate_policy_context(context, config["events"][0], self.sid, altered, persisted=False)
        validate_policy_context(context, config["events"][0], self.sid, altered, persisted=True)
        for collection, identifier in (("desktop_playlist_entries", origin["_id"]),
                                       ("desktop_media_library", binding["media_id"])):
            altered = copy.deepcopy(state)
            altered[collection][identifier]["data"]["station_id"] = str(uuid4())
            for persisted in (False, True):
                with self.subTest(foreign_collection=collection, persisted=persisted), self.assertRaises(StoreError):
                    validate_policy_context(context, config["events"][0], self.sid, altered, persisted=persisted)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store._state, state)
        self.assertEqual(self.fixture.db.read_bytes(), stored_bytes)

    def test_prepare_rechecks_binding_shape_version_and_tenant_without_writes(self):
        binding = self.fixture.verified_binding()
        playlist = self.save(self.create(), binding)
        row = playlist["tracks"][0]
        other = str(uuid4())
        self.store.put("stations", other,
                       {"code": "PREPARE-OTHER", "name": "Other", "timezone": "UTC", "status": "active"})
        library = self.store.get("desktop_media_library", binding["media_id"])
        self.store.put("station_items", str(uuid4()),
                       {"station_id": other, "item_id": library["item_id"], "eligible": False})
        foreign_id = str(uuid4())
        self.store.put("desktop_media_library", foreign_id,
                       {key: value for key, value in library.items() if not key.startswith("_")}
                       | {"station_id": other})
        online = {"provider": "youtube", "external_id": "dQw4w9WgXcQ",
                  "url": "https://music.youtube.com/watch?v=dQw4w9WgXcQ"}
        rows = [dict(row, media_id=foreign_id), dict(row, version_id=str(uuid4())),
                dict(row, media_file_id=str(uuid4())), dict(row, media_sha256="0" * 64),
                dict(row, **online), dict(row, analysis_media=binding),
                dict({key: value for key, value in row.items() if key not in binding}, **online,
                     analysis_media=dict(binding, media_id=foreign_id))]
        sequence, state, stored_bytes = self.store.sequence, copy.deepcopy(self.store._state), self.fixture.db.read_bytes()
        for candidate in rows:
            with self.subTest(candidate=candidate), patch.object(playlists, "_tracks", return_value=[candidate]):
                with self.assertRaises(StoreError):
                    self.prepare(playlist_id=playlist["_id"])
            self.assertEqual(self.store.sequence, sequence)
            self.assertEqual(self.store._state, state)
            self.assertEqual(self.fixture.db.read_bytes(), stored_bytes)

    def test_missing_changed_disabled_and_unverified_media_stay_ineligible(self):
        binding = self.fixture.verified_binding()
        playlist = self.save(self.create(), binding)
        original = desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"])
        path = Path(original["version"]["managed_path"])
        content = path.read_bytes()
        sequence, state, stored_bytes = self.store.sequence, copy.deepcopy(self.store._state), self.fixture.db.read_bytes()
        for missing in (False, True):
            with self.subTest(missing=missing):
                if missing:
                    path.unlink()
                else:
                    path.write_bytes(content[:-1] + bytes([content[-1] ^ 1]))
                row, = self.prepare(playlist_id=playlist["_id"])["tracks"]
                self.assertFalse(row["eligible"])
                self.assertEqual(row["reasons"], ["media_object_unavailable"])
                self.assertIsNone(row["detail"]["version"]["managed_path"])
                self.assertEqual(self.store.sequence, sequence)
                self.assertEqual(self.store._state, state)
                self.assertEqual(self.fixture.db.read_bytes(), stored_bytes)
        path.write_bytes(content)
        media = self.store.get("media_files", binding["media_file_id"])
        self.store.put("media_files", media["_id"],
                       {key: value for key, value in media.items() if not key.startswith("_")} | {"status": "error"},
                       expected_revision=media["_rev"])
        sequence = self.store.sequence
        row, = self.prepare(playlist_id=playlist["_id"])["tracks"]
        self.assertFalse(row["eligible"])
        self.assertIn("media_unverified", row["reasons"])
        self.assertIsNone(row["detail"]["version"]["managed_path"])
        self.assertEqual(self.store.sequence, sequence)
        library = self.store.get("desktop_media_library", binding["media_id"])
        self.store.put("desktop_media_library", library["_id"],
                       {key: value for key, value in library.items() if not key.startswith("_")} | {"status": "disabled"},
                       expected_revision=library["_rev"])
        sequence = self.store.sequence
        row, = self.prepare(playlist_id=playlist["_id"])["tracks"]
        self.assertEqual(row["reasons"], ["media_inactive"])
        self.assertIsNone(row["detail"])
        self.assertEqual(self.store.sequence, sequence)

    def test_prepare_keeps_copyright_decisions_and_draft_rights(self):
        binding = self.fixture.verified_binding()
        playlist = self.save(self.create(), binding)
        self.put("copyright_policy", "main", {"schema": 1, "restrict": True,
                                              "unregistered": "deny", "rules": []})
        sequence = self.store.sequence
        row, = self.prepare(playlist_id=playlist["_id"])["tracks"]
        self.assertTrue(row["media_eligible"])
        self.assertFalse(row["eligible"])
        self.assertEqual(row["reasons"], ["copyright_unregistered"])
        self.assertFalse(row["copyright"]["allowed"])
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store.get("versions", binding["version_id"])["status"], "draft")
        self.assertFalse(self.store.lookup("station_items", "station", self.sid)[0]["eligible"])

    def test_autodj_marker_save_reopen_prepare_legacy_edit_and_clear(self):
        binding = self.fixture.verified_binding()
        playlist = self.save(self.create(), binding)
        original = desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"])
        markers = {"cue_in_ms": 20, "intro_ms": 50, "out_ms": 180, "fade_start_ms": 160,
                   "fade_end_ms": 180, "auto_next_ms": 160, "overlap_ms": 20, "automatic": False,
                   "autodj_in_ms": 40, "autodj_out_ms": 140}
        saved = desktop_media.save_markers(self.store, self.principal, self.sid, binding["media_id"],
            original["media"]["_rev"], None, str(uuid4()), markers)

        def assert_prepared(store, expected):
            sequence = store.sequence
            for kind, fields in (("playlist", {"playlist_id": playlist["_id"]}),
                                 ("programme", {"config": self.programme(binding)}),
                                 ("automation", {"config": self.automation(playlist["_id"])})):
                with self.subTest(kind=kind, marker_revision=expected["_rev"]):
                    prepared = playback.prepare(store, self.principal, self.sid, kind, **fields)
                    rows = prepared["tracks"] if kind != "automation" else [
                        row for source in prepared["sources"] for row in source["tracks"]]
                    self.assertTrue(rows)
                    for row in rows:
                        self.assertTrue(row["eligible"])
                        actual = row["detail"]["markers"]
                        self.assertEqual(actual, expected)
                        self.assertEqual(actual["version_id"], binding["version_id"])
                        self.assertEqual(actual["media_file_id"], binding["media_file_id"])
                        self.assertEqual(actual["media_sha256"], binding["media_sha256"])
                        # AutoDJ OUT triggers a transition; the verified physical
                        # tail remains available and the formal cue path is untouched.
                        self.assertEqual(actual["out_ms"], 180)
                    self.assertEqual(prepared["sequence"], sequence)
            self.assertEqual(store.sequence, sequence)

        assert_prepared(self.store, saved["markers"])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        assert_prepared(reopened, saved["markers"])
        legacy = {key: value for key, value in markers.items() if not key.startswith("autodj_")}
        legacy["intro_ms"] = 60
        saved = desktop_media.save_markers(self.store, self.principal, self.sid, binding["media_id"],
            saved["media"]["_rev"], saved["markers"]["_rev"], str(uuid4()), legacy)
        self.assertEqual(saved["markers"]["autodj_in_ms"], 40)
        self.assertEqual(saved["markers"]["autodj_out_ms"], 140)
        assert_prepared(self.store, saved["markers"])
        cleared = desktop_media.save_markers(self.store, self.principal, self.sid, binding["media_id"],
            saved["media"]["_rev"], saved["markers"]["_rev"], str(uuid4()),
            {**legacy, "autodj_in_ms": None, "autodj_out_ms": None})
        self.assertIsNone(cleared["markers"]["autodj_in_ms"])
        self.assertIsNone(cleared["markers"]["autodj_out_ms"])
        assert_prepared(self.store, cleared["markers"])
        self.assertFalse(self.store.scan("radio_cues"))
        self.assertFalse(self.store.scan("playback_marker_sets"))

    def test_programme_ten_kinds_timing_and_unsaved_copy_are_durable(self):
        binding = self.fixture.verified_binding()
        config = self.programme(binding, kind="voice_track", start_policy="finish", max_wait_ms=30000)
        config["timing"] = {"event_id": config["events"][0]["id"], "start_time": "08:00", "target_time": "08:30",
                            "policy": "finish", "max_wait_ms": 30000}
        saved = self.put("programme", "Mevcut program", config)
        prepared = self.prepare("programme")
        self.assertEqual(prepared["config"], config)
        self.assertEqual(prepared["programme"]["revision"], saved["_rev"])
        self.assertTrue(prepared["tracks"][0]["eligible"])
        config["title"] = "Kaydedilmemiş yeni başlık"
        sequence = self.store.sequence
        unsaved = self.prepare("programme", config=config)
        config["title"] = "Çağırıcı değişikliği"
        self.assertEqual(unsaved["config"]["title"], "Kaydedilmemiş yeni başlık")
        self.assertEqual(self.store.sequence, sequence)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(playback.prepare(reopened, self.principal, self.sid, "programme")["config"], saved["config"])
        for kind in ("music", "announcement", "advertisement", "jingle", "program", "sweeper", "news", "effect", "voice_track", "other"):
            self.assertEqual(self.prepare("programme", config=self.programme(kind=kind))["config"]["events"][0]["kind"], kind)

    def test_invalid_timing_calendar_and_numeric_fields_do_not_write(self):
        original = self.programme()
        invalid = [dict(original, date="2026-02-30")]
        timing = {"event_id": original["events"][0]["id"], "start_time": "08:00", "target_time": "08:30",
                  "policy": "cut", "max_wait_ms": 0}
        for key, value in (("event_id", str(uuid4())), ("start_time", "24:00"), ("target_time", "1:00"),
                           ("policy", "live"), ("max_wait_ms", True), ("max_wait_ms", 600001)):
            invalid.append(dict(original, timing=dict(timing, **{key: value})))
        for key, value in (("start_policy", "now"), ("max_wait_ms", -1), ("max_wait_ms", True), ("duration_ms", True)):
            config = copy.deepcopy(original)
            config["events"][0][key] = value
            invalid.append(config)
        for config in invalid:
            sequence = self.store.sequence
            with self.assertRaises(StoreError):
                self.put("programme", "Mevcut program", config)
            with self.assertRaises(StoreError):
                self.prepare("programme", config=config)
            self.assertEqual(self.store.sequence, sequence)

    def test_automation_roles_current_programme_history_and_utc_semantics(self):
        binding = self.fixture.verified_binding()
        playlist = self.save(self.create(), binding)
        programme = self.put("programme", "Mevcut program", self.programme(binding))
        history = {"kind": "local_playback", "entries": [{"media_id": binding["media_id"], "version_id": binding["version_id"],
                   "artist": "", "album": "", "started_utc": "2026-10-06T08:30:00+03:00", "position_frames": 1}]}
        saved_history = self.put("playout_history", "Yerel oynatma geçmişi", history)
        station = self.store.get("stations", self.sid)
        self.store.put("stations", self.sid, {key: ("Europe/Istanbul" if key == "timezone" else value)
                                             for key, value in station.items() if not key.startswith("_")}, expected_revision=station["_rev"])
        config = self.automation(playlist["_id"])
        saved = self.put("automation", "main", config)
        sequence = self.store.sequence
        result = self.prepare("automation")
        self.assertEqual(result["config"], saved["config"])
        self.assertEqual(result["timezone"], "UTC")
        self.assertEqual(result["station_timezone"], "Europe/Istanbul")
        self.assertEqual([row["role"] for row in result["sources"]], ["music", "jingle", "fallback"])
        self.assertTrue(all(row["source_name"] == playlist["name"] and row["tracks"][0]["eligible"] for row in result["sources"]))
        self.assertEqual(result["programme"]["document_id"], programme["_id"])
        self.assertEqual(result["history"]["revision"], saved_history["_rev"])
        self.assertEqual(result["history"]["config"], history)
        self.assertEqual(result["sequence"], sequence)
        self.assertEqual(self.store.sequence, sequence)
        self.assertFalse(self.store.scan("playout_events"))

    def test_automation_history_ignores_per_player_checkpoints(self):
        playlist = self.create()
        config = self.automation(playlist["_id"])
        config["includeProgramme"] = False
        empty = {"kind": "local_playback", "entries": []}
        self.put("playout_history", "Oynatıcı 1 oynatma geçmişi", empty)
        self.put("playout_history", "Oynatıcı 32 oynatma geçmişi", empty)
        self.put("playout_history", "Oynatıcı 2 AutoDJ geçmişi", empty)
        self.put("playout_history", "Oynatıcı 31 AutoDJ geçmişi", empty)
        self.assertIsNone(self.prepare("automation", config=config)["history"])
        station = self.put("playout_history", "Yerel oynatma geçmişi", empty)
        self.assertEqual(self.prepare("automation", config=config)["history"]["document_id"], station["_id"])
        self.put("playout_history", "Oynatıcı 33 oynatma geçmişi", empty)
        with self.assertRaises(Conflict):
            self.prepare("automation", config=config)

    def test_prepare_rejects_noncanonical_decks_preserving_legacy_documents(self):
        playlist = self.create()
        config = self.automation(playlist["_id"])
        config["decks"] = ["native-card-1"]
        legacy = self.put("automation", "main", config)
        for key, value in (("decks", ["native-card-1"]), ("decks", ["0"]), ("decks", ["33"]),
                           ("decks", ["01"]), ("jingleDeck", "preview")):
            bad = dict(self.automation(playlist["_id"]), **{key: value})
            with self.assertRaises(StoreError):
                self.prepare("automation", config=bad)
        with self.assertRaises(StoreError):
            self.prepare("automation")
        self.assertEqual(api.get_document(self.store, self.principal, self.sid, "automation", legacy["_id"]), legacy)
        valid = self.prepare("automation", config=self.automation(playlist["_id"]))
        self.assertNotIn("deckTransitions", valid["config"])
        self.assertEqual(valid["diagnostics"], [{"code": "programme_missing"}])

    def test_autodj_deck_transitions_durable_cas_and_prepare(self):
        playlist = self.create()
        config = self.automation(playlist["_id"])
        policies = [{"deck_id": "1", "transition": 1, "overlap": 1.25},
                    {"deck_id": "2", "transition": 0, "overlap": 0}]
        config["deckTransitions"] = policies
        saved = self.put("automation", "main", config)
        self.assertEqual(saved["config"]["deckTransitions"], policies)
        sequence = self.store.sequence
        self.assertEqual(self.prepare("automation")["config"]["deckTransitions"], policies)
        self.assertEqual(self.store.sequence, sequence)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(playback.prepare(reopened, self.principal, self.sid, "automation")["config"]["deckTransitions"], policies)
        updated = copy.deepcopy(config)
        updated["deckTransitions"][0]["overlap"] = 2.5
        revised = self.put("automation", "main", updated, document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertGreater(revised["_rev"], saved["_rev"])
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            self.put("automation", "main", config, document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.prepare("automation")["config"], revised["config"])
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_autodj_deck_policy_rejects_invalid_settings_without_writes(self):
        playlist = self.create()
        valid = {"deck_id": "1", "transition": 1, "overlap": 1}
        invalid = [None, {}, [None], [valid, valid], [valid] * 33]
        invalid.extend([dict(valid, deck_id=identifier)] for identifier in ("0", "01", "3", "33", "preview"))
        invalid.extend([dict(valid, transition=mode)] for mode in (-1, 4, True, 1.0, "1"))
        invalid.extend([dict(valid, overlap=overlap)] for overlap in (-1, 30.1, True, "1", float("nan")))
        invalid.extend([[dict(valid, transition=0)], [dict(valid, transition=3)], [dict(valid, extra=0)]])
        sequence = self.store.sequence
        for policies in invalid:
            config = self.automation(playlist["_id"])
            config["deckTransitions"] = policies
            with self.subTest(policies=policies):
                with self.assertRaises(StoreError):
                    self.put("automation", "main", config)
                with self.assertRaises(StoreError):
                    self.prepare("automation", config=config)
                self.assertEqual(self.store.sequence, sequence)

    def test_changed_missing_symlink_and_disabled_media_are_ineligible(self):
        binding = self.fixture.verified_binding()
        playlist = self.save(self.create(), binding)
        detail = desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"])
        path = Path(detail["version"]["managed_path"])
        bytes_before = path.read_bytes()
        path.write_bytes(bytes_before[:-1] + bytes([bytes_before[-1] ^ 1]))
        row, = self.prepare(playlist_id=playlist["_id"])["tracks"]
        self.assertFalse(row["eligible"])
        self.assertEqual(row["reasons"], ["media_object_unavailable"])
        self.assertIsNone(row["detail"]["version"]["managed_path"])
        path.unlink()
        self.assertFalse(self.prepare(playlist_id=playlist["_id"])["tracks"][0]["eligible"])
        source = Path(self.fixture.temp.name) / "original.wav"
        source.write_bytes(bytes_before)
        path.symlink_to(source)
        self.assertFalse(self.prepare(playlist_id=playlist["_id"])["tracks"][0]["eligible"])
        path.unlink()
        path.write_bytes(bytes_before)
        library = self.store.get("desktop_media_library", binding["media_id"])
        self.store.put("desktop_media_library", library["_id"], {key: ("disabled" if key == "status" else value)
                       for key, value in library.items() if not key.startswith("_")}, expected_revision=library["_rev"])
        row, = self.prepare(playlist_id=playlist["_id"])["tracks"]
        self.assertEqual(row["reasons"], ["media_inactive"])
        self.assertIsNone(row["detail"])
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_history_typed_cas_replay_permission_reopen_and_bounds(self):
        binding = self.fixture.verified_binding()
        entry = {"media_id": binding["media_id"], "version_id": binding["version_id"], "artist": "", "album": "",
                 "started_utc": "2026-10-06T08:30:00Z", "position_frames": 17}
        config = {"kind": "local_playback", "entries": [entry]}
        key = str(uuid4())
        saved = api.put_document(self.store, self.principal, self.sid, "playout_history", "History", config, key)
        sequence = self.store.sequence
        self.assertEqual(api.put_document(self.store, self.principal, self.sid, "playout_history", "History", config, key), saved)
        self.assertEqual(self.store.sequence, sequence)
        revised = self.put("playout_history", "History", {"kind": "local_playback", "entries": [dict(entry, position_frames=48)]},
                           document_id=saved["_id"], expected_revision=saved["_rev"])
        with self.assertRaises(Conflict):
            self.put("playout_history", "History", config, document_id=saved["_id"], expected_revision=saved["_rev"])
        rotation = {"schema": 1, "config_key": "a1" * 32, "source_index": 49, "in_turn": 500,
                    "cursors": [2000] * 50, "fallback_cursor": 2000, "jingle_cursor": 2000,
                    "rng_hex": "0123456789abcdef", "complete_seen": [str(uuid4()) for _ in range(500)],
                    "jingle_phase": 100, "jingle_due": True}
        rotated = {"kind": "local_playback", "entries": [dict(entry, started_utc="2026-10-06T08:30:00.123456Z")], "rotation": rotation}
        revised = self.put("playout_history", "History", rotated,
                           document_id=revised["_id"], expected_revision=revised["_rev"])
        self.assertEqual(revised["config"], rotated)
        empty = dict(rotation, cursors=[], complete_seen=[], source_index=0, in_turn=0)
        self.put("playout_history", "Empty checkpoint", {"kind": "local_playback", "entries": [], "rotation": empty})
        invalid_rotations = [None, {}, dict(rotation, extra=1)]
        for field, value in (("schema", True), ("schema", 2), ("config_key", "A" * 64),
                             ("config_key", "a" * 63), ("source_index", 50), ("source_index", True),
                             ("in_turn", 501), ("in_turn", -1), ("cursors", [0] * 51),
                             ("jingle_phase", True), ("jingle_phase", 101), ("jingle_phase", -1), ("jingle_due", 1),
                             ("cursors", [True]), ("cursors", [2001]), ("cursors", "0"),
                             ("fallback_cursor", -1), ("jingle_cursor", 2001),
                             ("rng_hex", "ABCDEF0123456789"), ("rng_hex", "0" * 15),
                             ("complete_seen", [rotation["complete_seen"][0]] * 2),
                             ("complete_seen", [str(uuid4()) for _ in range(501)]),
                             ("complete_seen", [rotation["complete_seen"][0].upper()]),
                             ("complete_seen", ["not-an-entry"])):
            invalid_rotations.append(dict(rotation, **{field: value}))
        for wrong in invalid_rotations:
            with self.subTest(rotation=wrong):
                sequence = self.store.sequence
                with self.assertRaises(StoreError):
                    self.put("playout_history", "Bad checkpoint", dict(config, rotation=wrong))
                self.assertEqual(self.store.sequence, sequence)
        invalid = [{"kind": "on_air", "entries": [entry]}, {"kind": "local_playback", "entries": [entry] * 2001}]
        for key, value in (("media_id", str(uuid4())), ("version_id", str(uuid4())), ("started_utc", "2026-10-06T08:00:00"),
                           ("position_frames", True), ("position_frames", 0), ("position_frames", 4_147_200_001), ("approved", True)):
            invalid.append({"kind": "local_playback", "entries": [dict(entry, **{key: value})]})
        for wrong in invalid:
            sequence = self.store.sequence
            with self.assertRaises(StoreError):
                self.put("playout_history", "Bad", wrong)
            self.assertEqual(self.store.sequence, sequence)
        viewer = create_station_user(self.store, self.principal, self.sid, "viewer", "Viewer", ["view"])
        with self.assertRaises(Forbidden):
            api.put_document(self.store, authenticate(self.store, viewer["token"]), self.sid, "playout_history", "History", config, str(uuid4()))
        other = str(uuid4())
        self.store.put("stations", other, {"code": "HISTORY-OTHER", "name": "Other", "timezone": "UTC", "status": "active"})
        with self.assertRaises(StoreError):
            api.put_document(self.store, self.principal, other, "playout_history", "History", config, str(uuid4()))
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "playout_history", saved["_id"]), revised)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_candidate_and_response_budgets_and_invalid_kind_arguments(self):
        config = self.programme()
        event = config["events"][0]
        config["events"] = [dict(event, id=str(uuid4())) for _ in range(2001)]
        with patch.object(playback, "MAX_CANDIDATES", 2000), self.assertRaisesRegex(StoreError, "2000 candidate"):
            self.prepare("programme", config=config)
        # The shipped budget accepts an archive-sized queue in one preparation.
        self.assertEqual(len(self.prepare("programme", config=config)["tracks"]), 2001)
        config["events"] = [dict(event, id=str(uuid4()), primary_file="x" * 4096, note="n" * 2000) for _ in range(150)]
        with patch.object(playback, "MAX_RESPONSE_BYTES", 1_500_000), self.assertRaisesRegex(StoreError, "response budget"):
            self.prepare("programme", config=config)
        for kind, kwargs in (("invalid", {}), ("playlist", {}), ("programme", {"playlist_id": str(uuid4())}),
                             ("automation", {"entry_ids": []}), ("playlist", {"playlist_id": str(uuid4()), "config": {}})):
            with self.assertRaises(StoreError):
                self.prepare(kind, **kwargs)
        self.assertFalse(self.store.scan("desktop_mutations"))

    def test_programme_view_window_settings_prepare_unchanged_and_reject_invalid(self):
        view = {"event_fetch": 12, "event_show": 6, "content_fetch": 40, "content_show": 20, "refresh_seconds": 5}
        by_title = {"Sabah": dict(view, event_show=3), "Gece": dict(view, content_fetch=400, content_show=400)}
        config = self.programme(self.fixture.verified_binding()) | {"view": view, "view_by_title": by_title}
        saved = self.put("programme", "Mevcut program", config)
        prepared = self.prepare("programme")
        self.assertEqual(prepared["config"], config)
        self.assertEqual(prepared["config"]["view"], view)
        self.assertEqual(prepared["config"]["view_by_title"], by_title)
        self.assertEqual(prepared["programme"]["revision"], saved["_rev"])
        self.assertTrue(prepared["tracks"][0]["eligible"])
        sequence = self.store.sequence
        self.assertEqual(self.prepare("programme", config=config)["config"]["view"], view)
        self.assertEqual(self.store.sequence, sequence)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(playback.prepare(reopened, self.principal, self.sid, "programme")["config"], config)
        for mutate in (lambda c: c["view"].update(event_show=13), lambda c: c["view"].pop("refresh_seconds"),
                       lambda c: c["view_by_title"].update({"": view}), lambda c: c.update(view_by_title=[view])):
            broken = copy.deepcopy(config); mutate(broken)
            with self.assertRaises(StoreError):
                self.prepare("programme", config=broken)
            with self.assertRaises(StoreError):
                self.put("programme", "Mevcut program", broken, document_id=saved["_id"], expected_revision=saved["_rev"])
            self.assertEqual(self.store.sequence, sequence)

    def test_programme_window_prepares_selected_rows_and_keeps_the_whole_programme(self):
        config = self.programme()
        event = config["events"][0]
        config["events"] = [dict(event, id=str(uuid4()), title=f"Parça {index}") for index in range(600)]
        ids = [row["id"] for row in config["events"]]
        saved = self.put("programme", "Mevcut program", config)
        sequence = self.store.sequence
        # Request order does not matter: rows come back in programme order with
        # their programme-wide ordinals, and the whole programme travels along.
        result = self.prepare("programme", config=config, entry_ids=ids[120:125] + ids[:1])
        self.assertEqual([row["entry_id"] for row in result["tracks"]], ids[:1] + ids[120:125])
        self.assertEqual([row["ordinal"] for row in result["tracks"]], [0, 120, 121, 122, 123, 124])
        self.assertEqual(result["config"], config)
        self.assertEqual(len(result["config"]["events"]), 600)
        self.assertEqual(result["programme"]["tracks"], result["tracks"])
        stored = self.prepare("programme", entry_ids=ids[598:])
        self.assertEqual([row["ordinal"] for row in stored["tracks"]], [598, 599])
        self.assertEqual((stored["programme"]["document_id"], stored["programme"]["revision"]), (saved["_id"], saved["_rev"]))
        self.assertEqual(stored["config"], saved["config"])
        self.assertEqual(len(self.prepare("programme", config=config, entry_ids=ids[:500])["tracks"]), 500)
        self.assertEqual(self.prepare("programme", config=config, entry_ids=[])["tracks"], [])
        with self.assertRaises(StoreError):
            self.prepare("programme", config=config, entry_ids=ids[:501])
        with self.assertRaises(StoreError):
            self.prepare("programme", config=config, entry_ids=[ids[0], ids[0]])
        with self.assertRaises(Conflict):
            self.prepare("programme", config=config, entry_ids=[str(uuid4())])
        self.assertEqual(self.store.sequence, sequence)
        # The byte budget still covers the whole programme carried with a small window.
        config["events"] = [dict(event, id=str(uuid4()), primary_file="x" * 4096, note="n" * 2000) for _ in range(150)]
        with patch.object(playback, "MAX_RESPONSE_BYTES", 800_000), self.assertRaisesRegex(StoreError, "response budget"):
            self.prepare("programme", config=config, entry_ids=[config["events"][0]["id"]])
        self.assertEqual(len(self.prepare("programme", config=config, entry_ids=[config["events"][0]["id"]])["tracks"]), 1)
        self.assertEqual(self.store.sequence, sequence)

    def test_programme_empty_blocks_prepare_without_rows_and_keep_block_order(self):
        def block(kind, title):
            return {"id": str(uuid4()), "kind": kind, "title": title, "start_time": "", "note": ""}
        music, empty, closing = block("music", "Müzik"), block("announcement", "İçeriksiz anons"), block("music", "Kapanış")
        config = self.programme(block_id=music["id"])
        config["blocks"] = [music, empty, closing]
        config["events"].append(dict(config["events"][0], id=str(uuid4()), block_id=closing["id"]))
        saved = self.put("programme", "Mevcut program", config)
        for result in (self.prepare("programme"), self.prepare("programme", config=config)):
            self.assertEqual(result["config"]["blocks"], config["blocks"])
            self.assertEqual([row["ordinal"] for row in result["tracks"]], [0, 1])
            self.assertEqual([row["entry_id"] for row in result["tracks"]], [row["id"] for row in config["events"]])
        window = self.prepare("programme", entry_ids=[config["events"][1]["id"]])
        self.assertEqual([row["ordinal"] for row in window["tracks"]], [1])
        self.assertEqual(window["config"], saved["config"])
        only_blocks = dict(config, events=[])
        self.assertEqual(self.prepare("programme", config=only_blocks)["tracks"], [])
        self.assertEqual(self.prepare("programme", config=only_blocks)["config"]["blocks"], config["blocks"])

    @unittest.skipUnless("live" in api.PROGRAMME_KINDS, "4c's live programme block kind is not in PROGRAMME_KINDS yet")
    def test_programme_live_block_prepares_without_rows_and_keeps_its_live_object(self):
        def block(kind, title, **extra):
            return {"id": str(uuid4()), "kind": kind, "title": title, "start_time": "", "note": "", **extra}
        live = {"label": "talk", "channels": [3], "automation_id": "", "end_rule": "manual", "duration_ms": 0}
        music, talk, closing = block("music", "Müzik"), block("live", "Konuşma", live=live), block("music", "Kapanış")
        config = self.programme(block_id=music["id"])
        config["blocks"] = [music, talk, closing]
        config["events"].append(dict(config["events"][0], id=str(uuid4()), block_id=closing["id"]))
        saved = self.put("programme", "Mevcut program", config)
        self.assertEqual(saved["config"]["blocks"][1]["live"], live)
        for result in (self.prepare("programme"), self.prepare("programme", config=config)):
            self.assertEqual(result["config"]["blocks"][1], talk)
            self.assertEqual(result["config"]["blocks"][1]["live"], live)
            self.assertEqual([row["ordinal"] for row in result["tracks"]], [0, 1])
        window = self.prepare("programme", entry_ids=[config["events"][1]["id"]])
        self.assertEqual(window["config"]["blocks"][1]["live"], live)
        self.assertEqual([row["ordinal"] for row in window["tracks"]], [1])

    def test_real_concurrent_writer_cannot_mix_prepared_sequences(self):
        binding = self.fixture.verified_binding()
        playlist = self.save(self.create("Önceki kaynak adı"), binding)
        sequence = self.store.sequence
        writer_store = open_radio_store(self.fixture.db)
        entered, release, writer_started = threading.Event(), threading.Event(), threading.Event()
        results, errors = [], []
        original = desktop_media._managed_path
        def verify(store, media):
            path = original(store, media)
            entered.set()
            if not release.wait(5):
                raise RuntimeError("test synchronization timed out")
            return path
        def read():
            try:
                results.append(self.prepare(playlist_id=playlist["_id"]))
            except BaseException as exc:
                errors.append(exc)
        def write():
            writer_started.set()
            try:
                playlists.rename_playlist(writer_store, self.principal, self.sid, playlist["_id"], "Sonraki kaynak adı",
                                          expected_revision=playlist["_rev"], idempotency_key=str(uuid4()))
            except BaseException as exc:
                errors.append(exc)
        with patch.object(desktop_media, "_managed_path", side_effect=verify):
            reader = threading.Thread(target=read, daemon=True)
            writer = threading.Thread(target=write, daemon=True)
            reader.start()
            try:
                self.assertTrue(entered.wait(5))
                writer.start()
                self.assertTrue(writer_started.wait(5))
            finally:
                release.set()
                reader.join(5)
                if writer.ident:
                    writer.join(5)
        self.assertFalse(reader.is_alive())
        self.assertFalse(writer.is_alive())
        self.assertFalse(errors, errors)
        self.assertEqual(results[0]["sequence"], sequence)
        self.assertEqual(results[0]["source_name"], "Önceki kaynak adı")
        self.assertTrue(results[0]["tracks"][0]["eligible"])
        self.assertGreater(writer_store.sequence, sequence)
        self.assertEqual(self.prepare(playlist_id=playlist["_id"])["source_name"], "Sonraki kaynak adı")

    def test_authenticated_http_exact_allowlist_tenant_and_token_free_response(self):
        binding = self.fixture.verified_binding()
        playlist = self.save(self.create(), binding)
        viewer = create_station_user(self.store, self.principal, self.sid, "http-view", "View", ["view"])
        server = BoundedHTTPServer(("127.0.0.1", 0), make_handler(self.store), max_connections=4)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 3)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        def request(token=None, **body):
            connection = HTTPConnection(*server.server_address, timeout=5)
            headers = {"Content-Type": "application/json"}
            if token:
                headers["Authorization"] = "Bearer " + token
            connection.request("POST", "/v1/workflows/desktop-playout/prepare",
                               json.dumps({"station_id": self.sid, "kind": "playlist", "playlist_id": playlist["_id"], **body}), headers)
            response = connection.getresponse()
            status, result = response.status, json.loads(response.read())
            connection.close()
            return status, result
        self.assertEqual(request()[0], 403)
        self.assertEqual(request("wrong-token-value-with-length")[0], 403)
        status, result = request(viewer["token"])
        self.assertEqual(status, 200)
        self.assertTrue(result["tracks"][0]["eligible"])
        self.assertEqual(request(viewer["token"], station_id=str(uuid4()))[0], 403)
        self.assertEqual(request(self.fixture.identity["api_token"], online=True)[0], 422)
        encoded = json.dumps(result)
        self.assertNotIn(viewer["token"], encoded)
        self.assertNotIn(self.fixture.identity["api_token"], encoded)


if __name__ == "__main__":
    unittest.main()
