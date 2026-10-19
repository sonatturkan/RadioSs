"""Uçtan uca senaryo çalıştırıcı: uygulamanın şema yolunun aynısı (yazma yok, ses yok)."""
import json, os, subprocess
from datetime import datetime
from api import call, SID, ROOT

# RADIOS_BUILD_DIR=build-xx: e2e ikilisi ve sonuçlar o yapı dizininin altında (varsayılan build/).
H = os.path.join(ROOT, os.environ.get("RADIOS_BUILD_DIR", "build"), "otomasyon_testleri")
E2E = os.path.join(H, "e2e")
DBFILE = os.path.join(ROOT, "data", "database", "RadioS.sdb")


def us(dt):
    return int(dt.timestamp() * 1_000_000)


def sh(*args):
    r = subprocess.run([E2E, *map(str, args)], capture_output=True, text=True)
    return r.returncode, (r.stdout.strip() + (" " + r.stderr.strip() if r.stderr.strip() else "")).strip()


def backend(kind, config):
    try:
        return call("desktop-playout/prepare", station_id=SID, kind=kind, config=config), None
    except SystemExit as e:
        return None, str(e)


def run_case(name, weekly, now, decks="0,1", semi=False, trim=True, delay_s=2.0, history=None, seed=None):
    """Uygulamadaki sırayla: occ → backend(automation) → takvim planı → backend(programme)
    → birleştirme → yerel plan. delay_s, hazırlık gidiş-dönüşünü taklit eder. trim=True
    uygulamanın şema yoluyla aynıdır (playout_ui.c). seed=None uygulamanın kullandığı
    "<kuşak anahtarı>:0" tohumudur; farklı bir tam sayı aynı kuşağı başka tohumla planlar."""
    os.makedirs(H, exist_ok=True)
    tag = os.path.join(H, "case")
    json.dump(weekly, open(tag + "-wp.json", "w"), ensure_ascii=False)
    res = {"name": name}
    code, out = sh("occ", tag + "-wp.json", us(now), decks, tag)
    res["occ"] = out
    if code:
        res.update(step="occ", ok=False, message=out); return res
    start = int(out.split("start=")[1].split()[0]); end = int(out.split("end=")[1].split()[0])
    key = out.split("key=")[1].split()[0] if "key=" in out else "k"
    cfg = json.load(open(tag + "-config.json"))
    prepared, err = backend("automation", cfg)
    if err:
        res.update(step="prepare-automation", ok=False, message=err); return res
    # Gerçek çalma geçmişi her gün büyür; sabit (geçmiş) test tarihleri onunla çakışıp "geleceğe ait"
    # kayıtlar yüzünden tekrar süresini ihlal ederdi. Varsayılan: boş geçmiş (yalıtılmış, tekrarlanabilir).
    prepared["history"] = {"config": history if history is not None else {"kind": "playout_history", "entries": []}}
    json.dump(prepared, open(tag + "-prepared.json", "w"), ensure_ascii=False)
    at = max(us(now) + 5_000_000, start)  # uygulama: seçim, gerçek başlangıçtan 5 sn ileri (player_scope_execution.c)
    if at >= end:  # uygulama: kuşağın son saniyelerinde bitişte yeniden dener (iş durmaz)
        res.update(step="retry", ok=False, message="kuşak bitişinde yeniden dene (beklenen)"); return res
    code, out = sh("plan", tag + "-event.json", tag + "-prepared.json", SID, at, int(semi), f"{key}:{0 if seed is None else int(seed)}", tag + "-sel.json")
    if code:
        res.update(step="calendar-plan", ok=False, message=out); return res
    sel = json.load(open(tag + "-sel.json"))
    res["calendar_diagnostics"] = [d.get("reason") for d in sel.get("diagnostics", [])]
    res["calendar_events"] = len(sel["config"]["events"])
    result, err = backend("programme", sel["config"])
    if err:
        res.update(step="prepare-programme", ok=False, message=err); return res
    json.dump(result, open(tag + "-result.json", "w"), ensure_ascii=False)
    code, out = sh("merge", tag + "-result.json", tag + "-sel.json", tag + "-merged.json")
    if code:
        res.update(step="merge", ok=False, message=out); return res
    native_now = max(us(now), start) + int(delay_s * 1_000_000)
    code, out = sh("native", tag + "-merged.json", native_now, int(semi), int(trim), decks, DBFILE, tag + "-native.json")
    if code:
        res.update(step="native", ok=False, message=out); return res
    nat = json.load(open(tag + "-native.json"))
    res.update(step="done", ok=True, message=out, entries=nat["entries"], pool=nat["pool_count"],
               display=nat.get("display"), start=start, end=end, native_now=native_now)
    return res


def summary(res):
    if not res["ok"]:
        return f"✗ {res['name']}: [{res['step']}] {res['message'][:300]}"
    ents = res["entries"]
    modes, kinds = {}, {}
    for e in ents:
        k = ("EOF", "XF", "CUE", "MAN")[e["mode"]] + ("+BS" if e["beat_sync"] else "") + ("+LP" if e["effect"] else "")
        modes[k] = modes.get(k, 0) + 1
        kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
    adj = sum(e["adjunct"] for e in ents)
    tail = ""
    if ents:
        last = ents[-1]
        end_us = last["planned_utc_us"] + int(last["cue_s"] * 1e6)
        tail = f" son_bitiş-kuşak_sonu={(end_us - res['end']) / 1e6:+.1f}s"
    return f"✓ {res['name']}: {len(ents)} öğe {kinds} geçiş={modes} katman={adj} tanı={res['calendar_diagnostics']}{tail}"


def run_all(cases, save_prefix="TEST · ", put=None, results_file=None, only=None):
    """Her senaryoyu (isteğe bağlı) TEST şeması olarak backend'e kaydedip uçtan uca çalıştırır."""
    import traceback
    results = []
    for name, wp, now, run in cases:
        if only and not any(name.startswith(o) for o in only):
            continue
        saved = "-"
        if put:
            try:
                put("weekly_programme", save_prefix + name[:140], wp); saved = "kaydedildi"
            except SystemExit as e:
                saved = "backend reddetti: " + str(e).split(": ", 2)[-1][:200]
        try:
            res = run_case(name, wp, now, **run)
        except Exception:
            res = {"name": name, "ok": False, "step": "python", "message": traceback.format_exc()[-400:]}
        res["backend"] = saved
        results.append(res)
        print(summary(res)[:230], "| DB:", saved, flush=True)
    if results_file:
        json.dump(results, open(os.path.join(H, results_file), "w"), ensure_ascii=False, indent=1, default=str)
    return results
