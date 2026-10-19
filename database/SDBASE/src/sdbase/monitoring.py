"""Read-only engine statistics with whole-store administrator authorization."""

import shutil

from .identity import Forbidden


def require_engine_manager(store, principal):
    with store.read_snapshot(["users", "permissions", "stations"]) as snapshot:
        user = snapshot.get("users", principal.user_id)
        if principal.scope != "user" or not user or user.get("status") != "active":
            raise Forbidden("engine administration requires an active user manager")
        permissions = snapshot.lookup("permissions", "user", principal.user_id)
        if not any(not row.get("revoked_at") and row["permission"] == "manage" and row.get("station_id") is None
                   for row in permissions):
            raise Forbidden("engine statistics require global manage permission")


def engine_status(store):
    """Local operator statistics; credential hashes and record data are excluded."""
    with store._locked(exclusive=False):
        store._reload_locked()
        metadata = store._meta()
        result = {"store_id": metadata["store_id"], "schema_version": store.schema_version,
                  "sequence": store._sequence, "generation": metadata["generation"],
                  "node": {key: metadata.get(key) for key in
                           ("node_id", "node_role", "timeline_id", "epoch")},
                  "read_only_handle": store.read_only,
                  "collections": {name: len(rows) for name, rows in store._state.items()},
                  "database_bytes": {"journal": store.journal.stat().st_size,
                                     "checkpoint": store.checkpoint_file.stat().st_size
                                     if store.checkpoint_file.exists() else 0}}
    result["process_metrics"] = store.metrics.snapshot()
    usage = shutil.disk_usage(store.root)
    result["disk_bytes"] = {"total": usage.total, "used": usage.used, "free": usage.free}
    from .wal_archive import archive_status
    result["wal"] = archive_status(store, verify=False)
    from .replication import replication_status
    result["replication"] = replication_status(store)
    result["consistency"] = "Each section is captured under its own read lock; counters are process-local."
    return result


def read_engine_status(store, principal):
    require_engine_manager(store, principal)
    return engine_status(store)


def read_integrity(store, principal, **limits):
    require_engine_manager(store, principal)
    from .integrity import inspect_integrity
    return inspect_integrity(store, **limits)
