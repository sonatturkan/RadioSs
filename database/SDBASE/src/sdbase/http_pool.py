"""Bounded persistent HTTP connections for the endpoint protocol.

Requests are never retried automatically: a disconnected write can already
have committed on the server. Only fully consumed responses return to the
pool; early-close and failed connections are discarded.
"""

from collections import deque
from http.client import HTTPConnection, HTTPSConnection
from math import isfinite
from threading import Condition
from time import monotonic
from urllib.parse import urlsplit


class PooledResponse:
    def __init__(self, pool, connection, response):
        self.pool, self.connection, self.response = pool, connection, response
        self._released = False

    @property
    def headers(self):
        return self.response.headers

    @property
    def status(self):
        return self.response.status

    def read(self, amount=None):
        try:
            data = self.response.read(amount)
        except BaseException:
            self._release(False)
            raise
        if self.response.isclosed():
            self._release(not self.response.will_close)
        return data

    def _release(self, reusable):
        if not self._released:
            self._released = True
            self.pool._release(self.connection, reusable)

    def close(self):
        self._release(self.response.isclosed() and not self.response.will_close)
        self.response.close()

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.close()


class HTTPConnectionPool:
    def __init__(self, base_url, *, max_connections=4, timeout=20.0):
        parsed = urlsplit(base_url)
        if parsed.scheme not in {"http", "https"} or not parsed.hostname or \
                parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise ValueError("pool requires an HTTP(S) server URL without credentials/query")
        if type(max_connections) is not int or not 1 <= max_connections <= 128:
            raise ValueError("max_connections must be 1..128")
        if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or \
                not isfinite(timeout) or not 0 < timeout <= 3600:
            raise ValueError("timeout must be positive and at most 3600 seconds")
        self._scheme, self._host, self._port = parsed.scheme, parsed.hostname, parsed.port
        if self._scheme == "http" and self._host.lower() == "localhost":
            # Windows resolves "localhost" to ::1 first and needs about 2 s to
            # fall back to the IPv4 loopback listener on every new connection.
            # Plain HTTP has no certificate name to verify, so connect to the
            # literal; HTTPS keeps the name for host verification.
            self._host = "127.0.0.1"
        self._prefix = parsed.path.rstrip("/")
        self._limit, self._timeout = max_connections, float(timeout)
        self._condition = Condition()
        self._idle = deque()
        self._total = self._created = self._reused = self._discarded = 0
        self._closed = False

    def _acquire(self):
        deadline = monotonic() + self._timeout
        with self._condition:
            while True:
                if self._closed:
                    raise OSError("HTTP connection pool is closed")
                if self._idle:
                    self._reused += 1
                    return self._idle.popleft()
                if self._total < self._limit:
                    factory = HTTPSConnection if self._scheme == "https" else HTTPConnection
                    connection = factory(self._host, self._port, timeout=self._timeout)
                    self._total += 1
                    self._created += 1
                    return connection
                remaining = deadline - monotonic()
                if remaining <= 0:
                    raise TimeoutError("HTTP connection pool checkout timed out")
                self._condition.wait(remaining)

    def _release(self, connection, reusable):
        with self._condition:
            if reusable and not self._closed:
                self._idle.append(connection)
            else:
                connection.close()
                self._total -= 1
                self._discarded += 1
            self._condition.notify()

    def request(self, method, path, *, headers=None, body=None):
        if not path.startswith("/") or path.startswith("//"):
            raise ValueError("request path must be relative to the configured server")
        connection = self._acquire()
        try:
            connection.request(method, self._prefix + path, body=body, headers=headers or {})
            response = connection.getresponse()
        except BaseException:
            self._release(connection, False)
            raise
        return PooledResponse(self, connection, response)

    def stats(self):
        with self._condition:
            return {"max_connections": self._limit, "connections": self._total,
                    "idle": len(self._idle), "created": self._created,
                    "reused": self._reused, "discarded": self._discarded,
                    "closed": self._closed}

    def close(self):
        with self._condition:
            self._closed = True
            while self._idle:
                self._idle.popleft().close()
                self._total -= 1
            self._condition.notify_all()

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.close()
