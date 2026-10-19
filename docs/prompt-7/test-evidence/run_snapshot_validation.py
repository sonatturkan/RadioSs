#!/usr/bin/env python3
"""Capture public RadioS sources and validate an immutable prompt7 snapshot.

The live tree remains untouched. Production configuration, credentials,
databases, imported media, existing builds and lock files are never copied.
"""
from __future__ import annotations

import contextlib
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET


TARGETS = ("radios", "test_live_players_ui", "test_player_loader_ui",
           "test_audio_ui", "test_player_mapping_ui")
TESTS = ("native_audio", "native_live_players", "native_player_loader",
         "native_player_mapping")
TEST_SOURCES = ("test_audio_ui.c", "test_live_players_ui.c",
                "test_player_loader_ui.c", "test_player_mapping_ui.c")


def utc() -> str:
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def public_paths(root: Path) -> list[Path]:
    paths = [root / "CMakeLists.txt", root / "database/SDBASE/pyproject.toml",
             root / "config/radios.example.ini",
             root / "design/reference/tasarrım/index.html"]
    selections = {"src": {".c", ".h"}, "include": {".h"},
                  "cmake": {".cmake"}, "tests": {".c", ".h", ".py"},
                  "database/SDBASE/src": {".py"}}
    for directory, suffixes in selections.items():
        paths.extend(p for p in (root / directory).rglob("*")
                     if p.is_file() and p.suffix in suffixes
                     and "__pycache__" not in p.parts)
    paths.extend((root / "tools").glob("*.py"))
    paths.extend((root / "design/reference/tasarrım").rglob("*.css"))
    paths.extend(p for p in (root / "assets").rglob("*") if p.is_file()
                 and not p.name.endswith((".lock", ".tmp"))
                 and "__pycache__" not in p.parts)
    result = sorted(set(paths))
    for path in result:
        if not path.is_file() or path.is_symlink():
            raise RuntimeError("Public source must be a regular file: " +
                               str(path.relative_to(root)))
    return result


def manifest(root: Path) -> dict[str, str]:
    return {str(p.relative_to(root)): sha(p) for p in public_paths(root)}


def changed(before: dict, after: dict) -> list[str]:
    return sorted(p for p in set(before) | set(after)
                  if before.get(p) != after.get(p))


def inspect_tests(source: Path, log: Path, junit: Path) -> dict:
    expected_by_test = {
        name: re.findall(r'g_test_add_func\("([^"\n]+)"',
                         (source / "tests" / filename).read_text())
        for name, filename in zip(TESTS, TEST_SOURCES)
    }
    expected = [case for paths in expected_by_test.values() for case in paths]
    output = log.read_text()
    identities = dict(re.findall(r"^\s*Start\s+(\d+):\s+(\S+)\s*$", output,
                                 flags=re.MULTILINE))
    grouped = {name: [] for name in TESTS}
    for line in output.splitlines():
        prefix = re.match(r"^(\d+):\s?(.*)$", line)
        if prefix and identities.get(prefix[1]) in grouped:
            grouped[identities[prefix[1]]].append(prefix[2])
    helper = (source / "tests/run_audio_ui.py").read_text()
    independent_audio = ("def _run_cli() -> int:" in helper and
                         "main(expected_glib_case=case)" in helper)
    mapping_adapter = source / "tests/run_player_mapping_ui.py"
    mapping_module = (source / "cmake/Prompt7Tests.cmake").read_text()
    independent_mapping = (mapping_adapter.is_file()
                           and "run_player_mapping_ui.py" in mapping_module
                           and "collect_glib_cases=" in mapping_adapter.read_text()
                           and "expected_glib_case=" in mapping_adapter.read_text())
    groups = {}
    for name, lines in grouped.items():
        text = "\n".join(lines)
        actual = re.findall(r"^ok\s+\d+\s+(/[^\s]+)\s*$", text, re.MULTILINE)
        plans = [int(n) for n in re.findall(r"^1\.\.(\d+)\s*$", text, re.MULTILINE)]
        expected_plans = [len(expected_by_test[name])]
        if ((name == "native_audio" and independent_audio)
                or (name == "native_player_mapping" and independent_mapping)):
            expected_plans += [1] * len(expected_by_test[name])
        skipped = len(re.findall(r"^(?:not )?ok\s+\d+.*#\s*SKIP\b", text,
                                 re.MULTILINE | re.IGNORECASE))
        groups[name] = {"expected_glib_cases": expected_by_test[name],
                        "passed_glib_cases": actual, "tap_plans": plans,
                        "expected_tap_plans": expected_plans, "skipped": skipped,
                        "accepted": sorted(actual) == sorted(expected_by_test[name])
                        and plans == expected_plans and skipped == 0}
    actual = [case for group in groups.values() for case in group["passed_glib_cases"]]
    cases = ET.parse(junit).getroot().findall(".//testcase")
    details = [{"name": case.attrib.get("name"),
                "status": case.attrib.get("status"),
                "time": case.attrib.get("time"),
                "skipped": case.find("skipped") is not None,
                "failure": case.find("failure") is not None,
                "error": case.find("error") is not None} for case in cases]
    passed = (len(expected) == 34 and len(set(expected)) == 34
              and sorted(actual) == sorted(expected)
              and all(group["accepted"] for group in groups.values())
              and sorted(identities.values()) == sorted(TESTS)
              and len(details) == 4
              and sorted(d["name"] for d in details) == sorted(TESTS)
              and all(d["status"] == "run" and not any(
                  d[k] for k in ("skipped", "failure", "error"))
                      for d in details))
    return {"accepted": passed, "expected_glib_cases": expected,
            "passed_glib_cases": actual, "groups": groups,
            "native_audio_independent_fixtures": independent_audio,
            "native_player_mapping_independent_fixtures": independent_mapping,
            "junit_tests": details, "skipped": sum(d["skipped"] for d in details)
            + sum(g["skipped"] for g in groups.values())}


def main() -> int:
    root = Path(__file__).resolve().parents[3]
    evidence = root / "docs/prompt-7/test-evidence"
    tag = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    snapshot = root / "build" / ("prompt-7-snapshot-" + tag)
    base = evidence / ("snapshot-validation-" + tag)
    report_path = base.with_suffix(".json")
    report = {"agent": "prompt7/root", "runner_author": "prompt7/requirements_resume",
              "started_utc": utc(),
              "status": "started", "accepted": False,
              "source_mutations": [], "snapshot": str(snapshot.relative_to(root)),
              "scope": "Exact captured public-source snapshot; strict application build, "
              "23 GTK cases with controlled asynchronous boundaries, 8 existing isolated "
              "real PCM cases and 3 real PCM high-deck mapping cases. This result does not "
              "claim the continuously changing live tree, production DB, physical audio "
              "hardware or authenticated online-provider acceptance.",
              "public_design_dependency": "index.html and CSS required by css_to_gtk.py",
              "copy_mode_exception": "database/SDBASE/src/sdbase/http_server.py is 0644 so "
              "the existing PCM helper can edit its separate private copytree fixture. "
              "All captured bytes, including that file, must remain hash-identical."}

    def checkpoint() -> None:
        temp = report_path.with_suffix(".json.tmp")
        temp.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
        temp.replace(report_path)

    def stage(name: str, command: list[str], cwd: Path) -> int:
        log = Path(str(base) + "-" + name + ".log")
        report[name] = {"command": command, "cwd": str(cwd), "started_utc": utc(),
                        "status": "running", "stdout_log": str(log.relative_to(root))}
        checkpoint()
        environment = os.environ.copy()
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
        environment["GDK_BACKEND"] = "x11"
        with log.open("w") as output:
            result = subprocess.run(command, cwd=cwd, env=environment,
                                    stdout=output, stderr=subprocess.STDOUT)
        report[name].update(exit_code=result.returncode, status="finished", ended_utc=utc())
        checkpoint()
        print(json.dumps({"stage": name, "exit_code": result.returncode,
                          "report": str(report_path.relative_to(root))}), flush=True)
        return result.returncode

    try:
        snapshot.mkdir(parents=True, exist_ok=False)
        report["capture_attempts"] = []
        checkpoint()
        source = None
        for attempt in range(1, 3):
            destination = snapshot / ("source" if attempt == 1 else "source-retry")
            entry = {"attempt": attempt, "started_utc": utc(),
                     "source": str(destination.relative_to(root)), "accepted": False}
            report["capture_attempts"].append(entry)
            checkpoint()
            with contextlib.ExitStack() as stack:
                global_lock = stack.enter_context((root / "source-edit.lock").open("a+"))
                fcntl.flock(global_lock.fileno(), fcntl.LOCK_EX)
                paths = public_paths(root)
                for path in paths:
                    descriptor = stack.enter_context(Path(str(path) + ".lock").open("a+"))
                    fcntl.flock(descriptor.fileno(), fcntl.LOCK_EX)
                entry["lock_count"] = len(paths) + 1
                before = manifest(root)
                destination.mkdir()
                for path in paths:
                    target = destination / path.relative_to(root)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(path, target)
                    fixture_gate_source = "database/SDBASE/src/sdbase/http_server.py"
                    target.chmod(0o644 if str(path.relative_to(root)) == fixture_gate_source
                                 else 0o444)
                copied = manifest(destination)
                after = manifest(root)
                entry.update(origin_before=before, copied=copied, origin_after=after,
                             source_drift=changed(before, after),
                             copy_mismatch=changed(before, copied),
                             ended_utc=utc(), accepted=before == copied == after)
                checkpoint()
            if entry["accepted"]:
                source = destination
                report["captured_manifest"] = copied
                report["captured_source"] = str(source.relative_to(root))
                break
            print(json.dumps({"capture_attempt": attempt, "accepted": False,
                              "source_drift": entry["source_drift"]}), flush=True)
        if source is None:
            raise RuntimeError("Public source capture changed twice; no build was accepted.")
        build = snapshot / "build"
        configure_exit = stage("configure", ["cmake", "-S", str(source), "-B", str(build),
                               "-DCMAKE_BUILD_TYPE=Debug", "-DBUILD_TESTING=ON",
                               "-DRADIOS_GUI_TESTS=ON", "-DRADIOS_STRICT_WARNINGS=ON",
                               "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"], root)
        if configure_exit:
            raise RuntimeError("Snapshot configure failed; inspect preserved log.")
        build_exit = stage("build", ["cmake", "--build", str(build), "--target", *TARGETS,
                                   "--parallel", "4"], root)
        if build_exit:
            raise RuntimeError("Snapshot strict build failed; inspect preserved log.")
        binaries = [build / name for name in (*TARGETS, "libradios_ui.a")]
        report["binary_hashes_before_tests"] = {str(p.relative_to(root)): sha(p) for p in binaries}
        report["snapshot_manifest_before_tests"] = manifest(source)
        checkpoint()
        junit = Path(str(base) + "-ctest.xml")
        ctest_exit = stage("ctest", ["ctest", "--test-dir", str(build), "-V", "--output-junit",
                                  str(junit), "-R", "^(" + "|".join(TESTS) + ")$"], root)
        report["ctest"]["junit"] = str(junit.relative_to(root))
        report["binary_hashes_after_tests"] = {str(p.relative_to(root)): sha(p) for p in binaries}
        report["snapshot_manifest_after_tests"] = manifest(source)
        report["snapshot_source_drift"] = changed(report["captured_manifest"],
                                                   report["snapshot_manifest_after_tests"])
        report["live_manifest_after_tests"] = manifest(root)
        report["live_source_drift_since_capture"] = changed(report["captured_manifest"],
                                                              report["live_manifest_after_tests"])
        report["test_inspection"] = inspect_tests(source,
            root / report["ctest"]["stdout_log"], junit)
        report["accepted"] = (ctest_exit == 0 and report["test_inspection"]["accepted"]
            and report["captured_manifest"] == report["snapshot_manifest_before_tests"]
            == report["snapshot_manifest_after_tests"]
            and report["binary_hashes_before_tests"] == report["binary_hashes_after_tests"])
        report["status"] = "finished"
        return 0 if report["accepted"] else 1
    except BaseException as error:
        report.update(status="interrupted_or_failed", error=str(error))
        if isinstance(error, KeyboardInterrupt):
            raise
        return 1
    finally:
        report["ended_utc"] = utc()
        checkpoint()
        print(json.dumps({"accepted": report["accepted"], "status": report["status"],
                          "report": str(report_path.relative_to(root))}), flush=True)


if __name__ == "__main__":
    sys.exit(main())
