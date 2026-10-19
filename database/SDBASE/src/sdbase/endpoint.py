"""Offline-capable endpoint cache using only the Python standard library.

The local journal is append-only, fsynced, hash chained, and owned by one
process. Media is copied into a content-addressed store via a temporary file,
checked against an approved snapshot, and atomically renamed. This module
stores plans, commands and evidence; it never produces or measures audio.

An external playout process must explicitly accept/complete commands. On
startup, commands that were accepted before a crash become ``outcome_unknown``
and are never executed again automatically. Historical events use a separate
durable outbox and retain their idempotency keys until acknowledged.
"""

from __future__ import annotations

from datetime import date, datetime, time, timedelta, timezone
import hashlib
import json
from pathlib import Path
import re
from typing import Any, BinaryIO, Mapping
from uuid import UUID, uuid4
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from .endpoint_protocol import EndpointTransport, TransportError
from .endpoint_journal import EndpointError, EndpointJournal, _json
from .endpoint_media import EndpointMediaStore, MediaIntegrityError
from .snapshot_contract import SnapshotContractError, validate_snapshot_contract


_SHA = re.compile(r"^[0-9a-fA-F]{64}$")
_COMMAND_KINDS = {"start", "stop", "skip", "load_plan", "takeover", "other"}
_REVISION_COMMANDS = {"start", "skip", "load_plan"}
_EVENT_KINDS = {"attempt", "start", "complete", "fail", "interrupt", "skip",
                "manual_start", "manual_complete"}
_MAX_GRANT_CLOCK_SKEW = timedelta(minutes=5)


def _uuid(value: Any, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise EndpointError(f"{field} must be a UUID") from exc


def _instant(value: Any, field: str) -> datetime:
    try:
        result = value if isinstance(value, datetime) else datetime.fromisoformat(
            str(value).replace("Z", "+00:00"))
    except ValueError as exc:
        raise EndpointError(f"{field} must be an ISO timestamp") from exc
    if result.tzinfo is None or result.utcoffset() is None:
        raise EndpointError(f"{field} needs a timezone offset")
    return result.astimezone(timezone.utc)


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _local_day_start(day: date, zone: ZoneInfo) -> datetime:
    """First real UTC instant of a local day, including midnight DST gaps."""
    midnight = datetime.combine(day, time.min)
    for minute in range(24 * 60):
        wall = midnight + timedelta(minutes=minute)
        instants = []
        for fold in (0, 1):
            candidate = wall.replace(tzinfo=zone, fold=fold).astimezone(timezone.utc)
            if candidate.astimezone(zone).replace(tzinfo=None) == wall:
                instants.append(candidate)
        if instants:
            return min(instants)
    raise EndpointError("station has no local civil time on the grant expiry date")


class EndpointAgent:
    """One endpoint's durable local state, independent of the central engine."""

    def __init__(self, root: Path, station_id: str | UUID, endpoint_id: str | UUID,
                 engine_instance: str, allowed_source_roots: tuple[Path, ...] | list[Path] = ()):
        self.root = Path(root).expanduser().resolve()
        self.station_id = _uuid(station_id, "station_id")
        self.endpoint_id = _uuid(endpoint_id, "endpoint_id")
        if not isinstance(engine_instance, str) or not engine_instance.strip():
            raise EndpointError("engine_instance is required")
        self.engine_instance = engine_instance
        self.allowed_source_roots = tuple(Path(p).expanduser().resolve(strict=True)
                                          for p in allowed_source_roots)
        if any(not p.is_dir() for p in self.allowed_source_roots):
            raise EndpointError("source roots must be directories")
        self.objects = self.root / "objects"
        self.staging = self.root / "staging"
        self.objects.mkdir(parents=True, exist_ok=True)
        self.staging.mkdir(parents=True, exist_ok=True)
        self.journal = EndpointJournal(self.root)
        self.media_store = EndpointMediaStore(self.root, self.allowed_source_roots, self.journal)
        try:
            identity = self.journal.state["identity"]
            if identity is None:
                self.journal.append("initialize", {
                    "station_id": self.station_id, "endpoint_id": self.endpoint_id,
                    "engine_instance": self.engine_instance, "format_version": 1,
                })
            elif (identity["station_id"], identity["endpoint_id"],
                  identity["engine_instance"]) != (self.station_id, self.endpoint_id,
                                                      self.engine_instance):
                raise EndpointError("state directory belongs to another endpoint or engine")
            self.reconcile_startup()
        except BaseException:
            self.journal.close()
            raise

    @classmethod
    def open(cls, root: Path, allowed_source_roots: tuple[Path, ...] | list[Path] = ()) -> EndpointAgent:
        root = Path(root).expanduser().resolve()
        path = root / "Endpoint.sdb"
        if not path.is_file():
            raise EndpointError("endpoint is not initialized")
        with path.open("rb") as stream:
            first = stream.readline()
        try:
            record = json.loads(first)
            identity = record["payload"]
            if record["kind"] != "initialize":
                raise KeyError("initialize")
            return cls(root, identity["station_id"], identity["endpoint_id"],
                       identity["engine_instance"], allowed_source_roots)
        except (KeyError, ValueError, TypeError, json.JSONDecodeError) as exc:
            raise EndpointError("endpoint identity record is invalid") from exc

    def close(self) -> None:
        self.journal.close()

    def __enter__(self) -> EndpointAgent:
        return self

    def __exit__(self, _type: Any, _value: Any, _traceback: Any) -> None:
        self.close()

    @property
    def _state(self) -> dict[str, Any]:
        return self.journal.state

    @property
    def _identity(self) -> dict[str, Any]:
        return self._state["identity"]

    def _object_path(self, sha256: str) -> Path:
        return self.media_store._object_path(sha256)

    def install_snapshot(self, body: Mapping[str, Any]) -> dict[str, Any]:
        """Install one immutable, endpoint-scoped revision and media manifest."""
        if not isinstance(body, Mapping):
            raise EndpointError("snapshot must be an object")
        snapshot = dict(body)
        try:
            validate_snapshot_contract(snapshot)
        except SnapshotContractError as exc:
            raise EndpointError(str(exc)) from exc
        if (_uuid(snapshot.get("station_id"), "station_id") != self.station_id or
                _uuid(snapshot.get("endpoint_id"), "endpoint_id") != self.endpoint_id):
            raise EndpointError("snapshot belongs to another endpoint or station")
        revision = _uuid(snapshot.get("plan_revision_id"), "plan_revision_id")
        _uuid(snapshot.get("plan_id"), "plan_id")
        try:
            service_date = date.fromisoformat(snapshot["service_date"])
            zone = ZoneInfo(snapshot["station_timezone"])
            activate_after = _instant(snapshot["activate_after"], "activate_after")
        except (KeyError, TypeError, ValueError, ZoneInfoNotFoundError) as exc:
            raise EndpointError("snapshot requires service_date, station_timezone and activate_after") from exc
        if activate_after.astimezone(zone).date() != service_date:
            raise EndpointError("activate_after does not fall on the plan service date")
        number = snapshot.get("revision_no")
        if isinstance(number, bool) or not isinstance(number, int) or number < 1:
            raise EndpointError("revision_no must be positive")
        entries = snapshot.get("entries")
        media = snapshot.get("media")
        if not isinstance(entries, list) or not isinstance(media, list):
            raise EndpointError("snapshot entries and media must be lists")
        media_specs: dict[str, tuple[str, int]] = {}
        for asset in media:
            if not isinstance(asset, Mapping):
                raise EndpointError("media manifest entry must be an object")
            media_id = _uuid(asset.get("media_file_id"), "media_file_id")
            sha = asset.get("sha256")
            size = asset.get("size_bytes")
            if not isinstance(sha, str) or not _SHA.fullmatch(sha):
                raise EndpointError("media SHA-256 must be 64 hex digits")
            if isinstance(size, bool) or not isinstance(size, int) or size < 0:
                raise EndpointError("media size must be nonnegative")
            spec = (sha.lower(), size)
            if media_id in media_specs and media_specs[media_id] != spec:
                raise EndpointError("conflicting media IDs in snapshot")
            prior = self._state["media"].get(media_id)
            if prior and (prior["sha256"], prior["size_bytes"]) != spec:
                raise EndpointError("media file ID changed hash or size")
            media_specs[media_id] = spec
        entry_ids: set[str] = set()
        ordinals: set[int] = set()
        for entry in entries:
            if not isinstance(entry, Mapping):
                raise EndpointError("plan entry must be an object")
            entry_id = _uuid(entry.get("plan_entry_id"), "plan_entry_id")
            ordinal = entry.get("ordinal")
            if isinstance(ordinal, bool) or not isinstance(ordinal, int) or ordinal < 0:
                raise EndpointError("entry ordinal must be nonnegative")
            _instant(entry.get("scheduled_at"), "scheduled_at")
            kind = entry.get("entry_kind", entry.get("event_kind"))
            if not isinstance(kind, str) or not kind.strip():
                raise EndpointError("entry_kind or event_kind is required")
            if entry_id in entry_ids or ordinal in ordinals:
                raise EndpointError("duplicate plan entry ID or ordinal")
            entry_ids.add(entry_id)
            ordinals.add(ordinal)
            if entry.get("version_id") is not None:
                _uuid(entry["version_id"], "version_id")
            if entry.get("media_file_id") is not None and \
                    _uuid(entry["media_file_id"], "media_file_id") not in media_specs:
                raise EndpointError("plan entry media is absent from manifest")
        serialized = _json(snapshot)
        sha = hashlib.sha256(serialized.encode("utf-8")).hexdigest()
        prior = self._state["snapshots"].get(revision)
        if prior:
            if prior["sha256"] != sha:
                raise EndpointError("revision snapshot changed under the same ID")
            return {"plan_revision_id": revision, "sha256": sha, "replayed": True,
                    "ready": self.snapshot_ready(revision)}
        self.journal.append("snapshot", {"revision_id": revision,
                                         "body": snapshot, "sha256": sha})
        self.verify_media()
        return {"plan_revision_id": revision, "sha256": sha, "replayed": False,
                "ready": self.snapshot_ready(revision)}

    def snapshot_ready(self, revision_id: str | UUID) -> bool:
        revision = _uuid(revision_id, "plan_revision_id")
        snapshot = self._state["snapshots"].get(revision)
        if snapshot is None or not self._snapshot_execution_valid(snapshot):
            return False
        return all(self._state["media"][str(asset["media_file_id"])]["status"] == "verified"
                   for asset in snapshot["body"]["media"])

    @staticmethod
    def _snapshot_execution_valid(snapshot: Mapping[str, Any]) -> bool:
        """Historic v1 records remain readable, but cannot be activated by v2."""
        try:
            validate_snapshot_contract(snapshot["body"])
            digest = hashlib.sha256(_json(snapshot["body"]).encode("utf-8")).hexdigest()
            return digest == snapshot["sha256"]
        except (SnapshotContractError, KeyError, TypeError, ValueError):
            return False

    def _store_offline_grant(self, raw: Mapping[str, Any]) -> dict[str, Any] | None:
        """Persist an authenticated delivery grant before it can be used."""
        if not isinstance(raw, Mapping):
            raise EndpointError("offline grant must be an object")
        grant = dict(raw)
        fields = {"grant_version", "grant_id", "station_id", "endpoint_id", "plan_revision_id",
                  "snapshot_sha256", "base_revision_id", "base_engine_epoch",
                  "engine_instance", "engine_epoch", "activate_after",
                  "expires_at", "issued_at"}
        if set(grant) != fields:
            raise EndpointError("offline grant fields are incomplete or unknown")
        if grant["grant_version"] != 1 or isinstance(grant["grant_version"], bool):
            raise EndpointError("unsupported offline grant version")
        revision = _uuid(grant["plan_revision_id"], "plan_revision_id")
        base = _uuid(grant["base_revision_id"], "base_revision_id")
        _uuid(grant["grant_id"], "grant_id")
        if _uuid(grant["station_id"], "station_id") != self.station_id or \
                _uuid(grant["endpoint_id"], "endpoint_id") != self.endpoint_id or \
                grant["engine_instance"] != self.engine_instance:
            raise EndpointError("offline grant belongs to another endpoint or engine")
        if isinstance(grant["base_engine_epoch"], bool) or \
                not isinstance(grant["base_engine_epoch"], int) or \
                grant["base_engine_epoch"] < 1 or \
                isinstance(grant["engine_epoch"], bool) or \
                grant["engine_epoch"] != grant["base_engine_epoch"] + 1:
            raise EndpointError("offline grant engine epoch does not advance exactly once")
        identity = self._identity
        if identity["active_revision_id"] != base or \
                identity["engine_epoch"] != grant["base_engine_epoch"] or identity["fenced"]:
            raise EndpointError("offline grant is not based on the accepted local plan")
        staged = self._state["snapshots"].get(revision)
        active = self._state["snapshots"].get(base)
        if staged is None or active is None or \
                staged["sha256"] != grant["snapshot_sha256"] or \
                not isinstance(grant["snapshot_sha256"], str) or \
                not _SHA.fullmatch(grant["snapshot_sha256"]):
            raise EndpointError("offline grant snapshot digest is absent or differs")
        target = staged["body"]
        source = active["body"]
        target_day = date.fromisoformat(target["service_date"])
        source_day = date.fromisoformat(source["service_date"])
        if target_day != source_day + timedelta(days=1) or \
                target["station_timezone"] != source["station_timezone"]:
            raise EndpointError("offline grant may cover only the next local service date")
        activate = _instant(grant["activate_after"], "activate_after")
        expires = _instant(grant["expires_at"], "expires_at")
        issued = _instant(grant["issued_at"], "issued_at")
        expected_end = _local_day_start(target_day + timedelta(days=1),
                                        ZoneInfo(target["station_timezone"]))
        if activate != _instant(target["activate_after"], "snapshot.activate_after") or \
                issued >= activate or expires != expected_end or \
                expires <= activate:
            raise EndpointError("offline grant has an invalid service window")
        if _now() >= expires:
            return None
        existing = self._state["offline_grants"].get(revision)
        if existing is not None:
            if existing != grant:
                raise EndpointError("offline grant changed under the same plan revision")
            return existing
        if abs(_now() - issued) > _MAX_GRANT_CLOCK_SKEW:
            raise EndpointError("endpoint clock differs from offline grant issue time")
        self.journal.append("offline_grant", {"grant": grant})
        return grant

    def _active_authorized(self, now: datetime | None = None) -> bool:
        identity = self._identity
        revision = identity["active_revision_id"]
        if identity["fenced"] or revision is None or revision not in self._state["snapshots"]:
            return False
        instant = _now() if now is None else now
        body = self._state["snapshots"][revision]["body"]
        if not self._snapshot_execution_valid(self._state["snapshots"][revision]):
            return False
        zone = ZoneInfo(body["station_timezone"])
        if instant.astimezone(zone).date() != date.fromisoformat(body["service_date"]):
            return False
        grant_id = identity.get("offline_grant_id")
        if grant_id is None:
            return True
        grant = self._state["offline_grants"].get(revision)
        return bool(grant and grant["grant_id"] == grant_id and
                    _instant(grant["activate_after"], "activate_after") <= instant <
                    _instant(grant["expires_at"], "expires_at") and
                    grant["engine_epoch"] == identity["engine_epoch"])

    def _activate_preapproved_offline(self, now: datetime | None = None) -> dict[str, Any]:
        """Switch only to a fully staged next-day grant during a central outage."""
        instant = _now() if now is None else now
        identity = self._identity
        if identity.get("offline_grant_id") is not None:
            if self._active_authorized(instant):
                return {"active": True, "replayed": True,
                        "revision_id": identity["active_revision_id"]}
            if not identity["fenced"]:
                self.journal.append("fence", {"fenced": True})
            self._expire_pending()
            return {"active": False, "reason": "offline_grant_expired_or_invalid"}
        if identity["fenced"] or not identity["active_revision_id"]:
            return {"active": False, "reason": "no_accepted_base_plan"}
        candidates = []
        for grant in self._state["offline_grants"].values():
            if grant["base_revision_id"] != identity["active_revision_id"] or \
                    grant["base_engine_epoch"] != identity["engine_epoch"] or \
                    grant["engine_instance"] != self.engine_instance:
                continue
            activate = _instant(grant["activate_after"], "activate_after")
            expires = _instant(grant["expires_at"], "expires_at")
            if not activate <= instant < expires:
                continue
            candidates.append(grant)
        if not candidates:
            return {"active": False, "reason": "no_valid_next_day_grant"}
        latest = max(_instant(grant["issued_at"], "issued_at") for grant in candidates)
        selected = [grant for grant in candidates
                    if _instant(grant["issued_at"], "issued_at") == latest]
        if len(selected) != 1:
            return {"active": False, "reason": "ambiguous_next_day_grants"}
        grant = selected[0]
        revision = grant["plan_revision_id"]
        if not self.snapshot_ready(revision):
            return {"active": False, "reason": "offline_media_missing"}
        self._activate_local(revision, grant["engine_epoch"],
                             offline_grant_id=grant["grant_id"])
        return {"active": True, "replayed": False,
                "revision_id": revision, "engine_epoch": grant["engine_epoch"]}

    def stage_media_stream(self, media_file_id: str | UUID,
                           stream: BinaryIO) -> dict[str, Any]:
        return self.media_store.stage_media_stream(media_file_id, stream)

    def stage_media(self, media_file_id: str | UUID, source: Path) -> dict[str, Any]:
        return self.media_store.stage_media(media_file_id, source)

    def verify_media(self) -> dict[str, int]:
        return self.media_store.verify_media()

    def reconcile_startup(self) -> dict[str, Any]:
        removed = 0
        for path in self.staging.glob("*.partial"):
            if path.is_file() and not path.is_symlink():
                path.unlink()
                removed += 1
        counts = self.verify_media()
        active = self._identity.get("active_revision_id")
        invalid_contract = active is not None and not self._snapshot_execution_valid(
            self._state["snapshots"].get(active, {}))
        if invalid_contract or (self._identity.get("offline_grant_id") and not self._active_authorized()):
            if not self._identity["fenced"]:
                self.journal.append("fence", {"fenced": True})
        for command_id, command in list(self._state["commands"].items()):
            if command["status"] == "accepted":
                if (command_id, False) not in self._state["acks"]:
                    self.journal.append("ack_queued", {"command_id": command_id,
                                                       "executed": False})
                self.journal.append("command_status", {"command_id": command_id,
                                                       "status": "outcome_unknown"})
            elif command["status"] == "executed":
                if (command_id, True) not in self._state["acks"]:
                    self.journal.append("ack_queued", {"command_id": command_id,
                                                       "executed": True})
            elif command["status"] == "pending" and self._command_stale(command):
                self.journal.append("command_status", {"command_id": command_id,
                                                       "status": "expired"})
        return {"removed_partials": removed, "media": counts,
                "active_revision_id": self._identity["active_revision_id"]}

    def _command_stale(self, command: Mapping[str, Any]) -> bool:
        identity = self._identity
        return (not self._active_authorized() or
                _instant(command["expires_at"], "expires_at") <= _now() or
                command["engine_epoch"] != identity["engine_epoch"] or
                (command["command_kind"] in _REVISION_COMMANDS and
                 command["payload"].get("plan_revision_id") != identity["active_revision_id"]))

    def _expire_pending(self) -> None:
        for command_id, command in list(self._state["commands"].items()):
            if command["status"] == "pending" and self._command_stale(command):
                self.journal.append("command_status", {"command_id": command_id,
                                                       "status": "expired"})

    def active_manifest(self) -> dict[str, Any]:
        """Give an external playout process only a ready, unfenced local plan."""
        self.verify_media()
        identity = self._identity
        revision = identity["active_revision_id"]
        if not self._active_authorized() or not revision or not self.snapshot_ready(revision):
            raise EndpointError("there is no ready, accepted local plan")
        snapshot = self._state["snapshots"][revision]["body"]
        return {**snapshot, "local_media": {
            str(asset["media_file_id"]): str(self._object_path(asset["sha256"].lower()))
            for asset in snapshot["media"]
        }}

    def _activate_local(self, revision: str, epoch: int,
                        *, offline_grant_id: str | None = None) -> None:
        self.verify_media()
        if not self.snapshot_ready(revision):
            raise EndpointError("cannot activate a snapshot with unverified media")
        if epoch < self._identity["engine_epoch"]:
            raise EndpointError("engine epoch cannot move backwards")
        if offline_grant_id is not None:
            grant = self._state["offline_grants"].get(revision)
            if grant is None or grant["grant_id"] != offline_grant_id or \
                    grant["engine_epoch"] != epoch:
                raise EndpointError("offline activation lacks a matching staged grant")
        self.journal.append("activate", {"revision_id": revision,
                                         "engine_epoch": epoch,
                                         "offline_grant_id": offline_grant_id})
        self._expire_pending()

    def activate_plan(self, revision_id: str | UUID, engine_epoch: int,
                      transport: EndpointTransport) -> dict[str, Any]:
        """Accept centrally only after every local object has been verified."""
        revision = _uuid(revision_id, "plan_revision_id")
        if isinstance(engine_epoch, bool) or not isinstance(engine_epoch, int) or engine_epoch < 1:
            raise EndpointError("engine_epoch must be positive")
        self.verify_media()
        if revision not in self._state["snapshots"] or not self.snapshot_ready(revision):
            raise EndpointError("snapshot or verified media is missing")
        activate_after = _instant(self._state["snapshots"][revision]["body"]["activate_after"],
                                  "activate_after")
        if _now() < activate_after:
            raise EndpointError("plan service date has not started")
        if engine_epoch < self._identity["engine_epoch"]:
            raise EndpointError("engine epoch cannot move backwards")
        if (self._identity["active_revision_id"] not in (None, revision) and
                engine_epoch <= self._identity["engine_epoch"]):
            raise EndpointError("a new plan revision requires a newer engine epoch")
        response = transport.accept_plan(self.station_id, self.endpoint_id,
                                         revision, self.engine_instance, engine_epoch)
        if (_uuid(response.get("accepted_revision_id"), "accepted_revision_id") != revision or
                response.get("engine_epoch") != engine_epoch):
            raise TransportError("central plan acceptance did not match request")
        self._activate_local(revision, engine_epoch)
        return {"accepted_revision_id": revision, "engine_epoch": engine_epoch}

    def prefetch_distribution(self, transport: EndpointTransport,
                              revision_id: str | UUID,
                              *, expected_activate_after: str | None = None) -> dict[str, Any]:
        """Stage/verify future media and acknowledge delivery without activation."""
        revision = _uuid(revision_id, "plan_revision_id")
        snapshot = transport.fetch_snapshot(self.station_id, self.endpoint_id, revision)
        if _uuid(snapshot.get("plan_revision_id"), "plan_revision_id") != revision:
            raise EndpointError("central returned a different revision snapshot")
        if expected_activate_after is not None and \
                _instant(snapshot.get("activate_after"), "activate_after") != \
                _instant(expected_activate_after, "pending_activate_after"):
            raise EndpointError("pending activation time differs from snapshot")
        result = self.install_snapshot(snapshot)
        for asset in snapshot["media"]:
            media_id = _uuid(asset["media_file_id"], "media_file_id")
            spec = self._state["media"][media_id]
            if spec["status"] != "verified":
                with transport.open_media(self.station_id, self.endpoint_id, media_id) as stream:
                    self.stage_media_stream(media_id, stream)
            transport.report_verified_media(
                self.station_id, self.endpoint_id, media_id, spec["sha256"],
                f"endpoint://{self.endpoint_id}/objects/{spec['sha256']}",
            )
        if not self.snapshot_ready(revision):
            raise EndpointError("distribution media is incomplete")
        delivery_ack = transport.acknowledge_delivery(
            self.station_id, self.endpoint_id, revision, result["sha256"])
        offline_grant = (delivery_ack.get("offline_grant")
                         if isinstance(delivery_ack, Mapping) else None)
        if offline_grant is not None and (
                not isinstance(offline_grant, Mapping) or
                _uuid(offline_grant.get("plan_revision_id"), "offline grant revision") != revision):
            raise EndpointError("delivery grant belongs to another plan revision")
        if offline_grant is not None and delivery_ack.get("status") not in {"sent", "accepted"}:
            raise EndpointError("offline grant was not attached to an acknowledged delivery")
        saved_grant = (self._store_offline_grant(offline_grant)
                       if offline_grant is not None else None)
        return {"plan_revision_id": revision, "snapshot_sha256": result["sha256"],
                "media_count": len(snapshot["media"]), "ready": True,
                "activate_after": snapshot["activate_after"],
                "offline_grant_id": saved_grant["grant_id"] if saved_grant else None,
                "active": self._identity["active_revision_id"] == revision}

    def receive_distribution(self, transport: EndpointTransport,
                             revision_id: str | UUID,
                             engine_epoch: int) -> dict[str, Any]:
        """Prefetch a revision; accept it only when its service date starts."""
        staged = self.prefetch_distribution(transport, revision_id)
        if _now() < _instant(staged["activate_after"], "activate_after"):
            return staged
        accepted = self.activate_plan(revision_id, engine_epoch, transport)
        return {**staged, **accepted, "active": True}

    def reconcile_central(self, state: Mapping[str, Any]) -> dict[str, Any]:
        """Stop local control if central ownership changed; recover crash gap."""
        if _uuid(state.get("endpoint_id"), "endpoint_id") != self.endpoint_id:
            raise EndpointError("central state belongs to another endpoint")
        revision_raw = state.get("accepted_revision_id")
        revision = _uuid(revision_raw, "accepted_revision_id") if revision_raw else None
        epoch = state.get("engine_epoch")
        if (revision and state.get("engine_instance") == self.engine_instance and
                isinstance(epoch, int) and epoch >= self._identity["engine_epoch"] and
                revision in self._state["snapshots"]):
            self.verify_media()
            if self.snapshot_ready(revision):
                if (self._identity["active_revision_id"] != revision or
                        self._identity["engine_epoch"] != epoch or
                        self._identity["fenced"] or
                        self._identity.get("offline_grant_id") is not None):
                    self._activate_local(revision, epoch)
                return {"fenced": False, "active_revision_id": revision,
                        "engine_epoch": epoch}
        if not self._identity["fenced"]:
            self.journal.append("fence", {"fenced": True})
        self._expire_pending()
        return {"fenced": True, "active_revision_id": revision,
                "engine_epoch": epoch}

    def ingest_commands(self, commands: list[Mapping[str, Any]]) -> dict[str, int]:
        """Persist fresh controls; no command is executed by this method."""
        if not isinstance(commands, list):
            raise EndpointError("commands must be a list")
        identity = self._identity
        active = identity["active_revision_id"]
        if not self._active_authorized() or not active:
            raise EndpointError("endpoint has no accepted plan")
        counts = {"accepted": 0, "rejected": 0, "replayed": 0}
        for raw in commands:
            if not isinstance(raw, Mapping):
                raise EndpointError("command must be an object")
            command_id = _uuid(raw.get("id", raw.get("command_id")), "command_id")
            kind = raw.get("command_kind")
            payload = raw.get("payload")
            epoch = raw.get("engine_epoch")
            if kind not in _COMMAND_KINDS or not isinstance(payload, dict) or \
                    isinstance(epoch, bool) or not isinstance(epoch, int):
                raise EndpointError("invalid command envelope")
            issued = _instant(raw.get("issued_at"), "issued_at")
            expires = _instant(raw.get("expires_at"), "expires_at")
            if expires <= issued:
                raise EndpointError("invalid command TTL")
            command = {"command_id": command_id, "command_kind": kind,
                       "payload": payload, "engine_epoch": epoch,
                       "issued_at": issued.isoformat(),
                       "expires_at": expires.isoformat()}
            prior = self._state["commands"].get(command_id)
            if prior:
                if any(prior[key] != value for key, value in command.items()):
                    raise EndpointError("command ID was reused with changed content")
                counts["replayed"] += 1
            elif (expires <= _now() or epoch != identity["engine_epoch"] or
                  (kind in _REVISION_COMMANDS and payload.get("plan_revision_id") != active)):
                counts["rejected"] += 1
            else:
                if kind == "start":
                    self._start_entry_media(payload)
                self.journal.append("command", command)
                counts["accepted"] += 1
        return counts

    def pending_commands(self) -> list[dict[str, Any]]:
        self._expire_pending()
        active = self._identity["active_revision_id"]
        if not self._active_authorized() or not active:
            return []
        return [{key: value for key, value in command.items() if key != "status"}
                for command in self._state["commands"].values()
                if command["status"] == "pending"]

    def accept_command(self, command_id: str | UUID) -> dict[str, Any]:
        """A playout process accepted a fresh command; audio is not implied."""
        key = _uuid(command_id, "command_id")
        self._expire_pending()
        command = self._state["commands"].get(key)
        if not command or command["status"] != "pending" or self._command_stale(command):
            raise EndpointError("command is stale, unknown, or already accepted")
        if command["command_kind"] == "start":
            media_id = self._start_entry_media(command["payload"])
            if media_id and self.media_store.verify_media_file(media_id) != "verified":
                raise EndpointError("selected plan entry media is missing or corrupt")
        self.journal.append("command_status", {"command_id": key,
                                               "status": "accepted"})
        self.journal.append("ack_queued", {"command_id": key, "executed": False})
        return {"command_id": key, "status": "accepted"}

    def _start_entry_media(self, payload: Mapping[str, Any]) -> str | None:
        """Bind a start control to the active plan's exact entry and media."""
        active = self._identity["active_revision_id"]
        if not active:
            raise EndpointError("no accepted plan")
        entry_id = _uuid(payload.get("plan_entry_id"), "plan_entry_id")
        entries = self._state["snapshots"][active]["body"]["entries"]
        for entry in entries:
            if _uuid(entry["plan_entry_id"], "plan_entry_id") == entry_id:
                media_id = entry.get("media_file_id")
                return _uuid(media_id, "media_file_id") if media_id else None
        raise EndpointError("start command references an entry outside the active plan")

    def complete_command(self, command_id: str | UUID) -> dict[str, Any]:
        """Explicit execution callback from the external playout process."""
        key = _uuid(command_id, "command_id")
        command = self._state["commands"].get(key)
        if not command:
            raise EndpointError("command not found")
        if command["status"] == "executed":
            if (key, True) not in self._state["acks"]:
                self.journal.append("ack_queued", {"command_id": key,
                                                   "executed": True})
            return {"command_id": key, "status": "executed", "replayed": True}
        if (command["status"] != "accepted" or not self._active_authorized() or
                command["engine_epoch"] != self._identity["engine_epoch"]):
            raise EndpointError("execution cannot be confirmed for a stale command")
        self.journal.append("command_status", {"command_id": key,
                                               "status": "executed"})
        self.journal.append("ack_queued", {"command_id": key, "executed": True})
        return {"command_id": key, "status": "executed", "replayed": False}

    def queue_playout_event(self, payload: Mapping[str, Any]) -> dict[str, Any]:
        """Record immutable playout evidence before trying to reach the center."""
        if not isinstance(payload, Mapping):
            raise EndpointError("event must be an object")
        raw = dict(payload)
        raw.setdefault("engine_event_id", str(uuid4()))
        event_id = raw["engine_event_id"]
        if not isinstance(event_id, str) or not event_id.strip():
            raise EndpointError("engine_event_id is required")
        if (_uuid(raw.get("station_id"), "station_id") != self.station_id or
                _uuid(raw.get("endpoint_id"), "endpoint_id") != self.endpoint_id):
            raise EndpointError("event belongs to another endpoint or station")
        version = _uuid(raw.get("version_id"), "version_id")
        kind = raw.get("event_type")
        if kind not in _EVENT_KINDS:
            raise EndpointError("invalid event type")
        occurred = _instant(raw.get("occurred_at"), "occurred_at")
        attempt = raw.get("attempt_no", 1)
        if isinstance(attempt, bool) or not isinstance(attempt, int) or attempt < 1:
            raise EndpointError("attempt_no must be positive")
        entry = _uuid(raw["plan_entry_id"], "plan_entry_id") if raw.get("plan_entry_id") else None
        media = _uuid(raw["media_file_id"], "media_file_id") if raw.get("media_file_id") else None
        details = raw.get("details", {})
        if not isinstance(details, dict):
            raise EndpointError("event details must be an object")
        if kind in {"manual_start", "manual_complete"}:
            if not isinstance(details.get("attempt_key"), str) or not details["attempt_key"].strip():
                raise EndpointError("manual event needs details.attempt_key")
        elif entry is None:
            raise EndpointError("planned event needs plan_entry_id")
        if entry and not any(
            any(str(item.get("plan_entry_id")) == entry and
                str(item.get("version_id")) == version and
                (media is None or str(item.get("media_file_id")) == media)
                for item in snapshot["body"]["entries"])
            for snapshot in self._state["snapshots"].values()
        ):
            raise EndpointError("event entry/version/media does not match an installed plan")
        body = {"station_id": self.station_id, "endpoint_id": self.endpoint_id,
                "version_id": version, "engine_event_id": event_id,
                "event_type": kind, "occurred_at": occurred.isoformat(),
                "attempt_no": attempt, "plan_entry_id": entry,
                "media_file_id": media, "details": details}
        prior = self._state["events"].get(event_id)
        if prior:
            if prior["payload"] != body:
                raise EndpointError("event ID was reused with changed evidence")
            return {"engine_event_id": event_id, "replayed": True,
                    "delivered": prior["central_event_id"] is not None}
        self.journal.append("event", {"engine_event_id": event_id,
                                      "payload": body})
        return {"engine_event_id": event_id, "replayed": False,
                "delivered": False}

    def replay_events(self, transport: EndpointTransport, limit: int = 100) -> dict[str, Any]:
        if limit < 1:
            raise EndpointError("limit must be positive")
        delivered = 0
        pending = [event for event in self._state["events"].values()
                   if event["central_event_id"] is None][:limit]
        for event in pending:
            event_id = event["engine_event_id"]
            try:
                response = transport.record_playout_event(event["payload"])
                central_id = _uuid(response.get("event_id"), "event_id")
            except (TransportError, EndpointError, OSError) as exc:
                self.journal.append("event_attempt", {"engine_event_id": event_id})
                return {"delivered": delivered, "pending": self.pending_event_count(),
                        "error": str(exc)}
            self.journal.append("event_attempt", {"engine_event_id": event_id})
            self.journal.append("event_delivered", {"engine_event_id": event_id,
                                                    "central_event_id": central_id})
            delivered += 1
        return {"delivered": delivered, "pending": self.pending_event_count()}

    def pending_event_count(self) -> int:
        return sum(event["central_event_id"] is None
                   for event in self._state["events"].values())

    def flush_acks(self, transport: EndpointTransport, limit: int = 100) -> dict[str, Any]:
        delivered = discarded = 0
        pending = [ack for ack in self._state["acks"].values()
                   if ack["status"] == "queued"][:limit]
        for ack in pending:
            command = self._state["commands"][ack["command_id"]]
            if (self._identity["fenced"] or
                command["engine_epoch"] != self._identity["engine_epoch"] or
                (not ack["executed"] and
                 _instant(command["expires_at"], "expires_at") <= _now())):
                self.journal.append("ack_status", {"command_id": ack["command_id"],
                                                   "executed": ack["executed"],
                                                   "status": "discarded"})
                discarded += 1
                continue
            try:
                transport.acknowledge_command(
                    self.station_id, self.endpoint_id, ack["command_id"],
                    self.engine_instance, command["engine_epoch"], ack["executed"],
                )
            except (TransportError, OSError) as exc:
                return {"delivered": delivered, "discarded": discarded,
                        "error": str(exc)}
            self.journal.append("ack_status", {"command_id": ack["command_id"],
                                               "executed": ack["executed"],
                                               "status": "delivered"})
            delivered += 1
        return {"delivered": delivered, "discarded": discarded}

    def _reconcile_offline_reconnect(self, transport: EndpointTransport,
                                     remote: Mapping[str, Any]) -> Mapping[str, Any]:
        """Convert a local grant to central acceptance or fence divergent ownership."""
        grant_id = self._identity.get("offline_grant_id")
        if grant_id is None:
            return remote
        revision = self._identity["active_revision_id"]
        grant = self._state["offline_grants"].get(revision)
        if grant is None or grant.get("grant_id") != grant_id:
            self.journal.append("fence", {"fenced": True})
            return remote
        if (_uuid(remote.get("endpoint_id"), "endpoint_id") == self.endpoint_id and
                remote.get("accepted_revision_id") == revision and
                remote.get("engine_instance") == self.engine_instance and
                remote.get("engine_epoch") == grant["engine_epoch"]):
            return remote
        can_accept = (self._active_authorized() and
                      remote.get("accepted_revision_id") == grant["base_revision_id"] and
                      remote.get("engine_instance") == self.engine_instance and
                      remote.get("engine_epoch") == grant["base_engine_epoch"] and
                      remote.get("pending_revision_id") == revision)
        if can_accept:
            try:
                response = transport.accept_plan(
                    self.station_id, self.endpoint_id, revision,
                    self.engine_instance, grant["engine_epoch"])
                if (_uuid(response.get("accepted_revision_id"), "accepted_revision_id") != revision or
                        response.get("engine_epoch") != grant["engine_epoch"]):
                    raise EndpointError("central did not accept the granted engine epoch")
                return transport.plan_state(self.endpoint_id)
            except (TransportError, OSError, EndpointError):
                # Central was reached but could not confirm ownership. Local
                # control must stop until a fresh accepted state is observed.
                pass
        if not self._identity["fenced"]:
            self.journal.append("fence", {"fenced": True})
        self._expire_pending()
        return remote

    def sync_once(self, transport: EndpointTransport) -> dict[str, Any]:
        """One reconnect cycle; never replays an old command as a new action."""
        self.verify_media()
        try:
            remote = transport.plan_state(self.endpoint_id)
        except (TransportError, OSError) as exc:
            offline = self._activate_preapproved_offline()
            return {"online": False, "pending_events": self.pending_event_count(),
                    "offline_activation": offline,
                    "fenced": self._identity["fenced"], "error": str(exc)}
        remote = self._reconcile_offline_reconnect(transport, remote)
        central = self.reconcile_central(remote)
        if not central["fenced"] and remote.get("accepted_revision_id"):
            heartbeat = getattr(transport, "heartbeat_acceptance", None)
            if heartbeat is not None:
                try:
                    beat = heartbeat(self.station_id, self.endpoint_id,
                                     remote["accepted_revision_id"], self.engine_instance,
                                     self._identity["engine_epoch"])
                    if (_uuid(beat.get("accepted_revision_id"), "accepted_revision_id") !=
                            remote["accepted_revision_id"] or
                            beat.get("engine_epoch") != self._identity["engine_epoch"]):
                        raise EndpointError("central heartbeat response changed ownership")
                except (TransportError, OSError) as exc:
                    return {"online": False, "fenced": False,
                            "pending_events": self.pending_event_count(),
                            "error": str(exc)}
                except EndpointError as exc:
                    self.journal.append("fence", {"fenced": True})
                    self._expire_pending()
                    return {"online": True, "fenced": True, "error": str(exc)}
        # Historical events must not wait behind a failed media delivery.
        events = self.replay_events(transport)
        pending_revision = remote.get("pending_revision_id")
        delivery = None
        if pending_revision:
            owner = remote.get("engine_instance")
            if owner not in (None, self.engine_instance):
                return {"online": True, "fenced": True,
                        "error": "central endpoint belongs to another engine instance"}
            remote_epoch = remote.get("engine_epoch") or 0
            try:
                delivery = self.prefetch_distribution(
                    transport, pending_revision,
                    expected_activate_after=remote.get("pending_activate_after"))
                if _now() >= _instant(delivery["activate_after"], "activate_after"):
                    same_revision = remote.get("accepted_revision_id") == pending_revision
                    epoch = (remote_epoch if same_revision and owner == self.engine_instance else
                             max(self._identity["engine_epoch"], remote_epoch) + 1)
                    accepted = self.activate_plan(pending_revision, epoch, transport)
                    delivery = {**delivery, **accepted, "active": True}
                    remote = transport.plan_state(self.endpoint_id)
                    central = self.reconcile_central(remote)
            except (TransportError, OSError) as exc:
                return {"online": False, "fenced": central["fenced"],
                        "events": events, "pending_events": self.pending_event_count(),
                        "error": str(exc)}
        if central["fenced"]:
            return {"online": True, "fenced": True, "delivery": delivery,
                    "events": events}
        acks = self.flush_acks(transport)
        try:
            commands = self.ingest_commands(transport.poll_commands(
                self.station_id, self.endpoint_id, self.engine_instance,
                self._identity["engine_epoch"]))
        except (TransportError, OSError) as exc:
            return {"online": False, "fenced": False, "delivery": delivery,
                    "events": events, "acks": acks, "error": str(exc)}
        return {"online": True, "fenced": False, "delivery": delivery,
                "events": events, "acks": acks, "commands": commands}

    def status(self) -> dict[str, Any]:
        identity = self._identity
        active = identity["active_revision_id"]
        return {"station_id": self.station_id, "endpoint_id": self.endpoint_id,
                "engine_instance": self.engine_instance,
                "engine_epoch": identity["engine_epoch"],
                "active_revision_id": active, "fenced": identity["fenced"],
                "offline_grant_id": identity.get("offline_grant_id"),
                "offline_grant_expires_at": (
                    self._state["offline_grants"][active]["expires_at"]
                    if active in self._state["offline_grants"] and
                    identity.get("offline_grant_id") else None),
                "active_authorized": self._active_authorized(),
                "ready": bool(active and self.snapshot_ready(active)),
                "pending_commands": sum(c["status"] == "pending"
                                        for c in self._state["commands"].values()),
                "pending_acks": sum(a["status"] == "queued"
                                    for a in self._state["acks"].values()),
                "pending_events": self.pending_event_count(),
                "journal_records": self.journal.seq}
