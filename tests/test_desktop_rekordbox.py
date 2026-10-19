#!/usr/bin/env python3
"""Private-store XML scope, policy, atomicity, CAS/replay and HTTP tests."""
from base64 import b64encode
from copy import deepcopy
from http.client import HTTPConnection
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "database/SDBASE/src"))
from sdbase import desktop_rekordbox as api, desktop_playlist, importer
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate, bootstrap
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError, Transaction

XML = b'''<?xml version="1.0" encoding="UTF-8"?>
<DJ_PLAYLISTS Version="1.0.0"><PRODUCT Name="rekordbox" Version="7" Company="AlphaTheta"/>
<COLLECTION Entries="3">
<TRACK TrackID="1" Name="Bir" Artist="Artist" Album="Album" TotalTime="123.456" AverageBpm="127.5" Location="file://localhost/not-accessible/one.wav"><POSITION_MARK Name="Hot cue" Type="0" Start="1.25" Num="0"/></TRACK>
<TRACK TrackID="2" Name="Iki" TotalTime="60" Location="file://localhost/not-accessible/two.wav"/>
<TRACK TrackID="3" Name="Koleksiyon" TotalTime="30" Location="file://localhost/not-accessible/three.wav"/>
</COLLECTION><PLAYLISTS><NODE Type="0" Name="ROOT" Count="2">
<NODE Type="1" Name="Selected" KeyType="0" Entries="3"><TRACK Key="1"/><TRACK Key="1"/><TRACK Key="2"/></NODE>
<NODE Type="1" Name="Excluded" KeyType="0" Entries="1"><TRACK Key="3"/></NODE>
</NODE></PLAYLISTS></DJ_PLAYLISTS>'''


class NativeRekordbox(unittest.TestCase):
    def setUp(self):
        (ROOT / "build").mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="desktop-rekordbox-", dir=ROOT / "build")
        self.db = Path(self.temp.name) / "test.sdb"
        self.store = open_radio_store(self.db)
        self.identity = bootstrap(self.store, "admin", "XML", "XML station")
        self.sid = self.identity["station_id"]
        self.principal = authenticate(self.store, self.identity["api_token"])
        self.encoded = b64encode(XML).decode()

    def tearDown(self):
        self.temp.cleanup()

    def options(self, **changes):
        result = {"library_key": "tested-library", "selected_node_paths": [[0, 0]],
                  "include_collection": False, "name_policy": "append",
                  "duplicate_policy": "skip", "cue_policy": "preserve_as_dj"}
        result.update(changes)
        return result

    def preview(self, **changes):
        return api.preview_import(self.store, self.principal, self.sid, self.encoded, **self.options(**changes))

    def apply(self, preview, key=None, **changes):
        return api.apply_import(self.store, self.principal, self.sid, self.encoded,
            preview["preview_hash"], key or str(uuid4()), **self.options(**changes))

    def listed(self):
        return desktop_playlist.list_playlists(self.store, self.principal, self.sid)["playlists"]

    def test_readonly_preview_selected_scope_and_atomic_source_cue_persistence(self):
        before = self.store.sequence
        preview = self.preview()
        self.assertEqual(self.store.sequence, before)
        self.assertEqual(preview["selected_tracks"], 2)
        self.assertEqual(preview["destinations"][0]["added_count"], 2)
        self.assertEqual(preview["destinations"][0]["skipped"], 1)
        self.assertEqual(preview["cue_marks"], 1)
        result = self.apply(preview)
        self.assertEqual(self.store.sequence, before + 1)
        self.assertEqual([row["name"] for row in self.listed()], ["Selected"])
        rows = self.listed()[0]["tracks"]
        self.assertEqual([row["title"] for row in rows], ["Bir", "Iki"])
        self.assertEqual(rows[0]["duration_ms"], 123456)
        self.assertTrue(all(row["media_state"] == "unverified" for row in rows))
        self.assertTrue(all("media_id" not in row for row in rows))
        self.assertEqual(len(self.store.scan("items")), 2)
        self.assertTrue(all(row["lifecycle"] == "draft" for row in self.store.scan("items")))
        self.assertTrue(all(not row["eligible"] for row in self.store.scan("station_items")))
        self.assertEqual(self.store.scan("media_files"), [])
        self.assertEqual(self.store.scan("playback_marker_sets"), [])
        self.assertEqual(result["radio_cues_created"], 0)
        analyses = self.store.scan("dj_analysis")
        self.assertEqual(len(analyses), 2)
        self.assertEqual(sum(len(row["marks"]) for row in analyses), 1)
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_append_skip_copy_keep_and_collection(self):
        self.apply(self.preview())
        preview = self.preview()
        self.assertEqual(preview["destinations"][0]["added_count"], 0)
        self.assertEqual(preview["destinations"][0]["skipped"], 3)
        self.apply(preview)
        self.assertEqual(len(self.listed()[0]["tracks"]), 2)
        options = {"name_policy": "copy", "duplicate_policy": "keep", "include_collection": True}
        preview = self.preview(**options)
        self.apply(preview, **options)
        lists = {row["name"]: row for row in self.listed()}
        self.assertEqual(len(lists["Selected (2)"]["tracks"]), 3)
        self.assertEqual(len(lists["Rekordbox koleksiyonu"]["tracks"]), 3)
        self.assertEqual(len(self.store.scan("items")), 3)

    def test_replay_after_restart_exact_receipt_and_conflicting_key(self):
        preview = self.preview()
        key = str(uuid4())
        first = self.apply(preview, key)
        sequence = self.store.sequence
        self.store = open_radio_store(self.db)
        self.assertEqual(self.apply(preview, key), first)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(len(self.listed()[0]["tracks"]), 2)
        with self.assertRaises(Conflict):
            self.apply(preview, key, duplicate_policy="keep")

    def test_stale_preview_and_failed_commit_leave_no_partial_import(self):
        preview = self.preview()
        desktop_playlist.create_playlist(self.store, self.principal, self.sid,
            "Selected", idempotency_key=str(uuid4()))
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            self.apply(preview)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store.scan("items"), [])
        preview = self.preview()
        with patch.object(Transaction, "commit", side_effect=StoreError("injected failure")):
            with self.assertRaises(StoreError):
                self.apply(preview)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store.scan("items"), [])
        self.assertEqual(self.store.scan("dj_analysis"), [])
        self.assertEqual(self.listed()[0]["tracks"], [])

    def test_cue_review_ignore_and_preserved_original_importer(self):
        preview = self.preview(cue_policy="propose_radio_cues")
        self.apply(preview, cue_policy="propose_radio_cues")
        self.assertIn("awaiting_review", {row["review_status"] for row in self.store.scan("dj_analysis")})
        preview = self.preview(library_key="second-library", name_policy="copy", cue_policy="ignore")
        self.apply(preview, library_key="second-library", name_policy="copy", cue_policy="ignore")
        self.assertEqual(len(self.store.scan("dj_analysis")), 2)
        original = importer.preview_xml_import(self.store, self.principal, self.sid,
            "original-api", XML, selected_node_paths=[[0, 1]])
        applied = importer.apply_xml_import(self.store, self.principal, self.sid,
            "original-api", XML, "original-import", selected_node_paths=[[0, 1]],
            expected_preview_hash=original["preview_hash"])
        self.assertEqual(applied["counts"]["items_created"], 1)
        self.assertEqual(len(self.listed()), 2)

    def test_permission_boundaries_duplicate_names_and_limits(self):
        user = create_station_user(self.store, self.principal, self.sid, "view", "Viewer", ["view"])
        viewer = authenticate(self.store, user["token"])
        with self.assertRaises(Forbidden):
            api.preview_import(self.store, viewer, self.sid, self.encoded)
        limited = create_station_user(self.store, self.principal, self.sid, "import", "Importer", ["view", "import", "edit"])
        editor = authenticate(self.store, limited["token"])
        with self.assertRaises(Forbidden):
            api.preview_import(self.store, editor, str(uuid4()), self.encoded)
        for _ in range(2):
            desktop_playlist.create_playlist(self.store, self.principal, self.sid,
                "Selected", idempotency_key=str(uuid4()))
        with self.assertRaises(Conflict):
            self.preview()
        self.assertEqual(self.preview(name_policy="copy")["destinations"][0]["name"], "Selected (2)")
        for invalid in ("!", b64encode(b"x" * (api.XML_LIMIT + 1)).decode()):
            with self.assertRaises(StoreError):
                api.preview_import(self.store, self.principal, self.sid, invalid)
        with self.assertRaises(StoreError):
            self.preview(selected_node_paths=[[0]])
        with self.assertRaises(StoreError):
            api.apply_import(self.store, self.principal, self.sid, self.encoded, "0" * 64, str(uuid4()))

    def test_real_authenticated_http_workflows(self):
        server = BoundedHTTPServer(("127.0.0.1", 0), make_handler(self.store))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            def post(action, body):
                connection = HTTPConnection("127.0.0.1", server.server_address[1], timeout=8)
                connection.request("POST", "/v1/workflows/desktop-rekordbox/" + action,
                    body=json.dumps({"station_id": self.sid, "xml_base64": self.encoded, **body}),
                    headers={"Authorization": "Bearer " + self.identity["api_token"], "Content-Type": "application/json"})
                response = connection.getresponse()
                result = json.loads(response.read())
                code = response.status
                connection.close()
                self.assertEqual(code, 200, result)
                return result
            preview = post("preview", self.options())
            body = self.options() | {"expected_preview_hash": preview["preview_hash"], "idempotency_key": str(uuid4())}
            first = post("apply", body)
            self.assertEqual(post("apply", body), first)
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

    def test_same_library_key_and_track_ids_remain_station_local(self):
        station_id = str(uuid4())
        station = {key: value for key, value in self.store.get("stations", self.sid).items()
                   if key not in {"_id", "_rev"}}
        station.update(code="OTHER", name="Other station")
        self.store.put("stations", station_id, station, actor=self.principal.user_id)
        self.apply(self.preview())
        options = self.options()
        preview = api.preview_import(self.store, self.principal, station_id, self.encoded, **options)
        api.apply_import(self.store, self.principal, station_id, self.encoded,
            preview["preview_hash"], str(uuid4()), **options)
        first = self.listed()[0]
        second = desktop_playlist.list_playlists(self.store, self.principal, station_id)["playlists"][0]
        self.assertNotEqual(first["_id"], second["_id"])
        self.assertEqual(len(self.store.scan("items")), 4)
        namespaces = {row["source_namespace"] for row in self.store.scan("external_ids")}
        self.assertEqual(len(namespaces), 2)

    def test_membership_budget_and_large_decimal_proposals_are_bounded(self):
        source_entries = b'<TRACK Key="1"/><TRACK Key="1"/><TRACK Key="2"/>'
        larger = XML.replace(b'Name="Selected" KeyType="0" Entries="3"',
                             b'Name="Selected" KeyType="0" Entries="501"')
        larger = larger.replace(source_entries, b'<TRACK Key="1"/>' * 501)
        original = self.encoded
        self.encoded = b64encode(larger).decode()
        with self.assertRaises(StoreError):
            self.preview(duplicate_policy="keep")
        self.assertEqual(self.preview()["destinations"][0]["added_count"], 1)
        self.encoded = b64encode(XML.replace(b'TotalTime="123.456"', b'TotalTime="1e9999999"')).decode()
        self.apply(self.preview())
        self.assertIsNone(self.listed()[0]["tracks"][0]["duration_ms"])
        self.encoded = original

    def test_edit_without_file_location_preserves_duplicate_source_identity(self):
        self.encoded = b64encode(XML.replace(b' Location="file://localhost/not-accessible/one.wav"', b'')).decode()
        self.apply(self.preview())
        playlist = self.listed()[0]
        entry = playlist["tracks"][0]
        metadata = {key: entry[key] for key in desktop_playlist.METADATA}
        metadata["title"] = "Operator display correction"
        edited = desktop_playlist.save_entry(self.store, self.principal, self.sid, playlist["_id"],
            metadata, expected_revision=playlist["_rev"], idempotency_key=str(uuid4()),
            entry_id=entry["_id"], expected_entry_revision=entry["_rev"])
        changed = edited["tracks"][0]
        self.assertEqual(changed["title"], "Operator display correction")
        self.assertEqual({key: changed[key] for key in desktop_playlist.REKORDBOX_SOURCE},
                         {key: entry[key] for key in desktop_playlist.REKORDBOX_SOURCE})
        preview = self.preview()
        self.assertEqual(preview["destinations"][0]["added_count"], 0)
        self.apply(preview)
        self.assertEqual(len(self.listed()[0]["tracks"]), 2)


if __name__ == "__main__":
    unittest.main()
