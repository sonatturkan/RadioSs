"""Read-only, bounded native local playback input preparation.

This contract resolves saved catalog identities to freshly verified managed
bytes. It grants no content approval, formal plan publication, endpoint
acceptance or on-air evidence. Native C owns selection, scheduling and audio.
"""

from copy import deepcopy
import json
import re

from . import desktop_api as api, desktop_media, desktop_playlist, desktop_copyright, desktop_probability_policy
from .catalog import NotFound
from .storage import Conflict, StoreError

CONTRACT_VERSION = 1
# One preparation carries the complete queue with per-track media, analysis
# and marker detail (about 3.7 KB per track). Station playlists built from the
# whole archive run past 2000 rows and 1.5 MB, so the budget follows the native
# client's playout response cap (data_client.c PLAYOUT_LIMIT).
MAX_CANDIDATES = 10000
MAX_RESPONSE_BYTES = 40_000_000
_DECK = re.compile(r"(?:[1-9]|[12][0-9]|3[0-2])\Z")
# Native per-player queues (including AutoDJ scoped checkpoints) keep their own
# checkpoints under these names. They are not the station history that automation
# preparation resolves.
_PLAYER_HISTORY = re.compile(r"Oynatıcı (?:[1-9]|[12][0-9]|3[0-2]) (?:oynatma|AutoDJ) geçmişi\Z")


def _deck_ids(config):
    for key in ("decks", "jingleDeck"):
        values = config["decks"] if key == "decks" else [config["jingleDeck"]]
        for value in values:
            if value is not None and (not isinstance(value, str) or not _DECK.fullmatch(value)):
                raise StoreError(f"{key} requires canonical native deck identities 1..32")


def _document(store, station_id, kind, name):
    rows = [row for row in store.lookup("desktop_configurations", "station_kind", station_id, kind)
            if row["status"] == "active" and row["name"] == name]
    if len(rows) > 1:
        raise Conflict(f"multiple active {kind} documents require an explicit configuration")
    return rows[0] if rows else None


def _selected(rows, entry_ids):
    if entry_ids is None:
        return rows
    api._list(entry_ids, "entry_ids", 500)
    ids = [api._id(value, "entry_id") for value in entry_ids]
    if len(set(ids)) != len(ids):
        raise StoreError("entry_ids must be distinct")
    known = {row.get("_id", row.get("id")) for row in rows}
    if set(ids) - known:
        raise Conflict("selected entries are absent or changed; reload before preparing")
    selected = set(ids)
    return [row for row in rows if row.get("_id", row.get("id")) in selected]


def _approved_voice_cues(store, station_id, detail):
    """Small, read-only vocal windows from exact approved version/media cues.

    Earliest intro/vocal boundary and latest outro boundary are conservative
    when multiple approved cues exist. Desktop fade marks do not certify an
    instrumental outro and must never substitute for these approved cues.
    """
    from .radio_cues import approved_cues_for_version
    version = detail["version"]
    candidates = approved_cues_for_version(store, station_id, version["_id"], version["media_file_id"])
    selected = {}
    for cue in candidates:
        kind = cue["cue_kind"]
        position = cue["position_ms"]
        if kind not in {"intro_end", "vocal_start", "outro_start"} or \
                cue.get("media_duration_ms") != version["duration_ms"] or \
                type(position) is not int or not 0 <= position <= version["duration_ms"]:
            continue
        previous = selected.get(kind)
        if previous is None or (position > previous["position_ms"] if kind == "outro_start" else position < previous["position_ms"]):
            selected[kind] = cue
    fields = ("_id", "_rev", "station_id", "version_id", "media_file_id", "media_sha256",
              "media_duration_ms", "status", "cue_kind", "position_ms")
    return [{key: deepcopy(cue[key]) for key in fields} for _, cue in sorted(selected.items())]


class _Preparation:
    def __init__(self, store, station_id, copyright_policy=None):
        self.store, self.station_id = store, station_id
        self.count = 0
        self.details = {}
        self.copyright_policy = copyright_policy or deepcopy(desktop_copyright.DEFAULT_POLICY)
        self.checked_at = desktop_copyright.utc_now()

    def copyright(self, result):
        # Keep byte/integrity eligibility separately: a native queue may remain
        # loaded when a policy is disabled or a future validity window opens.
        result["media_eligible"] = result["eligible"]
        decision = desktop_copyright.evaluate(self.copyright_policy, result["metadata"], at=self.checked_at)
        result["copyright"] = decision
        result["reasons"].extend(decision["reasons"])
        result["eligible"] = result["media_eligible"] and decision["allowed"]
        return result

    def track(self, row, ordinal):
        self.count += 1
        if self.count > MAX_CANDIDATES:
            raise StoreError(f"playback preparation exceeds {MAX_CANDIDATES} candidate rows")
        metadata = {name: deepcopy(row.get(name, "")) for name in
                    ("title", "artist", "album", "genre", "key", "location", "comment")}
        metadata.update(duration_ms=row.get("duration_ms"), bpm=row.get("bpm", 0),
                        kind=row.get("kind", "music"), note=row.get("note", ""),
                        start_time=row.get("start_time", ""),
                        start_policy=row.get("start_policy", "sequential"),
                        max_wait_ms=row.get("max_wait_ms", 0))
        result = {"entry_id": row.get("_id", row.get("id")), "ordinal": ordinal,
                  "eligible": False, "reasons": [], "metadata": metadata, "detail": None}
        if "probability_policy" in row:
            desktop_probability_policy.validate_probability_policy(row["probability_policy"], self.station_id, self.store._state, persisted=True)
            result["probability_policy"] = deepcopy(row["probability_policy"])
        if "policy_context" in row:
            desktop_probability_policy.validate_policy_context(row["policy_context"], row, self.station_id, self.store._state, persisted=True)
            result["policy_context"] = deepcopy(row["policy_context"])
        binding = {key: row[key] for key in api.MEDIA_BINDING_KEYS if key in row}
        if desktop_playlist.ONLINE_REFERENCE_KEYS.intersection(row) or "analysis_media" in row:
            # Provider references alone do not authorize audio. An imported
            # analysis binding must pass the same exact local-media checks.
            fields = (desktop_playlist.METADATA | api.MEDIA_BINDING_KEYS |
                      desktop_playlist.ONLINE_SOURCE_KEYS | desktop_playlist.ANALYSIS_SOURCE_KEYS)
            source = desktop_playlist._metadata({name: row[name] for name in fields if name in row}, persisted=True)
            desktop_playlist._online_binding(source, self.station_id, self.store._state, persisted=True)
            metadata.update({name: deepcopy(source[name]) for name in desktop_playlist.ONLINE_SOURCE_KEYS if name in source})
            binding = source.get("analysis_media")
        if not binding:
            result["reasons"] = ["media_unbound"]
            return self.copyright(result)
        # Identity mismatches and tenant crossing are invalid input, rather
        # than an unavailable item that a fallback policy could ignore.
        api._media_binding(binding, self.station_id, self.store._state, persisted=True)
        key = tuple(binding[name] for name in sorted(api.MEDIA_BINDING_KEYS))
        if key not in self.details:
            library = self.store.get("desktop_media_library", api._id(binding["media_id"]))
            if library["status"] != "active":
                self.details[key] = (None, ["media_inactive"])
            else:
                detail = desktop_media._snapshot(self.store, *desktop_media._library(
                    self.store, self.station_id, binding["media_id"]))
                if detail.get("analysis"):
                    detail["analysis"].pop("waveform", None)
                # Read approved cues only after the exact managed media has
                # retained its verification; unavailable rows stay unavailable.
                if detail["version"]["verified"]:
                    cues = _approved_voice_cues(self.store, self.station_id, detail)
                    if cues:
                        detail["radio_cues"] = cues
                reasons = []
                version = detail["version"]
                if not version["verified"]:
                    reasons.append("media_unverified")
                elif not version["object_available"]:
                    reasons.append("media_object_unavailable")
                analysis = detail.get("analysis")
                if not analysis or analysis.get("sha256") != binding["media_sha256"] or \
                        type(version.get("duration_ms")) is not int or version["duration_ms"] <= 0:
                    reasons.append("media_analysis_unavailable")
                self.details[key] = (detail, reasons)
        detail, reasons = self.details[key]
        result["detail"], result["reasons"] = deepcopy(detail), list(reasons)
        if detail:
            actual = detail["media"]
            metadata.update(title=actual["title"], artist=actual["artist"], album=actual["album"],
                            kind=actual["kind"], genre=actual["genre"], artist_id=actual.get("artist_id"),
                            duration_ms=detail["version"]["duration_ms"])
        result["eligible"] = not reasons
        return self.copyright(result)

    def playlist(self, playlist_id, source_id=None, role="music"):
        identifier = api._id(playlist_id, "playlist_id")
        playlist = self.store.get("playlists", identifier)
        if not playlist or playlist["station_id"] != self.station_id:
            raise NotFound("playlist is absent or outside station")
        source = {"source_id": source_id, "source_name": playlist["name"], "role": role,
                  "playlist_id": playlist["_id"], "playlist_revision": playlist["_rev"],
                  "playlist_version_id": playlist.get("draft_version_id"), "tracks": [], "reasons": []}
        if playlist["status"] != "active":
            source["reasons"] = ["playlist_disabled"]
            return source
        version = self.store.get("playlist_versions", playlist["draft_version_id"]) if playlist.get("draft_version_id") else None
        if not version or version["playlist_id"] != identifier or version["status"] != "draft":
            source["reasons"] = ["playlist_draft_unavailable"]
            return source
        source["tracks"] = desktop_playlist._tracks(self.store, version["_id"])
        return source

    def programme(self, config, entry_ids=None, document=None):
        config = api.validate_configuration("programme", config, self.station_id, self.store._state, persisted=True)
        rows = _selected(config["events"], entry_ids)
        ordinals = {row["id"]: ordinal for ordinal, row in enumerate(config["events"])}
        return {"document_id": document["_id"] if document else None,
                "revision": document["_rev"] if document else None,
                "config": config,
                "tracks": [self.track(row, ordinals[row["id"]]) for row in rows]}


@api._consistent_read
def prepare(store, principal, station_id, kind, playlist_id=None, config=None, entry_ids=None):
    """Resolve one immutable GUI input at one sequence, without database writes."""
    station_id, station = api._station(store, principal, station_id)
    sequence = store.sequence
    if not isinstance(kind, str) or kind not in {"playlist", "programme", "automation"}:
        raise StoreError("prepare kind must be playlist, programme or automation")
    if kind == "playlist" and (playlist_id is None or config is not None) or \
            kind != "playlist" and playlist_id is not None or kind == "automation" and entry_ids is not None:
        raise StoreError("prepare arguments do not match the requested kind")
    copyright_policy, copyright_document = desktop_copyright.policy_snapshot(store, station_id)
    preparation = _Preparation(store, station_id, copyright_policy)
    result = {"contract_version": CONTRACT_VERSION, "kind": kind, "station_id": station_id,
              "station_timezone": station["timezone"], "timezone": "UTC", "sequence": sequence,
              "playback_scope": "local_draft", "config": None, "tracks": [], "sources": [],
              "programme": None, "history": None, "diagnostics": []}
    result["copyright_policy"] = copyright_policy
    result["copyright_policy_document_id"] = copyright_document["_id"] if copyright_document else None
    result["copyright_policy_revision"] = copyright_document["_rev"] if copyright_document else 0
    policy = _document(store, station_id, "playout_policy", "main")
    if policy:
        result["playout_policy"] = api.validate_configuration(
            "playout_policy", policy["config"], station_id, store._state, persisted=True)
    if kind == "playlist":
        source = preparation.playlist(playlist_id)
        rows = _selected(source["tracks"], entry_ids)
        result["tracks"] = [preparation.track(row, row["ordinal"]) for row in rows]
        result.update(playlist_id=source["playlist_id"], playlist_revision=source["playlist_revision"],
                      playlist_version_id=source["playlist_version_id"], source_name=source["source_name"])
        result["diagnostics"] = [{"code": reason} for reason in source["reasons"]]
    else:
        document = None
        if config is None:
            document = _document(store, station_id, kind, "Mevcut program" if kind == "programme" else "main")
            if document is None:
                raise NotFound("native playback configuration is absent")
            config = document["config"]
        if kind == "programme":
            programme = preparation.programme(config, entry_ids, document)
            result.update(config=programme["config"], tracks=programme["tracks"], programme=programme)
        else:
            config = api.validate_configuration("automation", config, station_id, store._state, persisted=True)
            _deck_ids(config)
            result["config"] = config
            for source in config["sources"]:
                result["sources"].append(preparation.playlist(source["playlist_id"], source["source_id"]))
            for field, role in (("jinglePlaylist", "jingle"), ("fallbackPlaylist", "fallback")):
                if config[field] is not None:
                    result["sources"].append(preparation.playlist(config[field], role=role))
            for source in result["sources"]:
                source["tracks"] = [preparation.track(row, row["ordinal"]) for row in source["tracks"]]
            if config["includeProgramme"]:
                programme = _document(store, station_id, "programme", "Mevcut program")
                if programme:
                    result["programme"] = preparation.programme(programme["config"], document=programme)
                else:
                    result["diagnostics"].append({"code": "programme_missing"})
            histories = [row for row in store.lookup("desktop_configurations", "station_kind", station_id, "playout_history")
                         if row["status"] == "active" and not _PLAYER_HISTORY.fullmatch(row["name"])]
            if len(histories) > 1:
                raise Conflict("multiple local playback histories require resolution")
            if histories:
                history = histories[0]
                clean = api.validate_configuration("playout_history", history["config"], station_id, store._state, persisted=True)
                result["history"] = {"document_id": history["_id"], "revision": history["_rev"], "config": clean}
    # Authenticated auxiliary pools remain separate from primary song order.
    jingle_ids = set()
    if kind == "automation":
        policies = list(result["config"].get("probability_source_policies", []))
        if "probability_policy" in result["config"]:
            policies.append(result["config"]["probability_policy"])
        for policy in policies:
            jingle_ids.update(desktop_probability_policy.jingle_playlist_ids(policy))
    for row in result["tracks"]:
        if "probability_policy" in row:
            jingle_ids.update(desktop_probability_policy.jingle_playlist_ids(row["probability_policy"]))
    if jingle_ids:
        result["jingle_sources"] = []
        for identifier in sorted(jingle_ids):
            source = preparation.playlist(identifier, role="jingle")
            result["jingle_sources"].append({"playlist_id": identifier,
                "tracks": [preparation.track(row, row["ordinal"]) for row in source["tracks"]]})
    if store.sequence != sequence:
        raise Conflict("database changed during playback preparation")
    if len(json.dumps(result, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode()) > MAX_RESPONSE_BYTES:
        raise StoreError(f"playback preparation exceeds the {MAX_RESPONSE_BYTES}-byte response budget")
    return result
