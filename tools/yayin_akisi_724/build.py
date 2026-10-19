"""RadioS 7/24 format with ~50-track category packs (prepare budget: 1.5 MB per block).

  python3 build2.py DIR plan        pack split + weekly pack assignment report (no writes)
  python3 build2.py DIR playlists   create pack playlists (skips existing names)
  python3 build2.py DIR compose     write main_new.json / weekly.json locally
  python3 build2.py DIR write       save main calendar + weekly programme
"""
import json, os, sys, uuid, collections, datetime
D = sys.argv[1]; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); sys.path.insert(0, D)
import classify, schedule as sc

U = lambda: str(uuid.uuid4())
MAIN_FROM, MAIN_TO = datetime.date(2026, 10, 17), datetime.date(2026, 11, 15)
WEEK_ANCHOR = datetime.date(2026, 10, 12)
PROGRAMME_NAME = "RadioS 7/24 Yayın Akışı"
PACKS = {"TR": 5, "POP": 3, "URBAN": 2, "EDM": 8, "HOUSE": 3, "DEEP": 2, "CLASSIC": 4, "MASHUP": 1, "DJSET": 1}
LABEL = {"TR": "Türkçe Pop", "POP": "Pop Hitleri", "URBAN": "Urban & Latin", "EDM": "Dans & EDM", "HOUSE": "House & Club",
         "DEEP": "Deep & Melodic", "CLASSIC": "Klasikler (Gold)", "MASHUP": "Mashup & Parti", "DJSET": "DJ Set & Megamix"}
DAYS = ["Pzt", "Sal", "Çar", "Per", "Cum", "Cmt", "Paz"]
BYTES_PER_TRACK = 4300; BUDGET = 1_400_000

def pack_name(cat, i): return "7/24 · %s%s" % (LABEL[cat], (" " + "ABCDEFGH"[i]) if PACKS[cat] > 1 else "")

def split():
    recs = json.load(open(D + "/media.json")); rows = collections.defaultdict(list)
    for r in recs: rows[classify.chill_override(r, classify.classify(r))].append(r)
    packs = {}
    for cat, k in PACKS.items():
        items = sorted(rows[cat], key=lambda r: ((r["media"].get("artist") or "~").lower(), (r["media"].get("title") or "").lower()))
        for i in range(k): packs[(cat, i)] = items[i::k]   # round-robin spreads one artist over packs
    return packs

def assign(packs):
    hours = {p: sum(r["version"]["duration_ms"] for r in rows) / 3.6e6 for p, rows in packs.items()}
    avg = {cat: sum(hours[(cat, i)] for i in range(PACKS[cat])) * 60 / sum(len(packs[(cat, i)]) for i in range(PACKS[cat])) for cat in PACKS}
    air = collections.Counter(); slots = []
    for d, day in enumerate(sc.WEEK):
        used_today = set()
        for start, end, key in day:
            weights = sc.ROTATIONS[key][1]; h = (end - start) / 60
            total = sum(w * avg[c] for c, w in weights.items()); chosen = {}
            for cat, w in weights.items():
                share = h * w * avg[cat] / total
                options = [(cat, i) for i in range(PACKS[cat])]
                fresh = [p for p in options if p not in used_today] or options
                pick = min(fresh, key=lambda p: (air[p] / max(hours[p], 0.01), p[1]))
                air[pick] += share; used_today.add(pick); chosen[cat] = pick[1]
            tracks = sum(len(packs[(c, i)]) for c, i in chosen.items())
            slots.append({"day": d, "start": start, "end": end, "key": key, "packs": chosen, "tracks": tracks,
                          "shares": {cat: h * w * avg[cat] / total for cat, w in weights.items()}})
    improve(slots, air, hours, packs)
    for s_ in slots: s_["tracks"] = sum(len(packs[(c, i)]) for c, i in s_["packs"].items())
    return slots, air, hours

def cost(slots, air, hours):
    """Spread of weekly turnover inside each category + same-day reuse of a pack."""
    total = 0.0
    for cat, k in PACKS.items():
        ratios = [air[(cat, i)] / hours[(cat, i)] for i in range(k)]
        mean = sum(ratios) / k; total += sum((r - mean) ** 2 for r in ratios)
    seen = collections.Counter((s["day"], c, i) for s in slots for c, i in s["packs"].items())
    return total + 0.3 * sum(n - 1 for n in seen.values() if n > 1)

def improve(slots, air, hours, packs):
    best = cost(slots, air, hours)
    for _ in range(40):
        changed = False
        for s in slots:
            for cat, cur in list(s["packs"].items()):
                for alt in range(PACKS[cat]):
                    if alt == cur: continue
                    share = s["shares"][cat]
                    air[(cat, cur)] -= share; air[(cat, alt)] += share; s["packs"][cat] = alt
                    tracks = sum(len(packs[(c, i)]) for c, i in s["packs"].items())
                    c = cost(slots, air, hours) if tracks * BYTES_PER_TRACK <= BUDGET else float("inf")
                    if c + 1e-9 < best: best, cur, changed = c, alt, True
                    else:
                        air[(cat, alt)] -= share; air[(cat, cur)] += share; s["packs"][cat] = cur
        if not changed: break

def plan():
    packs = split(); slots, air, hours = assign(packs)
    print("paket                           parça içerik  yayın/h dönüş")
    for p in sorted(packs, key=lambda p: (list(PACKS).index(p[0]), p[1])):
        print("%-32s %4d %5.1fs %6.1fs %5.2f" % (pack_name(*p), len(packs[p]), hours[p], air[p], air[p] / hours[p]))
    worst = max(slots, key=lambda s: s["tracks"])
    print("kuşak başına en çok parça:", worst["tracks"], "≈ %.2f MB" % (worst["tracks"] * BYTES_PER_TRACK / 1e6), DAYS[worst["day"]], sc.ROTATIONS[worst["key"]][0])
    assert all(s["tracks"] * BYTES_PER_TRACK <= BUDGET for s in slots), "budget"
    return packs, slots

def entry(r):
    m, v, a = r["media"], r["version"], r.get("analysis") or {}
    bpm = (a.get("rhythm") or {}).get("bpm") or 0
    return {"title": m["title"][:160], "artist": (m.get("artist") or "")[:120], "album": (m.get("album") or "")[:120],
            "genre": (m.get("genre") or "")[:120], "key": "", "location": "", "comment": "", "duration_ms": v["duration_ms"],
            "bpm": round(float(bpm), 3), "media_id": m["_id"], "version_id": m["version_id"],
            "media_file_id": m["media_file_id"], "media_sha256": v["sha256"]}

def playlists():
    from rapi import call, SID, key
    packs, slots = plan()
    existing = {p["name"]: p for p in call("desktop-playlists/list", station_id=SID)["playlists"]}
    mapping = {}
    for (cat, i), items in sorted(packs.items(), key=lambda kv: (list(PACKS).index(kv[0][0]), kv[0][1])):
        name = pack_name(cat, i)
        if name in existing:
            mapping["%s:%d" % (cat, i)] = existing[name]["_id"]; print("var:", name); continue
        made = call("desktop-playlists/create", station_id=SID, name=name, idempotency_key=key(), purpose="music",
                    entries=[entry(r) for r in items])
        mapping["%s:%d" % (cat, i)] = made["_id"]; print("oluşturuldu:", name, len(items))
    json.dump({"packs": mapping, "slots": [{k: v for k, v in s_.items() if k != "shares"} for s_ in slots]},
              open(D + "/pack_map.json", "w"), ensure_ascii=False, indent=1)

def cleanup():
    """Remove this session's first, too large category playlists (unused)."""
    from rapi import call, SID, key
    existing = {p["name"]: p for p in call("desktop-playlists/list", station_id=SID)["playlists"]}
    for cat, name in sc.CATS.items():
        if cat in ("MASHUP", "DJSET"): continue   # same names are reused as packs
        p_ = existing.get(name)
        if not p_: continue
        call("desktop-playlists/delete", station_id=SID, playlist_id=p_["_id"], expected_revision=p_["_rev"], idempotency_key=key())
        print("kaldırıldı:", name)

def slot_rotation(slot, mapping):
    name, weights, transitions = sc.ROTATIONS[slot["key"]]
    trs = []
    for mode, weight, overlap, effect in transitions:
        row = {"mode": mode, "weight": weight, "overlap_ms": overlap}
        if effect != "none": row["effect"] = effect
        trs.append(row)
    sources = [{"id": U(), "playlist_id": mapping["%s:%d" % (cat, slot["packs"][cat])], "frequency": 1, "count": 1, "weight": w,
                "start_minute": 0, "end_minute": 1440, "interval_minutes": 0, "random": False, "track_random": True,
                "advert_random": False, "sweeper_random": False, "media_kind": "all"} for cat, w in weights.items()]
    night = slot["key"] in ("GECE", "KAPANIS", "PSABAH")
    sweepers = json.load(open(D + "/sweeper_playlists.json"))
    sources.append({"id": U(), "playlist_id": sweepers["7/24 · Sweeper · " + ("Gece" if night else "Enerji")],
                    "frequency": 1, "count": 1, "weight": 12, "start_minute": 0, "end_minute": 1440,
                    "interval_minutes": 20 if night else 15, "random": False, "track_random": True, "advert_random": False,
                    "sweeper_random": True, "media_kind": "sweeper",
                    "settings": {"track_gap_minutes": 25, "artist_gap_minutes": 0, "album_gap_minutes": 0}})
    return {"id": U(), "name": "7/24 · %s · %s" % (DAYS[slot["day"]], name), "sources": sources,
            "settings": {"selection_mode": "weighted", "track_gap_minutes": 1440, "artist_gap_minutes": 60,
                         "album_gap_minutes": 0, "transitions": trs, "transition_selection_mode": "weighted",
                         "soft_end_seconds": 300,
                         "relax_gaps": True}}

def build_events(slots, rotations, first, last):
    out, day = [], first
    by_day = collections.defaultdict(list)
    for s_, rot in zip(slots, rotations): by_day[s_["day"]].append((s_, rot))
    while day <= last:
        for s_, rot in by_day[day.weekday()]:
            out.append({"id": U(), "title": sc.ROTATIONS[s_["key"]][0], "date": day.isoformat(), "start_minute": s_["start"],
                        "end_minute": s_["end"], "color": sc.COLORS[s_["key"]], "rotations": [{"id": U(), "rotation_id": rot["id"]}]})
        day += datetime.timedelta(days=1)
    return out

def compose():
    from rapi import docs
    data = json.load(open(D + "/pack_map.json")); mapping, slots = data["packs"], data["slots"]
    main = [r for r in docs("automation_calendar") if r["name"] == "main"][0]
    json.dump(main, open(D + "/main_before.json", "w"), ensure_ascii=False)
    cfg = json.loads(json.dumps(main["config"]))
    assert not any(r["name"].startswith("7/24 · ") for r in cfg.get("rotation_library", [])), "already composed"
    rots = [slot_rotation(s_, mapping) for s_ in slots]
    cfg.setdefault("rotation_library", []).extend(rots)
    cfg["events"].extend(build_events(slots, rots, MAIN_FROM, MAIN_TO))
    json.dump({"document_id": main["_id"], "expected_revision": main["_rev"], "config": cfg}, open(D + "/main_new.json", "w"), ensure_ascii=False)
    wrots = [slot_rotation(s_, mapping) for s_ in slots]
    weekly = {"schema": 1, "week_anchor": WEEK_ANCHOR.isoformat(), "week_count": 1, "repeat": True,
              "calendar": {"schema": 1, "timezone": cfg.get("timezone", "UTC"),
                           "events": build_events(slots, wrots, WEEK_ANCHOR, WEEK_ANCHOR + datetime.timedelta(days=6)),
                           "rotation_library": wrots}}
    json.dump(weekly, open(D + "/weekly.json", "w"), ensure_ascii=False)
    print("ana takvim rev", main["_rev"], "→ olay", len(cfg["events"]), "rotasyon", len(cfg["rotation_library"]))
    print("haftalık program: olay", len(weekly["calendar"]["events"]), "rotasyon", len(wrots))

def write():
    from rapi import call, docs, SID, key
    main = json.load(open(D + "/main_new.json"))
    saved = call("desktop/put", station_id=SID, kind="automation_calendar", name="main", config=main["config"],
                 document_id=main["document_id"], expected_revision=main["expected_revision"], idempotency_key=key())
    print("ana takvim kaydedildi:", json.dumps(saved, ensure_ascii=False)[:200])
    if [r for r in docs("weekly_programme") if r["name"] == PROGRAMME_NAME]: print("haftalık program zaten var"); return
    weekly = json.load(open(D + "/weekly.json"))
    made = call("desktop/put", station_id=SID, kind="weekly_programme", name=PROGRAMME_NAME, config=weekly, idempotency_key=key())
    print("haftalık program kaydedildi:", json.dumps(made, ensure_ascii=False)[:200])

if __name__ == "__main__":
    {"plan": plan, "playlists": playlists, "cleanup": cleanup, "compose": compose, "write": write}[sys.argv[2]]()
