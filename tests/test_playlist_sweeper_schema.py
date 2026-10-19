#!/usr/bin/env python3
"""Private real DB/HTTP acceptance for the Sweeper playlist purpose extension."""
from __future__ import annotations

from http.client import HTTPConnection
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4

import test_desktop_api as fixtures
from sdbase import desktop_playlist
from sdbase.api_server import BoundedHTTPServer
from sdbase.http_server import make_handler
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build/automation-radyo-pdf-20261013/backend/fixtures"
LEGACY_PURPOSES = ("music", "jingle", "advert", "announcement", "programme", "other")


class PlaylistSweeperSchema(unittest.TestCase):
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

    def create(self, name="Sweeper kaynağı", key=None, **extra):
        return desktop_playlist.create_playlist(self.store, self.principal, self.sid, name,
            idempotency_key=key or str(uuid4()), **extra)

    def listed(self, store=None):
        return desktop_playlist.list_playlists(store or self.store, self.principal, self.sid)["playlists"]

    def test_existing_six_purposes_remain_exact_and_durable(self):
        saved = [self.create("Eski tür " + purpose, purpose=purpose) for purpose in LEGACY_PURPOSES]
        reopened = open_radio_store(self.fixture.db, read_only=True)
        records = {row["_id"]: row for row in self.listed(reopened)}
        for row, purpose in zip(saved, LEGACY_PURPOSES):
            self.assertEqual(row["purpose"], purpose)
            self.assertEqual(records[row["_id"]]["purpose"], purpose)
        self.assertEqual(saved[2]["purpose"], "advert")

    def test_sweeper_create_list_reopen_and_integrity(self):
        saved = self.create(purpose="sweeper")
        self.assertEqual(saved["purpose"], "sweeper")
        self.assertEqual(saved["tracks"], [])
        self.assertEqual(saved["media_state"], "unverified")
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(self.listed(reopened), [saved])
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        self.assertEqual(list(reopened.scan("plans")), [])

    def test_sweeper_replay_does_not_write_or_change_purpose(self):
        key = str(uuid4())
        saved = self.create(key=key, purpose="sweeper")
        sequence = self.store.sequence
        self.assertEqual(self.create(key=key, purpose="sweeper"), saved)
        self.assertEqual(self.store.sequence, sequence)
        with self.assertRaises(Conflict):
            self.create(key=key, purpose="advert")
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.listed(), [saved])

    def test_omitted_and_explicit_music_keep_legacy_request_identity(self):
        key = str(uuid4())
        saved = self.create("Eski müzik", key=key)
        sequence = self.store.sequence
        self.assertEqual(self.create("Eski müzik", key=key, purpose="music"), saved)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(saved["purpose"], "music")

    def test_sweeper_rename_cas_and_invalid_purposes_are_atomic(self):
        saved = self.create(purpose="sweeper")
        changed = desktop_playlist.rename_playlist(self.store, self.principal, self.sid, saved["_id"],
            "Kalıcı Sweeper", expected_revision=saved["_rev"], idempotency_key=str(uuid4()))
        self.assertEqual(changed["purpose"], "sweeper")
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            desktop_playlist.rename_playlist(self.store, self.principal, self.sid, saved["_id"],
                "Eski revizyon", expected_revision=saved["_rev"], idempotency_key=str(uuid4()))
        for purpose in (None, True, 1, 1.0, [], {}, "", "Sweeper", "unsupported", "advertisement"):
            with self.subTest(purpose=purpose), self.assertRaises(StoreError):
                self.create("Geçersiz tür", purpose=purpose)
            self.assertEqual(self.store.sequence, sequence)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(self.listed(reopened), [changed])

    def test_authenticated_http_sweeper_creation_and_legacy_advert_readback(self):
        base = make_handler(self.store)
        class Handler(base):
            def log_message(self, *_args):
                pass
        server = BoundedHTTPServer(("127.0.0.1", 0), Handler, request_timeout=2)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        connection = HTTPConnection("127.0.0.1", server.server_port, timeout=3)
        try:
            headers = {"Content-Type": "application/json",
                       "Authorization": "Bearer " + self.fixture.identity["api_token"]}
            def post(operation, value):
                connection.request("POST", "/v1/workflows/desktop-playlists/" + operation, json.dumps(value), headers)
                response = connection.getresponse()
                return response.status, json.loads(response.read())
            saved = []
            for purpose in ("sweeper", "advert"):
                status, row = post("create", {"station_id": self.sid, "name": "HTTP " + purpose,
                    "purpose": purpose, "idempotency_key": str(uuid4())})
                self.assertEqual(status, 200)
                self.assertEqual(row["purpose"], purpose)
                saved.append(row)
            status, body = post("list", {"station_id": self.sid})
            self.assertEqual(status, 200)
            self.assertEqual({row["_id"]: row["purpose"] for row in body["playlists"]},
                             {row["_id"]: row["purpose"] for row in saved})
            sequence = self.store.sequence
            status, body = post("create", {"station_id": self.sid, "name": "HTTP invalid",
                "purpose": "invalid", "idempotency_key": str(uuid4())})
            self.assertEqual(status, 422)
            self.assertEqual(body["error"], "invalid_request")
            self.assertEqual(self.store.sequence, sequence)
            self.assertEqual({row["_id"]: row["purpose"] for row in self.listed(open_radio_store(self.fixture.db, read_only=True))},
                             {row["_id"]: row["purpose"] for row in saved})
        finally:
            connection.close()
            server.shutdown()
            server.server_close()
            worker.join(timeout=3)


if __name__ == "__main__":
    unittest.main()
