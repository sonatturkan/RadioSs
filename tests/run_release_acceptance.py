#!/usr/bin/env python3
"""Verify, relocate and run both actual RadioS distribution archives.

No host installation, production service, credentials or catalog is modified.
The source archive is built independently in its extracted Unicode/space path.
The report contains checks and archive hashes, never authentication material.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import shutil
import sys
import tarfile
import tempfile
import time
from uuid import uuid4

sys.dont_write_bytecode = True
from test_release_install import Installation, PROJECT, environment, packaging


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def production_markers() -> list[bytes]:
    """Keep private markers in memory solely to prove their exclusion."""
    path = PROJECT / "config/credentials.json"
    if not path.is_file() or path.is_symlink():
        return []
    credentials = json.loads(path.read_text(encoding="utf-8"))
    return [credentials[name].encode() for name in
            ("store_id", "user_id", "station_id", "endpoint_id", "api_token", "endpoint_token")
            if isinstance(credentials.get(name), str) and credentials[name]]


def extract_verified(archive: Path, destination: Path, markers: list[bytes]) -> tuple[Path, dict]:
    summary = packaging.verify(archive)
    require(not destination.exists(), "Acceptance extraction destination must be new")
    destination.mkdir(mode=0o700)
    with tarfile.open(archive, "r:gz") as stream:
        for member in stream:
            parts = PurePosixPath(member.name).parts
            require(parts and parts[0] == "RadioS" and
                    all(part not in {".", ".."} for part in parts) and
                    not PurePosixPath(member.name).is_absolute(), "Unsafe extraction member")
            target = destination.joinpath(*parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                target.chmod(member.mode)
            else:
                require(member.isfile() and member.size <= packaging.MAX_FILE,
                        "Only verified bounded regular files may be extracted")
                source = stream.extractfile(member)
                require(source is not None, "Missing archive member body")
                body = source.read(packaging.MAX_FILE + 1)
                require(len(body) == member.size, "Truncated archive member")
                require(not any(marker in body for marker in markers),
                        "Archive contains a current production identity or credential")
                target.parent.mkdir(parents=True, exist_ok=True)
                with target.open("xb") as output:
                    output.write(body)
                target.chmod(member.mode)
    require(hashlib.sha256(archive.read_bytes()).hexdigest() == summary["sha256"],
            "Archive changed during extraction")
    root = destination / "RadioS"
    require(not (root / "config/credentials.json").exists(), "Archive shipped credentials")
    require(not (root / "config/radios.ini").exists(), "Archive shipped installation-specific INI")
    require(not (root / "config/ui-preferences.ini").exists(), "Archive shipped user preferences")
    require(not any(path.is_file() for path in (root / "data").rglob("*")),
            "Archive shipped existing database, media, logs, backup or recovery data")
    return root, summary


def native_smoke(installation: Installation, label: str) -> dict:
    capture = installation.cwd / (label + " Türkçe pencere.png")
    result = installation.launch("--smoke-test", "--capture", str(capture))
    require("8 profil" in result.stdout, "Native profile acceptance did not complete")
    require(capture.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"),
            "Native window capture is not a PNG")
    return {"profiles": 8, "window_mapped": True, "css_font_assets_loaded": True,
            "capture_sha256": hashlib.sha256(capture.read_bytes()).hexdigest()}


def host_runtime_acceptance(root: Path, cwd: Path) -> dict:
    """Prove SDK-native relaunches share the launching host's owned service."""
    spawn = shutil.which("flatpak-spawn")
    require(bool(spawn) and Path("/.flatpak-info").is_file(),
            "--host-runtime requires flatpak-spawn inside an existing Flatpak environment")
    installation = Installation(root, cwd)
    prefix = [spawn, "--host", "--directory=" + str(cwd)]
    processes = []
    def command(*arguments: str, expected: int | None = 0):
        return installation.command(*prefix, *arguments, expected=expected)
    def service_cli(action: str):
        result = command(str(root / "run-database.sh"), action)
        return json.loads(result.stdout)
    def stop_owned(kind: str) -> None:
        program = """import os,signal,sys
from pathlib import Path
root=sys.argv[1]; kind=sys.argv[2]
for process in Path('/proc').glob('[0-9]*'):
 try:
  args=process.joinpath('cmdline').read_bytes().decode().rstrip('\\0').split('\\0')
  if root not in args: continue
  native=args[0] in (root+'/build/radios',root+'/build/release/radios')
  server=root+'/tools/radios_database.py' in args and 'serve' in args
  if not ((kind=='native' and native) or (kind=='service' and server)): continue
  descriptor=os.pidfd_open(int(process.name))
  try: signal.pidfd_send_signal(descriptor,signal.SIGTERM)
  finally: os.close(descriptor)
 except (OSError,ValueError,UnicodeError,IndexError): continue
"""
        command("python3", "-c", program, str(root), kind)
    def launch() -> subprocess.Popen:
        log = root / "data/logs/database-service.log"
        context_receipt = '"POST /v1/workflows/desktop/context HTTP/1.1" 200'
        before_contexts = log.read_text(encoding="utf-8").count(context_receipt) if log.is_file() else 0
        process = subprocess.Popen([*prefix, str(root / "run-radios.sh")], cwd=cwd,
            env=installation.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        processes.append(process)
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            if process.poll() is not None:
                _out, err = process.communicate()
                raise RuntimeError(installation.redact("Host automatic SDK native launch failed: " + err[-1800:]))
            if installation.credentials_path.exists() and installation.gui_lock_held():
                try:
                    status, health = installation.request("/health", authenticated=False)
                    contexts = log.read_text(encoding="utf-8").count(context_receipt) if log.is_file() else 0
                    if (status == 200 and health["store_id"] == installation.credentials()["store_id"]
                            and contexts > before_contexts):
                        return process
                except (OSError, ValueError):
                    pass
            time.sleep(0.02)
        raise RuntimeError("Host SDK native GUI did not authenticate and load its database context")
    def stop_native(process: subprocess.Popen) -> None:
        stop_owned("native")
        process.communicate(timeout=15)
        require(not installation.gui_lock_held(), "Host SDK native GUI lock did not release")
    try:
        direct = command(str(root / "build/radios"), "--version", expected=None)
        loader_missing = direct.returncode != 0 and (
            "error while loading shared libraries" in direct.stderr or
            (": version " in direct.stderr and "not found" in direct.stderr))
        checked = command(str(root / "run-radios.sh"), "--check-config")
        require(str(root) in checked.stdout, "Host fallback resolved another project")
        require(not installation.credentials_path.exists(), "Host passive fallback initialized credentials")
        explicit = command(str(root / "run-radios-sdk.sh"), "--check-config")
        require(str(root) in explicit.stdout, "Explicit SDK launcher resolved another project")
        capture = cwd / "Host SDK Türkçe pencere.png"
        smoke = command(str(root / "run-radios-sdk.sh"), "--smoke-test", "--capture", str(capture))
        require("8 profil" in smoke.stdout and capture.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"),
                "Host SDK eight-profile native smoke failed")
        require(not installation.credentials_path.exists(), "SDK native smoke initialized credentials")
        first = launch()
        original = installation.credentials_path.read_bytes()
        require(stat.S_IMODE(installation.credentials_path.stat().st_mode) == 0o600,
                "Host first launch credentials are not private")
        first_status = service_cli("status")
        require(first_status["state"] == "running", "The launching host could not recognize its owned service "
                f"(state={first_status.get('state')!r})")
        stop_native(first)
        second = launch()
        require(installation.credentials_path.read_bytes() == original,
                "SDK namespace relaunch changed private credentials")
        require(service_cli("status")["pid"] == first_status["pid"],
                "SDK namespace relaunch duplicated or failed to recognize its service")
        stop_native(second)
        service_cli("stop")
        require(service_cli("status")["state"] == "stopped", "Launching host could not stop its service")
        service_cli("start")
        require(service_cli("status")["state"] == "running", "Launching host could not restart its service")
        require(installation.credentials_path.read_bytes() == original,
                "SDK service restart replaced installation credentials")
        service_cli("stop")
        return {"host_loader_missing": bool(loader_missing), "automatic_fallback": True,
                "explicit_existing_sdk": "org.freedesktop.Sdk//25.08", "profiles": 8,
                "native_first_launch": True, "native_relaunch": True,
                "native_authenticated_context_on_each_launch": True,
                "separate_sdk_native_instances_share_owned_host_service": True,
                "host_service_status_stop_restart": True,
                "private_credentials_preserved": True}
    finally:
        with contextlib.suppress(Exception):
            stop_owned("native")
        with contextlib.suppress(Exception):
            service_cli("stop")
        with contextlib.suppress(Exception):
            stop_owned("service")
        for process in processes:
            with contextlib.suppress(Exception):
                process.communicate(timeout=10)
        installation.close()


def binary_acceptance(root: Path, cwd: Path, markers: list[bytes]) -> dict:
    installation = Installation(root, cwd)
    try:
        version = installation.launch("--version").stdout.strip()
        checked = installation.launch("--check-config")
        require(str(root) in checked.stdout, "Packaged configuration resolved another project")
        require(not installation.credentials_path.exists(), "Read-only launch initialized a database")
        before = native_smoke(installation, "ilk")
        require(not installation.credentials_path.exists(), "Native smoke initialized credentials")
        process = installation.native_start()
        credentials = installation.credentials()
        require(not any(credentials.get(name, "").encode() in markers for name in
                        ("store_id", "station_id", "user_id", "endpoint_id", "api_token", "endpoint_token")),
                "First launch reused a production identity")
        original = installation.credentials_path.read_bytes()
        require(stat.S_IMODE(installation.credentials_path.stat().st_mode) == 0o600,
                "First launch credentials are not private 0600")
        state = installation.inspect()
        require(state["schema_version"] == 4 and state["integrity"] == "ok",
                "Fresh packaged database failed integrity/schema acceptance")
        require(state["endpoint_status"] == ["offline"], "First launch advertised an online endpoint")
        require(state["collections"]["users"] == 1 and state["collections"]["stations"] == 1 and
                state["collections"]["items"] == 0 and state["collections"]["playlists"] == 0,
                "First launch created fixture catalog or duplicate installation identities")
        require(not set(state["config_kinds"]) & {"broadcast_local_run", "advert_local_run"},
                "First launch auto-started a broadcast or advert runtime")
        service_pid = installation.cli("status")["pid"]
        installation.native_stop(process)
        process = installation.native_start()
        require(installation.credentials_path.read_bytes() == original,
                "Repeated launch replaced private credentials")
        require(installation.cli("status")["pid"] == service_pid,
                "Repeated launch duplicated an owned database service")
        installation.native_stop(process)
        installation.cli("stop")
        process = installation.native_start()
        require(installation.credentials_path.read_bytes() == original,
                "Owned service restart changed installation identity")
        installation.native_stop(process)
        installation.cli("init")
        require(installation.credentials_path.read_bytes() == original,
                "Repeated init changed installation identity")

        # Exercise packaged workflow code over real authenticated loopback HTTP.
        sid = credentials["station_id"]
        status, context = installation.request("/v1/workflows/desktop/context", {"station_id": sid})
        require(status == 200 and context["user"]["_id"] == credentials["user_id"],
                "Packaged backend authenticated another identity")
        operation = {"station_id": sid, "kind": "channel", "name": "Taşınmış C kabulü",
                     "config": {"deck_count": 3, "slots": [0, 8, 31]},
                     "idempotency_key": str(uuid4()), "document_id": str(uuid4()), "expected_revision": 0}
        status, saved = installation.request("/v1/workflows/desktop/put", operation)
        require(status == 200 and saved["_id"] == operation["document_id"],
                "Packaged native document workflow did not commit")
        sequence = installation.request("/health", authenticated=False)[1]["sequence"]
        status, replay = installation.request("/v1/workflows/desktop/put", operation)
        require(status == 200 and replay == saved and
                installation.request("/health", authenticated=False)[1]["sequence"] == sequence,
                "Packaged idempotent replay changed committed state")
        changed = {**operation, "name": "Yeni ad", "idempotency_key": str(uuid4()),
                   "expected_revision": saved["_rev"]}
        status, successor = installation.request("/v1/workflows/desktop/put", changed)
        require(status == 200 and successor["_rev"] == saved["_rev"] + 1,
                "Packaged CAS successor did not commit")
        status, _ = installation.request("/v1/workflows/desktop/put",
                                         {**changed, "idempotency_key": str(uuid4())})
        require(status == 409, "Packaged stale CAS request was accepted")
        installation.cli("stop")
        installation.cli("start")
        status, reopened = installation.request("/v1/workflows/desktop/get",
            {"station_id": sid, "kind": "channel", "document_id": operation["document_id"]})
        require(status == 200 and reopened == successor, "Packaged committed document did not survive restart")
        require(installation.inspect()["integrity"] == "ok", "Packaged workflow damaged database integrity")
        installation.cli("stop")
        require(installation.cli("status")["state"] == "stopped", "Packaged service did not stop")
        return {"version": version, "nondefault_loopback_port": True, "fresh_identity": True,
                "private_credentials": True, "repeat_launch_preserves_credentials": True,
                "missing_service_restarted": True, "no_autoplay_or_endpoint_claim": True,
                "real_http_cas_replay_restart": True, "database_integrity": "ok",
                "native_smoke": before}
    finally:
        installation.close()


def source_acceptance(root: Path, cwd: Path, jobs: int) -> dict:
    started = time.monotonic()
    require(not (root / "build/radios").exists(), "Source archive unexpectedly shipped an executable")
    env = environment()
    for command in (["cmake", "-S", str(root), "-B", str(root / "build"),
                     "-DCMAKE_BUILD_TYPE=Release", "-DRADIOS_STRICT_WARNINGS=ON", "-DBUILD_TESTING=OFF"],
                    ["cmake", "--build", str(root / "build"), "--parallel", str(jobs)]):
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True,
                                timeout=240, check=False)
        require(result.returncode == 0, "Relocated source Release build failed: " + result.stderr[-2000:])
    cache = (root / "build/CMakeCache.txt").read_text()
    require("CMAKE_BUILD_TYPE:STRING=Release" in cache and "RADIOS_STRICT_WARNINGS:BOOL=ON" in cache,
            "Source acceptance did not build strict optimized Release")
    installation = Installation(root, cwd)
    try:
        checked = installation.launch("--check-config")
        require(str(root) in checked.stdout, "Fresh source executable resolved the original project")
        smoke = native_smoke(installation, "kaynak")
        require(not installation.credentials_path.exists(), "Source smoke initialized an installation")
        return {"fresh_strict_release_build": True, "elapsed_seconds": round(time.monotonic() - started, 2),
                "version": installation.launch("--version").stdout.strip(), "native_smoke": smoke}
    finally:
        installation.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary-archive", type=Path, required=True)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--report", type=Path, default=PROJECT / "build/stage14-release-acceptance.json")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--host-runtime", action="store_true",
                        help="Also prove SDK native relaunches and launching-host service ownership")
    args = parser.parse_args()
    try:
        require(bool(os.environ.get("WAYLAND_DISPLAY") or os.environ.get("DISPLAY")),
                "Archive acceptance requires a real native display session")
        require(1 <= args.jobs <= 16, "Build parallelism must be in 1..16")
        base = PROJECT / "build/stage14"
        base.mkdir(parents=True, exist_ok=True)
        started = time.monotonic()
        markers = production_markers()
        with tempfile.TemporaryDirectory(prefix="nihai-kabul-", dir=base) as temporary:
            temporary = Path(temporary)
            cwd = temporary / "Başka çalışma dizini"
            cwd.mkdir()
            binary_root, binary = extract_verified(args.binary_archive.resolve(strict=True),
                temporary / "İkili taşınmış kaynak kodlar", markers)
            source_root, source = extract_verified(args.source_archive.resolve(strict=True),
                temporary / "Kaynak taşınmış çalışma alanı", markers)
            require(binary["kind"] == "binary" and source["kind"] == "source" and
                    binary["version"] == source["version"], "Archives are not matching binary/source releases")
            host = None
            if args.host_runtime:
                host_root, _ = extract_verified(args.binary_archive.resolve(strict=True),
                    temporary / "Host taşınmış Türkçe kaynak", markers)
                host = host_runtime_acceptance(host_root, cwd)
            report = {"format": 1, "success": True, "version": binary["version"],
                "binary_archive": binary, "source_archive": source,
                "unicode_space_relocation": True, "independent_working_directory": True,
                "production_identity_and_data_excluded": True,
                "binary_acceptance": binary_acceptance(binary_root, cwd, markers),
                "source_acceptance": source_acceptance(source_root, cwd, args.jobs),
                "elapsed_seconds": round(time.monotonic() - started, 2)}
            if host is not None:
                report["host_runtime_acceptance"] = host
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 0
    except (RuntimeError, OSError, ValueError, KeyError, TypeError, AssertionError, packaging.PackageError,
            subprocess.SubprocessError, tarfile.TarError) as exc:
        print("Clean archive acceptance failed: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
