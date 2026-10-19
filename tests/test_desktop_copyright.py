#!/usr/bin/env python3
"""Station-scoped manual copyright dates, persistence and per-item filtering."""

import copy
from datetime import datetime, timezone
from http.client import HTTPConnection
import json
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4

import test_desktop_playout as fixtures
from sdbase import desktop_api as api, desktop_copyright as copyright, desktop_media
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError


NOW = datetime(2026, 10, 13, 12, 0, tzinfo=timezone.utc)


def rule(**extra):
    return {"id": str(uuid4()), "artist": "", "album": "", "title": "Gerçek DJ sampler",
            "starts_at": "2026-10-13T11:00:00+00:00", "ends_at": "2026-10-13T13:00:00+00:00",
            "note": "Operatörün girdiği süre", "enabled": True, **extra}


def policy(*rules, restrict=True, unregistered="allow"):
    return {"schema": 1, "restrict": restrict, "unregistered": unregistered, "rules": list(rules)}


class CopyrightPolicy(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopPlayout("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal
        self.clock = patch.object(copyright, "utc_now", return_value=NOW)
        self.clock.start()
        self.addCleanup(self.clock.stop)

    def put(self, config, **extra):
        return api.put_document(self.store, self.principal, self.sid,
                                "copyright_policy", "main", config, str(uuid4()), **extra)

    def test_persistence_reopen_replay_cas_and_tenant_isolation(self):
        original = policy(rule(artist="Işık", title="Bir şarkı", album="Albüm adı"))
        operation = str(uuid4())
        saved = api.put_document(self.store, self.principal, self.sid,
                                  "copyright_policy", "main", original, operation)
        sequence = self.store.sequence
        self.assertEqual(api.put_document(self.store, self.principal, self.sid,
            "copyright_policy", "main", original, operation), saved)
        self.assertEqual(self.store.sequence, sequence)
        original["rules"][0]["title"] = "Çağırıcı değişikliği"
        self.assertEqual(api.get_document(self.store, self.principal, self.sid,
            "copyright_policy", saved["_id"])["config"]["rules"][0]["title"], "Bir şarkı")
        changed = {**saved["config"], "restrict": False}
        updated = self.put(changed, document_id=saved["_id"], expected_revision=saved["_rev"])
        with self.assertRaises(Conflict):
            self.put(changed, document_id=saved["_id"], expected_revision=saved["_rev"])
        reopened = open_radio_store(self.fixture.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid,
            "copyright_policy", saved["_id"]), updated)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        other = str(uuid4())
        self.store.put("stations", other, {"code": "COPY_OTHER", "name": "Other", "timezone": "UTC", "status": "active"})
        self.assertEqual(api.list_documents(self.store, self.principal, other, "copyright_policy")["records"], [])
        with self.assertRaises(StoreError):
            api.get_document(self.store, self.principal, other, "copyright_policy", saved["_id"])
        viewer = create_station_user(self.store, self.principal, self.sid, "copyright-view", "Viewer", ["view"])
        principal = authenticate(self.store, viewer["token"])
        self.assertEqual(api.get_document(self.store, principal, self.sid, "copyright_policy", saved["_id"]), updated)
        with self.assertRaises(Forbidden):
            api.put_document(self.store, principal, self.sid, "copyright_policy", "main", changed, str(uuid4()))
        with self.assertRaises(Forbidden):
            api.list_documents(self.store, principal, other, "copyright_policy")

    def test_single_canonical_policy_and_disabled_document_cannot_replace_it(self):
        saved = self.put(policy(rule()))
        with self.assertRaises(Conflict):
            self.put(policy(rule()))
        with self.assertRaises(StoreError):
            api.put_document(self.store, self.principal, self.sid,
                              "copyright_policy", "Another", policy(rule()), str(uuid4()))
        disabled = api.delete_document(self.store, self.principal, self.sid, "copyright_policy",
            saved["_id"], saved["_rev"], str(uuid4()))
        replacement = self.put(policy(rule(), restrict=False))
        before = self.store.sequence
        with self.assertRaises(Conflict):
            self.put(saved["config"], document_id=saved["_id"], expected_revision=disabled["_rev"])
        self.assertEqual(self.store.sequence, before)
        self.assertEqual(copyright.policy_document(self.store, self.sid)["_id"], replacement["_id"])

    def test_validation_rejects_unbounded_ambiguous_and_invalid_dates(self):
        good = policy(rule())
        invalid = []
        for field, value in (("schema", 2), ("restrict", 1), ("unregistered", []), ("unknown", "x")):
            bad = copy.deepcopy(good); bad[field] = value; invalid.append(bad)
        for field, value in (("id", "not-a-UUID"), ("enabled", 1), ("artist", "x" * 201),
                             ("starts_at", "2026-10-13"), ("starts_at", "2026-10-13T12:00:00"),
                             ("ends_at", "2026-10-13T11:00:00+00:00"),
                             ("note", "x" * 2001), ("unknown", "unsupported")):
            bad = copy.deepcopy(good); bad["rules"][0][field] = value; invalid.append(bad)
        invalid.append(policy(rule(title="  \t\n")))
        invalid.append(policy(good["rules"][0], good["rules"][0]))
        invalid.append(policy(*(rule() for _ in range(copyright.MAX_RULES + 1))))
        for index, bad in enumerate(invalid):
            with self.subTest(index=index), self.assertRaises(StoreError):
                self.put(bad)
        self.assertFalse(self.store.scan("desktop_configurations"))

    def test_normalized_turkish_exact_matching_and_selector_intersection(self):
        rows = [{"title": "  ILGAZ   ŞARKISI  ", "artist": "İlhan Işık", "album": "Cafe\u0301"},
                {"title": "Ilgaz şarkısı", "artist": "Başka", "album": "Café"},
                {"title": "Ilgaz şarkısı canlı", "artist": "İlhan Işık", "album": "Café"},
                {"title": "Ilgaz şarkısı", "artist": "İlhan Işık", "album": "Diğer"}]
        expired = rule(title="ılgaz şarkısı", artist="ilhan ışık", album="Café",
                       ends_at="2026-10-13T11:59:00Z")
        config = copyright.validate_policy(policy(expired))
        first = copyright.evaluate(config, rows[0])
        self.assertFalse(first["allowed"])
        self.assertEqual(first["matching_rule_ids"], [expired["id"]])
        self.assertEqual(first["reasons"], ["copyright_expired"])
        for metadata in rows[1:]:
            self.assertTrue(copyright.evaluate(config, metadata)["allowed"])

    def test_boundaries_timezone_future_and_multiple_rules_intersect(self):
        wide = rule(starts_at="2026-10-13T13:00:00+03:00", ends_at="2026-10-13T17:00:00+03:00")
        narrow = rule(starts_at="2026-10-13T12:00:00Z", ends_at="2026-10-13T13:00:00Z")
        config = policy(wide, narrow)
        metadata = {"title": wide["title"]}
        at_start = copyright.evaluate(config, metadata, at=NOW)
        self.assertTrue(at_start["allowed"])
        self.assertEqual(at_start["valid_from"], "2026-10-13T12:00:00+00:00")
        self.assertEqual(at_start["valid_until"], "2026-10-13T13:00:00+00:00")
        self.assertEqual(copyright.evaluate(config, metadata,
            at=datetime(2026, 10, 13, 13, tzinfo=timezone.utc))["reasons"], ["copyright_expired"])
        self.assertEqual(copyright.evaluate(config, metadata,
            at=datetime(2026, 10, 13, 11, tzinfo=timezone.utc))["reasons"], ["copyright_not_started"])
        disjoint = policy(rule(ends_at="2026-10-13T11:59:00Z"), rule(starts_at="2026-10-13T12:01:00Z"))
        self.assertEqual(copyright.evaluate(disjoint, metadata)["reasons"],
                         ["copyright_not_started", "copyright_expired"])

    def test_restrict_off_disabled_rules_and_unmatched_default_allow(self):
        metadata = {"title": "Gerçek DJ sampler"}
        expired = rule(ends_at="2026-10-13T11:59:00Z")
        self.assertTrue(copyright.evaluate(policy(expired, restrict=False), metadata)["allowed"])
        self.assertTrue(copyright.evaluate(policy({**expired, "enabled": False}), metadata)["allowed"])
        self.assertTrue(copyright.evaluate(policy(expired), {"title": "Kaydı olmayan parça"})["allowed"])
        self.assertFalse(copyright.evaluate(policy(expired, unregistered="deny"), {"title": "Başka"})["allowed"])
        self.assertTrue(copyright.evaluate(policy(expired, restrict=False, unregistered="deny"), {"title": "Başka"})["allowed"])

    def test_playlist_only_matched_item_skipped_and_durable_sources_unchanged(self):
        first = self.fixture.fixture.verified_binding()
        second = self.fixture.fixture.verified_binding()
        detail = desktop_media.get_media(self.store, self.principal, self.sid, second["media_id"])
        desktop_media.save_metadata(self.store, self.principal, self.sid, second["media_id"],
            detail["media"]["_rev"], str(uuid4()), {"title": "Başka parça", "kind": "jingle", "artist": "Başka sanatçı"})
        playlist = self.fixture.save(self.fixture.create(), first)
        playlist = self.fixture.save(playlist, second)
        saved = self.put(policy(rule(ends_at="2026-10-13T11:59:00Z")))
        before = (self.store.sequence, self.store.scan("playlists"), self.store.scan("playlist_entries"))
        prepared = self.fixture.prepare(playlist_id=playlist["_id"])
        self.assertEqual([row["eligible"] for row in prepared["tracks"]], [False, True])
        self.assertTrue(all(row["media_eligible"] for row in prepared["tracks"]))
        self.assertEqual(prepared["tracks"][0]["reasons"], ["copyright_expired"])
        self.assertEqual(prepared["copyright_policy"], saved["config"])
        self.assertEqual(prepared["copyright_policy_document_id"], saved["_id"])
        self.assertEqual(prepared["copyright_policy_revision"], saved["_rev"])
        self.assertEqual((self.store.sequence, self.store.scan("playlists"), self.store.scan("playlist_entries")), before)
        disabled = self.put({**saved["config"], "restrict": False},
            document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertTrue(all(row["eligible"] for row in self.fixture.prepare(playlist_id=playlist["_id"])["tracks"]))
        self.assertEqual(self.store.get("playlists", playlist["_id"])["_rev"], playlist["_rev"])
        self.assertEqual(disabled["_rev"], 2)

    def test_programme_automation_jingle_fallback_and_unbound_base_preserved(self):
        binding = self.fixture.fixture.verified_binding()
        playlist = self.fixture.save(self.fixture.create(), binding)
        self.put(policy(rule(ends_at="2026-10-13T11:59:00Z")))
        programme = self.fixture.prepare("programme", config=self.fixture.programme(binding))
        self.assertFalse(programme["tracks"][0]["eligible"])
        self.assertTrue(programme["tracks"][0]["media_eligible"])
        automation = self.fixture.prepare("automation", config=self.fixture.automation(playlist["_id"]))
        self.assertEqual([source["role"] for source in automation["sources"]], ["music", "jingle", "fallback"])
        for source in automation["sources"]:
            self.assertFalse(source["tracks"][0]["eligible"])
            self.assertTrue(source["tracks"][0]["media_eligible"])
        unbound = self.fixture.prepare("programme", config=self.fixture.programme(title="Gerçek DJ sampler"))
        self.assertFalse(unbound["tracks"][0]["media_eligible"])
        self.assertEqual(unbound["tracks"][0]["reasons"], ["media_unbound", "copyright_expired"])

    def test_no_policy_default_and_future_window_opens_without_source_edit(self):
        binding = self.fixture.fixture.verified_binding()
        playlist = self.fixture.save(self.fixture.create(), binding)
        initial = self.fixture.prepare(playlist_id=playlist["_id"])
        self.assertEqual(initial["copyright_policy"], copyright.DEFAULT_POLICY)
        self.assertEqual(initial["copyright_policy_revision"], 0)
        self.assertIsNone(initial["copyright_policy_document_id"])
        saved = self.put(policy(rule(starts_at="2026-10-13T12:01:00Z")))
        self.assertFalse(self.fixture.prepare(playlist_id=playlist["_id"])["tracks"][0]["eligible"])
        before = self.store.sequence
        with patch.object(copyright, "utc_now", return_value=datetime(2026, 10, 13, 12, 1, tzinfo=timezone.utc)):
            self.assertTrue(self.fixture.prepare(playlist_id=playlist["_id"])["tracks"][0]["eligible"])
        self.assertEqual(self.store.sequence, before)
        self.assertEqual(api.get_document(self.store, self.principal, self.sid,
            "copyright_policy", saved["_id"])["_rev"], 1)

    def test_direct_media_get_and_context_refresh_policy_without_catalog_changes(self):
        binding = self.fixture.fixture.verified_binding()
        detail = desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"])
        context = api.desktop_context(self.store, self.principal, self.sid)
        self.assertEqual(detail["copyright_policy"], copyright.DEFAULT_POLICY)
        self.assertEqual(context["copyright_policy"], copyright.DEFAULT_POLICY)
        original_media = detail["media"]
        saved = self.put(policy(rule()))

        def check(expected):
            before = self.store.sequence
            detail = desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"])
            context = api.desktop_context(self.store, self.principal, self.sid)
            for response in (detail, context):
                self.assertEqual(response["copyright_policy"], expected["config"])
                self.assertEqual(response["copyright_policy_document_id"], expected["_id"])
                self.assertEqual(response["copyright_policy_revision"], expected["_rev"])
            self.assertEqual(detail["media"], original_media)
            self.assertEqual(self.store.sequence, before)

        check(saved)
        latest = self.put({**saved["config"], "restrict": False},
                          document_id=saved["_id"], expected_revision=saved["_rev"])
        check(latest)
        api.delete_document(self.store, self.principal, self.sid, "copyright_policy",
                            latest["_id"], latest["_rev"], str(uuid4()))
        for response in (desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"]),
                         api.desktop_context(self.store, self.principal, self.sid)):
            self.assertEqual(response["copyright_policy"], copyright.DEFAULT_POLICY)
            self.assertIsNone(response["copyright_policy_document_id"])
            self.assertEqual(response["copyright_policy_revision"], 0)

    def test_http_policy_crud_and_prepare_remain_authenticated_and_station_scoped(self):
        binding = self.fixture.fixture.verified_binding()
        playlist = self.fixture.save(self.fixture.create(), binding)
        server = BoundedHTTPServer(("127.0.0.1", 0), make_handler(self.store), max_connections=4)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 3)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)

        def request(action, body, token=None):
            connection = HTTPConnection(*server.server_address, timeout=5)
            headers = {"Content-Type": "application/json"}
            if token:
                headers["Authorization"] = "Bearer " + token
            connection.request("POST", "/v1/workflows/" + action, json.dumps(body), headers)
            response = connection.getresponse()
            result = response.status, json.loads(response.read())
            connection.close()
            return result

        token = self.fixture.fixture.identity["api_token"]
        body = {"station_id": self.sid, "kind": "copyright_policy", "name": "main",
                "config": policy(rule(ends_at="2026-10-13T11:59:00Z")), "idempotency_key": str(uuid4())}
        self.assertEqual(request("desktop/put", body)[0], 403)
        status, saved = request("desktop/put", body, token)
        self.assertEqual(status, 200)
        query = {"station_id": self.sid, "kind": "playlist", "playlist_id": playlist["_id"]}
        status, prepared = request("desktop-playout/prepare", query, token)
        self.assertEqual(status, 200)
        self.assertFalse(prepared["tracks"][0]["eligible"])
        self.assertTrue(prepared["tracks"][0]["media_eligible"])
        self.assertEqual(prepared["copyright_policy_document_id"], saved["_id"])
        self.assertNotIn(token, json.dumps(prepared))
        self.assertEqual(request("desktop-playout/prepare", {**query, "station_id": str(uuid4())}, token)[0], 404)


if __name__ == "__main__":
    unittest.main()
