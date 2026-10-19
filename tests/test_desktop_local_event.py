#!/usr/bin/env python3
"""Durable local Event recipes, strict references and unchanged live documents."""
import copy
from http.client import HTTPConnection
import json
import threading
import unittest
from uuid import uuid4

import test_desktop_automation_calendar as fixtures
from sdbase import desktop_api as api, desktop_playlist as playlists
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.catalog import NotFound
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError


class DesktopLocalEvent(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAutomationCalendar("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal
        self.playlist = self.fixture.playlist
        calendar = self.fixture.named_config()
        self.calendar = self.fixture.put(calendar)
        self.weekly = api.put_document(self.store, self.principal, self.sid, "weekly_programme", "Haftalık şema",
            {"schema": 1, "week_anchor": "2026-10-05", "week_count": 1, "repeat": True,
             "calendar": calendar}, str(uuid4()))
        self.programme = api.put_document(self.store, self.principal, self.sid, "programme", "Mevcut yayın",
            {"title": "Mevcut yayın", "date": "2026-10-15", "note": "koru", "events": []}, str(uuid4()))

    def source(self, kind="playlist", source=None, **changes):
        default = {"playlist": self.playlist, "automation_calendar": self.calendar, "weekly_programme": self.weekly}
        return {"id": str(uuid4()), "type": kind, "source_id": (source or default[kind])["_id"], **changes}

    def config(self, sources=None, **changes):
        return {"schema": 1, "title": "Yerel yayın eventi · Türkçe", "kind": "program",
                "sources": sources if sources is not None else [self.source(), self.source("weekly_programme")], **changes}

    def put(self, config, **options):
        return api.put_document(self.store, self.principal, self.sid, "local_event", "Yerel Event",
                                config, str(uuid4()), **options)

    def snapshot(self):
        return self.store.sequence, {name: self.store.scan(name) for name in
            ("desktop_configurations", "playlists", "desktop_playlist_entries", "desktop_mutations")}

    def rejected(self, config, **options):
        before = self.snapshot()
        with self.assertRaises(StoreError):
            self.put(config, **options)
        self.assertEqual(self.snapshot(), before)

    def test_save_replay_cas_reopen_without_changing_any_source_or_programme(self):
        before_documents = self.store.scan("desktop_configurations")
        before_playlists = self.store.scan("playlists")
        config = self.config([self.source(), self.source("automation_calendar"), self.source("weekly_programme")])
        original = copy.deepcopy(config)
        key = str(uuid4())
        saved = api.put_document(self.store, self.principal, self.sid, "local_event", "Yerel Event", config, key)
        sequence = self.store.sequence
        self.assertEqual(api.put_document(self.store, self.principal, self.sid, "local_event", "Yerel Event", config, key), saved)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(config, original)
        changed = copy.deepcopy(config)
        changed["title"] = "Bağımsız düzenleme"
        changed["sources"].reverse()
        actual = self.put(changed, document_id=saved["_id"], expected_revision=saved["_rev"])
        with self.assertRaises(Conflict):
            self.put(config, document_id=saved["_id"], expected_revision=saved["_rev"])
        for document in before_documents:
            self.assertEqual(self.store.get("desktop_configurations", document["_id"]), document)
        self.assertEqual(self.store.scan("playlists"), before_playlists)
        self.assertFalse(self.store.scan("desktop_playlist_entries"))
        before_read = self.snapshot()
        reopened = open_radio_store(self.fixture.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "local_event", actual["_id"]), actual)
        self.assertEqual(api.list_documents(reopened, self.principal, self.sid, "local_event")["records"], [actual])
        self.assertEqual(self.snapshot(), before_read)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_saved_playback_selections_reopen_without_mutating_sources(self):
        baseline = {name: self.store.scan(name) for name in
                    ("playlists", "desktop_playlist_entries", "plans", "playout_events")}
        for player, cluster in ((0, ""), (1, str(uuid4())), (32, str(uuid4()))):
            with self.subTest(player=player):
                config = self.config(playback={"player": player, "next_cluster_id": cluster})
                saved = self.put(config)
                reopened = open_radio_store(self.fixture.fixture.db, read_only=True)
                self.assertEqual(api.get_document(reopened, self.principal, self.sid,
                                                 "local_event", saved["_id"])["config"], config)
        for name, expected in baseline.items():
            self.assertEqual(self.store.scan(name), expected)
        self.assertEqual(self.store.get("desktop_configurations", self.programme["_id"]), self.programme)
        self.assertEqual(inspect_integrity(open_radio_store(self.fixture.fixture.db, read_only=True))["status"], "ok")

    def test_playback_selection_requires_exact_shape_integer_player_and_canonical_cluster(self):
        cluster = str(uuid4())
        invalid = [None, True, [], "auto", {}, {"player": 0}, {"next_cluster_id": ""},
                   {"player": 0, "next_cluster_id": "", "start": True}]
        invalid += [{"player": player, "next_cluster_id": ""}
                    for player in (None, True, -1, 33, 1.0, "1", [], {})]
        invalid += [{"player": 0, "next_cluster_id": value}
                    for value in (None, True, 1, [], {}, "bad", cluster.upper(), " " + cluster)]
        for value in invalid:
            with self.subTest(playback=value):
                self.rejected(self.config(playback=value))

    def test_ordered_repeated_sources_empty_draft_and_all_event_classifications(self):
        repeated = [self.source() for _ in range(100)]
        saved = self.put(self.config(repeated))
        self.assertEqual(saved["config"]["sources"], repeated)
        self.assertEqual(len({source["id"] for source in saved["config"]["sources"]}), 100)
        self.assertEqual(len({source["source_id"] for source in saved["config"]["sources"]}), 1)
        for kind in api.PROGRAMME_KINDS:
            with self.subTest(kind=kind):
                self.assertEqual(self.put(self.config([], kind=kind))["config"]["kind"], kind)

    def test_strict_schema_invalid_shapes_and_excess_sources_are_atomic(self):
        good = self.config()
        invalid = {"schema": [True, 0, 2, 1.0, None],
                   "title": [None, "", "   ", " Name", "Name ", "x" * 161, "bad\0title"],
                   "kind": [None, [], {}, True, "mixed", "unknown"],
                   "sources": [None, True, {}, [self.source()] * 101]}
        for field, values in invalid.items():
            for value in values:
                with self.subTest(field=field, value=value):
                    self.rejected(dict(good, **{field: value}))
        for field in good:
            broken = copy.deepcopy(good)
            del broken[field]
            self.rejected(broken)
        self.rejected(dict(good, start_audio=True))
        for value in (None, True, [], "playlist"):
            self.rejected(self.config([value]))
        source = self.source()
        for field, values in {"id": [None, "bad", source["id"].upper()],
                              "source_id": [None, str(uuid4()), self.playlist["_id"].upper()],
                              "type": [None, True, [], {}, "programme", "automation", "local_event"]}.items():
            for value in values:
                with self.subTest(source_field=field, value=value):
                    self.rejected(self.config([dict(source, **{field: value})]))
        for field in source:
            broken = dict(source)
            del broken[field]
            self.rejected(self.config([broken]))
        self.rejected(self.config([dict(source, name="stale name")]))
        self.rejected(self.config([source, dict(source)]))

    def test_same_station_reference_type_and_local_event_recursion_fences(self):
        other = str(uuid4())
        self.store.put("stations", other, {"code": "LOCAL-EVENT-OTHER", "name": "Other", "timezone": "UTC", "status": "active"})
        foreign_playlist = playlists.create_playlist(self.store, self.principal, other, "Başka istasyon", idempotency_key=str(uuid4()))
        foreign_calendar = api.put_document(self.store, self.principal, other, "automation_calendar", "Other calendar",
            {"schema": 1, "events": []}, str(uuid4()))
        self.rejected(self.config([self.source(source=foreign_playlist)]))
        self.rejected(self.config([self.source("automation_calendar", foreign_calendar)]))
        self.rejected(self.config([self.source("weekly_programme", self.calendar)]))
        self.rejected(self.config([self.source("automation_calendar", self.weekly)]))
        self.rejected(self.config([self.source("weekly_programme", self.programme)]))
        saved = self.put(self.config())
        self.rejected(self.config([self.source("weekly_programme", saved)]))
        with self.assertRaises(NotFound):
            api.get_document(self.store, self.principal, other, "local_event", saved["_id"])
        with self.assertRaises(NotFound):
            api.get_document(self.store, self.principal, self.sid, "programme", saved["_id"])

    def test_disabled_sources_reopen_and_existing_edits_but_cannot_add_occurrences(self):
        sources = [self.source(), self.source("automation_calendar"), self.source("weekly_programme")]
        saved = self.put(self.config(sources))
        playlists.delete_playlist(self.store, self.principal, self.sid, self.playlist["_id"],
            expected_revision=self.playlist["_rev"], idempotency_key=str(uuid4()))
        for document in (self.calendar, self.weekly):
            api.delete_document(self.store, self.principal, self.sid, document["kind"], document["_id"], document["_rev"], str(uuid4()))
        reopened = open_radio_store(self.fixture.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "local_event", saved["_id"]), saved)
        changed = self.config(list(reversed(sources)), title="Eski kaynakları incele")
        actual = self.put(changed, document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(actual["config"], changed)
        self.rejected(changed)
        for source in sources:
            duplicate = dict(source, id=str(uuid4()))
            self.rejected(self.config(sources + [duplicate]), document_id=actual["_id"], expected_revision=actual["_rev"])
        self.assertEqual(inspect_integrity(open_radio_store(self.fixture.fixture.db, read_only=True))["status"], "ok")

    def test_viewer_read_only_and_delete_retires_only_the_local_document(self):
        saved = self.put(self.config())
        viewer = create_station_user(self.store, self.principal, self.sid, "local-viewer", "Event izleyicisi", ["view"])
        principal = authenticate(self.store, viewer["token"])
        before = self.snapshot()
        self.assertEqual(api.list_documents(self.store, principal, self.sid, "local_event")["records"], [saved])
        with self.assertRaises(Forbidden):
            api.put_document(self.store, principal, self.sid, "local_event", "Denied", self.config(), str(uuid4()))
        with self.assertRaises(Forbidden):
            api.delete_document(self.store, principal, self.sid, "local_event", saved["_id"], saved["_rev"], str(uuid4()))
        self.assertEqual(self.snapshot(), before)
        source_documents = [self.store.get("desktop_configurations", doc["_id"]) for doc in (self.calendar, self.weekly, self.programme)]
        source_playlist = self.store.get("playlists", self.playlist["_id"])
        api.delete_document(self.store, self.principal, self.sid, "local_event", saved["_id"], saved["_rev"], str(uuid4()))
        self.assertEqual(api.list_documents(self.store, self.principal, self.sid, "local_event")["records"], [])
        self.assertEqual([self.store.get("desktop_configurations", doc["_id"]) for doc in (self.calendar, self.weekly, self.programme)], source_documents)
        self.assertEqual(self.store.get("playlists", self.playlist["_id"]), source_playlist)

    def test_http_crud_reopen_and_rejected_write_never_modify_other_documents(self):
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
                headers["Authorization"] = "Bearer " + self.fixture.fixture.identity["api_token"]
            connection.request("POST", "/v1/workflows/desktop/" + operation, json.dumps(body), headers)
            response = connection.getresponse()
            result = response.status, json.loads(response.read())
            connection.close()
            return result
        try:
            body = {"station_id": self.sid, "kind": "local_event", "name": "Yerel Event", "config": self.config(), "idempotency_key": str(uuid4())}
            self.assertEqual(request("put", body, False)[0], 403)
            status, saved = request("put", body)
            self.assertEqual(status, 200)
            before = self.snapshot()
            self.assertEqual(request("get", {"station_id": self.sid, "kind": "local_event", "document_id": saved["_id"]}), (200, saved))
            self.assertEqual(request("list", {"station_id": self.sid, "kind": "local_event"})[1]["records"], [saved])
            self.assertEqual(request("put", dict(body, document_id=saved["_id"], expected_revision=0, idempotency_key=str(uuid4())))[0], 409)
            self.assertEqual(request("put", dict(body, config=self.config([self.source(source=self.programme)]), idempotency_key=str(uuid4())))[0], 422)
            self.assertEqual(self.snapshot(), before)
            reopened = open_radio_store(self.fixture.fixture.db, read_only=True)
            self.assertEqual(api.get_document(reopened, self.principal, self.sid, "local_event", saved["_id"]), saved)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
