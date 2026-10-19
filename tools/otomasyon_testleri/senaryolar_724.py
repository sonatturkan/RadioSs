#!/usr/bin/env python3
"""7/24 doğrulama koşumları (salt okuma): canlı "RadioS 7/24 Yayın Akışı" ve 7/24 paket playlistleri.

Ses çalmaz; veri servisine yalnız list/prepare istekleri gider (hiçbir şey yazmaz, TEST playlisti
oluşturmaz). senaryolar.py'nin RS/TEST playlistlerine bağlı desenleri burada canlı 7/24
paketleriyle kurulur.

Kullanım:  RADIOS_BUILD_DIR=build-67 python3 senaryolar_724.py [GRUP ...] [--tohum N]
  L  canlı akış · gün × saat taraması ve kuşak sonları
  N  gece kuşakları farklı tohumlarla (--tohum N: 0..N-1, varsayılan 4)
  P  kuşak geçişleri: kuşak bitimine 90 sn / 10 sn kala ve sonraki kuşağın başı (program→program devam)
  G S Z C H K  senaryolar.py desenlerinin 7/24 paketleriyle sürümleri
İstekler arası varsayılan bekleme 1 sn (RADIOS_E2E_PACE_S).
Sonuçlar: <yapı>/otomasyon_testleri/sonuclar_724.json ve sonuclar_724.md (özet tablo).
"""
import copy, json, os, sys
os.environ.setdefault("RADIOS_E2E_PACE_S", "1")
from datetime import datetime, timezone
from lib import src, tr, rotation, event, weekly, overlay, playlists, ALL12, U
from api import docs
from e2e import run_case, summary, H

T = lambda h, m=0, s=0, day=14, month=10: datetime(2026, month, day, h, m, s, tzinfo=timezone.utc)
D = "2026-10-14"
LIVE_NAME = "RadioS 7/24 Yayın Akışı"
DAYS = {12: "Pzt", 13: "Sal", 14: "Çar", 15: "Per", 16: "Cum", 17: "Cmt", 18: "Paz"}
# Kuşak sınırları (UTC, docs/yayin-akisi-724-2026-10-17.md): Pzt–Per, Cum, Cmt, Paz
BOUNDS = {12: (6, 10, 14, 18, 21), 16: (6, 10, 14, 18, 21), 17: (3, 8, 12, 16, 19, 21), 18: (3, 9, 12, 16, 20)}


def build_cases(groups, seeds):
    P = playlists()

    def pid(name):
        full = "7/24 · " + name
        if full not in P:
            raise SystemExit(f"7/24 playlisti bulunamadı: {full}")
        return P[full]["_id"]
    POWER, TR, GOLD, URBAN = pid("Dans & EDM A"), pid("Türkçe Pop A"), pid("Klasikler (Gold) A"), pid("Urban & Latin A")
    NIGHT, SWEEP, NSWEEP, MIXED = pid("Deep & Melodic A"), pid("Sweeper · Enerji"), pid("Sweeper · Gece"), pid("Mashup & Parti")
    HOUSE, POP = pid("House & Club A"), pid("Pop Hitleri A")
    power_tracks = P["7/24 · Dans & EDM A"]["tracks"]
    live = [d for d in docs("weekly_programme") if d["name"] == LIVE_NAME]
    if not live:
        raise SystemExit("canlı 7/24 haftalık programı bulunamadı")
    W = live[0]["config"]
    C = []

    def case(group, name, wp, now, **run):
        if group in groups:
            C.append((group, name, wp, now, run))

    def one(name, rot, start=600, end=720, day=D, **wk):
        rots = rot if isinstance(rot, list) else [rot]
        return weekly([event(day, start, end, rots[:1], title=name)], rots, **wk)
    kind = lambda music, sweeper: {"music": music, "advertisement": {"mode": 0, "overlap_ms": 0},
                                   "jingle": {"mode": 1, "overlap_ms": 800}, "sweeper": sweeper}

    # --- L: canlı akış, her gün 8 saat dilimi + Pazartesi/Cumartesi kuşak sonları ---
    for day, label in DAYS.items():
        for h, m in ((0, 30), (3, 30), (6, 30), (9, 30), (12, 30), (15, 30), (18, 30), (21, 30)):
            case("L", f"L 7/24 · {label} {h:02d}:{m:02d}", W, T(h, m, day=day))
    for day in (12, 17):
        for hour in BOUNDS[day]:
            case("L", f"L 7/24 · {DAYS[day]} kuşak sonu {hour - 1:02d}:58:00", W, T(hour - 1, 58, 0, day=day))
    case("L", "L 7/24 · Paz 23:59:30 (hafta dönüşü)", W, T(23, 59, 30, day=18))

    # --- N: gece kuşakları, farklı tohumlar ---
    for seed in seeds:
        case("N", f"N gece Sal 01:30 · tohum {seed}", W, T(1, 30, day=13), seed=seed)
        case("N", f"N after party Cmt 00:30 · tohum {seed}", W, T(0, 30, day=17), seed=seed)
        case("N", f"N gece Paz 04:00 · tohum {seed}", W, T(4, 0, day=18), seed=seed)
        case("N", f"N gece seansı Per 03:00 · tohum {seed}", W, T(3, 0, day=15), seed=seed)

    # --- P: kuşak geçişleri (program→program) ---
    for day in (12, 17, 18):
        for hour in BOUNDS[day]:
            case("P", f"P {DAYS[day]} {hour:02d}:00 · bitişe 90 sn", W, T(hour - 1, 58, 30, day=day))
            case("P", f"P {DAYS[day]} {hour:02d}:00 · bitişe 10 sn (bitişte yeniden dene)", W, T(hour - 1, 59, 50, day=day))
            case("P", f"P {DAYS[day]} {hour:02d}:00 · sonraki kuşak başı", W, T(hour, 0, 0, day=day))
    case("P", "P Pzt 00:00:00 hafta başı", W, T(0, 0, 0, day=12))
    case("P", "P Paz 23:59:59 → Pzt", W, T(23, 59, 59, day=18))

    # --- H: geçiş geometrisi, zaman penceresi, kural uç durumları (7/24 paketleriyle) ---
    case("H", "H1 beatmatch 16sn + sweeper", one("H1", rotation("H1", [src(POWER, 70), src(SWEEP, 30, media_kind="sweeper")], transitions=tr(0, 0, 0, 100, beat_ms=16000))), T(10, 5))
    case("H", "H2 crossfade 30sn + sweeper", one("H2", rotation("H2", [src(POWER, 70), src(SWEEP, 30, media_kind="sweeper")], transitions=tr(0, 100, 0, 0, cf_ms=30000))), T(10, 5))
    case("H", "H3 AutoDJ tür: sweeper beatmatch 12sn", one("H3", rotation("H3", [src(POWER, 70), src(SWEEP, 30, media_kind="sweeper")], kind=kind({"mode": 3, "overlap_ms": 12000}, {"mode": 3, "overlap_ms": 12000}))), T(10, 5))
    case("H", "H4 cue %100", one("H4", rotation("H4", [src(POWER), src(GOLD)], transitions=tr(0, 0, 100, 0))), T(10, 5))
    case("H", "H7 low-pass crossfade 20sn + sweeper", one("H7", rotation("H7", [src(POWER, 70), src(SWEEP, 30, media_kind="sweeper")], transitions=tr(0, 100, 0, 0, cf_ms=20000, lowpass=True))), T(10, 5))
    pol = {"schema": 1, "seed": "h8", "transitions": [{"id": U(), "mode": 1, "overlap_ms": 6000, "effect": "lowpass", "early_seconds": 600, "probability": 100}]}
    case("H", "H8 olasılık geçişi early 600sn", one("H8", rotation("H8", [src(POWER)], policy=pol)), T(10, 5))
    for k, ov, label in (("H9a", overlay(SWEEP, "before_end", 5, 100), "before_end 5sn"),
                         ("H9b", overlay(SWEEP, "after_start", 9999, 100), "after_start 9999sn"),
                         ("H9c", overlay(SWEEP, "after_start", 8, 100, player=0), "katman oynatıcısı müzik havuzunda")):
        case("H", f"{k} {label}", one(k, rotation(k, [src(POWER)], policy={"schema": 1, "jingles": [ov]})), T(10, 5))
    r = rotation("H10", [src(POWER), src(GOLD)], transitions=tr(20, 50, 10, 20))
    case("H", "H10 geç katılım: bitişe 90sn", one("H10", r), T(11, 58, 30))
    case("H", "H11 geç katılım + 15sn hazırlık gecikmesi", one("H11", r), T(10, 33, 7), delay_s=15)
    case("H", "H12a 1 dakikalık blok", one("H12a", r, start=600, end=601), T(10, 0))
    case("H", "H12b 3 dakikalık blok", one("H12b", r, start=600, end=603), T(10, 0))
    case("H", "H13 tekrar aralığı 1000dk (tükenme)", one("H13", rotation("H13", [src(TR), src(URBAN)], gaps=(1000, 1000, 0)), start=600, end=780), T(10, 0))
    case("H", "H14 müzik listesinde sweeper filtresi", one("H14", rotation("H14", [src(POWER, media_kind="sweeper")])), T(10, 5))
    one1 = {k: [1] for k in ALL12}
    case("H", "H16 tek oynatıcı (SEMI) + bindirmeler", one("H16", rotation("H16", [src(POWER, autodj=one1), src(SWEEP, 30, media_kind="sweeper", autodj=one1)], players=one1, transitions=tr(0, 50, 0, 50, cf_ms=6000, beat_ms=12000))), T(10, 5), decks="0", semi=True)
    case("H", "H17 listeden adet 100 doğrusal", one("H17", rotation("H17", [src(TR, count=100, order="linear")], selection="linear")), T(10, 5))
    case("H", "H18 50 kaynak", one("H18", rotation("H18", [src([POWER, TR, GOLD, URBAN, SWEEP][i % 5], weight=(i % 7) + 1, media_kind="sweeper" if i % 5 == 4 else "all") for i in range(50)])), T(10, 5))
    case("H", "H19 saat dilimi Europe/Istanbul", one("H19", rotation("H19", [src(POWER), src(GOLD)], transitions=tr(20, 50, 10, 20)), tz="Europe/Istanbul"), T(7, 5))
    r = rotation("H20", [src(POWER), src(GOLD)])
    case("H", "H20 hafta dönüşü (Pazar 23:59:30)", weekly([event("2026-10-18", 1380, 1440, [r]), event("2026-10-12", 0, 60, [r])], [r]), T(23, 59, 30, day=18))
    case("H", "H21 tekrar kapalı, geçmiş hafta", weekly([event("2026-10-05", 600, 720, [r])], [r], anchor="2026-10-05", repeat=False), T(10, 5))
    rules = [{"entry_id": t["_id"], "weight": 0, "order": 0, "frequency": 1, "max_plays": 0, "start_minute": 0, "end_minute": 1440, "interval_minutes": 0} for t in power_tracks]
    case("H", "H23 tüm şarkı ağırlıkları 0", one("H23", rotation("H23", [src(POWER, order="weighted", rules=rules)])), T(10, 5))
    ra, rb = rotation("H24a", [src(POWER)]), rotation("H24b", [src(GOLD)])
    epol = {"schema": 1, "seed": "h24", "children": [
        {"id": U(), "scope": "automation", "ids": [ra["id"]], "probability": 70, "policy": {"schema": 1}},
        {"id": U(), "scope": "automation", "ids": [], "probability": 30, "policy": {"schema": 1, "children": [
            {"id": U(), "scope": "playlist", "ids": [GOLD], "probability": 100, "policy": {"schema": 1, "children": [
                {"id": U(), "scope": "track", "ids": [P["7/24 · Klasikler (Gold) A"]["tracks"][0]["_id"]], "probability": 60, "policy": {"schema": 1}},
                {"id": U(), "scope": "track", "ids": [], "probability": 40, "policy": {"schema": 1}}]}}]}}]}
    case("H", "H24 olasılık grupları otomasyon/playlist/şarkı", weekly([event(D, 600, 720, [ra, rb], policy=epol)], [ra, rb]), T(10, 5))
    case("H", "H25 aynı playlist iki kaynak", one("H25", rotation("H25", [src(POWER, order="linear"), src(POWER, order="linear")], selection="linear")), T(10, 5))
    pol = {"schema": 1, "transitions": [{"id": U(), "mode": 2, "overlap_ms": 2000, "probability": 100}]}
    case("H", "H26 AutoDJ tür + olasılık geçişi birlikte", one("H26", rotation("H26", [src(POWER), src(SWEEP, 30, media_kind="sweeper")], policy=pol, kind=kind({"mode": 3, "overlap_ms": 8000}, {"mode": 1, "overlap_ms": 1000}))), T(10, 5))
    pools = {"music": [1, 2], "advertisement": [1, 2], "jingle": [3], "sweeper": [3]}
    case("H", "H27 sweeper ayrı oynatıcıda (havuz 1-2 / 3)", one("H27", rotation("H27", [src(POWER, autodj=pools), src(SWEEP, 30, media_kind="sweeper", autodj=pools)], transitions=tr(0, 100, 0, 0), players=pools, policy={"schema": 1, "jingles": [overlay(SWEEP, "after_start", 8, 50, player=3)]})), T(10, 5), decks="0,1,2")
    pools3 = {k: [1, 2, 3] for k in ALL12}
    case("H", "H28 üç oynatıcılı havuz", one("H28", rotation("H28", [src(POWER, autodj=pools3), src(GOLD, autodj=pools3)], transitions=tr(0, 100, 0, 0), players=pools3)), T(10, 5), decks="0,1,2")

    # --- K: şarkı ortası katman ---
    case("K", "K1 katman after_start 8sn %100", one("K1", rotation("K1", [src(POWER)], policy={"schema": 1, "jingles": [overlay(SWEEP, "after_start", 8, 100)]})), T(10, 5))
    case("K", "K2 katman before_end 25sn %100", one("K2", rotation("K2", [src(POWER)], policy={"schema": 1, "jingles": [overlay(SWEEP, "before_end", 25, 100)]})), T(10, 5))
    case("K", "K3 katman %35 aralık 15dk", one("K3", rotation("K3", [src(POWER)], policy={"schema": 1, "seed": "k3", "jingles": [overlay(SWEEP, "after_start", 8, 35, interval=15)]})), T(10, 5))
    case("K", "K4 katman sıklık 3", one("K4", rotation("K4", [src(POWER)], policy={"schema": 1, "jingles": [dict(overlay(SWEEP, "after_start", 8, 100), frequency=3)]})), T(10, 5))
    case("K", "K5 katman +12dB / -60dB ducking", one("K5", rotation("K5", [src(POWER)], policy={"schema": 1, "jingles": [overlay(SWEEP, "after_start", 8, 100, duck=-60, gain=12, fade=5000)]})), T(10, 5))

    # --- G: geçiş türleri ---
    case("G", "G1 cue %100 (7/24 House)", one("G1", rotation("G1", [src(HOUSE)], transitions=tr(0, 0, 100, 0))), T(10, 5))
    case("G", "G2 beatmatch 30sn (azami)", one("G2", rotation("G2", [src(POWER)], transitions=tr(0, 0, 0, 100, beat_ms=30000))), T(10, 5))
    case("G", "G3 low-pass crossfade 1sn", one("G3", rotation("G3", [src(POWER)], transitions=tr(0, 100, 0, 0, cf_ms=1000, lowpass=True))), T(10, 5))
    pol = {"schema": 1, "transitions": [{"id": U(), "mode": 1, "overlap_ms": 4000, "probability": 40}, {"id": U(), "mode": 3, "overlap_ms": 8000, "probability": 30}]}
    case("G", "G4 olasılık toplamı %70 (kalan %30)", one("G4", rotation("G4", [src(POWER)], policy=pol)), T(10, 5))
    pol = {"schema": 1, "transitions": [{"id": U(), "mode": 1, "overlap_ms": 4000, "probability": 100, "excluded_times": [{"start_minute": 600, "end_minute": 660}]}]}
    case("G", "G5 geçiş dışlama 10:00-11:00", one("G5", rotation("G5", [src(POWER)], policy=pol)), T(10, 5))
    night_kind = kind({"mode": 3, "overlap_ms": 16000}, {"mode": 1, "overlap_ms": 1200})
    for d in (12, 13, 14, 15, 16, 17):
        case("G", f"G6 AutoDJ tür gece + sweeper · gün {d}", one("G6", rotation("G6", [src(NIGHT), src(NSWEEP, 30, media_kind="sweeper")], kind=night_kind), day=f"2026-10-{d}"), T(10, 5, day=d))

    # --- S: seçim desenleri ---
    case("S", "S1 doğrusal: sıklık 1/2/3", one("S1", rotation("S1", [src(POWER), src(GOLD, freq=2), src(SWEEP, freq=3, media_kind="sweeper")], selection="linear")), T(10, 5))
    case("S", "S2 karışık liste, sweeper rastgele (doğrusal)", one("S2", rotation("S2", [src(MIXED, order="linear", sweeper_random=True)])), T(10, 5))
    case("S", "S3 karışık liste, sweeper rastgele (karışık)", one("S3", rotation("S3", [src(MIXED, sweeper_random=True)])), T(10, 5))
    case("S", "S4 kaynak pencereleri 0-60 / 60-120", one("S4", rotation("S4", [src(POWER, start=600, end=660), src(GOLD, start=660, end=720)])), T(10, 5))
    ra, rb, rc = rotation("S5a", [src(POWER)]), rotation("S5b", [src(GOLD)]), rotation("S5c", [src(TR)])
    case("S", "S5 blok doğrusal 3 rotasyon", weekly([event(D, 600, 720, [ra, rb, rc], mode="linear")], [ra, rb, rc]), T(10, 5))
    case("S", "S6 atama ağırlığı 0 / 100", weekly([event(D, 600, 720, [ra, rb], weights=[0, 100])], [ra, rb]), T(10, 5))
    hist = {"kind": "playout_history", "entries": [{"media_id": t["media_id"], "version_id": t["version_id"], "artist": t["artist"], "album": "",
             "started_utc": "2026-10-14T09:%02d:00Z" % (5 * i), "position_frames": 0} for i, t in enumerate(power_tracks[:6])]}
    case("S", "S7 geçmiş: son 6 parça 60dk aralık", one("S7", rotation("S7", [src(POWER)], gaps=(60, 0, 0))), T(10, 5), history=hist)

    # --- Z: zaman / takvim ---
    r = rotation("Z", [src(POWER), src(GOLD)])
    evs = [event("2026-10-12", 600, 720, [r], title="1. hafta"), event("2026-10-20", 600, 720, [r], title="2. hafta Salı")]
    case("Z", "Z1 iki haftalık program · 2. hafta", weekly(evs, [r], weeks=2), T(10, 5, day=20))
    case("Z", "Z2 iki haftalık · 3. hafta (döngü)", weekly(evs, [r], weeks=2), T(10, 5, day=26))
    case("Z", "Z3 yaz saati: Berlin 25.10 01-05 yerel", weekly([event("2026-10-25", 60, 300, [r])], [r], anchor="2026-10-19", tz="Europe/Berlin"), T(1, 30, day=25))
    evs = [event(D, 1380, 1440, [r], title="23-24"), event("2026-10-15", 0, 60, [r], title="00-01")]
    case("Z", "Z4 gece yarısı bitişik · 23:59:59", weekly(evs, [r]), T(23, 59, 59))
    case("Z", "Z5 gece yarısı bitişik · 00:00:00", weekly(evs, [r]), T(0, 0, 0, day=15))

    # --- C: süreklilik ---
    for label, extra in (("sert bitiş", None), ("yumuşak 300", {"soft_end_seconds": 300})):
        rr = rotation(f"C1 {label}", [src(NIGHT), src(NSWEEP, 30, media_kind="sweeper")], kind=night_kind, extra=extra)
        for d in (12, 15, 16):
            case("C", f"C1 gece+sweeper {label} · gün {d}", one("C1", rr, day=f"2026-10-{d}"), T(10, 5, day=d))
    case("C", "C2a tükenme · gevşetme kapalı", one("C2a", rotation("C2a", [src(POWER)], gaps=(60, 0, 0))), T(10, 5), history=hist)
    case("C", "C2b tükenme · gevşetme açık", one("C2b", rotation("C2b", [src(POWER)], gaps=(60, 0, 0), extra={"relax_gaps": True})), T(10, 5), history=hist)
    for label, extra in (("sert", None), ("yumuşak 300", {"soft_end_seconds": 300})):
        rr = rotation(f"C3 {label}", [src(POWER), src(GOLD)], transitions=tr(20, 50, 10, 20), extra=extra)
        case("C", f"C3 bitişe 90sn geç katılım · {label}", one("C3", rr), T(11, 58, 30))
        case("C", f"C3b bitişe 20sn (yeniden doldurma) · {label}", one("C3b", rr), T(11, 59, 40))
    w = copy.deepcopy(W)
    for rr in w["calendar"]["rotation_library"]:
        rr["settings"].update({"soft_end_seconds": 300, "relax_gaps": True})
    for label, t in (("gece", T(22, 30)), ("gündüz sonu", T(12, 57)), ("akşam sonu", T(20, 58, 10))):
        case("C", f"C4 7/24 yumuşak+gevşet · {label}", w, t)
    return C


def artist_of(title):
    return title.split(" - ", 1)[0].strip().lower() if " - " in title else None


def metrics(res):
    """Rapor ölçütleri: doluluk, ilk parça gecikmesi, tekrar, art arda sweeper, kuşak sonu sapması, tanılar."""
    out = {"durum": "✓" if res["ok"] else ("beklenen" if res.get("step") == "retry" else "✗ " + res.get("step", "")),
           "mesaj": (res.get("message") or "")[:200], "tani": res.get("calendar_diagnostics", [])}
    if not res["ok"]:
        return out
    ents, start, end, now = res["entries"], res["start"], res["end"], res["native_now"]
    out["oge"] = len(ents)
    if ents:
        first = ents[0]["planned_utc_us"]
        last = ents[-1]
        last_end = last["planned_utc_us"] + int(last["cue_s"] * 1e6)
        out["ilk_gecikme_s"] = round((first - now) / 1e6, 1)
        out["doluluk"] = round(min(1.0, max(0.0, (last_end - now) / max(1, end - now))), 3)
        out["son_sapma_s"] = round((last_end - end) / 1e6, 1)
    out["ardisik_sweeper"] = sum(1 for a, b in zip(ents, ents[1:]) if a["kind"] == "sweeper" and b["kind"] == "sweeper")
    seen, last_artist, title_rep, artist_rep = set(), {}, 0, 0
    for e in ents:
        if e["kind"] != "music":
            continue
        if e["title"] in seen:
            title_rep += 1
        seen.add(e["title"])
        artist = artist_of(e["title"])
        if artist:
            prev = last_artist.get(artist)
            if prev is not None and e["planned_utc_us"] - prev < 60 * 60 * 1_000_000:
                artist_rep += 1
            last_artist[artist] = e["planned_utc_us"]
    out["sarki_tekrar"] = title_rep
    out["sanatci_60dk"] = artist_rep
    return out


def table(rows):
    head = "| Grup | Senaryo | Durum | Öğe | İlk gecikme (sn) | Doluluk | Kuşak sonu sapması (sn) | Art arda sweeper | Şarkı tekrarı | Sanatçı <60 dk | Tanı / mesaj |"
    lines = [head, "|" + "---|" * 11]
    for group, name, m in rows:
        cell = lambda k, f="{}": (f.format(m[k]) if k in m else "—")
        note = ", ".join(str(t) for t in m.get("tani", [])) or (m["mesaj"] if not m["durum"].startswith("✓") else "")
        lines.append(f"| {group} | {name} | {m['durum']} | {cell('oge')} | {cell('ilk_gecikme_s')} | {cell('doluluk', '{:.0%}')} | "
                     f"{cell('son_sapma_s', '{:+}')} | {cell('ardisik_sweeper')} | {cell('sarki_tekrar')} | {cell('sanatci_60dk')} | {note[:120]} |")
    return "\n".join(lines)


def group_summary(rows):
    lines = ["| Grup | Senaryo | ✓ | beklenen (bitişte yeniden dene) | ✗ | Ort. doluluk | En büyük ilk gecikme (sn) | Art arda sweeper | Şarkı tekrarı | Sanatçı <60 dk |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for group in sorted({g for g, _, _ in rows}):
        ms = [m for g, _, m in rows if g == group]
        ok = [m for m in ms if m["durum"] == "✓"]
        fills = [m["doluluk"] for m in ok if "doluluk" in m]
        delays = [m["ilk_gecikme_s"] for m in ok if "ilk_gecikme_s" in m]
        lines.append(f"| {group} | {len(ms)} | {len(ok)} | {sum(m['durum'] == 'beklenen' for m in ms)} | {sum(m['durum'].startswith('✗') for m in ms)} | "
                     f"{(sum(fills) / len(fills)):.0%} | {max(delays) if delays else '—'} | {sum(m.get('ardisik_sweeper', 0) for m in ok)} | "
                     f"{sum(m.get('sarki_tekrar', 0) for m in ok)} | {sum(m.get('sanatci_60dk', 0) for m in ok)} |" if fills else
                     f"| {group} | {len(ms)} | {len(ok)} | {sum(m['durum'] == 'beklenen' for m in ms)} | {sum(m['durum'].startswith('✗') for m in ms)} | — | — | — | — | — |")
    return "\n".join(lines)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    seeds = 4
    for a in sys.argv[1:]:
        if a.startswith("--tohum"):
            seeds = int(a.split("=", 1)[1]) if "=" in a else int(sys.argv[sys.argv.index(a) + 1])
    groups = set(args) if args else {"L", "N", "P", "G", "S", "Z", "C", "H", "K"}
    cases = build_cases(groups, range(seeds))
    rows, results = [], []
    for group, name, wp, now, run in cases:
        try:
            res = run_case(name, wp, now, **run)
        except Exception as error:  # noqa: BLE001 - raporlanır
            res = {"name": name, "ok": False, "step": "python", "message": repr(error)[-300:]}
        m = metrics(res)
        res["group"], res["metrics"] = group, m
        results.append(res)
        rows.append((group, name, m))
        print(summary(res)[:230], "|", {k: v for k, v in m.items() if k not in ("mesaj", "tani")}, flush=True)
    os.makedirs(H, exist_ok=True)
    json.dump(results, open(os.path.join(H, "sonuclar_724.json"), "w"), ensure_ascii=False, indent=1, default=str)
    report = "# 7/24 doğrulama koşumu · " + datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC") + "\n\n## Grup özeti\n\n" + \
        group_summary(rows) + "\n\n## Senaryolar\n\n" + table(rows) + "\n"
    open(os.path.join(H, "sonuclar_724.md"), "w", encoding="utf-8").write(report)
    print("\n" + group_summary(rows))
    failed = [r for r in results if r["metrics"]["durum"].startswith("✗")]
    print(f"\n{len(results)} senaryo · {len(failed)} düşen · rapor: {os.path.join(H, 'sonuclar_724.md')}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
