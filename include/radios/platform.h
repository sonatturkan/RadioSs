#ifndef RADIOS_PLATFORM_H
#define RADIOS_PLATFORM_H

/* RadioS Windows platform layer (Windows 10 1903+ / Windows 11, x64).
 *
 * Every file that touches the file system, processes, the console or time
 * zones includes this header instead of <windows.h>, <unistd.h> or
 * <sys/...>.  Win32 types never appear here; they stay inside
 * src/core/platform*.c and the backend files.
 *
 * Error conventions
 *  - Functions returning gint/gssize/gint64 behave like POSIX calls: >= 0 on
 *    success, -1 on failure with errno set (Win32/NTSTATUS codes are mapped by
 *    radios_platform_errno_from_win32()).  They never take a GError.
 *  - Functions returning gboolean or a pointer and taking GError set
 *    G_IO_ERROR / g_io_error_from_errno(errno) with "<subject>: <g_strerror>"
 *    and also leave errno set.
 *  - Lock contention reports EWOULDBLOCK (140 on the UCRT, which differs from
 *    EAGAIN = 11): test errno == EWOULDBLOCK || errno == EAGAIN.
 *  - A no-follow open of a link fails with ELOOP (G_IO_ERROR_TOO_MANY_LINKS).
 *  - All strings are UTF-8; returned strings are g_free()d, vectors
 *    g_strfreev()d.  All functions are thread-safe; one descriptor is not
 *    used by two threads at the same time unless stated otherwise. */

#include <glib.h>
#include <gio/gio.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <unistd.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ init */

/* Idempotent and thread-safe; call it first in main() of RadioS.exe and of
 * every test/tool.  Every platform function also calls it lazily.
 * Hardening: SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) plus
 * the directory the GLib DLL was loaded from (never PATH, so foreign runtime
 * DLLs such as C:\MinGW\bin cannot be picked up by later LoadLibrary calls),
 * SetDllDirectoryW(L""), safe SearchPath mode,
 * SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX),
 * _set_fmode(_O_BINARY) and HeapEnableTerminationOnCorruption. */
void radios_platform_init(void);

/* Writes a minidump (dbghelp MiniDumpWriteDump, private ACL because dumps hold
 * process memory) to <log_root>\crash\RadioS-<UTC yyyymmddThhmmssZ>-<pid>.dmp
 * for unhandled SEH exceptions and abort(); keeps the newest 10 dumps.  The
 * dump is written by a dedicated thread, so stack overflows are covered too.
 * Call once from main() after the log root is known. */
gboolean radios_platform_install_crash_handler(const gchar *log_root, GError **error);

/* ------------------------------------------------------------------ errors */

/* Sets *error (when error != NULL) to G_IO_ERROR / g_io_error_from_errno(saved_errno)
 * with "<formatted prefix>: <g_strerror(saved_errno)>", restores errno and
 * returns FALSE. */
gboolean radios_platform_set_io_error(GError **error, gint saved_errno,
                                      const gchar *format, ...) G_GNUC_PRINTF(3, 4);
/* Win32 error code (DWORD) -> errno.  Exported for backend code. */
gint radios_platform_errno_from_win32(guint32 win32_error);

/* ------------------------------------------------------------------ open flags */

#define RADIOS_O_RDONLY     0x0000      /* == _O_RDONLY */
#define RADIOS_O_WRONLY     0x0001      /* == _O_WRONLY */
#define RADIOS_O_RDWR       0x0002      /* == _O_RDWR */
#define RADIOS_O_APPEND     0x0008      /* == _O_APPEND */
#define RADIOS_O_CLOEXEC    0x0080      /* == _O_NOINHERIT */
#define RADIOS_O_CREAT      0x0100      /* == _O_CREAT */
#define RADIOS_O_TRUNC      0x0200      /* == _O_TRUNC */
#define RADIOS_O_EXCL       0x0400      /* == _O_EXCL */
#define RADIOS_O_BINARY     0x8000      /* == _O_BINARY */
#define RADIOS_O_NONBLOCK   0           /* no FIFOs in the Windows file system */
#define RADIOS_O_NOFOLLOW   0x00100000  /* radios_fs_* only, never pass to g_open() */
#define RADIOS_O_DIRECTORY  0x00200000  /* radios_fs_* only, never pass to g_open() */
#define RADIOS_O_ACCMODE    (RADIOS_O_RDONLY | RADIOS_O_WRONLY | RADIOS_O_RDWR)
#define RADIOS_AT_FDCWD     (-100)
#define RADIOS_AT_REMOVEDIR 0x200
/* g_open()/g_mkstemp_full() may receive RDONLY/WRONLY/RDWR/APPEND/CLOEXEC/CREAT/
 * TRUNC/EXCL/BINARY only.  radios_fs_* functions accept every RADIOS_O_* flag and
 * always open binary, non-inheritable descriptors whose handles share
 * READ|WRITE|DELETE (so atomic replacement of an open file works). */

/* ------------------------------------------------------------------ file info */

typedef enum {
    RADIOS_FILE_TYPE_UNKNOWN = 0,
    RADIOS_FILE_TYPE_REGULAR,
    RADIOS_FILE_TYPE_DIRECTORY,
    RADIOS_FILE_TYPE_LINK,   /* name-surrogate reparse point (symlink, junction,
                                mount point), only when not followed */
    RADIOS_FILE_TYPE_OTHER   /* pipe, console, device */
} RadioSFileType;

typedef struct {
    RadioSFileType type;
    guint64 device;       /* 64-bit volume serial number (FILE_ID_INFO) */
    guint64 inode;        /* low 64 bits of the 128-bit file id */
    guint64 inode_high;   /* high 64 bits of the file id (ReFS), else 0 */
    gint64  size;         /* bytes; 0 for directories */
    gint64  mtime_ns;     /* LastWriteTime, ns since 1970-01-01 UTC */
    gint64  ctime_ns;     /* FILE_BASIC_INFO.ChangeTime, ns since 1970-01-01 UTC */
    guint32 nlink;        /* number of hard links */
    guint32 attributes;   /* FILE_ATTRIBUTE_* */
    guint32 reparse_tag;  /* reparse tag when FILE_ATTRIBUTE_REPARSE_POINT, else 0 */
} RadioSFileInfo;

/* device, inode, inode_high, size, mtime_ns and ctime_ns equal (same file,
 * content unchanged). */
gboolean radios_file_info_same(const RadioSFileInfo *a, const RadioSFileInfo *b);
/* name starts with '.', or FILE_ATTRIBUTE_HIDDEN/FILE_ATTRIBUTE_SYSTEM is set
 * ($RECYCLE.BIN, System Volume Information).  info may be NULL. */
gboolean radios_file_info_is_hidden(const gchar *name, const RadioSFileInfo *info);

/* ------------------------------------------------------------------ path-level fs */

/* stat/lstat.  follow_links=FALSE reports the link itself (dangling links succeed). */
gint     radios_fs_stat(const gchar *path, gboolean follow_links, RadioSFileInfo *info);
/* open(2).  RADIOS_O_NOFOLLOW applies to the last component (ELOOP on a link).
 * With RADIOS_O_CREAT, (mode & 077) == 0 creates a private object (owner =
 * current user, protected DACL: user/SYSTEM/Administrators); otherwise the
 * object inherits its parent's ACL.  Opening a directory needs RADIOS_O_RDONLY
 * (write access -> EISDIR); RADIOS_O_DIRECTORY on a non-directory -> ENOTDIR. */
gint     radios_fs_open(const gchar *path, gint flags, guint mode);
/* unlink(2) of a file or a link (never follows).  POSIX-semantics delete: the
 * name disappears even while others hold the file open with FILE_SHARE_DELETE;
 * sharing violations are retried for up to 2 s.  A directory -> EISDIR. */
gint     radios_fs_unlink(const gchar *path);
/* rename(2) with atomic replacement of an existing file/link (never a
 * directory); same retry/fallback rules as radios_fs_renameat(). */
gint     radios_fs_rename(const gchar *from, const gchar *to);
/* g_mkdir_with_parents(); components created by this call get the private
 * directory descriptor (inherited by children) when (mode & 077) == 0.
 * Existing components may be links (followed).  An existing non-directory -> ENOTDIR. */
gint     radios_fs_mkdir_with_parents(const gchar *path, guint mode);
/* realpath(3) via GetFinalPathNameByHandleW: absolute, links/junctions resolved,
 * 8.3 names expanded, on-disk case, upper-case drive letter, "\\?\" stripped,
 * subst drives resolved, mapped network drives returned as UNC, no trailing
 * separator except for roots.  Relative input is resolved against the CWD.
 * NULL + errno (ENOENT when a component is missing). */
gchar   *radios_fs_realpath(const gchar *path, GError **error);
/* access(dir, R_OK|X_OK[|W_OK]) for directories, ACL-accurate: opens and lists
 * the directory and, with need_write, creates and deletes a hidden probe file
 * (also detects read-only media and Controlled Folder Access).  0 or -1 + errno. */
gint     radios_fs_dir_check_access(const gchar *directory, gboolean need_write);
/* Free bytes available to this user (quota-aware) and total bytes of the volume
 * holding directory.  FALSE + errno when unknown (outputs untouched). */
gboolean radios_fs_disk_space(const gchar *directory, gint64 *free_bytes, gint64 *total_bytes);

/* ------------------------------------------------------------------ descriptor-relative fs */

/* Opens the root of an absolute path ("C:\", "\\server\share\") as a directory
 * fd and returns the remaining components (split on '\' and '/', empty
 * components removed; a "." or ".." component -> EINVAL).  -1 + errno. */
gint     radios_fs_open_root(const gchar *absolute_path, gchar ***components);
/* Opens absolute_path walking every component with no-follow semantics from the
 * root (directories with RADIOS_O_DIRECTORY|RADIOS_O_NOFOLLOW, the last one
 * with flags|RADIOS_O_NOFOLLOW).  A link anywhere in the path -> ELOOP. */
gint     radios_fs_open_nofollow_path(const gchar *absolute_path, gint flags, guint mode);
/* openat(2).  dirfd may be RADIOS_AT_FDCWD (name is then any path).  With a
 * directory fd, name must be one valid component (radios_path_component_valid). */
gint     radios_fs_openat(gint dirfd, const gchar *name, gint flags, guint mode);
/* fstatat(2); follow_links=FALSE == AT_SYMLINK_NOFOLLOW.  Works on files that
 * others hold open exclusively (attribute-only open). */
gint     radios_fs_statat(gint dirfd, const gchar *name, gboolean follow_links, RadioSFileInfo *info);
/* mkdirat(2); (mode & 077) == 0 -> private directory whose ACEs are inherited. */
gint     radios_fs_mkdirat(gint dirfd, const gchar *name, guint mode);
/* unlinkat(2); flags 0 (file or file link; a directory -> EISDIR) or
 * RADIOS_AT_REMOVEDIR (empty directory, or a directory link/junction itself). */
gint     radios_fs_unlinkat(gint dirfd, const gchar *name, gint flags);
/* renameat(2) with atomic replacement of an existing file/link at newname:
 * FileRenameInfoEx(REPLACE_IF_EXISTS|POSIX_SEMANTICS|IGNORE_READONLY_ATTRIBUTE)
 * relative to newdirfd (works while the target is open with FILE_SHARE_DELETE),
 * FileRenameInfo fallback on FAT/exFAT/SMB, bounded retry (2 s) while another
 * program holds the source or target without FILE_SHARE_DELETE (EBUSY/EACCES
 * after that).  An existing directory at newname -> EISDIR. */
gint     radios_fs_renameat(gint olddirfd, const gchar *oldname, gint newdirfd, const gchar *newname);

/* ------------------------------------------------------------------ descriptor ops */

gint       radios_fd_stat(gint fd, RadioSFileInfo *info);
/* Names in a directory fd, excluding "." and ".."; order unspecified.  Must not
 * run concurrently on the same dirfd.  Elements are gchar* (freed by the array). */
GPtrArray *radios_fd_list_names(gint dirfd, GError **error);
/* pread/pwrite at a 64-bit offset (the file position is restored afterwards). */
gssize     radios_fd_pread(gint fd, gpointer buffer, gsize count, gint64 offset);
gssize     radios_fd_pwrite(gint fd, gconstpointer buffer, gsize count, gint64 offset);
gint64     radios_fd_seek(gint fd, gint64 offset, gint whence);   /* lseek, 64-bit */
gint       radios_fd_truncate(gint fd, gint64 length);            /* ftruncate, 64-bit */
gint       radios_fd_sync(gint fd);      /* FlushFileBuffers; 0 for read-only fds, pipes, consoles */
gint       radios_fd_sync_dir(gint dirfd);  /* 0: NTFS journals directory metadata */
/* close(2) that first releases a radios_fd_lock() lock (best effort).  Use it
 * for every fd passed to radios_fd_lock(); plain close() is fine elsewhere. */
gint       radios_fd_close(gint fd);

/* flock(2) emulation with LockFileEx/UnlockFileEx on the byte range
 * [RADIOS_LOCK_REGION_OFFSET, +1) — identical in the Python tools.  Locks belong
 * to the handle: two fds of one process conflict like two processes.  Every
 * SH/EX request first drops the caller's own lock (re-locking is idempotent and
 * converts SH<->EX non-atomically, as Linux flock).  Contention with NB: -1/EWOULDBLOCK. */
#define RADIOS_LOCK_SH 1
#define RADIOS_LOCK_EX 2
#define RADIOS_LOCK_NB 4
#define RADIOS_LOCK_UN 8
#define RADIOS_LOCK_REGION_OFFSET G_GUINT64_CONSTANT(0x4000000000000000)
#define RADIOS_LOCK_REGION_LENGTH 1
gint       radios_fd_lock(gint fd, gint operation);

/* ------------------------------------------------------------------ privacy (replaces 0600/0700 + st_uid) */

typedef enum {
    RADIOS_PRIVATE_OWNER           = 1 << 0, /* owner is the current user (or Administrators
                                                while the user is an administrator) */
    RADIOS_PRIVATE_NO_OTHERS       = 1 << 1, /* DACL grants data/metadata/control access only to
                                                user, owner, SYSTEM, Administrators, OWNER RIGHTS */
    RADIOS_PRIVATE_EXACT_FILE_MODE = 1 << 2  /* regular file + NO_OTHERS ("0600") */
} RadioSPrivateCheck;
/* TRUE when every requested check holds.  FALSE with errno = EACCES when a check
 * fails, ENOTSUP when the volume has no persistent ACLs (FAT/exFAT: fail closed),
 * or the errno of a failed query.  The fd needs READ_CONTROL (all radios_fs_*
 * and g_open() descriptors have it). */
gboolean   radios_fd_is_private(gint fd, guint checks);
gboolean   radios_fs_path_is_private(const gchar *path, gboolean follow_links, guint checks);
/* fchmod(0600 file / 0700 dir): explicit owner = current user + protected
 * owner-only DACL.  No-op success when already private.  0 or -1 + errno. */
gint       radios_fd_make_private(gint fd);
gboolean   radios_fs_path_make_private(const gchar *path, GError **error);

/* ------------------------------------------------------------------ path strings (no I/O) */

/* "X:\..." / "X:/..." or "\\server\share[\...]"; FALSE for "\foo", "C:foo",
 * relative paths and "\\?\" / "\\.\" device paths. */
gboolean   radios_path_is_absolute(const gchar *path);
/* "X:\" or "X:/", "\\server\share" with or without a trailing separator. */
gboolean   radios_path_is_root(const gchar *path);
/* Equality after separator unification, ordinal case-insensitive like NTFS
 * (CompareStringOrdinal, never locale or g_utf8_casefold rules). */
gboolean   radios_path_equal(const gchar *a, const gchar *b);
/* path == root or below root at a component boundary (same comparison rules);
 * roots ending with a separator ("C:\") are handled. */
gboolean   radios_path_is_inside(const gchar *root, const gchar *path);
/* g_canonicalize_filename(path, NULL) with '\' separators, upper-case drive
 * letter, "\\?\" / "\\?\UNC\" prefixes removed, no trailing separator except
 * for roots.  Relative input is resolved against the CWD. */
gchar     *radios_path_normalize(const gchar *path);
/* One file/directory name valid on Windows: valid UTF-8, non-empty, not "." or
 * "..", no control characters, none of \ / : * ? " < > |, no trailing '.' or ' ',
 * not a reserved device name (CON PRN AUX NUL COM0-9 LPT0-9 COM¹²³ LPT¹²³, any
 * case, with or without extension), at most 255 UTF-16 units. */
gboolean   radios_path_component_valid(const gchar *name);
/* Splits a relative path on '\' and '/', dropping empty components. */
gchar    **radios_path_split(const gchar *relative);
/* Wire form for project-relative paths exchanged with SDBASE: '\' -> '/'. */
gchar     *radios_path_to_portable(const gchar *relative);
/* Inverse: '/' -> '\'. */
gchar     *radios_path_from_portable(const gchar *relative);

/* ------------------------------------------------------------------ process identity, argv, threads */

/* GetModuleFileNameW(NULL): absolute UTF-8 path of the running executable. */
gchar     *radios_process_executable_path(GError **error);
/* main() calls this before option parsing; the remembered argv is the exact
 * UTF-8 command line (g_win32_get_command_line()), argc/argv are only a fallback. */
void       radios_process_remember_argv(gint argc, gchar **argv);
/* Copy of the remembered argv, else the current UTF-8 command line. */
gchar    **radios_process_argv(void);
/* UTF-8 argv for g_option_context_parse_strv() (g_win32_get_command_line();
 * argc/argv only as fallback).  Includes argv[0]. */
gchar    **radios_platform_get_argv(gint argc, gchar **argv);
guint32    radios_process_id(void);
/* Lower the calling thread below the audio engine (THREAD_PRIORITY_BELOW_NORMAL). */
void       radios_thread_lower_priority(void);

/* ------------------------------------------------------------------ graceful termination */

/* Name of the manual-reset stop event of a RadioS process ("Local\RadioS-stop-<pid>");
 * the Python service and tools use the same name. */
gchar     *radios_process_stop_event_name(guint32 pid);
/* SIGTERM/SIGINT replacement: the callback runs on the default GMainContext when
 * (a) another process signals "Local\RadioS-stop-<our pid>" (radios_process_request_stop()),
 * or (b) the attached console delivers CTRL_C/CTRL_BREAK/CTRL_CLOSE/LOGOFF/SHUTDOWN.
 * Return G_SOURCE_CONTINUE/REMOVE as with g_unix_signal_add().  For console close/
 * logoff/shutdown the console handler keeps the process alive (<= 4.5 s, the OS
 * limit) until radios_process_remove_termination_watch() is called, so call that
 * at the END of the teardown.  Returns a source id (> 0). */
guint      radios_process_add_termination_watch(GSourceFunc callback, gpointer data,
                                                GDestroyNotify notify);
void       radios_process_remove_termination_watch(guint id);
/* SetEvent on "Local\RadioS-stop-<pid>".  FALSE + G_IO_ERROR_NOT_FOUND when that
 * process has no stop channel (not running or not a RadioS process). */
gboolean   radios_process_request_stop(guint32 pid, GError **error);

/* ------------------------------------------------------------------ child processes (CreateProcessW) */

/* Never use GSubprocess/g_spawn*() for helpers: GLib spawns through a console
 * helper (visible window) and inherits every inheritable handle (lock handles
 * would outlive the GUI in the child).  This API always uses
 * CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT and an explicit
 * PROC_THREAD_ATTRIBUTE_HANDLE_LIST holding only the child's std handles. */
typedef struct RadioSProcess RadioSProcess;

typedef enum {
    RADIOS_SPAWN_DEFAULT          = 0,
    /* Outlives us: new process group, hidden console of its own
     * (CREATE_NO_WINDOW, deliberately not DETACHED_PROCESS: a console-less child
     * would open a visible console for every console-program grandchild and gets
     * no CTRL_LOGOFF/SHUTDOWN events), CREATE_BREAKAWAY_FROM_JOB (retried without
     * it when the job forbids it), std handles NUL unless pipes are requested. */
    RADIOS_SPAWN_DETACHED         = 1 << 0,
    RADIOS_SPAWN_STDIN_PIPE       = 1 << 1,
    RADIOS_SPAWN_STDOUT_PIPE      = 1 << 2,
    RADIOS_SPAWN_STDERR_PIPE      = 1 << 3,
    RADIOS_SPAWN_STDERR_TO_STDOUT = 1 << 4,  /* child's stderr = its stdout */
    RADIOS_SPAWN_NULL_STDIO       = 1 << 5,  /* std handles that are not pipes -> NUL */
    /* Relaunch: the child inherits a SYNCHRONIZE handle to this process and gets
     * RADIOS_RELAUNCH_WAIT_HANDLE=<value> in its environment
     * (see radios_platform_relaunch_wait()). */
    RADIOS_SPAWN_PASS_SELF_HANDLE = 1 << 6
} RadioSSpawnFlags;

#define RADIOS_PROCESS_WAIT_INFINITE G_MAXUINT

/* argv[0] must be an absolute path to an .exe (never searched in PATH/CWD).
 * Arguments are quoted with the MSVCRT/CommandLineToArgvW rules (.bat/.cmd are
 * rejected: cmd.exe re-parses its command line).  working_directory NULL =
 * inherit.  envp NULL = inherit, else the complete "NAME=VALUE" environment
 * (duplicates are resolved case-insensitively, the last one wins).
 * Std streams without a pipe flag inherit our std handle (NUL when ours is
 * unusable, with RADIOS_SPAWN_NULL_STDIO or RADIOS_SPAWN_DETACHED).
 * For every *_PIPE flag the parent end is returned through the matching fd
 * pointer as a binary, non-inheritable CRT fd that the caller owns (wrap it with
 * g_win32_input_stream_new_from_fd() for async reads) — or, when that pointer
 * is NULL, it stays inside the RadioSProcess for radios_process_communicate(). */
RadioSProcess *radios_process_spawn(const gchar *const *argv, const gchar *working_directory,
                                    const gchar *const *envp, guint flags,
                                    gint *stdin_fd, gint *stdout_fd, gint *stderr_fd,
                                    GError **error);
guint32    radios_process_get_id(RadioSProcess *process);
/* TRUE when the process has exited within timeout_ms (0 = poll,
 * RADIOS_PROCESS_WAIT_INFINITE = forever); *exit_code receives its exit code. */
gboolean   radios_process_wait(RadioSProcess *process, guint timeout_ms, gint *exit_code);
typedef void (*RadioSProcessExitFunc)(RadioSProcess *process, gint exit_code, gpointer data);
/* One-shot exit notification dispatched on context (NULL = default context).
 * The watch keeps the process object alive.  Returns the GSource id on that
 * context (g_main_context_find_source_by_id() + g_source_destroy() cancels it). */
guint      radios_process_watch(RadioSProcess *process, GMainContext *context,
                                RadioSProcessExitFunc callback, gpointer data);
/* TerminateProcess(); TRUE also when the process had already exited. */
gboolean   radios_process_terminate(RadioSProcess *process, gint exit_code);
/* Drops the caller's reference.  Never terminates the child; owned pipes are
 * closed once no communicate/watch operation uses the object any more. */
void       radios_process_free(RadioSProcess *process);
/* Feeds stdin_bytes (may be NULL) into an owned stdin pipe and closes it, collects
 * owned stdout/stderr pipes until EOF and waits for the exit.  At most max_bytes
 * per stream are kept (the rest is drained and discarded, so pass limit + 1 to
 * detect overflow).  timeout_ms (RADIOS_PROCESS_WAIT_INFINITE = none) bounds the
 * whole operation: on expiry the process is terminated and G_IO_ERROR_TIMED_OUT is
 * returned.  Cancellation returns G_IO_ERROR_CANCELLED and leaves the process
 * running.  When the child exited but a grandchild still holds the pipes, the
 * output collected so far is returned after a 1 s grace period.  Outputs may be NULL. */
gboolean   radios_process_communicate(RadioSProcess *process, GBytes *stdin_bytes, gsize max_bytes,
                                      guint timeout_ms, GBytes **stdout_bytes, GBytes **stderr_bytes,
                                      gint *exit_code, GCancellable *cancellable, GError **error);
/* Same in a worker thread; callback runs in the caller's thread-default context. */
void       radios_process_communicate_async(RadioSProcess *process, GBytes *stdin_bytes, gsize max_bytes,
                                            guint timeout_ms, GCancellable *cancellable,
                                            GAsyncReadyCallback callback, gpointer user_data);
gboolean   radios_process_communicate_finish(RadioSProcess *process, GAsyncResult *result,
                                             GBytes **stdout_bytes, GBytes **stderr_bytes,
                                             gint *exit_code, GError **error);
/* The command line CreateProcessW receives for argv (MSVCRT quoting); also the
 * right text to show to an operator who must run a command by hand. */
gchar     *radios_process_quote_command_line(const gchar *const *argv);
/* Current environment with Windows (case-insensitive) name semantics applied:
 * every "NAME=VALUE" in overrides sets NAME, a bare "NAME" removes it.  For the
 * envp of radios_process_spawn(), e.g. { "PYTHONUTF8=1", "PYTHONIOENCODING=utf-8",
 * "PYTHONDONTWRITEBYTECODE=1", "PYTHONHOME", "PYTHONPATH", NULL }. */
gchar    **radios_process_environment(const gchar *const *overrides);
/* Thin wrapper: radios_process_spawn(argv, cwd, NULL, RADIOS_SPAWN_DETACHED, ...)
 * and free.  For a GUI relaunch use radios_process_spawn() with
 * RADIOS_SPAWN_DETACHED | RADIOS_SPAWN_PASS_SELF_HANDLE. */
gboolean   radios_process_spawn_detached(const gchar *const *argv, const gchar *working_directory,
                                         GError **error);

/* ------------------------------------------------------------------ startup of the GUI-subsystem executable */

/* When stdout/stderr are not usable (Explorer/Start menu launch or a console
 * parent), AttachConsole(ATTACH_PARENT_PROCESS) and reopen the unusable streams
 * on CONOUT$.  Never changes the console code page.  TRUE when attached. */
gboolean   radios_platform_attach_parent_console(void);
/* After the main window is shown: FreeConsole() if radios_platform_attach_parent_console()
 * attached one (unless RADIOS_KEEP_CONSOLE=1), so Ctrl+C or closing that terminal
 * cannot stop the on-air application.  stdout/stderr then go to the log sink in
 * log_directory (NULL = NUL). */
void       radios_platform_detach_console(const gchar *log_directory);
/* fd 0/1/2 is connected to a file, pipe or console. */
gboolean   radios_platform_stdio_is_usable(gint fd);
/* Redirects the unusable ones of stdout/stderr (CRT fds and Win32 std handles)
 * to <log_directory>\radios-desktop.log (append, shared with other instances;
 * renamed to radios-desktop.log.1 first when it exceeds 5 MiB). */
gboolean   radios_platform_redirect_stdio_to_log(const gchar *log_directory, GError **error);
/* Child side of RADIOS_SPAWN_PASS_SELF_HANDLE (first statement of main()):
 * consumes RADIOS_RELAUNCH_WAIT_HANDLE and waits up to timeout_ms for the old
 * instance (only when the handle really is a process running this executable).
 * TRUE when it waited for an exited parent, FALSE when nothing was inherited or on timeout. */
gboolean   radios_platform_relaunch_wait(guint timeout_ms);

/* ------------------------------------------------------------------ time zones */

/* GLib on Windows cannot load IANA identifiers.  Persisted identifiers stay IANA
 * (the SDBASE service uses Python zoneinfo + tzdata).  Never call
 * g_time_zone_new_identifier()/g_time_zone_get_identifier() directly.
 *
 * Bundled tree (tzdata 2026.5 TZif files): $TZDIR when non-empty (tests), else the
 * first existing of <exe dir>\..\share\zoneinfo (runtime\bin and installed
 * <prefix>\bin), <exe dir>\share\zoneinfo, <ancestor of exe dir>\runtime\share\zoneinfo
 * (build trees inside the project), <GLib DLL dir>\..\share\zoneinfo. */
const gchar *radios_time_zone_directory(void);
/* Strict IANA syntax ([A-Za-z0-9_+-] components separated by '/', no empty, "." or
 * ".." component, no leading '/', no '\' or ':', <= 64 bytes) and the zone exists in
 * the bundled tzdata tree (TZif v2+ regular file, not followed through links) —
 * the same database the service uses.  Only without a bundled tree an ICU-known id
 * is accepted.  "UTC" is always TRUE. */
gboolean     radios_time_zone_identifier_exists(const gchar *identifier);
/* Drop-in replacement for g_time_zone_new_identifier(): NULL for unknown or
 * non-IANA ids (Windows names, POSIX/offset strings), else a new reference; NULL
 * identifier = local zone, like GLib.  "UTC" -> g_time_zone_new_utc().  Other ids:
 * the Windows registry zone (historical DST, e.g. Europe/Istanbul 2015-01 +02:00)
 * mapped with the system ICU, accepted only if it agrees with the bundled tzdata
 * from one year back to five years ahead; otherwise the zone's TZif footer rule
 * (current rules exact, earlier history approximated).  Each identifier gets its
 * own GTimeZone object (cached for the process lifetime), so
 * radios_time_zone_get_identifier() returns exactly the id passed here.  Use
 * radios_date_time_format() for %Z: registry zones carry localised names. */
GTimeZone   *radios_time_zone_new(const gchar *identifier);
/* IANA identifier of tz: the id it was created from by radios_time_zone_new();
 * for other zones (g_time_zone_new_local(): "Turkey Standard Time") the IANA id
 * ICU maps the Windows zone to ("Europe/Istanbul"); else g_time_zone_get_identifier().
 * The string is interned (never freed). */
const gchar *radios_time_zone_get_identifier(GTimeZone *tz);
/* Sorted canonical IANA identifiers (CLDR canonical set, tzdata names), each
 * accepted by radios_time_zone_identifier_exists(). */
gchar      **radios_time_zone_list_identifiers(void);
/* g_date_time_format() whose %Z is the tzdata abbreviation ("+03", "CET", "EST")
 * like on Linux, instead of the localised Windows registry name. */
gchar       *radios_date_time_format(GDateTime *datetime, const gchar *format);

/* ------------------------------------------------------------------ test support */

/* symlink(2) for tests: CreateSymbolicLinkW (unprivileged in Developer Mode).
 * Directories fall back to a junction (absolute target) without the privilege;
 * file links without it: -1 and G_IO_ERROR_NOT_SUPPORTED (call g_test_skip()). */
gint       radios_platform_test_symlink(const gchar *target, const gchar *link_path,
                                        gboolean directory, GError **error);
/* chmod(path, 0644) for tests: grants BUILTIN\Users read access. */
gboolean   radios_platform_test_make_public(const gchar *path, GError **error);

G_END_DECLS

#endif
