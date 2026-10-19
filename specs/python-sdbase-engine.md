# Windows port spec: area `python-sdbase-engine`

Analyzed copy: `C:\Users\sonatturkan\Desktop\RadioS\database\SDBASE\src\sdbase\` (all 28 assigned modules, plus `backup.py` helpers used by every other module, `api_server.py`, `read_snapshot.py` and the callers in `desktop_media.py` / `http_server.py` / `tools/` where they touch engine contracts). The phase was read-only: no project file was changed.

All Windows behaviour below was **measured on the target**, not assumed: official CPython **3.13.16 embeddable** (`python313.dll`, PSF Authenticode valid), Windows 11 Pro build 26300, NTFS, user `HACETTEPE\sonatturkan` (Entra SID `S-1-12-1-…`), `LongPathsEnabled` = **off**. The evidence scripts and results are in the scratchpad (no project files):

| Evidence | Path |
|---|---|
| Raw OS-semantics probe + JSON result | `C:\Users\sonatturkan\AppData\Local\Temp\claude\c--Users-sonatturkan-Desktop-radyo-projesi-kaynak-kodlar-RadioS\0ead7b3c-7fb3-425d-a304-a7629cf56b77\scratchpad\scripts\winprobe.py`, `...\scripts\winprobe-result.json` |
| Reference shim (validated) | `C:\Users\sonatturkan\AppData\Local\Temp\claude\c--Users-sonatturkan-Desktop-radyo-projesi-kaynak-kodlar-RadioS\0ead7b3c-7fb3-425d-a304-a7629cf56b77\scratchpad\scripts\proto\_platform.py` |
| Shim tests + result | `...\scripts\proto_test.py`, `...\scripts\proto-result.json` |
| Listener exclusivity probe | `...\scripts\sockprobe.py` |
| Read-only scan of the real Linux DB | `...\scripts\dbscan.py`, `...\scripts\journalscan.py`, `...\scripts\dbscan-result.json` |

(`...` = `C:\Users\sonatturkan\AppData\Local\Temp\claude\c--Users-sonatturkan-Desktop-radyo-projesi-kaynak-kodlar-RadioS\0ead7b3c-7fb3-425d-a304-a7629cf56b77\scratchpad`.)

---

## 0. Executive summary: blockers, most severe first

1. **`import fcntl` at module top** in 10 engine modules: storage.py:17, disk_index.py:13, endpoint_journal.py:5, migration.py:12, failover.py:10, integrity.py:12, online_maintenance.py:11, ops_scheduler.py:14, replication.py:15, replication_stream.py:14. `sdbase/__init__.py:3` imports `storage`, so **`import sdbase` itself fails** (`fcntl` is not importable; verified). Nothing starts until the shim lands.
2. **Directory fsync through `os.open(dir, O_RDONLY)`** fails with `PermissionError [Errno 13]` (verified) in 7 copies. The worst is `storage._atomic_json` (storage.py:129-133): it runs *after* `os.replace` succeeded, so every `Store()` creation, checkpoint and meta update raises.
3. **`backup._publish_absent`** (backup.py:154-168) calls Linux `renameat2` through `ctypes.CDLL(None)`. On Windows that raises `TypeError: LoadLibrary() argument 1 must be str, not None` (verified). Note it is a `TypeError`, not a `BackupError`, so callers' `except BackupError` misses it. This breaks backup, restore, migration, WAL enable, standby creation, shipments, retention, PITR, portable export/import, ops archives, and `tools/radios_reliability.py`.
4. **`os.replace` is not "rename over open file"** on Windows:
   - WinError 5 if *any* handle has the target open without FILE_SHARE_DELETE.
   - WinError 32 if the source is open.
   - It fails **even when the reader used FILE_SHARE_DELETE**, because MoveFileExW still lacks POSIX semantics on build 26300 (verified).
   - Python's `open()` never passes FILE_SHARE_DELETE.
   - Affected targets: meta, checkpoint, `head.json` (rewritten on **every commit** when WAL archival is on), manifests, scheduler/supervisor state, the corrupt-object repair, and the **live journal** (online_maintenance.py:219).
   - Antivirus, the Search indexer and backup/OneDrive agents cause the same transient failures.
5. **`os.statvfs` and `os.pread` do not exist** (verified): store_health.py:116, capacity_monitor.py:182/188, disk_index.py:416. They raise `AttributeError`, which the `except OSError` handlers do not catch.
6. **`ops_scheduler._validate_config`** (ops_scheduler.py:138-140) rejects every Windows directory: `st_mode` is always `0o40777` (verified), so `mode & 0o077` is never 0 and the scheduler can never run.
7. **`endpoint_journal` writes raw `os.write` on an fd opened without `O_BINARY`** (endpoint_journal.py:45 and :104). The CRT translates `\n` into `\r\n` (verified `610d0a62`), so `Endpoint.sdb` lines differ from the Linux format.
   - Important nuance (verified): **`os.fdopen()` forces the fd into binary mode** (CPython FileIO calls `_setmode(fd, O_BINARY)`). So every other `os.open` + `os.fdopen` path is byte-safe today. `endpoint_journal.py:104` is the only raw `os.write` in the package.
   - The shim still adds `O_BINARY` everywhere defensively.
8. **Symlink defenses do not see junctions.** On a junction, `Path.is_symlink()` and `DirEntry.is_symlink()` return False, `DirEntry.is_dir(follow_symlinks=False)` returns True, and `os.walk(followlinks=False)` **descends into junctions** (all verified). Every "must not contain symlinks" check must also reject name-surrogate reparse points.
9. **Manifest-relative path validation** uses `Path(relative).parts` (backup.py:66, disk_index.py:118, replication.py:219, wal_archive.py:392/429). On Windows this lets inputs escape the root (verified):
   - `"\\foo"` resolves to `C:\foo`.
   - `"D:foo"` resolves to `D:foo`.
   - `"x:stream"` (NTFS alternate data stream) and reserved device names (`con.json`, `nul`) are accepted.
10. **Files that are open or mmapped cannot be deleted, replaced or truncated** (verified WinError 5/32). This affects disk-index mmaps (disk_index.py:442-443), media objects that the C player or the HTTP media stream hold open, and AV scans of freshly written stage files. All cleanup paths (`shutil.rmtree` in `finally:` blocks, ~25 sites) can then raise and **mask the original exception**.
11. **Python 3.13's `mkdir(mode=0o700)` and `tempfile.mkdtemp()` apply a protected DACL** of SYSTEM, Administrators and **OWNER RIGHTS**. In this session the new directory's owner was `BUILTIN\Administrators` (elevated token), so the same user running un-elevated later cannot open it. These stages are **published as permanent directories** (`RadioS.sdb.wal`, backups, replicas, retention segments, migrated stores).
12. **HTTP listeners** (`failover_server`, `replication_stream`) inherit `HTTPServer.allow_reuse_address = 1`. On Windows a second server binds the same port and **silently succeeds** (verified). `SO_EXCLUSIVEADDRUSE` fixes it (verified). Also, `localhost` costs about 2 s per connect (IPv6 tried first): 2047 ms vs 90 ms measured. Always use `127.0.0.1`.

The **database files are byte-portable** from Linux to Windows: big-endian framing, JSON, and relative POSIX paths in engine metadata. As proof, Windows CPython parsed and SHA-verified all **3,417 frames (sequences 3163 to 6579) of the real 707 MB Linux journal** with 0 failures. The only absolute Linux paths are inside domain records written by the desktop layer (§8).

---

## 1. Verified Windows facts used by this spec

| Topic | Measured result (CPython 3.13.16, Win11 26300) | Consequence |
|---|---|---|
| Modules | `fcntl`, `resource`, `pwd` **not importable** | shim required |
| `os` attrs missing | `statvfs, pread, pwrite, O_NOFOLLOW, O_CLOEXEC, O_DIRECTORY, O_NONBLOCK, O_SYNC, O_DSYNC, sync, fdatasync, getuid, geteuid, fork, killpg, setsid` | AttributeError on direct use (engine uses `getattr(os,"O_*",0)` everywhere, so flags silently degrade) |
| `os` attrs present | `O_BINARY, O_NOINHERIT, fchmod, lchmod (3.13), link, symlink, umask, ftruncate, os.path.isreserved, os.path.isjunction`; `os.chmod in os.supports_fd` | `os.fchmod` (ops_scheduler.py:207) works on 3.13 |
| `os.open` default | text mode: `os.write(b"a\nb")` writes `61 0d 0a 62`; with `O_BINARY` writes `61 0a 62` | add `O_BINARY` |
| `os.fdopen(fd,"rb"/"ab")` on a text-mode fd | bytes unchanged (`x\r\ny\x1az` read verbatim; `1\n2\n` appended verbatim) | FileIO forces binary; only raw `os.read/os.write` are affected |
| `open("w")` text | `abc\n` becomes `61 62 63 0d 0a` | replication_stream.py:795 |
| `os.link(a,b,follow_symlinks=False)` | **works** on 3.13.16 (but `os.link not in os.supports_follow_symlinks`); existing dst: `FileExistsError` winerror 183 | don't depend on it; hard links also fail on FAT/exFAT |
| `os.replace` target open by Python `open()` | `PermissionError` winerror **5** | retry and share-delete readers |
| `os.replace` source open | `PermissionError` winerror **32** | retry |
| `os.replace` target open **with FILE_SHARE_DELETE** | still `PermissionError` winerror 5 | MoveFileExW lacks POSIX semantics |
| `SetFileInformationByHandle(FileRenameInfoEx, REPLACE_IF_EXISTS or POSIX_SEMANTICS)` | succeeds when the target is opened with FILE_SHARE_DELETE (old handle keeps reading old bytes); fails with winerror 32 if the target is opened by Python `open()` | use for `replace()` |
| `unlink` of an open file | Python `open()`: winerror 32; FILE_SHARE_DELETE opener: succeeds, name freed immediately, recreate OK | POSIX delete semantics exist on NTFS |
| `os.open(directory)` / `os.fsync(dirfd)` | `PermissionError [Errno 13]` | dir fsync must change |
| `FlushFileBuffers(dir handle)` | GENERIC_READ: winerror 5; **GENERIC_READ+GENERIC_WRITE + FILE_FLAG_BACKUP_SEMANTICS: OK, 0.20 ms** | best-effort dir flush is possible |
| file fsync cost | `FlushFileBuffers` avg **3.7 ms** | similar to Linux SSD |
| `stat(dir).st_mode` | `0o40777` | owner-only check must use ACLs |
| `stat` fields | `st_ino` 64-bit file id (changes on replace), `st_dev` 64-bit volume serial, `st_nlink` 1, `st_ctime == st_birthtime` (creation time), **no DeprecationWarning** at runtime | stamps keep working |
| `os.rename` file or dir onto existing | `FileExistsError` winerror 183 (never replaces) | native no-replace publish |
| rename a dir with an open child file | winerror 5 | close everything before publishing; retry for AV |
| `shutil.rmtree` with an open child | winerror 32 | robust rmtree |
| mmapped file | replace onto: winerror 5; unlink: 32; truncate: PermissionError | disk-index cleanup |
| read-only attribute (`chmod 0o444`) | replace onto it and unlink both fail with winerror 5 | never chmod to read-only; clear before delete |
| `LockFileEx` region at OffsetHigh=0x7FFFFFFF, Offset=0, len 1 | shared+shared OK; EX vs SH: error **33**; SH vs EX: 33; **EX twice on the same handle: 33 (not re-entrant)**; SH over own EX stacks (needs 2 unlocks); unlock of an unlocked region: **158**; reading offset 0 while another handle holds the high lock: OK; close without unlock released immediately (0.01 ms) | flock emulation (§4) |
| `LockFileEx` at offset 0 (low range) | other handle's `read()` fails with `PermissionError` (mandatory locking) | **must** lock at the high offset |
| cross-process / killed holder | lock acquired 0.0 s after `Popen.kill()` | crash recovery semantics preserved |
| `CreateFileW(FILE_FLAG_OPEN_REPARSE_POINT)` + tag check | symlink is refused race-free (`errno.ELOOP`=10062); following still possible without the flag | O_NOFOLLOW emulation |
| junction | `is_symlink` False, `is_junction` True, reparse tag `0xA0000003`, attribute 0x400, `os.walk` descends | `is_link_like()` |
| pathlib Windows join | `C:/x/uuid / "\\foo"` gives `C:\foo`; `/ "D:foo"` gives `D:foo`; `/ "C:foo"` gives `C:\x\uuid\foo`; `PureWindowsPath("/foo").is_absolute()` is False; equality and `is_relative_to` are case-insensitive | strict manifest parser |
| `os.path.isreserved` | True for `con.jsonl, nul, aux.txt, COM1.csv, lpt9, "x.", "x "`; **False for `a:b`** | also reject `:` explicitly |
| `fstat(NUL)` | `0o20000` (S_IFCHR) | existing `S_ISREG` checks already reject devices |
| `mkdir(mode=0o700)` / `mkdtemp` | SDDL `O:BA D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)` (owner became Administrators) | use explicit user-SID DACL |
| `GetDiskFreeSpaceExW` | total matches `shutil.disk_usage`; available-to-caller ≤ free | `disk_usage()` == statvfs `f_bavail` semantics |
| `RtlAreLongPathsEnabled()` | **False** on this host | MAX_PATH 260 applies |
| `SO_REUSEADDR` double bind | 2nd `TCPServer` on the same port **succeeds**; `HTTPServer.allow_reuse_address == 1` | use `SO_EXCLUSIVEADDRUSE` |
| `SO_EXCLUSIVEADDRUSE` | 2nd bind refused (10048), SO_REUSEADDR hijack refused (10013), rebind after close with TIME_WAIT OK | |
| `localhost` vs `127.0.0.1` | 2046.8 ms vs 90.1 ms connect | always use the literal |
| clocks | `monotonic`/`perf_counter` = QPC 100 ns; `time()` = GetSystemTimePreciseAsFileTime; `sleep(0.01)` ≈ 10.4 ms | lock polling loops fine |
| symlink creation | worked here (Developer Mode or elevated); not guaranteed on user machines | tests must skip or use junctions |

---

## 2. Engine contracts and invariants the port must preserve

These are the semantics the durability and locking design relies on. Every Windows change must keep them.

### 2.1 Store lock protocol (storage.py:227-258)
- Two layers. A per-`Store` `threading.RLock` (`_mutex`) serializes threads using one Store object. Then an OS file lock on `<db>.sdb.lock` (`RadioS.sdb.lock`) serializes **handles**: other `Store` instances in the same process and other processes.
- **A new file descriptor is opened per acquisition** (`self.lock_file.open("rb")`), locked SH (readers) or EX (writers), and released with `LOCK_UN`.
- flock semantics relied on:
  - Locks belong to the open file description, so two fds in one process conflict.
  - SH+SH is compatible.
  - `LOCK_NB` contention raises `BlockingIOError`.
  - A dead process releases its locks.
  - `LOCK_UN` on an unlocked fd is harmless.
- **LockFileEx keeps these per-handle semantics** (verified).
- Nested acquisitions are deliberately avoided. replication.py:111-113 says a second flock fd while holding the lock deadlocks. Windows behaves identically: a new handle conflicts. **Same-handle re-locking is NOT idempotent on Windows** (error 33), unlike flock. No engine call site re-locks the same fd or converts SH↔EX; the shim documents that constraint.
- Bounded wait: `LOCK_NB` polling every 10 ms until `lock_timeout` (default 30 s; 0.15 s in engine_acceptance), then `LockTimeout` and the `lock_timeouts` metric. `lock_timeout=None` makes a blocking flock.
- The lock file is **never written, replaced or deleted**. It is opened read-only. LockFileEx accepts a GENERIC_READ handle for EX locks (verified).
- Other lock files using the same protocol:

| Lock file | Defined at | Mode |
|---|---|---|
| `.build.lock` | disk_index.py:156-165 | EX\|NB |
| `owner.lock` | endpoint_journal.py:37-42 | EX\|NB, held for the journal's lifetime |
| `.<target>.sdb-migration.lock` | migration.py:54-74 | EX\|NB, `st_nlink==1` |
| `supervisor.lock`, `runner.lock` | failover.py:115-128, 329-344 | EX\|NB, `st_nlink==1` |
| `maintenance.lock` | online_maintenance.py:34-54 | EX\|NB, `st_nlink==1` |
| `scheduler.lock` | ops_scheduler.py:146-164 | EX\|NB |
| `server.lock` | replication_stream.py:146-165 | EX\|NB, lease |
| `follower.lock`, `runner.lock` | replication_stream.py:610-622 | EX\|NB |
| engine lock, SH\|NB polled 30 s | integrity.py:84-94 | SH |
| engine lock, EX\|NB polled 30 s | replication.py:688-698 | EX |
| engine lock, EX\|NB (store mutex + new fd) | disk_index.py:132-152 | EX |

- **The same lock files are shared with other languages and areas.** These must all use the *identical* byte range (§4.3):
  - `tools/radios_reliability.py:365-374` takes EX\|NB on `RadioS.sdb.lock`, `server.lock` and `gui.lock`.
  - `tools/radios_database.py:592-598` uses `server.lock`.
  - The C GUI holds `data/database-service/gui.lock` SH (src/core/recovery.c:367-369).
  - `tests/test_application_shutdown.py:29-39` probes it with EX\|NB.

### 2.2 Commit protocol (storage.py:634-755)
EX lock → `_assert_writable_locked` (role/epoch fence) → `_reload_locked(repair_tail=True)` → `archive_pending_locked` → validation → `frame = >I length + canonical JSON + SHA-256` → `journal.open("ab")`, write, `flush`, **`os.fsync`** → apply in memory → `archive_transaction_locked`. With WAL archival enabled, that last step writes `records/<seq>.json` (temp+fsync, then no-replace link) **and replaces `head.json` on every commit**. The commit is durable at the journal fsync; a WAL failure raises `ArchiveAcknowledgementError` without undoing the commit.

### 2.3 Checkpoint ordering (storage.py:547-576, retention.py:413-426, replication.py:434-444, wal_archive.py:888-896)
1. `_atomic_json(checkpoint)`: temp `xb`, fsync, `os.replace`, **dir fsync**.
2. Truncate the journal to 0 and fsync.
3. `_atomic_json(meta)` with `generation += 1`.

Crash safety needs the checkpoint rename to be durable **before** the truncation is durable; otherwise the result is the old checkpoint plus an empty journal, i.e. lost commits. On Linux the dir fsync in step 1 guarantees this. §7 gives the NTFS argument and the shim's best-effort directory flush.

### 2.4 Online maintenance (online_maintenance.py:139-243)
Capture under SH → write `checkpoint.pending` outside the lock → under EX, verify identity and stamp, copy the journal suffix into `journal.pending` → `os.replace(checkpoint.pending, checkpoint)` → dir fsync → `os.replace(journal.pending, **journal**)` → dir fsync → meta generation++. A crash between the two replaces is safe: the new checkpoint plus the full old journal replays fine (frames ≤ checkpoint sequence are skipped, storage.py:462-467). The `_after_publish(phase)` fault-injection seam (line 96) must stay.

### 2.5 Publication patterns (all must keep "complete or absent")
- **Atomic file replace:** `_atomic_json` (storage.py:121-135) is temp in the same dir (`.<name>.<uuid>.tmp`), `xb`, fsync, replace, dir fsync, then `finally: temp.unlink(missing_ok=True)`.
- **No-replace directory publish:** stage via `tempfile.mkdtemp` in the target's parent, fill, `_fsync_tree`, then `_publish_absent` (renameat2 RENAME_NOREPLACE). `EEXIST` maps to `BackupError("target directory appeared during publication")`.
- **No-replace file publish:** temp, fsync, `os.link(temp, final, follow_symlinks=False)` (raises `FileExistsError` if final exists), dir fsync, `finally: temp.unlink`. Sites: wal_archive.py:652-660 and :693-703, replication.py:545-559.
- **Content-addressed replace:** media.py:179-187 replaces an existing object only when it is corrupt (same digest, validated new bytes).

### 2.6 Change detection
`meta.generation`, `journal_size < _offset` and `checkpoint_stamp = (st_ino, st_size, st_mtime_ns)` (storage.py:418-429, 572-574; online_maintenance.py:183-192, 225-229). On Windows `st_ino` is the NTFS file id and **does change on replace** (verified), and `st_mtime_ns` has 100 ns resolution. The stamps are only held in memory (never persisted), so they need no portability. `disk_index._verify`/`_stamp` (disk_index.py:406-425, 494-501) add `st_ctime_ns`, which is creation time on Windows. That is weaker than inode-change time but adequate together with size, mtime and file id.

### 2.7 Safety checks
- Every path must be symlink-free: `is_symlink()` on components, `O_NOFOLLOW` opens, `S_ISREG` after open, `st_nlink == 1` for lock files.
- Manifests use relative POSIX paths checked against `""`, `"."`, `".."` and absolute paths.
- `stat(follow_symlinks=False)` plus `(st_dev, st_ino)` re-check before rmtree (migration.py:129-137).
- `before`/`after` `fstat` identity on copy sources (media.py:160-170, backup.py:112-124, integrity.py:328-365).

### 2.8 Recovery never guesses from PIDs
The only liveness signal is "can I take the kernel lock" (migration.py:55; docs/schema_migration.md:43). Windows releases LockFileEx locks when the handle closes or the process dies (verified after `Popen.kill()`). No engine module uses PIDs, `/proc`, signals or fork.

### 2.9 Paths stored inside engine artifacts are relative POSIX strings
These keep the formats host-independent and must stay `/`-joined f-strings (never `str(Path)`):

| Artifact | Path format | Defined at |
|---|---|---|
| backup manifest objects | `f"{database}.objects/{digest[:2]}/{digest}"` | backup.py:192 |
| retention segment files (stored **in the DB**) | `f"{database}.archives/{uuid}/records.jsonl"` | retention.py:358-361 |
| disk-index manifest | `f"{build_id}/{stem}.keys"` | disk_index.py:286-287 |
| WAL assets (hash-chained, replicated cross-host) | `assets/xx/digest` | wal_archive.py:376-377 |
| promotion intent (stored in meta) | `RadioS.sdb.replication/promotion-<hex>.json` | replication.py:913 |
| inventories | `relative_to(root).as_posix()` | backup.py:414, portable_export.py:299/303 |

These are consumed with `Path(...).parts` and must be parsed identically on both OSes (§3.9).

---

## 3. Findings by POSIX construct (what, where, why, and the Windows replacement)

### 3.1 `import fcntl` (module import) — 10 modules
Replace with `from . import _platform` and use `_platform.LOCK_SH/LOCK_EX/LOCK_NB/LOCK_UN` and `_platform.flock`. Keep the numeric values (1/2/4/8), because tests assert on `BlockingIOError` and use `fcntl.LOCK_*` bit-ors.

### 3.2 `fcntl.flock` (all sites in §2.1)
- Windows: `LockFileEx`/`UnlockFileEx` on `msvcrt.get_osfhandle(fd)` with `OVERLAPPED.Offset=0, OffsetHigh=0x7FFFFFFF`, `nNumberOfBytesToLockLow=1, High=0`.
- `LOCK_EX` adds `LOCKFILE_EXCLUSIVE_LOCK`; `LOCK_NB` adds `LOCKFILE_FAIL_IMMEDIATELY`.
- Error 33 (`ERROR_LOCK_VIOLATION`) or 997 (`ERROR_IO_PENDING`) becomes `BlockingIOError(errno.EAGAIN)`.
- `LOCK_UN` ignores 158 (`ERROR_NOT_LOCKED`). ops_scheduler.py:161-164 and replication_stream.py:621 call `LOCK_UN` even when the lock was never acquired.
- Why the high offset: Windows locks are **mandatory**. Locking byte 0 makes other handles' reads of that byte fail (verified). The region at 2^63−2^32 is beyond any data and below 2^63, so it stays safe for SMB and Samba.
- **Always unlock explicitly before `os.close`.** The docs say release on close "depends on available system resources", even though it was immediate here. Sites that rely on close-releases-lock must change to `_platform.unlock_and_close(fd)`: disk_index.py:150-151 and :165, migration.py:73-74, failover.py:128 and :344, online_maintenance.py:54, integrity.py:188.
- Pitfalls:
  - Same-handle EX re-lock fails (33), so do not call `flock(fd, LOCK_EX)` twice on one fd.
  - SH→EX conversion is not atomic on either OS. EX→SH on Windows is possible atomically (lock SH, then unlock once) but unused.
  - Blocking `LockFileEx` cannot be interrupted by Ctrl+C. It is only used when `lock_timeout=None`.
  - Some callers pass file objects (`fcntl.flock(stream, …)` in tools/tests), so the shim must accept `int` or `.fileno()`.

### 3.3 `os.replace` / rename over open files (§0 item 4)

| Site | What is replaced | Who may hold it open |
|---|---|---|
| storage.py:128 (`_atomic_json`) | meta, checkpoint, `head.json` (every commit with WAL), `job.json`, markers, disk-index `manifest.json`, `supervisor.json`, `owner.json`, `state.json`, `build.json` | unlocked readers in other processes or threads: failover.py:62-71 `_metadata`, replication_stream.py:602 `_workspace`, tools `setup._json_read(meta)` (radios_reliability.py:556), `wal_archive.read_archive_records` from `create_shipment` (replication.py:268) outside the store lock; plus AV, indexer and backup agents |
| media.py:185 | corrupt managed object | **C player (FFmpeg `_wsopen`, no FILE_SHARE_DELETE)**, `http_server.py:490` streaming it (`source.open("rb")` for the whole transfer) |
| online_maintenance.py:214 | `RadioS.sdb.checkpoint` | AV/backup only (store readers need the lock) |
| online_maintenance.py:219 | **`RadioS.sdb` journal** | AV/backup/OneDrive only (store readers need the lock) |
| ops_scheduler.py:212 | `scheduler-state.json` | status readers |
| replication_stream.py:720 | `incoming/…/<file>` | AV |

Replacement is `_platform.replace(src, dst, timeout=None)`:
1. Try `SetFileInformationByHandle(FileRenameInfoEx, FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS)` on a `DELETE|SYNCHRONIZE` handle to the source, opened with share-all, BACKUP_SEMANTICS and OPEN_REPARSE_POINT. This gives true POSIX replace when other openers allow FILE_SHARE_DELETE.
2. On winerror 1, 50 or 87 (FAT/exFAT/some SMB), remember the volume and fall back to `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.
3. Retry winerror 5/32/33/1224 with exponential backoff (1 ms up to a 50 ms step) until the deadline. The default is 5 s, overridable with env `SDBASE_FS_RETRY_SECONDS`.
4. Then re-raise the last `PermissionError`.

Never retry `FileNotFoundError`. A cross-volume replace raises (EXDEV-like), as on Linux. Verified: the retry succeeds 0.22 s after a reader closes, and it times out with winerror 32 while a Python `open()` reader persists.

Complement this with **share-delete readers** for engine-owned replaceable files (`_platform.open_regular` / `read_bytes_shared`). That makes the engine's own concurrent readers never block its writers.

Pitfalls:
- Holding the store EX lock while retrying stalls other clients for up to the timeout. Keep 5 s; the C client timeouts are ≥ 15 s.
- `media.py:185`: map the final `PermissionError` to `MediaError("managed object is in use (playback/stream); retry later")`.
- Never chmod targets read-only (verified failure).

### 3.4 Unlinking open files / rmtree
Windows fails while any handle lacks FILE_SHARE_DELETE, and while a file is mapped. Sites:

| Group | Sites |
|---|---|
| Cleanup in `finally:` | backup.py:279/721, disk_index.py:363, migration.py:431 (+ TemporaryDirectory at 360/460), online_maintenance.py:243, pitr.py:191, ops_archive.py:290, portable_export.py:286, portable_import.py:408, replication.py:212/332 (+ TemporaryDirectory 657), retention.py:373, wal_archive.py:588, backup.py:449 (TemporaryDirectory), store_provision.py:26 |
| Normal-path deletes | failover.py:214, disk_index.py:664, migration.py:138, online_maintenance.py:282, replication_stream.py:180/188/190/202/326/337/787/812 |

Replace with `_platform.rmtree(path, timeout=None, ignore_errors=False)`. It uses `shutil.rmtree(onexc=…)`, which clears FILE_ATTRIBUTE_READONLY and retries winerror 5/32 with backoff (verified on read-only and busy trees).

- In `finally:` cleanups that run while another exception propagates, call it with `ignore_errors=True` so the original error is not masked. Report or log the leftover stage. Every stage has an owner marker or name pattern, and the existing `recover_*` functions already clean those.
- `disk_index.recover_disk_index_builds` (disk_index.py:664): if rmtree fails with `PermissionError` (an obsolete build still mmapped by a live `ReadSnapshot` in this or another process), append the name to `skipped` instead of raising. The return shape stays unchanged.
- `replication_stream._BatchCache._remove` (line 202): on `PermissionError`, set `entry["delete"]=True` and let `sweep()` retry. `service_actions()` (line 498-499) runs `sweep()` inside `serve_forever`, and an exception there would kill the listener.
- disk_index.py:259/272: the `_merge_rows(...)` generators keep run files open if `_build_one` raises. Wrap them in `contextlib.closing(...)` so the stage rmtree can succeed.
- `TemporaryDirectory(...)` sites: on Windows use `_platform.temporary_directory(prefix, dir)` (private dir plus robust cleanup); POSIX keeps `tempfile.TemporaryDirectory`.

### 3.5 Directory fsync (7 implementations, ~30 call sites)
Implementations: storage.py:129-133 (inline in `_atomic_json`), media.py:66-71, disk_index.py:85-90, endpoint_journal.py:23-28 (`_fsync_directory`, **imported by endpoint_media.py:12**), backup.py:51-56 (`_fsync_dir`, imported by failover, online_maintenance, ops_archive, replication, replication_stream, wal_archive, migration, and `tools/radios_reliability.py`) and backup.py:59-61 (`_fsync_tree`), ops_scheduler.py:213-217 (inline), portable_export.py:55-60.

All become `_platform.fsync_dir(path)`, keeping each module's helper **name** as a thin wrapper. Behaviour:
- POSIX: unchanged (`O_RDONLY|O_DIRECTORY|O_CLOEXEC`, then fsync).
- Windows: best-effort `CreateFileW(dir, GENERIC_READ|GENERIC_WRITE, share-all, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS)` plus `FlushFileBuffers` (verified OK, 0.2 ms).
  - Missing directory: raise `FileNotFoundError`, as Linux does.
  - Access denied or unsupported (1/5/32/50/87): return False silently.
  - Env `SDBASE_DIR_FSYNC=0` makes it a pure no-op (the lead's default option). §7 argues durability either way.
- `_fsync_tree` keeps walking. A per-directory flush is cheap; `replication.apply_shipment` walks the whole standby root at line 623, which already does so on Linux.

### 3.6 `os.open` flags, text mode, O_NOFOLLOW / O_CLOEXEC / O_DIRECTORY / O_NONBLOCK
Engine sites, all using `getattr(os, "O_*", 0)`, so on Windows they silently lose NOFOLLOW and CLOEXEC and are text mode:
- backup.py:80-82 `_open_regular`: the hub used by integrity, online_maintenance (journal tail copy!), wal_archive (journal_transactions!), replication, replication_stream (media served over HTTP!), migration, failover, portable_import.
- disk_index.py:93-104 `_regular`.
- ops_archive.py:54-61 `_regular`.
- ops_scheduler.py:150-151 and :170.
- migration.py:60-61.
- failover.py:117 and :332.
- online_maintenance.py:43.
- replication.py:718 (`O_RDWR`, then `fdopen("r+b")` and **truncate**).
- replication_stream.py:146-147, :613 and :701 (`O_WRONLY|O_CREAT|O_APPEND`, then `fdopen("ab")`).
- endpoint_journal.py:37 and :45.

Replace them all with `_platform.os_open(path, flags, mode, nofollow=…)` or `_platform.open_regular(...)`:
- POSIX: `os.open(path, flags | O_CLOEXEC | (O_NOFOLLOW if nofollow))`, i.e. today's behaviour.
- Windows: `CreateFileW` plus `msvcrt.open_osfhandle`:
  - Always binary and non-inheritable.
  - Share mode **READ|WRITE|DELETE**, so the engine never blocks its own replaces or unlinks.
  - `nofollow` adds `FILE_FLAG_OPEN_REPARSE_POINT`, then checks `GetFileInformationByHandleEx(FileAttributeTagInfo)`. If the result is a name-surrogate reparse point, close and raise `OSError(errno.ELOOP)` (race-free, verified).
  - Dispositions: `O_CREAT|O_EXCL`→CREATE_NEW, `O_CREAT|O_TRUNC`→CREATE_ALWAYS, `O_CREAT`→OPEN_ALWAYS, `O_TRUNC`→TRUNCATE_EXISTING, else OPEN_EXISTING.
  - Access: `O_RDONLY`→GENERIC_READ, `O_WRONLY`→GENERIC_WRITE, `O_RDWR`→both.
  - `O_APPEND` is passed to `open_osfhandle`. The CRT then seeks to the end before each write; the engine has a single writer under a lock (verified `310a320a`).
  - Mode without `S_IWRITE` on create sets FILE_ATTRIBUTE_READONLY, as the CRT does.
  - Errors are built from `OSError(None, msg, filename, winerror)`, so Python maps them to the right subclass (verified: 2/3→FileNotFoundError, 5/32→PermissionError, 80/183→FileExistsError).
- `O_NONBLOCK` (used by tools and desktop_media, not by the engine) has no meaning for regular files on Windows: drop it.
- `O_DIRECTORY` is only used for the dir fsync, which is handled above.

### 3.7 `os.link(..., follow_symlinks=False)` (wal_archive.py:656, :700; replication.py:552)
Works on 3.13.16 but is not guaranteed (`os.link` is not in `os.supports_follow_symlinks`), and hard links fail on FAT/exFAT (USB backup or data drives). Use `_platform.link_noreplace(src, dst)`:
- POSIX: unchanged.
- Windows: `os.rename(src, dst)` with the retry loop. `MoveFileExW(flags=0)` is atomic, **never replaces**, and raises `FileExistsError` (183). That is the same exception the callers handle (replication.py:553). Because the source is consumed, the callers' `finally: temp.unlink(missing_ok=True)` becomes a no-op; on conflict the temp remains and is unlinked as before (verified).

### 3.8 `os.pread` (disk_index.py:416) and mmap (disk_index.py:442-443)
- Use `_platform.pread(fd, n, offset)`. On Windows this is `ReadFile` with `OVERLAPPED` offsets on the OS handle; it is CRT-mode independent and returns `b""` on `ERROR_HANDLE_EOF` (verified).
- `mmap.mmap(fd, 0, access=ACCESS_READ)` works unchanged. Mapped files cannot be deleted or replaced: see §3.4. New builds never overwrite mapped files (a new build directory per run), so publication is not blocked; only cleanup is.
- `ReadSnapshot.close()` (read_snapshot.py:164-173) releases readers and must stay. All current callers use `with`.

### 3.9 Path parsing, separators, reserved names, case, length

**Manifest-relative parsers.** These consume untrusted input: backups from elsewhere and **network replication bundles**.
- backup.py:64-76 `_safe_source` and backup.py:387-390 cold-path parts.
- disk_index.py:117-128 `_safe` and :393 `parts[0]`.
- replication.py:215-234 `_asset_path` and :811-816 `_read_promotion_intent`.
- wal_archive.py:392 and :429-431.

Replace them with `_platform.safe_relative_parts(relative)`, a single platform-independent parser:
- Reject empty strings, NUL, `\`, `:`, absolute paths, and empty, `.` or `..` components (`a//b` and a trailing `/` are rejected).
- Reject components with a trailing dot or space, and `os.path.isreserved` names on Windows (verified on 11 cases).
- Generated paths are always canonical, so this stricter rule loses nothing. Linux and Windows then accept exactly the same artifacts, which matters because replication is cross-host. Join with `root.joinpath(*parts)`.

**Single-name checks.**
- backup.py:310-312 (`database` filename; already rejects `/` and `\`) must also reject `:` and reserved names.
- wal_archive.py:345 and replication.py:362 use `Path(x).name == x`. On Windows a `\` splits, so the check already rejects it, but add an explicit `"\\" in x or ":" in x` check so Linux matches.

**New target names** (`backup._destination` backup.py:138-151, `migration._target_identity` :40-50). Add `_platform.valid_new_name(raw.name)`, rejecting reserved names, `<>:"|?*`, control characters, and trailing dot or space. Windows silently strips trailing dots, which would break `os.path.lexists` reasoning and the `.{name}.` stage prefixes.

**Case-insensitivity.**
- Engine-generated names are lowercase hex, UUIDs or fixed ASCII, so there are no collisions.
- `portable_export` writes `collections/{name}.jsonl` for collection names matching `[A-Za-z][A-Za-z0-9_]*` (portable_export.py:162/171). Add a casefold-collision check and `isreserved`, raising `PortableExportError` instead of a raw `FileExistsError` from `open("xb")`.
- `PureWindowsPath` equality and `is_relative_to` are case-insensitive (verified), which is safe for the containment checks: migration.py:340/350/445/456, backup.py:667, pitr.py:123, media.py:128, failover.py:82.
- Turkish edge case: NTFS uppercases `ı` and `i` to the same `I`, while Python's `str.lower` does not fold them. This is harmless here; it is documented in §10.

**`Path.resolve()` / realpath**:

| Call site | Line |
|---|---|
| `Store.__init__` (`requested = Path(root).resolve()`) | storage.py:160 |
| `failover._path` | failover.py:59 |
| `migration._source_database` | migration.py:154 |
| `verify_backup` | backup.py:434 |
| `wal_archive._root` | wal_archive.py:355 |
| `media._validate_source` | media.py:124-125 |

On Windows these resolve symlinks *and junctions*, and on mapped or `subst` drives they may return a `\\server\share\…` UNC form. `str(object_path(...))` is returned to the C client as `path`/`managed_path`, and the C side checks containment under `<database_file>.objects\` (src/playout/planner.c:309). This is cross-area: both sides must canonicalize the same way, compare case-insensitively, and accept that a mapped drive becomes UNC. Optional engine mitigation: on Windows, if `resolve()` changes the drive anchor, keep the `os.path.abspath` form after verifying `is_link_like` is False on every component.

**MAX_PATH** (long paths are **off** on this host):
- Longest generated paths are about 120 characters below the DB directory, e.g. `RadioS.sdb.indexes\.build-<uuid36>\.<sha64>.merge-<lvl>-<off>` and `RadioS.sdb.replication-stream\incoming\RadioS.sdb.objects\ab\<sha64>.part`.
- With the original root `C:\Users\sonatturkan\Desktop\radyo projesi\kaynak kodlar\RadioS\data\database` (78 characters) these reach ~210-220. Keep `<db dir>` ≤ 120 characters, or enable `LongPathsEnabled` (python.exe's manifest is already longPathAware).
- Engine startup should log a warning when `not _platform.long_paths_enabled()` and `len(str(store.root)) > 120`.
- `os.path.realpath` may return `\\?\`-prefixed paths beyond MAX_PATH, and the C side must accept them.

**Timestamps in names.** `ops_scheduler._new_path` uses `%Y%m%dT%H%M%S%fZ`, with no `:` (fine). No engine filename contains `:`.

### 3.10 `os.statvfs` (store_health.py:115-120, capacity_monitor.py:181-191)
Use `_platform.disk_usage(path)`, which returns `(total, available)`. On Windows this is `GetDiskFreeSpaceExW` with `lpFreeBytesAvailableToCaller` (quota-aware, the equivalent of `f_bavail`). Do **not** use `shutil.disk_usage().free` on Windows: it is total free space, not available-to-caller. Callers keep `except OSError`.

### 3.11 Permissions: `chmod`/`fchmod`/`umask`/mode checks
- **Harmless on Windows:**
  - `os.chmod(x, 0o600)`: online_maintenance.py:84/106, backup.py:114/212, replication.py:300, retention.py:345, wal_archive.py:696, ops_archive.py:205/230, portable_export.py:149/164/173, replication_stream.py:128/796.
  - `os.chmod(dir, 0o700)`: replication_stream.py:129, ops_archive.py:266, portable_export.py:239, portable_import.py:364.
  - `touch(mode=0o600)`: backup.py:456, replication.py:200, replication_stream.py:716. These only toggle FILE_ATTRIBUTE_READONLY when `S_IWRITE` is absent, which never happens here. Keep them.
  - `os.fchmod(fd, 0o600)` (ops_scheduler.py:207): available on 3.13; guard with `hasattr`.
- **Privacy creation**: replace with `_platform.mkdir_private` and `_platform.mkdtemp_private`. This avoids the OWNER RIGHTS/elevation trap (§0 item 11). On Windows they create a protected DACL `D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;<current user SID>)` with owner set to the user SID, verified with icacls. POSIX keeps `mkdir(0o700)` and `tempfile.mkdtemp`.
  - `mkdir(mode=0o700)`: disk_index.py:324/329, failover.py:90 (leaf only; parents default), online_maintenance.py:42/156, replication.py:159/915, replication_stream.py:117/788, retention.py:338, wal_archive.py:577/578/651, portable_export.py:242.
  - `tempfile.mkdtemp`: backup.py:228/674, migration.py:372, ops_archive.py:264, pitr.py:131, portable_export.py:237, portable_import.py:362, replication.py:155/292, retention.py:340, wal_archive.py:569.
- **Privacy check**: ops_scheduler.py:138-140 becomes `_platform.private_dir_ok(root)`.
  - POSIX: today's `S_ISDIR and not mode & 0o077`.
  - Windows: `GetNamedSecurityInfoW(OWNER|DACL)` requirements:
    - The owner must be the current user, SYSTEM or Administrators.
    - A NULL DACL fails.
    - Every *effective* `ACCESS_ALLOWED` ACE (inherit-only ACEs are skipped) must name the user, SYSTEM (S-1-5-18), Administrators (S-1-5-32-544), CREATOR OWNER (S-1-3-0), TrustedInstaller, or OWNER RIGHTS (S-1-3-4) when the owner is allowed. Otherwise the check fails unless the mask is only SYNCHRONIZE.
  - Verified: the private and inherited user-profile directories pass; `Users:R` and `Everyone:F` fail.
  - Update docs/ops_scheduler.md and docs/ops_archive.md (they say "0700").
- `st_uid`/`getuid`: none in the engine; the tools have them (cross-area).

### 3.12 Symlink and junction checks (`is_symlink` → `_platform.is_link_like`)
`is_link_like(path)` is lstat-based: `S_ISLNK`, or (FILE_ATTRIBUTE_REPARSE_POINT and the reparse tag has the name-surrogate bit `0x20000000`). That covers symlinks, junctions and mount points but **not** OneDrive placeholders or dedup files, so cloud-synced folders keep working. For `os.DirEntry`, use `_platform.entry_is_link_like(entry)`.

Sites:
- storage.py:200
- media.py:177, 329, 377, 459, 499, 502
- disk_index.py:109, 112, 122, 126, 647
- failover.py:57, 178, 205
- integrity.py:420
- migration.py:102, 136, 152, 160
- online_maintenance.py:40, 259, 274
- ops_scheduler.py:127, 132, 135, 203
- ops_archive.py:76
- replication.py:337, 681
- replication_stream.py:115, 171
- retention.py:336
- store_health.py:33
- wal_archive.py:353, 364
- backup.py:70, 74, 415 (`entry.is_symlink()`), 432
- portable_export.py:290, 297. The `os.walk(followlinks=False)` at line 294 descends into junctions, so filter `subdirs` in place with `is_link_like`.

Ancestor checks (`any(part.is_symlink() for part in (x, *x.parents))`) also see junctions now. The paths are usually already resolved, so ancestors are real directories; a junction-relocated install is reported as "unsafe", which is a deliberate safety parity. §10 calls out this decision.

### 3.13 Newline translation and text files
- Engine data files are written in binary with explicit `b"\n"`: `_json_bytes`, JSONL in retention.py:347, ops_archive.py:207, portable_export.py:166-167, manifests in backup.py:209 and ops_archive.py:228. **Do not change** them: SHA-256 digests cover exact bytes.
- CSV is written with `newline="", lineterminator="\n"` (portable_export.py:172-174) and read with `newline=""` (:342-343), so it is LF on Windows too. Keep it.
- Exception: replication_stream.py:795-799 writes `manifest.sha256` with `open("w", encoding="ascii")`, producing CRLF. It is not hashed (the reader `.strip()`s at replication.py:343), but write it binary for byte-identical artifacts.
- `read_text(encoding="utf-8")` of JSON (storage.py:262/307, pitr.py:50/140) is fine. Canonical JSON never contains raw CR or LF.
- endpoint_journal.py:104: see §0 item 7. Old Windows-written journals with `\r\n` stay readable, because `json.loads` tolerates trailing whitespace and the hash is computed over re-serialized JSON.

### 3.14 Endianness and binary formats
Journal frames use `struct.pack(">I")` plus a 32-byte SHA-256 (storage.py:142); disk-index headers use `">8sQQ"` and `">Q"` (disk_index.py:35-36). Both are explicit big-endian, so x64 Windows reads Linux files unchanged (verified on the real DB).

### 3.15 Signals, fork, /proc, PIDs
Not used by the engine modules. Long-running loops take a `threading.Event`: `run_online_maintenance(stop_event)`, `ops_scheduler.run_locked_loop(config, stop)`, `FailoverSupervisor.watch/run(stop_event)`, `follow_forever(stop_event=…)`, plus `serve_replication`'s `serve_forever`, which stops via `server.shutdown()`. On Windows the service wrapper (tools area) must set these events from a named event or console-control handler. SIGTERM cannot be delivered to a Python process on Windows (`os.kill` is TerminateProcess). `database/SDBASE/tools/ops_scheduler.py:95-105` and `stream_acceptance.py:37` install SIGTERM handlers, and `disk_index_acceptance.py:205` sends `signal.SIGKILL`, which does not exist on Windows; use `process.kill()`.

### 3.16 Sockets used by engine modules
- `failover_server.serve_failover` (failover_server.py:100) and `replication_stream.ReplicationStreamServer` (replication_stream.py:480-485) use `api_server.BoundedHTTPServer`, a `ThreadingHTTPServer`. On Windows, override `server_bind` to set `allow_reuse_address = False` and `setsockopt(SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1)` (verified). This file belongs to the HTTP area, but these two engine servers depend on it.
- `failover._probe` (failover.py:152), `http_pool.HTTPConnectionPool` (http_pool.py:84-85), `transport.HTTPTransport` and `replication_stream._Client` connect to configured hosts. Document and enforce `127.0.0.1`. Optionally, map a plain-HTTP `localhost` to `127.0.0.1` inside `HTTPConnectionPool._acquire`. Not for HTTPS, where certificate host verification needs the name. With `probe_timeout_seconds=1.0`, a `localhost` URL makes every failover probe cost about 1-2 s.
- `select`-based polling (`server.timeout=0.5`, `handle_request()`) works on Windows sockets.

### 3.17 Third-party interference (operational)
- Windows Defender, the Search indexer and backup or OneDrive sync agents open just-written files and the 700 MB journal. They cause sharing violations (handled by retries) and severe per-commit latency, because the journal is opened and closed for every commit and read.
- Installer guidance (packaging area):
  - Keep `data\` **outside OneDrive-synced folders**. On many machines Desktop is OneDrive-redirected; warn if `%OneDrive%` is a prefix of the data root.
  - Set FILE_ATTRIBUTE_NOT_CONTENT_INDEXED on `data\database`.
  - Recommend a Defender exclusion for `data\database` (admin).
- **Do not keep persistent journal handles** to save opens. A POSIX-semantics replace (online maintenance) would leave such a handle pointing at the unlinked old journal, and appends would be lost. Per-operation open/close stays.

---

## 4. Shim design: `database/SDBASE/src/sdbase/_platform.py`

The shim is a single module with **no imports from the rest of `sdbase`**, so tools can load it standalone by file path. Its POSIX branches reproduce today's calls exactly, so Linux keeps working unchanged. The reference implementation is in Appendix A and was **executed and validated on this host** (see proto-result.json). The Appendix A text was also extracted verbatim and smoke-tested (`...\scripts\appendix_check.py`) under **MSYS2 UCRT64 CPython 3.14.8**. That is the interpreter CMake's `Python3_EXECUTABLE` is likely to pick for CTest; the product runtime is the 3.13 embeddable. The engine tests should be run under both.

Minimum interpreter: **CPython 3.12** (`shutil.rmtree(onexc=)`, `DirEntry.is_junction`, `Path.is_junction`, 64-bit `st_dev`/`st_ino` on Windows). `os.path.isreserved` is 3.13+. Write `_isreserved = getattr(os.path, "isreserved", None) or _fallback_isreserved`, where the fallback checks a trailing dot or space, `:`, and a base name (before the first `.`, case-insensitive) in `CON PRN AUX NUL COM1-9 LPT1-9 CONIN$ CONOUT$`.

### 4.1 API (signatures, semantics, errors)

| Function / constant | POSIX behaviour | Windows behaviour | Errors |
|---|---|---|---|
| `IS_WINDOWS` | `os.name == "nt"` | | |
| `LOCK_SH=1, LOCK_EX=2, LOCK_NB=4, LOCK_UN=8` | same as fcntl | same values | |
| `WIN_LOCK_OFFSET = 0x7FFFFFFF_00000000`, `WIN_LOCK_LENGTH = 1` | unused | canonical lock region (shared with C and tools) | |
| `O_BINARY, O_NOINHERIT, O_CLOEXEC, O_NOFOLLOW, O_DIRECTORY, O_NONBLOCK` | `getattr(os, name, 0)` | same | |
| `open_flags(base, *, nofollow=False) -> int` | `base\|O_CLOEXEC\|(O_NOFOLLOW)` | `base\|O_BINARY\|O_NOINHERIT` | |
| `os_open(path, flags, mode=0o777, *, nofollow=False) -> int` | `os.open(path, open_flags(...), mode)` | CreateFileW share R/W/D + `open_osfhandle` (binary, non-inheritable); nofollow via OPEN_REPARSE_POINT + tag check | `OSError` subclasses; `OSError(ELOOP)` for a link with nofollow |
| `open_regular(path, *, write=False, create=False, mode=0o600, nofollow=True) -> int` | os_open + `S_ISREG(fstat)` | same | `OSError(EINVAL, "not a regular file")` |
| `read_bytes_shared(path, *, limit=None) -> bytes` | `open_regular` + read | does not block a concurrent replace | as above |
| `fsync_dir(path) -> bool` | open dir + fsync | best-effort FlushFileBuffers (GENERIC_WRITE + BACKUP_SEMANTICS); `SDBASE_DIR_FSYNC=0` gives a no-op | `FileNotFoundError` only |
| `fsync_tree(root) -> None` | walk bottom-up + `fsync_dir` | same | |
| `replace(src, dst, *, timeout=None) -> None` | `os.replace` | FileRenameInfoEx POSIX → MoveFileExW(REPLACE\|WRITE_THROUGH) fallback; retry 5/32/33/1224 up to `timeout` (default `SDBASE_FS_RETRY_SECONDS`=5) | last `PermissionError`; no retry for others |
| `publish_dir_noreplace(src, dst, *, timeout=None) -> None` | renameat2(RENAME_NOREPLACE) via `ctypes.CDLL(None)` (moved from backup.py) | `os.rename` (never replaces) + retry 5/32 | `FileExistsError` if dst exists; `OSError(ENOSYS)` if renameat2 is missing |
| `link_noreplace(src, dst, *, timeout=None) -> None` | `os.link(src, dst, follow_symlinks=False)` | `os.rename` + retry (src consumed) | `FileExistsError` |
| `flock(fd_or_file, operation) -> None` | `fcntl.flock` | LockFileEx/UnlockFileEx on the canonical region | `BlockingIOError(EAGAIN)` on NB contention; `ValueError` for a bad op; `LOCK_UN` of an unlocked handle is a no-op |
| `lock(fd, *, exclusive, timeout=None, poll=0.01) -> None` | flock / NB polling | same | `TimeoutError` |
| `unlock_and_close(fd) -> None` | `LOCK_UN` + close | same | |
| `locked_file(path, *, exclusive, timeout=None, create=False, write=False, mode=0o600)` (context manager → fd) | open_regular + lock | same | |
| `pread(fd, length, offset) -> bytes` | `os.pread` | ReadFile + OVERLAPPED; EOF gives `b""` | `OSError` |
| `disk_usage(path) -> (total, available)` | statvfs `f_blocks*f_frsize, f_bavail*f_frsize` | GetDiskFreeSpaceExW (available-to-caller) | `OSError` |
| `is_link_like(path) -> bool` / `entry_is_link_like(entry) -> bool` | `S_ISLNK` | + name-surrogate reparse points (junction, mount point) | never raises (False) |
| `safe_relative_parts(relative) -> tuple[str, ...]` | strict POSIX parser (§3.9) | + `isreserved` | `ValueError` |
| `valid_new_name(name) -> bool` | not empty/`.`/`..`, no NUL `/` `\` | + no `<>:"\|?*`, controls, trailing dot/space, reserved | |
| `rmtree(path, *, timeout=None, ignore_errors=False) -> bool` | `shutil.rmtree` | onexc: clear READONLY, retry 5/32 | `OSError` unless `ignore_errors` |
| `mkdir_private(path, *, exist_ok=False)` | `os.mkdir(path, 0o700)` | CreateDirectoryW with an explicit protected DACL (SYSTEM, Administrators, user SID; owner = user) | `FileExistsError` |
| `mkdtemp_private(prefix="tmp", suffix="", dir=None) -> str` | `tempfile.mkdtemp` | random 8 hex + `mkdir_private` | |
| `temporary_directory(prefix="tmp", suffix="", dir=None)` (context manager → str) | `tempfile.TemporaryDirectory` | `mkdtemp_private` + `rmtree(ignore_errors=True)` on exit | |
| `private_dir_ok(path) -> bool` | `S_ISDIR and not mode & 0o077` | DACL/owner inspection (§3.11) | `OSError` from the API |
| `current_user_sid() -> str` | n/a | TokenUser → string SID | |
| `long_paths_enabled() -> bool` | True | `RtlAreLongPathsEnabled()` | |

Tools-area functions that belong in the same shim (not used by the engine; the specification is owned by the tools/launcher spec):
- `process_identity(pid) -> tuple[int, int] | None` (pid plus creation FILETIME via `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `GetProcessTimes`).
- `owner_ok(path) -> bool`, replacing `st_uid == getuid()` (owner SID == current user SID).
- `named_stop_event(name) -> threading.Event`, a watcher thread on `CreateEventW("Local\\RadioS-<sha256(root)[:16]>-<service>-stop")`.
- `signal_stop(name) -> bool`.

### 4.2 Error mapping rules
- Build Windows errors as `OSError(None, ctypes.FormatError(code), filename, code, filename2)`. Python maps `winerror` to `errno` and the exception subclass (verified: 2/3→FileNotFoundError, 5/32→PermissionError, 80/183→FileExistsError). Existing `except FileNotFoundError` / `except FileExistsError` / `except OSError` clauses therefore keep working.
- Lock contention must be `BlockingIOError`. Engine code and tests catch exactly that: storage.py:246, disk_index.py:144/161, migration.py:69, failover.py:124/340, integrity.py:91, online_maintenance.py:50, ops_scheduler.py:157, replication.py:695, replication_stream.py:150/617, endpoint_journal.py:40.

### 4.3 Lock region contract shared with C and tools (must be identical)
`OVERLAPPED.Offset = 0x00000000`, `OVERLAPPED.OffsetHigh = 0x7FFFFFFF`, `nNumberOfBytesToLockLow = 1`, `nNumberOfBytesToLockHigh = 0`.
- Shared: no flags. Exclusive: `LOCKFILE_EXCLUSIVE_LOCK`. Non-blocking: `LOCKFILE_FAIL_IMMEDIATELY`.
- C needs `include/radios/platform.h` constants `RADIOS_LOCK_OFFSET_LOW 0x00000000u`, `RADIOS_LOCK_OFFSET_HIGH 0x7FFFFFFFu`, `RADIOS_LOCK_LENGTH 1u`.
- Tools load `sdbase/_platform.py` by path (`importlib.util.spec_from_file_location("radios_sdbase_platform", root/"database/SDBASE/src/sdbase/_platform.py")`) so the constants exist once in Python.
- Add a cross-language test: C holds SH on a temp `gui.lock`; Python `flock(EX|NB)` must raise `BlockingIOError`; after C unlocks, it must succeed.

### 4.4 Configuration knobs (env)
- `SDBASE_FS_RETRY_SECONDS` (default 5): replace/rename/rmtree retry budget.
- `SDBASE_DIR_FSYNC` (default 1): set 0 for a pure no-op directory flush on Windows.

---

## 5. Migration table (file:line → change)

Use the shim through each module's **existing private helper names**. Tools and tests monkeypatch and import them (§6), so keep the names and change only the bodies.

### storage.py
| Line(s) | Today | Change |
|---|---|---|
| 5-7 | docstring "Linux advisory locks" | "OS byte-range locks (flock / LockFileEx)" |
| 17 | `import fcntl` | `from . import _platform` |
| 128 | `os.replace(temp, path)` | `_platform.replace(temp, path)` |
| 129-133 | `os.open(path.parent, O_RDONLY)` + fsync | `_platform.fsync_dir(path.parent)` |
| 135 | `temp.unlink(missing_ok=True)` | keep (temp is closed) |
| 160-166 | `Path(root).resolve()`, `suffix == ".sdb"` | keep; optional Windows: `suffix.lower()` and the drive-anchor rule in §3.9 |
| 200-202 | `path.is_symlink()` | `_platform.is_link_like(path)` |
| 236 | `self.lock_file.open("rb")` | keep (or `os.fdopen(_platform.open_regular(self.lock_file), "rb")`) |
| 237, 239, 244, 256 | `fcntl.LOCK_*`, `fcntl.flock` | `_platform.LOCK_*`, `_platform.flock` (keep the explicit `LOCK_UN` before close) |
| 418-429, 572-574 | `st_ino/st_size/st_mtime_ns` stamp | keep |
| 430-455, 563-566, 744-747 | journal `r+b`/`ab`, truncate, fsync | keep (see §7 ordering) |

### media.py
| Line(s) | Change |
|---|---|
| 66-71 | `_fsync_dir` body becomes `_platform.fsync_dir(path)` |
| 133-142 | `_stage_object(store, source: Path \| int)`: an `int` is an already-opened no-follow fd (replacement for desktop_media.py:624's `/proc/self/fd/N`); `_stage_object_locked` copies from `os.fdopen(source, "rb", closefd=False)` after `os.lseek(source, 0, 0)`, with the same `fstat` before/after identity check |
| 155 | `source.open("rb")` for a `Path`: keep; map `PermissionError` to `MediaError("source is locked by another program")` |
| 177, 329, 377, 459, 499, 502 | `is_symlink()` → `_platform.is_link_like()` |
| 185 | `os.replace(temp, target)` → `_platform.replace(temp, target)`; on `PermissionError` raise `MediaError("managed object is in use; retry after playback/stream ends")` |
| 186, 148, 176 | `_fsync_dir` (via the helper) |

### disk_index.py
| Line(s) | Change |
|---|---|
| 13 | `import fcntl` → `from . import _platform` |
| 85-90 | `_fsync_dir` → `_platform.fsync_dir` |
| 93-104 | `_regular`: `_platform.open_regular(path, write=writable, create=create, mode=0o600)`, mapping `OSError` → `DiskIndexUnavailable` (keep messages) |
| 109, 112, 122, 126, 647 | `is_symlink` → `is_link_like` |
| 117-128 | `_safe`: `parts = _platform.safe_relative_parts(relative)` (`ValueError` → `DiskIndexUnavailable("unsafe disk index component path")`), then join |
| 142, 160 | `fcntl.flock(fd, LOCK_EX\|LOCK_NB)` → `_platform.flock(fd, _platform.LOCK_EX \| _platform.LOCK_NB)` |
| 150-151, 165 | `os.close(fd)` → `_platform.unlock_and_close(fd)` (only if locked; otherwise close) |
| 259, 272 | wrap `_merge_rows(...)` in `contextlib.closing` |
| 324, 329 | `mkdir(mode=0o700)` → `_platform.mkdir_private(root, exist_ok=True)` / `_platform.mkdir_private(stage)` |
| 356 | `os.rename(stage, root / run_id)` → `_platform.publish_dir_noreplace(stage, root / run_id)` |
| 363 | `shutil.rmtree(stage)` → `_platform.rmtree(stage, ignore_errors=True)` |
| 393 | `Path(entry["path"]).parts[0]` → `_platform.safe_relative_parts(entry["path"])[0]` |
| 416 | `os.pread` → `_platform.pread` |
| 664 | `shutil.rmtree(candidate)` → try `_platform.rmtree(candidate)`; `PermissionError` → `skipped.append(candidate.name)`, not `removed` |

### endpoint_journal.py
| Line(s) | Change |
|---|---|
| 5 | `import fcntl` → `from . import _platform` |
| 23-28 | `_fsync_directory` → `_platform.fsync_dir` (**keep the name**; endpoint_media.py:12 imports it) |
| 37 | `os.open(root/"owner.lock", O_CREAT\|O_RDWR, 0o600)` → `_platform.os_open(..., os.O_CREAT \| os.O_RDWR, 0o600, nofollow=True)` |
| 39, 65 | flock → `_platform.flock` |
| 45 | `os.open(self.path, O_CREAT\|O_RDWR\|O_APPEND, 0o600)` → `_platform.os_open(...)` (binary: **fixes the CRLF bug**) |
| 76, 77, 104, 108 | keep (`ftruncate`/`write`/`fsync` work on the binary fd) |

### migration.py
| Line(s) | Change |
|---|---|
| 12 | drop fcntl |
| 40-50 | `_target_identity`: reject `not _platform.valid_new_name(raw.name)` |
| 60-61 | `os.open(...)` → `_platform.os_open(path, os.O_CREAT \| os.O_RDWR, 0o600, nofollow=True)` |
| 68 | `_platform.flock` |
| 73-74 | `_platform.unlock_and_close(descriptor)` |
| 102, 136, 152, 160 | `is_link_like` |
| 138, 431 | `_platform.rmtree` (431: `ignore_errors=True`) |
| 360, 460 | `tempfile.TemporaryDirectory(...)` → `_platform.temporary_directory(...)` |
| 372-373 | `tempfile.mkdtemp` → `_platform.mkdtemp_private` |

### failover.py
| Line(s) | Change |
|---|---|
| 10 | drop fcntl |
| 57, 178, 205 | `is_link_like` |
| 90 | `self.root.parent.mkdir(parents=True, exist_ok=True)`; `_platform.mkdir_private(self.root, exist_ok=True)` |
| 117, 332 | `_platform.os_open(..., os.O_CREAT \| os.O_RDWR, 0o600, nofollow=True)` |
| 123, 339 | `_platform.flock` |
| 128, 344 | `_platform.unlock_and_close(fd)` |
| 214 | `_platform.rmtree(bundle)` |
| 152 | keep (configuration must use `127.0.0.1`) |

### integrity.py
| Line(s) | Change |
|---|---|
| 12 | drop fcntl |
| 86-94 | replace the loop with `_platform.lock(lock_fd, exclusive=False, timeout=30)`; `TimeoutError` → `_Incomplete("source lock wait exceeded 30 seconds")` |
| 188 | `_platform.unlock_and_close(lock_fd)` |
| 420 | `is_link_like` |

### online_maintenance.py
| Line(s) | Change |
|---|---|
| 11 | drop fcntl |
| 40, 259, 274 | `is_link_like` |
| 42, 156 | `_platform.mkdir_private(root, exist_ok=True)` / `_platform.mkdir_private(stage)` |
| 43 | `_platform.os_open(root/"maintenance.lock", os.O_RDWR \| os.O_CREAT, 0o600, nofollow=True)` |
| 49 | `_platform.flock` |
| 54 | `_platform.unlock_and_close(fd)` |
| 214, 219 | `os.replace` → `_platform.replace` (219 is the live journal; keep the order) |
| 243 | `_platform.rmtree(stage, ignore_errors=True)` |
| 282 | `_platform.rmtree(stage)` |

### ops_scheduler.py
| Line(s) | Change |
|---|---|
| 14 | drop fcntl |
| 127, 132, 135, 203 | `is_link_like` |
| 138-140 | `if not _platform.private_dir_ok(root): raise SchedulerError("output_dir must be accessible only by its owner (POSIX 0700 / owner-only ACL)")` |
| 150-151 | `_platform.os_open(path, os.O_RDWR \| os.O_CREAT, 0o600, nofollow=True)` |
| 156, 162 | `_platform.flock` (`LOCK_UN` is tolerant) |
| 170 | `_platform.os_open(path, os.O_RDONLY, nofollow=True)` (`FileNotFoundError` mapping unchanged) |
| 207 | `if hasattr(os, "fchmod"): os.fchmod(fd, 0o600)` |
| 212 | `_platform.replace(temporary, path)` |
| 213-217 | `_platform.fsync_dir(root)` |

### ops_archive.py
| Line(s) | Change |
|---|---|
| 54-63 | `_regular`: `os.fdopen(_platform.open_regular(path), "rb")` (keep the `ArchiveError` mapping) |
| 76 | `is_link_like` |
| 264-266 | `_platform.mkdtemp_private(prefix=..., dir=target.parent)` (drop the chmod or keep it, harmless) |
| 290 | `_platform.rmtree(stage, ignore_errors=True)` |

### replication.py
| Line(s) | Change |
|---|---|
| 15 | drop fcntl |
| 155-159, 292-293 | `_platform.mkdtemp_private`; `_platform.mkdir_private(replica_path)` |
| 212, 332 | `_platform.rmtree(stage, ignore_errors=True)` |
| 219-221 | `parts = _platform.safe_relative_parts(relative)` (`ValueError` → `ReplicationError("replication asset path is unsafe")`) |
| 337, 681 | `is_link_like` |
| 362 | add `or "\\" in database or ":" in database` |
| 552 | `os.link(stage, target, follow_symlinks=False)` → `_platform.link_noreplace(stage, target)` (keep the `except FileExistsError`) |
| 657 | `_platform.temporary_directory(prefix="sdbase-shipping-", dir=standby.root.parent)` |
| 690-698 | `_platform.lock(lock_fd, exclusive=True, timeout=30)`; `TimeoutError` → `ReplicationError("standby replay recovery lock timed out")` |
| 718 | `_platform.os_open(journal, os.O_RDWR, nofollow=True)` |
| 752-753 | `_platform.unlock_and_close(lock_fd)` |
| 811-813 | `parts = _platform.safe_relative_parts(relative)` |
| 915 | `_platform.mkdir_private(intent_path.parent, exist_ok=True)` |

### replication_stream.py
| Line(s) | Change |
|---|---|
| 14 | drop fcntl |
| 115, 171 | `is_link_like` |
| 117 | `path.parent.mkdir(parents=True, exist_ok=True)`; `_platform.mkdir_private(path, exist_ok=True)` |
| 146-147 | `_platform.os_open(..., os.O_RDWR \| os.O_CREAT, 0o600, nofollow=True)` |
| 149, 162, 373 | `_platform.flock` |
| 180, 188, 190, 326, 337, 787, 812 | `_platform.rmtree` |
| 202 | `_remove`: on `PermissionError`, set `entry["delete"] = True` and return (retried by `sweep`) |
| 613 | `_platform.os_open(lock_path, os.O_RDWR \| os.O_CREAT, 0o600, nofollow=True)` |
| 616, 621 | `_platform.flock` (unlock tolerant) |
| 701 | `_platform.os_open(partial, os.O_WRONLY \| os.O_CREAT \| os.O_APPEND, 0o600, nofollow=True)` |
| 720 | `_platform.replace(partial, target)` |
| 788 | `_platform.mkdir_private(incoming)` |
| 795-799 | write binary: `with hash_path.open("wb") as stream: stream.write((hashlib.sha256(encoded).hexdigest() + "\n").encode("ascii"))` |

### wal_archive.py
| Line(s) | Change |
|---|---|
| 345 | add the `"\\"`/`":"` rejection |
| 353, 364 | `is_link_like` |
| 392, 429-431 | `_platform.safe_relative_parts(asset["path"])` (`ValueError` → `WalArchiveError("unsafe or unsupported WAL asset path")`) |
| 569 | `_platform.mkdtemp_private(prefix=".wal-enable-", dir=store.root)` |
| 577-578, 651 | `_platform.mkdir_private(..., exist_ok=...)` |
| 588 | `_platform.rmtree(stage, ignore_errors=True)` |
| 656, 700 | `os.link(...)` → `_platform.link_noreplace(temporary, target)` / `_platform.link_noreplace(temporary, path)` |
| 704 | per-commit `_atomic_json(head.json)` (inherits retry and share-delete) |

### backup.py (hub; signatures preserved)
| Line(s) | Change |
|---|---|
| 11 | ctypes import no longer needed |
| 51-56 | `_fsync_dir(path)` → `_platform.fsync_dir(path)` |
| 59-61 | `_fsync_tree(root)` → `_platform.fsync_tree(root)` |
| 64-76 | `_safe_source`: `parts = _platform.safe_relative_parts(relative)` (`ValueError` → `BackupError("unsafe backup path")`); `is_link_like` for root and components |
| 79-88 | `_open_regular`: `_platform.open_regular(path)`; `OSError` → `BackupError(f"cannot read backup component: {path}")` or `BackupError(f"backup component is not a regular file: {path}")` (keep both messages) |
| 138-151 | `_destination`: add `if not _platform.valid_new_name(raw.name): raise BackupError("target must name a new directory")` |
| 154-168 | `_publish_absent`: `try: _platform.publish_dir_noreplace(stage, target)`; `except FileExistsError: raise BackupError("target directory appeared during publication")`; `except OSError as exc: raise BackupError("atomic no-replace directory rename is unavailable" if exc.errno == errno.ENOSYS else f"could not publish backup directory: {exc.strerror}")` (Linux messages unchanged); then `_fsync_dir(target.parent)` |
| 228, 674 | `_platform.mkdtemp_private` |
| 279, 721 | `_platform.rmtree(stage, ignore_errors=True)` |
| 310-312 | add `":"` and reserved-name rejection |
| 387-390 | `_platform.safe_relative_parts(entry["path"])` |
| 415 | `entry.is_symlink()` → `_platform.entry_is_link_like(entry)` (keep the `BackupError` message) |
| 432 | `is_link_like` |
| 449 | `_platform.temporary_directory(prefix="sdbase-verify-")` |

### retention.py
| Line(s) | Change |
|---|---|
| 336 | `is_link_like` |
| 338 | `_platform.mkdir_private(archive_root, exist_ok=True)` |
| 340 | `_platform.mkdtemp_private(prefix=".retention-", dir=archive_root)` |
| 373 | `_platform.rmtree(stage, ignore_errors=True)` |
| 358-361, 165, 276-277 | keep the POSIX f-strings and `endswith("/…")` checks (these strings live in the DB) |

### pitr.py
| Line(s) | Change |
|---|---|
| 131 | `mkdtemp_private` |
| 191 | `rmtree(ignore_errors=True)` |
| 89-99 | keep (the preview hash includes native absolute paths by design: same-machine review) |

### portable_export.py
| Line(s) | Change |
|---|---|
| 55-60 | `_fsync_dir` → shim |
| 162-171 | collect names first; reject `name.casefold()` collisions and `os.path.isreserved(f"{name}.jsonl")` on Windows with `PortableExportError` |
| 237-239 | `mkdtemp_private` |
| 242 | `mkdir_private` |
| 286 | `rmtree(ignore_errors=True)` |
| 290, 297 | `is_link_like` |
| 294 | after walking, filter `subdirs[:] = [d for d in subdirs if not _platform.is_link_like(Path(current)/d)]`, raising for links as today |

### portable_import.py
| Line(s) | Change |
|---|---|
| 362-364 | `mkdtemp_private` |
| 408 | `rmtree(ignore_errors=True)` |

### store_health.py / capacity_monitor.py / store_provision.py
| File:line | Change |
|---|---|
| store_health.py:33-35 | `is_link_like` |
| store_health.py:115-120 | `total_bytes, free_bytes = _platform.disk_usage(store.root)` inside the existing `try/except OSError` |
| capacity_monitor.py:181-191 | `database_free_bytes = _platform.disk_usage(store.root)[1]`; `media_free_bytes = _platform.disk_usage(media_root if media_root.exists() else store.root)[1]` |
| store_provision.py:26-27 | `_platform.temporary_directory(prefix=..., dir=target.parent)` |

### No code change
`snapshot_contract.py`, `read_snapshot.py` (must keep closing disk readers), `identity.py`, `http_limits.py`, `transport.py`, `http_pool.py` (optional loopback mapping, §3.16), `failover_server.py` (depends on the `api_server` fix).

---

## 6. Internal contracts that must stay byte- and name-compatible

1. **Module-level helper names used as monkeypatch seams or imports.** Keep the names and the call-through-global style; only change the bodies.
   - `storage._atomic_json`, imported and patched by name:
     - wal_archive: `patch("sdbase.wal_archive._atomic_json")` in recovery_acceptance.py:72/138.
     - replication: `replication._atomic_json = …` in replication_acceptance.py:57-85 and failover_acceptance.py:175-180.
     - Also used by disk_index, migration, failover, online_maintenance, pitr, portable_import, retention, replication_stream, and the stream/engine/maintenance/disk_index acceptance tools.
   - `replication._copy_file` (patched in replication_acceptance.py:59-69).
   - `backup._fsync_dir`, `_fsync_tree`, `_open_regular`, `_safe_source`, `_publish_absent`, `_copy_file`, `_digest_file`, `_inventory`, `_read_manifest`, `_validate_manifest`, `_write_manifest`, `_media_refs`. **tools/radios_reliability.py:153-306 and :410-738 call these directly**, with the same exception types (`BackupError`) and messages.
   - `endpoint_journal._fsync_directory` (endpoint_media.py:12).
   - `media._stage_object` and `_digest` (desktop_media.py:624/627, recording_jobs.py:20); `media.object_path` and `object_root`.
   - `online_maintenance._after_publish` (fault-injection seam).
2. **Exception contracts.** `BlockingIOError` on lock contention; `FileExistsError` from no-replace publication; `BackupError("target directory appeared during publication")`; `LockTimeout` from `Store._locked`; `DiskIndexUnavailable` for unsafe cache paths; and the `recover_*` functions returning `{"skipped": [...], "removed": [...]}` shapes.
3. **Returned path strings** (`"path": str(target)`, `cache_root`, `archive`, `target_path`, `source`) are native absolute paths. On Windows they contain backslashes and drive letters; JSON escaping handles them. The C client (managed_path) must treat them as native paths.
4. **On-disk formats unchanged:**
   - journal frames, checkpoint and meta envelopes (`{"payload":…,"sha256":…}` over `_json_bytes`)
   - WAL header/head/records
   - backup/archive/export manifests and `manifest.sha256` (`hex + "\n"`, binary)
   - JSONL with LF
   - disk-index `SDBIDX01` big-endian offsets
   - `Endpoint.sdb` JSON lines with LF (after the fix)
5. **Lock files and region** (§4.3) are shared with C and tools.
6. **Stage naming** (`.{target}.…partial`, `.build-<uuid>`, `.wal-enable-*`, `.retention-*`, `.<sha>.<id>.partial`, `.<seq>.json.<hex>.partial`) is matched by the recovery regexes: replication_stream.py:189, wal_archive.py:366, disk_index.py:654-660, migration.py:100. `mkdtemp_private` must keep the `prefix + random + suffix` shape, and the random part must match `[A-Za-z0-9_-]+`. Hex satisfies this.

---

## 7. Durability on NTFS without POSIX directory fsync

- **Commit durability** is unchanged: journal append, then `FlushFileBuffers` (os.fsync, 3.7 ms measured). NTFS flushes the file's data and the metadata needed to read it (size).
- **Rename durability and ordering.** NTFS journals metadata operations (renames, size changes) in `$LogFile` in LSN order. A later `FlushFileBuffers` on any file forces the log up to that file's metadata LSN, so earlier renames become durable too. Consider the checkpoint (§2.3): checkpoint rename (LSN a), journal truncation (LSN b > a), then `FlushFileBuffers(journal)`. After that flush the rename is durable, so "old checkpoint + empty journal" cannot occur on NTFS.
- **Belt and braces.** `fsync_dir()` issues a real directory `FlushFileBuffers` when permitted (verified cheap). `replace()` uses `MOVEFILE_WRITE_THROUGH` on the fallback path.
- **Weaker volumes.**

| Volume type | Limitation | Recommendation |
|---|---|---|
| FAT32 / exFAT | no journaling, no POSIX rename, no hard links | the shim degrades to MoveFileExW and rename; storage settings should warn when `data\database` is on such a volume |
| SMB shares | durability depends on the server | same warning |

  Detect the file system with `GetVolumeInformationByHandleW` (tools/storage-locations area) and refuse or warn.
- **Atomicity** of every published object stays all-or-nothing on all volumes: single rename.

---

## 8. Database byte-portability verdict (Linux → Windows)

**Verdict: portable as-is; no conversion step.** Evidence from the real Linux store in the original folder `C:\Users\sonatturkan\Desktop\radyo projesi\kaynak kodlar\RadioS\data\database`:
- `RadioS.sdb` is 707,323,604 bytes. It is a big-endian length-prefixed frame stream; the first frame is 16,615 bytes of compact sorted-key UTF-8 JSON. Windows CPython walked **all 3,417 frames (seq 3163-6579), 0 checksum failures, 13 s**.
- `RadioS.sdb.meta` (248 bytes) holds only format, generation, schema_version=4, node_role, store_id, node_id, timeline_id and epoch: no paths.
- `RadioS.sdb.checkpoint` (290 MB) is a JSON envelope.
- `RadioS.sdb.lock` is 0 bytes; its content is irrelevant, it is recreated if missing (storage.py:207), and it is never byte-locked on disk.
- `RadioS.sdb.objects\xx\<sha256>` uses lowercase-hex names, so case-insensitive NTFS has no collisions.
- WAL, backup, retention and disk-index manifests store relative POSIX paths (§2.9). The shim parser accepts them identically on both OSes.
- No absolute paths in engine metadata; `.indexes` caches are verified by checksum and rebuilt if stale; in-memory `st_ino` stamps are never persisted.

**Caveats (owned by other areas; the engine must not rewrite history):**
1. **Absolute Linux paths inside domain records**, found by a read-only scan:
   - `desktop_mutations.result.version.managed_path` (1,379 in the journal) and `result.detail.version.managed_path` (1,374), both under `/home/sturkan/Masaüstü/radyo projesi/kaynak kodlar/RadioS/data/database/RadioS.sdb.objects/…`.
   - `desktop_mutations.result.backup` (`…/data/backups/sifirlama-…`).
   - Mirrored inside the immutable `_audit` `changes.after` bodies.
   - The checkpoint holds 2,140 `managed_path`, 10 `location` and 6 `backup` values of the same form. (`mount: "/canli"` is an Icecast mount name, not a path.)
   - These come from idempotency receipts (desktop_media.py:675-678 → `add_operation(... result)`). A replayed request returns the stored stale path. Fix in desktop_media: recompute `managed_path` at read and replay time (it already does for normal reads, desktop_media.py:324-336/384) and never trust a stored one. `_audit` evidence must stay untouched (append-only, hash-relevant).
2. Other stored native paths written by non-engine modules: desktop_media provenance `source_path`/`source_root` (desktop_media.py:606), importer/mairlist/radioboss `source_path`, and settings path-mapping rules (settings.py:60). On Windows they are data, not usable paths. Those areas should map or ignore them.
3. Engine *operational* state files store absolute native paths and are **not portable across hosts by design**. A Linux state file on Windows is rejected safely ("belongs to another database"), never misused. Re-initialize after migration:
   - `ops_scheduler` `scheduler-state.json` (`database`, `output_dir`; ops_scheduler.py:228, 314)
   - failover `supervisor.json` (`source_database`, `standby_database`, `active_database`; failover.py:93-111)
   - migration stage markers, online-maintenance `job.json` (transient)
4. **Transport:** copy the data directory with binary-safe tools (robocopy, zip). **Never through git with `autocrlf`/`text=auto`**: `records.jsonl`, WAL `*.json` and `manifest.sha256` contain LF and would be rewritten, breaking SHA-256 checks. Add `.gitattributes` `data/** -text` (packaging).
5. Copied directories inherit the destination ACL. If an elevated tool created them with Python 3.13's 0o700 semantics, they may be owner=Administrators; the tools must use `mkdir_private`.

---

## 9. Tests: how the contracts are exercised, and what must change

### 9.1 SDBASE acceptance runners (`database/SDBASE/tools/*.py`; manual, not in CTest)
| Runner | Exercises | Windows adaptation |
|---|---|---|
| engine_acceptance.py | `LockTimeout` with `lock_timeout=0.15` while a **multiprocessing child holds `fcntl.flock(LOCK_EX)`** (lines 4, 20-24); HTTP pool reuse on 127.0.0.1; migrate_copy; WAL | `_hold_lock` uses `_platform.flock`; spawn start method (top-level function OK; guard `main()`) |
| disk_index_acceptance.py | build/publish/stale fallback, mmap readers, recovery after **`signal.SIGKILL`** (line 205), job lock while a child builds | `process.kill()`; expect `skipped` for in-use obsolete builds |
| maintenance_acceptance.py | online checkpoint with `_after_publish` crash injection; recovery ignores symlinked staging (line 228 `symlink_to`) | needs the symlink privilege, else skip; add a junction variant |
| recovery_acceptance.py | WAL/PITR with `patch("sdbase.wal_archive._atomic_json")` failures | keep the seam |
| replication_acceptance.py / failover_acceptance.py | patched `replication._atomic_json` / `_copy_file`; fencing and promotion | keep the seams |
| stream_acceptance.py | TLS listener (chmod key 0600), follower resume of partial downloads (**O_APPEND path**), SIGTERM stop (line 37) | Event-based stop |
| query_integrity_acceptance.py | integrity scrub under a SH lock, disk index | none beyond the shim |
| acceptance.py / acceptance_playout.py | general flows; `os.link` (line 163) | NTFS OK |
| benchmark.py | `/proc/self/statm`, `resource.getrusage/setrlimit` | `GetProcessMemoryInfo` / Job object limits, or skip on Windows |

### 9.2 CTest Python tests (tests/*.py) touching engine contracts
- `test_storage_locations.py`: Store + checkpoint (line 78); `RadioS.sdb.lock` existence (183); symlink refusals (105, 124).
- `test_reliability.py`:
  - holds `server.lock` EX\|NB (line 308-312) and `gui.lock` SH\|NB (313-317) with `fcntl` to force restore refusal, so the tools must use the shim with the same region;
  - binds a foreign socket on the service port (294-298), which works only once the listener uses `SO_EXCLUSIVEADDRUSE`.
- `test_application_shutdown.py:29-39`: probes the C GUI's `gui.lock`.
- `test_database_setup.py:208-221`: `/proc/<pid>/cmdline`, SIGTERM/SIGKILL.
- `test_release_install.py:150-198`: flock, `/proc/<pid>/exe`, `os.kill(…,15)`.

These need tools-area shim functions (`process_identity`, named stop events). CMake injects `PYTHONPATH=…/database/SDBASE/src:…/tests` with **`:`** (CMakeLists.txt:916, 1092). On Windows this must be `;`, escaped as `\;` inside `ENVIRONMENT`.

### 9.3 New Windows-specific tests to add (engine)
1. `_platform` unit tests (port `proto_test.py`): lock matrix in-process and cross-process plus kill; `BlockingIOError` mapping; idempotent `LOCK_UN`; `replace` over a share-delete reader (POSIX semantics) and over a Python `open()` reader (retry then timeout, then succeed after close); `publish_dir_noreplace` and `link_noreplace` conflicts; binary `os_open`; nofollow on symlink and junction; `pread`; `fsync_dir`; `disk_usage`; `safe_relative_parts` rejection set; `rmtree` with read-only and busy files; the `private_dir_ok` matrix (Users:R, Everyone:F).
2. **Text-mode regression:** `EndpointJournal.append` then the file has no `\r\n`; a journal frame containing `0d 0a` and `1a` round-trips through backup → verify → restore, and through online maintenance `_journal_tail`.
3. **Concurrency:** a reader thread or process hammering `read_bytes_shared(meta)` while commits replace meta and `head.json` gives no failures; with a plain `open()` reader, commits still succeed through retry.
4. **Junction parity:** a junction anywhere under `.objects`, `.indexes`, a backup root, an export package or a WAL root is rejected like a symlink.
5. **Path escape:** manifest entries `\\x`, `D:x`, `a:ads`, `con.json` are rejected by backup, disk_index, replication and WAL parsers, consistently on Linux and Windows.
6. **Mapped-file cleanup:** an open `ReadSnapshot` over a disk index, then `recover_disk_index_builds(cleanup=True)` of an obsolete build, reports it in `skipped`, never raises.
7. **Ops scheduler** accepts a `mkdir_private` directory and rejects an inherited `C:\` subdirectory (Authenticated Users) and `Users:R`.
8. **Listener exclusivity:** a second failover or replication listener on the same port fails to bind.
9. **Portability fixture:** a copy of a small Linux-created store (fixture bytes checked in with `-text`) opens on Windows: sequence, store_id, indexes and `inspect_integrity` all `ok`.

---

## 10. Risks and open decisions

1. **Third-party file handles** (Defender, Search indexer, OneDrive, backup agents, Explorer previews) cause sharing violations on replace and rmtree. Retries bound the damage, but an EX-locked writer can stall up to `SDBASE_FS_RETRY_SECONDS`. Mitigate in the installer: keep data out of OneDrive, mark it not-content-indexed, and recommend a Defender exclusion. Open decision: 5 s default vs. shorter for commit paths.
2. **Media objects held open without FILE_SHARE_DELETE** by FFmpeg in the C player and by `http_server.py:490` block the corrupt-object repair (media.py:185) and any deletion (desktop reset, media cleanup in other areas). They surface as `MediaError` after the retry window. C opens should add FILE_SHARE_DELETE where possible; FFmpeg's `file:` protocol does not, so a custom AVIO would be needed.
3. **Directory-fsync semantics.** Durability rests on NTFS log ordering plus the best-effort directory flush. On FAT32, exFAT and SMB the guarantees are weaker; warn or refuse. Decision for the lead: keep best-effort `FlushFileBuffers` (recommended; verified 0.2 ms) or a strict no-op (`SDBASE_DIR_FSYNC=0`).
4. **ACL/elevation.** Python 3.13's 0o700 handling creates owner=Administrators directories when elevated. Any engine directory created by an elevated run then denies the same user un-elevated, unless all creation goes through `mkdir_private`. Existing directories created during development may need `icacls /reset` or re-creation.
5. **Long paths are disabled** here (`RtlAreLongPathsEnabled` False). Deep project roots plus build and stage names approach 260 characters. Keep the DB directory short or enable long paths. `\\?\` forms returned by realpath must be accepted by C.
6. **Path identity across Python and C.** `resolve()` may convert mapped or `subst` drives to UNC, and case differs between `radios.ini` and the on-disk form. Prefix checks of `managed_path` against `database_file` in C (planner.c:309) must canonicalize the same way and compare case-insensitively.
7. **Junction checks on ancestors** (`any(part.is_symlink() for part in parents)`). Extending them to junctions is a safety-parity decision. A user who relocates `data\` through a junction gets "unsafe path" errors; that matches Linux, where a symlinked data directory is refused.
8. **LockFileEx is not re-entrant per handle.** Any future engine code that re-locks or converts on the same fd behaves differently than flock: error 33 instead of success.
9. **Graceful stop.** Without signals, every long-running engine loop depends on the tools/service wrapper setting its `threading.Event` (named event). An abrupt `TerminateProcess` remains crash-safe (locks are released, journal tails are repaired).
10. **Same-machine-only hashes and states** (pitr preview hash, scheduler and supervisor state with absolute paths) are intentionally non-portable. Document this for operators migrating from Linux.
11. **Turkish case folding.** NTFS treats `ı`/`I`/`i` as one case class, while Python's `PureWindowsPath` equality (`str.lower`) does not fold `ı`. Containment checks on user-chosen Turkish directory names could, in contrived cases, disagree with the file system. Engine-generated names are ASCII.
12. **Hard links** on exFAT or FAT data drives: `link_noreplace` avoids them, but other areas (acceptance_playout, tools) still use `os.link`.

---

## 11. Cross-area dependencies (what other areas must provide or agree on)

- **C portability layer** (`include/radios/platform.h`, `src/core/platform.c`):
  - The identical lock region and flags (§4.3) for `gui.lock`, the outbox `.lock`, `server.lock` and `RadioS.sdb.lock` (if C ever probes it); blocking `flock(LOCK_EX)` in recovery.c:164 maps to LockFileEx without FAIL_IMMEDIATELY.
  - Media-object and DB-sidecar opens with `FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE`.
  - Case-insensitive, canonical path containment for `managed_path` under `<database_file>.objects\`.
  - HTTP to the service at `127.0.0.1` only.
- **tools/ launcher and service helpers:**
  - Load `sdbase/_platform.py` by path, or a re-exporting `tools/_platform.py`. Replace `fcntl`, `os.O_NOFOLLOW`/`os.O_NONBLOCK` direct attributes, `st_uid != os.getuid()` (radios_reliability.py:371, 385, 390), `mode & 0o077`, `/proc` and SIGTERM/SIGKILL with shim functions: `owner_ok`, `private_dir_ok`, `process_identity`, named stop events.
  - Use `_platform.replace` for sidecar moves (radios_reliability.py:431). A directory `os.replace` onto an existing directory is impossible on Windows; Linux allows it for empty directories.
  - Never hold engine files open while renaming their parent directory.
- **desktop_media area:**
  - Replace `media._stage_object(store, Path(f"/proc/self/fd/{fd}"))` (desktop_media.py:624) with `media._stage_object(store, fd)`.
  - Replace `os.open(..., dir_fd=…)` and `O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK` (desktop_media.py:540-542; dir_fd is unsupported on Windows) with `_platform.open_regular(root / name)` plus an `is_link_like` check on the root.
  - Stop persisting or replaying absolute `managed_path`/`backup` paths in receipts (§8).
- **HTTP area** (`api_server.py`, `http_server.py`):
  - `BoundedHTTPServer`: `allow_reuse_address=False` plus `SO_EXCLUSIVEADDRUSE` on Windows (needed by failover_server and replication_stream).
  - Stream media through `os.fdopen(_platform.open_regular(path), "rb")` (share-delete) instead of `Path.open("rb")` (http_server.py:490).
- **Build/test area:**
  - `PYTHONPATH` separator `\;` (CMakeLists.txt:916, 1092) and `WORKING_DIRECTORY /tmp` (CMakeLists.txt:264).
  - Port the fcntl/SIGKILL/`/proc`/symlink-dependent tests (§9).
  - Register the new `_platform` tests.
- **Packaging/installer:**
  - Data root outside OneDrive, NOT_CONTENT_INDEXED, a Defender exclusion recommendation.
  - Enabling `LongPathsEnabled` (or keeping the path budget).
  - `.gitattributes` `-text` for data and fixtures.
  - Docs updates (docs/portable_export.md:31 mentions `renameat2`; docs/ops_scheduler.md:20 and docs/ops_archive.md:20 mention 0700).

---

## Appendix A: reference implementation of `sdbase/_platform.py`

Validated on this host: every function except `temporary_directory`/`valid_new_name` (trivial; added below) was exercised by `proto_test.py` (see proto-result.json). Copy it verbatim into `database/SDBASE/src/sdbase/_platform.py`, then add the two helpers.

```python
"""SDBASE portability layer: POSIX passthrough, Windows emulation."""
from __future__ import annotations

import errno
import os
import secrets
import shutil
import stat
import time
from contextlib import contextmanager
from pathlib import PurePosixPath

IS_WINDOWS = os.name == "nt"
LOCK_SH, LOCK_EX, LOCK_NB, LOCK_UN = 1, 2, 4, 8
WIN_LOCK_OFFSET = 0x7FFFFFFF_00000000      # == C RADIOS_LOCK_OFFSET_HIGH:LOW
WIN_LOCK_LENGTH = 1                        # == C RADIOS_LOCK_LENGTH
O_BINARY = getattr(os, "O_BINARY", 0)
O_NOINHERIT = getattr(os, "O_NOINHERIT", 0)
O_CLOEXEC = getattr(os, "O_CLOEXEC", 0)
O_NOFOLLOW = getattr(os, "O_NOFOLLOW", 0)
O_DIRECTORY = getattr(os, "O_DIRECTORY", 0)
O_NONBLOCK = getattr(os, "O_NONBLOCK", 0)
_RETRY_WINERRORS = frozenset({5, 32, 33, 1224})
_RETRY_SECONDS = float(os.environ.get("SDBASE_FS_RETRY_SECONDS", "5"))

if IS_WINDOWS:
    import ctypes
    import msvcrt
    from ctypes import wintypes as wt
    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _adv = ctypes.WinDLL("advapi32", use_last_error=True)
    _ntdll = ctypes.WinDLL("ntdll")
    GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
    DELETE, SYNCHRONIZE = 0x00010000, 0x00100000
    SHARE_ALL = 0x1 | 0x2 | 0x4
    CREATE_NEW, CREATE_ALWAYS, OPEN_EXISTING, OPEN_ALWAYS, TRUNCATE_EXISTING = 1, 2, 3, 4, 5
    FILE_ATTRIBUTE_READONLY, FILE_ATTRIBUTE_NORMAL, FILE_ATTRIBUTE_REPARSE_POINT = 0x1, 0x80, 0x400
    FILE_FLAG_BACKUP_SEMANTICS, FILE_FLAG_OPEN_REPARSE_POINT = 0x02000000, 0x00200000
    INVALID_HANDLE = wt.HANDLE(-1).value
    LOCKFILE_FAIL_IMMEDIATELY, LOCKFILE_EXCLUSIVE_LOCK = 0x1, 0x2
    MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH = 0x1, 0x8
    FILE_RENAME_FLAG_REPLACE_IF_EXISTS, FILE_RENAME_FLAG_POSIX_SEMANTICS = 0x1, 0x2
    FileAttributeTagInfo, FileRenameInfoEx = 9, 22
    NAME_SURROGATE_BIT = 0x20000000

    class _OVERLAPPED(ctypes.Structure):
        _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                    ("Offset", wt.DWORD), ("OffsetHigh", wt.DWORD), ("hEvent", wt.HANDLE)]

    class _TAG_INFO(ctypes.Structure):
        _fields_ = [("FileAttributes", wt.DWORD), ("ReparseTag", wt.DWORD)]

    class _SECURITY_ATTRIBUTES(ctypes.Structure):
        _fields_ = [("nLength", wt.DWORD), ("lpSecurityDescriptor", wt.LPVOID), ("bInheritHandle", wt.BOOL)]

    def _fn(dll, name, restype, *argtypes):
        function = getattr(dll, name); function.restype = restype; function.argtypes = list(argtypes)
        return function

    _CreateFileW = _fn(_k32, "CreateFileW", wt.HANDLE, wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE)
    _CloseHandle = _fn(_k32, "CloseHandle", wt.BOOL, wt.HANDLE)
    _LockFileEx = _fn(_k32, "LockFileEx", wt.BOOL, wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.POINTER(_OVERLAPPED))
    _UnlockFileEx = _fn(_k32, "UnlockFileEx", wt.BOOL, wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.POINTER(_OVERLAPPED))
    _ReadFile = _fn(_k32, "ReadFile", wt.BOOL, wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.POINTER(_OVERLAPPED))
    _FlushFileBuffers = _fn(_k32, "FlushFileBuffers", wt.BOOL, wt.HANDLE)
    _MoveFileExW = _fn(_k32, "MoveFileExW", wt.BOOL, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD)
    _SetFileInformationByHandle = _fn(_k32, "SetFileInformationByHandle", wt.BOOL, wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD)
    _GetFileInformationByHandleEx = _fn(_k32, "GetFileInformationByHandleEx", wt.BOOL, wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD)
    _GetDiskFreeSpaceExW = _fn(_k32, "GetDiskFreeSpaceExW", wt.BOOL, wt.LPCWSTR, ctypes.POINTER(ctypes.c_ulonglong),
                               ctypes.POINTER(ctypes.c_ulonglong), ctypes.POINTER(ctypes.c_ulonglong))
    _CreateDirectoryW = _fn(_k32, "CreateDirectoryW", wt.BOOL, wt.LPCWSTR, ctypes.POINTER(_SECURITY_ATTRIBUTES))
    _LocalFree = _fn(_k32, "LocalFree", wt.HLOCAL, wt.HLOCAL)
    _GetCurrentProcess = _fn(_k32, "GetCurrentProcess", wt.HANDLE)
    _OpenProcessToken = _fn(_adv, "OpenProcessToken", wt.BOOL, wt.HANDLE, wt.DWORD, ctypes.POINTER(wt.HANDLE))
    _GetTokenInformation = _fn(_adv, "GetTokenInformation", wt.BOOL, wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD))
    _ConvertSidToStringSidW = _fn(_adv, "ConvertSidToStringSidW", wt.BOOL, wt.LPVOID, ctypes.POINTER(wt.LPWSTR))
    _ConvertSddl = _fn(_adv, "ConvertStringSecurityDescriptorToSecurityDescriptorW", wt.BOOL, wt.LPCWSTR, wt.DWORD,
                       ctypes.POINTER(wt.LPVOID), ctypes.POINTER(wt.ULONG))
    _GetNamedSecurityInfoW = _fn(_adv, "GetNamedSecurityInfoW", wt.DWORD, wt.LPCWSTR, ctypes.c_int, wt.DWORD,
                                 ctypes.POINTER(wt.LPVOID), ctypes.POINTER(wt.LPVOID), ctypes.POINTER(wt.LPVOID),
                                 ctypes.POINTER(wt.LPVOID), ctypes.POINTER(wt.LPVOID))
    _GetAce = _fn(_adv, "GetAce", wt.BOOL, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.LPVOID))
    try:
        _RtlAreLongPathsEnabled = _fn(_ntdll, "RtlAreLongPathsEnabled", ctypes.c_ubyte)
    except AttributeError:
        _RtlAreLongPathsEnabled = None

    def _winerr(code, filename=None, filename2=None):
        return OSError(None, ctypes.FormatError(code).strip(), filename, code, filename2)

    def _create(path, access, disposition, flags, share=SHARE_ALL):
        handle = _CreateFileW(str(path), access, share, None, disposition, flags, None)
        if handle in (INVALID_HANDLE, None):
            raise _winerr(ctypes.get_last_error(), str(path))
        return handle

    def _is_name_surrogate_handle(handle) -> bool:
        info = _TAG_INFO()
        if not _GetFileInformationByHandleEx(handle, FileAttributeTagInfo, ctypes.byref(info), ctypes.sizeof(info)):
            raise _winerr(ctypes.get_last_error())
        return bool(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT and info.ReparseTag & NAME_SURROGATE_BIT)
else:
    import fcntl


def open_flags(base: int, *, nofollow: bool = False) -> int:
    return base | O_BINARY | O_NOINHERIT | O_CLOEXEC | (O_NOFOLLOW if nofollow else 0)


def os_open(path, flags: int, mode: int = 0o777, *, nofollow: bool = False) -> int:
    if not IS_WINDOWS:
        return os.open(path, open_flags(flags, nofollow=nofollow), mode)
    accmode = flags & (os.O_RDONLY | os.O_WRONLY | os.O_RDWR)
    access = {os.O_RDONLY: GENERIC_READ, os.O_WRONLY: GENERIC_WRITE}.get(accmode, GENERIC_READ | GENERIC_WRITE)
    if flags & os.O_CREAT and flags & os.O_EXCL: disposition = CREATE_NEW
    elif flags & os.O_CREAT and flags & os.O_TRUNC: disposition = CREATE_ALWAYS
    elif flags & os.O_CREAT: disposition = OPEN_ALWAYS
    elif flags & os.O_TRUNC: disposition = TRUNCATE_EXISTING
    else: disposition = OPEN_EXISTING
    attributes = FILE_ATTRIBUTE_READONLY if flags & os.O_CREAT and not mode & stat.S_IWRITE else FILE_ATTRIBUTE_NORMAL
    handle = _create(path, access, disposition, attributes | (FILE_FLAG_OPEN_REPARSE_POINT if nofollow else 0))
    try:
        if nofollow and _is_name_surrogate_handle(handle):
            raise OSError(errno.ELOOP, "refusing to open a symlink or junction", str(path))
        crt = (os.O_RDONLY if accmode == os.O_RDONLY else 0) | (os.O_APPEND if flags & os.O_APPEND else 0)
        return msvcrt.open_osfhandle(handle, crt)        # binary unless os.O_TEXT is passed
    except BaseException:
        _CloseHandle(handle)
        raise


def open_regular(path, *, write: bool = False, create: bool = False, mode: int = 0o600, nofollow: bool = True) -> int:
    descriptor = os_open(path, (os.O_RDWR if write else os.O_RDONLY) | (os.O_CREAT if create else 0), mode, nofollow=nofollow)
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise OSError(errno.EINVAL, "not a regular file", str(path))
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor


def read_bytes_shared(path, *, limit: int | None = None) -> bytes:
    with os.fdopen(open_regular(path), "rb") as stream:
        return stream.read() if limit is None else stream.read(limit)


def fsync_dir(path) -> bool:
    if not IS_WINDOWS:
        descriptor = os.open(path, os.O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        return True
    if os.environ.get("SDBASE_DIR_FSYNC", "1") == "0":
        return False
    handle = _CreateFileW(str(path), GENERIC_READ | GENERIC_WRITE, SHARE_ALL, None, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, None)
    if handle in (INVALID_HANDLE, None):
        code = ctypes.get_last_error()
        if code in (2, 3):
            raise _winerr(code, str(path))
        return False
    try:
        return bool(_FlushFileBuffers(handle))
    finally:
        _CloseHandle(handle)


def fsync_tree(root) -> None:
    for directory, _subdirs, _files in os.walk(root, topdown=False):
        fsync_dir(directory)


def _retrying(operation, *, timeout):
    deadline = time.monotonic() + (_RETRY_SECONDS if timeout is None else timeout)
    delay = 0.001
    while True:
        try:
            return operation()
        except OSError as exc:
            if getattr(exc, "winerror", None) not in _RETRY_WINERRORS or time.monotonic() >= deadline:
                raise
        time.sleep(delay)
        delay = min(delay * 2, 0.05)


_POSIX_RENAME_UNSUPPORTED: set[int] = set()


def _posix_rename(src: str, dst: str) -> None:
    handle = _create(src, DELETE | SYNCHRONIZE, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)
    try:
        class _RENAME(ctypes.Structure):
            _fields_ = [("Flags", wt.DWORD), ("RootDirectory", wt.HANDLE), ("FileNameLength", wt.DWORD),
                        ("FileName", ctypes.c_wchar * (len(dst) + 1))]
        info = _RENAME(FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS, None, len(dst) * 2, dst)
        if not _SetFileInformationByHandle(handle, FileRenameInfoEx, ctypes.byref(info), ctypes.sizeof(info)):
            raise _winerr(ctypes.get_last_error(), src, dst)
    finally:
        _CloseHandle(handle)


def replace(src, dst, *, timeout: float | None = None) -> None:
    if not IS_WINDOWS:
        os.replace(src, dst)
        return
    source, target = os.path.abspath(src), os.path.abspath(dst)

    def attempt():
        volume = os.stat(os.path.dirname(target)).st_dev
        if volume not in _POSIX_RENAME_UNSUPPORTED:
            try:
                _posix_rename(source, target)
                return
            except OSError as exc:
                if getattr(exc, "winerror", None) not in (1, 50, 87):
                    raise
                _POSIX_RENAME_UNSUPPORTED.add(volume)
        if not _MoveFileExW(source, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH):
            raise _winerr(ctypes.get_last_error(), source, target)
    _retrying(attempt, timeout=timeout)


def publish_dir_noreplace(src, dst, *, timeout: float | None = None) -> None:
    if not IS_WINDOWS:
        import ctypes as _ctypes
        libc = _ctypes.CDLL(None, use_errno=True)
        renameat2 = getattr(libc, "renameat2", None)
        if renameat2 is None:
            raise OSError(errno.ENOSYS, "atomic no-replace directory rename is unavailable")
        renameat2.argtypes = [_ctypes.c_int, _ctypes.c_char_p, _ctypes.c_int, _ctypes.c_char_p, _ctypes.c_uint]
        renameat2.restype = _ctypes.c_int
        if renameat2(-100, os.fsencode(src), -100, os.fsencode(dst), 1) != 0:
            code = _ctypes.get_errno()
            raise OSError(code, os.strerror(code), str(src), None, str(dst))   # EEXIST -> FileExistsError
        return
    _retrying(lambda: os.rename(src, dst), timeout=timeout)


def link_noreplace(src, dst, *, timeout: float | None = None) -> None:
    if not IS_WINDOWS:
        os.link(src, dst, follow_symlinks=False)
        return
    _retrying(lambda: os.rename(src, dst), timeout=timeout)


def _fileno(target) -> int:
    return target if isinstance(target, int) else target.fileno()


def flock(target, operation: int) -> None:
    if not IS_WINDOWS:
        fcntl.flock(_fileno(target), operation)
        return
    handle = msvcrt.get_osfhandle(_fileno(target))
    overlapped = _OVERLAPPED(0, 0, WIN_LOCK_OFFSET & 0xFFFFFFFF, WIN_LOCK_OFFSET >> 32, None)
    if operation & LOCK_UN:
        if not _UnlockFileEx(handle, 0, WIN_LOCK_LENGTH, 0, ctypes.byref(overlapped)):
            code = ctypes.get_last_error()
            if code != 158:
                raise _winerr(code)
        return
    if not operation & (LOCK_SH | LOCK_EX):
        raise ValueError("flock operation needs LOCK_SH, LOCK_EX or LOCK_UN")
    flags = (LOCKFILE_EXCLUSIVE_LOCK if operation & LOCK_EX else 0) | (LOCKFILE_FAIL_IMMEDIATELY if operation & LOCK_NB else 0)
    if not _LockFileEx(handle, flags, 0, WIN_LOCK_LENGTH, 0, ctypes.byref(overlapped)):
        code = ctypes.get_last_error()
        if code in (33, 997):
            raise BlockingIOError(errno.EAGAIN, "file lock is held by another handle")
        raise _winerr(code)


def lock(target, *, exclusive: bool, timeout: float | None = None, poll: float = 0.01) -> None:
    mode = LOCK_EX if exclusive else LOCK_SH
    if timeout is None:
        flock(target, mode)
        return
    deadline = time.monotonic() + timeout
    while True:
        try:
            flock(target, mode | LOCK_NB)
            return
        except BlockingIOError:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("file lock wait timed out") from None
            time.sleep(min(poll, remaining))


def unlock_and_close(descriptor: int) -> None:
    try:
        flock(descriptor, LOCK_UN)
    finally:
        os.close(descriptor)


@contextmanager
def locked_file(path, *, exclusive: bool, timeout: float | None = None, create: bool = False, write: bool = False, mode: int = 0o600):
    descriptor = open_regular(path, write=write or create, create=create, mode=mode)
    try:
        lock(descriptor, exclusive=exclusive, timeout=timeout)
    except BaseException:
        os.close(descriptor)
        raise
    try:
        yield descriptor
    finally:
        unlock_and_close(descriptor)


def pread(descriptor: int, length: int, offset: int) -> bytes:
    if not IS_WINDOWS:
        return os.pread(descriptor, length, offset)
    buffer = ctypes.create_string_buffer(length)
    done = wt.DWORD(0)
    overlapped = _OVERLAPPED(0, 0, offset & 0xFFFFFFFF, offset >> 32, None)
    if not _ReadFile(msvcrt.get_osfhandle(descriptor), buffer, length, ctypes.byref(done), ctypes.byref(overlapped)):
        code = ctypes.get_last_error()
        if code == 38:
            return b""
        raise _winerr(code)
    return buffer.raw[:done.value]


def disk_usage(path) -> tuple[int, int]:
    if not IS_WINDOWS:
        info = os.statvfs(path)
        return info.f_blocks * info.f_frsize, info.f_bavail * info.f_frsize
    available, total, free = ctypes.c_ulonglong(), ctypes.c_ulonglong(), ctypes.c_ulonglong()
    if not _GetDiskFreeSpaceExW(str(path), ctypes.byref(available), ctypes.byref(total), ctypes.byref(free)):
        raise _winerr(ctypes.get_last_error(), str(path))
    return total.value, available.value


def is_link_like(path) -> bool:
    try:
        info = os.lstat(path)
    except (OSError, ValueError):
        return False
    if stat.S_ISLNK(info.st_mode):
        return True
    if IS_WINDOWS:
        return bool(info.st_file_attributes & FILE_ATTRIBUTE_REPARSE_POINT and info.st_reparse_tag & NAME_SURROGATE_BIT)
    return False


def entry_is_link_like(entry: os.DirEntry) -> bool:
    return entry.is_symlink() or (IS_WINDOWS and (entry.is_junction() or is_link_like(entry.path)))


def safe_relative_parts(relative: str) -> tuple[str, ...]:
    if not isinstance(relative, str) or not relative or any(char in relative for char in "\x00\\:"):
        raise ValueError("unsafe relative path")
    if PurePosixPath(relative).is_absolute():
        raise ValueError("absolute path")
    parts = tuple(relative.split("/"))
    for part in parts:
        if part in {"", ".", ".."} or part != part.rstrip(". ") or (IS_WINDOWS and os.path.isreserved(part)):
            raise ValueError("unsafe path component")
    return parts


def valid_new_name(name: str) -> bool:
    if not isinstance(name, str) or name in {"", ".", ".."} or any(char in name for char in "\x00/\\"):
        return False
    if IS_WINDOWS and (any(char in name for char in '<>:"|?*') or any(ord(char) < 32 for char in name) or
                       name != name.rstrip(". ") or os.path.isreserved(name)):
        return False
    return True


def rmtree(path, *, timeout: float | None = None, ignore_errors: bool = False) -> bool:
    if not IS_WINDOWS:
        shutil.rmtree(path, ignore_errors=ignore_errors)
        return True
    deadline = time.monotonic() + (_RETRY_SECONDS if timeout is None else timeout)

    def onexc(function, failed, exc):
        if isinstance(exc, FileNotFoundError):
            return
        if isinstance(exc, PermissionError):
            try:
                os.chmod(failed, stat.S_IWRITE)
            except OSError:
                pass
        delay = 0.002
        while True:
            try:
                function(failed)
                return
            except FileNotFoundError:
                return
            except OSError as retry:
                if getattr(retry, "winerror", None) not in _RETRY_WINERRORS or time.monotonic() >= deadline:
                    raise
            time.sleep(delay)
            delay = min(delay * 2, 0.05)
    try:
        shutil.rmtree(path, onexc=onexc)
        return True
    except OSError:
        if ignore_errors:
            return False
        raise


def current_user_sid() -> str:
    token = wt.HANDLE()
    if not _OpenProcessToken(_GetCurrentProcess(), 0x0008, ctypes.byref(token)):
        raise _winerr(ctypes.get_last_error())
    try:
        size = wt.DWORD(0)
        _GetTokenInformation(token, 1, None, 0, ctypes.byref(size))
        buffer = ctypes.create_string_buffer(size.value)
        if not _GetTokenInformation(token, 1, buffer, size, ctypes.byref(size)):
            raise _winerr(ctypes.get_last_error())
        return _sid_string(ctypes.cast(buffer, ctypes.POINTER(wt.LPVOID))[0])
    finally:
        _CloseHandle(token)


def _sid_string(sid) -> str:
    text = wt.LPWSTR()
    if not _ConvertSidToStringSidW(sid, ctypes.byref(text)):
        raise _winerr(ctypes.get_last_error())
    try:
        return text.value
    finally:
        _LocalFree(ctypes.cast(text, wt.LPVOID))


def mkdir_private(path, *, exist_ok: bool = False) -> None:
    if not IS_WINDOWS:
        try:
            os.mkdir(path, 0o700)
        except FileExistsError:
            if not exist_ok or not os.path.isdir(path):
                raise
        return
    user = current_user_sid()
    sddl = f"O:{user}D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;{user})"
    descriptor = wt.LPVOID()
    if not _ConvertSddl(sddl, 1, ctypes.byref(descriptor), None):
        raise _winerr(ctypes.get_last_error())
    try:
        attributes = _SECURITY_ATTRIBUTES(ctypes.sizeof(_SECURITY_ATTRIBUTES), descriptor, False)
        if not _CreateDirectoryW(str(path), ctypes.byref(attributes)):
            code = ctypes.get_last_error()
            if code == 183 and exist_ok and os.path.isdir(path):
                return
            raise _winerr(code, str(path))
    finally:
        _LocalFree(descriptor)


def mkdtemp_private(prefix: str = "tmp", suffix: str = "", dir=None) -> str:
    if not IS_WINDOWS:
        import tempfile
        return tempfile.mkdtemp(suffix=suffix, prefix=prefix, dir=dir)
    parent = os.path.abspath(dir if dir is not None else os.environ.get("TEMP", "."))
    for _ in range(100):
        candidate = os.path.join(parent, prefix + secrets.token_hex(4) + suffix)
        try:
            mkdir_private(candidate)
            return candidate
        except FileExistsError:
            continue
    raise FileExistsError(errno.EEXIST, "no usable temporary directory name", parent)


@contextmanager
def temporary_directory(prefix: str = "tmp", suffix: str = "", dir=None):
    if not IS_WINDOWS:
        import tempfile
        with tempfile.TemporaryDirectory(suffix=suffix, prefix=prefix, dir=dir) as path:
            yield path
        return
    path = mkdtemp_private(prefix=prefix, suffix=suffix, dir=dir)
    try:
        yield path
    finally:
        rmtree(path, ignore_errors=True)   # never mask the body's exception; recovery cleans leftovers


_ALLOWED_TRUSTEES = {"S-1-5-18", "S-1-5-32-544", "S-1-3-0",
                     "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464"}


def private_dir_ok(path) -> bool:
    if not IS_WINDOWS:
        info = os.stat(path)
        return stat.S_ISDIR(info.st_mode) and not info.st_mode & 0o077
    if not os.path.isdir(path) or is_link_like(path):
        return False
    owner, dacl, descriptor = wt.LPVOID(), wt.LPVOID(), wt.LPVOID()
    code = _GetNamedSecurityInfoW(str(path), 1, 0x1 | 0x4, ctypes.byref(owner), None, ctypes.byref(dacl), None, ctypes.byref(descriptor))
    if code:
        raise _winerr(code, str(path))
    try:
        user = current_user_sid()
        owner_sid = _sid_string(owner)
        allowed = _ALLOWED_TRUSTEES | {user}
        if owner_sid not in allowed or not dacl.value:
            return False
        for index in range(ctypes.cast(dacl, ctypes.POINTER(ctypes.c_ushort))[2]):   # ACL.AceCount
            ace = wt.LPVOID()
            if not _GetAce(dacl, index, ctypes.byref(ace)):
                raise _winerr(ctypes.get_last_error())
            header = ctypes.cast(ace, ctypes.POINTER(ctypes.c_ubyte))
            if header[0] != 0 or header[1] & 0x08:        # ACCESS_ALLOWED only; skip INHERIT_ONLY
                continue
            mask = ctypes.cast(ace, ctypes.POINTER(wt.DWORD))[1]
            trustee = _sid_string(ctypes.c_void_p(ace.value + 8))
            if trustee == "S-1-3-4":                       # OWNER RIGHTS
                trustee = owner_sid
            if trustee not in allowed and mask & ~SYNCHRONIZE:
                return False
        return True
    finally:
        _LocalFree(descriptor)


def long_paths_enabled() -> bool:
    if not IS_WINDOWS:
        return True
    return bool(_RtlAreLongPathsEnabled and _RtlAreLongPathsEnabled())
```

POSIX-branch notes:
- `private_dir_ok`, `current_user_sid` and the `_k32`/`_adv` names exist only on Windows. On Linux, `private_dir_ok` returns before touching them, and `current_user_sid` must not be called (guard with `IS_WINDOWS` in callers).
- `publish_dir_noreplace` keeps the exact renameat2 call from backup.py:156-167.

## Appendix B: the eight engine bugs a Windows run would hit first (smoke order)
1. `import sdbase` raises ModuleNotFoundError `fcntl` (§3.1).
2. `open_radio_store()` raises `PermissionError` in `_atomic_json` dir fsync (§3.5).
3. Any backup or restore raises `TypeError` in `_publish_absent` (§0 item 3).
4. A commit under concurrent meta or `head.json` readers raises `PermissionError` from `os.replace` (§3.3).
5. Health and capacity endpoints raise `AttributeError: statvfs` (§3.10).
6. A disk-index query with a valid published cache crashes with `AttributeError: pread`. `DiskOrderedKeys.__init__` (disk_index.py:449) re-raises it, and the `ordered_disk_keys` except tuple (disk_index.py:594) does not include `AttributeError`, so the query fails instead of falling back to RAM. `inspect_disk_indexes` fails the same way (§3.8).
7. The ops scheduler always refuses `output_dir` (§3.11).
8. `Endpoint.sdb` is written with CRLF lines (§3.13).
