"""Small authenticated smoke check for a freshly installed LAN server."""

import argparse
import http.client
import json
import pathlib
import time
from urllib.parse import urlsplit


def request(address, method, path, body=None, headers=None):
    connection = http.client.HTTPConnection(address.hostname, address.port, timeout=15)
    connection.request(method, path, body=body, headers=headers or {})
    response = connection.getresponse()
    result = response.status, dict(response.getheaders()), response.read()
    connection.close()
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    parser.add_argument("--credentials-file", type=pathlib.Path, required=True)
    args = parser.parse_args()
    address = urlsplit(args.url)
    assert address.scheme == "http" and address.hostname and address.port
    lines = args.credentials_file.read_text(encoding="utf-8").splitlines()
    username = lines[0].split(":", 1)[1].strip()
    password = lines[1].split(":", 1)[1].strip()
    body = json.dumps({"username": username, "password": password}).encode()
    status, headers, payload = request(address, "POST", "/api/login", body,
                                       {"Content-Type": "application/json"})
    assert status == 200, (status, payload)
    cookie = {key.lower(): value for key, value in headers.items()}["set-cookie"].split(";", 1)[0]
    auth = {"Cookie": cookie}
    status, _, payload = request(address, "GET", "/api/me", headers=auth)
    assert status == 200 and json.loads(payload)["role"] == "superadmin", (status, payload)
    status, _, payload = request(address, "GET", "/api/server-info", headers=auth)
    assert status == 200, (status, payload)
    info = json.loads(payload)
    path = f"deployment-check-{int(time.time())}.txt"
    content = b"MyFolder Drogon ARM64 deployment check\n"
    try:
        status, _, payload = request(address, "PUT", "/api/upload?path=" + path, content, auth)
        assert status == 201, (status, payload)
        status, _, payload = request(address, "GET", "/api/download?path=" + path, headers=auth)
        assert status == 200 and payload == content, (status, payload)
    finally:
        status, _, payload = request(address, "DELETE", "/api/delete?path=" + path, headers=auth)
        assert status == 200, (status, payload)
    print(json.dumps({"login": "ok", "role": "superadmin", "transfer": "ok",
                      "server_name": info.get("name")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
