#!/usr/bin/env python3
"""Real isolated DB acceptance of saved routing topology, without runtime gates."""
import copy
import unittest
from uuid import uuid4

import test_desktop_api as fixtures
from test_desktop_routing import configuration
from sdbase import desktop_api as api
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import StoreError


def topology():
    kinds = ("application", "microphone", "compressor", "output", "main", "recorder")
    return {"nodes": [{"id": i + 1, "kind": kind, "x": i * 120.25, "y": i * 65.5}
                      for i, kind in enumerate(kinds)],
            "edges": [{"from": 1, "to": 4}, {"from": 1, "to": 6}, {"from": 2, "to": 3}]}


class RoutingGraphSchema(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal

    def put(self, config, **revision):
        return api.put_document(self.store, self.principal, self.sid, "audio_routing", "Ses yönlendirme",
                                config, str(uuid4()), **revision)

    def test_legacy_optional_roundtrip_and_no_runtime(self):
        old = self.put(configuration())
        self.assertNotIn("graph", old["config"])
        graph = topology()
        graph["edges"].append({"from": 1, "to": 5})  # Planned MAIN and WAV, never an automatic start.
        saved = self.put(configuration(graph=graph), document_id=old["_id"], expected_revision=old["_rev"])
        self.assertEqual(saved["config"]["graph"], graph)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "audio_routing", saved["_id"]), saved)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        for table in ("playout_events", "output_observations", "recording_attempts"):
            self.assertFalse(self.store.scan(table))
        self.assertNotIn("running", saved["config"])
        self.assertNotIn("application_index", saved["config"])

    def test_invalid_graph_update_is_atomic(self):
        saved = self.put(configuration(graph=topology()))
        revision = {"document_id": saved["_id"], "expected_revision": saved["_rev"]}
        sequence = self.store.sequence
        graph = topology()
        graph["nodes"].pop()  # Existing recorder edge now points to a missing node.
        with self.assertRaises(StoreError):
            self.put(configuration(graph=graph), **revision)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(api.get_document(self.store, self.principal, self.sid, "audio_routing", saved["_id"]), saved)

    def test_strict_ports_limits_identity_and_duplicate_microphone(self):
        bad = []
        for field, value in (("x", -1), ("x", 1400.01), ("y", 700.01), ("x", float("nan")),
                             ("x", True), ("id", True), ("id", 1.0), ("kind", "output")):
            graph = topology()
            graph["nodes"][0][field] = value
            bad.append(graph)
        graph = topology(); graph["nodes"].append(copy.deepcopy(graph["nodes"][0])); bad.append(graph)
        for edge in ({"from": 1, "to": 4}, {"from": 4, "to": 1}, {"from": 2, "to": 5},
                     {"from": 6, "to": 1}, {"from": True, "to": 6}, {"from": 1, "to": 6, "port": "fake"}):
            graph = topology(); graph["edges"].append(edge); bad.append(graph)
        graph = topology(); graph["edges"].extend([{"from": 2, "to": 6}, {"from": 3, "to": 6}]); bad.append(graph)
        for field, value in (("nodes", None), ("edges", "fake"), ("running", True)):
            graph = topology(); graph[field] = value; bad.append(graph)
        sequence = self.store.sequence
        for graph in bad:
            with self.subTest(graph=graph), self.assertRaises(StoreError):
                self.put(configuration(graph=graph))
            self.assertEqual(self.store.sequence, sequence)
        self.assertFalse(self.store.scan("desktop_configurations"))


    def test_v2_named_patchbay_roundtrip_and_no_runtime(self):
        graph = {"nodes": [
            {"id": 17, "kind": "microphone", "name": "Deneme mikrofonu", "x": 40,
             "y": 50, "gain_db": 0, "settings": {"device": "test_mic", "push_to_talk": False}},
            {"id": 25, "kind": "recorder", "name": "Ana kayit", "x": 300,
             "y": 50, "gain_db": -3, "settings": {"folder": "/tmp/radios", "prefix": "ana", "split_minutes": 30}},
            {"id": 26, "kind": "recorder", "name": "Yedek kayit", "x": 300,
             "y": 200, "gain_db": 0, "settings": {"folder": "", "prefix": "yedek", "split_minutes": 0}}],
            "edges": [{"from": 17, "to": 25}, {"from": 17, "to": 26}]}
        config = {"schema_version": 2, "active": "Canli", "configs": [{"name": "Canli", "graph": graph}]}
        saved = self.put(config)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "audio_routing", saved["_id"])["config"], config)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        for table in ("playout_events", "output_observations", "recording_attempts"):
            self.assertFalse(self.store.scan(table))
        sequence = self.store.sequence
        bad = copy.deepcopy(config)
        bad["configs"][0]["graph"]["nodes"][1]["settings"]["password"] = "PrivateSecret"
        with self.assertRaises(StoreError):
            self.put(bad, document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(self.store.sequence, sequence)
        bad = copy.deepcopy(config); bad["active"] = "Kayip"
        with self.assertRaises(StoreError):
            self.put(bad)

    def test_v2_stream_password_only_with_saved_flag(self):
        def stream(settings):
            node = {"id": 7, "kind": "stream", "name": "Yayın", "x": 0, "y": 0, "gain_db": 0, "settings": {
                "protocol": "icecast", "host": "yayin.example", "port": 8000, "mount": "/canli", "stream_id": 1,
                "username": "source", "bitrate_kbps": 128, "public": False, "station": "", "genre": "",
                "description": "", "website": "", **settings}}
            return {"schema_version": 2, "active": "Canli", "configs": [{"name": "Canli", "graph": {"nodes": [node], "edges": []}}]}
        saved = self.put(stream({"password_saved": True, "password": "gizli-şifre"}))
        reopened = open_radio_store(self.fixture.db, read_only=True)
        config = api.get_document(reopened, self.principal, self.sid, "audio_routing", saved["_id"])["config"]
        self.assertEqual(config["configs"][0]["graph"]["nodes"][0]["settings"]["password"], "gizli-şifre")
        self.assertTrue(config["configs"][0]["graph"]["nodes"][0]["settings"]["password_saved"])
        plain = self.put(stream({}))
        self.assertNotIn("password", plain["config"]["configs"][0]["graph"]["nodes"][0]["settings"])
        sequence = self.store.sequence
        for bad in ({"password": "gizli"}, {"password_saved": True}, {"password_saved": False, "password": "gizli"},
                    {"password_saved": "yes", "password": "gizli"}, {"password_saved": True, "password": 5},
                    {"password_saved": True, "password": "a\nb"}, {"password_saved": True, "password": "ş" * 513}):
            with self.subTest(bad=bad), self.assertRaises(StoreError):
                self.put(stream(bad))
            self.assertEqual(self.store.sequence, sequence)

    def test_v2_monitor_feedback_is_rejected_atomically(self):
        graph = {"nodes": [
            {"id": 101, "kind": "input", "name": "", "x": 0, "y": 0, "gain_db": 0,
             "settings": {"device": "test_sink.monitor"}},
            {"id": 102, "kind": "output", "name": "", "x": 200, "y": 0, "gain_db": 0,
             "settings": {"device": "test_sink"}}], "edges": [{"from": 101, "to": 102}]}
        before = self.store.sequence
        with self.assertRaises(StoreError):
            self.put({"schema_version": 2, "active": "Test", "configs": [{"name": "Test", "graph": graph}]})
        self.assertEqual(self.store.sequence, before)


if __name__ == "__main__":
    unittest.main()
