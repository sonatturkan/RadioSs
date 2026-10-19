"""Exact-byte native file adapters for existing reviewed offline workflows.

No client-supplied path is opened on the server. A private exclusive temporary
source carries the supplied bytes through the existing parser and hash-fenced
apply function. Source media paths remain unverified references.
"""
from __future__ import annotations

import base64
import binascii
from contextlib import contextmanager
import json
import os
from pathlib import Path
import tempfile

from . import desktop_api as api
from .storage import StoreError

MAX_SOURCE_BYTES = 1024 * 1024
MAX_RESULT_BYTES = 4 * 1024 * 1024


@contextmanager
def _source(store, source_base64, suffix=".csv"):
    if not isinstance(source_base64, str) or not 1 <= len(source_base64) <= 4 * ((MAX_SOURCE_BYTES + 2) // 3):
        raise StoreError("source file must contain 1..1048576 bytes")
    try:
        raw = base64.b64decode(source_base64.encode("ascii"), validate=True)
    except (UnicodeError, binascii.Error, ValueError) as exc:
        raise StoreError("source file encoding is invalid") from exc
    if not 1 <= len(raw) <= MAX_SOURCE_BYTES:
        raise StoreError("source file must contain 1..1048576 bytes")
    with tempfile.TemporaryDirectory(prefix=".native-transfer-", dir=store.root) as temporary:
        source = Path(temporary) / ("source" + suffix)
        descriptor = os.open(source, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
        with os.fdopen(descriptor, "wb") as output:
            output.write(raw)
            output.flush()
        yield source


def _result(value):
    if len(json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode()) > MAX_RESULT_BYTES:
        raise StoreError("file adapter result exceeds its 4194304-byte review limit")
    return value


def mairlist_preview(store, principal, station_id: str, source_base64: str, source_namespace: str,
                     encoding: str = "utf-8-sig") -> dict:
    """Review mAirList CSV bytes without importing or approving any media."""
    api._station(store, principal, station_id, "import")
    from .mairlist31_csv import preview_mairlist31_csv
    with _source(store, source_base64) as source:
        return _result(preview_mairlist31_csv(source, source_namespace=source_namespace, encoding=encoding, max_rows=1000))


def mairlist_apply(store, principal, station_id: str, source_base64: str, source_namespace: str,
                   expected_source_sha256: str, idempotency_key: str,
                   encoding: str = "utf-8-sig") -> dict:
    """Apply the reviewed exact CSV bytes as unapproved catalogue drafts."""
    api._station(store, principal, station_id, "import")
    from .mairlist31_import import apply_mairlist31_csv
    with _source(store, source_base64) as source:
        return _result(apply_mairlist31_csv(store, principal, station_id, source,
            source_namespace=source_namespace, expected_source_sha256=expected_source_sha256,
            idempotency_key=idempotency_key, encoding=encoding))


def radioboss_preview(store, principal, station_id: str, source_base64: str) -> dict:
    """Review plain RadioBOSS M3U8 bytes and every unsupported source row."""
    api._station(store, principal, station_id, "import")
    from .radioboss_m3u8 import preview_radioboss_m3u8
    with _source(store, source_base64, ".m3u8") as source:
        preview = preview_radioboss_m3u8(source)
        if preview["row_count"] > 1000:
            raise StoreError("native RadioBOSS import is limited to 1000 rows")
        return _result(preview)


def radioboss_apply(store, principal, station_id: str, source_base64: str, source_namespace: str,
                    expected_source_sha256: str, idempotency_key: str, service_date: str,
                    start_at: str, path_bindings: dict[str, dict[str, str]],
                    service_key: str = "default", territory: str = "*") -> dict:
    """Build a draft from reviewed bytes and explicitly selected verified media."""
    api._station(store, principal, station_id, "import")
    from .radioboss_m3u8_import import apply_radioboss_m3u8
    with _source(store, source_base64, ".m3u8") as source:
        return _result(apply_radioboss_m3u8(store, principal, station_id, str(source),
            source_namespace=source_namespace, expected_source_sha256=expected_source_sha256,
            idempotency_key=idempotency_key, service_date=service_date, start_at=start_at,
            path_bindings=path_bindings, service_key=service_key, territory=territory))


def mairlist_export(store, principal, station_id: str, items: list[dict]) -> dict:
    """Export selected immutable catalogue identities with explicit destination paths."""
    api._station(store, principal, station_id)
    from .mairlist31_export import export_mairlist31_csv
    api._list(items, "mAirList export items", 1000)
    rows, paths = [], {}
    for item in items:
        api._object(item, {"item_id", "version_id", "media_file_id", "expected_item_revision", "path"},
                    {"item_id", "version_id", "media_file_id", "expected_item_revision", "path"})
        identifier = api._id(item["media_file_id"], "media_file_id")
        api._text(item["path"], "export destination media path", 8192, True)
        if identifier in paths and paths[identifier] != item["path"]:
            raise StoreError("one media file cannot have conflicting export paths")
        paths[identifier] = item["path"]
        rows.append({"id":api._id(item["item_id"], "item_id"), "station_id":station_id,
                     "revision":api._int(item["expected_item_revision"], "expected_item_revision", 1, 2**63-1),
                     "version_id":api._id(item["version_id"], "version_id"), "media_file_id":identifier})
    result = export_mairlist31_csv(store, principal, station_id, rows, paths)
    result.update(content=result.pop("csv_text"), filename="radios-mairlist.csv", content_type="text/csv; charset=utf-8")
    return _result(result)


def radioboss_export(store, principal, station_id: str, plan_id: str, revision_id: str,
                     paths: list[dict]) -> dict:
    """Export a current approved plan and report every skipped or lossy field."""
    api._station(store, principal, station_id)
    from .radioboss_m3u8_export import export_radioboss_m3u8
    api._list(paths, "RadioBOSS export paths", 1000)
    path_map = {}
    for item in paths:
        api._object(item, {"media_file_id", "path"}, {"media_file_id", "path"})
        identifier = api._id(item["media_file_id"], "media_file_id")
        api._text(item["path"], "export destination media path", 8192, True)
        if identifier in path_map and path_map[identifier] != item["path"]:
            raise StoreError("one media file cannot have conflicting export paths")
        path_map[identifier] = item["path"]
    result = export_radioboss_m3u8(store, principal, station_id, plan_id, revision_id, path_map)
    result.update(content=result.pop("m3u8_text"), filename="radios-radioboss.m3u8", content_type="audio/x-mpegurl; charset=utf-8")
    return _result(result)
