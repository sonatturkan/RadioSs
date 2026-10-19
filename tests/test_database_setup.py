"""Acceptance tests for RadioS's project-local database lifecycle CLI.

Run with ``python3 -m unittest discover -s tests -p test_database_setup.py``.
Every project, database, credential, log and child process belongs to a fixture
under RadioS/build. No installed server or real RadioS catalog is modified.
"""

from __future__ import annotations

import configparser
import contextlib
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from uuid import UUID, uuid4


PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT / "build"
_USED_PORTS: set[int] = set()


def unused_port() -> int:
    """Each fixture gets a distinct ephemeral loopback port."""
    for _ in range(100):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        if port not in _USED_PORTS:
            _USED_PORTS.add(port)
            return port
    raise RuntimeError("Could not allocate a distinct fixture port")


class ForeignHandler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        body = json.dumps({"listener": "unrelated-test-server"}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args: object) -> None:
        pass


class DatabaseSetupTests(unittest.TestCase):
    def setUp(self) -> None:
        BUILD.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="database-cli-", dir=BUILD)
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "Türkçe kaynak kodlar RadioS"
        (self.root / "config").mkdir(parents=True)
        (self.root / "database" / "SDBASE").mkdir(parents=True)
        (self.root / "tools").mkdir()
        shutil.copy2(PROJECT / "CMakeLists.txt", self.root / "CMakeLists.txt")
        shutil.copy2(PROJECT / "config" / "radios.example.ini",
                     self.root / "config" / "radios.example.ini")
        shutil.copy2(PROJECT / "database" / "SDBASE" / "pyproject.toml",
                     self.root / "database" / "SDBASE" / "pyproject.toml")
        shutil.copytree(PROJECT / "database" / "SDBASE" / "src",
                        self.root / "database" / "SDBASE" / "src",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        for source in (PROJECT / "tools").glob("radios_database*.py"):
            shutil.copy2(source, self.root / "tools" / source.name)
        self.cli = self.root / "tools" / "radios_database.py"
        self.assertTrue(self.cli.is_file(), "Stage 5 database CLI has not been created")
        self.port = unused_port()
        self.config_path = self.root / "config" / "radios.ini"
        self.config = configparser.ConfigParser(interpolation=None)
        self.config.read(PROJECT / "config" / "radios.example.ini", encoding="utf-8")
        self.config["database"]["service_url"] = f"http://127.0.0.1:{self.port}"
        self.config["database"]["connect_timeout_seconds"] = "1"
        self.config["database"]["request_timeout_seconds"] = "3"
        self.write_config()
        self.database = self.root / "data" / "database" / "RadioS.sdb"
        self.credentials_path = self.root / "config" / "credentials.json"
        self.environment = os.environ.copy()
        self.environment.pop("RADIOS_SDBASE_TOKEN", None)
        self.environment.pop("PYTHONPATH", None)
        self.environment["PYTHONDONTWRITEBYTECODE"] = "1"
        self.addCleanup(self.cleanup_service)

    def write_config(self) -> None:
        with self.config_path.open("w", encoding="utf-8") as stream:
            self.config.write(stream)

    def credentials(self) -> dict:
        return json.loads(self.credentials_path.read_text(encoding="utf-8"))

    def assert_no_token_output(self, result: subprocess.CompletedProcess[str]) -> None:
        if not self.credentials_path.is_file():
            return
        try:
            credentials = self.credentials()
        except (ValueError, OSError):
            return
        for field in ("api_token", "endpoint_token"):
            token = credentials.get(field)
            if isinstance(token, str) and token:
                # Avoid assertNotIn: a failing assertion must not print a token.
                self.assertFalse(token in result.stdout or token in result.stderr,
                                 "Database CLI exposed a raw credential")

    def command(self, action: str, *arguments: str, success: bool | None = True,
                config: str | None = None) -> tuple[subprocess.CompletedProcess[str], dict | None]:
        command = [sys.executable, str(self.cli), "--project-root", str(self.root)]
        if config is not None:
            command.extend(("--config", config))
        command.extend((action, *arguments))
        result = subprocess.run(command, cwd=self.base, env=self.environment,
                                capture_output=True, text=True, timeout=25, check=False)
        self.assert_no_token_output(result)
        if success is True:
            self.assertEqual(result.returncode, 0,
                             f"{action} failed: {result.stderr[:1000]}")
        elif success is False:
            self.assertNotEqual(result.returncode, 0, f"{action} accepted invalid state")
            self.assertTrue(result.stderr.strip(), "Failure should explain itself on stderr")
        payload = None
        if result.stdout.strip():
            payload = json.loads(result.stdout)
            self.assertIsInstance(payload, dict, "CLI stdout must be one JSON object")
        if success is True:
            self.assertIsNotNone(payload, "Successful CLI commands return JSON")
        return result, payload

    def initialize(self, *arguments: str) -> dict:
        _result, payload = self.command("init", *arguments)
        assert payload is not None
        self.assertEqual(payload["schema_version"], 4)
        self.assertEqual(payload["endpoint_status"], "offline")
        for key in ("store_id", "user_id", "station_id", "endpoint_id"):
            self.assertEqual(str(UUID(payload[key])), payload[key])
        return payload

    def backend(self, program: str) -> dict:
        environment = self.environment.copy()
        environment["PYTHONPATH"] = str(self.root / "database" / "SDBASE" / "src")
        result = subprocess.run([sys.executable, "-c", program, str(self.database)],
                                cwd=self.root, env=environment, capture_output=True,
                                text=True, timeout=15, check=False)
        self.assertEqual(result.returncode, 0, result.stderr[:1000])
        return json.loads(result.stdout)

    def inspect_database(self) -> dict:
        return self.backend(
            "import json,sys; from sdbase.schema import open_radio_store; "
            "store=open_radio_store(sys.argv[1],read_only=True); "
            "collections=('users','stations','endpoints','items','versions','playlists','tokens'); "
            "print(json.dumps({'schema_version':store.schema_version,"
            "'sequence':store.sequence, 'records':{name:store.scan(name) for name in collections}}))"
        )

    def request(self, path: str, *, token: str | None = None,
                body: dict | None = None) -> tuple[int, dict | list]:
        headers = {}
        if token is not None:
            headers["Authorization"] = "Bearer " + token
        encoded = None
        if body is not None:
            encoded = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            connection.request("POST" if body is not None else "GET", path,
                               body=encoded, headers=headers)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def cleanup_service(self) -> None:
        # Stop first through the production ownership checks. If a test failed
        # during startup, also reap processes whose argv belongs to this exact
        # isolated fixture. Never use a port alone as a process identity.
        with contextlib.suppress(Exception):
            subprocess.run([sys.executable, str(self.cli), "--project-root", str(self.root), "stop"],
                           cwd=self.base, env=self.environment, capture_output=True,
                           timeout=12, check=False)
        candidates = []
        for process in Path("/proc").glob("[0-9]*"):
            try:
                arguments = process.joinpath("cmdline").read_bytes().decode().split("\0")
                local = any(arg == str(self.root) or arg.startswith(str(self.root) + os.sep)
                            for arg in arguments)
                server = any("radios_database" in arg or arg == "sdbase.cli" for arg in arguments)
                if local and server:
                    candidates.append(int(process.name))
            except (OSError, UnicodeError, ValueError):
                continue
        for pid in candidates:
            with contextlib.suppress(ProcessLookupError):
                os.kill(pid, signal.SIGTERM)
        deadline = time.monotonic() + 3
        def remains_owned(pid: int) -> bool:
            try:
                return str(self.root).encode() in Path(f"/proc/{pid}/cmdline").read_bytes()
            except OSError:
                return False
        while candidates and time.monotonic() < deadline:
            candidates = [pid for pid in candidates if remains_owned(pid)]
            if candidates:
                time.sleep(0.05)
        for pid in candidates:
            with contextlib.suppress(ProcessLookupError):
                os.kill(pid, signal.SIGKILL)

    def test_init_private_real_schema_and_offline_endpoint(self) -> None:
        summary = self.initialize("--username", "operator", "--station-code", "TESTFM",
                                  "--station-name", "Türkçe Radyo", "--timezone", "UTC")
        self.assertTrue(self.database.is_file())
        self.assertNotEqual(self.database.read_bytes()[:16], b"SQLite format 3\0")
        self.assertEqual(stat.S_IMODE(self.credentials_path.stat().st_mode), 0o600)
        credentials = self.credentials()
        for key in ("store_id", "user_id", "station_id", "endpoint_id"):
            self.assertEqual(credentials[key], summary[key])
        self.assertGreaterEqual(len(credentials["api_token"]), 20)
        self.assertGreaterEqual(len(credentials["endpoint_token"]), 20)
        records = self.inspect_database()["records"]
        self.assertEqual(len(records["users"]), 1)
        self.assertEqual(records["users"][0]["username"], "operator")
        self.assertEqual(records["stations"][0]["code"], "TESTFM")
        self.assertEqual(len(records["stations"]), 1)
        self.assertEqual(records["stations"][0]["name"], "Türkçe Radyo")
        self.assertEqual(records["endpoints"][0]["status"], "offline")
        self.assertEqual(len(records["endpoints"]), 1)
        self.assertEqual(records["endpoints"][0]["_id"], summary["endpoint_id"])
        self.assertEqual(records["items"], [])
        self.assertEqual(records["versions"], [])
        self.assertEqual(records["playlists"], [])

    def test_repeated_init_preserves_credentials_ids_and_rows(self) -> None:
        first = self.initialize()
        credential_bytes = self.credentials_path.read_bytes()
        before = self.inspect_database()
        second = self.initialize()
        for key in ("store_id", "user_id", "station_id", "endpoint_id"):
            self.assertEqual(first[key], second[key])
        self.assertEqual(self.credentials_path.read_bytes(), credential_bytes)
        self.assertEqual(self.inspect_database(), before)

    def test_unmanaged_valid_database_is_preserved_and_rejected(self) -> None:
        self.backend("import json,sys; from sdbase.schema import open_radio_store; "
                     "store=open_radio_store(sys.argv[1]); print(json.dumps({'schema_version':store.schema_version}))")
        before = self.database.read_bytes()
        self.command("init", success=False)
        self.assertEqual(self.database.read_bytes(), before)
        self.assertFalse(self.credentials_path.exists())

    def test_corrupt_unmanaged_database_is_preserved_and_rejected(self) -> None:
        self.database.parent.mkdir(parents=True)
        sentinel = b"unmanaged invalid database\n"
        self.database.write_bytes(sentinel)
        self.command("init", success=False)
        self.command("start", success=False)
        self.assertEqual(self.database.read_bytes(), sentinel)
        self.assertFalse(self.credentials_path.exists())

    def test_corrupt_and_mismatched_credentials_are_not_replaced(self) -> None:
        self.initialize()
        original = self.credentials_path.read_bytes()
        database_before = self.database.read_bytes()
        self.credentials_path.write_text("{corrupt credential file", encoding="utf-8")
        self.command("init", success=False)
        self.command("start", success=False)
        self.assertEqual(self.credentials_path.read_text(), "{corrupt credential file")
        self.credentials_path.write_bytes(original)
        credentials = self.credentials()
        credentials["store_id"] = str(uuid4())
        self.credentials_path.write_text(json.dumps(credentials), encoding="utf-8")
        mismatch = self.credentials_path.read_bytes()
        self.command("init", success=False)
        self.command("start", success=False)
        self.assertEqual(self.credentials_path.read_bytes(), mismatch)
        self.assertEqual(self.database.read_bytes(), database_before)

    def test_public_and_symlinked_credentials_are_rejected(self) -> None:
        self.initialize()
        original = self.credentials_path.read_bytes()
        self.credentials_path.chmod(0o644)
        self.command("init", success=False)
        self.command("start", success=False)
        self.assertEqual(self.credentials_path.read_bytes(), original)
        self.assertEqual(stat.S_IMODE(self.credentials_path.stat().st_mode), 0o644)
        self.credentials_path.chmod(0o600)
        outside = self.base / "external-credentials.json"
        self.credentials_path.rename(outside)
        self.credentials_path.symlink_to(outside)
        self.command("init", success=False)
        self.command("start", success=False)
        self.assertTrue(self.credentials_path.is_symlink())
        self.assertEqual(outside.read_bytes(), original)

    def test_config_file_and_database_path_escape_are_rejected(self) -> None:
        outside_config = self.base / "outside.ini"
        shutil.copy2(self.config_path, outside_config)
        self.command("init", success=False, config="../outside.ini")
        self.config["paths"]["database_file"] = "../outside.sdb"
        self.write_config()
        self.command("init", success=False)
        self.assertFalse((self.base / "outside.sdb").exists())
        self.assertFalse(self.credentials_path.exists())

    def test_symlink_database_escape_is_rejected(self) -> None:
        outside = self.base / "outside-database"
        outside.mkdir()
        self.database.parent.parent.mkdir(parents=True)
        self.database.parent.symlink_to(outside, target_is_directory=True)
        self.command("init", success=False)
        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse(self.credentials_path.exists())

    def test_nonloopback_service_urls_are_rejected(self) -> None:
        for host in ("0.0.0.0", "192.0.2.1", "example.invalid", "127.0.0.1.example.invalid"):
            with self.subTest(host=host):
                self.config["database"]["service_url"] = f"http://{host}:{self.port}"
                self.write_config()
                self.command("init", success=False)
                self.command("start", success=False)
                self.assertFalse(self.database.exists())

    def test_http_health_identity_and_protected_authorization(self) -> None:
        initialized = self.initialize()
        self.command("start")
        status, health = self.request("/health")
        self.assertEqual(status, 200)
        self.assertEqual(health["status"], "ok")
        self.assertEqual(health["store_id"], initialized["store_id"])
        for token in (None, "wrong-credential-long-enough-for-validation"):
            status, _error = self.request("/v1/capabilities", token=token)
            self.assertEqual(status, 403)
        status, capabilities = self.request("/v1/capabilities", token=self.credentials()["api_token"])
        self.assertEqual(status, 200)
        self.assertEqual(capabilities["schema_version"], 4)
        self.assertEqual(capabilities["database_format"], ".sdb")
        self.assertEqual(capabilities["desktop_api_version"], 1)
        self.assertIn("weekly_programme", capabilities["desktop_configuration_kinds"])
        self.assertRegex(capabilities["source_digest"], r"^[a-f0-9]{64}$")

    def test_owned_service_restarts_after_backend_source_change(self) -> None:
        self.initialize()
        credentials = self.credentials_path.read_bytes()
        database = self.inspect_database()
        _result, started = self.command("start")
        status, old = self.request("/v1/capabilities", token=self.credentials()["api_token"])
        self.assertEqual(status, 200)
        source = self.root / "database/SDBASE/src/sdbase/desktop_weekly_programme.py"
        source.write_text(source.read_text() + "\n# Fixture code update.\n")
        # The advertised digest stays bound to the process's startup snapshot.
        status, still_old = self.request("/v1/capabilities", token=self.credentials()["api_token"])
        self.assertEqual(status, 200)
        self.assertEqual(still_old["source_digest"], old["source_digest"])
        error, _payload = self.command("check", success=False)
        self.assertIn("eski kaynak kod", error.stderr)
        _result, updated = self.command("start")
        self.assertTrue(updated["restarted"])
        self.assertFalse(updated["already_running"])
        self.assertNotEqual(updated["pid"], started["pid"])
        self.assertNotEqual(updated["source_digest"], old["source_digest"])
        _result, again = self.command("start")
        self.assertEqual(again["pid"], updated["pid"])
        self.assertTrue(again["already_running"])
        self.assertFalse(again["restarted"])
        self.assertEqual(self.credentials_path.read_bytes(), credentials)
        self.assertEqual(self.inspect_database(), database)

    def test_start_status_stop_are_idempotent_and_preserve_database(self) -> None:
        self.initialize()
        first_credentials = self.credentials_path.read_bytes()
        before = self.inspect_database()
        self.command("start")
        _result, first = self.command("status")
        self.assertEqual(first["state"], "running")
        self.command("start")
        _result, second = self.command("status")
        self.assertEqual(second["state"], "running")
        if "pid" in first and "pid" in second:
            self.assertEqual(first["pid"], second["pid"])
        self.command("stop")
        _result, stopped = self.command("status")
        self.assertEqual(stopped["state"], "stopped")
        self.command("stop")
        self.assertEqual(self.inspect_database(), before)
        self.assertEqual(self.credentials_path.read_bytes(), first_credentials)

    def test_authenticated_check_and_integrity(self) -> None:
        self.initialize()
        self.command("start")
        _result, check = self.command("check")
        self.assertEqual(check["status"], "ok")
        status, report = self.request("/v1/operations/integrity", token=self.credentials()["api_token"])
        self.assertEqual(status, 200)
        self.assertEqual(report["status"], "ok")
        self.assertTrue(report["complete"])

    def test_authenticated_real_write_survives_service_restart(self) -> None:
        initialized = self.initialize()
        self.command("start")
        credentials = self.credentials()
        body = {"station_id": initialized["station_id"], "code": "STAGE5-ACCEPT",
                "name": "Kalıcı entegrasyon denemesi"}
        for token in (None, "wrong-credential-long-enough-for-validation"):
            status, _error = self.request("/v1/workflows/playlists/create", token=token, body=body)
            self.assertEqual(status, 403)
        status, created = self.request("/v1/workflows/playlists/create",
                                       token=credentials["api_token"], body=body)
        self.assertEqual(status, 200)
        self.assertEqual(created["version_no"], 1)
        self.command("stop")
        self.command("start")
        status, playlists = self.request("/v1/workflows/playlists/list", token=credentials["api_token"],
                                         body={"station_id": initialized["station_id"]})
        self.assertEqual(status, 200)
        self.assertEqual(len(playlists), 1)
        self.assertEqual(playlists[0]["_id"], created["playlist_id"])
        self.assertEqual(playlists[0]["name"], body["name"])
        status, versions = self.request("/v1/workflows/playlists/versions", token=credentials["api_token"],
                                        body={"station_id": initialized["station_id"],
                                              "playlist_id": created["playlist_id"]})
        self.assertEqual(status, 200)
        self.assertEqual(len(versions), 1)
        self.assertEqual(versions[0]["_id"], created["draft_version_id"])
        self.assertEqual(versions[0]["status"], "draft")
        self.assertEqual(versions[0]["entries"], [])
        self.command("stop")
        records = self.inspect_database()["records"]
        self.assertEqual(len(records["playlists"]), 1)
        self.assertEqual(records["items"], [])

    def test_foreign_listener_start_refused_and_stop_does_not_kill_it(self) -> None:
        self.initialize()
        listener = ThreadingHTTPServer(("127.0.0.1", self.port), ForeignHandler)
        listener.daemon_threads = True
        thread = threading.Thread(target=listener.serve_forever, daemon=True)
        thread.start()
        def close_listener() -> None:
            listener.shutdown()
            listener.server_close()
            thread.join(timeout=3)
        self.addCleanup(close_listener)
        self.command("start", success=False)
        self.command("stop", success=None)
        status, body = self.request("/health")
        self.assertEqual(status, 200)
        self.assertEqual(body, {"listener": "unrelated-test-server"})
        self.assertTrue(thread.is_alive())


if __name__ == "__main__":
    unittest.main(verbosity=2)
