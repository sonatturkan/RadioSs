#!/usr/bin/env python3
"""Run native GTK/data acceptance against an isolated genuine SDBASE store."""
from __future__ import annotations

import configparser
import json
import os
from pathlib import Path
import shutil
import re
import socket
import subprocess
import sys
import tempfile


PROJECT = Path(__file__).resolve().parents[1]


def main(*, collect_glib_cases: list[str] | None = None,
         expected_glib_case: str | None = None) -> int:
    if len(sys.argv) < 2:
        print("Usage: run_data_ui.py /absolute/path/to/test_data_ui [GLib test arguments]", file=sys.stderr)
        return 2
    executable = Path(sys.argv[1]).resolve(strict=True)
    build = PROJECT / "build"
    build.mkdir(exist_ok=True)
    environment = os.environ.copy()
    environment.pop("RADIOS_SDBASE_TOKEN", None)
    environment.pop("PYTHONPATH", None)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    environment["G_DEBUG"] = "fatal-warnings"
    with tempfile.TemporaryDirectory(prefix="native-data-ui-", dir=build) as temporary:
        root = Path(temporary) / "RadioS Türkçe kaynak"
        root.mkdir()
        shutil.copy2(PROJECT / "CMakeLists.txt", root / "CMakeLists.txt")
        shutil.copytree(PROJECT / "assets", root / "assets")
        shutil.copytree(PROJECT / "database/SDBASE/src", root / "database/SDBASE/src",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        shutil.copy2(PROJECT / "database/SDBASE/pyproject.toml", root / "database/SDBASE/pyproject.toml")
        # Only this private fixture copy gets a deterministic response gate.
        # The real store computes the LIST result first; the test can then make
        # a local edit before that older/newer revision reaches GTK's callback.
        # Production source and normal request behavior are untouched.
        server_source = root / "database/SDBASE/src/sdbase/http_server.py"
        source = server_source.read_text(encoding="utf-8")
        anchor = "                result = self._dispatch(method, path, query, principal)\n"
        gate = '''                _acceptance_gate = store.root / ".acceptance-list-hold"
                if (method == "POST" and path == "/v1/workflows/desktop/list" and
                        isinstance(result, dict) and _acceptance_gate.is_file()):
                    _acceptance_kind = _acceptance_gate.read_text(encoding="utf-8")
                    if any(row.get("kind") == _acceptance_kind for row in result.get("records", [])):
                        import time as _acceptance_time
                        _acceptance_ready = store.root / ".acceptance-list-ready"
                        _acceptance_ready.write_text("ready", encoding="utf-8")
                        _acceptance_ready.chmod(0o600)
                        _acceptance_deadline = _acceptance_time.monotonic() + 2.0
                        while _acceptance_gate.exists() and _acceptance_time.monotonic() < _acceptance_deadline:
                            _acceptance_time.sleep(0.005)
                if method == "POST" and path == "/v1/workflows/desktop/list" and isinstance(result, dict):
                    _acceptance_records = result.get("records", [])
                    _acceptance_document_kind = _acceptance_records[0].get("kind") if _acceptance_records else None
                    if _acceptance_document_kind:
                        with _acceptance_document_count_lock:
                            _acceptance_count = store.root / (".acceptance-document-read-count-" + _acceptance_document_kind)
                            _acceptance_previous = int(_acceptance_count.read_text()) if _acceptance_count.is_file() else 0
                            _acceptance_count.write_text(str(_acceptance_previous + 1), encoding="utf-8")
                            _acceptance_count.chmod(0o600)
                        _acceptance_document_gate = store.root / ".acceptance-document-read-hold"
                        if (_acceptance_document_gate.is_file() and
                                _acceptance_document_gate.read_text(encoding="utf-8") == _acceptance_document_kind):
                            import time as _acceptance_time
                            _acceptance_document_ready = store.root / ".acceptance-document-read-ready"
                            _acceptance_document_ready.write_text("ready", encoding="utf-8")
                            _acceptance_document_ready.chmod(0o600)
                            _acceptance_document_deadline = _acceptance_time.monotonic() + 8.0
                            while (_acceptance_document_gate.exists() and
                                   _acceptance_time.monotonic() < _acceptance_document_deadline):
                                _acceptance_time.sleep(0.005)
                _acceptance_drop = store.root / ".acceptance-import-drop"
                if method == "POST" and path == "/v1/workflows/desktop-media/import" and _acceptance_drop.is_file():
                    import socket as _acceptance_socket
                    _acceptance_drop.unlink()
                    self.close_connection = True
                    self.connection.shutdown(_acceptance_socket.SHUT_RDWR)
                    self.connection.close()
                    return
                _acceptance_playlist_drop = store.root / ".acceptance-playlist-drop"
                if (method == "POST" and path == "/v1/workflows/desktop-playlists/entry-save" and
                        _acceptance_playlist_drop.is_file()):
                    _acceptance_remaining = int(_acceptance_playlist_drop.read_text(encoding="utf-8"))
                    if _acceptance_remaining > 0:
                        _acceptance_playlist_drop.write_text(str(_acceptance_remaining - 1), encoding="utf-8")
                    else:
                        import socket as _acceptance_socket
                        _acceptance_playlist_drop.unlink()
                        self.close_connection = True
                        self.connection.shutdown(_acceptance_socket.SHUT_RDWR)
                        self.connection.close()
                        return
'''
        if source.count(anchor) != 1:
            raise RuntimeError("The isolated HTTP acceptance gate could not be installed")
        count_anchor = "    limits = RequestLimiter()\n"
        if source.count(count_anchor) != 1:
            raise RuntimeError("The isolated document request counter could not be installed")
        source = source.replace(count_anchor, count_anchor +
            "    from threading import Lock as _AcceptanceLock\n    _acceptance_document_count_lock = _AcceptanceLock()\n")
        server_source.write_text(source.replace(anchor, anchor + gate), encoding="utf-8")
        (root / "tools").mkdir()
        for source in (PROJECT / "tools").glob("radios_database*.py"):
            shutil.copy2(source, root / "tools" / source.name)
        (root / "config").mkdir()
        shutil.copy2(PROJECT / "config/radios.example.ini", root / "config/radios.example.ini")
        configuration = configparser.ConfigParser(interpolation=None)
        configuration.read(root / "config/radios.example.ini", encoding="utf-8")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        configuration["database"]["service_url"] = f"http://127.0.0.1:{port}"
        configuration["database"]["python_command"] = sys.executable
        configuration["database"]["connect_timeout_seconds"] = "1"
        configuration["database"]["request_timeout_seconds"] = "3"
        with (root / "config/radios.ini").open("w", encoding="utf-8") as stream:
            configuration.write(stream)
        for name, value in configuration["paths"].items():
            if name != "assets_root":
                destination = root / value
                (destination.parent if name == "database_file" else destination).mkdir(parents=True, exist_ok=True)
        command = [sys.executable, str(root / "tools/radios_database.py"), "--project-root", str(root)]
        try:
            initialized = subprocess.run(command + ["init", "--station-name", "GUI Kabul İstasyonu"],
                                         cwd=root, env=environment, capture_output=True, text=True, timeout=20)
            if initialized.returncode != 0:
                print("Isolated database initialization failed.", file=sys.stderr)
                return 1
            summary = json.loads(initialized.stdout)
            if summary.get("schema_version") != 4 or summary.get("endpoint_status") != "offline":
                print("Isolated database identity is invalid.", file=sys.stderr)
                return 1
            # This fixture's service must remain beside the runner: outage tests
            # and cleanup own its PID here. Host/SDK lifecycle is exercised by
            # the dedicated run_data_connection_ui.py runner instead.
            started = subprocess.run(command + ["start"], cwd=root, env=environment,
                                     capture_output=True, text=True, timeout=20, check=False)
            if started.returncode or json.loads(started.stdout).get("state") != "running":
                print("Isolated database service did not start.", file=sys.stderr)
                return 1
            validate_tap = collect_glib_cases is not None or expected_glib_case is not None
            capture = {"capture_output": True, "text": True} if validate_tap else {}
            result = subprocess.run([str(executable), str(root), *sys.argv[2:]], cwd=root, env=environment,
                                    timeout=100, check=False, **capture)
            if validate_tap:
                print(result.stdout, end="", flush=True)
                print(result.stderr, end="", file=sys.stderr, flush=True)
                if not result.returncode:
                    lines = result.stdout.splitlines()
                    plans = [int(line[3:]) for line in lines if re.fullmatch(r"1\.\.\d+", line)]
                    if collect_glib_cases is not None:
                        cases = [line[2:] if line.startswith("# /") else line
                                 for line in lines if line.startswith(("/", "# /"))]
                        if len(plans) != 1 or plans[0] <= 0 or plans[0] != len(cases):
                            raise RuntimeError("Native data discovery did not match its positive TAP plan")
                        collect_glib_cases.extend(cases)
                    if expected_glib_case is not None:
                        outcomes = [line for line in lines if re.match(r"(?:not )?ok \d+\b", line)]
                        if plans != [1] or outcomes != ["ok 1 " + expected_glib_case]:
                            raise RuntimeError("Native data case did not execute its exact expected test without skipping")
            if result.returncode:
                log = root / "data/logs/database-service.log"
                if log.is_file():
                    responses = re.findall(r'"(GET|POST) ([^"\n]+) HTTP/1\.1" (\d{3})',
                                           log.read_text(encoding="utf-8", errors="replace"))
                    print("Private fixture's last HTTP results:", file=sys.stderr)
                    for method, path, status in responses[-30:]:
                        print(f"{method} {path} {status}", file=sys.stderr)
            return result.returncode
        finally:
            subprocess.run(command + ["stop"], cwd=root, env=environment, capture_output=True,
                           text=True, timeout=15, check=False)


if __name__ == "__main__":
    raise SystemExit(main())
