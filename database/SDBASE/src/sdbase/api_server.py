"""HTTP listener with bounded active sockets, including idle keep-alive clients.

Windows listener rules (the base of the API, failover and replication servers):

* The port is owned exclusively (``SO_EXCLUSIVEADDRUSE``; never ``SO_REUSEADDR``,
  which on Windows lets a second socket of the same user bind the same
  127.0.0.1 port and take over part of the connections).  An exclusive port can
  stay unavailable for a short time after the previous listener closed while its
  accepted connections finish, so ``WSAEADDRINUSE`` is retried for a bounded
  time (``bind_retry_seconds``) before it is reported.
* No reverse DNS lookup at bind time (``HTTPServer.server_bind`` calls
  ``socket.getfqdn``, which can stall on LLMNR/NetBIOS).
* A client that drops its connection (``WSAECONNABORTED``/``WSAECONNRESET``) or
  times out is not a server error: no traceback in the service log.
* ``server_close()`` stops idle keep-alive connections of a
  ``StoppableRequestHandler`` at once instead of waiting for their idle timeout,
  while requests in progress finish (``block_on_close``).
"""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from math import inf, isfinite
import select
import socket
import socketserver
import sys
from threading import BoundedSemaphore, Event, Lock
from time import monotonic, sleep

_WSAEADDRINUSE = 10048
_WSAEACCES = 10013
_BIND_RETRY_STEP = 0.25


class BoundedHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False
    allow_reuse_address = False
    allow_reuse_port = False
    # Listen backlog: the desktop opens several connections at once at start-up.
    request_queue_size = 64
    bind_retry_seconds = 10.0

    def __init__(self, address, handler, *, max_connections=32, request_timeout=30.0,
                 bind_retry_seconds=None):
        if type(max_connections) is not int or not 1 <= max_connections <= 1024:
            raise ValueError("server max_connections must be 1..1024")
        if isinstance(request_timeout, bool) or not isinstance(request_timeout, (int, float)) or \
                not isfinite(request_timeout) or not 0 < request_timeout <= 3600:
            raise ValueError("request_timeout must be positive and at most 3600 seconds")
        if bind_retry_seconds is not None:
            if isinstance(bind_retry_seconds, bool) or not isinstance(bind_retry_seconds, (int, float)) or \
                    not isfinite(bind_retry_seconds) or not 0 <= bind_retry_seconds <= 60:
                raise ValueError("bind_retry_seconds must be 0..60 seconds")
            self.bind_retry_seconds = float(bind_retry_seconds)
        self.max_connections = max_connections
        self.request_timeout = float(request_timeout)
        self._slots = BoundedSemaphore(max_connections)
        self._stats_lock = Lock()
        self._active = self._peak = self._rejected = 0
        # Set before the listener exists: TCPServer.__init__ calls server_close()
        # when binding fails.
        self.stopping = Event()
        super().__init__(address, handler)

    def server_bind(self):
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        deadline = monotonic() + self.bind_retry_seconds
        while True:
            try:
                socketserver.TCPServer.server_bind(self)
                break
            except OSError as exc:
                if getattr(exc, "winerror", None) != _WSAEADDRINUSE or monotonic() >= deadline:
                    raise
            sleep(_BIND_RETRY_STEP)
        host, port = self.server_address[:2]
        self.server_name = host
        self.server_port = port

    def process_request(self, request, client_address):
        if self.stopping.is_set() or not self._slots.acquire(blocking=False):
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

    def handle_error(self, request, client_address):
        if isinstance(sys.exc_info()[1], (ConnectionError, TimeoutError)):
            return  # the client went away or stalled; nothing to report
        super().handle_error(request, client_address)

    def server_close(self):
        """Close the listener, end idle keep-alive connections, then (with
        block_on_close) wait for the requests that are still running."""
        self.stopping.set()
        super().server_close()

    def stats(self):
        with self._stats_lock:
            return {"max_connections": self.max_connections,
                    "request_timeout_seconds": self.request_timeout,
                    "active_connections": self._active, "peak_connections": self._peak,
                    "capacity_rejections": self._rejected}


class StoppableRequestHandler(BaseHTTPRequestHandler):
    """Keep-alive request loop that a stopping BoundedHTTPServer ends between requests.

    Between two requests the handler waits for the next request line in short
    slices instead of one blocking read of ``request_timeout`` seconds, so
    ``server_close()`` does not have to wait for idle clients (the desktop keeps
    one connection open for its whole session).  The idle limit stays the socket
    timeout, exactly as before."""

    idle_poll_seconds = 0.25

    def _server_stopping(self) -> bool:
        stopping = getattr(self.server, "stopping", None)
        return stopping is not None and stopping.is_set()

    def handle_one_request(self):
        if not self._next_request_arrived():
            self.close_connection = True
            return
        super().handle_one_request()

    def _next_request_arrived(self) -> bool:
        stopping = getattr(self.server, "stopping", None)
        if stopping is None:
            return True                      # a plain server: the blocking read as usual
        connection = self.connection
        timeout = connection.gettimeout()
        # Bytes the buffered reader already holds (a pipelined request) count as arrived.
        try:
            connection.settimeout(0)
            if self.rfile.peek(1):
                return True
        except OSError:
            return True                      # let the normal read report the broken connection
        finally:
            connection.settimeout(timeout)
        deadline = inf if timeout is None else monotonic() + timeout
        while not stopping.is_set():
            remaining = deadline - monotonic()
            if remaining <= 0:
                return False                 # idle keep-alive limit reached
            try:
                readable, _, _ = select.select([connection], [], [], min(self.idle_poll_seconds, remaining))
            except (OSError, ValueError):
                return True
            if readable:
                return True                  # data or end of stream: the normal read handles both
        return False


def describe_bind_error(exc, host, port):
    """Turkish operator text for a listener that cannot bind, or None for other errors."""
    code = getattr(exc, "winerror", None)
    if code == _WSAEADDRINUSE:
        return f"{host}:{port} kullanımda."
    if code == _WSAEACCES:
        return (f"{host}:{port} Windows tarafından ayrılmış veya başka bir uygulamaya özel; "
                "`netsh int ipv4 show excludedportrange protocol=tcp` ile denetleyin ve "
                "config\\radios.ini [database] service_url portunu değiştirin.")
    return None
