"""Station-scoped listener requests and their operator decisions.

Only opaque external keys and optional HMAC-SHA256 requester fingerprints are
stored. Names, messages, phone numbers, email addresses and IP addresses are
deliberately outside this data contract. A fulfilled request means a matching
playout attempt completed; it does not claim independent acoustic evidence.
"""

from __future__ import annotations

from datetime import datetime, timezone
import re
from uuid import UUID, uuid4

from .identity import Principal, require
from .storage import Conflict, Store, StoreError


STATES = frozenset({"pending", "accepted", "rejected", "queued", "fulfilled"})
REJECTION_REASONS = frozenset({
    "unavailable", "duplicate", "unsuitable", "policy", "withdrawn", "other",
})
_NAMESPACE = re.compile(r"[A-Za-z0-9_.:-]{1,64}\Z")
_DIGEST = re.compile(r"[0-9a-f]{64}\Z")


def _uuid(value: str, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _instant(value: str | datetime | None, field: str) -> str:
    if value is None:
        result = datetime.now(timezone.utc)
    elif isinstance(value, datetime):
        result = value
    elif isinstance(value, str):
        try:
            result = datetime.fromisoformat(value.replace("Z", "+00:00"))
        except ValueError as exc:
            raise StoreError(f"{field} must be an ISO 8601 instant") from exc
    else:
        raise StoreError(f"{field} must be an ISO 8601 instant")
    if result.tzinfo is None or result.utcoffset() is None:
        raise StoreError(f"{field} must contain a timezone")
    return result.astimezone(timezone.utc).isoformat()


def _body(row: dict) -> dict:
    return {key: value for key, value in row.items() if not key.startswith("_")}


def _visible(row: dict) -> dict:
    return {key: value for key, value in row.items() if key != "requester_fingerprint"}


def _item_in_station(store: Store, station_id: str, item_id: str) -> dict:
    item = store.get("items", item_id)
    if item is None or not store.lookup("station_items", "station_item", station_id, item_id):
        raise Conflict("requested item is not in station")
    return item


def _version_for_item(store: Store, item_id: str, version_id: str,
                      *, approved: bool) -> dict:
    version = store.get("versions", version_id)
    if version is None or version.get("item_id") != item_id:
        raise Conflict("requested version does not belong to item")
    if approved and version.get("status") != "approved":
        raise Conflict("selected version is not approved")
    return version


def _request(store: Store, station_id: str, request_id: str) -> dict:
    row = store.get("listener_requests", _uuid(request_id, "request_id"))
    if row is None or row["station_id"] != station_id:
        raise Conflict("listener request is not in station")
    return row


def submit_request(store: Store, principal: Principal, station_id: str, *,
                   source_namespace: str, external_request_key: str,
                   item_id: str, version_id: str | None = None,
                   requester_fingerprint: str | None = None,
                   requested_at: str | datetime | None = None) -> dict:
    """Create a pending request, or return the unchanged keyed request.

    ``requester_fingerprint`` may be a keyed HMAC-SHA256 hex digest supplied by
    the intake adapter. Never supply a raw email, phone number, IP or name.
    """
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "operate", station_id)
    if not isinstance(source_namespace, str) or not _NAMESPACE.fullmatch(source_namespace):
        raise StoreError("source_namespace must be an opaque 1..64 character name")
    if (not isinstance(external_request_key, str) or
            not 1 <= len(external_request_key) <= 200 or
            any(ord(char) < 32 or ord(char) == 127 for char in external_request_key)):
        raise StoreError("external_request_key must be 1..200 printable characters")
    if (requester_fingerprint is not None and
            (not isinstance(requester_fingerprint, str) or
             not _DIGEST.fullmatch(requester_fingerprint))):
        raise StoreError("requester_fingerprint must be a lowercase HMAC-SHA256 hex digest")
    item_id = _uuid(item_id, "item_id")
    version_id = _uuid(version_id, "version_id") if version_id else None
    requested_at = _instant(requested_at, "requested_at") if requested_at is not None else None
    identity = {"station_id": station_id, "source_namespace": source_namespace,
                "external_request_key": external_request_key, "item_id": item_id,
                "requested_version_id": version_id,
                "requester_fingerprint": requester_fingerprint}
    existing = store.lookup("listener_requests", "station_source_key",
                            station_id, source_namespace, external_request_key)
    if existing:
        row = existing[0]
        if any(row.get(key) != value for key, value in identity.items()):
            raise Conflict("external request key was reused for different content")
        if requested_at is not None and row["requested_at"] != requested_at:
            raise Conflict("external request key was reused with a different timestamp")
        return _visible(row) | {"replayed": True}
    sequence = store.sequence
    _item_in_station(store, station_id, item_id)
    if version_id:
        _version_for_item(store, item_id, version_id, approved=False)
    record = identity | {
        "status": "pending", "requested_at": requested_at or _instant(None, "requested_at"),
        "created_by": principal.user_id, "decided_at": None, "decided_by": None,
        "decision_reason": None, "selected_version_id": None,
        "plan_entry_id": None, "queue_mode": None, "queued_at": None,
        "queued_by": None, "fulfillment_attempt_id": None,
        "fulfilled_at": None, "fulfilled_by": None,
    }
    identifier = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("listener_requests", identifier, record)
    try:
        tx.commit()
    except Conflict:
        # A parallel replay may have committed first. The unique index is the
        # authority; an unrelated concurrent write still propagates Conflict.
        existing = store.lookup("listener_requests", "station_source_key",
                                station_id, source_namespace, external_request_key)
        if not existing:
            raise
        row = existing[0]
        if any(row.get(key) != value for key, value in identity.items()):
            raise Conflict("external request key was reused for different content")
        if requested_at is not None and row["requested_at"] != requested_at:
            raise Conflict("external request key was reused with a different timestamp")
        return _visible(row) | {"replayed": True}
    return _visible(store.get("listener_requests", identifier)) | {"replayed": False}


def decide_request(store: Store, principal: Principal, station_id: str,
                   request_id: str, *, decision: str,
                   expected_revision: int,
                   selected_version_id: str | None = None,
                   rejection_reason: str | None = None) -> dict:
    """Accept a request for an approved version or reject it with a code."""
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "operate", station_id)
    if decision not in {"accepted", "rejected"}:
        raise StoreError("decision must be accepted or rejected")
    row = _request(store, station_id, request_id)
    if row["_rev"] != expected_revision or row["status"] != "pending":
        raise Conflict("request changed or is no longer pending")
    sequence = store.sequence
    body = _body(row)
    body["status"] = decision
    body["decided_by"] = principal.user_id
    body["decided_at"] = _instant(None, "decided_at")
    if decision == "accepted":
        if rejection_reason is not None:
            raise StoreError("accepted request cannot have rejection_reason")
        selected = _uuid(selected_version_id, "selected_version_id") if selected_version_id else row.get("requested_version_id")
        if selected is None:
            raise StoreError("accepted request needs selected_version_id")
        if row.get("requested_version_id") and selected != row["requested_version_id"]:
            raise Conflict("selected version differs from requested version")
        item = _item_in_station(store, station_id, row["item_id"])
        if item.get("lifecycle") != "active":
            raise Conflict("requested item is not active")
        _version_for_item(store, row["item_id"], selected, approved=True)
        body["selected_version_id"] = selected
    else:
        if selected_version_id is not None or rejection_reason not in REJECTION_REASONS:
            raise StoreError("rejected request needs a reason code and no version")
        body["decision_reason"] = rejection_reason
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("listener_requests", row["_id"], body,
           expected_revision=expected_revision)
    tx.commit()
    return _visible(store.get("listener_requests", row["_id"]))


def queue_request(store: Store, principal: Principal, station_id: str,
                  request_id: str, *, expected_revision: int,
                  plan_entry_id: str | None = None) -> dict:
    """Record an operator queue decision, optionally tied to a current plan."""
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "operate", station_id)
    row = _request(store, station_id, request_id)
    if row["_rev"] != expected_revision or row["status"] != "accepted":
        raise Conflict("request changed or is not accepted")
    sequence = store.sequence
    item = _item_in_station(store, station_id, row["item_id"])
    if item.get("lifecycle") != "active":
        raise Conflict("requested item is no longer active")
    _version_for_item(store, row["item_id"], row["selected_version_id"], approved=True)
    entry_id = _uuid(plan_entry_id, "plan_entry_id") if plan_entry_id else None
    if entry_id:
        entry = store.get("plan_entries", entry_id)
        revision = store.get("plan_revisions", entry["plan_revision_id"]) if entry else None
        plan = store.get("plans", revision["plan_id"]) if revision else None
        if (entry is None or revision is None or plan is None or
                plan["station_id"] != station_id or
                plan.get("current_revision_id") != revision["_id"] or
                revision["status"] != "approved" or
                entry.get("event_kind") != "audio" or entry.get("state") != "planned" or
                entry.get("payload", {}).get("version_id") != row["selected_version_id"]):
            raise Conflict("plan entry is not a current approved matching audio selection")
    body = _body(row) | {"status": "queued", "plan_entry_id": entry_id,
                         "queue_mode": "plan" if entry_id else "manual",
                         "queued_by": principal.user_id,
                         "queued_at": _instant(None, "queued_at")}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("listener_requests", row["_id"], body,
           expected_revision=expected_revision)
    tx.commit()
    return _visible(store.get("listener_requests", row["_id"]))


def fulfill_request(store: Store, principal: Principal, station_id: str,
                    request_id: str, *, expected_revision: int,
                    playout_attempt_id: str) -> dict:
    """Mark fulfilled only after a matching completed playout attempt."""
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "operate", station_id)
    row = _request(store, station_id, request_id)
    if row["_rev"] != expected_revision or row["status"] != "queued":
        raise Conflict("request changed or is not queued")
    sequence = store.sequence
    attempt_id = _uuid(playout_attempt_id, "playout_attempt_id")
    attempt = store.get("playout_attempts", attempt_id)
    if (attempt is None or attempt["station_id"] != station_id or
            attempt.get("version_id") != row["selected_version_id"] or
            attempt.get("state") != "completed" or
            attempt.get("plan_entry_id") != row.get("plan_entry_id")):
        raise Conflict("playout attempt is not a completed matching request")
    body = _body(row) | {"status": "fulfilled",
                         "fulfillment_attempt_id": attempt_id,
                         "fulfilled_by": principal.user_id,
                         "fulfilled_at": _instant(None, "fulfilled_at")}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("listener_requests", row["_id"], body,
           expected_revision=expected_revision)
    tx.commit()
    return _visible(store.get("listener_requests", row["_id"]))


def list_requests(store: Store, principal: Principal, station_id: str, *,
                  status: str | None = None) -> list[dict]:
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "view", station_id)
    if status is not None and status not in STATES:
        raise StoreError("invalid listener request status")
    rows = store.lookup("listener_requests", "station", station_id)
    if status is not None:
        rows = [row for row in rows if row["status"] == status]
    return [_visible(row) for row in sorted(
        rows, key=lambda row: (row["requested_at"], row["_id"]))]
