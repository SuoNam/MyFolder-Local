"""Real browser-process restart checks against serve_auth_fixture.py ONLY.

Uses disposable persistent Chromium profiles, not exported/reinjected cookies or live user profiles.
The fixture's public test password must never be substituted with production credentials.
"""
import argparse
from pathlib import Path
import tempfile
import time
from urllib.parse import urlsplit
import uuid
import json
from playwright.sync_api import expect, sync_playwright


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', default='http://127.0.0.1:4180')
    parser.add_argument('--chrome', default=r'C:\Users\Lenovo\AppData\Local\ms-playwright\chromium-1208\chrome-win64\chrome.exe')
    args = parser.parse_args()
    assert urlsplit(args.url).hostname in ('127.0.0.1', 'localhost'), 'Use a disposable loopback fixture only'
    artifacts = Path(__file__).resolve().parents[1] / '.build' / 'auth-restart'
    artifacts.mkdir(parents=True, exist_ok=True)
    name = 'restart_' + uuid.uuid4().hex[:10]
    password = 'Disposable-browser-password-1'
    with tempfile.TemporaryDirectory(prefix='profile-', dir=artifacts) as directory, sync_playwright() as pw:
        admin_browser = pw.chromium.launch(executable_path=args.chrome, headless=True)
        admin = admin_browser.new_context()
        assert admin.request.post(args.url + '/api/login', data={'username': 'admin', 'password': 'Temporary-test-password-1'}).status == 200
        active = []
        errors = []

        def launch(profile):
            context = pw.chromium.launch_persistent_context(str(Path(directory) / profile), executable_path=args.chrome,
                                                           headless=True, viewport={'width': 1280, 'height': 900})
            active.append(context)
            page = context.pages[0]
            page.on('pageerror', lambda error: errors.append(str(error)))
            return context, page

        def close(context):
            context.close()  # Persistent-context close terminates its Chromium process.
            active.remove(context)

        def submit_login(page):
            page.locator('#acct').fill(name)
            page.locator('#pwd').fill(password)
            page.get_by_role('button', name='登录', exact=True).click()
            expect(page.locator('.me-name')).to_have_text(name)

        try:
            owner, page = launch('approved')
            # Let the ordinary login page finish before creating/approving an application.
            # This naturally exercises manual password login without mocking the automatic-activation API.
            with page.expect_response(lambda r: r.url.endswith('/api/registration/status')):
                page.goto(args.url)
            assert owner.request.post(args.url + '/api/register', data={
                'username': name, 'password': password, 'device_name': 'Disposable restart browser', 'trust_device': True}).status == 201
            rows = admin.request.get(args.url + '/api/admin/registrations').json()['requests']
            request_id = next(row['id'] for row in rows if row['username'] == name)
            assert admin.request.post(args.url + '/api/admin/registrations/review', data={'id': request_id, 'decision': 'approve'}).status == 200
            submit_login(page)
            expect(page.locator('.lan-login-status')).to_contain_text('此浏览器免密已开启')
            cookies = {cookie['name']: cookie for cookie in owner.cookies()}
            assert cookies['mf_device']['expires'] > time.time() + 89 * 86400
            assert cookies['mf_device']['httpOnly'] and not cookies['mf_device']['secure']
            assert 'mf_registration' not in cookies
            token = cookies['mf_device']['value']
            page.screenshot(path=str(artifacts / 'manual-login-approved.png'), full_page=True)
            close(owner)

            for cycle in (1, 2):
                owner, page = launch('approved')
                names = {cookie['name'] for cookie in owner.cookies()}
                assert 'mf_device' in names and 'session' not in names, 'Test must restore from persistent proof, not a saved session'
                with page.expect_response(lambda r: r.url.endswith('/api/auth/device-login') and r.status == 200):
                    page.goto(args.url)
                expect(page.locator('.me-name')).to_have_text(name)
                rotated = {cookie['name']: cookie for cookie in owner.cookies()}['mf_device']['value']
                assert rotated != token, 'Automatic login did not rotate its persistent credential'
                token = rotated
                page.screenshot(path=str(artifacts / f'reopened-{cycle}.png'), full_page=True)
                if cycle == 1:
                    close(owner)

            # Account approval cannot silently approve a different browser holding only the password.
            untrusted, other_page = launch('unapproved')
            other_page.goto(args.url)
            expect(other_page.get_by_role('heading', name='登录 MyFolder', exact=True)).to_be_visible()
            submit_login(other_page)
            expect(other_page.locator('.lan-login-status')).to_contain_text('此浏览器尚未开启免密')
            assert 'mf_device' not in {cookie['name'] for cookie in untrusted.cookies()}
            other_page.screenshot(path=str(artifacts / 'temporary-login-warning.png'), full_page=True)
            close(untrusted)
            untrusted, other_page = launch('unapproved')
            other_page.goto(args.url)
            expect(other_page.get_by_role('heading', name='登录 MyFolder', exact=True)).to_be_visible()
            # Existing accounts without the original registration receipt use normal device approval.
            submit_login(other_page)
            other_page.get_by_role('button', name='查看免密状态', exact=True).click()
            other_page.locator('#trusted-device-name').fill('Second persistent browser')
            other_page.get_by_role('button', name='申请免密登录', exact=True).click()
            expect(other_page.locator('[aria-label="当前浏览器免密状态"]')).to_contain_text('仍待管理员批准')
            devices = admin.request.get(args.url + '/api/admin/trusted-devices').json()['devices']
            device_id = next(device['id'] for device in devices if device['username'] == name and device['name'] == 'Second persistent browser')
            assert admin.request.post(args.url + '/api/admin/trusted-devices/review', data={'id': device_id, 'decision': 'approve'}).status == 200
            other_page.get_by_role('button', name='刷新', exact=True).click()
            expect(other_page.locator('[aria-label="当前浏览器免密状态"]')).to_contain_text('此浏览器免密已开启')
            other_page.locator('.nav-item').filter(has_text='文件区').click()
            expect(other_page.locator('.lan-login-status')).to_contain_text('此浏览器免密已开启')
            close(untrusted)
            untrusted, other_page = launch('unapproved')
            with other_page.expect_response(lambda r: r.url.endswith('/api/auth/device-login') and r.status == 200):
                other_page.goto(args.url)
            expect(other_page.locator('.me-name')).to_have_text(name)
            other_page.get_by_role('button', name='退出登录', exact=True).click()
            expect(other_page.get_by_role('heading', name='登录 MyFolder', exact=True)).to_be_visible()
            close(untrusted)

            page.get_by_role('button', name='退出登录', exact=True).click()
            expect(page.get_by_role('heading', name='登录 MyFolder', exact=True)).to_be_visible()
            assert 'mf_device' not in {cookie['name'] for cookie in owner.cookies()}
            close(owner)
            owner, page = launch('approved')
            page.goto(args.url)
            expect(page.get_by_role('heading', name='登录 MyFolder', exact=True)).to_be_visible()
            assert 'mf_device' not in {cookie['name'] for cookie in owner.cookies()}
            assert not errors, errors
        finally:
            for context in reversed(active):
                context.close()
            admin_browser.close()
    print(json.dumps({'manual_login_claim': 'passed', 'real_browser_restart_cycles': 2,
                      'automatic_login_and_rotation': True, 'unapproved_browser_stays_temporary': True,
                      'existing_account_device_approval_restart': True,
                      'explicit_logout_stays_logged_out': True}))


if __name__ == '__main__':
    main()
