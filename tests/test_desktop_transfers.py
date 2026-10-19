#!/usr/bin/env python3
"""Real private HTTP file adapters: exact bytes, drafts, approval, loss reports."""
from __future__ import annotations
import base64
import csv
from http.client import HTTPConnection
import io
import json
from pathlib import Path
import stat
import sys
import unittest
from uuid import uuid4

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "database/SDBASE/src"))
import test_desktop_api as fixtures
import test_desktop_workflow_bridge as bridge_fixtures
from sdbase import catalog, desktop_transfers as transfers, plans, rights, station_outputs
from sdbase.mairlist31_csv import FIELDS
from sdbase.schema import open_radio_store
from sdbase.storage import StoreError


def encode(raw):
    return base64.b64encode(raw).decode("ascii")


class DesktopTransfers(unittest.TestCase):
    http = bridge_fixtures.DesktopWorkflowBridge.http
    local_user = bridge_fixtures.DesktopWorkflowBridge.local_user

    def setUp(self):
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal

    def csv_bytes(self, title="Türkçe, müzik", external="track-1"):
        fields = {field:"" for field in FIELDS}
        fields.update(external_id=external, file_path="/kaynak/İstanbul.wav", title=title, artists="Sanatçı", duration="0.2")
        output = io.StringIO(newline="")
        csv.writer(output).writerow([fields[field] for field in FIELDS])
        return output.getvalue().encode("utf-8-sig")

    def ready_media(self):
        binding = self.fixture.verified_binding()
        version = self.store.get("versions", binding["version_id"])
        item = self.store.get("items", version["item_id"])
        catalog.approve_version(self.store, self.principal, self.sid, version["_id"], expected_revision=version["_rev"])
        catalog.set_lifecycle(self.store, self.principal, self.sid, item["_id"], "active", expected_revision=item["_rev"])
        association = self.store.lookup("station_items", "station_item", self.sid, item["_id"])[0]
        catalog.set_station_eligibility(self.store, self.principal, self.sid, item["_id"], True, expected_revision=association["_rev"])
        rights.grant(self.store, self.principal, self.sid, version["_id"], territory="*", output_id="*",
            starts_at="2026-01-01T00:00:00Z", ends_at="2028-01-01T00:00:00Z", source_ref="Owned private acceptance recording")
        return binding, self.store.get("items", item["_id"])

    def test_csv_preview_exact_apply_replay_reopen_and_changed_source_rejection(self):
        post = self.http(); source = self.csv_bytes()
        inputs = {"station_id":self.sid, "source_base64":encode(source), "source_namespace":"native-csv"}
        sequence = self.store.sequence
        status, preview = post("transfers/mairlist-preview", inputs)
        self.assertEqual(status, 200)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(preview["row_count"], 1)
        self.assertEqual(preview["entries"][0]["raw_fields"]["title"], "Türkçe, müzik")
        body = inputs | {"expected_source_sha256":preview["source_sha256"], "idempotency_key":str(uuid4())}
        status, saved = post("transfers/mairlist-apply", body)
        self.assertEqual(status, 200)
        sequence = self.store.sequence
        status, replayed = post("transfers/mairlist-apply", body)
        self.assertEqual(status, 200)
        self.assertTrue(replayed["replayed"])
        self.assertEqual(replayed["job_id"], saved["job_id"])
        self.assertEqual(self.store.sequence, sequence)
        changed = body | {"source_base64":encode(self.csv_bytes("Different source")), "idempotency_key":str(uuid4())}
        self.assertEqual(post("transfers/mairlist-apply", changed)[0], 409)
        self.assertEqual(self.store.sequence, sequence)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertIsNotNone(reopened.get("import_jobs", saved["job_id"]))
        items = reopened.scan("items")
        self.assertEqual(len(items), 1)
        self.assertEqual(items[0]["lifecycle"], "draft")
        self.assertFalse(reopened.scan("station_items")[0]["eligible"])
        self.assertEqual(reopened.scan("versions")[0]["status"], "draft")
        self.assertFalse(list(self.store.root.glob(".native-transfer-*")))

    def test_radioboss_explicit_binding_import_replay_real_approval_and_export(self):
        post = self.http(); binding, item = self.ready_media()
        source = b"#EXTM3U\n#EXTINF:1,Source title\n/source/song.wav\n"
        common = {"station_id":self.sid, "source_base64":encode(source)}
        status, preview = post("transfers/radioboss-preview", common)
        self.assertEqual(status, 200)
        body = common | {"source_namespace":"native-m3u8", "expected_source_sha256":preview["source_sha256"],
            "idempotency_key":str(uuid4()), "service_date":"2026-10-15", "start_at":"2026-10-15T12:00:00Z",
            "path_bindings":{"/source/song.wav":{"version_id":binding["version_id"], "media_file_id":binding["media_file_id"]}}}
        status, saved = post("transfers/radioboss-apply", body)
        self.assertEqual(status, 200)
        self.assertFalse(saved["plan_approved"])
        self.assertEqual(saved["audio_files_imported"], 0)
        sequence = self.store.sequence
        status, replayed = post("transfers/radioboss-apply", body)
        self.assertEqual(status, 200)
        self.assertTrue(replayed["replayed"])
        self.assertEqual(saved["plan_id"], replayed["plan_id"])
        self.assertEqual(self.store.sequence, sequence)
        output = {"station_id":self.sid, "plan_id":saved["plan_id"], "revision_id":saved["draft_revision_id"],
            "paths":[{"media_file_id":binding["media_file_id"], "path":"/hedef/İstanbul.wav"}]}
        self.assertEqual(post("transfers/radioboss-export", output)[0], 409)
        station_outputs.create_station_output(self.store, self.principal, self.sid, self.fixture.identity["endpoint_id"], "MAIN", geo_codes=["LOCAL"])
        plan = self.store.get("plans", saved["plan_id"])
        # The GUI hub (Yayın planları › Onayla) approves through the workflow.
        approval = {"station_id":self.sid, "plan_id":plan["_id"], "revision_id":saved["draft_revision_id"]}
        self.assertEqual(post("plans/approve", approval | {"expected_plan_revision":plan["_rev"] + 1})[0], 409)
        status, approved = post("plans/approve", approval | {"expected_plan_revision":plan["_rev"]})
        self.assertEqual(status, 200)
        self.assertEqual(self.store.get("plans", plan["_id"])["current_revision_id"], saved["draft_revision_id"])
        status, exported = post("transfers/radioboss-export", output)
        self.assertEqual(status, 200)
        self.assertTrue(exported["content"].startswith("#EXTM3U"))
        # ... and queues the approved snapshot for the station's endpoint.
        status, queued = post("plans/distribute", {"station_id":self.sid, "endpoint_id":self.fixture.identity["endpoint_id"],
                                                   "revision_id":saved["draft_revision_id"]})
        self.assertEqual(status, 200, queued)
        self.assertIn("/hedef/İstanbul.wav", exported["content"])
        self.assertEqual(exported["row_count"], 1)
        self.assertTrue(exported["losses"])
        self.assertEqual(exported["filename"], "radios-radioboss.m3u8")
        self.assertFalse(list(self.store.root.glob(".native-transfer-*")))

    def test_mairlist_export_uses_current_real_catalogue_identity_and_reports_losses(self):
        post = self.http(); binding, item = self.ready_media()
        body = {"station_id":self.sid, "items":[{"item_id":item["_id"], "version_id":binding["version_id"],
            "media_file_id":binding["media_file_id"], "expected_item_revision":item["_rev"], "path":"/hedef/İstanbul, müzik.wav"}]}
        status, output = post("transfers/mairlist-export", body)
        self.assertEqual(status, 200)
        rows = list(csv.reader(io.StringIO(output["content"])))
        self.assertEqual(len(rows), 1)
        self.assertEqual(len(rows[0]), 20)
        self.assertEqual(rows[0][1], item["_id"])
        self.assertEqual(rows[0][2], "/hedef/İstanbul, müzik.wav")
        self.assertEqual(output["exported_item_ids"], [item["_id"]])
        self.assertTrue(output["losses"])
        self.assertFalse(output["skipped"])
        self.assertEqual(output["filename"], "radios-mairlist.csv")
        stale = json.loads(json.dumps(body)); stale["items"][0]["expected_item_revision"] += 1
        status, rejected = post("transfers/mairlist-export", stale)
        self.assertEqual(status, 200)
        self.assertEqual(rejected["row_count"], 0)
        self.assertEqual(rejected["skipped"][0]["code"], "selected_catalog_view_is_stale")

    def test_exact_one_mib_file_fits_http_body_and_oversize_bytes_are_rejected(self):
        post = self.http()
        prefix = b"#EXTM3U\n#EXTINF:1,Song\n/source/song.wav\n"
        remaining = transfers.MAX_SOURCE_BYTES - len(prefix)
        chunk = b" " * 8191 + b"\n"
        source = prefix + chunk * (remaining // len(chunk)) + b" " * (remaining % len(chunk))
        self.assertEqual(len(source), 1048576)
        body = {"station_id":self.sid, "source_base64":encode(source)}
        self.assertLess(len(json.dumps(body).encode()), 2 * 1024 * 1024)
        sequence = self.store.sequence
        status, preview = post("transfers/radioboss-preview", body)
        self.assertEqual(status, 200)
        self.assertEqual(preview["source_bytes"], len(source))
        self.assertEqual(preview["row_count"], 1)
        self.assertEqual(post("transfers/radioboss-preview", body | {"source_base64":encode(source+b" ")})[0], 422)
        self.assertEqual(self.store.sequence, sequence)
        self.assertFalse(list(self.store.root.glob(".native-transfer-*")))

    def test_exclusive_private_sources_invalid_bytes_permissions_and_no_server_path(self):
        raw = self.csv_bytes()
        with transfers._source(self.store, encode(raw)) as source:
            self.assertEqual(source.read_bytes(), raw)
            self.assertEqual(stat.S_IMODE(source.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(source.parent.stat().st_mode), 0o700)
        self.assertFalse(source.exists())
        for invalid in ("", "%%%", "../credentials.json", "Türkçe", 1, None):
            with self.subTest(value=invalid), self.assertRaises(StoreError):
                with transfers._source(self.store, invalid): pass
        post = self.http(); token, _ = self.local_user()
        body = {"station_id":self.sid, "source_base64":encode(raw), "source_namespace":"native"}
        self.assertEqual(post("transfers/mairlist-preview", body, token=token)[0], 403)
        self.assertEqual(post("transfers/mairlist-preview", body | {"path":"/etc/passwd"})[0], 422)
        self.assertFalse(list(self.store.root.glob(".native-transfer-*")))


if __name__ == "__main__": unittest.main()
