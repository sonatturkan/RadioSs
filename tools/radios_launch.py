#!/usr/bin/env python3
"""Prepare this installation's database, then exec the native C desktop.

Read-only CLI modes never initialize or start a database. Normal launches
validate the native configuration before touching installation state. Existing
credentials, foreign ports and interrupted restore operations are preserved by
the same serialized service helpers used by run-database.sh.
"""
from __future__ import annotations

import argparse
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True


def build_current_sources(root: Path, *, force_sdk: bool = False) -> None:
    """Refresh the source installation's canonical binary before launching it."""
    if not (root / "src/app/main.c").is_file() or not (root / "CMakeLists.txt").is_file():
        return
    if os.environ.get("RADIOS_SKIP_BUILD"):
        print("RadioS: RADIOS_SKIP_BUILD ayarlı; mevcut build/radios derlenmeden açılıyor.", file=sys.stderr, flush=True)
        return
    cmake = shutil.which("cmake")
    pkg_config = shutil.which("pkg-config")
    dependencies = (
        "glib-2.0 >= 2.66", "gio-2.0 >= 2.66", "gtk+-3.0 >= 3.24",
        "fontconfig", "json-glib-1.0 >= 1.10", "libcurl >= 7.85",
        "libavformat >= 59.27.100", "libavcodec >= 59.37.100",
        "libavutil >= 57.28.100", "libswresample >= 4.7.100",
        "libavfilter >= 8.44.100", "libpulse >= 15.0",
    )
    native_tools = cmake and pkg_config and shutil.which("cc")
    native_ready = native_tools and subprocess.run(
        [pkg_config, "--exists", *dependencies], check=False,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    if native_ready and not force_sdk:
        command = [cmake]
    else:
        from radios_sdk import sdk_command
        command = sdk_command(root, Path("/usr/bin/cmake"), [])
    build = root / "build"
    binary = build / "radios"
    # Concurrent launches must not configure/link the same binary together.
    with (root / "default-binary.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        print("RadioS: Güncel kaynaklar derleniyor...", file=sys.stderr, flush=True)
        configure = [*command, "-S", str(root), "-B", str(build)]
        if not (build / "CMakeCache.txt").is_file():
            configure += ["-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF"]
        try:
            subprocess.run(configure, check=True, stdout=sys.stderr, cwd=root)
            subprocess.run([*command, "--build", str(build), "--target", "radios",
                            "--parallel", str(min(4, os.cpu_count() or 1))],
                           check=True, stdout=sys.stderr, cwd=root)
        except subprocess.CalledProcessError as exc:
            # A broken checkout must not keep the station off the air: a failed
            # configure/build leaves the previously linked binary untouched, so
            # that one opens with a warning. Without any binary the error stands.
            if not (binary.is_file() and os.access(binary, os.X_OK)):
                raise
            print(f"RadioS: Derleme başarısız (çıkış kodu {exc.returncode}); kaynaklar düzeltilene kadar "
                  "son sağlam build/radios açılıyor.", file=sys.stderr, flush=True)


def main(argv: list[str] | None = None, *, force_sdk: bool = False) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--config")
    options, _ = parser.parse_known_args(arguments)
    try:
        root = options.project_root.resolve(strict=True)
        passive = any(flag in arguments for flag in (
            "--version", "--help", "-h", "--help-all", "--check-config", "--smoke-test"))
        if not passive:
            build_current_sources(root, force_sdk=force_sdk)
        binary = root / "build/radios"
        if not binary.is_file() or not os.access(binary, os.X_OK):
            binary = root / "build/release/radios"
        if not binary.is_file() or not os.access(binary, os.X_OK):
            print("RadioS: C uygulaması bulunamadı. Kaynak paketini önce CMake ile derleyin.", file=sys.stderr)
            return 1
        native = [str(binary), *arguments]
        runtime = subprocess.run([str(binary), "--version"], capture_output=True,
                                 timeout=10, check=False)
        missing_runtime = runtime.returncode and (
            b"error while loading shared libraries" in runtime.stderr or
            (b": version " in runtime.stderr and b"not found" in runtime.stderr))
        if missing_runtime or force_sdk:
            from radios_sdk import sdk_command
            native = sdk_command(root, binary, arguments)
        if passive:
            os.execv(native[0], native)
        if not os.environ.get("WAYLAND_DISPLAY") and not os.environ.get("DISPLAY"):
            print("RadioS: Yerel Wayland/X11 masaüstü oturumu gerekli.", file=sys.stderr)
            return 1
        # Unknown options, invalid paths and broken INI files must fail before
        # setup. The executable owns the authoritative native option parser.
        checked = subprocess.run([*native, "--check-config"], capture_output=True,
                                 timeout=15, check=False)
        if checked.returncode:
            sys.stderr.buffer.write(checked.stderr)
            return checked.returncode if checked.returncode > 0 else 1
        import radios_database_setup as setup
        import radios_database as service
        cfg = setup.load_config(root, options.config)
        os.umask(0o077)
        with setup.operation_lock(cfg):
            pending = cfg.runtime_dir / "setup-pending.json"
            recovering = pending.exists() or pending.is_symlink()
            fresh = not cfg.credentials_file.exists() and not cfg.credentials_file.is_symlink()
            if fresh or recovering:
                if fresh and not recovering and service._occupied(cfg):
                    raise setup.DatabaseError("Yerel servis portu kullanımda; başka servise müdahale edilmedi. "
                                              "config/radios.ini içindeki service_url için boş bir port seçin.")
                setup.initialize(cfg)
            # This verifies stored identity and acts only on the installation's
            # owned service. A foreign listener never gets killed or adopted.
            service.start_service(cfg)
        os.execv(native[0], native)
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as exc:
        # Installation helpers deliberately redact authentication material.
        print(f"RadioS: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
