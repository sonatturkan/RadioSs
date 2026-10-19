#!/usr/bin/env python3
"""Verify native connection against an owned host service across SDK namespaces."""
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
import urllib.request
import uuid

PROJECT = Path(__file__).resolve().parents[1]
CASES = (
    ("/data-connection-ui/existing-service-without-helper", True),
    ("/data-connection-ui/stopped-service-starts-on-host", False),
    ("/data-connection-ui/outdated-service-restarts-on-host", True),
    ("/data-connection-ui/auth-error-does-not-start-service", True),
    ("/data-connection-ui/validation-preserves-latest-pending", True),
    ("/data-connection-ui/refresh-burst-shares-kind-snapshot", True),
    ("/data-connection-ui/unchanged-refresh-preserves-forms", True),
    ("/data-connection-ui/manual-save-ack-preserves-reconnected-identity", True),
)


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: run_data_connection_ui.py /path/to/test_data_connection_ui", file=sys.stderr)
        return 2
    executable = Path(sys.argv[1]).resolve(strict=True)
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", G_DEBUG="fatal-warnings")
    environment.pop("RADIOS_SDBASE_TOKEN", None)
    environment.pop("PYTHONPATH", None)
    environment["RADIOS_TEST_CONNECTION_BAD_TOKEN"] = "radios-invalid-connection-token-00000000"
    spawn = shutil.which("flatpak-spawn")
    host_boundary = Path("/.flatpak-info").is_file() and bool(spawn)
    helper = [spawn, "--host", "python3", "-B"] if host_boundary else [sys.executable, "-B"]
    print("Native service acceptance: " + ("SDK GUI / host service" if host_boundary else "native host"), flush=True)
    build = PROJECT / "build"
    build.mkdir(exist_ok=True)
    for case, running in CASES:
        with tempfile.TemporaryDirectory(prefix="data-connection-ui-", dir=build) as temporary:
            root = Path(temporary) / "RadioS Türkçe veri bağlantısı"
            root.mkdir(mode=0o700)
            shutil.copy2(PROJECT / "CMakeLists.txt", root / "CMakeLists.txt")
            shutil.copytree(PROJECT / "assets", root / "assets")
            shutil.copytree(PROJECT / "database/SDBASE/src", root / "database/SDBASE/src",
                            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
            shutil.copy2(PROJECT / "database/SDBASE/pyproject.toml", root / "database/SDBASE/pyproject.toml")
            gated_save = case.endswith("/manual-save-ack-preserves-reconnected-identity")
            if gated_save:
                # Hold only a committed save response in this private service,
                # so another real user can reconnect before its ACK arrives.
                source = root / "database/SDBASE/src/sdbase/http_server.py"
                content = source.read_text(encoding="utf-8")
                anchor = "                result = self._dispatch(method, path, query, principal)\n"
                gate = '''                if method == "POST" and path == "/v1/workflows/desktop/put":
                    import time as _data_time
                    _data_hold = store.root / ".data-put-hold"
                    if _data_hold.exists():
                        (store.root / ".data-put-ready").write_text("ready")
                    _data_deadline = _data_time.monotonic() + 6.0
                    while _data_hold.exists() and _data_time.monotonic() < _data_deadline:
                        _data_time.sleep(0.005)
'''
                if content.count(anchor) != 1:
                    raise RuntimeError("Cannot install private save response gate")
                source.write_text(content.replace(anchor, anchor + gate), encoding="utf-8")
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
            configuration["database"]["python_command"] = "python3" if host_boundary else sys.executable
            configuration["database"]["connect_timeout_seconds"] = "1"
            configuration["database"]["request_timeout_seconds"] = "8" if gated_save else "3"
            with (root / "config/radios.ini").open("w", encoding="utf-8") as stream:
                configuration.write(stream)
            for name, value in configuration["paths"].items():
                if name != "assets_root":
                    path = root / value
                    (path.parent if name == "database_file" else path).mkdir(parents=True, exist_ok=True)
            command = [*helper, str(root / "tools/radios_database.py"), "--project-root", str(root)]

            def lifecycle(action: str) -> dict:
                arguments = command + [action]
                if action == "init":
                    arguments += ["--station-name", "GUI Kabul İstasyonu"]
                result = subprocess.run(arguments, cwd=root, env=environment,
                                        capture_output=True, text=True, timeout=25, check=False)
                if result.returncode:
                    raise RuntimeError(f"Private service {action} failed; exit={result.returncode}")
                return json.loads(result.stdout)

            try:
                initialized = lifecycle("init")
                if initialized.get("schema_version") != 4:
                    raise RuntimeError("Private database schema is invalid")
                credentials = (root / "config/credentials.json").read_bytes()
                if running:
                    if lifecycle("start").get("state") != "running":
                        raise RuntimeError("Owned host service did not start")
                    if host_boundary:
                        isolated = subprocess.run([sys.executable, *command[len(helper):], "status"],
                                                  cwd=root, env=environment, capture_output=True,
                                                  text=True, timeout=10, check=False)
                        if isolated.returncode or json.loads(isolated.stdout).get("state") != "foreign":
                            raise RuntimeError("SDK regression did not exercise a different PID namespace")
                if case == "/data-connection-ui/outdated-service-restarts-on-host":
                    source = root / "database/SDBASE/src/sdbase/runtime_version.py"
                    source.write_bytes(source.read_bytes() + b"\n# Updated native service regression fixture.\n")
                if gated_save:
                    identity = json.loads(credentials)
                    create = urllib.request.Request(configuration["database"]["service_url"] +
                        "/v1/workflows/desktop/user-create", method="POST",
                        headers={"Authorization": "Bearer " + identity["api_token"], "Content-Type": "application/json"},
                        data=json.dumps({"station_id": identity["station_id"], "username": "ack-viewer",
                            "display_name": "ACK Viewer", "role": "custom", "additional_permissions": [],
                            "idempotency_key": str(uuid.uuid4())}).encode("utf-8"))
                    with urllib.request.urlopen(create, timeout=8) as response:
                        environment["RADIOS_TEST_CONNECTION_VIEWER_TOKEN"] = json.load(response)["token"]
                result = subprocess.run([str(executable), str(root), "-p", case],
                                        cwd=root, env=environment, timeout=75, check=False)
                if result.returncode:
                    return result.returncode
                if (root / "config/credentials.json").read_bytes() != credentials:
                    raise RuntimeError("Native connection changed installation credentials")
                if lifecycle("status").get("state") != "running":
                    raise RuntimeError("Host could not recognize the native-started service")
            finally:
                lifecycle("stop")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
