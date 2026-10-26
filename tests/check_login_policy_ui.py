"""Policy-management UI + real API, disposable loopback fixture only."""
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
    output = Path(__file__).resolve().parents[1] / '.build' / 'login-policy-ui'
    output.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=args.chrome, headless=True)
        owner = browser.new_context(viewport={'width': 390, 'height': 844}, has_touch=True, is_mobile=True)
        visitor = browser.new_context(viewport={'width': 390, 'height': 844})
        manager = browser.new_context(viewport={'width': 1280, 'height': 900})
        errors = []
        page = owner.new_page()
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(args.url)
        page.locator('#acct').fill('admin')
        page.locator('#pwd').fill('Temporary-test-password-1')
        page.get_by_role('button', name='登录', exact=True).click()
        expect(page.locator('.me-name')).to_have_text('admin')
        original = owner.request.get(args.url + '/api/admin/login-policy').json()['device_days']
        try:
            page.get_by_role('button', name='打开导航菜单').click()
            page.locator('.nav-item').filter(has_text='用户与权限').click()
            panel = page.locator('section[aria-label="浏览器免密设置"]')
            expect(panel.get_by_text(f'当前免密有效期：{original} 天')).to_be_visible()
            panel.locator('#login-lifetime-days').fill('7')
            panel.get_by_role('button', name='保存免密设置', exact=True).click()
            dialog = page.get_by_role('dialog', name='确认修改免密有效期')
            expect(dialog).to_be_visible()
            assert page.evaluate('document.documentElement.scrollWidth <= innerWidth + 1')
            dialog.get_by_role('button', name='取消', exact=True).click()
            assert owner.request.get(args.url + '/api/admin/login-policy').json()['device_days'] == original
            panel.get_by_role('button', name='保存免密设置', exact=True).click()
            dialog.get_by_role('button', name='确认保存', exact=True).click()
            expect(panel.get_by_text('当前免密有效期：7 天', exact=True)).to_be_visible()
            guest_page = visitor.new_page()
            guest_page.goto(args.url)
            expect(guest_page.get_by_role('checkbox', name='记住此浏览器（7 天）', exact=True)).to_be_checked()
            panel.locator('#login-lifetime-mode').click()
            page.get_by_role('option', name='长期有效（直到撤销）', exact=True).click()
            panel.get_by_role('button', name='保存免密设置', exact=True).click()
            dialog.get_by_role('button', name='确认保存', exact=True).click()
            expect(panel.get_by_text('当前免密有效期：长期有效', exact=True)).to_be_visible()
            assert owner.request.get(args.url + '/api/auth/options').json()['device_days'] == 0
            page.screenshot(path=str(output / 'unlimited-mobile.png'), full_page=True)
            guest_page.reload()
            expect(guest_page.get_by_role('checkbox', name='长期记住此浏览器', exact=True)).to_be_checked()
            guest_page.get_by_role('button', name='申请注册', exact=True).click()
            expect(guest_page.get_by_role('checkbox', name='审核通过后长期记住此浏览器', exact=True)).to_be_checked()
            guest_page.screenshot(path=str(output / 'unlimited-registration-mobile.png'), full_page=True)
            name = 'policy_admin_' + uuid.uuid4().hex[:8]
            assert owner.request.post(args.url + '/api/users', data={'username': name, 'password': 'Disposable-policy-password', 'role': 'admin'}).status == 201
            manager_page = manager.new_page()
            manager_page.on('pageerror', lambda error: errors.append(str(error)))
            manager_page.goto(args.url)
            manager_page.locator('#acct').fill(name)
            manager_page.locator('#pwd').fill('Disposable-policy-password')
            manager_page.get_by_role('button', name='登录', exact=True).click()
            expect(manager_page.locator('.me-name')).to_have_text(name)
            manager_page.locator('.nav-item').filter(has_text='用户与权限').click()
            expect(manager_page.get_by_text('当前免密有效期：长期有效', exact=True)).to_be_visible()
            expect(manager_page.get_by_role('button', name='保存免密设置', exact=True)).to_have_count(0)
            assert manager.request.post(args.url + '/api/admin/login-policy', data={'device_days': 1}).status == 403
            manager_page.locator('.nav-item').filter(has_text='登录设备').click()
            expect(manager_page.get_by_text('长期有效（直到撤销）', exact=True)).to_be_visible()
            manager_page.screenshot(path=str(output / 'unlimited-devices-desktop.png'), full_page=True)
            assert not errors, errors
        finally:
            assert owner.request.post(args.url + '/api/admin/login-policy', data={'device_days': original}).status == 200
            browser.close()
    print(json.dumps({'policy_ui_and_real_api': 'passed', 'finite_days': 7, 'unlimited': True,
                      'confirmation_cancel': True, 'admin_read_only': True, 'viewports': [390, 1280]}))


if __name__ == '__main__':
    main()
