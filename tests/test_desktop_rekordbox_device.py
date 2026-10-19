"""DeviceSQL parsing and real private-store USB tracklist import tests."""

from base64 import b64encode
from http.client import HTTPConnection
import json
import os
from pathlib import Path
import random
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "database/SDBASE/src"))
from sdbase import desktop_rekordbox as api, desktop_playlist
from sdbase.api_server import BoundedHTTPServer
from sdbase.http_server import make_handler
from sdbase.identity import authenticate, bootstrap
from sdbase.integrity import inspect_integrity
from sdbase.rekordbox_device import parse_device_library, _string
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError
from rekordbox_device_fixture import PAGE_SIZE, device_string, export_pdb


class DeviceReader(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("RADIOS_TEST_USB_EXPORT"), "GitHub sample is an explicit opt-in acceptance")
    def test_github_original_cdj_export_and_unicode(self):
        contents = Path(os.environ["RADIOS_TEST_USB_EXPORT"]).read_bytes()
        library = parse_device_library(contents, "/media/GitHub USB")
        self.assertEqual(library.source_sha256, "a7876a572998daa35896db77f937d9cc70727cd75af7aa1047555a28bc59c6df")
        self.assertEqual(len(library.tracks), 651)
        self.assertEqual(library.playlists[0].name, "test_formats")
        self.assertEqual([entry.track_id for entry in library.playlists[0].entries], [str(identifier) for identifier in range(612, 652)])
        self.assertEqual(library.tracks["48"].fields["Name"].value, "Obéissance")
        self.assertEqual(library.tracks["89"].fields["Name"].value, "人々の繋がり")

    def test_binary_layout_metadata_unicode_and_order(self):
        library = parse_device_library(export_pdb(23), "/media/DJ USB")
        self.assertEqual(len(library.tracks), 23)
        self.assertEqual([node.name for node in library.playlists], ["USB Sıra", "USB Dışarı"])
        self.assertEqual([entry.track_id for entry in library.playlists[0].entries], ["2", "1", "1"])
        track = library.tracks["1"]
        for field, expected in {"Name": "Birinci", "Artist": "Örnek DJ", "Album": "USB Album",
                                "Genre": "House", "Label": "Label", "Tonality": "8A",
                                "SampleRate": 48000, "BitRate": 320, "Year": 2026}.items():
            self.assertEqual(track.fields[field].value, expected)
        self.assertEqual(float(track.fields["AverageBpm"].value), 127.5)
        self.assertEqual(float(track.fields["TotalTime"].value), 123)
        self.assertEqual(track.source_path, "/media/DJ USB/Contents/Örnek DJ/Track 1.wav")
        self.assertEqual(track.marks, ())

    def test_device_strings_and_bad_encoding_bounds(self):
        for text in ("", "ASCII", "x" * 150, "Şarkı 人々"):
            self.assertEqual(_string(memoryview(device_string(text)), 0), text)
        self.assertEqual(_string(memoryview(device_string("long", True)), 0), "long")
        for raw in (b"\x00", b"\x40\x02\x00\x00", b"\x90\x05\x00\x00\x80", b"\xff"):
            with self.subTest(raw=raw), self.assertRaises(StoreError):
                _string(memoryview(raw), 0)

    def test_partial_row_group_can_use_unallocated_index_space(self):
        raw = bytearray(export_pdb())
        page = PAGE_SIZE * 6
        start = PAGE_SIZE - 24
        row = struct.pack("<HHIBB", 0x60, 0, 5, 3, 10) + device_string("Kolter")
        raw[page + start:page + start + len(row)] = row
        struct.pack_into("<H", raw, page + PAGE_SIZE - 6, start - 40)
        library = parse_device_library(bytes(raw), "/media/USB")
        self.assertEqual(library.tracks["1"].fields["Artist"].value, "Kolter")

    def test_broken_headers_pages_rows_and_path_escape(self):
        original = export_pdb()
        variants = [b"", b"<DJ_PLAYLISTS/>", original[:-1]]
        for offset, value in ((4, 1), (8, 1000), (36, 0xffffffff),
                              (PAGE_SIZE + 12, 1), (PAGE_SIZE * 2 + 8, 99)):
            broken = bytearray(original)
            struct.pack_into("<I", broken, offset, value)
            variants.append(bytes(broken))
        broken = bytearray(original)
        struct.pack_into("<H", broken, PAGE_SIZE * 3 - 6, 65535)
        variants.append(bytes(broken))
        for raw in variants:
            with self.subTest(length=len(raw)), self.assertRaises(StoreError):
                parse_device_library(raw, "/media/USB")
        for root in (None, "relative", "/media/../etc", "/media/USB\x00"):
            with self.subTest(root=root), self.assertRaises(StoreError):
                parse_device_library(original, root)
        with self.assertRaises(StoreError):
            parse_device_library(export_pdb(path="/Contents/../../etc/passwd"), "/media/USB")

    def test_playlist_cycles_and_unresolved_entries_are_rejected(self):
        original = export_pdb()

        def row_offset(kind, slot):
            for table in range(struct.unpack_from("<I", original, 8)[0]):
                offset = 28 + table * 16
                if struct.unpack_from("<I", original, offset)[0] == kind:
                    page = (struct.unpack_from("<I", original, offset + 8)[0] + 1) * PAGE_SIZE
                    return page + 40 + struct.unpack_from("<H", original, page + PAGE_SIZE - 6 - slot * 2)[0]
            self.fail("Fixture table not found")

        for offset, value in ((row_offset(7, 1), 10), (row_offset(7, 0), 12),
                              (row_offset(7, 1), 999), (row_offset(8, 0) + 4, 999),
                              (row_offset(8, 0) + 8, 10), (row_offset(8, 1), 3)):
            broken = bytearray(original)
            struct.pack_into("<I", broken, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(StoreError):
                parse_device_library(bytes(broken), "/media/USB")


class NativeDeviceImport(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rekordbox-device-", dir=ROOT / "build")
        self.db = Path(self.temp.name) / "test.sdb"
        self.store = open_radio_store(self.db)
        self.identity = bootstrap(self.store, "admin", "USB", "USB station")
        self.principal = authenticate(self.store, self.identity["api_token"])
        self.sid = self.identity["station_id"]
        self.source = {"pdb_zlib_base64": b64encode(zlib.compress(export_pdb())).decode(), "usb_root": "/media/Test USB"}
        self.options = {"library_key": "usb-test", "selected_node_paths": [[0, 0]], "duplicate_policy": "keep"}

    def tearDown(self):
        self.temp.cleanup()

    def preview(self, **changes):
        return api.preview_import(self.store, self.principal, self.sid, **(self.source | self.options | changes))

    def apply(self, preview, key=None, **changes):
        return api.apply_import(self.store, self.principal, self.sid, expected_preview_hash=preview["preview_hash"],
                                idempotency_key=key or str(uuid4()), **(self.source | self.options | changes))

    def test_discovery_transaction_reopen_and_never_xml(self):
        before = self.store.sequence
        with patch("sdbase.importer.parse_rekordbox_xml", side_effect=AssertionError("USB must not use XML")):
            discovery = self.preview(selected_node_paths=None)
            self.assertEqual(self.store.sequence, before)
            self.assertEqual(discovery["selected_tracks"], 0)
            self.assertEqual(discovery["playlist_nodes"][0]["display_path"], "USB Setleri / USB Sıra")
            preview = self.preview()
            self.assertEqual(preview["source_kind"], "rekordbox_device")
            self.assertEqual(preview["media_verified"], 0)
            key = str(uuid4())
            result = self.apply(preview, key)
            self.assertEqual(self.apply(preview, key), result)
        self.store = open_radio_store(self.db)
        playlists = desktop_playlist.list_playlists(self.store, self.principal, self.sid)["playlists"]
        self.assertEqual(len(playlists), 1)
        tracks = playlists[0]["tracks"]
        self.assertEqual([track["title"] for track in tracks], ["İkinci", "Birinci", "Birinci"])
        self.assertTrue(all(track["location"].startswith(self.source["usb_root"] + "/Contents/") for track in tracks))
        self.assertTrue(all(track["media_state"] == "unverified" for track in tracks))
        self.assertTrue(all(row["title_source"] == "rekordbox_device" for row in self.store.scan("items")))
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_skip_duplicate_stale_preview_and_changed_root(self):
        result = self.apply(self.preview(duplicate_policy="skip"), duplicate_policy="skip")
        self.assertEqual(result["playlists"][0]["added_count"], 2)
        stale = self.preview()
        desktop_playlist.create_playlist(self.store, self.principal, self.sid, "USB Sıra", idempotency_key=str(uuid4()))
        with self.assertRaises(Conflict):
            self.apply(stale)
        preview = self.preview(name_policy="copy")
        key = str(uuid4())
        self.apply(preview, key, name_policy="copy")
        with self.assertRaises(Conflict):
            self.apply(preview, key, name_policy="copy", usb_root="/media/Other USB")

    def test_snapshot_limits_bombs_truncation_and_ambiguous_source(self):
        for encoded in ("!", b64encode(b"not zlib").decode(),
                        b64encode(zlib.compress(b"x" * (16 * 1024 * 1024 + 1))).decode(),
                        b64encode(zlib.compress(export_pdb())[:-1]).decode(),
                        b64encode(zlib.compress(export_pdb()) + b"extra").decode()):
            with self.subTest(prefix=encoded[:15]), self.assertRaises(StoreError):
                self.preview(pdb_zlib_base64=encoded)
        with self.assertRaises(StoreError):
            self.preview(xml_base64="abc")
        with self.assertRaises(StoreError):
            self.preview(usb_root="relative")
        self.assertEqual(self.store.scan("items"), [])

    def test_authenticated_http_binary_preview_apply_replay(self):
        server = BoundedHTTPServer(("127.0.0.1", 0), make_handler(self.store))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            def post(action, extra):
                connection = HTTPConnection("127.0.0.1", server.server_address[1], timeout=8)
                connection.request("POST", "/v1/workflows/desktop-rekordbox/" + action,
                    json.dumps({"station_id": self.sid, **self.source, **self.options, **extra}),
                    {"Authorization": "Bearer " + self.identity["api_token"], "Content-Type": "application/json"})
                response = connection.getresponse()
                result = json.loads(response.read())
                connection.close()
                self.assertEqual(response.status, 200, result)
                return result
            preview = post("preview", {})
            body = {"expected_preview_hash": preview["preview_hash"], "idempotency_key": str(uuid4())}
            self.assertEqual(post("apply", body), post("apply", body))
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

    def test_large_binary_snapshot_uses_only_usb_route_budget(self):
        contents = export_pdb() + random.Random(20261007).randbytes(512 * PAGE_SIZE)
        self.source["pdb_zlib_base64"] = b64encode(zlib.compress(contents)).decode()
        self.assertGreater(len(self.source["pdb_zlib_base64"]), 2 * 1024 * 1024)
        server = BoundedHTTPServer(("127.0.0.1", 0), make_handler(self.store))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            body = json.dumps({"station_id": self.sid, **self.source, **self.options})
            headers = {"Authorization": "Bearer " + self.identity["api_token"], "Content-Type": "application/json"}
            for path, expected in (("/v1/workflows/desktop-rekordbox/preview", 200),
                                   ("/v1/workflows/desktop/list", 422)):
                connection = HTTPConnection("127.0.0.1", server.server_address[1], timeout=8)
                if expected == 200:
                    connection.request("POST", path, body, headers)
                else:
                    connection.putrequest("POST", path)
                    for name, value in headers.items():
                        connection.putheader(name, value)
                    connection.putheader("Content-Length", str(len(body)))
                    connection.endheaders()
                response = connection.getresponse()
                result = json.loads(response.read())
                connection.close()
                self.assertEqual(response.status, expected, result)
                if expected == 200:
                    self.assertEqual(result["selected_tracks"], 2)
                else:
                    self.assertIn("2097152", str(result))
        finally:
            server.shutdown()
            thread.join()
            server.server_close()


if __name__ == "__main__":
    unittest.main()
