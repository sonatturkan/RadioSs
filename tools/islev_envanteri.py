#!/usr/bin/env python3
"""Tasarım kontrol envanteri ve mevcut SDBASE API sözleşmesini üretir.

Bu araç C ile geliştirilecek RadioS uygulamasının geliştirme yardımcısıdır.
HTML yalnız görsel referanstır; kaynakları değiştirmez, veritabanı açmaz,
sunucu başlatmaz ve demo verilerini içe aktarmaz. Kaynaklar RadioS kökü
altındaki design/reference/tasarrım ve database/SDBASE dizinlerinden okunur.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
from html.parser import HTMLParser
import importlib
import inspect
import json
from pathlib import Path
import sys


PROJECT = Path(__file__).resolve().parents[1]
PROFILES = {
    "live-task-view": "broadcast",
    "playlist-task-view": "playlist",
    "automation-task-view": "automation",
    "dj-task-view": "dj",
    "emergency-task-view": "emergency",
    "advert-task-view": "advert",
    "technical-task-view": "technical",
}
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr"}
CONTROL_TAGS = {"button", "input", "select", "textarea", "summary", "a", "canvas"}
CONTROL_ROLES = {"button", "tab", "slider", "spinbutton", "meter", "switch"}


def compact(value: str) -> str:
    return " ".join(value.split())


class DesignParser(HTMLParser):
    def __init__(self, source: str) -> None:
        super().__init__(convert_charrefs=True)
        self.source = source
        self.stack: list[dict] = []
        self.controls: list[dict] = []
        self.labels: dict[str, str] = {}
        self.profiles: list[dict] = []
        self.scripts: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        inherited = self.stack[-1] if self.stack else {}
        profile = PROFILES.get(attributes.get("id"), inherited.get("profile", "shared"))
        context = {"tag": tag, "attrs": attributes, "text": [], "profile": profile,
                   "form": inherited.get("form"), "label": inherited.get("label"),
                   "scope": inherited.get("scope")}
        if tag == "form":
            context["form"] = attributes.get("id") or next(
                (key for key in attributes if key.startswith("data-")),
                f"form@{self.getpos()[0]}")
        if tag == "label":
            context["label"] = context
        if attributes.get("id") and tag in {"section", "dialog", "header", "nav", "footer"}:
            context["scope"] = attributes["id"]
        if "data-task-profile" in attributes:
            self.profiles.append({"key": attributes["data-task-profile"],
                                  "view_id": attributes.get("aria-controls"),
                                  "source": self.source, "line": self.getpos()[0]})
        if tag == "script" and attributes.get("src"):
            self.scripts.append(attributes["src"].split("?", 1)[0])
        is_control = tag in CONTROL_TAGS or attributes.get("role") in CONTROL_ROLES
        if is_control:
            item = {
                "key": f"{self.source}:{self.getpos()[0]}:{len(self.controls) + 1}",
                "source": self.source, "line": self.getpos()[0],
                "profile": profile, "scope": context["scope"], "form": context["form"],
                "tag": tag, "attributes": attributes,
                "disabled_in_design": "disabled" in attributes or attributes.get("aria-disabled") == "true",
                "design_only": True,
                "_context": context,
            }
            self.controls.append(item)
        if tag not in VOID:
            self.stack.append(context)

    def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self.handle_starttag(tag, attrs)
        if tag not in VOID:
            self.handle_endtag(tag)

    def handle_data(self, data: str) -> None:
        if self.stack and self.stack[-1]["tag"] not in {"script", "style"}:
            for context in self.stack:
                context["text"].append(data)

    def handle_endtag(self, tag: str) -> None:
        for index in range(len(self.stack) - 1, -1, -1):
            if self.stack[index]["tag"] == tag:
                for context in self.stack[index:]:
                    if context["tag"] == "label" and context["attrs"].get("for"):
                        self.labels[context["attrs"]["for"]] = compact("".join(context["text"]))
                del self.stack[index:]
                break

    def finish(self) -> None:
        for item in self.controls:
            context = item.pop("_context")
            attrs = item["attributes"]
            label = context.get("label")
            item["label"] = compact(attrs.get("aria-label") or
                                    self.labels.get(attrs.get("id"), "") or
                                    ("".join(label["text"]) if label is not None else "") or
                                    ("".join(context["text"]) if item["tag"] in {"button", "summary", "a"} else "") or
                                    attrs.get("title") or attrs.get("name") or attrs.get("id") or "")


def serialize(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True, allow_nan=False) + "\n"


def build_inventory(source_root: Path) -> tuple[dict, dict]:
    design_root = source_root / "design" / "reference" / "tasarrım"
    backend_root = source_root / "database" / "SDBASE" / "src"
    if not (design_root / "index.html").is_file() or not (backend_root / "sdbase" / "openapi.py").is_file():
        raise ValueError("RadioS kaynak kökünde design/reference/tasarrım/index.html ve database/SDBASE/src/sdbase/openapi.py bulunmalı.")
    pages = []
    parsers = {}
    for path in sorted(design_root.glob("*.html")):
        source = path.relative_to(source_root).as_posix()
        parser = DesignParser(source)
        parser.feed(path.read_text(encoding="utf-8"))
        parser.close()
        parser.finish()
        parsers[path.name] = parser
        pages.append({"source": source, "role": "active_design" if path.name == "index.html" else "design_fragment",
                      "control_count": len(parser.controls), "controls": parser.controls})
    # Read-only import of the backend's own contract generator. No Store instance.
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(backend_root))
    openapi = importlib.import_module("sdbase.openapi").openapi_document()
    actions = importlib.import_module("sdbase.workflow_api").ACTIONS
    workflows = []
    for name, action in sorted(actions.items()):
        signature = inspect.signature(action)
        source_file = Path(inspect.getsourcefile(action)).resolve()
        workflows.append({
            "path": f"/v1/workflows/{name}", "method": "POST", "function": action.__name__,
            "source": source_file.relative_to(source_root).as_posix(),
            "line": inspect.getsourcelines(action)[1],
            "parameters": [{"name": parameter.name,
                            "required": parameter.default is inspect.Parameter.empty,
                            "annotation": str(parameter.annotation),
                            "default": None if parameter.default is inspect.Parameter.empty else repr(parameter.default)}
                           for parameter in signature.parameters.values()
                           if parameter.name not in {"store", "principal"}],
        })
    methods = {"get", "post", "put", "patch", "delete", "head", "options"}
    operations = [{"path": path, "method": method.upper(), "operation_id": details.get("operationId")}
                  for path, value in sorted(openapi["paths"].items())
                  for method, details in sorted(value.items()) if method in methods]
    index = parsers["index.html"]
    sources = sorted(path for path in design_root.rglob("*") if path.is_file())
    sources += sorted((backend_root / "sdbase").glob("*.py"))
    counts = dict(sorted(Counter(item["profile"] for item in index.controls).items()))
    source_hashes = [{"source": path.relative_to(source_root).as_posix(),
                      "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for path in sources]
    inventory = {
        "schema_version": 1,
        "purpose": "Aşama 1: yalnız görsel kontrol envanteri ve gerçek SDBASE API sözleşmesi; uygulama değildir.",
        "design_is_architecture": False,
        "limitations": ["Statik HTML kontrolleri sayılır; JavaScript ile oluşan deck/mixer/pad satırları sayıya dahil değildir ve işlev haritasında ayrıca ele alınır.",
                        "Fragmentler tasarım referansıdır; aktif index.html kontrol sayısına tekrar eklenmez.",
                        "Kontrol sayısı benzersiz işlev sayısı değildir. HTML/JS davranışları C altyapısı olarak kullanılmaz.",
                        "OpenAPI bazı doğrudan gövdeleri yalnız genel nesne olarak belgeler; ayrıntılar servis/dispatcher koduyla doğrulanmalıdır."],
        "summary": {"profile_count": len(index.profiles), "active_design_control_count": len(index.controls),
                    "active_design_disabled_count": sum(item["disabled_in_design"] for item in index.controls),
                    "active_design_controls_by_profile": counts,
                    "design_html_file_count": len(pages), "design_script_count": len(index.scripts),
                    "workflow_count": len(workflows), "http_path_count": len(openapi["paths"]),
                    "http_operation_count": len(operations), "source_file_count": len(source_hashes)},
        "profiles": index.profiles, "design_scripts": index.scripts,
        "design_pages": pages, "workflows": workflows, "http_operations": operations,
        "source_hashes": source_hashes,
    }
    if {item["key"] for item in index.profiles} != set(PROFILES.values()):
        raise ValueError("Yedi tasarım profilinin tümü bulunamadı.")
    if len(workflows) != sum(path.startswith("/v1/workflows/") for path in openapi["paths"]):
        raise ValueError("Workflow listesi ile OpenAPI eşleşmiyor.")
    if any(not (source_root / item["source"]).is_file() for item in workflows):
        raise ValueError("Workflow kaynak dosyası bulunamadı.")
    return inventory, openapi


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=PROJECT,
                        help="RadioS dizin düzenini içeren kaynak kökü (varsayılan: bu proje).")
    parser.add_argument("--check", action="store_true", help="Mevcut çıktıları değiştirmeden kaynaklarla karşılaştır.")
    args = parser.parse_args()
    inventory, openapi = build_inventory(args.source_root.resolve())
    outputs = {PROJECT / "docs" / "islev-envanteri.json": inventory,
               PROJECT / "docs" / "sdbase-openapi.json": openapi}
    for path, document in outputs.items():
        content = serialize(document)
        if args.check:
            if not path.is_file() or path.read_text(encoding="utf-8") != content:
                print(f"Kaynakla eşleşmeyen çıktı: {path.name}", file=sys.stderr)
                return 1
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
    print(serialize(inventory["summary"]), end="")
    print("Kaynak ve sözleşme doğrulaması başarılı." if args.check else "Envanter ve API sözleşmesi üretildi.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
