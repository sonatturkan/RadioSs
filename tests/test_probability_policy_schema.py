#!/usr/bin/env python3
"""Private real DB/HTTP and managed PCM acceptance of PDF automation policies."""
import copy
from http.client import HTTPConnection
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4
import test_desktop_api as fixtures
import test_desktop_automation_calendar as calendar_fixtures
import test_desktop_playout as playout_fixtures
from sdbase import desktop_api as api, desktop_playlist as playlists, desktop_playout as playback
from sdbase.desktop_probability_policy import validate_probability_policy
from sdbase.api_server import BoundedHTTPServer
from sdbase.http_server import make_handler
from sdbase.schema import open_radio_store
from sdbase.integrity import inspect_integrity
from sdbase.storage import Conflict, StoreError
ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build/automation-radyo-pdf-20261013/backend/fixtures"

class ProbabilityPolicySchema(unittest.TestCase):
    def setUp(self):
        OUTPUT.mkdir(parents=True, exist_ok=True)
        original = tempfile.TemporaryDirectory
        def isolated(*args, **kwargs):
            kwargs["dir"] = OUTPUT
            return original(*args, **kwargs)
        self.helper = calendar_fixtures.DesktopAutomationCalendar("runTest")
        with patch.object(fixtures.tempfile, "TemporaryDirectory", side_effect=isolated):
            self.helper.setUp()
        self.addCleanup(self.helper.doCleanups)
        self.fixture = self.helper.fixture
        self.store, self.sid, self.principal = self.helper.store, self.helper.sid, self.helper.principal
        self.music = self.helper.add_track_rule_rows()
        self.jingle = playlists.create_playlist(self.store, self.principal, self.sid, "Jingle", purpose="jingle", idempotency_key=str(uuid4()))
    def policy(self):
        return {"schema":1, "seed":"Türkçe", "transitions":[{"id":str(uuid4()),"mode":1,"overlap_ms":2000,"early_seconds":10,"effect":"lowpass","probability":60,"frequency":2,"interval_minutes":15,"excluded_times":[{"start_minute":1380,"end_minute":60}]}],
          "jingles":[{"id":str(uuid4()),"playlist_id":self.jingle["_id"],"player":31,"anchor":"before_end","seconds":.1,"probability":25}],
          "children":[{"id":str(uuid4()),"scope":"track","ids":[self.music["tracks"][0]["_id"]],"probability":70,"policy":{"schema":1,"transitions":[]}},
                      {"id":str(uuid4()),"scope":"track","ids":[],"probability":None,"policy":{"schema":1}}]}
    def config(self):
        config=self.helper.named_config(); policy=self.policy()
        config["rotation_library"][0]["settings"].update(automation_schema=True,probability_policy=policy)
        config["rotation_library"][0]["sources"][0]["settings"]={"probability_policy":copy.deepcopy(policy)}
        config["events"][0]["probability_policy"]=copy.deepcopy(policy)
        return config
    def put(self,config,kind="automation_calendar",key=None,**extra):
        return api.put_document(self.store,self.principal,self.sid,kind,"PDF policy",config,key or str(uuid4()),**extra)
    def rejected(self,config):
        before=self.store.sequence
        with self.assertRaises(StoreError):self.put(config)
        self.assertEqual(self.store.sequence,before)
    def test_calendar_all_locations_cas_replay_reopen_without_mutation(self):
        config=self.config();original=copy.deepcopy(config);key=str(uuid4());saved=self.put(config,key=key);seq=self.store.sequence
        self.assertEqual(config,original);self.assertEqual(self.put(config,key=key),saved);self.assertEqual(self.store.sequence,seq)
        reopened=open_radio_store(self.fixture.db,read_only=True)
        self.assertEqual(api.get_document(reopened,self.principal,self.sid,"automation_calendar",saved["_id"])["config"],original)
        changed=copy.deepcopy(config);changed["rotation_library"][0]["settings"]["automation_schema"]=False
        self.put(changed,document_id=saved["_id"],expected_revision=saved["_rev"])
        with self.assertRaises(Conflict):self.put(config,document_id=saved["_id"],expected_revision=saved["_rev"])
        self.assertEqual(inspect_integrity(open_radio_store(self.fixture.db,read_only=True))["status"],"ok")
    def test_weekly_wrapper_and_legacy_no_policy_preservation(self):
        weekly={"schema":1,"week_anchor":"2026-10-05","week_count":1,"repeat":True,"calendar":self.config()}
        saved=self.put(weekly,kind="weekly_programme");self.assertEqual(saved["config"],weekly)
        legacy=self.helper.named_config();saved=self.put(legacy);self.assertEqual(saved["config"],legacy)
        self.assertNotIn("probability_policy",saved["config"]["rotation_library"][0]["settings"])
    def test_marker_strict_bool_root_only_and_checkpoint_not_authored(self):
        for value in (1,None,"true",[]):
            bad=self.config();bad["rotation_library"][0]["settings"]["automation_schema"]=value;self.rejected(bad)
        bad=self.config();bad["rotation_library"][0]["sources"][0]["settings"]["automation_schema"]=True;self.rejected(bad)
        bad=self.config();bad["events"][0]["policy_selection_checkpoint"]={};self.rejected(bad)
    def test_untrusted_numeric_enums_and_unknown_fields_are_atomic(self):
        mutations=[("probability",True),("probability",-1),("probability",101),("probability",float("nan")),("mode",[]),("effect",{}),("frequency",0),("interval_minutes",10081),("early_seconds",3601),("overlap_ms",30001)]
        for field,value in mutations:
            bad=self.config();bad["events"][0]["probability_policy"]["transitions"][0][field]=value
            with self.subTest(field=field,value=value):self.rejected(bad)
        for field,value in (("player",32),("anchor",[]),("seconds",float("inf")),("duck_db",1),("fade_ms",5001)):
            bad=self.config();bad["events"][0]["probability_policy"]["jingles"][0][field]=value;self.rejected(bad)
        bad=self.config();bad["events"][0]["probability_policy"]["unknown"]=True;self.rejected(bad)
        for value in ("Mars/Olympus","",3,None,"Europe/"+"x"*60):
            bad=self.config();bad["events"][0]["probability_policy"]["transitions"][0]["timezone"]=value
            with self.subTest(timezone=value):self.rejected(bad)
        local=self.config()
        for path in (local["events"][0]["probability_policy"]["transitions"][0],local["events"][0]["probability_policy"]["jingles"][0]):
            path["timezone"]="Europe/Istanbul"
        self.assertEqual(self.put(local)["config"],local)
    def test_groups_rest_disjoint_totals_schema_and_catalogue_fences(self):
        for mutate in (lambda p:p["children"].append(copy.deepcopy(p["children"][1])),
                       lambda p:p["children"][1].update(ids=p["children"][0]["ids"]),
                       lambda p:p["children"][1].update(probability=50),
                       lambda p:p["children"][0]["policy"].clear(),
                       lambda p:p["children"][0].update(ids=[str(uuid4())]),
                       lambda p:p["children"][0].update(scope=[])):
            bad=self.config();mutate(bad["events"][0]["probability_policy"]);self.rejected(bad)
        other=str(uuid4());self.store.put("stations",other,{"code":"OTHER","name":"Other","timezone":"UTC","status":"active"})
        with self.assertRaises(StoreError):validate_probability_policy(self.policy(),other,self.store._state)
    def test_limits_depth_choice_count_source_hint_count_and_undefined_values(self):
        policy={"schema":1,"transitions":[{"id":str(uuid4()),"mode":0,"probability":None},{"id":str(uuid4()),"mode":0,"probability":0}]}
        validate_probability_policy(policy,self.sid,self.store._state)
        bad=copy.deepcopy(policy);bad["transitions"]=[{"id":str(uuid4()),"mode":0} for _ in range(17)]
        with self.assertRaises(StoreError):validate_probability_policy(bad,self.sid,self.store._state)
        deep={"schema":1};root=deep
        for _ in range(9):
            child={"schema":1};root["children"]=[{"id":str(uuid4()),"scope":"track","ids":[],"policy":child}];root=child
        with self.assertRaises(StoreError):validate_probability_policy(deep,self.sid,self.store._state)
        config=playout_fixtures.DesktopPlayout.automation(self.music["_id"]);config["probability_source_policies"]=[{"schema":1}]*51
        with self.assertRaises(StoreError):api.validate_configuration("automation",config,self.sid,self.store._state)
    def bound(self,name):
        binding=self.fixture.verified_binding();pl=playlists.create_playlist(self.store,self.principal,self.sid,name,idempotency_key=str(uuid4()))
        pl=playlists.save_entry(self.store,self.principal,self.sid,pl["_id"],playout_fixtures.DesktopPlayout.metadata(binding),expected_revision=pl["_rev"],idempotency_key=str(uuid4()))
        return pl,binding
    def test_origin_context_differs_from_occurrence_and_media_association_is_enforced(self):
        pl,binding=self.bound("Real origin");origin=pl["tracks"][0]["_id"]
        ctx={"automation_id":str(uuid4()),"playlist_id":pl["_id"],"entry_id":origin,"source_id":str(uuid4())}
        config=playout_fixtures.DesktopPlayout.programme(binding,policy_context=ctx,probability_policy=self.policy())
        occurrence=config["events"][0]["id"];seq=self.store.sequence;result=playback.prepare(self.store,self.principal,self.sid,"programme",config=config)
        self.assertEqual(result["tracks"][0]["entry_id"],occurrence);self.assertNotEqual(occurrence,origin)
        self.assertEqual(result["tracks"][0]["policy_context"],ctx);self.assertEqual(self.store.sequence,seq)
        _,wrongbinding=self.bound("Different real media");bad=copy.deepcopy(config);bad["events"][0].update(wrongbinding)
        with self.assertRaises(StoreError):playback.prepare(self.store,self.principal,self.sid,"programme",config=bad)
        bad=copy.deepcopy(config);bad["events"][0]["policy_context"]["entry_id"]=occurrence
        with self.assertRaises(StoreError):playback.prepare(self.store,self.principal,self.sid,"programme",config=bad)
    def test_genuine_verified_auxiliary_pool_separate_in_initial_and_programme_prepare(self):
        self.jingle,binding=self.bound("Real jingle PCM");policy=self.policy();seq=self.store.sequence
        auto=playout_fixtures.DesktopPlayout.automation(self.music["_id"]);auto.update(jingleEnabled=False,jinglePlaylist=None,fallbackMode=0,fallbackPlaylist=None,includeProgramme=False,probability_policy=policy,probability_source_policies=[copy.deepcopy(policy)])
        initial=playback.prepare(self.store,self.principal,self.sid,"automation",config=auto)
        programme=playout_fixtures.DesktopPlayout.programme(probability_policy=policy)
        selected=playback.prepare(self.store,self.principal,self.sid,"programme",config=programme)
        self.assertEqual(len(selected["tracks"]),1);self.assertEqual(len(initial["jingle_sources"]),1)
        for result in (initial,selected):
            row=result["jingle_sources"][0]["tracks"][0];self.assertTrue(row["eligible"]);self.assertTrue(row["detail"]["version"]["verified"])
            self.assertEqual(row["detail"]["version"]["sha256"],binding["media_sha256"]);self.assertTrue(Path(row["detail"]["version"]["managed_path"]).is_file())
            self.assertEqual(row["detail"]["analysis"]["sample_frames"],1600)
        self.assertEqual(self.store.sequence,seq)
    def test_no_policy_prepare_shape_has_no_auxiliary_or_policy_defaults(self):
        result=playback.prepare(self.store,self.principal,self.sid,"playlist",playlist_id=self.music["_id"])
        self.assertNotIn("jingle_sources",result)
        for row in result["tracks"]:self.assertNotIn("probability_policy",row);self.assertNotIn("policy_context",row)
    def test_authenticated_http_policy_put_reopen_and_atomic_rejection(self):
        parent=make_handler(self.store)
        class Handler(parent):
            def log_message(self,*_args):pass
        server=BoundedHTTPServer(("127.0.0.1",0),Handler,request_timeout=2);worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start();connection=HTTPConnection("127.0.0.1",server.server_port,timeout=3)
        try:
            headers={"Content-Type":"application/json","Authorization":"Bearer "+self.fixture.identity["api_token"]}
            def post(body):
                connection.request("POST","/v1/workflows/desktop/put",json.dumps(body),headers);response=connection.getresponse();return response.status,json.loads(response.read())
            body={"station_id":self.sid,"kind":"automation_calendar","name":"HTTP PDF","config":self.config(),"idempotency_key":str(uuid4())};status,saved=post(body);self.assertEqual(status,200)
            reopened=open_radio_store(self.fixture.db,read_only=True);self.assertEqual(api.get_document(reopened,self.principal,self.sid,"automation_calendar",saved["_id"])["config"],body["config"])
            seq=self.store.sequence;bad=copy.deepcopy(body);bad["idempotency_key"]=str(uuid4());bad["config"]["events"][0]["probability_policy"]["transitions"][0]["probability"]=101
            status,error=post(bad);self.assertEqual(status,422);self.assertEqual(error["error"],"invalid_request");self.assertEqual(self.store.sequence,seq)
        finally:
            connection.close();server.shutdown();server.server_close();worker.join(timeout=3)

if __name__=="__main__":unittest.main()
