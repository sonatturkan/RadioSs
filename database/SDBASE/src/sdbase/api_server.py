"""HTTP listener with bounded active sockets, including idle keep-alive clients."""

from http.server import ThreadingHTTPServer
from math import isfinite
from threading import BoundedSemaphore, Lock


class BoundedHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, address, handler, *, max_connections=32, request_timeout=30.0):
        if type(max_connections) is not int or not 1 <= max_connections <= 1024:
            raise ValueError("server max_connections must be 1..1024")
        if isinstance(request_timeout, bool) or not isinstance(request_timeout, (int, float)) or \
                not isfinite(request_timeout) or not 0 < request_timeout <= 3600:
            raise ValueError("request_timeout must be positive and at most 3600 seconds")
        self.max_connections = max_connections
        self.request_timeout = float(request_timeout)
        self._slots = BoundedSemaphore(max_connections)
        self._stats_lock = Lock()
        self._active = self._peak = self._rejected = 0
        super().__init__(address, handler)

    def process_request(self, request, client_address):
        if not self._slots.acquire(blocking=False):
            with self._stats_lock:
                self._rejected += 1
            try:
                request.settimeout(1)
                request.sendall(b"HTTP/1.1 503 Service Unavailable\r\n"
                                b"Connection: close\r\nRetry-After: 1\r\n"
                                b"Content-Length: 0\r\n\r\n")
            except OSError:
                pass
            self.shutdown_request(request)
            return
        with self._stats_lock:
            self._active += 1
            self._peak = max(self._peak, self._active)
        try:
            super().process_request(request, client_address)
        except BaseException:
            self._release_slot()
            raise

    def _release_slot(self):
        with self._stats_lock:
            self._active -= 1
        self._slots.release()

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._release_slot()

    def stats(self):
        with self._stats_lock:
            return {"max_connections": self.max_connections,
                    "request_timeout_seconds": self.request_timeout,
                    "active_connections": self._active, "peak_connections": self._peak,
                    "capacity_rejections": self._rejected}
