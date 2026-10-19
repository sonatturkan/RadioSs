"""Provision an independent, fenced RadioS ``.sdb`` copy at an absent path.

The source journal, checkpoint and metadata are copied under a shared lock.
The new directory is published only after its media and database have been
verified, all inherited credentials have been revoked, and its store identity
has been changed.  No SQL server or vendor database is involved.
"""

from __future__ import annotations

from pathlib import Path
import tempfile

from .backup import _destination, create_backup, restore_backup
from .storage import Store


def provision_store_copy(source: Store, target_dir: str | Path) -> dict:
    """Create an independent, initially offline copy without changing source files.

    ``target_dir`` must not exist.  The operation needs free space for an
    intermediate verified snapshot as well as the published destination.
    Source writes may continue; the copied sequence is internally consistent.
    """
    target = _destination(target_dir)
    with tempfile.TemporaryDirectory(prefix=f".{target.name}.provision-",
                                     dir=target.parent) as temporary:
        backup_dir = Path(temporary) / "snapshot"
        snapshot = create_backup(source, backup_dir, checkpoint=False)
        result = restore_backup(snapshot["path"], target, provision_copy=True)
        return {**result, "provisioned_offline": True}
