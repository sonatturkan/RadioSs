"""Real private-store administration, tenancy, CAS and recovery acceptance."""
import copy
from datetime import datetime, timedelta, timezone
from http.client import HTTPConnection
import json
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4

import test_desktop_api as fixtures
import test_desktop_advert as advert_fixtures
from sdbase import desktop_api as api, desktop_admin as admin, desktop_broadcast as broadcast
from sdbase.access_admin import create_station_user, revoke_token
from sdbase.api_server import BoundedHTTPServer
from sdbase.catalog import NotFound
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate, issue_endpoint_token, require
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError


class DesktopAdmin(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAPI("runTest")
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal

    def user(self, name="operator", permissions=None):
        return create_station_user(self.store, self.principal, self.sid, name, name,
            permissions or ["view", "operate"])

    def save(self, user, **changes):
        row = self.store.get("users", user["user_id"])
        args = dict(station_id=self.sid, user_id=row["_id"], expected_revision=row["_rev"],
            display_name=row["display_name"], status=row["status"], permissions=["view", "operate"],
            idempotency_key=str(uuid4()))
        args.update(changes)
        return admin.user_save(self.store, self.principal, **args)

    def start(self):
        profile = broadcast.profile_save(self.store, self.principal, self.sid, "record", "MAIN",
            {"filename": "real-main.wav"}, str(uuid4()))
        return broadcast.run_start(self.store, self.principal, self.sid, "record", profile["_id"], profile["_rev"], str(uuid4()))

    def close(self, run, **changes):
        args = dict(station_id=self.sid, run_id=run["run_id"], expected_revision=run["revision"],
            idempotency_key=str(uuid4()), reason="Operator verified the old process is stopped")
        args.update(changes)
        return admin.recovery_close(self.store, self.principal, **args)

    def other_station(self):
        sid = str(uuid4())
        self.store.put("stations", sid, {"code": "OTHER", "name": "Foreign station", "timezone": "UTC", "status": "active"})
        return sid

    def test_overview_readonly_real_integrity_and_no_credentials(self):
        user = self.user()
        sequence = self.store.sequence
        result = admin.overview(self.store, self.principal, self.sid)
        self.assertEqual(result["sequence"], sequence)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(result["health"]["integrity"]["status"], "ok")
        self.assertTrue(result["health"]["audit_sequence_valid"])
        self.assertGreater(result["health"]["filesystem_free_bytes"], 0)
        self.assertEqual(result["recoverable_runs"], [])
        self.assertEqual(result["current_user"]["user_id"], self.principal.user_id)
        encoded = json.dumps(result)
        for secret in (self.fixture.identity["api_token"], user["token"], "token_hash", "before_revision"):
            self.assertNotIn(secret, encoded)
        self.assertTrue(next(row for row in result["users"] if row["user_id"] == user["user_id"])["editable"])
        self.assertFalse(result["current_user"]["editable"])

    def test_station_isolation_and_permission_scope(self):
        other = self.other_station()
        local = self.user("localmanager", ["view", "manage"])
        actor = authenticate(self.store, local["token"])
        foreign = create_station_user(self.store, self.principal, other, "foreignuser", "Secret foreign name", ["view"])
        sequence = self.store.sequence
        result = admin.overview(self.store, actor, self.sid)
        self.assertNotIn("Secret foreign name", json.dumps(result))
        with self.assertRaises(Forbidden):
            admin.overview(self.store, actor, other)
        with self.assertRaises(NotFound):
            admin.user_save(self.store, actor, self.sid, foreign["user_id"], 1, "No", "disabled", ["view"], str(uuid4()))
        with self.assertRaises(Forbidden):
            admin.overview(self.store, authenticate(self.store, self.user()["token"]), self.sid)
        endpoint = issue_endpoint_token(self.store, self.principal, self.sid, self.fixture.identity["endpoint_id"])
        with self.assertRaises(Forbidden):
            admin.overview(self.store, authenticate(self.store, endpoint), self.sid)
        self.assertGreaterEqual(self.store.sequence, sequence)

    def test_user_save_reopen_cas_replay_and_auth_disable(self):
        user = self.user()
        key = str(uuid4())
        result = self.save(user, display_name="Yeni ad", permissions=["view", "edit"], idempotency_key=key)
        self.assertEqual(result["revision"], 2)
        sequence = self.store.sequence
        replay = self.save(user, expected_revision=1, display_name="Yeni ad", permissions=["view", "edit"], idempotency_key=key)
        self.assertEqual(result, replay)
        self.assertEqual(self.store.sequence, sequence)
        with self.assertRaises(Conflict):
            self.save(user, expected_revision=1)
        with self.assertRaises(Conflict):
            self.save(user, idempotency_key=key)
        self.save(user, status="disabled", permissions=["view", "edit"])
        with self.assertRaises(Forbidden):
            authenticate(self.store, user["token"])
        reopened = open_radio_store(self.fixture.db, read_only=True)
        row = reopened.get("users", user["user_id"])
        self.assertEqual((row["display_name"], row["status"], row["_rev"]), ("Yeni ad", "disabled", 3))
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_global_and_multi_station_users_cannot_be_changed_locally(self):
        local = self.user("localmanager", ["view", "manage"])
        actor = authenticate(self.store, local["token"])
        with self.assertRaises(Forbidden):
            admin.user_save(self.store, actor, self.sid, self.principal.user_id, 1, "No", "active", ["view", "manage"], str(uuid4()))
        multi = self.user("multi")
        other = self.other_station()
        self.store.put("permissions", str(uuid4()), {"user_id": multi["user_id"], "permission": "view", "station_id": other, "station_scope": other})
        with self.assertRaises(Forbidden):
            self.save(multi)
        self.assertFalse(next(row for row in admin.overview(self.store, actor, self.sid)["users"] if row["user_id"] == multi["user_id"])["editable"])

    def test_grant_revocation_denies_old_principal_and_regrant_reuses_identity(self):
        user = self.user("editor", ["view", "edit", "manage"])
        actor = authenticate(self.store, user["token"])
        edit = next(row for row in self.store.lookup("permissions", "user", user["user_id"]) if row["permission"] == "edit")
        require(self.store, actor, "edit", self.sid)
        self.save(user, permissions=["view"])
        revoked = self.store.get("permissions", edit["_id"])
        self.assertTrue(revoked["revoked_at"])
        self.assertEqual(revoked["revoked_by"], self.principal.user_id)
        with self.assertRaises(Forbidden):
            require(self.store, actor, "edit", self.sid)
        with self.assertRaises(Forbidden):
            admin.overview(self.store, actor, self.sid)
        context = api.desktop_context(self.store, actor, self.sid)
        self.assertEqual([grant["permission"] for grant in context["permissions"]], ["view"])
        self.save(user, permissions=["view", "edit"])
        restored = self.store.get("permissions", edit["_id"])
        self.assertIsNone(restored["revoked_at"])
        self.assertEqual(restored["_rev"], 3)
        require(self.store, actor, "edit", self.sid)
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_revoked_credential_rejects_authenticated_principal_and_committed_replay(self):
        run = self.start()
        key = str(uuid4())
        accepted = broadcast.run_event(self.store, self.principal, self.sid, run["run_id"], run["revision"],
            "active", 100, 200, "Actual partial source", key)
        token = self.store.get("tokens", self.principal.credential_id)
        self.store.put("tokens", token["_id"], admin._body(token) | {"revoked_at": admin._now()}, expected_revision=token["_rev"])
        with self.assertRaises(Forbidden):
            require(self.store, self.principal, "operate", self.sid)
        with self.assertRaises(Forbidden):
            broadcast.run_event(self.store, self.principal, self.sid, run["run_id"], run["revision"],
                "active", 100, 200, "Actual partial source", key)
        self.assertEqual(self.store.get("desktop_configurations", accepted["run_id"])["config"]["state"], "active")

    def test_current_manager_cannot_disable_demote_or_revoke_self(self):
        user = self.user("localmanager", ["view", "manage"])
        actor = authenticate(self.store, user["token"])
        sequence = self.store.sequence
        for status, permissions in (("disabled", ["view", "manage"]), ("active", ["view"])):
            with self.assertRaises(Conflict):
                admin.user_save(self.store, actor, self.sid, user["user_id"], 1, "Self", status, permissions, str(uuid4()))
        with self.assertRaises(Conflict):
            admin.token_revoke(self.store, actor, self.sid, user["token_id"], 1, str(uuid4()))
        self.assertEqual(self.store.sequence, sequence)

    def test_legacy_token_revoke_also_rejects_current_user_lockout(self):
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            revoke_token(self.store, self.principal, self.principal.credential_id, expected_revision=1)
        self.assertEqual(self.store.sequence, sequence)
        require(self.store, self.principal, "manage", self.sid)

    def test_legacy_token_revoke_preserves_last_usable_manager(self):
        target = self.user("onlyusable", ["view", "manage"])
        view = next(row for row in self.store.lookup("permissions", "user", self.principal.user_id) if row["permission"] == "view")
        self.store.put("permissions", view["_id"], admin._body(view) | {"revoked_at": admin._now(),
            "revoked_by": self.principal.user_id}, expected_revision=view["_rev"])
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            revoke_token(self.store, self.principal, target["token_id"], expected_revision=1)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(authenticate(self.store, target["token"]).user_id, target["user_id"])

    def test_client_named_creation_cas_replay_and_queued_revision_one_tail(self):
        identifier, key = str(uuid4()), str(uuid4())
        body = dict(kind="channel", name="Client named", config={"deck_count": 2},
            idempotency_key=key, document_id=identifier, expected_revision=0)
        created = api.put_document(self.store, self.principal, self.sid, **body)
        self.assertEqual((created["_id"], created["_rev"]), (identifier, 1))
        sequence = self.store.sequence
        self.assertEqual(api.put_document(self.store, self.principal, self.sid, **body), created)
        self.assertEqual(self.store.sequence, sequence)
        updated = api.put_document(self.store, self.principal, self.sid, "channel", "Client named",
            {"deck_count": 3}, str(uuid4()), document_id=identifier, expected_revision=1)
        self.assertEqual(updated["_rev"], 2)
        self.assertEqual(updated["config"], {"deck_count": 3})
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(reopened.get("desktop_configurations", identifier), updated)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")

    def test_client_named_creation_never_overwrites_existing_or_foreign_identity(self):
        existing = self.fixture.put()
        other = self.other_station()
        sequence = self.store.sequence
        for sid in (self.sid, other):
            with self.assertRaises(Conflict):
                api.put_document(self.store, self.principal, sid, "channel", "Must not replace",
                    {"deck_count": 9}, str(uuid4()), document_id=existing["_id"], expected_revision=0)
        self.assertEqual(self.store.get("desktop_configurations", existing["_id"]), existing)
        self.assertEqual(self.store.sequence, sequence)

    def test_client_named_creation_concurrent_identity_claim_is_fenced(self):
        identifier = str(uuid4())
        original = self.store.get
        claimed = False
        def claim_after_read(collection, key):
            nonlocal claimed
            result = original(collection, key)
            if collection == "desktop_configurations" and key == identifier and result is None and not claimed:
                claimed = True
                now = admin._now()
                self.store.put("desktop_configurations", identifier, {"station_id": self.sid, "kind": "channel",
                    "name": "Concurrent claimant", "config": {"deck_count": 7}, "status": "active",
                    "created_by": self.principal.user_id, "created_at": now, "updated_by": self.principal.user_id, "updated_at": now})
            return result
        with patch.object(self.store, "get", claim_after_read), self.assertRaises(Conflict):
            api.put_document(self.store, self.principal, self.sid, "channel", "Stale creator",
                {"deck_count": 2}, str(uuid4()), document_id=identifier, expected_revision=0)
        self.assertEqual(self.store.get("desktop_configurations", identifier)["name"], "Concurrent claimant")

    def test_last_usable_manager_survives_disabled_and_expired_alternatives(self):
        target = self.user("onlyusable", ["view", "manage"])
        actor_row = self.user("maintenance", ["manage"])
        actor = authenticate(self.store, actor_row["token"])
        expired = self.user("expired", ["view", "manage"])
        original = self.store.get("users", self.principal.user_id)
        self.store.put("users", original["_id"], admin._body(original) | {"status": "disabled"}, expected_revision=original["_rev"])
        token = self.store.get("tokens", expired["token_id"])
        self.store.put("tokens", token["_id"], admin._body(token) | {"expires_at": (datetime.now(timezone.utc) - timedelta(days=1)).isoformat()}, expected_revision=token["_rev"])
        sequence = self.store.sequence
        with self.assertRaises(Conflict):
            admin.user_save(self.store, actor, self.sid, target["user_id"], 1, "Target", "disabled", ["view", "manage"], str(uuid4()))
        with self.assertRaises(Conflict):
            admin.token_revoke(self.store, actor, self.sid, target["token_id"], 1, str(uuid4()))
        self.assertEqual(self.store.sequence, sequence)

    def test_permission_change_between_authorization_and_commit_is_fenced(self):
        manager = self.user("localmanager", ["view", "manage"])
        actor = authenticate(self.store, manager["token"])
        target = self.user()
        grant = next(row for row in self.store.lookup("permissions", "user", actor.user_id) if row["permission"] == "manage")
        original = admin._station
        def revoke_after_authorization(store, principal, sid):
            result = original(store, principal, sid)
            store.put("permissions", grant["_id"], admin._body(grant) | {"revoked_at": admin._now(),
                "revoked_by": self.principal.user_id}, expected_revision=grant["_rev"])
            return result
        with patch.object(admin, "_station", revoke_after_authorization), self.assertRaises(Conflict):
            admin.user_save(self.store, actor, self.sid, target["user_id"], 1, "Must not commit", "active", ["view"], str(uuid4()))
        self.assertEqual(self.store.get("users", target["user_id"])["_rev"], 1)

    def test_token_revoke_is_durable_scoped_cas_and_idempotent(self):
        user = self.user()
        key = str(uuid4())
        result = admin.token_revoke(self.store, self.principal, self.sid, user["token_id"], 1, key)
        sequence = self.store.sequence
        self.assertEqual(admin.token_revoke(self.store, self.principal, self.sid, user["token_id"], 1, key), result)
        self.assertEqual(self.store.sequence, sequence)
        self.assertEqual(result["revision"], 2)
        with self.assertRaises(Forbidden):
            authenticate(self.store, user["token"])
        with self.assertRaises(Conflict):
            admin.token_revoke(self.store, self.principal, self.sid, user["token_id"], 1, str(uuid4()))
        other = self.other_station()
        foreign = create_station_user(self.store, self.principal, other, "foreign", "Foreign", ["view"])
        with self.assertRaises(NotFound):
            admin.token_revoke(self.store, self.principal, self.sid, foreign["token_id"], 1, str(uuid4()))
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_user_create_private_key_identity_no_plaintext_response_or_replay(self):
        key = str(uuid4())
        result = admin.user_create(self.store, self.principal, self.sid, "nativeadmin", "Native", "operator", [], key)
        self.assertNotIn("token", result)
        credential_file = fixtures.Path(result["credential_file"])
        self.assertEqual(credential_file.stat().st_mode & 0o777, 0o600)
        self.assertEqual(credential_file.parent.stat().st_mode & 0o777, 0o700)
        secret = json.loads(credential_file.read_text())
        self.assertEqual(authenticate(self.store, secret["token"]).user_id, result["user_id"])
        sequence = self.store.sequence
        self.assertEqual(admin.user_create(self.store, self.principal, self.sid, "nativeadmin", "Native", "operator", [], key), result)
        self.assertEqual(self.store.sequence, sequence)
        self.assertNotIn(secret["token"], json.dumps(admin.overview(self.store, self.principal, self.sid)))

    def test_recovery_preserves_partial_counters_and_unknown_lost_tail(self):
        run = self.start()
        run = broadcast.run_event(self.store, self.principal, self.sid, run["run_id"], run["revision"],
            "active", 12345, 7777, "Real local counters before process loss", str(uuid4()))
        original = self.store.get("desktop_configurations", run["run_id"])
        key = str(uuid4())
        result = self.close(run, idempotency_key=key)
        self.assertEqual(self.store.get("desktop_configurations", run["run_id"]), original)
        recovery = self.store.get("desktop_configurations", result["recovery_id"])
        self.assertEqual(recovery["config"]["last_observations"][-1]["frames"], 12345)
        self.assertEqual(recovery["config"]["last_observations"][-1]["bytes"], 7777)
        self.assertTrue(result["unknown_tail"])
        self.assertFalse(result["delivery_proven"])
        sequence = self.store.sequence
        self.assertEqual(self.close(run, idempotency_key=key), result)
        self.assertEqual(self.store.sequence, sequence)
        snapshot = admin.overview(self.store, self.principal, self.sid)
        self.assertEqual(snapshot["recoverable_runs"], [])
        self.assertEqual(snapshot["recovered_runs"][0]["recovery"]["original_state"], "active")
        reopened = open_radio_store(self.fixture.db, read_only=True)
        self.assertEqual(inspect_integrity(reopened)["status"], "ok")
        self.assertEqual(reopened.get("desktop_configurations", run["run_id"]), original)

    def test_recovery_cas_terminals_new_writes_and_committed_replay(self):
        run = self.start()
        key = str(uuid4())
        observed = broadcast.run_event(self.store, self.principal, self.sid, run["run_id"], run["revision"],
            "active", 48000, 192000, "Observed", key)
        with self.assertRaises(Conflict):
            self.close(run)
        self.close(observed)
        self.assertEqual(broadcast.run_event(self.store, self.principal, self.sid, run["run_id"], run["revision"],
            "active", 48000, 192000, "Observed", key), observed)
        with self.assertRaises(Conflict):
            broadcast.run_event(self.store, self.principal, self.sid, run["run_id"], observed["revision"],
                "completed", 96000, 384000, "Impossible late completion", str(uuid4()))
        with self.assertRaises(Conflict):
            self.close(observed)
        terminal = self.start()
        terminal = broadcast.run_event(self.store, self.principal, self.sid, terminal["run_id"], terminal["revision"],
            "interrupted", 0, 0, "Stopped before audio", str(uuid4()))
        with self.assertRaises(Conflict):
            self.close(terminal)

    def test_recovery_is_immutable_and_raw_configuration_crud_denied(self):
        run = self.start()
        result = self.close(run)
        row = self.store.get("desktop_configurations", result["recovery_id"])
        sequence = self.store.sequence
        changed = admin._body(row)
        changed["config"]["delivery_proven"] = True
        with self.assertRaises(StoreError):
            self.store.put("desktop_configurations", row["_id"], changed, expected_revision=row["_rev"])
        with self.assertRaises(StoreError):
            self.store.delete("desktop_configurations", row["_id"], expected_revision=row["_rev"])
        with self.assertRaises(StoreError):
            api.put_document(self.store, self.principal, self.sid, "admin_recovery", "No", row["config"], str(uuid4()))
        with self.assertRaises(StoreError):
            api.delete_document(self.store, self.principal, self.sid, "admin_recovery", row["_id"], 1, str(uuid4()))
        self.assertEqual(self.store.sequence, sequence)

    def test_recovery_station_permission_and_missing_run(self):
        run = self.start()
        other = self.other_station()
        with self.assertRaises(NotFound):
            self.close(run, station_id=other)
        with self.assertRaises(NotFound):
            self.close(run, run_id=str(uuid4()))
        actor = authenticate(self.store, self.user()["token"])
        with self.assertRaises(Forbidden):
            admin.recovery_close(self.store, actor, self.sid, run["run_id"], run["revision"], str(uuid4()), "No")

    def test_offline_restore_fences_old_start_replay_without_inventing_audio(self):
        profile = broadcast.profile_save(self.store, self.principal, self.sid, "record", "Restored MAIN",
            {"filename": "old-session.wav"}, str(uuid4()))
        key = str(uuid4())
        run = broadcast.run_start(self.store, self.principal, self.sid, "record", profile["_id"], profile["_rev"], key)
        original = self.store.get("desktop_configurations", run["run_id"])
        fenced = admin.fence_restored_runtime(self.store, self.principal.user_id)
        self.assertEqual(fenced["closed_runs"], 1)
        self.assertEqual(self.store.get("desktop_configurations", run["run_id"]), original)
        with self.assertRaises(Conflict):
            broadcast.run_start(self.store, self.principal, self.sid, "record", profile["_id"], profile["_rev"], key)
        with self.assertRaises(Conflict):
            broadcast.run_event(self.store, self.principal, self.sid, run["run_id"], 1, "active", 1, 1, "No late execution", str(uuid4()))
        sequence = self.store.sequence
        self.assertEqual(admin.fence_restored_runtime(self.store)["closed_runs"], 0)
        self.assertEqual(self.store.sequence, sequence)
        self.assertFalse(self.store.scan("playout_events"))
        self.assertEqual(inspect_integrity(self.store)["status"], "ok")

    def test_already_observed_start_replay_never_reexecutes_terminal_output(self):
        profile = broadcast.profile_save(self.store, self.principal, self.sid, "record", "MAIN",
            {"filename": "no-second-start.wav"}, str(uuid4()))
        key = str(uuid4())
        run = broadcast.run_start(self.store, self.principal, self.sid, "record", profile["_id"], profile["_rev"], key)
        broadcast.run_event(self.store, self.principal, self.sid, run["run_id"], 1, "completed", 48000, 192000, "Measured output", str(uuid4()))
        with self.assertRaises(Conflict):
            broadcast.run_start(self.store, self.principal, self.sid, "record", profile["_id"], profile["_rev"], key)

    def test_health_probe_failure_stays_unknown(self):
        with patch.object(admin, "inspect_integrity", side_effect=OSError("do not leak a filesystem path")):
            snapshot = admin.overview(self.store, self.principal, self.sid)
        self.assertEqual(snapshot["health"]["status"], "unavailable")
        self.assertEqual(snapshot["health"]["integrity"]["status"], "unavailable")
        self.assertNotIn("do not leak", json.dumps(snapshot))

    def test_http_exact_contract_unknown_fields_and_auth(self):
        server = BoundedHTTPServer(("127.0.0.1", 0), make_handler(self.store))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        def request(path, data, token=None):
            connection = HTTPConnection(*server.server_address, timeout=5)
            try:
                headers = {"Content-Type": "application/json"}
                if token:
                    headers["Authorization"] = "Bearer " + token
                connection.request("POST", "/v1/workflows/desktop-admin/" + path, json.dumps(data), headers)
                response = connection.getresponse()
                return response.status, json.loads(response.read())
            finally:
                connection.close()
        status, result = request("overview", {"station_id": self.sid}, self.fixture.identity["api_token"])
        self.assertEqual(status, 200)
        self.assertEqual(result["current_user"]["user_id"], self.principal.user_id)
        self.assertEqual(request("overview", {"station_id": self.sid})[0], 403)
        self.assertEqual(request("overview", {"station_id": self.sid, "password": "invalid"}, self.fixture.identity["api_token"])[0], 422)
        target = self.user()
        body = {"station_id": self.sid, "user_id": target["user_id"], "expected_revision": 1,
            "display_name": "HTTP", "status": "active", "permissions": ["view", "edit"], "idempotency_key": str(uuid4())}
        status, result = request("user-save", body, self.fixture.identity["api_token"])
        self.assertEqual(status, 200)
        self.assertEqual(result["revision"], 2)
        body["idempotency_key"] = str(uuid4())
        self.assertEqual(request("user-save", body, self.fixture.identity["api_token"])[0], 409)


class AdvertRecovery(unittest.TestCase):
    def test_offline_restore_fences_terminal_advert_without_downgrading_completion(self):
        fixture = advert_fixtures.DesktopAdvert("runTest")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        document, _, _, _ = fixture.reservation()
        run = fixture.start(document)
        span = run["manifest"]["entries"][0]
        run = fixture.append(run, [fixture.event(span, frames=1),
            fixture.event(span, state="local_complete", frames=span["expected_frames"])])
        original = fixture.store.get("desktop_configurations", run["run_id"])
        self.assertEqual(original["config"]["state"], "local_complete")
        result = admin.fence_restored_runtime(fixture.store, fixture.principal.user_id)
        self.assertEqual((result["closed_runs"], result["fenced_runs"]), (0, 1))
        fence = fixture.store.get("desktop_configurations", result["recovery_ids"][0])
        self.assertEqual(fence["kind"], "admin_restore_fence")
        self.assertFalse(fence["config"]["unknown_tail"])
        self.assertEqual(fence["config"]["original_state"], "local_complete")
        self.assertEqual(fixture.store.get("desktop_configurations", run["run_id"]), original)
        with self.assertRaises(Conflict):
            fixture.append(run, [fixture.event(span, frames=1, occurrence=2)])
        self.assertEqual(inspect_integrity(fixture.store)["status"], "ok")

    def test_advert_partial_receipt_preserved_no_formal_delivery_on_closure(self):
        fixture = advert_fixtures.DesktopAdvert("runTest")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        document, campaign, binding, reservation = fixture.reservation()
        run = fixture.start(document)
        span = run["manifest"]["entries"][0]
        run = fixture.append(run, [fixture.event(span, frames=77)])
        original = fixture.store.get("desktop_configurations", run["run_id"])
        result = admin.recovery_close(fixture.store, fixture.principal, fixture.sid, run["run_id"],
            run["revision"], str(uuid4()), "Verified process loss; tail cannot be measured")
        self.assertFalse(result["delivery_proven"])
        self.assertEqual(fixture.store.get("desktop_configurations", run["run_id"]), original)
        with self.assertRaises(Conflict):
            fixture.append(run, [fixture.event(span, state="local_complete", frames=span["expected_frames"])])
        report = fixture.report()["config"]["snapshot"]
        rows = [row for row in report["rows"] if row["record_class"] == "local"]
        self.assertEqual(rows[0]["state"], "unknown")
        self.assertEqual(rows[0]["consumed_frames"], 77)
        self.assertFalse(rows[0]["delivery_proven"])
        self.assertFalse(fixture.store.scan("playout_events"))
        self.assertEqual(inspect_integrity(fixture.store)["status"], "ok")


if __name__ == "__main__":
    unittest.main()
