"""Dry-run the app's schema path for sample times (no DB writes, no audio)."""
import json, os, subprocess, sys, collections, datetime
D = sys.argv[1]; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); sys.path.insert(0, D)
from rapi import call, SID
import classify
E2E = D + "/e2e"; ROOT = "/home/sturkan/Masaüstü/radyo projesi/kaynak kodlar/RadioS"
DBFILE = ROOT + "/data/database/RadioS.sdb"
WORK = D + "/verify"; os.makedirs(WORK, exist_ok=True)
recs = json.load(open(D + "/media.json"))
CAT = {r["media"]["_id"]: classify.chill_override(r, classify.classify(r)) for r in recs}
ARTIST = {r["media"]["_id"]: (r["media"].get("artist") or "").strip().lower() for r in recs}
TITLE = {r["media"]["_id"]: r["media"].get("title") or "" for r in recs}

def us(dt): return int(dt.replace(tzinfo=datetime.timezone.utc).timestamp() * 1_000_000)
def sh(*args):
    r = subprocess.run([E2E, *map(str, args)], capture_output=True, text=True, timeout=300)
    return r.returncode, (r.stdout.strip() + (" " + r.stderr.strip() if r.stderr.strip() else "")).strip()

def run(name, wp_path, now):
    tag = os.path.join(WORK, "case")
    code, out = sh("occ", wp_path, us(now), "0,1", tag)
    if code: return name, False, "occ: " + out
    start = int(out.split("start=")[1].split()[0]); end = int(out.split("end=")[1].split()[0])
    k = out.split("key=")[1].split()[0] if "key=" in out else "k"
    cfg = json.load(open(tag + "-config.json"))
    try: prepared = call("desktop-playout/prepare", station_id=SID, kind="automation", config=cfg)
    except Exception as e: return name, False, "prepare-automation: " + str(e)[:400]
    prepared["history"] = {"config": {"kind": "playout_history", "entries": []}}
    json.dump(prepared, open(tag + "-prepared.json", "w"), ensure_ascii=False)
    at = max(us(now) + 5_000_000, start)
    code, out = sh("plan", tag + "-event.json", tag + "-prepared.json", SID, at, 0, k + ":0", tag + "-sel.json")
    if code: return name, False, "plan: " + out
    sel = json.load(open(tag + "-sel.json")); diags = collections.Counter(d.get("reason") for d in sel.get("diagnostics", []))
    try: result = call("desktop-playout/prepare", station_id=SID, kind="programme", config=sel["config"])
    except Exception as e: return name, False, "prepare-programme: " + str(e)[:400]
    json.dump(result, open(tag + "-result.json", "w"), ensure_ascii=False)
    code, out = sh("merge", tag + "-result.json", tag + "-sel.json", tag + "-merged.json")
    if code: return name, False, "merge: " + out
    code, out = sh("native", tag + "-merged.json", max(us(now), start) + 2_000_000, 0, 1, "0,1", DBFILE, tag + "-native.json")
    if code: return name, False, "native: " + out
    nat = json.load(open(tag + "-native.json")); ents = nat["entries"]
    modes = collections.Counter(("EOF", "XF", "CUE", "MAN")[e["mode"]] + ("+BS" if e["beat_sync"] else "") + ("+LP" if e["effect"] else "") for e in ents)
    ids = [e.get("media_id") for e in ents]
    cats = collections.Counter(e.get("kind", "?") for e in ents)
    sw = [e["planned_utc_us"] for e in ents if e.get("kind") == "sweeper"]
    gaps = [(b - a) / 6e7 for a, b in zip(sw, sw[1:])]
    adjacent = sum(1 for a, b in zip(ents, ents[1:]) if a.get("kind") == "sweeper" and b.get("kind") == "sweeper")
    cats["sweeper_min_aralık_dk"] = round(min(gaps), 1) if gaps else -1
    cats["sweeper_art_arda"] = adjacent
    dup = len(ids) - len(set(ids))
    # artist separation inside the plan (minutes between same artist)
    last_seen, close = {}, 0
    for e in ents:
        a = ARTIST.get(e.get("media_id"), ""); t = e["planned_utc_us"]
        if a and a in last_seen and t - last_seen[a] < 60 * 60 * 1_000_000: close += 1
        if a: last_seen[a] = t
    tail = ""
    if ents:
        last = ents[-1]; end_us = last["planned_utc_us"] + int(last["cue_s"] * 1e6)
        first_gap = (ents[0]["planned_utc_us"] - max(us(now), start)) / 1e6
        tail = " ilk_başlangıç+%.0fs son_bitiş-kuşak_sonu=%+.0fs" % (first_gap, (end_us - end) / 1e6)
    span = (end - max(us(now), start)) / 3.6e9
    return name, True, "%d öğe/%.1fs %s geçiş=%s tekrar=%d aynı_sanatçı<60dk=%d tanı=%s%s" % (
        len(ents), span, dict(cats.most_common()), dict(modes), dup, close, dict(diags), tail)

if __name__ == "__main__":
    wp = sys.argv[2]; cases = json.loads(sys.argv[3])
    ok = 0
    for name, iso in cases:
        n, good, msg = run(name, wp, datetime.datetime.fromisoformat(iso))
        ok += good; print(("✓ " if good else "✗ ") + n + ": " + msg, flush=True)
    print("SONUÇ %d/%d" % (ok, len(cases)))
