#!/usr/bin/env python3
"""Native USB/legacy XML import with private credentials, store and fault gates."""
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

from test_desktop_rekordbox import XML
from rekordbox_device_fixture import export_pdb

PROJECT = Path(__file__).resolve().parents[1]


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: run_rekordbox_ui.py /path/to/test_rekordbox_ui [GLib test arguments]", file=sys.stderr)
        return 2
    executable = Path(sys.argv[1]).resolve(strict=True)
    environment = os.environ.copy()
    environment.pop("RADIOS_SDBASE_TOKEN", None)
    environment.pop("PYTHONPATH", None)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    environment["G_DEBUG"] = "fatal-warnings"
    (PROJECT / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-rekordbox-ui-", dir=PROJECT / "build") as temporary:
        root = Path(temporary) / "RadioS Türkçe kaynak"
        root.mkdir()
        shutil.copy2(PROJECT / "CMakeLists.txt", root / "CMakeLists.txt")
        shutil.copytree(PROJECT / "assets", root / "assets")
        shutil.copytree(PROJECT / "database/SDBASE/src", root / "database/SDBASE/src",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        shutil.copy2(PROJECT / "database/SDBASE/pyproject.toml", root / "database/SDBASE/pyproject.toml")
        server = root / "database/SDBASE/src/sdbase/http_server.py"
        source = server.read_text(encoding="utf-8")
        anchor = "                result = self._dispatch(method, path, query, principal)\n"
        gates = '''                _xml_hold = store.root / ".acceptance-rekordbox-preview-hold"
                if method == "POST" and path == "/v1/workflows/desktop-rekordbox/preview" and _xml_hold.is_file():
                    import time as _xml_time
                    (store.root / ".acceptance-rekordbox-preview-ready").write_text("ready", encoding="utf-8")
                    _xml_deadline = _xml_time.monotonic() + 8.0
                    while _xml_hold.exists() and _xml_time.monotonic() < _xml_deadline:
                        _xml_time.sleep(0.005)
                _xml_drop = store.root / ".acceptance-rekordbox-drop"
                if method == "POST" and path == "/v1/workflows/desktop-rekordbox/apply" and _xml_drop.is_file():
                    import socket as _xml_socket
                    _xml_drop.unlink()
                    self.close_connection = True
                    self.connection.shutdown(_xml_socket.SHUT_RDWR)
                    self.connection.close()
                    return
'''
        if source.count(anchor) != 1:
            raise RuntimeError("Cannot install isolated Rekordbox response fault gates")
        server.write_text(source.replace(anchor, anchor + gates), encoding="utf-8")
        (root / "test-rekordbox.xml").write_bytes(XML)
        usb = root / "USB örnek" / "PIONEER" / "rekordbox"
        usb.mkdir(parents=True)
        (usb / "export.pdb").write_bytes(export_pdb())
        github_export = environment.get("RADIOS_TEST_USB_EXPORT")
        if github_export:
            github_usb = root / "GitHub USB" / "PIONEER" / "rekordbox"
            github_usb.mkdir(parents=True)
            shutil.copy2(github_export, github_usb / "export.pdb")
        (root / "tools").mkdir()
        for path in (PROJECT / "tools").glob("radios_database*.py"):
            shutil.copy2(path, root / "tools" / path.name)
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
        configuration["database"]["request_timeout_seconds"] = "5"
        with (root / "config/radios.ini").open("w", encoding="utf-8") as stream:
            configuration.write(stream)
        for name, value in configuration["paths"].items():
            if name != "assets_root":
                target = root / value
                (target.parent if name == "database_file" else target).mkdir(parents=True, exist_ok=True)
        command = [sys.executable, str(root / "tools/radios_database.py"), "--project-root", str(root)]
        try:
            initialized = subprocess.run(command + ["init", "--station-name", "XML Kabul İstasyonu"],
                                         cwd=root, env=environment, capture_output=True, text=True, timeout=20)
            if initialized.returncode or json.loads(initialized.stdout).get("schema_version") != 4:
                print("Private native XML database initialization failed.", file=sys.stderr)
                return 1
            started = subprocess.run(command + ["start"], cwd=root, env=environment,
                                     capture_output=True, text=True, timeout=20)
            if started.returncode:
                print("Private XML service did not start.", file=sys.stderr)
                return 1
            return subprocess.run([str(executable), str(root), *sys.argv[2:]], cwd=root,
                                  env=environment, timeout=100, check=False).returncode
        finally:
            subprocess.run(command + ["stop"], cwd=root, env=environment,
                           capture_output=True, text=True, timeout=15, check=False)


if __name__ == "__main__":
    raise SystemExit(main())
