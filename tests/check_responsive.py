"""Browser layout checks with disposable API fixtures; never uses deployment accounts.

Build web/dist, serve it locally, then run this script with --url.
Requires the optional Playwright Python package and Chromium.
"""

import argparse
import json
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from playwright.sync_api import expect, sync_playwright

ROOT = Path(__file__).resolve().parents[1]
PERMISSIONS = ["upload", "download", "modify", "delete"]
LONG_FILE = "项目交付与共享资料_" + "long-filename-" * 12 + ".pdf"
LONG_FOLDER = "共享项目与完整测试记录_" + "folder-" * 12


def fixture(route):
    request = route.request
    parsed = urlsplit(request.url)
    endpoint = parsed.path
    user = {"id": 1, "username": "演示管理员", "role": "superadmin", "reset_until": 0, "browser_login": "approved"}
    info = {"name": "MyFolder 移动端布局验证服务器", "server_id": "fixture-server",
            "storage_path": "/var/lib/myfolder-lan/storage/" + "shared-project/" * 10,
            "storage_limit": 5 * 1024**3, "storage_used": 1024**3, "file_count": 42,
            "remotes": [{"id": 7, "remote_name": "外连服务器_" + "remote-" * 12, "status": "approved"}]}
    bodies = {
        "/api/auth/options": {"registration": True, "trusted_login": True, "secure_transport": False, "device_days": 90},
        "/api/trusted-devices": {"devices": [{"id": 1, "user_id": 1, "name": "个人工作电脑的浏览器_" + "browser-" * 10, "status": "approved", "created_at": 1700000000, "expires_at": 1900000000, "last_used_at": 1700000000, "reviewed_at": 1700000000, "current": True}], "trusted_login": True, "secure_transport": False},
        "/api/admin/registrations": {"requests": [{"id": 1, "username": "new_member", "device_name": "申请设备_" + "device-" * 12, "trust_device": 1, "status": "pending", "created_at": 1700000000, "expires_at": 1900000000, "reviewed_at": None}]},
        "/api/admin/trusted-devices": {"devices": [{"id": 1, "user_id": 2, "username": "测试成员", "name": "待审核设备_" + "device-" * 12, "status": "pending", "created_at": 1700000000, "expires_at": 1900000000, "last_used_at": 1700000000, "reviewed_at": None}]},
        "/api/me": user, "/api/login": user, "/api/server-info": info,
        "/api/users": {"users": [user, {"id": 2, "username": "测试成员", "role": "user", "reset_until": 0}]},
        "/api/grants": {"grants": [{"id": 1, "user_id": 2, "path": LONG_FOLDER,
                                     "upload": 1, "download": 1, "modify": 1, "delete_allowed": 1}]},
        "/api/password-requests": {"requests": [{"id": 1, "username": "测试成员", "role": "user",
                                                   "created_at": 1700000000, "reviewed_at": None, "status": "pending"}]},
        "/api/remotes": {"outgoing": [{"id": 7, "url": "https://files.example.test:8443",
                                        "remote_name": "演示外连服务器", "status": "approved", "created_at": 1700000000}]},
        "/api/remotes/incoming": {"incoming": [{"id": "remote-1", "source_name": "演示申请服务器",
                                                  "created_at": 1700000000, "status": "pending", "grants": []}]},
    }
    if endpoint == "/api/registration/status":
        route.fulfill(status=404, content_type="application/json", body='{"error":"No registration"}')
        return
    if endpoint.endswith("/list"):
        path = parse_qs(parsed.query).get("path", [""])[0]
        body = {"path": path, "permissions": PERMISSIONS, "items": [
            {"name": LONG_FOLDER, "path": LONG_FOLDER, "directory": True, "size": None,
             "modified": 1700000000, "permissions": PERMISSIONS},
            {"name": LONG_FILE, "path": LONG_FILE, "directory": False, "size": 123456789,
             "modified": 1700000000, "permissions": PERMISSIONS},
        ]}
    else:
        body = bodies.get(endpoint, {"ok": True})
    route.fulfill(status=200, content_type="application/json", body=json.dumps(body, ensure_ascii=False))


def assert_fits(page, description):
    overflow = page.evaluate("""() => ({
        viewport: innerWidth,
        document: document.documentElement.scrollWidth,
        offenders: [...document.querySelectorAll('main, .main, .page, .card, .topbar')]
          .filter(e => e.getBoundingClientRect().right > innerWidth + 1)
          .map(e => e.className)
    })""")
    assert overflow["document"] <= overflow["viewport"] + 1, (description, overflow)


def navigate(page, mobile, text):
    if mobile:
        page.get_by_role("button", name="打开导航菜单").click()
        expect(page.locator(".navigation-backdrop")).to_be_visible()
    page.locator(".nav-item").filter(has_text=text).click()
    expect(page.get_by_role("heading", name=text, exact=True)).to_be_visible()
    if mobile:
        expect(page.locator(".navigation-backdrop")).to_have_count(0)
        expect(page.locator("#main-navigation")).to_be_hidden()
        assert page.locator(".main").get_attribute("inert") is None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:4178")
    parser.add_argument("--chrome", default=r"C:\Users\Lenovo\AppData\Local\ms-playwright\chromium-1208\chrome-win64\chrome.exe")
    args = parser.parse_args()
    artifacts = ROOT / ".build" / "responsive"
    artifacts.mkdir(parents=True, exist_ok=True)
    results = []
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(executable_path=args.chrome, headless=True)
        for width, height in [(320, 740), (375, 812), (390, 844), (667, 375), (768, 1024), (1280, 800)]:
            mobile = width <= 760
            context = browser.new_context(viewport={"width": width, "height": height},
                                          has_touch=mobile, is_mobile=mobile, locale="zh-CN")
            context.route("**/api/**", fixture)
            page = context.new_page()
            errors = []
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.goto(args.url)
            expect(page.get_by_role("heading", name="MyFolder 移动端布局验证服务器 · 文件区")).to_be_visible()
            expect(page.get_by_role("button", name="打开导航菜单")).to_be_visible() if mobile else expect(page.locator(".mobile-appbar")).to_be_hidden()
            assert_fits(page, f"files-{width}")
            assert page.locator(".lan-file-button").first.bounding_box()["width"] <= width, f"File name overflow at {width}px"
            page.get_by_role("button", name=f"移动或重命名 {LONG_FILE}", exact=True).click()
            expect(page.get_by_role("dialog", name="移动或重命名", exact=True)).to_be_visible()
            assert_fits(page, f"dialog-{width}")
            page.get_by_role("button", name="取消", exact=True).click()
            page.locator(".lan-file-button").first.click()
            expect(page.locator(".crumbs b")).to_have_text(LONG_FOLDER)
            assert_fits(page, f"breadcrumb-{width}")
            page.evaluate("scrollTo(0, 0)")
            page.screenshot(path=str(artifacts / f"files-{width}.png"), full_page=True)
            for label in ["服务器信息", "登录设备", "用户与权限", "外连服务器"]:
                navigate(page, mobile, label)
                assert_fits(page, f"{label}-{width}")
                if mobile:
                    assert page.locator(".lan-table-wrap").evaluate_all("elements => elements.every(e => e.scrollWidth <= e.clientWidth + 1)"), f"Table controls overflow at {width}px"
                page.evaluate("scrollTo(0, 0)")
                page.screenshot(path=str(artifacts / f"page-{label}-{width}.png"), full_page=True)
            if mobile:
                toggle = page.get_by_role("button", name="打开导航菜单")
                toggle.click()
                close = page.get_by_role("button", name="关闭导航菜单")
                expect(close).to_be_focused()
                close.press("Shift+Tab")
                expect(page.get_by_role("button", name="退出登录", exact=True)).to_be_focused()
                page.keyboard.press("Escape")
                expect(toggle).to_be_focused()
                toggle.click()
                page.locator("#server-choice").click()
                page.get_by_role("option").last.click()
                expect(page.locator(".navigation-backdrop")).to_have_count(0)
                expect(toggle).to_be_focused()
                assert_fits(page, f"remote-files-{width}")
                toggle.click()
            page.get_by_role("button", name="退出登录", exact=True).click()
            expect(page.get_by_role("heading", name="登录 MyFolder")).to_be_visible()
            expect(page.locator(".auth-brand")).to_be_visible()
            assert_fits(page, f"login-{width}")
            if mobile:
                assert float(page.locator("#acct").evaluate("e => getComputedStyle(e).fontSize").removesuffix("px")) >= 16
            page.screenshot(path=str(artifacts / f"login-{width}.png"), full_page=True)
            page.get_by_role("button", name="忘记密码？").click()
            expect(page.get_by_role("heading", name="忘记密码", exact=True)).to_be_visible()
            assert_fits(page, f"forgot-{width}")
            page.get_by_role("button", name="返回登录").click()
            page.get_by_role("button", name="申请注册", exact=True).click()
            expect(page.get_by_role("heading", name="申请注册", exact=True)).to_be_visible()
            assert_fits(page, f"registration-{width}")
            page.screenshot(path=str(artifacts / f"registration-{width}.png"), full_page=True)
            page.get_by_role("button", name="返回登录", exact=True).click()
            page.locator("#acct").fill("fixture_admin")
            page.locator("#pwd").fill("Disposable-test-password")
            page.get_by_role("button", name="登录", exact=True).click()
            expect(page.locator(".lan-login-status")).to_contain_text("此浏览器免密已开启")
            assert_fits(page, f"login-status-{width}")
            page.screenshot(path=str(artifacts / f"login-status-{width}.png"), full_page=True)
            assert not errors, errors
            results.append({"width": width, "height": height, "status": "passed"})
            context.close()
        browser.close()
    print(json.dumps({"viewports": results, "screenshots": str(artifacts)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
