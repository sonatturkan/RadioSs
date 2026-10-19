#!/usr/bin/env python3
"""Private station/version-specific rights reads and explicit existing grants."""
from copy import deepcopy
from http.client import HTTPConnection
import json
import threading
import unittest
from unittest.mock import patch
from uuid import uuid4

import test_desktop_media as fixtures
from sdbase import desktop_media as api, rights, station_outputs
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.catalog import NotFound
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import StoreError

class DesktopMediaRights(unittest.TestCase):
    def setUp(self):
        self.fixture=fixtures.DesktopMedia('runTest');self.fixture.setUp();self.addCleanup(self.fixture.tearDown)
        self.store,self.sid,self.principal=self.fixture.store,self.fixture.sid,self.fixture.principal
        self.imported=self.fixture.load();self.mid=self.imported['media']['_id'];self.vid=self.imported['version']['_id']
        self.output=station_outputs.create_station_output(self.store,self.principal,self.sid,self.fixture.identity['endpoint_id'],'WEB',geo_codes=['LOCAL'])
    def read(self,**changes):
        return api.media_rights(self.store,self.principal,self.sid,self.mid,self.vid,**changes)
    def grant(self,**changes):
        return rights.grant(self.store,self.principal,self.sid,self.vid,territory='*',output_id='WEB',
            starts_at='2026-10-11T00:00:00Z',ends_at='2026-10-12T00:00:00Z',source_ref='Explicit private contract',**changes)
    def test_actual_grants_readonly_no_byte_inspection_copy_reopen_integrity(self):
        grant=self.grant();before=self.store.sequence
        with patch('sdbase.desktop_media._managed_path',side_effect=AssertionError('metadata read must not inspect audio')):
            value=self.read()
        self.assertEqual(self.store.sequence,before);self.assertTrue(value['can_approve']);self.assertEqual(value['records'][0]['rights'],[grant]);self.assertEqual(value['outputs'],[self.output]);self.assertEqual(value['records'][0]['file']['sha256'],self.fixture.analysis['sha256'])
        value['records'][0]['rights'][0]['source_ref']='Unpersisted';self.assertEqual(self.store.get('rights_grants',grant['_id'])['source_ref'],'Explicit private contract')
        reopened=open_radio_store(self.fixture.db,read_only=True)
        self.assertEqual(api.media_rights(reopened,self.principal,self.sid,self.mid,self.vid)['records'][0]['rights'],[grant]);self.assertEqual(inspect_integrity(reopened)['status'],'ok')
    def test_bounded_pages_selected_grant_focus_and_foreign_grant_fence(self):
        grants=[self.grant() for _ in range(53)];before=self.store.sequence
        first=self.read(limit=20);self.assertEqual(len(first['records'][0]['rights']),20);self.assertEqual(first['rights_total'],53);self.assertEqual(first['rights_next_offset'],20)
        seen=[]
        for offset in (0,20,40):seen.extend(g['_id'] for g in self.read(offset=offset,limit=20)['records'][0]['rights'])
        self.assertEqual(seen,sorted(g['_id'] for g in grants));chosen=seen[-1];page=self.read(limit=20,grant_id=chosen);self.assertEqual(page['rights_offset'],40);self.assertIn(chosen,[g['_id'] for g in page['records'][0]['rights']]);self.assertIsNone(page['rights_next_offset'])
        with self.assertRaises(NotFound):self.read(grant_id=str(uuid4()))
        self.assertEqual(self.store.sequence,before)
    def test_typed_bounds_version_identity_and_unverified_selection_are_atomic(self):
        before=self.store.sequence
        for name,values in {'offset':[True,-1,'0',1.5],'limit':[True,0,101,'20']}.items():
            for value in values:
                with self.subTest(name=name,value=value):
                    with self.assertRaises(StoreError):self.read(**{name:value})
        with self.assertRaises(NotFound):api.media_rights(self.store,self.principal,self.sid,self.mid,str(uuid4()))
        with self.assertRaises(StoreError):self.read(grant_id='invalid')
        self.assertEqual(self.store.sequence,before)
        file_id=self.imported['media']['media_file_id'];media=self.store.get('media_files',file_id);data={k:v for k,v in media.items() if not k.startswith('_')};data['status']='error';self.store.put('media_files',file_id,data,expected_revision=media['_rev'])
        sequence=self.store.sequence
        with self.assertRaises(StoreError):self.read()
        self.assertEqual(self.store.sequence,sequence)
    def test_viewer_forbidden_write_cross_station_and_grant_filter(self):
        viewer=create_station_user(self.store,self.principal,self.sid,'rights-viewer','Viewer',['view']);principal=authenticate(self.store,viewer['token'])
        view=api.media_rights(self.store,principal,self.sid,self.mid,self.vid);self.assertFalse(view['can_approve'])
        before=self.store.sequence
        with self.assertRaises(Forbidden):rights.grant(self.store,principal,self.sid,self.vid,territory='*',output_id='WEB',starts_at='2026-10-11T00:00:00Z',ends_at='2026-10-12T00:00:00Z',source_ref='Denied')
        self.assertEqual(self.store.sequence,before)
        other=str(uuid4());self.store.put('stations',other,{'code':'RIGHTS-OTHER','name':'Other','timezone':'UTC','status':'active'})
        with self.assertRaises(NotFound):api.media_rights(self.store,self.principal,other,self.mid,self.vid)
        with self.assertRaises(Forbidden):api.media_rights(self.store,principal,other,self.mid,self.vid)
        self.assertEqual(self.read()['records'][0]['rights'],[])
    def test_fresh_output_catalogue_disabled_outputs_and_empty_not_invented(self):
        before=self.store.sequence;self.assertEqual(self.read()['outputs'],[self.output]);self.assertEqual(self.store.sequence,before)
        station_outputs.update_station_output(self.store,self.principal,self.sid,self.output['_id'],expected_revision=self.output['_rev'],status='disabled')
        before=self.store.sequence;value=self.read();self.assertEqual(value['outputs'],[]);self.assertEqual(value['records'][0]['rights'],[]);self.assertEqual(self.store.sequence,before);self.assertEqual(inspect_integrity(self.store)['status'],'ok')
    def test_http_authenticated_read_explicit_write_receipt_and_actual_readback(self):
        base=make_handler(self.store)
        class Handler(base):
            def log_message(self,*args):pass
        server=BoundedHTTPServer(('127.0.0.1',0),Handler,request_timeout=2);worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
        def request(path,body,auth=True):
            c=HTTPConnection('127.0.0.1',server.server_port,timeout=2);headers={'Content-Type':'application/json','Connection':'close'}
            if auth:headers['Authorization']='Bearer '+self.fixture.identity['api_token']
            c.request('POST',path,json.dumps(body),headers);r=c.getresponse();value=r.status,json.loads(r.read());c.close();return value
        try:
            body={'station_id':self.sid,'media_id':self.mid,'version_id':self.vid}
            self.assertEqual(request('/v1/workflows/desktop-media/rights',body,False)[0],403);before=self.store.sequence
            self.assertEqual(request('/v1/workflows/desktop-media/rights',body)[0],200);self.assertEqual(self.store.sequence,before)
            intent={'station_id':self.sid,'version_id':self.vid,'territory':'*','output_id':'WEB','starts_at':'2026-10-11T00:00:00Z','ends_at':'2026-10-12T00:00:00Z','decision':'deny','source_ref':'Native explicit contract'}
            terms={k:v for k,v in intent.items() if k not in ('station_id','version_id')}
            self.assertEqual(request('/v1/workflows/desktop-media/rights',dict(body,intent=terms))[1]['matching_grants'],[])
            status,grant=request('/v1/rights',intent);self.assertEqual(status,200);self.assertEqual(grant['approved_by'],self.principal.user_id)
            before=self.store.sequence;status,value=request('/v1/workflows/desktop-media/rights',dict(body,grant_id=grant['_id']));self.assertEqual(status,200);self.assertEqual(value['records'][0]['rights'],[grant]);self.assertEqual(self.store.sequence,before)
            self.assertEqual(request('/v1/workflows/desktop-media/rights',dict(body,intent=terms))[1]['matching_grants'],[grant]);self.assertEqual(self.store.sequence,before)
        finally:server.shutdown();server.server_close();worker.join(timeout=3)
        self.assertEqual(inspect_integrity(self.store)['status'],'ok')

if __name__=='__main__':unittest.main(verbosity=2)
