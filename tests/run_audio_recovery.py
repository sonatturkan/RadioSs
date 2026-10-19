#!/usr/bin/env python3
"""Restart only an isolated Pulse daemon; never control the host audio service."""
from __future__ import annotations

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

PROJECT = Path(__file__).resolve().parents[1]
PIPEWIRE_PROGRAMS = ("pipewire", "wireplumber", "pipewire-pulse")


def host_prefix() -> list[str] | None:
    """Where PipeWire's programs run: directly, or on the host through
    flatpak-spawn when the tests themselves run inside a sandbox."""
    if all(shutil.which(name) for name in PIPEWIRE_PROGRAMS):
        return []
    if shutil.which("flatpak-spawn"):
        probe = subprocess.run(["flatpak-spawn", "--host", "which", *PIPEWIRE_PROGRAMS],
                               capture_output=True, timeout=15, check=False)
        if probe.returncode == 0:
            return ["flatpak-spawn", "--host"]
    return None


def pipewire_main(executable: Path, prefix: list[str]) -> int:
    """Without pulseaudio: an owned PipeWire instance (no D-Bus, no hardware
    monitors, its own runtime/config/state directories) whose pulse server is
    stopped and restarted on the test's requests. Only processes started here
    are signalled, by PID; the user's and shared sound servers are untouched."""
    if not shutil.which("pactl"):
        print("Private PipeWire restart test requires pactl.", file=sys.stderr)
        return 77
    cache = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache")
    cache.mkdir(parents=True, exist_ok=True)
    # Short and inside the home directory: the socket path stays under the
    # unix socket limit and is reachable from a sandbox and from the host.
    root = Path(tempfile.mkdtemp(prefix="radios-ar-", dir=cache))
    run_dir, logs = root / "run", root / "logs"
    for directory in (run_dir, logs, root / "state", root / "data",
                      root / "config/pipewire/pipewire.conf.d", root / "config/wireplumber/wireplumber.conf.d"):
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    (root / "config/pipewire/pipewire.conf.d/90-radios-test.conf").write_text(
        "context.properties = {\n    support.dbus = false\n}\n", encoding="utf-8")
    (root / "config/wireplumber/wireplumber.conf.d/90-radios-test.conf").write_text(
        "wireplumber.profiles = {\n  radiostest = {\n    inherits = [ policy, mixin.systemwide-session ]\n  }\n}\n",
        encoding="utf-8")
    quote = lambda path: shlex.quote(str(path))
    clean = (f"env -i HOME={quote(Path.home())} PATH=/usr/bin:/bin LANG=C.UTF-8 XDG_RUNTIME_DIR={quote(run_dir)} "
             f"PIPEWIRE_RUNTIME_DIR={quote(run_dir)} XDG_CONFIG_HOME={quote(root / 'config')} "
             f"XDG_STATE_HOME={quote(root / 'state')} XDG_DATA_HOME={quote(root / 'data')}")
    server = "unix:" + str(run_dir / "pulse" / "native")
    environment = os.environ.copy()
    for key in ("PULSE_RUNTIME_PATH", "PULSE_STATE_PATH", "PULSE_COOKIE"):
        environment.pop(key, None)
    environment["RADIOS_TEST_PULSE_SERVER"] = server
    environment["PULSE_SERVER"] = server
    pids: dict[str, str] = {}

    def spawn(name: str, *arguments: str) -> str:
        command = (f"setsid {clean} {name} {' '.join(map(shlex.quote, arguments))} "
                   f"> {quote(logs / (name + '.log'))} 2>&1 < /dev/null & echo $!")
        return subprocess.run(prefix + ["sh", "-c", command], capture_output=True, text=True,
                              timeout=15, check=True).stdout.strip()

    def alive(pid: str) -> bool:
        return subprocess.run(prefix + ["kill", "-0", pid], capture_output=True, timeout=10, check=False).returncode == 0

    def terminate(name: str) -> None:
        pid = pids.pop(name, None)
        if not pid or not pid.isdigit():
            return
        subprocess.run(prefix + ["kill", pid], capture_output=True, timeout=10, check=False)
        deadline = time.monotonic() + 5
        while alive(pid) and time.monotonic() < deadline:
            time.sleep(0.05)
        if alive(pid):
            subprocess.run(prefix + ["kill", "-9", pid], capture_output=True, timeout=10, check=False)

    def start(missing: bool = False) -> None:
        pids["pipewire-pulse"] = spawn("pipewire-pulse")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if subprocess.run(["pactl", "--server", server, "info"], capture_output=True,
                              timeout=3, check=False).returncode == 0:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("Private PipeWire pulse server did not become ready")
        for sink in (["fallback_only"] if missing else ["radios_recovery_main", "radios_recovery_cue"]):
            subprocess.run(["pactl", "--server", server, "load-module", "module-null-sink", "sink_name=" + sink,
                            "rate=48000", "channels=2"], capture_output=True, timeout=10, check=True)

    test_process = None
    try:
        pids["pipewire"] = spawn("pipewire")
        deadline = time.monotonic() + 10
        while not (run_dir / "pipewire-0").exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        if not (run_dir / "pipewire-0").exists():
            raise RuntimeError("Private PipeWire core did not start")
        pids["wireplumber"] = spawn("wireplumber", "-p", "radiostest")
        start()
        test_process = subprocess.Popen([str(executable), str(root), *sys.argv[2:]], env=environment)
        handled: set[str] = set()
        deadline = time.monotonic() + 120
        while test_process.poll() is None and time.monotonic() < deadline:
            for number in (1, 2, 3):
                request = f"stop-{number}"
                if (root / request).is_file() and request not in handled:
                    terminate("pipewire-pulse")
                    handled.add(request)
                    (root / f"stopped-{number}").write_text("stopped")
            for name in ("restart-1", "restart-missing", "restart-3"):
                if (root / name).is_file() and name not in handled:
                    start(missing=name == "restart-missing")
                    handled.add(name)
                    (root / name.replace("restart", "restarted", 1)).write_text("ready")
            time.sleep(0.01)
        if test_process.poll() is None:
            raise RuntimeError("Private PipeWire restart acceptance timed out")
        return test_process.returncode
    except Exception as error:
        print(str(error), file=sys.stderr)
        for log in sorted(logs.glob("*.log")):
            print(log.name, log.read_text(encoding="utf-8", errors="replace")[-2000:], file=sys.stderr)
        return 1
    finally:
        if test_process and test_process.poll() is None:
            test_process.terminate()
            test_process.wait(timeout=5)
        for name in ("pipewire-pulse", "wireplumber", "pipewire"):
            terminate(name)
        shutil.rmtree(root, ignore_errors=True)

def main() -> int:
    if len(sys.argv) < 2:
        return 2
    executable = Path(sys.argv[1]).resolve(strict=True)
    daemon = os.environ.get("RADIOS_TEST_PULSEAUDIO_BIN") or shutil.which("pulseaudio")
    if not daemon:
        candidate = PROJECT / "build/stage13-pulse/pulseaudio"
        daemon = str(candidate) if candidate.is_file() else None
    if not daemon:
        prefix = host_prefix()
        if prefix is not None:
            return pipewire_main(executable, prefix)
        print("Private Pulse restart test requires pulseaudio, RADIOS_TEST_PULSEAUDIO_BIN or PipeWire.", file=sys.stderr)
        return 77
    environment = os.environ.copy()
    daemon_process = test_process = None
    with tempfile.TemporaryDirectory(prefix="audio-recovery-", dir=PROJECT / "build") as temporary:
        root = Path(temporary)
        (root / "runtime").mkdir(mode=0o700)
        (root / "state").mkdir(mode=0o700)
        directory_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
        # libpulse's server-list parser treats spaces as separators. A short
        # procfs alias still resolves to this project-private directory and
        # supports the real workspace's Turkish characters and spaces.
        socket = Path(f"/proc/{os.getpid()}/fd/{directory_fd}/native")
        server = "unix:" + str(socket)
        environment["PULSE_RUNTIME_PATH"] = str(root / "runtime")
        environment["PULSE_STATE_PATH"] = str(root / "state")
        environment["RADIOS_TEST_PULSE_SERVER"] = server
        environment["PULSE_SERVER"] = server
        daemon_environment = environment.copy()
        # An isolated server never acquires a name on the user's desktop bus.
        daemon_environment["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path=/proc/{os.getpid()}/fd/{directory_fd}/no-session-bus"
        module_path = os.environ.get("RADIOS_TEST_PULSE_MODULE_PATH")
        prefix_modules = PROJECT / "build/stage13-pulse/usr/lib/pulse-17.0/modules"
        if not module_path and prefix_modules.is_dir():
            module_path = str(prefix_modules)
        def start(missing: bool = False) -> subprocess.Popen:
            config = root / "private.pa"
            socket_argument = str(socket).replace("\\", "\\\\").replace('"', '\\"')
            lines = [f'load-module module-native-protocol-unix socket="{socket_argument}" auth-anonymous=1']
            for sink in (["fallback_only"] if missing else ["radios_recovery_main", "radios_recovery_cue"]):
                lines.append(f"load-module module-null-sink sink_name={sink} rate=48000 channels=2")
            config.write_text("\n".join(lines) + "\n", encoding="utf-8")
            args = [daemon, "--daemonize=no", "--exit-idle-time=-1", "--use-pid-file=no",
                    "--disable-shm=yes", "--high-priority=no", "--realtime=no", "--log-target=stderr", "-n", "--file=" + str(config)]
            if module_path:
                args.append("--dl-search-path=" + module_path)
            process = subprocess.Popen(args, env=daemon_environment, stdout=subprocess.DEVNULL, stderr=log)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError("Private Pulse daemon exited before readiness")
                status = subprocess.run(["pactl", "--server", server, "info"], env=environment,
                                        capture_output=True, timeout=2, check=False)
                if not status.returncode:
                    return process
                time.sleep(0.05)
            process.terminate()
            process.wait(timeout=5)
            raise RuntimeError("Private Pulse daemon did not become ready")
        def stop(process: subprocess.Popen) -> None:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
        with (root / "pulse.log").open("wb") as log:
            try:
                daemon_process = start()
                test_process = subprocess.Popen([str(executable), str(root), *sys.argv[2:]], env=environment)
                handled: set[str] = set()
                deadline = time.monotonic() + 90
                while test_process.poll() is None and time.monotonic() < deadline:
                    for number in (1, 2, 3):
                        request = f"stop-{number}"
                        if (root / request).is_file() and request not in handled:
                            stop(daemon_process)
                            handled.add(request)
                            (root / f"stopped-{number}").write_text("stopped")
                    for name in ("restart-1", "restart-missing", "restart-3"):
                        if (root / name).is_file() and name not in handled:
                            daemon_process = start(missing=name == "restart-missing")
                            handled.add(name)
                            (root / name.replace("restart", "restarted", 1)).write_text("ready")
                    time.sleep(0.01)
                if test_process.poll() is None:
                    test_process.terminate()
                    test_process.wait(timeout=5)
                    raise RuntimeError("Private Pulse restart acceptance timed out")
                return test_process.returncode
            except Exception as error:
                print(str(error), file=sys.stderr)
                print((root / "pulse.log").read_text(encoding="utf-8", errors="replace")[-8000:], file=sys.stderr)
                return 1
            finally:
                if test_process and test_process.poll() is None:
                    test_process.terminate()
                    test_process.wait(timeout=5)
                if daemon_process:
                    stop(daemon_process)
                os.close(directory_fd)

if __name__ == "__main__":
    raise SystemExit(main())
