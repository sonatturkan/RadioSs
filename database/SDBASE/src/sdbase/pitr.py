"""Exact transaction-boundary recovery from a base backup and sealed WAL chain."""

from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
import tempfile

from .backup import (BackupError, _copy_file, _destination, _fence_restored_store,
                     _fsync_tree, _issue_provision_admin, _publish_absent,
                     _read_manifest, _revoke_other_tokens, _rotate_store_identity,
                     _safe_source, _validate_manifest, _verify_fenced_store, verify_backup)
from .storage import Store, _atomic_json
from .wal_archive import (_digest, _header, _instant, _root, apply_archive_record,
                          hydrate_assets, read_archive_records)


class RecoveryError(BackupError):
    pass


def preview_recovery(base_backup: str | Path, wal_directory: str | Path, *,
                     target_sequence: int | None = None, target_time: str | None = None,
                     inclusive: bool = True) -> dict:
    """Verify the complete input and choose a precise committed sequence.

    Exactly one target is mandatory. Wall time means the archive's monotonic
    effective commit time, explicitly separated from a possibly backward-moving
    host clock. An archive ending before the requested time cannot prove that
    target and is rejected instead of silently recovering only its available end.
    """
    if (target_sequence is None) == (target_time is None) or type(inclusive) is not bool:
        raise RecoveryError("specify exactly one recovery target: sequence or timestamp")
    verified = verify_backup(base_backup)
    base = Path(verified["path"])
    manifest = _read_manifest(base)
    root = _root(wal_directory)
    header = _header(root)
    identity = header["payload"]
    if verified["store_id"] != identity["source_store_id"] or \
            verified["schema_version"] != identity["schema_version"] or \
            verified["database"] != identity["database"] or \
            verified["sequence"] < identity["start_sequence"]:
        raise RecoveryError("base backup is outside this WAL source, schema, or archive timeline")
    _validate_manifest(manifest)
    # A base includes source metadata, so identity remains checkable even if a
    # legacy backup manifest does not yet carry its convenience WAL anchor.
    import json
    meta = json.loads((base / (verified["database"] + ".meta")).read_text(encoding="utf-8"))
    if meta.get("timeline_id") != identity["timeline_id"] or meta.get("epoch") != identity["epoch"] or \
            meta.get("wal_archive", {}).get("archive_id") != identity["archive_id"]:
        raise RecoveryError("base backup belongs to another primary timeline or WAL archive")
    records = read_archive_records(root, source_store_id=verified["store_id"],
                                   timeline_id=identity["timeline_id"], epoch=identity["epoch"])
    all_by_sequence = {row["payload"]["sequence"]: row for row in records}
    start, base_sequence = identity["start_sequence"], verified["sequence"]
    if base_sequence != start and base_sequence not in all_by_sequence:
        raise RecoveryError("base backup sequence is not sealed in this archive")
    anchor = header["sha256"] if base_sequence == start else all_by_sequence[base_sequence]["sha256"]
    declared_anchor = manifest.get("wal_anchor")
    if declared_anchor is not None and (not isinstance(declared_anchor, dict) or
            declared_anchor.get("sha256") != anchor or declared_anchor.get("sequence") != base_sequence or
            declared_anchor.get("archive_id") != identity["archive_id"]):
        raise RecoveryError("base backup WAL anchor does not match the sealed chain")
    last = records[-1]["payload"]["sequence"] if records else start
    base_time = (identity["created_at"] if base_sequence == start else
                 all_by_sequence[base_sequence]["payload"]["effective_at"])
    if target_sequence is not None:
        if type(target_sequence) is not int or not base_sequence <= target_sequence <= last:
            raise RecoveryError("requested sequence is before the base or beyond the sealed archive")
        target = target_sequence
    else:
        moment = _instant(target_time)
        end_time = records[-1]["payload"]["effective_at"] if records else identity["created_at"]
        if moment < _instant(base_time) or moment > _instant(end_time):
            raise RecoveryError("requested timestamp is outside the provable archive time range")
        if not inclusive and moment == _instant(base_time):
            raise RecoveryError("exclusive timestamp would precede the available base")
        eligible = [row["payload"]["sequence"] for row in records
                    if row["payload"]["sequence"] > base_sequence and
                    (_instant(row["payload"]["effective_at"]) <= moment if inclusive else
                     _instant(row["payload"]["effective_at"]) < moment)]
        target = max(eligible, default=base_sequence)
    selected = [row for row in records if base_sequence < row["payload"]["sequence"] <= target]
    if len(selected) != target - base_sequence:
        raise RecoveryError("recovery target has missing transactions")
    final_digest = anchor if not selected else selected[-1]["sha256"]
    result = {"base_backup": str(base), "wal_directory": str(root),
              "database": verified["database"], "source_store_id": verified["store_id"],
              "source_timeline_id": identity["timeline_id"], "source_epoch": identity["epoch"],
              "schema_version": verified["schema_version"], "base_sequence": base_sequence,
              "target_sequence": target, "target_time": target_time, "inclusive": inclusive,
              "recovery_records": len(selected), "checkpoint_barriers": sum(
                  row["payload"]["kind"] == "checkpoint" for row in selected),
              "base_anchor_sha256": anchor, "target_anchor_sha256": final_digest,
              "base_manifest_sha256": hashlib.sha256((base / "manifest.json").read_bytes()).hexdigest(),
              "archive_header_sha256": header["sha256"]}
    return result | {"preview_sha256": _digest(result)}


def recover_point_in_time(base_backup: str | Path, wal_directory: str | Path,
                          target_directory: str | Path, *,
                          target_sequence: int | None = None, target_time: str | None = None,
                          inclusive: bool = True, expected_preview_sha256: str | None = None) -> dict:
    """Restore, verify, fence and atomically publish a new independent node.

    No source data is changed. Source credentials, endpoint ownership and pending
    engine actions are fenced; a new recovery administrator is returned once.
    Operational fencing itself creates later transactions, reported separately
    from the exact recovered source sequence.
    """
    preview = preview_recovery(base_backup, wal_directory, target_sequence=target_sequence,
                               target_time=target_time, inclusive=inclusive)
    if expected_preview_sha256 is not None and expected_preview_sha256 != preview["preview_sha256"]:
        raise RecoveryError("recovery inputs differ from the reviewed preview")
    from .schema import SCHEMA_VERSION, open_radio_store
    if preview["schema_version"] != SCHEMA_VERSION:
        raise RecoveryError("recover with a matching schema package, then migrate the recovered copy")
    target = _destination(target_directory)
    source = Path(preview["base_backup"])
    wal = Path(preview["wal_directory"])
    if target.is_relative_to(source) or target.is_relative_to(wal):
        raise RecoveryError("recovery target must be outside its input archives")
    manifest = _read_manifest(source)
    database, files, _objects = _validate_manifest(manifest)
    records = read_archive_records(wal, after_sequence=preview["base_sequence"],
                                   through_sequence=preview["target_sequence"],
                                   source_store_id=preview["source_store_id"],
                                   timeline_id=preview["source_timeline_id"], epoch=preview["source_epoch"])
    stage = Path(tempfile.mkdtemp(prefix=f".{target.name}.", suffix=".pitr", dir=target.parent))
    try:
        for relative, entry in files.items():
            _copy_file(_safe_source(source, relative), stage / relative,
                       expected_sha=entry["sha256"], expected_size=entry["size_bytes"])
        # Detach the source archiver before any staging commits: it is external
        # to a base backup and belongs to the old primary timeline.
        import json
        meta_path = stage / (database + ".meta")
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        meta.pop("wal_archive", None)
        meta["node_role"] = "primary"
        _atomic_json(meta_path, meta)
        store = Store(stage / database, schema_version=SCHEMA_VERSION)
        with store._locked(exclusive=True):
            store._reload_locked()
            for record in records:
                hydrate_assets(wal, record, stage)
                apply_archive_record(store, record)
        if store.sequence != preview["target_sequence"]:
            raise RecoveryError("replay did not reach the exact requested transaction")
        # Check all current managed media and all registered cold archive files,
        # including assets first introduced after the original base snapshot.
        from .wal_archive import _assets_from_state
        from .backup import _digest_file
        for asset in _assets_from_state(store._state, database):
            digest, size = _digest_file(_safe_source(stage, asset["path"]))
            if digest != asset["sha256"] or size != asset["size_bytes"]:
                raise RecoveryError("recovered media or cold history checksum mismatch")
        recovered_sequence = store.sequence
        recovered_state_sha256 = _digest({"state": store._state,
            "tombstones": {name: sorted(ids) for name, ids in store._tombstones.items()},
            "retired_unique_keys": {name: {key: sorted(values) for key, values in indexes.items()}
                                    for name, indexes in store._retired_unique_keys.items()}})
        # Existing endpoint restoration guards intentionally prevent these live
        # transitions. A generic engine applies only trusted recovery fencing in
        # this unpublished directory, then the full domain schema is reopened.
        counts = _fence_restored_store(store)
        counts["other_tokens_revoked"] = _revoke_other_tokens(store)
        credential = _issue_provision_admin(store)
        final_sequence = store.checkpoint()
        source_store_id, store_id = _rotate_store_identity(store)
        reopened = open_radio_store(stage / database)
        if reopened.sequence != final_sequence or reopened._meta()["store_id"] != store_id:
            raise RecoveryError("recovered copy failed its final schema reopening")
        _verify_fenced_store(reopened, allowed_token_hash=hashlib.sha256(
            credential["admin_token"].encode()).hexdigest())
        from .identity import authenticate
        if authenticate(reopened, credential["admin_token"]).user_id != credential["admin_user_id"]:
            raise RecoveryError("new recovery administrator cannot authenticate")
        _fsync_tree(stage)
        _publish_absent(stage, target)
        return {"path": str(target), "database": database, "store_id": store_id,
                "source_store_id": source_store_id, "source_timeline_id": preview["source_timeline_id"],
                "base_sequence": preview["base_sequence"], "recovered_sequence": recovered_sequence,
                "restored_sequence": final_sequence, "recovered_state_sha256": recovered_state_sha256,
                "replayed_records": len(records), "checkpoint_barriers": preview["checkpoint_barriers"],
                "target_anchor_sha256": preview["target_anchor_sha256"], **counts, **credential}
    finally:
        if stage.exists():
            shutil.rmtree(stage)
