#!/usr/bin/env python3
"""Settings › Export/Import: the .st transfer package round trip, inspection, jobs and HTTP."""
from __future__ import annotations

from copy import deepcopy
import json
import os
from pathlib import Path
import re
import sys
import threading
import time
import unittest
from unittest.mock import patch
from uuid import uuid4
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "database/SDBASE/src"))
import test_desktop_api as fixtures
import test_desktop_workflow_bridge as bridge_fixtures
from sdbase import desktop_api, desktop_media, desktop_playlist
from sdbase import desktop_transfer_package as transfer
from sdbase.integrity import inspect_integrity
from sdbase.media import object_path
from sdbase.storage import StoreError
from sdbase.workflow_api import ACTIONS

PREFERENCES = {"schema_version": 1, "ui": {"text_size": 2, "default_profile": 3, "header_hidden_fields": 5},
               "audio_panel": {"sample_rate": 48000, "buffer_frames": 512, "click_sound": 1, "click_volume": 2}}
TRANSITION = {"values": ["120", "0", "", "110", "0", "2", "2", "-10"], "files": ["/path/a.wav", ""],
              "flag": False, "preview_level": -6}
ACTION_NAMES = ("desktop-transfer/groups", "desktop-transfer/export-start", "desktop-transfer/job",
                "desktop-transfer/job-cancel", "desktop-transfer/inspect", "desktop-transfer/import-start")


def automation(playlist_id):
    return {"name": "Otomasyon", "seed": "1", "planDate": "2026-10-13", "startTime": "09:00", "windowStart": "00:00",
            "windowEnd": "23:59", "sourceMode": 0, "trackMode": 0, "transition": 0, "boundaryTransition": 0,
            "deckMode": 0, "fallbackMode": 0, "trackGap": 0, "artistGap": 0, "albumGap": 0, "previewCount": 10,
            "jingleEvery": 2, "overlap": 0, "boundaryOverlap": 0, "includeProgramme": False, "jingleEnabled": False,
            "jingleDry": False, "requireMetadata": False, "jinglePlaylist": None, "fallbackPlaylist": None,
            "jingleDeck": None, "weekdays": [1], "decks": [],
            "sources": [{"source_id": str(uuid4()), "playlist_id": playlist_id, "weight": 1, "count": 1,
                         "start": "00:00", "end": "23:59", "days": 0}]}


def row(title, **extra):
    return {"title": title, "artist": "", "album": "", "duration_ms": 200, "bpm": 0, "genre": "", "key": "",
            "location": "", "comment": "", **extra}


class DesktopTransferPackage(unittest.TestCase):
    http = bridge_fixtures.DesktopWorkflowBridge.http
    local_user = bridge_fixtures.DesktopWorkflowBridge.local_user

    def setUp(self):
        self.fixture = self.station()
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal
        self.root = Path(self.fixture.temp.name).resolve()
        self.out = self.root / "paketler"
        self.out.mkdir()

    def station(self):
        fixture = fixtures.DesktopAPI("runTest")
        fixture.setUp()
        self.addCleanup(fixture.tearDown)
        return fixture

    def wait(self, fixture, job_id, timeout=60):
        deadline = time.monotonic() + timeout
        while True:
            view = transfer.job(fixture.store, fixture.principal, fixture.sid, job_id)
            if view["state"] != "running":
                return view
            self.assertLess(time.monotonic(), deadline, "transfer job did not finish in time")
            time.sleep(0.02)

    def put(self, kind, config, name=None):
        return desktop_api.put_document(self.store, self.principal, self.sid, kind, name or kind, config, str(uuid4()))

    def song(self):
        """One verified song with a marker draft and a rhythm grid, as the desktop stores them."""
        binding = self.fixture.verified_binding()
        detail = desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"])
        markers = {"cue_in_ms": 0, "intro_ms": 30, "out_ms": 200, "fade_start_ms": 180, "fade_end_ms": 200,
                   "auto_next_ms": 190, "overlap_ms": 10, "automatic": False, "autodj_in_ms": 20, "autodj_out_ms": 150}
        desktop_media.save_markers(self.store, self.principal, self.sid, binding["media_id"], detail["media"]["_rev"],
                                   None, str(uuid4()), markers)
        detail = desktop_media.get_media(self.store, self.principal, self.sid, binding["media_id"])
        analysis = deepcopy(detail["analysis"])
        analysis.pop("audible", None)
        analysis["rhythm"] = {"schema": 1, "algorithm": "radios-onset-grid-v1", "bpm": 0, "first_beat_ms": 0,
                              "confidence": 0, "analyzed_ms": 200}
        desktop_media.save_analysis(self.store, self.principal, self.sid, binding["media_id"], binding["version_id"],
                                    detail["media"]["_rev"], detail["analysis_revision"], str(uuid4()), analysis)
        return binding

    def export(self, fixture=None, **options):
        fixture = fixture or self.fixture
        started = transfer.export_start(fixture.store, fixture.principal, fixture.sid, str(self.out), **options)
        self.assertEqual(started["state"], "running")
        finished = self.wait(fixture, started["job_id"])
        self.assertEqual(finished["state"], "done", finished)
        return Path(started["package_path"]), finished["result"]

    def import_package(self, fixture, package, **options):
        started = transfer.import_start(fixture.store, fixture.principal, fixture.sid, str(package), **options)
        self.assertEqual(started["state"], "running")
        finished = self.wait(fixture, started["job_id"])
        self.assertEqual(finished["state"], "done", finished)
        return finished["result"]

    def documents(self, fixture, kind):
        return [row for row in fixture.store.scan("desktop_configurations")
                if row["kind"] == kind and row["status"] == "active" and row["station_id"] == fixture.sid]

    def test_actions_are_registered_with_the_shared_signature(self):
        for name in ACTION_NAMES:
            self.assertIn(name, ACTIONS)
        self.assertIs(ACTIONS["desktop-transfer/groups"], transfer.groups)
        self.assertIs(ACTIONS["desktop-transfer/export-start"], transfer.export_start)
        self.assertIs(ACTIONS["desktop-transfer/job"], transfer.job)
        self.assertIs(ACTIONS["desktop-transfer/job-cancel"], transfer.job_cancel)
        self.assertIs(ACTIONS["desktop-transfer/inspect"], transfer.inspect)
        self.assertIs(ACTIONS["desktop-transfer/import-start"], transfer.import_start)

    def test_groups_cover_every_transferable_kind_with_turkish_labels_and_media_totals(self):
        binding = self.song()
        self.put("channel", {"deck_count": 6})
        self.put("midi_mapping", {"device": "", "maps": []})
        desktop_playlist.create_playlist(self.store, self.principal, self.sid, "Gece", idempotency_key=str(uuid4()))
        result = transfer.groups(self.store, self.principal, self.sid)
        ids = [group["id"] for group in result["groups"]]
        self.assertEqual(set(ids), set(desktop_api.KINDS) - desktop_api.LEGACY_KINDS - set(transfer.desktop_reset.KEPT_KINDS) | {"playlists", "local_settings", "view_profiles", "import_profiles", "campaigns"})
        self.assertEqual(len(ids), len(set(ids)))
        labels = {group["id"]: group["label"] for group in result["groups"]}
        self.assertEqual(labels["audio_preferences"], "Ses tercihleri")
        self.assertEqual(labels["automation"], "Rotasyonlar ve şemalar")
        self.assertEqual(labels["playlists"], "Playlistler")
        self.assertTrue(all(label for label in labels.values()))
        counts = {group["id"]: group["count"] for group in result["groups"] if group["count"]}
        self.assertEqual(counts, {"channel": 1, "midi_mapping": 1, "playlists": 1})
        size = object_path(self.store, binding["media_sha256"]).stat().st_size
        self.assertEqual(result["media"], {"count": 1, "bytes": size})
        self.assertNotIn("playout_history", labels)
        self.assertNotIn("online_services", labels)

    def test_round_trip_restores_settings_playlists_and_media_into_a_new_store(self):
        binding = self.song()
        digest = binding["media_sha256"]
        playlist = desktop_playlist.create_playlist(self.store, self.principal, self.sid, "Sabah", idempotency_key=str(uuid4()),
            entries=[row("Bağlı", **binding), row("Yalnız metin")])
        plain = [self.put("channel", {"deck_count": 6}), self.put("dj_mixer", fixtures.dj_configuration()),
                 self.put("audio_preferences", fixtures.audio_preferences_configuration()),
                 self.put("desktop_preferences", PREFERENCES), self.put("midi_mapping", {"device": "", "maps": []}),
                 self.put("automation", automation(playlist["_id"]))]
        bound = [self.put("sampler", {"pads": [{"key": "channel-1-pad-1", "title": "Pad", "file": "/draft.wav", **binding}]}),
                 self.put("transition", TRANSITION | {"media_a": dict(binding)}),
                 self.put("programme", {"title": "Sabah", "date": "2026-10-18", "note": "", "events": [
                     {"id": str(uuid4()), "title": "Parça", "artist": "", "album": "", "kind": "music", "start_time": "",
                      "duration_ms": 200, "version_label": "", "primary_file": "", "backup_file": "", "note": "", **binding}]})]
        sequence = self.store.sequence
        package, result = self.export(settings="all", media=True)
        self.assertEqual(self.store.sequence, sequence)
        self.assertRegex(package.name, r"^RadioS-Test-station-\d{8}-\d{4}\.st$")
        self.assertEqual(result["warnings"], [])
        self.assertEqual(result["media"]["count"], 1)
        self.assertFalse(list(self.out.glob(".*")))
        size = object_path(self.store, digest).stat().st_size
        with zipfile.ZipFile(package) as zf:
            names = set(zf.namelist())
            self.assertIsNone(zf.testzip())
            self.assertEqual({name.split("/", 1)[0] for name in names}, {"ayarlar", "medya", "bilgiler"})
            self.assertEqual({name for name in names if name.startswith("ayarlar/")},
                             {f"ayarlar/{kind}.json" for kind in ("channel", "dj_mixer", "audio_preferences", "desktop_preferences",
                                                                 "midi_mapping", "automation", "sampler", "transition", "programme", "playlists")})
            self.assertIn("medya/Albümsüz/sampler.wav", names)
            self.assertEqual(zf.getinfo("medya/Albümsüz/sampler.wav").compress_type, zipfile.ZIP_STORED)
            self.assertEqual(zf.getinfo("ayarlar/channel.json").compress_type, zipfile.ZIP_DEFLATED)
            self.assertEqual(zf.read("medya/Albümsüz/sampler.wav"), object_path(self.store, digest).read_bytes())
            manifest = json.loads(zf.read("bilgiler/manifest.json"))
            self.assertEqual((manifest["format"], manifest["version"]), ("radios-transfer-package", 1))
            self.assertEqual(manifest["station"], {"id": self.sid, "name": "Test station"})
            self.assertEqual(manifest["scope"], {"settings": "all", "kinds": list(transfer.GROUPS), "media": True})
            self.assertEqual(manifest["media"], {"count": 1, "bytes": size})
            self.assertEqual(manifest["redacted"], [])
            self.assertEqual(manifest["schema_version"], self.store.schema_version)
            self.assertEqual({(entry["kind"], entry["count"]) for entry in manifest["settings"]},
                             {("channel", 1), ("dj_mixer", 1), ("audio_preferences", 1), ("desktop_preferences", 1), ("midi_mapping", 1),
                              ("automation", 1), ("sampler", 1), ("transition", 1), ("programme", 1), ("playlists", 1)})
            songs = json.loads(zf.read("bilgiler/medya.json"))["songs"]
            self.assertEqual(len(songs), 1)
            self.assertEqual((songs[0]["sha256"], songs[0]["size_bytes"], songs[0]["path"], songs[0]["file_name"]),
                             (digest, size, "medya/Albümsüz/sampler.wav", "sampler.wav"))
            self.assertEqual((songs[0]["title"], songs[0]["kind"], songs[0]["media_file_id"], songs[0]["library_id"]),
                             ("Gerçek DJ sampler", "jingle", binding["media_file_id"], binding["media_id"]))
            self.assertEqual(songs[0]["analysis"], f"bilgiler/analiz/{digest}.json")
            records = json.loads(zf.read(songs[0]["analysis"]))
            self.assertEqual(records["analysis"]["analysis"]["sha256"], digest)
            self.assertEqual(records["markers"]["intro_ms"], 30)
            self.assertEqual(records["rhythm"]["rhythm"]["algorithm"], "radios-onset-grid-v1")
            for record in records.values():
                self.assertFalse({"_id", "_rev", "station_id", "media_file_id", "created_by"} & set(record))
            saved = json.loads(zf.read("ayarlar/channel.json"))
            self.assertEqual(saved["kind"], "channel")
            self.assertEqual(saved["documents"], [{"id": plain[0]["_id"], "name": "channel", "revision": plain[0]["_rev"], "config": {"deck_count": 6}}])
            lists = json.loads(zf.read("ayarlar/playlists.json"))["documents"]
            self.assertEqual([entry["sha256"] for entry in lists[0]["config"]["entries"]], [digest, None])
            self.assertEqual(lists[0]["id"], playlist["_id"])
            self.assertEqual(zf.read("bilgiler/manifest.json").decode()[-1], "\n")
        inspected = transfer.inspect(self.store, self.principal, self.sid, str(package))
        self.assertEqual(inspected["media"], {"count": 1, "bytes": size})
        self.assertEqual(inspected["warnings"], [])
        self.assertEqual({entry["kind"] for entry in inspected["settings"]}, {entry["kind"] for entry in manifest["settings"]})
        self.assertEqual(inspected["manifest"]["station"]["id"], self.sid)

        target = self.station()
        media_root = Path(target.temp.name).resolve() / "müzik"
        media_root.mkdir()
        imported = self.import_package(target, package, settings=True, media=True, media_root=str(media_root))
        self.assertEqual(imported["warnings"], [])
        self.assertEqual(imported["media"], {"placed": 1, "registered": 1, "reused": 0, "skipped": 0, "bytes": size})
        self.assertEqual(imported["playlists"], {"created": 1, "updated": 0, "skipped": 0})
        self.assertEqual(imported["entries"], {"bound": 1, "unbound": 0})
        self.assertEqual(imported["documents"], {"created": 9, "updated": 0, "skipped": 0})
        placed = media_root / "Albümsüz" / "sampler.wav"
        self.assertEqual(placed.read_bytes(), object_path(self.store, digest).read_bytes())
        self.assertEqual(object_path(target.store, digest).read_bytes(), placed.read_bytes())
        listing = desktop_media.list_media(target.store, target.principal, target.sid)
        self.assertEqual(listing["total"], 1)
        record = listing["records"][0]
        self.assertEqual(record["version"]["sha256"], digest)
        self.assertEqual(record["media"]["_id"], binding["media_id"])  # free identities are kept
        self.assertEqual(record["version"]["media_file_id"], binding["media_file_id"])
        detail = desktop_media.get_media(target.store, target.principal, target.sid, record["media"]["_id"])
        self.assertTrue(detail["version"]["object_available"])
        self.assertEqual(detail["analysis"]["rhythm"]["algorithm"], "radios-onset-grid-v1")
        self.assertEqual((detail["markers"]["intro_ms"], detail["markers"]["autodj_in_ms"], detail["markers"]["status"]), (30, 20, "draft"))
        self.assertEqual(detail["markers"]["created_by"], target.principal.user_id)
        for document in plain + bound:
            restored = target.store.get("desktop_configurations", document["_id"])
            self.assertEqual((restored["kind"], restored["name"], restored["status"], restored["station_id"]),
                             (document["kind"], document["name"], "active", target.sid))
            self.assertEqual(restored["config"], document["config"])
            self.assertEqual(restored["created_by"], target.principal.user_id)
        snapshot = desktop_playlist.list_playlists(target.store, target.principal, target.sid)["playlists"]
        self.assertEqual([(entry["_id"], entry["name"]) for entry in snapshot], [(playlist["_id"], "Sabah")])
        tracks = snapshot[0]["tracks"]
        self.assertEqual([track["title"] for track in tracks], ["Bağlı", "Yalnız metin"])
        self.assertEqual([track["_id"] for track in tracks], [track["_id"] for track in playlist["tracks"]])
        self.assertEqual({key: tracks[0][key] for key in desktop_api.MEDIA_BINDING_KEYS}, binding)
        self.assertEqual(tracks[0]["location"], str(placed))
        self.assertNotIn("media_sha256", tracks[1])
        self.assertEqual(inspect_integrity(target.store)["status"], "ok")

        # Importing again finds the same bytes and identities: nothing is duplicated.
        again = self.import_package(target, package, settings=True, media=True, media_root=str(media_root))
        self.assertEqual(again["warnings"], [])
        self.assertEqual(again["media"], {"placed": 0, "registered": 0, "reused": 1, "skipped": 0, "bytes": 0})
        self.assertEqual(again["playlists"], {"created": 0, "updated": 1, "skipped": 0})
        self.assertEqual(again["documents"], {"created": 0, "updated": 9, "skipped": 0})
        self.assertEqual(desktop_media.list_media(target.store, target.principal, target.sid)["total"], 1)
        self.assertEqual(len([row for row in target.store.scan("desktop_configurations") if row["status"] == "active"]), 9)
        snapshot = desktop_playlist.list_playlists(target.store, target.principal, target.sid)["playlists"]
        self.assertEqual([track["title"] for track in snapshot[0]["tracks"]], ["Bağlı", "Yalnız metin"])
        self.assertEqual(target.store.get("desktop_configurations", plain[0]["_id"])["_rev"], 2)
        self.assertFalse(list(media_root.rglob(".*")))
        self.assertEqual(inspect_integrity(target.store)["status"], "ok")

    def test_selected_group_export_and_kind_filtered_import(self):
        channel = self.put("channel", {"deck_count": 4})
        self.put("midi_mapping", {"device": "", "maps": []})
        package, result = self.export(settings="selected", kinds=["channel"], media=False, file_name="kanal.st")
        self.assertEqual(package, self.out / "kanal.st")
        self.assertEqual(result["media"], {"count": 0, "bytes": 0})
        with zipfile.ZipFile(package) as zf:
            self.assertEqual(set(zf.namelist()), {"ayarlar/channel.json", "bilgiler/medya.json", "bilgiler/manifest.json"})
            manifest = json.loads(zf.read("bilgiler/manifest.json"))
            self.assertEqual(manifest["scope"], {"settings": "selected", "kinds": ["channel"], "media": False})
            self.assertEqual(json.loads(zf.read("bilgiler/medya.json")), {"version": 1, "songs": []})
        inspected = transfer.inspect(self.store, self.principal, self.sid, str(package))
        self.assertEqual(inspected["settings"], [{"kind": "channel", "label": "DJ kanalları", "count": 1}])
        self.assertEqual(inspected["media"], {"count": 0, "bytes": 0})
        target = self.station()
        imported = self.import_package(target, package, settings=True, kinds=["channel", "midi_mapping"], media=False)
        self.assertEqual(imported["documents"], {"created": 1, "updated": 0, "skipped": 0})
        self.assertEqual(imported["media_root"], None)
        self.assertEqual(target.store.get("desktop_configurations", channel["_id"])["config"], {"deck_count": 4})
        self.assertEqual(self.documents(target, "midi_mapping"), [])
        self.assertEqual(len(self.documents(target, "channel")), 1)
        # A second export keeps the first package: equal names are refused.
        with self.assertRaises(StoreError):
            transfer.export_start(self.store, self.principal, self.sid, str(self.out), settings="selected", kinds=["channel"],
                                  media=False, file_name="kanal.st")

    def test_selected_documents_export_only_the_chosen_records_of_each_group(self):
        """Guide §12: "seçili radyo ayarları" may be single documents inside a group, not only whole groups."""
        from sdbase import settings
        evening = desktop_playlist.create_playlist(self.store, self.principal, self.sid, "akşam listesi", idempotency_key=str(uuid4()))
        morning = desktop_playlist.create_playlist(self.store, self.principal, self.sid, "Sabah listesi", idempotency_key=str(uuid4()))
        first = self.put("midi_mapping", {"device": "", "maps": []}, name="Denetleyici B")
        second = self.put("midi_mapping", {"device": "", "maps": []}, name="denetleyici a")
        channel = self.put("channel", {"deck_count": 4})
        kept = settings.put_import_profile(self.store, self.principal, self.sid, name="Arşiv", library_key="main", config={})
        settings.put_import_profile(self.store, self.principal, self.sid, name="USB", library_key="usb", config={})
        listed = {group["id"]: group for group in transfer.groups(self.store, self.principal, self.sid)["groups"]}
        # Every group lists what its export carries, ordered by name regardless of case.
        self.assertEqual(listed["playlists"]["documents"], [{"id": evening["_id"], "name": "akşam listesi"},
                                                            {"id": morning["_id"], "name": "Sabah listesi"}])
        self.assertEqual(listed["midi_mapping"]["documents"], [{"id": second["_id"], "name": "denetleyici a"},
                                                               {"id": first["_id"], "name": "Denetleyici B"}])
        self.assertEqual(listed["channel"]["documents"], [{"id": channel["_id"], "name": "channel"}])
        self.assertEqual([row["name"] for row in listed["import_profiles"]["documents"]], ["Arşiv", "USB"])
        self.assertEqual(listed["local_settings"]["documents"], [])
        for group in listed.values():
            self.assertEqual(group["count"], len(group["documents"]), group["id"])

        chosen = {"playlists": [morning["_id"]], "midi_mapping": [first["_id"]], "import_profiles": [kept["_id"]]}
        kinds = ["midi_mapping", "playlists", "channel", "import_profiles"]
        package, result = self.export(settings="selected", kinds=kinds, media=False, file_name="secili-belgeler.st",
                                      documents=chosen)
        self.assertEqual(result["warnings"], [])
        self.assertEqual({(entry["kind"], entry["count"]) for entry in result["settings"]},
                         {("midi_mapping", 1), ("playlists", 1), ("channel", 1), ("import_profiles", 1)})
        with zipfile.ZipFile(package) as zf:
            self.assertEqual({name for name in zf.namelist() if name.startswith("ayarlar/")},
                             {f"ayarlar/{kind}.json" for kind in kinds})
            stored = {kind: [row["id"] for row in json.loads(zf.read(f"ayarlar/{kind}.json"))["documents"]] for kind in kinds}
            self.assertEqual(stored, {"midi_mapping": [first["_id"]], "playlists": [morning["_id"]],
                                      "channel": [channel["_id"]], "import_profiles": [kept["_id"]]})
            manifest = json.loads(zf.read("bilgiler/manifest.json"))
            self.assertEqual(manifest["scope"], {"settings": "selected", "kinds": [kind for kind in transfer.GROUPS if kind in kinds],
                                                 "media": False, "documents": chosen})
        inspected = transfer.inspect(self.store, self.principal, self.sid, str(package))
        self.assertEqual({(entry["kind"], entry["count"]) for entry in inspected["settings"]},
                         {("midi_mapping", 1), ("playlists", 1), ("channel", 1), ("import_profiles", 1)})

        target = self.station()
        imported = self.import_package(target, package, settings=True, media=False)
        self.assertEqual(imported["warnings"], [])
        self.assertEqual(imported["playlists"], {"created": 1, "updated": 0, "skipped": 0})
        self.assertEqual(imported["documents"], {"created": 2, "updated": 0, "skipped": 0})
        self.assertEqual(imported["profiles"], {"created": 1, "updated": 0, "skipped": 0})
        self.assertEqual([row["_id"] for row in self.documents(target, "midi_mapping")], [first["_id"]])
        self.assertEqual(target.store.get("desktop_configurations", first["_id"])["name"], "Denetleyici B")
        self.assertIsNone(target.store.get("desktop_configurations", second["_id"]))
        self.assertEqual([row["_id"] for row in self.documents(target, "channel")], [channel["_id"]])
        restored = desktop_playlist.list_playlists(target.store, target.principal, target.sid)["playlists"]
        self.assertEqual([(row["_id"], row["name"]) for row in restored], [(morning["_id"], "Sabah listesi")])
        self.assertEqual([row["name"] for row in settings.list_import_profiles(target.store, target.principal, target.sid)], ["Arşiv"])
        self.assertEqual(inspect_integrity(target.store)["status"], "ok")

        # A chosen identity that is gone or disabled is reported, the rest still travels.
        desktop_api.delete_document(self.store, self.principal, self.sid, "midi_mapping", second["_id"], second["_rev"], str(uuid4()))
        self.assertEqual(transfer.groups(self.store, self.principal, self.sid)["groups"][transfer.GROUPS.index("midi_mapping")]["documents"],
                         [{"id": first["_id"], "name": "Denetleyici B"}])
        unknown = str(uuid4())
        package, result = self.export(settings="selected", kinds=["midi_mapping", "playlists"], media=False, file_name="eksik.st",
                                      documents={"midi_mapping": [second["_id"], unknown, first["_id"].upper()],
                                                 "playlists": [unknown]})
        self.assertEqual(len(result["warnings"]), 3)
        self.assertTrue(all("seçili kayıt bulunamadı veya etkin değil" in warning for warning in result["warnings"]))
        self.assertEqual([warning.split(":", 1)[0] for warning in result["warnings"]], ["MIDI eşlemeleri", "MIDI eşlemeleri", "Playlistler"])
        self.assertIn(second["_id"], result["warnings"][0])
        self.assertIn(unknown, result["warnings"][1])
        self.assertEqual([entry["kind"] for entry in result["settings"]], ["midi_mapping"])
        with zipfile.ZipFile(package) as zf:
            self.assertNotIn("ayarlar/playlists.json", zf.namelist())
            self.assertEqual([row["id"] for row in json.loads(zf.read("ayarlar/midi_mapping.json"))["documents"]], [first["_id"]])
            self.assertEqual(json.loads(zf.read("bilgiler/manifest.json"))["scope"]["documents"],
                             {"midi_mapping": [second["_id"], unknown, first["_id"]], "playlists": [unknown]})

        # Refusals are Turkish, start no job and write nothing.
        identifier = first["_id"]
        sequence = self.store.sequence
        for options in ({"settings": "all", "documents": {"midi_mapping": [identifier]}},
                        {"settings": "all", "documents": {}},
                        {"settings": "none", "media": True, "documents": {"midi_mapping": [identifier]}},
                        {"settings": "selected", "kinds": ["channel"], "documents": {"midi_mapping": [identifier]}},
                        {"settings": "selected", "kinds": ["midi_mapping"], "documents": {"tokens": [identifier]}},
                        {"settings": "selected", "kinds": ["midi_mapping"], "documents": [identifier]},
                        {"settings": "selected", "kinds": ["midi_mapping"], "documents": {"midi_mapping": identifier}},
                        {"settings": "selected", "kinds": ["midi_mapping"], "documents": {"midi_mapping": []}},
                        {"settings": "selected", "kinds": ["midi_mapping"], "documents": {"midi_mapping": ["belge"]}},
                        {"settings": "selected", "kinds": ["midi_mapping"], "documents": {"midi_mapping": [7]}},
                        {"settings": "selected", "kinds": ["midi_mapping"], "documents": {"midi_mapping": [identifier, identifier.upper()]}},
                        {"settings": "selected", "kinds": ["midi_mapping"],
                         "documents": {"midi_mapping": [str(uuid4()) for _ in range(transfer.MAX_GROUP_DOCUMENTS + 1)]}},
                        {"settings": "selected", "kinds": ["local_settings"], "documents": {"local_settings": [identifier]}}):
            with self.subTest(options=options), self.assertRaisesRegex(StoreError, "[Bb]elge"):
                transfer.export_start(self.store, self.principal, self.sid, str(self.out),
                                      **({"media": False, "file_name": "red.st"} | options))
        self.assertEqual(sorted(path.name for path in self.out.iterdir()), ["eksik.st", "secili-belgeler.st"])
        self.assertEqual(self.store.sequence, sequence)

    def test_settings_only_import_strips_unresolved_media_bindings(self):
        binding = self.song()
        playlist = desktop_playlist.create_playlist(self.store, self.principal, self.sid, "Bağlı liste", idempotency_key=str(uuid4()),
            entries=[row("Bağlı", **binding)])
        sampler = self.put("sampler", {"pads": [{"key": "channel-1-pad-1", "title": "Pad", "file": "/draft.wav", **binding}]})
        transition = self.put("transition", TRANSITION | {"media_a": dict(binding)})
        package, _result = self.export(settings="all", media=False)
        with zipfile.ZipFile(package) as zf:
            self.assertFalse([name for name in zf.namelist() if name.startswith("medya/")])
        target = self.station()
        imported = self.import_package(target, package, settings=True, media=False)
        self.assertEqual(imported["documents"], {"created": 2, "updated": 0, "skipped": 0})
        self.assertEqual(imported["entries"], {"bound": 0, "unbound": 1})
        self.assertEqual(len(imported["warnings"]), 2)
        self.assertTrue(all("medya bağı çözülemedi" in warning for warning in imported["warnings"]))
        self.assertEqual(target.store.get("desktop_configurations", sampler["_id"])["config"],
                         {"pads": [{"key": "channel-1-pad-1", "title": "Pad", "file": "/draft.wav"}]})
        self.assertEqual(target.store.get("desktop_configurations", transition["_id"])["config"], TRANSITION)
        tracks = desktop_playlist.list_playlists(target.store, target.principal, target.sid)["playlists"][0]["tracks"]
        self.assertEqual([track["title"] for track in tracks], ["Bağlı"])
        self.assertFalse(desktop_api.MEDIA_BINDING_KEYS & set(tracks[0]))
        self.assertEqual(inspect_integrity(target.store)["status"], "ok")
        # The target already holding the same bytes rebinds settings without media in the package.
        binding_two = target.verified_binding()
        self.assertEqual(binding_two["media_sha256"], binding["media_sha256"])
        imported = self.import_package(target, package, settings=True, media=False)
        self.assertEqual(imported["warnings"], [])
        self.assertEqual(imported["entries"], {"bound": 1, "unbound": 0})
        restored = target.store.get("desktop_configurations", sampler["_id"])["config"]
        self.assertEqual({key: restored["pads"][0][key] for key in desktop_api.MEDIA_BINDING_KEYS}, binding_two)
        self.assertEqual(target.store.get("desktop_configurations", transition["_id"])["config"]["media_a"], binding_two)
        self.assertEqual(inspect_integrity(target.store)["status"], "ok")

    def test_station_credentials_round_trip_in_private_local_package(self):
        stream = self.put("broadcast_stream", {"url": "https://radyo.example/canli", "username": "fixture-account", "bitrate_kbps": 128,
                                             "password_saved": True, "password": "fixture-" + uuid4().hex})
        package, result = self.export(settings="selected", kinds=["broadcast_stream"], media=False)
        self.assertEqual(result["redacted"], [])
        self.assertEqual(package.stat().st_mode & 0o777, 0o600)
        with zipfile.ZipFile(package) as zf:
            saved = json.loads(zf.read("ayarlar/broadcast_stream.json"))["documents"][0]["config"]
            self.assertEqual(saved, stream["config"])
        target = self.station()
        imported = self.import_package(target, package, settings=True, media=False)
        self.assertEqual(imported["warnings"], [])
        self.assertEqual(target.store.get("desktop_configurations", stream["_id"])["config"], stream["config"])

    def test_cross_station_identity_collisions_preserve_relationships_and_reimport(self):
        playlist = desktop_playlist.create_playlist(self.store, self.principal, self.sid, "Source playlist", idempotency_key=str(uuid4()))
        source = self.put("automation", automation(playlist["_id"]))
        package, _ = self.export(settings="all", media=False)
        target_id = str(uuid4())
        self.store.put("stations", target_id, {"code": "TRANSFER", "name": "Transfer target", "timezone": "UTC", "status": "active"})
        from types import SimpleNamespace
        target = SimpleNamespace(store=self.store, principal=self.principal, sid=target_id)
        for index in range(2):
            imported = self.import_package(target, package, settings=True, media=False)
            self.assertEqual(imported["warnings"], [])
            restored_lists = desktop_playlist.list_playlists(self.store, self.principal, target_id)["playlists"]
            self.assertEqual(len(restored_lists), 1)
            docs = self.documents(target, "automation")
            self.assertEqual(len(docs), 1)
            self.assertEqual(docs[0]["config"]["sources"][0]["playlist_id"], restored_lists[0]["_id"])
            self.assertNotEqual(restored_lists[0]["_id"], playlist["_id"])
            self.assertEqual(self.store.get("desktop_configurations", source["_id"])["config"], source["config"])
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_export_and_import_refuse_without_free_space(self):
        """Packages and placed media must leave the disk usable; refusal starts no job and writes nothing."""
        self.song()
        self.put("midi_mapping", {"device": "", "maps": []})
        package, _ = self.export(settings="all", media=True, file_name="tam.st")
        target = self.station()
        media_root = self.root / "yerlesim"
        media_root.mkdir()
        margin = transfer.desktop_reset.SPACE_MARGIN_BYTES
        self.addCleanup(setattr, transfer.desktop_reset, "SPACE_MARGIN_BYTES", margin)
        transfer.desktop_reset.SPACE_MARGIN_BYTES = 2 ** 62
        with self.assertRaisesRegex(StoreError, "Dışa aktarma için yeterli boş alan yok"):
            transfer.export_start(self.store, self.principal, self.sid, str(self.out), settings="all", media=True,
                                  file_name="dolu.st")
        self.assertEqual(sorted(path.name for path in self.out.iterdir()), ["tam.st"])
        with self.assertRaisesRegex(StoreError, "İçe aktarma için yeterli boş alan yok"):
            transfer.import_start(target.store, target.principal, target.sid, str(package), settings=True,
                                  media=True, media_root=str(media_root))
        self.assertEqual(list(media_root.iterdir()), [])
        transfer.desktop_reset.SPACE_MARGIN_BYTES = margin
        # The same requests succeed once there is room; no refused job blocks them.
        result = self.import_package(target, package, settings=True, media=True, media_root=str(media_root))
        self.assertEqual(result["media"]["placed"], 1)

    def test_named_view_and_import_profiles_round_trip_with_category_relationship(self):
        from sdbase import settings
        category_id = str(uuid4())
        self.store.put("categories", category_id, {"station_id": self.sid, "code": "MUSIC", "name": "Music"})
        view = settings.put_view_profile(self.store, self.principal, self.sid, name="Music view", view_kind="library",
                                        columns=["title", "artist"], filters={"category_id": category_id, "kind": "music"})
        profile = settings.put_import_profile(self.store, self.principal, self.sid, name="Import defaults", library_key="main", config={})
        package, result = self.export(settings="all", media=False)
        target = self.station()
        for index in range(2):
            result = self.import_package(target, package, settings=True, media=False)
            self.assertEqual(result["warnings"], [])
            views = settings.list_view_profiles(target.store, target.principal, target.sid)
            profiles = settings.list_import_profiles(target.store, target.principal, target.sid)
            self.assertEqual(len(views), 1)
            self.assertEqual(len(profiles), 1)
            self.assertEqual(views[0]["columns"], ["title", "artist"])
            self.assertEqual(views[0]["filters"], {"category_id": category_id, "kind": "music"})
            self.assertEqual(views[0]["owner_user_id"], target.principal.user_id)
            self.assertEqual(target.store.get("categories", category_id)["station_id"], target.sid)
        from sdbase import desktop_reset
        reset = desktop_reset.apply(target.store, target.principal, target.sid, ["factory"], "SIFIRLA", str(uuid4()), backup=False)
        self.assertEqual(reset["profiles"], 2)
        self.assertEqual(settings.list_view_profiles(target.store, target.principal, target.sid), [])
        self.assertEqual(settings.list_import_profiles(target.store, target.principal, target.sid), [])
        self.assertEqual(inspect_integrity(target.store)["status"], "ok")

    def test_advertising_campaigns_round_trip_as_drafts_with_sales_fields(self):
        from sdbase import desktop_advert
        fields = {"advertiser": "Kahve Dükkanı", "owner": "Satış", "reference": "SÖZ-42", "start_date": "2026-11-01",
                  "end_date": "2026-11-30", "target_hint": "MAIN", "spot_title": "Kahve spotu", "version_label": "v1",
                  "slot": "08:15", "competition": "Kahve", "duration_seconds": 30, "planned_count": 12,
                  "capacity_seconds": 120, "intro_seconds": 0, "outro_seconds": 0, "announcement_seconds": 0,
                  "price": 1500.0, "currency": "TRY", "position": 1, "fallback": 0, "skip_empty": True, "contact": "0212"}
        draft = desktop_api.save_campaign(self.store, self.principal, self.sid, "Kasım kahve", fields, str(uuid4()))
        approved = desktop_api.save_campaign(self.store, self.principal, self.sid, "Ekim kampanyası",
                                             fields | {"spot_title": "Ekim spotu"}, str(uuid4()))
        desktop_advert.campaign_state(self.store, self.principal, self.sid, approved["_id"], approved["_rev"], "approve", str(uuid4()))
        cancelled = desktop_api.save_campaign(self.store, self.principal, self.sid, "İptal edilen", fields, str(uuid4()))
        desktop_advert.campaign_state(self.store, self.principal, self.sid, cancelled["_id"], cancelled["_rev"], "cancel", str(uuid4()))
        group = {row["id"]: row for row in transfer.groups(self.store, self.principal, self.sid)["groups"]}["campaigns"]
        self.assertEqual((group["label"], group["count"]), ("Reklam kampanyaları", 2))
        self.assertEqual([row["name"] for row in group["documents"]], ["Ekim kampanyası", "Kasım kahve"])
        package, exported = self.export(settings="all", media=False)
        self.assertIn({"kind": "campaigns", "label": "Reklam kampanyaları", "count": 2}, exported["settings"])
        target = self.station()
        result = self.import_package(target, package, settings=True, media=False)
        self.assertEqual(result["campaigns"], {"created": 2, "updated": 0, "skipped": 0, "bound": 0})
        self.assertEqual([text for text in result["warnings"] if "Kampanya" in text],
                         ["Kampanya taslak olarak alındı, onayı bu istasyonda yeniden verilmeli: Ekim kampanyası"])
        rows = {row["name"]: row for row in desktop_api.list_campaigns(target.store, target.principal, target.sid)["records"]}
        self.assertEqual(set(rows), {"Ekim kampanyası", "Kasım kahve"})
        self.assertEqual(rows["Kasım kahve"]["desktop_fields"], fields)
        self.assertEqual(rows["Ekim kampanyası"]["desktop_fields"]["spot_title"], "Ekim spotu")
        self.assertEqual(rows["Kasım kahve"]["advertiser_name"], "Kahve Dükkanı")
        self.assertTrue(all(row["status"] == "draft" for row in rows.values()))
        again = self.import_package(target, package, settings=True, media=False)
        self.assertEqual(again["campaigns"], {"created": 0, "updated": 2, "skipped": 0, "bound": 0})
        self.assertEqual(len(desktop_api.list_campaigns(target.store, target.principal, target.sid)["records"]), 2)
        self.assertEqual(inspect_integrity(target.store)["status"], "ok")
        chosen, _ = self.export(settings="selected", kinds=["campaigns"], documents={"campaigns": [draft["_id"]]},
                                media=False, file_name="kampanya.st")
        other = self.station()
        self.assertEqual(self.import_package(other, chosen, settings=True, media=False)["campaigns"]["created"], 1)
        self.assertEqual([row["name"] for row in desktop_api.list_campaigns(other.store, other.principal, other.sid)["records"]],
                         ["Kasım kahve"])

    def test_local_workstation_settings_round_trip_and_fixed_file_scope(self):
        local = {"media-archive.ini": "[archive]\nversion=1\n", "library-analysis.ini": "[analysis]\nworkers=3\n",
                 "storage.ini": "[storage]\nversion=1\nrecording_root=/fixture/recordings\n", "audio-panel.ini": None}
        package, result = self.export(settings="selected", kinds=["local_settings"], media=False, local_settings=local)
        inspected = transfer.inspect(self.store, self.principal, self.sid, str(package))
        self.assertEqual(inspected["settings"][0]["kind"], "local_settings")
        target = self.station()
        imported = self.import_package(target, package, settings=True, media=False)
        self.assertEqual(imported["local_settings"], local)
        self.assertEqual(imported["documents"], {"created": 0, "updated": 0, "skipped": 0})
        for bad in ({"../outside.ini": "[a]\nx=1\n"}, {"storage.ini": "not ini"}):
            with self.assertRaises(StoreError):
                transfer.export_start(self.store, self.principal, self.sid, str(self.out),
                                      settings="all", media=False, file_name="bad.st", local_settings=bad)

    def test_single_file_provenance_keeps_the_album_folder(self):
        """A file imported on its own has no folder below its root: the package keeps its album folder."""
        binding = self.song()
        library = self.store.get("desktop_media_library", binding["media_id"])
        desktop_media.save_metadata(self.store, self.principal, self.sid, library["_id"], library["_rev"], str(uuid4()),
            {"title": "Tek dosya", "kind": "music", "artist": "Sanatci", "album": "Tek Albüm",
             "genre": "", "tags": [], "year": "", "comment": ""})
        library = self.store.get("desktop_media_library", library["_id"])
        tx = self.store.begin(actor=self.principal.user_id)
        tx.put("desktop_media_library", library["_id"], desktop_media._data(library) | {
            "source_path": "/indirilenler/tek.wav", "source_root": "/indirilenler", "source_relative_path": "tek.wav"},
            expected_revision=library["_rev"])
        tx.commit()
        package, _ = self.export(settings="none", media=True)
        with zipfile.ZipFile(package) as zf:
            self.assertIn("medya/Tek Albüm/tek.wav", zf.namelist())
        target = self.station()
        root = Path(target.temp.name).resolve() / "yeni"; root.mkdir()
        self.import_package(target, package, settings=False, media=True, media_root=str(root))
        self.assertTrue((root / "Tek Albüm" / "tek.wav").is_file())
        self.assertEqual(target.store.get("desktop_media_library", binding["media_id"])["source_relative_path"], "Tek Albüm/tek.wav")

    def test_equal_bytes_keep_distinct_albums_bindings_source_trees_and_restore_metadata(self):
        bindings = [self.song(), self.song()]
        self.assertEqual(bindings[0]["media_sha256"], bindings[1]["media_sha256"])
        paths = ["Turkce/2020/parca.wav", "Yabanci/2021/parca.wav"]
        for index, binding in enumerate(bindings):
            library = self.store.get("desktop_media_library", binding["media_id"])
            desktop_media.save_metadata(self.store, self.principal, self.sid, library["_id"], library["_rev"], str(uuid4()),
                {"title": "Kayit " + str(index), "kind": "music", "artist": "Sanatci " + str(index),
                 "album": "Album " + str(index), "genre": "", "tags": [], "year": "", "comment": ""})
            library = self.store.get("desktop_media_library", library["_id"])
            tx = self.store.begin(actor=self.principal.user_id)
            data = desktop_media._data(library) | {"source_path": "/old/music/" + paths[index],
                "source_root": "/old/music", "source_relative_path": paths[index]}
            tx.put("desktop_media_library", library["_id"], data, expected_revision=library["_rev"])
            tx.commit()
        playlist = desktop_playlist.create_playlist(self.store, self.principal, self.sid, "Two albums", idempotency_key=str(uuid4()),
            entries=[row("First", **bindings[0]), row("Second", **bindings[1])])
        package, result = self.export(settings="all", media=True)
        with zipfile.ZipFile(package) as zf:
            self.assertTrue(all("medya/" + path in zf.namelist() for path in paths))
        target = self.station()
        root = Path(target.temp.name).resolve() / "new-music"; root.mkdir()
        result = self.import_package(target, package, settings=True, media=True, media_root=str(root))
        self.assertEqual(result["warnings"], [])
        self.assertEqual(result["media"]["registered"], 2)
        self.assertTrue(all((root / path).is_file() for path in paths))
        tracks = desktop_playlist.list_playlists(target.store, target.principal, target.sid)["playlists"][0]["tracks"]
        self.assertEqual([track["media_id"] for track in tracks], [entry["media_id"] for entry in bindings])
        for index, binding in enumerate(bindings):
            library = target.store.get("desktop_media_library", binding["media_id"])
            self.assertEqual(library["album"], "Album " + str(index))
            self.assertEqual(library["source_path"], str(root / paths[index]))
        library = target.store.get("desktop_media_library", bindings[1]["media_id"])
        metadata = {key: library[key] for key in desktop_media._METADATA_KEYS - {"title", "kind"}}
        metadata.update(title="Changed locally", kind="music", album="Changed album")
        desktop_media.save_metadata(target.store, target.principal, target.sid, library["_id"], library["_rev"], str(uuid4()), metadata)
        again = self.import_package(target, package, settings=True, media=True, media_root=str(root))
        self.assertEqual(again["warnings"], [])
        self.assertEqual(again["media"]["reused"], 2)
        self.assertEqual(target.store.get("desktop_media_library", bindings[1]["media_id"])["album"], "Album 1")
        self.assertEqual(inspect_integrity(target.store)["status"], "ok")

    def test_destination_package_and_selection_validation(self):
        self.put("channel", {"deck_count": 6})
        sequence = self.store.sequence
        link = self.root / "bağlantı"
        os.symlink(self.out, link)
        plain = self.root / "dosya.txt"
        plain.write_text("x")
        for destination in ("paketler", str(self.root / "yok"), str(plain), str(link), str(ROOT / "src"), str(ROOT / "config"),
                            "/", str(self.out) + "/", "", None, 7):
            with self.subTest(destination=destination), self.assertRaises(StoreError):
                transfer.export_start(self.store, self.principal, self.sid, destination, settings="all", media=False)
        for options in ({"settings": "selected", "kinds": []}, {"settings": "selected", "kinds": ["channel", "channel"]},
                        {"settings": "selected", "kinds": ["tokens"]}, {"settings": "none", "media": False},
                        {"settings": "hepsi"}, {"settings": "all", "media": "evet"}, {"settings": "all", "file_name": "../paket.st"},
                        {"settings": "all", "file_name": "paket.zip"}, {"settings": "all", "file_name": ".gizli.st"},
                        {"settings": "all", "file_name": "alt/paket.st"}):
            with self.subTest(options=options), self.assertRaises(StoreError):
                transfer.export_start(self.store, self.principal, self.sid, str(self.out), **options)
        bogus = self.out / "bozuk.st"
        bogus.write_bytes(b"not a package")
        empty_zip = self.out / "bos.st"
        with zipfile.ZipFile(empty_zip, "w") as zf:
            zf.writestr("ayarlar/channel.json", "{}")
        foreign = self.out / "yabanci.st"
        with zipfile.ZipFile(foreign, "w") as zf:
            zf.writestr("bilgiler/manifest.json", json.dumps({"format": "other", "version": 1}))
            zf.writestr("../escape.json", "{}")
        wrong_suffix = self.out / "paket.zip"
        wrong_suffix.write_bytes(empty_zip.read_bytes())
        for path in (str(bogus), str(empty_zip), str(foreign), str(wrong_suffix), "paket.st", str(self.out / "yok.st"),
                     str(self.out), str(link / "bozuk.st")):
            with self.subTest(path=path), self.assertRaises(StoreError):
                transfer.inspect(self.store, self.principal, self.sid, path)
            with self.subTest(path=path, action="import"), self.assertRaises(StoreError):
                transfer.import_start(self.store, self.principal, self.sid, path, settings=True, media=False)
        package, _result = self.export(settings="all", media=False)
        for options in ({"settings": True, "media": True}, {"settings": True, "media": True, "media_root": "müzik"},
                        {"settings": True, "media": True, "media_root": str(ROOT / "src")},
                        {"settings": False, "media": False}, {"settings": True, "kinds": ["tokens"]},
                        {"settings": "evet"}, {"settings": True, "kinds": "channel"}):
            with self.subTest(options=options), self.assertRaises(StoreError):
                transfer.import_start(self.store, self.principal, self.sid, str(package), **options)
        with self.assertRaises(StoreError):
            transfer.job(self.store, self.principal, self.sid, str(uuid4()))
        with self.assertRaises(StoreError):
            transfer.job_cancel(self.store, self.principal, self.sid, "not-a-uuid")
        self.assertEqual(self.store.sequence, sequence)

    def test_second_job_is_refused_and_cancel_removes_the_partial_package(self):
        self.song()
        started, release = threading.Event(), threading.Event()
        self.addCleanup(release.set)
        original = transfer._stream_song

        def slow(zf, info, source, digest, job):
            started.set()
            release.wait(10)
            return original(zf, info, source, digest, job)

        with patch.object(transfer, "_stream_song", slow):
            first = transfer.export_start(self.store, self.principal, self.sid, str(self.out), settings="all", media=True, file_name="birinci.st")
            self.assertTrue(started.wait(10))
            with self.assertRaises(StoreError) as refused:
                transfer.export_start(self.store, self.principal, self.sid, str(self.out), settings="all", media=False, file_name="ikinci.st")
            self.assertEqual(str(refused.exception), "Bir aktarım işi zaten çalışıyor.")
            with self.assertRaises(StoreError):
                transfer.import_start(self.store, self.principal, self.sid, str(self.out / "birinci.st"), settings=True, media=False)
            running = transfer.job(self.store, self.principal, self.sid, first["job_id"])
            self.assertEqual((running["kind"], running["state"], running["package_path"]), ("export", "running", str(self.out / "birinci.st")))
            self.assertTrue(list(self.out.glob(".birinci.st.*.partial")))
            cancelled = transfer.job_cancel(self.store, self.principal, self.sid, first["job_id"])
            self.assertIn(cancelled["state"], {"running", "cancelled"})
            release.set()
            finished = self.wait(self.fixture, first["job_id"])
        self.assertEqual((finished["state"], finished["message"]), ("cancelled", "İptal edildi."))
        self.assertNotIn("result", finished)
        self.assertEqual(list(self.out.iterdir()), [])
        self.assertEqual(transfer.job_cancel(self.store, self.principal, self.sid, first["job_id"])["state"], "cancelled")
        package, _result = self.export(settings="all", media=True, file_name="ikinci.st")
        self.assertTrue(package.is_file())
        self.assertEqual([view["state"] for view in (transfer.job(self.store, self.principal, self.sid, first["job_id"]),)], ["cancelled"])

    def test_http_groups_export_start_job_polling_inspect_and_permissions(self):
        self.song()
        channel = self.put("channel", {"deck_count": 6})
        post = self.http()
        status, groups = post("desktop-transfer/groups")
        self.assertEqual(status, 200)
        self.assertEqual(groups["media"]["count"], 1)
        self.assertIn({"id": "channel", "label": "DJ kanalları", "count": 1,
                       "documents": [{"id": channel["_id"], "name": "channel"}]}, groups["groups"])
        status, started = post("desktop-transfer/export-start", {"station_id": self.sid, "destination_dir": str(self.out),
                                                                 "settings": "all", "kinds": [], "media": True})
        self.assertEqual(status, 200, started)
        self.assertEqual(started["state"], "running")
        deadline = time.monotonic() + 60
        while True:
            status, view = post("desktop-transfer/job", {"station_id": self.sid, "job_id": started["job_id"]})
            self.assertEqual(status, 200)
            if view["state"] != "running":
                break
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.02)
        self.assertEqual(view["state"], "done", view)
        self.assertEqual(view["progress"], 100)
        self.assertEqual(view["package_path"], started["package_path"])
        self.assertTrue(Path(started["package_path"]).is_file())
        self.assertEqual(view["result"]["media"]["count"], 1)
        status, inspected = post("desktop-transfer/inspect", {"station_id": self.sid, "package_path": started["package_path"]})
        self.assertEqual(status, 200)
        self.assertEqual(inspected["media"]["count"], 1)
        status, cancelled = post("desktop-transfer/job-cancel", {"station_id": self.sid, "job_id": started["job_id"]})
        self.assertEqual((status, cancelled["state"]), (200, "done"))
        self.assertEqual(post("desktop-transfer/job", {"station_id": self.sid, "job_id": str(uuid4())})[0], 422)
        self.assertEqual(post("desktop-transfer/export-start", {"station_id": self.sid, "destination_dir": "göreli"})[0], 422)
        # A per-document choice crosses HTTP; "all" settings refuse it.
        status, refused = post("desktop-transfer/export-start", {"station_id": self.sid, "destination_dir": str(self.out),
                                                                 "settings": "all", "media": False,
                                                                 "documents": {"channel": [channel["_id"]]}})
        self.assertEqual(status, 422, refused)
        status, started = post("desktop-transfer/export-start", {"station_id": self.sid, "destination_dir": str(self.out),
                                                                 "file_name": "kanal.st", "settings": "selected", "kinds": ["channel"],
                                                                 "documents": {"channel": [channel["_id"]]}, "media": False})
        self.assertEqual(status, 200, started)
        view = self.wait(self.fixture, started["job_id"])
        self.assertEqual(view["state"], "done", view)
        self.assertEqual(view["result"]["settings"], [{"kind": "channel", "label": "DJ kanalları", "count": 1}])
        with zipfile.ZipFile(started["package_path"]) as zf:
            self.assertEqual(json.loads(zf.read("bilgiler/manifest.json"))["scope"]["documents"], {"channel": [channel["_id"]]})
        token, _viewer = self.local_user()
        self.assertEqual(post("desktop-transfer/groups", token=token)[0], 403)
        self.assertEqual(post("desktop-transfer/job", {"station_id": self.sid, "job_id": started["job_id"]}, token=token)[0], 403)
        self.assertEqual(post("desktop-transfer/inspect", {"station_id": self.sid, "package_path": started["package_path"]}, token=token)[0], 403)


if __name__ == "__main__":
    unittest.main()
