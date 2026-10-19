"""Previewable, atomic bulk edits with revision-safe conditional undo."""

from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from uuid import UUID, uuid4

from .catalog import require_shared_item_permission
from .identity import Principal, require
from .metadata import _field, _validate_value, value_for
from .storage import Conflict, Store, StoreError


def _body(record: dict) -> dict:
    return {key: deepcopy(value) for key, value in record.items() if not key.startswith("_")}


def _new_value(mode: str, before: object, value: object = None) -> object:
    if mode == "untouched":
        return before
    if mode == "set":
        return value
    if mode == "clear":
        return None
    if mode in ("add", "remove"):
        if not isinstance(before, list) and before is not None:
            raise StoreError("add/remove need an array field")
        if not isinstance(value, list):
            raise StoreError("add/remove value must be an array")
        current = list(before or [])
        if mode == "add":
            for element in value:
                if element not in current:
                    current.append(element)
        else:
            current = [element for element in current if element not in value]
        return current
    raise StoreError("unknown bulk operation mode")


def _prepare(store: Store, station_id: str, item_ids: list[str],
             operations: dict[str, dict]) -> tuple[list[dict], list[dict]]:
    if not item_ids or len(item_ids) > 500 or len(item_ids) != len(set(item_ids)):
        raise StoreError("bulk selection needs 1..500 distinct item IDs")
    if not operations:
        raise StoreError("bulk operation cannot be empty")
    changes: list[dict] = []
    summary: list[dict] = []
    for raw_id in item_ids:
        item_id = str(UUID(raw_id))
        item = store.get("items", item_id)
        station_rows = store.lookup("station_items", "station_item", station_id, item_id)
        if item is None or not station_rows:
            raise StoreError(f"item {item_id} is not in station")
        association = station_rows[0]
        item_before, item_after = _body(item), _body(item)
        station_before, station_after = _body(association), _body(association)
        field_summaries = []
        for key, operation in operations.items():
            if not isinstance(operation, dict):
                raise StoreError("each bulk operation must be an object")
            mode = operation.get("mode")
            if mode == "untouched":
                continue
            if key == "title":
                if mode != "set" or not isinstance(operation.get("value"), str) or not operation["value"].strip():
                    raise StoreError("title only supports nonempty set")
                item_after["title"] = operation["value"].strip()
                field_summaries.append({"field": key, "before": item_before["title"],
                                        "after": item_after["title"]})
            elif key == "category_id":
                if mode not in ("set", "clear"):
                    raise StoreError("category only supports set/clear")
                new = str(UUID(operation["value"])) if mode == "set" else None
                if new is not None:
                    category = store.get("categories", new)
                    if category is None or category["station_id"] != station_id:
                        raise StoreError("category belongs to another station")
                station_after["category_id"] = new
                field_summaries.append({"field": key, "before": station_before.get("category_id"),
                                        "after": new})
            else:
                field = _field(store, key)
                if field["scope"] != "item":
                    raise StoreError("bulk item edit cannot modify version metadata")
                local = store.lookup("metadata_values", "field_target", field["_id"],
                                     "items", item_id, station_id)
                local = local[0] if local else None
                effective = value_for(store, key, item_id, station_id=station_id)
                prior_value = None if effective is None or effective["is_cleared"] else effective["value"]
                new_value = _new_value(mode, prior_value, operation.get("value"))
                if mode not in ("set", "add", "remove", "clear"):
                    raise StoreError("unsupported metadata operation")
                if new_value is not None:
                    _validate_value(field, new_value)
                after = _body(local) if local else {
                    "field_id": field["_id"], "target_collection": "items",
                    "target_id": item_id, "station_scope": station_id,
                }
                after.update({"value": new_value, "is_cleared": new_value is None,
                              "source_kind": "manual", "approval_status": "unreviewed"})
                changes.append({"collection": "metadata_values",
                                "key": local["_id"] if local else str(uuid4()),
                                "before": _body(local) if local else None,
                                "before_rev": local["_rev"] if local else None,
                                "after": after, "after_rev": (local["_rev"] + 1) if local else 1})
                field_summaries.append({"field": key, "before": prior_value, "after": new_value})
        for collection, record, before, after in (("items", item, item_before, item_after),
                                                  ("station_items", association, station_before, station_after)):
            if before != after:
                changes.append({"collection": collection, "key": record["_id"],
                                "before": before, "before_rev": record["_rev"],
                                "after": after, "after_rev": record["_rev"] + 1})
        summary.append({"item_id": item_id, "item_revision": item["_rev"],
                        "changes": field_summaries})
    return changes, summary


def preview(store: Store, principal: Principal, station_id: str, item_ids: list[str],
            operations: dict[str, dict]) -> dict:
    station_id = str(UUID(station_id))
    require(store, principal, "bulk_edit", station_id)
    title_operation = operations.get("title") if isinstance(operations, dict) else None
    if isinstance(title_operation, dict) and title_operation.get("mode") == "set":
        for item_id in item_ids:
            require_shared_item_permission(store, principal, str(UUID(item_id)), "bulk_edit")
    changes, summary = _prepare(store, station_id, item_ids, operations)
    return {"station_id": station_id, "selected": len(summary),
            "document_changes": len(changes), "items": summary,
            "store_sequence": store.sequence}


def apply(store: Store, principal: Principal, station_id: str, item_ids: list[str],
          operations: dict[str, dict], expected_revisions: dict[str, int],
          *, idempotency_key: str) -> dict:
    station_id = str(UUID(station_id))
    sequence = store.sequence
    require(store, principal, "bulk_edit", station_id)
    title_operation = operations.get("title") if isinstance(operations, dict) else None
    if isinstance(title_operation, dict) and title_operation.get("mode") == "set":
        for item_id in item_ids:
            require_shared_item_permission(store, principal, str(UUID(item_id)), "bulk_edit")
    if not idempotency_key:
        raise StoreError("bulk idempotency key required")
    intent = {"station_id": station_id, "item_ids": item_ids, "operations": operations,
              "expected_revisions": expected_revisions}
    fingerprint = hashlib.sha256(json.dumps(intent, sort_keys=True,
                                            separators=(",", ":")).encode()).hexdigest()
    existing = store.lookup("bulk_jobs", "station_key", station_id, idempotency_key)
    if existing:
        if existing[0]["fingerprint"] != fingerprint:
            raise Conflict("bulk idempotency key reused for a different edit")
        return existing[0]
    changes, summary = _prepare(store, station_id, item_ids, operations)
    job_id = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    for row in summary:
        item_id = row["item_id"]
        if expected_revisions.get(item_id) != row["item_revision"]:
            raise Conflict(f"item {item_id} changed since preview")
        tx.expect("items", item_id, row["item_revision"])
    for change in changes:
        tx.put(change["collection"], change["key"], change["after"],
               expected_revision=change["before_rev"])
    tx.put("bulk_jobs", job_id, {"station_id": station_id,
        "idempotency_key": idempotency_key, "fingerprint": fingerprint,
        "status": "completed", "changes": changes,
        "selected": len(summary), "applied": len(summary), "skipped": [],
        "created_by": principal.user_id})
    tx.commit()
    return store.get("bulk_jobs", job_id)


def undo(store: Store, principal: Principal, station_id: str, job_id: str,
         *, expected_job_revision: int) -> dict:
    station_id, job_id = str(UUID(station_id)), str(UUID(job_id))
    sequence = store.sequence
    require(store, principal, "bulk_edit", station_id)
    job = store.get("bulk_jobs", job_id)
    if job is None or job["station_id"] != station_id:
        raise StoreError("bulk job not found in station")
    if job["status"] != "completed":
        raise Conflict("bulk job is not undoable")
    for change in job["changes"]:
        if change["collection"] == "items":
            require_shared_item_permission(store, principal, change["key"], "bulk_edit")
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    for change in job["changes"]:
        current = store.get(change["collection"], change["key"])
        if current is None or current["_rev"] != change["after_rev"] or _body(current) != change["after"]:
            raise Conflict("bulk undo would overwrite a newer edit")
        if change["before"] is None:
            tx.delete(change["collection"], change["key"],
                      expected_revision=change["after_rev"])
        else:
            tx.put(change["collection"], change["key"], change["before"],
                   expected_revision=change["after_rev"])
    job_body = _body(job)
    job_body["status"] = "undone"
    job_body["undone_by"] = principal.user_id
    tx.put("bulk_jobs", job_id, job_body, expected_revision=expected_job_revision)
    tx.commit()
    return store.get("bulk_jobs", job_id)
