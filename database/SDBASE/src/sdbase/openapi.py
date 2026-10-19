"""Generate the public workflow contract from the dispatch allowlist."""

from __future__ import annotations

from datetime import date, datetime
from collections.abc import Iterable, Sequence
import inspect
from pathlib import Path
from types import UnionType
from typing import Any, Literal, Mapping, Union, get_args, get_origin, get_type_hints


def _type_schema(annotation: Any) -> dict:
    if annotation in (inspect.Signature.empty, Any):
        return {}
    origin = get_origin(annotation)
    arguments = get_args(annotation)
    if origin in (Union, UnionType):
        variants = [_type_schema(item) for item in arguments if item is not type(None)]
        if type(None) in arguments:
            variants.append({"type": "null"})
        return {"anyOf": variants}
    if origin is Literal:
        return {"enum": list(arguments)}
    if annotation is str or annotation is Path:
        return {"type": "string"}
    if annotation is bool:
        return {"type": "boolean"}
    if annotation is int:
        return {"type": "integer"}
    if annotation is float:
        return {"type": "number"}
    if annotation is date:
        return {"type": "string", "format": "date"}
    if annotation is datetime:
        return {"type": "string", "format": "date-time"}
    if origin in (list, tuple, set, frozenset) or origin in (Iterable, Sequence):
        return {"type": "array", "items": _type_schema(arguments[0]) if arguments else {}}
    if origin is dict or origin is Mapping or origin is not None and \
            isinstance(origin, type) and issubclass(origin, Mapping):
        return {"type": "object", "additionalProperties":
                _type_schema(arguments[1]) if len(arguments) > 1 else {}}
    return {}


def _request_schema(action: Any) -> dict:
    signature = inspect.signature(action)
    try:
        hints = get_type_hints(action)
    except (NameError, TypeError):
        hints = {}
    properties = {}
    required = []
    allows_extra = False
    for name, parameter in signature.parameters.items():
        if name in {"store", "principal"}:
            continue
        if parameter.kind is inspect.Parameter.VAR_KEYWORD:
            allows_extra = True
            continue
        if parameter.kind is inspect.Parameter.VAR_POSITIONAL:
            continue
        if name.startswith("_"):
            continue
        properties[name] = _type_schema(hints.get(name, parameter.annotation))
        if name.startswith("expected_") and "revision" in name:
            properties[name]["description"] = (
                "Optimistic precondition from the relevant detail row's _rev. "
                "Use plan._rev for expected_plan_revision and entry._rev for "
                "expected_entry_revision; other expected_revision fields refer "
                "to the row being changed. A stale value returns HTTP 409; "
                "reread and reconcile before submitting a new change.")
        if parameter.default is inspect.Parameter.empty:
            required.append(name)
        elif parameter.default is not None and isinstance(parameter.default,
                                                          (str, int, float, bool)):
            properties[name]["default"] = parameter.default
    return {"type": "object", "properties": properties,
            "required": required, "additionalProperties": allows_extra}


def _direct_query_parameters(method: str, route: str) -> list[dict]:
    """Document query fields that the direct HTTP dispatcher actually reads."""
    query_fields = {
        ("GET", "/v1/import-profiles"): (("station_id", "uuid", True),),
        ("GET", "/v1/views"): (("station_id", "uuid", True),),
        ("GET", "/v1/operations/integrity"): (
            ("max_records", "integer", False), ("max_bytes", "integer", False),
            ("verify_media", "boolean", False), ("verify_cold", "boolean", False)),
        ("GET", "/v1/items"): (
            ("station_id", "uuid", True), ("title", "string", False),
            ("artist", "string", False), ("contribution_role", "string", False),
            ("category_id", "uuid", False), ("category", "string", False),
            ("version_label", "string", False), ("kind", "string", False),
            ("min_duration_ms", "integer", False),
            ("max_duration_ms", "integer", False),
            ("endpoint_id", "uuid", False), ("ready", "boolean", False),
            ("slot_start", "date-time", False),
            ("slot_end", "date-time", False),
            ("tolerant", "boolean", False), ("limit", "integer", False),
            ("cursor", "string", False)),
        ("POST", "/v1/versions/{version_id}/media"): (("station_id", "uuid", True),),
        ("GET", "/v1/endpoints/{endpoint_id}/snapshots/{revision_id}"): (
            ("station_id", "uuid", True),),
        ("GET", "/v1/endpoints/{endpoint_id}/media/{media_file_id}"): (
            ("station_id", "uuid", True),),
        ("GET", "/v1/endpoints/{endpoint_id}/commands"): (
            ("station_id", "uuid", True), ("engine_instance", "string", True),
            ("engine_epoch", "integer", True)),
    }
    from .read_api import READ_ROUTES
    if route in READ_ROUTES and method == "GET":
        if route == "/v1/stations/{station_id}/items":
            query_fields[(method, route)] = (("limit", "integer", False),
                                              ("cursor", "string", False))
        elif not route.startswith("/v1/stations/"):
            query_fields[(method, route)] = (("station_id", "uuid", True),)
            if route == "/v1/plans/{plan_id}/entries":
                query_fields[(method, route)] += (("revision_id", "uuid", False),)
    parameters = []
    for name, kind, required in query_fields.get((method, route), ()):
        schema = ({"type": "string", "format": kind}
                  if kind in {"uuid", "date-time"}
                  else {"type": kind})
        parameters.append({"name": name, "in": "query", "required": required,
                           "schema": schema})
    return parameters


def _detail_response_schema(route: str) -> dict:
    """Expose the identity and optimistic revision of every detail row."""
    row = {"type": "object", "required": ["_id", "_rev"],
           "properties": {"_id": {"type": "string", "format": "uuid"},
                          "_rev": {"type": "integer", "minimum": 1,
                                   "description": "Engine revision of this row"}},
           "additionalProperties": True}
    properties = {"sequence": {"type": "integer", "minimum": 0,
                               "description": "Consistent committed sequence for this read"}}
    required = ["sequence"]
    if route.startswith("/v1/stations/") and route.endswith("/items"):
        properties.update({"station_id": {"type": "string", "format": "uuid"},
                           "station_items": {"type": "array", "items": row},
                           "next_cursor": {"type": ["string", "null"]}})
        required += ["station_id", "station_items", "next_cursor"]
    elif route.startswith("/v1/items/") or route.startswith("/v1/versions/") or \
            route.startswith("/v1/stations/"):
        properties.update({"item": row, "station_item": row})
        required += ["item", "station_item"]
        if route.startswith("/v1/versions/"):
            properties["version"] = row
            required.append("version")
        elif route.endswith("/versions"):
            properties["versions"] = {"type": "array", "items": row}
            required.append("versions")
    else:
        properties["plan"] = row
        required.append("plan")
        if route.endswith("/revisions"):
            properties["revisions"] = {"type": "array", "items": row}
            required.append("revisions")
        elif route.endswith("{plan_id}"):
            properties.update({"draft_revision": {"anyOf": [row, {"type": "null"}]},
                               "current_revision": {"anyOf": [row, {"type": "null"}]}})
            required += ["draft_revision", "current_revision"]
        else:
            properties["revision"] = row
            required.append("revision")
            if route.endswith("/entries"):
                properties["entries"] = {"type": "array", "items": row}
                required.append("entries")
            elif route.endswith("{entry_id}"):
                properties["entry"] = row
                required.append("entry")
    return {"type": "object", "required": required, "properties": properties,
            "additionalProperties": False}


def _direct_write_schema(route: str) -> dict | None:
    station = {"type": "string", "format": "uuid"}
    revision = {"type": "integer", "minimum": 1,
                "description": "Relevant detail row _rev; stale revision returns 409. "
                               "Reread and reconcile before resubmission."}
    fields = {
        "/v1/items/{item_id}/eligibility": (
            {"station_id": station, "eligible": {"type": "boolean"},
             "expected_revision": dict(revision, description="station_item._rev optimistic precondition")},
            ["station_id", "eligible", "expected_revision"]),
        "/v1/items/{item_id}/lifecycle": (
            {"station_id": station, "target": {"enum": ["draft", "active", "archived", "withdrawn", "trashed"]},
             "expected_revision": dict(revision, description="item._rev optimistic precondition")},
            ["station_id", "target", "expected_revision"]),
        "/v1/versions/{version_id}/approve": (
            {"station_id": station,
             "expected_revision": dict(revision, description="version._rev optimistic precondition")},
            ["station_id", "expected_revision"]),
        "/v1/plans/{plan_id}/entries": (
            {"station_id": station, "revision_id": station, "entry": {"type": "object"},
             "expected_plan_revision": dict(revision, description="plan._rev optimistic precondition"),
             "entry_id": station,
             "expected_entry_revision": dict(revision, description="entry._rev; required when editing an existing entry")},
            ["station_id", "revision_id", "entry", "expected_plan_revision"]),
        "/v1/plans/{plan_id}/approve": (
            {"station_id": station, "revision_id": station,
             "expected_plan_revision": dict(revision, description="plan._rev optimistic precondition")},
            ["station_id", "revision_id", "expected_plan_revision"]),
    }
    if route not in fields:
        return None
    properties, required = fields[route]
    return {"type": "object", "properties": properties, "required": required,
            "additionalProperties": False}


def openapi_document() -> dict:
    """Return OpenAPI 3.1 for all named workflows plus the core plan route."""
    from . import __version__
    from .workflow_api import ACTIONS
    from .read_api import READ_ROUTES

    paths = {}
    for name, action in sorted(ACTIONS.items()):
        group, operation = name.split("/", 1)
        description = inspect.getdoc(action) or ""
        paths[f"/v1/workflows/{name}"] = {"post": {
            "operationId": f"{group}_{operation.replace('-', '_')}",
            "summary": description.splitlines()[0][:200] if description else name,
            "tags": [group],
            "requestBody": {"required": True, "content": {
                "application/json": {"schema": _request_schema(action)}}},
            "responses": {"200": {"description": "Workflow result"},
                          "403": {"description": "Forbidden"},
                          "409": {"description": "Conflict"},
                          "422": {"description": "Invalid request"},
                          "429": {"description": "Rate limited"}},
            "security": [{"bearerAuth": []}],
        }}
    paths["/v1/plans"] = {"post": {
        "operationId": "create_plan", "tags": ["plans"],
        "summary": "Create a draft plan for one station, date and broadcast service",
        "requestBody": {"required": True, "content": {"application/json": {"schema": {
            "type": "object", "additionalProperties": False,
            "required": ["station_id", "service_date"],
            "properties": {"station_id": {"type": "string", "format": "uuid"},
                           "service_date": {"type": "string", "format": "date"},
                           "service_key": {"type": "string", "default": "default"}}}}}},
        "responses": {"200": {"description": "Created plan"},
                      "409": {"description": "Conflict"},
                      "422": {"description": "Invalid request"},
                      "429": {"description": "Rate limited"}},
        "security": [{"bearerAuth": []}],
    }}
    # These routes have specialized bodies documented in docs/api.md. Their
    # generated contract still records exact methods, paths, media types,
    # authentication and path parameters instead of silently omitting them.
    direct_routes = {
        "GET": [
            "/health", "/v1/capabilities", "/v1/items",
            "/v1/operations/metrics", "/v1/operations/integrity",
            "/v1/import-profiles", "/v1/views",
            "/v1/access/tokens/{token_id}",
            "/v1/endpoints/{endpoint_id}/state",
            "/v1/endpoints/{endpoint_id}/snapshots/{revision_id}",
            "/v1/endpoints/{endpoint_id}/media/{media_file_id}",
            "/v1/endpoints/{endpoint_id}/commands",
            *READ_ROUTES,
        ],
        "POST": [
            "/v1/access/users", "/v1/access/users/{user_id}/tokens",
            "/v1/access/tokens/{token_id}/revoke",
            "/v1/import-profiles", "/v1/import-profiles/{profile_id}",
            "/v1/views", "/v1/imports/rekordbox/preview",
            "/v1/imports/rekordbox/apply", "/v1/items",
            "/v1/items/{item_id}/versions", "/v1/items/{item_id}/share",
            "/v1/items/{item_id}/eligibility", "/v1/items/{item_id}/lifecycle",
            "/v1/versions/{version_id}/approve",
            "/v1/versions/{version_id}/media",
            "/v1/media/{media_file_id}/verify", "/v1/rights",
            "/v1/plans/{plan_id}/entries", "/v1/plans/{plan_id}/approve",
            "/v1/plans/{plan_id}/distribute", "/v1/engine/commands",
            "/v1/playout/events", "/v1/monitors/tokens",
            "/v1/endpoints/{endpoint_id}/tokens", "/v1/observations",
            "/v1/endpoints/{endpoint_id}/media/{media_file_id}/verify",
            "/v1/endpoints/{endpoint_id}/deliveries/{revision_id}/ack",
            "/v1/endpoints/{endpoint_id}/plans/{revision_id}/accept",
            "/v1/endpoints/{endpoint_id}/plans/{revision_id}/heartbeat",
            "/v1/endpoints/{endpoint_id}/commands/{command_id}/ack",
        ],
    }
    for method, routes in direct_routes.items():
        for route in routes:
            parameters = [{"name": part[1:-1], "in": "path", "required": True,
                           "schema": {"type": "string", "format": "uuid"}}
                          for part in route.split("/") if part.startswith("{")]
            operation = {
                "operationId": method.lower() + "_" + route.strip("/").replace("/", "_")
                               .replace("{", "").replace("}", "").replace("-", "_"),
                "summary": method + " " + route,
                "description": "Specialized request and response fields: docs/api.md",
                "parameters": parameters + _direct_query_parameters(method, route),
                "responses": {"200": {"description": "Operation result"},
                              "403": {"description": "Forbidden"},
                              "409": {"description": "Conflict"},
                              "422": {"description": "Invalid request"},
                              "429": {"description": "Rate limited"}},
            }
            if route in READ_ROUTES and method == "GET":
                operation["description"] = (
                    "Requires station view permission and a user credential. "
                    "Outside-station and missing records return 404; denied "
                    "station permissions return 403. Every row includes _id "
                    "and _rev. Multiple rows share one captured sequence; "
                    "later commits do not alter that immutable view. For entries without "
                    "revision_id, the draft is selected before the current "
                    "approved revision. Collections have deterministic order.")
                operation["responses"]["200"]["content"] = {
                    "application/json": {"schema": _detail_response_schema(route)}}
                operation["responses"]["404"] = {"description": "Missing or outside-station resource"}
            if route == "/health":
                operation["description"] = "Readable node identity; does not prove audio output or primary ownership."
                operation["responses"]["200"]["content"] = {"application/json": {"schema": {
                    "type": "object", "required": ["status", "sequence", "format", "store_id",
                        "node_role", "node_id", "timeline_id", "epoch"],
                    "properties": {"status": {"const": "ok"}, "sequence": {"type": "integer", "minimum": 0},
                        "format": {"const": "sdbase-journal-v1"},
                        "store_id": {"type": "string", "format": "uuid"},
                        "node_id": {"type": "string", "format": "uuid"},
                        "timeline_id": {"type": "string", "format": "uuid"},
                        "epoch": {"type": "integer", "minimum": 1},
                        "node_role": {"enum": ["primary", "standby", "fenced"]}}}}}
            else:
                operation["security"] = [{"bearerAuth": []}]
            if method == "POST":
                media_type = ("application/octet-stream"
                              if route == "/v1/versions/{version_id}/media"
                              else "application/json")
                operation["requestBody"] = {"required": True, "content": {
                    media_type: {"schema": ({"type": "string", "format": "binary"}
                                          if media_type == "application/octet-stream"
                                          else _direct_write_schema(route) or {"type": "object"})}}}
            paths.setdefault(route, {})[method.lower()] = operation
    paths["/v1/openapi.json"] = {"get": {
        "operationId": "get_openapi", "summary": "Read the API contract",
        "responses": {"200": {"description": "OpenAPI 3.1 document"}},
        "security": [{"bearerAuth": []}],
    }}
    return {"openapi": "3.1.0",
            "info": {"title": "RadioS SDBASE workflow API", "version": __version__,
                     "description": "Every public HTTP route is listed. Named workflows "
                                    "derive request fields from service signatures; "
                                    "specialized request details are in docs/api.md."},
            "servers": [{"url": "/"}],
            "components": {"securitySchemes": {"bearerAuth": {
                "type": "http", "scheme": "bearer"}}},
            "paths": paths}
