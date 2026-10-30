"""Isolated real-socket space, authorization and quota checks. Never uses production data."""
import concurrent.futures
from contextlib import closing
import io
import json
import os
import sqlite3
import subprocess
import threading
import time
import unittest
import zipfile
from urllib.parse import quote
from test_async_app import BASE, Node

GIB = 1024 ** 3


class FileSpaces(unittest.TestCase):
    def setUp(self):
        self.node = Node()
        self.addCleanup(self.node.close)
        self.admin = self.node.login()

    def call(self, method, path, body=None, cookie=None):
        return self.node.request(method, path, body, cookie)

    def data(self, path, cookie):
        status, _, payload = self.call('GET', path, cookie=cookie)
        self.assertEqual(status, 200, payload)
        return json.loads(payload)

    def user(self, name, role='user'):
        self.assertEqual(self.call('POST', '/api/users', {'username': name, 'password': 'Disposable-space-password', 'role': role}, self.admin)[0], 201)
        person = next(row for row in self.data('/api/users', self.admin)['users'] if row['username'] == name)
        return person['id'], self.node.login(name, 'Disposable-space-password')

    def configure(self, uid, quota=5 * GIB, upload=False, modify=False, delete=False, actor=None):
        return self.call('POST', '/api/admin/storage', {'user_id': uid, 'storage_limit': quota,
                         'public_upload': upload, 'public_modify': modify, 'public_delete': delete}, actor or self.admin)

    def put(self, space, name, content, actor):
        return self.call('PUT', f'/api/spaces/{space}/upload?path={quote(name, safe="")}', content, actor)

    def sql(self, query, values=()):
        with closing(sqlite3.connect(self.node.data / 'myfolder.sqlite3')) as db, db:
            return db.execute(query, values).fetchall()

    def restart(self):
        self.node.process.terminate(); self.node.process.wait(timeout=5); self.node.process.stderr.close()
        env = dict(os.environ, MYFOLDER_DATA=str(self.node.data), MYFOLDER_HOST='127.0.0.1',
                   MYFOLDER_PORT=str(self.node.port), MYFOLDER_WEB=str(BASE / 'web' / 'dist'))
        self.node.process = subprocess.Popen([os.environ['MYFOLDER_CPP_BINARY']], cwd=BASE, env=env,
                                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            try:
                if self.call('GET', '/')[0] == 200: return
            except OSError: pass
            time.sleep(.05)
        self.fail('Test server did not restart')

    def test_personal_default_quota_and_cross_account_isolation(self):
        alice_id, alice = self.user('alice')
        _, bob = self.user('bob')
        for actor in (alice, bob, self.admin):
            info = self.data('/api/spaces', actor)
            self.assertEqual(info['personal'], {'used': 0, 'limit': 5 * GIB})
        self.assertEqual(self.put('personal', 'secret.txt', b'alice private', alice)[0], 201)
        self.assertEqual(self.put('personal', 'secret.txt', b'bob private', bob)[0], 201)
        self.assertEqual(self.call('GET', '/api/spaces/personal/download?path=secret.txt', cookie=alice)[2], b'alice private')
        self.assertEqual(self.call('GET', f'/api/spaces/personal/download?path=secret.txt&user_id={alice_id}', cookie=bob)[2], b'bob private')
        self.assertEqual(self.call('GET', '/api/spaces/personal/download?path=secret.txt', cookie=self.admin)[0], 404)
        self.assertEqual(self.data('/api/spaces/public/list', bob)['items'], [])
        self.assertEqual(self.call('GET', '/api/download?path=secret.txt', cookie=self.admin)[0], 404)
        status, _, payload = self.call('GET', '/api/spaces/personal/folder.zip', cookie=alice)
        self.assertEqual(status, 200)
        with zipfile.ZipFile(io.BytesIO(payload)) as bundle:
            self.assertEqual(bundle.namelist(), ['secret.txt'])
            self.assertEqual(bundle.read('secret.txt'), b'alice private')
        for operation in ('list', 'download', 'folder.zip'):
            self.assertEqual(self.call('GET', f'/api/spaces/personal/{operation}?path=..%2Fpublic', cookie=alice)[0], 400)
        self.assertEqual(self.call('GET', '/api/spaces', cookie=None)[0], 401)
        self.assertEqual(self.put('personal', 'anonymous', b'x', None)[0], 401)

    def test_public_visible_to_new_registered_users_without_legacy_grants(self):
        from test_auth_approvals import Browser
        person = Browser(self.node)
        self.assertEqual(person.register('new_space_user')[0], 201)
        requests = self.data('/api/admin/registrations', self.admin)['requests']
        request_id = next(item['id'] for item in requests if item['username'] == 'new_space_user')
        self.assertEqual(self.call('POST', '/api/admin/registrations/review', {'id': request_id, 'decision': 'approve'}, self.admin)[0], 200)
        self.assertEqual(person.login('new_space_user', 'new-password')[0], 200)
        cookie = 'session=' + person.cookies['session']
        self.assertEqual(self.call('GET', '/api/list', cookie=cookie)[0], 403)
        self.assertEqual(self.data('/api/spaces/public/list', cookie)['permissions'], ['download'])
        self.assertEqual(self.put('personal', 'my.txt', b'private', cookie)[0], 201)
        self.assertEqual(self.put('public', 'no.txt', b'blocked', cookie)[0], 403)
        self.assertEqual(self.put('public', 'hello.txt', b'public data', self.admin)[0], 201)
        self.assertEqual(self.call('GET', '/api/spaces/public/download?path=hello.txt', cookie=cookie)[2], b'public data')

    def test_public_upload_modify_and_delete_are_independent_and_revocable(self):
        uid, actor = self.user('writer')
        self.assertEqual(self.put('public', 'p.txt', b'public', self.admin)[0], 201)
        self.assertEqual(self.configure(uid, upload=True)[0], 200)
        self.assertEqual(self.put('public', 'new.txt', b'new', actor)[0], 201)
        self.assertEqual(self.put('public', 'p.txt', b'overwrite', actor)[0], 403)
        self.assertEqual(self.call('DELETE', '/api/spaces/public/delete?path=new.txt', cookie=actor)[0], 403)
        self.assertEqual(self.configure(uid, modify=True)[0], 200)
        self.assertEqual(self.put('public', 'p.txt', b'modified', actor)[0], 201)
        self.assertEqual(self.put('public', 'another.txt', b'new', actor)[0], 403)
        self.assertEqual(self.call('POST', '/api/spaces/public/move', {'source': 'p.txt', 'destination': 'renamed.txt'}, actor)[0], 200)
        self.assertEqual(self.configure(uid)[0], 200)
        self.assertEqual(self.put('public', 'renamed.txt', b'blocked', actor)[0], 403)
        self.assertEqual(self.configure(uid, delete=True)[0], 200)
        self.assertEqual(self.call('DELETE', '/api/spaces/public/delete?path=new.txt', cookie=actor)[0], 200)
        self.assertEqual(self.call('DELETE', '/api/spaces/public/delete?path=', cookie=self.admin)[0], 400)
        self.assertEqual(self.call('GET', '/api/spaces/public/list')[0], 401)

    def test_personal_quota_overwrite_shrink_and_separate_usage(self):
        uid, alice = self.user('quota_alice')
        bob_id, bob = self.user('quota_bob')
        self.assertEqual(self.configure(uid, quota=4)[0], 200)
        self.assertEqual(self.configure(bob_id, quota=4)[0], 200)
        self.assertEqual(self.put('personal', 'a', b'abcd', alice)[0], 201)
        self.assertEqual(self.put('personal', 'b', b'x', alice)[0], 413)
        self.assertEqual(self.put('personal', 'a', b'four', bob)[0], 201)
        self.assertEqual(self.data('/api/spaces', alice)['personal'], {'used': 4, 'limit': 4})
        self.assertEqual(self.configure(uid, quota=2)[0], 200)
        self.assertEqual(self.put('personal', 'a', b'abc', alice)[0], 201)
        self.assertEqual(self.put('personal', 'a', b'12345', alice)[0], 413)
        self.assertEqual(self.put('personal', 'a', b'x', alice)[0], 201)
        self.assertEqual(self.put('personal', 'b', b'y', alice)[0], 201)
        self.assertEqual(self.data('/api/spaces/personal/list', alice)['storage'], {'used': 2, 'limit': 2})
        self.assertEqual(self.call('DELETE', '/api/spaces/personal/delete?path=a', cookie=alice)[0], 200)
        self.assertEqual(self.data('/api/spaces', alice)['personal']['used'], 1)

    def test_server_quota_counts_public_private_and_legacy(self):
        _, actor = self.user('total_quota')
        self.assertEqual(self.put('personal', 'a', b'123', actor)[0], 201)
        self.assertEqual(self.put('public', 'b', b'123', self.admin)[0], 201)
        self.assertEqual(self.call('PUT', '/api/upload?path=legacy', b'123', self.admin)[0], 201)
        self.assertEqual(self.data('/api/server-info', self.admin)['storage_used'], 9)
        self.assertEqual(self.call('POST', '/api/server-info', {'name': 'Quota', 'storage_limit': 10}, self.admin)[0], 200)
        self.assertEqual(self.put('personal', 'too-big', b'xx', actor)[0], 413)
        self.assertEqual(self.put('public', 'too-big', b'xx', self.admin)[0], 413)
        self.assertEqual(self.call('PUT', '/api/upload?path=too-big', b'xx', self.admin)[0], 413)

    def test_management_roles_and_validation(self):
        uid, actor = self.user('managed')
        manager_id, manager = self.user('manager', 'admin')
        other_id, _ = self.user('other_manager', 'admin')
        self.assertEqual(self.call('GET', '/api/admin/storage', cookie=actor)[0], 403)
        self.assertEqual(self.configure(uid, actor=actor)[0], 403)
        self.assertEqual(self.configure(uid, quota=10, upload=True, actor=manager)[0], 200)
        self.assertEqual(self.configure(other_id, actor=manager)[0], 403)
        self.assertEqual(self.configure(1, actor=manager)[0], 403)
        self.assertNotIn(other_id, [row['id'] for row in self.data('/api/admin/storage', manager)['users']])
        self.assertEqual(self.configure(manager_id, quota=10, actor=manager)[0], 200)
        for value in (0, -1, 1.5, True, '50', 2**64 - 1):
            with self.subTest(value=value):
                self.assertEqual(self.configure(uid, quota=value)[0], 400)
        self.assertEqual(self.configure(uid, upload='true')[0], 400)
        self.assertEqual(self.configure(99999)[0], 404)
        self.assertEqual(self.data('/api/spaces', actor)['personal']['limit'], 10)

    def test_deleted_user_id_reuse_does_not_inherit_private_files(self):
        uid, actor = self.user('old_user')
        self.assertEqual(self.put('personal', 'private.txt', b'keep private', actor)[0], 201)
        key = self.sql('SELECT storage_key FROM user_storage WHERE user_id=?', (uid,))[0][0]
        self.assertEqual(self.call('DELETE', f'/api/users?id={uid}', cookie=self.admin)[0], 200)
        new_id, new_user = self.user('new_user')
        self.assertEqual(new_id, uid)  # SQLite may reuse the maximum deleted integer ID.
        self.assertEqual(self.data('/api/spaces/personal/list', new_user)['items'], [])
        self.assertNotEqual(self.sql('SELECT storage_key FROM user_storage WHERE user_id=?', (new_id,))[0][0], key)
        self.assertTrue((self.node.data / 'personal' / key / 'private.txt').is_file())
        self.assertEqual(self.data('/api/server-info', self.admin)['storage_used'], len(b'keep private'))

    def test_upgrade_keeps_legacy_files_grants_and_persists_new_configuration(self):
        uid, actor = self.user('upgrade_user')
        self.assertEqual(self.call('PUT', '/api/upload?path=legacy.txt', b'old data', self.admin)[0], 201)
        grants = self.sql('SELECT * FROM grants')
        self.node.process.terminate(); self.node.process.wait(timeout=5); self.node.process.stderr.close()
        with closing(sqlite3.connect(self.node.data / 'myfolder.sqlite3')) as db, db:
            db.execute('DROP TABLE public_grants'); db.execute('DROP TABLE user_storage')
        self.restart()
        self.assertEqual(self.sql('SELECT * FROM grants'), grants)
        self.assertEqual(self.call('GET', '/api/download?path=legacy.txt', cookie=actor)[2], b'old data')
        self.assertEqual(self.data('/api/spaces/public/list', actor)['items'], [])
        self.assertEqual(self.data('/api/spaces/personal/list', actor)['items'], [])
        self.assertEqual(self.configure(uid, quota=123, upload=True)[0], 200)
        self.assertEqual(self.put('personal', 'own.txt', b'private', actor)[0], 201)
        self.restart()
        self.assertEqual(self.data('/api/spaces', actor)['personal'], {'used': 7, 'limit': 123})
        self.assertIn('upload', self.data('/api/spaces/public/list', actor)['permissions'])
        self.assertEqual(self.call('GET', '/api/spaces/public/download?path=legacy.txt', cookie=actor)[0], 404)

    @unittest.skipUnless(os.name == 'nt', 'Windows path parsing check')
    def test_windows_drive_ads_and_trimmed_parent_segments_are_rejected(self):
        _, actor = self.user('windows_paths')
        for path in ('G:/outside', 'safe/.. /outside', 'file.txt:stream', 'safe/../outside'):
            with self.subTest(path=path):
                self.assertEqual(self.call('GET', '/api/spaces/personal/download?path=' + quote(path, safe=''), cookie=actor)[0], 400)

    def test_concurrent_personal_uploads_respect_quota_and_do_not_collide_across_users(self):
        uid, actor = self.user('parallel_alice')
        other_id, other = self.user('parallel_bob')
        self.assertEqual(self.configure(uid, quota=4 * 1024**2)[0], 200)
        self.assertEqual(self.configure(other_id, quota=4 * 1024**2)[0], 200)
        content = b'x' * (3 * 1024**2)
        barrier = threading.Barrier(2)
        def send(name, cookie):
            barrier.wait(timeout=10)
            return self.put('personal', name, content, cookie)[0]
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = [pool.submit(send, name, actor) for name in ('a', 'b')]
            self.assertEqual(sorted(result.result(timeout=30) for result in results), [201, 413])
        self.assertEqual(self.data('/api/spaces', actor)['personal']['used'], len(content))
        # Same relative names in different personal roots must not share a reservation key.
        for name in ('a', 'b'):
            self.call('DELETE', f'/api/spaces/personal/delete?path={name}', cookie=actor)
        barrier = threading.Barrier(2)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = [pool.submit(send, 'same', cookie) for cookie in (actor, other)]
            self.assertEqual([result.result(timeout=30) for result in results], [201, 201])

    @unittest.skipIf(os.name == 'nt', 'Linux deployment filesystem check')
    def test_symlinks_cannot_escape_personal_root(self):
        uid, actor = self.user('symlink_user')
        self.data('/api/spaces', actor)
        key = self.sql('SELECT storage_key FROM user_storage WHERE user_id=?', (uid,))[0][0]
        root = self.node.data / 'personal' / key
        secret = self.node.data / 'outside.txt'
        secret.write_bytes(b'outside')
        (root / 'escape').symlink_to(secret)
        self.assertEqual(self.call('GET', '/api/spaces/personal/download?path=escape', cookie=actor)[0], 400)
        self.assertEqual(self.data('/api/spaces/personal/list', actor)['items'], [])
        status, _, payload = self.call('GET', '/api/spaces/personal/folder.zip', cookie=actor)
        self.assertEqual(status, 200)
        with zipfile.ZipFile(io.BytesIO(payload)) as bundle:
            self.assertEqual(bundle.namelist(), [])


if __name__ == '__main__':
    unittest.main()
