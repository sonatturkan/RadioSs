#!/usr/bin/env python3
"""Private persisted weekly schedules: CAS, typed boundaries and station fences."""
import copy
from http.client import HTTPConnection
import json
import threading
import unittest
from uuid import uuid4

import test_desktop_automation_calendar as fixtures
from sdbase import desktop_api as api, desktop_playlist as playlists
from sdbase.access_admin import create_station_user
from sdbase.api_server import BoundedHTTPServer
from sdbase.catalog import NotFound
from sdbase.http_server import make_handler
from sdbase.identity import Forbidden, authenticate
from sdbase.integrity import inspect_integrity
from sdbase.schema import open_radio_store
from sdbase.storage import Conflict, StoreError


class DesktopWeeklyProgramme(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.DesktopAutomationCalendar('runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.store, self.sid, self.principal = self.fixture.store, self.fixture.sid, self.fixture.principal

    def config(self, calendar=None, **changes):
        return {'schema': 1, 'week_anchor': '2026-10-05', 'week_count': 1,
                'repeat': False, 'calendar': calendar if calendar is not None else self.fixture.named_config(), **changes}

    def put(self, config, **options):
        return api.put_document(self.store, self.principal, self.sid, 'weekly_programme',
                                'Haftalık yayın programı', config, str(uuid4()), **options)

    def rejected(self, config):
        before = self.store.sequence, self.store.scan('desktop_configurations')
        with self.assertRaises(StoreError):
            self.put(config)
        self.assertEqual((self.store.sequence, self.store.scan('desktop_configurations')), before)

    def test_save_replay_cas_readonly_reopen_and_integrity(self):
        config = self.config(repeat=True)
        original = copy.deepcopy(config)
        key = str(uuid4())
        saved = api.put_document(self.store, self.principal, self.sid, 'weekly_programme',
                                'Haftalık yayın programı', config, key)
        seq = self.store.sequence
        self.assertEqual(api.put_document(self.store, self.principal, self.sid, 'weekly_programme',
                                         'Haftalık yayın programı', config, key), saved)
        self.assertEqual(self.store.sequence, seq)
        changed = copy.deepcopy(config); changed['repeat'] = False
        actual = self.put(changed, document_id=saved['_id'], expected_revision=saved['_rev'])
        with self.assertRaises(Conflict):
            self.put(config, document_id=saved['_id'], expected_revision=saved['_rev'])
        reopened = open_radio_store(self.fixture.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, 'weekly_programme', actual['_id']), actual)
        self.assertEqual(api.list_documents(reopened, self.principal, self.sid, 'weekly_programme')['records'], [actual])
        self.assertEqual(config, original)
        self.assertEqual(inspect_integrity(reopened)['status'], 'ok')

    def test_disabled_source_allows_weekly_repeat_edit_and_local_timezone(self):
        config = self.config()
        config['calendar']['timezone'] = 'Europe/Istanbul'
        config['calendar']['events'][0]['start_minute'] = 540
        saved = self.put(config)
        playlist = self.fixture.playlist
        playlists.delete_playlist(self.store, self.principal, self.sid, playlist['_id'],
            expected_revision=playlist['_rev'], idempotency_key=str(uuid4()))
        changed = copy.deepcopy(config); changed['repeat'] = True
        actual = self.put(changed, document_id=saved['_id'], expected_revision=saved['_rev'])
        self.assertEqual(actual['config'], changed)
        bad = copy.deepcopy(changed)
        bad['calendar']['rotation_library'][0]['sources'][0]['frequency'] += 1
        with self.assertRaises(StoreError):
            self.put(bad, document_id=actual['_id'], expected_revision=actual['_rev'])

    def test_strict_schema_monday_bounds_bool_and_unknown_fields_are_atomic(self):
        good = self.config()
        for field, values in {'schema': [True, 0, 2, 1.0], 'week_anchor': ['2026-10-06', '2026-02-30', '2026-10-05T00:00:00Z', None],
                'week_count': [True, 0, 53, 1.5], 'repeat': [0, 1, 'true', None], 'calendar': [[], None, {'schema': 1, 'events': True}]}.items():
            for value in values:
                with self.subTest(field=field, value=value): self.rejected(dict(good, **{field: value}))
        for field in good:
            bad = copy.deepcopy(good); del bad[field]; self.rejected(bad)
        self.rejected(dict(good, fake_gate=True))
        bad = copy.deepcopy(good); bad['calendar']['events'][0]['date'] = '2026-10-12'; self.rejected(bad)
        bad['calendar']['events'][0]['date'] = '2026-10-04'; self.rejected(bad)
        self.rejected(self.config({'schema': 1, 'events': []}, week_anchor='9999-12-27'))

    def test_complete_multiweek_snapshot_preserves_empty_events_library_song_rules_and_routing(self):
        playlist = self.fixture.add_track_rule_rows()
        rules = [self.fixture.song_rule(playlist['tracks'][0]['_id'], weight=30, order=4, max_plays=2)]
        calendar = self.fixture.named_config(sources=[self.fixture.rotation(count=3,
            track_selection_mode='weighted', track_rules=rules, autodj=self.fixture.autodj_pools())])
        calendar['events'][0]['rotation_selection_mode'] = 'weighted'
        calendar['events'][0]['rotations'][0]['weight'] = 70
        calendar['events'].append(self.fixture.event(date='2026-10-19', rotations=[]))
        empty_definition = {'id': str(uuid4()), 'name': 'Boş taslak', 'sources': [],
                            'settings': copy.deepcopy(calendar['rotation_library'][0]['settings'])}
        calendar['rotation_library'].append(empty_definition)
        calendar['events'].append(self.fixture.event(date='2026-10-25', rotations=[
            {'id': str(uuid4()), 'rotation_id': empty_definition['id'], 'weight': 0}]))
        before = copy.deepcopy(calendar)
        saved = self.put(self.config(calendar, week_count=3, repeat=True))
        self.assertEqual(saved['config']['calendar'], before)
        self.assertEqual(calendar, before)
        self.assertEqual(len(saved['config']['calendar']['events']), 3)
        self.assertEqual(len(saved['config']['calendar']['rotation_library']), 2)

    def test_empty_patterns_52weeks_and_last_day_boundaries(self):
        self.assertEqual(self.put(self.config({'schema': 1, 'events': []}, week_count=52))['config']['week_count'], 52)
        event = self.fixture.event(date='2027-10-03', start_minute=1439, end_minute=1440)
        config = self.config(self.fixture.config(event), week_count=52)
        saved = self.put(config)
        self.assertEqual(saved['config'], config)
        config['calendar']['events'][0]['date'] = '2027-10-04'
        self.rejected(config)

    def test_station_reference_permissions_and_kind_fences(self):
        saved = self.put(self.config())
        viewer = create_station_user(self.store, self.principal, self.sid, 'weekly-viewer', 'Program izleyicisi', ['view'])
        principal = authenticate(self.store, viewer['token'])
        self.assertEqual(api.get_document(self.store, principal, self.sid, 'weekly_programme', saved['_id']), saved)
        with self.assertRaises(Forbidden):
            api.put_document(self.store, principal, self.sid, 'weekly_programme', 'Denied', self.config(), str(uuid4()))
        other = str(uuid4())
        self.store.put('stations', other, {'code': 'WEEKLY-OTHER', 'name': 'Other', 'timezone': 'UTC', 'status': 'active'})
        foreign = playlists.create_playlist(self.store, self.principal, other, 'Foreign', idempotency_key=str(uuid4()))
        self.rejected(self.config(self.fixture.config(self.fixture.event(rotations=[self.fixture.rotation(playlist_id=foreign['_id'])]))))
        with self.assertRaises(NotFound): api.get_document(self.store, self.principal, other, 'weekly_programme', saved['_id'])
        with self.assertRaises(NotFound): api.get_document(self.store, self.principal, self.sid, 'automation_calendar', saved['_id'])

    def test_stale_song_rule_persisted_readable_new_save_rejected(self):
        playlist = self.fixture.add_track_rule_rows(); entry = playlist['tracks'][0]
        config = self.config(self.fixture.named_config(sources=[self.fixture.rotation(
            track_rules=[self.fixture.song_rule(entry['_id'])])]))
        saved = self.put(config)
        playlists.remove_entries(self.store, self.principal, self.sid, playlist['_id'],
            [{'entry_id': entry['_id'], 'expected_revision': entry['_rev']}],
            expected_revision=playlist['_rev'], idempotency_key=str(uuid4()))
        reopened = open_radio_store(self.fixture.fixture.db, read_only=True)
        self.assertEqual(api.get_document(reopened, self.principal, self.sid, 'weekly_programme', saved['_id'])['config'], config)
        self.rejected(config)

    def test_http_generic_workflow_contract_probe_does_not_commit(self):
        base = make_handler(self.store)
        class Handler(base):
            def log_message(self, *arguments): pass
        server = BoundedHTTPServer(('127.0.0.1', 0), Handler, request_timeout=2)
        worker = threading.Thread(target=server.serve_forever, daemon=True); worker.start()
        def request(operation, body, auth=True):
            conn = HTTPConnection('127.0.0.1', server.server_port, timeout=2)
            headers = {'Content-Type': 'application/json', 'Connection': 'close'}
            if auth: headers['Authorization'] = 'Bearer ' + self.fixture.fixture.identity['api_token']
            conn.request('POST', '/v1/workflows/desktop/' + operation, json.dumps(body), headers)
            response = conn.getresponse(); result = response.status, json.loads(response.read()); conn.close(); return result
        try:
            config = self.config(); config['calendar']['events'][0]['rotation_selection_mode'] = 'weighted'
            config['calendar']['events'][0]['rotations'][0]['weight'] = 0
            body = {'station_id': self.sid, 'kind': 'weekly_programme', 'name': 'HTTP program',
                    'config': config, 'idempotency_key': str(uuid4())}
            self.assertEqual(request('put', body, False)[0], 403)
            status, saved = request('put', body); self.assertEqual(status, 200)
            before = self.store.sequence, self.store.scan('desktop_configurations')
            probe = dict(body, document_id=saved['_id'], expected_revision=-1, idempotency_key=str(uuid4()))
            self.assertEqual(request('put', probe)[0], 409)
            self.assertEqual((self.store.sequence, self.store.scan('desktop_configurations')), before)
            self.assertEqual(request('get', {'station_id': self.sid, 'kind': 'weekly_programme', 'document_id': saved['_id']}), (200, saved))
            self.assertEqual(request('list', {'station_id': self.sid, 'kind': 'weekly_programme'})[1]['records'], [saved])
        finally:
            server.shutdown(); server.server_close(); worker.join(timeout=3)
        self.assertEqual(inspect_integrity(self.store)['status'], 'ok')


if __name__ == '__main__': unittest.main(verbosity=2)
