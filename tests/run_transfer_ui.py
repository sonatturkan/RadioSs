#!/usr/bin/env python3
"""Native Export/Import page acceptance against a private fake transfer service.

The fake answers the data-service handshake and the six desktop-transfer
operations of docs/kilavuz-2026-10-18-D.md with fixed JSON, so the GTK pages
can be verified before (and independently of) the real package backend.
"""
from __future__ import annotations

import configparser
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import uuid

PROJECT = Path(__file__).resolve().parents[1]
STORE, USER, STATION, ENDPOINT = (str(uuid.uuid4()) for _ in range(4))
API_TOKEN = "transfer-ui-api-" + uuid.uuid4().hex
ENDPOINT_TOKEN = "transfer-ui-endpoint-" + uuid.uuid4().hex
KINDS = ["automation", "automation_calendar", "weekly_programme", "live_players", "audio_preferences", "programme"]
# The programme group lists its documents for a per-document export choice
# (tests/test_transfer_ui.c knows these identities); the audio group answers
# like a service from before that choice, without a document list.
PROGRAMMES = [{"id": "6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a01", "name": "Akşam kuşağı"},
              {"id": "6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a02", "name": "Öğle haberleri"},
              {"id": "6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a03", "name": "Sabah programı"}]
GROUPS = [{"id": "audio_preferences", "label": "Ses aygıtı ve ses yüksekliği ayarları", "count": 3},
          {"id": "programme", "label": "Programlar", "count": 12, "documents": PROGRAMMES}]
MEDIA = {"count": 7, "bytes": 123456789}


class FakeTransferService(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, root: Path):
        super().__init__(("127.0.0.1", 0), Handler)
        self.root = root
        self.jobs: dict[str, dict] = {}
        self.lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args) -> None:  # keep ctest output quiet
        pass

    def _send(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/health":
            return self._send(200, {"status": "ok", "store_id": STORE, "format": "sdbase-journal-v1"})
        if self.path == "/v1/capabilities":
            return self._send(200, {"api_version": "0.8.1", "schema_version": 4, "desktop_api_version": 1,
                                    "source_digest": "sahte", "desktop_configuration_kinds": KINDS})
        return self._send(404, {"error": "not_found"})

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0") or 0)
        body = json.loads(self.rfile.read(length) or b"{}")
        if self.path == "/v1/workflows/desktop/context":
            return self._send(200, {
                "user": {"_id": USER, "username": "aktarim", "display_name": "Aktarım Kabulü", "status": "active"},
                "station": {"_id": STATION, "name": "Test Radyo", "timezone": "Europe/Istanbul"},
                "permissions": [{"user_id": USER, "permission": "manage", "station_id": STATION}],
                "endpoints": [], "users": [], "outputs": [], "collections": {}, "sequence": 1})
        prefix = "/v1/workflows/desktop-transfer/"
        if not self.path.startswith(prefix):
            return self._send(404, {"error": "not_found"})
        operation = self.path[len(prefix):]
        server: FakeTransferService = self.server  # type: ignore[assignment]
        (server.root / f"fake-{operation}.json").write_text(json.dumps(body, ensure_ascii=False), encoding="utf-8")
        with server.lock:
            status, payload = transfer(server.jobs, operation, body)
        return self._send(status, payload)


def transfer(jobs: dict, operation: str, body: dict) -> tuple[int, dict]:
    if operation == "groups":
        return 200, {"groups": GROUPS, "media": MEDIA}
    if operation == "export-start":
        if body.get("file_name") == "hata.st":
            return 422, {"error": "invalid_request", "detail": "Hedef klasör yazılabilir değil."}
        job_id = str(uuid.uuid4())
        package = str(Path(body["destination_dir"]) / body.get("file_name", "RadioS.st"))
        jobs[job_id] = {"kind": "export", "polls": 0, "cancelled": False, "package_path": package,
                        "slow": body.get("file_name") == "yavas.st"}
        return 200, {"job_id": job_id, "state": "running", "package_path": package}
    if operation == "inspect":
        path = Path(str(body.get("package_path", "")))
        if path.name == "bozuk.st":
            return 422, {"error": "invalid_request", "detail": "Paket bütünlüğü doğrulanamadı."}
        if not path.is_file():
            return 422, {"error": "invalid_request", "detail": "Paket bulunamadı."}
        return 200, {"manifest": {"format": "radios-transfer-package", "version": 1,
                                  "created_at": "2026-10-18T07:30:00+00:00", "application_version": "0.19.0",
                                  "station": {"id": STATION, "name": "Test Radyo"},
                                  "scope": {"settings": "all", "kinds": [g["id"] for g in GROUPS], "media": True}},
                     "settings": [{"kind": g["id"], "label": g["label"], "count": g["count"]} for g in GROUPS],
                     "media": MEDIA, "warnings": ["Gizli alanlar boşaltıldı: server (2)"]}
    if operation == "import-start":
        job_id = str(uuid.uuid4())
        jobs[job_id] = {"kind": "import", "polls": 0, "cancelled": False, "slow": False}
        return 200, {"job_id": job_id, "state": "running"}
    job = jobs.get(str(body.get("job_id", "")))
    if job is None:
        return 404, {"error": "not_found", "detail": "Aktarım işi bulunamadı."}
    if operation == "job-cancel":
        job["cancelled"] = True
        return 200, {"job_id": body["job_id"], "state": "cancelled"}
    if operation != "job":
        return 404, {"error": "not_found"}
    job["polls"] += 1
    reply = {"job_id": body["job_id"], "kind": job["kind"]}
    if job["cancelled"]:
        reply.update(state="cancelled", progress=25, message="İş iptal edildi.")
    elif job["slow"] or job["polls"] < 2:
        reply.update(state="running", progress=50, message="Dosyalar kopyalanıyor… (3/7)")
    elif job["kind"] == "export":
        reply.update(state="done", progress=100, message="Paket yazıldı.", package_path=job["package_path"])
    else:
        reply.update(state="done", progress=100, message="15 ayar belgesi ve 7 şarkı geri yüklendi.",
                     result={"settings": 15, "media": 7,
                             "warnings": ["Bir paket kaydı atlandı: test kaydı"]})
    return 200, reply


def prepare_root(root: Path, port: int) -> None:
    (root / "config").mkdir()
    shutil.copy2(PROJECT / "config/radios.example.ini", root / "config/radios.example.ini")
    shutil.copytree(PROJECT / "assets", root / "assets")
    (root / "database/SDBASE").mkdir(parents=True)
    shutil.copy2(PROJECT / "database/SDBASE/pyproject.toml", root / "database/SDBASE/pyproject.toml")
    configuration = configparser.ConfigParser(interpolation=None)
    configuration.read(root / "config/radios.example.ini", encoding="utf-8")
    configuration["database"]["service_url"] = f"http://127.0.0.1:{port}"
    configuration["database"]["python_command"] = sys.executable
    configuration["database"]["connect_timeout_seconds"] = "1"
    configuration["database"]["request_timeout_seconds"] = "3"
    with (root / "config/radios.ini").open("w", encoding="utf-8") as stream:
        configuration.write(stream)
    for name, value in configuration["paths"].items():
        if name != "assets_root":
            path = root / value
            (path.parent if name == "database_file" else path).mkdir(parents=True, exist_ok=True)
    database = root / configuration["paths"]["database_file"]
    database.write_bytes(b"")
    database.with_name(database.name + ".meta").write_text(json.dumps(
        {"schema_version": 4, "store_id": STORE, "format": "sdbase-journal-v1"}), encoding="utf-8")
    identity = {"schema_version": 1, "store_id": STORE, "user_id": USER, "station_id": STATION,
                "endpoint_id": ENDPOINT, "api_token": API_TOKEN, "endpoint_token": ENDPOINT_TOKEN,
                "database_file": configuration["paths"]["database_file"], "station_name": "Test Radyo",
                "username": "aktarim"}
    descriptor = os.open(root / "config/credentials.json", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        json.dump(identity, stream)


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: run_transfer_ui.py /path/to/test_transfer_ui [GLib test arguments]", file=sys.stderr)
        return 2
    executable = Path(sys.argv[1]).resolve(strict=True)
    environment = os.environ.copy()
    environment.pop("RADIOS_SDBASE_TOKEN", None)
    environment.pop("PYTHONPATH", None)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    environment["G_DEBUG"] = "fatal-warnings"
    build = PROJECT / "build"
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-transfer-ui-", dir=build) as temporary:
        root = Path(temporary) / "RadioS aktarım kabulü"
        root.mkdir()
        server = FakeTransferService(root)
        prepare_root(root, server.server_address[1])
        thread = threading.Thread(target=server.serve_forever, name="fake-transfer-service", daemon=True)
        thread.start()
        try:
            result = subprocess.run([str(executable), str(root), *sys.argv[2:]], cwd=root, env=environment,
                                    timeout=80, check=False)
            return result.returncode
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
