"""Real browser + real API test against serve_auth_fixture.py ONLY.

Run a disposable fixture, or forward its loopback port using authorized SSH.
No fixture JSON is substituted, apart from one deliberately injected 401 to check silent renewal.
"""
import argparse
import json
from pathlib import Path
from playwright.sync_api import expect, sync_playwright


def navigate(page, name, mobile=False):
    if mobile:
        page.get_by_role('button', name='打开导航菜单').click()
    page.locator('.nav-item').filter(has_text=name).click()
    if mobile:
        expect(page.locator('#main-navigation')).to_be_hidden()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', default='http://127.0.0.1:4180')
    parser.add_argument('--chrome', default=r'C:\Users\Lenovo\AppData\Local\ms-playwright\chromium-1208\chrome-win64\chrome.exe')
    args = parser.parse_args()
    artifacts = Path(__file__).resolve().parents[1] / '.build' / 'auth-ui'
    artifacts.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=args.chrome, headless=True)
        visitor_context = browser.new_context(viewport={'width': 390, 'height': 844}, has_touch=True, is_mobile=True, locale='zh-CN')
        admin_context = browser.new_context(viewport={'width': 1280, 'height': 900}, locale='zh-CN')
        visitor, admin = visitor_context.new_page(), admin_context.new_page()
        errors = []
        for page in (visitor, admin):
            page.on('pageerror', lambda error: errors.append(str(error)))
            page.goto(args.url)
        admin.locator('#acct').fill('admin')
        admin.locator('#pwd').fill('Temporary-test-password-1')
        admin.get_by_role('button', name='登录', exact=True).click()
        expect(admin.locator('.me-name')).to_have_text('admin')
        visitor.get_by_role('button', name='申请注册', exact=True).click()
        expect(visitor.get_by_text('当前使用 HTTP，通信未加密；请仅在可信内网使用，HTTPS 可选。', exact=True)).to_be_visible()
        expect(visitor.get_by_role('checkbox', name='审核通过后记住此浏览器（90 天）')).to_be_checked()
        visitor.locator('#acct').fill('browser_applicant')
        visitor.locator('#pwd').fill('Browser-password-1')
        visitor.locator('#confirm-password').fill('Browser-password-1')
        visitor.locator('#registration-device-name').fill('移动端测试浏览器')
        visitor.get_by_role('checkbox', name='审核通过后记住此浏览器（90 天）').check()
        visitor.get_by_role('button', name='提交注册申请').click()
        expect(visitor.get_by_text('browser_applicant · 等待管理员审核')).to_be_visible()
        navigate(admin, '用户与权限')
        row = admin.locator('section').filter(has=admin.get_by_role('heading', name='注册申请审核', exact=True)).locator('tr').filter(has_text='browser_applicant')
        row.get_by_role('button', name='通过注册', exact=True).click()
        expect(row).to_contain_text('已通过')
        visitor.get_by_role('button', name='检查审核结果').click()
        expect(visitor.locator('.me-name')).to_have_text('browser_applicant')
        cookies = {cookie['name']: cookie for cookie in visitor_context.cookies()}
        assert cookies['mf_device']['httpOnly'] and cookies['mf_device']['sameSite'] == 'Strict'
        assert not cookies['mf_device']['secure'], 'Default HTTP mode unexpectedly required a Secure cookie'
        token = cookies['mf_device']['value']
        visitor_context.clear_cookies(name='session')
        visitor.reload()
        expect(visitor.locator('.me-name')).to_have_text('browser_applicant')
        rotated = {c['name']: c['value'] for c in visitor_context.cookies()}['mf_device']
        assert token != rotated, 'Persistent credential did not rotate on automatic login'
        # One API 401 must transparently renew and retry without showing the login form.
        injected = {'count': 0}
        def once_unauthorized(route):
            injected['count'] += 1
            if injected['count'] == 1:
                route.fulfill(status=401, content_type='application/json', body='{"error":"test session expiry"}')
            else:
                route.continue_()
        visitor.route('**/api/server-info', once_unauthorized)
        navigate(visitor, '服务器信息', mobile=True)
        expect(visitor.get_by_role('heading', name='服务器信息', exact=True)).to_be_visible()
        expect(visitor.get_by_text('服务器内部错误', exact=True)).to_have_count(0)
        expect(visitor.locator('.me-name')).to_have_text('browser_applicant')
        visitor.wait_for_function("document.querySelector('.lan-info-list') !== null")
        assert injected['count'] >= 2, '401 did not trigger a retry'
        visitor.unroute('**/api/server-info', once_unauthorized)
        navigate(visitor, '登录设备', mobile=True)
        expect(visitor.get_by_text('当前使用 HTTP，通信未加密；请仅在可信内网使用，HTTPS 可选。', exact=True)).to_be_visible()
        expect(visitor.get_by_text('此浏览器', exact=True)).to_be_visible()
        visitor.screenshot(path=str(artifacts / 'trusted-mobile.png'), full_page=True)
        admin.get_by_role('button', name='刷新审核列表').click()
        panel = admin.locator('section').filter(has=admin.get_by_role('heading', name='浏览器登录管理', exact=True))
        device_row = panel.locator('tr').filter(has_text='browser_applicant')
        device_row.get_by_role('button', name='撤销免密', exact=True).click()
        admin.get_by_role('button', name='确认撤销', exact=True).click()
        expect(device_row).to_contain_text('已撤销')
        visitor.reload()
        expect(visitor.get_by_role('heading', name='登录 MyFolder', exact=True)).to_be_visible()
        visitor.locator('#acct').fill('browser_applicant')
        visitor.locator('#pwd').fill('Browser-password-1')
        visitor.get_by_role('button', name='登录', exact=True).click()
        expect(visitor.locator('.me-name')).to_have_text('browser_applicant')
        navigate(visitor, '登录设备', mobile=True)
        visitor.locator('#trusted-device-name').fill('重新申请的浏览器')
        expect(visitor.locator('[aria-label="当前浏览器免密状态"]')).to_contain_text('此浏览器免密已开启')
        visitor.get_by_role('button', name='保存名称并记住', exact=True).click()
        expect(visitor.get_by_text('此浏览器已记住，90 天，无需管理员再次审批')).to_be_visible()
        admin.get_by_role('button', name='刷新审核列表').click()
        expect(panel.locator('tr').filter(has_text='重新申请的浏览器')).to_contain_text('已通过')
        navigate(visitor, '登录设备', mobile=True)
        visitor.get_by_role('button', name='刷新', exact=True).click()
        expect(visitor.locator('tr').filter(has_text='重新申请的浏览器')).to_contain_text('已通过')
        visitor.get_by_role('button', name='打开导航菜单').click()
        visitor.get_by_role('button', name='退出登录', exact=True).click()
        visitor.reload()
        expect(visitor.get_by_role('heading', name='登录 MyFolder', exact=True)).to_be_visible()
        assert 'mf_device' not in {c['name'] for c in visitor_context.cookies()}, 'Logout retained the device credential'
        visitor.screenshot(path=str(artifacts / 'signed-out-mobile.png'), full_page=True)
        assert not errors, errors
        browser.close()
        print(json.dumps({'real_browser_real_api': 'passed', 'registration_approval': True, 'automatic_login_and_rotation': True,
                          'silent_session_renewal': True, 'admin_revocation': True, 'reapply_and_logout': True}))


if __name__ == '__main__':
    main()
