"""RadioS: refresh the dev runtime prefix (<root>\\runtime) from a fresh install stage.

Run by the CMake target ``radios_runtime`` with the pinned MSYS2 UCRT64 Python
(``python.exe -I -X utf8 -B``); standard library only, Windows only.

    sync_runtime.py --stage <build>\\runtime-stage --prefix <root>\\runtime
                    --keep python --keep share/zoneinfo [--version 0.19.0] [--build-type Release]

Guarantees
* Kept subtrees (``--keep``: the embedded CPython and share\\zoneinfo) are never read for
  writing, replaced or deleted.
* All or nothing: nothing is changed while ``bin\\RadioS.exe`` or any file that would be
  replaced/removed is in use (a running RadioS maps its exe and DLLs without
  FILE_SHARE_WRITE, so opening them for writing fails with a sharing violation). If a file
  becomes busy between the check and the switch, every completed step is rolled back.
* The prefix must look like a RadioS runtime (only bin/lib/share/etc, the kept trees and the
  stage manifest at top level) - a wrong --prefix such as the project root is refused instead
  of being mirrored.
* Every new file is copied next to its target, hash-checked, then switched in with
  os.replace(); the replaced file is moved aside first so a rollback can restore it.
* ``.radios-stage.json`` (LF, UTF-8) records version, build type and every file's SHA-256.

Exit codes: 0 success (updated or already current), 1 refused or failed (runtime unchanged),
2 usage error.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import sys
import time

MANIFEST = ".radios-stage.json"
MANAGED_TOP = {"bin", "lib", "share", "etc"}
TMP_NEW = ".radios-new-"
TMP_OLD = ".radios-old-"
RETRIES = 20
RETRY_DELAY = 0.1


class SyncError(Exception):
    pass


def say(tr: str, en: str) -> None:
    print(f"RadioS: {tr} / {en}", flush=True)


def key(rel: str) -> str:
    # NTFS names are case-insensitive: compare like Windows does.
    return rel.casefold()


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_kept(rel: str, keeps: list[str]) -> bool:
    k = key(rel)
    return any(k == keep or k.startswith(keep + "/") for keep in keeps)


def walk(root: str, keeps: list[str]) -> tuple[dict[str, str], set[str]]:
    """Return ({key: relative path} of files, {key} of directories) below root, keeps excluded."""
    files: dict[str, str] = {}
    dirs: set[str] = set()
    for current, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(current, root).replace("\\", "/")
        rel_dir = "" if rel_dir == "." else rel_dir
        kept = []
        for name in dirnames:
            rel = f"{rel_dir}/{name}" if rel_dir else name
            if is_kept(rel, keeps):
                kept.append(name)
                continue
            if os.path.islink(os.path.join(current, name)):
                raise SyncError(f"bağlantı/reparse noktası desteklenmez / links are not supported: {rel}")
            dirs.add(key(rel))
        for name in kept:
            dirnames.remove(name)
        for name in filenames:
            rel = f"{rel_dir}/{name}" if rel_dir else name
            if is_kept(rel, keeps) or (not rel_dir and name == MANIFEST):
                continue
            files[key(rel)] = rel
    return files, dirs


def check_prefix_shape(prefix: str, keeps: list[str]) -> None:
    if not os.path.isdir(prefix):
        return
    allowed = {key(top) for top in MANAGED_TOP} | {keep.split("/")[0] for keep in keeps} | {key(MANIFEST)}
    foreign = sorted(name for name in os.listdir(prefix) if key(name) not in allowed)
    if foreign:
        raise SyncError(
            f"{prefix} bir RadioS çalışma ortamına benzemiyor (beklenmeyen: {', '.join(foreign)}); hiçbir şey "
            f"değiştirilmedi / does not look like a RadioS runtime prefix (unexpected: {', '.join(foreign)}); "
            "nothing was changed")


def open_for_write_error(path: str) -> str | None:
    """None when the file can be opened for writing (not mapped/locked by a running process)."""
    try:
        fd = os.open(path, os.O_RDWR | getattr(os, "O_BINARY", 0))
    except FileNotFoundError:
        return None
    except OSError as error:
        winerror = getattr(error, "winerror", None)
        return f"{error.strerror} (WinError {winerror})" if winerror else str(error)
    os.close(fd)
    return None


def retry(operation, *args) -> None:
    """os.replace/os.remove with a short bounded retry (antivirus scanners open files briefly)."""
    for attempt in range(RETRIES):
        try:
            operation(*args)
            return
        except PermissionError:
            if attempt == RETRIES - 1:
                raise
            time.sleep(RETRY_DELAY)


def clear_readonly(path: str) -> None:
    try:
        mode = os.stat(path).st_mode
        if not mode & stat.S_IWRITE:
            os.chmod(path, mode | stat.S_IWRITE)
    except FileNotFoundError:
        pass


def remove_leftovers(prefix: str, keeps: list[str]) -> None:
    """Delete temporaries of an interrupted earlier run (best effort; busy ones stay)."""
    if not os.path.isdir(prefix):
        return
    for current, dirnames, filenames in os.walk(prefix):
        rel_dir = os.path.relpath(current, prefix).replace("\\", "/")
        dirnames[:] = [d for d in dirnames if not is_kept(f"{rel_dir}/{d}" if rel_dir != "." else d, keeps)]
        for name in filenames:
            if TMP_NEW in name or TMP_OLD in name:
                path = os.path.join(current, name)
                try:
                    clear_readonly(path)
                    os.remove(path)
                except OSError:
                    say(f"eski geçici dosya silinemedi: {path}", f"could not delete old temporary file: {path}")


def sync(stage: str, prefix: str, keeps: list[str], version: str, build_type: str) -> int:
    stage = os.path.abspath(stage)
    prefix = os.path.abspath(prefix)
    if not os.path.isfile(os.path.join(stage, "bin", "RadioS.exe")):
        raise SyncError(f"{stage}\\bin\\RadioS.exe yok; önce 'cmake --install' çalışmalı / missing; "
                        "run 'cmake --install' first")
    stage_k, prefix_k = key(stage), key(prefix)
    if prefix_k == stage_k or prefix_k.startswith(stage_k + os.sep) or stage_k.startswith(prefix_k + os.sep):
        raise SyncError("--stage ve --prefix iç içe olamaz / must not contain each other")
    check_prefix_shape(prefix, keeps)
    remove_leftovers(prefix, keeps)

    stage_files, stage_dirs = walk(stage, keeps)
    prefix_files, prefix_dirs = (walk(prefix, keeps) if os.path.isdir(prefix) else ({}, set()))
    for k in stage_files:
        if k in prefix_dirs:
            raise SyncError(f"çalışma ortamında dosya yerine klasör var / directory where a file belongs: "
                            f"{stage_files[k]} (klasörü silin / delete it)")
    for k in prefix_files:
        if k in stage_dirs:
            raise SyncError(f"çalışma ortamında klasör yerine dosya var / file where a directory belongs: "
                            f"{prefix_files[k]} (dosyayı silin / delete it)")

    hashes: dict[str, str] = {}
    sizes: dict[str, int] = {}
    changed: list[str] = []   # keys whose target exists and differs
    added: list[str] = []     # keys whose target does not exist
    for k, rel in sorted(stage_files.items()):
        source = os.path.join(stage, rel)
        hashes[k] = sha256(source)
        sizes[k] = os.path.getsize(source)
        if k not in prefix_files:
            added.append(k)
            continue
        target = os.path.join(prefix, prefix_files[k])
        if os.path.getsize(target) != sizes[k] or sha256(target) != hashes[k]:
            changed.append(k)
    removed = sorted(k for k in prefix_files if k not in stage_files)

    def write_manifest() -> None:
        document = {
            "format": 1,
            "version": version,
            "build_type": build_type,
            "kept": keeps,
            "files": {stage_files[k]: {"size": sizes[k], "sha256": hashes[k]} for k in sorted(stage_files)},
        }
        os.makedirs(prefix, exist_ok=True)
        path = os.path.join(prefix, MANIFEST)
        temporary = path + f"{TMP_NEW}{os.getpid()}"
        with open(temporary, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(document, handle, ensure_ascii=False, indent=1, sort_keys=True)
            handle.write("\n")
        retry(os.replace, temporary, path)

    if not changed and not added and not removed:
        write_manifest()
        say("çalışma ortamı zaten güncel", "runtime is already up to date")
        return 0

    # --- 1. in-use check (all or nothing) --------------------------------------------------
    busy = []
    probe = [prefix_files[k] for k in changed] + [prefix_files[k] for k in removed]
    exe_key = key("bin/RadioS.exe")
    if exe_key in prefix_files and prefix_files[exe_key] not in probe:
        probe.insert(0, prefix_files[exe_key])
    for rel in probe:
        problem = open_for_write_error(os.path.join(prefix, rel))
        if problem:
            busy.append(f"{rel}: {problem}")
    if busy:
        print("RadioS çalışıyor; çalışma ortamı güncellenmedi (kapatıp yeniden deneyin). / RadioS is running; "
              "the runtime was not updated (close it and try again).", file=sys.stderr)
        for line in busy[:20]:
            print(f"  kullanımda/in use: {line}", file=sys.stderr)
        return 1

    # --- 2. prepare every new file next to its target ----------------------------------------
    pid = os.getpid()
    prepared: dict[str, str] = {}
    try:
        for k in changed + added:
            rel = stage_files[k] if k in added else prefix_files[k]
            target = os.path.join(prefix, rel)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            temporary = f"{target}{TMP_NEW}{pid}"
            shutil.copy2(os.path.join(stage, stage_files[k]), temporary)
            prepared[k] = temporary
            if sha256(temporary) != hashes[k]:
                raise SyncError(f"kopya doğrulanamadı / copy verification failed: {rel}")
    except (OSError, SyncError):
        for temporary in prepared.values():
            try:
                os.remove(temporary)
            except OSError:
                pass
        raise

    # --- 3. switch (with rollback) ------------------------------------------------------------
    done: list[tuple[str, str, str | None]] = []   # (action, target, aside)
    try:
        for k in changed:
            target = os.path.join(prefix, prefix_files[k])
            aside = f"{target}{TMP_OLD}{pid}"
            clear_readonly(target)
            retry(os.replace, target, aside)
            done.append(("aside", target, aside))
            retry(os.replace, prepared[k], target)
            del prepared[k]   # only once it is in place: a failed switch leaves it for the cleanup
            done.append(("placed", target, aside))
        for k in added:
            target = os.path.join(prefix, stage_files[k])
            retry(os.replace, prepared[k], target)
            del prepared[k]
            done.append(("added", target, None))
        for k in removed:
            target = os.path.join(prefix, prefix_files[k])
            aside = f"{target}{TMP_OLD}{pid}"
            clear_readonly(target)
            retry(os.replace, target, aside)
            done.append(("aside", target, aside))
    except OSError as error:
        failures = []
        for action, target, aside in reversed(done):
            try:
                if action == "placed":
                    os.remove(target)
                elif action == "aside":
                    os.replace(aside, target)
                elif action == "added":
                    os.remove(target)
            except OSError as undo_error:
                failures.append(f"{target}: {undo_error}")
        for temporary in prepared.values():
            try:
                os.remove(temporary)
            except OSError:
                pass
        if failures:
            raise SyncError("güncelleme ve geri alma başarısız; çalışma ortamını yeniden oluşturun / update and "
                            f"rollback failed, rebuild the runtime: {error}; " + "; ".join(failures))
        raise SyncError(f"bir dosya kullanımda; değişiklikler geri alındı / a file became busy; changes were "
                        f"rolled back: {error}") from error

    # --- 4. cleanup ----------------------------------------------------------------------------
    for action, target, aside in done:
        if action == "aside" and aside and os.path.exists(aside):
            try:
                retry(os.remove, aside)
            except OSError:
                say(f"eski dosya bir sonraki eşitlemede silinecek: {aside}",
                    f"old file will be deleted by the next sync: {aside}")
    for d in sorted((prefix_dirs - stage_dirs), key=lambda value: value.count("/"), reverse=True):
        path = os.path.join(prefix, d)
        try:
            os.rmdir(path)
        except OSError:
            pass   # not empty (e.g. an aside file that could not be deleted yet)
    # GTK ignores an icon cache older than its theme directory: refresh the cache mtime of
    # every theme whose tree changed (the cache content itself came from the stage).
    touched = {k.split("/")[2] for k in changed + added + removed if k.startswith("share/icons/") and k.count("/") >= 3}
    for theme in sorted(touched):
        cache = os.path.join(prefix, "share", "icons", theme, "icon-theme.cache")
        if os.path.isfile(cache):
            os.utime(cache, None)
    write_manifest()
    say(f"çalışma ortamı güncellendi: {len(changed)} değişti, {len(added)} eklendi, {len(removed)} silindi",
        f"runtime updated: {len(changed)} changed, {len(added)} added, {len(removed)} removed")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stage", required=True, help="fresh 'cmake --install --component runtime' output")
    parser.add_argument("--prefix", required=True, help="runtime prefix to refresh (<root>\\runtime)")
    parser.add_argument("--keep", action="append", default=[], help="relative subtree never touched")
    parser.add_argument("--version", default="")
    parser.add_argument("--build-type", default="")
    args = parser.parse_args(argv)
    if os.name != "nt":
        print("sync_runtime.py yalnız Windows'ta çalışır / runs on Windows only", file=sys.stderr)
        return 2
    keeps = []
    for keep in args.keep:
        normal = keep.replace("\\", "/").strip("/")
        parts = normal.split("/")
        if not normal or any(part in ("", ".", "..") for part in parts) or ":" in normal:
            parser.error(f"geçersiz --keep / invalid --keep: {keep!r}")
        keeps.append(key(normal))
    try:
        return sync(args.stage, args.prefix, keeps, args.version, args.build_type)
    except SyncError as error:
        print(f"RadioS: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"RadioS: eşitleme başarısız; çalışma ortamı değiştirilmedi / sync failed; runtime unchanged: "
              f"{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
