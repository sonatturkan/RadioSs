"""Bounded, process-local request buckets for the threaded HTTP listener."""

from __future__ import annotations

from math import ceil
from threading import Lock
from time import monotonic


class RequestLimiter:
    """Token buckets for clients and credentials, with stale-key eviction."""

    def __init__(self, max_keys: int = 10_000) -> None:
        self._lock = Lock()
        self._buckets: dict[str, tuple[float, float]] = {}
        self._max_keys = max_keys

    def retry_after(self, key: str, *, capacity: float,
                    refill_per_second: float) -> int:
        now = monotonic()
        with self._lock:
            if len(self._buckets) >= self._max_keys:
                self._buckets = {name: state for name, state in self._buckets.items()
                                 if now - state[1] < 300}
                if len(self._buckets) >= self._max_keys and key not in self._buckets:
                    return 60
            tokens, last = self._buckets.get(key, (capacity, now))
            tokens = min(capacity, tokens + max(0.0, now - last) * refill_per_second)
            if tokens < 1:
                self._buckets[key] = (tokens, now)
                return max(1, ceil((1 - tokens) / refill_per_second))
            self._buckets[key] = (tokens - 1, now)
            return 0
