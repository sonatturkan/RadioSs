#!/usr/bin/env python3
"""Private real DB/HTTP acceptance of stream song-information source settings."""
from __future__ import annotations

import copy
from http.client import HTTPConnection
import itertools
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4

import test_desktop_api as fixtures
from sdbase import desktop_api as api
from sdbase.api_server import BoundedHTTPServer
from sdbase.http_server import make_handler
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build/stream-metadata-source-20261013/backend/fixtures"
OPTIONAL = {"metadata_source": "player", "metadata_player": 31,
            "metadata_format": "{artist} - {title}", "metadata_manual": "Sanatçı · Parça"}


def configuration(**metadata):
    settings = {"protocol": "icecast", "host": "127.0.0.1", "port": 8000,
                "mount": "/canli", "stream_id": 1, "username": "source",
                "bitrate_kbps": 128, "public": False, "station": "RadioS",
                "genre": "", "description": "", "website": "", **metadata}
    graph = {"nodes": [{"id": 42, "kind": "stream", "name": "Canlı yayın", "x": 120,
                         "y": 180, "gain_db": 0, "settings": settings}], "edges": []}
    return {"schema_version": 2, "active": "Canlı", "configs": [{"name": "Canlı", "graph": graph}]}


def settings(config):
    return config["configs"][0]["graph"]["nodes"][0]["settings"]


class StreamMetadataSchema(unittest.TestCase):
    def setUp(self):
        OUTPUT.mkdir(parents=True, exist_ok=True)
        original = tempfile.TemporaryDirectory
        def isolated(*args, **kwargs):
            kwargs["dir"] = OUTPUT
            return original(*args, **kwargs)
        self.fixture = fixtures.DesktopAPI("runTest")
        with patch.object(fixtures.tempfile, "TemporaryDirectory", side_effect=isolated):
            self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal

    def put(self, config, key=None, **revision):
        return api.put_document(self.store, self.principal, self.sid, "audio_routing", "Ses yönlendirme",
                                config, key or str(uuid4()), **revision)

    def reopened(self, saved):
        reopened = open_radio_store(self.fixture.db, read_only=True)
        return api.get_document(reopened, self.principal, self.sid, "audio_routing", saved["_id"])

    def test_legacy_stream_settings_reopen_without_metadata(self):
        config = configuration()
        self.assertEqual(len(settings(config)), 12)
        saved = self.put(config)
        self.assertEqual(self.reopened(saved)["config"], config)
        self.assertFalse(set(OPTIONAL) & set(settings(saved["config"])))
        self.assertEqual(inspect_integrity(open_radio_store(self.fixture.db, read_only=True))["status"], "ok")

    def test_all_optional_subsets_persist_without_requiring_other_metadata(self):
        saved = self.put(configuration())
        names = list(OPTIONAL)
        for count in range(5):
            for subset in itertools.combinations(names, count):
                config = configuration(**{key: OPTIONAL[key] for key in subset})
                with self.subTest(fields=subset):
                    saved = self.put(config, document_id=saved["_id"], expected_revision=saved["_rev"])
                    self.assertEqual(self.reopened(saved)["config"], config)

    def test_manual_player_automation_and_utf8_limits_roundtrip_without_execution(self):
        saved = self.put(configuration())
        # UTF-8 bytes, not character count: these hit the exact 255/1024 limits.
        format_text, manual_text = "ş" * 127 + "x", "ğ" * 512
        self.assertEqual(len(format_text.encode()), 255)
        self.assertEqual(len(manual_text.encode()), 1024)
        for source, player in (("manual", 0), ("player", 31), ("automation", 0)):
            config = configuration(metadata_source=source, metadata_player=player,
                                   metadata_format=format_text, metadata_manual=manual_text)
            saved = self.put(config, document_id=saved["_id"], expected_revision=saved["_rev"])
            self.assertEqual(settings(self.reopened(saved)["config"]), settings(config))
        for collection in ("playout_events", "output_observations", "recording_attempts"):
            self.assertFalse(self.store.scan(collection))
        self.assertFalse(any(row["kind"] == "broadcast_local_run" for row in self.store.scan("desktop_configurations")))

    def test_metadata_update_exact_replay_and_stale_revision_rejection(self):
        saved = self.put(configuration())
        config = configuration(**OPTIONAL)
        key = str(uuid4())
        revision = {"document_id": saved["_id"], "expected_revision": saved["_rev"]}
        updated = self.put(config, key, **revision)
        sequence = self.store.sequence
        self.assertEqual(self.put(config, key, **revision), updated)
        self.assertEqual(self.store.sequence, sequence)
        with self.assertRaises(Conflict):
            self.put(configuration(metadata_source="automation"), **revision)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.reopened(updated), updated)

    def test_invalid_metadata_types_limits_controls_and_unknown_fields_are_atomic(self):
        saved = self.put(configuration(**OPTIONAL))
        sequence = self.store.sequence
        bad = [("metadata_source", value) for value in ("", "main", "url", True, 1, None, [], {})]
        bad += [("metadata_player", value) for value in (-1, 32, True, 1.0, "1", None)]
        bad += [("metadata_format", value) for value in ("ş" * 128, "x" * 256, "x\n", "x\0", "x\x7f", 12, None)]
        bad += [("metadata_manual", value) for value in ("ğ" * 513, "x" * 1025, "x\r", "x\t", False, None)]
        bad += [("metadata_password", "PrivateFixtureOnly"), ("password", "PrivateFixtureOnly")]
        for field, value in bad:
            config = configuration(**OPTIONAL)
            settings(config)[field] = value
            with self.subTest(field=field, value_type=type(value).__name__):
                with self.assertRaises(StoreError):
                    self.put(config, document_id=saved["_id"], expected_revision=saved["_rev"])
                self.assertEqual(self.store.sequence, sequence)
                self.assertEqual(self.reopened(saved), saved)

    def test_metadata_is_stream_only_and_old_required_fields_remain_required(self):
        saved = self.put(configuration())
        bad = configuration(metadata_source="manual")
        del settings(bad)["host"]
        other = configuration()
        node = other["configs"][0]["graph"]["nodes"][0]
        node["kind"], node["settings"] = "main", {"metadata_source": "automation"}
        for config in (bad, other):
            with self.assertRaises(StoreError):
                self.put(config)
        self.assertEqual(self.reopened(saved), saved)

    def test_authenticated_http_source_save_and_rejection_survive_reopen(self):
        base = make_handler(self.store)
        class Handler(base):
            def log_message(self, *_args):
                pass
        server = BoundedHTTPServer(("127.0.0.1", 0), Handler, request_timeout=2)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        connection = HTTPConnection("127.0.0.1", server.server_port, timeout=3)
        try:
            body = {"station_id": self.sid, "kind": "audio_routing", "name": "Metadata HTTP",
                    "config": configuration(**OPTIONAL), "idempotency_key": str(uuid4())}
            def post(value):
                connection.request("POST", "/v1/workflows/desktop/put", json.dumps(value),
                                   {"Content-Type": "application/json",
                                    "Authorization": "Bearer " + self.fixture.identity["api_token"]})
                response = connection.getresponse()
                return response.status, json.loads(response.read())
            status, saved = post(body)
            self.assertEqual(status, 200)
            self.assertEqual(self.reopened(saved), saved)
            before = self.store.sequence
            bad = copy.deepcopy(body)
            bad.update(document_id=saved["_id"], expected_revision=saved["_rev"], idempotency_key=str(uuid4()))
            settings(bad["config"])["metadata_player"] = 32
            status, response = post(bad)
            self.assertEqual(status, 422)
            self.assertEqual(response["error"], "invalid_request")
            self.assertEqual(self.store.sequence, before)
            self.assertEqual(self.reopened(saved), saved)
        finally:
            connection.close()
            server.shutdown()
            server.server_close()
            worker.join(timeout=3)


if __name__ == "__main__":
    unittest.main()
