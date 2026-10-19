# Area spec: audio-misc-media (Windows 10/11 x64 port)

Project analysed: `C:\Users\sonatturkan\Desktop\RadioS` (read-only pass, 2026-10-19).
Builds on `specs\PLATFORM_FACTS.md` (GLib 2.90.1, FFmpeg 8.x = libavcodec 63 / libavfilter 12, libcurl 8.23, gcc 16.2 UCRT64,
`sizeof(off_t)==4` unless `-D_FILE_OFFSET_BITS=64`, default `_fmode` text, IANA zones unsupported by GLib on Windows,
"Windows only: every Linux facility is replaced by its best Windows equivalent").

Files in scope:

| File | Lines | Platform exposure |
|---|---|---|
| `src/audio/output.c` | 575 | WAV recorder (`open`/`write`/`lseek`/`fsync`), libcurl Icecast PUT, GIO SHOUTcast socket, worker threads |
| `src/audio/decoder.c` | 429 | **Heavy**: `openat`/`O_NOFOLLOW`/`O_DIRECTORY` chain, `lstat`/`fstat` identity with `st_ino`/`st_mtim`/`st_ctim`, `pread` |
| `src/audio/transition.c` | 125 | `g_mkstemp_full(O_CLOEXEC)`, `write`, `close`, `g_unlink` |
| `src/audio/tempo.c` | 135 | FFmpeg avfilter only (portable; FFmpeg-8 behaviour check) |
| `src/audio/dsp.c` | 899 | Pure math (libm numerics only) |
| `src/audio/engine.c` | 3509 | GThread/GCond timing, wall clock, `GTimeZone` (IANA), big stack arrays, backend contract |
| `src/media/analyzer.c` | 533 | **Heavy**: same as decoder + `mkdirat`/`openat(O_CREAT|O_EXCL)`/`unlinkat`/`fchmod`/`fsync(dir)`/`read`/`write`, `/` separators |
| `src/media/loudness.c` | 113 | Pure math |
| `src/media/rhythm.c` | 283 | Pure math (`long double`, `lround`→32-bit `long`) |
| `include/radios/audio*.h`, `media*.h`, `loudness.h`, `rhythm.h` | – | Contracts (documented below); no OS types in signatures |

None of these files use: `pthread_*`, `sched_*`, rtkit, `clock_gettime`, `nanosleep`, signals, `/proc`, `/dev/urandom`,
`flock`, `rename`/`link`, `fork`/`exec`, `getuid`. All threading is GThread/GMutex/GCond/GPrivate + C11 `<stdatomic.h>`;
all sockets are GIO or libcurl. The hard work is in file access (decoder.c, analyzer.c) and in engine timing/stack/timezone.

---------------------------------------------------------------------------------------------------------------------

## 0. Summary of required changes (checklist)

1. **Compile blockers** (mingw-w64 has none of these): `O_CLOEXEC`, `O_NOFOLLOW`, `O_DIRECTORY`, `O_NONBLOCK`, `openat`,
   `mkdirat`, `unlinkat`, `fchmod`, `pread`, `lstat`, `fsync`, `struct stat.st_mtim/st_ctim`. Sites: decoder.c 75,78,82,98,115,
   143,151,154,165,168,346,65; analyzer.c 69,72,75,85,92,94,104,117,134,235,243,247,258,262,447,451,452,480,497,501,506,520;
   output.c 124,142; transition.c 71. Replace with the platform API in §2 (Win32 only, no POSIX fallback needed).
2. **Binary mode everywhere**: WAV recorder (output.c:124), transition temp WAV (transition.c:71), staged copy read/write
   (analyzer.c:485/489). Text mode would turn `0x0A` into `0x0D 0x0A` and stop reads at `0x1A` → silently corrupt audio.
3. **Handle inheritance**: every handle must be non-inheritable (`O_CLOEXEC` → `_O_NOINHERIT` / `bInheritHandle=FALSE`), or
   the spawned SDBASE `python.exe` inherits WAV/decoder handles and keeps files locked.
4. **Sharing modes are part of the contract on Windows** (§3.3): decoder handle = `FILE_SHARE_READ|FILE_SHARE_DELETE`,
   analyzer handle = `READ|WRITE|DELETE`, recorder = `READ|DELETE`. Without `FILE_SHARE_DELETE` the UI's
   "unload then delete preview file" (src/ui/audio_ui.c:1256-1260) silently leaks files.
5. **File identity** = (VolumeSerialNumber, 128-bit FileId, EndOfFile, LastWriteTime) from
   `GetFileInformationByHandleEx(FileIdInfo/FileBasicInfo/FileStandardInfo)`; "lstat" = same query on a path opened with
   `FILE_FLAG_OPEN_REPARSE_POINT` (or one `GetFileInformationByName(FileStatBasicByNameInfo)` call on Win11 24H2+).
   Do **not** compare ChangeTime (backup tools clearing the Archive bit would fail on-air decks).
6. **Path rules** (§3.4): reject `:` beyond the drive letter (NTFS alternate data streams!), device namespaces
   (`\\?\`, `\\.\`), reserved device names (`CON`, `NUL`, `COM1`…), trailing dot/space components; separator-aware,
   case-insensitive prefix checks; persisted relative paths use `/`.
7. **64-bit offsets**: build with `-D_FILE_OFFSET_BITS=64` (decided) and still use `gint64` + `_lseeki64`/`ReadFile`
   offsets explicitly; `MAX_FILE_BYTES` is exactly 2 GiB which does not fit a 32-bit `off_t`.
8. **Engine timing on Windows** (§4.6.3): MMCSS "Pro Audio" for `radios-render`, `timeBeginPeriod(1)` + opt out of EcoQoS
   power throttling (incl. `PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION`, Win11 ignores timer requests of minimized
   windows otherwise), optional `SetThreadExecutionState` keep-awake.
9. **Stack size**: link every exe with `-Wl,--stack,8388608`; convert engine.c:2961/3389 500-item arrays (~732 KB each) to heap.
10. **Time zone**: engine.c:1113/1195 must use `radios_time_zone_new_identifier()` (ICU mapping per PLATFORM_FACTS);
    core `loudness_policy.c:31-35` (TZDIR `/usr/share/zoneinfo` file test) currently rejects even `"UTC"` on Windows,
    which makes `radios_audio_engine_apply_preferences()` fail → cross-area blocker.
11. **libcurl TLS**: `CURLSSLOPT_NATIVE_CA` for https Icecast; single process-wide `curl_global_init`.
12. **Tests**: test_audio_decoder.c, test_media.c, test_output.c, test_transition.c need Windows adaptations (§6) and
    helpers (§7).

---------------------------------------------------------------------------------------------------------------------

## 1. Build-level items

| Where | Construct | Windows action |
|---|---|---|
| output.c:1, decoder.c:1, transition.c:1, analyzer.c:1 | `#define _POSIX_C_SOURCE 200809L` | Harmless on mingw (also turns on `__USE_MINGW_ANSI_STDIO`). Keep or remove; it no longer provides `openat` etc. Replace the POSIX includes (`<unistd.h>`, `<fcntl.h>`, `<sys/stat.h>`) with `#include "radios/platform.h"`. |
| CMakeLists.txt:6 | `CMAKE_C_EXTENSIONS OFF` (`-std=c11`) | Fine. `<windows.h>` compiles under `-std=c11`; it is a system header so `-Wpedantic -Werror` (lines 35-37) do not fire inside it. platform.c may use `-std=gnu11` if needed. |
| global | `off_t` 32-bit | `-D_FILE_OFFSET_BITS=64` globally (decided). Caveat: with it mingw-w64 `#define`s `stat`→`_stat64`; **never pass a `struct stat`/`GStatBuf` between app code and GLib on Windows** (layout must match the GLib DLL's build). This area stops using `struct stat` completely (identity API §2.1). Tests that call `g_stat()` (test_transition.c:52) must use the identity API or `GFileInfo`. |
| global | default `_fmode` = text | App sets binary (decided), but every open in this area passes `_O_BINARY` explicitly (tests and helpers may not link the startup object). |
| CMakeLists.txt:46 | `radios_audio` = decoder, tempo, engine, **pulse_backend**, dsp, transition, output, patchbay, channel_input | Replace `pulse_backend.c` with the WASAPI backend (other area); this area's files compile unchanged except the replacements listed here. Link `radios_audio` with `-lavrt -lwinmm` (MMCSS, timer) and platform lib. |
| link | stack reserve | `-Wl,--stack,8388608` on every executable (app + tests). GNU ld's PE default is 2 MiB; Linux main thread and GThreads have 8 MiB. GThreads on Windows use `_beginthreadex(…, 0 …)` → inherit the exe's reserve, so this one flag covers all threads (x64 address space makes 70+ × 8 MiB reserves harmless). See §4.6.5 for the concrete large frames. |
| FFmpeg | MSYS2 FFmpeg 8.x (avcodec 63) | All FFmpeg APIs used in this area exist in 8.x: `AVChannelLayout`, `swr_alloc_set_opts2`, `AVFormatContext.io_open`, `max_probe_packets`, `AV_FRAME_FLAG_CORRUPT`, `AVCodecContext.pkt_timebase/max_samples`, `avfilter_graph_alloc_filter`/`avfilter_init_str`/`avfilter_link`/`AVFilterGraph.nb_threads`, `av_buffersrc_add_frame_flags`. Verify at first compile; see tempo §4.4 for a behavioural check. |
| runtime DLLs | `libmp3lame-0.dll` | Must be shipped (output.c:150 uses `libmp3lame`). See §4.1.6. |
| runtime DLLs | GLib `gspawn-win64-helper*.exe` | Tests spawn `ffmpeg.exe` via `g_spawn_sync` (test_audio_decoder.c:88, test_media.c:125); GLib on Windows needs its spawn helpers next to the GLib DLL (present in `C:\msys64\ucrt64\bin`). |

---------------------------------------------------------------------------------------------------------------------

## 2. Platform API needed by this area (exact signatures and semantics)

All functions live in `include/radios/platform.h` / `src/core/platform.c` (Windows implementation only). UTF-8 in,
UTF-8 out; internally convert with `g_utf8_to_utf16()` and use `W` APIs; prefix `\\?\` (or `\\?\UNC\`) when the wide path
is ≥ `MAX_PATH` (path must be canonical, backslash-separated, no `.`/`..`, before prefixing). On failure functions return
`-1`/`NULL` and set `errno` via `radios_errno_from_win32(GetLastError())` (never leave `errno == 0`; default `EIO`).

### 2.1 File identity (replaces `struct stat` + `same_file()` in decoder.c:56-61, analyzer.c:57-62)

```c
typedef enum {
    RADIOS_FILE_MISSING = 0, RADIOS_FILE_REGULAR, RADIOS_FILE_DIRECTORY,
    RADIOS_FILE_LINK,   /* reparse point whose tag IsReparseTagNameSurrogate(): symlink, junction, mount point */
    RADIOS_FILE_OTHER   /* GetFileType()!=FILE_TYPE_DISK (pipe, char device such as NUL/CON) */
} RadioSFileKind;
typedef struct {
    RadioSFileKind kind;
    guint64 volume;          /* FILE_ID_INFO.VolumeSerialNumber            (st_dev) */
    guint8  id[16];          /* FILE_ID_INFO.FileId (FILE_ID_128)          (st_ino) */
    guint64 size;            /* FILE_STANDARD_INFO.EndOfFile               (st_size) */
    gint64  mtime;           /* FILE_BASIC_INFO.LastWriteTime, 100 ns units (st_mtim) */
    gint64  ctime;           /* FILE_BASIC_INFO.ChangeTime  (informational, NOT compared) */
    guint32 attributes;      /* FILE_ATTRIBUTE_* */
    guint32 reparse_tag;     /* 0 unless FILE_ATTRIBUTE_REPARSE_POINT */
} RadioSFileIdentity;

/* fstat(): queries the handle behind a CRT fd (_get_osfhandle). */
int radios_file_identity_fd(int fd, RadioSFileIdentity *out);
/* lstat(): never follows the last component. RADIOS_FILE_MISSING + return -1/ENOENT if absent.
 * Fast path (Win11 24H2+, resolve once via GetProcAddress(kernel32/kernelbase, "GetFileInformationByName")):
 *   GetFileInformationByName(wpath, FileStatBasicByNameInfo, &FILE_STAT_BASIC_INFORMATION) — gives FileId128,
 *   VolumeSerialNumber, LastWriteTime, ChangeTime, EndOfFile, FileAttributes, ReparseTag, DeviceType without a handle.
 * Fallback: CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL,
 *   OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS) + same queries as the fd variant + CloseHandle. */
int radios_file_identity_path(const gchar *utf8_path, RadioSFileIdentity *out);
/* Equality used by every "file changed / replaced" check: kind, volume, id, size and mtime. */
gboolean radios_file_identity_same(const RadioSFileIdentity *a, const RadioSFileIdentity *b);
```

Implementation notes:
* fd variant: `GetFileInformationByHandleEx(h, FileIdInfo, …)`; if that fails with `ERROR_INVALID_PARAMETER`
  (FAT32/exFAT, some SMB servers) fall back to `GetFileInformationByHandle()` → `dwVolumeSerialNumber`,
  `nFileIndexHigh/Low` zero-extended into `id`. Then `FileBasicInfo` (times, attributes), `FileStandardInfo`
  (EndOfFile, Directory), `FileAttributeTagInfo` (reparse tag), `GetFileType`.
* The path and fd variants must fill identical fields from identical sources so that `same()` compares like with like.
* Query via a handle (or `GetFileInformationByName`) — **never** `FindFirstFileW`/directory enumeration data, which NTFS
  updates lazily (stale size/time while a writer has the file open).
* Why ChangeTime is excluded: on Linux `st_ctim` changes only on chmod/chown/xattr/link-count, rare for media. On Windows
  ChangeTime changes whenever any attribute changes — backup software clearing `FILE_ATTRIBUTE_ARCHIVE`, OneDrive
  pin/unpin, indexer flags — which would make a playing deck fail with "değişti". With the decoder denying write sharing
  (§3.3) content cannot change under an open decoder anyway.
* LastWriteTime granularity: NTFS 100 ns but **updated at handle cleanup** for writes through another handle (NTFS/FAT
  defer the timestamp until close/flush); FAT32 2 s, exFAT 10 ms. In-place modification detection therefore relies on
  deny-write sharing (decoder) or the second SHA-256 pass (analyzer), not on timestamps.

### 2.2 Path policy

```c
/* TRUE for "X:\..." (drive-absolute) and "\\server\share\..." (UNC). Stricter than g_path_is_absolute(),
 * which on Windows also accepts "\foo" (current-drive relative). */
gboolean radios_path_is_absolute(const gchar *utf8);
/* Local media/recording path acceptable on Windows: absolute per above (after g_canonicalize_filename),
 * not "\\?\", "\\.\", "//?/", "//./"; no ':' except at index 1 (else NTFS alternate data stream);
 * no component equal (case-insensitive, with or without extension) to CON PRN AUX NUL COM0-9 LPT0-9
 * COM¹²³ LPT¹²³ CONIN$ CONOUT$; no component ending in '.' or ' '; no characters < 0x20 or <>"|?*.
 * Sets G_IO_ERROR_INVALID_FILENAME with a Turkish message. */
gboolean radios_path_local_valid(const gchar *utf8, GError **error);
/* Separator-aware prefix test: TRUE when 'path' equals 'root' or continues with a separator after it.
 * Comparison is ordinal case-insensitive (CompareStringOrdinal(..., TRUE)) after normalising '/' to '\'
 * and upper-casing the drive letter. *rest = offset of the first char after the separator. */
gboolean radios_path_has_dir_prefix(const gchar *path, const gchar *root, gsize *rest);
/* Native relative path -> persisted/portable form: every '\' becomes '/'. */
gchar *radios_path_to_portable(const gchar *native_relative);
```

### 2.3 No-follow opens and directory handles (replace openat chains)

```c
typedef enum {
    RADIOS_OPEN_READ          = 1 << 0,   /* GENERIC_READ */
    RADIOS_OPEN_SHARE_WRITE   = 1 << 1,   /* add FILE_SHARE_WRITE (default share = READ|DELETE) */
    RADIOS_OPEN_SEQUENTIAL    = 1 << 2    /* FILE_FLAG_SEQUENTIAL_SCAN hint */
} RadioSOpenFlags;
/* Open an existing regular file without following a link in ANY component (decoder.c open_local, analyzer.c
 * open_local_path). Returns a CRT fd (_open_osfhandle(h, _O_RDONLY) — binary, non-inheritable handle).
 * errno: ENOENT/ENOTDIR (missing), ELOOP (a component or the leaf is a name-surrogate reparse point),
 * EISDIR (leaf is a directory), ENODEV (leaf not FILE_TYPE_DISK), EBUSY (ERROR_SHARING_VIOLATION),
 * EACCES, ENAMETOOLONG, EIO. */
int radios_open_nofollow(const gchar *utf8_abs, RadioSOpenFlags flags);

typedef struct RadioSDir RadioSDir;   /* held directory handle + its canonical UTF-8/UTF-16 path */
RadioSDir *radios_dir_open_nofollow(const gchar *utf8_abs_dir);                     /* open("/")+openat chain */
RadioSDir *radios_dir_open_child(RadioSDir *parent, const gchar *name, gboolean create); /* mkdirat(EEXIST ok)+openat(O_DIRECTORY|O_NOFOLLOW) */
int        radios_dir_create_file(RadioSDir *dir, const gchar *name);              /* openat(O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW) -> fd */
int        radios_dir_unlink(RadioSDir *dir, const gchar *name);                   /* unlinkat(dir, name, 0) */
int        radios_dir_identity(RadioSDir *dir, RadioSFileIdentity *out);           /* fstat(dirfd) */
int        radios_dir_sync(RadioSDir *dir);                                        /* fsync(dirfd): no-op, returns 0 */
const gchar *radios_dir_path(const RadioSDir *dir);
void       radios_dir_close(RadioSDir *dir);
```

Windows algorithm ("held chain", pure Win32, TOCTOU-safe):
1. Canonicalise (`g_canonicalize_filename`), validate with `radios_path_local_valid`, split into root (`C:\` or
   `\\server\share\`) + components (split on **both** separators).
2. Open the root, then each directory component by its full path with
   `CreateFileW(p, FILE_LIST_DIRECTORY|FILE_TRAVERSE|FILE_READ_ATTRIBUTES|SYNCHRONIZE,
   FILE_SHARE_READ|FILE_SHARE_WRITE /* no FILE_SHARE_DELETE */, NULL, OPEN_EXISTING,
   FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT)`; reject with `ELOOP` if `FileAttributeTagInfo` shows a
   name-surrogate reparse tag, `ENOTDIR` if not a directory. **Keep every handle open** until the leaf is opened.
   - The handle must include a data access right (`FILE_LIST_DIRECTORY` = `FILE_READ_DATA`): NT share-access checks ignore
     handles that only have `FILE_READ_ATTRIBUTES`/`SYNCHRONIZE`, so an attribute-only handle would not block a rename.
   - Holding the chain without `FILE_SHARE_DELETE` makes every component un-renameable/undeletable (rename needs `DELETE`
     access → sharing violation), so no component can be swapped for a junction between check and use. This is the
     Windows equivalent of the Linux `openat(parent, O_NOFOLLOW)` walk.
3. Open the leaf by full path with `FILE_FLAG_OPEN_REPARSE_POINT` (never follows), check kind (REGULAR/DIRECTORY), then
   release the parent handles (Linux closes parent fds too).
4. Allowed reparse tags: only name surrogates are "links". Cloud-file placeholders (`IO_REPARSE_TAG_CLOUD_*`,
   OneDrive) and dedup are not links — but see §3.6 for hydration.
5. `radios_dir_open_child(create=TRUE)`: `CreateDirectoryW(parent\name, NULL)` (ignore `ERROR_ALREADY_EXISTS`), then
   open as in step 2 and keep the handle (`RadioSDir` holds it without `FILE_SHARE_DELETE` for its lifetime, which
   freezes the import directory exactly like the Linux dirfd identity check intends).
6. `radios_dir_create_file`: `CreateFileW(dir\name, GENERIC_WRITE|DELETE|FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_DELETE,
   NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL)` → `_open_osfhandle(h, _O_WRONLY)` (binary). `CREATE_NEW` = `O_EXCL`
   (fails with `ERROR_FILE_EXISTS` → `EEXIST`; it never follows a pre-existing link because it fails on any existing
   name). `FILE_SHARE_READ` is required because analyzer.c:507 re-opens the copy for reading while this handle is open.
7. `radios_dir_unlink`: prefer deleting through an open handle with
   `SetFileInformationByHandle(h, FileDispositionInfoEx, {FILE_DISPOSITION_FLAG_DELETE|FILE_DISPOSITION_FLAG_POSIX_SEMANTICS})`
   (name disappears immediately even while other handles are open; fall back to `FileDispositionInfo` on FAT/exFAT), else
   `DeleteFileW(dir\name)`. Windows 10 1809+ `DeleteFileW` itself uses POSIX semantics on NTFS (verify); on FAT the name
   stays "delete pending" until the last handle closes.
8. Optional alternative (true openat): `NtCreateFile` with `OBJECT_ATTRIBUTES.RootDirectory = parent handle`,
   `FILE_OPEN_REPARSE_POINT` (0x00200000), `FILE_DIRECTORY_FILE`/`FILE_NON_DIRECTORY_FILE`, `FILE_SYNCHRONOUS_IO_NONALERT`,
   dispositions `FILE_OPEN`/`FILE_CREATE`/`FILE_OPEN_IF`; NTSTATUS → `RtlNtStatusToDosError` → errno. Same results; only
   needed if the held-chain blocking of concurrent renames turns out to be a problem.

### 2.4 I/O primitives

```c
/* pread(): ReadFile(h, buf, (DWORD)MIN(count, 0x7FFFF000), &got, &ov) with ov.Offset/OffsetHigh = offset on a
 * synchronous handle. ERROR_HANDLE_EOF -> return 0. Never returns -1 with errno==0.
 * WARNING: unlike POSIX, this MOVES the handle's file pointer; never mix with read()/_read() on the same fd. */
gssize radios_pread(int fd, void *buffer, gsize count, gint64 offset);
/* write(): _write loop with count clamped to INT_MAX per call; EINTR does not exist on Windows. */
gboolean radios_write_all(int fd, const void *data, gsize size);
gint64 radios_lseek64(int fd, gint64 offset, int whence);        /* _lseeki64 */
int    radios_fsync(int fd);                                      /* FlushFileBuffers(_get_osfhandle(fd)) (= g_fsync/_commit) */
int    radios_fchmod_private(int fd);                             /* fchmod(fd, 0600): no-op, returns 0 (ACL inherited from the RadioS root) */
/* open(path, O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC, 0600) for the WAV recorder: CREATE_NEW, GENERIC_WRITE,
 * FILE_SHARE_READ|FILE_SHARE_DELETE, non-inheritable, FILE_ATTRIBUTE_NORMAL; returns binary CRT fd. */
int    radios_open_create_exclusive(const gchar *utf8_abs);
/* g_mkstemp_full(tmpl, O_RDWR|O_CLOEXEC, 0600) equivalent: g_mkstemp_full(tmpl, O_RDWR|_O_BINARY|_O_NOINHERIT, 0600). */
int    radios_mkstemp_private(gchar *tmpl_utf8);
int    radios_errno_from_win32(DWORD error);
void   radios_secure_zero(gpointer data, gsize size);             /* SecureZeroMemory; replaces memset() password wipes */
```

`radios_errno_from_win32` minimum table: `ERROR_FILE_NOT_FOUND`/`ERROR_PATH_NOT_FOUND`/`ERROR_INVALID_DRIVE`→ENOENT;
`ERROR_ACCESS_DENIED`/`ERROR_PRIVILEGE_NOT_HELD`→EACCES; `ERROR_SHARING_VIOLATION`/`ERROR_LOCK_VIOLATION`→EBUSY;
`ERROR_FILE_EXISTS`/`ERROR_ALREADY_EXISTS`→EEXIST; `ERROR_DISK_FULL`/`ERROR_HANDLE_DISK_FULL`→ENOSPC;
`ERROR_FILE_TOO_LARGE`→EFBIG; `ERROR_DIRECTORY`→ENOTDIR; `ERROR_CANT_RESOLVE_FILENAME`/`ERROR_STOPPED_ON_SYMLINK`→ELOOP;
`ERROR_FILENAME_EXCED_RANGE`→ENAMETOOLONG; `ERROR_INVALID_NAME`/`ERROR_INVALID_PARAMETER`→EINVAL;
`ERROR_NOT_READY`/`ERROR_DEVICE_NOT_CONNECTED`/`ERROR_CRC`/anything else→EIO. `ERROR_DELETE_PENDING`→ENOENT.

### 2.5 Threads, timers, power, time zones, network init

```c
gpointer radios_thread_audio_priority_begin(const gchar *task);   /* AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
                                                                     fallback SetThreadPriority(THREAD_PRIORITY_HIGHEST) */
void     radios_thread_audio_priority_end(gpointer cookie);        /* AvRevertMmThreadCharacteristics */
void     radios_timer_resolution_acquire(void);                    /* refcounted timeBeginPeriod(1) + SetProcessInformation(
                                                                      ProcessPowerThrottling, {CURRENT_VERSION,
                                                                      ControlMask=EXECUTION_SPEED|IGNORE_TIMER_RESOLUTION(0x4),
                                                                      StateMask=0}) */
void     radios_timer_resolution_release(void);                    /* timeEndPeriod(1) when count reaches 0 */
void     radios_power_keep_awake(gboolean on);                     /* refcounted SetThreadExecutionState(ES_CONTINUOUS|
                                                                      ES_SYSTEM_REQUIRED|ES_AWAYMODE_REQUIRED) */
GTimeZone *radios_time_zone_new_identifier(const gchar *iana_or_windows_id); /* per PLATFORM_FACTS (ICU ucal_getWindowsTimeZoneID) */
void     radios_curl_global_init(void);                            /* once per process, from main()/test main, before threads */
void     radios_curl_apply_tls_defaults(CURL *curl);               /* CURLOPT_SSL_OPTIONS |= CURLSSLOPT_NATIVE_CA */
```

`PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION` (0x4) may be missing in mingw headers — define it. Background: since
Windows 11 a process whose windows are minimized/occluded loses its `timeBeginPeriod` request unless it opts out this way;
since Windows 10 2004 the request is per-process.

---------------------------------------------------------------------------------------------------------------------

## 3. Cross-cutting Windows semantics this area depends on

### 3.1 Text vs binary
Default `_fmode` is text. `_open`/`_wopen` without `_O_BINARY`, `read()`/`write()` on such fds translate `\n`↔`\r\n` and
treat `0x1A` as EOF on read. Every fd in this area carries audio bytes → binary always (handles made by `CreateFileW` +
`_open_osfhandle` without `_O_TEXT` are binary). FFmpeg custom AVIO and `ReadFile` are unaffected.

### 3.2 Handle inheritance
CRT handles are inheritable unless `_O_NOINHERIT`; `CreateFileW` handles are not inheritable when
`lpSecurityAttributes == NULL`. RadioS spawns the SDBASE Python service and helper tools; inherited handles would keep WAV
recordings/decoded files locked after RadioS exits or crashes and block deletes/renames. Map `O_CLOEXEC` → non-inheritable
everywhere.

### 3.3 Sharing-mode model (new contract on Windows)

| Handle | Access | Share mode | Why |
|---|---|---|---|
| Decoder source (decoder.c:352) | `GENERIC_READ` | `READ | DELETE` | Others may read; **writers are refused while a deck holds the verified file** (in-place edits cannot reach on-air audio; replacement/rename/delete still possible and detected by path identity → CHANGED). `DELETE` sharing lets audio_ui.c:1260 delete the preview right after `radios_audio_engine_unload()` returns although the deck worker closes the decoder asynchronously (engine.c:684-687), and lets SDBASE move/replace library files (with POSIX-semantics rename). |
| Analyzer source (analyzer.c:92) | `GENERIC_READ` | `READ | WRITE | DELETE` | Must open while our own staging handle is writing (analyzer.c:507) and while another writer exists (test_media.c `changed_source` keeps a writer open); modification is detected by the second SHA-256 pass (analyzer.c:373-377). |
| Staged copy (analyzer.c:480) | `GENERIC_WRITE|DELETE` | `READ | DELETE` | Analyzer re-opens it for reading; cleanup deletes it while still open (analyzer.c:520 before 522). |
| Import dir chain / `RadioSDir` | `FILE_LIST_DIRECTORY|…` | `READ | WRITE` (no DELETE) | Freezes the directory chain (no junction swap). |
| WAV recorder (output.c:124) | `GENERIC_WRITE` | `READ | DELETE` | Live file can be inspected; same unlink semantics as Linux. |
| Transition temp (transition.c:71) | via `g_mkstemp_full` → `_wopen` | `_SH_DENYNO` = `READ | WRITE` | Closed before any delete/decode (transition.c:107/110), so the missing DELETE share is harmless. |

Decision needed by the lead (recommended: as in the table). Alternative "A": decoder shares WRITE too (Linux-identical);
then in-place modification through a still-open writer is not detected until the writer closes (NTFS defers LastWriteTime
to handle cleanup), i.e. modified bytes could reach air. Recommendation B costs only a "file in use" error for a tag
editor while the track is loaded, and test `changed_file` must change (§6.1).

New error text when the decoder open fails with `EBUSY` (sharing violation): map to `RADIOS_AUDIO_DECODER_ERROR_IO`,
message "Ses dosyası başka bir uygulama tarafından yazılmak üzere açık; o uygulamayı kapatıp yeniden deneyin."

### 3.4 Paths
* `g_canonicalize_filename()` on Windows returns `\`-separated paths; `g_path_get_dirname`/`basename` accept both.
  Hard-coded `"/"` splitting (decoder.c:74, analyzer.c:68, 449) and `'/'` checks (analyzer.c:443) break → use
  `radios_path_has_dir_prefix` and split on `G_IS_DIR_SEPARATOR`.
* `:` after the drive letter addresses an NTFS alternate data stream ("song.wav:x" opens a hidden stream of
  "song.wav"; "rec\show:1.wav" creates file "show" with a stream). Reject (§2.2) in decoder, analyzer, recorder.
* `strstr(path, "://")` (decoder.c:339, analyzer.c:82) still rejects URLs; on Windows also reject device namespaces
  explicitly (they never reach `CreateFileW`; opening `\\.\pipe\x` would connect to a pipe server).
* Reserved device names (`NUL`, `CON`, `COM1`, … also `nul.wav` on Windows 10) open devices → `GetFileType != DISK` →
  `RADIOS_FILE_OTHER` → same error as a FIFO on Linux (INVALID).
* Trailing `.`/space in a component is silently stripped by Win32 → reject (else the recorder's reported path ≠ actual file).
* Case-insensitive file system: path string equality (engine.c:689, 3047, 3153) only decides "same source → no reload";
  a case difference just causes a reload — acceptable, no change. Prefix/confinement checks must be case-insensitive.
* Long paths: contracts allow 4096 UTF-8 bytes (output.c:506, engine.c:1535, 2835, 2854). The platform layer prefixes
  `\\?\` for long paths; additionally ship the app manifest with `<longPathAware>true</longPathAware>` (Windows also
  needs the `LongPathsEnabled` policy for non-prefixed paths in third-party DLLs).
* Persisted/relative paths are `/`-separated (analyzer.c:517, `RadioSMediaAnalysis.relative_path`, see media.h:47-49).

### 3.5 Large files
`MAX_FILE_BYTES`/`RADIOS_MEDIA_MAX_SIZE_BYTES` = exactly 2³¹ bytes (decoder.c:14, media.h:13) — one more than a signed
32-bit `off_t`. All sizes/offsets are `guint64/gint64` in `RadioSFileIdentity` and `radios_pread`. WAV recording reaches
4 GiB (output.c:13); sequential `_write` is fine beyond 4 GiB, header rewrite at offset 0 uses `radios_lseek64`.
On FAT32 the 4 GiB−1 file limit is hit ~5 bytes before the RIFF limit → `ERROR_FILE_TOO_LARGE`/`ENOSPC` → "WAV PCM write
failed", header still finalized (valid partial WAV) — same as Linux vfat, document only.

### 3.6 Windows environment hazards (document in operator docs; some need code)
* **Controlled Folder Access** (Defender ransomware protection) blocks writes by unknown apps to Documents/Music/Desktop/
  Pictures/Videos: recorder `CreateFileW` → `ERROR_ACCESS_DENIED`. Message already includes `g_strerror`; add a hint in
  the Turkish UI text ("Windows Güvenliği › Denetimli klasör erişimi RadioS'u engelliyor olabilir").
* **OneDrive Files On-Demand**: library files may be cloud placeholders (`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`/
  `FILE_ATTRIBUTE_OFFLINE`); the first read triggers a download that can block a deck worker for minutes. Recommended:
  decoder/analyzer reject such files up front with `RADIOS_AUDIO_DECODER_ERROR_IO` / `RADIOS_MEDIA_ERROR_IO` and message
  "Dosya yalnızca bulutta (OneDrive); önce bu bilgisayara indirin." (check `attributes` from the identity query).
* **Defender real-time scan** on close/open adds latency and may briefly hold freshly written files (staged copy, WAV,
  preview) — other components' rename/delete must retry on sharing violation (platform atomic-replace already does).
* **Removable/network media** errors (`ERROR_DEVICE_NOT_CONNECTED`, `ERROR_NETNAME_DELETED`) → `EIO` → deck ERROR, same as
  Linux `EIO`.

---------------------------------------------------------------------------------------------------------------------

## 4. File-by-file analysis

### 4.1 `src/audio/output.c` — MAIN recorder and Internet stream delivery

Contract (include/radios/audio_output.h:31-75): one recorder and one stream handle per engine, both fed only by the final
MAIN mix (48 kHz stereo float) through a 5 s lock-free queue (`QUEUE_FRAMES`, output.c:12); overrun fails the output
without blocking the renderer; recorder = WAV PCM16, absolute path, **existing files never overwritten**, finalized on
stop, 4 GiB RIFF limit; stream = HTTP(S) PUT raw MP3 (Icecast 2.4) or SHOUTcast 1/2 legacy source login on port+1;
metadata via a separate worker; passwords never in snapshots/errors and wiped after use.

#### 4.1.1 Queue and threads (output.c:15-100, 343-370, 430-444) — portable, keep
* `_Atomic guint64 read, write, …` (31-34) with acquire/release (62-71, 84-96): GCC on x86-64 Windows inlines 64-bit
  atomics (no `libatomic`). Keep.
* `take()` (58-78): polls with `g_cond_wait_until(+10 ms)`; GLib/Windows uses `SleepConditionVariableSRW` rounded up to ms;
  with the default 15.6 ms timer the poll is ~16 ms — irrelevant for a 5 s queue. 5 s starvation detection uses
  `g_get_monotonic_time()` (QPC on Windows) — keep.
* `radios_audio_output_push_internal` (79-100) runs on the render thread with the engine mutex held: atomics + one
  `g_cond_signal` (`WakeConditionVariable`, no mutex needed). Real-time safe enough; keep.
* Spin-waits `while (atomic_load(&pushing)) g_thread_yield();` (360, 484): `g_thread_yield` = `Sleep(0)`/`SwitchToThread`.
  The pusher is the MMCSS render thread (higher priority) so the spin terminates; keep.
* Threads `g_thread_new("radios-output"/"radios-metadata")` (441-442): GLib sets the thread description on Windows.
* Password wipes `memset(password, 0, …)` before `g_free` (311, 363, 397, 426, 471, 486): use `radios_secure_zero()`
  (`SecureZeroMemory`) — a plain memset before free may be elided by GCC at -O2. Not Windows-specific, but trivial to fix now.

#### 4.1.2 WAV recorder `record_run` (output.c:101-147) — must change
| Line | Linux | Windows replacement |
|---|---|---|
| 112-121 `write_all` | `write()` loop, retries `EINTR` | `radios_write_all(fd, data, bytes)` (`_write` per chunk ≤ 16 KiB here; no EINTR). |
| 124 | `open(path, O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC, 0600)` | `radios_open_create_exclusive(path)` = `CreateFileW(CREATE_NEW, GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_DELETE, NULL, …)` → `_open_osfhandle(h, _O_WRONLY)` (binary). Keeps "existing files are never overwritten" atomically (`ERROR_FILE_EXISTS`→`EEXIST`, message "WAV could not be created: File exists" — test_output.c:111-112 asserts the substring "created"). 0600 has no meaning; ACL is inherited from the folder. Pitfall: a name in "delete pending" state returns `ERROR_ACCESS_DENIED` (`EACCES`) instead of `EEXIST` — still a failure, fine. |
| 125 | `g_strerror(errno)` | errno must be set by the platform function (mapping §2.4). |
| 142 | `lseek(fd, 0, SEEK_SET) == 0 && write_all(header) && fsync(fd) == 0` | `radios_lseek64(fd, 0, SEEK_SET) == 0 && radios_write_all(...) && radios_fsync(fd) == 0` (`FlushFileBuffers`). Sequential writes need no seek; total bytes ≤ 4 GiB+44. |
| 143 | `close(fd)` | `_close(fd)` (closes the HANDLE). |
| 506 (`radios_audio_output_start_record`) | `g_path_is_absolute(path)`, ≤4096 bytes, UTF-8 | add `radios_path_is_absolute(path)` (reject `\foo`) and `radios_path_local_valid(path)` (ADS `:`, reserved names, trailing dot/space). Error stays `RADIOS_AUDIO_ERROR_INVALID` with the existing text or "Kayıt dosyası yolu Windows'ta geçersiz." The caller-side name builder is src/audio/patchbay.c:375-385 (`<prefix>-YYYYMMDD-HHMMSS.wav`, no colons) — its `prefix` comes from the routing graph and must be validated with the same rule (patchbay area). |

Finalization semantics to preserve: header written with `frames=0` first (127), PCM16 little-endian conversion (133-137),
header rewritten with the actual count even after a write failure (141-145), `recording_finalized` reflects
seek+write+fsync+close success (144). Disk-full and FAT32 limit → failure with valid partial WAV.

#### 4.1.3 Encoder (output.c:148-194) — portable
`avcodec_find_encoder_by_name("libmp3lame")` (150) with fallback `AV_CODEC_ID_MP3` (151). On Windows FFmpeg builds the
fallback may resolve to **`mp3_mf`** (Media Foundation wrapper), which accepts only S16 input and a few bitrates →
`avcodec_open2` fails with FLTP (155) → "Native MP3 encoder could not be opened". Requirement: ship `libmp3lame-0.dll`
(MSYS2 FFmpeg links LAME) and add a packaging smoke test that `avcodec_find_encoder_by_name("libmp3lame")` is non-NULL.
Optionally make the fallback skip encoders whose `sample_fmts` lack `AV_SAMPLE_FMT_FLTP` (`avcodec_get_supported_config`
in FFmpeg ≥ 7.1).

#### 4.1.4 Icecast/HTTP PUT via libcurl `stream_run` (output.c:195-286)
* `curl_global_init(CURL_GLOBAL_DEFAULT)` lazily inside `g_once` (433). On Windows this also performs `WSAStartup`. libcurl
  ≥ 7.84 is thread-safe here, but the data client (core) uses libcurl too → call `radios_curl_global_init()` once from
  `main()` (and test mains) before any thread; keep the `g_once` as a harmless second call (refcounted by curl).
* `CURLOPT_NOSIGNAL` (232): Unix-only meaning, harmless.
* `CURLOPT_PROTOCOLS_STR "http,https"` (232) requires curl ≥ 7.85 — MSYS2 8.23 OK.
* **TLS**: for `https://` mounts the MSYS2 libcurl uses its configured TLS backend; when the app is relocated the
  OpenSSL CA bundle path baked into the build is wrong. Call `radios_curl_apply_tls_defaults(curl)` in `curl_common()`
  (228-236) → `curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA)` so the Windows certificate store
  is used (works for OpenSSL and Schannel builds). Alternatively ship `ca-bundle.crt` and set `CURLOPT_CAINFO`.
* Proxy: libcurl does not read WinINet/IE proxy settings; same as Linux (env vars only). Document.
* `Transfer-Encoding:` suppression and `CURLOPT_INFILESIZE_LARGE -1` (249, 265) — protocol behaviour, unchanged.
* `stream_progress` (220-227) `curl_off_t` is 64-bit on Windows; fine.
* Status reading `%ld` with `glong`/`long` (283, 421): `long` is 32-bit on Windows, HTTP codes fit; `CURLINFO_RESPONSE_CODE`
  requires `long*` — matches.

#### 4.1.5 SHOUTcast legacy source (output.c:287-342) and metadata worker (371-429)
* GIO `GSocketClient` (298-300), `g_output_stream_write_all`, `g_input_stream_read` — portable (Winsock behind GIO, GIO
  calls `WSAStartup` itself). No SIGPIPE on Windows (GIO uses `MSG_NOSIGNAL` on Linux only); a peer reset surfaces as
  `G_IO_ERROR_CONNECTION_CLOSED/BROKEN_PIPE` → "SHOUTcast connection was closed by the server" (332). Keep.
* `g_socket_client_set_timeout(client, 5)` (298): GIO implements it with its own poll on Windows. Keep.
* Host validation (538) rejects `" /\\?#@[]"`; IPv6 literal bracketing (546-547). Portable.
* Metadata URLs built with `curl_easy_escape` + `g_uri_build` (386-411). Portable; apply TLS defaults in the Icecast branch
  (uses `curl_common`, 407).
* Firewall: outbound only; no Windows Firewall prompt.

#### 4.1.6 Tests touching output.c — see §6.3 (test_output.c uses BSD sockets directly).

### 4.2 `src/audio/decoder.c` — verified local decoder (rewrite of the file-access layer)

Contract (include/radios/audio_decoder.h:26-48): worker-only; mandatory 64-hex SHA-256 verified over the **entire regular
local file** before decoding; **every path component must be free of symbolic links**; every read/seek re-checks that the
opened file is still the one at the path (`ERROR_CHANGED`); custom AVIO only, no FFmpeg protocol or external reference is
ever opened; position in exact 48 kHz output frames; cancellable.

Error-code mapping that tests assert and the port must keep:

| Situation | Linux mechanism | Error | Windows mechanism |
|---|---|---|---|
| empty path, URL, bad SHA | 339 | `INVALID` | + `radios_path_local_valid` failure (ADS, device namespace, reserved name) → `INVALID` |
| leaf missing / symlink / FIFO / device / directory | `lstat` + `S_ISREG` (346) | `INVALID` | `radios_file_identity_path` kind ≠ REGULAR (LINK, OTHER, DIRECTORY, MISSING) |
| size 0 or > 2 GiB | 349 | `LIMIT` | `identity.size` |
| a parent component is a link, or open fails | `open_local` (352) | `IO` | `radios_open_nofollow` fails (`ELOOP`, `ENOENT`, `EBUSY`, `EACCES`) |
| fd identity ≠ pre-open path identity | 354 | `CHANGED` | `radios_file_identity_fd` vs path identity via `radios_file_identity_same` |
| hash mismatch | 104-105 | `HASH` | unchanged |
| path or fd identity changes later | `unchanged()` (62-70) | `CHANGED` | same two identity queries |
| cloud-only placeholder (new) | – | – | `IO`, message per §3.6 |

Line-by-line:

| Line | Linux construct | Windows replacement |
|---|---|---|
| 1-8 | `_POSIX_C_SOURCE`, `<fcntl.h>`, `<sys/stat.h>`, `<unistd.h>` | `#include "radios/platform.h"` |
| 21-23 | `gint fd; struct stat identity;` | `gint fd; RadioSFileIdentity identity;` |
| 56-61 `same_file` | compares `st_dev, st_ino, st_size, st_mtim, st_ctim` | `radios_file_identity_same()` (volume, 128-bit id, size, LastWriteTime; ctime excluded §2.1) |
| 62-70 `unchanged` | `fstat(fd)` + `lstat(path)` + `S_ISREG` each call | `radios_file_identity_fd` + `radios_file_identity_path`; both must be REGULAR and same as `identity`. Cost on Windows: fd query = 3-4 `GetFileInformationByHandleEx` calls; path query = 1 `GetFileInformationByName` (Win11 24H2+, this machine) or `CreateFileW`+queries+`CloseHandle`. Called twice per `radios_audio_decoder_read` (≈12 reads/s per playing deck) — acceptable; do not add caching (it would weaken the contract). |
| 71-86 `open_local` | `g_strsplit(dir, "/")`, `open("/", O_RDONLY|O_DIRECTORY|O_CLOEXEC)`, `openat(…O_NOFOLLOW…)` chain, leaf `O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC` | `radios_open_nofollow(path, RADIOS_OPEN_READ)` (share READ|DELETE per §3.3). `O_NONBLOCK` existed only to avoid blocking on a FIFO open; Windows has no FS FIFOs and device namespaces are rejected before any open. |
| 93-107 `verify_hash` | `pread` 64 KiB blocks, EINTR retry | `radios_pread`; no EINTR. Optionally open with `RADIOS_OPEN_SEQUENTIAL`. |
| 108-119 `local_read` | `pread`; `AVERROR(errno)` on error | `radios_pread`; errno is guaranteed non-zero (an `AVERROR(0)`=0 return would be misread by FFmpeg as "0 bytes"). |
| 120-128 `local_seek` | pure offset arithmetic | unchanged (`AVSEEK_SIZE` returns `identity.size`). |
| 134-135 `deny_external`, 201-203 whitelist/`enable_drefs=0` | FFmpeg sandbox | unchanged. |
| 139-176 `container_complete` | `pread` of WAV/Ogg/MP4 headers | `radios_pread` (same offsets, 64-bit). |
| 204 | `avformat_open_input(&fmt, decoder->path, …)` with `AVFMT_FLAG_CUSTOM_IO` | The path is only a name hint (extension probing); FFmpeg never opens it (custom `pb`). Backslashes/UTF-8 are fine. (FFmpeg's own file protocol on Windows does convert UTF-8→UTF-16, but it is not used.) |
| 339 | `strstr(path, "://")` | keep; add `radios_path_local_valid(path)` → `INVALID`. Edge: `C://music` contains `://`; harmless rejection (canonical form is `C:\music`). |
| 343 | `g_canonicalize_filename(path, NULL)` | keep (result is `\`-separated). |
| 346-348 | `lstat` + `S_ISREG` | `radios_file_identity_path(decoder->path, &before)`; kind must be REGULAR. Also reject `attributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS|FILE_ATTRIBUTE_OFFLINE)` (§3.6) with IO. |
| 349-351 | size limit | unchanged on `before.size`. |
| 352-353 | `open_local` | `radios_open_nofollow`; on failure keep `IO` and the existing message; for `EBUSY` use the sharing-violation message (§3.3). |
| 354-356 | `fstat` + `same_file(before, identity)` | `radios_file_identity_fd` + `radios_file_identity_same`. |
| 427 | `close(fd)` | `_close(fd)`. |

Ownership/thread notes: the decoder belongs to a deck worker (engine.c:664-749); `radios_audio_decoder_close` may happen
after the UI already deleted the file (allowed by `FILE_SHARE_DELETE`). Pending-delete state: reads through the open handle
keep working after a POSIX-semantics delete, as on Linux.

### 4.3 `src/audio/transition.c` — transition preview renderer

Contract (include/radios/audio_transition.h:18-33): worker-only; constant memory; writes a private temporary IEEE-float
stereo WAV in `directory` (caller deletes it, see src/ui/audio_ui.c:1502 `cache_root/transition-preview`), returns path +
SHA-256 + frame count; on failure/cancel leaves no artifact (test_transition.c:72,74 count files).

| Line | Linux | Windows |
|---|---|---|
| 1-8 | POSIX includes | platform.h; `<glib/gstdio.h>` stays. |
| 44-54 `write_all` | `write()` + EINTR, error via `g_io_error_from_errno(errno)` | `radios_write_all` (or `_write` loop); errno from CRT is valid. |
| 68 | template `transition-XXXXXX` | unchanged (no extension; the decoder probes content). |
| 70 | `g_mkdir_with_parents(directory, 0700)` | unchanged (mode ignored on Windows). |
| 71 | `g_mkstemp_full(temporary, O_RDWR | O_CLOEXEC, 0600)` — **`O_CLOEXEC` does not exist on mingw (compile error)** | `radios_mkstemp_private(temporary)` = `g_mkstemp_full(temporary, O_RDWR | _O_BINARY | _O_NOINHERIT, 0600)`. GLib adds `O_CREAT|O_EXCL` and calls `_wopen`; it does **not** add `O_BINARY` for `g_mkstemp_full` (only `g_mkstemp` does) → without it the float WAV is corrupted by `\n`→`\r\n` and its SHA no longer matches what the decoder reads. |
| 73-76, 102 | little-endian header/sample conversion with `GUINT32_TO_LE` | portable. |
| 78 | `gfloat pcm_a[BLOCK*2], pcm_b[…], mix[…]` = 48 KiB stack | fine with 8 MiB stack (and with 2 MiB). |
| 107 | `close(fd)` | `_close(fd)` (unchanged API name works on mingw; keep `close` only if `<io.h>` old names are available — prefer `_close`). |
| 110 | `g_unlink(temporary)` after close | unchanged (file is closed first; no sharing issue). |

### 4.4 `src/audio/tempo.c` — atempo filter wrapper (portable)

* Pure FFmpeg libavfilter: `abuffer` args string `"time_base=1/48000:sample_rate=48000:sample_fmt=flt:channel_layout=stereo"`
  (41-42) contains only integers → locale-safe under the Turkish locale (decimal comma). The tempo ratio is set with the
  typed `av_opt_set_double` (49) — keep; never format ratios with `%f`/`g_strdup_printf` (Turkish locale → `0,9375`).
* `tempo->graph->nb_threads = 1` (40) — keep (deterministic output).
* Line 101 changes `tempo` on an **initialized** atempo instance via `av_opt_set_double(filter->priv, …)` (deliberately not
  `avfilter_graph_send_command`, see the comment at 89-95). This relies on atempo internals. The Windows build moves from
  the Linux distro FFmpeg to FFmpeg 8.x (libavfilter 12): run test_tempo (tests/test_tempo.c:47, 384-387) early; if the
  ratio change stops taking effect, the fallback is `avfilter_graph_send_command(graph, "tempo", "tempo", buf, NULL, 0, 0)`
  with `buf` formatted by `g_ascii_dtostr` (locale-independent) — behaviour change to be accepted by the lead.
* Stack: `gfloat input[4096*2]` (86) = 32 KiB. Fine.

### 4.5 `src/audio/dsp.c` — mixer DSP (pure math, no code change)

* No OS calls. `RadioSAudioDsp` (37-66) embeds 32×2 + 5 `EffectState`s each with `history[48002][2]` floats (22-27,
  384 KiB) → ≈ 26 MB, allocated once with `g_new0` (292). On Windows `calloc` of this size maps fresh zero pages
  (`VirtualAlloc`), cheap; keep.
* Denormals are flushed manually (filter 196-205); MXCSR FTZ/DAZ defaults are the same on Windows x64. Keep.
* Numerics: `pow/exp/sin/cos/tan/log10/floor` come from mingw-w64 + UCRT libm instead of glibc → results can differ in the
  last ulp. All tests in this area compare with tolerances (test_dsp.c `near(…, 1e-6)`, test_transition.c `close_to(…,
  1e-5)`), so no golden-hash problem was found; keep it that way (never compare PCM SHA-256s computed on Linux).
* Very large per-frame stack locals in `radios_audio_dsp_process_frame_with_external` (546-549): ~6 KiB. Fine.

### 4.6 `src/audio/engine.c` — engine core (threads, clocks, scheduling, stack, time zone, backend contract)

#### 4.6.1 Threads (all GThread) — keep, add priority
| Thread | Created | Wait pattern | Windows note |
|---|---|---|---|
| `radios-render` | 1137 (realtime only) | renders 480-frame blocks while every ready bus queue < 4 blocks (962-970), else `g_cond_wait_until(+3 ms)` (971) | Wrap the body of `render_worker` (954-975) with `radios_thread_audio_priority_begin("Pro Audio")` / `…_end`. Without a raised timer resolution the 3 ms wait becomes ~15.6 ms; the 40 ms queue (`RENDER_FRAMES*4`) + WASAPI buffer absorbs that, but margins shrink — see 4.6.3. |
| `radios-devices` (control) | 1136 | `g_cond_wait_until(+20 ms)` (1093); flush settle 8 ms quiet / 40 ms max (24-25, 976-989) | Normal priority. Calls the backend API (§4.6.6). |
| `radios-runner` | 3214 | `+10 ms` (2485) | Normal priority. |
| `radios-decode` ×≤66 | 1389, 1502, 3196 (lazily) | `g_cond_wait` (674), `+10 ms` when 1 s ahead (713) | Normal priority; each deck worker owns its decoder (decoder.c) and hashing. Up to ~72 threads in total; with 8 MiB reserve each this is <600 MB of *reserved* address space — fine on x64. |
| output/metadata | output.c:441-442 | see 4.1.1 | – |

`GPrivate runner_scope` (201) → TLS on Windows; `GMutex`/`GCond` → SRWLOCK/CONDITION_VARIABLE (not recursive, unfair, no
priority inheritance — same class of behaviour as Linux non-PI futex mutexes). A MMCSS-boosted render thread contends for
`engine->mutex` with normal-priority threads; Windows' balance-set manager eventually boosts starved holders. No change,
but keep critical sections short (as now).

#### 4.6.2 Clocks
* `g_get_monotonic_time()` (713, 971, 981-987, 1093, 2485, output.c): QPC-based in GLib ≥ 2.66 on Windows; fine.
* `g_get_real_time()` (825 `block_wall`, 998, 1211, 2008, 2134-2159, 2322, 2413-2428, 2466): GLib on Windows reads
  `GetSystemTimeAsFileTime` (verify for 2.90.1) whose granularity is the system tick (15.6 ms default, 1 ms after
  `timeBeginPeriod(1)`). It is used for UTC-scheduled starts (`runner_frame_locked(…, block_wall + frame*1e6/48000…)`,
  841) and receipts. The coarse clock can only lag true time, so scheduled starts are late by ≤ one tick, never early
  (matches the "never start early" comment at 826-829). With 4.6.3 the error is ≤ 1 ms. Do **not** replace
  `g_get_real_time` with a platform function in the engine: tests wrap it (`-Wl,--wrap=g_get_real_time`,
  cmake/Pdf2HeaderStatus.cmake:7) — but note that with GLib's `__declspec(dllimport)` declarations GNU ld `--wrap` does
  not intercept `__imp_g_get_real_time` references on Windows (test-infra area must verify/replace that technique).

#### 4.6.3 Timer resolution, power throttling, keep-awake (new, Windows-only)
In `radios_audio_engine_new` when `realtime` (1132-1138): `radios_timer_resolution_acquire()` and
`radios_power_keep_awake(TRUE)`; release both in `radios_audio_engine_unref` after the threads are joined (1149-1157).
Reasons: (a) all engine waits are 3-20 ms polls; (b) Windows 11 drops a minimized/occluded process's timer request and may
put it on EcoQoS unless it opts out — a broadcast automation app is normally minimized; (c) system sleep stops the
on-air output (Linux desktops would also suspend, but RadioS on Linux typically ran on always-on systems; on Windows
laptops/desktops sleep-after-idle is on by default).
Optional improvement (needs the WASAPI backend): let the backend signal the engine condition (`g_cond_broadcast`) after
it consumed output so the render loop becomes event-driven instead of 3 ms polling.

#### 4.6.4 Time zone (engine.c:187, 419-432, 1113, 1195-1196, 1209)
`engine->loudness_zone = g_time_zone_new_identifier("UTC")` (1113) works on Windows. But
`g_time_zone_new_identifier(prefs->loudness_policy.timezone)` (1195) returns NULL for IANA ids on Windows
(PLATFORM_FACTS) → "Loudness policy time zone is unavailable". Replace both calls with
`radios_time_zone_new_identifier()` (ICU-mapped). Additionally the preceding
`radios_audio_loudness_policy_validate()` (1193; src/core/loudness_policy.c:31-35) requires the file
`$TZDIR|/usr/share/zoneinfo/<id>` to exist — on Windows it fails even for the default `"UTC"`, so **every**
`radios_audio_engine_apply_preferences()` call fails. That function is in the core area; it must use the same wrapper
(validity = ICU knows the IANA id). `loudness_clock_locked` (419-432) itself (`g_date_time_new_from_unix_utc`,
`g_date_time_to_timezone`) is portable.

#### 4.6.5 Stack usage (Windows default is 2 MiB with GNU ld)
| Site | Size | Thread | Action |
|---|---|---|---|
| engine.c:2961 `RadioSAudioRunItem combined[500]` (`radios_audio_engine_runner_append`) | ≈ 732 KB (`sizeof(RadioSAudioRunItem)` ≈ 1.46 KB: `RadioSAudioTransitionShape` alone ≈ 1 KB of curves) | caller (UI/GTK main loop) | `g_new(RadioSAudioRunItem, run->count + count)` + `g_free` on every return path. |
| engine.c:3389 `RadioSAudioRunItem copies[500]` (`radios_audio_engine_player_runner_configure`) | ≈ 732 KB | caller | heap as above. |
| src/playout/planner.c:1104, 1724 `original[500]` (+`gint64 times[500]`) | ≈ 732 KB each | caller (other area) | heap (cross-area note): planner → engine call chains can stack 1.5 MB. |
| engine.c:995/1073 `RadioSPulseSnapshot physical, settled` | ≈ 133 KB each (`RadioSAudioDevice outputs[64], inputs[64]`, 1 036 B each) | control | fine. |
| `RadioSAudioSnapshot` (audio.h:93-110) on callers' stacks (UI, tests) | ≈ 265 KB | callers | fine with 8 MiB; heap if a caller nests several. |
| render_worker 956 `samples[3][960]` | 11.5 KB | render | fine. |
Primary fix: `-Wl,--stack,8388608` for all executables (restores Linux headroom for every GThread); secondary: the heap
conversions above.

#### 4.6.6 Backend contract the engine relies on (audio_internal.h:16-36) — for the WASAPI backend author
The engine keeps the existing internal API; on Windows `radios_pulse_backend_*` are implemented by the WASAPI backend
(names may be kept or aliased; `pulse_server` becomes a backend hint: NULL = default endpoints; suggest `"null"` = an
in-process headless backend that consumes rings in real time, for CI machines without audio hardware).
* `radios_pulse_backend_new(RadioSAudioRing *outputs[3], RadioSAudioRing *capture, const gchar *server)` (engine.c:1134):
  backend thread(s) consume `outputs[bus]` with `radios_audio_ring_read(ring, buf, n, &last_tag)` (tags enabled,
  `ring_init(…, TRUE)` 1116) and produce microphone frames into `capture` (`OUTPUT_CAPACITY` 8192 frames, no tags).
* `radios_pulse_backend_snapshot()` (995, 1074) must be callable without the engine mutex, cheap (called every 20 ms), and
  fill: `connected`; `ready[3]` (stream exists and accepts PCM); `microphone_ready`; `played_tag[bus]` = tag of the
  **last frame actually played by the device** (Pulse uses the server read index; WASAPI: `IAudioClock::GetPosition`
  mapped through a ring of written tags; never the written position — `actual_cursor_locked` 486-517 and runner receipts
  2111-2183 depend on it); `consumed_frames[bus]` in 48 kHz frames; `output_sample_rate/buffer_frames` (actual);
  `peak/rms`; `error`, `bus_error[bus]`, `microphone_error` (Turkish, non-empty only on real failure: loss detection at
  999-1003 and 1055-1058 tests "not ready AND error text"); `outputs[]/inputs[]` device lists (`name` ≤ 512 bytes UTF-8,
  the persisted id — use the WASAPI endpoint id string; `@default` remains the "system default" sentinel
  (engine.c:348)).
* `radios_pulse_backend_flush()` (1063): drop all queued device audio, keep streams ready when possible; the engine resets
  `played_tag` to 0 afterwards (1083) and copies `ready[]` from the post-flush snapshot (1074, 1082).
  `flush_buses(mask)` (1064) — same for CUE/MONITOR only.
* `configure_output(rate, frames)` (1062): 44100/48000 Hz, power-of-two 64..4096 frames (engine.c:1222); internal PCM
  stays 48 kHz (use `src/audio/output_resampler.c`, portable, for 44.1 kHz or a device mix format ≠ 48 kHz).
* `apply(prefs)`, `refresh()`, `reconnect()` (1065-1068) are called from the control thread every turn (≤ 20 ms):
  `reconnect` must be cheap when nothing is lost (bounded retry, never blocking for seconds).

#### 4.6.7 Other engine items
* Device-name validation (1179-1185): UTF-8 ≤ 512 bytes — WASAPI endpoint ids (`{0.0.0.00000000}.{guid}`) fit.
* Path validation for loads/queues (1535, 2835, 2854): `g_path_is_absolute` → `radios_path_is_absolute` +
  `radios_path_local_valid` (else a `\foo` or `x.wav:ads` path is accepted and fails later in the decoder with a less clear
  message). Keep the 4096-byte limit.
* `g_path_get_basename(spec->path)` for default titles (651): handles `\` on Windows. Keep.
* `g_uuid_string_random()` (644-645): GLib RNG, portable.
* Large heap-embedded state (`RenderHistory history[512]` ≈ 11 MB, 195): allocated via `g_new0` (1100). Fine.

### 4.7 `src/media/analyzer.c` — media analysis and private import staging (rewrite of the file-access layer)

Contract (include/radios/media.h:41-58): worker-only, read-only full decode with custom AVIO; SHA-256 + identity checks
reject a source modified/replaced during analysis (`RADIOS_MEDIA_ERROR_CHANGED`); `stage_*` copies the source to a private
UUID file under the configured `import_root` inside the project (confinement: every component opened without following
links; `RADIOS_MEDIA_ERROR_CONFINEMENT`), analyses that stable snapshot, removes its own copy on failure/cancel, and returns
`relative_path` "safe to send to the backend".

Error mapping to keep (tests assert it): `lstat` failure → `IO` (85); non-regular/link/FIFO → `INVALID_FILE` (86-88);
size 0 or > 2 GiB → `LIMIT` (89-91); open failure → `IO` (92-93); identity mismatch → `CHANGED` (95-97, 104-106);
import root outside project or a link in its chain → `CONFINEMENT` (443-445, 452-455, 501-504).

| Line | Linux construct | Windows replacement |
|---|---|---|
| 1-9 | POSIX includes | platform.h |
| 16-21 `LocalInput.fd` | CRT fd | unchanged type; created by `radios_open_nofollow`. |
| 57-62 `same_file` | `struct stat` fields | `radios_file_identity_same` |
| 64-77 `open_local_path(path, flags)` | canonicalize, split `"/"`, `open("/")`, `openat(O_NOFOLLOW|O_DIRECTORY)` chain, leaf `openat(flags|O_NOFOLLOW|O_CLOEXEC)`; called for files (92) **and** for the project root directory (447, `O_RDONLY|O_DIRECTORY`) | files: `radios_open_nofollow(path, RADIOS_OPEN_READ|RADIOS_OPEN_SHARE_WRITE)`; directory: `radios_dir_open_nofollow(root)` returning `RadioSDir*`. |
| 79-99 `open_source` | `strstr("://")`, `lstat`, `S_ISREG`, size, `open_local_path(O_RDONLY|O_NONBLOCK)`, `fstat`, `same_file` | `radios_path_local_valid` → `INVALID_FILE`; `radios_file_identity_path` (failure → `IO` via `io_error` with mapped errno; kind≠REGULAR → `INVALID_FILE`; cloud placeholder → `IO`); size → `LIMIT`; `radios_open_nofollow` → `IO`; `radios_file_identity_fd` + same → `CHANGED`. Out-param becomes `RadioSFileIdentity *identity`. |
| 101-109 `unchanged(fd, path, identity)` | `fstat` + `lstat` | identity fd + path, both REGULAR. |
| 111-125 `hash_fd` | `pread` 64 KiB, EINTR | `radios_pread` |
| 127-138 `local_read` | `pread`, `AVERROR(errno)` | `radios_pread` (errno guaranteed) |
| 231-270 `container_complete` | `pread` | `radios_pread` |
| 279 | `g_canonicalize_filename(path, NULL)` → `result->path` | keep (native `\` path; consumers: src/ui/media_jobs.c:97,100 use it as a local path and `g_path_get_basename`). |
| 280, 283 | `identity.st_size` | `identity.size` |
| 303 | `avformat_open_input(&format, path, …)` custom IO | name hint only; unchanged. |
| 389 | `close(fd)` | `_close(fd)` |
| 435-460 `import_directory` | `root = canonicalize(project_root)`, `absolute = canonicalize(import_root, root)`; prefix check `g_str_has_prefix(absolute, root) && absolute[len] == '/'` (443); `relative = absolute + len + 1`; `open_local_path(root, O_RDONLY|O_DIRECTORY)` (447); `g_strsplit(relative, "/")` (449); per part `mkdirat(fd, part, 0700)` + `openat(…O_DIRECTORY|O_NOFOLLOW…)` (451-452) | `radios_path_has_dir_prefix(absolute, root, &rest)` (case-insensitive, `\`-aware; also handle `root` = `C:\` whose canonical form already ends with a separator) → else `CONFINEMENT`; `relative` = `absolute + rest` (native); `RadioSDir *dir = radios_dir_open_nofollow(root)` (failure → `IO` "Proje klasörü açılamadı"); split `relative` on `G_IS_DIR_SEPARATOR`; per part `radios_dir_open_child(dir, part, TRUE)` — `ELOOP`/not-a-directory → `CONFINEMENT`, other errors → `IO` "Import klasörü oluşturulamadı"; return the `RadioSDir*` of the import dir (and its native absolute path). Note: `config->import_root` comes from src/core/config.c `resolve_inside_root` (another area) — it must yield a plain `X:\…` path without `\\?\` prefix and with the same casing as `project_root`. |
| 462-525 `stage_file` | see rows below | |
| 467 | `open_source(source, &identity)` | as above |
| 474-479 | `uuid + lowercase extension` (allow-list), `path = g_build_filename(directory, name)` | unchanged (`g_build_filename` → `\`). |
| 480 | `openat(directory_fd, name, O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0600)` | `radios_dir_create_file(dir, name)` (CREATE_NEW, binary, non-inheritable, share READ|DELETE). |
| 485 | `read(source_fd, …)` sequential | **Replace with `radios_pread(source_fd, buf, n, total)`.** On Windows a CRT `read()` on a text-mode fd would corrupt data, and the emulated `pread` used elsewhere moves the shared file pointer — explicit offsets make the copy independent of both. |
| 489 | `write(destination_fd, …)` | `radios_write_all` |
| 496 | `unchanged(source_fd, source, &identity)` | identity API |
| 497 | `fchmod(destination_fd, 0600) || fsync(destination_fd) || fsync(directory_fd)` | `radios_fchmod_private` (no-op) `|| radios_fsync(dest) || radios_dir_sync(dir)` (no-op, decided). |
| 500-504 | `fstat(directory_fd)` vs `lstat(directory)` dev/ino + `S_ISDIR` | `radios_dir_identity(dir)` vs `radios_file_identity_path(directory)` (volume+id equal, kind DIRECTORY). With the held chain this cannot fail except through bugs; keep for parity. |
| 506 | `fstat(destination_fd, &copied)` | `radios_file_identity_fd` |
| 507 | `analyze_file(path, …)` while `destination_fd` is still open for writing | works only because the copy handle shares READ and the analyzer opens with share READ|WRITE|DELETE (§3.3). |
| 509-516 | size + hash cross-checks; `unchanged(destination_fd, path, &copied)` | identity API; the copy's LastWriteTime is stable because all writes happened before the snapshot (NTFS updates the FCB time on write; both queries read the FCB). |
| 517 | `result->relative_path = g_build_filename(relative, name)` | **Must be `/`-separated**: `gchar *rel = radios_path_to_portable(relative); result->relative_path = g_strconcat(rel, "/", name, NULL);` (test_media.c:181 expects prefix `"data/içe aktar/"`; persisted form is portable). |
| 520 | `unlinkat(directory_fd, name, 0); fsync(directory_fd);` — runs **before** `close(destination_fd)` (522) | Either reorder (close destination first, then `radios_dir_unlink`) or keep the order and delete through the open handle (`FileDispositionInfoEx` + POSIX semantics, requires the `DELETE` access requested in `radios_dir_create_file`). Recommended: reorder **and** use `radios_dir_unlink`; then drop the dir fsync. Test `cancellation_cleanup`/`malformed_truncated` require the import dir to be empty when the call returns. |
| 522-524 | `close` ×3 | `_close` for fds, `radios_dir_close(dir)` |

`long double` accumulation (29, 218, 365, 367): GCC on x64 Windows keeps 80-bit `long double` (unlike MSVC) — identical
results to Linux; no printing of `long double` (mingw printf would need `__mingw_printf`).

JSON output (`radios_media_analysis_to_json`, 412-431): json-glib serializes doubles with `g_ascii_dtostr`
(locale-independent) — Turkish decimal comma cannot leak in. Keep.

### 4.8 `src/media/loudness.c`, `src/media/rhythm.c` — pure DSP

* loudness.c: K-weighting biquads (`tan/pow/log10`), BS.1770 gating. No OS calls. libm ulp differences only.
* rhythm.c: `RadioSRhythmAnalyzer` embeds `energy[72000]` + `low[72000]` doubles (11) = 1.15 MB → heap via `g_new0` (18). Keep.
  `lround()` returns `long` = 32-bit on Windows (102, 106 store into `glong`); values are bin indices < 72 000 — safe.
  `llround` (252, 257-258, 278, 282) 64-bit — safe. `sqrtl`/`long double` (72-74) — 80-bit with GCC, same as Linux.
* Both serialize doubles through json-glib (locale-safe).

### 4.9 Headers in scope (contracts; no OS types)

| Header | Notes for Windows |
|---|---|
| `audio.h` | `radios_audio_engine_new(gboolean realtime, const gchar *pulse_server)` (115): keep signature; `pulse_server` = backend hint (see 4.6.6). Device-name fields 513 bytes (34-37, 50, 90) hold WASAPI endpoint ids. Comments mention "Pulse" (88) — update wording only. |
| `audio_internal.h` | Backend interface (16-36) implemented by WASAPI; ring API (7-14) portable (`stdatomic`). |
| `audio_output.h` | Contract table in 4.1. "existing files are never overwritten" (41-43) → `CREATE_NEW`. |
| `audio_decoder.h` | Contract in 4.2; add to the comment: "On Windows the open handle refuses writers (sharing) and allows delete/rename." |
| `audio_transition.h` | 24-25: caller deletes the private temp WAV — works because transition closes before returning. |
| `audio_tempo.h` | inline math only. |
| `audio_dsp.h` | `RADIOS_AUDIO_DECKS 66`, structs; pure. |
| `audio_runner.h` | `RadioSAudioRunItem` ≈ 1.46 KB → stack note (4.6.5). |
| `audio_patchbay.h`, `audio_channel_input.h` | Pulse-specific vocabulary in signatures/comments (`pulse_server` params, ".monitor" sources, virtual sinks); implemented in the backend/patchbay area. Keep signatures, reinterpret `pulse_server` as backend hint. |
| `audio_ui.h` | UI area. |
| `media.h` | 13 (2 GiB) → 64-bit sizes; 47-49 `relative_path` portable `/` form (decision). |
| `media_archive.h` | Implemented in src/core/media_archive.c (other area): its contract says "0600, atomic replacement, links rejected", "skipping hidden" files, index cache keyed by "size and modification time" — on Windows: hidden = leading dot **or** `FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM`; links = name-surrogate reparse points; mtime granularity per FS (FAT 2 s). |
| `media_geometry.h`, `media_jobs.h`, `media_routing.h`, `media_ui.h`, `media_archive_ui.h` | No OS dependencies. |
| `loudness.h`, `loudness_policy.h`, `rhythm.h` | Pure; loudness_policy timezone note (4.6.4). |

---------------------------------------------------------------------------------------------------------------------

## 5. Concrete code shape (reference for the implementer)

### 5.1 decoder.c open path (Windows)
```c
RadioSFileIdentity before;
if (radios_file_identity_path(decoder->path, &before) != 0 || before.kind != RADIOS_FILE_REGULAR)
    → INVALID "Ses kaynağı normal yerel dosya olmalı; sembolik bağlantı/aygıt/kuyruk kabul edilmez."
if (before.attributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_OFFLINE)) → IO (cloud message)
if (!before.size || before.size > MAX_FILE_BYTES) → LIMIT
decoder->fd = radios_open_nofollow(decoder->path, RADIOS_OPEN_READ | RADIOS_OPEN_SEQUENTIAL);
if (decoder->fd < 0) → IO (EBUSY → sharing message; else existing message)
if (radios_file_identity_fd(decoder->fd, &decoder->identity) != 0 || !radios_file_identity_same(&before, &decoder->identity))
    → CHANGED
```
`unchanged()`: `radios_file_identity_fd(fd,&a)==0 && radios_file_identity_path(path,&b)==0 && a.kind==REGULAR &&
b.kind==REGULAR && same(&identity,&a) && same(&identity,&b)`.

### 5.2 analyzer.c staging (Windows)
```c
RadioSDir *dir = import_directory(config, &directory, &relative, error);      /* held chain */
fd = radios_dir_create_file(dir, name);
for (total = 0; total < identity.size; ) { n = radios_pread(source_fd, buf, MIN(sizeof buf, identity.size-total), total); … radios_write_all(fd, buf, n); total += n; }
radios_fsync(fd); /* fchmod + dir sync are no-ops */
… analyze_file(path) …
cleanup: if (!result && created) { _close(fd); fd = -1; radios_dir_unlink(dir, name); }
```

### 5.3 engine.c realtime hooks
```c
/* radios_audio_engine_new, inside if (realtime) */
radios_timer_resolution_acquire(); radios_power_keep_awake(TRUE);
/* render_worker */
gpointer mmcss = radios_thread_audio_priority_begin("Pro Audio"); … loop … radios_thread_audio_priority_end(mmcss);
/* radios_audio_engine_unref, after joins, if (engine->realtime) */
radios_power_keep_awake(FALSE); radios_timer_resolution_release();
```

---------------------------------------------------------------------------------------------------------------------

## 6. How tests exercise these contracts (and what changes on Windows)

Registered in CMakeLists.txt:216-232 (`media`, `audio`, `audio_decoder`, `tempo`, `dsp`, `effect_return`, `transition`,
`runner`, `output`, `channel_input`), 169-189 (`rhythm`, `loudness`, `loudness_engine`), 238-243 (`audio_recovery`, Pulse),
538-542 and 778-790 (`audio_devices`, `runner_pool_preload_pcm` via tests/run_audio_ui.py with private Pulse sinks).
Realtime engine tests skip unless `RADIOS_TEST_MAIN_SINK` etc. are set (test_audio.c:207-208, 522-523, 601-602, 664-665)
— on Windows the harness must provide equivalent endpoints (WASAPI area; suggested "null" backend).

### 6.1 tests/test_audio_decoder.c
| Line | POSIX use | Windows adaptation |
|---|---|---|
| 1-8 | `_POSIX_C_SOURCE`, `<fcntl.h>`, `<sys/stat.h>`, `<unistd.h>` | include a `tests/platform_test.h` helper header |
| 15-26 `remove_tree` | `lstat` + `S_ISDIR` | use `radios_file_identity_path` (do not recurse into links/junctions; `g_rmdir` removes a junction itself) |
| 48 | `g_chmod(path, 0600)` | harmless (only toggles read-only); keep |
| 60 | Turkish file name "Ölçülen özel ses.wav" | good UTF-8 coverage, keep |
| 82-91 | `g_spawn_sync("ffmpeg", …)` with UTF-8 args | works (needs `ffmpeg.exe` on PATH and GLib spawn helpers); output paths use `/` — accepted by Windows |
| 203-204, 313-314 | `open(O_CREAT|O_WRONLY)` + `ftruncate(fd, 64 MiB / 2 GiB+1)` | `radios_test_truncate(path, size, sparse=TRUE)`: `CreateFileW` + `DeviceIoControl(FSCTL_SET_SPARSE)` + `SetFileInformationByHandle(FileEndOfFileInfo)`. A 32-bit `off_t` would turn `2 GiB+1` into 1 → wrong error; with `_FILE_OFFSET_BITS=64` mingw's `ftruncate64` works but allocates 2 GiB of disk. |
| 250-256 | `symlink(file)` → expect `INVALID`; `symlink(dir)` parent → expect `IO` | file: `CreateSymbolicLinkW(…, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)`; if `ERROR_PRIVILEGE_NOT_HELD` (no Developer Mode/admin) `g_test_skip` that half. Directory: create a **junction** (`FSCTL_SET_REPARSE_POINT`, `IO_REPARSE_TAG_MOUNT_POINT`, no privilege needed) → expect `IO`. |
| 257-259 | `mkfifo` → `INVALID` | replace with `NUL` / `<dir>\CON.wav` / `\\.\pipe\x` → expect `INVALID`; and a directory path → `INVALID`. Add an ADS case `wave + ":evil"` → `INVALID`. |
| 266-277 `changed_file` | `open(O_WRONLY)` + `pwrite` while decoder open → read/seek `CHANGED` | With the recommended sharing (§3.3) the writer open fails: assert `radios_test_open_write()` fails with `EBUSY`/sharing violation (integrity preserved). Add a replacement case: write a different file and `MoveFileExW(…, MOVEFILE_REPLACE_EXISTING)` over the decoded path (succeeds because the decoder shares DELETE; on NTFS use POSIX-semantics rename via `SetFileInformationByHandle(FileRenameInfoEx, FILE_RENAME_FLAG_POSIX_SEMANTICS|FILE_RENAME_FLAG_REPLACE_IF_EXISTS)` if classic `MoveFileEx` refuses an open target) → next read `CHANGED`; and a delete case (`DeleteFileW` succeeds, next read `CHANGED`). |
| 198-199 `cancel_soon` | `g_usleep(1000)` | `Sleep(1)` → 1-16 ms; still lands during 64 MiB hashing; keep |

### 6.2 tests/test_media.c
* 19-25 `lstat`/`S_ISDIR` tree removal → identity API.
* 56-57 `import_root = <project>/data/içe aktar` (non-ASCII + space) — good coverage.
* 181 `g_str_has_prefix(relative_path, "data/içe aktar/")` — passes with the `/` rule (4.7 row 517).
* 183-184 `lstat` + `st_mode & 0777 == 0600` → `#ifdef G_OS_WIN32` replace with "is REGULAR via identity API"; skip the
  mode check (no POSIX modes).
* 185 `g_build_filename(project, relative_path) == result->path` → on Windows compare after
  `radios_path_to_portable()` on both or compare identities.
* 210-213 `symlink` / `mkfifo` → same helpers/skips as 6.1.
* 214-217 `open(O_WRONLY|O_CREAT|O_EXCL)` + `ftruncate(2 GiB+1)` → `radios_test_truncate`.
* 224-235 confinement: `symlink(outside, project/link)` (directory) → junction helper (no privilege) keeps the test
  meaningful; `symlink(project, source-link)` → junction.
* 266-279 `cancellation_cleanup` polls the import dir with `g_dir_open` while staging — with the held chain the import
  directory cannot be deleted/renamed during staging (fine; the test only lists it).
* 281-297 `changed_source` mutator: `open(O_WRONLY)` + `pwrite(…, 100)` every 1 ms while `radios_media_analyze` runs →
  needs `radios_test_open_write` (share RWD) + `radios_test_pwrite`; the analyzer opens with share READ|WRITE|DELETE, so it
  still opens and the second hash pass yields `CHANGED`. Keep.

### 6.3 tests/test_output.c
* 5-11 `<arpa/inet.h> <poll.h> <sys/socket.h> <sys/time.h> <unistd.h>` and the whole fixture HTTP server (119-202):
  `socket/bind/getsockname/listen/accept/recv/send(MSG_NOSIGNAL)/shutdown(SHUT_WR)/close/poll`, `SO_RCVTIMEO` with
  `struct timeval`. Rewrite with GIO (`GSocketListener`/`GSocket` with `g_socket_set_timeout(10)`,
  `g_socket_receive`, `g_socket_send`, `g_socket_shutdown(…, FALSE, TRUE, …)`, `g_socket_close`) — portable and avoids
  Winsock `SOCKET` vs `int`, `WSAStartup`, `closesocket`, `SD_SEND`, `WSAPoll`, DWORD-millisecond `SO_RCVTIMEO` differences.
* 88-118 recording: expects 6444-byte WAV, `"created"` substring on the second start (EEXIST) — holds with
  `CREATE_NEW`. Teardown unlinks all files (52-58) — WAV handle is closed by then; decoders closed.
* 299-304 validation: `"relative.wav"` rejected (also by the strict absolute rule), `file:///tmp/output` rejected — fine.
* Streaming tests use `http://127.0.0.1:<port>`; curl on Windows fine. Timing loops `g_usleep(1000)` fine.

### 6.4 tests/test_transition.c
* 6, 52 `struct stat st; g_stat(path, &st); st.st_mode & 0777 == 0600; st.st_size == frames*8+44` → on Windows drop the
  mode assert and read the size via the identity API or `GFileInfo` (avoid `GStatBuf` ABI questions with
  `_FILE_OFFSET_BITS=64`).
* 63 deletes the preview after `radios_audio_decoder_close` — fine.

### 6.5 Other tests in the area
* test_tempo.c, test_dsp.c, test_rhythm.c, test_loudness.c, test_loudness_engine.c, test_runner*.c,
  test_automation_adjunct.c, test_song_seek.c: offline engine + numeric tolerances; no OS calls besides GLib. Expected to
  pass unchanged once decoder/analyzer are ported (they all load WAV fixtures through the decoder).
* tests/test_audio_soak.c:78 `RadioSAudioRunItem items[400]` on the stack (≈ 585 KB) and `radios_audio_engine_new(TRUE,
  RADIOS_TEST_PULSE_SERVER)` → needs the 8 MiB stack and the Windows audio test harness.
* tests/test_audio_recovery.c (pactl/parec/`kill`/`waitpid`/`sys/wait.h`) — Pulse-only; replaced by WASAPI-area tests.

---------------------------------------------------------------------------------------------------------------------

## 7. Test helper header to add (`tests/platform_test.h`, Windows)

```c
int    radios_test_symlink(const gchar *target, const gchar *link, gboolean directory); /* -1 + errno=EPERM when unprivileged */
int    radios_test_junction(const gchar *target_dir, const gchar *link_dir);            /* FSCTL_SET_REPARSE_POINT mount point */
int    radios_test_truncate(const gchar *path, gint64 size, gboolean sparse);           /* create-or-open, set EOF */
int    radios_test_open_write(const gchar *path);                                       /* GENERIC_WRITE, share R|W|D, binary fd */
gssize radios_test_pwrite(int fd, const void *data, gsize size, gint64 offset);         /* WriteFile + OVERLAPPED */
int    radios_test_replace(const gchar *source, const gchar *target);                   /* POSIX-semantics rename-over */
/* GIO loopback HTTP fixture used by test_output.c (accept loop + per-connection threads). */
```

---------------------------------------------------------------------------------------------------------------------

## 8. Risks / open questions

1. **Sharing policy decision** (§3.3): recommended deny-write for the decoder changes observable behaviour (tag editors get
   "file in use" while a track is loaded) and requires the `changed_file` test rewrite. Alternative keeps Linux parity but
   weakens in-place-modification detection (NTFS defers LastWriteTime to handle close).
2. **Held directory chain** blocks other processes from renaming/deleting any directory on the path for the duration of an
   open/import (milliseconds for decoder opens; whole staging duration for imports). If that collides with SDBASE
   moves, switch to the `NtCreateFile(RootDirectory)` variant.
3. **Junction rejection**: legacy profile junctions ("Documents and Settings", "My Music" under Documents) and folders
   mounted as volumes are reparse points → rejected like Linux symlinks (IO/CONFINEMENT). Operators using such paths get
   an error; message should say "Yol bir bağlantı (junction/sembolik bağlantı) içeriyor".
4. **Time zone wrapper** is a cross-area prerequisite: without it `radios_audio_engine_apply_preferences` always fails on
   Windows (core loudness_policy.c:31-35).
5. **FFmpeg 8.x atempo** runtime ratio change through `priv` (tempo.c:101) may behave differently than the Linux FFmpeg —
   verify test_tempo first.
6. **MP3 encoder availability**: `mp3_mf` fallback cannot encode FLTP → ship `libmp3lame-0.dll`, add a smoke test.
7. **Timer/EcoQoS**: without §4.6.3 a minimized RadioS on Windows 11 can have 15.6 ms wake-ups and EcoQoS throttling;
   render margins (40 ms ring + device buffer) shrink and dropouts become possible under load.
8. **`--wrap=g_get_real_time`** in tests does not intercept dllimport'ed GLib calls on Windows (test-infra area).
9. **`GStatBuf` ABI** with `_FILE_OFFSET_BITS=64` (mingw `#define stat _stat64`) vs the GLib DLL — avoid `g_stat/g_lstat`
   with `GStatBuf` in code compiled with that define, or verify sizes once with a 5 GiB sparse file.
10. **Cloud placeholders/Defender/Controlled Folder Access** (§3.6) are environment issues that surface as IO errors; UI
    texts should hint at the cause.
11. **libm differences**: no golden PCM hashes found in this area's tests; any future test must keep tolerances.
12. **GetFileInformationByName** exists only on Windows 11 24H2+ (resolve dynamically; the fallback path costs one
    `CreateFileW` per identity check).

## 9. Cross-area dependencies
* platform.h/platform.c owner: everything in §2 (identity, path policy, held-chain opens/`RadioSDir`, pread/write/fsync/
  lseek64/create-exclusive/mkstemp, errno mapping, MMCSS/timer/power helpers, `radios_time_zone_new_identifier`,
  `radios_curl_global_init`/TLS defaults, `radios_secure_zero`).
* Build/packaging: `-D_FILE_OFFSET_BITS=64`, `-Wl,--stack,8388608` for every exe, link `-lavrt -lwinmm`, manifest
  `longPathAware`, ship `libmp3lame-0.dll`, GLib spawn helpers for tests, `ffmpeg.exe` on test PATH.
* WASAPI backend: implement the internal backend contract in §4.6.6 (played_tag semantics, flush, configure_output,
  cheap reconnect, device ids ≤ 512 bytes, `@default` sentinel), MMCSS on its own threads, optional engine wake-up and a
  headless "null" backend for CI.
* Core (loudness_policy.c, weekly plans, UI time code): adopt `radios_time_zone_new_identifier`; remove the TZDIR file test.
* Core config: `project_root`/`import_root`/`cache_root` as plain canonical `X:\…` paths (no `\\?\`), consistent casing.
* Patchbay: validate recorder `prefix` with `radios_path_local_valid`; it calls `radios_audio_output_start_record`.
* Playout planner (src/playout/planner.c:1104, 1724): heap instead of 500-item stack arrays.
* SDBASE/Python: receives `source_path`/`source_root` as native Windows paths (src/ui/media_jobs.c:130-133) and the
  staged file name `import_path`; must move staged files with retry on sharing violation (Defender).
* Test infrastructure: GIO HTTP fixture, `tests/platform_test.h`, replacement for `--wrap` on dllimport'ed GLib symbols.
