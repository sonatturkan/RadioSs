#!/usr/bin/env python3
"""Append PROMPT-2 coordination events without rewriting shared session logs."""

import argparse
from contextlib import ExitStack
from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path


def append_locked(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as stream:
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        os.write(stream.fileno(), text.encode("utf-8"))
        os.fsync(stream.fileno())
        fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--actor", required=True)
    parser.add_argument("--topic", required=True)
    parser.add_argument("--body-file", required=True, type=Path)
    parser.add_argument("--file", action="append", default=[])
    parser.add_argument("--source-time")
    args = parser.parse_args()

    local_root = Path(__file__).resolve().parent
    project_root = local_root.parents[1]
    shared_root = project_root.parent / "prompt" / "agent"
    now = datetime.now(timezone.utc)
    stamp = now.strftime("%Y-%m-%d %H:%M:%S UTC")
    hour = now.strftime("%Y-%m-%d %H:00–%H:59 UTC")
    body = args.body_file.read_text(encoding="utf-8").strip()
    file_line = ", ".join(f"`{path}`" for path in args.file) or "Kaynak kod değişikliği yok."
    source_line = f"\nAjanın bildirdiği kaynak zamanı: {args.source_time}.\n" if args.source_time else ""
    record = (
        f"\n\n---\n# PROMPT-2 — {args.topic}\n\n"
        f"**{stamp} — {hour} saatlik kayıt — PROMPT-2/{args.actor}.**\n"
        f"{source_line}\nİlgili/değişen dosyalar: {file_line}\n\n{body}\n"
    )

    # Use the conventions already advertised by the independent live sessions.
    with ExitStack() as stack:
        locks = []
        for name in (".AGENT.md.lock", "AGENT.md.lock"):
            lock = stack.enter_context((shared_root / name).open("a"))
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            locks.append(lock)
        append_locked(shared_root / "AGENT.md", record)
        for lock in reversed(locks):
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)

    append_locked(local_root / "coordination-log.md", record)
    append_locked(local_root / "logs" / now.strftime("%Y-%m-%d") / f"{now:%H}.md", record)
    event = {
        "time_utc": stamp,
        "scope": "PROMPT-2",
        "actor": args.actor,
        "topic": args.topic,
        "source_time": args.source_time,
        "files": args.file,
        "body": body,
    }
    append_locked(local_root / "logs" / "events.jsonl", json.dumps(event, ensure_ascii=False) + "\n")
    print(f"PROMPT-2 ortak/yerel/saatlik olay kaydı eklendi: {stamp}")


if __name__ == "__main__":
    main()
