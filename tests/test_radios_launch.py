#!/usr/bin/env python3
"""Launcher build step: skip flag, last-good binary fallback, hard failure without a binary.

No real cmake runs: subprocess.run is replaced inside tools/radios_launch.py
and a private temporary project root is used. The user's installation,
build directory and data service are never touched."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import radios_launch  # noqa: E402


class LauncherBuild(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="radios-launch-test-"))
        (self.root / "src/app").mkdir(parents=True)
        (self.root / "src/app/main.c").write_text("int main(void) { return 0; }\n")
        (self.root / "CMakeLists.txt").write_text("project(RadioS LANGUAGES C)\n")
        (self.root / "build").mkdir()
        self.calls = []
        self.original_run = radios_launch.subprocess.run
        self.original_which = radios_launch.shutil.which
        radios_launch.shutil.which = lambda name: "/usr/bin/" + name
        self.addCleanup(self.restore)

    def restore(self):
        radios_launch.subprocess.run = self.original_run
        radios_launch.shutil.which = self.original_which
        os.environ.pop("RADIOS_SKIP_BUILD", None)
        shutil.rmtree(self.root, ignore_errors=True)

    def fake_run(self, *, fail_build):
        def run(command, **options):
            self.calls.append(list(command))
            if "--exists" in command:
                return subprocess.CompletedProcess(command, 0)
            if "--build" in command and fail_build:
                raise subprocess.CalledProcessError(1, command)
            return subprocess.CompletedProcess(command, 0)
        radios_launch.subprocess.run = run

    def last_good_binary(self):
        binary = self.root / "build/radios"
        binary.write_text("#!/bin/sh\nexit 0\n")
        binary.chmod(0o755)
        return binary

    def test_normal_launch_configures_and_builds_radios_target(self):
        self.fake_run(fail_build=False)
        radios_launch.build_current_sources(self.root)
        builds = [call for call in self.calls if "--build" in call]
        self.assertEqual(len(builds), 1)
        self.assertIn("radios", builds[0])
        self.assertTrue((self.root / "default-binary.lock").is_file())

    def test_skip_flag_never_runs_cmake(self):
        os.environ["RADIOS_SKIP_BUILD"] = "1"
        self.fake_run(fail_build=False)
        radios_launch.build_current_sources(self.root)
        self.assertEqual(self.calls, [])

    def test_failed_build_falls_back_to_last_good_binary(self):
        binary = self.last_good_binary()
        self.fake_run(fail_build=True)
        radios_launch.build_current_sources(self.root)
        self.assertTrue(any("--build" in call for call in self.calls))
        self.assertEqual(binary.read_text(), "#!/bin/sh\nexit 0\n")

    def test_failed_build_without_binary_still_fails(self):
        self.fake_run(fail_build=True)
        with self.assertRaises(subprocess.CalledProcessError):
            radios_launch.build_current_sources(self.root)

    def test_passive_project_without_sources_builds_nothing(self):
        (self.root / "src/app/main.c").unlink()
        self.fake_run(fail_build=False)
        radios_launch.build_current_sources(self.root)
        self.assertEqual(self.calls, [])


class SdkSandbox(unittest.TestCase):
    def test_sdk_window_sees_mounted_disks_besides_home_and_project(self):
        """Export targets, packages and archive folders may live on a connected disk."""
        import radios_sdk
        command = radios_sdk.sdk_command(Path("/proje kökü"), Path("/proje kökü/build/radios"), ["--profile", "routing"])
        for option in ("--filesystem=home", "--filesystem=/proje kökü", "--filesystem=/run/media",
                       "--filesystem=/media", "--filesystem=/mnt"):
            self.assertIn(option, command)
        self.assertEqual(command[-3:], ["org.freedesktop.Sdk//25.08", "--profile", "routing"])


if __name__ == "__main__":
    unittest.main()
