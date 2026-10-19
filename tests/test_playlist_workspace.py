#!/usr/bin/env python3
"""Playlist purpose and atomic upper-source to lower-target copy contracts."""

import copy
import unittest
from unittest.mock import patch
from uuid import uuid4

import test_desktop_api as fixtures
from sdbase import desktop_playlist as api
from sdbase.access_admin import create_station_user
from sdbase.identity import Forbidden, authenticate
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError
from sdbase.workflow_api import dispatch_workflow


class PlaylistWorkspace(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store = self.fixture.store
        self.sid = self.fixture.sid
        self.principal = self.fixture.principal

    @staticmethod
    def metadata(title="Parça", **changes):
        return {"title": title, "artist": "Sanatçı", "album": "Albüm", "duration_ms": 200,
                "bpm": 0, "genre": "", "key": "", "location": "", "comment": "", **changes}

    def create(self, name="Hedef", **options):
        return api.create_playlist(self.store, self.principal, self.sid, name,
                                   idempotency_key=options.pop("idempotency_key", str(uuid4())), **options)

    def append(self, playlist, entries, **options):
        return api.append_entries(self.store, self.principal, self.sid, playlist["_id"], entries,
                                  expected_revision=options.pop("expected_revision", playlist["_rev"]),
                                  idempotency_key=options.pop("idempotency_key", str(uuid4())), **options)

    def draft_state(self):
        return {name: self.store.scan(name) for name in
                ("playlists", "playlist_versions", api.COLLECTION, "desktop_mutations")}

    def test_purpose_persistence_default_compatibility_and_validation(self):
        for purpose in sorted(api.PLAYLIST_PURPOSES):
            with self.subTest(purpose=purpose):
                playlist = self.create(purpose, purpose=purpose)
                self.assertEqual(playlist["purpose"], purpose)
                self.assertEqual(self.store.get("playlists", playlist["_id"])["purpose"], purpose)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual({row["purpose"] for row in api.list_playlists(reopened, self.principal, self.sid)["playlists"]},
                         api.PLAYLIST_PURPOSES)
        key = str(uuid4())
        playlist = self.create("Eski varsayılan", idempotency_key=key)
        sequence = self.store.sequence
        self.assertEqual(self.create("Eski varsayılan", purpose="music", idempotency_key=key), playlist)
        with self.assertRaises(Conflict):
            self.create("Eski varsayılan", purpose="jingle", idempotency_key=key)
        self.assertEqual(self.store.sequence, sequence)
        before = self.draft_state()
        for purpose in (None, "", "advertisement", "unknown", [], {}, True):
            with self.subTest(invalid=purpose), self.assertRaises(StoreError):
                self.create(purpose=purpose)
        self.assertEqual(self.draft_state(), before)

    def test_legacy_playlist_snapshot_defaults_to_music(self):
        playlist = self.create()
        legacy = {key: value for key, value in playlist.items()
                  if key not in {"_id", "_rev", "purpose", "version_id", "version_revision", "tracks", "media_state"}}
        self.store.put("playlists", playlist["_id"], legacy, expected_revision=playlist["_rev"])
        self.assertNotIn("purpose", self.store.get("playlists", playlist["_id"]))
        self.assertEqual(api.list_playlists(self.store, self.principal, self.sid)["playlists"][0]["purpose"], "music")

    def test_append_preserves_order_verified_local_and_metadata_identities(self):
        binding = self.fixture.verified_binding()
        entries = [self.metadata("Yerel", **binding), self.metadata("Metadata")]
        original = copy.deepcopy(entries)
        playlist = self.create(purpose="jingle", entries=[self.metadata("İlk")])
        result = self.append(playlist, entries)
        self.assertEqual(entries, original)
        self.assertEqual(result["purpose"], "jingle")
        self.assertEqual(result["_rev"], playlist["_rev"] + 1)
        self.assertEqual([row["title"] for row in result["tracks"]], ["İlk", "Yerel", "Metadata"])
        self.assertEqual([row["ordinal"] for row in result["tracks"]], list(range(3)))
        self.assertEqual({key: result["tracks"][1][key] for key in binding}, binding)
        self.assertFalse(set(binding).intersection(result["tracks"][2]))
        # Retired provider sources reject the whole batch; nothing is appended partially.
        online = self.metadata("Sağlayıcı", provider="youtube", external_id="dQw4w9WgXcQ",
                               url="https://music.youtube.com/watch?v=dQw4w9WgXcQ", analysis_media=binding)
        sequence = self.store.sequence
        with self.assertRaisesRegex(StoreError, "no longer supported"):
            self.append(result, [self.metadata("Sonraki"), online])
        self.assertEqual(self.store.sequence, sequence)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.list_playlists(reopened, self.principal, self.sid)["playlists"], [result])
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_append_receipt_replay_and_changed_order_conflict(self):
        playlist = self.create()
        entries = [self.metadata("A"), self.metadata("B")]
        key = str(uuid4())
        result = self.append(playlist, entries, idempotency_key=key)
        sequence = self.store.sequence
        self.assertEqual(self.append(playlist, copy.deepcopy(entries), idempotency_key=key), result)
        for changed in (list(reversed(entries)), [entries[0]], [self.metadata("C"), entries[1]]):
            with self.assertRaises(Conflict):
                self.append(playlist, changed, idempotency_key=key)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(len(result["tracks"]), 2)

    def test_append_invalid_batch_stale_revision_and_permission_are_atomic(self):
        binding = self.fixture.verified_binding()
        playlist = self.create(entries=[self.metadata("İlk")])
        before = self.draft_state()
        sequence = self.store.sequence
        for entries in ([], [self.metadata(), {"title": "Eksik"}], [self.metadata()] * 501,
                        [self.metadata(), self.metadata(**(binding | {"media_id": str(uuid4())}))],
                        [self.metadata(), self.metadata(**(binding | {"media_sha256": "0" * 64}))]):
            with self.subTest(count=len(entries)), self.assertRaises(StoreError):
                self.append(playlist, entries)
        for revision in (None, True, 0):
            with self.subTest(revision=revision), self.assertRaises(StoreError):
                self.append(playlist, [self.metadata()], expected_revision=revision)
        self.assertEqual(self.draft_state(), before)
        self.assertEqual(self.store.sequence, sequence)
        result = self.append(playlist, [self.metadata("Güncel")])
        before = self.draft_state()
        with self.assertRaises(Conflict):
            self.append(playlist, [self.metadata("Eski revizyon")])
        viewer = create_station_user(self.store, self.principal, self.sid, "workspace-viewer", "Viewer", ["view"])
        viewer_principal = authenticate(self.store, viewer["token"])
        with self.assertRaises(Forbidden):
            api.append_entries(self.store, viewer_principal, self.sid, result["_id"], [self.metadata()],
                               expected_revision=result["_rev"], idempotency_key=str(uuid4()))
        self.assertEqual(self.draft_state(), before)

    def test_append_precommit_failure_and_concurrent_change_leave_no_partial_batch(self):
        playlist = self.create()
        entries = [self.metadata("A"), self.metadata("B")]
        before = self.draft_state()
        key = str(uuid4())
        with patch.object(api, "_receipt", side_effect=StoreError("injected receipt failure")):
            with self.assertRaises(StoreError):
                self.append(playlist, entries, idempotency_key=key)
        self.assertEqual(self.draft_state(), before)
        original_receipt = api._receipt

        def concurrent_change(*args):
            station = self.store.get("stations", self.sid)
            self.store.put("stations", self.sid,
                           {key: value for key, value in station.items() if not key.startswith("_")} | {"name": "Changed"},
                           expected_revision=station["_rev"])
            return original_receipt(*args)

        with patch.object(api, "_receipt", side_effect=concurrent_change):
            with self.assertRaises(Conflict):
                self.append(playlist, entries, idempotency_key=key)
        self.assertEqual(self.draft_state(), before)
        self.assertEqual(len(self.append(playlist, entries, idempotency_key=key)["tracks"]), 2)

    def test_append_combined_500_row_boundary_and_workflow_dispatch(self):
        playlist = self.create(entries=[self.metadata(f"Parça {index}") for index in range(499)])
        request = {"station_id": self.sid, "playlist_id": playlist["_id"], "expected_revision": playlist["_rev"],
                   "idempotency_key": str(uuid4()), "entries": [self.metadata("Son")]}
        result = dispatch_workflow(self.store, self.principal, "POST",
                                   "/v1/workflows/desktop-playlists/entries-append", lambda: request)
        self.assertEqual(len(result["tracks"]), 500)
        self.assertEqual(result["tracks"][-1]["ordinal"], 499)
        before = self.draft_state()
        with self.assertRaises(StoreError):
            self.append(result, [self.metadata("Fazla")])
        self.assertEqual(self.draft_state(), before)
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")


if __name__ == "__main__":
    unittest.main()
