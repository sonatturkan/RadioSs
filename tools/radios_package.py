#!/usr/bin/env python3
"""Build and verify bounded, reproducible RadioS distributions without user data."""
from __future__ import annotations

import argparse
import configparser
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tarfile
import tempfile

MAX_FILES = 5000
MAX_FILE = 64 * 1024 * 1024
MAX_TOTAL = 512 * 1024 * 1024
MANIFEST = "packaging/manifest.json"
DATA_DIRS = (
    "database", "endpoints", "imports", "cache", "logs", "recordings", "backups",
)
COMMON_FILES = (
    "CMakeLists.txt", "README.md", "run-radios.sh", "run-radios-sdk.sh", "run-database.sh",
    "config/README.md", "config/radios.example.ini", "database/README.md",
    "database/SDBASE/README.md", "database/SDBASE/IMPLEMENTATION_STATUS.md",
    "database/SDBASE/pyproject.toml",
)
COMMON_TREES = {
    "assets": {".css", ".woff2", ".txt", ".md", ".svg", ".png"},
    "design": {".css", ".html", ".js", ".woff2", ".txt", ".md", ".svg", ".png"},
    "docs": {".md", ".json"},
    "tools": {".py", ".md"},
    "packaging": {".py", ".md", ".desktop", ".svg", ".txt"},
    "database/SDBASE/src": {".py"},
    "database/SDBASE/docs": {".md"},
}
SOURCE_TREES = {
    "src": {".c", ".h", ".md"}, "include": {".h", ".md"},
    "cmake": {".cmake", ".md"}, "tests": {".c", ".h", ".py", ".md"},
    "database/SDBASE/tools": {".py"},
}
PRUNED = {"__pycache__", ".git", ".venv", "node_modules", ".pytest_cache"}


class PackageError(Exception):
    pass


class _BoundedReader:
    def __init__(self, stream):
        self.stream = stream
        self.count = 0

    def read(self, size=-1):
        if size < 0 or size > MAX_FILE:
            raise PackageError("Arşiv tek okuma boyutu sınırı aşıyor.")
        value = self.stream.read(size)
        self.count += len(value)
        if self.count > MAX_TOTAL + 16 * 1024 * 1024:
            raise PackageError("Arşiv açılmış toplam boyut sınırını aşıyor.")
        return value


def _json(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2,
                       allow_nan=False) + "\n").encode()


def _safe_relative(name: str) -> str:
    path = PurePosixPath(name)
    if (not name or path.is_absolute() or str(path) != name or
            any(part in {".", ".."} for part in path.parts) or
            "\\" in name or any(ord(char) < 32 for char in name)):
        raise PackageError("Paket yolu geçerli bir göreli yol olmalı.")
    return name


def _allowed(name: str, kind: str) -> bool:
    if name in COMMON_FILES or name in {MANIFEST, "packaging/runtime.json"}:
        return True
    if name == "build/radios":
        return kind == "binary"
    trees = {**COMMON_TREES, **(SOURCE_TREES if kind == "source" else {})}
    for base, extensions in trees.items():
        if name.startswith(base + "/"):
            rest = PurePosixPath(name[len(base) + 1:])
            if any(part in PRUNED or part.startswith(".") for part in rest.parts):
                return False
            return rest.suffix in extensions
    return False


def _read(root: Path, relative: str) -> tuple[bytes, int]:
    """Open every path component without following a substituted symlink."""
    _safe_relative(relative)
    directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        parts = PurePosixPath(relative).parts
        for component in parts[:-1]:
            next_fd = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                              dir_fd=directory)
            os.close(directory)
            directory = next_fd
        fd = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                     dir_fd=directory)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_FILE or info.st_nlink != 1:
                raise PackageError("Paket girdisi boyutu sınırlı bir normal dosya olmalı.")
            with os.fdopen(os.dup(fd), "rb") as stream:
                body = stream.read(MAX_FILE + 1)
            after = os.fstat(fd)
            if (len(body) != info.st_size or len(body) > MAX_FILE or
                    (info.st_size, info.st_mtime_ns, info.st_ctime_ns) !=
                    (after.st_size, after.st_mtime_ns, after.st_ctime_ns)):
                raise PackageError("Paket girdisi okuma sırasında değişti.")
            return body, 0o755 if relative.endswith(".sh") else 0o644
        finally:
            os.close(fd)
    finally:
        os.close(directory)


def _version(root: Path) -> str:
    source, _ = _read(root, "CMakeLists.txt")
    match = re.search(rb"project\(RadioS\s+VERSION\s+(\d+\.\d+\.\d+)\s", source)
    if not match:
        raise PackageError("CMake proje sürümü bulunamadı.")
    return match.group(1).decode()


def _template_guard(body: bytes) -> None:
    cfg = configparser.ConfigParser(interpolation=None)
    cfg.read_string(body.decode())
    for section in cfg.sections():
        for key, value in cfg.items(section):
            if (key in {"token", "password", "secret", "user_id", "station_id", "endpoint_id"}
                    or key.endswith(("_token", "_password", "_secret"))) and value.strip():
                raise PackageError("Örnek yapılandırma gerçek kimlik veya sır içeremez.")


def collect(root: Path, kind: str) -> dict[str, tuple[bytes, int]]:
    files = {}
    for name in COMMON_FILES:
        files[name] = _read(root, name)
    _template_guard(files["config/radios.example.ini"][0])
    trees = {**COMMON_TREES, **(SOURCE_TREES if kind == "source" else {})}
    for base, extensions in trees.items():
        target = root / base
        if target.is_symlink() or not target.is_dir():
            raise PackageError("Paket kaynak dizini normal bir dizin olmalı: " + base)
        for directory, children, names in os.walk(target, followlinks=False):
            children[:] = sorted(child for child in children if child not in PRUNED)
            for child in children:
                if (Path(directory) / child).is_symlink():
                    raise PackageError("Paket kaynak dizini sembolik bağlantı içeremez.")
            for name in sorted(names):
                path = Path(directory) / name
                relative = path.relative_to(root).as_posix()
                if path.is_symlink():
                    raise PackageError("Paket kaynak dosyası sembolik bağlantı olamaz.")
                if path.suffix in extensions and _allowed(relative, kind):
                    files[relative] = _read(root, relative)
    if len(files) > MAX_FILES or sum(len(body) for body, _ in files.values()) > MAX_TOTAL:
        raise PackageError("Paket kaynakları sınırları aşıyor.")
    return files


def runtime_info(root: Path, build_dir: Path, version: str) -> tuple[bytes, dict]:
    build_dir = build_dir.absolute()
    try:
        relative = build_dir.relative_to(root).as_posix()
    except ValueError as error:
        raise PackageError("Derleme dizini RadioS içinde olmalı.") from error
    executable, _ = _read(root, relative + "/radios")
    if executable[:4] != b"\x7fELF":
        raise PackageError("İkili paket gerçek ELF uygulaması gerektirir.")
    cache, _ = _read(root, relative + "/CMakeCache.txt")
    if b"CMAKE_BUILD_TYPE:STRING=Release\n" not in cache:
        raise PackageError("İkili paket için CMAKE_BUILD_TYPE=Release derlemesi gerekir.")
    env = {**os.environ, "LC_ALL": "C"}
    def run(*arguments: str) -> str:
        return subprocess.run(arguments, check=True, capture_output=True, text=True,
                              timeout=15, env=env).stdout
    actual = run(str(build_dir / "radios"), "--version")
    if not actual.startswith("RadioS " + version + " "):
        raise PackageError("Derlenmiş uygulama ile CMake sürümü aynı değil.")
    dynamic = run("readelf", "-d", str(build_dir / "radios"))
    if re.search(r"\((?:RPATH|RUNPATH)\)", dynamic):
        raise PackageError("Dağıtım ikilisinde ortamına bağlı RPATH/RUNPATH bulunamaz.")
    versions = run("readelf", "-V", str(build_dir / "radios"))
    header = run("readelf", "-h", str(build_dir / "radios"))
    machine_match = re.search(r"Machine:\s*(.+)", header)
    architectures = {"Advanced Micro Devices X86-64": "x86_64", "AArch64": "aarch64"}
    machine = machine_match.group(1).strip() if machine_match else "unknown"
    if machine not in architectures:
        raise PackageError("Paket mimarisi desteklenen Linux ELF mimarisi olmalı.")
    glibc = sorted(set(re.findall(r"Name: GLIBC_([0-9.]+)", versions)),
                   key=lambda text: tuple(map(int, text.split("."))))
    info = {
        "architecture": architectures[machine], "os": "Linux", "python_minimum": "3.11",
        "glibc_minimum": glibc[-1] if glibc else None,
        "needed_libraries": sorted(re.findall(r"Shared library: \[([^\]]+)\]", dynamic)),
        "system_libraries_bundled": False, "build_type": "Release",
        "audio_service": "PulseAudio veya PipeWire Pulse; erişilebilir cihazlar",
        "display": "GTK 3 destekleyen masaüstü oturumu",
    }
    if not info["needed_libraries"] or any("asan" in name or "ubsan" in name
                                            for name in info["needed_libraries"]):
        raise PackageError("İkili paket normal dinamik Release uygulaması olmalı.")
    return executable, info


def _directories(files: dict[str, tuple[bytes, int]]) -> list[str]:
    directories = {"RadioS", "RadioS/data"}
    directories.update("RadioS/data/" + name for name in DATA_DIRS)
    for name in files:
        parent = PurePosixPath("RadioS/" + name).parent
        while str(parent) != ".":
            directories.add(str(parent))
            parent = parent.parent
    return sorted(directories)


def _write_archive(path: Path, files: dict[str, tuple[bytes, int]]) -> None:
    with path.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as tar:
                for name in _directories(files):
                    item = tarfile.TarInfo(name)
                    item.type, item.mode = tarfile.DIRTYPE, 0o755
                    tar.addfile(item)
                for name, (body, mode) in sorted(files.items()):
                    item = tarfile.TarInfo("RadioS/" + name)
                    item.size, item.mode = len(body), mode
                    tar.addfile(item, io.BytesIO(body))
        raw.flush()
        os.fsync(raw.fileno())


def _strict_json(body: bytes) -> dict:
    def pairs(values):
        result = {}
        for key, value in values:
            if key in result:
                raise PackageError("Paket manifestinde yinelenen JSON anahtarı var.")
            result[key] = value
        return result
    value = json.loads(body, object_pairs_hook=pairs,
                       parse_constant=lambda _: (_ for _ in ()).throw(PackageError("JSON sabiti geçersiz.")))
    if not isinstance(value, dict):
        raise PackageError("Paket manifesti nesne olmalı.")
    return value


def verify(archive: Path) -> dict:
    fd = os.open(archive, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    observed = {}
    directories = set()
    total = 0
    manifest = None
    with os.fdopen(fd, "rb") as raw:
        info = os.fstat(raw.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_TOTAL:
            raise PackageError("Paket arşivi normal dosya olmalı.")
        digest = hashlib.file_digest(raw, "sha256").hexdigest()
        raw.seek(0)
        compressed = gzip.GzipFile(fileobj=raw, mode="rb")
        bounded = _BoundedReader(compressed)
        with tarfile.open(fileobj=bounded, mode="r|") as tar:
            for item in tar:
                name = _safe_relative(item.name)
                if name in observed or name in directories:
                    raise PackageError("Arşivde yinelenen yol var.")
                if (item.mode & 0o7000 or item.uid != 0 or item.gid != 0 or
                        item.mtime != 0 or item.uname or item.gname or
                        set(item.pax_headers) - {"path"}):
                    raise PackageError("Arşiv izinleri/kimliği beklenenden farklı.")
                if item.isdir():
                    if item.mode != 0o755:
                        raise PackageError("Arşiv dizin izinleri geçersiz.")
                    directories.add(name)
                    if len(directories) > MAX_FILES:
                        raise PackageError("Arşiv dizin sayısı sınırı aşıyor.")
                    continue
                if (item.type not in {tarfile.REGTYPE, tarfile.AREGTYPE} or item.sparse is not None or
                        not name.startswith("RadioS/") or item.mode not in {0o644, 0o755}):
                    raise PackageError("Arşiv yalnız RadioS altında normal dosyalar içerebilir.")
                relative = name[len("RadioS/"):]
                if item.size > MAX_FILE or item.size < 0:
                    raise PackageError("Arşiv dosyası boyut sınırını aşıyor.")
                total += item.size
                if total > MAX_TOTAL or len(observed) >= MAX_FILES:
                    raise PackageError("Arşiv toplam sınırları aşıyor.")
                stream = tar.extractfile(item)
                if stream is None:
                    raise PackageError("Arşiv dosyası okunamıyor.")
                body = stream.read(MAX_FILE + 1)
                if len(body) != item.size:
                    raise PackageError("Arşiv dosya içeriği eksik.")
                observed[name] = {"size": item.size, "mode": item.mode,
                                  "sha256": hashlib.sha256(body).hexdigest()}
                if relative == MANIFEST:
                    manifest = _strict_json(body)
        # Validate the gzip trailer even when tar stops at its end-of-archive blocks.
        while bounded.read(65536):
            pass
        compressed.close()
    if (not manifest or manifest.get("format") != 1 or
            manifest.get("kind") not in {"binary", "source"} or
            not isinstance(manifest.get("version"), str) or
            not re.fullmatch(r"\d+\.\d+\.\d+", manifest.get("version", "")) or
            manifest.get("user_data_included") is not False or manifest.get("root") != "RadioS"):
        raise PackageError("Paket manifest sözleşmesi geçersiz.")
    expected = manifest.get("files")
    if not isinstance(expected, dict):
        raise PackageError("Paket dosya manifesti geçersiz.")
    for name, record in expected.items():
        _safe_relative(name)
        if not _allowed(name, manifest["kind"]) or name == MANIFEST:
            raise PackageError("Paket manifestinde izin verilmeyen dosya var.")
        if record != observed.get("RadioS/" + name):
            raise PackageError("Arşiv dosyası SHA-256/boyut/izin doğrulamasından geçmedi.")
    if set(observed) != {"RadioS/" + name for name in expected} | {"RadioS/" + MANIFEST}:
        raise PackageError("Arşivde listelenmemiş veya eksik dosya var.")
    expected_directories = set(_directories({name: (b"", 0) for name in expected}))
    expected_directories.update(_directories({MANIFEST: (b"", 0)}))
    if directories != expected_directories:
        raise PackageError("Arşiv dizin listesi taşınabilir paket düzeniyle aynı değil.")
    required = set(COMMON_FILES) | {"packaging/runtime.json", "assets/fonts/LICENSE.txt",
                                  "database/SDBASE/src/sdbase/__init__.py"}
    if manifest["kind"] == "binary":
        required.add("build/radios")
    else:
        required.update({"src/app/main.c", "include/radios/config.h", "tests/test_packaging.py"})
    if not required <= expected.keys():
        raise PackageError("Arşivde zorunlu dağıtım dosyaları eksik.")
    return {"valid": True, "kind": manifest["kind"], "version": manifest["version"],
            "files": len(expected), "bytes": total, "sha256": digest,
            "archive": archive.name, "user_data_included": False}


def package(root: Path, build_dir: Path, kinds: tuple[str, ...] = ("binary", "source")) -> list[dict]:
    root = root.absolute()
    if root.is_symlink() or not root.is_dir():
        raise PackageError("RadioS proje kökü normal bir dizin olmalı.")
    version = _version(root)
    executable, runtime = runtime_info(root, build_dir, version) if "binary" in kinds else (None, {})
    dist = root / "dist"
    if dist.is_symlink():
        raise PackageError("Dağıtım dizini sembolik bağlantı olamaz.")
    dist.mkdir(mode=0o755, exist_ok=True)
    reports = []
    # All outputs must verify before any final archive replaces a previous release.
    with tempfile.TemporaryDirectory(prefix=".package-", dir=dist) as temporary:
        staged = []
        for kind in kinds:
            files = collect(root, kind)
            info = runtime if kind == "binary" else {
                "os": "Linux", "python_minimum": "3.11", "build": "C11 / CMake 3.20+ / pkg-config",
                "dependencies": "Minimum sürümler CMakeLists.txt içinde; kaynak hedef sistemde derlenir.",
                "system_libraries_bundled": False,
            }
            files["packaging/runtime.json"] = (_json(info), 0o644)
            if kind == "binary":
                files["build/radios"] = (executable, 0o755)
            files.pop(MANIFEST, None)
            manifest = {"format": 1, "kind": kind, "version": version,
                        "user_data_included": False, "root": "RadioS",
                        "files": {name: {"size": len(body), "mode": mode,
                                         "sha256": hashlib.sha256(body).hexdigest()}
                                  for name, (body, mode) in sorted(files.items())}}
            files[MANIFEST] = (_json(manifest), 0o644)
            suffix = "linux-" + runtime["architecture"] if kind == "binary" else "source"
            filename = "RadioS-" + version + "-" + suffix + ".tar.gz"
            path = Path(temporary) / filename
            _write_archive(path, files)
            report = verify(path)
            staged.append((path, report))
        for path, _ in staged:
            destination = dist / path.name
            checksum = dist / (path.name + ".sha256")
            if destination.is_symlink() or checksum.is_symlink():
                raise PackageError("Dağıtım çıktısı sembolik bağlantı olamaz.")
        for path, report in staged:
            destination = dist / path.name
            checksum = dist / (path.name + ".sha256")
            checksum_stage = Path(temporary) / checksum.name
            with checksum_stage.open("w", encoding="ascii") as stream:
                stream.write(report["sha256"] + "  " + path.name + "\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(path, destination)
            os.replace(checksum_stage, checksum)
            reports.append(report)
        directory = os.open(dist, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    return reports


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, default=Path(__file__).absolute().parent.parent)
    parser.add_argument("--build-dir", type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("package")
    build.add_argument("--kind", choices=("both", "binary", "source"), default="both")
    check = commands.add_parser("verify")
    check.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "verify":
            result = verify(args.archive)
        else:
            kinds = ("binary", "source") if args.kind == "both" else (args.kind,)
            build_dir = args.build_dir or args.project_root / "build/release"
            if not build_dir.is_absolute():
                build_dir = args.project_root / build_dir
            result = package(args.project_root, build_dir, kinds)
        print(_json(result).decode(), end="")
        return 0
    except (PackageError, OSError, ValueError, EOFError, configparser.Error,
            tarfile.TarError, subprocess.SubprocessError) as error:
        print("Paketleme hatası: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
