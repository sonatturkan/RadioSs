"""Bounded process-local counters and timing samples, without record contents."""

from collections import deque
from threading import Lock
from time import monotonic


class EngineMetrics:
    def __init__(self, sample_limit: int = 1024):
        if type(sample_limit) is not int or not 1 <= sample_limit <= 10000:
            raise ValueError("sample_limit must be 1..10000")
        self._lock = Lock()
        self._started = monotonic()
        self._limit = sample_limit
        self._counters = {}
        self._timings = {}

    def increment(self, name: str, count: int = 1) -> None:
        with self._lock:
            self._counters[name] = self._counters.get(name, 0) + count

    def observe(self, name: str, value: float) -> None:
        with self._lock:
            self._timings.setdefault(name, deque(maxlen=self._limit)).append(float(value))

    def snapshot(self) -> dict:
        with self._lock:
            timings = {}
            for name, values in self._timings.items():
                ordered = sorted(values)
                timings[name] = {"samples": len(ordered), "p50": ordered[(len(ordered)-1)//2],
                                 "p95": ordered[min(len(ordered)-1, int(len(ordered)*0.95))],
                                 "max": ordered[-1]}
            return {"scope": "process", "uptime_seconds": round(monotonic()-self._started, 3),
                    "sample_limit": self._limit, "counters": dict(self._counters),
                    "timings": timings}
