#!/usr/bin/env python3
"""Run the native desktop in an already installed matching Flatpak SDK.

This helper installs nothing and downloads nothing. The project remains the
installation root; the matching runtime supplies the C desktop's shared libraries.
Database setup and its service remain in the caller's PID namespace.
"""
from __future__ import annotations

from pathlib import Path
import shutil
import sys


def sdk_command(root: Path, binary: Path, arguments: list[str]) -> list[str]:
    flatpak = shutil.which("flatpak")
    if flatpak:
        command = [flatpak]
    elif Path("/.flatpak-info").is_file() and shutil.which("flatpak-spawn"):
        command = [shutil.which("flatpak-spawn"), "--host", "flatpak"]
    else:
        raise RuntimeError("Flatpak bulunamadı. Gerekli sistem kütüphanelerini sağlayın veya kaynak paketini bu sistemde derleyin.")
    # Removable and extra disks (USB music, export targets, an archive or
    # backup disk) are mounted outside home; absent paths are skipped.
    return [*command, "run", "--socket=wayland", "--socket=fallback-x11",
            "--socket=pulseaudio", "--share=network", "--filesystem=home",
            "--filesystem=/run/media", "--filesystem=/media", "--filesystem=/mnt",
            "--talk-name=org.freedesktop.Flatpak",
            "--filesystem=" + str(root), "--command=" + str(binary),
            "org.freedesktop.Sdk//25.08", *arguments]


def main() -> int:
    # Backend setup stays in the caller's PID namespace. Only the native C
    # frontend enters the SDK; a later launch can still recognize the service.
    import radios_launch
    return radios_launch.main(force_sdk=True)


if __name__ == "__main__":
    raise SystemExit(main())
