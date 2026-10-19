"""Senaryo kurucuları: playlistler, rotasyonlar, bloklar, haftalık program."""
import copy, uuid
from api import call, SID

U = lambda: str(uuid.uuid4())
_media = None


def media(refresh=False):
    global _media
    if _media is None or refresh:
        _media = call("desktop-media/list", station_id=SID, limit=200)["records"]
    return _media


def entry_row(r):
    m, v, a = r["media"], r["version"], r.get("analysis") or {}
    bpm = (a.get("rhythm") or {}).get("bpm") or 0
    return {"title": m["title"], "artist": m["artist"], "album": m["album"], "genre": m.get("genre", ""), "key": "",
            "location": "", "comment": "", "duration_ms": v["duration_ms"], "bpm": round(float(bpm), 3),
            "media_id": m["_id"], "version_id": m["version_id"], "media_file_id": m["media_file_id"], "media_sha256": v["sha256"]}


def playlists():
    return {p["name"]: p for p in call("desktop-playlists/list", station_id=SID)["playlists"]}


def ensure_playlist(name, rows, purpose="music"):
    pls = playlists()
    if name in pls:
        return pls[name]["_id"]
    body = dict(station_id=SID, name=name, idempotency_key=U(), purpose=purpose)
    if rows:  # boş playlist: entries alanı hiç gönderilmez
        body["entries"] = rows
    return call("desktop-playlists/create", **body)["_id"]


ALL12 = {"music": [1, 2], "advertisement": [1, 2], "jingle": [1, 2], "sweeper": [1, 2]}


def src(playlist, weight=100, count=1, order="shuffle", media_kind="all", interval=0, freq=1, start=0, end=1440,
        sweeper_random=False, advert_random=False, rules=None, autodj=None, settings=None):
    row = {"id": U(), "playlist_id": playlist, "frequency": freq, "weight": weight, "count": count,
           "interval_minutes": interval, "start_minute": start, "end_minute": end, "media_kind": media_kind,
           "random": False, "track_random": order != "linear", "advert_random": advert_random,
           "sweeper_random": sweeper_random, "autodj": copy.deepcopy(autodj or ALL12)}
    if order in ("linear", "weighted"):
        row["track_selection_mode"] = order
    if rules:
        row["track_rules"] = rules
    if settings:
        row["settings"] = settings
    return row


def tr(eof=100, cf=0, cue=0, beat=0, cf_ms=4000, cue_ms=3000, beat_ms=8000, lowpass=False):
    rows = [{"mode": 0, "weight": eof, "overlap_ms": 0}, {"mode": 1, "weight": cf, "overlap_ms": cf_ms},
            {"mode": 2, "weight": cue, "overlap_ms": cue_ms}, {"mode": 3, "weight": beat, "overlap_ms": beat_ms}]
    if lowpass:
        rows[1]["effect"] = "lowpass"
    return rows


def settings(selection="weighted", transitions=None, tsel="weighted", gaps=(0, 0, 0), policy=None, kind=None, players=None, extra=None):
    s = {"selection_mode": selection, "track_gap_minutes": gaps[0], "artist_gap_minutes": gaps[1],
         "album_gap_minutes": gaps[2], "transitions": transitions or tr(), "transition_selection_mode": tsel,
         "autodj_players": copy.deepcopy(players or ALL12)}
    if policy:
        s["probability_policy"] = policy
    if kind:
        s["kind_transitions"] = kind
    if extra:
        s.update(extra)
    return s


def rotation(name, sources, **kw):
    return {"id": U(), "name": name, "settings": settings(**kw), "sources": sources}


def event(date, start, end, rots, title="TEST blok", mode="weighted", weights=None, policy=None):
    ev = {"id": U(), "title": title, "date": date, "start_minute": start, "end_minute": end, "color": "#888888",
          "rotation_selection_mode": mode,
          "rotations": [{"id": U(), "rotation_id": r["id"], "weight": (weights[i] if weights else 100)} for i, r in enumerate(rots)]}
    if policy:
        ev["probability_policy"] = policy
    return ev


def weekly(events, library, anchor="2026-10-12", weeks=1, repeat=True, tz=None):
    cal = {"schema": 1, "events": events, "rotation_library": library}
    if tz:
        cal["timezone"] = tz
    return {"schema": 1, "week_anchor": anchor, "week_count": weeks, "repeat": repeat, "calendar": cal}


def overlay(playlist, anchor="after_start", seconds=8, prob=100, duck=-10, fade=300, interval=0, player=2, gain=0):
    return {"id": U(), "playlist_id": playlist, "player": player, "anchor": anchor, "seconds": seconds,
            "duck_db": duck, "gain_db": gain, "fade_ms": fade, "probability": prob, "interval_minutes": interval}
