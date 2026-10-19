"""Versioned MIDI/cartwall device mappings; no event execution."""

from __future__ import annotations

from typing import Any, Mapping
from uuid import NAMESPACE_URL, UUID, uuid4, uuid5

from .identity import Principal, require
from .storage import Conflict, Store
from ._production_common import (ProductionError, _id, _body, _now,
                                 _version_in_station)


MIDI_EVENTS = frozenset({"note_on", "control_change", "program_change"})
TRIGGER_MODES = frozenset({"play_once", "toggle", "hold", "stop"})
PAD_TARGETS = frozenset({"cart", "version"})


def _device_spec(value: Mapping[str, Any]) -> dict:
    if not isinstance(value, Mapping) or set(value) - {
        "protocol", "manufacturer", "model", "input_port_key", "output_port_key",
        "notes", "midi_channel_policy", "fixed_channel",
    }:
        raise ProductionError("device_spec has unsupported fields")
    if value.get("protocol") != "midi1":
        raise ProductionError("only MIDI 1 profile metadata is supported")
    clean = {"protocol": "midi1"}
    for key in ("manufacturer", "model", "input_port_key", "output_port_key", "notes"):
        if key in value:
            item = value[key]
            if not isinstance(item, str) or len(item) > 256:
                raise ProductionError(f"device_spec.{key} must be a short string")
            clean[key] = item
    policy = value.get("midi_channel_policy", "per_pad")
    if policy not in {"per_pad", "fixed"}:
        raise ProductionError("unsupported MIDI channel policy")
    clean["midi_channel_policy"] = policy
    fixed = value.get("fixed_channel")
    if policy == "fixed":
        if isinstance(fixed, bool) or not isinstance(fixed, int) or not 1 <= fixed <= 16:
            raise ProductionError("fixed MIDI channel must be 1..16")
        clean["fixed_channel"] = fixed
    elif fixed is not None:
        raise ProductionError("fixed_channel requires fixed MIDI channel policy")
    return clean


def _profile_in_station(store: Store, station_id: str, profile_id: str) -> dict:
    profile = store.get("controller_profiles", profile_id)
    if profile is None or profile["station_id"] != station_id:
        raise ProductionError("controller profile is outside station")
    return profile


def _profile_version(store: Store, station_id: str,
                     profile_version_id: str) -> tuple[dict, dict]:
    version = store.get("controller_profile_versions", profile_version_id)
    if version is None:
        raise ProductionError("controller profile version is absent")
    return version, _profile_in_station(store, station_id, version["profile_id"])


def create_controller_profile(
    store: Store, principal: Principal, station_id: str | UUID, *,
    code: str, name: str, device_spec: Mapping[str, Any],
) -> dict:
    """Create profile identity and first draft mapping version."""
    station = _id(station_id, "station_id")
    require(store, principal, "manage", station)
    if not isinstance(code, str) or not code.strip() or len(code) > 64 or \
            not isinstance(name, str) or not name.strip() or len(name) > 128:
        raise ProductionError("profile code and name must be short nonempty strings")
    profile_id, version_id = str(uuid4()), str(uuid4())
    now = _now()
    tx = store.begin(actor=principal.user_id)
    tx.put("controller_profiles", profile_id, {
        "station_id": station, "code": code.strip(), "name": name.strip(),
        "status": "active", "current_version_id": None,
        "created_by": principal.user_id, "created_at": now,
    })
    tx.put("controller_profile_versions", version_id, {
        "profile_id": profile_id, "version_no": 1, "status": "draft",
        "device_spec": _device_spec(device_spec),
        "created_by": principal.user_id, "created_at": now,
    })
    tx.commit()
    return {"profile_id": profile_id, "draft_version_id": version_id,
            "version_no": 1}


def fork_controller_profile(
    store: Store, principal: Principal, station_id: str | UUID,
    profile_id: str | UUID, *, expected_profile_revision: int,
    device_spec: Mapping[str, Any] | None = None,
) -> dict:
    """Copy a published mapping to a new draft; published rows stay unchanged."""
    station, profile_id = _id(station_id, "station_id"), _id(profile_id, "profile_id")
    sequence = store.sequence
    require(store, principal, "manage", station)
    profile = _profile_in_station(store, station, profile_id)
    if profile["_rev"] != expected_profile_revision:
        raise Conflict("controller profile changed")
    current_id = profile.get("current_version_id")
    if not current_id:
        raise Conflict("publish the first draft before forking")
    versions = store.lookup("controller_profile_versions", "profile", profile_id)
    if any(row["status"] == "draft" for row in versions):
        raise Conflict("controller profile already has a draft")
    previous = store.get("controller_profile_versions", current_id)
    if previous is None or previous["status"] != "published":
        raise Conflict("current controller profile version is not published")
    new_id = str(uuid4())
    banks = store.lookup("controller_banks", "profile_version", current_id)
    bank_ids = {row["_id"]: str(uuid4()) for row in banks}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("controller_profile_versions", new_id, {
        "profile_id": profile_id,
        "version_no": max(row["version_no"] for row in versions) + 1,
        "status": "draft", "based_on_id": current_id,
        "device_spec": _device_spec(device_spec) if device_spec is not None else previous["device_spec"],
        "created_by": principal.user_id, "created_at": _now(),
    })
    for bank in banks:
        tx.put("controller_banks", bank_ids[bank["_id"]],
               _body(bank) | {"profile_version_id": new_id})
        for pad in store.lookup("controller_pads", "bank", bank["_id"]):
            tx.put("controller_pads", str(uuid4()),
                   _body(pad) | {"bank_id": bank_ids[bank["_id"]]})
    tx.expect("controller_profiles", profile_id, profile["_rev"])
    tx.expect("controller_profile_versions", current_id, previous["_rev"])
    tx.commit()
    return {"profile_id": profile_id, "draft_version_id": new_id,
            "version_no": max(row["version_no"] for row in versions) + 1}


def put_controller_bank(
    store: Store, principal: Principal, station_id: str | UUID,
    profile_version_id: str | UUID, *, bank_no: int, name: str,
    expected_revision: int | None = None,
) -> dict:
    station, version_id = _id(station_id, "station_id"), _id(profile_version_id, "profile_version_id")
    require(store, principal, "manage", station)
    version, _ = _profile_version(store, station, version_id)
    if version["status"] != "draft":
        raise Conflict("published controller mappings are immutable")
    if isinstance(bank_no, bool) or not isinstance(bank_no, int) or not 0 <= bank_no <= 127 or \
            not isinstance(name, str) or not name.strip() or len(name) > 128:
        raise ProductionError("bank number must be 0..127 and name must be nonempty")
    bank_id = str(uuid5(NAMESPACE_URL, f"sdbase-controller-bank:{version_id}:{bank_no}"))
    previous = store.get("controller_banks", bank_id)
    if previous is None and expected_revision is not None or \
            previous is not None and expected_revision != previous["_rev"]:
        raise Conflict("controller bank revision changed")
    body = {"profile_version_id": version_id, "bank_no": bank_no,
            "name": name.strip()}
    tx = store.begin(actor=principal.user_id)
    tx.expect("controller_profile_versions", version_id, version["_rev"])
    tx.put("controller_banks", bank_id, body,
           expected_revision=previous["_rev"] if previous else None)
    tx.commit()
    return store.get("controller_banks", bank_id)


def _pad_target(store: Store, station_id: str, kind: str,
                target_id: str, *, publication: bool) -> dict:
    if kind not in PAD_TARGETS:
        raise ProductionError("pad target must be a cart or content version")
    if kind == "cart":
        cart = store.get("carts", target_id)
        if cart is None or cart.get("station_id") != station_id:
            raise ProductionError("pad cart is outside station")
        if publication and cart.get("status") != "active":
            raise Conflict("pad cart is not active")
        return cart
    version, _ = _version_in_station(store, station_id, target_id)
    if publication and version.get("status") != "approved":
        raise Conflict("pad content version is not approved")
    return version


def put_controller_pad(
    store: Store, principal: Principal, station_id: str | UUID,
    bank_id: str | UUID, *, pad_no: int, label: str,
    midi_event: str, channel: int, number: int,
    target_kind: str, target_id: str | UUID, trigger_mode: str,
    expected_revision: int | None = None,
) -> dict:
    station, bank_id, target_id = (_id(station_id, "station_id"),
                                   _id(bank_id, "bank_id"), _id(target_id, "target_id"))
    require(store, principal, "manage", station)
    bank = store.get("controller_banks", bank_id)
    if bank is None:
        raise ProductionError("controller bank is absent")
    version, _ = _profile_version(store, station, bank["profile_version_id"])
    if version["status"] != "draft":
        raise Conflict("published controller mappings are immutable")
    if isinstance(pad_no, bool) or not isinstance(pad_no, int) or not 0 <= pad_no <= 127 or \
            not isinstance(label, str) or not label.strip() or len(label) > 128 or \
            midi_event not in MIDI_EVENTS or isinstance(channel, bool) or \
            not isinstance(channel, int) or not 1 <= channel <= 16 or \
            isinstance(number, bool) or not isinstance(number, int) or not 0 <= number <= 127 or \
            trigger_mode not in TRIGGER_MODES:
        raise ProductionError("pad label, MIDI address or trigger mode is invalid")
    spec = version["device_spec"]
    if spec.get("midi_channel_policy") == "fixed" and channel != spec.get("fixed_channel"):
        raise ProductionError("pad channel differs from fixed device profile channel")
    _pad_target(store, station, target_kind, target_id, publication=False)
    pad_id = str(uuid5(NAMESPACE_URL, f"sdbase-controller-pad:{bank_id}:{pad_no}"))
    previous = store.get("controller_pads", pad_id)
    if previous is None and expected_revision is not None or \
            previous is not None and expected_revision != previous["_rev"]:
        raise Conflict("controller pad revision changed")
    # Reject duplicate MIDI addresses within one bank before the unique index.
    for other in store.lookup("controller_pads", "bank", bank_id):
        if other["_id"] != pad_id and (other["midi_event"], other["channel"], other["number"]) == \
                (midi_event, channel, number):
            raise Conflict("MIDI address already maps to another pad in this bank")
    body = {"bank_id": bank_id, "pad_no": pad_no, "label": label.strip(),
            "midi_event": midi_event, "channel": channel, "number": number,
            "target_kind": target_kind, "target_id": target_id,
            "trigger_mode": trigger_mode}
    tx = store.begin(actor=principal.user_id)
    tx.expect("controller_profile_versions", version["_id"], version["_rev"])
    tx.expect("controller_banks", bank_id, bank["_rev"])
    tx.put("controller_pads", pad_id, body,
           expected_revision=previous["_rev"] if previous else None)
    tx.commit()
    return store.get("controller_pads", pad_id)


def publish_controller_profile(
    store: Store, principal: Principal, station_id: str | UUID,
    profile_version_id: str | UUID, *, expected_version_revision: int,
    expected_profile_revision: int,
) -> dict:
    station, version_id = _id(station_id, "station_id"), _id(profile_version_id, "profile_version_id")
    sequence = store.sequence
    require(store, principal, "manage", station)
    version, profile = _profile_version(store, station, version_id)
    if version["status"] != "draft" or version["_rev"] != expected_version_revision or \
            profile["_rev"] != expected_profile_revision:
        raise Conflict("controller draft or profile changed")
    banks = store.lookup("controller_banks", "profile_version", version_id)
    pads = [pad for bank in banks for pad in store.lookup("controller_pads", "bank", bank["_id"])]
    if not pads:
        raise Conflict("controller profile needs at least one mapped pad")
    targets = [_pad_target(store, station, pad["target_kind"], pad["target_id"],
                           publication=True) for pad in pads]
    old_id = profile.get("current_version_id")
    old = store.get("controller_profile_versions", old_id) if old_id else None
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    for target, pad in zip(targets, pads):
        tx.expect("carts" if pad["target_kind"] == "cart" else "versions",
                  target["_id"], target["_rev"])
    tx.put("controller_profile_versions", version_id,
           _body(version) | {"status": "published",
                             "published_at": _now(), "published_by": principal.user_id},
           expected_revision=expected_version_revision)
    if old:
        tx.put("controller_profile_versions", old["_id"],
               _body(old) | {"status": "superseded"}, expected_revision=old["_rev"])
    tx.put("controller_profiles", profile["_id"],
           _body(profile) | {"current_version_id": version_id},
           expected_revision=expected_profile_revision)
    tx.commit()
    return controller_profile_snapshot(store, station, profile["_id"])


def controller_profile_snapshot(store: Store, station_id: str | UUID,
                                profile_id: str | UUID) -> dict:
    """Describe the published map for a separate authorized MIDI executor."""
    sequence = store.sequence
    station, profile_id = _id(station_id, "station_id"), _id(profile_id, "profile_id")
    profile = _profile_in_station(store, station, profile_id)
    version_id = profile.get("current_version_id")
    if not version_id:
        raise Conflict("controller profile is not published")
    version = store.get("controller_profile_versions", version_id)
    if version is None or version["status"] != "published":
        raise Conflict("current controller profile version is not published")
    banks = []
    for bank in sorted(store.lookup("controller_banks", "profile_version", version_id),
                       key=lambda row: row["bank_no"]):
        pads = sorted(store.lookup("controller_pads", "bank", bank["_id"]),
                      key=lambda row: row["pad_no"])
        banks.append({"bank_id": bank["_id"], "bank_no": bank["bank_no"],
                      "name": bank["name"], "pads": [_body(pad) | {"pad_id": pad["_id"]}
                                                     for pad in pads]})
    if store.sequence != sequence:
        raise Conflict("controller profile changed while building its snapshot")
    return {"profile_id": profile_id, "station_id": station,
            "code": profile["code"], "name": profile["name"],
            "profile_version_id": version_id, "version_no": version["version_no"],
            "device_spec": version["device_spec"], "banks": banks,
            "execution_capability": "data_only"}
