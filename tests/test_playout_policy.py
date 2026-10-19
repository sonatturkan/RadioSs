"""Persisted general-queue defaults accompany all future preparation kinds."""
import copy
import unittest
from uuid import uuid4

import test_desktop_playout as fixtures
from sdbase import desktop_api as api
from sdbase.storage import Conflict, StoreError


class PlayoutPolicy(fixtures.DesktopPlayout):
    def test_policy_roundtrip_all_preparation_kinds_and_cas(self):
        policy = {"schema": 1, "mode": 1, "overlap_ms": 1600, "beat_sync": True}
        saved = self.put("playout_policy", "main", policy)
        playlist = self.create()
        for kind, args in (("playlist", {"playlist_id": playlist["_id"]}),
                           ("programme", {"config": self.programme()}),
                           ("automation", {"config": self.automation(playlist["_id"])})):
            sequence = self.store.sequence
            prepared = self.prepare(kind, **args)
            self.assertEqual(prepared["playout_policy"], policy)
            self.assertEqual(self.store.sequence, sequence)
            prepared["playout_policy"]["mode"] = 3
            self.assertEqual(api.get_document(self.store, self.principal, self.sid,
                                             "playout_policy", saved["_id"])["config"], policy)
        updated = {"schema": 1, "mode": 0, "overlap_ms": 0, "beat_sync": False}
        self.put("playout_policy", "main", updated, document_id=saved["_id"],
                 expected_revision=saved["_rev"])
        self.assertEqual(self.prepare(playlist_id=playlist["_id"])["playout_policy"], updated)
        with self.assertRaises(Conflict):
            self.put("playout_policy", "main", policy, document_id=saved["_id"],
                     expected_revision=saved["_rev"])

    def test_policy_rejects_inconsistent_modes_and_unknown_fields(self):
        valid = {"schema": 1, "mode": 1, "overlap_ms": 3000, "beat_sync": True}
        for field, value in (("mode", 4), ("mode", True), ("overlap_ms", 30001),
                             ("overlap_ms", 0), ("beat_sync", 1), ("schema", 2),
                             ("secret", "unsupported")):
            bad = copy.deepcopy(valid)
            bad[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(StoreError):
                self.put("playout_policy", "main", bad)
        for mode in (0, 3):
            bad = {**valid, "mode": mode}
            with self.assertRaises(StoreError):
                self.put("playout_policy", "main", bad)
            self.assertEqual(api.validate_configuration("playout_policy",
                {"schema": 1, "mode": mode, "overlap_ms": 0, "beat_sync": False},
                self.sid, self.store._state)["mode"], mode)

    def test_one_active_canonical_policy(self):
        policy = {"schema": 1, "mode": 1, "overlap_ms": 1000, "beat_sync": False}
        self.put("playout_policy", "main", policy)
        with self.assertRaises(Conflict):
            self.put("playout_policy", "main", policy)
        with self.assertRaises(StoreError):
            self.put("playout_policy", "Other", policy)

    def test_no_policy_retains_legacy_preparation(self):
        self.assertNotIn("playout_policy", self.prepare(playlist_id=self.create()["_id"]))

    def test_disabled_old_policy_cannot_replace_recreated_canonical_policy(self):
        original = {"schema": 1, "mode": 1, "overlap_ms": 1600, "beat_sync": True}
        old = self.put("playout_policy", "main", original)
        disabled = api.delete_document(self.store, self.principal, self.sid, "playout_policy",
            old["_id"], old["_rev"], str(uuid4()))
        replacement = {"schema": 1, "mode": 0, "overlap_ms": 0, "beat_sync": False}
        current = self.put("playout_policy", "main", replacement)
        playlist = self.create()
        before = self.store.sequence, self.store.scan("desktop_configurations")
        with self.assertRaisesRegex(Conflict, "general playback policy already exists"):
            self.put("playout_policy", "main", original, document_id=old["_id"],
                     expected_revision=disabled["_rev"])
        self.assertEqual((self.store.sequence, self.store.scan("desktop_configurations")), before)
        active = [row for row in self.store.scan("desktop_configurations")
                  if row["kind"] == "playout_policy" and row["status"] == "active"]
        self.assertEqual([row["_id"] for row in active], [current["_id"]])
        self.assertEqual(self.prepare(playlist_id=playlist["_id"])["playout_policy"], replacement)


if __name__ == "__main__":
    # Run the new policy cases without repeating the inherited fixture suite.
    names = [name for name in PlayoutPolicy.__dict__ if name.startswith("test_")]
    result = unittest.TextTestRunner().run(unittest.TestSuite(PlayoutPolicy(name) for name in names))
    raise SystemExit(not result.wasSuccessful())
