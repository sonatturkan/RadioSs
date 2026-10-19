import json, os, subprocess, sys, time
D = sys.argv[1]; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); sys.path.insert(0, D)
from rapi import call, SID, key
ROOT = "/home/sturkan/Masaüstü/radyo projesi/kaynak kodlar/RadioS"
FOLDER = "/home/sturkan/Masaüstü/Yeni Klasör 3"
known = {r["version"]["sha256"] for r in json.load(open(D + "/media.json"))}
results = []
for name in sorted(os.listdir(FOLDER)):
    if not name.lower().endswith(".mp3"): continue
    p = subprocess.run([D + "/analiz", ROOT, ROOT + "/config/radios.example.ini", os.path.join(FOLDER, name)], capture_output=True, text=True)
    if p.returncode: print("analiz hatası", name, p.stderr[:300]); continue
    h = json.loads(p.stdout); snap = os.path.join(ROOT, "data", "imports", h["import_path"])
    if h["sha256"] in known:
        print("zaten kütüphanede:", name); os.path.exists(snap) and os.unlink(snap); continue
    meta = {"title": (h["title"] or os.path.splitext(name)[0])[:200], "kind": "sweeper", "artist": h["artist"][:200],
            "album": h["album"][:200], "genre": h["genre"][:120], "year": "", "comment": "", "tags": ["sweeper"]}
    body = dict(station_id=SID, import_path=h["import_path"], source_name=h["source_name"], idempotency_key=key(),
                metadata=meta, analysis=h["analysis"])
    for attempt in range(8):
        try:
            res = call("desktop-media/import", **body); break
        except RuntimeError as e:
            if "409" in str(e) and "changed since preview" in str(e): time.sleep(0.5 * (attempt + 1)); continue
            raise
    os.path.exists(snap) and os.unlink(snap)
    results.append({"file": name, "title": meta["title"], "result": res}); print("aktarıldı:", meta["title"], json.dumps(res)[:160])
json.dump(results, open(D + "/sweepers.json", "w"), ensure_ascii=False, indent=1)
