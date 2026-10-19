"""Stable HTTP listener that routes each request to the supervised primary.

Handlers keep one Store for the entire request. The next request, including
on a keep-alive socket, selects the promoted node only after the supervisor's
durable identity and role checks pass. Domain authentication stays unchanged.
"""

from http.server import BaseHTTPRequestHandler
import json
from threading import Event, Lock, Thread

from . import __version__
from .api_server import BoundedHTTPServer
from .http_server import make_handler
from .schema import open_radio_store
from .storage import StoreError, WriteRejected


class FailoverRouter:
    def __init__(self, supervisor):
        self.supervisor = supervisor
        self._mutex = Lock()
        self._stores = {}
        self._handlers = {}

    def handler(self):
        state = self.supervisor.status()
        path = state["active_database"]
        with self._mutex:
            if path not in self._stores:
                self._stores[path] = open_radio_store(path)
            store = self._stores[path]
            with store._locked(exclusive=False):
                store._reload_locked()
                store._assert_writable_locked()
                metadata = store._meta()
                expected = dict(state["standby_identity"] if state["state"] == "promoted"
                                else state["source_identity"])
                if state["state"] == "promoted":
                    expected.update({field: state["promotion_result"][field] for field in
                                     ("timeline_id", "epoch")})
                if any(metadata.get(field) != expected.get(field) for field in
                       ("store_id", "node_id", "timeline_id", "epoch")):
                    raise WriteRejected("routing target differs from the supervised primary identity")
            if path not in self._handlers:
                self._handlers[path] = make_handler(store)
            return self._handlers[path]


def make_failover_handler(supervisor):
    router = FailoverRouter(supervisor)

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "RadioS-SDBASE-Router/" + __version__

        def setup(self):
            self.request.settimeout(getattr(self.server, "request_timeout", 30))
            super().setup()

        def _route(self, method):
            try:
                handler_class = router.handler()
            except (StoreError, OSError, ValueError, KeyError) as exc:
                self.close_connection = True
                encoded = json.dumps({"error": "primary_unavailable", "detail": str(exc)}).encode()
                self.send_response(503)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.send_header("Connection", "close")
                self.send_header("Retry-After", "1")
                self.end_headers()
                self.wfile.write(encoded)
                return
            # Parse/connection state is shared; the domain handler's close flag
            # propagates to BaseHTTPRequestHandler's keep-alive request loop.
            delegate = object.__new__(handler_class)
            delegate.__dict__ = self.__dict__
            delegate._run(method)

        def do_GET(self):
            self._route("GET")

        def do_POST(self):
            self._route("POST")

    return Handler


def serve_failover(supervisor, host="127.0.0.1", port=8000, *,
                   max_connections=32, request_timeout=30):
    stop = Event()
    worker_errors = []
    def monitor():
        try:
            supervisor.run(stop)
        except BaseException as exc:
            worker_errors.append(exc)
            stop.set()
    server = BoundedHTTPServer((host, port), make_failover_handler(supervisor),
                              max_connections=max_connections, request_timeout=request_timeout)
    worker = Thread(target=monitor, daemon=True, name="sdbase-failover-supervisor")
    worker.start()
    try:
        # A bounded polling loop lets a failed supervisor stop the listener;
        # stale routing must not continue silently after monitor failure.
        server.timeout = 0.5
        while not stop.is_set():
            server.handle_request()
        if worker_errors:
            raise StoreError("failover supervisor stopped unexpectedly") from worker_errors[0]
    finally:
        stop.set()
        server.server_close()
        worker.join(timeout=max(1, supervisor.policy.probe_timeout_seconds + 1))
