#!/usr/bin/env python3
"""Library upkeep end to end, on a private store with synthetic songs.

Five songs in different states go through the real native upkeep scan
(tests/test_rhythm_upkeep.c): a complete analysis without AutoDJ points, a
missing grid, a legacy grid, an operator's manual OUT point, and silence. The
live database and the running application are never touched; no window opens
and no audio device is used.
"""
import json
import math
import pathlib
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import uuid
import wave

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "database/SDBASE/src"))
from sdbase import desktop_media
from sdbase.api_server import BoundedHTTPServer
from sdbase.http_server import make_handler
from sdbase.identity import authenticate, bootstrap, issue_endpoint_token
from sdbase.schema import open_radio_store

RATE = 22050
ALGORITHM = "radios-autodj-mix-v2"
BINARY = None


def write_song(path, *, outro_from=28.0, total=40.0, bpm=120, first_kick=0.4, pad_hz=440, silent=False):
    """A kick on every beat; the first `outro_from` seconds also carry a pad and
    off-beat hats, then only kicks remain (a beats-only outro)."""
    beat, rng, samples = 60.0 / bpm, random.Random(int(first_kick * 1000) + pad_hz), []
    for index in range(int(total * RATE)):
        t = index / RATE
        value = 0.0
        if not silent and t >= first_kick:
            phase = (t - first_kick) % beat
            value += 0.8 * math.exp(-phase * 25) * math.sin(2 * math.pi * (55 + 45 * math.exp(-phase * 30)) * phase)
            if t < outro_from:
                value += 0.2 * math.sin(2 * math.pi * pad_hz * t)
                if (t - first_kick - beat / 2) % beat < 0.03:
                    value += 0.25 * (rng.random() * 2 - 1)
        samples.append(round(max(-1.0, min(1.0, value)) * 30000))
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1); output.setsampwidth(2); output.setframerate(RATE)
        output.writeframes(struct.pack("<%dh" % len(samples), *samples))
    path.chmod(0o600)


class RhythmUpkeep(unittest.TestCase):
    def test_upkeep_grids_points_manual_and_silence(self):
        with tempfile.TemporaryDirectory(prefix="rhythm-upkeep-", dir=ROOT / "build") as name:
            tmp = pathlib.Path(name)
            imports = tmp / "imports"; imports.mkdir(mode=0o700)
            (tmp / "config").mkdir(mode=0o700)
            database = tmp / "RadioS.sdb"
            store = open_radio_store(database)
            store.desktop_import_root = imports
            identity = bootstrap(store, "admin", "UPKEEP", "Upkeep station")
            sid = identity["station_id"]
            principal = authenticate(store, identity["api_token"])

            songs = {"complete": dict(first_kick=0.40, pad_hz=440), "no_grid": dict(first_kick=0.45, pad_hz=523),
                     "legacy": dict(first_kick=0.50, pad_hz=587), "manual": dict(first_kick=0.55, pad_hz=659),
                     "silence": dict(silent=True, total=12.0)}
            imported = {}
            for key, options in songs.items():
                file = imports / (key + ".wav")
                write_song(file, **options)
                out = subprocess.run([BINARY, "analyze", str(file)], capture_output=True, text=True, check=True)
                analysis = json.loads(out.stdout)
                if key == "no_grid":
                    analysis.pop("rhythm"); analysis.pop("loudness")
                elif key == "legacy":
                    analysis["rhythm"] = {"schema": 1, "algorithm": "radios-onset-grid-v1", "bpm": 0, "first_beat_ms": 0,
                                          "confidence": 0, "analyzed_ms": analysis["duration_ms"]}
                metadata = {"title": key, "kind": "music", "artist": "Test", "album": "", "genre": "", "tags": [], "year": "", "comment": ""}
                imported[key] = desktop_media.import_media(store, principal, station_id=sid, import_path=file.name,
                    source_name=file.name, idempotency_key=str(uuid.uuid4()), metadata=metadata, analysis=analysis)["media"]["_id"]
            # The operator already placed AutoDJ OUT by hand; IN is missing.
            detail = desktop_media.get_media(store, principal, sid, imported["manual"])
            duration = detail["version"]["duration_ms"]
            desktop_media.save_markers(store, principal, sid, imported["manual"], detail["media"]["_rev"], None, str(uuid.uuid4()),
                {"cue_in_ms": 0, "intro_ms": 0, "out_ms": duration, "fade_start_ms": duration, "fade_end_ms": duration,
                 "auto_next_ms": duration, "overlap_ms": 0, "automatic": False, "autodj_out_ms": 30000})

            meta = json.loads(pathlib.Path(str(database) + ".meta").read_text())
            credentials = tmp / "config" / "credentials.json"
            credentials.write_text(json.dumps({"schema_version": 1, "store_id": meta["store_id"], "user_id": identity["user_id"],
                "station_id": sid, "endpoint_id": identity["endpoint_id"], "database_file": "RadioS.sdb",
                "api_token": identity["api_token"],
                "endpoint_token": issue_endpoint_token(store, principal, sid, identity["endpoint_id"])}))
            credentials.chmod(0o600)

            class Quiet(make_handler(store)):
                def log_message(self, *args):
                    pass
            server = BoundedHTTPServer(("127.0.0.1", 0), Quiet, request_timeout=60)
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            self.addCleanup(thread.join, 5); self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
            run = subprocess.run([BINARY, "scan", str(tmp), str(database), "http://127.0.0.1:%d" % server.server_port],
                                 capture_output=True, text=True, timeout=300)
            self.assertEqual(run.returncode, 0, run.stderr)
            passes = {int(line.split()[1].rstrip(":")): json.loads(line.split(": ", 1)[1]) for line in run.stdout.splitlines() if line.startswith("pass ")}

            # Pass 1: two grids analysed, four songs marked, silence settled.
            first = passes[1]
            self.assertEqual((first["analyzed"], first["failed"], first["marked"], len(first["settled"])), (2, 0, 4, 1), first)
            # Pass 2 has nothing left to do and writes nothing.
            second = passes[2]
            self.assertEqual((second["analyzed"], second["failed"], second["marked"], second["settled"]), (0, 0, 0, []), second)

            for key in ("complete", "no_grid", "legacy", "manual"):
                snapshot = desktop_media.get_media(store, principal, sid, imported[key])
                rhythm, markers = snapshot["analysis"]["rhythm"], snapshot["markers"]
                self.assertEqual(rhythm["algorithm"], "radios-onset-grid-v3", key)
                self.assertTrue(snapshot["analysis"].get("loudness"), key)
                self.assertTrue(119.0 < rhythm["bpm"] < 121.0, (key, rhythm))
                source = markers["autodj_source"]
                self.assertTrue(0 <= markers["autodj_in_ms"] <= 3000, (key, markers))
                if key == "manual":
                    # A point the operator placed is never replaced.
                    self.assertEqual(markers["autodj_out_ms"], 30000)
                    self.assertEqual(source, {"autodj_in_ms": ALGORITHM, "autodj_out_ms": "manual"})
                else:
                    # OUT starts the beats-only outro (28 s), on a bar line.
                    self.assertTrue(26000 <= markers["autodj_out_ms"] <= 30000, (key, markers))
                    self.assertEqual(source, {"autodj_in_ms": ALGORITHM, "autodj_out_ms": ALGORITHM})
            silence = desktop_media.get_media(store, principal, sid, imported["silence"])
            self.assertIsNone(silence["markers"])
            self.assertEqual(silence["analysis"]["rhythm"]["bpm"], 0)


    def test_parallel_scan_marks_many_songs(self):
        """Six workers assign AutoDJ points to 24 songs at once. Their commits race on the same
        store (a commit between another assignment's preparation and its own rejects it), so
        every song must still be marked within the pass: the scan reads and retries on conflict."""
        count = 24
        with tempfile.TemporaryDirectory(prefix="rhythm-parallel-", dir=ROOT / "build") as name:
            tmp = pathlib.Path(name)
            imports = tmp / "imports"; imports.mkdir(mode=0o700)
            (tmp / "config").mkdir(mode=0o700)
            database = tmp / "RadioS.sdb"
            store = open_radio_store(database)
            store.desktop_import_root = imports
            identity = bootstrap(store, "admin", "PARALLEL", "Parallel station")
            sid = identity["station_id"]
            principal = authenticate(store, identity["api_token"])
            ids = []
            for index in range(count):
                file = imports / ("song%02d.wav" % index)
                write_song(file, total=14.0, outro_from=9.0, first_kick=0.30 + 0.01 * index, pad_hz=300 + 11 * index)
                out = subprocess.run([BINARY, "analyze", str(file)], capture_output=True, text=True, check=True)
                metadata = {"title": "song%02d" % index, "kind": "music", "artist": "Test", "album": "", "genre": "", "tags": [], "year": "", "comment": ""}
                ids.append(desktop_media.import_media(store, principal, station_id=sid, import_path=file.name, source_name=file.name,
                    idempotency_key=str(uuid.uuid4()), metadata=metadata, analysis=json.loads(out.stdout))["media"]["_id"])
            meta = json.loads(pathlib.Path(str(database) + ".meta").read_text())
            credentials = tmp / "config" / "credentials.json"
            credentials.write_text(json.dumps({"schema_version": 1, "store_id": meta["store_id"], "user_id": identity["user_id"],
                "station_id": sid, "endpoint_id": identity["endpoint_id"], "database_file": "RadioS.sdb",
                "api_token": identity["api_token"],
                "endpoint_token": issue_endpoint_token(store, principal, sid, identity["endpoint_id"])}))
            credentials.chmod(0o600)

            class Quiet(make_handler(store)):
                def log_message(self, *args):
                    pass
            server = BoundedHTTPServer(("127.0.0.1", 0), Quiet, request_timeout=60)
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            self.addCleanup(thread.join, 5); self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
            run = subprocess.run([BINARY, "scan", str(tmp), str(database), "http://127.0.0.1:%d" % server.server_port, "6"],
                                 capture_output=True, text=True, timeout=300)
            self.assertEqual(run.returncode, 0, run.stderr)
            passes = {int(line.split()[1].rstrip(":")): json.loads(line.split(": ", 1)[1]) for line in run.stdout.splitlines() if line.startswith("pass ")}
            first, second = passes[1], passes[2]
            self.assertEqual((first["analyzed"], first["failed"], first["marked"], first["settled"]), (0, 0, count, []), first)
            self.assertEqual((second["analyzed"], second["failed"], second["marked"], second["settled"]), (0, 0, 0, []), second)
            # Informational: how many commits the store rejected while the workers raced.
            print("parallel scan: %d commit conflicts retried" % store.metrics.snapshot()["counters"].get("conflicts", 0), file=sys.stderr)
            for media_id in ids:
                markers = desktop_media.get_media(store, principal, sid, media_id)["markers"]
                self.assertEqual(markers["autodj_source"], {"autodj_in_ms": ALGORITHM, "autodj_out_ms": ALGORITHM}, media_id)
                self.assertTrue(0 <= markers["autodj_in_ms"] < markers["autodj_out_ms"], markers)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: run_rhythm_upkeep.py <test_rhythm_upkeep binary>")
    BINARY = sys.argv.pop(1)
    unittest.main(verbosity=2)
