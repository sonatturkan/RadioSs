"""Isolated real-byte traffic reservations, local receipts and immutable reports."""
import copy
from datetime import datetime, timezone
from http.client import HTTPConnection
import json
import math
from pathlib import Path
import threading
import struct
import unittest
from unittest.mock import patch
from uuid import uuid4
import wave

import test_desktop_api as fixtures
from sdbase import desktop_advert as advert, desktop_api as api, desktop_media, rights
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.catalog import NotFound, share_item
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError


class DesktopAdvert(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal

    @staticmethod
    def fields(**changes):
        return dict(advertiser="Actual client", owner="Sales", reference="Invoice metadata", start_date="2040-10-06",
            end_date="2040-10-06", target_hint="Local MAIN", spot_title="Display-only spot", version_label="Display label",
            duration_seconds=30, planned_count=10, price=12.5, currency="TRY", contact="", slot="", capacity_seconds=0,
            intro_seconds=0, outro_seconds=0, announcement_seconds=0, competition="", position=0, fallback=0,
            skip_empty=True, **changes)

    @staticmethod
    def break_config(**changes):
        result = {"date": "2040-10-06", "output_id": "MAIN", "region_id": "LOCAL", "time": "12:00",
            "capacity_seconds": 10, "tolerance_seconds": 0, "fixed_seconds": 0, "opener_seconds": 0,
            "closer_seconds": 0, "empty_mode": 0, "start_mode": 0, "wait_seconds": 5}
        result.update(changes)
        return result

    def media(self, kind="advertisement", title="Measured advert", sample_rate=8000, frames=1600):
        if sample_rate != 8000 or frames != 1600:
            imports = Path(self.fixture.temp.name) / "imports"
            imports.mkdir(exist_ok=True, mode=0o700); self.store.desktop_import_root = imports
            path = imports / (str(uuid4()) + ".wav")
            samples = [round(10000 * math.sin(2 * math.pi * 400 * index / sample_rate)) for index in range(frames)]
            with wave.open(str(path), "wb") as output:
                output.setnchannels(1); output.setsampwidth(2); output.setframerate(sample_rate)
                output.writeframes(struct.pack("<" + str(frames) + "h", *samples))
            path.chmod(0o600)
            normalized = [sample / 32768 for sample in samples]
            hop = math.ceil(frames / 2048); waveform = []
            for start in range(0, frames, hop):
                waveform.extend((min(normalized[start:start + hop]), max(normalized[start:start + hop])))
            analysis = {"sha256": advert.hashlib.sha256(path.read_bytes()).hexdigest(), "size_bytes": path.stat().st_size,
                "duration_ms": round(frames * 1000 / sample_rate), "sample_rate": sample_rate, "channels": 1,
                "codec": "pcm_s16le", "format": "wav", "bitrate": sample_rate * 16, "peak": max(map(abs, normalized)),
                "rms": math.sqrt(sum(sample * sample for sample in normalized) / frames), "clipped": False, "clipped_samples": 0,
                "sample_frames": frames, "waveform_frames_per_bin": hop, "waveform": waveform}
            detail = desktop_media.import_media(self.store, self.principal, self.sid, path.name, "actual.wav", str(uuid4()),
                {"title": title, "kind": kind}, analysis)
            return {"media_id": detail["media"]["_id"], "version_id": detail["version"]["_id"],
                    "media_file_id": detail["version"]["media_file_id"], "media_sha256": detail["version"]["sha256"]}
        original = desktop_media.import_media
        def importing(*args, **kwargs):
            values = list(args)
            values[6] = dict(values[6], kind=kind, title=title)
            return original(*values, **kwargs)
        with patch.object(desktop_media, "import_media", importing):
            return self.fixture.verified_binding()

    def campaign(self, **fields):
        config = self.fields()
        config.update(fields)
        return api.save_campaign(self.store, self.principal, self.sid, "Campaign", config, str(uuid4()))

    def bound_campaign(self, approved=False, **fields):
        binding = self.media()
        campaign = self.campaign(**fields)
        campaign = advert.bind(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], idempotency_key=str(uuid4()), **binding)
        if approved:
            result = advert.spot_approve(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], str(uuid4()))
            campaign = advert.campaign_state(self.store, self.principal, self.sid, campaign["_id"], result["campaign_revision"], "approve", str(uuid4()))
            rights.grant(self.store, self.principal, self.sid, binding["version_id"], territory="LOCAL", output_id="MAIN",
                         starts_at=campaign["starts_at"], ends_at=campaign["ends_at"], source_ref="Isolated fixture explicit operator permission")
        return campaign, binding

    def output(self):
        return advert.output_create(self.store, self.principal, self.sid, "MAIN", str(uuid4()))

    def saved_break(self, config=None, **extra):
        return advert.break_save(self.store, self.principal, self.sid, "Actual dated break", config or self.break_config(), str(uuid4()), **extra)

    def reservation(self, config=None):
        self.output()
        campaign, binding = self.bound_campaign(approved=True)
        document = self.saved_break(config)
        preview = advert.preview(self.store, self.principal, self.sid, document["_id"], document["_rev"])
        result = advert.reserve(self.store, self.principal, self.sid, document["_id"], document["_rev"], preview["preview_hash"], preview["plan_revision"], str(uuid4()))
        return result["document"], campaign, binding, result

    def start(self, document):
        prepared = advert.prepare(self.store, self.principal, self.sid, document["_id"], document["_rev"])
        run = advert.run_start(self.store, self.principal, self.sid, document["_id"], document["_rev"], prepared["manifest_hash"], str(uuid4()))
        return run

    @staticmethod
    def event(span, state="started", frames=1, occurrence=1, **extra):
        return {"event_key": str(uuid4()), "entry_id": span["entry_id"], "occurrence": occurrence,
            "state": state, "consumed_frames": frames, "occurred_at": "2040-10-06T12:00:00+00:00", "main_sink": "isolated-null-main", **extra}

    def append(self, run, events, **extra):
        return advert.run_event(self.store, self.principal, self.sid, run["run_id"], run["revision"], str(uuid4()), events, **extra)

    def report(self, **extra):
        return advert.report_create(self.store, self.principal, self.sid, "2040-10-06", "2040-10-06", str(uuid4()), **extra)

    def test_empty_overview_readonly_and_output_explicit_replay(self):
        sequence = self.store.sequence
        snapshot = advert.overview(self.store, self.principal, self.sid)
        self.assertEqual(snapshot["campaigns"], [])
        self.assertEqual(snapshot["outputs"], [])
        self.assertEqual(snapshot["breaks"], [])
        self.assertEqual(self.store.sequence, sequence)
        key = str(uuid4())
        output = advert.output_create(self.store, self.principal, self.sid, "MAIN", key)
        sequence = self.store.sequence
        self.assertEqual(advert.output_create(self.store, self.principal, self.sid, "MAIN", key), output)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(snapshot["outputs"], [])
        self.assertEqual(output["geo_codes"], ["LOCAL"])
        self.assertEqual(advert.overview(self.store, self.principal, self.sid)["outputs"][0]["_id"], output["_id"])
        with self.assertRaises(Conflict):
            advert.output_create(self.store, self.principal, self.sid, "OTHER", key)

    def test_real_spot_approval_atomic_catalog_obligation_and_state_cas(self):
        campaign, binding = self.bound_campaign()
        self.assertFalse(self.store.scan("spots"))
        key = str(uuid4())
        result = advert.spot_approve(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], key)
        sequence = self.store.sequence
        self.assertEqual(advert.spot_approve(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], key), result)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store.get("versions", binding["version_id"])["status"], "approved")
        library = self.store.get("desktop_media_library", binding["media_id"])
        self.assertTrue(self.store.lookup("station_items", "station_item", self.sid, library["item_id"])[0]["eligible"])
        obligation = self.store.get("obligations", result["obligation_ids"][0])
        self.assertEqual(obligation["required_plays"], 10)
        approved = advert.campaign_state(self.store, self.principal, self.sid, campaign["_id"], result["campaign_revision"], "approve", str(uuid4()))
        paused = advert.campaign_state(self.store, self.principal, self.sid, campaign["_id"], approved["_rev"], "pause", str(uuid4()))
        with self.assertRaises(Conflict):
            advert.campaign_state(self.store, self.principal, self.sid, campaign["_id"], approved["_rev"], "resume", str(uuid4()))
        resumed = advert.campaign_state(self.store, self.principal, self.sid, campaign["_id"], paused["_rev"], "resume", str(uuid4()))
        self.assertEqual(resumed["status"], "approved")
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_binding_rejects_unverified_kind_foreign_and_tampered_bytes(self):
        campaign = self.campaign()
        jingle = self.media(kind="jingle")
        with self.assertRaises(StoreError):
            advert.bind(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], idempotency_key=str(uuid4()), **jingle)
        binding = self.media()
        other = str(uuid4())
        self.store.put("stations", other, {"code": "FOREIGN", "name": "Other station", "timezone": "UTC", "status": "active"})
        sid = self.sid
        try:
            self.sid = other
            foreign = self.media(frames=1601)
        finally:
            self.sid = sid
        sequence = self.store.sequence
        with self.assertRaises(StoreError):
            advert.bind(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], idempotency_key=str(uuid4()), **foreign)
        self.assertEqual(self.store.sequence, sequence)
        for wrong in (dict(binding, media_sha256="0" * 64), dict(binding, media_id=str(uuid4()))):
            sequence = self.store.sequence
            with self.assertRaises(StoreError):
                advert.bind(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], idempotency_key=str(uuid4()), **wrong)
            self.assertEqual(self.store.sequence, sequence)
        detail = desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"])
        Path(detail["version"]["managed_path"]).write_bytes(b"changed managed content")
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            advert.bind(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], idempotency_key=str(uuid4()), **binding)
        self.assertEqual(self.store.sequence, sequence)
        self.assertFalse(self.store.scan("spots"))

    def test_bound_campaign_sales_edit_preserves_approved_catalog(self):
        campaign, binding = self.bound_campaign()
        approved = advert.spot_approve(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], str(uuid4()))
        library = self.store.get("desktop_media_library", binding["media_id"])
        item, version = self.store.get("items", library["item_id"]), self.store.get("versions", binding["version_id"])
        fields = self.fields(); fields.update(price=19, spot_title="Sales display", version_label="Sales version label")
        changed = api.save_campaign(self.store, self.principal, self.sid, "New sales name", fields, str(uuid4()), campaign_id=campaign["_id"], expected_revision=approved["campaign_revision"])
        self.assertEqual(changed["desktop_fields"]["price"], 19)
        self.assertEqual(self.store.get("items", item["_id"]), item)
        self.assertEqual(self.store.get("versions", version["_id"]), version)
        campaign = advert.campaign_state(self.store, self.principal, self.sid, changed["_id"], changed["_rev"], "approve", str(uuid4()))
        source = desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"])["version"]["managed_path"]
        original_bytes = Path(source).read_bytes()
        original_spot = self.store.get("spots", approved["spot"]["_id"])
        fields.update(price=25, contact="Changed business contact")
        saved = api.save_campaign(self.store, self.principal, self.sid, "Approved sales edit", fields, str(uuid4()),
                                  campaign_id=campaign["_id"], expected_revision=campaign["_rev"])
        self.assertEqual(saved["status"], "approved")
        self.assertEqual(saved["desktop_fields"]["price"], 25)
        sequence = self.store.sequence
        for key, value in (("advertiser", "Other buyer"), ("start_date", "2040-10-05"), ("end_date", "2040-10-07"),
                           ("planned_count", 11), ("slot", "13:00"), ("capacity_seconds", 60), ("competition", "Other category"), ("position", 1)):
            with self.assertRaises(Conflict):
                api.save_campaign(self.store, self.principal, self.sid, "Must not change contract", dict(fields, **{key: value}), str(uuid4()),
                                  campaign_id=saved["_id"], expected_revision=saved["_rev"])
            self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(Path(source).read_bytes(), original_bytes)
        self.assertEqual(self.store.get("spots", original_spot["_id"]), original_spot)
        self.assertEqual(self.store.get("items", item["_id"]), item)
        self.assertEqual(self.store.get("versions", version["_id"]), version)

    def test_measured_overhead_capacity_bridge_edit_and_date_move_rollback(self):
        self.output()
        binding = self.media(kind="jingle")
        config = self.break_config(opener=binding, opener_seconds=1)
        document = self.saved_break(config)
        self.assertEqual(document["config"]["bridge"]["overhead_ms"], 200)
        self.assertEqual(document["config"]["bridge"]["spot_budget_ms"], 9800)
        changed = self.saved_break(dict(config, time="13:00"), document_id=document["_id"], expected_revision=document["_rev"])
        self.assertEqual(changed["_id"], document["_id"])
        self.assertEqual(changed["_rev"], document["_rev"] + 1)
        old = self.store.get("plan_entries", document["break_entry_id"])
        self.assertEqual(old["state"], "cancelled")
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            self.saved_break(dict(config, date="2040-10-07"), document_id=changed["_id"], expected_revision=changed["_rev"])
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(len(self.store.scan("plans")), 1)
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_missing_overhead_binding_blocks_execution_and_invalid_codes_roll_back(self):
        self.output()
        document = self.saved_break(self.break_config(fixed_seconds=1))
        self.assertFalse(document["config"]["bridge"]["overhead_ready"])
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            advert.preview(self.store, self.principal, self.sid, document["_id"], document["_rev"])
        self.assertEqual(self.store.sequence, sequence)
        for config in (self.break_config(output_id="MISSING"), self.break_config(region_id="OUTSIDE"),
                       self.break_config(capacity_seconds=True), self.break_config(time="25:00")):
            with self.assertRaises(StoreError):
                self.saved_break(config)
        self.assertEqual(self.store.sequence, sequence)

    def test_actual_preview_reservation_complete_digest_cas_and_fresh_prepare(self):
        self.output(); campaign, binding = self.bound_campaign(approved=True)
        document = self.saved_break()
        sequence = self.store.sequence
        preview = advert.preview(self.store, self.principal, self.sid, document["_id"], document["_rev"])
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(preview["used_ms"], 200)
        self.assertEqual(preview["selected"][0]["campaign_id"], campaign["_id"])
        key = str(uuid4())
        args = (self.store, self.principal, self.sid, document["_id"], document["_rev"], preview["preview_hash"], preview["plan_revision"], key)
        result = advert.reserve(*args)
        sequence = self.store.sequence
        self.assertEqual(advert.reserve(*args), result)
        self.assertEqual(self.store.sequence, sequence)
        with self.assertRaises(Conflict):
            advert.reserve(*args, category_gap_slots=0)
        with self.assertRaises(Conflict):
            advert.reserve(*args[:-1], str(uuid4()))
        prepared = advert.prepare(self.store, self.principal, self.sid, document["_id"], result["break_revision"])
        manifest = prepared["manifest"]
        self.assertEqual(manifest["entries"][0]["entry_id"], prepared["playback"]["tracks"][0]["entry_id"])
        self.assertEqual(manifest["entries"][0]["expected_frames"], 9600)
        self.assertEqual(manifest["entries"][0]["media_id"], binding["media_id"])
        self.assertTrue(prepared["playback"]["tracks"][0]["eligible"])
        self.assertNotIn("managed_path", json.dumps(manifest))
        self.assertFalse(self.store.scan("playout_events"))
        self.assertEqual(self.store.get("plan_revisions", result["plan_revision_id"])["status"], "draft")

    def test_paused_campaign_or_changed_file_prevents_prepared_run(self):
        document, campaign, binding, _ = self.reservation()
        prepared = advert.prepare(self.store, self.principal, self.sid, document["_id"], document["_rev"])
        paused = advert.campaign_state(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], "pause", str(uuid4()))
        with self.assertRaises(Conflict):
            advert.run_start(self.store, self.principal, self.sid, document["_id"], document["_rev"], prepared["manifest_hash"], str(uuid4()))
        advert.campaign_state(self.store, self.principal, self.sid, campaign["_id"], paused["_rev"], "resume", str(uuid4()))
        detail = desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"])
        Path(detail["version"]["managed_path"]).write_bytes(b"tampered")
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            advert.prepare(self.store, self.principal, self.sid, document["_id"], document["_rev"])
        self.assertEqual(self.store.sequence, sequence)

    def test_local_receipts_actual_spans_append_only_terminal_and_no_formal_delivery(self):
        document, _, _, reservation = self.reservation()
        run = self.start(document); span = run["manifest"]["entries"][0]
        start = self.event(span)
        terminal = self.event(span, "local_complete", span["expected_frames"], occurred_at="2040-10-06T12:00:00.200000Z")
        key = str(uuid4())
        args = (self.store, self.principal, self.sid, run["run_id"], run["revision"], key, [start, terminal])
        saved = advert.run_event(*args)
        sequence = self.store.sequence
        self.assertEqual(advert.run_event(*args), saved)
        self.assertEqual(self.store.sequence, sequence)
        record = api.get_document(self.store, self.principal, self.sid, "advert_local_run", run["run_id"])
        self.assertEqual(record["config"]["state"], "local_complete")
        self.assertFalse(record["config"]["delivery_proven"])
        self.assertEqual(self.store.get("placements", reservation["placement_ids"][0])["state"], "planned")
        self.assertFalse(self.store.scan("playout_events"))
        self.assertFalse(self.store.scan("output_observations"))
        with self.assertRaises(StoreError):
            self.append(saved, [self.event(span, "interrupted", 5)])
        with self.assertRaises(Conflict):
            self.append(run, [self.event(span, occurrence=2)])
        bad = copy.deepcopy(record["config"]); bad["events"][0]["consumed_frames"] = 2
        with self.assertRaises(StoreError):
            self.store.put("desktop_configurations", record["_id"], dict(advert._body(record), config=bad), expected_revision=record["_rev"])

    def test_cancel_preserves_consumed_reservation_and_unplayed_replan_history(self):
        document, _, _, reservation = self.reservation()
        run = self.start(document); span = run["manifest"]["entries"][0]
        self.append(run, [self.event(span), self.event(span, "interrupted", 100)])
        original = self.store.get("placements", reservation["placement_ids"][0])
        cancelled = advert.cancel(self.store, self.principal, self.sid, document["_id"], document["_rev"], str(uuid4()))
        self.assertIn(span["entry_id"], cancelled["retained_consumed_entry_ids"])
        self.assertEqual(self.store.get("placements", original["_id"]), original)
        with self.assertRaises(Conflict):
            self.saved_break({key: value for key, value in document["config"].items() if key != "bridge"}, document_id=document["_id"], expected_revision=cancelled["break_revision"])

    def test_immutable_report_filter_reopen_json_csv_and_separate_evidence(self):
        document, campaign, _, _ = self.reservation()
        run = self.start(document); span = run["manifest"]["entries"][0]
        self.append(run, [self.event(span), self.event(span, "local_complete", span["expected_frames"])])
        report = self.report(campaign_id=campaign["_id"], output_id="MAIN")
        snapshot = report["config"]["snapshot"]
        self.assertEqual(snapshot["counters"]["local_complete"], 1)
        self.assertFalse(snapshot["delivery_proven"])
        self.assertEqual({row["record_class"] for row in snapshot["rows"]}, {"local", "planned"})
        self.assertTrue(all(row["campaign_id"] == campaign["_id"] for row in snapshot["rows"]))
        # New receipts do not retroactively change an immutable report run.
        saved = api.get_document(self.store, self.principal, self.sid, "advert_local_run", run["run_id"])
        self.append({"run_id": run["run_id"], "revision": saved["_rev"]}, [self.event(span, occurrence=2), self.event(span, "interrupted", 10, occurrence=2)])
        self.assertEqual(advert.report_get(self.store, self.principal, self.sid, report["_id"]), report)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(advert.report_get(reopened, self.principal, self.sid, report["_id"]), report)
        for format in ("json", "csv"):
            export = advert.report_export(reopened, self.principal, self.sid, report["_id"], format)
            self.assertIn("local_source_consumption", export["content"])
            self.assertFalse(export["partial"])
        row = api.get_document(self.store, self.principal, self.sid, "advert_report", report["_id"])
        with self.assertRaises(StoreError):
            self.store.put("desktop_configurations", row["_id"], dict(advert._body(row), name="Changed snapshot title"), expected_revision=row["_rev"])
        with self.assertRaises(StoreError):
            api.delete_document(self.store, self.principal, self.sid, "advert_report", row["_id"], row["_rev"], str(uuid4()))
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_owned_kind_injection_notification_per_user_and_permissions(self):
        campaign = self.campaign()
        overview = advert.overview(self.store, self.principal, self.sid)
        notification = overview["notifications"][0]["id"]
        viewer = create_station_user(self.store, self.principal, self.sid, "advert-viewer", "Viewer", ["view"])
        principal = authenticate(self.store, viewer["token"])
        advert.notifications_read(self.store, principal, self.sid, [notification], str(uuid4()))
        self.assertTrue(advert.overview(self.store, principal, self.sid)["notifications"][0]["read"])
        self.assertFalse(advert.overview(self.store, self.principal, self.sid)["notifications"][0]["read"])
        with self.assertRaises(Forbidden):
            advert.campaign_state(self.store, principal, self.sid, campaign["_id"], campaign["_rev"], "approve", str(uuid4()))
        for kind in ("advert_local_run", "advert_report", "advert_notifications"):
            with self.assertRaises(StoreError):
                api.put_document(self.store, self.principal, self.sid, kind, "Injected", {}, str(uuid4()))
        # Activating a global catalog item requires activate in every station
        # sharing it, even when the local traffic operator has approve/edit.
        bound, binding = self.bound_campaign()
        library = self.store.get("desktop_media_library", binding["media_id"])
        other = str(uuid4())
        self.store.put("stations", other, {"code": "SHARED", "name": "Shared station", "timezone": "UTC", "status": "active"})
        share_item(self.store, self.principal, other, library["item_id"])
        actor = create_station_user(self.store, self.principal, self.sid, "local-approver", "Local approver", ["view", "edit", "approve", "activate"])
        approver = authenticate(self.store, actor["token"])
        for permission in ("edit", "approve"):
            self.store.put("permissions", str(uuid4()), {"user_id": approver.user_id, "permission": permission, "station_id": other, "station_scope": other})
        sequence = self.store.sequence
        with self.assertRaises(Forbidden):
            advert.spot_approve(self.store, approver, self.sid, bound["_id"], bound["_rev"], str(uuid4()))
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store.get("versions", binding["version_id"])["status"], "draft")
        with self.assertRaises(Forbidden):
            advert.overview(self.store, principal, other)
        with self.assertRaises(NotFound):
            advert.campaign_get(self.store, self.principal, other, bound["_id"])

    def test_explicit_rights_grant_repairs_approved_spot_and_missing_rights_stays_blocked(self):
        self.output(); campaign, binding = self.bound_campaign()
        approval = advert.spot_approve(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], str(uuid4()))
        campaign = advert.campaign_state(self.store, self.principal, self.sid, campaign["_id"], approval["campaign_revision"], "approve", str(uuid4()))
        document = self.saved_break()
        blocked = advert.preview(self.store, self.principal, self.sid, document["_id"], document["_rev"])
        self.assertEqual(blocked["selected"], [])
        self.assertIn("not_ready:rights_unconfirmed_for_slot", blocked["rejected"][0]["reasons"])
        self.assertFalse(self.store.scan("rights_grants"))
        with self.assertRaises(StoreError):
            advert.spot_approve(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], str(uuid4()),
                rights={"territory": "LOCAL", "output_id": "MAIN", "source_ref": " "})
        with self.assertRaises(StoreError):
            advert.spot_approve(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], str(uuid4()),
                rights={"territory": "FOREIGN", "output_id": "MAIN", "source_ref": "Explicit operator license reference"})
        allowed = advert.spot_approve(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], str(uuid4()),
            rights={"territory": "LOCAL", "output_id": "MAIN", "source_ref": "Explicit operator license reference"})
        self.assertEqual(allowed["spot"]["_id"], approval["spot"]["_id"])
        self.assertEqual(len(self.store.scan("spots")), 1)
        self.assertEqual(len(self.store.scan("obligations")), 1)
        grant = self.store.scan("rights_grants")[0]
        self.assertEqual(grant["version_id"], binding["version_id"])
        self.assertEqual(grant["starts_at"], campaign["starts_at"])
        self.assertEqual(grant["ends_at"], campaign["ends_at"])
        self.assertEqual(grant["source_ref"], "Explicit operator license reference")
        self.assertEqual(len(advert.preview(self.store, self.principal, self.sid, document["_id"], document["_rev"])["selected"]), 1)

    def test_station_calendar_local_slot_and_dst_gap_fold_rejected(self):
        self.output()
        station = self.store.get("stations", self.sid)
        self.store.put("stations", self.sid, dict(advert._body(station), timezone="Europe/Istanbul"), expected_revision=station["_rev"])
        document = self.saved_break()
        self.assertEqual(self.store.get("plan_entries", document["break_entry_id"])["scheduled_at"], "2040-10-06T09:00:00Z")
        campaign, _ = self.bound_campaign(slot="12:00", capacity_seconds=30, end_date="2040-10-07", planned_count=4)
        approval = advert.spot_approve(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], str(uuid4()))
        obligations = [self.store.get("obligations", identifier) for identifier in approval["obligation_ids"]]
        self.assertEqual([row["required_plays"] for row in obligations], [2, 2])
        self.assertTrue(all(row["window_start"].endswith("12:00:00+03:00") for row in obligations))
        station = self.store.get("stations", self.sid)
        self.store.put("stations", self.sid, dict(advert._body(station), timezone="Europe/Berlin"), expected_revision=station["_rev"])
        sequence = self.store.sequence
        for day in ("2026-03-29", "2026-10-25"):
            with self.assertRaises(StoreError):
                self.saved_break(self.break_config(date=day, time="02:30"))
        self.assertEqual(self.store.sequence, sequence)

    def test_permission_revocation_between_authorization_and_commit_is_fenced(self):
        self.output()
        user = create_station_user(self.store, self.principal, self.sid, "advert-editor", "Editor", ["view", "edit"])
        principal = authenticate(self.store, user["token"])
        permission = next(row for row in self.store.lookup("permissions", "user", principal.user_id) if row["permission"] == "edit")
        original = api._station
        revoked = False
        def authorized_then_revoked(store, actor, sid, permission_name="view"):
            nonlocal revoked
            result = original(store, actor, sid, permission_name)
            if actor == principal and permission_name == "edit" and not revoked:
                revoked = True
                store.delete("permissions", permission["_id"], expected_revision=permission["_rev"])
            return result
        with patch.object(api, "_station", authorized_then_revoked):
            with self.assertRaises(Conflict):
                advert.break_save(self.store, principal, self.sid, "Must not commit", self.break_config(), str(uuid4()))
        self.assertTrue(revoked)
        self.assertFalse(self.store.scan("plans"))
        self.assertFalse(self.store.scan("desktop_configurations"))

    def test_invalid_local_receipts_foreign_identity_frames_bool_and_terminal_rollback(self):
        document, _, _, _ = self.reservation(); run = self.start(document)
        span = run["manifest"]["entries"][0]
        invalid = [self.event(span, frames=0), self.event(span, frames=True),
            self.event(span, "local_complete", span["expected_frames"]),
            self.event(span, "local_complete", span["expected_frames"] - 1),
            self.event(span, "interrupted", span["expected_frames"] + 1),
            self.event(span, entry_id=str(uuid4())), self.event(span, occurrence=True),
            self.event(span, occurred_at="2040-10-06T12:00:00"), self.event(span, main_sink="device\nname")]
        sequence = self.store.sequence
        for event in invalid:
            with self.assertRaises(StoreError):
                self.append(run, [event])
            self.assertEqual(self.store.sequence, sequence)
        saved = self.append(run, [self.event(span, "failed", 0, reason="C decoder validation failed")])
        self.assertEqual(api.get_document(self.store, self.principal, self.sid, "advert_local_run", saved["run_id"])["config"]["state"], "failed")
        report = self.report()
        self.assertEqual(report["config"]["snapshot"]["counters"]["local_failed"], 1)
        self.assertFalse(report["config"]["snapshot"]["delivery_proven"])

    def test_unplayed_cancel_replan_keeps_old_placements_and_blocks_generic_bridge_overwrite(self):
        document, _, _, reservation = self.reservation()
        with self.assertRaises(StoreError):
            api.put_document(self.store, self.principal, self.sid, "advert_break", "Overwrite", 
                {key: value for key, value in document["config"].items() if key != "bridge"}, str(uuid4()),
                document_id=document["_id"], expected_revision=document["_rev"])
        cancelled = advert.cancel(self.store, self.principal, self.sid, document["_id"], document["_rev"], str(uuid4()))
        placement = self.store.get("placements", reservation["placement_ids"][0])
        self.assertEqual(placement["state"], "cancelled")
        rewritten = self.saved_break(self.break_config(time="13:00"), document_id=document["_id"], expected_revision=cancelled["break_revision"])
        self.assertEqual(self.store.get("placements", placement["_id"]), placement)
        self.assertNotEqual(rewritten["break_entry_id"], document["config"]["bridge"]["break_entry_id"])
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_frozen_run_records_original_media_after_a_later_planned_creative_change(self):
        document, _, binding, _ = self.reservation(); run = self.start(document)
        span = run["manifest"]["entries"][0]
        replacement = self.media(title="Replacement creative")
        entry = self.store.get("plan_entries", span["entry_id"])
        changed = advert._body(entry)
        changed["payload"]["version_id"] = replacement["version_id"]
        self.store.put("plan_entries", entry["_id"], changed, expected_revision=entry["_rev"])
        self.append(run, [self.event(span), self.event(span, "interrupted", 100)])
        preserved = api.get_document(self.store, self.principal, self.sid, "advert_local_run", run["run_id"])
        self.assertEqual(preserved["config"]["manifest"]["entries"][0]["version_id"], binding["version_id"])
        self.assertNotEqual(binding["version_id"], replacement["version_id"])
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_started_only_unknown_empty_notexecuted_report_and_csv_formula_paging(self):
        self.output(); campaign, _ = self.bound_campaign(approved=True)
        library = self.store.get("desktop_media_library", campaign["desktop_binding"]["media_id"])
        item = self.store.get("items", library["item_id"])
        self.store.put("items", item["_id"], dict(advert._body(item), title="=FORMULA()"), expected_revision=item["_rev"])
        document = self.saved_break()
        preview = advert.preview(self.store, self.principal, self.sid, document["_id"], document["_rev"])
        reservation = advert.reserve(self.store, self.principal, self.sid, document["_id"], document["_rev"], preview["preview_hash"], preview["plan_revision"], str(uuid4()))
        document = reservation["document"]
        self.start(document)
        run = self.start(document); span = run["manifest"]["entries"][0]
        self.append(run, [self.event(span)])
        report = self.report()
        counters = report["config"]["snapshot"]["counters"]
        self.assertEqual(counters["local_notexecuted"], 1)
        self.assertEqual(counters["local_unknown"], 1)
        export = advert.report_export(self.store, self.principal, self.sid, report["_id"], "csv")
        self.assertIn("'=FORMULA()", export["content"])
        page = advert.report_get(self.store, self.principal, self.sid, report["_id"], limit=1)
        self.assertEqual(len(page["config"]["snapshot"]["rows"]), 1)
        self.assertEqual(page["total"], 3)
        self.assertEqual(page["next_offset"], 1)
        self.assertEqual(page["snapshot_hash"], report["snapshot_hash"])

    def test_http_actions_authentication_tenancy_and_unknown_fields_rejected(self):
        class Handler(make_handler(self.store)):
            def log_message(self, *args):
                pass
        server = BoundedHTTPServer(("127.0.0.1", 0), Handler, max_connections=4, request_timeout=3)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        self.addCleanup(server.server_close); self.addCleanup(thread.join, 2); self.addCleanup(server.shutdown)
        def request(action, body=None, token=None):
            connection = HTTPConnection(*server.server_address, timeout=3)
            headers = {"Content-Type": "application/json"}
            if token is not None:
                headers["Authorization"] = "Bearer " + token
            connection.request("POST", "/v1/workflows/desktop-advert/" + action,
                               json.dumps(body or {"station_id": self.sid}), headers)
            response = connection.getresponse(); value = json.loads(response.read()); connection.close()
            return response.status, value
        self.assertEqual(request("overview")[0], 403)
        self.assertEqual(request("overview", token="wrong")[0], 403)
        status, value = request("overview", token=self.fixture.identity["api_token"])
        self.assertEqual(status, 200)
        self.assertEqual(value["campaigns"], [])
        self.assertNotIn(self.fixture.identity["api_token"], json.dumps(value))
        self.assertEqual(request("overview", {"station_id": self.sid, "fake_delivery_proven": True}, self.fixture.identity["api_token"])[0], 422)
        self.assertEqual(request("output-create", {"station_id": self.sid, "output_id": "MAIN", "idempotency_key": str(uuid4())}, self.fixture.identity["api_token"])[0], 200)

    def test_all_empty_break_policies_use_only_real_measured_sources_or_explicit_skip(self):
        self.output()
        sources = {role: self.media(kind="jingle", title=role) for role in ("opener", "fixed", "closer")}
        for mode, roles in ((0, ["fixed"]), (1, []), (2, ["opener", "fixed", "closer"])):
            with self.subTest(empty_mode=mode):
                config = self.break_config(time=str(12 + mode) + ":00", empty_mode=mode,
                    opener_seconds=1, fixed_seconds=1, closer_seconds=1, **sources)
                document = self.saved_break(config)
                preview = advert.preview(self.store, self.principal, self.sid, document["_id"], document["_rev"])
                self.assertEqual(preview["selected"], [])
                reservation = advert.reserve(self.store, self.principal, self.sid, document["_id"], document["_rev"],
                    preview["preview_hash"], preview["plan_revision"], str(uuid4()))
                self.assertEqual(reservation["placement_ids"], [])
                prepared = advert.prepare(self.store, self.principal, self.sid, document["_id"], reservation["break_revision"])
                if not roles:
                    self.assertTrue(prepared["skipped"])
                    self.assertIsNone(prepared["manifest"])
                    self.assertIsNone(prepared["playback"])
                    with self.assertRaises(Conflict):
                        advert.run_start(self.store, self.principal, self.sid, document["_id"], reservation["break_revision"], "0" * 64, str(uuid4()))
                else:
                    self.assertFalse(prepared["skipped"])
                    self.assertEqual([entry["role"] for entry in prepared["manifest"]["entries"]], roles)
                    self.assertTrue(all(entry["expected_frames"] == 9600 for entry in prepared["manifest"]["entries"]))
        self.assertFalse(self.store.scan("campaigns")); self.assertFalse(self.store.scan("spots"))
        self.assertFalse(self.store.scan("obligations")); self.assertFalse(self.store.scan("placements"))
        self.assertFalse(self.store.scan("playout_events"))

    def test_permitted_capacity_overrun_and_fractional_frames_are_exact_and_hash_bound(self):
        self.output()
        binding = self.media(frames=81600)  # 10.2 measured seconds, beyond nominal10.
        campaign = self.campaign()
        campaign = advert.bind(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], idempotency_key=str(uuid4()), **binding)
        approval = advert.spot_approve(self.store, self.principal, self.sid, campaign["_id"], campaign["_rev"], str(uuid4()),
            rights={"territory": "LOCAL", "output_id": "MAIN", "source_ref": "Explicit fixture authorization"})
        advert.campaign_state(self.store, self.principal, self.sid, campaign["_id"], approval["campaign_revision"], "approve", str(uuid4()))
        document = self.saved_break(self.break_config(tolerance_seconds=1, start_mode=2, wait_seconds=17))
        preview = advert.preview(self.store, self.principal, self.sid, document["_id"], document["_rev"])
        self.assertEqual(preview["used_ms"], 10200)
        self.assertEqual(preview["nominal_capacity_ms"], 10000)
        self.assertEqual(preview["effective_capacity_ms"], 11000)
        self.assertEqual(preview["tolerance_ms"], 1000)
        nominal = self.saved_break(self.break_config(time="13:00"))
        rejected = advert.preview(self.store, self.principal, self.sid, nominal["_id"], nominal["_rev"])
        self.assertEqual(rejected["selected"], [])
        self.assertEqual(rejected["effective_capacity_ms"], 10000)
        preview = advert.preview(self.store, self.principal, self.sid, document["_id"], document["_rev"])
        reservation = advert.reserve(self.store, self.principal, self.sid, document["_id"], document["_rev"], preview["preview_hash"], preview["plan_revision"], str(uuid4()))
        prepared = advert.prepare(self.store, self.principal, self.sid, document["_id"], reservation["break_revision"])
        manifest = prepared["manifest"]
        self.assertEqual((manifest["capacity_ms"], manifest["tolerance_ms"], manifest["effective_capacity_ms"]), (10000, 1000, 11000))
        self.assertEqual((manifest["start_mode"], manifest["wait_seconds"]), (2, 17))
        self.assertNotEqual(advert._manifest_hash(dict(manifest, wait_seconds=18)), prepared["manifest_hash"])
        fraction = self.media(kind="jingle", sample_rate=44100, frames=8821)
        fractional = self.saved_break(self.break_config(time="14:00", empty_mode=2, opener=fraction, opener_seconds=1))
        empty = advert.preview(self.store, self.principal, self.sid, fractional["_id"], fractional["_rev"])
        # The huge10.2s advert cannot fit the9.8s remaining spot budget.
        self.assertEqual(empty["selected"], [])
        reserved = advert.reserve(self.store, self.principal, self.sid, fractional["_id"], fractional["_rev"], empty["preview_hash"], empty["plan_revision"], str(uuid4()))
        actual = advert.prepare(self.store, self.principal, self.sid, fractional["_id"], reserved["break_revision"])
        self.assertEqual(actual["manifest"]["entries"][0]["expected_frames"], math.ceil(8821 * 48000 / 44100))
        self.assertGreater(actual["manifest"]["entries"][0]["expected_frames"], 200 * 48)


if __name__ == "__main__":
    unittest.main()
