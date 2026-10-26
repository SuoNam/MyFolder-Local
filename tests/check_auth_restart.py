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
    parser.add_argument('--device-days', type=int, default=90)
    args = parser.parse_args()
    assert urlsplit(args.url).hostname in ('127.0.0.1', 'localhost'), 'Use a disposable loopback fixture only'
    assert 0 <= args.device_days <= 3650
    artifacts = Path(__file__).resolve().parents[1] / '.build' / 'auth-restart'
    artifacts.mkdir(parents=True, exist_ok=True)
    name = 'restart_' + uuid.uuid4().hex[:10]
    password = 'Disposable-browser-password-1'
    with tempfile.TemporaryDirectory(prefix='profile-', dir=artifacts) as directory, sync_playwright() as pw:
        admin_browser = pw.chromium.launch(executable_path=args.chrome, headless=True)
        admin = admin_browser.new_context()
        assert admin.request.post(args.url + '/api/login', data={'username': 'admin', 'password': 'Temporary-test-password-1'}).status == 200
        original_days = admin.request.get(args.url + '/api/admin/login-policy').json()['device_days']
        assert admin.request.post(args.url + '/api/admin/login-policy', data={'device_days': args.device_days}).status == 200
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

        def submit_login(page, remember=True):
            page.locator('#acct').fill(name)
            page.locator('#pwd').fill(password)
            label = '长期记住此浏览器' if args.device_days == 0 else f'记住此浏览器（{args.device_days} 天）'
            checkbox = page.get_by_role('checkbox', name=label, exact=True)
            expect(checkbox).to_be_checked()
            if not remember:
                checkbox.uncheck()
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
            cookie_days = min(args.device_days or 400, 400)
            assert cookies['mf_device']['expires'] > time.time() + (cookie_days - 1) * 86400
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

            # The approved account can remember a clean browser using only its correct password.
            untrusted, other_page = launch('unapproved')
            other_page.goto(args.url)
            expect(other_page.get_by_role('heading', name='登录 MyFolder', exact=True)).to_be_visible()
            submit_login(other_page)
            expect(other_page.locator('.lan-login-status')).to_contain_text('此浏览器免密已开启')
            assert 'mf_device' in {cookie['name'] for cookie in untrusted.cookies()}
            other_page.screenshot(path=str(artifacts / 'clean-browser-default-remember.png'), full_page=True)
            close(untrusted)
            untrusted, other_page = launch('unapproved')
            with other_page.expect_response(lambda r: r.url.endswith('/api/auth/device-login') and r.status == 200):
                other_page.goto(args.url)
            expect(other_page.locator('.me-name')).to_have_text(name)
            other_page.get_by_role('button', name='退出登录', exact=True).click()
            expect(other_page.get_by_role('heading', name='登录 MyFolder', exact=True)).to_be_visible()
            close(untrusted)

            # Explicit opt-out is the only normal password-login path that does not keep a persistent proof.
            public, public_page = launch('public-computer')
            public_page.goto(args.url)
            expect(public_page.get_by_role('heading', name='登录 MyFolder', exact=True)).to_be_visible()
            submit_login(public_page, remember=False)
            expect(public_page.locator('.lan-login-status')).to_contain_text('本次未记住浏览器')
            assert 'mf_device' not in {cookie['name'] for cookie in public.cookies()}
            close(public)
            public, public_page = launch('public-computer')
            public_page.goto(args.url)
            expect(public_page.get_by_role('heading', name='登录 MyFolder', exact=True)).to_be_visible()
            close(public)

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
            assert admin.request.post(args.url + '/api/admin/login-policy', data={'device_days': original_days}).status == 200
            admin_browser.close()
    print(json.dumps({'manual_login_claim': 'passed', 'real_browser_restart_cycles': 2,
                      'configured_device_days': args.device_days,
                      'automatic_login_and_rotation': True, 'clean_browser_password_login_remembers': True,
                      'no_registration_receipt_or_device_approval': True, 'explicit_opt_out_stays_temporary': True,
                      'explicit_logout_stays_logged_out': True}))


if __name__ == '__main__':
    main()
