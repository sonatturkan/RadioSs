"""Approved native vocal windows use private real DB/media, never user data."""
import copy
import unittest
from uuid import uuid4
import test_desktop_playout as fixtures
from sdbase import catalog, radio_cues
from sdbase.schema import open_radio_store
from sdbase.integrity import inspect_integrity


class PlayoutVocalCues(unittest.TestCase):
    def setUp(self):
        self.helper = fixtures.DesktopPlayout("runTest")
        self.helper.setUp()
        self.addCleanup(self.helper.doCleanups)
        self.store, self.sid, self.principal = self.helper.store, self.helper.sid, self.helper.principal
        self.binding = self.helper.fixture.verified_binding()
        version = self.store.get("versions", self.binding["version_id"])
        catalog.approve_version(self.store, self.principal, self.sid, version["_id"], expected_revision=version["_rev"])
        self.playlist = self.helper.save(self.helper.create(), self.binding)

    def cue(self, kind, position, *, approve=True, station=None, binding=None):
        binding = binding or self.binding
        station = station or self.sid
        cue = radio_cues.create_radio_cue(self.store, self.principal, station,
            binding["version_id"], cue_kind=kind, position_value=position, unit="milliseconds",
            media_file_id=binding["media_file_id"])
        return radio_cues.approve_radio_cue(self.store, self.principal, station, cue["_id"],
            expected_revision=cue["_rev"]) if approve else cue

    def prepared(self):
        sequence = self.store.sequence
        result = self.helper.prepare(playlist_id=self.playlist["_id"])
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(result["sequence"], sequence)
        self.assertTrue(result["tracks"][0]["eligible"])
        return result["tracks"][0]["detail"]

    def test_approved_only_conservative_bounded_immutable_window_reopen(self):
        draft = self.cue("vocal_start", 1, approve=False)
        self.assertNotIn("radio_cues", self.prepared())
        early = self.cue("vocal_start", 30)
        self.cue("vocal_start", 60)
        intro = self.cue("intro_end", 40)
        self.cue("outro_start", 100)
        outro = self.cue("outro_start", 150)
        self.cue("fade_start", 20)
        expected = {early["_id"], intro["_id"], outro["_id"]}
        detail = self.prepared()
        self.assertEqual({cue["_id"] for cue in detail["radio_cues"]}, expected)
        self.assertEqual(len(detail["radio_cues"]), 3)
        for cue in detail["radio_cues"]:
            self.assertGreaterEqual(cue["_rev"], 2)
            self.assertEqual(cue["status"], "approved")
            self.assertEqual(cue["station_id"], self.sid)
            for name in ("version_id", "media_file_id", "media_sha256"):
                self.assertEqual(cue[name], self.binding[name])
            self.assertEqual(cue["media_duration_ms"], 200)
        self.assertNotIn(draft["_id"], expected)
        original = copy.deepcopy(detail["radio_cues"])
        detail["radio_cues"][0]["position_ms"] = 199
        self.assertEqual(self.prepared()["radio_cues"], original)
        reopened = open_radio_store(self.helper.fixture.db, read_only=True)
        self.assertEqual(reopened.get("radio_cues", early["_id"]), early)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_withdrawn_wrong_station_and_other_version_are_excluded(self):
        revoked = self.cue("outro_start", 100)
        radio_cues.withdraw_radio_cue(self.store, self.principal, self.sid, revoked["_id"],
            expected_revision=revoked["_rev"], reason="Private regression withdrawal")
        other_station = str(uuid4())
        self.store.put("stations", other_station, {"code":"PRIVATE2", "name":"Private station", "timezone":"UTC", "status":"active"})
        item_id = self.store.get("versions", self.binding["version_id"])["item_id"]
        catalog.share_item(self.store, self.principal, other_station, item_id)
        foreign = self.cue("outro_start", 150, station=other_station)
        other = self.helper.fixture.verified_binding()
        version = self.store.get("versions", other["version_id"])
        catalog.approve_version(self.store, self.principal, self.sid, version["_id"], expected_revision=version["_rev"])
        unrelated = self.cue("vocal_start", 20, binding=other)
        self.assertNotIn("radio_cues", self.prepared())
        self.assertEqual(self.store.get("radio_cues", foreign["_id"])["status"], "approved")
        self.assertEqual(self.store.get("radio_cues", unrelated["_id"])["status"], "approved")


if __name__ == "__main__":
    unittest.main()
