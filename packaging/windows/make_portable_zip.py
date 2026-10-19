"""RadioS: deterministic portable ZIP of a verified package stage.

Run by the CMake target ``radios_portable`` with the pinned MSYS2 UCRT64 Python
(``python.exe -I -X utf8 -B``); standard library only.

    make_portable_zip.py --stage <build>\\stage\\RadioS --output <root>\\dist\\RadioS-0.19.0-windows-x64.zip
                         [--root-name RadioS]

The archive holds the stage below one top-level folder (default ``RadioS/``), including empty
directories (the ``data\\*`` skeleton). Identical stages give byte-identical archives:
entries sorted by their UTF-8 name, timestamps fixed to 1980-01-01 00:00:00, Unix attributes
(files 0644, directories 0755), DEFLATE level 9, no comment, no extra fields. The archive is
written to a temporary file, re-read and CRC-checked, then moved into place; a
``<name>.sha256`` file ("<hex>  <name>", LF) is written next to it.

Exit codes: 0 success, 1 failure (no output replaced), 2 usage error.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
import zipfile

FIXED_TIME = (1980, 1, 1, 0, 0, 0)
FILE_ATTR = (0o100644 << 16)
DIR_ATTR = (0o040755 << 16) | 0x10   # + MS-DOS directory bit
MAX_NAME = 200   # keep <install dir> + name below MAX_PATH on a default Windows setup


def collect(stage: str, root_name: str) -> list[tuple[str, str | None]]:
    """[(archive name, source path or None for a directory)], sorted by UTF-8 bytes."""
    entries: list[tuple[str, str | None]] = [(root_name + "/", None)]
    seen: set[str] = set()
    for current, dirnames, filenames in os.walk(stage):
        dirnames.sort()
        rel_dir = os.path.relpath(current, stage).replace("\\", "/")
        prefix = root_name + "/" + ("" if rel_dir == "." else rel_dir + "/")
        for name in dirnames:
            path = os.path.join(current, name)
            if os.path.islink(path):
                raise ValueError(f"bağlantı desteklenmez / link not supported: {path}")
            entries.append((prefix + name + "/", None))
        for name in filenames:
            path = os.path.join(current, name)
            if os.path.islink(path) or not os.path.isfile(path):
                raise ValueError(f"normal dosya değil / not a regular file: {path}")
            entries.append((prefix + name, path))
    for name, _ in entries:
        folded = name.casefold()
        if folded in seen:
            raise ValueError(f"büyük/küçük harf çakışması / case-insensitive duplicate: {name}")
        seen.add(folded)
        if len(name) > MAX_NAME:
            raise ValueError(f"yol çok uzun / path too long ({len(name)} > {MAX_NAME}): {name}")
        if not name.isascii():
            raise ValueError(f"ASCII olmayan yol / non-ASCII path: {name}")
    entries.sort(key=lambda item: item[0].encode("utf-8"))
    return entries


def write_zip(entries: list[tuple[str, str | None]], output: str) -> None:
    # writestr() with an explicit ZipInfo keeps every header field under our control
    # (files are at most a few tens of MB, reading them whole is fine).
    with zipfile.ZipFile(output, "w", allowZip64=True) as archive:
        for name, source in entries:
            info = zipfile.ZipInfo(name, date_time=FIXED_TIME)
            info.create_system = 3
            if source is None:
                info.external_attr = DIR_ATTR
                archive.writestr(info, b"", compress_type=zipfile.ZIP_STORED)
            else:
                info.external_attr = FILE_ATTR
                with open(source, "rb") as handle:
                    data = handle.read()
                archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stage", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--root-name", default="RadioS")
    args = parser.parse_args(argv)
    if not args.root_name or any(c in args.root_name for c in "/\\:"):
        parser.error("geçersiz --root-name / invalid --root-name")
    stage = os.path.abspath(args.stage)
    output = os.path.abspath(args.output)
    if not os.path.isfile(os.path.join(stage, "bin", "RadioS.exe")):
        print(f"RadioS: {stage} doğrulanmış bir paket aşaması değil / is not a verified package stage",
              file=sys.stderr)
        return 1
    temporary = output + f".tmp-{os.getpid()}"
    try:
        entries = collect(stage, args.root_name)
        os.makedirs(os.path.dirname(output), exist_ok=True)
        write_zip(entries, temporary)
        with zipfile.ZipFile(temporary) as archive:
            bad = archive.testzip()
            if bad is not None:
                raise ValueError(f"CRC hatası / CRC error: {bad}")
            if len(archive.infolist()) != len(entries):
                raise ValueError("girdi sayısı tutmuyor / entry count mismatch")
        digest = hashlib.sha256()
        with open(temporary, "rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
        os.replace(temporary, output)
        checksum = output + ".sha256"
        with open(checksum + ".tmp", "w", encoding="utf-8", newline="\n") as handle:
            handle.write(f"{digest.hexdigest()}  {os.path.basename(output)}\n")
        os.replace(checksum + ".tmp", checksum)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        for path in (temporary, output + ".sha256.tmp"):
            try:
                os.remove(path)
            except OSError:
                pass
        print(f"RadioS: taşınabilir ZIP oluşturulamadı / portable ZIP failed: {error}", file=sys.stderr)
        return 1
    files = sum(1 for _, source in entries if source is not None)
    print(f"RadioS: {output} ({files} dosya/files, sha256 {digest.hexdigest()})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
