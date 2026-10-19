#!/usr/bin/env python3
"""Kılavuz 2026-10-18 R10.1: which data-service operations the native GUI reaches.

Every workflow action (workflow_api.ACTIONS) and HTTP route (http_server.py,
read_api.py) is accounted for in one of these ways:

  gui      a GUI source calls it: the exact path literal, a printf/concat
           pattern of it, or a family builder "/v1/workflows/<family>/%s"
           (or ".../<family>/") with the operation literal in the same file;
  hub      Teknik yönetim › İş akışları generates its form from the catalogue
           (desktop_workflows.FAMILIES);
  same     the route runs the same backend function as a gui/hub action;
  not-gui  listed in NOT_GUI below with the reason it is no operator function.

Anything else is a gap. Prints a Markdown report; exits 1 when gaps remain.

usage: gui_backend_coverage.py [--root DIR] [--gaps]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BACKEND = Path("database/SDBASE/src/sdbase")

# Operations that are not operator functions, or whose function the GUI
# already performs through another path. Keys are workflow ids or
# "METHOD /route" with {id} for identifiers.
NOT_GUI = {
    "desktop/campaign-delete":
        "Aynı işlev GUI'de: Reklam › Sil, desktop-advert/campaign-state (cancel); bu eski yol yalnız taslak/onaylı kampanyayı iptal eder.",
    "desktop/campaign-get":
        "GUI aynı kampanyayı desktop-advert/campaign-get ile (spotlar, çıkışlar ve ses bağıyla) okur.",
    "desktop/user-create":
        "Eski yol yanıtta düz metin erişim anahtarı döndürür; GUI (Teknik › Kişi ata) aynı işlevi anahtarı yanıtta taşımayan desktop-admin/user-create ile yapar.",
    "desktop-broadcast/profile-save":
        "Aynı işlev GUI'de: yayın/kayıt/meta profilleri desktop/put belge kaydıyla saklanır; profile_save yalnız api.put_document sarmalayıcısıdır.",
    "POST /v1/versions/{id}/media": "REST medya yükleme hattının 1. adımı; GUI dosyayı desktop-media/import ile SHA doğrulamalı tek adımda alır.",
    "POST /v1/media/{id}/verify": "REST yükleme hattının 2. adımı (yüklenen baytları yeniden özetleyip onaylar); GUI içe aktarması bu doğrulamayı içe aktarırken yapar.",
    "GET /v1/openapi.json": "API tanımı; istemci geliştiriciler içindir.",
    "POST /v1/engine/commands": "Yayın motoruna makine komutu; motor ve uç noktalar kendi belirteçleriyle kullanır.",
    "POST /v1/playout/events": "Yayın motorunun çalma olayı bildirimi (makine).",
    "POST /v1/observations": "Uç nokta çıkış gözlemi (makine).",
    "POST /v1/monitors/tokens": "İzleme aracına belirteç; harici izleme yazılımı içindir.",
    "POST /v1/access/users": "Eski REST kullanıcı oluşturma; GUI Teknik › Kişi ata ile desktop-admin/user-create kullanır.",
    "POST /v1/access/tokens/{id}/revoke": "Eski REST belirteç iptali; GUI Yönetim'de desktop-admin/token-revoke kullanır.",
    "GET /v1/access/tokens/{id}": "Tek belirtecin REST okuması; GUI desktop-admin/overview ile listeler.",
}
# Routes whose operation the GUI performs through another path: the
# equivalent workflow, or (reads) the collection the workflow hub's record
# list shows (desktop-workflows/records).
SAME = {
    "POST /v1/plans/{id}/distribute": "plans/distribute",
    "GET /v1/items": "desktop-media/list",
}
READS = {
    "/v1/items/{id}": "items", "/v1/items/{id}/versions": "versions", "/v1/versions/{id}": "versions",
    "/v1/plans/{id}": "plans", "/v1/plans/{id}/entries": "plan_entries", "/v1/plans/{id}/entries/{id}": "plan_entries",
    "/v1/plans/{id}/revisions": "plan_revisions", "/v1/plans/{id}/revisions/{id}": "plan_revisions",
    "/v1/plans/{id}/revisions/{id}/entries": "plan_entries",
    "/v1/stations/{id}/items": "station_items", "/v1/stations/{id}/items/{id}": "station_items",
}
ENDPOINT_PREFIX = "/v1/endpoints/"  # device-scoped machine routes, endpoint credentials only

LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
# Transport policy tables in data_client.c name paths without calling them.
POLICY_TABLES = re.compile(r"^(?:static\s+[\w\s*]+?)?\b(rate_retry_read|response_limit|playout_preparation_path)\(")


def literals(path: Path):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    skip = False
    for number, line in enumerate(lines, 1):
        if path.name == "data_client.c" and POLICY_TABLES.match(line) and not line.rstrip().endswith(";"):
            skip = True
        if skip:
            if line.startswith("}"):
                skip = False
            continue
        for match in LITERAL.finditer(line):
            yield number, match.group(1)


def workflow_actions(root: Path) -> dict[str, str]:
    text = (root / BACKEND / "workflow_api.py").read_text(encoding="utf-8")
    block = text[text.index("ACTIONS: dict[str, Action] = {"):]
    block = block[:block.index("\n}\n")]
    return {m.group(1): m.group(2) for m in
            re.finditer(r'^\s*"([a-z0-9-]+/[a-z0-9-]+)"\s*:\s*([A-Za-z_][\w.]*)', block, re.M)}


def hub_families(root: Path) -> set[str]:
    text = (root / BACKEND / "desktop_workflows.py").read_text(encoding="utf-8")
    block = text[text.index("FAMILIES = {"):]
    block = block[:block.index("}")]
    return set(re.findall(r'"([a-z0-9-]+)"\s*:', block))


def http_routes(root: Path) -> list[tuple[str, str, str]]:
    """(method, route with {id}, called backend function)."""
    routes = []
    lines = (root / BACKEND / "http_server.py").read_text(encoding="utf-8").splitlines()

    def called(start):
        for line in lines[start:start + 8]:
            found = re.search(r"return\s+(?:self\.)?([A-Za-z_]\w*)\.([A-Za-z_]\w*)\(", line)
            if found:
                return found.group(1) + "." + found.group(2)
            found = re.search(r"return\s+(?:self\.)?(_[A-Za-z_]\w*)\(", line)
            if found:
                return found.group(1)
        return ""

    pattern = None
    for index, line in enumerate(lines):
        full = re.search(r're\.fullmatch\(rf?"([^"]+)",\s*path\)', line)
        if full:
            pattern = full.group(1)
            continue
        method = re.search(r'method == "(GET|POST|PUT|DELETE)"', line)
        if not method:
            continue
        if pattern and "and match" in line:
            routes.append((method.group(1), pattern, called(index)))
            pattern = None
            continue
        paths = re.findall(r'path == "([^"]+)"', line)
        if "path in {" in line:
            chunk = "\n".join(lines[index:index + 4])
            paths += re.findall(r'"(/[^"]+)"', chunk[:chunk.index("}") + 1])
        for path in paths:
            routes.append((method.group(1), path, called(index)))
    text = (root / BACKEND / "read_api.py").read_text(encoding="utf-8")
    for route, function in re.findall(r'\(rf"(/v1/[^"]+)",\s*([a-z_]+)\)', text):
        routes.append(("GET", route, "read_api." + function))
    if "/v1/stations/{uuid_pattern}/items" in text:
        routes.append(("GET", "/v1/stations/{id}/items", "read_api.list_station_items"))
        routes.append(("GET", "/v1/stations/{id}/items/{id}", "read_api.list_station_items"))
    normal = []
    for method, route, function in routes:
        route = re.sub(r"\(\{_UUID\}\)|\{uuid_pattern\}", "{id}", route)
        if route.startswith("/v1/workflows/") or "(?:" in route:
            continue
        normal.append((method, route, function))
    return sorted(set(normal))


def record_collections(root: Path) -> set[str]:
    """Collections the workflow hub's record list (desktop-workflows/records) reads."""
    sys.path.insert(0, str(root / BACKEND.parent))
    try:
        from sdbase import desktop_workflows
        return set(desktop_workflows.COLLECTIONS)
    except Exception:  # an unimportable backend leaves the reads unaccounted
        return set()
    finally:
        sys.path.pop(0)


def gui_references(root: Path, actions: dict[str, str]):
    """workflow id or route -> first 'file:line' calling it."""
    files = sorted((root / "src/ui").glob("*.c")) + sorted((root / "src/core").glob("*.c")) + \
        sorted((root / "src").glob("*.c"))
    files = [f for f in files if f.name != "recovery.c"]
    found: dict[str, str] = {}
    ops_by_family: dict[str, set[str]] = {}
    for action in actions:
        family, op = action.split("/", 1)
        ops_by_family.setdefault(family, set()).add(op)
    for path in files:
        where = path.relative_to(root).as_posix()
        items = list(literals(path))
        builders = set()
        for number, text in items:
            exact = re.fullmatch(r"/v1/workflows/([a-z0-9-]+)/([a-z0-9-]+)", text)
            if exact:
                found.setdefault(exact.group(1) + "/" + exact.group(2), f"{where}:{number}")
            family = re.fullmatch(r"/v1/workflows/([a-z0-9-]+)/(?:%s)?", text)
            if family:
                builders.add(family.group(1))
            if text.startswith("/v1/") and not text.startswith("/v1/workflows/") or text == "/health":
                route = re.sub(r"%[sdu]|%\" G_[A-Z0-9_]+ \"", "{id}", text.split("?", 1)[0])
                found.setdefault("ROUTE " + route, f"{where}:{number}")
                if route.endswith("/"):  # g_strconcat(prefix, id, NULL)
                    found.setdefault("ROUTE " + route + "{id}", f"{where}:{number}")
        for family in builders:
            for number, text in items:
                if text in ops_by_family.get(family, ()):
                    found.setdefault(family + "/" + text, f"{where}:{number}")
    return found


def audit(root: Path):
    actions = workflow_actions(root)
    hub = hub_families(root)
    refs = gui_references(root, actions)
    rows = []
    reached_functions = {}
    for action, function in sorted(actions.items()):
        family = action.split("/", 1)[0]
        if action in refs:
            rows.append(("workflow", action, "gui", refs[action]))
        elif family in hub:
            rows.append(("workflow", action, "hub", "Teknik yönetim › İş akışları"))
        elif action in NOT_GUI:
            rows.append(("workflow", action, "not-gui", NOT_GUI[action]))
        else:
            rows.append(("workflow", action, "gap", ""))
        if rows[-1][2] in {"gui", "hub"}:
            reached_functions.setdefault(function.split(".")[-1], action)
    collections = record_collections(root)
    states = {name: state for _, name, state, _ in rows}
    for method, route, function in http_routes(root):
        key = f"{method} {route}"
        name = function.split(".")[-1]
        if "ROUTE " + route in refs:
            rows.append(("http", key, "gui", refs["ROUTE " + route]))
        elif key in SAME and states.get(SAME[key]) in {"gui", "hub"}:
            rows.append(("http", key, "same", f"{function} = {SAME[key]} ({states[SAME[key]]})"))
        elif method == "GET" and READS.get(route) in collections:
            rows.append(("http", key, "same", f"Teknik yönetim › İş akışları kayıt listesi: desktop-workflows/records · {READS[route]}"))
        elif key in NOT_GUI:
            rows.append(("http", key, "not-gui", NOT_GUI[key]))
        elif route.startswith(ENDPOINT_PREFIX):
            rows.append(("http", key, "not-gui", "Uç nokta cihazının kendi belirteciyle kullandığı makine yolu."))
        elif name and name in reached_functions:
            rows.append(("http", key, "same", f"{function} = {reached_functions[name]}"))
        else:
            rows.append(("http", key, "gap", function))
    return rows


def report(rows) -> str:
    counts = {}
    for kind, _, state, _ in rows:
        counts[(kind, state)] = counts.get((kind, state), 0) + 1
    workflows = sum(v for (k, _), v in counts.items() if k == "workflow")
    routes = sum(v for (k, _), v in counts.items() if k == "http")
    out = [f"İş akışı: {workflows} · HTTP yolu: {routes}", ""]
    for kind in ("workflow", "http"):
        parts = [f"{state} {counts.get((kind, state), 0)}" for state in ("gui", "hub", "same", "not-gui", "gap")]
        out.append(f"- {kind}: " + " · ".join(parts))
    out += ["", "| Tür | İşlem | Durum | Kanıt / gerekçe |", "|---|---|---|---|"]
    for kind, name, state, evidence in rows:
        out.append(f"| {kind} | `{name}` | {state} | {evidence.replace('|', '/')} |")
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--gaps", action="store_true", help="print only the gaps")
    args = parser.parse_args()
    rows = audit(args.root)
    gaps = [row for row in rows if row[2] == "gap"]
    if args.gaps:
        for kind, name, _, function in gaps:
            print(f"{kind}\t{name}\t{function}")
    else:
        sys.stdout.write(report(rows))
    return 1 if gaps else 0


if __name__ == "__main__":
    raise SystemExit(main())
