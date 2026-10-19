#!/usr/bin/env python3
"""Native workflow discovery with real isolated HTTP, catalog ownership and CAS."""
from __future__ import annotations

import copy
from http.client import HTTPConnection
import json
from pathlib import Path
import sys
import threading
import unittest
from uuid import uuid4

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "database/SDBASE/src"))
import test_desktop_api as fixtures
from sdbase import desktop_workflows as bridge
from sdbase import clocks, catalog_admin, desktop_playlist, rotation
from sdbase.api_server import BoundedHTTPServer
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate, issue_endpoint_token
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError
from sdbase.workflow_api import ACTIONS


class DesktopWorkflowBridge(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal
        self.other = str(uuid4())
        self.store.put("stations", self.other, {"code":"OTHER", "name":"Another station", "timezone":"UTC", "status":"active"})

    def rows(self, collection, **fields):
        return bridge.records(self.store, self.principal, self.sid, collection, **fields)

    def category(self, name, station=None):
        return catalog_admin.create_category(self.store, self.principal, station or self.sid, str(uuid4()), name)

    def test_catalogue_is_exact_existing_specialist_allowlist_and_read_only(self):
        sequence = self.store.sequence
        result = bridge.catalogue(self.store, self.principal, self.sid)
        self.assertEqual(result["sequence"], sequence)
        self.assertEqual(self.store.sequence, sequence)
        actual = {action["id"]:action for family in result["families"] for action in family["actions"]}
        expected = {name for name in ACTIONS if name.split("/")[0] in bridge.FAMILIES}
        self.assertEqual(set(actual), expected)
        self.assertEqual(len(result["families"]), len(bridge.FAMILIES))
        self.assertEqual(len(actual), len(expected))
        for name, action in actual.items():
            self.assertEqual(action["path"], "/v1/workflows/" + name)
            self.assertIsNotNone(ACTIONS[name])
            self.assertTrue(action["label"])
            self.assertNotIn("store", action["request_schema"]["properties"])
            self.assertNotIn("principal", action["request_schema"]["properties"])
            self.assertFalse(any(field["name"].startswith("_") for field in action["fields"]))
        destination = ROOT / "build/integration-completion/backend/catalogue-example.json"
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(result, ensure_ascii=False, indent=2))

    def test_domain_schemas_keep_real_rules_and_family_specific_version_identity(self):
        result = bridge.catalogue(self.store, self.principal, self.sid)
        actions = {a["id"]:a for f in result["families"] for a in f["actions"]}
        def field(action, name):
            return actions[action]["request_schema"]["properties"][name]
        self.assertEqual(field("clocks/put-slot", "version_id")["x-radios-reference"], "clock_template_versions")
        self.assertEqual(field("playlists/publish", "version_id")["x-radios-reference"], "playlist_versions")
        self.assertEqual(field("smart-blocks/publish", "version_id")["x-radios-reference"], "smart_block_versions")
        self.assertEqual(field("carts/put-cut", "version_id")["x-radios-reference"], "versions")
        revision = field("smart-blocks/publish", "expected_block_revision")
        self.assertEqual((revision["x-radios-reference"], revision["x-radios-reference-value"]), ("smart_blocks", "_rev"))
        self.assertEqual([branch["title"] for branch in field("smart-blocks/create", "rule_tree")["oneOf"]],
                         ["Alan koşulu", "Bütün koşullar", "Koşullardan biri", "Koşulun tersi"])
        self.assertIn("payload", field("clocks/put-slot", "entry_template")["properties"])
        self.assertIn("artist_gap_minutes", field("rotation/put-rule", "config")["properties"])
        self.assertEqual(field("smart-blocks/create", "selection_order")["enum"], ["ordered", "shuffle"])
        self.assertEqual(field("playlists/put-entry", "source_kind")["enum"], ["event", "smart_block", "podcast_feed"])
        self.assertEqual(field("production/create-placement", "kind")["enum"], ["sweeper", "voice_track"])
        self.assertEqual(field("metadata/review", "decision")["enum"], ["approved", "rejected"])
        self.assertEqual(field("bulk/apply", "idempotency_key")["format"], "uuid")
        self.assertNotIn("default", field("smart-blocks/create", "target_duration_ms"))

    def test_station_record_selection_preserves_actual_identity_revision_and_isolation(self):
        first, foreign = self.category("Yerel"), self.category("Foreign", self.other)
        sequence = self.store.sequence
        result = self.rows("categories")
        self.assertEqual([row["_id"] for row in result["records"]], [first["_id"]])
        self.assertEqual(result["records"][0]["_rev"], first["_rev"])
        self.assertEqual(result["records"][0]["label"], "Yerel")
        self.assertNotIn(foreign["_id"], json.dumps(result))
        self.assertEqual(self.store.sequence, sequence)
        destination = ROOT / "build/integration-completion/backend/records-example.json"
        destination.write_text(json.dumps(result, ensure_ascii=False, indent=2))

    def test_indirect_clock_version_slot_and_exception_are_owned_through_real_parent(self):
        own = clocks.create_clock_template(self.store, self.principal, self.sid, code="OWN", name="Local clock")
        foreign = clocks.create_clock_template(self.store, self.principal, self.other, code="OTHER", name="Foreign clock")
        entry = {"event_kind":"marker", "payload":{"label":"Haber"}, "duration_kind":"none"}
        slot = clocks.put_clock_slot(self.store, self.principal, self.sid, own["draft_version_id"], ordinal=0, offset_ms=0, entry_template=entry)
        clocks.put_clock_slot(self.store, self.principal, self.other, foreign["draft_version_id"], ordinal=0, offset_ms=0, entry_template=entry)
        self.assertEqual([row["_id"] for row in self.rows("clock_slots")["records"]], [slot["_id"]])
        versions = self.rows("clock_template_versions")["records"]
        self.assertEqual([row["_id"] for row in versions], [own["draft_version_id"]])
        template = self.store.get("clock_templates", own["template_id"])
        version = self.store.get("clock_template_versions", own["draft_version_id"])
        clocks.publish_clock_version(self.store, self.principal, self.sid, version["_id"], expected_version_revision=version["_rev"], expected_template_revision=template["_rev"])
        assignment = clocks.assign_clock(self.store, self.principal, self.sid, template_version_id=version["_id"], weekday=1, local_hour=12, starts_on="2026-10-12")
        exception = clocks.set_clock_exception(self.store, self.principal, self.sid, assignment["_id"], "2026-10-12", action="skip")
        self.assertEqual([row["_id"] for row in self.rows("clock_exceptions")["records"]], [exception["_id"]])

    def test_cursor_pages_once_and_is_fenced_to_scope_and_committed_sequence(self):
        ids = {self.category(str(i))["_id"] for i in range(4)}
        first = self.rows("categories", limit=2)
        self.assertIsNotNone(first["next_cursor"])
        second = self.rows("categories", limit=2, cursor=first["next_cursor"])
        self.assertIsNone(second["next_cursor"])
        self.assertEqual(ids, {row["_id"] for result in (first, second) for row in result["records"]})
        with self.assertRaises(StoreError): self.rows("rotation_rules", cursor=first["next_cursor"])
        self.category("Concurrent commit")
        with self.assertRaises(Conflict): self.rows("categories", cursor=first["next_cursor"])
        for invalid in ("%%", "x" * 2049, 1, "e30="):
            with self.subTest(value=str(invalid)[:20]), self.assertRaises(StoreError):
                self.rows("categories", cursor=invalid)

    def test_unsafe_collections_limits_and_hidden_credentials_cannot_be_selected(self):
        for collection in ("tokens", "permissions", "_meta", "desktop_mutations", "../../config", [], None):
            with self.subTest(collection=collection), self.assertRaises(StoreError): self.rows(collection)
        for limit in (0, 201, True, 1.5):
            with self.assertRaises(StoreError): self.rows("categories", limit=limit)
        category = self.category("Visible")
        body = {k:v for k,v in category.items() if k not in {"_id", "_rev"}}
        body.update(secret="do-not-display", password="do-not-display", token="do-not-display", config={"secret":"hidden"})
        self.store.put("categories", category["_id"], body, expected_revision=category["_rev"])
        output = self.rows("categories")
        self.assertNotIn("do-not-display", json.dumps(output))
        self.assertNotIn("config", output["records"][0])

    def local_user(self, permissions=("view",)):
        from sdbase.access_admin import create_station_user
        result = create_station_user(self.store, self.principal, self.sid, "reader" + str(uuid4())[:8], "Reader", list(permissions))
        return result["token"], authenticate(self.store, result["token"])

    def test_readonly_discovery_does_not_grant_mutation_or_global_resource_visibility(self):
        _, viewer = self.local_user()
        bridge.catalogue(self.store, viewer, self.sid)
        bridge.records(self.store, viewer, self.sid, "categories")
        with self.assertRaises(Forbidden):
            rotation.put_rotation_rule(self.store, viewer, self.sid, name="Not allowed", config={})
        with self.assertRaises(Forbidden): bridge.catalogue(self.store, viewer, self.other)
        with self.assertRaises(Forbidden): bridge.records(self.store, viewer, self.sid, "field_definitions")
        _, local_manager = self.local_user(("view", "manage"))
        with self.assertRaises(Forbidden): bridge.records(self.store, local_manager, self.sid, "people")

    def test_authorized_stations_and_managed_users_are_display_only(self):
        stations = self.rows("stations")["records"]
        self.assertEqual({row["_id"] for row in stations}, {self.sid, self.other})
        _, viewer = self.local_user()
        own = bridge.records(self.store, viewer, self.sid, "stations")["records"]
        self.assertEqual([row["_id"] for row in own], [self.sid])
        with self.assertRaises(Forbidden): bridge.records(self.store, viewer, self.sid, "users")
        users = self.rows("users")["records"]
        self.assertIn(viewer.user_id, {row["_id"] for row in users})
        self.assertNotIn("token_hash", json.dumps(users))
        self.assertNotIn("expires_at", json.dumps(users))

    def test_exact_record_readback_cannot_cross_station_or_ignore_new_revision(self):
        own = self.category("Own")
        foreign = self.category("Foreign", self.other)
        self.assertEqual(self.rows("categories", record_id=own["_id"])["records"][0]["_rev"], own["_rev"])
        self.assertEqual(self.rows("categories", record_id=foreign["_id"])["records"], [])
        changed = catalog_admin.update_category(self.store, self.principal, self.sid, own["_id"], expected_revision=own["_rev"], name="Updated")
        self.assertEqual(self.rows("categories", record_id=own["_id"])["records"][0]["_rev"], changed["_rev"])
        for bad in ("missing", 123, [], "../config"):
            with self.assertRaises(StoreError): self.rows("categories", record_id=bad)


    def test_real_http_structured_smartblock_clock_and_recording_fields_are_accepted(self):
        post = self.http()
        status, block = post("smart-blocks/create", {"station_id":self.sid, "code":"native-complex", "name":"Native rule groups",
            "rule_tree":{"all":[{"field":"genre", "op":"contains", "value":"Rock"},
                                  {"not":{"field":"title", "op":"eq", "value":"Excluded"}}]},
            "target_duration_ms":180000, "selection_order":"shuffle", "selection_mode":"dynamic"})
        self.assertEqual(status, 200)
        status, versions = post("smart-blocks/versions", {"station_id":self.sid, "block_id":block["block_id"]})
        self.assertEqual(status, 200)
        self.assertIn("Excluded", json.dumps(versions))
        status, template = post("clocks/create", {"station_id":self.sid, "code":"native-hour", "name":"Native clock"})
        self.assertEqual(status, 200)
        status, slot = post("clocks/put-slot", {"station_id":self.sid, "version_id":template["draft_version_id"], "ordinal":0,
            "offset_ms":0, "slot_kind":"fixed", "entry_template":{"event_kind":"marker", "payload":{"label":"Haber"}, "duration_kind":"none"}})
        self.assertEqual(status, 200)
        status, reread = post("desktop-workflows/records", {"station_id":self.sid, "collection":"clock_slots", "record_id":slot["_id"]})
        self.assertEqual(status, 200)
        self.assertEqual(reread["records"][0]["_id"], slot["_id"])
        from sdbase import catalog
        item = catalog.create_item(self.store, self.principal, self.sid, kind="voice_track", title="Yeni kayıt hedefi")
        association = self.store.lookup("station_items", "station_item", self.sid, item["_id"])[0]
        catalog.set_station_eligibility(self.store, self.principal, self.sid, item["_id"], True, expected_revision=association["_rev"])
        destination = catalog.create_version(self.store, self.principal, self.sid, item["_id"], label="Boş kayıt taslağı")
        status, recording = post("recording/create", {"station_id":self.sid, "endpoint_id":self.fixture.identity["endpoint_id"],
            "code":"native-recording", "name":"Zamanlı mikrofon kaydı", "source":{"kind":"input", "input_id":"microphone"},
            "destination_version_id":destination["_id"], "schedule":{"weekdays":[1,2,3,4,5], "starts_on":"2026-10-12"},
            "start_trigger":{"kind":"clock", "local_time":"12:00"}, "end_trigger":{"kind":"duration", "duration_ms":60000}})
        self.assertEqual(status, 200)
        status, reread = post("desktop-workflows/records", {"station_id":self.sid, "collection":"recording_jobs", "record_id":recording["_id"]})
        self.assertEqual(status, 200)
        self.assertEqual(reread["records"][0]["name"], "Zamanlı mikrofon kaydı")

    def test_preview_bound_bulk_apply_receipt_replay_and_revision_safe_undo(self):
        post = self.http(); binding = self.fixture.verified_binding()
        item_id = self.store.get("versions", binding["version_id"])["item_id"]
        preview_args = {"station_id":self.sid, "item_ids":[item_id], "operations":{"title":{"mode":"set", "value":"İncelenen toplu başlık"}}}
        status, preview = post("bulk/preview", preview_args)
        self.assertEqual(status, 200)
        revisions = {row["item_id"]:row["item_revision"] for row in preview["items"]}
        body = preview_args | {"expected_revisions":revisions, "idempotency_key":str(uuid4())}
        status, applied = post("bulk/apply", body)
        self.assertEqual(status, 200)
        sequence = self.store.sequence
        status, replayed = post("bulk/apply", body)
        self.assertEqual(status, 200)
        self.assertEqual(replayed, applied)
        self.assertEqual(self.store.sequence, sequence)
        status, undone = post("bulk/undo", {"station_id":self.sid, "job_id":applied["_id"], "expected_job_revision":applied["_rev"]})
        self.assertEqual(status, 200)
        self.assertEqual(undone["status"], "undone")

    def test_preview_bound_station_clone_real_endpoint_and_activation(self):
        post = self.http()
        parameters = {"source_station_id":self.sid, "new_code":"NATIVE-CLONE", "new_name":"Yeni yayın istasyonu", "idempotency_key":str(uuid4())}
        status, preview = post("stations/clone-preview", parameters)
        self.assertEqual(status, 200)
        status, applied = post("stations/clone-apply", parameters | {"preview_hash":preview["snapshot_hash"]})
        self.assertEqual(status, 200)
        clone = applied["station"]
        self.assertEqual(clone["status"], "disabled")
        self.assertIn(clone["_id"], {row["_id"] for row in self.rows("stations")["records"]})
        status, endpoint = post("stations/endpoint-create", {"station_id":clone["_id"], "code":"main", "name":"Yeni yayın ucu"})
        self.assertEqual(status, 200)
        status, selected = post("desktop-workflows/records", {"station_id":clone["_id"], "collection":"endpoints", "record_id":endpoint["_id"]})
        self.assertEqual(status, 200)
        self.assertEqual(selected["records"][0]["_id"], endpoint["_id"])
        status, active = post("stations/activate-clone", {"station_id":clone["_id"], "expected_revision":clone["_rev"]})
        self.assertEqual(status, 200)
        self.assertEqual(active["status"], "active")

    def test_existing_catalogue_aliases_preserve_real_prerequisite_decisions(self):
        post = self.http()
        status, item = post("catalog/item-create", {"station_id":self.sid, "kind":"voice_track", "title":"Kayıt için yeni içerik"})
        self.assertEqual(status, 200)
        status, version = post("catalog/version-create", {"station_id":self.sid, "item_id":item["_id"], "label":"İlk taslak sürüm"})
        self.assertEqual(status, 200)
        association = self.store.lookup("station_items", "station_item", self.sid, item["_id"])[0]
        sequence = self.store.sequence
        self.assertEqual(post("catalog/item-eligibility", {"station_id":self.sid, "item_id":item["_id"], "eligible":"false", "expected_revision":association["_rev"]})[0], 422)
        self.assertEqual(self.store.sequence, sequence)
        status, eligible = post("catalog/item-eligibility", {"station_id":self.sid, "item_id":item["_id"], "eligible":True, "expected_revision":association["_rev"]})
        self.assertEqual(status, 200)
        self.assertTrue(eligible["eligible"])
        status, active = post("catalog/item-lifecycle", {"station_id":self.sid, "item_id":item["_id"], "target":"active", "expected_revision":item["_rev"]})
        self.assertEqual(status, 200)
        self.assertEqual(active["lifecycle"], "active")
        status, approved = post("catalog/version-approve", {"station_id":self.sid, "version_id":version["_id"], "expected_revision":version["_rev"]})
        self.assertEqual(status, 200)
        self.assertEqual(approved["status"], "approved")
        status, shared = post("catalog/item-share", {"station_id":self.other, "item_id":item["_id"]})
        self.assertEqual(status, 200)
        self.assertFalse(shared["eligible"])
        status, plan = post("plans/create", {"station_id":self.sid, "service_date":"2026-10-16"})
        self.assertEqual(status, 200)
        self.assertIsNotNone(self.store.get("plans", plan["plan_id"]))

    def http(self):
        parent = make_handler(self.store)
        class Handler(parent):
            def log_message(self, *_args): pass
        server = BoundedHTTPServer(("127.0.0.1", 0), Handler, request_timeout=3)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        self.addCleanup(worker.join, 3)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        def post(action, data=None, token=None):
            connection = HTTPConnection("127.0.0.1", server.server_port, timeout=5)
            headers = {"Content-Type":"application/json"}
            if token is not False: headers["Authorization"] = "Bearer " + (token or self.fixture.identity["api_token"])
            try:
                connection.request("POST", "/v1/workflows/" + action, json.dumps(data or {"station_id":self.sid}), headers)
                response = connection.getresponse()
                return response.status, json.loads(response.read())
            finally:
                connection.close()
        return post

    def test_real_http_discovery_records_mutation_cas_reopen_and_atomic_errors(self):
        post = self.http()
        status, catalogue = post("desktop-workflows/catalogue")
        self.assertEqual(status, 200)
        fields = next(a for f in catalogue["families"] for a in f["actions"] if a["id"] == "rotation/put-rule")["request_schema"]["properties"]
        self.assertIn("artist_gap_minutes", fields["config"]["properties"])
        status, created = post("rotation/put-rule", {"station_id":self.sid, "name":"GUI-bound real rule", "config":{"artist_gap_minutes":30}})
        self.assertEqual(status, 200)
        status, records = post("desktop-workflows/records", {"station_id":self.sid, "collection":"rotation_rules"})
        self.assertEqual(status, 200)
        self.assertEqual(records["records"][0]["_id"], created["_id"])
        status, updated = post("rotation/put-rule", {"station_id":self.sid, "name":"Updated", "config":{"artist_gap_minutes":15}, "rule_id":created["_id"], "expected_revision":created["_rev"]})
        self.assertEqual(status, 200)
        sequence = self.store.sequence
        status, _ = post("rotation/put-rule", {"station_id":self.sid, "name":"Stale", "config":{}, "rule_id":created["_id"], "expected_revision":created["_rev"]})
        self.assertEqual(status, 409)
        self.assertEqual(self.store.sequence, sequence)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(reopened.get("rotation_rules", updated["_id"])["config"]["artist_gap_minutes"], 15)
        self.assertEqual(post("desktop-workflows/records", {"station_id":self.sid, "collection":"tokens"})[0], 422)
        self.assertEqual(post("desktop-workflows/catalogue", token=False)[0], 403)
        token, _ = self.local_user()
        self.assertEqual(post("desktop-workflows/catalogue", token=token)[0], 200)
        self.assertEqual(post("rotation/put-rule", {"station_id":self.sid, "name":"Denied", "config":{}}, token=token)[0], 403)
        endpoint_token = issue_endpoint_token(self.store, self.principal, self.sid, self.fixture.identity["endpoint_id"])
        self.assertEqual(post("desktop-workflows/catalogue", token=endpoint_token)[0], 403)


if __name__ == "__main__":
    unittest.main()
