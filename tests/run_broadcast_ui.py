#!/usr/bin/env python3
"""Isolated native broadcast acceptance with owned PCM sinks and HTTP mount."""
from __future__ import annotations

import base64
import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import tempfile
import threading
from urllib.parse import parse_qs, urlsplit

import run_audio_ui as base


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="stage12-http-", dir=base.PROJECT / "build") as temporary:
        root = Path(temporary)
        capture = root / "stream.mp3"
        metadata = root / "metadata.json"
        authorization = "Basic " + base64.b64encode(b"source:stage12-session-secret").decode()

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_arguments):
                pass

            def reply(self, status: int):
                self.send_response(status)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def do_PUT(self):
                if self.path != "/live.mp3" or self.headers.get("Authorization") != authorization:
                    self.reply(401)
                    return
                if self.headers.get("Content-Type") != "audio/mpeg" or self.headers.get("Transfer-Encoding") or self.headers.get("Content-Length"):
                    self.reply(400)
                    return
                self.send_response(200)
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.flush()
                with capture.open("wb") as stream:
                    while True:
                        payload = self.rfile.read1(65536)
                        if not payload:
                            break
                        stream.write(payload)
                        stream.flush()
                self.close_connection = True

            def do_GET(self):
                parsed = urlsplit(self.path)
                query = parse_qs(parsed.query)
                if parsed.path != "/admin/metadata" or self.headers.get("Authorization") != authorization or query.get("mount") != ["/live.mp3"] or query.get("mode") != ["updinfo"]:
                    self.reply(401)
                    return
                metadata.write_text(json.dumps({"title": query.get("song", [""])[0], "authorization_seen": True}, ensure_ascii=False), encoding="utf-8")
                self.reply(200)

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        environment = {
            "RADIOS_TEST_STREAM_URL": f"http://127.0.0.1:{server.server_port}/live.mp3",
            "RADIOS_TEST_STREAM_CAPTURE": str(capture),
            "RADIOS_TEST_METADATA_CAPTURE": str(metadata),
            "RADIOS_TEST_STREAM_PASSWORD": "stage12-session-secret",
        }
        old_environment = {key: os.environ.get(key) for key in environment}
        os.environ.update(environment)
        original_copytree = base.shutil.copytree

        def copy_fixture(source, destination, *args, **kwargs):
            result = original_copytree(source, destination, *args, **kwargs)
            if Path(source) == base.PROJECT / "database/SDBASE/src":
                script = Path(destination) / "sdbase/http_server.py"
                text = script.read_text(encoding="utf-8")
                anchor = "                result = self._dispatch(method, path, query, principal)\n"
                gate = '''                if method == "POST" and path == "/v1/workflows/desktop-broadcast/run-start":
                    import time as _stage12_time
                    _stage12_hold = store.root / ".acceptance-broadcast-start-hold"
                    if _stage12_hold.exists():
                        (store.root / ".acceptance-broadcast-start-ready").write_text("1")
                    _stage12_deadline = _stage12_time.monotonic() + 3.0
                    while _stage12_hold.exists() and _stage12_time.monotonic() < _stage12_deadline:
                        _stage12_time.sleep(0.005)
                if method == "POST" and path == "/v1/workflows/desktop-broadcast/run-event":
                    import time as _stage13_time
                    _stage13_hold = store.root / ".acceptance-broadcast-event-hold"
                    if _stage13_hold.exists():
                        (store.root / ".acceptance-broadcast-event-ready").write_text("1")
                    _stage13_deadline = _stage13_time.monotonic() + 3.0
                    while _stage13_hold.exists() and _stage13_time.monotonic() < _stage13_deadline:
                        _stage13_time.sleep(0.005)
'''
                if text.count(anchor) != 1:
                    raise RuntimeError("Private broadcast response gate cannot be installed")
                script.write_text(text.replace(anchor, anchor + gate), encoding="utf-8")
            return result

        base.shutil.copytree = copy_fixture
        try:
            return base.main(test_timeout=240)
        finally:
            base.shutil.copytree = original_copytree
            for key, value in old_environment.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
