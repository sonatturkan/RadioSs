#!/usr/bin/env python3
"""Exercise recovered automation scenarios on private bytes/store/HTTP only.

The historical api.py is deliberately never imported: it opens normal credentials
at import time. Only the existing pure scenario builders and C driver are reused.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
from http.client import HTTPConnection
import importlib
import json
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile
import threading
import types
from uuid import uuid4
import wave

PROJECT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(PROJECT / "database/SDBASE/src"))
from sdbase.api_server import BoundedHTTPServer
from sdbase.http_server import make_handler
from sdbase.identity import bootstrap
from sdbase.schema import open_radio_store


def compile_driver(build: Path, output: Path) -> None:
    packages = "json-glib-1.0 gio-2.0 libavformat libavcodec libavutil libswresample libavfilter libswscale libpulse fontconfig libcurl".split()
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", *packages], text=True))
    libraries = [build / ("libradios_" + name + ".a") for name in ("playout", "core", "audio")]
    for library in libraries:
        if not library.is_file():
            raise RuntimeError("Build the native libraries first: " + str(library))
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-I" + str(PROJECT / "include"),
                    "-I" + str(build / "generated"), str(PROJECT / "tools/otomasyon_testleri/e2e.c"),
                    "-Wl,--start-group", *map(str, libraries), "-Wl,--end-group", *flags, "-lm", "-o", str(output)], check=True)


def import_pcm(root: Path, call, station: str, number: int, seconds: int, kind="music", bpm=125):
    imports = root / "imports"
    imports.mkdir(exist_ok=True)
    path = imports / (str(number) + ".wav")
    rate, amplitude = 8000, 1000 + number
    period = round(rate * 60 / bpm) if bpm else rate
    # A known periodic square pulse gives genuine distinct PCM and a known grid.
    cycle = struct.pack("<h", amplitude) * (period // 2) + struct.pack("<h", -amplitude) * (period - period // 2)
    frames = seconds * rate
    data = (cycle * (frames // period + 1))[:frames * 2]
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1); output.setsampwidth(2); output.setframerate(rate); output.writeframes(data)
    analysis = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "size_bytes": path.stat().st_size,
                "duration_ms": seconds * 1000, "sample_rate": rate, "channels": 1, "codec": "pcm_s16le", "format": "wav",
                "bitrate": 128000, "peak": amplitude / 32768, "rms": amplitude / 32768,
                "clipped": False, "clipped_samples": 0, "sample_frames": frames,
                "waveform_frames_per_bin": frames, "waveform": [-amplitude / 32768, amplitude / 32768]}
    if bpm and seconds >= 4:
        analysis["rhythm"] = {"schema": 1, "algorithm": "radios-onset-grid-v1", "bpm": bpm,
                              "first_beat_ms": 0, "confidence": 1, "analyzed_ms": min(seconds * 1000, 120000)}
    return call("desktop-media/import", station_id=station, import_path=path.name, source_name=path.name,
                idempotency_key=str(uuid4()), analysis=analysis,
                metadata={"title": "Private PCM " + str(number), "artist": "Artist " + str(number), "album": "Private",
                          "kind": kind, "genre": "", "tags": [], "year": "2026", "comment": "Isolated recovery regression"})


def run_private(root: Path, driver: Path):
    store = open_radio_store(root / "private.sdb")
    store.desktop_import_root = root / "imports"
    identity = bootstrap(store, "admin", "RECOVERY", "Private automation regression")
    station = identity["station_id"]
    class Handler(make_handler(store)):
        def log_message(self, *args):
            pass
    server = BoundedHTTPServer(("127.0.0.1", 0), Handler, request_timeout=20)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    requests = []
    def call(workflow, **body):
        if body.get("station_id") != station:
            raise RuntimeError("Private station fence rejected request")
        connection = HTTPConnection("127.0.0.1", server.server_port, timeout=25)
        try:
            connection.request("POST", "/v1/workflows/" + workflow, json.dumps(body),
                               {"Authorization": "Bearer " + identity["api_token"], "Content-Type": "application/json", "Connection": "close"})
            response = connection.getresponse()
            payload = json.loads(response.read())
            requests.append({"workflow": workflow, "status": response.status})
            if response.status != 200:
                raise SystemExit("HTTP " + str(response.status) + " " + workflow + ": " + json.dumps(payload)[:1000])
            return payload
        finally:
            connection.close()
    private_api = types.ModuleType("api")
    private_api.__file__ = "<private-automation-api>"
    private_api.ROOT, private_api.SID, private_api.call = str(root), station, call
    private_api.docs = lambda kind: call("desktop/list", station_id=station, kind=kind)["records"]
    sys.modules["api"] = private_api
    sys.path.insert(0, str(PROJECT / "tools/otomasyon_testleri"))
    try:
        lib, e2e = importlib.import_module("lib"), importlib.import_module("e2e")
        assert sys.modules["api"].__file__ == "<private-automation-api>"
        e2e.H, e2e.E2E, e2e.DBFILE = str(root / "cases"), str(driver), str(root / "private.sdb")
        media = [import_pcm(root, call, station, 1, 180), import_pcm(root, call, station, 2, 200),
                 import_pcm(root, call, station, 3, 10, "sweeper", 0), import_pcm(root, call, station, 4, 3, "music", 0)]
        for index in (0, 1, 3):
            row = media[index]
            duration = row["version"]["duration_ms"]
            cue_in = 500 if index == 3 else 0
            markers = {"cue_in_ms":cue_in,"intro_ms":0 if index==3 else 30000,"out_ms":duration,
                       "fade_start_ms":duration,"fade_end_ms":duration,"auto_next_ms":duration,
                       "overlap_ms":0,"automatic":False}
            media[index] = call("desktop-media/markers-save",station_id=station,media_id=row["media"]["_id"],
                                expected_revision=row["media"]["_rev"],expected_marker_revision=None,
                                idempotency_key=str(uuid4()),markers=markers)
        music = lib.ensure_playlist("Private music", [lib.entry_row(row) for row in media[:2]])
        sweeper = lib.ensure_playlist("Private sweeper", [lib.entry_row(media[2])], "sweeper")
        short = lib.ensure_playlist("Private three seconds", [lib.entry_row(media[3])])
        empty = lib.ensure_playlist("Private empty", [])
        now = datetime(2026, 10, 14, 10, 5, tzinfo=timezone.utc)
        cases = []
        def add(name, sources, *, start=600, end=720, at=now, expected="done", check=None, run=None, **settings):
            rotation = lib.rotation(name, sources, **settings)
            weekly = lib.weekly([lib.event("2026-10-14", start, end, [rotation], title=name)], [rotation])
            cases.append((name, weekly, at, run or {}, expected, check))
        no_short_chain = lambda r: all(a["kind"] not in {"sweeper", "jingle"} or b["kind"] not in {"sweeper", "jingle"}
                                       for a, b in zip(r["entries"], r["entries"][1:]))
        add("H1 beatmatch16-short-sweeper", [lib.src(music,70),lib.src(sweeper,30,media_kind="sweeper")],
            transitions=lib.tr(0,0,0,100,beat_ms=16000), check=no_short_chain)
        add("H2 crossfade30-short-sweeper", [lib.src(music,70),lib.src(sweeper,30,media_kind="sweeper")],
            transitions=lib.tr(0,100,0,0,cf_ms=30000), check=no_short_chain)
        one = {kind:[1] for kind in lib.ALL12}
        add("H16 one-player-overlap", [lib.src(music,autodj=one)], players=one,
            transitions=lib.tr(0,100,0,0,cf_ms=30000), run={"decks":"0","semi":True},
            check=lambda r: all(e["mode"] == 0 for e in r["entries"]))
        add("H22 empty-source-with-playable", [lib.src(empty),lib.src(music)])
        add("H11 prepare-delay15", [lib.src(music)], transitions=lib.tr(0,0,0,100), run={"delay_s":15})
        add("K1 genuine-midtrack-layer", [lib.src(music)], policy={"schema":1,"jingles":[lib.overlay(sweeper)]},
            check=lambda r: any(e["adjunct"] for e in r["entries"]))
        add("K2 outside-intro-layer-skipped", [lib.src(music)], policy={"schema":1,"jingles":[lib.overlay(sweeper,seconds=60)]},
            check=lambda r: not any(e["adjunct"] for e in r["entries"]) and any(d["reason"]=="probability_jingle_no_eligible_safe_window" for d in r["display"]["diagnostics"]))
        add("C1 sweeper-no-chain", [lib.src(music,30),lib.src(sweeper,70,media_kind="sweeper")], check=no_short_chain)
        add("C2 strict-gap-exhaustion", [lib.src(music)], gaps=(1000,1000,0),
            check=lambda r: len(r["entries"]) == 2)
        add("C2 relaxed-gap-continuity", [lib.src(music)], gaps=(1000,1000,0), extra={"relax_gaps":True},
            check=lambda r: len(r["entries"]) > 2 and all(a["title"] != b["title"] for a,b in zip(r["entries"],r["entries"][1:])))
        late = datetime(2026,10,14,11,58,30,tzinfo=timezone.utc)
        add("C3 strict-latejoin-waits", [lib.src(music)], at=late, expected="wait")
        add("C3 soft-latejoin-native", [lib.src(music)], at=late, extra={"soft_end_seconds":300},
            check=lambda r: "calendar_soft_end_overrun" in r["calendar_diagnostics"])
        add("G short-cue-crossfade", [lib.src(short),lib.src(music)], transitions=lib.tr(0,100,0,0,cf_ms=30000),
            check=lambda r: all(e["overlap_s"] < e["cue_s"] for e in r["entries"]))
        results=[]
        for name, weekly, at, run, expected, check in cases:
            result=e2e.run_case(name,weekly,at,**run)
            accepted=result["ok"] if expected=="done" else (result["step"]=="calendar-plan" and "WOULD_BLOCK" in result["message"])
            if accepted and check:
                accepted=bool(check(result))
            result["accepted"],result["expected"] = accepted,expected
            results.append(result)
            print(("PASS " if accepted else "FAIL ") + name + ": " + result["step"] + " " + result["message"][:160],flush=True)
        return {"cases":results,"http_requests":requests,"normal_database_touched":False,"normal_credentials_read":False,
                "http_port":server.server_port,"generated_pcm_records":len(media),"scope":"13 recovered difficult combinations; not the full historical 110 catalogue"}
    finally:
        server.shutdown();server.server_close();worker.join(timeout=5)
        sys.modules.pop("api",None)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir",type=Path,default=PROJECT/"build")
    parser.add_argument("--results",type=Path,default=PROJECT/"build/claude-recovery-20261015/calendar/stress-results.json")
    args=parser.parse_args()
    args.results.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="automation-stress-private-",dir=PROJECT/"build") as temporary:
        root=Path(temporary)
        driver=root/"e2e"
        compile_driver(args.build_dir.resolve(),driver)
        result=run_private(root,driver)
    result["private_fixture_removed"]=not root.exists()
    result["at"]=datetime.now(timezone.utc).isoformat()
    result["passed"]=sum(row["accepted"] for row in result["cases"])
    result["total"]=len(result["cases"])
    args.results.write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n")
    return 0 if result["passed"]==result["total"] and result["private_fixture_removed"] else 1


if __name__=="__main__":
    raise SystemExit(main())
