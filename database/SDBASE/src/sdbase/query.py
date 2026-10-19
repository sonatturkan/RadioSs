"""Bounded document query planning over immutable SDBASE read snapshots.

Exact hash indexes supply candidates; compatible index key prefixes/ranges
use lazily sorted snapshot keys. Candidate sets are intersected from smallest
to largest. This module parses no SQL and stores no external database format.
"""

from __future__ import annotations

import base64
import binascii
from bisect import bisect_left, bisect_right
from dataclasses import dataclass
import hashlib
import json
import math
import re
from typing import TYPE_CHECKING

from .storage import Conflict, StoreError
from .disk_index import DiskIndexUnavailable, DiskKeyWindow, ordered_disk_keys

if TYPE_CHECKING:
    from .read_snapshot import ReadSnapshot


class QueryBudgetExceeded(StoreError):
    """The query needs more work than its caller explicitly allowed."""


_LOW = (-1, 0)
_HIGH = (4, 0)


def _scalar(value):
    if value is None or isinstance(value, (str, bool, int)):
        return value
    if isinstance(value, float) and math.isfinite(value):
        return value
    raise StoreError("query values must be finite JSON scalars")


def _atom(value):
    if value is None:
        return (0, 0)
    if isinstance(value, (bool, int, float)):
        return (1, value)
    if isinstance(value, str):
        return (2, value)
    raise StoreError("query ordering requires scalar fields")


def _field(name):
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,127}", name):
        raise StoreError("query fields must be simple document field names")
    return name


def _normalize(equal, ranges, order_by, descending, limit, max_scan, max_index_keys, allow_scan, max_candidates):
    if equal is not None and not isinstance(equal, dict):
        raise StoreError("equal must be a field/value object")
    equal = {_field(key): _scalar(value) for key, value in (equal or {}).items()}
    if ranges is not None and not isinstance(ranges, dict):
        raise StoreError("ranges must be a field/bounds object")
    clean_ranges = {}
    for field, bound in (ranges or {}).items():
        _field(field)
        if not isinstance(bound, dict) or set(bound) - {"minimum", "maximum", "include_minimum", "include_maximum"}:
            raise StoreError("range bounds use minimum/maximum and inclusive flags")
        if not ("minimum" in bound or "maximum" in bound):
            raise StoreError("a range needs at least one bound")
        clean = {key: _scalar(bound[key]) for key in ("minimum", "maximum") if key in bound}
        if any(value is None or isinstance(value, bool) for value in clean.values()):
            raise StoreError("range bounds must be numbers or strings")
        if len(clean) == 2 and (type(clean["minimum"]) is str) != (type(clean["maximum"]) is str):
            raise StoreError("range bounds must have compatible types")
        for flag in ("include_minimum", "include_maximum"):
            clean[flag] = bound.get(flag, True)
            if type(clean[flag]) is not bool:
                raise StoreError("range inclusive flags must be booleans")
        if len(clean) >= 4 and _atom(clean["minimum"]) > _atom(clean["maximum"]):
            raise StoreError("range maximum precedes minimum")
        clean_ranges[field] = clean
    if isinstance(order_by, str):
        order_by = (order_by,)
    if not isinstance(order_by, (tuple, list)) or len(order_by) > 8:
        raise StoreError("order_by must contain at most eight field names")
    order_by = tuple(_field(field) for field in order_by)
    if len(set(order_by)) != len(order_by):
        raise StoreError("order_by cannot repeat fields")
    if not order_by:
        order_by = ("_id",)
    if type(descending) is not bool or type(allow_scan) is not bool:
        raise StoreError("descending and allow_scan must be booleans")
    for name, value, upper in (("limit", limit, 1000), ("max_scan", max_scan, 1_000_000),
                               ("max_index_keys", max_index_keys, 1_000_000),
                               ("max_candidates", max_candidates, 1_000_000)):
        if type(value) is not int or not 1 <= value <= upper:
            raise StoreError(f"{name} must be an integer from 1 to {upper}")
    return equal, clean_ranges, order_by


def _ordered_keys(view, collection, index, max_index_keys, *, force_memory=False):
    if not force_memory:
        reader = ordered_disk_keys(view, collection, index)
        if reader is not None:
            return reader, 0
    buckets = view._indices.get((collection, index), {})
    if len(buckets) > max_index_keys:
        raise QueryBudgetExceeded(f"index {index} has {len(buckets)} keys; max_index_keys={max_index_keys}")
    with view._cache_lock:
        cached = view._ordered_indices.get((collection, index))
        built = cached is None
        if built:
            cached = sorted((tuple(_atom(part) for part in key), key) for key in buckets)
            view._ordered_indices[(collection, index)] = cached
        return cached, len(buckets) if built else 0


def _window(view, collection, name, prefix, bound, max_index_keys, *, force_memory=False):
    spec = view.index_specs[collection][name]
    ordered, built = _ordered_keys(view, collection, name, max_index_keys, force_memory=force_memory)
    normalized = tuple(_atom(part) for part in prefix)
    remaining = len(spec.fields) - len(prefix)
    lower = normalized + (_LOW,) * remaining
    upper = normalized + (_HIGH,) * remaining
    lower_right = upper_right = False
    if bound is not None:
        tail = remaining - 1
        if "minimum" in bound:
            inclusive = bound["include_minimum"]
            lower = normalized + (_atom(bound["minimum"]),) + ((_LOW if inclusive else _HIGH),) * tail
            lower_right = not inclusive
        if "maximum" in bound:
            inclusive = bound["include_maximum"]
            upper = normalized + (_atom(bound["maximum"]),) + ((_HIGH if inclusive else _LOW),) * tail
            upper_right = inclusive
    # The auxiliary raw key is never part of comparison; indexed JSON scalars
    # can have heterogeneous types while normalized keys have a total order.
    normalized_keys = _KeyProjection(ordered)
    start = (bisect_right if lower_right else bisect_left)(normalized_keys, lower)
    stop = (bisect_right if upper_right else bisect_left)(normalized_keys, upper)
    return ordered[start:stop], built


class _KeyProjection:
    def __init__(self, rows):
        self.rows = rows

    def __len__(self):
        return len(self.rows)

    def __getitem__(self, index):
        return self.rows[index][0]


@dataclass
class _Plan:
    candidates: set[str] | None
    paths: list[dict]
    ordering_index: str | None
    ordering_keys: list | None
    collection_rows: int
    keys_built: int


def _plan(view, collection, equal, ranges, order_by, max_index_keys, allow_scan, max_candidates):
    view._check(collection)
    paths = []
    sets = []
    if "_id" in equal:
        key = str(equal["_id"])
        ids = {key} if key in view._records.get(collection, {}) else set()
        paths.append({"index": "primary_id", "access": "exact", "candidate_rows": len(ids), "index_keys_examined": 1})
        sets.append(ids)
    ordering_index = None
    ordering_keys = None
    keys_built = 0
    skipped = []
    for name, spec in view.index_specs.get(collection, {}).items():
        prefix = []
        for field in spec.fields:
            if field not in equal or equal[field] is None:
                break
            prefix.append(equal[field])
        if len(prefix) == len(spec.fields):
            members = view._indices.get((collection, name), {}).get(tuple(prefix), ())
            if len(members) > max_candidates:
                skipped.append({"index": name, "access": "skipped", "reason": "candidate budget",
                                "candidate_rows": len(members)})
                continue
            ids = set(members)
            paths.append({"index": name, "access": "exact", "candidate_rows": len(ids), "index_keys_examined": 1})
            sets.append(ids)
            continue
        next_field = spec.fields[len(prefix)]
        bound = ranges.get(next_field)
        compatible_order = tuple(spec.fields[len(prefix):]) == tuple(
            field for field in order_by if field != "_id") and "_id" not in order_by[:-1]
        if not prefix and bound is None:
            continue
        # Hash indexes omit a row when any indexed field is null/missing. A
        # composite prefix alone therefore is not a complete candidate set.
        # It is safe when every omitted suffix would also fail the query, or
        # when actual snapshot membership covers every row in the collection.
        suffix = spec.fields[len(prefix):]
        suffix_constrained = all(field in ranges or field in equal and equal[field] is not None
                                 for field in suffix)
        buckets = view._indices.get((collection, name), {})
        if not suffix_constrained and sum(len(ids) for ids in buckets.values()) != view.count(collection):
            skipped.append({"index": name, "access": "skipped",
                            "reason": "index omits rows with unconstrained null fields"})
            continue
        previous_reader = getattr(view, "_disk_readers", {}).get((collection, name))
        disk_start = dict(previous_reader.work) if previous_reader is not None else {}
        try:
            try:
                keys, built = _window(view, collection, name, prefix, bound, max_index_keys)
                if isinstance(keys, DiskKeyWindow):
                    if len(keys) > max_index_keys:
                        raise QueryBudgetExceeded("disk selected index-record budget")
                    if len(keys) > max_candidates:
                        raise QueryBudgetExceeded("candidate budget")
                    ids = {identity for _normalized, _raw, identity in keys.records()}
                else:
                    if sum(len(buckets[key]) for _, key in keys) > max_candidates:
                        raise QueryBudgetExceeded("candidate budget")
                    ids = set().union(*(buckets[key] for _, key in keys)) if keys else set()
            except DiskIndexUnavailable as exc:
                _disable_disk(view, (collection, name), str(exc))
                keys, built = _window(view, collection, name, prefix, bound, max_index_keys, force_memory=True)
                if sum(len(buckets[key]) for _, key in keys) > max_candidates:
                    raise QueryBudgetExceeded("candidate budget")
                ids = set().union(*(buckets[key] for _, key in keys)) if keys else set()
        except QueryBudgetExceeded as exc:
            detail = str(exc)
            reason = "candidate budget" if "candidate budget" in detail else (
                "disk selected index-record budget" if "disk selected index-record budget" in detail else "index-key budget")
            skipped.append({"index": name, "access": "skipped", "reason": reason})
            continue
        keys_built += built
        path = {"index": name, "access": "range" if bound else "prefix",
                "candidate_rows": len(ids), "index_keys_examined": len(keys), "ordered_keys_built": built,
                "backend": "disk" if isinstance(keys, DiskKeyWindow) else "memory"}
        if isinstance(keys, DiskKeyWindow):
            path["_disk_reader"] = keys.source
            path["_disk_start"] = disk_start
        elif (collection, name) in getattr(view, "_disk_fallbacks", {}):
            path["disk_fallback"] = view._disk_fallbacks[(collection, name)]
        paths.append(path)
        sets.append(ids)
        if compatible_order:
            ordering_index, ordering_keys = name, keys
    if sets:
        sets.sort(key=len)
        candidates = sets[0]
        for other in sets[1:]:
            candidates.intersection_update(other)
    else:
        if not allow_scan:
            detail = "; ".join(f"{path['index']}: {path['reason']}" for path in skipped)
            raise QueryBudgetExceeded("no usable bounded index path; tighten filters, raise the explicit budget or add an index" +
                                      (f" ({detail})" if detail else ""))
        if view.count(collection) > max_candidates:
            raise QueryBudgetExceeded("collection scan exceeds max_candidates; tighten filters or raise the explicit budget")
        candidates = None
    paths.extend(skipped)
    return _Plan(candidates, paths, ordering_index, ordering_keys, view.count(collection), keys_built)


def _matches(row, equal, ranges):
    if any(row.get(field) != value for field, value in equal.items()):
        return False
    for field, bound in ranges.items():
        value = row.get(field)
        if value is None or isinstance(value, bool) or not isinstance(value, (str, int, float)):
            return False
        for key, flag, compare in (("minimum", "include_minimum", -1), ("maximum", "include_maximum", 1)):
            if key not in bound:
                continue
            endpoint = bound[key]
            if (isinstance(value, str)) != (isinstance(endpoint, str)):
                return False
            relation = -1 if value < endpoint else (1 if value > endpoint else 0)
            if relation == compare or relation == 0 and not bound[flag]:
                return False
    return True


def _order_key(row, order_by):
    values = tuple(_atom(row.get(field)) for field in order_by)
    return values + ((_atom(row["_id"]),) if "_id" not in order_by else ())


def _claim_digest(collection, equal, ranges, order_by, descending):
    value = {"collection": collection, "equal": equal, "ranges": ranges,
             "order_by": order_by, "descending": descending}
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    allow_nan=False).encode()).hexdigest()


def _decode(cursor, view, collection, digest, order_by):
    try:
        if not isinstance(cursor, str) or not 1 <= len(cursor) <= 8192:
            raise ValueError
        claim = json.loads(base64.b64decode(cursor + "=" * (-len(cursor) % 4), altchars=b"-_", validate=True))
        if not isinstance(claim, dict) or claim.get("v") != 1 or claim.get("query") != digest:
            raise ValueError
        if claim.get("store_id") != view.store_id:
            raise ValueError
        if type(claim.get("generation")) is not int:
            raise ValueError
        if claim["generation"] != view.collection_generation((collection,)):
            raise Conflict("query cursor expired because collection changed")
        values = claim["after"]
        if not isinstance(values, list) or len(values) != len(order_by) + ("_id" not in order_by):
            raise ValueError
        return tuple(_atom(_scalar(value)) for value in values)
    except (ValueError, TypeError, KeyError, UnicodeError, binascii.Error) as exc:
        raise StoreError("invalid or mismatched query cursor") from exc


def _encode(view, collection, digest, row, order_by):
    values = [row.get(field) for field in order_by]
    if "_id" not in order_by:
        values.append(row["_id"])
    claim = {"v": 1, "store_id": view.store_id, "query": digest,
             "generation": view.collection_generation((collection,)), "after": values}
    return base64.urlsafe_b64encode(json.dumps(claim, sort_keys=True, separators=(",", ":"),
                                             allow_nan=False).encode()).rstrip(b"=").decode()


def _disable_disk(view, identity, detail):
    if not hasattr(view, "_disk_disabled"):
        view._disk_disabled = set()
    view._disk_disabled.add(identity)
    view._disk_fallbacks[identity] = detail
    reader = view._disk_readers.pop(identity, None)
    if reader is not None:
        reader.close()


def _description(plan, view, collection, max_scan, max_index_keys, max_candidates):
    paths = []
    for path in plan.paths:
        copied = {key: value for key, value in path.items() if not key.startswith("_")}
        if "_disk_reader" in path:
            copied["disk_work"] = {key: value - path["_disk_start"].get(key, 0)
                                   for key, value in path["_disk_reader"].work.items()}
        paths.append(copied)
    return {"sequence": view.sequence, "collection_generation": view.collection_generation((collection,)),
            "collection": collection, "collection_rows": plan.collection_rows,
            "candidate_rows": len(plan.candidates) if plan.candidates is not None else plan.collection_rows,
            "access": "index_intersection" if len([p for p in plan.paths if p["access"] != "skipped"]) > 1 else
                      ("index" if plan.candidates is not None else "collection_scan"),
            "paths": paths, "ordering": "index" if plan.ordering_index else "primary_id_or_bounded_sort",
            "ordering_index": plan.ordering_index, "ordered_keys_built": plan.keys_built,
            "max_scan": max_scan, "max_index_keys": max_index_keys, "max_candidates": max_candidates}


def explain_query(view: ReadSnapshot, collection: str, *, equal=None, ranges=None,
                  order_by=(), descending=False, limit=100, max_scan=10000,
                  max_index_keys=100000, allow_scan=False, cursor=None, max_candidates=100000) -> dict:
    equal, ranges, order_by = _normalize(equal, ranges, order_by, descending, limit,
                                         max_scan, max_index_keys, allow_scan, max_candidates)
    if cursor is not None:
        _decode(cursor, view, collection, _claim_digest(collection, equal, ranges, order_by, descending), order_by)
    plan = _plan(view, collection, equal, ranges, order_by, max_index_keys, allow_scan, max_candidates)
    return _description(plan, view, collection, max_scan, max_index_keys, max_candidates)


def _query_page(view: ReadSnapshot, collection: str, *, equal=None, ranges=None,
               order_by=(), descending=False, limit=100, cursor=None,
               max_scan=10000, max_index_keys=100000, allow_scan=False, max_candidates=100000) -> dict:
    """Return filtered records, a generation-bound keyset cursor and actual work.

    ``max_scan`` bounds fetched documents. Building an ordered key cache is
    separately bounded by ``max_index_keys``. Capture/index-set memory cost is
    reported in the snapshot documentation, outside these query work budgets.
    """
    equal, ranges, order_by = _normalize(equal, ranges, order_by, descending, limit,
                                         max_scan, max_index_keys, allow_scan, max_candidates)
    digest = _claim_digest(collection, equal, ranges, order_by, descending)
    after = _decode(cursor, view, collection, digest, order_by) if cursor else None
    plan = _plan(view, collection, equal, ranges, order_by, max_index_keys, allow_scan, max_candidates)
    candidates = plan.candidates if plan.candidates is not None else set(view._records.get(collection, {}))
    ordered_path = order_by == ("_id",) or plan.ordering_index is not None
    if order_by == ("_id",):
        ids = sorted(candidates, reverse=descending)
    elif plan.ordering_index:
        buckets = view._indices[(collection, plan.ordering_index)]
        keys = reversed(plan.ordering_keys) if descending else plan.ordering_keys
        prefix_count = len(view.index_specs[collection][plan.ordering_index].fields) - len(
            tuple(field for field in order_by if field != "_id"))
        ids = (key for _, index_key in keys for key in sorted(buckets[index_key], reverse=descending)
               if key in candidates and (after is None or
                  ((tuple(_atom(value) for value in index_key[prefix_count:]) + (_atom(key),)) < after
                   if descending else
                   (tuple(_atom(value) for value in index_key[prefix_count:]) + (_atom(key),)) > after)))
    else:
        ids = iter(candidates)
    rows = []
    examined = 0
    for key in ids:
        # Skip cursor prefixes before fetching documents on the primary path.
        if after is not None and order_by == ("_id",):
            normalized_id = (_atom(key),)
            if (normalized_id >= after if descending else normalized_id <= after):
                continue
        if examined >= max_scan:
            raise QueryBudgetExceeded(f"query exceeded max_scan={max_scan}; tighten filters or raise the explicit budget")
        examined += 1
        row = view.get(collection, key)
        if not _matches(row, equal, ranges):
            continue
        sort_key = _order_key(row, order_by)
        if after is not None and (sort_key >= after if descending else sort_key <= after):
            continue
        rows.append(row)
        if ordered_path and len(rows) > limit:
            break
    if not ordered_path:
        rows.sort(key=lambda row: _order_key(row, order_by), reverse=descending)
    has_more = len(rows) > limit
    selected = rows[:limit]
    explanation = _description(plan, view, collection, max_scan, max_index_keys, max_candidates)
    explanation.update({"rows_examined": examined, "rows_returned": len(selected)})
    return {"rows": selected, "next_cursor": _encode(view, collection, digest, selected[-1], order_by)
            if has_more and selected else None, "sequence": view.sequence, "explain": explanation}


def query_page(view: ReadSnapshot, collection: str, *, equal=None, ranges=None,
               order_by=(), descending=False, limit=100, cursor=None,
               max_scan=10000, max_index_keys=100000, allow_scan=False, max_candidates=100000) -> dict:
    """Query one immutable view; damaged optional disk caches fall back to RAM."""
    arguments = dict(equal=equal, ranges=ranges, order_by=order_by, descending=descending,
                     limit=limit, cursor=cursor, max_scan=max_scan,
                     max_index_keys=max_index_keys, allow_scan=allow_scan, max_candidates=max_candidates)
    try:
        return _query_page(view, collection, **arguments)
    except DiskIndexUnavailable as exc:
        for identity in tuple(getattr(view, "_disk_readers", {})):
            _disable_disk(view, identity, str(exc))
        return _query_page(view, collection, **arguments)
