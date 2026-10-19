"""Real authenticated desktop playlist/automation HTTP persistence tests.

Fixtures, credentials, custom .sdb databases and servers remain under RadioS/build.
User-entered track metadata must never create approved media or playout entries.
"""

from __future__ import annotations

import copy
import unittest
from uuid import UUID, uuid4

import test_database_setup as lifecycle


class PlaylistDataTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = lifecycle.DatabaseSetupTests("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.seed = self.fixture.initialize()
        self.station = self.seed["station_id"]
        self.token = self.fixture.credentials()["api_token"]
        self.fixture.command("start")

    def post(self, action: str, body: dict, *, status: int = 200,
             token: str | None = None) -> dict:
        code, result = self.fixture.request(
            "/v1/workflows/" + action,
            token=self.token if token is None else token,
            body={"station_id": self.station, **body})
        self.assertEqual(code, status, f"{action}: {result}")
        self.assertIsInstance(result, dict)
        return result

    def create(self, name: str = "Türkçe çalma listesi") -> dict:
        row = self.post("desktop-playlists/create", {
            "name": name, "idempotency_key": str(uuid4())})
        self.assertEqual(str(UUID(row["_id"])), row["_id"])
        self.assertEqual(str(UUID(row["version_id"])), row["version_id"])
        self.assertEqual(row["tracks"], [])
        self.assertEqual(row["media_state"], "unverified")
        return row

    @staticmethod
    def metadata(title: str = "Dış dosya metadata taslağı") -> dict:
        return {"title": title, "artist": "Sanatçı", "album": "Albüm",
                "duration_ms": 123456, "bpm": 127.5, "genre": "Pop",
                "key": "Am", "location": "/erişimi/doğrulanmayan/şarkı.wav",
                "comment": "Kullanıcı girdisi; gerçek ses analizi yapılmadı."}

    def save(self, playlist: dict, metadata: dict, *, entry: dict | None = None) -> dict:
        body = {"playlist_id": playlist["_id"], "expected_revision": playlist["_rev"],
                "metadata": metadata, "idempotency_key": str(uuid4())}
        if entry is not None:
            body.update(entry_id=entry["_id"], expected_entry_revision=entry["_rev"])
        return self.post("desktop-playlists/entry-save", body)

    def listed(self) -> list[dict]:
        return self.post("desktop-playlists/list", {})["playlists"]

    def inspect(self) -> dict:
        return self.fixture.backend(
            "import json,sys; from sdbase.schema import open_radio_store; "
            "store=open_radio_store(sys.argv[1],read_only=True); "
            "names=('playlists','playlist_versions','playlist_entries',"
            "'desktop_playlist_entries','desktop_configurations','items','versions','events'); "
            "print(json.dumps({name:store.scan(name) for name in names}))")

    def restart(self) -> None:
        self.fixture.command("stop")
        self.fixture.command("start")

    def test_online_provider_entries_are_rejected_over_http(self) -> None:
        playlist = self.save(self.create("Karışık kaynaklar"), self.metadata("Yerel taslak"))
        references = (("youtube", "dQw4w9WgXcQ", "https://music.youtube.com/watch?v=dQw4w9WgXcQ"),
                      ("spotify", "4uLU6hMCjMI75M1A2tKUQC", "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"),
                      ("apple_music", "1594677759", "https://music.apple.com/tr/song/1594677759"))
        for provider, external_id, url in references:
            body = {"playlist_id": playlist["_id"], "expected_revision": playlist["_rev"],
                    "metadata": dict(self.metadata(provider), provider=provider,
                                     external_id=external_id, url=url),
                    "idempotency_key": str(uuid4())}
            result = self.post("desktop-playlists/entry-save", body, status=422)
            self.assertEqual(result["error"], "invalid_request")
            self.assertIn("no longer supported", result["detail"])
        self.restart()
        self.assertEqual(self.listed(), [playlist])
        self.assertEqual([row["title"] for row in playlist["tracks"]], ["Yerel taslak"])
        records = self.inspect()
        self.assertFalse(records["items"])
        self.assertFalse(records["versions"])
        self.assertFalse(records["playlist_entries"])
        self.assertFalse(records["events"])

    def test_metadata_uuid_revision_roundtrip_without_fake_playable_media(self) -> None:
        playlist = self.create()
        identity, version = playlist["_id"], playlist["version_id"]
        playlist = self.save(playlist, self.metadata())
        row = playlist["tracks"][0]
        self.assertEqual(str(UUID(row["_id"])), row["_id"])
        self.assertEqual(row["playlist_version_id"], version)
        self.assertEqual(row["media_state"], "unverified")
        self.assertEqual({key: row[key] for key in self.metadata()}, self.metadata())
        changed = self.metadata("Başlığı değişen kayıt")
        changed["duration_ms"] = None
        playlist = self.save(playlist, changed, entry=row)
        self.assertEqual(playlist["_id"], identity)
        self.assertEqual(playlist["version_id"], version)
        self.assertEqual(playlist["tracks"][0]["_id"], row["_id"])
        self.assertGreater(playlist["tracks"][0]["_rev"], row["_rev"])
        self.assertIsNone(playlist["tracks"][0]["duration_ms"])
        self.restart()
        self.assertEqual(self.listed(), [playlist])
        self.fixture.command("stop")
        records = self.inspect()
        for name in ("items", "versions", "playlist_entries", "events"):
            self.assertEqual(records[name], [], f"Metadata invented {name}")
        self.assertEqual(len(records["desktop_playlist_entries"]), 1)
        self.assertEqual(records["playlist_versions"][0]["status"], "draft")

    def test_rename_order_and_multi_remove_preserve_ids_across_restart(self) -> None:
        playlist = self.create()
        for name in ("Bir", "İki", "Üç"):
            playlist = self.save(playlist, self.metadata(name))
        identifiers = [row["_id"] for row in playlist["tracks"]]
        playlist = self.post("desktop-playlists/rename", {
            "playlist_id": playlist["_id"], "expected_revision": playlist["_rev"],
            "name": "Yeniden adlandırılan liste", "idempotency_key": str(uuid4())})
        order = list(reversed(identifiers))
        playlist = self.post("desktop-playlists/reorder", {
            "playlist_id": playlist["_id"], "expected_revision": playlist["_rev"],
            "entries": order, "idempotency_key": str(uuid4())})
        self.assertEqual([row["_id"] for row in playlist["tracks"]], order)
        self.assertEqual([row["ordinal"] for row in playlist["tracks"]], [0, 1, 2])
        removed = playlist["tracks"][:2]
        playlist = self.post("desktop-playlists/entry-remove", {
            "playlist_id": playlist["_id"], "expected_revision": playlist["_rev"],
            "entries": [{"entry_id": row["_id"], "expected_revision": row["_rev"]}
                        for row in removed], "idempotency_key": str(uuid4())})
        self.assertEqual([row["_id"] for row in playlist["tracks"]], [order[2]])
        self.assertEqual(playlist["tracks"][0]["ordinal"], 0)
        self.restart()
        self.assertEqual(self.listed(), [playlist])
        self.fixture.command("stop")
        retired = [row for row in self.inspect()["desktop_playlist_entries"]
                   if row["status"] == "removed"]
        self.assertEqual({row["_id"] for row in retired}, {row["_id"] for row in removed})
        self.assertTrue(all(row["active_ordinal"] is None for row in retired))

    def test_stale_parent_and_entry_cas_reject_without_partial_writes(self) -> None:
        playlist = self.save(self.create(), self.metadata())
        stale = copy.deepcopy(playlist)
        playlist = self.save(playlist, self.metadata("İlk editörün kaydı"), entry=playlist["tracks"][0])
        self.post("desktop-playlists/rename", {
            "playlist_id": playlist["_id"], "expected_revision": stale["_rev"],
            "name": "Stale ekran", "idempotency_key": str(uuid4())}, status=409)
        self.post("desktop-playlists/entry-save", {
            "playlist_id": playlist["_id"], "expected_revision": playlist["_rev"],
            "entry_id": stale["tracks"][0]["_id"],
            "expected_entry_revision": stale["tracks"][0]["_rev"],
            "metadata": self.metadata("Kaybolmaması gereken kullanıcı girdisi"),
            "idempotency_key": str(uuid4())}, status=409)
        self.post("desktop-playlists/reorder", {
            "playlist_id": playlist["_id"], "expected_revision": playlist["_rev"],
            "entries": [], "idempotency_key": str(uuid4())}, status=409)
        self.assertEqual(self.listed(), [playlist])

    def test_receipt_replay_does_not_duplicate_playlist_or_entry(self) -> None:
        create = {"name": "Tekrarlanan istek", "idempotency_key": str(uuid4())}
        playlist = self.post("desktop-playlists/create", create)
        self.assertEqual(self.post("desktop-playlists/create", create), playlist)
        entry = {"playlist_id": playlist["_id"], "expected_revision": playlist["_rev"],
                 "metadata": self.metadata(), "idempotency_key": str(uuid4())}
        playlist = self.post("desktop-playlists/entry-save", entry)
        self.assertEqual(self.post("desktop-playlists/entry-save", entry), playlist)
        altered = {**entry, "metadata": self.metadata("Aynı anahtar farklı içerik")}
        self.post("desktop-playlists/entry-save", altered, status=409)
        self.restart()
        self.assertEqual(self.post("desktop-playlists/entry-save", entry), playlist)
        self.assertEqual(self.listed(), [playlist])
        self.assertEqual(len(playlist["tracks"]), 1)

    def test_invalid_metadata_and_foreign_identity_leave_stored_rows_unchanged(self) -> None:
        playlist = self.save(self.create(), self.metadata())
        invalid = [self.metadata(), self.metadata(), self.metadata(), self.metadata()]
        invalid[0]["duration_ms"] = -1
        invalid[1]["bpm"] = True
        invalid[2]["media_state"] = "verified"
        invalid[3]["title"] = " "
        for metadata in invalid:
            self.post("desktop-playlists/entry-save", {
                "playlist_id": playlist["_id"], "expected_revision": playlist["_rev"],
                "metadata": metadata, "idempotency_key": str(uuid4())}, status=422)
        other = self.save(self.create("Başka liste"), self.metadata("Başka kayıt"))
        self.post("desktop-playlists/entry-save", {
            "playlist_id": playlist["_id"], "expected_revision": playlist["_rev"],
            "entry_id": other["tracks"][0]["_id"],
            "expected_entry_revision": other["tracks"][0]["_rev"],
            "metadata": self.metadata(), "idempotency_key": str(uuid4())}, status=409)
        before = {row["_id"]: row for row in self.listed()}
        self.assertEqual(before[playlist["_id"]], playlist)
        self.assertEqual(before[other["_id"]], other)

    def test_soft_delete_cas_and_authentication_never_delete_history(self) -> None:
        playlist = self.save(self.create(), self.metadata())
        body = {"station_id": self.station, "playlist_id": playlist["_id"],
                "expected_revision": playlist["_rev"], "idempotency_key": str(uuid4())}
        for token in (None, "incorrect-bearer-token"):
            status, _result = self.fixture.request("/v1/workflows/desktop-playlists/delete",
                                                   token=token, body=body)
            self.assertEqual(status, 403)
        result = self.post("desktop-playlists/delete", body)
        self.assertTrue(result["deleted"])
        self.assertEqual(self.post("desktop-playlists/delete", body), result)
        self.assertEqual(self.listed(), [])
        self.restart()
        self.assertEqual(self.listed(), [])
        _command, check = self.fixture.command("check")
        self.assertEqual(check["status"], "ok")
        self.fixture.command("stop")
        records = self.inspect()
        self.assertEqual(records["playlists"][0]["status"], "disabled")
        self.assertEqual(records["desktop_playlist_entries"][0]["_id"], playlist["tracks"][0]["_id"])

    @staticmethod
    def automation(playlist_id: str) -> dict:
        return {"name": "Gerçek kaydedilen rotasyon", "seed": "RADIOS-STAGE6",
                "planDate": "2026-10-06", "startTime": "08:30", "windowStart": "06:00",
                "windowEnd": "23:00", "sourceMode": 1, "trackMode": 2, "transition": 1,
                "boundaryTransition": 0, "deckMode": 0, "fallbackMode": 2,
                "trackGap": 30, "artistGap": 60, "albumGap": 90, "previewCount": 20,
                "jingleEvery": 4, "overlap": 2.5, "boundaryOverlap": 1.0,
                "includeProgramme": True, "jingleEnabled": False, "jingleDry": True,
                "requireMetadata": True, "jinglePlaylist": None,
                "fallbackPlaylist": playlist_id, "jingleDeck": None,
                "weekdays": [1, 2, 3, 4, 5], "decks": ["native-card-1"],
                "sources": [{"source_id": str(uuid4()), "playlist_id": playlist_id,
                             "weight": 25, "count": 2, "start": "06:30", "end": "22:30", "days": 1}]}

    def test_automation_typed_rules_reference_real_playlist_and_persist(self) -> None:
        playlist = self.create()
        config = self.automation(playlist["_id"])
        create = {"kind": "automation", "name": "main", "config": config,
                  "idempotency_key": str(uuid4())}
        record = self.post("desktop/put", create)
        self.assertEqual(record["config"], config)
        self.assertEqual(self.post("desktop/put", create), record)
        self.restart()
        listing = self.post("desktop/list", {"kind": "automation"})
        self.assertEqual(listing["records"], [record])
        changed = copy.deepcopy(config)
        changed["sources"][0]["weight"] = 37
        updated = self.post("desktop/put", {"kind": "automation", "name": "main",
            "config": changed, "document_id": record["_id"], "expected_revision": record["_rev"],
            "idempotency_key": str(uuid4())})
        self.assertEqual(updated["_id"], record["_id"])
        self.assertGreater(updated["_rev"], record["_rev"])
        self.post("desktop/put", {"kind": "automation", "name": "main", "config": config,
            "document_id": record["_id"], "expected_revision": record["_rev"],
            "idempotency_key": str(uuid4())}, status=409)
        self.restart()
        self.assertEqual(self.post("desktop/get", {"kind": "automation",
                         "document_id": record["_id"]}), updated)
        self.fixture.command("stop")
        self.assertEqual(self.inspect()["events"], [])

    def test_automation_invalid_references_and_numbers_are_rejected_atomically(self) -> None:
        playlist = self.create()
        valid = self.automation(playlist["_id"])
        cases = [copy.deepcopy(valid) for _ in range(4)]
        cases[0]["sources"][0]["playlist_id"] = str(uuid4())
        cases[1]["sources"][0]["weight"] = 0
        cases[2]["weekdays"] = [1, 1]
        cases[3]["overlap"] = 31
        for config in cases:
            self.post("desktop/put", {"kind": "automation", "name": "main", "config": config,
                                      "idempotency_key": str(uuid4())}, status=422)
        self.assertEqual(self.post("desktop/list", {"kind": "automation"})["records"], [])


class MixedPlaylistSourceTests(unittest.TestCase):
    """Real local bytes, provider identity, CAS and persisted schema validation."""

    def setUp(self) -> None:
        import test_desktop_api as fixtures
        from sdbase import desktop_playlist
        self.api = desktop_playlist
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store, self.station, self.principal = (self.fixture.store, self.fixture.sid,
                                                   self.fixture.principal)
        self.playlist = self.api.create_playlist(self.store, self.principal, self.station,
                                                "Karışık playlist", idempotency_key=str(uuid4()))

    @staticmethod
    def online(provider="youtube"):
        identity, url = {
            "youtube": ("dQw4w9WgXcQ", "https://music.youtube.com/watch?v=dQw4w9WgXcQ"),
            "spotify": ("4uLU6hMCjMI75M1A2tKUQC", "https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"),
            "apple_music": ("1594677759", "https://music.apple.com/tr/song/1594677759")}[provider]
        return dict(PlaylistDataTests.metadata(provider), provider=provider,
                    external_id=identity, url=url)

    def save(self, metadata, **changes):
        arguments = {"expected_revision": self.playlist["_rev"], "idempotency_key": str(uuid4()),
                     **changes}
        self.playlist = self.api.save_entry(self.store, self.principal, self.station,
                                            self.playlist["_id"], metadata, **arguments)
        return self.playlist

    def test_verified_local_and_metadata_rows_keep_order_after_reopen(self):
        from sdbase.integrity import inspect_integrity
        from sdbase.schema import open_radio_store
        binding = self.fixture.verified_binding()
        self.save(dict(PlaylistDataTests.metadata("Yerel doğrulanmış medya"), **binding))
        for title in ("İkinci taslak", "Üçüncü taslak"):
            self.save(PlaylistDataTests.metadata(title))
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(self.api.list_playlists(reopened, self.principal, self.station)["playlists"],
                         [self.playlist])
        self.assertEqual([row["ordinal"] for row in self.playlist["tracks"]], [0, 1, 2])
        self.assertEqual({key: self.playlist["tracks"][0][key] for key in binding}, binding)
        self.assertTrue(all(not set(binding).intersection(row) for row in self.playlist["tracks"][1:]))
        self.assertEqual(len(reopened.scan("items")), 1)
        self.assertFalse(reopened.scan("playlist_entries"))
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_online_sources_and_partial_bindings_are_rejected_atomically(self):
        from sdbase.storage import StoreError
        good = self.online()
        binding = self.fixture.verified_binding()
        retired = [good, dict(good, provider="unsupported"), dict(good, external_id="bad"),
                   dict(good, url="https://music.youtube.com/watch?v=dQw4w9WgXcQ&token=secret"),
                   dict(good, online_id=str(uuid4())),
                   {key: value for key, value in good.items() if key != "provider"},
                   {key: value for key, value in good.items() if key != "external_id"},
                   {key: value for key, value in good.items() if key != "url"},
                   dict(PlaylistDataTests.metadata(), online_id=str(uuid4())),
                   dict(good, analysis_media=binding),
                   dict(PlaylistDataTests.metadata(), analysis_media=binding),
                   dict(good, **binding), dict(good, media_id=binding["media_id"])]
        partial = [dict(PlaylistDataTests.metadata(), media_id=binding["media_id"]),
                   dict(PlaylistDataTests.metadata(), **binding, unknown_field=True)]
        sequence = self.store.sequence
        for metadata in retired:
            with self.subTest(metadata=metadata), self.assertRaisesRegex(StoreError, "no longer supported"):
                self.save(metadata)
            with self.subTest(create=metadata), self.assertRaisesRegex(StoreError, "no longer supported"):
                self.api.create_playlist(self.store, self.principal, self.station, "Reddedilen kaynak",
                                         idempotency_key=str(uuid4()), entries=[metadata])
            self.assertEqual(self.store.sequence, sequence)
        for metadata in partial:
            with self.subTest(metadata=metadata), self.assertRaises(StoreError):
                self.save(metadata)
            self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.playlist["tracks"], [])
        self.assertFalse(self.store.scan("desktop_playlist_entries"))

    def test_entry_cas_replay_and_permissions(self):
        from sdbase.access_admin import create_station_user
        from sdbase.identity import Forbidden, authenticate
        from sdbase.storage import Conflict
        metadata, key = PlaylistDataTests.metadata("CAS taslağı"), str(uuid4())
        revision = self.playlist["_rev"]
        first = self.save(metadata, expected_revision=revision, idempotency_key=key)
        sequence = self.store.sequence
        replay = self.api.save_entry(self.store, self.principal, self.station, first["_id"],
                                      metadata, expected_revision=revision, idempotency_key=key)
        self.assertEqual(replay, first)
        self.assertEqual(self.store.sequence, sequence)
        with self.assertRaises(Conflict):
            self.save(dict(metadata, comment="different replay"), expected_revision=revision,
                      idempotency_key=key)
        old = first["tracks"][0]
        current = self.save(dict(metadata, comment="Edited"), entry_id=old["_id"],
                            expected_entry_revision=old["_rev"])
        self.assertEqual(current["tracks"][0]["_id"], old["_id"])
        with self.assertRaises(Conflict):
            self.save(metadata, entry_id=old["_id"], expected_entry_revision=old["_rev"])
        viewer = create_station_user(self.store, self.principal, self.station,
                                     "mixed-viewer", "Viewer", ["view"])
        principal = authenticate(self.store, viewer["token"])
        self.assertEqual(self.api.list_playlists(self.store, principal, self.station)["playlists"],
                         [current])
        with self.assertRaises(Forbidden):
            self.api.save_entry(self.store, principal, self.station, current["_id"], metadata,
                                expected_revision=current["_rev"], idempotency_key=str(uuid4()))

    def test_persisted_source_fields_cannot_bypass_validation_and_legacy_rows_stay_readable(self):
        from sdbase import desktop_playout
        from sdbase.integrity import inspect_integrity
        from sdbase.schema import open_radio_store
        from sdbase.storage import StoreError
        binding = self.fixture.verified_binding()
        self.save(dict(PlaylistDataTests.metadata("Yerel doğrulanmış"), **binding))
        row = self.playlist["tracks"][0]
        body = {key: value for key, value in row.items() if key not in {"_id", "_rev"}}
        sequence = self.store.sequence
        for changes in ({"url": "https://example.invalid/audio"}, {"external_id": "bad"},
                        {"provider": "youtube"}, {"media_id": str(uuid4())},
                        {"analysis_media": binding}):
            with self.subTest(changes=changes), self.assertRaises(StoreError):
                self.store.put("desktop_playlist_entries", row["_id"], dict(body, **changes),
                               expected_revision=row["_rev"])
            self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store.get("desktop_playlist_entries", row["_id"]), row)
        # A provider row written before the retirement keeps its shape and stays readable.
        legacy = {key: value for key, value in body.items() if key not in binding}
        legacy.update(provider="youtube", external_id="dQw4w9WgXcQ",
                      url="https://music.youtube.com/watch?v=dQw4w9WgXcQ", ordinal=1, active_ordinal=1,
                      title="Eski sağlayıcı satırı")
        self.store.put("desktop_playlist_entries", str(uuid4()), legacy)
        for bad in ({"external_id": ""}, {"url": 5}, {"provider": "youtube", "external_id": None}):
            with self.subTest(bad=bad), self.assertRaises(StoreError):
                self.store.put("desktop_playlist_entries", str(uuid4()),
                               dict(legacy, ordinal=2, active_ordinal=2, **bad))
        reopened = open_radio_store(self.fixture.db, read_only=True)
        listed, = self.api.list_playlists(reopened, self.principal, self.station)["playlists"]
        self.assertEqual([entry["title"] for entry in listed["tracks"]],
                         ["Yerel doğrulanmış", "Eski sağlayıcı satırı"])
        self.assertEqual(listed["tracks"][1]["external_id"], "dQw4w9WgXcQ")
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        prepared = desktop_playout.prepare(reopened, self.principal, self.station, "playlist",
                                           playlist_id=self.playlist["_id"])
        self.assertEqual([track["reasons"] for track in prepared["tracks"]], [[], ["media_unbound"]])
        self.assertEqual(prepared["tracks"][1]["metadata"]["url"], legacy["url"])

    def test_retired_external_playlist_documents_and_http_contract(self):
        from http.client import HTTPConnection
        import json
        import threading
        from sdbase import desktop_api as api
        from sdbase.api_server import BoundedHTTPServer
        from sdbase.http_server import make_handler
        from sdbase.storage import StoreError
        metadata = self.online()
        config = {"schema_version": 1, "provider": metadata["provider"], "remote_id": "",
                  "remote_url": "", "description": "", "items": [
                      {"provider_id": metadata["external_id"], "url": metadata["url"],
                       "title": "Kaynak", "artist": "", "duration_ms": 0}]}
        sequence = self.store.sequence
        for kind in ("external_playlist", "online_services"):
            with self.subTest(kind=kind), self.assertRaisesRegex(StoreError, "unsupported configuration kind"):
                api.put_document(self.store, self.principal, self.station, kind, "Kaynak liste", config, str(uuid4()))
            with self.subTest(list=kind), self.assertRaisesRegex(StoreError, "unsupported configuration kind"):
                api.list_documents(self.store, self.principal, self.station, kind)
        self.assertEqual(self.store.sequence, sequence)
        binding = self.fixture.verified_binding()
        base = make_handler(self.store)
        class Handler(base):
            def log_message(self, *_args):
                pass
        server = BoundedHTTPServer(("127.0.0.1", 0), Handler, request_timeout=3)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            def request(action, body):
                connection = HTTPConnection("127.0.0.1", server.server_port, timeout=3)
                connection.request("POST", "/v1/workflows/" + action, json.dumps(body),
                                   {"Content-Type": "application/json", "Connection": "close",
                                    "Authorization": "Bearer " + self.fixture.identity["api_token"]})
                response = connection.getresponse()
                result = json.loads(response.read())
                connection.close()
                return response.status, result
            sequence = self.store.sequence
            for rejected in (dict(self.online(), analysis_media=binding), self.online("spotify")):
                status, result = request("desktop-playlists/entry-save", {
                    "station_id": self.station, "playlist_id": self.playlist["_id"],
                    "expected_revision": self.playlist["_rev"], "idempotency_key": str(uuid4()),
                    "metadata": rejected})
                self.assertEqual(status, 422, result)
                self.assertEqual(result["error"], "invalid_request")
                self.assertIn("no longer supported", result["detail"])
            status, result = request("desktop/put", {
                "station_id": self.station, "kind": "external_playlist", "name": "Kaynak liste",
                "config": config, "idempotency_key": str(uuid4())})
            self.assertEqual(status, 422, result)
            self.assertEqual(self.store.sequence, sequence)
            status, result = request("desktop-playlists/entry-save", {
                "station_id": self.station, "playlist_id": self.playlist["_id"],
                "expected_revision": self.playlist["_rev"], "idempotency_key": str(uuid4()),
                "metadata": dict(PlaylistDataTests.metadata("HTTP yerel"), **binding)})
            self.assertEqual(status, 200, result)
            self.assertEqual({key: result["tracks"][0][key] for key in binding}, binding)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)


if __name__ == "__main__":
    unittest.main()
