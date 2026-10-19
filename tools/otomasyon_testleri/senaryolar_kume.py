#!/usr/bin/env python3
"""Küme / kümesiz çalışma senaryoları (salt okuma) · Kılavuz §08 "Kümeli ve kümesiz çalışma" ve
"7/24 otomasyon ve doğrulama". Senaryo tanımları: docs/kilavuz-2026-10-18-4c/kume.md (K1–K6,
T1–T4, L1–L6); ek olarak S1 (şema kümeden kümeye devir) ve LT1–LT2 (canlı oynatıcı topolojisindeki
kümede canlı 7/24 akışı).

Her senaryo uygulamanın şema zincirini çalıştırır (e2e.run_case: kuşak seçimi → kapsam
yönlendirmesi → prepare(automation) → takvim planı → prepare(programme) → birleştirme → yerel
plan) ve sonucu doğrular; yalnız yazdırmaz:
  · zincir tamamlanır, öğe sayısı > 0
  · yönlendirilen her kaynak/tür havuzu kuralın modeline eşit (kapsam dışı kayıtlı oynatıcı düşer,
    boş kalan tür kapsamın tamamına döner) ve senaryonun beklediği havuz
  · her satırın havuzu (yerel plan "decks") yönlendirilen havuza eşit; satır havuzu ve seçilen
    fiziksel oynatıcı kapsam içinde (küme dışı oynatıcıya satır düşmez)
  · bindirmeli ardışık iki satır aynı fiziksel oynatıcıda değil; pool_count = kullanılan oynatıcılar
  · art arda iki sweeper yok; geçiş türleri (EOF/XF/CUE/+BS) beklenen kümede
  · senaryoya özgü: tek oynatıcıya düşen bindirme kesmeye döner (K4), havuz devri ve sınır
    geçişi (S1), kuşak/bitiş sınırı ve tanılar (L4–L6)
Veri servisine yalnız desktop/list, desktop-playlists/list ve desktop-playout/prepare gider; hiçbir
şey yazılmaz, ses çalınmaz. Kimlikler senaryo adından türetilir (uuid5): aynı veri ve aynı kod
aynı seçimi verir.

Kullanım:  RADIOS_BUILD_DIR=build-4c RADIOS_E2E_PACE_S=1 python3 senaryolar_kume.py [K T L S LT ...]
Sonuçlar:  <yapı>/otomasyon_testleri/sonuclar_kume.json ve sonuclar_kume.md (ara dosyalar kume/)
"""
import hashlib, json, os, sys, uuid
from collections import Counter
from datetime import datetime, timezone
os.environ.setdefault("RADIOS_E2E_PACE_S", "1")
import e2e, lib
from api import ROOT, docs
from e2e import run_case, us
from lib import event, playlists, rotation, src, tr, weekly

OUT = e2e.H                        # sonuç dosyaları: <yapı>/otomasyon_testleri
e2e.H = os.path.join(OUT, "kume")  # ara dosyalar (case-*.json) ayrı: senaryolar_724.py koşumuyla çakışmaz
KINDS = ("music", "advertisement", "jingle", "sweeper")
MODES = ("EOF", "XF", "CUE", "MAN")
LIVE_NAME = "RadioS 7/24 Yayın Akışı"
D = "2026-10-14"
T = lambda h, m=0, s=0, day=14: datetime(2026, 10, day, h, m, s, tzinfo=timezone.utc)
P = lambda *players: {k: list(players) for k in KINDS}
XF = tr(0, 100, 0, 0)
NOTICE = {"küme": " bu kümede yok; kümenin oynatıcıları kullanılıyor.",
          "kümesizler": " Kümesizler içinde yok; Kümesizler oynatıcıları kullanılıyor.",
          "oynatıcı": " bu oynatıcının kapsamında yok; kapsamın oynatıcıları kullanılıyor."}

# Kimlikler senaryo adından türetilir: kuşak anahtarı (event kimliği) ve dolayısıyla tohum sabit kalır.
_ids = {"ad": "", "n": 0}


def _uid():
    _ids["n"] += 1
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f"radios/senaryolar_kume/{_ids['ad']}/{_ids['n']}"))


lib.U = _uid


def label(e):
    return MODES[e["mode"]] + ("+BS" if e["beat_sync"] else "") + ("+LP" if e["effect"] else "")


def overlapping(lab):
    return not lab.startswith(("EOF", "MAN"))


def autodj_kind(entry):
    return entry["kind"] if entry["kind"] in KINDS[1:] else "music"


def rule_labels(rule):
    """Bir geçiş kuralının yerel planda görülebilecek etiketleri. Belgeli bozulmalar dahil: beatmatch,
    ritim analizi geçersizse düz crossfade olur (planner.c calendar_beatmatch_plan); low-pass,
    bindirme kalmazsa düşer (transition_filter_without_overlap)."""
    mode, lowpass = rule.get("mode", 0), rule.get("effect") == "lowpass"
    if mode == 0:
        return {"EOF"}
    if mode == 2:
        return {"CUE", "CUE+LP"} if lowpass else {"CUE"}
    base = {"XF", "XF+BS"} if mode == 3 else {"XF"}
    return base | ({b + "+LP" for b in base} if lowpass else set())


def configured_labels(settings, kind_out, kind_in):
    """Rotasyon ayarının bu sınır için izin verdiği geçişler (automation_calendar_plan.c: kind_transitions
    varsa sınırın türü — gelen müzik dışıysa onun, değilse gidenin — kuralı; yoksa ağırlığı > 0 olan
    transitions satırları; AutoDJ otomasyonu açıksa tasarlanmış ön ayarlar)."""
    policy = settings.get("kind_transitions")
    if policy:
        rules = [policy.get(kind_in if kind_in != "music" else kind_out) or {"mode": 0}]
    else:
        rules = [t for t in settings.get("transitions", []) if t.get("weight", 0) > 0]
    labels = set().union(*(rule_labels(r) for r in rules)) if rules else set()
    if (settings.get("autodj_automation") or {}).get("enabled"):
        for preset in settings.get("autodj_presets") or []:
            labels |= {"XF", "XF+BS"} if preset.get("beat_sync") else {"XF"}
    return labels


def route_model(saved, scope1):
    """automation_scope.c:routed_distribution kuralı: kayıtlı sıra korunur, kapsam dışı düşer,
    boş kalan tür kapsamın tamamına döner; kayıt yoksa her tür kapsamın tamamı."""
    if saved is None:
        return {k: list(scope1) for k in KINDS}
    return {k: [p for p in saved[k] if p in scope1] or list(scope1) for k in KINDS}


def saved_pool(library, source):
    """Yönlendirilmiş kaynağın kayıtlı havuzu: kaynağın autodj'si, yoksa rotasyon (+kaynak) ayarındaki autodj_players."""
    rot = library.get(source.get("calendar_rotation_id"))
    orig = next((s for s in (rot or {}).get("sources", []) if s["id"] == source.get("calendar_source_id")), None)
    if orig is None:
        return None
    if "autodj" in orig:
        return orig["autodj"]
    merged = dict(rot.get("settings") or {}, **(orig.get("settings") or {}))
    return merged.get("autodj_players")


def live_groups():
    """Canlı oynatıcı topolojisi (desktop/list live_players): {küme adı: bağlama sırasındaki sıfır tabanlı üyeler}."""
    rows = [d for d in docs("live_players") if d.get("status", "active") == "active"]
    if not rows:
        return {}, None
    cfg = rows[0]["config"]
    players = cfg.get("players", {})
    slots = sorted(cfg.get("slots", list(range(cfg.get("deck_count", 0)))))
    groups = {}
    for cluster in cfg.get("clusters", []):
        members = [s for s in slots if players.get(str(s + 1), {}).get("cluster_id", "") == cluster["id"]]
        first = players.get(str(members[0] + 1), {}) if members else {}
        target = first.get("autodj_target", 0) - 1  # automation_scope.c: devam hedefi ikinci sıraya alınır
        if first.get("autodj_mode") == 2 and target in members[1:]:
            members.remove(target); members.insert(1, target)
        groups[cluster["name"]] = dict(members=members, automation_id=cluster.get("automation_id", ""))
    return groups, rows[0]


def build_cases(groups):
    PL = playlists()
    short = {p["_id"]: name.replace("7/24 · ", "") for name, p in PL.items()}

    def pid(name):
        full = "7/24 · " + name
        if full not in PL:
            raise SystemExit(f"7/24 playlisti bulunamadı: {full}")
        return PL[full]["_id"]
    POWER, HOUSE, GOLD, TRPOP, POP, URBAN, SWEEP = map(pid, (
        "Dans & EDM A", "House & Club A", "Klasikler (Gold) A", "Türkçe Pop A", "Pop Hitleri A",
        "Urban & Latin A", "Sweeper · Enerji"))
    live = [d for d in docs("weekly_programme") if d["name"] == LIVE_NAME]
    if not live:
        raise SystemExit("canlı 7/24 haftalık programı bulunamadı")
    W = live[0]["config"]
    topology, topo_doc = live_groups()
    meta = {"canli_program": {"ad": LIVE_NAME, "_id": live[0]["_id"], "_rev": live[0].get("_rev"),
                              "guncelleme": live[0].get("updated_at")},
            "canli_topoloji": {"_rev": (topo_doc or {}).get("_rev"),
                               "kumeler": {n: [m + 1 for m in g["members"]] for n, g in topology.items()}}}
    C = []

    def case(group, name, wp, now, decks, scope="küme", semi=False, labels=None, **expect):
        if group in groups:
            C.append(dict(group=group, name=name, wp=wp, now=now, decks=decks, scope=scope, semi=semi,
                          labels=labels or {}, short=short, expect=expect))

    def synth(key, rows, players=None, **kw):
        """rows: [(etiket, playlist, kayıtlı havuz | None, src seçenekleri)]. None: kaynakta/rotasyonda havuz kaydı yok."""
        _ids.update(ad=key, n=0)
        built = []
        for lab, playlist, pool, opt in rows:
            row = src(playlist, autodj=pool, **opt)
            if pool is None:
                row.pop("autodj")
            built.append((lab, row))
        rot = rotation(key, [r for _, r in built], players=players, **kw)
        if players is None:
            rot["settings"].pop("autodj_players")
        return weekly([event(D, 600, 720, [rot], title=key)], [rot]), {r["id"]: lab for lab, r in built}

    # --- K: kümeli ---
    wp, lab = synth("K1", [("A", POWER, None, {}), ("B", GOLD, None, {})], transitions=XF)
    case("K", "K1 küme {1,2} · varsayılan havuz · crossfade", wp, T(10, 5), "0,1", labels=lab,
         pools={"A": P(1, 2), "B": P(1, 2)}, eksik=set(), allowed={"XF"}, required={"XF"}, pool_count=2)
    wp, lab = synth("K2", [("A", POWER, P(1, 2, 3, 4), {}), ("B", GOLD, P(1, 2, 3, 4), {})], players=P(1, 2, 3, 4), transitions=XF)
    case("K", "K2 kayıtlı havuz {1,2,3,4} iki üyeli kümede (daralma)", wp, T(10, 5), "0,1", labels=lab,
         pools={"A": P(1, 2), "B": P(1, 2)}, eksik={3, 4}, allowed={"XF"}, required={"XF"}, pool_count=2)
    wp, lab = synth("K3", [("A", POWER, P(1, 2), {}), ("B", HOUSE, P(3, 4), {})], players=P(1, 2), transitions=XF)
    case("K", "K3 kümeler arası havuz A {1,2} / B {3,4} · A kümesinde", wp, T(10, 5), "0,1", labels=lab,
         pools={"A": P(1, 2), "B": P(1, 2)}, eksik={3, 4}, allowed={"XF"}, required={"XF"}, pool_count=2)
    wp, lab = synth("K4", [("A", POWER, P(1, 2), {}), ("B", HOUSE, P(2), {"count": 2})], players=P(1, 2), transitions=XF)
    case("K", "K4 küme içi satır havuzu {1,2} → {2} (B iki ardışık)", wp, T(10, 5), "0,1", labels=lab,
         pools={"A": P(1, 2), "B": P(2)}, eksik=set(), allowed={"XF"}, required={"XF"}, pool_count=2, tek_oynatici_kesme=True)
    wp, lab = synth("K5", [("Müzik", POWER, None, {"weight": 70}), ("Sweeper", SWEEP, None, {"weight": 30, "media_kind": "sweeper"})], transitions=XF)
    case("K", "K5 üç üyeli küme + sweeper", wp, T(10, 5), "0,1,2", labels=lab,
         pools={"Müzik": P(1, 2, 3), "Sweeper": P(1, 2, 3)}, eksik=set(), allowed={"XF"}, required={"XF"}, pool_count=3, turler={"sweeper"})
    wp, lab = synth("K6", [(k, pl, P(1, 2, 3, 4), {}) for k, pl in (("A", POWER), ("B", HOUSE), ("C", POP), ("D", URBAN))],
                    players=P(1, 2, 3, 4), transitions=tr(0, 50, 0, 50, beat_ms=8000))
    case("K", "K6 dört üyeli küme · dört kaynak · XF + beatmatch", wp, T(10, 5), "0,1,2,3", labels=lab,
         pools={k: P(1, 2, 3, 4) for k in "ABCD"}, eksik=set(), allowed={"XF", "XF+BS"}, required={"XF", "XF+BS"}, pool_count=4)

    # --- T: kümesiz ---
    wp, lab = synth("T1", [("A", POWER, P(1, 2), {}), ("B", GOLD, P(1, 2), {})], players=P(1, 2), transitions=XF)
    case("T", "T1 tek bağımsız oynatıcı (Yarı AutoDJ) · havuz {1,2} → [1]", wp, T(10, 5), "0", scope="oynatıcı", semi=True, labels=lab,
         pools={"A": P(1), "B": P(1)}, eksik={2}, allowed={"EOF"}, pool_count=1)
    wp, lab = synth("T2", [("A", HOUSE, P(1, 2), {})], players=P(1, 2), transitions=tr(0, 0, 0, 100, beat_ms=8000))
    case("T", "T2 tek bağımsız oynatıcı 3 (Yarı) · beatmatch istenmiş", wp, T(10, 5), "2", scope="oynatıcı", semi=True, labels=lab,
         pools={"A": P(3)}, eksik={1, 2}, allowed={"EOF"}, pool_count=1)
    wp, lab = synth("T3", [("A", TRPOP, None, {}), ("B", POP, None, {})], transitions=XF)
    case("T", "T3 Kümesizler · 3 oynatıcı", wp, T(10, 5), "0,1,2", scope="kümesizler", labels=lab,
         pools={"A": P(1, 2, 3), "B": P(1, 2, 3)}, eksik=set(), allowed={"XF"}, required={"XF"}, pool_count=3)
    wp, lab = synth("T4", [("A", GOLD, None, {}), ("B", URBAN, None, {})], transitions=XF)
    case("T", "T4 bağımsız 5 + AutoDJ devam 6", wp, T(10, 5), "4,5", scope="oynatıcı", labels=lab,
         pools={"A": P(5, 6), "B": P(5, 6)}, eksik=set(), allowed={"XF"}, required={"XF"}, pool_count=2)

    # --- S: şema kümeden kümeye devir (dört oynatıcılı kapsamda iki havuz) ---
    wp, lab = synth("S1", [("A", POWER, P(1, 2), {"count": 3}), ("B", HOUSE, P(3, 4), {"count": 3})],
                    players=P(1, 2, 3, 4), selection="linear", transitions=XF)
    case("S", "S1 şema kümeden kümeye devir · A {1,2} → B {3,4}", wp, T(10, 5), "0,1,2,3", labels=lab,
         pools={"A": P(1, 2), "B": P(3, 4)}, eksik=set(), allowed={"XF"}, required={"XF"}, pool_count=4,
         fiziksel={"A": {1, 2}, "B": {3, 4}}, sinir_gecisi=True)

    # --- L: canlı "RadioS 7/24 Yayın Akışı" (kayıtlı havuz yok → kapsamın tamamı; geçişler rotasyon ayarından) ---
    case("L", "L1 canlı 7/24 · tek üye Yarı AutoDJ · Çar 10:05", W, T(10, 5), "0", scope="oynatıcı", semi=True,
         pools="kapsam", allowed={"EOF"}, pool_count=1, kusak=(T(10), T(14)))
    case("L", "L2 canlı 7/24 · iki üyeli küme · Çar 03:30", W, T(3, 30), "0,1",
         pools="kapsam", allowed="ayar", required_any=True, pool_count=2, kusak=(T(0), T(6)))
    case("L", "L3 canlı 7/24 · dört üyeli küme · Çar 18:45", W, T(18, 45), "0,1,2,3",
         pools="kapsam", allowed="ayar", required_any=True, pool_count=4, kusak=(T(18), T(21)))
    case("L", "L4 canlı 7/24 · Kümesizler 3 oynatıcı · Çar 23:58 (gece yarısı)", W, T(23, 58), "0,1,2", scope="kümesizler",
         pools="kapsam", allowed="ayar", kusak=(T(21), T(0, day=15)), bitis=True)
    case("L", "L5 canlı 7/24 · hafta dönüşü · Paz 23:55", W, T(23, 55, day=18), "0,1",
         pools="kapsam", allowed="ayar", kusak=(T(20, day=18), T(0, day=19)), bitis=True)
    case("L", "L6 canlı 7/24 · kuşak sonuna 40 sn · Çar 13:59:20", W, T(13, 59, 20), "0,1",
         pools="kapsam", allowed="ayar", kusak=(T(10), T(14)), bitis=True, tani={"calendar_soft_end_overrun"})

    # --- LT: canlı oynatıcı topolojisinde canlı programa atanmış küme. Kullanıcının o anki kayıtlı
    # havuzları sınanır: beklenti sabit havuz değil kuralın kendisi (pools="kural") ---
    assigned = [(n, g) for n, g in topology.items() if g["automation_id"] == live[0]["_id"] and len(g["members"]) > 1]
    if assigned:
        name, g = assigned[0]
        decks, members = ",".join(map(str, g["members"])), "-".join(str(m + 1) for m in g["members"])
        case("LT", f"LT1 canlı topoloji · küme '{name}' (Oynatıcı {members}) · Paz 01:00 After Party", W, T(1, 0, day=18), decks,
             pools="kural", allowed="ayar", required_any=True, kusak=(T(0, day=18), T(3, day=18)))
        case("LT", f"LT2 canlı topoloji · küme '{name}' (Oynatıcı {members}) · Paz 13:45 Pazar Türkçe Pop", W, T(13, 45, day=18), decks,
             pools="kural", allowed="ayar", required_any=True, kusak=(T(12, day=18), T(16, day=18)))
    elif "LT" in groups:
        print("! canlı topolojide canlı 7/24 programına atanmış çok üyeli küme yok: LT senaryoları atlandı", flush=True)
    return C, meta


def verify(c, res, routed):
    """Beklentileri sınar: (bilgi, [(geçti, açıklama)])."""
    x, checks, info = c["expect"], [], {}

    def need(ok, text):
        checks.append((bool(ok), text))
        return ok
    scope1 = [int(d) + 1 for d in c["decks"].split(",")]
    info["kapsam"] = scope1
    if not need(res.get("ok"), f"zincir tamamlandı (adım: {res.get('step')})"):
        info["mesaj"] = (res.get("message") or "")[:400]
        return info, checks
    ents, disp = res["entries"], res.get("display") or {}
    rows, n = disp.get("entries", []), len(res["entries"])
    diags = [d.get("reason") for d in disp.get("diagnostics", [])]
    cal_diags = res.get("calendar_diagnostics", [])
    labs = [label(e) for e in ents]
    info.update(oge=n, turler=dict(Counter(e["kind"] for e in ents)), gecisler=dict(Counter(labs)), pool_count=res.get("pool"),
                tanilar=sorted(set(cal_diags) | set(diags)), kusak_baslik=res.get("occ", "").split("title=")[-1].split(" start=")[0])
    need(n > 0, "öğe sayısı > 0")
    need(len(rows) == n, f"yerel plan görünümü {len(rows)} satır = {n} öğe")
    if x.get("kusak"):
        a, b = x["kusak"]
        day = ("Pzt", "Sal", "Çar", "Per", "Cum", "Cmt", "Paz")
        need((res["start"], res["end"]) == (us(a), us(b)), f"kuşak {day[a.weekday()]} {a:%H:%M}–{day[b.weekday()]} {b:%H:%M} seçildi")

    # 1) Yönlendirme: C çıktısı (yönlendirilmiş event) = kural modeli = senaryonun beklediği havuz.
    library = {r["id"]: r for r in c["wp"]["calendar"]["rotation_library"]}
    by_id, routed_pools, dropped, bad = {}, {}, set(), []
    for s in (routed or {}).get("rotations", []):
        lab = c["labels"].get(s.get("calendar_source_id")) or c["short"].get(s.get("playlist_id"), s.get("playlist_id", "?"))
        by_id[s["id"]] = (lab, s)
        saved, got = saved_pool(library, s), s.get("autodj")
        routed_pools[lab] = got
        if saved:
            dropped |= {p for k in KINDS for p in saved[k] if p not in (got or {}).get(k, [])}
        if got != route_model(saved, scope1):
            bad.append(f"{lab}: {got} ≠ kural {route_model(saved, scope1)}")
        want = x.get("pools")
        want = {k: list(scope1) for k in KINDS} if want == "kapsam" else want.get(lab) if isinstance(want, dict) else None
        if want is not None and got != want:
            bad.append(f"{lab}: {got} ≠ beklenen {want}")
    need(routed and by_id, "yönlendirilmiş event okundu")
    need(not bad, "yönlendirilen havuzlar kural ve beklentiyle aynı" + (": " + "; ".join(bad[:4]) if bad else ""))
    info["havuzlar"] = routed_pools
    info["dusen_oynaticilar"] = sorted(dropped)
    if x.get("eksik") is not None:
        need(dropped == x["eksik"], f"kapsam dışı düşen kayıtlı oynatıcılar {sorted(dropped)} = beklenen {sorted(x['eksik'])}")
    info["uyari"] = ("Şemadaki " + ", ".join(f"Oynatıcı {p}" for p in sorted(dropped)) + NOTICE[c["scope"]]) if dropped else None

    # 2) Satırlar: havuz = yönlendirilen havuz; havuz ve fiziksel oynatıcı kapsamda; bindirmede farklı oynatıcı.
    decks = [r.get("deck_index") for r in rows]
    info["fiziksel"] = "-".join(str(d + 1) if d is not None else "?" for d in decks[:24]) + ("…" if n > 24 else "")
    pool_bad, outside, phys_bad, same = [], [], [], []
    for i, (e, r) in enumerate(zip(ents, rows)):
        lab, s = by_id.get(r.get("source_id"), (None, None))
        kind = e["kind"] if e["kind"] in KINDS[1:] else "music"
        pool = [d + 1 for d in e["decks"]]
        if s is None or pool != (s.get("autodj") or {}).get(kind):
            pool_bad.append(f"#{i} {lab}/{kind} {pool}")
        if not set(pool) <= set(scope1) or decks[i] is None or decks[i] + 1 not in scope1:
            outside.append(f"#{i} havuz {pool} oynatıcı {None if decks[i] is None else decks[i] + 1}")
        if e.get("adjunct") and e.get("adjunct_player", -1) + 1 not in scope1:
            outside.append(f"#{i} katman oynatıcısı {e.get('adjunct_player', -1) + 1}")
        if decks[i] is not None and decks[i] + 1 not in pool:
            phys_bad.append(f"#{i} oynatıcı {decks[i] + 1} ∉ {pool}")
        if e["overlap_s"] > 0 and i + 1 < len(decks) and decks[i + 1] == decks[i]:
            same.append(f"#{i}→#{i + 1} oynatıcı {decks[i] + 1}")
    need(not pool_bad, "satır havuzları yönlendirilen havuzla aynı" + (": " + ", ".join(pool_bad[:5]) if pool_bad else ""))
    need(not outside, "küme/kapsam dışı oynatıcıya satır yok" + (": " + ", ".join(outside[:5]) if outside else ""))
    need(not phys_bad, "fiziksel oynatıcı satırın havuzunda" + (": " + ", ".join(phys_bad[:5]) if phys_bad else ""))
    need(not same, "bindirmeli ardışık satırlar farklı oynatıcıda" + (": " + ", ".join(same[:5]) if same else ""))
    used = sorted({d for d in decks if d is not None})
    info["kullanilan"] = [d + 1 for d in used]
    need(res.get("pool") == len(used), f"pool_count {res.get('pool')} = kullanılan oynatıcı sayısı {len(used)}")
    if x.get("pool_count") is not None:
        need(res.get("pool") == x["pool_count"], f"pool_count {res.get('pool')} = beklenen {x['pool_count']}")

    # 3) Süreklilik ve geçişler.
    pairs = sum(1 for a, b in zip(ents, ents[1:]) if a["kind"] == b["kind"] == "sweeper")
    need(pairs == 0, f"art arda sweeper yok ({pairs})")
    for k in x.get("turler", ()):
        need(info["turler"].get(k), f"'{k}' türü planda")
    dropped_ids = {d.get("entry_id") for d in disp.get("diagnostics", []) if d.get("reason") == "routing_overlap_dropped"}
    cut = {i for i, r in enumerate(rows) if r.get("entry_id") in dropped_ids}
    allowed = x.get("allowed")
    if allowed:
        odd, union = [], set()
        for i, l in enumerate(labs):
            if allowed == "ayar":  # canlı rotasyon: bu sınırın kendi ayarı
                settings = (by_id.get(rows[i].get("source_id"), (None, None))[1] or {}).get("calendar_settings") or {}
                ok_set = configured_labels(settings, autodj_kind(ents[i]), autodj_kind(ents[i + 1]) if i + 1 < n else autodj_kind(ents[i]))
            else:
                ok_set = allowed
            union |= ok_set
            # EOF'a belgeli iki dönüş: tek oynatıcıya düşen bindirme (routing_overlap_dropped) ve planın tek
            # fiziksel oynatıcı kullandığı partide son öğe (planner.c: "The batch's last item has no successor").
            if l in ok_set or (l == "EOF" and (i in cut or (i == n - 1 and res.get("pool") == 1))):
                continue
            odd.append(f"#{i} {l}")
        info["izinli_gecisler"] = sorted(union)
        need(not odd, f"geçişler {sorted(union)} içinde" + (": " + ", ".join(odd[:6]) if odd else ""))
    for l in x.get("required", ()):
        need(l in labs, f"'{l}' geçişi var")
    if x.get("required_any") and n > 1:
        need(any(overlapping(l) for l in labs), "en az bir bindirmeli geçiş var")
    if c["semi"]:
        need(all(l == "EOF" for l in labs) and not any(e["beat_sync"] for e in ents), "Yarı AutoDJ: bütün geçişler EOF, beatmatch yok")
    if x.get("tek_oynatici_kesme"):
        single = [i for i in range(n - 1) if len(ents[i]["decks"]) == 1 and ents[i]["decks"] == ents[i + 1]["decks"]]
        wrong = [f"#{i} {labs[i]}" for i in single if labs[i] != "EOF" or ents[i]["overlap_s"] > 0 or i not in cut]
        need(single, f"tek oynatıcılı havuzda ardışık satır çifti var ({len(single)})")
        need(not wrong, "tek oynatıcıya düşen bindirme kesmeye döndü (routing_overlap_dropped)" + (": " + ", ".join(wrong[:5]) if wrong else ""))
        info["kesmeye_donen"] = len(cut)
    for lab, allowed_decks in (x.get("fiziksel") or {}).items():
        wrong = [f"#{i} {d + 1}" for i, (r, d) in enumerate(zip(rows, decks)) if by_id.get(r.get("source_id"), ("",))[0] == lab and d + 1 not in allowed_decks]
        need(not wrong, f"{lab} satırları {sorted(allowed_decks)} oynatıcılarında" + (": " + ", ".join(wrong[:5]) if wrong else ""))
    if x.get("sinir_gecisi"):
        bounds = [i for i in range(n - 1) if rows[i].get("source_id") != rows[i + 1].get("source_id")]
        lost = [f"#{i} {labs[i]}" for i in bounds if not overlapping(labs[i]) or ents[i]["overlap_s"] <= 0]
        need(bounds, f"havuz değişen kaynak sınırı var ({len(bounds)})")
        need(not lost, "kaynak/havuz sınırında bindirmeli geçiş korundu" + (": " + ", ".join(lost[:5]) if lost else ""))
        need("routing_overlap_dropped" not in diags, "routing_overlap_dropped yok")
    for reason in x.get("tani", ()):
        need(reason in cal_diags or reason in diags, f"tanı {reason}")
    if ents:
        last = ents[-1]
        info["son_sapma_s"] = round((last["planned_utc_us"] + last["cue_s"] * 1e6 - res["end"]) / 1e6, 1)
        if x.get("bitis"):
            soft = max([(s.get("calendar_settings") or {}).get("soft_end_seconds", 0) for _, s in by_id.values()] or [0])
            limit = soft + max(30.0, last["cue_s"] / 5)
            need(info["son_sapma_s"] <= limit, f"son öğe kuşak sonundan en çok {limit:.0f} sn taşar ({info['son_sapma_s']:+} sn)")
    return info, checks


def fmt_pools(pools):
    out = []
    for lab, p in (pools or {}).items():
        if not p:
            out.append(f"{lab}:?")
            continue
        text = f"{lab}:{p['music']}"
        others = {k: p[k] for k in KINDS[1:] if p[k] != p["music"]}
        if others:
            text += " (" + ", ".join(f"{k[:3]}:{v}" for k, v in others.items()) + ")"
        out.append(text)
    return "; ".join(out) if len(out) < 4 else f"{len(out)} kaynak: " + "; ".join(sorted(set(o.split(":", 1)[1] for o in out)))


def report(results, meta, build):
    ok = sum(r["durum"] == "✓" for r in results)
    lines = [f"# Küme / kümesiz senaryoları · {datetime.now(timezone.utc):%Y-%m-%d %H:%M} UTC", "",
             "Komut: `cd tools/otomasyon_testleri && RADIOS_BUILD_DIR=" + os.environ.get("RADIOS_BUILD_DIR", "build") +
             " RADIOS_E2E_PACE_S=" + os.environ.get("RADIOS_E2E_PACE_S", "1") + " python3 senaryolar_kume.py`", "",
             "Kod (UTC saat / sha256): " + ", ".join(f"{k} {v}" for k, v in build.items()) +
             ("; **uyarı: " + "; ".join(meta.get("derleme_uyarilari")) + "**" if meta.get("derleme_uyarilari") else "; derleme güncel"), "",
             f"Canlı veri: \"{meta['canli_program']['ad']}\" rev {meta['canli_program']['_rev']} ({meta['canli_program']['guncelleme']}), "
             f"live_players rev {meta['canli_topoloji']['_rev']} kümeler {meta['canli_topoloji']['kumeler']}", "",
             f"**Sonuç: {ok}/{len(results)} geçti.** Oynatıcı numaraları 1 tabanlı; \"fiziksel\" yerel planın her satır için seçtiği oynatıcı.", "",
             "| Grup | Senaryo | Kapsam | Durum | Öğe | Türler | pool_count (kullanılan) | Yönlendirilen havuzlar | Geçişler | Fiziksel sıra | Uyarı (kural; e2e yazdırmaz) | Tanılar |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in results:
        i = r["bilgi"]
        lines.append(f"| {r['grup']} | {r['ad']} | {i.get('kapsam')}{' Yarı' if r['yari'] else ''} | {r['durum']} | {i.get('oge', '—')} | "
                     f"{i.get('turler', '—')} | {i.get('pool_count', '—')} ({i.get('kullanilan', '—')}) | {fmt_pools(i.get('havuzlar'))} | "
                     f"{i.get('gecisler', '—')} | {i.get('fiziksel', '—')} | {i.get('uyari') or '—'} | {', '.join(i.get('tanilar', [])) or '—'} |")
    failed = [r for r in results if r["durum"] != "✓"]
    lines += ["", "## Tutmayan beklentiler", ""]
    lines += [f"- **{r['ad']}**: " + "; ".join(t for okk, t in r["denetimler"] if not okk) + (f" — {r['bilgi'].get('mesaj')}" if r["bilgi"].get("mesaj") else "")
              for r in failed] or ["Yok."]
    return "\n".join(lines) + "\n"


def build_info():
    """Sınanan kodun kimliği ve sürüklenme uyarıları (e2e kütüphaneden eski / kaynak kütüphaneden yeni)."""
    mtime = lambda path: os.path.getmtime(path) if os.path.exists(path) else 0
    stamp = lambda t: datetime.fromtimestamp(t, timezone.utc).strftime("%H:%M:%S") if t else "?"
    build = os.path.dirname(OUT)
    info, warnings = {"e2e": stamp(mtime(e2e.E2E))}, []
    sources = {"libradios_core.a": ("src/core/automation_scope.c", "src/core/automation_calendar_plan.c",
                                    "src/core/weekly_programme_plan.c"), "libradios_playout.a": ("src/playout/planner.c",)}
    for lib_name, paths in sources.items():
        lib_time = mtime(os.path.join(build, lib_name))
        info[lib_name] = stamp(lib_time)
        if lib_time > mtime(e2e.E2E):
            warnings.append(f"e2e ikilisi {lib_name}'den eski: build.sh ile yeniden bağlayın")
        for path in paths:
            full = os.path.join(ROOT, path)
            try:
                info[os.path.basename(path)] = "sha256:" + hashlib.sha256(open(full, "rb").read()).hexdigest()[:12]
            except OSError:
                info[os.path.basename(path)] = "?"
            if mtime(full) > lib_time:
                warnings.append(f"{path} {lib_name}'den yeni: derlenmemiş değişiklik sınanmadı")
    return info, warnings


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    groups = set(args) if args else {"K", "T", "S", "L", "LT"}
    build, warnings = build_info()
    for warning in warnings:
        print("! " + warning, flush=True)
    cases, meta = build_cases(groups)
    meta["derleme_uyarilari"] = warnings
    os.makedirs(e2e.H, exist_ok=True)
    results = []
    for c in cases:
        for name in ("event", "config", "prepared", "sel", "result", "merged", "native"):
            try:
                os.remove(os.path.join(e2e.H, f"case-{name}.json"))
            except FileNotFoundError:
                pass
        try:
            res = run_case(c["name"], c["wp"], c["now"], decks=c["decks"], semi=c["semi"])
        except Exception as error:  # noqa: BLE001 - raporlanır
            res = {"name": c["name"], "ok": False, "step": "python", "message": repr(error)[-300:]}
        routed = None
        if res.get("occ", "").startswith("OCC ok"):
            routed = json.load(open(os.path.join(e2e.H, "case-event.json")))
        try:
            info, checks = verify(c, res, routed)
        except Exception as error:  # noqa: BLE001 - doğrulayıcı hatası da ✗ sayılır
            info, checks = {"mesaj": repr(error)[-300:]}, [(False, "doğrulayıcı hatası")]
        failed = [t for okk, t in checks if not okk]
        row = {"grup": c["group"], "ad": c["name"], "yari": c["semi"], "durum": "✓" if not failed else "✗", "bilgi": info,
               "denetimler": checks, "satirlar": [dict(baslik=e["title"], tur=e["kind"], gecis=label(e), bindirme_s=round(e["overlap_s"], 2),
                                                       havuz=[d + 1 for d in e["decks"]], oynatici=None if r.get("deck_index") is None else r["deck_index"] + 1,
                                                       kaynak=r.get("source_name"))
                                                  for e, r in zip(res.get("entries") or [], (res.get("display") or {}).get("entries", []))]}
        results.append(row)
        print(f"{row['durum']} {c['name']}: öğe={info.get('oge', '—')} pool={info.get('pool_count', '—')} kullanılan={info.get('kullanilan', '—')} "
              f"geçiş={info.get('gecisler', '—')} fiziksel={info.get('fiziksel', '—')}" + (" | ✗ " + "; ".join(failed)[:400] if failed else ""), flush=True)
    json.dump({"tarih": datetime.now(timezone.utc).isoformat(), "kod": build, "veri": meta, "sonuclar": results},
              open(os.path.join(OUT, "sonuclar_kume.json"), "w"), ensure_ascii=False, indent=1, default=str)
    open(os.path.join(OUT, "sonuclar_kume.md"), "w", encoding="utf-8").write(report(results, meta, build))
    bad = [r for r in results if r["durum"] != "✓"]
    print(f"\n{len(results)} senaryo · {len(results) - len(bad)} geçti · {len(bad)} düşen · rapor: {os.path.join(OUT, 'sonuclar_kume.md')}")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
