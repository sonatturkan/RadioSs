"""RadioS otomasyon zor kombinasyon senaryoları.

Kullanım:  python3 senaryolar.py [ÖNEK ...] [--kaydet]
  ÖNEK: H (geçiş/zaman/kural uç durumları), K (şarkı ortası katman kontrolü),
        L (canlı 7/24 şema), G (geçiş türleri), S (seçim desenleri), Z (zaman/takvim),
        İ (şarkı işaretleri × geçişler), C (süreklilik: art arda sweeper, yumuşak son, gevşetme)
  --kaydet: her senaryoyu backend'e "TEST · ..." haftalık programı olarak da kaydeder
            (backend doğrulamasını sınar; şemalar Yayın › Şema listesinde görünür).
"""
import copy, sys
from datetime import datetime, timezone
from lib import *
from api import call, SID, put, docs
from e2e import run_all

T = lambda h, m=0, s=0, day=14, month=10: datetime(2026, month, day, h, m, s, tzinfo=timezone.utc)
D = "2026-10-14"


def fixtures():
    M = media(); R = lambda i: entry_row(M[i])
    title = {r["media"]["title"]: i for i, r in reversed(list(enumerate(M)))}  # ilk kopya
    def idx(prefix, nth=0):
        hits = [i for i, r in enumerate(M) if r["media"]["title"].startswith(prefix)]
        return hits[nth]
    ensure_playlist("TEST · Tek parça", [R(idx("Dancing Queen"))])
    ensure_playlist("TEST · Boş", [])
    ensure_playlist("TEST · BPM bilinmiyor", [R(idx("Ayo Technology")), R(idx("Ric Flair Drip"))])
    ensure_playlist("TEST · Uzun miks", [R(idx("DJ Snake")), R(idx("1977"))])
    ensure_playlist("TEST · Aynı sanatçı (50 Cent)", [R(idx("50 Cent - P.I.M.P.")), R(idx("Ayo Technology")), R(idx("Candy Shop")), R(idx("DJ Snake"))])
    ensure_playlist("TEST · Karışık tür", [R(idx("Do It To It (Extended")), R(idx("Radio Sonat - Atmosferik")), R(idx("Dancin")),
                                          R(idx("Radio Sonat - Enerji")), R(idx("LF SYSTEM")), R(idx("Radio Sonat - Gece"))])
    ensure_playlist("TEST · Yavaş (Africa)", [R(idx("Africa"))])
    ensure_playlist("TEST · Cue işaretli", [R(idx("Gimme!")), R(idx("Shouse", 1)), R(idx("Indila"))])
    ensure_playlist("TEST · İşaretli varyant", [R(idx("Roman Blanco", 1)), R(idx("Do It To It (Extended")), R(idx("LF SYSTEM")), R(idx("Dancin"))])
    return playlists()


def build_cases():
    P = fixtures()
    pid = lambda n: P[n]["_id"]
    POWER, TR, GOLD, URBAN = pid("RS · Power Dance"), pid("RS · Türkçe Pop"), pid("RS · Gold & Klasik"), pid("RS · Urban & Hip-Hop")
    NIGHT, SWEEP, NSWEEP = pid("RS · Gece Kulübü (Extended)"), pid("RS · Sweeper & İstasyon ID"), pid("RS · Gece Sweeper")
    ONE, EMPTY, NOBPM, LONG = pid("TEST · Tek parça"), pid("TEST · Boş"), pid("TEST · BPM bilinmiyor"), pid("TEST · Uzun miks")
    MIXED, SLOW, MARKED = pid("TEST · Karışık tür"), pid("TEST · Yavaş (Africa)"), pid("TEST · Cue işaretli")
    C = []
    case = lambda name, wp, now, **run: C.append((name, wp, now, run))
    def one(name, rot, start=600, end=720, day=D, **wk):
        rots = rot if isinstance(rot, list) else [rot]
        return weekly([event(day, start, end, rots[:1], title=name)], rots, **wk)
    kind = lambda music, sweeper: {"music": music, "advertisement": {"mode": 0, "overlap_ms": 0}, "jingle": {"mode": 1, "overlap_ms": 800}, "sweeper": sweeper}

    # --- H: geçiş geometrisi, zaman penceresi, kural uç durumları ---
    case("H1 beatmatch 16sn + 10sn sweeper", one("H1", rotation("H1", [src(POWER, 70), src(SWEEP, 30, media_kind="sweeper")], transitions=tr(0, 0, 0, 100, beat_ms=16000))), T(10, 5))
    case("H2 crossfade 30sn + sweeper", one("H2", rotation("H2", [src(POWER, 70), src(SWEEP, 30, media_kind="sweeper")], transitions=tr(0, 100, 0, 0, cf_ms=30000))), T(10, 5))
    case("H3 AutoDJ tür: sweeper beatmatch 12sn", one("H3", rotation("H3", [src(POWER, 70), src(SWEEP, 30, media_kind="sweeper")], kind=kind({"mode": 3, "overlap_ms": 12000}, {"mode": 3, "overlap_ms": 12000}))), T(10, 5))
    case("H4 cue %100 (işaretsiz parçalar)", one("H4", rotation("H4", [src(POWER), src(GOLD)], transitions=tr(0, 0, 100, 0))), T(10, 5))
    case("H5 beatmatch BPM=0 parçalar", one("H5", rotation("H5", [src(NOBPM), src(POWER)], transitions=tr(0, 0, 0, 100))), T(10, 5))
    case("H6 beatmatch 93↔125 BPM (%34 fark)", one("H6", rotation("H6", [src(SLOW), src(POWER)], selection="linear", transitions=tr(0, 0, 0, 100))), T(10, 5))
    case("H7 low-pass crossfade 20sn + sweeper", one("H7", rotation("H7", [src(POWER, 70), src(SWEEP, 30, media_kind="sweeper")], transitions=tr(0, 100, 0, 0, cf_ms=20000, lowpass=True))), T(10, 5))
    pol = {"schema": 1, "seed": "h8", "transitions": [{"id": U(), "mode": 1, "overlap_ms": 6000, "effect": "lowpass", "early_seconds": 600, "probability": 100}]}
    case("H8 olasılık geçişi early 600sn", one("H8", rotation("H8", [src(POWER)], policy=pol)), T(10, 5))
    for k, ov, label in (("H9a", overlay(SWEEP, "before_end", 5, 100), "before_end 5sn (10sn ses sığmaz)"),
                         ("H9b", overlay(SWEEP, "after_start", 9999, 100), "after_start 9999sn"),
                         ("H9c", overlay(SWEEP, "after_start", 8, 100, player=0), "katman oynatıcısı müzik havuzunda"),
                         ("H9d", overlay(EMPTY, "after_start", 8, 100), "katman playlisti boş"),
                         ("H9e", overlay(LONG, "after_start", 8, 100), "katman uzun müzik (sığmaz)")):
        case(f"{k} {label}", one(k, rotation(k, [src(POWER)], policy={"schema": 1, "jingles": [ov]})), T(10, 5))
    r = rotation("H10", [src(POWER), src(GOLD)], transitions=tr(20, 50, 10, 20))
    case("H10 geç katılım: bitişe 90sn", one("H10", r), T(11, 58, 30))
    case("H11 geç katılım + 15sn hazırlık gecikmesi", one("H11", r), T(10, 33, 7), delay_s=15)
    case("H12a 1 dakikalık blok", one("H12a", r, start=600, end=601), T(10, 0))
    case("H12b 3 dakikalık blok", one("H12b", r, start=600, end=603), T(10, 0))
    case("H13 tekrar aralığı 1000dk (tükenme)", one("H13", rotation("H13", [src(TR), src(URBAN)], gaps=(1000, 1000, 0)), start=600, end=780), T(10, 0))
    case("H14 müzik listesinde sweeper filtresi", one("H14", rotation("H14", [src(POWER, media_kind="sweeper")])), T(10, 5))
    case("H15 yalnız çalınamayan kaynak (boş playlist)", one("H15", rotation("H15", [src(EMPTY)])), T(10, 5))
    one1 = {k: [1] for k in ALL12}
    case("H16 tek oynatıcı (SEMI) + bindirmeler", one("H16", rotation("H16", [src(POWER, autodj=one1), src(SWEEP, 30, media_kind="sweeper", autodj=one1)], players=one1, transitions=tr(0, 50, 0, 50, cf_ms=6000, beat_ms=12000))), T(10, 5), decks="0", semi=True)
    case("H17 3 parçalık listeden adet 100", one("H17", rotation("H17", [src(TR, count=100, order="linear")], selection="linear")), T(10, 5))
    case("H18 50 kaynak", one("H18", rotation("H18", [src([POWER, TR, GOLD, URBAN, SWEEP][i % 5], weight=(i % 7) + 1, media_kind="sweeper" if i % 5 == 4 else "all") for i in range(50)])), T(10, 5))
    case("H19 saat dilimi Europe/Istanbul", one("H19", rotation("H19", [src(POWER), src(GOLD)], transitions=tr(20, 50, 10, 20)), tz="Europe/Istanbul"), T(7, 5))
    r = rotation("H20", [src(POWER), src(GOLD)])
    case("H20 hafta dönüşü (Pazar 23:59:30)", weekly([event("2026-10-18", 1380, 1440, [r]), event("2026-10-12", 0, 60, [r])], [r]), T(23, 59, 30, day=18))
    case("H21 tekrar kapalı, geçmiş hafta", weekly([event("2026-10-05", 600, 720, [r])], [r], anchor="2026-10-05", repeat=False), T(10, 5))
    case("H22 boş playlist + geçerli kaynak", one("H22", rotation("H22", [src(EMPTY), src(POWER)])), T(10, 5))
    rules = [{"entry_id": t["_id"], "weight": 0, "order": 0, "frequency": 1, "max_plays": 0, "start_minute": 0, "end_minute": 1440, "interval_minutes": 0} for t in P["RS · Power Dance"]["tracks"]]
    case("H23 tüm şarkı ağırlıkları 0", one("H23", rotation("H23", [src(POWER, order="weighted", rules=rules)])), T(10, 5))
    ra, rb = rotation("H24a", [src(POWER)]), rotation("H24b", [src(GOLD)])
    epol = {"schema": 1, "seed": "h24", "children": [
        {"id": U(), "scope": "automation", "ids": [ra["id"]], "probability": 70, "policy": {"schema": 1}},
        {"id": U(), "scope": "automation", "ids": [], "probability": 30, "policy": {"schema": 1, "children": [
            {"id": U(), "scope": "playlist", "ids": [GOLD], "probability": 100, "policy": {"schema": 1, "children": [
                {"id": U(), "scope": "track", "ids": [P["RS · Gold & Klasik"]["tracks"][0]["_id"]], "probability": 60, "policy": {"schema": 1}},
                {"id": U(), "scope": "track", "ids": [], "probability": 40, "policy": {"schema": 1}}]}}]}}]}
    case("H24 olasılık grupları otomasyon/playlist/şarkı", weekly([event(D, 600, 720, [ra, rb], policy=epol)], [ra, rb]), T(10, 5))
    case("H25 aynı playlist iki kaynak (art arda tekrar)", one("H25", rotation("H25", [src(POWER, order="linear"), src(POWER, order="linear")], selection="linear")), T(10, 5))
    pol = {"schema": 1, "transitions": [{"id": U(), "mode": 2, "overlap_ms": 2000, "probability": 100}]}
    case("H26 AutoDJ tür + olasılık geçişi birlikte", one("H26", rotation("H26", [src(POWER), src(SWEEP, 30, media_kind="sweeper")], policy=pol, kind=kind({"mode": 3, "overlap_ms": 8000}, {"mode": 1, "overlap_ms": 1000}))), T(10, 5))
    pools = {"music": [1, 2], "advertisement": [1, 2], "jingle": [3], "sweeper": [3]}
    case("H27 sweeper ayrı oynatıcıda (havuz 1-2 / 3)", one("H27", rotation("H27", [src(POWER, autodj=pools), src(SWEEP, 30, media_kind="sweeper", autodj=pools)], transitions=tr(0, 100, 0, 0), players=pools, policy={"schema": 1, "jingles": [overlay(SWEEP, "after_start", 8, 50, player=3)]})), T(10, 5), decks="0,1,2")
    pools3 = {k: [1, 2, 3] for k in ALL12}
    case("H28 üç oynatıcılı havuz", one("H28", rotation("H28", [src(POWER, autodj=pools3), src(GOLD, autodj=pools3)], transitions=tr(0, 100, 0, 0), players=pools3)), T(10, 5), decks="0,1,2")

    # --- K: şarkı ortası katman (olması gereken davranış) ---
    case("K1 katman after_start 8sn %100", one("K1", rotation("K1", [src(POWER)], policy={"schema": 1, "jingles": [overlay(SWEEP, "after_start", 8, 100)]})), T(10, 5))
    case("K2 katman before_end 25sn %100", one("K2", rotation("K2", [src(POWER)], policy={"schema": 1, "jingles": [overlay(SWEEP, "before_end", 25, 100)]})), T(10, 5))
    case("K3 katman %35 aralık 15dk", one("K3", rotation("K3", [src(POWER)], policy={"schema": 1, "seed": "k3", "jingles": [overlay(SWEEP, "after_start", 8, 35, interval=15)]})), T(10, 5))
    case("K4 katman sıklık 3", one("K4", rotation("K4", [src(POWER)], policy={"schema": 1, "jingles": [dict(overlay(SWEEP, "after_start", 8, 100), frequency=3)]})), T(10, 5))
    case("K5 katman +12dB / -60dB ducking", one("K5", rotation("K5", [src(POWER)], policy={"schema": 1, "jingles": [overlay(SWEEP, "after_start", 8, 100, duck=-60, gain=12, fade=5000)]})), T(10, 5))

    # --- L: canlı 7/24 şema ---
    live = [d for d in docs("weekly_programme") if d["name"] == "RadioS 7/24 Yayın Akışı"]
    if live:
        for label, t in (("gece", T(1, 30, day=15)), ("sabah", T(4, 30, day=15)), ("gündüz", T(8, 0, day=15)), ("drive", T(14, 0, day=15)),
                         ("akşam", T(18, 0, day=15)), ("party Cmt", T(9, 0, day=17)), ("kuşak sonu 12:58", T(12, 58, day=15))):
            case(f"L 7/24 şema · {label}", live[0]["config"], t)

    # --- G: geçiş türleri ---
    case("G1 cue işaretli parçalar %100", one("G1", rotation("G1", [src(MARKED)], transitions=tr(0, 0, 100, 0))), T(10, 5))
    case("G2 beatmatch 30sn (azami)", one("G2", rotation("G2", [src(POWER)], transitions=tr(0, 0, 0, 100, beat_ms=30000))), T(10, 5))
    case("G3 low-pass crossfade 1sn", one("G3", rotation("G3", [src(POWER)], transitions=tr(0, 100, 0, 0, cf_ms=1000, lowpass=True))), T(10, 5))
    pol = {"schema": 1, "transitions": [{"id": U(), "mode": 1, "overlap_ms": 4000, "probability": 40}, {"id": U(), "mode": 3, "overlap_ms": 8000, "probability": 30}]}
    case("G4 olasılık toplamı %70 (kalan %30)", one("G4", rotation("G4", [src(POWER)], policy=pol)), T(10, 5))
    pol = {"schema": 1, "transitions": [{"id": U(), "mode": 1, "overlap_ms": 4000, "probability": 100, "excluded_times": [{"start_minute": 600, "end_minute": 660}]}]}
    case("G5 geçiş dışlama 10:00-11:00", one("G5", rotation("G5", [src(POWER)], policy=pol)), T(10, 5))
    night_kind = kind({"mode": 3, "overlap_ms": 16000}, {"mode": 1, "overlap_ms": 1200})
    for d in (12, 13, 14, 15, 16, 17):
        case(f"G6 AutoDJ tür gece + sweeper · gün {d}", one("G6", rotation("G6", [src(NIGHT), src(NSWEEP, 30, media_kind="sweeper")], kind=night_kind), day=f"2026-10-{d}"), T(10, 5, day=d))

    # --- S: seçim desenleri ---
    case("S1 doğrusal: sıklık 1/2/3", one("S1", rotation("S1", [src(POWER), src(GOLD, freq=2), src(SWEEP, freq=3, media_kind="sweeper")], selection="linear")), T(10, 5))
    case("S2 karışık tür, sweeper rastgele (doğrusal)", one("S2", rotation("S2", [src(MIXED, order="linear", sweeper_random=True)])), T(10, 5))
    case("S3 karışık tür, sweeper rastgele (karışık)", one("S3", rotation("S3", [src(MIXED, sweeper_random=True)])), T(10, 5))
    case("S4 kaynak pencereleri 0-60 / 60-120", one("S4", rotation("S4", [src(POWER, start=600, end=660), src(GOLD, start=660, end=720)])), T(10, 5))
    ra, rb, rc = rotation("S5a", [src(POWER)]), rotation("S5b", [src(GOLD)]), rotation("S5c", [src(TR)])
    case("S5 blok doğrusal 3 rotasyon", weekly([event(D, 600, 720, [ra, rb, rc], mode="linear")], [ra, rb, rc]), T(10, 5))
    case("S6 atama ağırlığı 0 / 100", weekly([event(D, 600, 720, [ra, rb], weights=[0, 100])], [ra, rb]), T(10, 5))
    hist = {"kind": "playout_history", "entries": [{"media_id": t["media_id"], "version_id": t["version_id"], "artist": t["artist"], "album": "",
             "started_utc": "2026-10-14T09:%02d:00Z" % (5 * i), "position_frames": 0} for i, t in enumerate(P["RS · Power Dance"]["tracks"][:6])]}
    case("S7 geçmiş: son 6 Power parçası 60dk aralık", one("S7", rotation("S7", [src(POWER)], gaps=(60, 0, 0))), T(10, 5), history=hist)

    # --- Z: zaman / takvim ---
    r = rotation("Z", [src(POWER), src(GOLD)])
    evs = [event("2026-10-12", 600, 720, [r], title="1. hafta"), event("2026-10-20", 600, 720, [r], title="2. hafta Salı")]
    case("Z1 iki haftalık program · 2. hafta", weekly(evs, [r], weeks=2), T(10, 5, day=20))
    case("Z2 iki haftalık · 3. hafta (döngü)", weekly(evs, [r], weeks=2), T(10, 5, day=26))
    case("Z3 yaz saati: Berlin 25.10 01-05 yerel", weekly([event("2026-10-25", 60, 300, [r])], [r], anchor="2026-10-19", tz="Europe/Berlin"), T(1, 30, day=25))
    evs = [event(D, 1380, 1440, [r], title="23-24"), event("2026-10-15", 0, 60, [r], title="00-01")]
    case("Z4 gece yarısı bitişik · 23:59:59", weekly(evs, [r]), T(23, 59, 59))
    case("Z5 gece yarısı bitişik · 00:00:00", weekly(evs, [r]), T(0, 0, 0, day=15))

    # --- C: süreklilik (art arda sweeper yok, yumuşak kuşak sonu, gevşetme) ---
    for label, extra in (("sert bitiş", None), ("yumuşak 300", {"soft_end_seconds": 300})):
        rr = rotation(f"C1 {label}", [src(NIGHT), src(NSWEEP, 30, media_kind="sweeper")], kind=night_kind, extra=extra)
        for d in (12, 15, 16):
            case(f"C1 gece+sweeper {label} · gün {d}", one("C1", rr, day=f"2026-10-{d}"), T(10, 5, day=d))
    case("C2a tükenme · gevşetme kapalı", one("C2a", rotation("C2a", [src(POWER)], gaps=(60, 0, 0))), T(10, 5), history=hist)
    case("C2b tükenme · gevşetme açık", one("C2b", rotation("C2b", [src(POWER)], gaps=(60, 0, 0), extra={"relax_gaps": True})), T(10, 5), history=hist)
    for label, extra in (("sert", None), ("yumuşak 300", {"soft_end_seconds": 300})):
        rr = rotation(f"C3 {label}", [src(POWER), src(GOLD)], transitions=tr(20, 50, 10, 20), extra=extra)
        case(f"C3 bitişe 90sn geç katılım · {label}", one("C3", rr), T(11, 58, 30))
        case(f"C3b bitişe 20sn (yeniden doldurma) · {label}", one("C3b", rr), T(11, 59, 40))
    if live:
        w = copy.deepcopy(live[0]["config"])
        for rr in w["calendar"]["rotation_library"]:
            rr["settings"].update({"soft_end_seconds": 300, "relax_gaps": True})
        for label, t in (("gece", T(22, 30)), ("gündüz sonu", T(12, 57)), ("akşam sonu", T(20, 58, 10))):
            case(f"C4 7/24 yumuşak+gevşet · {label}", w, t)
    return C


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    run_all(build_cases(), put=put if "--kaydet" in sys.argv else None, results_file="sonuclar.json", only=args or None)
