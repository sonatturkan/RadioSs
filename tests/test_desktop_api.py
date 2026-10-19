#!/usr/bin/env python3
"""Isolated native configuration, traffic, tenancy, CAS and HTTP acceptance."""
import copy
from datetime import datetime, timedelta, timezone
import hashlib
from http.client import HTTPConnection
import json
import math
from pathlib import Path
import sys
import struct
import tempfile
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "database/SDBASE/src"))
from sdbase import desktop_api as api
from sdbase import desktop_playlist
from sdbase import desktop_media
from sdbase import desktop_playout as playout
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.catalog import NotFound
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate, bootstrap, issue_endpoint_token
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError


def dj_configuration():
    effect = {"type": 0, "enabled": False, "depth": 0.5, "send_db": 0,
              "return_db": -6, "monitor_path": 1, "tempo_bpm": 120}
    channels = [{"source_deck": index, "eq_low_db": 0, "eq_mid_db": 0, "eq_high_db": 0,
                 "pan": 0, "gain_db": 0, "mute": False, "solo": False,
                 "crossfade_group": 0, "fx_slot": -1, "fx_send_db": 0, "fx_return_db": -6}
                for index in range(32)]
    return {"channels": channels,
            "master": {"eq_low_db": 0, "eq_mid_db": 0, "eq_high_db": 0, "mute": False, "fx_slot": -1,
                       "fx_send_db": 0, "fx_return_db": -6},
            "effects": [copy.deepcopy(effect) for _ in range(5)], "master_fx": copy.deepcopy(effect),
            "crossfader": 0, "speech": {"ducking": True, "bed_db": -18}}


def audio_preferences_configuration():
    return {"main_sink": "@DEFAULT_SINK@", "cue_sink": "", "monitor_sink": "",
            "microphone_source": "", "main_gain_db": 0, "cue_gain_db": -12,
            "monitor_gain_db": -12, "microphone_gain_db": 0,
            "cue_source": -1, "monitor_source": -1}


def loudness_policy_configuration():
    return {"schema_version": 1, "players": list(range(1, 33)),
            "contexts": ["manual", "automation", "autodj"], "phase": "always",
            "automation_mode": "all", "automation_ids": [], "pairs_mode": "all", "pairs": [],
            "schedule_enabled": False, "timezone": "UTC", "windows": []}


class DesktopAPI(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="desktop-api-", dir=ROOT / "build")
        self.db = Path(self.temp.name) / "test.sdb"
        self.store = open_radio_store(self.db)
        self.identity = bootstrap(self.store, "admin", "TEST", "Test station")
        self.sid = self.identity["station_id"]
        self.principal = authenticate(self.store, self.identity["api_token"])

    def tearDown(self):
        self.temp.cleanup()

    def put(self, kind="channel", config=None, **extra):
        return api.put_document(self.store,self.principal,self.sid,kind,"Native",config or {"deck_count":6},str(uuid4()),**extra)

    def test_cas_and_reopen(self):
        old = self.put()
        new = self.put(config={"deck_count":7},document_id=old["_id"],expected_revision=old["_rev"])
        with self.assertRaises(Conflict):
            self.put(config={"deck_count":8},document_id=old["_id"],expected_revision=old["_rev"])
        reopened = open_radio_store(self.db,read_only=True)
        self.assertEqual(api.get_document(reopened,self.principal,self.sid,"channel",old["_id"]),new)
        self.assertEqual(inspect_integrity(reopened)["status"],"ok")

    def test_disabled_automation_references_allow_unrelated_settings_only(self):
        playlist = desktop_playlist.create_playlist(self.store, self.principal, self.sid,
            "Otomasyon kaynağı", idempotency_key=str(uuid4()))
        config = {"name": "Otomasyon", "seed": "1", "planDate": "2026-10-13",
            "startTime": "09:00", "windowStart": "00:00", "windowEnd": "23:59",
            "sourceMode": 0, "trackMode": 0, "transition": 0, "boundaryTransition": 0,
            "deckMode": 0, "fallbackMode": 2, "trackGap": 0, "artistGap": 0,
            "albumGap": 0, "previewCount": 10, "jingleEvery": 2, "overlap": 0,
            "boundaryOverlap": 0, "includeProgramme": False, "jingleEnabled": True,
            "jingleDry": False, "requireMetadata": False, "jinglePlaylist": playlist["_id"],
            "fallbackPlaylist": playlist["_id"], "jingleDeck": None, "weekdays": [1],
            "decks": [], "sources": [{"source_id": str(uuid4()), "playlist_id": playlist["_id"],
                "weight": 1, "count": 1, "start": "00:00", "end": "23:59", "days": 0}]}
        saved = self.put("automation", config)
        desktop_playlist.delete_playlist(self.store, self.principal, self.sid, playlist["_id"],
            expected_revision=playlist["_rev"], idempotency_key=str(uuid4()))
        changed = copy.deepcopy(config); changed["previewCount"] = 20
        actual = self.put("automation", changed, document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(actual["config"], changed)
        bad = copy.deepcopy(changed); bad["sources"][0]["count"] = 2
        with self.assertRaises(StoreError):
            self.put("automation", bad, document_id=actual["_id"], expected_revision=actual["_rev"])
        with self.assertRaises(StoreError):
            self.put("automation", changed)
        disabled = copy.deepcopy(changed); disabled["jingleEnabled"] = False
        disabled["fallbackMode"] = 0
        stopped = self.put("automation", disabled, document_id=actual["_id"], expected_revision=actual["_rev"])
        with self.assertRaises(StoreError):
            self.put("automation", changed, document_id=stopped["_id"], expected_revision=stopped["_rev"])

    def test_output_observation_links_to_unfinished_emergency_attempt(self):
        from sdbase import emergency_alerts, playout, station_outputs
        endpoint_id = self.identity["endpoint_id"]
        station_outputs.create_station_output(self.store, self.principal, self.sid,
            endpoint_id, "MAIN", geo_codes=["LOCAL"])
        now = datetime.now(timezone.utc)
        alert = emergency_alerts.register_alert(self.store, self.principal, self.sid,
            source_namespace="regression", external_alert_key="unfinished-attempt",
            purpose="test", starts_at=now - timedelta(minutes=1),
            ends_at=now + timedelta(minutes=10), geo_codes=["LOCAL"], output_ids=["MAIN"],
            tts_text="Deneme yayını", tts_language="tr")
        attempt = emergency_alerts.start_alert_attempt(self.store, self.principal, self.sid,
            endpoint_id, alert["_id"], attempt_key="regression-attempt", output_id="MAIN",
            geo_code="LOCAL", started_at=now, tts_capable=True, authorize_test=True)
        token = issue_endpoint_token(self.store, self.principal, self.sid, endpoint_id)
        endpoint = authenticate(self.store, token)
        observation = playout.record_output_observation(self.store, endpoint, self.sid,
            endpoint_id, "emergency-signal", "audio_present", now, details={"output_id": "MAIN"})
        linked = emergency_alerts.link_output_observation(self.store, self.principal, self.sid,
            endpoint_id, attempt["_id"], observation["observation_id"])
        self.assertEqual(linked["attempt_id"], attempt["_id"])
        self.assertEqual(self.store.get("emergency_alert_attempts", attempt["_id"])["state"], "started")
        self.assertEqual(inspect_integrity(open_radio_store(self.db, read_only=True))["status"], "ok")

    def test_replay_and_conflicting_key(self):
        key = str(uuid4())
        a = api.put_document(self.store,self.principal,self.sid,"channel","Decks",{"deck_count":6},key)
        seq = self.store.sequence
        b = api.put_document(self.store,self.principal,self.sid,"channel","Decks",{"deck_count":6},key)
        self.assertEqual(a,b)
        self.assertEqual(self.store.sequence,seq)
        with self.assertRaises(Conflict):
            api.put_document(self.store,self.principal,self.sid,"channel","Decks",{"deck_count":7},key)

    def test_tenants_permissions_and_no_secrets(self):
        viewer = create_station_user(self.store,self.principal,self.sid,"viewer","Viewer",["view"])
        principal = authenticate(self.store,viewer["token"])
        document = self.put()
        self.assertEqual(api.list_documents(self.store,principal,self.sid,"channel")["records"][0]["_id"],document["_id"])
        with self.assertRaises(Forbidden):
            api.put_document(self.store,principal,self.sid,"channel","No",{"deck_count":1},str(uuid4()))
        other = str(uuid4())
        self.store.put("stations",other,{"code":"OTHER","name":"Other","timezone":"UTC","status":"active"})
        with self.assertRaises(Forbidden):
            api.list_documents(self.store,principal,other,"channel")
        with self.assertRaises(NotFound):
            api.get_document(self.store,self.principal,other,"channel",document["_id"])
        context = json.dumps(api.desktop_context(self.store,principal,self.sid))
        for secret in (self.identity["api_token"],viewer["token"],"token_hash"):
            self.assertNotIn(secret,context)
        endpoint = issue_endpoint_token(self.store,self.principal,self.sid,self.identity["endpoint_id"])
        with self.assertRaises(Forbidden):
            api.desktop_context(self.store,authenticate(self.store,endpoint),self.sid)

    def test_typed_config_rejects_claims_and_unknown_fields(self):
        for config in ({"deck_count":True},{"deck_count":33},{"deck_count":6,"online":True}):
            with self.assertRaises(StoreError):
                self.put(config=config)
        with self.assertRaises(StoreError):
            self.put(kind="tokens",config={"deck_count":1})
        self.assertEqual(len(self.store.scan("desktop_configurations")),0)

    def test_channel_stable_slots_preserve_gaps_and_legacy(self):
        old = self.put("channel", {"deck_count": 2})
        saved = self.put("channel", {"deck_count": 3, "slots": [0, 8, 31]},
                         document_id=old["_id"], expected_revision=old["_rev"])
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "channel", saved["_id"]), saved)
        sequence = self.store.sequence
        for slots in ([0, 0, 31], [0, 8], [0, 8, 32], [0, 8, True], [0, 8, 1.0], [0, 8, -1], "0,8,31"):
            with self.subTest(slots=slots), self.assertRaises(StoreError):
                self.put("channel", {"deck_count": 3, "slots": slots})
            self.assertEqual(self.store.sequence, sequence)

    def test_channel_names_preserve_unicode_master_gaps_and_clearing(self):
        legacy = self.put("channel", {"deck_count": 3, "slots": [0, 8, 31]})
        self.assertNotIn("names", legacy["config"])
        names = {"0": "Ana çıkış", "1": "Müzik 🎵", "9": "Konuk mikrofonu", "32": "🎵" * 64}
        saved = self.put("channel", {"deck_count": 3, "slots": [0, 8, 31], "names": names},
                         document_id=legacy["_id"], expected_revision=legacy["_rev"])
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "channel", saved["_id"]), saved)
        self.assertEqual(saved["config"]["names"], names)
        cleared = self.put("channel", {"deck_count": 3, "slots": [0, 8, 31], "names": {"0": "", "1": ""}},
                           document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(cleared["config"]["names"], {"0": "", "1": ""})
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "channel", cleared["_id"]), cleared)
        empty = self.put("channel", {"deck_count": 1, "names": {}})
        self.assertEqual(empty["config"]["names"], {})
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_channel_names_reject_noncanonical_keys_types_and_length(self):
        invalid_names = [None, [], "Kanal", True, 1]
        invalid_names += [{key: "Kanal"} for key in ("-1", "33", "01", "1.0", "MASTER", "", "١", 1)]
        invalid_names += [{"1": value} for value in (None, True, 1, 1.5, [], {}, "🎵" * 65, "bad\0name")]
        sequence = self.store.sequence
        for names in invalid_names:
            with self.subTest(names=names), self.assertRaises(StoreError):
                self.put("channel", {"deck_count": 1, "names": names})
            self.assertEqual(self.store.sequence, sequence)
        self.assertFalse(self.store.scan("desktop_configurations"))

    def test_audio_preferences_permissions_replay_cas_and_reopen(self):
        config = {"main_sink": "@DEFAULT_SINK@", "cue_sink": "headphone-output",
                  "monitor_sink": "", "microphone_source": "internal-microphone",
                  "main_gain_db": -6.0, "cue_gain_db": -12.0, "monitor_gain_db": -18.0,
                  "microphone_gain_db": 6.0, "cue_source": 32, "monitor_source": -1,
                  "player_automation": [{"player": 1, "sync_master": 3, "autodj_target": 3},
                                        {"player": 3, "sync_master": 0, "autodj_target": 1}]}
        key = str(uuid4())
        saved = api.put_document(self.store, self.principal, self.sid, "audio_preferences", "Ses aygıtları", config, key)
        sequence = self.store.sequence
        replay = api.put_document(self.store, self.principal, self.sid, "audio_preferences", "Ses aygıtları", config, key)
        self.assertEqual(replay, saved)
        self.assertEqual(self.store.sequence, sequence)
        editor = create_station_user(self.store, self.principal, self.sid, "audio-editor", "Editor", ["view", "edit"])
        with self.assertRaises(Forbidden):
            api.put_document(self.store, authenticate(self.store, editor["token"]), self.sid, "audio_preferences", "Ses aygıtları", config, str(uuid4()))
        changed = dict(config, cue_sink="second-output", cue_source=0)
        current = api.put_document(self.store, self.principal, self.sid, "audio_preferences", "Ses aygıtları", changed, str(uuid4()), document_id=saved["_id"], expected_revision=saved["_rev"])
        with self.assertRaises(Conflict):
            api.put_document(self.store, self.principal, self.sid, "audio_preferences", "Ses aygıtları", config, str(uuid4()), document_id=saved["_id"], expected_revision=saved["_rev"])
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "audio_preferences", current["_id"]), current)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        self.assertNotIn("microphone_gate", current["config"])
        self.assertNotIn("playing", current["config"])

    def test_audio_preferences_reject_runtime_claims_invalid_devices_and_levels(self):
        valid = {"main_sink": "@DEFAULT_SINK@", "cue_sink": "", "monitor_sink": "",
                 "microphone_source": "", "main_gain_db": 0, "cue_gain_db": -12,
                 "monitor_gain_db": -12, "microphone_gain_db": 0,
                 "cue_source": -1, "monitor_source": -1}
        for key, value in (("main_sink", "bad\nname"), ("cue_sink", "x" * 513),
                           ("main_gain_db", float("nan")), ("cue_gain_db", 7),
                           ("monitor_gain_db", True), ("microphone_gain_db", 31),
                           ("cue_source", 33), ("monitor_source", True),
                           ("microphone_gate", True), ("playing", True), ("online", True),
                           ("player_automation", [{"player": 1, "sync_master": 1, "autodj_target": 0}]),
                           ("player_automation", [{"player": 1, "sync_master": 2, "autodj_target": 0}, {"player": 2, "sync_master": 1, "autodj_target": 0}])):
            sequence = self.store.sequence
            with self.assertRaises(StoreError):
                self.put("audio_preferences", dict(valid, **{key: value}))
            self.assertEqual(self.store.sequence, sequence)
        self.assertFalse(self.store.scan("desktop_configurations"))

    def test_loudness_policy_roundtrip_replay_cas_and_dormant_selectors(self):
        policy = loudness_policy_configuration()
        # A stored selector remains useful when its automation is not assigned
        # yet or has been removed. It never authorizes playback in another station.
        policy.update(players=[32, 1], contexts=["autodj", "automation"], phase="transition",
            automation_mode="selected", automation_ids=[str(uuid4()), str(uuid4())],
            pairs_mode="selected", pairs=[{"from": 32, "to": 1}, {"from": 1, "to": 32}],
            schedule_enabled=True, timezone="Europe/Istanbul",
            windows=[{"days": [7, 1], "start_minute": 1320, "end_minute": 240},
                     {"days": [5], "start_minute": 600, "end_minute": 600}])
        config = audio_preferences_configuration() | {"loudness_normalize": True,
            "loudness_target_lufs": -14.5, "loudness_policy": policy}
        original = copy.deepcopy(config); key = str(uuid4())
        saved = api.put_document(self.store, self.principal, self.sid, "audio_preferences", "Ses aygıtları", config, key)
        self.assertEqual(config, original)
        sequence = self.store.sequence
        self.assertEqual(api.put_document(self.store, self.principal, self.sid, "audio_preferences", "Ses aygıtları", config, key), saved)
        self.assertEqual(self.store.sequence, sequence)
        changed = copy.deepcopy(config); changed["loudness_policy"]["phase"] = "steady"
        changed["loudness_target_lufs"] = -16
        current = api.put_document(self.store, self.principal, self.sid, "audio_preferences", "Ses aygıtları", changed,
            str(uuid4()), document_id=saved["_id"], expected_revision=saved["_rev"])
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            api.put_document(self.store, self.principal, self.sid, "audio_preferences", "Ses aygıtları", config,
                str(uuid4()), document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(self.store.sequence, sequence)
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "audio_preferences", current["_id"]), current)
        self.assertEqual(current["config"], changed)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_loudness_policy_legacy_absence_and_explicit_empty_filters_are_preserved(self):
        legacy = audio_preferences_configuration()
        variants = [legacy, legacy | {"loudness_normalize": True, "loudness_target_lufs": -5},
                    legacy | {"loudness_normalize": False, "loudness_target_lufs": -30}]
        empty = loudness_policy_configuration()
        empty.update(players=[], contexts=[], automation_mode="selected", automation_ids=[],
                     pairs_mode="selected", pairs=[], schedule_enabled=True, windows=[])
        variants.append(legacy | {"loudness_normalize": True, "loudness_target_lufs": -14, "loudness_policy": empty})
        for config in variants:
            with self.subTest(config=config):
                saved = self.put("audio_preferences", config)
                self.assertEqual(saved["config"], config)
                reopened = open_radio_store(self.db, read_only=True)
                self.assertEqual(api.get_document(reopened, self.principal, self.sid, "audio_preferences", saved["_id"])["config"], config)
                self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        # Enabling a window with no selected days is a deliberate no-match
        # setting, while equal minutes on selected days mean a whole day.
        empty["windows"] = [{"days": [], "start_minute": 0, "end_minute": 0}]
        self.assertEqual(self.put("audio_preferences", legacy | {"loudness_policy": empty})["config"]["loudness_policy"], empty)

    def test_loudness_policy_strict_shapes_types_bounds_and_atomic_rejection(self):
        valid = loudness_policy_configuration(); invalid = [None, [], True, 1, "future"]
        for field in valid:
            missing = copy.deepcopy(valid); del missing[field]; invalid.append(missing)
        for field, values in {
            "schema_version": [True, 0, 2, "1"],
            "players": [None, {}, "1", [True], [0], [33], [1.0], [1, 1], list(range(1, 34))],
            "contexts": [None, "manual", ["future"], [True], [{}], ["manual", "manual"], ["manual"] * 4],
            "phase": [None, True, {}, "future"],
            "automation_mode": [None, [], "future"],
            "automation_ids": [None, "id", [True], [123], ["not-a-UUID"],
                ["ABCDEFAB-CDEF-4ABC-8DEF-ABCDEFABCDEF"], ["abcdefabcdef4abc8defabcdefabcdef"],
                ["abcdefab-cdef-4abc-8def-abcdefabcdef"] * 2, [str(uuid4()) for _ in range(65)]],
            "pairs_mode": [None, {}, "future"],
            "pairs": [None, {}, [{"from": 1}], [{"from": 1, "to": 2, "playing": True}],
                [{"from": 1, "to": 1}], [{"from": 0, "to": 2}], [{"from": 1, "to": 33}],
                [{"from": True, "to": 2}], [{"from": 1, "to": 2.0}],
                [{"from": 1, "to": 2}] * 2, [{"from": 1, "to": 2}] * 993],
            "schedule_enabled": [None, 0, 1, "true"],
            "timezone": [None, True, "", "../UTC", "/usr/share/zoneinfo/UTC", "No/Such_Zone",
                         "UTC\0", "x" * 128, "🎵" * 64],
            "windows": [None, {}, [{}], [True], [{"days": [1], "start_minute": 0, "end_minute": 0}] * 17],
        }.items():
            for value in values:
                policy = copy.deepcopy(valid); policy[field] = value; invalid.append(policy)
        future = copy.deepcopy(valid); future["future_runtime_gain"] = True; invalid.append(future)
        window = {"days": [1], "start_minute": 0, "end_minute": 1439}
        for field, values in {"days": [None, "1", [True], [0], [8], [1.0], [1, 1]],
                              "start_minute": [True, -1, 1440, 1.5, "60"],
                              "end_minute": [None, False, -1, 1440, 1.5]}.items():
            for value in values:
                bad_window = copy.deepcopy(window); bad_window[field] = value
                policy = copy.deepcopy(valid); policy["windows"] = [bad_window]; invalid.append(policy)
        for field in window:
            bad_window = copy.deepcopy(window); del bad_window[field]
            policy = copy.deepcopy(valid); policy["windows"] = [bad_window]; invalid.append(policy)
        bad_window = window | {"enabled": True}
        invalid.append(valid | {"windows": [bad_window]})
        sequence = self.store.sequence
        for policy in invalid:
            with self.subTest(policy=policy), self.assertRaises(StoreError):
                self.put("audio_preferences", audio_preferences_configuration() | {"loudness_policy": policy})
            self.assertEqual(self.store.sequence, sequence)
        self.assertFalse(self.store.scan("desktop_configurations"))
        self.assertFalse(self.store.scan("desktop_mutations"))

    def test_loudness_policy_all_slot_pair_limits_unused_selectors_and_windows(self):
        policy = loudness_policy_configuration()
        policy.update(automation_ids=[str(uuid4()) for _ in range(64)],
            pairs=[{"from": source, "to": target} for source in range(1, 33)
                   for target in range(1, 33) if source != target],
            windows=[{"days": list(range(1, 8)), "start_minute": 1439, "end_minute": 0}] * 16)
        saved = self.put("audio_preferences", audio_preferences_configuration() | {"loudness_policy": policy})
        self.assertEqual(saved["config"]["loudness_policy"], policy)
        self.assertEqual(len(saved["config"]["loudness_policy"]["pairs"]), 992)
        # ALL modes do not consume selected lists, but malformed dormant lists
        # still fail validation rather than producing a later engine surprise.
        malformed = copy.deepcopy(policy); malformed["pairs"][0]["to"] = 0
        sequence = self.store.sequence
        with self.assertRaises(StoreError):
            self.put("audio_preferences", audio_preferences_configuration() | {"loudness_policy": malformed},
                     document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(self.store.sequence, sequence)

    def test_loudness_policy_backup_and_portable_import_preserve_legacy_and_scopes(self):
        from sdbase.backup import create_backup, restore_backup
        from sdbase.portable_export import create_portable_export
        from sdbase.portable_import import restore_portable_export
        policy = loudness_policy_configuration()
        policy.update(players=[1, 32], phase="steady", automation_mode="assigned",
            automation_ids=[str(uuid4())], schedule_enabled=True, timezone="Europe/Istanbul",
            windows=[{"days": [7], "start_minute": 1380, "end_minute": 60}])
        legacy = self.put("audio_preferences", audio_preferences_configuration() | {"loudness_normalize": True, "loudness_target_lufs": -12})
        scoped = self.put("audio_preferences", audio_preferences_configuration() | {"loudness_normalize": True, "loudness_target_lufs": -16, "loudness_policy": policy})
        source_sequence = self.store.sequence
        root = Path(self.temp.name)
        create_backup(self.store, root / "physical-backup")
        restored = restore_backup(root / "physical-backup", root / "physical-restored")
        physical = open_radio_store(Path(restored["path"]) / restored["database"], read_only=True)
        create_portable_export(self.store, self.principal, root / "portable-package")
        imported = restore_portable_export(root / "portable-package", root / "portable-restored")
        portable = open_radio_store(Path(imported["path"]) / imported["database"], read_only=True)
        for store in (physical, portable):
            for document in (legacy, scoped):
                self.assertEqual(store.get("desktop_configurations", document["_id"])["config"], document["config"])
            self.assertEqual(inspect_integrity(store)["status"], "ok")
        self.assertEqual(self.store.sequence, source_sequence)

    def test_dj_mixer_permissions_replay_cas_and_reopen(self):
        config = dj_configuration()
        config["channels"][0].update(source_deck=-2, eq_low_db=-26, eq_mid_db=6,
                                     pan=-1, gain_db=-60, crossfade_group=1, fx_slot=4, loudness_bypass=True)
        config["channels"][31].update(source_deck=-1, pan=1, gain_db=6, crossfade_group=2)
        config["effects"][4].update(type=13, depth=1, tempo_bpm=300, enabled=True)
        config["master_fx"].update(type=6, depth=0, tempo_bpm=30)
        config["master"]["fx_slot"] = 4
        config["crossfader"] = -1
        editor = create_station_user(self.store, self.principal, self.sid, "dj-editor", "DJ", ["view", "edit"])
        principal = authenticate(self.store, editor["token"])
        key = str(uuid4())
        saved = api.put_document(self.store, principal, self.sid, "dj_mixer", "DJ mixer", config, key)
        sequence = self.store.sequence
        self.assertEqual(api.put_document(self.store, principal, self.sid, "dj_mixer", "DJ mixer", config, key), saved)
        self.assertEqual(self.store.sequence, sequence)
        changed = copy.deepcopy(config)
        changed["crossfader"] = 1
        with self.assertRaises(Conflict):
            api.put_document(self.store, principal, self.sid, "dj_mixer", "DJ mixer", changed, key)
        viewer = create_station_user(self.store, self.principal, self.sid, "dj-viewer", "Viewer", ["view"])
        view_principal = authenticate(self.store, viewer["token"])
        self.assertEqual(api.get_document(self.store, view_principal, self.sid, "dj_mixer", saved["_id"]), saved)
        with self.assertRaises(Forbidden):
            api.put_document(self.store, view_principal, self.sid, "dj_mixer", "DJ mixer", changed, str(uuid4()))
        other = str(uuid4())
        self.store.put("stations", other, {"code": "OTHER-DJ", "name": "Other", "timezone": "UTC", "status": "active"})
        with self.assertRaises(Forbidden):
            api.list_documents(self.store, principal, other, "dj_mixer")
        with self.assertRaises(NotFound):
            api.get_document(self.store, self.principal, other, "dj_mixer", saved["_id"])
        revised = api.put_document(self.store, principal, self.sid, "dj_mixer", "DJ mixer", changed,
                                   str(uuid4()), document_id=saved["_id"], expected_revision=saved["_rev"])
        with self.assertRaises(Conflict):
            api.put_document(self.store, principal, self.sid, "dj_mixer", "DJ mixer", config,
                             str(uuid4()), document_id=saved["_id"], expected_revision=saved["_rev"])
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, principal, self.sid, "dj_mixer", revised["_id"]), revised)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_dj_channel_routes_persist_and_reject_feedback(self):
        config = dj_configuration()
        config["channels"][7].update(send_channel=0, send_db=-9)
        config["channels"][0].update(return_channel=7, return_db=-3)
        saved = self.put("dj_mixer", config)
        self.assertEqual(saved["config"], config)
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "dj_mixer", saved["_id"]), saved)
        for field, value in (("send_channel", 0), ("return_channel", 0), ("send_channel", 32),
                             ("return_channel", -2), ("send_channel", True), ("return_channel", 1.5),
                             ("send_db", float("nan")), ("return_db", 7)):
            invalid = copy.deepcopy(config)
            invalid["channels"][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(StoreError):
                self.put("dj_mixer", invalid)
        for destination in (7, 2):
            invalid = copy.deepcopy(config)
            invalid["channels"][0]["send_channel"] = destination
            if destination == 2:
                invalid["channels"][2]["send_channel"] = 7
            with self.subTest(destination=destination), self.assertRaises(StoreError):
                self.put("dj_mixer", invalid)

    def test_dj_master_routes_persist_and_validate(self):
        config = dj_configuration()
        config["master"].update(send_channel=7, return_channel=7, send_db=-9, return_db=-3)
        saved = self.put("dj_mixer", config)
        self.assertEqual(saved["config"], config)
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "dj_mixer", saved["_id"]), saved)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        for field, value in (("send_channel", 32), ("return_channel", -2), ("send_channel", True),
                             ("return_channel", 1.5), ("send_db", float("nan")), ("send_db", -61),
                             ("return_db", float("inf")), ("return_db", 7), ("return_db", True)):
            invalid = copy.deepcopy(config)
            invalid["master"][field] = value
            sequence = self.store.sequence
            with self.subTest(field=field, value=value), self.assertRaises(StoreError):
                self.put("dj_mixer", invalid)
            self.assertEqual(self.store.sequence, sequence)
        changed = copy.deepcopy(config)
        changed["master"].update(send_channel=31, return_channel=-1, send_db=6, return_db=-60)
        revised = api.put_document(self.store, self.principal, self.sid, "dj_mixer", "Native", changed,
                                   str(uuid4()), document_id=saved["_id"], expected_revision=saved["_rev"])
        with self.assertRaises(Conflict):
            api.put_document(self.store, self.principal, self.sid, "dj_mixer", "Native", config,
                             str(uuid4()), document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(revised["config"], changed)

    def test_dj_effect_returns_persist_and_allow_independent_effect_sources(self):
        config = dj_configuration()
        config["channels"][0].update(fx_slot=1, return_effect=3, return_channel=-1, return_db=-3)
        config["channels"][1].update(fx_slot=-1, return_effect=0)
        config["channels"][2].update(return_effect=-1)
        config["channels"][31].update(return_effect=4)
        config["master"].update(fx_slot=0, return_effect=2, return_channel=-1, return_db=-9)
        saved = self.put("dj_mixer", config)
        self.assertEqual(saved["config"], config)
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "dj_mixer", saved["_id"]), saved)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        changed = copy.deepcopy(config)
        changed["channels"][0]["return_effect"] = -1
        changed["master"]["return_effect"] = 4
        revised = api.put_document(self.store, self.principal, self.sid, "dj_mixer", "Native", changed,
                                   str(uuid4()), document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(revised["config"], changed)
        with self.assertRaises(Conflict):
            api.put_document(self.store, self.principal, self.sid, "dj_mixer", "Native", config,
                             str(uuid4()), document_id=saved["_id"], expected_revision=saved["_rev"])

    def test_dj_effect_returns_accept_legacy_absence_without_rewriting_routes(self):
        config = dj_configuration()
        config["channels"][0].update(fx_slot=2, return_channel=7)
        config["master"].update(fx_slot=3, return_channel=7)
        saved = self.put("dj_mixer", config)
        self.assertEqual(saved["config"], config)
        self.assertNotIn("return_effect", saved["config"]["channels"][0])
        self.assertNotIn("return_effect", saved["config"]["master"])
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "dj_mixer", saved["_id"]), saved)
        native = copy.deepcopy(config)
        native["channels"][0].update(return_channel=-1, return_effect=2)
        native["master"].update(return_channel=-1, return_effect=3)
        migrated = api.put_document(self.store, self.principal, self.sid, "dj_mixer", "Native", native,
                                    str(uuid4()), document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(migrated["config"], native)

    def test_dj_effect_returns_reject_noninteger_and_invalid_slot_without_writes(self):
        for target in ("channel", "master"):
            for value in (-2, 5, 31, True, False, 1.0, 1.5, None, "2"):
                config = dj_configuration()
                strip = config["channels"][0] if target == "channel" else config["master"]
                strip["return_effect"] = value
                sequence = self.store.sequence
                with self.subTest(target=target, value=value), self.assertRaises(StoreError):
                    self.put("dj_mixer", config)
                self.assertEqual(self.store.sequence, sequence)
        self.assertFalse(self.store.scan("desktop_configurations"))
        self.assertFalse(self.store.scan("desktop_mutations"))

    def test_dj_mixer_rejects_invalid_shapes_ranges_and_live_claims(self):
        changes = [(("channels", 0, "source_deck"), value) for value in (-3, 32, True, 1.5)]
        changes += [(("channels", 0, key), value) for key, value in (
            ("eq_low_db", -27), ("eq_mid_db", 7), ("eq_high_db", float("nan")),
            ("pan", float("inf")), ("pan", -1.01), ("gain_db", True), ("gain_db", 10 ** 1000),
            ("mute", 1), ("solo", "false"), ("loudness_bypass", 1), ("loudness_bypass", "false"), ("crossfade_group", 3), ("crossfade_group", True),
            ("fx_slot", 5), ("fx_slot", -2), ("fx_slot", 1.0), ("fx_send_db", -61),
            ("fx_return_db", 7), ("cue", True), ("playing", True))]
        changes += [(("effects", 0, key), value) for key, value in (
            ("type", 14), ("type", True), ("enabled", 1), ("depth", 1.01), ("depth", True),
            ("send_db", float("nan")), ("return_db", 7), ("monitor_path", 3),
            ("monitor_path", 1.0), ("tempo_bpm", 29), ("tempo_bpm", 301), ("cue", True))]
        changes += [(("master", "gain_db"), 0), (("master", "eq_low_db"), -27),
                    (("master", "mute"), "false"), (("master_fx", "depth"), -0.01),
                    (("crossfader",), 1.01), (("crossfader",), True),
                    (("speech", "ducking"), 1), (("speech", "bed_db"), -37),
                    (("speech", "bed_db"), 1), (("speech", "microphone_gate"), True),
                    (("online",), True), (("channels",), []), (("effects",), [])]
        changes += [(("master", "fx_slot"), value) for value in (-2, 5, True, 1.0)]
        for path, value in changes:
            with self.subTest(path=path, value=value):
                config = dj_configuration()
                target = config
                for key in path[:-1]:
                    target = target[key]
                target[path[-1]] = value
                sequence = self.store.sequence
                with self.assertRaises(StoreError):
                    self.put("dj_mixer", config)
                self.assertEqual(self.store.sequence, sequence)
        for field in ("channels", "effects", "master", "master_fx", "crossfader", "speech"):
            config = dj_configuration()
            del config[field]
            with self.assertRaises(StoreError):
                self.put("dj_mixer", config)
        config = dj_configuration()
        config["channels"].append(copy.deepcopy(config["channels"][0]))
        with self.assertRaises(StoreError):
            self.put("dj_mixer", config)
        config = dj_configuration()
        config["effects"].append(copy.deepcopy(config["effects"][0]))
        with self.assertRaises(StoreError):
            self.put("dj_mixer", config)
        self.assertFalse(self.store.scan("desktop_configurations"))
        self.assertFalse(self.store.scan("desktop_mutations"))

    def verified_binding(self):
        imports = Path(self.temp.name) / "imports"
        imports.mkdir(exist_ok=True, mode=0o700)
        self.store.desktop_import_root = imports
        path = imports / (str(uuid4()) + ".wav")
        samples = [round(10000 * math.sin(2 * math.pi * 400 * index / 8000)) for index in range(1600)]
        with wave.open(str(path), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(8000)
            output.writeframes(struct.pack("<1600h", *samples))
        path.chmod(0o600)
        normalized = [sample / 32768 for sample in samples]
        waveform = []
        for start in range(0, len(samples), 400):
            waveform.extend((min(normalized[start:start + 400]), max(normalized[start:start + 400])))
        analysis = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "size_bytes": path.stat().st_size,
                    "duration_ms": 200, "sample_rate": 8000, "channels": 1, "codec": "pcm_s16le", "format": "wav",
                    "bitrate": 128000, "peak": max(map(abs, normalized)),
                    "rms": math.sqrt(sum(value * value for value in normalized) / len(normalized)),
                    "clipped": False, "clipped_samples": 0, "sample_frames": 1600,
                    "waveform_frames_per_bin": 400, "waveform": waveform}
        metadata = {"title": "Gerçek DJ sampler", "kind": "jingle", "artist": "", "album": "",
                    "genre": "", "tags": [], "year": "", "comment": "Isolated verified PCM"}
        detail = desktop_media.import_media(self.store, self.principal, self.sid, path.name, "sampler.wav",
                                           str(uuid4()), metadata, analysis)
        return {"media_id": detail["media"]["_id"], "version_id": detail["version"]["_id"],
                "media_file_id": detail["version"]["media_file_id"], "media_sha256": detail["version"]["sha256"]}

    def test_sampler_and_transition_verified_binding_reopen_and_legacy(self):
        binding = self.verified_binding()
        pad = {"key": "channel-1-pad-1", "title": "Verified", "file": "/draft-only.wav", **binding}
        saved = self.put("sampler", {"pads": [pad, {"key": "channel-0-pad-1", "title": "Legacy", "file": "/legacy.wav"}]})
        transition = {"values": ["0.2", "0", "", "0.2", "0", "0.1", "0.1", "-10"],
                      "files": ["/legacy-a.wav", ""], "flag": False, "preview_level": -6,
                      "media_a": copy.deepcopy(binding), "media_b": copy.deepcopy(binding)}
        edited = self.put("transition", transition)
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "sampler", saved["_id"]), saved)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "transition", edited["_id"]), edited)
        # Loss of verification invalidates new assignment, not historical
        # station/config integrity. Playback must revalidate the live record.
        media = self.store.get("media_files", binding["media_file_id"])
        self.store.put("media_files", media["_id"], {key: value for key, value in media.items() if not key.startswith("_")} | {"status": "error"},
                       expected_revision=media["_rev"])
        with self.assertRaises(StoreError):
            self.put("sampler", {"pads": [pad]})
        self.assertEqual(inspect_integrity(open_radio_store(self.db, read_only=True))["status"], "ok")

    def test_verified_bindings_reject_partial_mismatch_and_other_station(self):
        binding = self.verified_binding()
        pad = {"key": "channel-1-pad-1", "title": "Verified", "file": "", **binding}
        broken = []
        for key in binding:
            candidate = copy.deepcopy(pad)
            del candidate[key]
            broken.append(candidate)
        broken += [dict(pad, **{key: str(uuid4())}) for key in ("media_id", "version_id", "media_file_id")]
        broken += [dict(pad, media_sha256="0" * 64), dict(pad, media_sha256=binding["media_sha256"].upper()),
                   dict(pad, media_id=True), dict(pad, media_sha256=True)]
        for candidate in broken:
            sequence = self.store.sequence
            with self.assertRaises(StoreError):
                self.put("sampler", {"pads": [candidate]})
            self.assertEqual(self.store.sequence, sequence)
        other = str(uuid4())
        self.store.put("stations", other, {"code": "OTHER-MEDIA", "name": "Other", "timezone": "UTC", "status": "active"})
        with self.assertRaises(StoreError):
            api.put_document(self.store, self.principal, other, "sampler", "Foreign", {"pads": [pad]}, str(uuid4()))
        with self.assertRaises(StoreError):
            self.put("markers", {"values": ["0.2", "0", "", "0.2", "", "", "", "0"],
                                 "files": [], "flag": False, "preview_level": 0, "media_a": binding})
        self.assertFalse(self.store.scan("desktop_configurations"))

    def test_editors_programme_sampler_and_preparation(self):
        transition = {"values":["120","0","","110","0","2","2","-10"],"files":["/path/a.wav",""],"flag":False,"preview_level":-6}
        self.put("transition",transition)
        markers = {"values":["120","0","","110","","","","0"],"files":[],"flag":False,"preview_level":0}
        self.put("markers",markers)
        self.put("programme",{"title":"Morning","date":"2026-10-06","note":"", "events":[{"id":str(uuid4()),"title":"Draft","artist":"","album":"","kind":"music","start_time":"","duration_ms":30000,"version_label":"v1","primary_file":"/draft.wav","backup_file":"","note":""}]})
        self.put("sampler",{"pads":[{"key":"channel-1-pad-1","title":"Pad","file":"/draft.wav"}]})
        self.put("emergency",{"queues":[[],[]],"preparation":{"takeover/Acil liste adı":"Backup","alert/Test":"1","alert/Gerçek":"0"}})
        sequence=self.store.sequence
        with self.assertRaises(StoreError):
            self.put("emergency",{"queues":[[],[]],"preparation":{"alert/Test":"enabled"}})
        self.assertEqual(self.store.sequence,sequence)
        self.assertFalse(self.store.scan("media_files"))
        self.assertFalse(self.store.scan("plan_revisions"))
        self.assertFalse(self.store.scan("playback_marker_sets"))
        broken = copy.deepcopy(transition);broken["values"][6]="nan"
        with self.assertRaises(StoreError):
            self.put("transition",broken)
        broken = copy.deepcopy(markers);broken["flag"]=True
        with self.assertRaises(StoreError):
            self.put("markers",broken)

    def test_independent_player_queue_validation_cas_and_reopen(self):
        legacy = {"title": "Queue", "date": "2026-10-08", "note": "", "events": []}
        self.put("programme", legacy)
        queues = []
        for player in (1, 32):
            configuration = legacy | {"queue_player": player, "queue_transition": 3, "queue_overlap": 0}
            queues.append(api.put_document(self.store, self.principal, self.sid, "programme",
                f"Oynatıcı {player} yayın sırası", configuration, str(uuid4())))
        first, second = queues
        updated = api.put_document(self.store, self.principal, self.sid, "programme", first["name"],
            first["config"] | {"note": "only player one"}, str(uuid4()),
            document_id=first["_id"], expected_revision=first["_rev"])
        with self.assertRaises(Conflict):
            api.put_document(self.store, self.principal, self.sid, "programme", first["name"], first["config"],
                str(uuid4()), document_id=first["_id"], expected_revision=first["_rev"])
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "programme", updated["_id"]), updated)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "programme", second["_id"]), second)
        for fields in ({"queue_player": 0}, {"queue_player": 33}, {"queue_player": True},
                       {"queue_transition": 0}, {"queue_player": 1, "queue_transition": 1},
                       {"queue_player": 1, "queue_transition": 4}, {"queue_player": 1, "queue_overlap": 1},
                       {"queue_player": 1, "queue_overlap": float("nan")}):
            with self.subTest(fields=fields), self.assertRaises(StoreError):
                self.put("programme", legacy | fields)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_player_autodj_pair_persistence_and_validation(self):
        legacy = {"title": "Pair", "date": "2026-10-08", "note": "", "events": []}
        pair = legacy | {"autodj_players": [1, 3], "beat_sync": True}
        saved = api.put_document(self.store, self.principal, self.sid, "programme", "Pair", pair, str(uuid4()))
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "programme", saved["_id"])["config"], pair)
        for fields in ({"autodj_players": [1, 1]}, {"autodj_players": [0, 2]}, {"autodj_players": [1, 33]},
                       {"autodj_players": [True, 2]}, {"autodj_players": [1]}, {"beat_sync": True},
                       {"autodj_players": [1, 2], "queue_player": 1}, {"autodj_players": [1, 2], "beat_sync": 1}):
            sequence = self.store.sequence
            with self.subTest(fields=fields), self.assertRaises(StoreError):
                self.put("programme", legacy | fields)
            self.assertEqual(self.store.sequence, sequence)

    def test_programme_blocks_and_retired_online_events(self):
        def event(block=None, **extra):
            row = {"id":str(uuid4()),"title":"Parça","artist":"","album":"","kind":"music","start_time":"","duration_ms":1000,
                   "version_label":"","primary_file":"","backup_file":"","note":""}
            if block:
                row["block_id"] = block
            return row | extra
        def block(kind="music", title="Müzik eventi"):
            return {"id":str(uuid4()),"kind":kind,"title":title,"start_time":"","note":""}
        music, empty, advert = block(), block("announcement", "Anons eventi"), block("advertisement", "Reklam eventi")
        online = {"online_provider":"youtube","online_id":"dQw4w9WgXcQ","online_url":"https://music.youtube.com/watch?v=dQw4w9WgXcQ"}
        config = {"title":"Bloklu","date":"2026-10-07","note":"","blocks":[music, empty, advert],
                  "events":[event(music["id"]), event(music["id"], title="İkinci parça"), event(advert["id"], kind="advertisement")]}
        saved = self.put("programme", config)
        self.assertEqual(saved["config"]["blocks"][1]["title"], "Anons eventi")
        legacy = {"title":"Eski","date":"2026-10-07","note":"","events":[event()]}
        self.put("programme", legacy)
        sequence = self.store.sequence
        broken_cases = []
        for mutate in (
                lambda c: c["events"][0].pop("block_id"),
                lambda c: c["events"][0].update(block_id=str(uuid4())),
                lambda c: c["events"].reverse(),
                lambda c: c["blocks"].append(dict(c["blocks"][0])),
                lambda c: c["blocks"][0].update(kind="playlist"),
                lambda c: c["blocks"][0].update(title=""),
                lambda c: c["blocks"][0].update(start_time="25:00"),
                lambda c: c["blocks"][0].update(extra=True),
                lambda c: c["blocks"][1].update(id=c["events"][0]["id"]),
                lambda c: c.pop("blocks"),
                lambda c: c["events"][1].update(**online),
                lambda c: c["events"][1].update(online_provider="youtube"),
                lambda c: c["events"][1].update(**online, primary_file="/song.mp3"),
                lambda c: c["events"].append(event(music["id"], **online))):
            broken = copy.deepcopy(config)
            mutate(broken)
            broken_cases.append(broken)
        for broken in broken_cases:
            with self.assertRaises(StoreError):
                self.put("programme", broken)
        self.assertEqual(self.store.sequence, sequence)
        prepared = playout.prepare(self.store, self.principal, self.sid, "programme", config=config)
        self.assertEqual([track["reasons"] for track in prepared["tracks"]], [["media_unbound"]] * 3)

    def test_programme_view_window_settings_persist_reopen_and_reject_invalid(self):
        view = {"event_fetch": 20, "event_show": 10, "content_fetch": 100, "content_show": 50, "refresh_seconds": 2}
        legacy = {"title": "Pencere", "date": "2026-10-18", "note": "", "events": []}
        # Records without the window keys stay valid and are stored unchanged.
        self.assertNotIn("view", self.put("programme", legacy)["config"])
        by_title = {"Sabah kuşağı": dict(view, event_fetch=1, event_show=1), "Gece": dict(view, refresh_seconds=60)}
        saved = self.put("programme", legacy | {"view": view, "view_by_title": by_title})
        self.assertEqual(saved["config"]["view"], view)
        self.assertEqual(saved["config"]["view_by_title"], by_title)
        edges = {"event_fetch": 500, "event_show": 200, "content_fetch": 500, "content_show": 500, "refresh_seconds": 1}
        self.assertEqual(self.put("programme", legacy | {"view": edges})["config"]["view"], edges)
        full = {f"Program {index}": view for index in range(64)}
        self.assertEqual(len(self.put("programme", legacy | {"view_by_title": full})["config"]["view_by_title"]), 64)
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "programme", saved["_id"]), saved)
        sequence = self.store.sequence
        broken = [{"view": {k: v for k, v in view.items() if k != key}} for key in view]
        broken += [{"view": dict(view, event_fetch=9)}, {"view": dict(view, content_fetch=49)},
                   {"view": dict(view, event_fetch=0)}, {"view": dict(view, event_fetch=501)}, {"view": dict(view, event_show=201)},
                   {"view": dict(view, content_fetch=501)}, {"view": dict(view, content_show=0)},
                   {"view": dict(view, refresh_seconds=0)}, {"view": dict(view, refresh_seconds=61)},
                   {"view": dict(view, refresh_seconds=True)}, {"view": dict(view, event_fetch=20.0)}, {"view": dict(view, event_show="10")},
                   {"view": dict(view, extra=1)}, {"view": []}, {"view": None},
                   {"view_by_title": {f"Program {index}": view for index in range(65)}},
                   {"view_by_title": {"": view}}, {"view_by_title": {" ": view}}, {"view_by_title": {"x" * 161: view}},
                   {"view_by_title": {"Sabah": dict(view, event_show=21)}}, {"view_by_title": {"Sabah": dict(view, content_show=101)}},
                   {"view_by_title": [view]}, {"view_by_title": {"Sabah": None}}, {"view_by_title": {"Sabah": {}}}]
        for fields in broken:
            with self.subTest(fields=fields), self.assertRaises(StoreError):
                self.put("programme", legacy | fields)
            self.assertEqual(self.store.sequence, sequence)
        with self.assertRaisesRegex(StoreError, "fetch counts must not be below the shown counts"):
            self.put("programme", legacy | {"view": dict(view, event_show=20, event_fetch=19)})
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_sampler_full_station_boundary(self):
        pads=[{"key":f"channel-{channel}-pad-{pad}","title":"","file":""} for channel in range(33) for pad in range(1,5)]
        saved=self.put("sampler",{"pads":pads})
        self.assertEqual(len(saved["config"]["pads"]),132)
        sequence=self.store.sequence
        for key in ("channel-33-pad-1","channel-0-pad-5","channel-01-pad-1","channel-99-pad-99"):
            with self.assertRaises(StoreError):
                self.put("sampler",{"pads":[{"key":key,"title":"","file":""}]})
        with self.assertRaises(StoreError):
            self.put("sampler",{"pads":pads+[pads[0]]})
        self.assertEqual(self.store.sequence,sequence)
        self.assertEqual(inspect_integrity(self.store)["status"],"ok")

    def test_delete_revision_and_replay(self):
        row = self.put()
        key = str(uuid4())
        result = api.delete_document(self.store,self.principal,self.sid,"channel",row["_id"],row["_rev"],key)
        self.assertEqual(result["status"],"disabled")
        self.assertEqual(api.list_documents(self.store,self.principal,self.sid,"channel")["records"],[])
        seq = self.store.sequence
        self.assertEqual(api.delete_document(self.store,self.principal,self.sid,"channel",row["_id"],row["_rev"],key),result)
        self.assertEqual(self.store.sequence,seq)

    def test_real_campaign_and_unverified_spot(self):
        fields = dict(advertiser="Client",owner="Sales",reference="Ref",start_date="2026-10-06",end_date="2026-10-06",target_hint="Main",spot_title="Spot",version_label="v1",duration_seconds=30,planned_count=10,price=10.50,currency="TRY",contact="",slot="",capacity_seconds=0,intro_seconds=0,outro_seconds=0,announcement_seconds=0,competition="",position=0,fallback=0,skip_empty=True)
        key = str(uuid4())
        result = api.save_campaign(self.store,self.principal,self.sid,"Campaign",fields,key)
        self.assertEqual(result["status"],"draft")
        self.assertEqual(self.store.get("items",result["desktop_item_id"])["kind"],"advertisement")
        self.assertEqual(self.store.get("versions",result["desktop_version_id"])["status"],"draft")
        self.assertFalse(self.store.scan("media_files"))
        self.assertFalse(self.store.scan("spots"))
        self.assertFalse(self.store.scan("placements"))
        self.assertEqual(api.save_campaign(self.store,self.principal,self.sid,"Campaign",fields,key),result)
        fields["reference"]="Changed"
        revised = api.save_campaign(self.store,self.principal,self.sid,"Campaign",fields,str(uuid4()),campaign_id=result["_id"],expected_revision=result["_rev"])
        with self.assertRaises(Conflict):
            api.save_campaign(self.store,self.principal,self.sid,"Campaign",fields,str(uuid4()),campaign_id=result["_id"],expected_revision=result["_rev"])
        self.assertEqual(api.list_campaigns(self.store,self.principal,self.sid)["records"][0]["_id"],revised["_id"])
        version_key=str(uuid4())
        version_args=dict(campaign_id=revised["_id"],expected_revision=revised["_rev"],name="v2",duration_seconds=20,valid_from_time="",valid_to_time="",file_reference="/unverified/spot.wav",idempotency_key=version_key)
        version=api.create_campaign_version(self.store,self.principal,self.sid,**version_args)
        self.assertEqual(version["version_no"],2)
        self.assertEqual(version["status"],"draft")
        self.assertEqual(api.create_campaign_version(self.store,self.principal,self.sid,**version_args),version)
        self.assertEqual(len(self.store.scan("versions")),2)
        self.assertEqual(api.get_campaign(self.store,self.principal,self.sid,revised["_id"])["desktop_version_id"],version["_id"])
        self.assertEqual(inspect_integrity(self.store)["status"],"ok")

    def test_user_key_precommit_failure_cleanup(self):
        with patch.object(self.store,"begin",side_effect=StoreError("forced precommit failure")):
            with self.assertRaises(StoreError):
                api.create_user(self.store,self.principal,self.sid,"failed-user","Failed","operator",[],str(uuid4()))
        self.assertEqual(list((self.store.root/"private-user-keys").iterdir()),[])
        self.assertFalse(self.store.lookup("users","username","failed-user"))

    def test_user_key_durable_without_receipt_secrets(self):
        key=str(uuid4())
        args=dict(username="new-dj",display_name="DJ",role="dj",additional_permissions=["view"],idempotency_key=key)
        created=api.create_user(self.store,self.principal,self.sid,**args)
        token=created["token"]
        file=Path(created["credential_file"])
        self.assertEqual(file.stat().st_mode&0o777,0o600)
        self.assertEqual(json.loads(file.read_text())["token"],token)
        self.assertEqual(authenticate(self.store,token).user_id,created["user_id"])
        seq=self.store.sequence
        replay=api.create_user(self.store,self.principal,self.sid,**args)
        self.assertNotIn("token",replay)
        self.assertEqual(replay["user_id"],created["user_id"])
        self.assertEqual(self.store.sequence,seq)
        self.assertNotIn(token,json.dumps(self.store.scan("desktop_mutations")))
        self.assertNotIn(token,json.dumps(self.store.scan("_audit")))
        with self.assertRaises(StoreError):
            api.create_user(self.store,self.principal,self.sid,**(args|{"username":"temporary","temporary":True,"idempotency_key":str(uuid4())}))
        self.assertEqual(inspect_integrity(self.store)["status"],"ok")

    def test_http_contract(self):
        base = make_handler(self.store)
        class Handler(base):
            def log_message(self,*args): pass
        server = BoundedHTTPServer(("127.0.0.1",0),Handler,request_timeout=2)
        worker = threading.Thread(target=server.serve_forever,daemon=True);worker.start()
        def request(path,body,auth=True):
            conn = HTTPConnection("127.0.0.1",server.server_port,timeout=2)
            headers={"Content-Type":"application/json","Connection":"close"}
            if auth: headers["Authorization"]="Bearer "+self.identity["api_token"]
            conn.request("POST",path,json.dumps(body),headers)
            response=conn.getresponse();value=json.loads(response.read());status=response.status;conn.close();return status,value
        try:
            self.assertEqual(request("/v1/workflows/desktop/context",{"station_id":self.sid},False)[0],403)
            self.assertEqual(request("/v1/workflows/desktop/get",{"station_id":self.sid,"kind":"channel","document_id":str(uuid4())})[0],404)
            body=dict(station_id=self.sid,kind="channel",name="Decks",config={"deck_count":6},idempotency_key=str(uuid4()))
            status,row=request("/v1/workflows/desktop/put",body);self.assertEqual(status,200)
            body["config"]={"deck_count":7};self.assertEqual(request("/v1/workflows/desktop/put",body)[0],409)
            self.assertEqual(request("/v1/workflows/desktop/put",body|{"idempotency_key":str(uuid4()),"config":{"deck_count":False}})[0],422)
        finally:
            server.shutdown();server.server_close();worker.join(timeout=3)


class DjMixerLiveInputs(unittest.TestCase):
    """R5.1/R5.2 fields the native mixer saves beside its DSP configuration."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="desktop-api-", dir=ROOT / "build")
        self.db = Path(self.temp.name) / "test.sdb"
        self.store = open_radio_store(self.db)
        self.identity = bootstrap(self.store, "admin", "TEST", "Test station")
        self.sid = self.identity["station_id"]
        self.principal = authenticate(self.store, self.identity["api_token"])

    def tearDown(self):
        self.temp.cleanup()

    def put(self, config):
        return api.put_document(self.store, self.principal, self.sid, "dj_mixer", "DJ mixer", config, str(uuid4()))

    def test_labels_inputs_and_network_sources_persist(self):
        config = dj_configuration()
        config["channels"][0]["label"] = "dj"
        config["channels"][1]["label"] = "talk"
        config["channels"][2].update(label="music", source_deck=-1,
                                     input={"kind": "network", "device": "https://akis.example/canli", "name": "Ağ yayını"})
        config["channels"][3].update(source_deck=-1, input={"kind": "capture", "name": "Giriş: Stüdyo",
                                                            "device": "alsa_input.usb-Stüdyo.analog-stereo"})
        config["network_sources"] = [{"name": "Ağ yayını", "url": "https://akis.example/canli"},
                                     {"name": "Saha", "url": "srt://saha.example:9000?mode=caller"}]
        saved = self.put(config)
        self.assertEqual(saved["config"], config)
        reopened = open_radio_store(self.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "dj_mixer", saved["_id"]), saved)
        # Limits are UTF-8 bytes, like the native 128/512-byte buffers.
        edge = copy.deepcopy(config)
        edge["channels"][3]["input"].update(name="ş" * 64, device="a" * 512)
        edge["network_sources"] = [{"name": "n%d" % i, "url": "udp://@:%d" % (1000 + i)} for i in range(32)]
        self.assertEqual(self.put(edge)["config"], edge)
        # The native rule refuses only ASCII spaces and controls; refusing more
        # would fail an autosave the mixer itself accepted.
        native = dj_configuration()
        native["channels"][5].update(source_deck=-1, input={"kind": "network", "device": "http://radyo.example/a\u00a0b",
                                                           "name": "\u00a0"})
        self.put(native)

    def test_labels_inputs_and_network_sources_reject_invalid(self):
        base = dj_configuration()
        base["channels"][4].update(source_deck=-1, input={"kind": "network", "device": "http://radyo.example/a", "name": "A"})
        self.put(copy.deepcopy(base))
        cases = [
            ("label-none", lambda c: c["channels"][0].update(label="none")),
            ("label-empty", lambda c: c["channels"][0].update(label="")),
            ("label-case", lambda c: c["channels"][0].update(label="DJ")),
            ("label-type", lambda c: c["channels"][0].update(label=1)),
            ("kind", lambda c: c["channels"][4]["input"].update(kind="file")),
            ("missing-name", lambda c: c["channels"][4]["input"].pop("name")),
            ("extra-field", lambda c: c["channels"][4]["input"].update(gain=1)),
            ("not-object", lambda c: c["channels"][4].update(input="http://radyo.example/a")),
            ("empty-device", lambda c: c["channels"][4]["input"].update(kind="capture", device="")),
            ("device-bytes", lambda c: c["channels"][4]["input"].update(kind="capture", device="ç" * 257)),
            ("name-bytes", lambda c: c["channels"][4]["input"].update(name="ğ" * 65)),
            ("name-nul", lambda c: c["channels"][4]["input"].update(name="a\0b")),
            ("surrogate", lambda c: c["channels"][4]["input"].update(name="\ud800")),
            ("file-url", lambda c: c["channels"][4]["input"].update(device="file:///etc/passwd")),
            ("tcp-url", lambda c: c["channels"][4]["input"].update(device="tcp://10.0.0.1:5000")),
            ("bare-scheme", lambda c: c["channels"][4]["input"].update(device="http://")),
            ("embedded-scheme", lambda c: c["channels"][4]["input"].update(device="http:x://y")),
            ("space", lambda c: c["channels"][4]["input"].update(device="http://a b/")),
            ("control", lambda c: c["channels"][4]["input"].update(device="http://a/\x07")),
            ("player-too", lambda c: c["channels"][4].update(source_deck=4)),
            ("microphone-too", lambda c: c["channels"][4].update(source_deck=-2)),
            ("sources-type", lambda c: c.update(network_sources={"name": "a"})),
            ("sources-many", lambda c: c.update(network_sources=[{"name": "n", "url": "udp://@:1"}] * 33)),
            ("source-url", lambda c: c.update(network_sources=[{"name": "n", "url": "data:x"}])),
            ("source-name", lambda c: c.update(network_sources=[{"name": "", "url": "udp://@:1"}])),
            ("source-field", lambda c: c.update(network_sources=[{"name": "n", "url": "udp://@:1", "deck": 1}])),
        ]
        for name, mutate in cases:
            config = copy.deepcopy(base)
            mutate(config)
            with self.subTest(case=name), self.assertRaises(StoreError):
                self.put(config)


class DesktopPreferencesViews(unittest.TestCase):
    """R3.4: named views and active_view in the desktop_preferences document."""

    UI = {"text_size": 1, "default_profile": 0, "header_hidden_fields": 7, "content_scale_percent": 73,
          "mixer_scale_percent": 102, "effector_scale_percent": 100, "mixer_height_percent": 100,
          "effector_height_percent": 100}

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="desktop-views-", dir=ROOT / "build")
        self.store = open_radio_store(Path(self.temp.name) / "test.sdb")
        identity = bootstrap(self.store, "admin", "TEST", "Test station")
        self.sid = identity["station_id"]
        self.principal = authenticate(self.store, identity["api_token"])

    def tearDown(self):
        self.temp.cleanup()

    def put(self, config, **extra):
        return api.put_document(self.store, self.principal, self.sid, "desktop_preferences", "Masaüstü tercihleri",
                                config, str(uuid4()), **extra)

    def config(self, views, active=None):
        config = {"schema_version": 1, "ui": dict(self.UI), "views": views}
        if active is not False:
            config["active_view"] = active
        return config

    def view(self, name, **ui):
        return {"name": name, "ui": dict(self.UI, **ui)}

    def test_views_and_active_view_round_trip(self):
        legacy = self.put({"schema_version": 1, "ui": dict(self.UI)})
        views = [self.view("Gece yayını", content_scale_percent=73),
                 self.view("Gündüz · Stüdyo", text_size=2, default_profile=3, header_hidden_fields=5,
                           content_scale_percent=125, mixer_scale_percent=50, effector_height_percent=150),
                 {"name": "Ş" * 40, "ui": {"text_size": 0, "default_profile": 8, "header_hidden_fields": 0}}]
        saved = self.put(self.config(views, "Gündüz · Stüdyo"), document_id=legacy["_id"], expected_revision=legacy["_rev"])
        stored = api.get_document(self.store, self.principal, self.sid, "desktop_preferences", saved["_id"])
        self.assertEqual(stored["config"]["views"], views)
        self.assertEqual(stored["config"]["active_view"], "Gündüz · Stüdyo")
        for config in (self.config(views, None), self.config([], None), self.config([], False),
                       self.config([self.view(f"Görünüm {i}") for i in range(16)], "Görünüm 15")):
            with self.subTest(views=len(config["views"]), active=config.get("active_view", "-")):
                self.put(config)

    def test_views_reject_invalid(self):
        ui = dict(self.UI)
        cases = [
            ("too-many", self.config([self.view(f"V{i}") for i in range(17)])),
            ("duplicate", self.config([self.view("Aynı"), self.view("Aynı")])),
            ("empty-name", self.config([self.view("")])),
            ("space-only", self.config([self.view("   ")])),
            ("leading-space", self.config([self.view(" Gece")])),
            ("trailing-tab", self.config([self.view("Gece\t")])),
            ("control", self.config([self.view("Ge\x07ce")])),
            ("long-name", self.config([self.view("x" * 41)])),
            ("name-type", self.config([{"name": 5, "ui": ui}])),
            ("missing-ui", self.config([{"name": "Gece"}])),
            ("extra-field", self.config([dict(self.view("Gece"), shortcut="F1")])),
            ("not-object", self.config(["Gece"])),
            ("views-type", self.config({"Gece": ui})),
            ("ui-missing-key", self.config([{"name": "Gece", "ui": {"text_size": 1, "default_profile": 0}}])),
            ("ui-extra-key", self.config([self.view("Gece", last_profile=3)])),
            ("ui-scale-low", self.config([self.view("Gece", content_scale_percent=49)])),
            ("ui-scale-high", self.config([self.view("Gece", mixer_height_percent=151)])),
            ("ui-bool", self.config([self.view("Gece", text_size=True)])),
            ("active-unknown", self.config([self.view("Gece")], "Gündüz")),
            ("active-type", self.config([self.view("Gece")], 0)),
            ("active-without-views", {"schema_version": 1, "ui": ui, "active_view": "Gece"}),
        ]
        for name, config in cases:
            with self.subTest(case=name), self.assertRaises(StoreError):
                self.put(config)


if __name__ == "__main__":
    unittest.main(verbosity=2)
