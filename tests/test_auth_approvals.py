"""Real-socket auth tests on disposable databases, never on deployment accounts."""
import hashlib
import http.client
from http.cookies import SimpleCookie
import json
import os
import sqlite3
import subprocess
import time
import unittest
from test_async_app import BASE, Node


class Browser:
    def __init__(self, node, secure=False, cookies=None, forwarded_ip=None):
        self.node, self.secure, self.cookies = node, secure, dict(cookies or {})
        self.forwarded_ip = forwarded_ip
        self.set_cookies = []

    def call(self, method, path, body=None):
        conn = http.client.HTTPConnection('127.0.0.1', self.node.port, timeout=15)
        headers = {'Cookie': '; '.join(f'{k}={v}' for k, v in self.cookies.items())}
        # Emulate a trusted loopback HTTPS reverse proxy without installing TLS on the test listener.
        if self.secure:
            headers['X-Forwarded-Proto'] = 'https'
        if self.forwarded_ip:
            headers['X-Forwarded-For'] = self.forwarded_ip
        if body is not None:
            headers['Content-Type'] = 'application/json'
            body = json.dumps(body).encode()
        conn.request(method, path, body, headers)
        response = conn.getresponse()
        self.set_cookies = [v for k, v in response.getheaders() if k.lower() == 'set-cookie']
        for value in self.set_cookies:
            parsed = SimpleCookie(); parsed.load(value)
            for key, item in parsed.items():
                if item['max-age'] == '0': self.cookies.pop(key, None)
                else: self.cookies[key] = item.value
        result = response.status, json.loads(response.read() or b'{}')
        conn.close()
        return result

    def login(self, username='admin', password=None):
        return self.call('POST', '/api/login', {'username': username, 'password': password or self.node.initial_password})

    def register(self, username='applicant', trust=False):
        return self.call('POST', '/api/register', {'username': username, 'password': 'new-password',
                         'device_name': 'Personal Chrome', 'trust_device': trust, 'role': 'admin'})


class ApprovalAuthentication(unittest.TestCase):
    def setUp(self):
        self.node = Node()
        self.addCleanup(self.node.close)
        self.admin = Browser(self.node)
        self.assertEqual(self.admin.login()[0], 200)

    def sql(self, query, params=()):
        with sqlite3.connect(self.node.data / 'myfolder.sqlite3') as db:
            return db.execute(query, params).fetchall()

    def review_registration(self, name, decision='approve'):
        status, body = self.admin.call('GET', '/api/admin/registrations')
        self.assertEqual(status, 200)
        item = next(r for r in body['requests'] if r['username'] == name)
        self.assertNotIn('password_hash', item)
        self.assertNotIn('receipt_hash', item)
        self.assertEqual(self.admin.call('POST', '/api/admin/registrations/review', {'id': item['id'], 'decision': decision})[0], 200)
        return item['id']

    def approve_device(self, device_id):
        self.assertEqual(self.admin.call('POST', '/api/admin/trusted-devices/review', {'id': device_id, 'decision': 'approve'})[0], 200)

    def existing_user(self, name='member', role='user'):
        self.assertEqual(self.admin.call('POST', '/api/users', {'username': name, 'password': 'secret123', 'role': role})[0], 201)
        browser = Browser(self.node)
        self.assertEqual(browser.login(name, 'secret123')[0], 200)
        return browser

    def trusted_browser(self, name='member'):
        browser = self.existing_user(name)
        status, device = browser.call('POST', '/api/trusted-devices/request', {'device_name': name + ' browser'})
        self.assertEqual(status, 201)
        self.assertEqual(browser.call('POST', '/api/auth/device-login', {})[0], 401)
        self.approve_device(device['id'])
        self.assertEqual(browser.call('POST', '/api/auth/device-login', {})[0], 200)
        return browser, device['id']

    def test_registration_approval_and_least_privilege(self):
        person = Browser(self.node)
        self.assertEqual(person.register()[0], 201)
        self.assertEqual(person.call('GET', '/api/me')[0], 401)
        self.assertEqual(person.login('applicant', 'new-password')[0], 401)
        self.assertEqual(person.call('GET', '/api/admin/registrations')[0], 401)
        self.assertEqual(person.call('POST', '/api/registration/activate', {})[0], 403)
        request_id = self.review_registration('applicant')
        self.assertEqual(self.admin.call('POST', '/api/admin/registrations/review', {'id': request_id, 'decision': 'approve'})[0], 409)
        receipt = dict(person.cookies)
        status, user = person.call('POST', '/api/registration/activate', {})
        self.assertEqual((status, user['role']), (200, 'user'))
        self.assertEqual(person.call('GET', '/api/list?path=')[0], 403)
        self.assertEqual(Browser(self.node, cookies=receipt).call('POST', '/api/registration/activate', {})[0], 409)
        self.assertEqual(self.sql('SELECT COUNT(*) FROM grants WHERE user_id=?', (user['id'],))[0][0], 0)
        self.assertEqual(self.sql('SELECT password_hash FROM registration_requests WHERE id=?', (request_id,))[0][0], '')

    def test_receipts_duplicate_rejection_and_expiry(self):
        person = Browser(self.node)
        self.assertEqual(person.register('rejected')[0], 201)
        self.assertEqual(Browser(self.node).register('rejected')[0], 409)
        self.assertEqual(Browser(self.node).call('GET', '/api/registration/status')[0], 404)
        self.assertEqual(Browser(self.node, cookies={'mf_registration': 'forged'}).call('POST', '/api/registration/activate', {})[0], 401)
        self.review_registration('rejected', 'reject')
        self.assertEqual(person.call('GET', '/api/registration/status')[1]['status'], 'rejected')
        self.assertEqual(person.call('POST', '/api/registration/activate', {})[0], 403)
        self.assertEqual(person.login('rejected', 'new-password')[0], 401)
        expired = Browser(self.node)
        self.assertEqual(expired.register('expired')[0], 201)
        self.sql('UPDATE registration_requests SET expires_at=? WHERE username=?', (int(time.time()) - 1, 'expired'))
        self.assertEqual(expired.call('GET', '/api/registration/status')[1]['status'], 'expired')
        self.assertEqual(expired.call('POST', '/api/registration/activate', {})[0], 401)
        self.assertEqual(self.sql('SELECT COUNT(*) FROM users WHERE username=?', ('rejected',))[0][0], 0)

    def test_registration_trust_cookie_rotation_and_admin_revocation(self):
        person = Browser(self.node)
        self.assertEqual(person.register('trusted_new', trust=True)[0], 201)
        self.review_registration('trusted_new')
        self.assertEqual(person.call('POST', '/api/registration/activate', {})[0], 200)
        self.assertEqual(len(person.set_cookies), 3)
        self.assertTrue(all('HttpOnly' in c and 'Secure' not in c and 'SameSite=Strict' in c for c in person.set_cookies))
        old_token = person.cookies['mf_device']
        self.assertNotEqual(self.sql('SELECT token_hash FROM trusted_devices')[0][0], old_token)
        restored = Browser(self.node, cookies={'mf_device': old_token})
        self.assertEqual(restored.call('POST', '/api/auth/device-login', {})[0], 200)
        self.assertNotEqual(old_token, restored.cookies['mf_device'])
        self.assertEqual(Browser(self.node, cookies={'mf_device': old_token}).call('POST', '/api/auth/device-login', {})[0], 409)
        self.sql('UPDATE trusted_devices SET previous_until=0')
        self.assertEqual(Browser(self.node, cookies={'mf_device': old_token}).call('POST', '/api/auth/device-login', {})[0], 401)
        device_id = restored.call('GET', '/api/trusted-devices')[1]['devices'][0]['id']
        self.assertEqual(self.admin.call('POST', '/api/trusted-devices/revoke', {'id': device_id})[0], 200)
        self.assertEqual(restored.call('GET', '/api/me')[0], 401)
        self.assertEqual(restored.call('POST', '/api/auth/device-login', {})[0], 401)

    def test_admin_device_review_is_superadmin_only(self):
        manager = self.existing_user('manager', 'admin')
        person = self.existing_user('ordinary')
        device_id = manager.call('POST', '/api/trusted-devices/request', {'device_name': 'Manager browser'})[1]['id']
        self.assertEqual(manager.call('POST', '/api/admin/trusted-devices/review', {'id': device_id, 'decision': 'approve'})[0], 403)
        self.assertEqual(person.call('GET', '/api/admin/trusted-devices')[0], 403)
        self.assertEqual(person.call('POST', '/api/trusted-devices/revoke', {'id': device_id})[0], 403)
        self.assertEqual(manager.call('POST', '/api/trusted-devices/revoke', {'id': device_id})[0], 200)
        device_id = manager.call('POST', '/api/trusted-devices/request', {'device_name': 'New manager browser'})[1]['id']
        self.approve_device(device_id)
        self.assertEqual(manager.call('POST', '/api/auth/device-login', {})[0], 200)

    def test_password_reset_revokes_trust_sessions_and_unclaimed_registration(self):
        browser, device_id = self.trusted_browser()
        self.assertEqual(browser.call('POST', '/api/password-requests', {'username': 'member'})[0], 200)
        requests = self.admin.call('GET', '/api/password-requests')[1]['requests']
        request_id = next(r['id'] for r in requests if r['username'] == 'member')
        self.assertEqual(self.admin.call('POST', '/api/password-requests/review', {'id': request_id, 'decision': 'approve'})[0], 200)
        self.assertEqual(browser.call('GET', '/api/me')[0], 401)
        self.assertEqual(browser.call('POST', '/api/auth/device-login', {})[0], 401)
        self.assertEqual(browser.login('member', 'secret123')[0], 401)
        self.assertEqual(browser.login('member', '123456')[0], 200)
        self.assertEqual(browser.call('POST', '/api/trusted-devices/request', {'device_name': 'Not yet reset'})[0], 403)
        self.assertEqual(browser.call('POST', '/api/password', {'password': 'replacement'})[0], 200)
        self.assertEqual(browser.call('GET', '/api/me')[0], 200)
        self.assertNotIn('mf_device', browser.cookies)
        pending_claim = Browser(self.node)
        self.assertEqual(pending_claim.register('unclaimed')[0], 201)
        self.review_registration('unclaimed')
        pending_claim.call('POST', '/api/password-requests', {'username': 'unclaimed'})
        request_id = next(r['id'] for r in self.admin.call('GET', '/api/password-requests')[1]['requests'] if r['username'] == 'unclaimed')
        self.assertEqual(self.admin.call('POST', '/api/password-requests/review', {'id': request_id, 'decision': 'approve'})[0], 200)
        self.assertEqual(pending_claim.call('POST', '/api/registration/activate', {})[0], 409)

    def test_logout_revokes_only_this_browser_and_expires_all_local_cookies(self):
        first, _ = self.trusted_browser()
        second = Browser(self.node)
        self.assertEqual(second.login('member', 'secret123')[0], 200)
        device_id = second.call('POST', '/api/trusted-devices/request', {'device_name': 'Second browser'})[1]['id']
        self.approve_device(device_id)
        self.assertEqual(second.call('POST', '/api/auth/device-login', {})[0], 200)
        stolen = Browser(self.node, cookies=dict(first.cookies))
        self.assertEqual(first.call('POST', '/api/logout', {})[0], 200)
        self.assertEqual(len(first.set_cookies), 3)
        self.assertEqual(first.cookies, {})
        self.assertEqual(stolen.call('GET', '/api/me')[0], 401)
        self.assertEqual(stolen.call('POST', '/api/auth/device-login', {})[0], 401)
        self.assertEqual(second.call('GET', '/api/me')[0], 200)

    def test_device_absolute_and_idle_expiry(self):
        browser, device_id = self.trusted_browser()
        self.sql('UPDATE trusted_devices SET expires_at=? WHERE id=?', (int(time.time()) - 1, device_id))
        self.assertEqual(browser.call('GET', '/api/me')[0], 401)
        self.assertEqual(browser.call('POST', '/api/auth/device-login', {})[0], 401)
        self.sql('UPDATE trusted_devices SET expires_at=?,last_used_at=? WHERE id=?', (int(time.time()) + 86400, int(time.time()) - 31 * 86400, device_id))
        self.assertEqual(browser.call('POST', '/api/auth/device-login', {})[0], 401)

    def test_http_is_available_by_default_without_override(self):
        plain = Browser(self.node)
        options = plain.call('GET', '/api/auth/options')[1]
        self.assertTrue(options['trusted_login'])
        self.assertFalse(options['secure_transport'])
        self.assertEqual(plain.register('plain_trust', trust=True)[0], 201)
        self.assertTrue(all('Secure' not in cookie for cookie in plain.set_cookies))
        self.review_registration('plain_trust')
        self.assertEqual(plain.call('POST', '/api/registration/activate', {})[0], 200)
        self.assertFalse(plain.call('GET', '/api/trusted-devices')[1]['secure_transport'])
        plain.cookies.pop('session')
        self.assertEqual(plain.call('POST', '/api/auth/device-login', {})[0], 200)

    def test_optional_https_policy_and_secure_cookies(self):
        other = Node(extra_env={'MYFOLDER_REQUIRE_HTTPS_DEVICE_LOGIN': '1'})
        try:
            plain = Browser(other)
            self.assertFalse(plain.call('GET', '/api/auth/options')[1]['trusted_login'])
            self.assertEqual(plain.register('denied_trust', trust=True)[0], 400)
            self.assertEqual(plain.register('plain_user', trust=False)[0], 201)
            self.assertEqual(plain.login()[0], 200)
            self.assertEqual(plain.call('POST', '/api/trusted-devices/request', {'device_name': 'HTTP browser'})[0], 400)
            secure = Browser(other, secure=True)
            options = secure.call('GET', '/api/auth/options')[1]
            self.assertTrue(options['trusted_login'])
            self.assertTrue(options['secure_transport'])
            self.assertEqual(secure.register('https_trust', trust=True)[0], 201)
            self.assertTrue(all('Secure' in cookie for cookie in secure.set_cookies))
            registrations = plain.call('GET', '/api/admin/registrations')[1]['requests']
            request_id = next(r['id'] for r in registrations if r['username'] == 'https_trust')
            self.assertEqual(plain.call('POST', '/api/admin/registrations/review', {'id': request_id, 'decision': 'approve'})[0], 200)
            receipt = dict(secure.cookies)
            self.assertEqual(Browser(other, cookies=receipt).call('POST', '/api/registration/activate', {})[0], 400)
            self.assertEqual(secure.call('POST', '/api/registration/activate', {})[0], 200)
            self.assertTrue(all('HttpOnly' in c and 'Secure' in c and 'SameSite=Strict' in c for c in secure.set_cookies))
            self.assertEqual(Browser(other, cookies=dict(secure.cookies)).call('POST', '/api/auth/device-login', {})[0], 400)
            self.assertEqual(secure.call('POST', '/api/auth/device-login', {})[0], 200)
        finally:
            other.close()

    def test_untrusted_forwarded_header_cannot_claim_https(self):
        for strict in ('0', '1'):
            with self.subTest(require_https=strict):
                other = Node(extra_env={'MYFOLDER_TRUSTED_PROXY_IPS': '192.0.2.1',
                                       'MYFOLDER_REQUIRE_HTTPS_DEVICE_LOGIN': strict})
                try:
                    forged = Browser(other, secure=True)
                    options = forged.call('GET', '/api/auth/options')[1]
                    self.assertFalse(options['secure_transport'])
                    self.assertEqual(options['trusted_login'], strict == '0')
                    self.assertEqual(forged.register('forged', trust=True)[0], 201 if strict == '0' else 400)
                    self.assertEqual(forged.register('plain_user', trust=False)[0], 201)
                    self.assertTrue(all('Secure' not in cookie for cookie in forged.set_cookies))
                finally:
                    other.close()

    def test_registration_rate_limit(self):
        for i in range(5):
            self.assertEqual(Browser(self.node).register('candidate' + str(i))[0], 201)
        self.assertEqual(Browser(self.node).register('overflow')[0], 429)

    def test_trusted_proxy_limits_actual_peer_not_forged_first_hop(self):
        for i in range(5):
            caller = Browser(self.node, secure=True, forwarded_ip='198.51.100.' + str(i) + ', 192.0.2.10')
            self.assertEqual(caller.register('proxied' + str(i))[0], 201)
        self.assertEqual(Browser(self.node, secure=True, forwarded_ip='198.51.100.99, 192.0.2.10').register('over_limit')[0], 429)
        self.assertEqual(Browser(self.node, secure=True, forwarded_ip='192.0.2.11').register('other_peer')[0], 201)

    def test_expired_session_can_still_log_out_and_forget_trust(self):
        browser, device_id = self.trusted_browser()
        token = browser.cookies['mf_device']
        self.sql('UPDATE sessions SET expires_at=0 WHERE device_id=?', (device_id,))
        self.assertEqual(browser.call('POST', '/api/logout', {})[0], 200)
        self.assertEqual(browser.cookies, {})
        self.assertEqual(Browser(self.node, cookies={'mf_device': token}).call('POST', '/api/auth/device-login', {})[0], 401)

    def test_existing_database_migration_keeps_users_and_sessions(self):
        self.node.process.terminate(); self.node.process.wait(timeout=5); self.node.process.stderr.close()
        rows = self.sql('SELECT token_hash,user_id,expires_at FROM sessions')
        with sqlite3.connect(self.node.data / 'myfolder.sqlite3') as db:
            db.execute('DROP TABLE sessions')
            db.execute('DROP TABLE registration_requests')
            db.execute('DROP TABLE trusted_devices')
            db.execute('CREATE TABLE sessions(token_hash TEXT PRIMARY KEY,user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,expires_at INTEGER NOT NULL)')
            db.executemany('INSERT INTO sessions VALUES(?,?,?)', rows)
        env = dict(os.environ, MYFOLDER_DATA=str(self.node.data), MYFOLDER_HOST='127.0.0.1',
                   MYFOLDER_PORT=str(self.node.port), MYFOLDER_WEB=str(BASE / 'web' / 'dist'))
        self.node.process = subprocess.Popen([os.environ['MYFOLDER_CPP_BINARY']], cwd=BASE, env=env,
                                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            try:
                if self.node.request('GET', '/')[0] == 200: break
            except OSError: pass
            time.sleep(.05)
        self.assertEqual(self.admin.call('GET', '/api/me')[0], 200)
        self.assertEqual(self.admin.login()[0], 200)
        self.assertIn('device_id', [row[1] for row in self.sql('PRAGMA table_info(sessions)')])


if __name__ == '__main__':
    unittest.main()
