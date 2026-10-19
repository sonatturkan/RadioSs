#!/usr/bin/env python3
"""Run native GTK/data acceptance against an isolated genuine SDBASE store."""
from __future__ import annotations

import configparser
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import importlib


PROJECT = Path(__file__).resolve().parents[1]


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: run_admin_ui.py /absolute/path/to/test_admin_ui [GLib test arguments]", file=sys.stderr)
        return 2
    executable = Path(sys.argv[1]).resolve(strict=True)
    build = PROJECT / "build"
    build.mkdir(exist_ok=True)
    environment = os.environ.copy()
    environment.pop("RADIOS_SDBASE_TOKEN", None)
    environment.pop("PYTHONPATH", None)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    environment["G_DEBUG"] = "fatal-warnings"
    with tempfile.TemporaryDirectory(prefix="native-admin-ui-", dir=build) as temporary:
        root = Path(temporary) / "RadioS Türkçe kaynak"
        root.mkdir()
        shutil.copy2(PROJECT / "CMakeLists.txt", root / "CMakeLists.txt")
        shutil.copytree(PROJECT / "assets", root / "assets")
        shutil.copytree(PROJECT / "database/SDBASE/src", root / "database/SDBASE/src",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        shutil.copy2(PROJECT / "database/SDBASE/pyproject.toml", root / "database/SDBASE/pyproject.toml")
        (root / "tools").mkdir()
        for source in (PROJECT / "tools").glob("radios_*.py"):
            shutil.copy2(source, root / "tools" / source.name)
        # Fixture-only launch barrier proves close cancels an actually running
        # helper. The production helper and its publication logic are untouched.
        reliability = root / "tools/radios_reliability.py"
        fixture_source = reliability.read_text(encoding="utf-8")
        anchor = "from __future__ import annotations\n"
        gate = '''import sys as _acceptance_sys
from pathlib import Path as _AcceptancePath
import time as _acceptance_time
if "--project-root" in _acceptance_sys.argv:
    _acceptance_root = _AcceptancePath(_acceptance_sys.argv[_acceptance_sys.argv.index("--project-root") + 1])
    _acceptance_hold = _acceptance_root / ".acceptance-tool-hold"
    if _acceptance_hold.exists():
        (_acceptance_root / ".acceptance-tool-ready").write_text("ready")
        _acceptance_deadline = _acceptance_time.monotonic() + 20
        while _acceptance_hold.exists() and _acceptance_time.monotonic() < _acceptance_deadline:
            _acceptance_time.sleep(0.01)
'''
        if fixture_source.count(anchor) != 1:
            raise RuntimeError("The isolated helper launch barrier could not be installed")
        reliability.write_text(fixture_source.replace(anchor, anchor + gate), encoding="utf-8")
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
            # A real station-view key is injected before the native process
            # creates any threads. Installation/service credentials stay intact.
            sys.path.insert(0, str(root / "tools"))
            setup = importlib.import_module("radios_database_setup")
            private_config = setup.load_config(root)
            backend = setup.backend_import(private_config)
            private_credentials = setup.load_credentials(private_config)
            store = backend.open_radio_store(private_config.database_file)
            access = importlib.import_module(backend.package_name + ".access_admin")
            view_user = access.create_station_user(store,
                backend.authenticate(store, private_credentials["api_token"]),
                private_credentials["station_id"], "native_admin_view", "Native Yalnız İzleme", ["view"])
            environment["RADIOS_TEST_VIEW_TOKEN"] = view_user["token"]
            started = subprocess.run(command + ["start"], cwd=root, env=environment,
                                     capture_output=True, text=True, timeout=20)
            if started.returncode:
                print("Isolated administration service did not start.", file=sys.stderr)
                return 1
            result = subprocess.run([str(executable), str(root), *sys.argv[2:]], cwd=root, env=environment,
                                    timeout=150, check=False)
            return result.returncode
        finally:
            subprocess.run(command + ["stop"], cwd=root, env=environment, capture_output=True,
                           text=True, timeout=15, check=False)


if __name__ == "__main__":
    raise SystemExit(main())
