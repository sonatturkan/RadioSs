#!/usr/bin/env python3
"""Distribution integrity and private-data exclusion acceptance."""
from __future__ import annotations

import copy
import gzip
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
from uuid import uuid4

ROOT = Path(__file__).absolute().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import radios_package as packaging


class Packaging(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="radios-package-")
        self.root = Path(self.temporary.name) / "RadioS"
        self.root.mkdir()
        for name in packaging.COMMON_FILES:
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, destination)
        for tree in {**packaging.COMMON_TREES, **packaging.SOURCE_TREES}:
            (self.root / tree).mkdir(parents=True, exist_ok=True)
        for name in ("assets/fonts/LICENSE.txt", "assets/fonts/InterVariable.woff2",
                     "database/SDBASE/src/sdbase/__init__.py", "src/app/main.c",
                     "include/radios/config.h", "tests/test_packaging.py",
                     "tools/radios_database.py", "design/reference/tasarrım/index.html"):
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, destination)
        (self.root / "data").mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def package(self):
        return packaging.package(self.root, self.root / "build/release", ("source",))[0]

    def archive(self):
        self.package()
        return next((self.root / "dist").glob("*.tar.gz"))

    def rewrite(self, path, callback):
        entries = []
        with tarfile.open(path, "r:gz") as tar:
            for item in tar:
                body = tar.extractfile(item).read() if item.isfile() else None
                entries.append((copy.copy(item), body))
        callback(entries)
        with path.open("wb") as file:
            with gzip.GzipFile(filename="", mode="wb", fileobj=file, mtime=0) as raw:
                with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as tar:
                    for item, body in entries:
                        tar.addfile(item, io.BytesIO(body) if body is not None else None)

    def test_deterministic_allowlist_excludes_real_data(self):
        secret = ("PRIVATE-PRODUCTION-" + uuid4().hex).encode()
        for name in ("config/credentials.json", "config/radios.ini", "config/ui-preferences.ini",
                     "data/database/RadioS.sdb", "data/recovery/receipt.json", "data/backups/db.zip",
                     "data/recordings/private.wav", "data/logs/service.log", "build/tests/key.json",
                     "tools/__pycache__/private.pyc", "database/SDBASE/.venv/private.py"):
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(secret)
        first = self.package()
        path = self.root / "dist" / first["archive"]
        bytes_before = path.read_bytes()
        second = self.package()
        self.assertEqual(first, second)
        self.assertEqual(bytes_before, path.read_bytes())
        with tarfile.open(path, "r:gz") as tar:
            names = {item.name for item in tar}
            self.assertNotIn("RadioS/config/credentials.json", names)
            self.assertIn("RadioS/data/database", names)
            self.assertIn("RadioS/assets/fonts/LICENSE.txt", names)
            self.assertIn("RadioS/design/reference/tasarrım/index.html", names)
            for item in tar.getmembers():
                if item.isfile():
                    self.assertNotIn(secret, tar.extractfile(item).read())
        self.assertFalse(list((self.root / "dist").glob(".package-*")))

    def test_no_third_party_bundle_and_private_sessions_are_never_packaged(self):
        # Only local media is supported: no vendored online-service libraries
        # or interpreters ship, and per-user private state never enters a package.
        secret = ("PRIVATE-SESSION-" + uuid4().hex).encode()
        for name in ("config/private/session/station/user/browser.json",
                     "vendor/python/anything/__init__.py", "vendor/bin/helper",
                     "vendor/README.md", "vendor/dependencies.json"):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(secret)
        path = self.archive()
        with tarfile.open(path, "r:gz") as archive:
            names = {item.name for item in archive}
            self.assertFalse([name for name in names if name.startswith("RadioS/vendor")], names)
            self.assertFalse([name for name in names if name.startswith("RadioS/config/private")], names)
            for item in archive:
                if item.isfile():
                    self.assertNotIn(secret, archive.extractfile(item).read())

    def test_changed_file_and_changed_mode_are_rejected(self):
        path = self.archive()
        def alter(entries):
            item, body = next(entry for entry in entries if entry[0].name == "RadioS/README.md")
            entries[entries.index((item, body))] = (item, b"X" * len(body))
        self.rewrite(path, alter)
        with self.assertRaises(packaging.PackageError):
            packaging.verify(path)
        path = self.archive()
        self.rewrite(path, lambda entries: setattr(next(item for item, _ in entries
                          if item.name == "RadioS/run-radios.sh"), "mode", 0o644))
        with self.assertRaises(packaging.PackageError):
            packaging.verify(path)

    def test_unlisted_file_even_normal_extension_is_rejected(self):
        path = self.archive()
        def append(entries):
            item = tarfile.TarInfo("RadioS/docs/unreviewed.json")
            item.size, item.mode = 2, 0o644
            entries.append((item, b"{}"))
        self.rewrite(path, append)
        with self.assertRaises(packaging.PackageError):
            packaging.verify(path)

    def test_manifest_cannot_allow_runtime_credentials(self):
        path = self.archive()
        def append(entries):
            item = tarfile.TarInfo("RadioS/config/credentials.json")
            item.size, item.mode = 2, 0o644
            entries.append((item, b"{}"))
            for index, (record, body) in enumerate(entries):
                if record.name == "RadioS/" + packaging.MANIFEST:
                    value = json.loads(body)
                    value["files"]["config/credentials.json"] = {"size": 2, "mode": 0o644,
                         "sha256": packaging.hashlib.sha256(b"{}").hexdigest()}
                    updated = packaging._json(value)
                    record.size = len(updated)
                    entries[index] = (record, updated)
        self.rewrite(path, append)
        with self.assertRaises(packaging.PackageError):
            packaging.verify(path)

    def test_traversal_symlink_hardlink_duplicate_and_extra_directory_rejected(self):
        for form in ("traversal", "symlink", "hardlink", "duplicate", "directory"):
            with self.subTest(form=form):
                path = self.archive()
                def append(entries):
                    item = tarfile.TarInfo("RadioS/evil")
                    if form == "traversal":
                        item.name = "RadioS/../credentials.json"
                    elif form == "symlink":
                        item.type, item.linkname = tarfile.SYMTYPE, "/etc/passwd"
                    elif form == "hardlink":
                        item.type, item.linkname = tarfile.LNKTYPE, "RadioS/README.md"
                    elif form == "duplicate":
                        item = copy.copy(next(record for record, _ in entries if record.name == "RadioS/README.md"))
                        item.size = 0
                    else:
                        item.type, item.mode = tarfile.DIRTYPE, 0o755
                    entries.append((item, None if form in {"symlink", "hardlink", "directory"} else b""))
                self.rewrite(path, append)
                with self.assertRaises(packaging.PackageError):
                    packaging.verify(path)

    def test_corruption_missing_required_manifest_and_bounds_rejected(self):
        path = self.archive()
        body = path.read_bytes()
        path.write_bytes(body[:len(body) // 2])
        with self.assertRaises((packaging.PackageError, tarfile.TarError, EOFError)):
            packaging.verify(path)
        path = self.archive()
        self.rewrite(path, lambda entries: entries.__setitem__(slice(None),
                          [(item, data) for item, data in entries
                           if item.name != "RadioS/" + packaging.MANIFEST]))
        with self.assertRaises(packaging.PackageError):
            packaging.verify(path)
        path = self.archive()
        with patch.object(packaging, "MAX_FILES", 1):
            with self.assertRaises(packaging.PackageError):
                packaging.verify(path)

    def test_source_symlink_file_directory_and_dist_rejected(self):
        secret = self.root / "config/credentials.json"
        secret.write_text("PRIVATE")
        for relative in ("docs/leak.json", "docs/leakdir", "dist"):
            with self.subTest(relative=relative):
                link = self.root / relative
                if link.is_dir():
                    link.rmdir()
                link.symlink_to(secret if relative.endswith("json") else secret.parent,
                                target_is_directory=not relative.endswith("json"))
                with self.assertRaises((packaging.PackageError, OSError)):
                    self.package()
                link.unlink()

    def test_poisoned_template_never_produces_archive(self):
        with (self.root / "config/radios.example.ini").open("a") as stream:
            stream.write("\n[private]\nadmin_token = REAL_PRIVATE_KEY\n")
        with self.assertRaises(packaging.PackageError):
            self.package()
        self.assertFalse(list((self.root / "dist").glob("*.tar.gz")))
        self.assertFalse(list((self.root / "dist").glob(".package-*")))

    def test_source_hardlink_to_credentials_is_rejected(self):
        private = self.root / "config/credentials.json"
        private.write_text("PRIVATE")
        os.link(private, self.root / "docs/not-a-safe-document.json")
        with self.assertRaises(packaging.PackageError):
            self.package()
        self.assertFalse(list((self.root / "dist").glob("*.tar.gz")))

    def test_gzip_trailer_damage_and_manifest_types_are_rejected(self):
        path = self.archive()
        path.write_bytes(path.read_bytes()[:-1])
        with self.assertRaises(EOFError):
            packaging.verify(path)
        for version in ([], 14, None):
            with self.subTest(version=version):
                path = self.archive()
                def modify(entries):
                    for index, (item, body) in enumerate(entries):
                        if item.name == "RadioS/" + packaging.MANIFEST:
                            value = json.loads(body)
                            value["version"] = version
                            updated = packaging._json(value)
                            item.size = len(updated)
                            entries[index] = (item, updated)
                self.rewrite(path, modify)
                with self.assertRaises(packaging.PackageError):
                    packaging.verify(path)

    def test_failed_stage_keeps_existing_archive_and_cleans_temporary_files(self):
        path = self.archive()
        before = path.read_bytes()
        with patch.object(packaging, "verify", side_effect=packaging.PackageError("failed")):
            with self.assertRaises(packaging.PackageError):
                self.package()
        self.assertEqual(before, path.read_bytes())
        self.assertFalse(list((self.root / "dist").glob(".package-*")))

    def test_binary_runtime_manifest_and_executable_are_preserved(self):
        executable = b"\x7fELF" + b"FAKE_FIXTURE_FOR_LAYOUT_ONLY"
        runtime = {"architecture": "x86_64", "glibc_minimum": "2.34",
                   "needed_libraries": ["libavcodec.so.61"], "build_type": "Release"}
        with patch.object(packaging, "runtime_info", return_value=(executable, runtime)):
            result = packaging.package(self.root, self.root / "build/release", ("binary",))[0]
        with tarfile.open(self.root / "dist" / result["archive"], "r:gz") as tar:
            item = tar.getmember("RadioS/build/radios")
            self.assertEqual(0o755, item.mode)
            self.assertEqual(executable, tar.extractfile(item).read())
            self.assertEqual(runtime, json.load(tar.extractfile("RadioS/packaging/runtime.json")))
            self.assertNotIn("RadioS/src/app/main.c", tar.getnames())

    def test_binary_refuses_nonrelease_nonelf_and_external_build(self):
        build = self.root / "build/release"
        build.mkdir(parents=True)
        (build / "radios").write_bytes(b"\x7fELFfixture")
        (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Debug\n")
        with self.assertRaises(packaging.PackageError):
            packaging.runtime_info(self.root, build, packaging._version(self.root))
        (build / "radios").write_bytes(b"plain shell script")
        with self.assertRaises(packaging.PackageError):
            packaging.runtime_info(self.root, build, packaging._version(self.root))
        with self.assertRaises(packaging.PackageError):
            packaging.runtime_info(self.root, self.root.parent, packaging._version(self.root))


if __name__ == "__main__":
    unittest.main(verbosity=2)
