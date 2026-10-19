#!/usr/bin/env python3
"""Atomic selected-catalog playlist drafts, binding checks and replay contracts."""

import copy
import sys
import unittest
from unittest.mock import patch
from uuid import uuid4

sys.dont_write_bytecode = True
import test_desktop_api as fixtures
from sdbase import desktop_playlist as api
from sdbase.access_admin import create_station_user
from sdbase.identity import Forbidden, authenticate
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError
from sdbase.workflow_api import dispatch_workflow


class PlaylistSelection(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store = self.fixture.store
        self.sid = self.fixture.sid
        self.principal = self.fixture.principal

    @staticmethod
    def metadata(title="Seçili parça", binding=None, **changes):
        return {"title": title, "artist": "Sanatçı", "album": "Albüm", "duration_ms": 200,
                "bpm": 0, "genre": "", "key": "", "location": "", "comment": "",
                **(binding or {}), **changes}

    def create(self, entries=None, name="Seçilenler", key=None, principal=None, station=None):
        return api.create_playlist(self.store, principal or self.principal, station or self.sid,
                                   name, idempotency_key=key or str(uuid4()), entries=entries)

    def draft_state(self):
        return {collection: self.store.scan(collection) for collection in
                ("playlists", "playlist_versions", api.COLLECTION, "desktop_mutations")}

    def test_order_verified_bindings_reopen_and_draft_only(self):
        binding = self.fixture.verified_binding()
        selected = [self.metadata("İkinci", binding), self.metadata("Birinci", binding)]
        original = copy.deepcopy(selected)
        playlist = self.create(selected)
        self.assertEqual(selected, original)
        self.assertEqual([row["title"] for row in playlist["tracks"]], ["İkinci", "Birinci"])
        self.assertEqual([row["ordinal"] for row in playlist["tracks"]], [0, 1])
        self.assertEqual([row["active_ordinal"] for row in playlist["tracks"]], [0, 1])
        self.assertEqual(len({row["_id"] for row in playlist["tracks"]}), 2)
        for row in playlist["tracks"]:
            self.assertEqual({key: row[key] for key in binding}, binding)
            self.assertEqual(row["media_state"], "unverified")
            self.assertEqual(row["playlist_version_id"], playlist["version_id"])
        self.assertIsNone(playlist["current_version_id"])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.list_playlists(reopened, self.principal, self.sid)["playlists"], [playlist])
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        self.assertEqual(reopened.get("playlist_versions", playlist["version_id"])["status"], "draft")
        self.assertFalse(reopened.scan("playout_events"))
        self.assertFalse(reopened.scan("plan_revisions"))

    def test_unbound_online_reference_is_metadata_only(self):
        playlist = self.create([self.metadata("Çevrimiçi", location="youtube_music:abcdefghijk", duration_ms=None)])
        row, = playlist["tracks"]
        self.assertEqual(row["location"], "youtube_music:abcdefghijk")
        self.assertIsNone(row["duration_ms"])
        self.assertEqual(row["media_state"], "unverified")
        self.assertNotIn("media_id", row)
        self.assertFalse(self.store.scan("items"))
        self.assertFalse(self.store.scan("versions"))
        self.assertFalse(self.store.scan("media_files"))

    def test_replay_and_changed_selection_or_order_conflict(self):
        selected = [self.metadata("A"), self.metadata("B")]
        key = str(uuid4())
        playlist = self.create(selected, key=key)
        sequence = self.store.sequence
        self.assertEqual(self.create(copy.deepcopy(selected), key=key), playlist)
        self.assertEqual(self.store.sequence, sequence)
        for changed in (list(reversed(selected)), [self.metadata("A")],
                        [self.metadata("A", artist="Başka"), self.metadata("B")]):
            with self.assertRaises(Conflict):
                self.create(changed, key=key)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(len(self.store.scan("playlists")), 1)

    def test_empty_create_keeps_legacy_request_identity(self):
        key = str(uuid4())
        playlist = api.create_playlist(self.store, self.principal, self.sid,
                                       "Eski boş liste", idempotency_key=key)
        sequence = self.store.sequence
        self.assertEqual(self.create(name="Eski boş liste", key=key), playlist)
        self.assertEqual(playlist["tracks"], [])
        self.assertEqual(self.store.sequence, sequence)
        with self.assertRaises(Conflict):
            self.create([self.metadata()], name="Eski boş liste", key=key)

    def test_invalid_selection_is_atomic_and_leaves_no_receipt(self):
        before = self.draft_state()
        sequence = self.store.sequence
        invalid = [[], "bad", {}, (self.metadata(),), [None], [self.metadata()] * 501,
                   [self.metadata(), {"title": "Incomplete"}],
                   [self.metadata(), self.metadata(duration_ms=0)],
                   [self.metadata(), self.metadata(title="   ")],
                   [self.metadata(), self.metadata(bpm=float("nan"))],
                   [self.metadata(), self.metadata(unsupported=True)]]
        for selection in invalid:
            with self.subTest(selection_type=type(selection).__name__, count=len(selection)):
                with self.assertRaises(StoreError):
                    self.create(selection)
                self.assertEqual(self.draft_state(), before)
                self.assertEqual(self.store.sequence, sequence)

    def test_permission_and_cross_station_bindings(self):
        viewer = create_station_user(self.store, self.principal, self.sid,
                                     "selection-viewer", "Viewer", ["view"])
        view_principal = authenticate(self.store, viewer["token"])
        before = self.draft_state()
        with self.assertRaises(Forbidden):
            self.create([self.metadata()], principal=view_principal)
        self.assertEqual(self.draft_state(), before)
        binding = self.fixture.verified_binding()
        other = str(uuid4())
        self.store.put("stations", other,
                       {"code": "SELECT-OTHER", "name": "Other", "timezone": "UTC", "status": "active"})
        before = self.draft_state()
        sequence = self.store.sequence
        with self.assertRaises(StoreError):
            self.create([self.metadata(), self.metadata(binding=binding)], station=other)
        self.assertEqual(self.draft_state(), before)
        self.assertEqual(self.store.sequence, sequence)

    def test_partial_mismatched_and_stale_bindings_roll_back_entire_selection(self):
        binding = self.fixture.verified_binding()
        bad_bindings = [{"media_id": binding["media_id"]},
                        dict(binding, version_id=str(uuid4())),
                        dict(binding, media_file_id=str(uuid4())),
                        dict(binding, media_sha256="F" * 64)]
        before = self.draft_state()
        sequence = self.store.sequence
        for bad in bad_bindings:
            with self.assertRaises(StoreError):
                self.create([self.metadata("Valid"), self.metadata("Invalid", bad)])
            self.assertEqual(self.draft_state(), before)
            self.assertEqual(self.store.sequence, sequence)
        file = self.store.get("media_files", binding["media_file_id"])
        self.store.put("media_files", file["_id"],
                       {key: value for key, value in file.items() if not key.startswith("_")} | {"status": "error"},
                       expected_revision=file["_rev"])
        before = self.draft_state()
        sequence = self.store.sequence
        with self.assertRaises(StoreError):
            self.create([self.metadata(), self.metadata(binding=binding)])
        self.assertEqual(self.draft_state(), before)
        self.assertEqual(self.store.sequence, sequence)

    def test_confirmed_replay_survives_later_loss_of_media_verification(self):
        binding = self.fixture.verified_binding()
        selected = [self.metadata(binding=binding)]
        key = str(uuid4())
        playlist = self.create(selected, key=key)
        file = self.store.get("media_files", binding["media_file_id"])
        self.store.put("media_files", file["_id"],
                       {name: value for name, value in file.items() if not name.startswith("_")} | {"status": "error"},
                       expected_revision=file["_rev"])
        sequence = self.store.sequence
        self.assertEqual(self.create(selected, key=key), playlist)
        self.assertEqual(self.store.sequence, sequence)

    def test_retired_online_sources_are_rejected_without_writes(self):
        binding = self.fixture.verified_binding()
        online = self.metadata(provider="youtube", external_id="dQw4w9WgXcQ",
                               url="https://music.youtube.com/watch?v=dQw4w9WgXcQ")
        before = self.draft_state()
        sequence = self.store.sequence
        for row in (online, dict(online, online_id=str(uuid4())), dict(online, analysis_media=binding),
                    dict(self.metadata(), analysis_media=binding), dict(online, **binding)):
            with self.subTest(row=row), self.assertRaisesRegex(StoreError, "no longer supported"):
                self.create([self.metadata(), row])
            self.assertEqual(self.draft_state(), before)
            self.assertEqual(self.store.sequence, sequence)
        with self.assertRaisesRegex(StoreError, "unsupported configuration kind"):
            self.fixture.put("external_playlist", {"schema_version": 1, "provider": "youtube", "items": []})
        self.assertEqual(self.store.sequence, sequence)
        self.assertFalse(self.store.scan(api.COLLECTION))

    def test_precommit_failure_does_not_publish_partial_list(self):
        before = self.draft_state()
        sequence = self.store.sequence
        key = str(uuid4())
        with patch.object(api, "_receipt", side_effect=StoreError("injected receipt failure")):
            with self.assertRaises(StoreError):
                self.create([self.metadata("A"), self.metadata("B")], key=key)
        self.assertEqual(self.draft_state(), before)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(len(self.create([self.metadata("A"), self.metadata("B")], key=key)["tracks"]), 2)

    def test_database_change_before_commit_rejects_whole_batch(self):
        original_receipt = api._receipt

        def concurrent_change(*args):
            station = self.store.get("stations", self.sid)
            self.store.put("stations", self.sid,
                           {key: value for key, value in station.items() if not key.startswith("_")} | {"name": "Changed"},
                           expected_revision=station["_rev"])
            return original_receipt(*args)

        before = self.draft_state()
        with patch.object(api, "_receipt", side_effect=concurrent_change):
            with self.assertRaises(Conflict):
                self.create([self.metadata("A"), self.metadata("B")])
        self.assertEqual(self.draft_state(), before)

    def test_500_row_boundary_and_workflow_dispatch(self):
        entries = [self.metadata(f"Track {index}") for index in range(500)]
        request = {"station_id": self.sid, "name": "Five hundred", "idempotency_key": str(uuid4()), "entries": entries}
        playlist = dispatch_workflow(self.store, self.principal, "POST",
                                     "/v1/workflows/desktop-playlists/create", lambda: request)
        self.assertEqual(len(playlist["tracks"]), 500)
        self.assertEqual([row["ordinal"] for row in playlist["tracks"]], list(range(500)))
        self.assertEqual([row["title"] for row in playlist["tracks"]], [row["title"] for row in entries])
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")


if __name__ == "__main__":
    unittest.main()
