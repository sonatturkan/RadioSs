"""Private real-byte formal approval and native immutable loader acceptance."""
import copy
from http.client import HTTPConnection
import json
import threading
import unittest
from uuid import uuid4

import test_desktop_media as media_fixtures
from sdbase import desktop_api, desktop_media, desktop_programme as api, catalog, rights, station_outputs
from sdbase.api_server import BoundedHTTPServer
from sdbase.http_server import make_handler
from sdbase.access_admin import create_station_user
from sdbase.identity import authenticate, Forbidden
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError


class DesktopProgramme(unittest.TestCase):
    def setUp(self):
        self.fixture = media_fixtures.DesktopMedia("runTest"); self.fixture.setUp(); self.addCleanup(self.fixture.tearDown)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal
        self.media = self.fixture.load(); library, version = self.media["media"], self.media["version"]
        self.binding = {"media_id": library["_id"], "version_id": version["_id"],
                        "media_file_id": version["media_file_id"], "media_sha256": version["sha256"]}
        self.config = {"title": "İncelenen gerçek sıra", "date": "2026-10-06", "note": "", "events": [
            {"id": str(uuid4()), "title": library["title"], "artist": library["artist"], "album": library["album"],
             "kind": "music", "start_time": "08:30", "duration_ms": 200, "version_label": "v1",
             "primary_file": "", "backup_file": "", "note": "", **self.binding}]}
        self.document = desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Mevcut program", self.config, str(uuid4()))
        station_outputs.create_station_output(self.store, self.principal, self.sid,
            self.fixture.identity["endpoint_id"], "LOCAL_MAIN", geo_codes=["TR"])

    def overview(self):
        return api.overview(self.store, self.principal, self.sid, self.document["_id"])

    def submit(self, **changes):
        data = {"document_id": self.document["_id"], "expected_document_revision": self.document["_rev"], "idempotency_key": str(uuid4())}
        current = self.overview()["plan"]
        if current: data["expected_plan_revision"] = current["_rev"]
        data.update(changes)
        return api.submit(self.store, self.principal, self.sid, **data)

    def guarded(self):
        overview = self.overview()
        return {"document_id": self.document["_id"], "plan_id": overview["plan"]["_id"],
                "revision_id": overview["revision"]["_id"], "expected_plan_revision": overview["plan"]["_rev"]}

    def authorize_media(self):
        library, version = self.media["media"], self.media["version"]
        catalog.approve_version(self.store, self.principal, self.sid, version["_id"], expected_revision=version["_rev"])
        item = self.store.get("items", library["item_id"])
        catalog.set_lifecycle(self.store, self.principal, self.sid, item["_id"], "active", expected_revision=item["_rev"])
        association = self.store.lookup("station_items", "station_item", self.sid, item["_id"])[0]
        catalog.set_station_eligibility(self.store, self.principal, self.sid, item["_id"], True, expected_revision=association["_rev"])
        rights.grant(self.store, self.principal, self.sid, version["_id"], territory="*", output_id="LOCAL_MAIN",
            starts_at="2026-10-06T00:00:00Z", ends_at="2026-10-07T00:00:00Z", source_ref="isolated explicit permission")

    def approve(self):
        return api.approve(self.store, self.principal, self.sid, **self.guarded())

    def test_typed_block_playlist_provenance_persists_reopens_and_rejects_invalid_atomically(self):
        playlist_id = str(uuid4())
        config = copy.deepcopy(self.config)
        music = {"id": str(uuid4()), "kind": "music", "title": "Müzik bloğu",
                 "start_time": "", "note": "", "playlist_id": playlist_id}
        jingle = {"id": str(uuid4()), "kind": "jingle", "title": "Jingle bloğu",
                  "start_time": "", "note": "", "playlist_id": playlist_id}
        legacy = {"id": str(uuid4()), "kind": "music", "title": "Eski boş blok",
                  "start_time": "", "note": ""}
        config["blocks"] = [music, jingle, legacy]
        config["events"][0]["block_id"] = music["id"]
        config["events"].append(dict(config["events"][0], id=str(uuid4()),
                                     block_id=jingle["id"], kind="jingle", title="Aradaki jingle"))
        sequence = self.store.sequence
        saved = desktop_api.put_document(self.store, self.principal, self.sid, "programme",
            "Kaynak playlisti korunan bloklar", config, str(uuid4()),
            document_id=self.document["_id"], expected_revision=self.document["_rev"])
        self.assertEqual(self.store.sequence, sequence + 1)
        self.assertEqual(saved["config"], config)
        self.assertNotIn("playlist_id", saved["config"]["blocks"][2])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        actual = desktop_api.get_document(reopened, self.principal, self.sid, "programme", saved["_id"])
        self.assertEqual(actual["config"], config)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        # Invalid provenance must not consume a CAS revision or mutation receipt.
        sequence = self.store.sequence
        receipts = self.store.scan("desktop_mutations")
        for invalid in (None, "", "not-a-uuid", playlist_id + "-junk", 7, True, [], {}):
            with self.subTest(playlist_id=invalid):
                broken = copy.deepcopy(config)
                broken["blocks"][1]["playlist_id"] = invalid
                with self.assertRaisesRegex(StoreError, "block playlist_id must be a UUID"):
                    desktop_api.put_document(self.store, self.principal, self.sid, "programme",
                        "Kaynak playlisti korunan bloklar", broken, str(uuid4()),
                        document_id=saved["_id"], expected_revision=saved["_rev"])
                self.assertEqual(self.store.sequence, sequence)
                self.assertEqual(self.store.scan("desktop_mutations"), receipts)
                self.assertEqual(desktop_api.get_document(self.store, self.principal, self.sid,
                    "programme", saved["_id"]), saved)

    def test_event_assignment_sources_and_playback_reopen_with_strict_station_references(self):
        from sdbase import desktop_playlist
        playlist = desktop_playlist.create_playlist(self.store, self.principal, self.sid,
                                                    "Evente atanmış kaynak", idempotency_key=str(uuid4()))
        playlist_before = self.store.get("playlists", playlist["_id"])
        playback = {"player": 3, "next_cluster_id": str(uuid4())}
        source = {"id": str(uuid4()), "type": "playlist", "source_id": playlist["_id"]}
        recipe = {"schema": 1, "title": "Atanmış event", "kind": "music",
                  "sources": [source], "playback": playback}
        config = copy.deepcopy(self.config)
        block = {"id": str(uuid4()), "kind": "music", "title": "Atanmış event",
                 "start_time": "", "note": "", "playback": playback, "local_event": recipe}
        config["blocks"] = [block]
        config["events"][0]["block_id"] = block["id"]
        saved = desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Atanmış event",
            config, str(uuid4()), document_id=self.document["_id"], expected_revision=self.document["_rev"])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(desktop_api.get_document(reopened, self.principal, self.sid,
                                                  "programme", saved["_id"])["config"], config)
        self.assertEqual(self.store.get("playlists", playlist["_id"]), playlist_before)
        before = self.store.sequence, self.store.scan("desktop_mutations")
        broken_blocks = [dict(block, playback=None), dict(block, playback={"player": True, "next_cluster_id": ""}),
                         dict(block, playback={"player": 33, "next_cluster_id": ""}),
                         dict(block, local_event=dict(recipe, sources=[dict(source, source_id=str(uuid4()))])),
                         dict(block, local_event=dict(recipe, sources=[dict(source, type="local_event")])),
                         dict(block, local_event=dict(recipe, unexpected=True)),
                         dict(block, local_event=dict(recipe, playback={"player": 0, "next_cluster_id": "bad"}))]
        for invalid in broken_blocks:
            with self.subTest(block=invalid):
                broken = copy.deepcopy(config); broken["blocks"] = [invalid]
                with self.assertRaises(StoreError):
                    desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Atanmış event",
                        broken, str(uuid4()), document_id=saved["_id"], expected_revision=saved["_rev"])
                self.assertEqual((self.store.sequence, self.store.scan("desktop_mutations")), before)
        desktop_playlist.delete_playlist(self.store, self.principal, self.sid, playlist["_id"],
            expected_revision=playlist["_rev"], idempotency_key=str(uuid4()))
        config["blocks"][0]["title"] = "Kaynağı kapanmış event incelenebilir"
        updated = desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Atanmış event",
            config, str(uuid4()), document_id=saved["_id"], expected_revision=saved["_rev"])
        self.assertEqual(updated["config"], config)
        config["blocks"][0]["local_event"]["sources"].append(dict(source, id=str(uuid4())))
        with self.assertRaises(StoreError):
            desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Atanmış event",
                config, str(uuid4()), document_id=updated["_id"], expected_revision=updated["_rev"])
        self.assertEqual(inspect_integrity(open_radio_store(self.fixture.db, read_only=True))["status"], "ok")

    def test_atomic_submission_replay_and_real_prerequisite_rejection(self):
        seq = self.store.sequence; key = str(uuid4()); result = self.submit(idempotency_key=key)
        self.assertEqual(self.store.sequence, seq + 1)
        self.assertEqual(self.submit(idempotency_key=key, expected_plan_revision=None), result)
        self.assertEqual(self.store.sequence, seq + 1)
        overview = self.overview(); reasons = {row["code"] for row in overview["blockers"]}
        self.assertTrue({"version_not_approved", "item_not_active", "station_not_eligible", "rights_unconfirmed_for_slot"} <= reasons)
        with self.assertRaises(Conflict): self.approve()
        self.assertEqual(self.store.get("versions", self.binding["version_id"])["status"], "draft")
        self.assertFalse(self.store.scan("rights_grants")); self.assertEqual(overview["revision"]["status"], "draft")
        self.assertFalse(self.store.scan("plan_distributions"))

    def test_approved_snapshot_current_revision_sha_and_explicit_distribution(self):
        self.submit(); self.authorize_media(); self.approve(); guard = self.guarded()
        self.assertEqual(self.overview()["revision"]["status"], "approved")
        seq = self.store.sequence
        prepared = api.prepare(self.store, self.principal, self.sid, endpoint_id=self.fixture.identity["endpoint_id"], **guard)
        self.assertEqual(self.store.sequence, seq); self.assertFalse(prepared["delivery_proven"])
        self.assertEqual(prepared["formal_snapshot"]["plan_revision_id"], guard["revision_id"])
        track = prepared["tracks"][0]; self.assertTrue(track["eligible"])
        frozen = prepared["formal_snapshot"]["entries"][0]["playback_markers"]
        self.assertEqual(track["detail"]["markers"]["out_ms"], frozen["cue_out_ms"])
        self.assertFalse(self.store.scan("plan_distributions")); self.assertFalse(self.store.scan("playout_events"))
        distribution = api.distribute(self.store, self.principal, self.sid, endpoint_id=self.fixture.identity["endpoint_id"], **guard)
        self.assertFalse(distribution["delivery_proven"]); self.assertEqual(len(self.store.scan("plan_distributions")), 1)
        path = self.media["version"]["managed_path"]
        from pathlib import Path
        Path(path).write_bytes(b"changed")
        with self.assertRaises(Conflict): api.prepare(self.store, self.principal, self.sid, endpoint_id=self.fixture.identity["endpoint_id"], **self.guarded())

    def test_cas_edit_fork_preserves_approved_revision_and_reopen(self):
        self.submit(); self.authorize_media(); self.approve(); old = self.overview(); guard = self.guarded()
        frozen_revision = copy.deepcopy(old["revision"]); frozen_entries = copy.deepcopy(old["entries"])
        with self.assertRaises(Conflict): self.submit()
        api.fork(self.store, self.principal, self.sid, **guard)
        with self.assertRaises(Conflict): api.fork(self.store, self.principal, self.sid, **guard)
        self.config["note"] = "Yeni taslak"
        self.document = desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Mevcut program", self.config, str(uuid4()), document_id=self.document["_id"], expected_revision=self.document["_rev"])
        self.submit(); current = self.overview()
        self.assertNotEqual(current["revision"]["_id"], frozen_revision["_id"])
        self.assertEqual(self.store.get("plan_revisions", frozen_revision["_id"]), frozen_revision)
        self.assertEqual(self.store.lookup("plan_entries", "revision", frozen_revision["_id"]), frozen_entries)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        view = api.overview(reopened, self.principal, self.sid, self.document["_id"])
        self.assertEqual(view["revision"]["native_source"]["document_revision"], self.document["_rev"])
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_permission_foreign_scope_and_changed_document(self):
        viewer = create_station_user(self.store, self.principal, self.sid, "viewer", "Viewer", ["view"])
        principal = authenticate(self.store, viewer["token"])
        api.overview(self.store, principal, self.sid, self.document["_id"])
        with self.assertRaises(Forbidden): api.submit(self.store, principal, self.sid, self.document["_id"], self.document["_rev"], str(uuid4()))
        self.submit(); self.authorize_media(); guard = self.guarded()
        self.config["note"] = "Yeni girdi"
        self.document = desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Mevcut program", self.config, str(uuid4()), document_id=self.document["_id"], expected_revision=self.document["_rev"])
        with self.assertRaises(Conflict): api.approve(self.store, self.principal, self.sid, **guard)
        reasons = {row["code"] for row in self.overview()["blockers"]}
        self.assertIn("native_source_changed", reasons)
        with self.assertRaises(Conflict): self.submit(expected_document_revision=self.document["_rev"] - 1)
        with self.assertRaises(Conflict): api.approve(self.store, self.principal, self.sid, **dict(guard, revision_id=str(uuid4())))

    def test_native_prepare_requires_operate_and_matching_saved_source(self):
        self.submit(); self.authorize_media(); self.approve(); guard = self.guarded()
        activated = create_station_user(self.store, self.principal, self.sid, "activator", "Endpoint activator", ["view", "activate"])
        principal = authenticate(self.store, activated["token"])
        with self.assertRaises(Forbidden):
            api.prepare(self.store, principal, self.sid, endpoint_id=self.fixture.identity["endpoint_id"], **guard)
        self.config["note"] = "Changed after approval"
        self.document = desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Mevcut program", self.config, str(uuid4()), document_id=self.document["_id"], expected_revision=self.document["_rev"])
        with self.assertRaises(Conflict):
            api.prepare(self.store, self.principal, self.sid, endpoint_id=self.fixture.identity["endpoint_id"], **guard)

    def test_prerequisites_are_readonly_bound_and_include_actual_revisions(self):
        sequence = self.store.sequence
        result = api.prerequisites(self.store, self.principal, self.sid, self.document["_id"])
        self.assertEqual(self.store.sequence, sequence)
        record = result["records"][0]
        self.assertEqual(record["version"]["_rev"], self.store.get("versions", self.binding["version_id"])["_rev"])
        self.assertEqual(record["item"]["lifecycle"], "draft")
        self.assertFalse(record["association"]["eligible"])
        self.assertEqual(record["rights"], [])
        self.assertEqual(result["outputs"][0]["output_id"], "LOCAL_MAIN")
        with self.assertRaises(catalog.NotFound): api.prerequisites(self.store, self.principal, self.sid, str(uuid4()))

    def test_native_utc_instant_retained_and_station_date_change_rejected(self):
        for day in ("2026-03-29", "2026-10-25"):
            self.assertEqual(api._native_start(day, "02:30").isoformat(), day + "T02:30:00+00:00")
        station = self.store.get("stations", self.sid)
        self.store.put("stations", self.sid, {key:value for key,value in station.items() if key not in {"_id", "_rev"}} | {"timezone":"Europe/Istanbul"}, expected_revision=station["_rev"])
        self.submit()
        entry = self.overview()["entries"][0]
        self.assertEqual(entry["scheduled_at"], "2026-10-06T08:30:00Z")
        self.assertEqual(api.prerequisites(self.store, self.principal, self.sid, self.document["_id"])["records"][0]["starts_at"], "2026-10-06T08:30:00+00:00")
        self.config["events"][0]["start_time"] = "23:30"
        self.document = desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Mevcut program", self.config, str(uuid4()), document_id=self.document["_id"], expected_revision=self.document["_rev"])
        sequence = self.store.sequence
        with self.assertRaises(StoreError): self.submit()
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.overview()["entries"][0], entry)

    def test_native_cut_finish_timing_rejected_without_mutation(self):
        for policy in ("cut", "finish"):
            self.config["timing"] = {"event_id": self.config["events"][0]["id"], "start_time": "08:00", "target_time": "08:30", "policy": policy, "max_wait_ms": 30000}
            self.document = desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Mevcut program", self.config, str(uuid4()), document_id=self.document["_id"], expected_revision=self.document["_rev"])
            sequence = self.store.sequence
            with self.assertRaisesRegex(StoreError, "cut/finish timing"): self.submit()
            self.assertEqual(self.store.sequence, sequence)
            self.assertFalse(self.store.scan("plans")); self.assertFalse(self.store.scan("plan_revisions"))
            self.assertFalse(self.store.scan("plan_entries")); self.assertFalse(self.store.scan("plan_distributions"))

    def test_native_event_timing_rejected_without_mutation(self):
        for policy, wait in (("cut", 0), ("finish", 0), ("sequential", 1000)):
            self.config["events"][0].update(start_policy=policy, max_wait_ms=wait)
            self.document = desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Mevcut program", self.config, str(uuid4()), document_id=self.document["_id"], expected_revision=self.document["_rev"])
            sequence = self.store.sequence
            with self.assertRaisesRegex(StoreError, "event cut/finish"): self.submit()
            self.assertEqual(self.store.sequence, sequence); self.assertFalse(self.store.scan("plans"))

    def test_native_custom_marker_draft_not_silently_discarded(self):
        desktop_media.save_markers(self.store, self.principal, self.sid, self.binding["media_id"], self.media["media"]["_rev"], None, str(uuid4()),
            {"cue_in_ms": 20, "out_ms": 180, "intro_ms": 0, "fade_start_ms": 180, "fade_end_ms": 180, "auto_next_ms": 180, "overlap_ms": 0, "automatic": False})
        sequence = self.store.sequence
        with self.assertRaisesRegex(StoreError, "native marker draft"): self.submit()
        self.assertEqual(self.store.sequence, sequence); self.assertFalse(self.store.scan("plans"))

    def test_large_formal_plan_local_loader_rejects_before_track_response(self):
        original = self.config["events"][0]
        self.config["events"] = [dict(original, id=str(uuid4()), start_time=f"{(8 * 60 + 30 + i) // 60:02d}:{(8 * 60 + 30 + i) % 60:02d}") for i in range(501)]
        self.document = desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Mevcut program", self.config, str(uuid4()), document_id=self.document["_id"], expected_revision=self.document["_rev"])
        self.submit(); self.authorize_media(); self.approve()
        sequence = self.store.sequence
        with self.assertRaisesRegex(Conflict, "at most 500 entries"):
            api.prepare(self.store, self.principal, self.sid, endpoint_id=self.fixture.identity["endpoint_id"], **self.guarded())
        self.assertEqual(self.store.sequence, sequence)

    def online_event(self, provider, identifier, url):
        event = {key: value for key, value in self.config["events"][0].items()
                 if key not in desktop_api.MEDIA_BINDING_KEYS}
        event.update(id=str(uuid4()), title="Doğrudan servis parçası", start_time="",
                     online_provider=provider, online_id=identifier, online_url=url)
        return event

    def test_event_list_view_settings_persist_and_do_not_block_formal_submission(self):
        view = {"event_fetch": 10, "event_show": 5, "content_fetch": 50, "content_show": 25, "refresh_seconds": 3}
        config = copy.deepcopy(self.config) | {"view": view, "view_by_title": {self.config["title"]: dict(view, event_show=2)}}
        self.document = desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Mevcut program", config,
            str(uuid4()), document_id=self.document["_id"], expected_revision=self.document["_rev"])
        self.assertEqual(self.document["config"]["view"], view)
        result = self.submit()
        self.assertEqual(result["source"]["document_revision"], self.document["_rev"])
        overview = self.overview()
        self.assertTrue(overview["source_matches"]); self.assertEqual(len(overview["entries"]), 1)
        self.assertEqual(self.store.get("desktop_configurations", self.document["_id"])["config"]["view"], view)
        self.assertEqual(inspect_integrity(open_radio_store(self.fixture.db, read_only=True))["status"], "ok")

    def test_online_programme_events_are_retired_and_rejected_without_mutation(self):
        spotify = "4uLU6hMCjMI75M1A2tKUQC"
        valid = self.online_event("spotify", "spotify:track:" + spotify, "https://open.spotify.com/track/" + spotify)
        apple = self.online_event("apple_music", "apple_music:1594677759", "https://music.apple.com/tr/song/1594677759")
        youtube = self.online_event("youtube", "dQw4w9WgXcQ", "https://music.youtube.com/watch?v=dQw4w9WgXcQ")
        retired = [valid, apple, youtube, dict(valid, online_provider="other"),
                   dict(valid, primary_file="/audio.mp3"), dict(valid, **self.binding)]
        for name in ("online_id", "online_url", "online_provider"):
            event = dict(valid); event.pop(name); retired.append(event)
        sequence = self.store.sequence
        receipts = self.store.scan("desktop_mutations")
        for event in retired:
            with self.subTest(provider=event.get("online_provider"), identifier=event.get("online_id"), url=event.get("online_url")):
                config = copy.deepcopy(self.config); config["events"].append(event)
                with self.assertRaisesRegex(StoreError, "no longer supported"):
                    desktop_api.put_document(self.store, self.principal, self.sid, "programme", "Servis sırası", config,
                        str(uuid4()), document_id=self.document["_id"], expected_revision=self.document["_rev"])
                self.assertEqual(self.store.sequence, sequence)
                self.assertEqual(self.store.scan("desktop_mutations"), receipts)
                self.assertEqual(desktop_api.get_document(self.store, self.principal, self.sid, "programme", self.document["_id"]), self.document)
        # A stored legacy event only has to keep its shape and stays readable.
        stored = self.store.get("desktop_configurations", self.document["_id"])
        legacy = copy.deepcopy({key: value for key, value in stored.items() if not key.startswith("_")})
        legacy["config"]["events"].append(valid)
        self.store.put("desktop_configurations", self.document["_id"], legacy, expected_revision=stored["_rev"])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        actual = desktop_api.get_document(reopened, self.principal, self.sid, "programme", self.document["_id"])
        self.assertEqual(actual["config"]["events"][-1], valid)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_retired_provider_programme_put_is_rejected_over_real_authenticated_http(self):
        server = BoundedHTTPServer(("127.0.0.1", 0), make_handler(self.store))
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        def request(action, body):
            connection = HTTPConnection("127.0.0.1", server.server_port, timeout=5)
            connection.request("POST", "/v1/workflows/desktop/" + action, json.dumps(body),
                {"Authorization": "Bearer " + self.fixture.identity["api_token"], "Content-Type": "application/json"})
            response = connection.getresponse(); data = json.loads(response.read()); connection.close()
            return response.status, data
        spotify = "4uLU6hMCjMI75M1A2tKUQC"
        config = copy.deepcopy(self.config)
        config["events"].extend([
            self.online_event("spotify", "spotify:track:" + spotify, "https://open.spotify.com/track/" + spotify),
            self.online_event("apple_music", "apple_music:1594677759", "https://music.apple.com/tr/song/1594677759")])
        body = {"station_id": self.sid, "kind": "programme", "name": "Doğrudan servis sırası", "config": config,
                "idempotency_key": str(uuid4()), "document_id": self.document["_id"], "expected_revision": self.document["_rev"]}
        sequence = self.store.sequence
        status, result = request("put", body); self.assertEqual(status, 422, result)
        self.assertIn("no longer supported", result["detail"])
        self.assertEqual(self.store.sequence, sequence)
        status, current = request("get", {"station_id": self.sid, "kind": "programme", "document_id": self.document["_id"]})
        self.assertEqual(status, 200); self.assertEqual(current, self.document)
        local = dict(body, name="Yerel sıra", config=copy.deepcopy(self.config), idempotency_key=str(uuid4()))
        status, saved = request("put", local); self.assertEqual(status, 200, saved)
        self.assertEqual(saved["config"], self.config)
        self.assertEqual(saved["_rev"], self.document["_rev"] + 1)

    def test_real_http_submit_review_approval_and_negative_method(self):
        server = BoundedHTTPServer(("127.0.0.1", 0), make_handler(self.store)); thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        def request(action, body, method="POST"):
            connection = HTTPConnection("127.0.0.1", server.server_port, timeout=5)
            connection.request(method, "/v1/workflows/desktop-programme/" + action, json.dumps(body), {"Authorization":"Bearer " + self.fixture.identity["api_token"], "Content-Type":"application/json"})
            response = connection.getresponse(); data = json.loads(response.read()); connection.close(); return response.status, data
        base = {"station_id":self.sid, "document_id":self.document["_id"]}
        status, _ = request("overview", base); self.assertEqual(status, 200)
        status, _ = request("submit", {**base, "expected_document_revision":self.document["_rev"], "idempotency_key":str(uuid4())}); self.assertEqual(status, 200)
        status, _ = request("approve", {"station_id":self.sid, **self.guarded()}); self.assertEqual(status, 409)
        self.authorize_media()
        status, _ = request("approve", {"station_id":self.sid, **self.guarded()}); self.assertEqual(status, 200)
        status, _ = request("overview", base, "GET"); self.assertEqual(status, 422)


if __name__ == "__main__": unittest.main()
