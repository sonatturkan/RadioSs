#!/usr/bin/env python3
"""Record a source-stable prompt7 build and focused CTest run, phase by phase."""
import contextlib
import datetime
import fcntl
import hashlib
import json
import pathlib
import subprocess


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources(root, include_unselected_tests=False):
    paths = [root / "CMakeLists.txt"]
    for directory in ("src", "include", "cmake"):
        paths.extend(p for p in (root / directory).rglob("*")
                     if p.is_file() and p.suffix in (".c", ".h", ".cmake", ".py"))
    paths.extend(root / "tests" / name for name in (
        "test_live_players_ui.c", "test_player_loader_ui.c", "test_audio_ui.c",
        "run_audio_ui.py"))
    # The PCM fixture clones these public sources into its private project.
    paths.extend((root / "database/SDBASE/src").rglob("*.py"))
    paths.extend((root / "tools").glob("radios_database*.py"))
    paths.extend(p for p in (root / "assets").rglob("*") if p.is_file())
    paths.extend((root / "database/SDBASE/pyproject.toml",
                  root / "config/radios.example.ini"))
    if include_unselected_tests:
        paths.extend(p for p in (root / "tests").rglob("*")
                     if p.is_file() and p.suffix in (".c", ".h", ".py"))
    return {str(p.relative_to(root)): sha(p) for p in sorted(set(paths))}


def main():
    root = pathlib.Path(__file__).resolve().parents[3]
    evidence = root / "docs/prompt-7/test-evidence"
    locks = ["source-edit.lock", "CMakeLists.txt.lock", "src/ui/broadcast.c.lock",
             "src/ui/player_loader.c.lock", "src/ui/media.c.lock", "src/ui/dj.c.lock",
             "include/radios/player_transport.h.lock", "tests/test_live_players_ui.c.lock",
             "tests/test_player_loader_ui.c.lock", "tests/test_audio_ui.c.lock",
             "tests/run_audio_ui.py.lock"]
    with contextlib.ExitStack() as stack:
        for name in locks:
            descriptor = stack.enter_context(open(root / name, "a+"))
            fcntl.flock(descriptor.fileno(), fcntl.LOCK_EX)
        tag = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        base = evidence / ("final-fenced-validation-" + tag)
        report_path = pathlib.Path(str(base) + ".json")
        report = {"agent": "prompt7/root", "started_utc": utc(), "locks": locks,
                  "source_mutations": [], "accepted": False, "status": "started",
                  "manifest_scope": "All native/CMake sources, the three selected test sources, "
                  "the PCM execution helper, and public fixture sources/assets/example config. "
                  "Unselected test files are observed separately and cannot affect these binaries.",
                  "scope": "Strict combined build; 18 live-player and 5 loader GTK cases with "
                  "mocked async boundaries; isolated real PCM/null-output audio regression. "
                  "No physical hardware or live provider service claim."}

        def checkpoint():
            temp = report_path.with_suffix(".json.tmp")
            temp.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
            temp.replace(report_path)

        def run_stage(name, command):
            log = pathlib.Path(str(base) + "-" + name + ".log")
            report[name] = {"command": command, "started_utc": utc(),
                            "stdout_log": str(log.relative_to(root)), "status": "running"}
            checkpoint()
            with log.open("w") as output:
                result = subprocess.run(command, cwd=root, stdout=output,
                                        stderr=subprocess.STDOUT)
            report[name].update(exit_code=result.returncode, ended_utc=utc(), status="finished")
            checkpoint()
            print(json.dumps({"stage": name, "exit_code": result.returncode,
                              "json": str(report_path.relative_to(root))}), flush=True)
            return result.returncode

        try:
            report["sources_before_build"] = sources(root)
            report["observed_sources_before_build"] = sources(root, True)
            dj = (root / "src/ui/dj.c").read_text()
            assert '#include "radios/player_transport.h"' in dj
            assert ("if (!ui->master) {\n        "
                    "radios_broadcast_player_library_loaded(ctx, (guint)deck);") in dj
            report["dj_hook"] = "Present in latest locked source; root did not mutate dj.c."
            checkpoint()
            build_exit = run_stage("build", ["cmake", "--build", "build/prompt-7-validation",
                                   "--target", "radios", "test_live_players_ui",
                                   "test_player_loader_ui", "test_audio_ui", "--parallel", "4"])
            report["sources_after_build"] = sources(root)
            report["binary_hashes"] = {
                str(p.relative_to(root)): sha(p)
                for p in (root / "build/prompt-7-validation").iterdir()
                if p.is_file() and p.name in ("radios", "test_live_players_ui",
                    "test_player_loader_ui", "test_audio_ui", "libradios_ui.a")}
            checkpoint()
            if build_exit == 0 and report["sources_before_build"] == report["sources_after_build"]:
                junit = pathlib.Path(str(base) + "-ctest.xml")
                run_stage("ctest", ["ctest", "--test-dir", "build/prompt-7-validation", "-V",
                          "--output-junit", str(junit), "-R",
                          "^(native_live_players|native_player_loader|native_audio)$"])
                report["ctest"]["junit"] = str(junit.relative_to(root))
            else:
                report["ctest"] = {"not_run": "Build failed or source manifest changed."}
            report["sources_after_tests"] = sources(root)
            report["observed_sources_after_tests"] = sources(root, True)
            before, after = report["sources_before_build"], report["sources_after_tests"]
            report["source_drift"] = sorted(p for p in set(before) | set(after)
                                             if before.get(p) != after.get(p))
            observed_before = report["observed_sources_before_build"]
            observed_after = report["observed_sources_after_tests"]
            report["unselected_test_drift"] = sorted(
                p for p in set(observed_before) | set(observed_after)
                if p not in before and p not in after
                and observed_before.get(p) != observed_after.get(p))
            report["accepted"] = (build_exit == 0 and report.get("ctest", {}).get("exit_code") == 0
                                  and before == report["sources_after_build"] == after)
            report["status"] = "finished"
        except BaseException as error:
            report["status"] = "interrupted_or_failed"
            report["error"] = str(error)
            raise
        finally:
            report["ended_utc"] = utc()
            checkpoint()
            print(json.dumps({"accepted": report["accepted"], "status": report["status"],
                              "json": str(report_path.relative_to(root)),
                              "source_drift": report.get("source_drift")}), flush=True)


if __name__ == "__main__":
    main()
