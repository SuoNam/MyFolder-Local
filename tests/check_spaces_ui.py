"""Private/public spaces with real users and real API on a disposable fixture only."""
import argparse
import json
from pathlib import Path
from urllib.parse import urlsplit
import uuid
from playwright.sync_api import expect, sync_playwright


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', default='http://127.0.0.1:4180')
    parser.add_argument('--chrome', default=r'C:\Users\Lenovo\AppData\Local\ms-playwright\chromium-1208\chrome-win64\chrome.exe')
    args = parser.parse_args()
    assert urlsplit(args.url).hostname in ('127.0.0.1', 'localhost'), 'Disposable loopback fixture only'
    output = Path(__file__).resolve().parents[1] / '.build' / 'spaces-ui'
    output.mkdir(parents=True, exist_ok=True)
    prefix = uuid.uuid4().hex[:8]
    password = 'Disposable-spaces-password'
    with sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=args.chrome, headless=True)
        admin = browser.new_context(viewport={'width': 390, 'height': 844}, has_touch=True, is_mobile=True)
        alice = browser.new_context(viewport={'width': 390, 'height': 844}, has_touch=True, is_mobile=True)
        bob = browser.new_context(viewport={'width': 1280, 'height': 900})
        errors = []

        def login(context, name, secret):
            page = context.new_page()
            page.on('pageerror', lambda e: errors.append(str(e)))
            page.goto(args.url)
            page.locator('#acct').fill(name)
            page.locator('#pwd').fill(secret)
            page.get_by_role('button', name='登录', exact=True).click()
            expect(page.locator('.me-name')).to_have_text(name)
            expect(page.get_by_role('tab', name='我的文件', exact=True)).to_have_attribute('aria-selected', 'true')
            return page

        def put(page, name, content):
            expect(page.locator('.lan-progress')).to_have_count(0)
            with page.expect_response(lambda response: '/upload?' in response.url and response.request.method == 'PUT') as pending:
                page.locator('input[type=file]').first.set_input_files({'name': name, 'mimeType': 'text/plain', 'buffer': content})
            return pending.value

        admin_page = login(admin, 'admin', 'Temporary-test-password-1')
        for name in ('alice_' + prefix, 'bob_' + prefix):
            assert admin.request.post(args.url + '/api/users', data={'username': name, 'password': password, 'role': 'user'}).status == 201
        alice_page = login(alice, 'alice_' + prefix, password)
        bob_page = login(bob, 'bob_' + prefix, password)
        private_name = 'private-' + prefix + '.txt'
        result = put(alice_page, private_name, b'secret')
        alice_page.screenshot(path=str(output / 'first-upload.png'), full_page=True)
        assert result.status == 201, (result.status, result.text())
        expect(alice_page.get_by_role('status').filter(has_text='已上传 1 个文件')).to_be_visible()
        expect(alice_page.get_by_role('button', name=private_name, exact=True)).to_be_visible()
        expect(alice_page.get_by_label('个人空间用量')).to_contain_text('6 B / 5.00 GB')
        expect(bob_page.get_by_role('button', name=private_name, exact=True)).to_have_count(0)
        assert bob.request.get(args.url + '/api/spaces/personal/download?path=' + private_name).status == 404
        bob_page.get_by_role('tab', name='公共区', exact=True).click()
        expect(bob_page.get_by_role('button', name='上传文件', exact=True)).to_be_disabled()
        public_name = 'team-' + prefix + '.txt'
        assert admin.request.put(args.url + '/api/spaces/public/upload?path=' + public_name, data=b'public').status == 201
        bob_page.get_by_role('button', name='刷新', exact=True).click()
        expect(bob_page.get_by_role('button', name=public_name, exact=True)).to_be_visible()
        admin_page.get_by_role('button', name='打开导航菜单').click()
        admin_page.locator('.nav-item').filter(has_text='用户与权限').click()
        panel = admin_page.locator('section[aria-label="个人配额与公共区权限"]')
        panel.locator('#storage-user').click()
        admin_page.get_by_role('option', name='alice_' + prefix, exact=True).click()
        panel.locator('#personal-quota').fill('0.00000001')  # Rounded to 11 bytes.
        panel.get_by_role('checkbox', name='上传 / 新建', exact=True).check()
        # Refresh uses the real API but must not overwrite the unsaved quota/permission draft.
        with admin_page.expect_response(lambda response: response.url.endswith('/api/admin/storage') and response.request.method == 'GET'):
            admin_page.get_by_role('button', name='刷新', exact=True).click()
        expect(panel.locator('#personal-quota')).to_have_js_property('valueAsNumber', 0.00000001)
        expect(panel.get_by_role('checkbox', name='上传 / 新建', exact=True)).to_be_checked()
        with admin_page.expect_request(lambda request: request.url.endswith('/api/admin/storage') and request.method == 'POST') as pending_save:
            panel.get_by_role('button', name='保存空间配置', exact=True).click()
        saved = pending_save.value.post_data_json
        assert saved['storage_limit'] == 11 and saved['public_upload'] is True, saved
        expect(panel.get_by_text('个人配额和公共区权限已保存', exact=True)).to_be_visible()
        actual = next(row for row in admin.request.get(args.url + '/api/admin/storage').json()['users'] if row['username'] == 'alice_' + prefix)
        assert actual['storage_limit'] == 11 and actual['public_upload'] is True, actual
        alice_page.get_by_role('button', name='刷新', exact=True).click()
        expect(alice_page.get_by_label('个人空间用量')).to_contain_text('6 B / 11 B')
        put(alice_page, 'too-large.txt', b'123456')
        expect(alice_page.get_by_role('alert')).to_contain_text('个人空间容量不足')
        alice_page.get_by_role('tab', name='公共区', exact=True).click()
        expect(alice_page.get_by_role('button', name='上传文件', exact=True)).to_be_enabled()
        published = 'alice-public-' + prefix + '.txt'
        put(alice_page, published, b'shared')
        expect(alice_page.get_by_role('status').filter(has_text='已上传 1 个文件')).to_be_visible()
        put(alice_page, published, b'changed')
        expect(alice_page.get_by_role('alert')).to_contain_text('修改权限')
        bob_page.get_by_role('button', name='刷新', exact=True).click()
        expect(bob_page.get_by_role('button', name=published, exact=True)).to_be_visible()
        expect(bob_page.get_by_role('button', name='移动或重命名 ' + published, exact=True)).to_have_count(0)
        for page in (alice_page, bob_page, admin_page):
            assert page.evaluate('document.documentElement.scrollWidth <= innerWidth + 1')
        alice_page.screenshot(path=str(output / 'public-mobile.png'), full_page=True)
        bob_page.screenshot(path=str(output / 'public-desktop.png'), full_page=True)
        admin_page.screenshot(path=str(output / 'quota-permissions-mobile.png'), full_page=True)
        alice_page.get_by_role('tab', name='我的文件', exact=True).click()
        expect(alice_page.get_by_role('button', name=private_name, exact=True)).to_be_visible()
        alice_page.screenshot(path=str(output / 'private-mobile.png'), full_page=True)
        assert not errors, errors
        browser.close()
    print(json.dumps({'real_spaces_ui': 'passed', 'private_isolation': True, 'public_read_only_default': True,
                      'admin_grants_upload': True, 'quota_rejection': True, 'viewports': [390, 1280]}))


if __name__ == '__main__':
    main()
