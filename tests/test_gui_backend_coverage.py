#!/usr/bin/env python3
"""Kılavuz 2026-10-18 R10.1: every data-service operation reaches the GUI.

tools/gui_backend_coverage.py accounts for each workflow action and HTTP route
(GUI call, workflow hub form, an equivalent path, or a reasoned non-operator
classification). A new backend operation without any of these fails here.
"""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import gui_backend_coverage as coverage  # noqa: E402


class GuiBackendCoverage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = coverage.audit(ROOT)
        cls.state = {name: (state, evidence) for _, name, state, evidence in cls.rows}

    def test_no_operation_is_left_without_gui(self):
        gaps = [f"{kind} {name} {evidence}".strip() for kind, name, state, evidence in self.rows if state == "gap"]
        self.assertEqual(gaps, [])

    def test_every_workflow_action_and_route_is_listed(self):
        actions = coverage.workflow_actions(ROOT)
        routes = coverage.http_routes(ROOT)
        self.assertGreaterEqual(len(actions), 250)
        self.assertGreaterEqual(len(routes), 50)
        self.assertEqual(sum(1 for kind, *_ in self.rows if kind == "workflow"), len(actions))
        self.assertEqual(sum(1 for kind, *_ in self.rows if kind == "http"), len(routes))

    def test_connections_made_for_r10_1(self):
        for name in ("desktop/campaign-version", "desktop-admin/user-create", "GET /v1/import-profiles",
                     "POST /v1/import-profiles", "POST /v1/import-profiles/{id}",
                     "POST /v1/imports/rekordbox/preview", "POST /v1/imports/rekordbox/apply"):
            self.assertEqual(self.state[name][0], "gui", name)
        for name in ("plans/entry-put", "plans/approve", "plans/distribute"):
            self.assertEqual(self.state[name][0], "hub", name)
        self.assertEqual(self.state["POST /v1/plans/{id}/distribute"][0], "same")

    def test_classified_operations_carry_a_reason(self):
        for kind, name, state, evidence in self.rows:
            if state in {"not-gui", "same", "gui"}:
                self.assertTrue(evidence, f"{kind} {name}")


if __name__ == "__main__":
    unittest.main()
