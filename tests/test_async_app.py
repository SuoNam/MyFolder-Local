"""Exercise the Drogon backend over real sockets."""

import http.client
import io
import json
import os
import socket
import subprocess
import tempfile
import time
import unittest
import zipfile
from pathlib import Path

BASE = Path(__file__).resolve().parents[1]


class Node:
    def __init__(self):
        self.temp = tempfile.TemporaryDirectory()
        self.data = Path(self.temp.name)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            self.port = listener.getsockname()[1]
        env = dict(os.environ, MYFOLDER_DATA=str(self.data), MYFOLDER_HOST="127.0.0.1",
                   MYFOLDER_PORT=str(self.port), MYFOLDER_WEB=str(BASE / "web" / "dist"))
        default_binary = BASE / "cpp" / "build" / ("Release/myfolder-lan.exe" if os.name == "nt" else "myfolder-lan")
        command = [env.get("MYFOLDER_CPP_BINARY", str(default_binary))]
        initialized = subprocess.run(command + ["--init-admin"], cwd=BASE, env=env,
                                     capture_output=True, text=True, encoding="utf-8",
                                     errors="replace", check=True)
        self.initial_password = initialized.stdout.splitlines()[1].split(":", 1)[1].strip()
        self.process = subprocess.Popen(command, cwd=BASE,
                                        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            if self.process.poll() is not None:
                raise AssertionError(self.process.stderr.read().decode(errors="replace"))
            try:
                if self.request("GET", "/")[0] == 200:
                    break
            except OSError:
                pass
            time.sleep(.05)
        else:
            raise AssertionError("Drogon server did not start")

    @property
    def url(self):
        return f"http://127.0.0.1:{self.port}"

    def request(self, method, path, body=None, cookie=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        headers = dict(headers or {})
        if cookie:
            headers["Cookie"] = cookie
        if isinstance(body, dict):
            body = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        normalized = {"-".join(part.capitalize() for part in key.split("-")): value
                      for key, value in response.getheaders()}
        result = response.status, normalized, response.read()
        connection.close()
        return result

    def login(self, username="admin", password=None):
        if password is None:
            password = self.initial_password
        status, headers, data = self.request("POST", "/api/login", {"username": username, "password": password})
        if status != 200:
            raise AssertionError((status, data))
        return headers["Set-Cookie"].split(";", 1)[0]

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.process.stderr.close()
        for attempt in range(10):
            try:
                self.temp.cleanup()
                break
            except PermissionError:
                if attempt == 9:
                    raise
                time.sleep(.1)


class DrogonIntegration(unittest.TestCase):

    def test_roles_transfer_quota_and_remote_streaming(self):
        source, target = Node(), Node()
        try:
            admin, target_admin = source.login(), target.login()
            self.assertEqual(source.request("POST", "/api/users", {
                "username": "member", "password": "secret2", "role": "user"}, admin)[0], 201)
            member = source.login("member", "secret2")
            self.assertEqual(source.request("PUT", "/api/upload?path=a.txt", b"hello world", member)[0], 403)
            self.assertEqual(source.request("PUT", "/api/upload?path=a.txt", b"hello world", admin)[0], 201)
            partial = source.request("GET", "/api/download?path=a.txt", cookie=member,
                                     headers={"Range": "bytes=6-10"})
            self.assertEqual((partial[0], partial[2], partial[1].get("Content-Range")),
                             (206, b"world", "bytes 6-10/11"))
            self.assertEqual(source.request("POST", "/api/server-info", {
                "name": "Source", "storage_limit": 11}, admin)[0], 200)
            self.assertEqual(source.request("PUT", "/api/upload?path=b.txt", b"x", admin)[0], 413)
            self.assertEqual(source.request("POST", "/api/remotes", {"url": target.url}, admin)[0], 201)
            incoming = json.loads(target.request("GET", "/api/remotes/incoming", cookie=target_admin)[2])["incoming"][0]
            self.assertEqual(target.request("POST", "/api/remotes/incoming/grants", {
                "id": incoming["id"], "path": "/", "upload": True, "download": True}, target_admin)[0], 200)
            self.assertEqual(target.request("POST", "/api/remotes/incoming/review", {
                "id": incoming["id"], "decision": "approve"}, target_admin)[0], 200)
            self.assertEqual(source.request("POST", "/api/remotes/sync", {}, admin)[0], 200)
            self.assertEqual(source.request("PUT", "/api/remote/1/upload?path=remote.txt", b"streamed", admin)[0], 201)
            self.assertEqual(source.request("GET", "/api/remote/1/download?path=remote.txt", cookie=admin)[2], b"streamed")
            self.assertEqual(target.request("GET", "/api/download?path=remote.txt", cookie=target_admin)[2], b"streamed")
        finally:
            try:
                source.close()
            finally:
                target.close()


    def test_file_permissions_zip_and_password_review(self):
        node = Node()
        try:
            admin = node.login()
            self.assertEqual(node.request("POST", "/api/mkdir", {"path": "shared"}, admin)[0], 201)
            self.assertEqual(node.request("POST", "/api/users", {
                "username": "manager", "password": "secret2", "role": "admin"}, admin)[0], 201)
            manager = node.login("manager", "secret2")
            self.assertEqual(node.request("POST", "/api/users", {
                "username": "badadmin", "password": "secret3", "role": "admin"}, manager)[0], 403)
            self.assertEqual(node.request("POST", "/api/users", {
                "username": "alice", "password": "secret3", "role": "user"}, manager)[0], 201)
            users = json.loads(node.request("GET", "/api/users", cookie=admin)[2])["users"]
            uid = next(item["id"] for item in users if item["username"] == "alice")
            grants = json.loads(node.request("GET", f"/api/grants?user_id={uid}", cookie=admin)[2])["grants"]
            self.assertEqual([(g["path"], g["download"]) for g in grants], [("", 1)])
            alice = node.login("alice", "secret3")
            self.assertEqual(node.request("PUT", "/api/upload?path=shared%2Fa.txt", b"abc", alice)[0], 403)
            self.assertEqual(node.request("POST", "/api/grants", {
                "user_id": uid, "path": "shared", "upload": True, "download": True,
                "modify": True, "delete": True}, manager)[0], 200)
            self.assertEqual(node.request("PUT", "/api/upload?path=shared%2Fa.txt", b"abcdef", alice)[0], 201)
            status, _, archive = node.request("GET", "/api/folder.zip?path=shared", cookie=alice)
            self.assertEqual(status, 200)
            with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
                self.assertEqual(bundle.read("a.txt"), b"abcdef")
            self.assertEqual(node.request("POST", "/api/move", {
                "source": "shared/a.txt", "destination": "shared/b.txt"}, alice)[0], 200)
            self.assertEqual(node.request("DELETE", "/api/delete?path=shared%2Fb.txt", cookie=alice)[0], 200)
            self.assertEqual(node.request("POST", "/api/password-requests", {"username": "alice"})[0], 200)
            requests = json.loads(node.request("GET", "/api/password-requests", cookie=manager)[2])["requests"]
            self.assertEqual(node.request("POST", "/api/password-requests/review", {
                "id": requests[0]["id"], "decision": "approve"}, manager)[0], 200)
            reset = node.login("alice", "123456")
            self.assertEqual(node.request("POST", "/api/password", {"password": "newsecret"}, reset)[0], 200)
            node.login("alice", "newsecret")
        finally:
            node.close()


if __name__ == "__main__":
    unittest.main()
