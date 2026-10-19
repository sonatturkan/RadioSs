#!/usr/bin/env python3
"""Player groups persist independently of mixer topology and runtime state."""
import copy
import unittest
from uuid import uuid4

import test_desktop_api as fixtures
from sdbase import desktop_api as api
from sdbase.desktop_player_topology import group_players, route_pools
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError


def configuration():
    cluster = str(uuid4())
    return {"schema_version": 1, "deck_count": 3, "slots": [0, 2, 9],
            "names": {"1": "Müzik A", "3": "Müzik B", "10": "Bağımsız"},
            "clusters": [{"id": cluster, "name": "Müzik", "automation_id": ""}],
            "players": {"1": {"cluster_id": cluster, "automation_id": "", "autodj_mode": 2, "autodj_target": 3},
                        "3": {"cluster_id": cluster, "automation_id": "", "autodj_mode": 2, "autodj_target": 0},
                        "10": {"cluster_id": "", "automation_id": "", "autodj_mode": 1, "autodj_target": 0}}}


class DesktopPlayerTopology(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal

    def put(self, config, **options):
        return api.put_document(self.store, self.principal, self.sid, "live_players", "Canlı oynatıcılar",
                                config, str(uuid4()), **options)

    def programme(self, station=None):
        return api.put_document(self.store, self.principal, station or self.sid, "weekly_programme", "Haftalık şema",
            {"schema": 1, "week_anchor": "2026-10-05", "week_count": 1, "repeat": False,
             "calendar": {"schema": 1, "events": []}}, str(uuid4()))

    def test_roundtrip_cas_and_mixer_independence(self):
        mixer = self.fixture.put(config={"deck_count": 6, "names": {"0": "MASTER", "1": "Mikrofon"}})
        config = configuration()
        saved = self.put(config)
        changed = copy.deepcopy(config); changed["clusters"][0]["name"] = "Ana müzik"
        new = self.put(changed, document_id=saved["_id"], expected_revision=saved["_rev"])
        with self.assertRaises(Conflict):
            self.put(config, document_id=saved["_id"], expected_revision=saved["_rev"])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "live_players", new["_id"]), new)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "channel", mixer["_id"]), mixer)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        self.assertFalse(self.store.scan("playout_events"))
        self.assertFalse(self.store.scan("output_observations"))

    def test_strict_types_groups_and_unknown_fields_are_atomic(self):
        good = configuration(); saved = self.put(good)
        invalid = []
        for field, values in {"schema_version": [True, 2, 1.0], "deck_count": [0, 33, True, 3.0],
                              "slots": [[0, 0, 9], [0, 2], [0, True, 9], [0, 2, 32]],
                              "clusters": [None, [{}]], "players": [None, {"2": {}}],
                              "names": [{"01": "Bad"}, {"1": None}]}.items():
            invalid.extend(dict(good, **{field: value}) for value in values)
        for changes in ({"cluster_id": str(uuid4())}, {"autodj_mode": True},
                        {"autodj_mode": 1, "autodj_target": 3}, {"autodj_target": 1},
                        {"autodj_target": 10}, {"autodj_target": 2}, {"fake": True},
                        {"automation_id": "missing"}, {"automation_id": str(uuid4())}):
            bad = copy.deepcopy(good); bad["players"]["1"].update(changes); invalid.append(bad)
        bad = copy.deepcopy(good); bad["clusters"].append(copy.deepcopy(bad["clusters"][0])); invalid.append(bad)
        bad = copy.deepcopy(good); bad["clusters"][0]["name"] = "  "; invalid.append(bad)
        invalid.append(dict(good, playing=True))
        before = self.store.sequence
        for bad in invalid:
            with self.subTest(config=bad), self.assertRaises(StoreError):
                self.put(bad, document_id=saved["_id"], expected_revision=saved["_rev"])
            self.assertEqual(self.store.sequence, before)
        self.assertEqual(api.get_document(self.store, self.principal, self.sid, "live_players", saved["_id"]), saved)

    def test_per_player_cluster_programmes_and_station_fences(self):
        shared = self.programme(); individual = self.programme()
        config = configuration(); config["clusters"][0]["automation_id"] = shared["_id"]
        config["players"]["1"]["automation_id"] = individual["_id"]
        saved = self.put(config)
        self.assertEqual(saved["config"], config)
        bad = copy.deepcopy(config); bad["players"]["1"]["automation_id"] = saved["_id"]
        with self.assertRaises(StoreError): self.put(bad)
        other = str(uuid4())
        self.store.put("stations", other, {"code": "TOPOLOGY-OTHER", "name": "Other", "timezone": "UTC", "status": "active"})
        foreign = self.programme(other)
        bad["players"]["1"]["automation_id"] = foreign["_id"]
        with self.assertRaises(StoreError): self.put(bad)
        api.delete_document(self.store, self.principal, self.sid, "weekly_programme", shared["_id"], shared["_rev"], str(uuid4()))
        changed = copy.deepcopy(config); changed["names"]["10"] = "Diğer"
        updated = self.put(changed, document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(updated["config"], changed)
        with self.assertRaises(StoreError): self.put(changed)
        changed["players"]["10"]["automation_id"] = shared["_id"]
        with self.assertRaises(StoreError):
            self.put(changed, document_id=updated["_id"], expected_revision=updated["_rev"])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_group_pools_clamp_to_defined_players(self):
        config = configuration(); cluster = config["clusters"][0]["id"]
        self.assertEqual(group_players(config, cluster), [1, 3])
        self.assertEqual(group_players(config, ""), [10])
        pools = {"music": [1, 2, 3, 4], "advertisement": [3, 1], "jingle": [4, 2], "sweeper": [1]}
        routed, notice = route_pools(config, cluster, pools)
        self.assertEqual(routed, {"music": [1, 3], "advertisement": [3, 1], "jingle": [1, 3], "sweeper": [1]})
        self.assertEqual(notice, "Şemadaki Oynatıcı 2, Oynatıcı 4 bu kümede yok; kümenin oynatıcıları kullanılıyor.")
        self.assertEqual(route_pools(config, cluster, {"music": [3, 1]}), ({"music": [3, 1]}, None))
        routed, notice = route_pools(config, "", {"music": [1, 10]})
        self.assertEqual(routed, {"music": [10]})
        self.assertIn("Oynatıcı 1 Kümesizler içinde yok", notice)
        self.assertEqual(pools["music"], [1, 2, 3, 4])


    def test_optional_defaults_count_only(self):
        config = {"schema_version": 1, "deck_count": 6}
        self.assertEqual(self.put(config)["config"], config)
        # Mixer records never accept player-group fields.
        with self.assertRaises(StoreError): self.fixture.put(config={"deck_count": 6, "clusters": []})


if __name__ == "__main__":
    unittest.main()
