"""Native MAIN output profiles and separately classified local runtime receipts.

Stream profiles carry an encoder password only when the operator explicitly
asked for it to be saved (``password_saved`` true with a ``password`` string);
otherwise they never contain one. Execution receipts never contain encoder
passwords. Receipts record C engine observations, not listener reception,
canonical endpoint delivery, or alert publication. Emergency preparation reopens verified catalog bytes on every
attempt; a free text path or a saved preparation form cannot authorize audio.
"""
from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timezone
import hashlib
import json
from urllib.parse import urlsplit
from uuid import UUID, uuid4, uuid5

from . import desktop_api as api, desktop_playout
from .catalog import NotFound
from .identity import Forbidden
from .storage import Conflict, StoreError

PROFILE_KINDS = {"stream": "broadcast_stream", "record": "broadcast_record", "metadata": "broadcast_metadata"}
TERMINAL = frozenset({"completed", "failed", "interrupted"})
MAX_EVENTS = 1000
MAX_BYTES = 1_800_000


def _now():
    return datetime.now(timezone.utc).isoformat()


def _body(row):
    return {key: deepcopy(value) for key, value in row.items() if key not in {"_id", "_rev"}}


def _hash(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
        separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def _bounded(value):
    if len(json.dumps(value, ensure_ascii=False, allow_nan=False).encode()) > MAX_BYTES:
        raise StoreError("broadcast snapshot exceeds byte budget")
    return value


def _revision(row, expected):
    if type(expected) is not int or expected < 1 or row["_rev"] != expected:
        raise Conflict("broadcast input changed; reload before applying")


def _document(store, sid, kind, identifier):
    row = store.get("desktop_configurations", api._id(identifier, "document_id"))
    if row is None or row["station_id"] != sid or row["kind"] != kind or row["status"] != "active":
        raise NotFound("broadcast document is absent or outside station")
    return row


def _receipt_profile(config):
    """Frozen profile copy for receipts and start responses: never a password."""
    profile = deepcopy(config)
    profile.pop("password", None)
    profile.pop("password_saved", None)
    return profile


def _put_run(tx, principal, sid, name, config, old=None):
    now = _now()
    body = {"station_id": sid, "kind": "broadcast_local_run", "name": name,
        "config": deepcopy(config), "status": "active",
        "created_by": old["created_by"] if old else principal.user_id,
        "created_at": old["created_at"] if old else now,
        "updated_by": principal.user_id, "updated_at": now}
    identifier = old["_id"] if old else str(uuid4())
    tx.put("desktop_configurations", identifier, body,
           expected_revision=old["_rev"] if old else None)
    return body | {"_id": identifier, "_rev": old["_rev"] + 1 if old else 1}


def validate_configuration(kind, config, sid, state, *, persisted=False):
    if kind == "broadcast_stream":
        keys = {"url", "username", "bitrate_kbps"}
        api._object(config, keys | {"password", "password_saved"}, keys)
        # A saved password is an explicit operator choice: the flag and the
        # value travel together, so an unflagged secret is still rejected.
        if "password_saved" in config:
            api._bool(config["password_saved"], "password_saved")
        if ("password" in config) != bool(config.get("password_saved", False)):
            raise StoreError("stream password is stored only together with password_saved=true")
        if "password" in config:
            password = api._text(config["password"], "password", 256)
            if len(password.encode()) > 256 or any(ord(char) < 32 or ord(char) == 127 for char in password):
                raise StoreError("stream password must be at most 256 UTF-8 bytes without control characters")
        api._url(config["url"], "url")
        parsed = urlsplit(config["url"])
        if parsed.query or parsed.fragment or not parsed.path or parsed.path == "/" or \
                "\\" in config["url"] or any(ord(char) < 32 or ord(char) == 127 for char in config["url"]):
            raise StoreError("stream URL requires a concrete HTTP(S) mount without query or fragment")
        api._text(config["username"], "username", 128, True)
        if len(config["username"].encode()) > 256 or ":" in config["username"] or any(ord(char) < 32 or ord(char) == 127 for char in config["username"]):
            raise StoreError("stream username cannot contain a colon or control characters")
        if type(config["bitrate_kbps"]) is not int or config["bitrate_kbps"] not in {64, 96, 128, 160, 192, 256, 320}:
            raise StoreError("stream bitrate must be one of 64, 96, 128, 160, 192, 256, 320 kbps")
    elif kind == "broadcast_record":
        api._object(config, {"filename"}, {"filename"})
        value = api._text(config["filename"], "filename", 200, True)
        if value != value.strip() or value in {".", ".."} or "/" in value or "\\" in value or \
                not value.endswith(".wav") or value.startswith(".") or \
                len(value.encode()) > 240 or \
                any(ord(char) < 32 or ord(char) == 127 for char in value):
            raise StoreError("recording filename must be one visible leaf ending in .wav")
    elif kind == "broadcast_metadata":
        api._object(config, {"title", "automatic"}, {"title", "automatic"})
        value = api._text(config["title"], "title", 1024)
        if len(value.encode()) > 1024 or any(ord(char) < 32 or ord(char) == 127 for char in value):
            raise StoreError("metadata title must be at most 1024 UTF-8 bytes without controls")
        api._bool(config["automatic"], "automatic")
    elif kind == "broadcast_local_run":
        keys = {"schema_version", "manifest", "manifest_hash", "events", "state", "proof_class", "delivery_proven"}
        api._object(config, keys, keys)
        if type(config["schema_version"]) is not int or config["schema_version"] != 1 or \
                config["proof_class"] != "local_main_runtime" or config["delivery_proven"] is not False:
            raise StoreError("broadcast receipt classification is invalid")
        manifest = config["manifest"]
        fields = {"kind", "profile_id", "profile_revision", "profile_name", "profile", "queue_index", "user_id"}
        api._object(manifest, fields, fields)
        if manifest["kind"] not in {"stream", "record", "emergency"}:
            raise StoreError("broadcast run kind is unsupported")
        api._id(manifest["profile_id"], "profile_id")
        api._int(manifest["profile_revision"], "profile_revision", 1, 2 ** 63 - 1)
        api._text(manifest["profile_name"], "profile_name", 160, True)
        if api._id(manifest["user_id"], "user_id") not in state.get("users", {}):
            raise StoreError("broadcast run owner is absent")
        if manifest["kind"] == "emergency":
            api._int(manifest["queue_index"], "queue_index", 0, 1)
            api.validate_configuration("emergency", manifest["profile"], sid, state, persisted=True)
        else:
            if manifest["queue_index"] is not None:
                raise StoreError("only emergency runs have a queue index")
            validate_configuration(PROFILE_KINDS[manifest["kind"]], manifest["profile"], sid, state, persisted=True)
        if config["manifest_hash"] != _hash(manifest):
            raise StoreError("broadcast manifest hash differs from its frozen profile")
        events = api._list(config["events"], "events", MAX_EVENTS)
        seen, frames, output_bytes, terminal = set(), 0, 0, False
        for event in events:
            fields = {"event_key", "state", "frames", "bytes", "detail", "occurred_at"}
            api._object(event, fields, fields)
            key = api._id(event["event_key"], "event_key")
            if key in seen or terminal or not isinstance(event["state"], str) or event["state"] not in {"active"} | TERMINAL:
                raise StoreError("broadcast events must be distinct and end at one terminal result")
            seen.add(key)
            api._int(event["frames"], "frames", frames, 2 ** 63 - 1)
            api._int(event["bytes"], "bytes", output_bytes, 2 ** 63 - 1)
            api._text(event["detail"], "detail", 1000)
            api._stamp(event["occurred_at"], "occurred_at")
            frames, output_bytes = event["frames"], event["bytes"]
            if event["state"] == "completed" and (frames == 0 or manifest["kind"] != "emergency" and output_bytes == 0):
                raise StoreError("completed native output requires actual nonzero counters")
            terminal = event["state"] in TERMINAL
        actual = events[-1]["state"] if events else "prepared"
        if config["state"] != actual:
            raise StoreError("broadcast run state differs from appended observations")


def guard_run(before, after):
    if after is None or after.get("status") != "active":
        raise StoreError("broadcast runtime receipts cannot be removed or hidden")
    for key in ("schema_version", "manifest", "manifest_hash", "proof_class", "delivery_proven"):
        if before["config"][key] != after["config"][key]:
            raise StoreError("broadcast runtime manifest and evidence classification are immutable")
    old, new = before["config"]["events"], after["config"]["events"]
    if len(new) < len(old) or new[:len(old)] != old:
        raise StoreError("broadcast runtime observations are append-only")


@api._consistent_read
def overview(store, principal, station_id):
    sid, _ = api._station(store, principal, station_id)
    profiles = {kind: [row for row in store.lookup("desktop_configurations", "station_kind", sid, document_kind)
                      if row["status"] == "active"] for kind, document_kind in PROFILE_KINDS.items()}
    emergency = [row for row in store.lookup("desktop_configurations", "station_kind", sid, "emergency")
                 if row["status"] == "active"]
    runs = sorted(store.lookup("desktop_configurations", "station_kind", sid, "broadcast_local_run"),
                  key=lambda row: (row["created_at"], row["_id"]), reverse=True)[:50]
    if sum(map(len, profiles.values())) + len(emergency) > 500:
        raise StoreError("broadcast workspace exceeds 500 profiles")
    # A bounded status overview does not repeatedly send archived queue contents.
    summaries = [{"run_id": row["_id"], "revision": row["_rev"], "kind": row["config"]["manifest"]["kind"],
        "name": row["name"], "state": row["config"]["state"], "created_at": row["created_at"],
        "last_event": deepcopy(row["config"]["events"][-1]) if row["config"]["events"] else None,
        "proof_class": "local_main_runtime", "delivery_proven": False} for row in runs]
    return _bounded({"profiles": profiles, "emergency": emergency, "runs": summaries, "sequence": store.sequence})


def profile_save(store, principal, station_id, kind, name, config, idempotency_key,
                 document_id=None, expected_revision=None):
    if not isinstance(kind, str) or kind not in PROFILE_KINDS:
        raise StoreError("profile kind must be stream, record or metadata")
    return api.put_document(store, principal, station_id, PROFILE_KINDS[kind], name, config,
        idempotency_key, document_id=document_id, expected_revision=expected_revision)


def _emergency_playback(store, principal, sid, row, queue_index):
    api._int(queue_index, "queue_index", 0, 1)
    saved = api.validate_configuration("emergency", row["config"], sid, store._state, persisted=True)
    queue = saved["queues"][queue_index]
    if not queue or len(queue) > 500:
        raise StoreError("emergency execution requires 1..500 managed media entries")
    events = []
    for index, entry in enumerate(queue):
        if not api.MEDIA_BINDING_KEYS.issubset(entry):
            raise StoreError("emergency entry is unbound; pick verified library media before takeover")
        # Reject disabled/unverified current bindings before selecting any fallback.
        binding = {key: entry[key] for key in api.MEDIA_BINDING_KEYS}
        api._media_binding(binding, sid, store._state)
        values = entry["values"]
        event = {"id": entry.get("id") or str(uuid5(UUID(row["_id"]), f"{queue_index}:{index}")),
            "title": values[0][:160] or "Acil medya", "artist": values[1][:120], "album": values[2][:120],
            "kind": "other", "start_time": "", "duration_ms": 0, "version_label": "Doğrulanmış medya",
            "primary_file": "", "backup_file": "", "note": "Yerel acil akış", **binding}
        events.append(event)
    programme = {"title": row["name"], "date": datetime.now(timezone.utc).date().isoformat(),
                 "note": "Yerel doğrulanmış acil liste", "events": events}
    prepared = desktop_playout.prepare(store, principal, sid, "programme", config=programme)
    if not all(track["eligible"] for track in prepared["tracks"]):
        raise Conflict("emergency media became unavailable; no partial takeover is allowed")
    prepared["emergency"] = {"document_id": row["_id"], "revision": row["_rev"], "queue_index": queue_index,
        "repeat": saved.get("preparation", {}).get("takeover/Liste bitince") == "Listeyi tekrarla"}
    return prepared


@api._consistent_read
def emergency_prepare(store, principal, station_id, document_id, expected_revision, queue_index=0):
    sid, _ = api._station(store, principal, station_id, "operate")
    row = _document(store, sid, "emergency", document_id)
    _revision(row, expected_revision)
    return _bounded({"document_id": row["_id"], "revision": row["_rev"], "queue_index": queue_index,
                     "playback": _emergency_playback(store, principal, sid, row, queue_index)})


def run_start(store, principal, station_id, kind, profile_id, expected_revision,
              idempotency_key, queue_index=0):
    sequence = store.sequence
    sid, station = api._station(store, principal, station_id, "operate")
    if station["status"] != "active":
        raise Conflict("native output execution requires an active station")
    if not isinstance(kind, str) or kind not in {"stream", "record", "emergency"}:
        raise StoreError("native run kind must be stream, record or emergency")
    api._int(queue_index, "queue_index", 0, 1)
    if kind != "emergency" and queue_index != 0:
        raise StoreError("queue_index belongs only to emergency execution")
    payload = {"kind": kind, "profile_id": profile_id, "expected_revision": expected_revision, "queue_index": queue_index}
    receipt, digest, replay = api.operation_replay(store, principal, sid, idempotency_key, "desktop-broadcast/run-start", payload)
    row = _document(store, sid, "emergency" if kind == "emergency" else PROFILE_KINDS[kind], profile_id)
    _revision(row, expected_revision)
    playback = _emergency_playback(store, principal, sid, row, queue_index) if kind == "emergency" else None
    if replay is not None:
        from .desktop_admin import assert_start_replay
        assert_start_replay(store, sid, replay["run_id"])
        return replay | ({"playback": playback} if playback is not None else {})
    manifest = {"kind": kind, "profile_id": row["_id"], "profile_revision": row["_rev"],
        "profile_name": row["name"], "profile": _receipt_profile(row["config"]),
        "queue_index": queue_index if kind == "emergency" else None, "user_id": principal.user_id}
    config = {"schema_version": 1, "manifest": manifest, "manifest_hash": _hash(manifest),
              "events": [], "state": "prepared", "proof_class": "local_main_runtime", "delivery_proven": False}
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    run = _put_run(tx, principal, sid, row["name"], config)
    result = {"run_id": run["_id"], "revision": run["_rev"], "kind": kind, "profile": _receipt_profile(row["config"]),
              "proof_class": "local_main_runtime", "delivery_proven": False}
    response = _bounded(result | ({"playback": playback} if playback is not None else {}))
    api.add_operation(tx, principal, sid, receipt, idempotency_key, "desktop-broadcast/run-start", digest, result)
    tx.commit()
    return response


def run_event(store, principal, station_id, run_id, expected_revision, state,
              frames, bytes, detail, idempotency_key):
    sequence = store.sequence
    sid, _ = api._station(store, principal, station_id, "operate")
    payload = {"run_id": run_id, "expected_revision": expected_revision, "state": state,
               "frames": frames, "bytes": bytes, "detail": detail}
    receipt, digest, replay = api.operation_replay(store, principal, sid, idempotency_key, "desktop-broadcast/run-event", payload)
    if replay is not None:
        return replay
    row = _document(store, sid, "broadcast_local_run", run_id)
    from .desktop_admin import assert_run_open
    assert_run_open(store, sid, run_id)
    if row["config"]["manifest"]["user_id"] != principal.user_id:
        raise Forbidden("native output observations require the run owner")
    _revision(row, expected_revision)
    config = deepcopy(row["config"])
    if config["state"] in TERMINAL:
        raise Conflict("native output already has a terminal observation")
    if len(config["events"]) >= MAX_EVENTS:
        raise StoreError("native run observation budget exhausted")
    config["events"].append({"event_key": api._id(idempotency_key, "idempotency_key"),
        "state": state, "frames": frames, "bytes": bytes, "detail": detail, "occurred_at": _now()})
    config["state"] = state
    validate_configuration("broadcast_local_run", config, sid, store._state, persisted=True)
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    updated = _put_run(tx, principal, sid, row["name"], config, row)
    result = {"run_id": updated["_id"], "revision": updated["_rev"], "state": state,
              "proof_class": "local_main_runtime", "delivery_proven": False}
    api.add_operation(tx, principal, sid, receipt, idempotency_key, "desktop-broadcast/run-event", digest, result)
    tx.commit()
    return result
