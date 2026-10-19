"""Private real-byte AutoDJ assignment: boundaries, preservation and CAS."""
from copy import deepcopy
from http.client import HTTPConnection
import hashlib
import json
import math
import struct
import threading
import unittest
from uuid import uuid4
from unittest.mock import patch
import wave

import test_desktop_media as fixtures
from sdbase import desktop_autodj as api, desktop_media, desktop_playlist, desktop_playout
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.catalog import NotFound
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError


class DesktopAutoDJ(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopMedia("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal

    def media(self, *, silent=False, title="Ölçülmüş şarkı"):
        path = self.fixture.imports / (str(uuid4()) + ".wav")
        samples = [0 if silent or index < 8000 or index >= 40000 else
                   round(16000 * math.sin(2 * math.pi * 400 * index / 8000))
                   for index in range(48000)]
        with wave.open(str(path), "wb") as output:
            output.setnchannels(1); output.setsampwidth(2); output.setframerate(8000)
            output.writeframes(struct.pack("<48000h", *samples))
        path.chmod(0o600)
        normalized = [sample / 32768 for sample in samples]
        waveform = []
        for start in range(0, len(samples), 400):
            waveform.extend((min(normalized[start:start + 400]), max(normalized[start:start + 400])))
        analysis = {**self.fixture.analysis, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    "size_bytes": path.stat().st_size, "duration_ms": 6000,
                    "peak": max(map(abs, normalized)), "rms": math.sqrt(sum(value * value for value in normalized) / len(normalized)),
                    "sample_frames": 48000, "waveform": waveform}
        return self.fixture.load(import_path=path.name, analysis=analysis,
                                 metadata={**self.fixture.metadata, "title": title})

    def args(self, detail, **changes):
        result = {"station_id": self.sid, "media_id": detail["media"]["_id"],
                  "version_id": detail["version"]["_id"], "media_sha256": detail["version"]["sha256"],
                  "expected_revision": detail["media"]["_rev"],
                  "expected_marker_revision": detail["markers"]["_rev"] if detail["markers"] else None,
                  "idempotency_key": str(uuid4())}
        result.update(changes)
        return result

    def assign(self, detail, **changes):
        return api.assign_markers(self.store, self.principal, **self.args(detail, **changes))

    def marked(self, detail, **changes):
        markers = {"cue_in_ms": 500, "intro_ms": 2000, "out_ms": 5500,
                   "fade_start_ms": 4500, "fade_end_ms": 5500, "auto_next_ms": 5000,
                   "overlap_ms": 500, "automatic": True, **changes}
        return desktop_media.save_markers(self.store, self.principal, self.sid, detail["media"]["_id"],
            detail["media"]["_rev"], detail["markers"]["_rev"] if detail["markers"] else None, str(uuid4()), markers)

    def playlist(self, *details):
        playlist = desktop_playlist.create_playlist(self.store, self.principal, self.sid,
            "Private selected playlist", idempotency_key=str(uuid4()))
        for detail in details:
            binding = {"media_id": detail["media"]["_id"], "version_id": detail["version"]["_id"],
                       "media_file_id": detail["version"]["media_file_id"], "media_sha256": detail["version"]["sha256"]}
            metadata = {"title": "Private track", "artist": "", "album": "", "duration_ms": 6000,
                        "bpm": 0, "genre": "", "key": "", "location": "", "comment": "", **binding}
            playlist = desktop_playlist.save_entry(self.store, self.principal, self.sid, playlist["_id"], metadata,
                expected_revision=playlist["_rev"], idempotency_key=str(uuid4()))
        return playlist

    def test_measured_span_durable_replay_and_real_playout_binding(self):
        detail = self.media()
        playlist = self.playlist(detail)
        immutable = self.store.scan("desktop_media_analysis")
        args = self.args(detail, out_lead_ms=1000, playlist_id=playlist["_id"])
        result = api.assign_markers(self.store, self.principal, **args)
        self.assertEqual((result["status"], result["reason"]), ("updated", "assigned"))
        marks = result["detail"]["markers"]
        self.assertEqual((marks["autodj_in_ms"], marks["autodj_out_ms"]), (1000, 4000))
        self.assertEqual((marks["cue_in_ms"], marks["out_ms"], marks["automatic"]), (0, 6000, False))
        self.assertEqual(self.store.scan("desktop_media_analysis"), immutable)
        sequence = self.store.sequence
        self.assertEqual(api.assign_markers(self.store, self.principal, **args), result)
        self.assertEqual(self.store.sequence, sequence)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        reread = desktop_media.get_media(reopened, self.principal, self.sid, detail["media"]["_id"])
        self.assertEqual(reread["markers"], marks)
        prepared = desktop_playout.prepare(reopened, self.principal, self.sid, "playlist", playlist_id=playlist["_id"])
        self.assertTrue(prepared["tracks"][0]["eligible"])
        self.assertEqual(prepared["tracks"][0]["detail"]["markers"], marks)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        with self.assertRaises(Conflict):
            api.assign_markers(self.store, self.principal, **dict(args, idempotency_key=str(uuid4())))
        with self.assertRaises(Conflict):
            api.assign_markers(self.store, self.principal, **dict(args, out_lead_ms=0))

    def test_overwrite_only_autodj_preserves_cues_and_manual_grid(self):
        detail = self.media()
        measured = deepcopy(detail["analysis"])
        measured["rhythm"] = {"schema": 1, "algorithm": "radios-manual-grid-v1", "bpm": 120.0,
                              "first_beat_ms": 1000, "confidence": 1.0, "analyzed_ms": 6000}
        detail = desktop_media.save_analysis(self.store, self.principal, self.sid, detail["media"]["_id"],
            detail["version"]["_id"], detail["media"]["_rev"], detail["analysis_revision"], str(uuid4()), measured)
        detail = self.marked(detail, autodj_in_ms=1500, autodj_out_ms=3500)
        original = deepcopy(detail["markers"])
        # Points the operator placed are manual; neither upkeep nor overwrite replaces them.
        self.assertEqual(original["autodj_source"], {"autodj_in_ms": "manual", "autodj_out_ms": "manual"})
        rhythm = self.store.scan("desktop_media_rhythm")
        for overwrite in (False, True):
            with patch.object(api, "_points", wraps=api._points) as calculate:
                skipped = self.assign(detail, out_lead_ms=1000, overwrite=overwrite)
                calculate.assert_not_called()
            self.assertEqual((skipped["status"], skipped["reason"]), ("skipped", "existing_markers"))
            self.assertEqual(skipped["detail"]["markers"], original)
        with patch.object(api, "_points", wraps=api._points) as calculate:
            result = self.assign(detail, out_lead_ms=1000, replace_manual=True)
            calculate.assert_called_once()
            self.assertEqual(calculate.call_args.args[1:5], (500, 5500, -45, 1000))
            self.assertEqual(calculate.call_args.args[5], measured["rhythm"])
        self.assertEqual(result["status"], "updated")
        marks = result["detail"]["markers"]
        self.assertEqual((marks["autodj_in_ms"], marks["autodj_out_ms"]), (1000, 4000))
        self.assertEqual(marks["autodj_source"], {"autodj_in_ms": api.ALGORITHM, "autodj_out_ms": api.ALGORITHM})
        for key in desktop_media._MARKER_KEYS | {"created_at", "created_by", "_id"}:
            self.assertEqual(marks[key], original[key], key)
        self.assertEqual(self.store.scan("desktop_media_rhythm"), rhythm)
        self.assertEqual(result["detail"]["analysis"]["rhythm"], measured["rhythm"])
        unchanged = self.assign(result["detail"], out_lead_ms=1000, overwrite=True)
        self.assertEqual(unchanged["reason"], "unchanged")

    def test_point_provenance_manual_moves_and_legacy_upgrade(self):
        automatic = self.assign(self.media(), out_lead_ms=1000)["detail"]
        self.assertEqual(automatic["markers"]["autodj_source"],
                         {"autodj_in_ms": api.ALGORITHM, "autodj_out_ms": api.ALGORITHM})
        # Moving one point on the waveform makes only that point manual; a
        # cue edit that resends the same AutoDJ values keeps their sources.
        moved = self.marked(automatic, cue_in_ms=0, autodj_in_ms=1000, autodj_out_ms=4500)
        self.assertEqual(moved["markers"]["autodj_source"],
                         {"autodj_in_ms": api.ALGORITHM, "autodj_out_ms": "manual"})
        recalculated = self.assign(moved, out_lead_ms=500, overwrite=True)
        self.assertEqual(recalculated["detail"]["markers"]["autodj_out_ms"], 4500)
        self.assertEqual(recalculated["detail"]["markers"]["autodj_source"]["autodj_out_ms"], "manual")
        cleared = self.marked(recalculated["detail"], autodj_in_ms=None, autodj_out_ms=None)
        self.assertNotIn("autodj_source", cleared["markers"])
        # Points stored before provenance existed are earlier automatic
        # analyses: the next pass recalculates them without overwrite.
        legacy = self.marked(self.media(), autodj_in_ms=1200, autodj_out_ms=1900,
                             autodj_source={"autodj_in_ms": "manual", "autodj_out_ms": "manual"})
        row = self.store.lookup("desktop_media_markers", "station_media", self.sid, legacy["version"]["media_file_id"])[0]
        data = {key: value for key, value in row.items() if not key.startswith("_") and key != "autodj_source"}
        self.store.put("desktop_media_markers", row["_id"], data, expected_revision=row["_rev"])
        legacy = desktop_media.get_media(self.store, self.principal, self.sid, legacy["media"]["_id"])
        upgraded = self.assign(legacy, out_lead_ms=1000)
        self.assertEqual((upgraded["status"], upgraded["detail"]["markers"]["autodj_in_ms"],
                          upgraded["detail"]["markers"]["autodj_out_ms"]), ("updated", 1000, 4000))
        with self.assertRaises(StoreError):
            self.marked(self.media(), autodj_in_ms=1000, autodj_source={"autodj_in_ms": "guess"})

    def test_recalculated_identical_points_preserve_revisions_and_replay(self):
        detail = self.assign(self.media(), out_lead_ms=1000)["detail"]
        immutable = {table: deepcopy(self.store.scan(table)) for table in (
            "desktop_media_library", "desktop_media_markers", "desktop_media_analysis",
            "desktop_media_rhythm")}
        args = self.args(detail, out_lead_ms=1000, overwrite=True)
        sequence = self.store.sequence
        with patch.object(api, "_points", wraps=api._points) as calculate:
            result = api.assign_markers(self.store, self.principal, **args)
            calculate.assert_called_once()
        # 'skipped' is the compatible HTTP write status. Its 'unchanged'
        # reason proves recalculation succeeded and found the same pair.
        self.assertEqual((result["status"], result["reason"]), ("skipped", "unchanged"))
        self.assertIn("yeniden hesaplandı", result["reason_text"])
        self.assertEqual(result["detail"]["media"], detail["media"])
        self.assertEqual(result["detail"]["markers"], detail["markers"])
        for table, rows in immutable.items():
            self.assertEqual(self.store.scan(table), rows, table)
        # The only commit is a durable operation receipt, not fake marker or
        # library revisions; replay returns it without calculating or writing.
        self.assertEqual(self.store.sequence, sequence + 1)
        sequence = self.store.sequence
        with patch.object(api, "_points", side_effect=AssertionError("replay recalculated")):
            self.assertEqual(api.assign_markers(self.store, self.principal, **args), result)
        self.assertEqual(self.store.sequence, sequence)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        with patch.object(api, "_points", side_effect=AssertionError("durable replay recalculated")):
            self.assertEqual(api.assign_markers(reopened, self.principal, **args), result)
        for table, rows in immutable.items():
            self.assertEqual(reopened.scan(table), rows, table)

        # Current automatic points are protected when overwrite is off, and
        # recalculated when it is on. The ordinary Cue/fade settings stay.
        with patch.object(api, "_points", side_effect=AssertionError("protected points recalculated")):
            protected = self.assign(detail, out_lead_ms=500, overwrite=False)
        self.assertEqual(protected["reason"], "existing_markers")
        self.assertEqual(protected["detail"]["markers"], detail["markers"])
        with patch.object(api, "_points", wraps=api._points) as calculate:
            changed = self.assign(detail, out_lead_ms=500, overwrite=True)
            calculate.assert_called_once()
        self.assertEqual((changed["status"], changed["reason"]), ("updated", "assigned"))
        marks = changed["detail"]["markers"]
        self.assertEqual((marks["autodj_in_ms"], marks["autodj_out_ms"]), (1000, 4500))
        self.assertEqual(changed["detail"]["media"]["_rev"], detail["media"]["_rev"] + 1)
        self.assertEqual(marks["_rev"], detail["markers"]["_rev"] + 1)
        for key in desktop_media._MARKER_KEYS | {"created_at", "created_by", "_id"}:
            self.assertEqual(marks[key], detail["markers"][key], key)
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def rhythmic_media(self, outro_bar):
        """70 s at 120 BPM, bars from 1000 ms: a tone with a kick on every beat,
        then (from ``outro_bar``) kicks only. Sound starts with a 600 ms pickup."""
        path = self.fixture.imports / (str(uuid4()) + ".wav")
        outro_ms = 1000 + outro_bar * 2000 if outro_bar is not None else 69000
        samples = []
        for index in range(560000):
            ms = index / 8
            kick = 1000 <= ms < 69000 and (ms - 1000) % 500 < 100
            tone = 600 <= ms < outro_ms
            samples.append(round((14000 * math.sin(2 * math.pi * 60 * index / 8000) if kick else 0) +
                                 (6000 * math.sin(2 * math.pi * 400 * index / 8000) if tone else 0)))
        with wave.open(str(path), "wb") as output:
            output.setnchannels(1); output.setsampwidth(2); output.setframerate(8000)
            output.writeframes(struct.pack("<560000h", *samples))
        path.chmod(0o600)
        normalized = [sample / 32768 for sample in samples]
        waveform = []
        for start in range(0, len(samples), 400):
            waveform.extend((min(normalized[start:start + 400]), max(normalized[start:start + 400])))
        analysis = {**self.fixture.analysis, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    "size_bytes": path.stat().st_size, "duration_ms": 70000,
                    "peak": max(map(abs, normalized)), "rms": math.sqrt(sum(value * value for value in normalized) / len(normalized)),
                    "sample_frames": 560000, "waveform": waveform}
        detail = self.fixture.load(import_path=path.name, analysis=analysis,
                                   metadata={**self.fixture.metadata, "title": "Ritimli şarkı"})
        measured = deepcopy(detail["analysis"])
        measured["rhythm"] = {"schema": 1, "algorithm": "radios-manual-grid-v1", "bpm": 120.0,
                              "first_beat_ms": 1000, "confidence": 1.0, "analyzed_ms": 70000}
        return desktop_media.save_analysis(self.store, self.principal, self.sid, detail["media"]["_id"],
            detail["version"]["_id"], detail["media"]["_rev"], detail["analysis_revision"], str(uuid4()), measured)

    def structural_measurement(self, shape="section"):
        """Real measured PCM: a musical body, break and sustained dense ending.

        The body has a repeating melodic envelope and beat pulses. The final
        section keeps a high floor; min/max bins retain the same 64 ms
        resolution as a long native import. Controls preserve the waveform
        shape, vary gain slowly, or introduce only a short late disturbance.
        """
        from array import array
        import sys

        path = self.fixture.imports / (str(uuid4()) + ".wav")
        rate, frames, hop = 8000, 1040000, 512
        samples, waveform = array("h"), []
        low, high, squares, peak = 1.0, -1.0, 0.0, 0.0
        for index in range(frames):
            ms = index * 1000 / rate
            pulse = math.exp(-(ms % 500) / 500 * 35)
            base = .12 if shape == "section" and 76000 <= ms < 100000 else .35
            amplitude = (base + .3 * pulse) * (.35 + .65 * abs(math.sin(2 * math.pi * ms / 800)))
            if (shape == "section" and 100000 <= ms < 129000) or \
                    (shape == "burst" and 100000 <= ms < 106000):
                amplitude = .65 + .65 * pulse
            if shape == "gain" and ms >= 100000:
                amplitude *= 1.25
            if shape == "fade" and ms >= 90000:
                amplitude *= max(0, (129000 - ms) / 39000)
            if shape == "spike" and 120000 <= ms < 120050:
                amplitude = 2
            sample = round(max(-1, min(1, amplitude * math.sin(2 * math.pi * 400 * index / rate))) * 32767) if ms < 129000 else 0
            samples.append(sample)
            value = sample / 32768
            low, high = min(low, value), max(high, value)
            squares += value * value; peak = max(peak, abs(value))
            if (index + 1) % hop == 0 or index + 1 == frames:
                waveform.extend((low, high)); low, high = 1.0, -1.0
        if sys.byteorder != "little":
            samples.byteswap()
        with wave.open(str(path), "wb") as output:
            output.setnchannels(1); output.setsampwidth(2); output.setframerate(rate)
            output.writeframes(samples.tobytes())
        path.chmod(0o600)
        analysis = {**self.fixture.analysis, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    "size_bytes": path.stat().st_size, "duration_ms": 130000, "sample_rate": rate,
                    "peak": peak, "rms": math.sqrt(squares / frames), "sample_frames": frames,
                    "waveform_frames_per_bin": hop, "waveform": waveform,
                    "clipped": False, "clipped_samples": 0}
        return path, analysis

    def test_dense_final_section_is_not_an_exit_and_preserves_physical_tail(self):
        path, analysis = self.structural_measurement()
        detail = self.fixture.load(import_path=path.name, analysis=analysis,
                                   metadata={**self.fixture.metadata, "title": "Yoğun son bölüm"})
        measured = deepcopy(detail["analysis"])
        measured["rhythm"] = {"schema": 1, "algorithm": "radios-manual-grid-v1", "bpm": 120.0,
                              "first_beat_ms": 0, "downbeat_ms": 0, "confidence": 1.0, "analyzed_ms": 120000}
        detail = desktop_media.save_analysis(self.store, self.principal, self.sid, detail["media"]["_id"],
            detail["version"]["_id"], detail["media"]["_rev"], detail["analysis_revision"], str(uuid4()), measured)
        original_analysis = deepcopy(detail["analysis"])
        result = self.assign(detail, overwrite=True, out_lead_ms=1000)
        # The song keeps its energy until the sound stops at 129 s: OUT is
        # the bar start two bars before that, in the final part of the song.
        self.assertEqual((result["status"], result["detail"]["markers"]["autodj_in_ms"],
                          result["detail"]["markers"]["autodj_out_ms"]), ("updated", 0, 124000))
        marks = result["detail"]["markers"]
        # OUT triggers a transition. It does not shorten the source or alter
        # Cue/fades/manual grid.
        self.assertEqual((marks["cue_in_ms"], marks["out_ms"], marks["fade_start_ms"],
                          marks["fade_end_ms"], marks["auto_next_ms"]), (0, 130000, 130000, 130000, 130000))
        self.assertEqual(result["detail"]["analysis"], original_analysis)
        reopened = desktop_media.get_media(open_radio_store(self.fixture.db, read_only=True), self.principal,
                                          self.sid, detail["media"]["_id"])
        self.assertEqual(reopened["markers"], marks)
        self.assertEqual(reopened["analysis"]["rhythm"], measured["rhythm"])
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_without_trusted_grid_uses_measured_windows(self):
        _, analysis = self.structural_measurement()
        for rhythm in (None, {"bpm": 120, "confidence": .2, "first_beat_ms": 0},
                       {"bpm": 0, "confidence": 0, "first_beat_ms": 0}):
            with self.subTest(rhythm=rhythm):
                points, reason = api._points(analysis, 0, 130000, -45, 0, rhythm)
                self.assertIsNone(reason)
                self.assertEqual(points, (0, 124000))

    def test_fade_starts_the_exit_and_steady_endings_leave_two_bars(self):
        rhythm = {"bpm": 120, "confidence": 1, "first_beat_ms": 0, "downbeat_ms": 0}
        for shape, expected in (("flat", 124000), ("gain", 124000), ("burst", 124000),
                                ("spike", 124000), ("fade", 100000)):
            _, analysis = self.structural_measurement(shape)
            for grid in (rhythm, None):
                with self.subTest(shape=shape, grid=grid is not None):
                    points, reason = api._points(analysis, 0, 130000, -45, 0, grid)
                    self.assertIsNone(reason)
                    self.assertEqual(points, (0, expected))
                    # OUT is never in the first part of the song.
                    self.assertGreater(points[1], 0.55 * 129000)

    def test_beat_grid_mixes_in_beats_only_outro_not_at_the_end(self):
        # The kicks-only outro starts at bar 22 (45 s): OUT is that bar start
        # and IN is the first downbeat, not the pickup. The lead is not
        # applied to an outro.
        result = self.assign(self.rhythmic_media(22), out_lead_ms=1000)
        marks = result["detail"]["markers"]
        self.assertEqual((result["status"], marks["autodj_in_ms"], marks["autodj_out_ms"]), ("updated", 1000, 45000))
        # A song that never thins out leaves two bars before its sound ends.
        result = self.assign(self.rhythmic_media(None), out_lead_ms=1000)
        marks = result["detail"]["markers"]
        self.assertEqual((marks["autodj_in_ms"], marks["autodj_out_ms"]), (1000, 65000))

    def test_partial_pair_crop_and_conflicting_preserved_point(self):

        detail = self.marked(self.media(), autodj_in_ms=1700)
        result = self.assign(detail, out_lead_ms=1000)
        self.assertEqual((result["detail"]["markers"]["autodj_in_ms"], result["detail"]["markers"]["autodj_out_ms"]), (1700, 4000))
        cropped = self.marked(result["detail"], cue_in_ms=2000, intro_ms=2000, out_ms=4500,
                              fade_start_ms=4000, fade_end_ms=4500, auto_next_ms=4500,
                              autodj_in_ms=None, autodj_out_ms=None)
        result = self.assign(cropped, out_lead_ms=500)
        self.assertEqual((result["detail"]["markers"]["autodj_in_ms"], result["detail"]["markers"]["autodj_out_ms"]), (2000, 4000))
        conflict = self.marked(result["detail"], autodj_in_ms=4500, autodj_out_ms=None)
        skipped = self.assign(conflict, out_lead_ms=1000)
        self.assertEqual(skipped["reason"], "preserved_marker_conflict")
        self.assertEqual(skipped["detail"]["markers"], conflict["markers"])
        # The manual IN survives overwrite; only an explicit manual replacement moves it.
        self.assertEqual(self.assign(conflict, out_lead_ms=1000, overwrite=True)["reason"], "preserved_marker_conflict")
        forced = self.assign(conflict, out_lead_ms=1000, replace_manual=True)
        self.assertEqual(forced["status"], "updated")

    def test_silent_threshold_unavailable_and_excessive_lead_skip(self):
        silent = self.media(silent=True)
        self.assertEqual(self.assign(silent)["reason"], "no_audible_audio")
        self.assertIsNone(desktop_media.get_media(self.store, self.principal, self.sid, silent["media"]["_id"])["markers"])
        detail = self.media()
        self.assertEqual(self.assign(detail, threshold_db=-6)["reason"], "no_audible_audio")
        self.assertEqual(self.assign(detail, out_lead_ms=4000)["reason"], "out_lead_exceeds_span")
        from sdbase.media import object_path
        object_path(self.store, detail["version"]["sha256"]).unlink()
        self.assertEqual(self.assign(detail)["reason"], "media_unavailable")
        self.assertFalse(self.store.scan("desktop_media_markers"))

    def test_current_playlist_membership_identity_cas_and_tenant(self):
        detail, outside = self.media(), self.media(title="Outside")
        playlist = self.playlist(detail)
        self.assertEqual(self.assign(detail, playlist_id=playlist["_id"])["status"], "updated")
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            self.assign(outside, playlist_id=playlist["_id"])
        self.assertEqual(self.store.sequence, sequence)
        self.assertIsNone(desktop_media.get_media(self.store, self.principal, self.sid, outside["media"]["_id"])["markers"])
        for changes in ({"version_id": str(uuid4())}, {"media_sha256": "a" * 64},
                        {"expected_revision": outside["media"]["_rev"] + 1}, {"expected_marker_revision": 1}):
            with self.assertRaises(Conflict): self.assign(outside, **changes)
        viewer = create_station_user(self.store, self.principal, self.sid, "autodj-viewer", "Viewer", ["view"])
        with self.assertRaises(Forbidden):
            api.assign_markers(self.store, authenticate(self.store, viewer["token"]), **self.args(outside))
        other_sid = str(uuid4())
        self.store.put("stations", other_sid, {"code": "OTHER", "name": "Other", "timezone": "UTC", "status": "active"})
        with self.assertRaises(NotFound): self.assign(outside, station_id=other_sid)

    def test_playlist_removed_during_measurement_cannot_publish_stale_markers(self):
        detail = self.media()
        playlist = self.playlist(detail)
        entry = playlist["tracks"][0]
        calculate = api._points
        def changed_during_measurement(*args):
            points = calculate(*args)
            desktop_playlist.remove_entries(self.store, self.principal, self.sid, playlist["_id"],
                [{"entry_id": entry["_id"], "expected_revision": entry["_rev"]}],
                expected_revision=playlist["_rev"], idempotency_key=str(uuid4()))
            return points
        with patch.object(api, "_points", side_effect=changed_during_measurement):
            with self.assertRaises(Conflict): self.assign(detail, playlist_id=playlist["_id"])
        self.assertFalse(self.store.scan("desktop_media_markers"))
        self.assertEqual(desktop_media.get_media(self.store, self.principal, self.sid,
            detail["media"]["_id"])["media"]["_rev"], detail["media"]["_rev"])
        with self.assertRaises(Conflict): self.assign(detail, playlist_id=playlist["_id"])
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_http_validation_auth_replay_and_no_partial_mutation(self):
        detail = self.media()
        base = make_handler(self.store)
        class Handler(base):
            def log_message(self, *args): pass
        server = BoundedHTTPServer(("127.0.0.1", 0), Handler, request_timeout=3)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        self.addCleanup(worker.join, 4); self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        def request(body, auth=True):
            conn = HTTPConnection("127.0.0.1", server.server_port, timeout=3)
            headers = {"Content-Type": "application/json", "Connection": "close"}
            if auth: headers["Authorization"] = "Bearer " + self.fixture.identity["api_token"]
            conn.request("POST", "/v1/workflows/desktop-autodj/markers-assign", json.dumps(body), headers)
            response = conn.getresponse(); result = response.status, json.loads(response.read()); conn.close()
            return result
        args = self.args(detail)
        self.assertEqual(request(args, False)[0], 403)
        sequence = self.store.sequence
        for changes in ({"threshold_db": float("nan")}, {"threshold_db": -61}, {"out_lead_ms": 30001},
                        {"overwrite": 1}, {"expected_marker_revision": 0}, {"media_sha256": "invalid"}):
            self.assertEqual(request({**args, **changes})[0], 422)
            self.assertEqual(self.store.sequence, sequence)
        status, result = request(args)
        self.assertEqual(status, 200); self.assertEqual(result["status"], "updated")
        sequence = self.store.sequence
        self.assertEqual(request(args), (200, result)); self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(request({**args, "idempotency_key": str(uuid4())})[0], 409)


class AutoDJBeatPointFallback(unittest.TestCase):
    def waveform(self, *, pulse=True):
        # Low-level tonal audio at the end must not be treated as a beats-only
        # section merely because its waveform floor is quieter than the body.
        waveform = []
        for start in range(0, 70000, 50):
            level = .2 if 600 <= start < 45000 else .04 if 45000 <= start < 69000 else 0
            if pulse and 1000 <= start < 69000 and (start - 1000) % 500 < 100:
                level = .6
            waveform.extend((-level, level))
        return {"sample_rate": 8000, "sample_frames": 560000,
                "waveform_frames_per_bin": 400, "waveform": waveform}

    def test_quiet_tail_is_the_exit_with_or_without_pulses(self):
        grid = {"bpm": 120, "confidence": 1, "first_beat_ms": 1000}
        self.assertEqual(api._points(self.waveform(pulse=False), 0, 70000, -45, 1000, grid),
                         ((1000, 45000), None))
        self.assertEqual(api._points(self.waveform(), 0, 70000, -45, 1000, grid),
                         ((1000, 45000), None))

    def test_optional_downbeat_presence_preserves_zero_and_fallback_phase(self):
        # The backend wire schema uses member presence, not the C struct's
        # has_downbeat flag. With the same beat grid, bars may start two
        # beats earlier; a genuine zero downbeat must survive that distinction.
        base = {"schema": 1, "algorithm": "radios-manual-grid-v1", "bpm": 120,
                "first_beat_ms": 1000, "confidence": 1, "analyzed_ms": 70000}
        cases = (({}, (1000, 45000)), ({"downbeat_ms": 0}, (2000, 44000)),
                 ({"downbeat_ms": 1000}, (1000, 45000)))
        for extra, expected in cases:
            validated = desktop_media._rhythm({**base, **extra}, 70000)
            self.assertEqual(api._points(self.waveform(), 0, 70000, -45, 1000, validated),
                             (expected, None))
            self.assertEqual("downbeat_ms" in validated, "downbeat_ms" in extra)
        with self.assertRaises(StoreError):
            desktop_media._rhythm({**base, "downbeat_ms": 0, "has_downbeat": False}, 70000)

    def test_unreliable_grid_uses_measured_windows_and_short_spans_the_sound(self):
        analysis = self.waveform()
        for grid in (None, {"bpm": 120, "confidence": .44, "first_beat_ms": 1000},
                     {"bpm": 0, "confidence": 1, "first_beat_ms": 1000},
                     {"bpm": 120, "confidence": 1}):
            self.assertEqual(api._points(analysis, 0, 70000, -45, 1000, grid),
                             ((600, 44000), None))
        # Cropping keeps the quiet tail long enough (14 s) to remain the exit.
        self.assertEqual(api._points(analysis, 0, 60000, -45, 1000,
            {"bpm": 120, "confidence": 1, "first_beat_ms": 1000}), ((1000, 45000), None))
        # Too short to judge sections: the measured sound minus the lead.
        self.assertEqual(api._points(analysis, 0, 12000, -45, 1000, None), ((600, 11000), None))

if __name__ == "__main__":
    unittest.main(verbosity=2)
