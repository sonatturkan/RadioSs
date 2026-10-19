#!/usr/bin/env python3
"""Yönetim › Veritabanı durumu › Tam denetim: the whole-store integrity scrub job."""
from __future__ import annotations

from http.client import HTTPConnection
import json
import os
import subprocess
import threading
import time
import unittest
from unittest.mock import patch
from uuid import UUID, uuid4

import test_desktop_api as fixtures
from sdbase import desktop_integrity
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate
from sdbase.integrity import inspect_integrity
from sdbase.media import object_path
from sdbase.storage import StoreError
from sdbase.workflow_api import ACTIONS


class DesktopIntegrity(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal
        # Cleanups run last-in first-out: no job may still read the store when
        # its temporary directory is removed or the next test starts one.
        self.addCleanup(self.settle)

    def settle(self, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with desktop_integrity._LOCK:
                if not desktop_integrity._running_locked():
                    return
            time.sleep(0.02)
        self.fail("an integrity job was still running")

    def start(self, principal=None, **options):
        return desktop_integrity.integrity_start(self.store, principal or self.principal, self.sid, **options)

    def wait(self, job_id, timeout=60):
        deadline = time.monotonic() + timeout
        while True:
            view = desktop_integrity.integrity_job(self.store, self.principal, self.sid, job_id)
            if view["state"] != "running":
                return view
            self.assertLess(time.monotonic(), deadline, "integrity job did not finish in time")
            time.sleep(0.02)

    def user(self, permissions):
        created = create_station_user(self.store, self.principal, self.sid, "user" + str(uuid4())[:8],
                                      "Denetim kullanıcısı", list(permissions))
        return created["token"], authenticate(self.store, created["token"])

    def test_actions_are_registered_with_the_shared_signature(self):
        self.assertIs(ACTIONS["desktop-admin/integrity-start"], desktop_integrity.integrity_start)
        self.assertIs(ACTIONS["desktop-admin/integrity-job"], desktop_integrity.integrity_job)

    def test_full_check_reads_the_whole_store_and_verifies_media(self):
        binding = self.fixture.verified_binding()
        media_bytes = object_path(self.store, binding["media_sha256"]).stat().st_size
        database_bytes = sum(path.stat().st_size for path in (self.store.journal, self.store.meta_file)) + \
            (self.store.checkpoint_file.stat().st_size if self.store.checkpoint_file.exists() else 0)
        sequence = self.store.sequence
        started = self.start()
        self.assertEqual(started["state"], "running")
        self.assertEqual(str(UUID(started["job_id"])), started["job_id"])
        self.assertTrue(started["started_at"].endswith("+00:00"))
        self.assertNotIn("result", started)
        finished = self.wait(started["job_id"])
        self.assertEqual(finished["state"], "done", finished)
        self.assertEqual((finished["job_id"], finished["started_at"]), (started["job_id"], started["started_at"]))
        self.assertIn("finished_at", finished)
        report = finished["result"]
        self.assertEqual((report["status"], report["complete"], report["issue_count"]), ("ok", True, 0), report)
        self.assertEqual(report["checks"]["managed_media"],
                         {"status": "passed", "expected_objects": 1, "verified_objects": 1})
        for check in ("storage", "records", "indexes", "cold_history"):
            self.assertEqual(report["checks"][check]["status"], "passed", check)
        self.assertEqual(report["store_id"], self.store._meta()["store_id"])
        self.assertEqual(report["sequence"], sequence)
        # The budgets cover the database files and every managed object.
        self.assertTrue(report["limits"]["verify_media"] and report["limits"]["verify_cold"])
        self.assertGreaterEqual(report["limits"]["max_bytes"], database_bytes + media_bytes)
        self.assertGreaterEqual(report["work"]["bytes_read"], database_bytes + media_bytes)
        self.assertGreaterEqual(report["limits"]["max_records"], report["work"]["records_checked"])
        # A budget only slightly too small stops the same scrub honestly.
        bounded = inspect_integrity(self.store, max_bytes=report["work"]["bytes_read"] - 1,
                                    max_records=report["limits"]["max_records"])
        self.assertEqual(bounded["status"], "incomplete")
        # Reading never writes.
        self.assertEqual(self.store.sequence, sequence)

    def test_full_check_without_media_skips_hashing(self):
        self.fixture.verified_binding()
        finished = self.wait(self.start(verify_media=False)["job_id"])
        report = finished["result"]
        self.assertEqual(report["status"], "incomplete")
        self.assertEqual(report["checks"]["managed_media"], {"status": "skipped", "reason": "verification disabled"})
        self.assertFalse(report["limits"]["verify_media"])

    def test_second_concurrent_job_is_rejected(self):
        entered, release = threading.Event(), threading.Event()
        real = desktop_integrity._scan

        def held(*args, **kwargs):
            entered.set()
            release.wait(30)
            return real(*args, **kwargs)

        with patch.object(desktop_integrity, "_scan", held):
            first = self.start()
            try:
                self.assertTrue(entered.wait(10))
                with self.assertRaises(StoreError) as caught:
                    self.start()
                self.assertEqual(str(caught.exception), "Bir bütünlük denetimi zaten çalışıyor.")
                self.assertEqual(desktop_integrity.integrity_job(self.store, self.principal, self.sid,
                                                                 first["job_id"])["state"], "running")
            finally:
                release.set()
            self.assertEqual(self.wait(first["job_id"])["state"], "done")
        again = self.start()
        self.assertNotEqual(again["job_id"], first["job_id"])
        self.assertEqual(self.wait(again["job_id"])["result"]["status"], "ok")

    def test_scrub_runs_in_a_separate_process_and_reports_its_errors(self):
        calls = []
        real_run = desktop_integrity.subprocess.run

        def recorded(command, **kwargs):
            calls.append(command)
            return real_run(command, **kwargs)

        with patch.object(desktop_integrity.subprocess, "run", recorded):
            finished = self.wait(self.start()["job_id"])
        self.assertEqual(finished["result"]["status"], "ok")
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][1:3], ["-m", "sdbase.cli"])
        self.assertIn("integrity", calls[0])
        self.assertEqual(calls[0][calls[0].index("--db") + 1], str(self.store.journal))
        failed = subprocess.CompletedProcess([], 1, "", "sdbase: database source path must not contain symlinks\n")
        with patch.object(desktop_integrity.subprocess, "run", lambda *a, **k: failed):
            finished = self.wait(self.start()["job_id"])
        self.assertEqual((finished["state"], finished["message"]),
                         ("failed", "Tam denetim tamamlanamadı: database source path must not contain symlinks"))

    def test_unexpected_scrub_error_ends_the_job_as_failed(self):
        def broken(*_args, **_kwargs):
            raise RuntimeError("disk okunamadı")

        with patch.object(desktop_integrity, "_scan", broken):
            finished = self.wait(self.start()["job_id"])
        self.assertEqual(finished["state"], "failed")
        self.assertEqual(finished["message"], "Tam denetim tamamlanamadı: RuntimeError: disk okunamadı")
        self.assertNotIn("result", finished)
        self.assertIn("finished_at", finished)
        # A failed job frees the single slot.
        self.assertEqual(self.wait(self.start()["job_id"])["result"]["status"], "ok")

    def test_finished_jobs_are_bounded(self):
        first = self.start()
        self.wait(first["job_id"])
        for _ in range(desktop_integrity.MAX_FINISHED_JOBS):
            self.wait(self.start()["job_id"])
        with desktop_integrity._LOCK:
            kept = list(desktop_integrity._JOBS.values())
        self.assertLessEqual(len(kept), desktop_integrity.MAX_FINISHED_JOBS)
        with self.assertRaises(StoreError) as caught:
            desktop_integrity.integrity_job(self.store, self.principal, self.sid, first["job_id"])
        self.assertEqual(str(caught.exception), "Bütünlük denetimi bulunamadı.")

    def test_view_only_and_station_managers_are_forbidden(self):
        job = self.start()
        self.wait(job["job_id"])
        for permissions in (("view",), ("view", "manage")):
            _, principal = self.user(permissions)
            with self.assertRaises(Forbidden):
                self.start(principal)
            with self.assertRaises(Forbidden):
                desktop_integrity.integrity_job(self.store, principal, self.sid, job["job_id"])
        with self.assertRaises(StoreError):
            desktop_integrity.integrity_start(self.store, self.principal, "istasyon")
        with self.assertRaises(StoreError):
            self.start(verify_media="evet")
        with self.assertRaises(StoreError):
            desktop_integrity.integrity_job(self.store, self.principal, self.sid, "iş")
        with self.assertRaises(StoreError):
            desktop_integrity.integrity_job(self.store, self.principal, self.sid, str(uuid4()))

    def test_corrupted_media_object_is_reported(self):
        binding = self.fixture.verified_binding()
        path = object_path(self.store, binding["media_sha256"])
        original = path.read_bytes()
        damaged = bytearray(original)
        damaged[len(damaged) // 2] ^= 0xFF
        os.chmod(path, 0o600)
        path.write_bytes(bytes(damaged))
        finished = self.wait(self.start()["job_id"])
        self.assertEqual(finished["state"], "done", finished)
        report = finished["result"]
        self.assertEqual(report["status"], "invalid")
        self.assertEqual(report["checks"]["managed_media"],
                         {"status": "failed", "expected_objects": 1, "verified_objects": 0})
        self.assertEqual(report["issue_count"], 1)
        issue = report["issues"][0]
        self.assertEqual(issue["code"], "managed_media_invalid")
        self.assertIn(binding["media_sha256"], issue["detail"])
        self.assertIn("checksum differs", issue["detail"])
        # Restored bytes verify again.
        path.write_bytes(original)
        self.assertEqual(self.wait(self.start()["job_id"])["result"]["status"], "ok")

    def test_http_workflow_routes(self):
        parent = make_handler(self.store)

        class Handler(parent):
            def log_message(self, *_args):
                pass

        server = BoundedHTTPServer(("127.0.0.1", 0), Handler, request_timeout=3)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        self.addCleanup(worker.join, 3)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)

        def post(action, body, token=None):
            connection = HTTPConnection("127.0.0.1", server.server_port, timeout=10)
            try:
                connection.request("POST", "/v1/workflows/desktop-admin/" + action, json.dumps(body),
                                   {"Content-Type": "application/json",
                                    "Authorization": "Bearer " + (token or self.fixture.identity["api_token"])})
                response = connection.getresponse()
                return response.status, json.loads(response.read())
            finally:
                connection.close()

        status, started = post("integrity-start", {"station_id": self.sid})
        self.assertEqual((status, started["state"]), (200, "running"))
        deadline = time.monotonic() + 60
        while True:
            status, view = post("integrity-job", {"station_id": self.sid, "job_id": started["job_id"]})
            self.assertEqual(status, 200)
            if view["state"] != "running":
                break
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.05)
        self.assertEqual((view["state"], view["result"]["status"]), ("done", "ok"))
        viewer, _ = self.user(("view",))
        self.assertEqual(post("integrity-start", {"station_id": self.sid}, viewer)[0], 403)
        self.assertEqual(post("integrity-job", {"station_id": self.sid, "job_id": started["job_id"]}, viewer)[0], 403)
        self.assertEqual(post("integrity-start", {"station_id": self.sid, "unknown": True})[0], 422)
        entered, release = threading.Event(), threading.Event()
        real = desktop_integrity._scan

        def held(*args, **kwargs):
            entered.set()
            release.wait(30)
            return real(*args, **kwargs)

        with patch.object(desktop_integrity, "_scan", held):
            status, running = post("integrity-start", {"station_id": self.sid, "verify_media": True})
            try:
                self.assertEqual(status, 200)
                self.assertTrue(entered.wait(10))
                status, refused = post("integrity-start", {"station_id": self.sid})
                self.assertEqual((status, refused["detail"]), (422, "Bir bütünlük denetimi zaten çalışıyor."))
            finally:
                release.set()
        self.settle()


if __name__ == "__main__":
    unittest.main(verbosity=2)
