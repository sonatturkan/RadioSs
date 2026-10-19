#!/usr/bin/env python3
"""Real isolated calendar persistence, input, station and HTTP acceptance."""

import copy
from datetime import date, timedelta
from http.client import HTTPConnection
import json
import threading
import unittest
from uuid import uuid4

import test_desktop_api as fixtures
from sdbase import desktop_api as api, desktop_playlist as playlists
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.catalog import NotFound
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError


class DesktopAutomationCalendar(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store = self.fixture.store
        self.sid = self.fixture.sid
        self.principal = self.fixture.principal
        self.playlist = playlists.create_playlist(
            self.store, self.principal, self.sid, "Takvim kaynak listesi",
            idempotency_key=str(uuid4()))

    def add_track_rule_rows(self, count=2, playlist=None):
        playlist = playlist or self.playlist
        metadata = {"title": "Doğru kayıt", "artist": "Sanatçı", "album": "Albüm",
                    "duration_ms": None, "bpm": 0, "genre": "", "key": "",
                    "location": "", "comment": ""}
        result = playlists.append_entries(self.store, self.principal, self.sid,
            playlist["_id"], [dict(metadata) for _ in range(count)],
            expected_revision=playlist["_rev"], idempotency_key=str(uuid4()))
        if playlist["_id"] == self.playlist["_id"]:
            self.playlist = result
        return result

    @staticmethod
    def song_rule(entry_id, **changes):
        return {"entry_id": entry_id, "weight": 100, "order": 0, "frequency": 1,
                "max_plays": 0, "start_minute": 0, "end_minute": 1440,
                "interval_minutes": 0, **changes}

    def test_track_rules_zero_source_weight_sparse_cas_and_reopen(self):
        playlist = self.add_track_rule_rows()
        rules = [self.song_rule(playlist["tracks"][0]["_id"], weight=0, order=4999,
                               frequency=100, max_plays=200, interval_minutes=10080)]
        config = self.named_config(sources=[self.rotation(weight=0,
            track_selection_mode="weighted", track_rules=rules)])
        original = copy.deepcopy(config)
        initial = self.put(config)
        self.assertEqual(config, original)
        changed = copy.deepcopy(config)
        changed["rotation_library"][0]["sources"][0]["weight"] = 70
        changed["rotation_library"][0]["sources"][0]["track_selection_mode"] = "linear"
        saved = self.put(changed, document_id=initial["_id"], expected_revision=initial["_rev"])
        with self.assertRaises(Conflict):
            self.put(config, document_id=initial["_id"], expected_revision=initial["_rev"])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        actual = api.get_document(reopened, self.principal, self.sid,
                                 "automation_calendar", saved["_id"])
        self.assertEqual(actual["config"], changed)
        self.assertEqual(len(actual["config"]["rotation_library"][0]["sources"][0]["track_rules"]), 1)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_track_rule_shapes_types_limits_and_playlist_fences_are_atomic(self):
        playlist = self.add_track_rule_rows()
        valid = self.song_rule(playlist["tracks"][0]["_id"])
        for field, values in {"weight": [True, -1, 101, 0.5],
                "order": [True, -1, 5000], "frequency": [True, 0, 101],
                "max_plays": [True, -1, 201], "start_minute": [True, -1, 1440],
                "end_minute": [True, 0, 1441], "interval_minutes": [True, -1, 10081],
                "entry_id": [None, "not-an-id", str(uuid4())]}.items():
            for value in values:
                with self.subTest(field=field, value=value):
                    rule = dict(valid, **{field: value})
                    self.rejected(self.named_config(sources=[self.rotation(track_rules=[rule])]))
        for rules in [True, {}, [valid] * 501, [valid, dict(valid)],
                      [dict(valid, extra=1)], [dict(valid, start_minute=480, end_minute=480)]]:
            self.rejected(self.named_config(sources=[self.rotation(track_rules=rules)]))
        for field in valid:
            incomplete = dict(valid); del incomplete[field]
            self.rejected(self.named_config(sources=[self.rotation(track_rules=[incomplete])]))
        for mode in [None, True, "random", "", "LINEAR", []]:
            self.rejected(self.named_config(sources=[self.rotation(track_selection_mode=mode)]))
        other = playlists.create_playlist(self.store, self.principal, self.sid,
                                         "Başka liste", idempotency_key=str(uuid4()))
        foreign = self.add_track_rule_rows(1, other)["tracks"][0]["_id"]
        self.rejected(self.named_config(sources=[self.rotation(track_rules=[self.song_rule(foreign)])]))
        # Original draft rows stay unchanged across all failed writes.
        self.assertEqual(self.store.get("desktop_playlist_entries", valid["entry_id"])["status"], "active")

    def test_removed_song_rule_remains_readable_but_cannot_be_resubmitted(self):
        playlist = self.add_track_rule_rows()
        entry = playlist["tracks"][0]
        config = self.named_config(sources=[self.rotation(track_selection_mode="linear",
            track_rules=[self.song_rule(entry["_id"], max_plays=2)])])
        saved = self.put(config)
        playlists.remove_entries(self.store, self.principal, self.sid, playlist["_id"],
            [{"entry_id": entry["_id"], "expected_revision": entry["_rev"]}],
            expected_revision=playlist["_rev"], idempotency_key=str(uuid4()))
        reopened = open_radio_store(self.fixture.db, read_only=True)
        actual = api.get_document(reopened, self.principal, self.sid,
                                 "automation_calendar", saved["_id"])
        self.assertEqual(actual["config"], config)
        self.rejected(config)

    def rotation(self, **changes):
        return {"id": str(uuid4()), "playlist_id": self.playlist["_id"],
                "frequency": 3, "random": True, "track_random": False,
                "advert_random": True, "sweeper_random": False, **changes}

    def event(self, **changes):
        return {"id": str(uuid4()), "title": "Sabah programı · Türkçe",
                "date": "2026-10-09", "start_minute": 480, "end_minute": 600,
                "color": "#90C8EB", "rotations": [self.rotation()], **changes}

    def config(self, *events):
        return {"schema": 1, "events": list(events) if events else [self.event()]}

    def put(self, config, **options):
        return api.put_document(self.store, self.principal, self.sid,
                                "automation_calendar", "Haftalık otomasyon",
                                config, str(uuid4()), **options)

    def rejected(self, config):
        sequence = self.store.sequence
        documents = self.store.scan("desktop_configurations")
        with self.assertRaises(StoreError):
            self.put(config)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store.scan("desktop_configurations"), documents)

    def named_config(self, *, sources=None, settings=None):
        definition = {"id": str(uuid4()), "name": "Sabah seçkisi",
                      "sources": sources if sources is not None else [self.rotation(frequency=1, count=2)],
                      "settings": settings if settings is not None else {
                          "selection_mode": "linear", "track_gap_minutes": 15,
                          "artist_gap_minutes": 30, "album_gap_minutes": 45,
                          "transitions": [{"mode": 0, "weight": 10, "overlap_ms": 0},
                                          {"mode": 1, "weight": 70, "overlap_ms": 2500},
                                          {"mode": 2, "weight": 20, "overlap_ms": 1000}]}}
        event = self.event(rotations=[{"id": str(uuid4()), "rotation_id": definition["id"]}])
        return {"schema": 1, "events": [event], "rotation_library": [definition]}

    @staticmethod
    def autodj_pools():
        return {"music": [1, 32], "advertisement": [3], "jingle": [4], "sweeper": [5]}

    def test_autodj_named_and_legacy_pools_survive_override_disable_cas_reopen(self):
        config = self.named_config(sources=[self.rotation(autodj=self.autodj_pools(),
                                      settings={"artist_gap_minutes": 0})])
        config["events"][0]["rotations"].append(self.rotation(autodj={
            "music": list(range(1, 33)), "advertisement": [1], "jingle": [1], "sweeper": [1]}))
        original = copy.deepcopy(config)
        first = self.put(config)
        self.assertEqual(config, original)
        changed = copy.deepcopy(config)
        del changed["rotation_library"][0]["sources"][0]["settings"]
        saved = self.put(changed, document_id=first["_id"], expected_revision=first["_rev"])
        with self.assertRaises(Conflict):
            self.put(original, document_id=first["_id"], expected_revision=first["_rev"])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        actual = api.get_document(reopened, self.principal, self.sid, "automation_calendar", saved["_id"])
        self.assertEqual(actual["config"], changed)
        self.assertEqual(actual["config"]["rotation_library"][0]["sources"][0]["autodj"], self.autodj_pools())
        self.assertEqual(actual["config"]["rotation_library"][0]["sources"][0]["id"], original["rotation_library"][0]["sources"][0]["id"])
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_autodj_untrusted_pool_values_are_atomic_for_named_and_legacy(self):
        invalids = [None, [], True, {}, {"music": [1]},
                    {**self.autodj_pools(), "extra": [6]}]
        invalids += [{**self.autodj_pools(), "music": value} for value in
                    [[], None, "1", [0], [33], [-1], [True], [1.0], ["1"], [1, 1],
                     list(range(1, 33)) + [1]]]
        for named in (False, True):
            for invalid in invalids:
                with self.subTest(named=named, routing=invalid):
                    config = self.named_config() if named else self.config()
                    source = config["rotation_library"][0]["sources"][0] if named else config["events"][0]["rotations"][0]
                    source["autodj"] = invalid
                    self.rejected(config)

    def test_transition_linear_local_beatmatch_effect_and_sparse_override_reopen(self):
        config = self.named_config(sources=[self.rotation(autodj=self.autodj_pools(),
                                settings={"transition_selection_mode": "weighted"})])
        settings = config["rotation_library"][0]["settings"]
        settings["transition_selection_mode"] = "linear"
        settings["transitions"] = [
            {"mode": 0, "weight": 10, "overlap_ms": 0, "effect": "none"},
            {"mode": 1, "weight": 50, "overlap_ms": 2500, "effect": "lowpass"},
            {"mode": 2, "weight": 20, "overlap_ms": 1000},
            {"mode": 3, "weight": 20, "overlap_ms": 1500, "effect": "lowpass"}]
        before = copy.deepcopy(config)
        saved = self.put(config)
        self.assertEqual(config, before)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid,
                         "automation_calendar", saved["_id"])["config"], before)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_lowpass_requires_overlap_for_enabled_and_disabled_rules_atomically(self):
        for mode in (1, 2):
            for weight in (0, 100):
                for override in (False, True):
                    with self.subTest(mode=mode, weight=weight, override=override):
                        config = self.named_config()
                        choices = [{"mode": 0, "weight": 100, "overlap_ms": 0},
                                   {"mode": mode, "weight": weight, "overlap_ms": 0, "effect": "lowpass"}]
                        if override:
                            config["rotation_library"][0]["sources"][0]["settings"] = {"transitions": choices}
                        else:
                            config["rotation_library"][0]["settings"]["transitions"] = choices
                        self.rejected(config)

    def test_transition_new_untrusted_fields_fail_without_writes(self):
        for value in [True, 0, None, {}, [], "random"]:
            for override in (False, True):
                with self.subTest(value=value, override=override):
                    config = self.named_config()
                    if override:
                        config["rotation_library"][0]["sources"][0]["settings"] = {"transition_selection_mode": value}
                    else:
                        config["rotation_library"][0]["settings"]["transition_selection_mode"] = value
                    self.rejected(config)
        choices = [
            {"mode": 0, "weight": 1, "overlap_ms": 0, "effect": "lowpass"},
            {"mode": 3, "weight": 1, "overlap_ms": 0},
            {"mode": 4, "weight": 1, "overlap_ms": 1},
            {"mode": 1, "weight": 1, "overlap_ms": 1, "beat_sync": True}]
        choices += [{"mode": 1, "weight": 1, "overlap_ms": 1, "effect": value}
                    for value in [True, 1, None, [], {}, "reverb"]]
        for choice in choices:
            with self.subTest(choice=choice):
                config = self.named_config()
                config["rotation_library"][0]["settings"]["transitions"] = [choice]
                self.rejected(config)

    def test_autodj_kind_policy_and_channels_persist_and_reject_bad_values(self):
        config = self.named_config()
        settings = config["rotation_library"][0]["settings"]
        # SYNC beatmatch may carry the share of its transitions (the rest crossfade).
        settings["kind_transitions"] = {"music": {"mode": 3, "overlap_ms": 8000, "sync_percent": 60},
                                        "advertisement": {"mode": 0, "overlap_ms": 0},
                                        "jingle": {"mode": 1, "overlap_ms": 1000}, "sweeper": {"mode": 2, "overlap_ms": 500}}
        settings["autodj_players"] = {"music": [1, 2], "advertisement": [3], "jingle": [3], "sweeper": [3, 4]}
        saved = self.put(config)
        self.assertEqual(saved["config"]["rotation_library"][0]["settings"]["kind_transitions"], settings["kind_transitions"])
        self.assertEqual(saved["config"]["rotation_library"][0]["settings"]["autodj_players"], settings["autodj_players"])
        bad_policies = [{"music": {"mode": 3, "overlap_ms": 0}}, {"music": {"mode": 0, "overlap_ms": 10}},
                        {"music": {"mode": 4, "overlap_ms": 10}}, {"music": {"mode": 1}}, {"music": {"mode": 1, "overlap_ms": 40000}},
                        {"music": {"mode": 1, "overlap_ms": 1000, "sync_percent": 50}},
                        {"music": {"mode": 3, "overlap_ms": 1000, "sync_percent": 101}},
                        {"music": {"mode": 3, "overlap_ms": 1000, "sync_percent": "50"}},
                        {"music": {"mode": 3, "overlap_ms": 1000, "sync": 50}}]
        for change in bad_policies:
            with self.subTest(change=change):
                broken = copy.deepcopy(config)
                broken["rotation_library"][0]["settings"]["kind_transitions"].update(change)
                self.rejected(broken)
        for players in ([], [0], [1, 1], [33], ["1"]):
            with self.subTest(players=players):
                broken = copy.deepcopy(config)
                broken["rotation_library"][0]["settings"]["autodj_players"]["jingle"] = players
                self.rejected(broken)
        broken = copy.deepcopy(config)
        del broken["rotation_library"][0]["settings"]["kind_transitions"]["sweeper"]
        self.rejected(broken)

    @staticmethod
    def transition_preset(name="Yankı ile çık", **changes):
        def lane(volume):
            return {"volume": volume, "bass": [[0, 1], [1, 1]], "tone": [[0, 0], [1, 0]], "tone_kind": "none"}
        preset = {"id": str(uuid4()), "name": name, "duration_ms": 6000, "beat_sync": True,
                  "out": lane([[0, 1], [0.6, 0.8], [1, 0]]), "in": lane([[0, 0], [0.4, 1], [1, 1]]),
                  "effect": {"type": "echo", "depth": 70, "side": "out", "mix": [[0, 0], [1, 1]], "bpm": 120}}
        preset.update(changes)
        return preset

    def test_kind_transition_distribution_persists_and_rejects_bad_entries(self):
        # Contract 18.10 (AutoDJ page): kind entries give a content kind its own
        # share per transition type or designed preset; the remainder resolves
        # into the rotation's kind-less distribution, which stays required.
        config = self.named_config()
        settings = config["rotation_library"][0]["settings"]
        first, second = self.transition_preset(), self.transition_preset("Düz geçiş", beat_sync=False)
        settings["autodj_presets"] = [first, second]
        settings["autodj_automation"] = {"enabled": True, "kinds": {"jingle": {"presets": [second["id"]], "rotation": "sequential"}}}
        settings["transitions"] = [
            {"mode": 0, "weight": 100, "overlap_ms": 0},
            {"kind": "music", "mode": 1, "weight": 40, "overlap_ms": 3000},
            {"kind": "music", "mode": 3, "weight": 30, "overlap_ms": 8000, "effect": "lowpass"},
            {"kind": "music", "preset_id": first["id"], "mode": 1, "overlap_ms": 0, "weight": 30},
            {"kind": "jingle", "preset_id": second["id"], "mode": 1, "overlap_ms": 0, "weight": 100}]
        saved = self.put(config)
        self.assertEqual(saved["config"]["rotation_library"][0]["settings"]["transitions"], settings["transitions"])
        base = {"mode": 0, "weight": 100, "overlap_ms": 0}
        bad = [
            [{"kind": "music", "mode": 1, "weight": 100, "overlap_ms": 3000}],
            [base, {"kind": "music", "mode": 1, "weight": 60, "overlap_ms": 3000}, {"kind": "music", "mode": 2, "weight": 41, "overlap_ms": 500}],
            [base, {"kind": "music", "mode": 1, "weight": 10, "overlap_ms": 3000}, {"kind": "music", "mode": 1, "weight": 10, "overlap_ms": 2000}],
            [base, {"kind": "music", "mode": 1, "weight": 0, "overlap_ms": 3000}],
            [base, {"kind": "talk", "mode": 1, "weight": 10, "overlap_ms": 3000}],
            [base, {"kind": "music", "mode": 0, "weight": 10, "overlap_ms": 500}],
            [base, {"kind": "music", "mode": 3, "weight": 10, "overlap_ms": 0}],
            [base, {"kind": "music", "mode": 1, "weight": 10, "overlap_ms": 3000, "extra": 1}],
            [base, {"kind": "music", "preset_id": str(uuid4()), "mode": 1, "overlap_ms": 0, "weight": 10}],
            [base, {"kind": "music", "preset_id": first["id"], "mode": 1, "overlap_ms": 500, "weight": 10}],
            [base, {"kind": "music", "preset_id": first["id"], "mode": 2, "overlap_ms": 0, "weight": 10}],
            [base, {"kind": "music", "preset_id": first["id"], "mode": 1, "overlap_ms": 0, "weight": 10},
             {"kind": "music", "preset_id": first["id"], "mode": 1, "overlap_ms": 0, "weight": 10}],
            [base, {"kind": "music", "preset_id": second["id"], "mode": 1, "overlap_ms": 0, "weight": 10}],
            [base, {"preset_id": first["id"], "mode": 1, "overlap_ms": 0, "weight": 10}],
            [base] + [{"kind": "music", "mode": 1, "weight": 1, "overlap_ms": 1000}] * 64]
        for choices in bad:
            with self.subTest(choices=choices[:3]):
                broken = copy.deepcopy(saved["config"])
                broken["rotation_library"][0]["settings"]["transitions"] = choices
                self.rejected(broken)
        override = copy.deepcopy(saved["config"])
        override["rotation_library"][0]["sources"][0]["settings"] = {"transitions": [
            base, {"kind": "music", "preset_id": first["id"], "mode": 1, "overlap_ms": 0, "weight": 10}]}
        self.rejected(override)

    def test_autodj_presets_and_automation_persist_and_reject_bad_values(self):
        config = self.named_config()
        settings = config["rotation_library"][0]["settings"]
        first, second = self.transition_preset(), self.transition_preset("Düz geçiş", beat_sync=False)
        settings["autodj_presets"] = [first, second]
        settings["autodj_automation"] = {
            "enabled": True,
            "kinds": {"music": {"presets": [first["id"], second["id"]], "rotation": "sequential"},
                      "jingle": {"presets": [], "rotation": "random"}},
            # A night window crosses midnight: end before start is allowed.
            "schedule": [{"id": str(uuid4()), "name": "Gece", "days": [5, 6, 7], "start_minute": 1320,
                          "end_minute": 360, "kind": "music", "presets": [second["id"]], "rotation": "random"}]}
        saved = self.put(config)
        stored = saved["config"]["rotation_library"][0]["settings"]
        self.assertEqual(stored["autodj_presets"], settings["autodj_presets"])
        self.assertEqual(stored["autodj_automation"], settings["autodj_automation"])
        lane = {"volume": [[0, 1], [1, 0]], "bass": [[0, 1], [1, 1]], "tone": [[0, 0], [1, 0]], "tone_kind": "none"}
        bad_presets = [
            self.transition_preset(duration_ms=100),
            self.transition_preset(name=" "),
            self.transition_preset(effect={"type": "chorus", "depth": 50, "side": "out", "mix": [[0, 0], [1, 1]]}),
            self.transition_preset(effect={"type": "echo", "depth": 101, "side": "out", "mix": [[0, 0], [1, 1]]}),
            self.transition_preset(effect={"type": "echo", "depth": 50, "side": "both", "mix": [[0, 0], [1, 1]]}),
            self.transition_preset(out={**lane, "volume": [[0, 1], [0.5, 1]]}),
            self.transition_preset(out={**lane, "volume": [[0, 1], [0.7, 0], [0.4, 1], [1, 0]]}),
            self.transition_preset(out={**lane, "volume": [[0, 1], [1, True]]}),
            self.transition_preset(out={**lane, "tone_kind": "bandpass"}),
            self.transition_preset(id="not-a-uuid"),
            {**self.transition_preset(), "extra": 1},
        ]
        for preset in bad_presets:
            with self.subTest(preset=preset):
                broken = copy.deepcopy(config)
                broken["rotation_library"][0]["settings"]["autodj_presets"] = [preset]
                broken["rotation_library"][0]["settings"]["autodj_automation"] = {"enabled": False, "kinds": {}}
                self.rejected(broken)
        duplicate = copy.deepcopy(config)
        duplicate["rotation_library"][0]["settings"]["autodj_presets"] = [first, dict(first)]
        self.rejected(duplicate)
        bad_automation = [
            {"enabled": True, "kinds": {"music": {"presets": [str(uuid4())], "rotation": "sequential"}}},
            {"enabled": True, "kinds": {"news": {"presets": [], "rotation": "sequential"}}},
            {"enabled": True, "kinds": {"music": {"presets": [first["id"]], "rotation": "shuffle"}}},
            {"enabled": True, "kinds": {"music": {"presets": [first["id"], first["id"]], "rotation": "random"}}},
            {"enabled": "yes", "kinds": {}},
            {"enabled": True, "kinds": {}, "schedule": [{"id": str(uuid4()), "name": "Boş", "days": [1], "start_minute": 0,
                                                       "end_minute": 1440, "kind": "all", "presets": [], "rotation": "sequential"}]},
            {"enabled": True, "kinds": {}, "schedule": [{"id": str(uuid4()), "name": "Aynı saat", "days": [1], "start_minute": 600,
                                                       "end_minute": 600, "kind": "all", "presets": [first["id"]], "rotation": "sequential"}]},
            {"enabled": True, "kinds": {}, "schedule": [{"id": str(uuid4()), "name": "Gün", "days": [0], "start_minute": 0,
                                                       "end_minute": 1440, "kind": "music", "presets": [first["id"]], "rotation": "sequential"}]},
            {"enabled": True, "kinds": {}, "schedule": [{"id": str(uuid4()), "name": "Tür", "days": [1], "start_minute": 0,
                                                       "end_minute": 1440, "kind": "news", "presets": [first["id"]], "rotation": "sequential"}]},
        ]
        for automation in bad_automation:
            with self.subTest(automation=automation):
                broken = copy.deepcopy(config)
                broken["rotation_library"][0]["settings"]["autodj_automation"] = automation
                self.rejected(broken)
        # Presets belong to the rotation, never to a playlist override.
        override = copy.deepcopy(config)
        override["rotation_library"][0]["sources"][0]["settings"] = {"autodj_presets": [first]}
        self.rejected(override)

    def test_named_shared_rotation_sparse_override_cas_and_reopen(self):
        config = self.named_config(sources=[self.rotation(frequency=3, count=2, weight=70,
                            start_minute=495, end_minute=1290, interval_minutes=45,
                            settings={"artist_gap_minutes": 0}), self.rotation(frequency=1)])
        definition = config["rotation_library"][0]
        config["events"].append(self.event(start_minute=600, end_minute=720,
                        rotations=[{"id": str(uuid4()), "rotation_id": definition["id"]}]))
        initial = self.put(config)
        changed = copy.deepcopy(config)
        changed["rotation_library"][0]["name"] = "Akşam seçkisi"
        changed["rotation_library"][0]["settings"]["selection_mode"] = "weighted"
        saved = self.put(changed, document_id=initial["_id"], expected_revision=initial["_rev"])
        with self.assertRaises(Conflict):
            self.put(config, document_id=initial["_id"], expected_revision=initial["_rev"])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        actual = api.get_document(reopened, self.principal, self.sid, "automation_calendar", saved["_id"])
        self.assertEqual(actual["config"], changed)
        self.assertEqual(actual["config"]["events"], config["events"])
        self.assertEqual(actual["config"]["rotation_library"][0]["sources"], definition["sources"])
        self.assertEqual(actual["config"]["rotation_library"][0]["sources"][0]["settings"], {"artist_gap_minutes": 0})

    def test_named_empty_library_empty_draft_and_legacy_mix(self):
        self.put({"schema": 1, "events": [], "rotation_library": []})
        config = self.named_config(sources=[])
        config["events"][0]["rotations"].append(self.rotation())
        saved = self.put(config)
        self.assertEqual(saved["config"], config)

    def test_named_settings_untrusted_types_and_transition_bounds_are_atomic(self):
        for field, invalids in {
                "selection_mode": [[], {}, True, 1, None, "random"],
                "track_gap_minutes": [True, -1, 10081, 0.5],
                "artist_gap_minutes": ["30", -1], "album_gap_minutes": [None, 10081],
                "transitions": [[], True, [{"mode": 0, "weight": 0, "overlap_ms": 0}],
                   [{"mode": 0, "weight": 1, "overlap_ms": 1}],
                   [{"mode": 1, "weight": 1, "overlap_ms": 30001}],
                   [{"mode": 1, "weight": True, "overlap_ms": 0}],
                   [{"mode": 3, "weight": 1, "overlap_ms": 0}],
                   [{"mode": 1, "weight": 1, "overlap_ms": 0}, {"mode": 1, "weight": 1, "overlap_ms": 0}]]}.items():
            for invalid in invalids:
                with self.subTest(field=field, value=invalid):
                    config = self.named_config()
                    config["rotation_library"][0]["settings"][field] = invalid
                    self.rejected(config)
        config = self.named_config()
        del config["rotation_library"][0]["settings"]["album_gap_minutes"]
        self.rejected(config)

    def test_named_source_bounds_and_sparse_override_preserve_default(self):
        for field, invalid in [("weight", -1), ("weight", 101), ("count", 0), ("count", True),
                ("frequency", 0), ("start_minute", 1440), ("end_minute", 0),
                ("interval_minutes", 10081), ("settings", {"selection_mode": "weighted"}),
                ("settings", {"artist_gap_minutes": -1}), ("entry_ids", [])]:
            with self.subTest(field=field, value=invalid):
                config = self.named_config(); config["rotation_library"][0]["sources"][0][field] = invalid
                self.rejected(config)
        config = self.named_config()
        config["rotation_library"][0]["sources"][0].update(start_minute=600, end_minute=600)
        self.rejected(config)
        config = self.named_config(sources=[self.rotation(frequency=1, settings={})])
        self.assertEqual(self.put(config)["config"], config)

    def test_named_reference_identity_and_expanded_limits_are_atomic(self):
        for modification in ("unknown", "mixed_assignment", "duplicate_assignment", "source_collision", "expanded"):
            with self.subTest(modification=modification):
                config = self.named_config()
                definition, event = config["rotation_library"][0], config["events"][0]
                if modification == "unknown": event["rotations"][0]["rotation_id"] = str(uuid4())
                elif modification == "mixed_assignment": event["rotations"][0]["playlist_id"] = self.playlist["_id"]
                elif modification == "duplicate_assignment": event["rotations"].append(copy.deepcopy(event["rotations"][0]))
                elif modification == "source_collision": definition["sources"][0]["id"] = definition["id"]
                else:
                    definition["sources"] = [self.rotation(frequency=1) for _ in range(26)]
                    event["rotations"].append({"id": str(uuid4()), "rotation_id": definition["id"]})
                self.rejected(config)
        config = self.named_config()
        config["rotation_library"] = [{"id": str(uuid4()), "name": "Taslak", "sources": [],
                                        "settings": copy.deepcopy(config["rotation_library"][0]["settings"])} for _ in range(201)]
        config["events"] = []
        self.rejected(config)

    def test_named_unused_definition_playlist_station_fence(self):
        config = self.named_config()
        config["events"] = []
        config["rotation_library"][0]["sources"][0]["playlist_id"] = str(uuid4())
        self.rejected(config)

    def test_empty_calendar_and_midnight_adjacent_intervals_persist(self):
        empty = self.put({"schema": 1, "events": []})
        config = self.config(self.event(start_minute=0, end_minute=480),
                             self.event(start_minute=480, end_minute=1439),
                             self.event(start_minute=1439, end_minute=1440))
        saved = self.put(config, document_id=empty["_id"], expected_revision=empty["_rev"])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid,
                         "automation_calendar", saved["_id"]), saved)
        self.assertEqual(saved["config"], config)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_event_order_rotation_order_and_cas_reorder(self):
        first = self.event(start_minute=1000, end_minute=1100,
                           rotations=[self.rotation(), self.rotation(frequency=9)])
        second = self.event(start_minute=0, end_minute=100)
        config = self.config(first, second)
        before = copy.deepcopy(config)
        old = self.put(config)
        self.assertEqual(config, before)
        self.assertEqual(old["config"], before)
        changed = copy.deepcopy(config)
        changed["events"].reverse()
        changed["events"][1]["rotations"].reverse()
        new = self.put(changed, document_id=old["_id"], expected_revision=old["_rev"])
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            self.put(config, document_id=old["_id"], expected_revision=old["_rev"])
        self.assertEqual(self.store.sequence, sequence)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid,
                         "automation_calendar", new["_id"])["config"], changed)

    def test_replay_preserves_revision_sequence_and_detects_changed_payload(self):
        config = self.config()
        key = str(uuid4())
        arguments = (self.store, self.principal, self.sid, "automation_calendar",
                     "Haftalık otomasyon", config, key)
        saved = api.put_document(*arguments)
        sequence = self.store.sequence
        self.assertEqual(api.put_document(*arguments), saved)
        self.assertEqual(self.store.sequence, sequence)
        changed = copy.deepcopy(config)
        changed["events"][0]["title"] = "Yeni program adı"
        with self.assertRaises(Conflict):
            api.put_document(*arguments[:5], changed, key)
        self.assertEqual(self.store.sequence, sequence)

    def test_copy_requires_fresh_event_and_rotation_ids(self):
        first = self.event(rotations=[self.rotation(), self.rotation()])
        duplicate = copy.deepcopy(first)
        duplicate["date"] = "2026-10-10"
        self.rejected(self.config(first, duplicate))
        duplicate["id"] = str(uuid4())
        self.rejected(self.config(first, duplicate))
        for rotation in duplicate["rotations"]:
            rotation["id"] = str(uuid4())
        saved = self.put(self.config(first, duplicate))
        self.assertEqual(saved["config"]["events"][0], first)
        self.assertEqual(saved["config"]["events"][1], duplicate)
        identities = [event["id"] for event in saved["config"]["events"]]
        identities += [rotation["id"] for event in saved["config"]["events"]
                       for rotation in event["rotations"]]
        self.assertEqual(len(identities), len(set(identities)))
        aliased = copy.deepcopy(first)
        aliased["rotations"][0]["id"] = first["id"]
        self.rejected(self.config(aliased))

    def test_overlaps_rejected_regardless_of_input_order(self):
        for start, end in ((479, 481), (480, 600), (500, 510), (550, 650), (400, 700)):
            first, second = self.event(), self.event(start_minute=start, end_minute=end)
            for events in ((first, second), (second, first)):
                with self.subTest(start=start, end=end):
                    self.rejected(self.config(*events))
        same_time_other_day = self.event(date="2026-10-10")
        self.assertEqual(len(self.put(self.config(self.event(), same_time_other_day))["config"]["events"]), 2)

    def test_top_level_missing_unknown_and_wrong_types_are_atomic(self):
        invalid = [None, [], {}, {"schema": 1}, {"events": []},
                   {"schema": True, "events": []}, {"schema": 2, "events": []},
                   {"schema": 1.0, "events": []}, {"schema": 1, "events": {}},
                   {"schema": 1, "events": [], "playing": True}]
        for config in invalid:
            with self.subTest(config=config):
                self.rejected(config)

    def test_event_time_date_color_text_and_field_types_are_atomic(self):
        invalid = [("id", "bad-uuid"), ("id", 1), ("title", ""), ("title", "  "),
                   ("title", "x" * 161), ("title", "bad\0title"), ("title", True),
                   ("date", "2026-02-29"), ("date", "2026-2-09"), ("date", None),
                   ("start_minute", -1), ("start_minute", 1440), ("start_minute", True),
                   ("start_minute", 1.0), ("end_minute", 0), ("end_minute", 1441),
                   ("end_minute", False), ("end_minute", 480), ("end_minute", 479),
                   ("color", "red"), ("color", "#12345"), ("color", "#GG0000"),
                   ("color", None), ("rotations", {}), ("runtime", "playing")]
        for key, value in invalid:
            with self.subTest(key=key, value=value):
                self.rejected(self.config(self.event(**{key: value})))
        for key in self.event():
            incomplete = self.event()
            del incomplete[key]
            with self.subTest(missing=key):
                self.rejected(self.config(incomplete))
        self.rejected(self.config(None))

    def test_rotation_uuid_reference_frequency_and_flags_are_atomic(self):
        invalid = [("id", "bad"), ("playlist_id", str(uuid4())),
                   ("playlist_id", None), ("frequency", 0), ("frequency", 101),
                   ("frequency", True), ("frequency", 1.0), ("frequency", "3"),
                   ("random", 1), ("track_random", "true"), ("advert_random", None),
                   ("sweeper_random", []), ("broadcasted", True)]
        for key, value in invalid:
            with self.subTest(key=key, value=value):
                self.rejected(self.config(self.event(rotations=[self.rotation(**{key: value})])))
        for key in self.rotation():
            incomplete = self.rotation()
            del incomplete[key]
            with self.subTest(missing=key):
                self.rejected(self.config(self.event(rotations=[incomplete])))
        self.rejected(self.config(self.event(rotations=[None])))

    def test_optional_media_kind_preserves_legacy_and_survives_cas_reopen(self):
        legacy = self.config()
        saved_legacy = self.put(legacy)
        self.assertEqual(saved_legacy["config"], legacy)
        self.assertNotIn("media_kind", saved_legacy["config"]["events"][0]["rotations"][0])
        kinds = ("all", "music", "jingle", "advertisement", "sweeper")
        config = self.config(self.event(rotations=[self.rotation(media_kind=kind) for kind in kinds]))
        before = copy.deepcopy(config)
        saved = self.put(config)
        self.assertEqual(config, before)
        self.assertEqual(saved["config"], before)
        changed = copy.deepcopy(config)
        changed["events"][0]["rotations"][1]["media_kind"] = "jingle"
        updated = self.put(changed, document_id=saved["_id"], expected_revision=saved["_rev"])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid,
                         "automation_calendar", updated["_id"])["config"], changed)
        for original, rotation in zip(config["events"][0]["rotations"], changed["events"][0]["rotations"]):
            self.assertEqual({key: value for key, value in original.items() if key != "media_kind"},
                             {key: value for key, value in rotation.items() if key != "media_kind"})
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_optional_media_kind_unknown_and_wrong_types_are_atomic(self):
        for value in ("unknown", "track", "Music", "", "music ", None, True, 1, [], {}):
            with self.subTest(media_kind=value):
                self.rejected(self.config(self.event(rotations=[self.rotation(media_kind=value)])))

    def test_event_and_rotation_capacity_limits(self):
        events = [self.event(date=(date(2026, 10, 9) + timedelta(days=index)).isoformat(),
                             rotations=[]) for index in range(500)]
        self.assertEqual(len(self.put(self.config(*events))["config"]["events"]), 500)
        self.rejected(self.config(*events, self.event(date="2028-10-09")))
        event = self.event(rotations=[self.rotation(frequency=100) for _ in range(50)])
        self.assertEqual(len(self.put(self.config(event))["config"]["events"][0]["rotations"]), 50)
        event["rotations"].append(self.rotation())
        self.rejected(self.config(event))

    def test_disabled_playlist_keeps_saved_calendar_readable_but_blocks_new_save(self):
        saved = self.put(self.config())
        playlists.delete_playlist(self.store, self.principal, self.sid, self.playlist["_id"],
                                  expected_revision=self.playlist["_rev"], idempotency_key=str(uuid4()))
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid,
                         "automation_calendar", saved["_id"]), saved)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        self.rejected(saved["config"])

    def test_disabled_source_allows_unrelated_edit_but_rejects_changed_or_copied_row(self):
        config = self.config()
        config["events"][0]["id"] = config["events"][0]["id"].upper()
        config["events"][0]["rotations"][0]["id"] = config["events"][0]["rotations"][0]["id"].upper()
        saved = self.put(config)
        deleted = playlists.delete_playlist(self.store, self.principal, self.sid,
            self.playlist["_id"], expected_revision=self.playlist["_rev"], idempotency_key=str(uuid4()))
        self.assertEqual(deleted["configuration_uses"][0]["document_id"], saved["_id"])
        self.assertIn(saved["name"], deleted["warning"])
        changed = copy.deepcopy(saved["config"])
        changed["events"][0]["title"] = "Yeni başlık"
        actual = self.put(changed, document_id=saved["_id"], expected_revision=saved["_rev"])
        for mutate in (lambda row: row.update(frequency=4),
                       lambda row: row.update(id=str(uuid4()))):
            bad = copy.deepcopy(changed)
            mutate(bad["events"][0]["rotations"][0])
            sequence = self.store.sequence
            with self.assertRaisesRegex(StoreError, "calendar playlist_id.*rotation"):
                self.put(bad, document_id=actual["_id"], expected_revision=actual["_rev"])
            self.assertEqual(self.store.sequence, sequence)
        # A different event is a new use, even if a row's identity is moved.
        moved = copy.deepcopy(changed)
        moved["events"][0]["id"] = str(uuid4())
        with self.assertRaises(StoreError):
            self.put(moved, document_id=actual["_id"], expected_revision=actual["_rev"])
        self.assertEqual(inspect_integrity(open_radio_store(self.fixture.db, read_only=True))["status"], "ok")

    def test_disabled_library_preserves_existing_assignment_but_rejects_new_assignment(self):
        saved = self.put(self.named_config())
        playlists.delete_playlist(self.store, self.principal, self.sid, self.playlist["_id"],
            expected_revision=self.playlist["_rev"], idempotency_key=str(uuid4()))
        changed = copy.deepcopy(saved["config"])
        changed["rotation_library"][0]["name"] = "Yeni rotasyon adı"
        actual = self.put(changed, document_id=saved["_id"], expected_revision=saved["_rev"])
        changed["events"].append(self.event(date="2026-10-10", rotations=[{
            "id": str(uuid4()), "rotation_id": changed["rotation_library"][0]["id"]}]))
        with self.assertRaisesRegex(StoreError, "calendar assigned playlist_id"):
            self.put(changed, document_id=actual["_id"], expected_revision=actual["_rev"])

    def test_optional_timezone_preserves_local_dates_and_rejects_invalid_zones(self):
        config = self.config(self.event(start_minute=540, end_minute=600))
        config["timezone"] = "Europe/Istanbul"
        saved = self.put(config)
        self.assertEqual(saved["config"], config)
        for zone in ("Unknown/Nowhere", "", True, None, "/etc/passwd", "../UTC"):
            with self.subTest(zone=zone):
                self.rejected(dict(config, timezone=zone))

    def test_idempotent_calendar_write_replays_after_playlist_is_disabled(self):
        config, key = self.config(), str(uuid4())
        saved = api.put_document(self.store, self.principal, self.sid, "automation_calendar",
            "Takvim", config, key)
        playlists.delete_playlist(self.store, self.principal, self.sid, self.playlist["_id"],
            expected_revision=self.playlist["_rev"], idempotency_key=str(uuid4()))
        sequence = self.store.sequence
        replay = api.put_document(self.store, self.principal, self.sid, "automation_calendar",
            "Takvim", config, key)
        self.assertEqual(replay, saved)
        self.assertEqual(self.store.sequence, sequence)

    def test_assignment_weight_mode_cas_reopen_and_legacy_preservation(self):
        config = self.named_config()
        config['events'][0]['rotation_selection_mode'] = 'weighted'
        config['events'][0]['rotations'][0]['weight'] = 0
        config['events'][0]['rotations'].append(self.rotation())
        original = copy.deepcopy(config)
        saved = self.put(config)
        changed = copy.deepcopy(config); changed['events'][0]['rotation_selection_mode'] = 'linear'
        changed['events'][0]['rotations'][0]['weight'] = 100
        actual = self.put(changed, document_id=saved['_id'], expected_revision=saved['_rev'])
        with self.assertRaises(Conflict): self.put(config, document_id=saved['_id'], expected_revision=saved['_rev'])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, 'automation_calendar', actual['_id'])['config'], changed)
        self.assertEqual(config, original)
        self.assertNotIn('weight', actual['config']['events'][0]['rotations'][1])
        self.assertEqual(inspect_integrity(reopened)['status'], 'ok')

    def test_assignment_weight_mode_types_and_legacy_unknown_are_atomic(self):
        for value in [True, -1, 101, 1.5, None, '50']:
            config = self.named_config(); config['events'][0]['rotations'][0]['weight'] = value; self.rejected(config)
        for value in [True, None, 'random', '', []]:
            config = self.named_config(); config['events'][0]['rotation_selection_mode'] = value; self.rejected(config)
        config = self.named_config(); config['events'][0]['rotations'][0]['extra'] = 1; self.rejected(config)
        config = self.config(); config['events'][0]['rotations'][0]['weight'] = 50; self.rejected(config)

    def test_station_permissions_and_foreign_playlist_reference(self):
        saved = self.put(self.config())
        viewer = create_station_user(self.store, self.principal, self.sid,
                                     "calendar-viewer", "Takvim izleyicisi", ["view"])
        viewer_principal = authenticate(self.store, viewer["token"])
        self.assertEqual(api.list_documents(self.store, viewer_principal, self.sid,
                         "automation_calendar")["records"], [saved])
        with self.assertRaises(Forbidden):
            api.put_document(self.store, viewer_principal, self.sid, "automation_calendar",
                             "Denied", self.config(), str(uuid4()))
        other = str(uuid4())
        self.store.put("stations", other, {"code": "CALENDAR-OTHER", "name": "Other",
                                          "timezone": "UTC", "status": "active"})
        other_playlist = playlists.create_playlist(self.store, self.principal, other,
                                                   "Foreign source", idempotency_key=str(uuid4()))
        self.rejected(self.config(self.event(rotations=[self.rotation(playlist_id=other_playlist["_id"])])))
        with self.assertRaises(Forbidden):
            api.list_documents(self.store, viewer_principal, other, "automation_calendar")
        with self.assertRaises(NotFound):
            api.get_document(self.store, self.principal, other, "automation_calendar", saved["_id"])

    def test_http_put_get_overlap_replay_and_stale_revision(self):
        base = make_handler(self.store)
        class Handler(base):
            def log_message(self, *arguments):
                pass
        server = BoundedHTTPServer(("127.0.0.1", 0), Handler, request_timeout=2)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        def request(operation, body, auth=True):
            connection = HTTPConnection("127.0.0.1", server.server_port, timeout=2)
            headers = {"Content-Type": "application/json", "Connection": "close"}
            if auth:
                headers["Authorization"] = "Bearer " + self.fixture.identity["api_token"]
            connection.request("POST", "/v1/workflows/desktop/" + operation, json.dumps(body), headers)
            response = connection.getresponse()
            status, value = response.status, json.loads(response.read())
            connection.close()
            return status, value
        try:
            body = {"station_id": self.sid, "kind": "automation_calendar", "name": "HTTP takvim",
                    "config": self.config(), "idempotency_key": str(uuid4())}
            self.assertEqual(request("put", body, False)[0], 403)
            status, saved = request("put", body)
            self.assertEqual(status, 200)
            sequence = self.store.sequence
            self.assertEqual(request("put", body), (200, saved))
            self.assertEqual(self.store.sequence, sequence)
            self.assertEqual(request("get", {"station_id": self.sid, "kind": "automation_calendar",
                             "document_id": saved["_id"]}), (200, saved))
            update = dict(body, document_id=saved["_id"], expected_revision=saved["_rev"],
                          idempotency_key=str(uuid4()), name="Changed")
            self.assertEqual(request("put", update)[0], 200)
            self.assertEqual(request("put", dict(update, idempotency_key=str(uuid4())))[0], 409)
            invalid = dict(body, idempotency_key=str(uuid4()), config=self.config(self.event(), self.event()))
            self.assertEqual(request("put", invalid)[0], 422)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=3)
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_excluded_kinds_rotation_level_distinct_known_and_reopen(self):
        settings = {"selection_mode": "linear", "track_gap_minutes": 0, "artist_gap_minutes": 0, "album_gap_minutes": 0,
                    "transitions": [{"mode": 0, "weight": 100, "overlap_ms": 0}],
                    "automation_schema": True, "excluded_kinds": ["advertisement", "sweeper"]}
        config = self.named_config(settings=settings)
        original = copy.deepcopy(config)
        saved = self.put(config)
        self.assertEqual(config, original)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        actual = api.get_document(reopened, self.principal, self.sid, "automation_calendar", saved["_id"])
        self.assertEqual(actual["config"]["rotation_library"][0]["settings"]["excluded_kinds"], ["advertisement", "sweeper"])
        weekly = {"schema": 1, "week_anchor": "2026-10-05", "week_count": 1, "repeat": True, "calendar": copy.deepcopy(config)}
        saved_weekly = api.put_document(self.store, self.principal, self.sid, "weekly_programme", "Haftalık", weekly, str(uuid4()))
        self.assertEqual(saved_weekly["config"], weekly)
        for value in (["Reklam"], ["music", "music"], "advertisement", [1], {}, None,
                      ["music", "advertisement", "jingle", "sweeper", "news"]):
            bad = copy.deepcopy(config)
            bad["rotation_library"][0]["settings"]["excluded_kinds"] = value
            with self.subTest(value=value):
                self.rejected(bad)
        bad = copy.deepcopy(config)
        bad["rotation_library"][0]["sources"][0]["settings"] = {"excluded_kinds": ["jingle"]}
        self.rejected(bad)
        empty = copy.deepcopy(config)
        empty["rotation_library"][0]["settings"]["excluded_kinds"] = []
        self.put(empty, document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")


    # --- Kılavuz 4c: şema akışı (automation_steps / link_transitions / programme_entry) ---

    @staticmethod
    def schema_settings(**changes):
        return {"selection_mode": "linear", "track_gap_minutes": 0, "artist_gap_minutes": 0, "album_gap_minutes": 0,
                "transitions": [{"mode": 0, "weight": 100, "overlap_ms": 0}], "automation_schema": True, **changes}

    def schema_config(self):
        """One active schema with playlist A (steps 1,2), live step and playlist C, plus a live event schema."""
        preset = self.transition_preset("Konuşmaya giriş")
        a, c = self.rotation(frequency=1, count=1, start_date="2026-10-01", end_date="2026-10-31",
                             settings={"automation_steps": [{"order": 1, "kind": "music"}, {"order": 2, "kind": "music"}]}), \
               self.rotation(frequency=1, count=1, settings={"automation_steps": [{"order": 2, "kind": "music"}]})
        live_schema = {"id": str(uuid4()), "name": "Konuşma şeması", "sources": [],
                       "settings": self.schema_settings(automation_steps=[{"order": 1, "kind": "music"},
                                                                          {"order": 2, "kind": "talk", "transition": {"mode": 0, "overlap_ms": 0}}])}
        talk = {"id": str(uuid4()), "source_type": "live", "frequency": 1, "random": False, "track_random": False,
                "advert_random": False, "sweeper_random": False,
                "live": {"title": "Sabah sohbeti", "label": "talk", "channels": [3, 4], "automation_id": live_schema["id"],
                         "end_rule": "duration", "duration_ms": 180000}}
        settings = self.schema_settings(
            autodj_presets=[preset],
            automation_steps=[{"order": 1, "kind": "music", "preset_id": preset["id"]},
                              {"order": 2, "kind": "music", "transition": {"mode": 1, "overlap_ms": 2500}},
                              {"order": 3, "kind": "talk"}],
            link_transitions=[{"from_source_id": a["id"], "to_source_id": talk["id"], "preset_id": preset["id"]},
                              {"from_source_id": talk["id"], "to_source_id": c["id"], "transition": {"mode": 3, "overlap_ms": 4000}}],
            programme_entry={"source_order": 1, "transition": {"mode": 1, "overlap_ms": 3000},
                             "alternatives": [{"weight": 30, "source_order": 3, "preset_id": preset["id"]},
                                              {"weight": 20, "source_order": 2}]})
        definition = {"id": str(uuid4()), "name": "Sabah şeması", "sources": [a, talk, c], "settings": settings}
        event = self.event(rotations=[{"id": str(uuid4()), "rotation_id": definition["id"]}],
                           live={"label": "dj", "channels": [1], "automation_id": live_schema["id"], "end_rule": "manual", "duration_ms": 0})
        return {"schema": 1, "events": [event], "rotation_library": [definition, live_schema]}

    def test_schema_steps_links_entry_live_persist_cas_reopen_and_weekly(self):
        config = self.schema_config()
        config["rotation_library"][1]["settings"]["automation_steps"][1]["channels"] = [12, 32]
        original = copy.deepcopy(config)
        saved = self.put(config)
        self.assertEqual(config, original)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        actual = api.get_document(reopened, self.principal, self.sid, "automation_calendar", saved["_id"])
        self.assertEqual(actual["config"], original)
        stored = actual["config"]["rotation_library"][0]
        self.assertEqual(stored["sources"][1]["source_type"], "live")
        self.assertEqual(stored["sources"][1]["live"]["duration_ms"], 180000)
        self.assertEqual(stored["sources"][0]["start_date"], "2026-10-01")
        self.assertEqual(stored["settings"]["programme_entry"]["alternatives"][0]["weight"], 30)
        self.assertEqual(actual["config"]["rotation_library"][1]["settings"]["automation_steps"][1]["channels"], [12, 32])
        self.assertEqual(actual["config"]["events"][0]["live"]["end_rule"], "manual")
        weekly = {"schema": 1, "week_anchor": "2026-10-05", "week_count": 1, "repeat": True, "calendar": copy.deepcopy(config)}
        saved_weekly = api.put_document(self.store, self.principal, self.sid, "weekly_programme", "Haftalık", weekly, str(uuid4()))
        self.assertEqual(saved_weekly["config"], weekly)
        # Sparse edit under CAS: the live step turns manual and loses its schema.
        changed = copy.deepcopy(config)
        changed["rotation_library"][0]["sources"][1]["live"].update(end_rule="manual", duration_ms=0, automation_id="")
        del changed["rotation_library"][0]["settings"]["programme_entry"]["alternatives"]
        again = self.put(changed, document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(again["config"], changed)
        with self.assertRaises(Conflict):
            self.put(config, document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_schema_steps_links_entry_untrusted_values_are_atomic(self):
        base = self.schema_config()
        preset_id = base["rotation_library"][0]["settings"]["autodj_presets"][0]["id"]
        a_id, talk_id, c_id = [row["id"] for row in base["rotation_library"][0]["sources"]]

        def settings_case(**changes):
            broken = copy.deepcopy(base)
            broken["rotation_library"][0]["settings"].update(changes)
            return broken
        step = {"order": 5, "kind": "jingle"}
        bad_steps = [True, {}, [step] * 33, [dict(step, order=0)], [dict(step, order=100)], [dict(step, order=True)],
                     [dict(step, kind="news")], [dict(step, kind="")], [step, dict(step, kind="music")],
                     [dict(step, preset_id=str(uuid4()))], [dict(step, preset_id="x")],
                     [dict(step, preset_id=preset_id, transition={"mode": 0, "overlap_ms": 0})],
                     [dict(step, transition={"mode": 4, "overlap_ms": 0})], [dict(step, transition={"mode": 1, "overlap_ms": 30001})],
                     [dict(step, transition={"mode": 1})], [dict(step, transition={"mode": 1, "overlap_ms": 0, "effect": "none"})],
                     [dict(step, extra=1)], [{"kind": "music"}], [{"order": 1}],
                     [dict(step, kind="talk", channels=[0])], [dict(step, kind="talk", channels=[33])],
                     [dict(step, kind="talk", channels=[1, 1])], [dict(step, kind="talk", channels=[True])],
                     [dict(step, kind="talk", channels="1, 2")], [dict(step, channels=[1])]]
        for value in bad_steps:
            with self.subTest(automation_steps=value):
                self.rejected(settings_case(automation_steps=value))
        link = {"from_source_id": a_id, "to_source_id": c_id}
        bad_links = [True, {}, [link] * 51, [link, dict(link)], [dict(link, from_source_id=str(uuid4()))],
                     [dict(link, to_source_id="x")], [dict(link, preset_id=str(uuid4()))],
                     [dict(link, preset_id=preset_id, transition={"mode": 0, "overlap_ms": 0})],
                     [dict(link, transition={"mode": -1, "overlap_ms": 0})], [dict(link, extra=1)], [{"from_source_id": a_id}]]
        for value in bad_links:
            with self.subTest(link_transitions=value):
                self.rejected(settings_case(link_transitions=value))
        entry = {"source_order": 1}
        bad_entries = [True, [], {}, dict(entry, source_order=0), dict(entry, source_order=51), dict(entry, source_order="1"),
                       dict(entry, preset_id=str(uuid4())), dict(entry, preset_id=preset_id, transition={"mode": 0, "overlap_ms": 0}),
                       dict(entry, transition={"mode": 1, "overlap_ms": 30001}), dict(entry, extra=1),
                       dict(entry, alternatives=[{"weight": 60, "source_order": 2}, {"weight": 41, "source_order": 3}]),
                       dict(entry, alternatives=[{"weight": 1, "source_order": 2}] * 9),
                       dict(entry, alternatives=[{"weight": 101, "source_order": 2}]),
                       dict(entry, alternatives=[{"weight": 10, "source_order": 0}]),
                       dict(entry, alternatives=[{"weight": 10}]), dict(entry, alternatives=[{"source_order": 2}]),
                       dict(entry, alternatives=[{"weight": 10, "source_order": 2, "preset_id": str(uuid4())}]),
                       dict(entry, alternatives=[{"weight": 10, "source_order": 2, "extra": 1}])]
        for value in bad_entries:
            with self.subTest(programme_entry=value):
                self.rejected(settings_case(programme_entry=value))
        # Link defaults and the programme entry belong to the rotation, not to a playlist override.
        for field, value in (("link_transitions", [link]), ("programme_entry", entry), ("automation_steps", True),
                             ("automation_steps", [dict(step, preset_id=str(uuid4()))])):
            broken = copy.deepcopy(base)
            broken["rotation_library"][0]["sources"][0]["settings"] = {field: value}
            with self.subTest(override=field):
                self.rejected(broken)
        # Boundary values that must still be accepted.
        accepted = settings_case(automation_steps=[dict(step, order=99, transition={"mode": 3, "overlap_ms": 30000})] +
                                 [{"order": i, "kind": "dj"} for i in range(1, 32)],
                                 programme_entry=dict(entry, source_order=50, alternatives=[{"weight": 100, "source_order": 50}]))
        self.put(accepted)
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_live_source_date_window_and_event_live_untrusted_values_are_atomic(self):
        base = self.schema_config()
        schema_id = base["rotation_library"][1]["id"]

        def source_case(index, **changes):
            broken = copy.deepcopy(base)
            broken["rotation_library"][0]["sources"][index].update(changes)
            return broken

        def live_case(**changes):
            broken = copy.deepcopy(base)
            broken["rotation_library"][0]["sources"][1]["live"].update(changes)
            return broken
        talk = base["rotation_library"][0]["sources"][1]
        for config in (source_case(1, playlist_id=self.playlist["_id"]), source_case(1, track_rules=[]),
                       source_case(1, track_selection_mode="linear"), source_case(1, source_type="stream"),
                       source_case(1, source_type="playlist"), source_case(0, live=talk["live"]), source_case(0, source_type="live"),
                       source_case(0, start_date="2026-11-01"), source_case(0, start_date="01.10.2026"), source_case(0, end_date=20261031),
                       source_case(0, end_date=""), source_case(1, start_date="2026-10-02", end_date="2026-10-01"),
                       live_case(label="host"), live_case(label=""), live_case(channels=[0]), live_case(channels=[33]),
                       live_case(channels=[3, 3]), live_case(channels=list(range(1, 34))), live_case(channels="3"), live_case(channels=[True]),
                       live_case(automation_id="x"), live_case(automation_id=str(uuid4())),
                       live_case(end_rule="timer"), live_case(end_rule="schema"), live_case(duration_ms=0),
                       live_case(duration_ms=86400001), live_case(duration_ms=1.5), live_case(title="x" * 161),
                       live_case(extra=1)):
            with self.subTest(config=config["rotation_library"][0]["sources"]):
                self.rejected(config)
        for field in ("label", "channels", "automation_id", "end_rule", "duration_ms"):
            broken = copy.deepcopy(base)
            del broken["rotation_library"][0]["sources"][1]["live"][field]
            with self.subTest(missing=field):
                self.rejected(broken)
        broken = copy.deepcopy(base)
        del broken["rotation_library"][0]["sources"][1]["live"]
        self.rejected(broken)
        # A plain rotation (no automation_schema) is not a live event schema, for steps and for the event flag.
        plain = copy.deepcopy(base)
        plain["rotation_library"][1]["settings"]["automation_schema"] = False
        self.rejected(plain)
        del plain["rotation_library"][1]["settings"]["automation_schema"]
        self.rejected(plain)
        plain["rotation_library"][0]["sources"][1]["live"]["automation_id"] = ""
        self.rejected(plain)
        plain["events"][0]["live"]["automation_id"] = ""
        self.put(plain)
        # The event flag: no title, calendar end rules only, schema reference required.
        for live in ({"label": "dj", "channels": [1], "automation_id": schema_id, "end_rule": "schema", "duration_ms": 0},
                     {"label": "dj", "channels": [1], "automation_id": schema_id, "end_rule": "manual", "duration_ms": 0, "title": "x"},
                     {"label": "dj", "channels": [1], "automation_id": schema_id, "end_rule": "duration", "duration_ms": 0},
                     {"label": "music", "channels": [1, 1], "automation_id": "", "end_rule": "manual", "duration_ms": 0}, [], True):
            broken = copy.deepcopy(base)
            broken["events"][0]["live"] = live
            with self.subTest(event_live=live):
                self.rejected(broken)
        # The runtime entry order is never persisted; a legacy row knows no live/date fields.
        broken = copy.deepcopy(base)
        broken["events"][0]["schema_entry_source_order"] = 1
        self.rejected(broken)
        for field, value in (("source_type", "live"), ("start_date", "2026-10-01"), ("live", talk["live"])):
            legacy = self.config()
            legacy["events"][0]["rotations"][0][field] = value
            with self.subTest(legacy=field):
                self.rejected(legacy)
        # Accepted boundaries: empty channels, no schema, manual rule, open date window, an untitled live step.
        open_window = copy.deepcopy(base)
        del open_window["rotation_library"][0]["sources"][0]["end_date"]
        open_window["rotation_library"][0]["sources"][1]["live"] = {"label": "dj", "channels": [], "automation_id": "",
                                                                    "end_rule": "manual", "duration_ms": 0}
        open_window["events"][0]["live"] = {"label": "music", "channels": list(range(1, 33)), "automation_id": "",
                                            "end_rule": "duration", "duration_ms": 86400000}
        self.put(open_window)
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_programme_live_block_kind_and_member(self):
        def block(kind, **extra):
            return {"id": str(uuid4()), "kind": kind, "title": "Live event", "start_time": "", "note": "", **extra}
        live = {"label": "talk", "channels": [3, 4], "automation_id": str(uuid4()), "end_rule": "schema", "duration_ms": 0}
        music = block("music")
        event = {"id": str(uuid4()), "title": "Parça", "artist": "", "album": "", "kind": "music", "start_time": "", "duration_ms": 1000,
                 "version_label": "", "primary_file": "", "backup_file": "", "note": "", "block_id": music["id"]}
        config = {"title": "Canlı bloklu", "date": "2026-10-18", "note": "",
                  "blocks": [music, block("live", live=live), block("music", live={**live, "end_rule": "manual"})], "events": [event]}
        saved = api.put_document(self.store, self.principal, self.sid, "programme", "Canlı", config, str(uuid4()))
        self.assertEqual(saved["config"], config)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        actual = api.get_document(reopened, self.principal, self.sid, "programme", saved["_id"])
        self.assertEqual(actual["config"]["blocks"][1]["live"], live)
        for duration in ({**live, "end_rule": "duration", "duration_ms": 90000}, {**live, "automation_id": ""}):
            fine = copy.deepcopy(config)
            fine["blocks"][1]["live"] = duration
            api.put_document(self.store, self.principal, self.sid, "programme", "Canlı", fine, str(uuid4()))
        sequence = self.store.sequence
        for bad in ({**live, "label": "host"}, {**live, "channels": [0]}, {**live, "channels": [4, 4]}, {**live, "channels": [33]},
                    {**live, "automation_id": "x"}, {**live, "end_rule": "timer"}, {**live, "end_rule": "duration", "duration_ms": 0},
                    {**live, "duration_ms": 86400001}, {**live, "title": "Başlık"}, {**live, "extra": 1},
                    {key: value for key, value in live.items() if key != "end_rule"}, [], True, None):
            broken = copy.deepcopy(config)
            broken["blocks"][1]["live"] = bad
            with self.subTest(live=bad):
                with self.assertRaises(StoreError):
                    api.put_document(self.store, self.principal, self.sid, "programme", "Canlı", broken, str(uuid4()))
        broken = copy.deepcopy(config)
        broken["blocks"][1]["kind"] = "talk"
        with self.assertRaises(StoreError):
            api.put_document(self.store, self.principal, self.sid, "programme", "Canlı", broken, str(uuid4()))
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

if __name__ == "__main__":
    unittest.main(verbosity=2)
