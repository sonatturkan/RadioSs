"""Source identity used to detect a local service running an earlier build."""

import hashlib
import os
from pathlib import Path

from . import _platform


DESKTOP_API_VERSION = 1


def source_digest() -> str:
    """Hash sorted package basenames and bytes, separated by NUL bytes.

    Same selection and order as the C client and the tools (cross-language
    contract): regular files whose name ends with the exact lowercase ".py",
    ordered by the UTF-8 bytes of the name.  Windows globbing and WindowsPath
    ordering are case-insensitive, so neither is used here.  The files are read
    with a share-delete reader so an update replacing a module is never blocked.
    """
    package = Path(__file__).parent
    with os.scandir(package) as entries:
        names = sorted((entry.name for entry in entries
                        if entry.name.endswith(".py") and entry.is_file(follow_symlinks=False)),
                       key=lambda name: name.encode("utf-8"))
    digest = hashlib.sha256()
    for name in names:
        digest.update(name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(_platform.read_bytes_shared(package / name))
        digest.update(b"\0")
    return digest.hexdigest()
