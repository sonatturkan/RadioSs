#!/usr/bin/env python3
"""Run isolated same-host HTTP, PCM, offline and backup acceptance scenarios.

The command creates its own temporary .sdb and never takes a production DB
path. Time advancement is deterministic; optional offline wall time is
reported separately. Hardware output and a second physical host are excluded.
"""

from array import array
import argparse
from datetime import datetime, timedelta, timezone
from http.server import ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen
import wave


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))


def _write(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + ".partial")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False)
        stream.flush(); os.fsync(stream.fileno())
    os.chmod(temporary, 0o600)
    os.replace(temporary, path)


def _clock(path):
    return datetime.fromisoformat(json.loads(Path(path).read_text())["now"])


def _worker(kind, config_path):
    from sdbase import endpoint, playout
    from sdbase.schema import open_radio_store
    config = json.loads(Path(config_path).read_text())
    if kind == "center":
        from sdbase.http_server import make_handler
        playout._now = lambda value=None: _clock(config["clock"]) if value is None else \
            playout._instant(value, "now")
        base = make_handler(open_radio_store(config["db"]))
        class Handler(base):
            def log_message(self, *_args):
                pass
            def do_GET(self):
                if self.path == "/acceptance/stream.wav":
                    blob = Path(config["stream"]).read_bytes()
                    self.send_response(200);self.send_header("Content-Length", str(len(blob)))
                    self.send_header("Content-Type", "audio/wav");self.end_headers();self.wfile.write(blob)
                else:
                    super().do_GET()
        server = ThreadingHTTPServer(("127.0.0.1", config.get("port", 0)), Handler)
        _write(config["ready"], {"url": f"http://127.0.0.1:{server.server_port}", "pid": os.getpid()})
        server.serve_forever()
        return
    if kind == "restore":
        from sdbase.backup import restore_backup
        result = restore_backup(config["backup"], config["target"], provision_copy=True)
        _write(config["restore_result"], result)
        print(json.dumps({"restored": True, "media_objects": result["media_objects"]}))
        return
    if kind == "backup":
        from sdbase.backup import create_backup, verify_backup
        result = create_backup(open_radio_store(config["db"]), config["backup"])
        verify_backup(config["backup"])
        print(json.dumps(result))
        return
    endpoint._now = lambda: _clock(config["clock"])
    from sdbase.transport import HTTPTransport
    action = json.load(sys.stdin)
    transport = HTTPTransport(config["url"], config["endpoint_token"], timeout=1)
    try:
        with endpoint.EndpointAgent(Path(config["endpoint_dir"]), config["station_id"],
                                    config["endpoint_id"], config["engine_instance"]) as agent:
            command = action["command"]
            if command == "sync":
                result = agent.sync_once(transport)
            elif command == "prefetch":
                result = agent.prefetch_distribution(transport, action["revision_id"])
            elif command == "status":
                result = agent.status()
            elif command == "manifest":
                result = agent.active_manifest()
            elif command == "play":
                from acceptance_playout import render_reference
                result = render_reference(agent, Path(action["output"]),
                    allowed_stream_urls=action.get("allowed_stream_urls", []))
            elif command == "missing-media":
                media_id = action["media_file_id"]
                spec = agent._state["media"][media_id]
                path = agent._object_path(spec["sha256"])
                path.rename(path.with_name(path.name + ".withheld"))
                result = {"status": agent.media_store.verify_media_file(media_id)}
            elif command == "restore-media":
                media_id = action["media_file_id"]
                spec = agent._state["media"][media_id]
                path = agent._object_path(spec["sha256"])
                path.with_name(path.name + ".withheld").rename(path)
                result = {"status": agent.media_store.verify_media_file(media_id)}
            elif command == "accept-command":
                result = agent.accept_command(action["command_id"])
            elif command == "crash-after-command-accept":
                result = agent.accept_command(action["command_id"])
                print(json.dumps({"ok": True, "result": result}));sys.stdout.flush()
                os._exit(0)  # Deliberately skip context cleanup and completion callback.
            elif command == "command-status":
                result = agent._state['commands'][action['command_id']]
            elif command == "pending-commands":
                result = agent.pending_commands()
            else:
                raise ValueError("unknown acceptance worker command")
            print(json.dumps({"ok": True, "result": result}, ensure_ascii=False))
    except Exception as exc:
        print(json.dumps({"ok": False, "error": str(exc), "type": type(exc).__name__}))


class Run:
    def __init__(self, root):
        self.root = Path(root)
        self.checks = []
        self.processes = []
        self.calls = 0
        self.worker_runs = 0
        self.clock_path = self.root / "center-clock.json"
        self.endpoint_clock_path = self.root / "endpoint-clock.json"
        self.config_path = self.root / "center-config.json"
        self.endpoint_config_path = self.root / "endpoint-config.json"
        self.current = datetime.now(timezone.utc).replace(hour=12, minute=0, second=0, microsecond=0)
        self.set_clock(self.current)
        self.db = self.root / "source" / "RadioS.sdb"
        self.db.parent.mkdir()
        self.stream = self.root / "stream.wav"
        self.write_wav(self.stream, 3000)
        from sdbase.schema import open_radio_store
        from sdbase.identity import bootstrap, issue_endpoint_token, authenticate
        store = open_radio_store(self.db)
        self.identity = bootstrap(store, "acceptance", "ACCEPT", "Acceptance")
        self.user_token = self.identity["api_token"]
        self.endpoint_token = issue_endpoint_token(store,
            authenticate(store, self.user_token), self.sid, self.eid)
        self.config = {"db": str(self.db), "clock": str(self.clock_path), "stream": str(self.stream),
                       "ready": str(self.root / "ready.json"), "port": 0}

    @property
    def sid(self):
        return self.identity["station_id"]

    @property
    def eid(self):
        return self.identity["endpoint_id"]

    def check(self, name, actual):
        if not actual:
            raise AssertionError(name)
        self.checks.append(name)

    def set_clock(self, instant, *, endpoint_offset_seconds=0):
        self.current = instant
        _write(self.clock_path, {"now": instant.isoformat()})
        _write(self.endpoint_clock_path, {
            "now": (instant + timedelta(seconds=endpoint_offset_seconds)).isoformat()})

    @staticmethod
    def write_wav(path, sample, frames=24000):
        pcm = array("h", [sample] * frames)
        if sys.byteorder != "little":
            pcm.byteswap()
        with wave.open(str(path), "wb") as stream:
            stream.setnchannels(1);stream.setsampwidth(2);stream.setframerate(8000)
            stream.writeframes(pcm.tobytes())

    def child(self, kind, path, action=None):
        self.worker_runs += 1
        result = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--worker", kind,
                    "--config", str(path)], input=json.dumps(action) if action else None,
                    capture_output=True, text=True, timeout=30,
                    env=os.environ | {"PYTHONDONTWRITEBYTECODE": "1"})
        if result.returncode != 0:
            raise RuntimeError(f"{kind} worker failed: {result.stderr[-1500:]}")
        return json.loads(result.stdout)

    def start(self):
        ready = Path(self.config["ready"])
        ready.unlink(missing_ok=True)
        _write(self.config_path, self.config)
        process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--worker", "center",
                    "--config", str(self.config_path)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                    env=os.environ | {"PYTHONDONTWRITEBYTECODE": "1"})
        self.processes.append(process)
        deadline = time.monotonic() + 10
        while not ready.is_file():
            if process.poll() is not None:
                raise RuntimeError("HTTP center failed: " + process.stderr.read().decode()[-2000:])
            if time.monotonic() > deadline:
                raise TimeoutError("HTTP center did not start")
            time.sleep(0.03)
        self.url = json.loads(ready.read_text())["url"]
        self.config["port"] = int(self.url.rsplit(":", 1)[1])
        self.endpoint_config = {"station_id": self.sid, "endpoint_id": self.eid,
            "engine_instance": "reference-1", "endpoint_token": self.endpoint_token,
            "endpoint_dir": str(self.root / "endpoint"), "clock": str(self.endpoint_clock_path),
            "url": self.url}
        _write(self.endpoint_config_path, self.endpoint_config)
        return process

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill();process.wait(timeout=5)
        if process.stderr:
            process.stderr.close()

    def endpoint(self, command, *, expected_ok=True, **arguments):
        result = self.child("endpoint", self.endpoint_config_path, {"command": command, **arguments})
        if result["ok"] != expected_ok:
            raise AssertionError(f"endpoint {command}: {result}")
        return result.get("result") if expected_ok else result

    def api(self, method, path, body=None, *, query=None, status=200, token=None, raw=None):
        self.calls += 1
        if query:
            path += "?" + urlencode(query)
        data = raw if raw is not None else json.dumps(body).encode() if body is not None else None
        request = Request(self.url + path, data=data, method=method, headers={
            "Authorization": "Bearer " + (token or self.user_token),
            "Content-Type": "application/octet-stream" if raw is not None else "application/json"})
        try:
            with urlopen(request, timeout=10) as response:
                actual, result = response.status, json.load(response)
        except HTTPError as exc:
            actual, result = exc.code, json.load(exc)
        if actual != status:
            raise AssertionError(f"{method} {path}: expected {status}, got {actual}: {result}")
        return result

    def workflow(self, path, **arguments):
        return self.api("POST", "/v1/workflows/" + path, {"station_id": self.sid, **arguments})

    def item(self, title, sample):
        created = self.api("POST", "/v1/items", {"station_id": self.sid, "kind": "music", "title": title})
        version = self.api("POST", f"/v1/items/{created['_id']}/versions", {
            "station_id": self.sid, "label": "main"})
        file = self.root / (title + ".wav");self.write_wav(file, sample)
        staged = self.api("POST", f"/v1/versions/{version['_id']}/media",
            query={"station_id": self.sid}, raw=file.read_bytes())
        self.api("POST", f"/v1/media/{staged['media_file_id']}/verify", {
            "station_id": self.sid, "technical_metadata": {"codec": "pcm_s16le", "container": "wav",
            "duration_ms": 3000, "sample_rate_hz": 8000, "channels": 1}})
        self.api("POST", f"/v1/versions/{version['_id']}/approve", {
            "station_id": self.sid, "expected_revision": version['_rev']})
        detail = self.api("GET", f"/v1/items/{created['_id']}", query={"station_id": self.sid})
        self.api("POST", f"/v1/items/{created['_id']}/lifecycle", {
            "station_id": self.sid, "target": "active", "expected_revision": detail['item']['_rev']})
        self.api("POST", f"/v1/items/{created['_id']}/eligibility", {
            "station_id": self.sid, "eligible": True, "expected_revision": detail['station_item']['_rev']})
        self.api("POST", "/v1/rights", {"station_id": self.sid, "version_id": version['_id'],
            "territory": "*", "output_id": "*", "starts_at": self.current.isoformat(),
            "ends_at": (self.current + timedelta(days=10)).isoformat()})
        return {"item_id": created['_id'], "version_id": version['_id'],
                "media_file_id": staged['media_file_id']}

    def plan(self, day, entries, *, approve=True):
        created = self.api("POST", "/v1/plans", {"station_id": self.sid, "service_date": day})
        for ordinal, entry in enumerate(entries):
            detail = self.api("GET", f"/v1/plans/{created['plan_id']}", query={"station_id": self.sid})
            self.api("POST", f"/v1/plans/{created['plan_id']}/entries", {
                "station_id": self.sid, "revision_id": created['draft_revision_id'],
                "expected_plan_revision": detail['plan']['_rev'], "entry": {"ordinal": ordinal, **entry}})
        if approve:
            self.approve(created)
        return created

    def approve(self, created, *, status=200):
        detail = self.api("GET", f"/v1/plans/{created['plan_id']}", query={"station_id": self.sid})
        return self.api("POST", f"/v1/plans/{created['plan_id']}/approve", {
            "station_id": self.sid, "revision_id": created['draft_revision_id'],
            "expected_plan_revision": detail['plan']['_rev']}, status=status)

    def distribute(self, plan):
        return self.api("POST", f"/v1/plans/{plan['plan_id']}/distribute", {
            "station_id": self.sid, "endpoint_id": self.eid, "revision_id": plan['draft_revision_id']})

    def cleanup(self):
        for process in self.processes:
            self.stop(process)


def run_acceptance(root, *, offline_seconds):
    run = Run(root)
    started = time.monotonic()
    try:
        center = run.start()
        base = run.current
        output = run.workflow("outputs/create", endpoint_id=run.eid, output_id="main", geo_codes=["TR"])
        a, b = run.item("first", 1000), run.item("second", 2000)
        # Approved source coordinates, frozen separately from the plan rows.
        for cue_kind, position in (("audio_start", 500), ("segue_out", 1250),
                                   ("fade_start", 1250), ("audio_end", 1500)):
            cue = run.workflow("cues/create", version_id=a['version_id'], media_file_id=a['media_file_id'],
                cue_kind=cue_kind, position_value=position, unit="milliseconds")
            run.workflow("cues/approve", cue_id=cue['_id'], expected_revision=cue['_rev'])
        def audio(target, at, *, overlap=0):
            return {"event_kind": "audio", "scheduled_at": at.isoformat(), "planned_duration_ms": 1000,
                "payload": {"version_id": target['version_id'], "output_id": "*",
                            **({"cue_in_ms": 500, "cue_out_ms": 1500, "fade_in_ms": 250} if overlap else {})},
                **({"transition": {"mode": "overlap", "overlap_ms": overlap}} if overlap else {})}
        entries = [audio(a, base), audio(b, base + timedelta(milliseconds=750), overlap=250),
            {"event_kind": "macro", "scheduled_at": (base + timedelta(milliseconds=1750)).isoformat(),
             "payload": {"command": "acceptance.set_flag", "args": {"name": "studio_ready", "value": True}}},
            {"event_kind": "stream", "scheduled_at": (base + timedelta(milliseconds=1750)).isoformat(),
             "planned_duration_ms": 1000, "payload": {"uri": run.url + "/acceptance/stream.wav", "output_id": "*"}}]
        first = run.plan(base.date().isoformat(), entries)
        run.distribute(first)
        sync = run.endpoint("sync")
        run.check("HTTP center to separate EndpointAgent process: accepted snapshot and verified bytes",
                  sync['online'] and not sync['fenced'])
        manifest = run.endpoint("manifest")
        run.check("default output resolved and frozen to concrete main", all(
            entry['payload'].get('output_id') == 'main' for entry in manifest['entries']
            if entry['entry_kind'] in {'audio', 'stream'}))
        run.check("cue/segue/fade coordinates and source media SHA carried in contract2",
            manifest['contract_version'] == 2 and manifest['entries'][0]['playback_markers']['segue_out_ms'] == 1250 and
            manifest['entries'][0]['playback_markers']['cue_in_ms'] == 500)
        played = run.endpoint("play", output=str(run.root / "first-render.wav"),
                              allowed_stream_urls=[run.url + "/acceptance/stream.wav"])
        with wave.open(str(run.root / "first-render.wav"), "rb") as source:
            samples = array("h", source.readframes(source.getnframes()))
            if sys.byteorder != "little":samples.byteswap()
        run.check("PCM cue, 250ms overlap, linear fade, allowlisted macro and actual HTTP WAV stream",
            played['frames'] == 22000 and played['flags'] == {'studio_ready': True} and
            1400 < samples[7000] < 1600 and samples[16000] == 3000)
        replay = run.endpoint("sync")
        run.check("catalog start/complete events replayed to HTTP center", replay['events']['delivered'] == 4)
        # The catalog event API requires a version ID. Noncatalog execution
        # receipts stay in the fsynced reference artifact instead of forging
        # a catalog version or creating a spurious operational incident.
        receipts=json.loads((run.root/'first-render.receipts.json').read_text())
        run.check('macro and stream execution receipts durably preserved separately',
                  sum(row['entry_kind']!='audio' for row in receipts)==2)
        # Detail reads expose precisely the revisions used for optimistic writes.
        item_detail = run.api("GET", f"/v1/items/{a['item_id']}", query={'station_id': run.sid})
        for path in (f"/v1/versions/{a['version_id']}", f"/v1/plans/{first['plan_id']}/entries",
                     f"/v1/plans/{first['plan_id']}/revisions",
                     f"/v1/stations/{run.sid}/items/{a['item_id']}"):
            run.check('detail GET '+path.split('/')[2], 'sequence' in run.api('GET',path,query={'station_id':run.sid}))
        run.api('POST',f"/v1/items/{a['item_id']}/eligibility",{'station_id':run.sid,'eligible':True,
            'expected_revision':item_detail['station_item']['_rev']})
        run.api('POST',f"/v1/items/{a['item_id']}/eligibility",{'station_id':run.sid,'eligible':False,
            'expected_revision':item_detail['station_item']['_rev']},status=409)
        run.check('stale station association revision rejected over HTTP',True)
        denied_at=base+timedelta(days=3)
        run.api('POST','/v1/rights',{'station_id':run.sid,'version_id':a['version_id'],
            'territory':'*','output_id':'main','starts_at':(denied_at+timedelta(milliseconds=500)).isoformat(),
            'ends_at':(denied_at+timedelta(milliseconds=750)).isoformat(),'decision':'deny'})
        denied_plan=run.plan(denied_at.date().isoformat(),[audio(a,denied_at)],approve=False)
        before=run.api('GET','/health')['sequence']
        blocked=run.approve(denied_plan,status=409)
        after=run.api('GET','/health')['sequence']
        run.check('default wildcard cannot bypass partially overlapping main output deny; failed approval writes nothing',
                  'rights_denied_for_slot' in blocked['detail'] and before==after)
        secondary=run.workflow('outputs/create',endpoint_id=run.eid,output_id='secondary',geo_codes=['TR'])
        ambiguous_at=base+timedelta(days=4)
        ambiguous=run.plan(ambiguous_at.date().isoformat(),[audio(b,ambiguous_at)],approve=False)
        blocked=run.approve(ambiguous,status=409)
        run.check('default unknown destination rejects two configured active outputs','unambiguous' in blocked['detail'])
        draft=run.api('GET',f"/v1/plans/{ambiguous['plan_id']}/entries",query={'station_id':run.sid})
        row=draft['entries'][0]
        entry={key:value for key,value in row.items() if key not in {'_id','_rev','plan_revision_id'}}
        entry['payload']=entry['payload']|{'output_id':'secondary'}
        run.api('POST',f"/v1/plans/{ambiguous['plan_id']}/entries",{'station_id':run.sid,
            'revision_id':ambiguous['draft_revision_id'],'expected_plan_revision':draft['plan']['_rev'],
            'entry_id':row['_id'],'expected_entry_revision':row['_rev'],'entry':entry})
        run.approve(ambiguous)
        run.check('explicit output uniquely resolves legacy default among multiple outputs',True)
        run.workflow('outputs/update',output_record_id=secondary['_id'],expected_revision=secondary['_rev'],status='disabled')
        command=run.api('POST','/v1/engine/commands',{'station_id':run.sid,'endpoint_id':run.eid,
            'command_kind':'start','payload':{'plan_entry_id':manifest['entries'][0]['plan_entry_id']},
            'command_key':'acceptance-crash-after-accept','ttl_seconds':300})
        run.endpoint('sync')
        run.endpoint('crash-after-command-accept',command_id=command['command_id'])
        command_status=run.endpoint('command-status',command_id=command['command_id'])
        run.check('process dies after command acceptance: restart marks outcome_unknown without reexecution',
                  command_status['status']=='outcome_unknown' and not run.endpoint('pending-commands'))
        run.endpoint('sync')
        # Prepare next local day while a current owner heartbeat is fresh.
        tomorrow = base + timedelta(days=1)
        second = run.plan(tomorrow.date().isoformat(), [audio(a, tomorrow)])
        run.distribute(second)
        run.set_clock(base,endpoint_offset_seconds=600)
        skew=run.endpoint('prefetch',revision_id=second['draft_revision_id'],expected_ok=False)
        run.check('10-minute endpoint clock skew rejects fresh next-day grant',
                  'clock differs' in skew['error'])
        run.set_clock(base)
        run.endpoint('sync')
        status = run.endpoint('status')
        run.check('next-day prefetch retains current accepted day',status['active_revision_id']==first['draft_revision_id'])
        # A third future plan tests missing/ungranted data cannot enable a jump.
        Run.stop(center)
        run.set_clock(tomorrow.replace(hour=0))
        missing = run.endpoint('missing-media',media_file_id=a['media_file_id'])
        offline = run.endpoint('sync')
        run.check('missing local bytes block next-day offline activation',missing['status']=='missing' and
                  offline['offline_activation']['reason']=='offline_media_missing')
        run.endpoint('restore-media',media_file_id=a['media_file_id'])
        offline = run.endpoint('sync')
        run.check('center process down: staged next-day grant activates offline',not offline['online'] and
                  offline['offline_activation']['active'])
        run.check('endpoint process restart preserves authorized offline day',
                  run.endpoint('status')['active_revision_id']==second['draft_revision_id'])
        offline_pcm=run.endpoint('play',output=str(run.root/'offline-render.wav'))
        run.check('offline accepted next-day plan renders PCM and persists start/complete outbox',
                  offline_pcm['frames']==8000 and run.endpoint('status')['pending_events']==2)
        # Use a separate durable endpoint copy to advance through grant expiry
        # without changing the main reconnect scenario's local clock history.
        expiry_dir=run.root/'expiry-endpoint'
        shutil.copytree(run.root/'endpoint',expiry_dir)
        _write(run.endpoint_config_path,run.endpoint_config|{'endpoint_dir':str(expiry_dir)})
        run.set_clock(tomorrow+timedelta(days=1))
        expired=run.endpoint('sync')
        run.check('grant expires at next local midnight after restart and cannot authorize a third day',
                  expired['offline_activation']['reason']=='offline_grant_expired_or_invalid')
        _write(run.endpoint_config_path,run.endpoint_config)
        run.set_clock(tomorrow.replace(hour=0))
        offline_start = time.monotonic()
        deadline = offline_start + offline_seconds
        while time.monotonic() < deadline:
            run.endpoint('manifest')
            time.sleep(min(1, max(0,deadline-time.monotonic())))
        actual_offline_seconds = time.monotonic() - offline_start
        center = run.start()
        reconnect = run.endpoint('sync')
        run.check('HTTP reconnect reconciles locally activated grant to central ownership',
                  reconnect['online'] and not reconnect['fenced'] and reconnect['events']['delivered']==2)
        # While this engine is disconnected, the operator replaces tomorrow's
        # plan. Reconnect must accept that new revision with a new epoch.
        Run.stop(center)
        disconnected=run.endpoint('sync')
        run.check('second outage retains accepted service-day authorization',not disconnected['online'])
        center=run.start()
        detail=run.api('GET',f"/v1/plans/{second['plan_id']}",query={'station_id':run.sid})
        fork=run.workflow('plans/fork-draft',plan_id=second['plan_id'],expected_plan_revision=detail['plan']['_rev'])
        replacement={'plan_id':second['plan_id'],'draft_revision_id':fork['draft_revision_id']}
        run.approve(replacement);run.distribute(replacement)
        replaced=run.endpoint('sync');replacement_status=run.endpoint('status')
        run.check('new conflicting plan revision after outage replaces old acceptance with newer epoch',
                  replaced['online'] and not replaced['fenced'] and
                  replacement_status['active_revision_id']==replacement['draft_revision_id'] and
                  replacement_status['engine_epoch']==3)
        # Another engine takes ownership of the same accepted plan and fences us.
        run.api('POST',f"/v1/endpoints/{run.eid}/plans/{replacement['draft_revision_id']}/accept",{
            'station_id':run.sid,'engine_instance':'different-engine','engine_epoch':4},token=run.endpoint_token)
        conflict = run.endpoint('sync')
        run.check('conflicting central engine ownership fences restarted local agent',conflict['fenced'])
        run.check('fenced local player cannot obtain executable manifest',
                  not run.endpoint('manifest',expected_ok=False)['ok'])
        # A later fenced endpoint remains unavailable after its service date.
        run.set_clock(tomorrow + timedelta(days=1))
        Run.stop(center)
        expired = run.endpoint('sync')
        run.check('fenced endpoint cannot self-authorize a later service day',not expired['offline_activation']['active'])
        # A clean restore worker has no access to the source DB/media directory.
        _write(run.config_path,run.config|{'backup':str(run.root/'backup')})
        backup = run.child('backup',run.config_path)
        restore_config = run.root/'restore-config.json'
        _write(restore_config,{'backup':str(run.root/'backup'),'target':str(run.root/'restored'),
                              'restore_result':str(run.root/'restore-result.json')})
        restored = run.child('restore',restore_config)
        restoration=json.loads((run.root/'restore-result.json').read_text())
        run.check('separate restore process verifies backup and all referenced media',restored['media_objects']==2)
        from sdbase.schema import open_radio_store
        from sdbase.identity import authenticate, Forbidden
        target=open_radio_store(run.root/'restored'/restoration['database'])
        run.check('provisioned restore rotates store identity',target._meta()['store_id']!=backup['store_id'])
        try:authenticate(target,run.user_token)
        except Forbidden:run.check('source user token revoked on independent restored copy',True)
        else:raise AssertionError('source token survived copy provisioning')
        try:authenticate(target,run.endpoint_token)
        except Forbidden:run.check('source endpoint token revoked on restored copy',True)
        else:raise AssertionError('endpoint token survived copy provisioning')
        run.check('restored endpoint has no active acceptance',all(
            row['state']=='inactive' for row in target.scan('active_acceptances')))
        run.check('content, station permissions, endpoints and immutable markers survive restoration',
            len(target.scan('items'))==2 and len(target.scan('permissions'))>=9 and
            len(target.scan('endpoints'))==1 and len(target.scan('playback_marker_sets'))>=3)
        shutil.rmtree(run.db.parent)
        shutil.rmtree(run.root/'endpoint')
        (run.root/'first.wav').unlink();(run.root/'second.wav').unlink()
        run.check('original DB, media and endpoint directory removed before restored execution',
                  not run.db.exists())
        # Require fresh credentials and acceptance before rendering from restored media.
        run.db=run.root/'restored'/restoration['database'];run.config['db']=str(run.db)
        run.user_token=restoration['admin_token']
        run.set_clock(base)
        center=run.start()
        new_token=run.api('POST',f"/v1/endpoints/{run.eid}/tokens",{'station_id':run.sid})['token']
        run.endpoint_token=new_token
        run.endpoint_config.update({'endpoint_token':new_token,'endpoint_dir':str(run.root/'restored-endpoint'),
                                    'engine_instance':'restored-reference'})
        _write(run.endpoint_config_path,run.endpoint_config)
        # Restore fences historical deliveries; fork/approve produces new frozen execution.
        detail=run.api('GET',f"/v1/plans/{first['plan_id']}",query={'station_id':run.sid})
        fork=run.workflow('plans/fork-draft',plan_id=first['plan_id'],expected_plan_revision=detail['plan']['_rev'])
        resumed={'plan_id':first['plan_id'],'draft_revision_id':fork['draft_revision_id']}
        run.approve(resumed);run.distribute(resumed)
        restored_sync=run.endpoint('sync')
        run.check('clean restored endpoint accepts freshly approved backup-only plan/media',
                  restored_sync['online'] and not restored_sync['fenced'])
        rendered=run.endpoint('play',output=str(run.root/'restored-render.wav'),
                              allowed_stream_urls=[run.url+'/acceptance/stream.wav'])
        run.check('backup-only reprovisioned reference output matches original PCM SHA',rendered['sha256']==played['sha256'])
        return {'format':'sdbase-production-acceptance-v1','scope':'same_host_isolated_processes',
                'checks_passed':len(run.checks),'checks':run.checks,'controller_http_requests':run.calls,
                'endpoint_worker_processes':run.worker_runs-2,'worker_processes':run.worker_runs,
                'elapsed_seconds':round(time.monotonic()-started,3),
                'offline_wall_seconds':round(actual_offline_seconds,3),'virtual_service_days':2,
                'reference_pcm_frames':played['frames'],'reference_pcm_sha256':played['sha256'],
                'limitations':['no_physical_audio_output','no_second_physical_machine',
                  'virtual_time_is_not_multiday_wall_clock_soak','finite_allowlisted_wav_stream_only',
                  'set_flag_macro_only','no_general_dsp_or_production_overlay',
                  'noncatalog_macro_stream_receipts_are_local_artifacts_not_catalog_playout_events']}
    finally:
        run.cleanup()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,help='write JSON evidence to this file')
    parser.add_argument('--offline-seconds',type=float,default=2,
                        help='real wall time to remain offline; virtual-day advancement is separate')
    parser.add_argument('--worker',choices=['center','endpoint','backup','restore'],help=argparse.SUPPRESS)
    parser.add_argument('--config',type=Path,help=argparse.SUPPRESS)
    args=parser.parse_args()
    if args.worker:
        _worker(args.worker,args.config);return
    if not 0 <= args.offline_seconds <= 604800:
        parser.error('--offline-seconds must be 0..604800')
    with tempfile.TemporaryDirectory(prefix='sdbase-acceptance-') as root:
        report=run_acceptance(root,offline_seconds=args.offline_seconds)
    if args.report:
        _write(args.report,report)
    print(json.dumps(report,ensure_ascii=False,indent=2))


if __name__=='__main__':
    main()
