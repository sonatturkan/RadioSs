#!/usr/bin/env python3
"""Actual private TLS service, no browser, no production credentials or network."""
import http.server
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time


class Handler(http.server.BaseHTTPRequestHandler):
    requests = []

    def log_message(self, *_args):
        pass

    def do_HEAD(self):
        type(self).requests.append((self.path, dict(self.headers)))
        if self.path == "/slow":
            time.sleep(2)
        code = {"/success": 204, "/auth": 401, "/redirect": 302, "/method": 405}.get(self.path, 200)
        self.send_response(code)
        if self.path == "/redirect":
            self.send_header("Location", "https://example.invalid/never-contact")
        if self.path == "/large":
            for index in range(8):
                self.send_header(f"X-Large-{index}", "x" * 10000)
        self.send_header("Content-Length", "0")
        try:
            self.end_headers()
        except (OSError, ssl.SSLError):
            pass


def main():
    executable = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="radios-private-tls-") as directory:
        root = Path(directory)
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-keyout", str(root / "key.pem"), "-out", str(root / "ca.pem"),
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
                       check=True, capture_output=True, timeout=15)
        (root / "key.pem").chmod(0o600)
        service = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(root / "ca.pem", root / "key.pem")
        service.socket = context.wrap_socket(service.socket, server_side=True)
        thread = threading.Thread(target=service.serve_forever, daemon=True)
        thread.start()
        environment = os.environ.copy()
        environment["RADIOS_TEST_ACCESS_URL"] = f"https://127.0.0.1:{service.server_port}"
        environment["RADIOS_TEST_ACCESS_CA"] = str(root / "ca.pem")
        try:
            result = subprocess.run([str(executable), *sys.argv[2:]], env=environment, timeout=35)
            paths = [path for path, _ in Handler.requests]
            assert "/never-contact" not in paths and len(paths) == 6, paths
            for _path, headers in Handler.requests:
                assert not any(key.lower() in {"authorization", "cookie", "proxy-authorization"} for key in headers)
            print(json.dumps({"private_tls_requests": len(paths), "redirect_followed": False,
                              "credentials_sent": False, "production_data_used": False}))
            return result.returncode
        finally:
            service.shutdown()
            service.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
