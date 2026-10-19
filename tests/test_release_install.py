#!/usr/bin/env python3
"""Relocated native launcher acceptance; every installation stays in build/.

The longer archive extraction and source rebuild acceptance is available in
run_release_acceptance.py. These tests exercise the same real launcher, C
executable and SDBASE service without modifying the installed RadioS identity.
"""
from __future__ import annotations

import configparser
import contextlib
import fcntl
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from uuid import UUID

PROJECT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(PROJECT / "tools"))
import radios_package as packaging


def environment() -> dict[str, str]:
    value = os.environ.copy()
    for name in ("RADIOS_SDBASE_TOKEN", "PYTHONPATH", "RADIOS_TEST_VIEW_TOKEN"):
        value.pop(name, None)
    value["PYTHONDONTWRITEBYTECODE"] = "1"
    value["G_DEBUG"] = "fatal-warnings"
    return value


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class Installation:
    def __init__(self, root: Path, cwd: Path):
        self.root, self.cwd = root, cwd
        self.env = environment()
        self.port = free_port()
        self.config = configparser.ConfigParser(interpolation=None)
        self.config.read(root / "config/radios.example.ini", encoding="utf-8")
        self.config["database"]["service_url"] = f"http://127.0.0.1:{self.port}"
        self.config["database"]["python_command"] = sys.executable
        self.config["database"]["connect_timeout_seconds"] = "1"
        self.config["database"]["request_timeout_seconds"] = "3"
        self.config_path = root / "config/radios.ini"
        self.write_config()
        self.processes: list[subprocess.Popen] = []

    @property
    def credentials_path(self) -> Path:
        return self.root / "config/credentials.json"

    def write_config(self) -> None:
        with self.config_path.open("w", encoding="utf-8") as stream:
            self.config.write(stream)
        self.config_path.chmod(0o600)

    def credentials(self) -> dict:
        return json.loads(self.credentials_path.read_text(encoding="utf-8"))

    def redact(self, text: str) -> str:
        with contextlib.suppress(OSError, ValueError):
            for name in ("api_token", "endpoint_token"):
                token = self.credentials().get(name)
                if token:
                    text = text.replace(token, "[REDACTED]")
        return text

    def command(self, *arguments: str, expected: int | None = 0,
                env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(arguments, cwd=self.cwd, env=env or self.env,
                                capture_output=True, text=True, timeout=25, check=False)
        with contextlib.suppress(OSError, ValueError):
            credentials = self.credentials()
            for name in ("api_token", "endpoint_token"):
                if credentials.get(name) in result.stdout or credentials.get(name) in result.stderr:
                    raise AssertionError("A release command exposed a private credential")
        if expected is not None and result.returncode != expected:
            raise AssertionError(self.redact(
                f"Release command failed ({result.returncode}): {result.stderr[:1500]}"))
        return result

    def cli(self, action: str, *arguments: str, expected: int | None = 0) -> dict | None:
        result = self.command(sys.executable, str(self.root / "tools/radios_database.py"),
                              "--project-root", str(self.root), action, *arguments,
                              expected=expected)
        return json.loads(result.stdout) if result.stdout.strip() else None

    def launch(self, *arguments: str, expected: int | None = 0,
               env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        return self.command(str(self.root / "run-radios.sh"), *arguments,
                            expected=expected, env=env)

    def request(self, path: str, body: dict | None = None,
                *, authenticated: bool = True) -> tuple[int, dict]:
        headers = {}
        if authenticated:
            headers["Authorization"] = "Bearer " + self.credentials()["api_token"]
        if body is not None:
            headers["Content-Type"] = "application/json"
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            connection.request("POST" if body is not None else "GET", path,
                               body=json.dumps(body) if body is not None else None, headers=headers)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def inspect(self) -> dict:
        program = """import json,sys
from sdbase.schema import open_radio_store
from sdbase.integrity import inspect_integrity
store=open_radio_store(sys.argv[1],read_only=True)
names=('users','stations','endpoints','tokens','permissions','items','versions','playlists','desktop_configurations')
print(json.dumps({'sequence':store.sequence,'schema_version':store.schema_version,
 'integrity':inspect_integrity(store)['status'],
 'collections':{name:len(store.scan(name)) for name in names},
 'endpoint_status':[row['status'] for row in store.scan('endpoints')],
 'config_kinds':[row['kind'] for row in store.scan('desktop_configurations')]}))
"""
        env = self.env.copy()
        env["PYTHONPATH"] = str(self.root / "database/SDBASE/src")
        result = self.command(sys.executable, "-c", program,
                              str(self.root / "data/database/RadioS.sdb"), env=env)
        return json.loads(result.stdout)

    def gui_lock_held(self) -> bool:
        path = self.root / "data/database-service/gui.lock"
        if not path.is_file():
            return False
        with path.open("rb") as stream:
            try:
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return True
            fcntl.flock(stream, fcntl.LOCK_UN)
        return False

    def native_start(self) -> subprocess.Popen:
        process = subprocess.Popen([str(self.root / "run-radios.sh")], cwd=self.cwd,
                                   env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True)
        self.processes.append(process)
        deadline = time.monotonic() + 15
        expected = self.root / "build/radios"
        while time.monotonic() < deadline:
            if process.poll() is not None:
                _out, err = process.communicate()
                raise AssertionError(self.redact("Native first launch failed: " + err[:1500]))
            try:
                actual = Path(f"/proc/{process.pid}/exe").resolve(strict=True)
                if actual == expected and self.gui_lock_held():
                    status, health = self.request("/health", authenticated=False)
                    if status == 200 and health.get("store_id") == self.credentials()["store_id"]:
                        return process
            except (OSError, ValueError, http.client.HTTPException):
                pass
            time.sleep(0.02)
        raise AssertionError("Relocated native window/service did not become ready")

    def native_stop(self, process: subprocess.Popen) -> None:
        if process.poll() is None:
            process.terminate()
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=3)

    def close(self) -> None:
        for process in self.processes:
            self.native_stop(process)
        with contextlib.suppress(Exception):
            self.cli("stop")
        # Only this exact extracted installation can own a leftover child.
        for entry in Path("/proc").glob("[0-9]*"):
            try:
                arguments = entry.joinpath("cmdline").read_bytes().decode().rstrip("\0").split("\0")
                if ("serve" in arguments and str(self.root) in arguments and
                        str(self.root / "tools/radios_database.py") in arguments):
                    os.kill(int(entry.name), 15)
            except (OSError, UnicodeError, ValueError):
                continue


class ReleaseInstall(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.files = packaging.collect(PROJECT, "binary")
        override = os.environ.get("RADIOS_RELEASE_BINARY")
        binary = Path(override) if override else PROJECT / "build/radios"
        if not binary.is_file():
            raise RuntimeError("Build the real C desktop before release installation acceptance")
        cls.binary = binary.read_bytes()

    def setUp(self) -> None:
        base = PROJECT / "build/stage14"
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="kabul-", dir=base)
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        root = self.base / "Taşınmış Türkçe kaynak RadioS"
        for name, (body, mode) in self.files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(body)
            path.chmod(mode)
        (root / "build").mkdir()
        (root / "build/radios").write_bytes(self.binary)
        (root / "build/radios").chmod(0o755)
        for name in packaging.DATA_DIRS:
            (root / "data" / name).mkdir(parents=True, exist_ok=True)
        self.cwd = self.base / "Farklı çalışma klasörü"
        self.cwd.mkdir()
        self.installation = Installation(root, self.cwd)
        self.addCleanup(self.installation.close)
        self.root = root

    def assert_uninitialized(self) -> None:
        self.assertFalse(self.installation.credentials_path.exists())
        self.assertFalse((self.root / "data/database/RadioS.sdb").exists())
        self.assertFalse((self.root / "data/database-service/service.json").exists())

    def test_passive_modes_from_other_directory_never_initialize(self) -> None:
        for flag in ("--version", "--help", "--check-config"):
            with self.subTest(flag=flag):
                result = self.installation.launch(flag)
                self.assertTrue(result.stdout.strip())
                self.assert_uninitialized()
        self.assertIn(str(self.root), self.installation.launch("--check-config").stdout)

    def test_invalid_options_and_config_fail_before_initialization(self) -> None:
        for arguments in (("--not-a-radios-option",), ("--config", "config/missing.ini"),
                          ("--capture", str(self.cwd / "capture.png")), ("unexpected-positional",)):
            with self.subTest(arguments=arguments):
                result = self.installation.launch(*arguments, expected=None)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr.strip())
                self.assert_uninitialized()
        self.installation.config["paths"]["database_file"] = "../outside.sdb"
        self.installation.write_config()
        self.assertNotEqual(self.installation.launch(expected=None).returncode, 0)
        self.assert_uninitialized()
        self.assertFalse((self.base / "outside.sdb").exists())

    def test_no_desktop_session_does_not_create_an_identity(self) -> None:
        env = self.installation.env.copy()
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        self.assertNotEqual(self.installation.launch(expected=None, env=env).returncode, 0)
        self.assert_uninitialized()

    def test_smoke_profiles_assets_without_service_or_database(self) -> None:
        if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
            self.skipTest("Native smoke needs a real display session")
        capture = self.cwd / "Türkçe pencere.png"
        result = self.installation.launch("--smoke-test", "--capture", str(capture))
        self.assertIn("8 profil", result.stdout)
        self.assertEqual(capture.read_bytes()[:8], b"\x89PNG\r\n\x1a\n")
        self.assert_uninitialized()

    def test_first_native_launch_and_repeat_preserve_fresh_identity(self) -> None:
        if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
            self.skipTest("Native launch needs a real display session")
        first = self.installation.native_start()
        credentials = self.installation.credentials()
        original = self.installation.credentials_path.read_bytes()
        for name in ("store_id", "user_id", "station_id", "endpoint_id"):
            self.assertEqual(str(UUID(credentials[name])), credentials[name])
        self.assertEqual(stat.S_IMODE(self.installation.credentials_path.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(self.installation.config_path.stat().st_mode), 0o600)
        state = self.installation.inspect()
        self.assertEqual(state["integrity"], "ok")
        self.assertEqual(state["endpoint_status"], ["offline"])
        self.assertEqual(state["collections"]["users"], 1)
        self.assertEqual(state["collections"]["stations"], 1)
        self.assertEqual(state["collections"]["items"], 0)
        self.assertEqual(state["collections"]["playlists"], 0)
        self.assertFalse(set(state["config_kinds"]) & {"broadcast_local_run", "advert_local_run"})
        service = self.installation.cli("status")
        self.installation.native_stop(first)
        second = self.installation.native_start()
        self.assertEqual(self.installation.credentials_path.read_bytes(), original)
        self.assertEqual(self.installation.cli("status")["pid"], service["pid"])
        self.installation.native_stop(second)
        self.installation.cli("init")
        self.assertEqual(self.installation.credentials_path.read_bytes(), original)
        self.assertEqual(self.installation.inspect(), state)

    def test_missing_owned_service_restarts_with_same_credentials(self) -> None:
        if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
            self.skipTest("Native launch needs a real display session")
        self.installation.cli("init")
        original = self.installation.credentials_path.read_bytes()
        self.installation.cli("start")
        self.installation.cli("stop")
        process = self.installation.native_start()
        self.assertEqual(self.installation.credentials_path.read_bytes(), original)
        self.assertEqual(self.installation.cli("status")["state"], "running")
        self.installation.native_stop(process)

    def test_foreign_listener_never_adopted_or_stopped(self) -> None:
        class Foreign(BaseHTTPRequestHandler):
            def do_GET(self):
                body = b'{"listener":"foreign-release-acceptance"}'
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            def log_message(self, *_arguments):
                pass
        server = ThreadingHTTPServer(("127.0.0.1", self.installation.port), Foreign)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            self.assertNotEqual(self.installation.launch(expected=None).returncode, 0)
            self.assert_uninitialized()
            self.installation.cli("init")
            original = self.installation.credentials_path.read_bytes()
            before = self.installation.inspect()
            self.assertNotEqual(self.installation.launch(expected=None).returncode, 0)
            self.assertEqual(self.installation.credentials_path.read_bytes(), original)
            status, payload = self.installation.request("/health", authenticated=False)
            self.assertEqual((status, payload["listener"]), (200, "foreign-release-acceptance"))
            self.assertEqual(self.installation.inspect(), before)
            self.assertFalse((self.root / "data/database-service/service.json").exists())
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)

    def test_pending_restore_preserved_and_blocks_launch(self) -> None:
        self.installation.cli("init")
        original = self.installation.credentials_path.read_bytes()
        before = self.installation.inspect()
        marker = self.root / "data/database-service/restore-pending.json"
        marker.write_bytes(b'{"acceptance":"unfinished restore"}\n')
        marker.chmod(0o600)
        self.assertNotEqual(self.installation.launch(expected=None).returncode, 0)
        self.assertEqual(marker.read_bytes(), b'{"acceptance":"unfinished restore"}\n')
        self.assertEqual(self.installation.credentials_path.read_bytes(), original)
        self.assertEqual(self.installation.inspect(), before)
        self.assertFalse((self.root / "data/database-service/service.json").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
