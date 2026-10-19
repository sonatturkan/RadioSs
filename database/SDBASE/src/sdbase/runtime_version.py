"""Source identity used to detect a local service running an earlier build."""

import hashlib
from pathlib import Path


DESKTOP_API_VERSION = 1


def source_digest() -> str:
    """Hash sorted package basenames and bytes, separated by NUL bytes."""
    digest = hashlib.sha256()
    for source in sorted(Path(__file__).parent.glob("*.py")):
        digest.update(source.name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(source.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()
