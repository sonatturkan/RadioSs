"""Şarkı işaretleri (cue / intro / fade / otomatik sonraki / AutoDJ giriş-çıkış) × geçiş türleri.

DİKKAT: "Roman Blanco" (Señorita) parçasının İKİNCİ kopyasının işaretlerini geçici olarak
değiştirir (yayın playlistlerinde kullanılmayan kayıt) ve sonunda nötr işaretlere döndürür.
Kullanım: python3 isaretler.py
"""
import traceback
from datetime import datetime, timezone
import lib
from lib import *
from api import call, SID
from e2e import run_case, summary


def target():
    hits = [i for i, r in enumerate(media(refresh=True)) if r["media"]["title"].startswith("Roman Blanco")]
    return hits[1]


def set_markers(values):
    r = media(refresh=True)[target()]; mk = r.get("markers") or {}
    call("desktop-media/markers-save", station_id=SID, media_id=r["media"]["_id"], expected_revision=r["media"]["_rev"],
         expected_marker_revision=mk.get("_rev"), idempotency_key=U(), markers=values)


def variants(dur):
    return {
        "v1 tam set": dict(cue_in_ms=15000, intro_ms=25000, out_ms=170000, fade_start_ms=160000, fade_end_ms=170000,
                           auto_next_ms=165000, overlap_ms=4000, automatic=True, autodj_in_ms=16000, autodj_out_ms=160000),
        "v2 yalnız AutoDJ çıkış 2:00": dict(cue_in_ms=0, intro_ms=0, out_ms=dur, fade_start_ms=dur, fade_end_ms=dur,
                           auto_next_ms=dur, overlap_ms=0, automatic=False, autodj_in_ms=None, autodj_out_ms=120000),
        "v3 3sn cue aralığı": dict(cue_in_ms=100000, intro_ms=0, out_ms=103000, fade_start_ms=103000, fade_end_ms=103000,
                           auto_next_ms=103000, overlap_ms=0, automatic=False, autodj_in_ms=None, autodj_out_ms=None),
        "v4 AutoDJ 1sn pencere": dict(cue_in_ms=5000, intro_ms=0, out_ms=dur, fade_start_ms=dur, fade_end_ms=dur,
                           auto_next_ms=dur, overlap_ms=0, automatic=False, autodj_in_ms=60000, autodj_out_ms=61000),
    }


def check(res, values):
    out = []
    ents = res.get("entries") or []
    for i, e in enumerate(ents[:-1]):
        if not e["title"].startswith("Roman Blanco"):
            continue
        span = (values["out_ms"] - values["cue_in_ms"]) / 1000
        nxt = (ents[i + 1]["planned_utc_us"] - e["planned_utc_us"]) / 1e6
        out.append(f"cue_s={e['cue_s']:.1f}/{span:.1f} geçiş@{nxt:.1f}s ov={e['overlap_s']:.1f} mode={e['mode']}"
                   + (f" katman@{e['adjunct_at_s']:.1f}" if e.get("adjunct") else ""))
        if len(out) >= 2:
            break
    return "; ".join(out)


if __name__ == "__main__":
    P = playlists()
    PL, SWEEP = P["TEST · İşaretli varyant"]["_id"], P["RS · Sweeper & İstasyon ID"]["_id"]
    dur = media()[target()]["version"]["duration_ms"]
    neutral = dict(cue_in_ms=0, intro_ms=0, out_ms=dur, fade_start_ms=dur, fade_end_ms=dur, auto_next_ms=dur,
                   overlap_ms=0, automatic=False, autodj_in_ms=None, autodj_out_ms=None)
    modes = {"EOF": dict(transitions=tr(100)), "XF 4sn": dict(transitions=tr(0, 100, cf_ms=4000)),
             "CUE": dict(transitions=tr(0, 0, 100, cue_ms=3000)), "BM 8sn": dict(transitions=tr(0, 0, 0, 100, beat_ms=8000)),
             "katman": dict(transitions=tr(0, 100, cf_ms=4000), policy={"schema": 1, "jingles": [overlay(SWEEP, "after_start", 5, 50), overlay(SWEEP, "before_end", 20, 50)]})}
    one = {k: [1] for k in ALL12}
    now = datetime(2026, 10, 14, 10, 5, tzinfo=timezone.utc)
    try:
        for vname, values in variants(dur).items():
            set_markers(values)
            for mname, kw in modes.items():
                for semi in ((False, True) if mname == "XF 4sn" else (False,)):
                    name = f"İ {vname} · {mname}" + (" · tek oynatıcı" if semi else "")
                    rot = rotation(name, [src(PL, order="linear", autodj=one if semi else None)], selection="linear", players=one if semi else None, **kw)
                    wp = weekly([event("2026-10-14", 600, 720, [rot], title=name)], [rot])
                    try:
                        res = run_case(name, wp, now, decks="0" if semi else "0,1", semi=semi)
                    except Exception:
                        res = {"name": name, "ok": False, "step": "python", "message": traceback.format_exc()[-300:]}
                    print(summary(res)[:150], "|", check(res, values) if res.get("ok") else "", flush=True)
    finally:
        import time
        for attempt in range(30):  # servis yeniden başlıyor olabilir: bekle ve yeniden dene
            try:
                set_markers(neutral); print("işaretler nötre döndü"); break
            except (SystemExit, OSError) as exc:
                print("işaret geri yükleme bekliyor:", str(exc)[:80]); time.sleep(5)
        else:
            print("UYARI: işaretler nötre döndürülemedi; isaretler.py'yi yeniden çalıştırın")
