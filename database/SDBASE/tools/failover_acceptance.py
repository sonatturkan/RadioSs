#!/usr/bin/env python3
"""Disposable local failover proof: real API process kill and durable fencing."""
from pathlib import Path
import argparse
from datetime import datetime, timezone
from http.client import HTTPConnection
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from unittest.mock import patch
from uuid import uuid4

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))


def run():
    from sdbase import failover, replication
    from sdbase.catalog import create_item
    from sdbase.failover import FailoverSupervisor, FailoverPolicy, FailoverError
    from sdbase.identity import bootstrap, authenticate, issue_endpoint_token
    from sdbase.schema import open_radio_store
    from sdbase.storage import WriteRejected
    checks = []
    def check(name, value=True):
        if not value:
            raise AssertionError(name)
        checks.append({"name": name, "passed": True})
    def reject(name, action):
        try:
            action()
        except (FailoverError, WriteRejected):
            check(name)
        else:
            raise AssertionError(name)
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="sdbase-failover-") as temporary:
        root = Path(temporary)
        source = open_radio_store(root / "primary" / "RadioS.sdb")
        credential = bootstrap(source, "supervisor", "R", "Failover proof")
        principal = authenticate(source, credential["api_token"])
        endpoint_token = issue_endpoint_token(source, principal, credential["station_id"], credential["endpoint_id"])
        from sdbase.station_outputs import create_station_output
        from sdbase import plans, playout
        sid, eid = credential["station_id"], credential["endpoint_id"]
        create_station_output(source, principal, sid, eid, "main", geo_codes=["TR"])
        instant = datetime.now(timezone.utc)
        plan = plans.create_plan(source, principal, sid, instant.date().isoformat())
        plans.put_entry(source, principal, sid, plan["plan_id"], plan["draft_revision_id"], {
            "ordinal": 0, "event_kind": "macro", "scheduled_at": instant.isoformat(),
            "payload": {"command": "acceptance.set_flag", "args": {}}}, expected_plan_revision=1)
        plans.approve_plan(source, principal, sid, plan["plan_id"], plan["draft_revision_id"], expected_plan_revision=2)
        snapshot = plans.build_snapshot(source, principal, sid, eid, plan["draft_revision_id"])
        playout.prepare_distribution(source, principal, sid, eid, plan["draft_revision_id"], snapshot)
        ep = authenticate(source, endpoint_token)
        playout.acknowledge_delivery(source, ep, sid, eid, plan["draft_revision_id"], playout._sha(snapshot))
        playout.accept_plan(source, ep, sid, eid, plan["draft_revision_id"], "acceptance-engine", 1)
        playout.queue_command(source, principal, sid, eid, "stop", {}, "pending-before-failover")
        replication.create_standby(source, root / "standby")
        standby = open_radio_store(root / "standby" / "RadioS.sdb")
        create_item(source, principal, credential["station_id"], kind="music", title="Not yet replicated")
        program = ("import os,json;from pathlib import Path;from sdbase.schema import open_radio_store;"
            "from sdbase.http_server import make_handler;from sdbase.api_server import BoundedHTTPServer;"
            "s=open_radio_store(os.environ['SDB_ACCEPTANCE_DATABASE']);"
            "server=BoundedHTTPServer(('127.0.0.1',0),make_handler(s));"
            "Path(os.environ['SDB_ACCEPTANCE_READY']).write_text(json.dumps({'port':server.server_port}));"
            "server.serve_forever()")
        env = os.environ | {"SDB_ACCEPTANCE_DATABASE": str(source.journal),
            "SDB_ACCEPTANCE_READY": str(root / "ready.json"), "PYTHONPATH": str(ROOT / "src"),
            "PYTHONDONTWRITEBYTECODE": "1"}
        process = subprocess.Popen([sys.executable, "-c", program], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        servers, clients = [], []
        from http.server import BaseHTTPRequestHandler
        quiet_logs = patch.object(BaseHTTPRequestHandler, "log_message", lambda *args: None)
        quiet_logs.start()
        def listener(handler):
            from sdbase.api_server import BoundedHTTPServer
            server = BoundedHTTPServer(("127.0.0.1", 0), handler)
            worker = threading.Thread(target=server.serve_forever, daemon=True);worker.start()
            servers.append((server, worker))
            client = HTTPConnection("127.0.0.1", server.server_port, timeout=3)
            clients.append(client)
            return client
        def api(client, method, path, *, body=None, token=None):
            encoded = json.dumps(body).encode() if body is not None else None
            client.request(method, path, body=encoded, headers={"Authorization": "Bearer " +
                           (token or credential["api_token"]), "Content-Type": "application/json"})
            response = client.getresponse();result = json.loads(response.read())
            return response.status, result
        try:
            deadline = time.monotonic() + 10
            while not (root / "ready.json").exists():
                if time.monotonic() >= deadline or process.poll() is not None:
                    raise AssertionError("primary HTTP process did not start")
                time.sleep(.02)
            port = json.loads((root / "ready.json").read_text())["port"]
            url = f"http://127.0.0.1:{port}"
            policy = FailoverPolicy(probe_interval_seconds=1, probe_timeout_seconds=.2,
                                     cooldown_seconds=30, catchup_max_records=1)
            supervisor = FailoverSupervisor(source.journal, standby.journal, url, root / "job", policy=policy)
            from sdbase.failover_server import make_failover_handler
            from sdbase.http_server import make_handler
            routed = listener(make_failover_handler(supervisor))
            original_listener = listener(make_handler(source))
            code, health = api(routed, "GET", "/health")
            socket_before = routed.sock
            check("stable router initially selects the source primary", code == 200 and health["node_id"] == source._meta()["node_id"])
            code, active = api(original_listener, "GET", f"/v1/endpoints/{eid}/state", token=endpoint_token)
            check("original listener has genuine active acceptance before handover", code == 200 and
                  source.get("active_acceptances", eid)["state"] == "active")
            clock = [time.time()]
            with patch("sdbase.failover.time.time", side_effect=lambda: clock[0]):
                check("matching healthy service arms automatic failover", supervisor.tick()["health_confirmed"])
                # An unavailable health endpoint with no verified baseline must
                # never trigger takeover of a healthy but misconfigured pair.
                unarmed = FailoverSupervisor(source.journal, standby.journal, "http://127.0.0.1:1",
                                             root / "unarmed", policy=policy)
                check("unverified service URL cannot arm promotion", not unarmed.tick()["health_confirmed"] and
                      unarmed.status()["state"] == "blocked")
                clock[0] += 2
                with patch.object(supervisor, "_probe", return_value={"result": "missing", "detail": "transient"}):
                    check("one transient health failure is retained without promotion", supervisor.tick()["failures"] == 1)
                clock[0] += 2
                check("recovered healthy service resets consecutive failures", supervisor.tick()["failures"] == 0)
                foreign = FailoverSupervisor(source.journal, standby.journal, url, root / "foreign", policy=policy)
                foreign.tick();clock[0] += 2
                with patch.object(foreign, "_probe", return_value={"result": "foreign", "detail": "wrong node"}):
                    check("foreign service identity latches an operator intervention", foreign.tick()["operator_required"])
                clock[0] += 10
                with patch.object(foreign, "_probe", return_value={"result": "missing", "detail": "missing"}):
                    check("foreign identity cannot later become automatic outage evidence", foreign.tick()["state"] == "blocked")
                withheld = source.journal.with_name("withheld.sdb")
                source.journal.rename(withheld)
                check("unreachable original database refuses automatic takeover", supervisor.tick()["state"] == "blocked")
                check("supervisor never recreates a missing source journal", not source.journal.exists())
                withheld.rename(source.journal)
                clock[0] += 2;supervisor.tick()
                with supervisor._job():
                    reject("another supervisor cannot acquire the job lock", lambda: supervisor.tick())
                monitor_stop = threading.Event()
                decisions = supervisor.watch(monitor_stop)
                try:
                    next(decisions)
                    other_decisions = supervisor.watch(threading.Event())
                    reject("second monitor cannot own the job between probe ticks", lambda: next(other_decisions))
                    monitor_stop.set()
                    try:
                        next(decisions)
                    except StopIteration:
                        check("monitor shutdown releases continuous ownership")
                    else:
                        raise AssertionError("stopped monitor continued")
                    replacement = supervisor.watch(threading.Event())
                    try:
                        check("replacement monitor acquires released job", next(replacement)["state"] == "watching")
                    finally:
                        replacement.close()
                finally:
                    decisions.close()
                process.kill();process.wait(timeout=5)
                check("real primary HTTP child process was killed", process.returncode < 0)
                for count in (1, 2):
                    clock[0] += 2
                    state = supervisor.tick()
                    check(f"{count} health misses do not prematurely promote", state["state"] == "service_failed" and
                          source._meta()["node_role"] == "primary")
                # Persisted hysteresis survives a supervisor process/restart.
                supervisor = FailoverSupervisor(source.journal, standby.journal, url, root / "job", policy=policy)
                clock[0] += 2
                original = replication._atomic_json
                def crash_after_fencing(path, value):
                    original(path, value)
                    if path == source.meta_file and value.get("node_role") == "fenced":
                        raise OSError("acceptance fault after primary fencing")
                with patch("sdbase.replication._atomic_json", side_effect=crash_after_fencing):
                    state = supervisor.tick()
                check("failure after fencing preserves durable promotion intent", state["state"] == "promotion_pending" and
                      source._meta()["node_role"] == "fenced")
                reject("old cached primary writer is fenced", lambda: create_item(source, principal,
                       credential["station_id"], kind="music", title="Old primary must reject"))
                supervisor = FailoverSupervisor(source.journal, standby.journal, url, root / "job", policy=policy)
                state = supervisor.tick()
                check("restart resumes and finishes the recorded handover", state["state"] == "promoted")
                check("active database selection changes only after promotion", state["active_database"] == str(standby.journal))
                code, routed_health = api(routed, "GET", "/health")
                check("same keep-alive socket selects the promoted node and epoch", code == 200 and routed.sock is socket_before and
                      routed_health["node_id"] == standby._meta()["node_id"] and routed_health["epoch"] == standby._meta()["epoch"])
                check("old source still retains evidence that must not authorize execution", source.get("active_acceptances", eid)["state"] == "active" and
                      bool(source.scan("engine_commands")))
                for suffix in ("state", f"snapshots/{plan['draft_revision_id']}?station_id={sid}",
                               f"commands?station_id={sid}&engine_instance=acceptance-engine&engine_epoch=1"):
                    code, result = api(original_listener, "GET", f"/v1/endpoints/{eid}/" + suffix, token=endpoint_token)
                    check("fenced original listener rejects endpoint GET " + suffix.split("?")[0], code == 503)
                code, created = api(routed, "POST", "/v1/items", body={"station_id": sid, "kind": "music", "title": "Routed new primary"})
                check("admin create through stable router succeeds after failover", code == 200)
                code, detail = api(routed, "GET", f"/v1/items/{created['_id']}?station_id={sid}")
                check("admin detail read through stable router returns the new revision", code == 200 and
                      detail["item"]["_rev"] == created["_rev"])
                promoted = open_radio_store(standby.journal)
                check("lagging standby is caught up before promotion", any(row["title"] == "Not yet replicated"
                      for row in promoted.scan("items")))
                check("promotion creates a new timeline and higher epoch", promoted._meta()["timeline_id"] !=
                      source._meta()["timeline_id"] and promoted._meta()["epoch"] == source._meta()["epoch"] + 1)
                reject("cached standby handle cannot write after its epoch changes", lambda: create_item(standby, principal,
                       credential["station_id"], kind="music", title="Reopen required"))
                create_item(promoted, authenticate(promoted, credential["api_token"]), credential["station_id"],
                            kind="music", title="New primary accepts")
                check("reopened promoted primary accepts normal authorized writes", len(promoted.scan("items")) == 3)
                check("completed promotion is idempotent", supervisor.tick()["state"] == "promoted")
                reject("operator reset cannot reenable an old fenced primary", supervisor.reset)
                # Target replacement must not keep receiving requests solely
                # because an old supervisor state selected its filename.
                from sdbase.storage import _atomic_json
                metadata = promoted._meta();metadata["timeline_id"] = str(uuid4())
                _atomic_json(promoted.meta_file, metadata)
                code, result = api(routed, "GET", "/health")
                check("stable router rejects a foreign promoted target timeline", code == 503)
                reject("recorded routing target losing ownership is refused", supervisor.tick)
        finally:
            for client in clients:
                client.close()
            for server, worker in servers:
                server.shutdown();server.server_close();worker.join(timeout=3)
            quiet_logs.stop()
            if process.poll() is None:
                process.kill();process.wait(timeout=5)
    return {"format": "sdbase-failover-acceptance-v1", "passed": len(checks), "failed": 0,
            "elapsed_seconds": round(time.monotonic() - started, 3), "checks": checks,
            "scope": "real local HTTP process kill; deterministic probe interval clock; reachable-file fencing; no network quorum"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = run()
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
