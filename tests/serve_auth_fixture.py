"""Short-lived loopback fixture for browser QA. Never uses the installed service/data.

This exercises the default HTTP mode without an insecure-login override.
The administrator password below is public test data, not a deployment credential.
"""
import argparse
from contextlib import closing
import base64
import hashlib
import json
import os
import signal
import sqlite3
import time
from test_async_app import Node


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=4181)
    parser.add_argument('--seconds', type=int, default=600)
    args = parser.parse_args()
    node = Node(port=args.port)
    try:
        salt = os.urandom(16)
        digest = hashlib.pbkdf2_hmac('sha256', b'Temporary-test-password-1', salt, 310000)
        encoded = 'pbkdf2:310000:' + base64.b64encode(salt).decode() + ':' + base64.b64encode(digest).decode()
        with closing(sqlite3.connect(node.data / 'myfolder.sqlite3')) as db, db:
            db.execute('UPDATE users SET password=? WHERE username=?', (encoded, 'admin'))
        def finish(*_):
            raise KeyboardInterrupt
        signal.signal(signal.SIGTERM, finish)
        signal.signal(signal.SIGINT, finish)
        print(json.dumps({'ready': True, 'port': node.port, 'disposable_data': str(node.data)}), flush=True)
        deadline = time.monotonic() + min(args.seconds, 900)
        while time.monotonic() < deadline:
            if node.process.poll() is not None:
                raise RuntimeError('Test server stopped unexpectedly')
            time.sleep(.2)
    except KeyboardInterrupt:
        pass
    finally:
        node.close()


if __name__ == '__main__':
    main()
