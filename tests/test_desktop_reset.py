#!/usr/bin/env python3
"""Settings › Sıfırlama: retire-only resets, automatic verified backup, replay and permissions."""
from datetime import datetime
from http.client import HTTPConnection
import json
from pathlib import Path
import threading
import unittest
from uuid import uuid4

import test_desktop_media as media_fixture
from sdbase import desktop_api, desktop_media, desktop_playlist, desktop_reset
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.http_server import make_handler
from sdbase.backup import verify_backup
from sdbase.identity import Forbidden, authenticate
from sdbase.integrity import inspect_integrity
from sdbase.storage import StoreError

PREFERENCES = {"schema_version": 1, "ui": {"text_size": 2, "default_profile": 3, "header_hidden_fields": 5},
               "audio_panel": {"sample_rate": 48000, "buffer_frames": 512, "click_sound": 1, "click_volume": 2}}


class DesktopReset(unittest.TestCase):
    # The media fixture's real staged WAV, without rerunning its own tests.
    setUp = media_fixture.DesktopMedia.setUp
    tearDown = media_fixture.DesktopMedia.tearDown
    args = media_fixture.DesktopMedia.args
    load = media_fixture.DesktopMedia.load

    def put(self, kind, config):
        return desktop_api.put_document(self.store, self.principal, self.sid, kind, kind, config, str(uuid4()))

    def binding(self, imported):
        library = self.store.get("desktop_media_library", imported["media"]["_id"])
        file = self.store.get("media_files", library["media_file_id"])
        return {"media_id": library["_id"], "version_id": library["version_id"],
                "media_file_id": library["media_file_id"], "media_sha256": file["sha256"]}

    def row(self, title, **extra):
        return {"title": title, "artist": "", "album": "", "duration_ms": 200, "bpm": 0, "genre": "",
                "key": "", "location": "", "comment": "", **extra}

    def apply(self, scopes, **extra):
        return desktop_reset.apply(self.store, self.principal, self.sid, scopes, "SIFIRLA", str(uuid4()), **extra)

    def active(self, kind):
        return [row for row in self.store.scan("desktop_configurations") if row["kind"] == kind and row["status"] == "active"]

    def test_media_reset_retires_songs_and_their_playlist_rows_after_backup(self):
        first, second = self.load(), self.load()
        playlist = desktop_playlist.create_playlist(self.store, self.principal, self.sid, "Sabah", idempotency_key=str(uuid4()),
            entries=[self.row("Bağlı", **self.binding(first)), self.row("Yalnız metin"), self.row("İkinci", **self.binding(second))])
        preview = desktop_reset.preview(self.store, self.principal, self.sid, ["media"])
        self.assertEqual((preview["media"], preview["playlist_entries"], preview["playlists"]), (2, 2, 0))
        key = str(uuid4())
        result = desktop_reset.apply(self.store, self.principal, self.sid, ["media"], "SIFIRLA", key)
        self.assertTrue(result["backup"])
        self.assertEqual(verify_backup(result["backup"])["store_id"], self.store._meta()["store_id"])
        self.assertEqual(desktop_media.list_media(self.store, self.principal, self.sid)["total"], 0)
        snapshot = desktop_playlist.list_playlists(self.store, self.principal, self.sid)["playlists"]
        self.assertEqual([row["_id"] for row in snapshot], [playlist["_id"]])
        tracks = desktop_playlist._tracks(self.store, snapshot[0]["draft_version_id"])
        self.assertEqual([(row["title"], row["ordinal"]) for row in tracks], [("Yalnız metin", 0)])
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")
        # The exact retry replays without a second backup or change.
        backups = sorted(Path(result["backup"]).parent.iterdir())
        sequence = self.store.sequence
        self.assertEqual(desktop_reset.apply(self.store, self.principal, self.sid, ["media"], "SIFIRLA", key), result)
        self.assertEqual((self.store.sequence, sorted(Path(result["backup"]).parent.iterdir())), (sequence, backups))
        # Retired songs can be imported again as new library rows.
        self.assertEqual(desktop_media.list_media(self.store, self.principal, self.sid)["total"], 0)
        self.load()
        self.assertEqual(desktop_media.list_media(self.store, self.principal, self.sid)["total"], 1)

    def test_scopes_are_separate_and_factory_keeps_receipts(self):
        preferences = self.put("desktop_preferences", PREFERENCES)
        midi = self.put("midi_mapping", {"device": "", "maps": []})
        channel = self.put("channel", {"deck_count": 6})
        playlist = desktop_playlist.create_playlist(self.store, self.principal, self.sid, "Gece", idempotency_key=str(uuid4()))
        result = self.apply(["settings"], backup=False)
        self.assertIsNone(result["backup"])
        self.assertEqual(result["documents"], {"desktop_preferences": 1, "midi_mapping": 1})
        self.assertFalse(self.active("desktop_preferences") or self.active("midi_mapping"))
        self.assertEqual([row["_id"] for row in self.active("channel")], [channel["_id"]])
        self.assertEqual(len(desktop_playlist.list_playlists(self.store, self.principal, self.sid)["playlists"]), 1)
        self.assertEqual(self.apply(["mixer"], backup=False)["documents"], {"channel": 1})
        self.assertFalse(self.active("channel"))
        imported = self.load()
        factory = self.apply(["factory"], backup=False)
        self.assertEqual((factory["playlists"], factory["media"]), (1, 1))
        self.assertFalse(desktop_playlist.list_playlists(self.store, self.principal, self.sid)["playlists"])
        self.assertEqual(desktop_media.list_media(self.store, self.principal, self.sid)["total"], 0)
        self.assertIn("playout_history", factory["kept"])
        self.assertEqual(self.store.get("desktop_media_library", imported["media"]["_id"])["status"], "disabled")
        self.assertEqual(self.store.get("playlists", playlist["_id"])["status"], "disabled")
        self.assertEqual(self.store.get("desktop_configurations", preferences["_id"])["status"], "disabled")
        self.assertEqual(self.store.get("desktop_configurations", midi["_id"])["status"], "disabled")
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_automation_reset_covers_local_events(self):
        self.assertIn("local_event", desktop_reset.AUTOMATION_KINDS)
        event = self.put("local_event", {"schema": 1, "title": "Yerel etkinlik", "kind": "program", "sources": []})
        channel = self.put("channel", {"deck_count": 6})
        for scope in ("settings", "mixer", "playlists", "media"):
            with self.subTest(scope=scope):
                self.assertNotIn("local_event", desktop_reset.preview(self.store, self.principal, self.sid, [scope])["documents"])
        preview = desktop_reset.preview(self.store, self.principal, self.sid, ["automation"])
        self.assertEqual(preview["documents"], {"local_event": 1})
        result = self.apply(["automation"], backup=False)
        self.assertEqual(result["documents"], {"local_event": 1})
        self.assertEqual(self.store.get("desktop_configurations", event["_id"])["status"], "disabled")
        self.assertFalse(self.active("local_event"))
        self.assertEqual([row["_id"] for row in self.active("channel")], [channel["_id"]])
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_backup_now_writes_a_verified_backup_without_resetting(self):
        midi = self.put("midi_mapping", {"device": "", "maps": []})
        imported = self.load()
        sequence = self.store.sequence
        result = desktop_reset.backup(self.store, self.principal, self.sid)
        path = Path(result["path"])
        self.assertTrue(path.is_dir())
        self.assertTrue(path.name.startswith("yedek-"), path.name)
        self.assertTrue((path / "manifest.json").is_file())
        self.assertEqual(verify_backup(result["path"])["store_id"], self.store._meta()["store_id"])
        self.assertEqual((result["store_id"], result["sequence"]), (self.store._meta()["store_id"], sequence))
        self.assertGreaterEqual(result["files"], 2)
        self.assertEqual(result["media_objects"], 1)
        self.assertIsNotNone(datetime.fromisoformat(result["created_at"]).tzinfo)
        # Nothing is reset: the station's records stay active and the sequence is unchanged.
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual([row["_id"] for row in self.active("midi_mapping")], [midi["_id"]])
        self.assertEqual(self.store.get("desktop_media_library", imported["media"]["_id"])["status"], "active")
        # A second request writes a second, distinct folder; the key is only validated.
        again = desktop_reset.backup(self.store, self.principal, self.sid, idempotency_key=str(uuid4()))
        self.assertNotEqual(again["path"], result["path"])
        self.assertTrue(Path(again["path"]).is_dir())
        with self.assertRaises(StoreError):
            desktop_reset.backup(self.store, self.principal, self.sid, idempotency_key="not-a-key")
        editor = create_station_user(self.store, self.principal, self.sid, "backup-editor", "Editor", ["view", "edit"])
        with self.assertRaises(Forbidden):
            desktop_reset.backup(self.store, authenticate(self.store, editor["token"]), self.sid)
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_backup_and_reset_refuse_without_free_space(self):
        """A copy that would leave less than the margin free is refused before anything is written."""
        midi = self.put("midi_mapping", {"device": "", "maps": []})
        self.load()
        backups = Path(self.store.root).parent / "backups"
        before = sorted(backups.iterdir()) if backups.exists() else []
        margin = desktop_reset.SPACE_MARGIN_BYTES
        self.addCleanup(setattr, desktop_reset, "SPACE_MARGIN_BYTES", margin)
        desktop_reset.SPACE_MARGIN_BYTES = 2 ** 62
        sequence = self.store.sequence
        with self.assertRaisesRegex(StoreError, "Yedek için yeterli boş alan yok"):
            desktop_reset.backup(self.store, self.principal, self.sid)
        with self.assertRaisesRegex(StoreError, "Yedek için yeterli boş alan yok"):
            self.apply(["settings"], backup=True)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(sorted(backups.iterdir()) if backups.exists() else [], before)
        self.assertEqual([row["_id"] for row in self.active("midi_mapping")], [midi["_id"]])
        # Without a backup nothing is copied, so the reset itself still works.
        self.assertEqual(self.apply(["settings"], backup=False)["documents"], {"midi_mapping": 1})
        desktop_reset.SPACE_MARGIN_BYTES = margin
        self.assertTrue(Path(desktop_reset.backup(self.store, self.principal, self.sid)["path"]).is_dir())

    def test_factory_reset_retires_profiles_and_frees_their_names(self):
        from sdbase import settings
        view = settings.put_view_profile(self.store, self.principal, self.sid, name="Sabah görünümü",
                                         view_kind="library", columns=["title", "artist"], filters={"kind": "music"})
        profile = settings.put_import_profile(self.store, self.principal, self.sid, name="Varsayılan içe aktarım",
                                              library_key="main", config={})
        preview = desktop_reset.preview(self.store, self.principal, self.sid, ["factory"])
        self.assertEqual(preview["profiles"], 2)
        result = self.apply(["factory"], backup=False)
        self.assertEqual(result["profiles"], 2)
        self.assertEqual(settings.list_view_profiles(self.store, self.principal, self.sid), [])
        self.assertEqual(settings.list_import_profiles(self.store, self.principal, self.sid), [])
        # The store keeps every profile row: retired, renamed, with the old name on record.
        retired_view, retired_profile = self.store.get("view_profiles", view["_id"]), self.store.get("import_profiles", profile["_id"])
        self.assertEqual((retired_view["retired_name"], retired_profile["retired_name"]), ("Sabah görünümü", "Varsayılan içe aktarım"))
        self.assertIsNotNone(retired_view["retired_at"])
        self.assertEqual(retired_profile["status"], "disabled")
        # Their names are free again and a later reset retires only the new rows.
        fresh = settings.put_view_profile(self.store, self.principal, self.sid, name="Sabah görünümü",
                                          view_kind="library", columns=["title"], filters={})
        settings.put_import_profile(self.store, self.principal, self.sid, name="Varsayılan içe aktarım",
                                    library_key="main", config={})
        self.assertEqual([row["_id"] for row in settings.list_view_profiles(self.store, self.principal, self.sid)], [fresh["_id"]])
        self.assertEqual(self.apply(["factory"], backup=False)["profiles"], 2)
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_confirmation_scopes_and_manage_permission(self):
        sequence = self.store.sequence
        for scopes, confirm in ((["media"], "sifirla"), ([], "SIFIRLA"), (["everything"], "SIFIRLA"),
                                (["media", "media"], "SIFIRLA"), ("media", "SIFIRLA")):
            with self.subTest(scopes=scopes, confirm=confirm), self.assertRaises(StoreError):
                desktop_reset.apply(self.store, self.principal, self.sid, scopes, confirm, str(uuid4()))
        editor = create_station_user(self.store, self.principal, self.sid, "reset-editor", "Editor", ["view", "edit"])
        who = authenticate(self.store, editor["token"])
        for action in (lambda: desktop_reset.preview(self.store, who, self.sid, ["settings"]),
                       lambda: desktop_reset.apply(self.store, who, self.sid, ["settings"], "SIFIRLA", str(uuid4()))):
            with self.assertRaises(Forbidden):
                action()
        self.assertEqual(self.store.sequence, sequence + 1)  # only the editor account was created

    def test_http_preview_and_apply_as_the_desktop_calls_them(self):
        self.load()
        base = make_handler(self.store)

        class Handler(base):
            def log_message(self, *args):
                pass

        server = BoundedHTTPServer(("127.0.0.1", 0), Handler, request_timeout=5)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        self.addCleanup(lambda: (server.shutdown(), server.server_close(), worker.join(timeout=3)))

        def post(action, body):
            connection = HTTPConnection("127.0.0.1", server.server_port, timeout=10)
            try:
                connection.request("POST", "/v1/workflows/desktop-reset/" + action, json.dumps(body),
                                   {"Content-Type": "application/json", "Connection": "close",
                                    "Authorization": "Bearer " + self.identity["api_token"]})
                response = connection.getresponse()
                return response.status, json.loads(response.read())
            finally:
                connection.close()

        status, preview = post("preview", {"station_id": self.sid, "scopes": ["media"]})
        self.assertEqual((status, preview["media"]), (200, 1))
        status, refused = post("apply", {"station_id": self.sid, "scopes": ["media"], "confirm": "evet",
                                         "backup": False, "idempotency_key": str(uuid4())})
        self.assertGreaterEqual(status, 400)
        status, result = post("apply", {"station_id": self.sid, "scopes": ["media"], "confirm": "SIFIRLA",
                                        "backup": True, "idempotency_key": str(uuid4())})
        self.assertEqual((status, result["media"]), (200, 1))
        self.assertTrue(Path(result["backup"]).is_dir())
        self.assertEqual(desktop_media.list_media(self.store, self.principal, self.sid)["total"], 0)
        # "Şimdi yedek al" is a workflow of its own, as reset_ui.c posts it.
        status, backup = post("backup", {"station_id": self.sid})
        self.assertEqual(status, 200, backup)
        self.assertTrue(Path(backup["path"]).is_dir())
        self.assertTrue(Path(backup["path"]).name.startswith("yedek-"))
        self.assertIn("created_at", backup)

    def test_scales_optional_integer_range_and_rejected_changes_are_atomic(self):
        legacy = self.put("desktop_preferences", PREFERENCES)
        self.assertEqual(legacy["config"], PREFERENCES)
        for key in ("content_scale_percent", "mixer_scale_percent", "effector_scale_percent"):
            self.assertNotIn(key, legacy["config"]["ui"])
            for percent in (50, 53, 100, 143, 150):
                config = json.loads(json.dumps(PREFERENCES))
                config["ui"][key] = percent
                with self.subTest(key=key, percent=percent):
                    saved = self.put("desktop_preferences", config)
                    self.assertEqual(saved["config"], config)
                    self.assertEqual(self.store.get("desktop_configurations", saved["_id"])["config"], config)
            for percent in (0, 49, 151, -1, True, False, 100.0, "100", None):
                config = json.loads(json.dumps(PREFERENCES))
                config["ui"][key] = percent
                sequence = self.store.sequence
                with self.subTest(key=key, percent=percent), self.assertRaises(StoreError):
                    self.put("desktop_preferences", config)
                self.assertEqual(self.store.sequence, sequence)

    def test_content_scale_backups_preserve_legacy_and_explicit_preferences(self):
        from sdbase.backup import create_backup, restore_backup
        from sdbase.portable_export import create_portable_export
        from sdbase.portable_import import restore_portable_export
        from sdbase.schema import open_radio_store
        legacy = self.put("desktop_preferences", PREFERENCES)
        config = json.loads(json.dumps(PREFERENCES))
        config["ui"]["content_scale_percent"] = 143
        config["ui"]["mixer_scale_percent"] = 79
        config["ui"]["effector_scale_percent"] = 131
        modern = self.put("desktop_preferences", config)
        sequence = self.store.sequence
        root = Path(self.temp.name)
        create_backup(self.store, root / "scale-backup")
        restored = restore_backup(root / "scale-backup", root / "scale-restored")
        physical = open_radio_store(Path(restored["path"]) / restored["database"], read_only=True)
        create_portable_export(self.store, self.principal, root / "scale-portable")
        imported = restore_portable_export(root / "scale-portable", root / "scale-imported")
        portable = open_radio_store(Path(imported["path"]) / imported["database"], read_only=True)
        for store in (physical, portable):
            for document in (legacy, modern):
                self.assertEqual(store.get("desktop_configurations", document["_id"])["config"], document["config"])
            self.assertEqual(inspect_integrity(store)["status"], "ok")
        self.assertEqual(self.store.sequence, sequence)

    def test_desktop_preferences_document_validation(self):
        saved = self.put("desktop_preferences", PREFERENCES)
        self.assertEqual(saved["config"], PREFERENCES)
        self.assertEqual(self.put("desktop_preferences", {"schema_version": 1})["config"], {"schema_version": 1})
        for changes in ({"ui": {"text_size": 3, "default_profile": 0, "header_hidden_fields": 0}},
                        {"ui": {"text_size": 1}},
                        {"audio_panel": {"sample_rate": 96000, "buffer_frames": 256, "click_sound": 0, "click_volume": 1}},
                        {"audio_panel": {"sample_rate": 48000, "buffer_frames": 300, "click_sound": 0, "click_volume": 1}},
                        {"schema_version": 2}, {"password": "secret"}):
            with self.subTest(changes=changes), self.assertRaises(StoreError):
                self.put("desktop_preferences", {"schema_version": 1, **changes})


if __name__ == "__main__":
    unittest.main()
