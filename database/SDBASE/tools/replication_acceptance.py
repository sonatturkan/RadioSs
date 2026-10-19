#!/usr/bin/env python3
"""Isolated real-process standby, media, retention and handover acceptance.

The public runner accepts only an optional report path. All databases, bundles,
credentials, servers and crash artifacts belong to its TemporaryDirectory.
It does not accept or alter an existing production database.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import json
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from uuid import uuid4


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))


def worker(kind: str, config_path: Path) -> None:
    from sdbase.schema import open_radio_store
    from sdbase.storage import _atomic_json
    import sdbase.replication as replication
    config = json.loads(config_path.read_bytes())
    if kind == "writer":
        store = open_radio_store(config["db"])
        key = str(uuid4())
        store.put("items", key, {"kind": "music", "title": "separate process", "lifecycle": "draft"})
        print(json.dumps({"item_id": key}))
        return
    if kind == "reader":
        store = open_radio_store(config["db"], read_only=True)
        print(json.dumps(store.get("items", config["item_id"])))
        return
    if kind == "http":
        from sdbase.api_server import BoundedHTTPServer
        from sdbase.http_server import make_handler
        handler = make_handler(open_radio_store(config["db"]))
        class Quiet(handler):
            def log_message(self, *_args):
                pass
        server = BoundedHTTPServer(("127.0.0.1", 0), Quiet, max_connections=4, request_timeout=2)
        _atomic_json(Path(config["ready"]), {"url": f"http://127.0.0.1:{server.server_port}"})
        server.serve_forever()
        return
    standby = open_radio_store(config["standby"])
    saved = replication._atomic_json
    if kind == "media-copy-crash":
        original_copy = replication._copy_file
        def fail_media_copy(source, target, **options):
            if target.name.endswith(".replica-partial"):
                target.parent.mkdir(parents=True, exist_ok=True)
                with target.open("xb") as stream:
                    stream.write(Path(source).read_bytes()[:87])
                    stream.flush()
                    os.fsync(stream.fileno())
                os._exit(73)
            return original_copy(source, target, **options)
        replication._copy_file = fail_media_copy
        replication.apply_shipment(standby, config["bundle"])
    elif kind == "replay-crash":
        def fail_progress(path, value):
            progress = value.get("replication", {})
            if path == standby.meta_file and progress.get("last_replayed_sequence") == config["target_sequence"] and \
                    not progress.get("pending"):
                os._exit(71)  # Journal fsync completed; progress publication did not.
            return saved(path, value)
        replication._atomic_json = fail_progress
        replication.apply_shipment(standby, config["bundle"])
    elif kind == "promotion-crash":
        def fail_primary(path, value):
            if path == standby.meta_file and value.get("node_role") == "primary":
                os._exit(72)  # Ownership checkpoint durable; new primary is unpublished.
            return saved(path, value)
        replication._atomic_json = fail_primary
        replication.promote_standby(open_radio_store(config["source"]), standby,
            expected_source_sequence=config["sequence"], expected_source_timeline_id=config["timeline"])
    raise AssertionError("crash point was not reached")


class Run:
    def __init__(self, root: Path):
        self.root = root
        self.checks: list[str] = []
        self.processes: list[subprocess.Popen] = []
        self.worker_count = 0

    def check(self, name, result=True):
        if not result:
            raise AssertionError(name)
        self.checks.append(name)

    def denied(self, name, call):
        from sdbase.storage import StoreError
        try:
            call()
        except StoreError:
            self.check(name)
        else:
            raise AssertionError(name)

    def config(self, kind, values):
        from sdbase.storage import _atomic_json
        self.worker_count += 1
        path = self.root / f"{self.worker_count}-{kind}.json"
        _atomic_json(path, values)
        os.chmod(path, 0o600)
        return path

    def child(self, kind, values, expected_exit=0):
        path = self.config(kind, values)
        result = subprocess.run([sys.executable, str(Path(__file__).resolve()),
            "--worker", kind, "--config", str(path)], capture_output=True, text=True,
            timeout=30, env=os.environ | {"PYTHONDONTWRITEBYTECODE": "1"})
        if result.returncode != expected_exit:
            raise AssertionError(f"{kind} exited {result.returncode}: {result.stderr}")
        return json.loads(result.stdout) if result.stdout else None

    def server(self, database):
        ready = self.root / f"http-{self.worker_count}.ready.json"
        path = self.config("http", {"db": str(database), "ready": str(ready)})
        process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
            "--worker", "http", "--config", str(path)], stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, env=os.environ | {"PYTHONDONTWRITEBYTECODE": "1"})
        self.processes.append(process)
        deadline = time.monotonic() + 10
        while not ready.exists():
            if process.poll() is not None:
                raise AssertionError("standby HTTP worker failed: " + process.stderr.read().decode())
            if time.monotonic() > deadline:
                raise AssertionError("standby HTTP worker readiness timed out")
            time.sleep(0.02)
        return json.loads(ready.read_bytes())["url"]

    @staticmethod
    def request(url, token, path, *, body=None):
        request = Request(url + path,
            data=None if body is None else json.dumps(body).encode(),
            headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        try:
            with urlopen(request, timeout=5) as response:
                return response.status, json.load(response)
        except HTTPError as exc:
            return exc.code, json.load(exc)

    def close(self):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            if process.stderr:
                process.stderr.close()

    def execute(self):
        from sdbase.schema import open_radio_store
        from sdbase.identity import bootstrap, authenticate, issue_endpoint_token
        from sdbase.catalog import create_item, create_version
        from sdbase.media import stage_media, object_path
        from sdbase.retention import preview_retention, apply_retention, verify_cold_history
        from sdbase.ops_archive import create_archive, verify_archive
        from sdbase.storage import _atomic_json, _frame
        import sdbase.replication as repl

        primary = open_radio_store(self.root / "primary" / "RadioS.sdb")
        identity = bootstrap(primary, "acceptance", "RP", "Replication acceptance")
        principal = authenticate(primary, identity["api_token"])
        station = identity["station_id"]
        issue_endpoint_token(primary, principal, station, identity["endpoint_id"])
        repl.create_standby(primary, self.root / "standby")
        replica = open_radio_store(self.root / "standby" / "RadioS.sdb")
        base = repl.replication_status(replica)
        self.check("physical identity preserved; node identity distinct",
            primary._meta()["store_id"] == replica._meta()["store_id"] and
            primary._meta()["node_id"] != replica._meta()["node_id"])
        self.denied("standby transactions rejected", lambda: replica.put("items", str(uuid4()),
            {"kind": "music", "title": "blocked", "lifecycle": "draft"}))
        self.denied("standby checkpoint rejected", replica.checkpoint)
        created = self.child("writer", {"db": str(primary.journal)})
        item_id = created["item_id"]
        self.check("separate writer process committed", primary.get("items", item_id)["title"] == "separate process")
        item = create_item(primary, principal, station, kind="music", title="media delta")
        version = create_version(primary, principal, station, item["_id"], label="one")
        media_source = self.root / "media.bin"
        media_source.write_bytes(b"verified-media-delta" * 200)
        media = stage_media(primary, principal, station, version["_id"], media_source, [self.root])
        primary.checkpoint()
        shipment = self.root / "first-bundle"
        repl.create_shipment(primary, shipment, after_sequence=base["sequence"],
            expected_previous_sha256=base["replication"]["last_record_sha256"])
        self.child("media-copy-crash", {"standby": str(replica.journal), "bundle": str(shipment)}, expected_exit=73)
        self.check("killed media copy publishes no incomplete object", not object_path(replica, media["sha256"]).exists())
        self.check("killed asset copy leaves replay position unchanged", replica.sequence == base["sequence"])
        result = repl.apply_shipment(replica, shipment)
        self.check("incremental replay across primary checkpoint", result["sequence"] == primary.sequence)
        self.check("managed media bytes verified on replica", object_path(replica, media["sha256"]).read_bytes() == media_source.read_bytes())
        blocked_media = self.root / "blocked-media.bin"
        blocked_media.write_bytes(b"must-not-publish-on-standby")
        blocked_digest = hashlib.sha256(blocked_media.read_bytes()).hexdigest()
        self.denied("direct library media mutation rejected on standby", lambda: stage_media(replica,
            authenticate(replica, identity["api_token"]), station, version["_id"], blocked_media, [self.root]))
        self.check("rejected standby media creates no managed bytes", not object_path(replica, blocked_digest).exists())
        self.check("separate read-only reader process sees delta",
            self.child("reader", {"db": str(replica.journal), "item_id": item_id})["title"] == "separate process")
        self.check("duplicate bundle idempotent", repl.apply_shipment(replica, shipment)["duplicate"])
        self.denied("explicit read-only maintenance handle rejected",
            lambda: repl.apply_shipment(open_radio_store(replica.journal, read_only=True), shipment))
        url = self.server(replica.journal)
        status, read = self.request(url, identity["api_token"], f"/v1/items/{item['_id']}?station_id={station}")
        self.check("standby authenticated HTTP detail and revision", status == 200 and read["item"]["_id"] == item["_id"] and read["item"]["_rev"] == item["_rev"])
        status, _body = self.request(url, identity["api_token"], "/v1/items",
            body={"station_id": station, "kind": "music", "title": "must not write"})
        self.check("standby HTTP mutation returns retryable 503", status == 503)
        preview = preview_retention(primary, principal, keep_days={"_audit": 1},
            as_of=datetime.now(timezone.utc) + timedelta(days=3))
        maintenance = apply_retention(primary, principal, preview)
        self.check("retention created cold history", maintenance["purged_records"] > 0)
        result = repl.catch_up(primary, replica)
        self.check("retention checkpoint replayed", result["lag_transactions"] == 0 and bool(replica.scan("retention_segments")))
        self.check("cold files byte verified", verify_cold_history(replica)["segments"] == 1)
        primary.put("items", str(uuid4()), {"kind": "music", "title": "after retention", "lifecycle": "draft"})
        progress = repl.replication_status(replica)["replication"]
        second = self.root / "second-bundle"
        repl.create_shipment(primary, second, after_sequence=progress["last_replayed_sequence"],
            expected_previous_sha256=progress["last_record_sha256"])
        before = replica.sequence
        original = (second / "records.json").read_bytes()
        (second / "records.json").write_bytes(original + b" ")
        self.denied("tampered component rejected", lambda: repl.apply_shipment(replica, second))
        self.check("tamper rejection leaves replay position unchanged", replica.sequence == before)
        (second / "records.json").write_bytes(original)
        self.child("replay-crash", {"standby": str(replica.journal), "bundle": str(second),
            "target_sequence": before + 1}, expected_exit=71)
        replica = open_radio_store(replica.journal)
        self.check("real process crash leaves durable pending frame", repl.replication_status(replica)["pending_replay"])
        self.check("same bundle recovers post-fsync crash", repl.apply_shipment(replica, second)["sequence"] == primary.sequence)
        primary.put("items", str(uuid4()), {"kind": "music", "title": "torn tail", "lifecycle": "draft"})
        progress = repl.replication_status(replica)["replication"]
        third = self.root / "third-bundle"
        repl.create_shipment(primary, third, after_sequence=progress["last_replayed_sequence"],
            expected_previous_sha256=progress["last_record_sha256"])
        record = json.loads((third / "records.json").read_bytes())[0]
        metadata = replica._meta()
        metadata["replication"]["pending"] = {"sequence": record["payload"]["sequence"],
            "sha256": record["sha256"], "bundle_id": json.loads((third / "manifest.json").read_bytes())["bundle_id"]}
        _atomic_json(replica.meta_file, metadata)
        with replica.journal.open("ab") as stream:
            stream.write(_frame(record["payload"]["transaction"])[:87])
            stream.flush()
            os.fsync(stream.fileno())
        self.denied("ordinary standby startup refuses torn tail", lambda: open_radio_store(replica.journal))
        repaired = repl.recover_standby_replay(replica.journal, third)
        self.check("explicit verified prefix repair and replay", repaired["repaired_tail_bytes"] == 87 and repaired["sequence"] == primary.sequence)
        replica = open_radio_store(replica.journal)
        primary.put("items", str(uuid4()), {"kind": "music", "title": "gap one", "lifecycle": "draft"})
        gap_start = primary.sequence
        primary.put("items", str(uuid4()), {"kind": "music", "title": "gap two", "lifecycle": "draft"})
        gap_bundle = self.root / "gap-bundle"
        repl.create_shipment(primary, gap_bundle, after_sequence=gap_start,
            expected_previous_sha256=repl._anchor(primary, gap_start))
        self.denied("out-of-order bundle rejected", lambda: repl.apply_shipment(replica, gap_bundle))
        repl.catch_up(primary, replica)
        sequence, timeline = primary.sequence, primary._meta()["timeline_id"]
        self.denied("stale promotion position rejected", lambda: repl.promote_standby(primary, replica,
            expected_source_sequence=sequence - 1, expected_source_timeline_id=timeline))
        self.child("promotion-crash", {"source": str(primary.journal), "standby": str(replica.journal),
            "sequence": sequence, "timeline": timeline}, expected_exit=72)
        self.check("crashed handover fences source before target publication",
            primary._meta()["node_role"] == "fenced" and replica._meta()["node_role"] == "standby")
        self.denied("fenced source refuses freshly opened writer", lambda: open_radio_store(primary.journal).put("items", str(uuid4()),
            {"kind": "music", "title": "old", "lifecycle": "draft"}))
        promoted = repl.promote_standby(primary, open_radio_store(replica.journal),
            expected_source_sequence=sequence, expected_source_timeline_id=timeline)
        self.check("interrupted promotion resumes exact intent", promoted["target_role"] == "primary" and promoted["epoch"] == 2)
        self.denied("old destination writer fenced by epoch", lambda: replica.put("items", str(uuid4()),
            {"kind": "music", "title": "old handle", "lifecycle": "draft"}))
        fresh = open_radio_store(replica.journal)
        self.check("endpoint credential revoked at promotion",
            all(row.get("revoked_at") for row in fresh.scan("tokens") if row["scope"] == "endpoint"))
        fresh.put("items", str(uuid4()), {"kind": "music", "title": "new timeline", "lifecycle": "draft"})
        self.check("fresh promoted primary accepts writes", fresh.sequence == sequence + 2)
        self.denied("old timeline shipment rejected after promotion", lambda: repl.apply_shipment(fresh, third))
        self.check("completed promotion command idempotent", repl.promote_standby(primary, fresh,
            expected_source_sequence=sequence, expected_source_timeline_id=timeline)["already_promoted"])
        archive = self.root / "audit"
        create_archive(fresh, archive)
        verified = verify_archive(archive)
        self.check("promotion before-after audit remains exportable", verified["last_sequence"] == fresh.sequence)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--worker", choices=["writer", "reader", "http", "media-copy-crash", "replay-crash", "promotion-crash"], help=argparse.SUPPRESS)
    parser.add_argument("--config", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        worker(args.worker, args.config)
        return
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="sdbase-replication-acceptance-") as temporary:
        run = Run(Path(temporary))
        try:
            run.execute()
            result = {"ok": True, "passed": len(run.checks), "checks": run.checks,
                      "real_process_workers": run.worker_count,
                      "seconds": round(time.monotonic() - started, 3),
                      "scope": "same_host_manual_asynchronous_standby_and_fenced_handover",
                      "excluded": ["network_quorum", "automatic_failover", "power_loss_hardware", "second_physical_host"]}
        finally:
            run.close()
    encoded = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")


if __name__ == "__main__":
    main()
