"""Isolated persisted MAIN profiles, frozen local receipts and emergency bytes."""
import copy
from http.client import HTTPConnection
import json
from pathlib import Path
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4

import test_desktop_api as fixtures
from sdbase import desktop_api as api, desktop_broadcast as broadcast, desktop_media
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.catalog import NotFound
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate, issue_endpoint_token
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError


class DesktopBroadcast(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal

    def profile(self, kind="record", config=None, **kwargs):
        defaults = {"record": {"filename": "MAIN.wav"},
                    "stream": {"url": "http://127.0.0.1:19821/radios.mp3", "username": "source", "bitrate_kbps": 128},
                    "metadata": {"title": "RadioS", "automatic": False}}
        return broadcast.profile_save(self.store, self.principal, self.sid, kind,
            "Native MAIN", defaults[kind] if config is None else config, str(uuid4()), **kwargs)

    def emergency(self, queues=None, **kwargs):
        if queues is None:
            binding = self.fixture.verified_binding()
            queues = [[self.entry(binding)], []]
        return api.put_document(self.store, self.principal, self.sid, "emergency", "Acil",
            {"queues": queues, "preparation": {}}, str(uuid4()), **kwargs)

    @staticmethod
    def entry(binding=None, **kwargs):
        return {"id": str(uuid4()), "kind": 0,
            "values": ["Acil", "", "", "00:00", "", "", "", "", ""],
            "file": "/display-only/untrusted.wav", **(binding or {}), **kwargs}

    def start(self, profile=None, kind="record", **kwargs):
        profile = profile or self.profile(kind)
        return broadcast.run_start(self.store, self.principal, self.sid, kind, profile["_id"],
            profile["_rev"], str(uuid4()), **kwargs)

    def event(self, run, state="active", frames=48000, output_bytes=192000, **kwargs):
        return broadcast.run_event(self.store, self.principal, self.sid, run["run_id"], run["revision"],
            state, frames, output_bytes, "Isolated native observation", str(uuid4()), **kwargs)

    def test_empty_overview_is_readonly_and_secret_free(self):
        sequence = self.store.sequence
        snapshot = broadcast.overview(self.store, self.principal, self.sid)
        self.assertEqual(snapshot["profiles"], {"record": [], "stream": [], "metadata": []})
        self.assertEqual(snapshot["runs"], [])
        self.assertEqual(snapshot["emergency"], [])
        self.assertEqual(self.store.sequence, sequence)
        self.assertNotIn(self.fixture.identity["api_token"], json.dumps(snapshot))

    def test_emergency_repeat_uses_saved_acknowledged_preparation(self):
        row = self.emergency()
        prepared = broadcast.emergency_prepare(self.store, self.principal, self.sid, row["_id"], row["_rev"])
        self.assertFalse(prepared["playback"]["emergency"]["repeat"])
        config = copy.deepcopy(row["config"])
        config["preparation"]["takeover/Liste bitince"] = "Listeyi tekrarla"
        saved = api.put_document(self.store, self.principal, self.sid, "emergency", "Acil", config,
            str(uuid4()), document_id=row["_id"], expected_revision=row["_rev"])
        with self.assertRaises(Conflict):
            broadcast.emergency_prepare(self.store, self.principal, self.sid, row["_id"], row["_rev"])
        run = self.start(saved, kind="emergency")
        self.assertTrue(run["playback"]["emergency"]["repeat"])
        self.assertEqual(len(run["playback"]["tracks"]), 1)

    def test_profiles_reopen_cas_replay_and_optional_metadata(self):
        key = str(uuid4())
        config = {"url": "https://radio.example/live.mp3", "username": "source", "bitrate_kbps": 192}
        saved = broadcast.profile_save(self.store, self.principal, self.sid, "stream", "Real mount", config, key)
        sequence = self.store.sequence
        self.assertEqual(broadcast.profile_save(self.store, self.principal, self.sid, "stream", "Real mount", config, key), saved)
        self.assertEqual(self.store.sequence, sequence)
        config = dict(config, bitrate_kbps=256)
        revised = broadcast.profile_save(self.store, self.principal, self.sid, "stream", "Real mount", config,
            str(uuid4()), saved["_id"], saved["_rev"])
        with self.assertRaises(Conflict):
            broadcast.profile_save(self.store, self.principal, self.sid, "stream", "Real mount", config,
                str(uuid4()), saved["_id"], saved["_rev"])
        metadata = self.profile("metadata", {"title": "Türkçe radyo", "automatic": True})
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "broadcast_stream", saved["_id"]), revised)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "broadcast_metadata", metadata["_id"]), metadata)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_profile_validation_no_paths_secrets_unknowns_or_controls(self):
        stream = {"url": "http://127.0.0.1/live.mp3", "username": "source", "bitrate_kbps": 128}
        variants = [("stream", dict(stream, url=value)) for value in (
            "file:///tmp/x", "ftp://host/live", "http://user:secret@host/live", "http://host/",
            "http://host/live?password=secret", "http://host/live#token", "http://host/live\x01", "http://host/live\\x")]
        variants += [("stream", dict(stream, bitrate_kbps=value)) for value in (True, 120, 128.0, -1)]
        variants += [("stream", dict(stream, username=value)) for value in ("", "a:b", "source\n", "😀" * 65)]
        variants += [("stream", dict(stream, password="must not persist")), ("stream", dict(stream, on_air=True))]
        variants += [("stream", dict(stream, password_saved=True)), ("stream", dict(stream, password="x", password_saved=False)),
                     ("stream", dict(stream, password="x", password_saved="yes")), ("stream", dict(stream, password=12, password_saved=True)),
                     ("stream", dict(stream, password="a\nb", password_saved=True)), ("stream", dict(stream, password="ş" * 129, password_saved=True))]
        variants += [("record", {"filename": value}) for value in (
            "../x.wav", "/tmp/x.wav", ".hidden.wav", "x.mp3", "x.WAV", "x\\y.wav", "x.wav ", "x\n.wav", "😀" * 100 + ".wav")]
        variants += [("metadata", {"title": value, "automatic": False}) for value in ("😀" * 257, "title\n")]
        variants += [("metadata", {"title": "ok", "automatic": "yes"})]
        sequence = self.store.sequence
        for kind, config in variants:
            with self.subTest(kind=kind, config=config), self.assertRaises(StoreError):
                self.profile(kind, config)
            self.assertEqual(self.store.sequence, sequence)

    def test_saved_stream_password_persists_reloads_and_never_enters_receipts(self):
        config = {"url": "http://127.0.0.1:19821/radios.mp3", "username": "source", "bitrate_kbps": 128,
                  "password": "gizli-şifre", "password_saved": True}
        saved = self.profile("stream", config)
        self.assertEqual(saved["config"], config)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "broadcast_stream", saved["_id"])["config"], config)
        self.assertEqual(broadcast.overview(self.store, self.principal, self.sid)["profiles"]["stream"][0]["config"], config)
        run = self.start(saved, kind="stream")
        self.assertNotIn("password", run["profile"])
        self.assertNotIn("password_saved", run["profile"])
        row = self.store.get("desktop_configurations", run["run_id"])
        self.assertNotIn("gizli", json.dumps(row))
        self.assertEqual(row["config"]["manifest"]["profile"], {"url": config["url"], "username": "source", "bitrate_kbps": 128})
        self.assertEqual(self.event(run)["state"], "active")
        # Unticking "save" removes the password from the stored profile; the plain form stays valid.
        plain = {"url": config["url"], "username": "source", "bitrate_kbps": 128}
        revised = broadcast.profile_save(self.store, self.principal, self.sid, "stream", "Native MAIN", plain,
            str(uuid4()), saved["_id"], saved["_rev"])
        self.assertEqual(revised["config"], plain)
        self.assertEqual(inspect_integrity(open_radio_store(self.fixture.db, read_only=True))["status"], "ok")

    def test_manage_operate_view_and_endpoint_permissions(self):
        user = create_station_user(self.store, self.principal, self.sid, "operator", "Operator", ["view", "operate"])
        operator = authenticate(self.store, user["token"])
        profile = self.profile()
        self.assertEqual(broadcast.overview(self.store, operator, self.sid)["profiles"]["record"][0]["_id"], profile["_id"])
        with self.assertRaises(Forbidden):
            broadcast.profile_save(self.store, operator, self.sid, "record", "No", {"filename": "x.wav"}, str(uuid4()))
        run = broadcast.run_start(self.store, operator, self.sid, "record", profile["_id"], profile["_rev"], str(uuid4()))
        with self.assertRaises(Forbidden):
            broadcast.run_event(self.store, self.principal, self.sid, run["run_id"], run["revision"], "failed", 0, 0, "", str(uuid4()))
        viewer = create_station_user(self.store, self.principal, self.sid, "viewer", "Viewer", ["view"])
        with self.assertRaises(Forbidden):
            broadcast.run_start(self.store, authenticate(self.store, viewer["token"]), self.sid,
                "record", profile["_id"], profile["_rev"], str(uuid4()))
        token = issue_endpoint_token(self.store, self.principal, self.sid, self.fixture.identity["endpoint_id"])
        with self.assertRaises(Forbidden):
            broadcast.run_start(self.store, authenticate(self.store, token), self.sid,
                "record", profile["_id"], profile["_rev"], str(uuid4()))

    def test_run_durable_manifest_replay_no_endpoint_delivery_or_fake_audio(self):
        profile = self.profile("stream")
        key = str(uuid4())
        run = broadcast.run_start(self.store, self.principal, self.sid, "stream", profile["_id"], profile["_rev"], key)
        sequence = self.store.sequence
        self.assertEqual(broadcast.run_start(self.store, self.principal, self.sid, "stream", profile["_id"], profile["_rev"], key), run)
        self.assertEqual(self.store.sequence, sequence)
        row = self.store.get("desktop_configurations", run["run_id"])
        self.assertEqual(row["config"]["state"], "prepared")
        self.assertEqual(row["config"]["events"], [])
        self.assertFalse(row["config"]["delivery_proven"])
        for collection in ("playout_events", "output_observations", "emergency_alert_attempts", "recording_attempts"):
            self.assertFalse(self.store.scan(collection))
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(reopened.get("desktop_configurations", run["run_id"]), row)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_event_cas_replay_monotonic_and_one_terminal(self):
        run = self.start()
        key = str(uuid4())
        active = broadcast.run_event(self.store, self.principal, self.sid, run["run_id"], run["revision"],
            "active", 48000, 192000, "First encoded PCM", key)
        sequence = self.store.sequence
        self.assertEqual(broadcast.run_event(self.store, self.principal, self.sid, run["run_id"], run["revision"],
            "active", 48000, 192000, "First encoded PCM", key), active)
        self.assertEqual(self.store.sequence, sequence)
        with self.assertRaises(Conflict):
            self.event(run)
        for state, frames, output_bytes in (("active", 47999, 192000), ("completed", 48000, 191999),
            ("on_air", 48000, 192000), ("active", True, 192000), ([], 48000, 192000)):
            with self.assertRaises(StoreError):
                self.event(active, state, frames, output_bytes)
        done = self.event(active, "completed", 96000, 384000)
        self.assertEqual(done["state"], "completed")
        with self.assertRaises(Conflict):
            self.event(done)
        self.assertFalse(done["delivery_proven"])
        self.assertEqual(len(self.store.get("desktop_configurations", done["run_id"])["config"]["events"]), 2)

    def test_zero_frames_completion_rejected_but_failure_valid(self):
        run = self.start()
        with self.assertRaises(StoreError):
            self.event(run, "completed", 0, 0)
        failed = self.event(run, "failed", 0, 0)
        self.assertEqual(failed["state"], "failed")
        self.assertEqual(broadcast.overview(self.store, self.principal, self.sid)["runs"][0]["last_event"]["frames"], 0)

    def test_managed_receipts_cannot_be_forged_rewritten_deleted_or_hidden(self):
        run = self.start()
        row = self.store.get("desktop_configurations", run["run_id"])
        with self.assertRaises(StoreError):
            api.put_document(self.store, self.principal, self.sid, "broadcast_local_run", "Forgery", row["config"], str(uuid4()))
        with self.assertRaises(StoreError):
            api.delete_document(self.store, self.principal, self.sid, "broadcast_local_run", row["_id"], row["_rev"], str(uuid4()))
        changed = broadcast._body(row)
        changed["config"]["manifest"]["profile"]["filename"] = "different.wav"
        changed["config"]["manifest_hash"] = broadcast._hash(changed["config"]["manifest"])
        with self.assertRaises(StoreError):
            self.store.put("desktop_configurations", row["_id"], changed, expected_revision=row["_rev"])
        with self.assertRaises(StoreError):
            self.store.put("desktop_configurations", row["_id"], broadcast._body(row) | {"status": "disabled"}, expected_revision=row["_rev"])
        active = self.event(run)
        row = self.store.get("desktop_configurations", active["run_id"])
        changed = broadcast._body(row); changed["config"]["events"] = []; changed["config"]["state"] = "prepared"
        with self.assertRaises(StoreError):
            self.store.put("desktop_configurations", row["_id"], changed, expected_revision=row["_rev"])
        with self.assertRaises(StoreError):
            self.store.delete("desktop_configurations", row["_id"], expected_revision=row["_rev"])

    def test_revision_foreign_profile_and_disabled_station_execution(self):
        profile = self.profile()
        with self.assertRaises(Conflict):
            broadcast.run_start(self.store, self.principal, self.sid, "record", profile["_id"], True, str(uuid4()))
        with self.assertRaises(NotFound):
            self.start(profile, "stream")
        other = str(uuid4())
        self.store.put("stations", other, {"code": "OTHER-BC", "name": "Other", "timezone": "UTC", "status": "active"})
        with self.assertRaises(NotFound):
            broadcast.run_start(self.store, self.principal, other, "record", profile["_id"], profile["_rev"], str(uuid4()))
        station = self.store.get("stations", self.sid)
        self.store.put("stations", self.sid, {key: value for key, value in station.items() if not key.startswith("_")} | {"status": "disabled"},
            expected_revision=station["_rev"])
        with self.assertRaises(Conflict):
            self.start(profile)

    def test_emergency_binding_schema_partial_foreign_and_duplicate_ids(self):
        binding = self.fixture.verified_binding()
        entry = self.entry(binding)
        for key in binding:
            broken = dict(entry); del broken[key]
            with self.assertRaises(StoreError):
                self.emergency([[broken], []])
        with self.assertRaises(StoreError):
            self.emergency([[entry], [copy.deepcopy(entry)]])
        with self.assertRaises(StoreError):
            self.emergency([[dict(entry, media_sha256="0" * 64)], []])
        legacy = self.emergency([[self.entry()], []])
        self.assertEqual(len(legacy["config"]["queues"][0]), 1)
        sequence = self.store.sequence
        with self.assertRaises(StoreError):
            broadcast.emergency_prepare(self.store, self.principal, self.sid, legacy["_id"], legacy["_rev"])
        self.assertEqual(self.store.sequence, sequence)

    def test_emergency_prepare_real_bytes_readonly_bounded_and_no_untrusted_file(self):
        binding = self.fixture.verified_binding()
        entry = self.entry(binding)
        row = self.emergency([[entry], []])
        sequence = self.store.sequence
        prepared = broadcast.emergency_prepare(self.store, self.principal, self.sid, row["_id"], row["_rev"])
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(prepared["playback"]["kind"], "programme")
        self.assertEqual(prepared["playback"]["tracks"][0]["entry_id"], entry["id"])
        detail = prepared["playback"]["tracks"][0]["detail"]
        self.assertTrue(detail["version"]["object_available"])
        self.assertEqual(detail["version"]["sha256"], binding["media_sha256"])
        self.assertNotIn("waveform", detail["analysis"])
        self.assertNotIn(entry["file"], json.dumps(prepared))
        with self.assertRaises(StoreError):
            broadcast.emergency_prepare(self.store, self.principal, self.sid, row["_id"], row["_rev"], 1)
        with self.assertRaises(StoreError):
            broadcast.emergency_prepare(self.store, self.principal, self.sid, row["_id"], row["_rev"], True)
        with self.assertRaises(Conflict):
            broadcast.emergency_prepare(self.store, self.principal, self.sid, row["_id"], row["_rev"] + 1)

    def test_emergency_legacy_id_stability_durable_start_and_partial_preflight_rejection(self):
        binding = self.fixture.verified_binding()
        entry = self.entry(binding); del entry["id"]
        row = self.emergency([[entry], []])
        first = broadcast.emergency_prepare(self.store, self.principal, self.sid, row["_id"], row["_rev"])
        second = broadcast.emergency_prepare(self.store, self.principal, self.sid, row["_id"], row["_rev"])
        self.assertEqual(first["playback"]["tracks"][0]["entry_id"], second["playback"]["tracks"][0]["entry_id"])
        run = self.start(row, "emergency")
        self.assertEqual(run["playback"]["tracks"][0]["entry_id"], first["playback"]["tracks"][0]["entry_id"])
        self.assertNotIn("managed_path", json.dumps(self.store.get("desktop_configurations", run["run_id"])))
        done = self.event(run, "completed", 9600, 0)
        self.assertEqual(done["state"], "completed")
        mixed = self.emergency([[self.entry(binding), self.entry()], []])
        sequence = self.store.sequence
        with self.assertRaises(StoreError):
            self.start(mixed, "emergency")
        self.assertEqual(self.store.sequence, sequence)

    def test_emergency_tampered_disabled_unverified_bytes_never_create_run(self):
        binding = self.fixture.verified_binding()
        row = self.emergency([[self.entry(binding)], []])
        detail = desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"])
        path = Path(detail["version"]["managed_path"])
        original = path.read_bytes(); path.chmod(0o600); path.write_bytes(b"tampered")
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            self.start(row, "emergency")
        self.assertEqual(self.store.sequence, sequence)
        path.write_bytes(original)
        key = str(uuid4())
        run = broadcast.run_start(self.store, self.principal, self.sid, "emergency", row["_id"], row["_rev"], key)
        path.write_bytes(b"tampered again")
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            broadcast.run_start(self.store, self.principal, self.sid, "emergency", row["_id"], row["_rev"], key)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store.get("desktop_configurations", run["run_id"])["config"]["state"], "prepared")
        path.write_bytes(original)
        media = self.store.get("media_files", binding["media_file_id"])
        self.store.put("media_files", media["_id"], {key: value for key, value in media.items() if not key.startswith("_")} | {"status": "error"},
            expected_revision=media["_rev"])
        with self.assertRaises(StoreError):
            self.start(row, "emergency")
        self.assertEqual(inspect_integrity(open_radio_store(self.fixture.db, read_only=True))["status"], "ok")

    def test_failed_response_budget_and_transaction_do_not_create_orphan_run(self):
        row = self.emergency()
        sequence = self.store.sequence
        with patch.object(broadcast, "MAX_BYTES", 100), self.assertRaises(StoreError):
            self.start(row, "emergency")
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store.lookup("desktop_configurations", "station_kind", self.sid, "broadcast_local_run"), [])
        profile = self.profile()
        sequence = self.store.sequence
        with patch.object(self.store, "begin", side_effect=StoreError("isolated precommit failure")), self.assertRaises(StoreError):
            self.start(profile)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.store.lookup("desktop_configurations", "station_kind", self.sid, "broadcast_local_run"), [])

    def test_http_contract_auth_cas_validation_registered_routes(self):
        base = make_handler(self.store)
        class Handler(base):
            def log_message(self, *args): pass
        server = BoundedHTTPServer(("127.0.0.1", 0), Handler, request_timeout=2)
        worker = threading.Thread(target=server.serve_forever, daemon=True); worker.start()
        def request(action, body, auth=True, method="POST"):
            conn = HTTPConnection("127.0.0.1", server.server_port, timeout=2)
            headers = {"Content-Type": "application/json", "Connection": "close"}
            if auth:
                headers["Authorization"] = "Bearer " + self.fixture.identity["api_token"]
            conn.request(method, "/v1/workflows/desktop-broadcast/" + action, json.dumps(body), headers)
            response = conn.getresponse(); value = json.loads(response.read()); status = response.status
            conn.close(); return status, value
        try:
            self.assertEqual(request("overview", {"station_id": self.sid}, False)[0], 403)
            self.assertEqual(request("overview", {"station_id": self.sid}, method="GET")[0], 422)
            self.assertEqual(request("overview", {"station_id": self.sid, "principal": "forged"})[0], 422)
            body = {"station_id": self.sid, "kind": "record", "name": "MAIN", "config": {"filename": "test.wav"}, "idempotency_key": str(uuid4())}
            status, saved = request("profile-save", body); self.assertEqual(status, 200)
            status, run = request("run-start", {"station_id": self.sid, "kind": "record", "profile_id": saved["_id"],
                "expected_revision": saved["_rev"], "idempotency_key": str(uuid4())}); self.assertEqual(status, 200)
            status, failed = request("run-event", {"station_id": self.sid, "run_id": run["run_id"], "expected_revision": run["revision"],
                "state": "failed", "frames": 0, "bytes": 0, "detail": "Isolated failure", "idempotency_key": str(uuid4())})
            self.assertEqual(status, 200); self.assertEqual(failed["state"], "failed")
            self.assertEqual(request("profile-save", body | {"config": {"filename": "../escape.wav"}, "idempotency_key": str(uuid4())})[0], 422)
            self.assertEqual(request("emergency-prepare", {"station_id": self.sid, "document_id": str(uuid4()), "expected_revision": 1})[0], 404)
            snapshot = request("overview", {"station_id": self.sid})[1]
            self.assertEqual(snapshot["runs"][0]["state"], "failed")
            self.assertFalse(snapshot["runs"][0]["delivery_proven"])
        finally:
            server.shutdown(); server.server_close(); worker.join(timeout=3)


if __name__ == "__main__":
    unittest.main(verbosity=2)
