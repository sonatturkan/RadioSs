#!/usr/bin/env python3
"""Real isolated HTTP/DB acceptance for saved native audio routing choices."""

import copy
from http.client import HTTPConnection
import json
import threading
import unittest
from uuid import uuid4

import test_desktop_api as fixtures
from sdbase import desktop_api as api
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.http_server import make_handler
from sdbase.identity import issue_endpoint_token
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict


def configuration(**changes):
    return {"schema_version": 1, "application_name": "", "application_binary": "",
            "output_sink": "", "microphone_source": "", "application_enabled": True,
            "microphone_enabled": False, "application_gain_db": 0,
            "microphone_gain_db": 0, "compressor": "voice", **changes}


class DesktopRouting(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal
        base = make_handler(self.store)

        class Handler(base):
            def log_message(self, *args):
                pass

        self.server = BoundedHTTPServer(("127.0.0.1", 0), Handler, request_timeout=2)
        self.worker = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.worker.start()
        self.addCleanup(self.close_server)

    def close_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.worker.join(timeout=3)
        self.assertFalse(self.worker.is_alive())

    def request(self, action, body, *, token=None, auth=True, method="POST"):
        connection = HTTPConnection("127.0.0.1", self.server.server_port, timeout=2)
        headers = {"Content-Type": "application/json", "Connection": "close"}
        if auth:
            headers["Authorization"] = "Bearer " + (token or self.fixture.identity["api_token"])
        try:
            connection.request(method, "/v1/workflows/desktop/" + action, json.dumps(body), headers)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def put_body(self, config=None, **extra):
        return {"station_id": self.sid, "kind": "audio_routing", "name": "Ses yönlendirme",
                "config": configuration() if config is None else config,
                "idempotency_key": str(uuid4()), **extra}

    def put(self, config=None, **extra):
        return api.put_document(self.store, self.principal, self.sid, "audio_routing", "Ses yönlendirme",
                                configuration() if config is None else config, str(uuid4()), **extra)

    def test_empty_selection_readonly_list_and_persistent_preference(self):
        body = {"station_id": self.sid, "kind": "audio_routing"}
        sequence = self.store.sequence
        status, empty = self.request("list", body)
        self.assertEqual(status, 200)
        self.assertEqual(empty["records"], [])
        self.assertEqual(self.store.sequence, sequence)
        status, saved = self.request("put", self.put_body())
        self.assertEqual(status, 200)
        self.assertEqual(saved["config"], configuration())
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, "audio_routing", saved["_id"]), saved)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        for collection in ("playout_events", "output_observations", "recording_attempts"):
            self.assertFalse(self.store.scan(collection))
        self.assertEqual({row["kind"] for row in self.store.scan("desktop_configurations")}, {"audio_routing"})

    def test_configured_devices_utf8_gains_compressor_and_no_runtime_claim(self):
        config = configuration(application_name="Müzik / tarayıcı", application_binary="firefox",
            output_sink="alsa_output.usb-Kulaklık.stereo", microphone_source="alsa_input.usb-Mikrofon.mono",
            application_gain_db=-3.25, microphone_gain_db=4.5, microphone_enabled=True, compressor="music")
        status, saved = self.request("put", self.put_body(config))
        self.assertEqual(status, 200)
        self.assertEqual(saved["config"], config)
        self.assertEqual(set(saved["config"]), set(configuration()))
        self.assertNotIn(self.fixture.identity["api_token"], json.dumps(saved))
        # A saved enabled choice cannot carry a running gate or a Pulse ID.
        self.assertNotIn("running", saved["config"])
        self.assertNotIn("application_index", saved["config"])
        for compressor in ("off", "voice", "music"):
            self.assertEqual(self.put(configuration(compressor=compressor))["config"]["compressor"], compressor)

    def test_http_cas_exact_replay_and_conflicting_key_survive_reopen(self):
        create = self.put_body(configuration(application_name="Browser", application_binary="firefox"))
        status, first = self.request("put", create)
        self.assertEqual(status, 200)
        sequence = self.store.sequence
        self.assertEqual(self.request("put", create), (200, first))
        self.assertEqual(self.store.sequence, sequence)
        changed = copy.deepcopy(create)
        changed["config"]["compressor"] = "off"
        self.assertEqual(self.request("put", changed)[0], 409)
        update = self.put_body(configuration(application_name="Browser", application_binary="firefox", compressor="music"),
            document_id=first["_id"], expected_revision=first["_rev"])
        status, current = self.request("put", update)
        self.assertEqual(status, 200)
        self.assertEqual(current["_rev"], first["_rev"] + 1)
        sequence = self.store.sequence
        self.assertEqual(self.request("put", update), (200, current))
        self.assertEqual(self.store.sequence, sequence)
        stale = update | {"idempotency_key": str(uuid4())}
        self.assertEqual(self.request("put", stale)[0], 409)
        get = {"station_id": self.sid, "kind": "audio_routing", "document_id": current["_id"]}
        self.assertEqual(self.request("get", get), (200, current))
        reopened = open_radio_store(self.fixture.db)
        self.assertEqual(api.put_document(reopened, self.principal, self.sid, "audio_routing", update["name"],
            update["config"], update["idempotency_key"], update["document_id"], update["expected_revision"]), current)
        self.assertEqual(reopened.sequence, sequence)

    def test_rejects_typed_invalid_fields_without_db_mutation(self):
        variants = []
        for key in ("application_name", "application_binary", "output_sink", "microphone_source"):
            limit = 256 if key == "application_binary" else 512
            variants.extend(configuration(**{key: value}) for value in (
                True, None, [], "a" * (limit + 1), "é" * (limit // 2 + 1), "bad\nname", "bad\x7fname", "\ud800"))
        for key in ("application_enabled", "microphone_enabled"):
            variants.extend(configuration(**{key: value}) for value in (0, 1, "true", None, []))
        for key in ("application_gain_db", "microphone_gain_db"):
            variants.extend(configuration(**{key: value}) for value in (True, -60.1, 12.1, "0", None, float("inf"), float("nan")))
        variants.extend(configuration(schema_version=value) for value in (True, 0, 2, "1", 1.0))
        variants.extend(configuration(compressor=value) for value in (True, 0, None, [], {}, "Voice", "limiter"))
        missing = configuration()
        del missing["microphone_enabled"]
        variants.extend((missing, [], {}, "not a configuration"))
        sequence = self.store.sequence
        for config in variants:
            with self.subTest(config=config):
                status, _ = self.request("put", self.put_body(config))
                self.assertEqual(status, 422)
                self.assertEqual(self.store.sequence, sequence)
        self.assertFalse(self.store.scan("desktop_configurations"))
        self.assertFalse(self.store.scan("desktop_mutations"))

    def test_rejects_ephemeral_paths_secrets_gates_and_forged_actor(self):
        sequence = self.store.sequence
        for key, value in (("application_index", 14), ("sink_input_index", 14), ("running", True),
                ("recording", True), ("paused", False), ("autostart", True), ("filename", "capture.wav"),
                ("recording_path", "/tmp/capture.wav"), ("password", "do not persist"), ("api_token", "do not persist")):
            with self.subTest(field=key):
                self.assertEqual(self.request("put", self.put_body(configuration(**{key: value})))[0], 422)
                self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.request("put", self.put_body() | {"principal": "forged"})[0], 422)
        self.assertEqual(self.store.sequence, sequence)

    def test_manage_save_delete_view_read_and_station_isolation(self):
        saved = self.put()
        editor = create_station_user(self.store, self.principal, self.sid, "routing-editor", "Editor", ["view", "edit", "operate"])
        token = editor["token"]
        listing = {"station_id": self.sid, "kind": "audio_routing"}
        status, result = self.request("list", listing, token=token)
        self.assertEqual(status, 200)
        self.assertEqual(result["records"], [saved])
        sequence = self.store.sequence
        self.assertEqual(self.request("put", self.put_body(), token=token)[0], 403)
        delete = {"station_id": self.sid, "kind": "audio_routing", "document_id": saved["_id"],
                  "expected_revision": saved["_rev"], "idempotency_key": str(uuid4())}
        self.assertEqual(self.request("delete", delete, token=token)[0], 403)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.request("list", listing, auth=False)[0], 403)
        endpoint_token = issue_endpoint_token(self.store, self.principal, self.sid, self.fixture.identity["endpoint_id"])
        self.assertEqual(self.request("list", listing, token=endpoint_token)[0], 403)
        other = str(uuid4())
        self.store.put("stations", other, {"code": "ROUTING-OTHER", "name": "Other", "timezone": "UTC", "status": "active"})
        self.assertEqual(self.request("list", listing | {"station_id": other}, token=token)[0], 403)
        self.assertEqual(self.request("get", listing | {"station_id": other, "document_id": saved["_id"]})[0], 404)
        self.assertEqual(self.request("list", listing | {"station_id": other})[1]["records"], [])
        status, disabled = self.request("delete", delete)
        self.assertEqual(status, 200)
        self.assertEqual(disabled["status"], "disabled")
        sequence = self.store.sequence
        self.assertEqual(self.request("delete", delete), (200, disabled))
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(self.request("list", listing)[1]["records"], [])

    def test_native_named_identity_revision_zero_and_boundaries(self):
        identifier = str(uuid4())
        config = configuration(application_name="é" * 256, application_binary="x" * 256,
            output_sink="x" * 512, microphone_source="x" * 512,
            application_gain_db=-60, microphone_gain_db=12, compressor="off")
        saved = self.put(config, document_id=identifier, expected_revision=0)
        self.assertEqual(saved["_id"], identifier)
        self.assertEqual(saved["_rev"], 1)
        self.assertEqual(saved["config"], config)
        with self.assertRaises(Conflict):
            self.put(config, document_id=identifier, expected_revision=0)
        self.assertEqual(inspect_integrity(open_radio_store(self.fixture.db, read_only=True))["status"], "ok")

    def speech_put_body(self, config, **extra):
        return {"station_id": self.sid, "kind": "dj_mixer", "name": "DJ mixer",
                "config": config, "idempotency_key": str(uuid4()), **extra}

    def test_speech_effector_legacy_preference_remains_compatible(self):
        config = fixtures.dj_configuration()
        original = copy.deepcopy(config)
        self.assertNotIn("fx_slot", config["speech"])
        status, saved = self.request("put", self.speech_put_body(config))
        self.assertEqual(status, 200)
        self.assertEqual(config, original)
        self.assertEqual(saved["config"], original)
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid,
                                         "dj_mixer", saved["_id"]), saved)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_speech_effector_four_choices_and_off_roundtrip_replay_cas_reopen(self):
        original = fixtures.dj_configuration()
        previous = None
        for selected in (-1, 0, 1, 2, 3):
            with self.subTest(fx_slot=selected):
                config = copy.deepcopy(original)
                config["speech"]["fx_slot"] = selected
                extra = {} if previous is None else {
                    "document_id": previous["_id"], "expected_revision": previous["_rev"]}
                body = self.speech_put_body(config, **extra)
                status, saved = self.request("put", body)
                self.assertEqual(status, 200)
                self.assertEqual(saved["config"], config)
                sequence = self.store.sequence
                self.assertEqual(self.request("put", body), (200, saved))
                self.assertEqual(self.store.sequence, sequence)
                get = {"station_id": self.sid, "kind": "dj_mixer", "document_id": saved["_id"]}
                self.assertEqual(self.request("get", get), (200, saved))
                reopened = open_radio_store(self.fixture.db, read_only=True)
                self.assertEqual(api.get_document(reopened, self.principal, self.sid,
                                                 "dj_mixer", saved["_id"]), saved)
                self.assertEqual(inspect_integrity(reopened)["status"], "ok")
                if previous is not None:
                    stale = self.speech_put_body(config, document_id=previous["_id"],
                                                 expected_revision=previous["_rev"])
                    self.assertEqual(self.request("put", stale)[0], 409)
                    self.assertEqual(self.store.sequence, sequence)
                previous = saved
        self.assertEqual(len(self.store.scan("desktop_configurations")), 1)
        for collection in ("playout_events", "output_observations", "recording_attempts"):
            self.assertFalse(self.store.scan(collection))

    def test_speech_effector_rejects_invalid_values_and_runtime_gates(self):
        sequence = self.store.sequence
        for value in (-2, 4, 5, True, False, 1.0, "1", None, [], {}, float("inf"), float("nan")):
            with self.subTest(fx_slot=value):
                config = fixtures.dj_configuration()
                config["speech"]["fx_slot"] = value
                self.assertEqual(self.request("put", self.speech_put_body(config))[0], 422)
                self.assertEqual(self.store.sequence, sequence)
        for key in ("microphone_gate", "push_to_talk", "on_air"):
            with self.subTest(runtime_field=key):
                config = fixtures.dj_configuration()
                config["speech"]["fx_slot"] = 0
                config["speech"][key] = True
                self.assertEqual(self.request("put", self.speech_put_body(config))[0], 422)
                self.assertEqual(self.store.sequence, sequence)
        self.assertFalse(self.store.scan("desktop_configurations"))
        self.assertFalse(self.store.scan("desktop_mutations"))


    def test_speech_master_channel_legacy_and_partial_preferences(self):
        previous = None
        for fields in ({}, {"to_master": False}, {"channel": 31},
                       {"to_master": True, "channel": 7}):
            with self.subTest(fields=fields):
                config = fixtures.dj_configuration()
                config["speech"].update(fields)
                original = copy.deepcopy(config)
                extra = {} if previous is None else {
                    "document_id": previous["_id"], "expected_revision": previous["_rev"]}
                status, saved = self.request("put", self.speech_put_body(config, **extra))
                self.assertEqual(status, 200)
                self.assertEqual(config, original)
                self.assertEqual(saved["config"], original)
                self.assertEqual(set(saved["config"]["speech"]), set(original["speech"]))
                reopened = open_radio_store(self.fixture.db, read_only=True)
                self.assertEqual(api.get_document(reopened, self.principal, self.sid,
                                                 "dj_mixer", saved["_id"]), saved)
                self.assertEqual(inspect_integrity(reopened)["status"], "ok")
                previous = saved
        self.assertEqual(len(self.store.scan("desktop_configurations")), 1)

    def test_speech_master_channel_all_identities_independent_fx_replay_cas(self):
        original = fixtures.dj_configuration()
        previous = None
        routes = [(False, channel, channel % 5 - 1) for channel in range(-1, 32)]
        routes += [(True, 7, slot) for slot in range(-1, 4)]
        routes += [(False, 7, slot) for slot in range(-1, 4)]
        for to_master, channel, fx_slot in routes:
            with self.subTest(to_master=to_master, channel=channel, fx_slot=fx_slot):
                config = copy.deepcopy(original)
                config["speech"].update(to_master=to_master, channel=channel, fx_slot=fx_slot)
                untouched = copy.deepcopy(config)
                extra = {} if previous is None else {
                    "document_id": previous["_id"], "expected_revision": previous["_rev"]}
                body = self.speech_put_body(config, **extra)
                status, saved = self.request("put", body)
                self.assertEqual(status, 200)
                self.assertEqual(config, untouched)
                self.assertEqual(saved["config"], untouched)
                sequence = self.store.sequence
                # Every identity crosses real HTTP and survives reopening. Test
                # replay/read/CAS on boundaries and both independent FX modes
                # without exceeding the fixture's unchanged request limiter.
                check_cas = to_master or channel in (-1, 0, 7, 31)
                if check_cas:
                    self.assertEqual(self.request("put", body), (200, saved))
                    self.assertEqual(self.store.sequence, sequence)
                    get = {"station_id": self.sid, "kind": "dj_mixer", "document_id": saved["_id"]}
                    self.assertEqual(self.request("get", get), (200, saved))
                reopened = open_radio_store(self.fixture.db, read_only=True)
                self.assertEqual(api.get_document(reopened, self.principal, self.sid,
                                                 "dj_mixer", saved["_id"]), saved)
                self.assertEqual(inspect_integrity(reopened)["status"], "ok")
                if previous is not None and check_cas:
                    stale = self.speech_put_body(config, document_id=previous["_id"],
                                                 expected_revision=previous["_rev"])
                    self.assertEqual(self.request("put", stale)[0], 409)
                    self.assertEqual(self.store.sequence, sequence)
                previous = saved
        self.assertEqual(len(self.store.scan("desktop_configurations")), 1)
        for collection in ("playout_events", "output_observations", "recording_attempts"):
            self.assertFalse(self.store.scan(collection))

    def test_speech_master_channel_rejects_invalid_types_and_ranges(self):
        sequence = self.store.sequence
        invalid = {
            "to_master": (-1, 0, 1, 0.0, 1.0, "true", "false", None, [], {}),
            "channel": (-2, 32, 999, True, False, 1.0, "1", None, [], {},
                        float("inf"), float("nan")),
        }
        for field, values in invalid.items():
            for value in values:
                with self.subTest(field=field, value=value):
                    config = fixtures.dj_configuration()
                    config["speech"].update(to_master=True, channel=7, fx_slot=3)
                    config["speech"][field] = value
                    self.assertEqual(self.request("put", self.speech_put_body(config))[0], 422)
                    self.assertEqual(self.store.sequence, sequence)
        self.assertFalse(self.store.scan("desktop_configurations"))
        self.assertFalse(self.store.scan("desktop_mutations"))



if __name__ == "__main__":
    unittest.main(verbosity=2)
