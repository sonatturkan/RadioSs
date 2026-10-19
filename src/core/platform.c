/* RadioS platform layer: initialisation, errors, path strings, file system,
 * privacy (ACLs), advisory locks and test helpers (Windows 10 1903+).
 *
 * Descriptor model: every fd returned here is a binary, non-inheritable CRT fd
 * wrapping a synchronous HANDLE that shares READ|WRITE|DELETE.  Descriptor-
 * relative operations use NtCreateFile(RootDirectory = directory handle), so
 * openat/renameat/unlinkat never re-resolve a path (no TOCTOU through links). */

#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>
#include <winioctl.h>
#include <aclapi.h>
#include <sddl.h>

#include "radios/platform.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifndef FILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE
#define FILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE 0x00000040
#endif
#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
#endif
#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif
#ifndef FILE_OPEN_FOR_BACKUP_INTENT
#define FILE_OPEN_FOR_BACKUP_INTENT 0x00004000
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif
#ifndef OBJ_CASE_INSENSITIVE
#define OBJ_CASE_INSENSITIVE 0x00000040L
#endif
#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001
#define FILE_CREATE 0x00000002
#define FILE_OPEN_IF 0x00000003
#define FILE_OVERWRITE 0x00000004
#define FILE_OVERWRITE_IF 0x00000005
#endif
#ifndef STATUS_REPARSE_POINT_ENCOUNTERED
#define STATUS_REPARSE_POINT_ENCOUNTERED ((NTSTATUS)0xC000050BL)
#endif
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

#define RETRY_BUDGET_MS 2000

/* ------------------------------------------------------------------ init */

static struct {
    PSID user;                 /* token user SID (owned) */
    gboolean administrator;    /* effective member of BUILTIN\Administrators */
    PSECURITY_DESCRIPTOR file_sd, directory_sd;
    PACL file_dacl, directory_dacl;  /* point into the SDs */
    BYTE system_sid[SECURITY_MAX_SID_SIZE], admins_sid[SECURITY_MAX_SID_SIZE];
    BYTE owner_rights_sid[SECURITY_MAX_SID_SIZE], users_sid[SECURITY_MAX_SID_SIZE];
} security;

static gchar *module_dir_wide(HMODULE module, WCHAR **wide_out)
{
    DWORD capacity = MAX_PATH;
    for (guint attempt = 0; attempt < 6; ++attempt, capacity *= 4) {
        WCHAR *buffer = g_new(WCHAR, capacity);
        DWORD length = GetModuleFileNameW(module, buffer, capacity);
        if (length == 0) { g_free(buffer); return NULL; }
        if (length < capacity) {
            WCHAR *slash = wcsrchr(buffer, L'\\');
            if (slash) *slash = L'\0';
            gchar *utf8 = g_utf16_to_utf8((const gunichar2 *)buffer, -1, NULL, NULL, NULL);
            if (wide_out) *wide_out = buffer; else g_free(buffer);
            return utf8;
        }
        g_free(buffer);
    }
    return NULL;
}

static void init_security(void)
{
    HANDLE token = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, NULL, 0, &size);
        TOKEN_USER *user = size ? g_malloc(size) : NULL;
        if (user && GetTokenInformation(token, TokenUser, user, size, &size) && IsValidSid(user->User.Sid)) {
            DWORD length = GetLengthSid(user->User.Sid);
            security.user = g_malloc(length);
            if (!CopySid(length, security.user, user->User.Sid)) g_clear_pointer(&security.user, g_free);
        }
        g_free(user);
        CloseHandle(token);
    }
    DWORD size = SECURITY_MAX_SID_SIZE;
    CreateWellKnownSid(WinLocalSystemSid, NULL, security.system_sid, &size);
    size = SECURITY_MAX_SID_SIZE;
    CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, security.admins_sid, &size);
    size = SECURITY_MAX_SID_SIZE;
    CreateWellKnownSid(WinCreatorOwnerRightsSid, NULL, security.owner_rights_sid, &size);
    size = SECURITY_MAX_SID_SIZE;
    CreateWellKnownSid(WinBuiltinUsersSid, NULL, security.users_sid, &size);
    BOOL member = FALSE;
    if (CheckTokenMembership(NULL, security.admins_sid, &member)) security.administrator = member != FALSE;
    LPWSTR sid = NULL;
    if (security.user && ConvertSidToStringSidW(security.user, &sid)) {
        /* Explicit owner so elevated and non-elevated runs agree (an elevated
         * token would make BUILTIN\Administrators the owner).  Same SDDL in the
         * Python tools. */
        gchar *text = g_utf16_to_utf8((const gunichar2 *)sid, -1, NULL, NULL, NULL);
        gchar *file = g_strdup_printf("O:%sD:P(A;;FA;;;%s)(A;;FA;;;SY)(A;;FA;;;BA)", text, text);
        gchar *directory = g_strdup_printf("O:%sD:P(A;OICI;FA;;;%s)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", text, text);
        gunichar2 *file_w = g_utf8_to_utf16(file, -1, NULL, NULL, NULL);
        gunichar2 *directory_w = g_utf8_to_utf16(directory, -1, NULL, NULL, NULL);
        BOOL present = FALSE, defaulted = FALSE;
        if (file_w && ConvertStringSecurityDescriptorToSecurityDescriptorW((LPCWSTR)file_w, SDDL_REVISION_1, &security.file_sd, NULL))
            GetSecurityDescriptorDacl(security.file_sd, &present, &security.file_dacl, &defaulted);
        if (directory_w && ConvertStringSecurityDescriptorToSecurityDescriptorW((LPCWSTR)directory_w, SDDL_REVISION_1,
                                                                               &security.directory_sd, NULL))
            GetSecurityDescriptorDacl(security.directory_sd, &present, &security.directory_dacl, &defaulted);
        g_free(text); g_free(file); g_free(directory); g_free(file_w); g_free(directory_w);
        LocalFree(sid);
    }
}

static gpointer platform_init_once(gpointer unused)
{
    (void)unused;
    /* DLL search: application directory + System32 + the directory GLib was
     * loaded from (MSYS2 bin in development, the application dir when bundled).
     * PATH is never searched: C:\MinGW\bin on the machine PATH contains
     * incompatible runtime DLLs. */
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    WCHAR *exe_dir = NULL, *glib_dir = NULL;
    gchar *exe = module_dir_wide(NULL, &exe_dir);
    HMODULE glib = GetModuleHandleW(L"libglib-2.0-0.dll");
    gchar *glib_path = glib ? module_dir_wide(glib, &glib_dir) : NULL;
    if (glib_dir && (!exe_dir || CompareStringOrdinal(exe_dir, -1, glib_dir, -1, TRUE) != CSTR_EQUAL))
        AddDllDirectory(glib_dir);
    g_free(exe); g_free(glib_path); g_free(exe_dir); g_free(glib_dir);
    SetDllDirectoryW(L"");
    SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT);
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    /* Binary default (no CRLF/^Z translation), also for the std streams the CRT
     * opened in text mode before main(): output bytes are what the code wrote. */
    _set_fmode(_O_BINARY);
    for (gint fd = 0; fd <= 2; ++fd)
        if (_get_osfhandle(fd) >= 0) _setmode(fd, _O_BINARY);
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
    init_security();
    return NULL;
}

void radios_platform_init(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, platform_init_once, NULL);
}

/* ------------------------------------------------------------------ errors */

gint radios_platform_errno_from_win32(guint32 code)
{
    switch (code) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME: case ERROR_NOT_READY: case ERROR_DELETE_PENDING: case ERROR_BAD_UNIT:
    case ERROR_NO_MORE_FILES: case ERROR_DEV_NOT_EXIST:
        return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: case ERROR_CANNOT_MAKE: case ERROR_NETWORK_ACCESS_DENIED:
    case ERROR_INVALID_OWNER: case ERROR_ELEVATION_REQUIRED: case ERROR_VIRUS_INFECTED: case ERROR_VIRUS_DELETED:
        return EACCES;
    case ERROR_SHARING_VIOLATION: case ERROR_PIPE_BUSY: case ERROR_BUSY: case ERROR_USER_MAPPED_FILE:
        return EBUSY;
    case ERROR_LOCK_VIOLATION: case ERROR_IO_PENDING: return EWOULDBLOCK;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return EEXIST;
    case ERROR_DIRECTORY: return ENOTDIR;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_NOT_SAME_DEVICE: return EXDEV;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: case ERROR_DISK_QUOTA_EXCEEDED: return ENOSPC;
    case ERROR_WRITE_PROTECT: return EROFS;
    case ERROR_FILENAME_EXCED_RANGE: case ERROR_BUFFER_OVERFLOW: return ENAMETOOLONG;
    case ERROR_INVALID_NAME: case ERROR_INVALID_PARAMETER: case ERROR_BAD_PATHNAME: case ERROR_NEGATIVE_SEEK:
    case ERROR_INVALID_FLAGS: case ERROR_NO_UNICODE_TRANSLATION: case ERROR_BAD_ARGUMENTS:
        return EINVAL;
    case ERROR_NOT_SUPPORTED: case ERROR_INVALID_FUNCTION: case ERROR_CALL_NOT_IMPLEMENTED: return ENOTSUP;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: case ERROR_NO_SYSTEM_RESOURCES: return ENOMEM;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_CANT_ACCESS_FILE: case ERROR_CANT_RESOLVE_FILENAME: case ERROR_STOPPED_ON_SYMLINK: return ELOOP;
    case ERROR_INVALID_HANDLE: case ERROR_INVALID_TARGET_HANDLE: return EBADF;
    case ERROR_BROKEN_PIPE: case ERROR_NO_DATA: return EPIPE;
    case ERROR_OPERATION_ABORTED: return EINTR;
    case ERROR_TIMEOUT: case WAIT_TIMEOUT: return ETIMEDOUT;
    case ERROR_BAD_EXE_FORMAT: case ERROR_EXE_MACHINE_TYPE_MISMATCH: return ENOEXEC;
    default: return EIO;
    }
}

static gint errno_from_status(NTSTATUS status)
{
    switch (status) {
    case STATUS_DELETE_PENDING: return ENOENT;
    case STATUS_FILE_IS_A_DIRECTORY: return EISDIR;
    case STATUS_NOT_A_DIRECTORY: return ENOTDIR;
    case STATUS_REPARSE_POINT_ENCOUNTERED: case STATUS_STOPPED_ON_SYMLINK: return ELOOP;
    case STATUS_OBJECT_NAME_COLLISION: return EEXIST;
    case STATUS_DIRECTORY_NOT_EMPTY: return ENOTEMPTY;
    case STATUS_NO_MEDIA_IN_DEVICE: case STATUS_DEVICE_NOT_READY: return ENOENT;
    case STATUS_OBJECT_NAME_INVALID: case STATUS_OBJECT_PATH_SYNTAX_BAD: return EINVAL;
    default: return radios_platform_errno_from_win32(RtlNtStatusToDosError(status));
    }
}

static gint fail(gint code) { errno = code; return -1; }
static gint fail_win32(void) { return fail(radios_platform_errno_from_win32(GetLastError())); }

gboolean radios_platform_set_io_error(GError **error, gint saved_errno, const gchar *format, ...)
{
    if (error) {
        va_list args;
        va_start(args, format);
        gchar *prefix = g_strdup_vprintf(format, args);
        va_end(args);
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno), "%s: %s", prefix, g_strerror(saved_errno));
        g_free(prefix);
    }
    errno = saved_errno;
    return FALSE;
}

/* ------------------------------------------------------------------ UTF-16 helpers */

static WCHAR *utf16(const gchar *text)
{
    if (!text) { errno = EINVAL; return NULL; }
    gunichar2 *wide = g_utf8_to_utf16(text, -1, NULL, NULL, NULL);
    if (!wide) errno = EINVAL;
    return (WCHAR *)wide;
}

static gchar *utf8(const WCHAR *text, glong length)
{
    return g_utf16_to_utf8((const gunichar2 *)text, length, NULL, NULL, NULL);
}

static gboolean wide_separator(WCHAR c) { return c == L'\\' || c == L'/'; }

/* Fully qualified NT path ("\??\C:\x", "\??\UNC\srv\share\x") of a Win32 path,
 * resolved against the CWD like every Win32 API (GetFullPathNameW also handles
 * '/' separators, "." and ".." and long paths). */
static WCHAR *nt_path(const gchar *path)
{
    if (!path || !*path) { errno = path ? ENOENT : EINVAL; return NULL; }
    WCHAR *wide = utf16(path);
    if (!wide) return NULL;
    for (WCHAR *p = wide; *p; ++p) if (*p == L'/') *p = L'\\';
    WCHAR *full = NULL;
    gboolean device_input = wcsncmp(wide, L"\\\\.\\", 4) == 0;
    if (wcsncmp(wide, L"\\\\?\\", 4) == 0 || wcsncmp(wide, L"\\??\\", 4) == 0) {
        full = wide; wide = NULL;
    } else {
        DWORD capacity = GetFullPathNameW(wide, 0, NULL, NULL);
        if (capacity == 0) { gint code = radios_platform_errno_from_win32(GetLastError()); g_free(wide); errno = code; return NULL; }
        full = g_new(WCHAR, capacity + 1);
        DWORD length = GetFullPathNameW(wide, capacity + 1, full, NULL);
        if (length == 0 || length > capacity) { g_free(full); g_free(wide); errno = EINVAL; return NULL; }
        g_free(wide);
        /* "C:\x\CON", "x\NUL.txt": legacy DOS device names must never turn a
         * file path into a device. */
        if (!device_input && wcsncmp(full, L"\\\\.\\", 4) == 0) { g_free(full); errno = EINVAL; return NULL; }
    }
    gsize length = wcslen(full);
    WCHAR *result;
    if (wcsncmp(full, L"\\\\?\\", 4) == 0 || wcsncmp(full, L"\\??\\", 4) == 0 || wcsncmp(full, L"\\\\.\\", 4) == 0) {
        result = g_new(WCHAR, length + 1);
        memcpy(result, full, (length + 1) * sizeof(WCHAR));
        result[1] = L'?'; result[2] = L'?';
        result[0] = L'\\'; result[3] = L'\\';
    } else if (length >= 2 && full[0] == L'\\' && full[1] == L'\\') {
        result = g_new(WCHAR, length + 7);
        wcscpy(result, L"\\??\\UNC");
        wcscat(result, full + 1);
    } else {
        result = g_new(WCHAR, length + 5);
        wcscpy(result, L"\\??\\");
        wcscat(result, full);
    }
    g_free(full);
    return result;
}

/* Win32 path for APIs without an NT variant: '\' separators and the "\\?\" form
 * for long absolute paths. */
static WCHAR *win32_path(const gchar *path)
{
    WCHAR *nt = nt_path(path);
    if (!nt) return NULL;
    gsize length = wcslen(nt);
    if (length - 4 < 240) {
        /* short: plain DOS form ("C:\x" or "\\srv\share\x") */
        WCHAR *result;
        if (wcsncmp(nt, L"\\??\\UNC\\", 8) == 0) {
            result = g_new(WCHAR, length);
            wcscpy(result, L"\\");
            wcscat(result, nt + 7);
        } else {
            result = g_new(WCHAR, length - 3);
            wcscpy(result, nt + 4);
        }
        g_free(nt);
        return result;
    }
    nt[1] = L'\\';  /* "\??\" -> "\\?\" */
    return nt;
}

/* ------------------------------------------------------------------ handles and descriptors */

static HANDLE fd_handle(gint fd)
{
    /* _get_osfhandle() returns -1 for closed fds and -2 (_NO_CONSOLE_FILENO,
     * also the GetCurrentThread() pseudo handle!) for std fds of a GUI process
     * without console: both are EBADF. */
    intptr_t raw = fd >= 0 ? _get_osfhandle(fd) : -1;
    if (raw == -1 || raw == -2 || raw == 0) { errno = EBADF; return INVALID_HANDLE_VALUE; }
    return (HANDLE)raw;
}

static gint wrap_handle(HANDLE h, gint flags)
{
    gint crt = _O_BINARY | _O_NOINHERIT;
    if (flags & RADIOS_O_APPEND) crt |= _O_APPEND;
    if ((flags & RADIOS_O_ACCMODE) == RADIOS_O_RDONLY) crt |= _O_RDONLY;
    gint fd = _open_osfhandle((intptr_t)h, crt);
    if (fd < 0) { CloseHandle(h); return fail(EMFILE); }
    return fd;
}

static NTSTATUS nt_create(HANDLE root, const WCHAR *name, ACCESS_MASK access, ULONG attributes, ULONG share,
                          ULONG disposition, ULONG options, PSECURITY_DESCRIPTOR sd, HANDLE *out)
{
    gsize bytes = wcslen(name) * sizeof(WCHAR);
    if (bytes > 0xFFF0) return STATUS_NAME_TOO_LONG;
    UNICODE_STRING string = { (USHORT)bytes, (USHORT)bytes, (PWSTR)name };
    OBJECT_ATTRIBUTES object;
    InitializeObjectAttributes(&object, &string, OBJ_CASE_INSENSITIVE, root, sd);
    IO_STATUS_BLOCK io;
    memset(&io, 0, sizeof io);
    *out = NULL;
    NTSTATUS status = NtCreateFile(out, access, &object, &io, NULL, attributes, share, disposition,
                                   options | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(status)) *out = NULL;
    return status;
}

/* A component usable relative to a directory handle. */
static gboolean single_component(const gchar *name)
{
    return radios_path_component_valid(name);
}

/* Target of an fs operation: a directory handle + component, or a full NT path. */
typedef struct {
    HANDLE root;
    WCHAR *name;
} FsTarget;

static gboolean fs_target(gint dirfd, const gchar *name, FsTarget *target)
{
    target->root = NULL;
    target->name = NULL;
    if (!name) { errno = EINVAL; return FALSE; }
    if (dirfd == RADIOS_AT_FDCWD) {
        target->name = nt_path(name);
        return target->name != NULL;
    }
    HANDLE root = fd_handle(dirfd);
    if (root == INVALID_HANDLE_VALUE) return FALSE;
    if (!single_component(name)) { errno = EINVAL; return FALSE; }
    target->root = root;
    target->name = utf16(name);
    return target->name != NULL;
}

static void fs_target_clear(FsTarget *target) { g_clear_pointer(&target->name, g_free); }

static gint64 filetime_ns(LARGE_INTEGER value)
{
    return (value.QuadPart - G_GINT64_CONSTANT(116444736000000000)) * 100;
}

static gint64 filetime_ns_ft(FILETIME ft)
{
    LARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = (LONG)ft.dwHighDateTime;
    return filetime_ns(value);
}

static gint handle_info(HANDLE h, RadioSFileInfo *info)
{
    memset(info, 0, sizeof *info);
    DWORD type = GetFileType(h);
    if (type != FILE_TYPE_DISK) {
        if (type == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR) return fail_win32();
        info->type = RADIOS_FILE_TYPE_OTHER;
        return 0;
    }
    BY_HANDLE_FILE_INFORMATION by;
    if (!GetFileInformationByHandle(h, &by)) return fail_win32();
    info->attributes = by.dwFileAttributes;
    info->nlink = by.nNumberOfLinks;
    info->device = by.dwVolumeSerialNumber;
    info->inode = ((guint64)by.nFileIndexHigh << 32) | by.nFileIndexLow;
    info->mtime_ns = filetime_ns_ft(by.ftLastWriteTime);
    info->size = (by.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0
               : (gint64)(((guint64)by.nFileSizeHigh << 32) | by.nFileSizeLow);
    FILE_BASIC_INFO basic;
    if (GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic)) info->ctime_ns = filetime_ns(basic.ChangeTime);
    else info->ctime_ns = info->mtime_ns;
    if (by.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        FILE_ATTRIBUTE_TAG_INFO tag;
        if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag)) info->reparse_tag = tag.ReparseTag;
    }
    FILE_ID_INFO id;
    if (GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id)) {
        guint64 low = 0, high = 0;
        memcpy(&low, id.FileId.Identifier, 8);
        memcpy(&high, id.FileId.Identifier + 8, 8);
        info->device = id.VolumeSerialNumber;
        info->inode = low;
        info->inode_high = high;
    }
    if ((info->attributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(info->reparse_tag))
        info->type = RADIOS_FILE_TYPE_LINK;
    else if (info->attributes & FILE_ATTRIBUTE_DIRECTORY) info->type = RADIOS_FILE_TYPE_DIRECTORY;
    else info->type = RADIOS_FILE_TYPE_REGULAR;
    return 0;
}

gboolean radios_file_info_same(const RadioSFileInfo *a, const RadioSFileInfo *b)
{
    return a && b && a->device == b->device && a->inode == b->inode && a->inode_high == b->inode_high &&
           a->size == b->size && a->mtime_ns == b->mtime_ns && a->ctime_ns == b->ctime_ns;
}

gboolean radios_file_info_is_hidden(const gchar *name, const RadioSFileInfo *info)
{
    if (name && name[0] == '.') return TRUE;
    return info && (info->attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
}

static gboolean same_object(HANDLE a, HANDLE b)
{
    FILE_ID_INFO ia, ib;
    if (GetFileInformationByHandleEx(a, FileIdInfo, &ia, sizeof ia) && GetFileInformationByHandleEx(b, FileIdInfo, &ib, sizeof ib))
        return ia.VolumeSerialNumber == ib.VolumeSerialNumber && memcmp(&ia.FileId, &ib.FileId, sizeof ia.FileId) == 0;
    BY_HANDLE_FILE_INFORMATION ba, bb;
    return GetFileInformationByHandle(a, &ba) && GetFileInformationByHandle(b, &bb) &&
           ba.dwVolumeSerialNumber == bb.dwVolumeSerialNumber && ba.nFileIndexHigh == bb.nFileIndexHigh &&
           ba.nFileIndexLow == bb.nFileIndexLow;
}

static gboolean status_retryable(NTSTATUS status)
{
    return status == STATUS_SHARING_VIOLATION || status == STATUS_LOCK_NOT_GRANTED || status == STATUS_FILE_LOCK_CONFLICT;
}

static void retry_sleep(guint *attempt, gint64 started_us)
{
    static const guint steps[] = { 5, 10, 20, 40, 80, 160 };
    guint ms = *attempt < G_N_ELEMENTS(steps) ? steps[*attempt] : 250;
    gint64 elapsed = (g_get_monotonic_time() - started_us) / 1000;
    if (elapsed + ms > RETRY_BUDGET_MS) ms = (guint)MAX(1, RETRY_BUDGET_MS - elapsed);
    ++*attempt;
    Sleep(ms);
}

static gboolean retry_allowed(gint64 started_us)
{
    return (g_get_monotonic_time() - started_us) / 1000 < RETRY_BUDGET_MS;
}

/* ------------------------------------------------------------------ open */

typedef struct {
    ACCESS_MASK access;
    ULONG disposition, options;
    gboolean directory, nofollow, private_object;
} OpenSpec;

static gboolean open_spec(gint flags, guint mode, OpenSpec *spec)
{
    memset(spec, 0, sizeof *spec);
    gint accmode = flags & RADIOS_O_ACCMODE;
    if (accmode == (RADIOS_O_WRONLY | RADIOS_O_RDWR)) { errno = EINVAL; return FALSE; }
    gboolean write = accmode != RADIOS_O_RDONLY;
    spec->directory = (flags & RADIOS_O_DIRECTORY) != 0;
    spec->nofollow = (flags & RADIOS_O_NOFOLLOW) != 0;
    if (spec->directory && (write || (flags & (RADIOS_O_CREAT | RADIOS_O_TRUNC)))) {
        errno = write ? EISDIR : EINVAL;
        return FALSE;
    }
    ACCESS_MASK access = SYNCHRONIZE | FILE_READ_ATTRIBUTES | READ_CONTROL;
    if (spec->directory) access |= FILE_LIST_DIRECTORY | FILE_TRAVERSE;
    else {
        if (accmode == RADIOS_O_RDONLY || accmode == RADIOS_O_RDWR) access |= FILE_GENERIC_READ;
        if (write) {
            access |= FILE_GENERIC_WRITE;
            /* Append-only access makes every write atomic at the end of file
             * (several processes appending to one log); truncation needs WRITE_DATA. */
            if ((flags & RADIOS_O_APPEND) && !(flags & RADIOS_O_TRUNC)) access &= ~(ACCESS_MASK)FILE_WRITE_DATA;
        }
    }
    spec->access = access;
    if ((flags & RADIOS_O_CREAT) && (flags & RADIOS_O_EXCL)) spec->disposition = FILE_CREATE;
    else if ((flags & RADIOS_O_CREAT) && (flags & RADIOS_O_TRUNC)) spec->disposition = FILE_OVERWRITE_IF;
    else if (flags & RADIOS_O_CREAT) spec->disposition = FILE_OPEN_IF;
    else if (flags & RADIOS_O_TRUNC) spec->disposition = FILE_OVERWRITE;
    else spec->disposition = FILE_OPEN;
    ULONG options = FILE_OPEN_FOR_BACKUP_INTENT;
    if (spec->directory) options |= FILE_DIRECTORY_FILE;
    else if (write) options |= FILE_NON_DIRECTORY_FILE;
    if (spec->nofollow) options |= FILE_OPEN_REPARSE_POINT;
    spec->options = options;
    spec->private_object = (flags & RADIOS_O_CREAT) && (mode & 077) == 0;
    if (spec->private_object && !security.file_sd) { errno = EACCES; return FALSE; }
    return TRUE;
}

/* Opens a target; for no-follow opens rejects name-surrogate reparse points
 * (ELOOP) and re-opens other reparse points (OneDrive placeholders, dedup, WOF)
 * normally, insisting that both opens see the same file. */
static HANDLE open_target(const FsTarget *target, const OpenSpec *spec)
{
    HANDLE h = NULL;
    PSECURITY_DESCRIPTOR sd = spec->private_object ? security.file_sd : NULL;
    /* A no-follow O_TRUNC must not truncate a reparse point itself (OneDrive
     * placeholder data): open without truncating, check, then truncate. */
    gboolean truncate_after = spec->nofollow && (spec->disposition == FILE_OVERWRITE || spec->disposition == FILE_OVERWRITE_IF);
    ULONG disposition = !truncate_after ? spec->disposition : spec->disposition == FILE_OVERWRITE ? FILE_OPEN : FILE_OPEN_IF;
    gint64 started = g_get_monotonic_time();
    guint attempt = 0;
    NTSTATUS status;
    for (;;) {
        status = nt_create(target->root, target->name, spec->access, FILE_ATTRIBUTE_NORMAL,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition, spec->options, sd, &h);
        /* A name another program deleted the classic way stays "delete pending"
         * until its last handle closes: creating it again succeeds shortly. */
        gboolean pending_create = status == STATUS_DELETE_PENDING && disposition != FILE_OPEN && disposition != FILE_OVERWRITE;
        if ((!status_retryable(status) && !pending_create) || !retry_allowed(started)) break;
        retry_sleep(&attempt, started);
    }
    if (!NT_SUCCESS(status)) {
        gint code = errno_from_status(status);
        if (spec->nofollow && (code == ENOTDIR || code == EISDIR)) {
            /* A link where a directory/file was demanded: report the link. */
            HANDLE probe = NULL;
            if (NT_SUCCESS(nt_create(target->root, target->name, FILE_READ_ATTRIBUTES | SYNCHRONIZE, 0,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                     FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT, NULL, &probe))) {
                FILE_ATTRIBUTE_TAG_INFO tag;
                if (GetFileInformationByHandleEx(probe, FileAttributeTagInfo, &tag, sizeof tag) &&
                    (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag.ReparseTag))
                    code = ELOOP;
                CloseHandle(probe);
            }
        }
        errno = code;
        return NULL;
    }
    if (spec->nofollow) {
        FILE_ATTRIBUTE_TAG_INFO tag;
        if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag)) {
            gint code = radios_platform_errno_from_win32(GetLastError());
            CloseHandle(h); errno = code; return NULL;
        }
        if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (IsReparseTagNameSurrogate(tag.ReparseTag)) { CloseHandle(h); errno = ELOOP; return NULL; }
            HANDLE again = NULL;
            NTSTATUS second = nt_create(target->root, target->name, spec->access, FILE_ATTRIBUTE_NORMAL,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                        spec->options & ~(ULONG)FILE_OPEN_REPARSE_POINT, NULL, &again);
            if (!NT_SUCCESS(second)) { CloseHandle(h); errno = errno_from_status(second); return NULL; }
            gboolean same = same_object(h, again);
            CloseHandle(h);
            if (!same) { CloseHandle(again); errno = ELOOP; return NULL; }
            h = again;
        }
    }
    if (truncate_after) {
        FILE_END_OF_FILE_INFO end;
        end.EndOfFile.QuadPart = 0;
        if (!SetFileInformationByHandle(h, FileEndOfFileInfo, &end, sizeof end)) {
            gint code = radios_platform_errno_from_win32(GetLastError());
            CloseHandle(h); errno = code; return NULL;
        }
    }
    return h;
}

static gint open_fd(gint dirfd, const gchar *name, gint flags, guint mode)
{
    radios_platform_init();
    OpenSpec spec;
    if (!open_spec(flags, mode, &spec)) return -1;
    FsTarget target;
    if (!fs_target(dirfd, name, &target)) return -1;
    HANDLE h = open_target(&target, &spec);
    fs_target_clear(&target);
    if (!h) return -1;
    return wrap_handle(h, flags);
}

gint radios_fs_open(const gchar *path, gint flags, guint mode)
{
    return open_fd(RADIOS_AT_FDCWD, path, flags, mode);
}

gint radios_fs_openat(gint dirfd, const gchar *name, gint flags, guint mode)
{
    return open_fd(dirfd, name, flags, mode);
}

static gint stat_target(gint dirfd, const gchar *name, gboolean follow_links, RadioSFileInfo *info)
{
    radios_platform_init();
    if (!info) return fail(EINVAL);
    FsTarget target;
    if (!fs_target(dirfd, name, &target)) return -1;
    HANDLE h = NULL;
    NTSTATUS status = nt_create(target.root, target.name, FILE_READ_ATTRIBUTES | SYNCHRONIZE, 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                FILE_OPEN_FOR_BACKUP_INTENT | (follow_links ? 0 : FILE_OPEN_REPARSE_POINT), NULL, &h);
    fs_target_clear(&target);
    if (!NT_SUCCESS(status)) return fail(errno_from_status(status));
    gint result = handle_info(h, info);
    gint code = errno;
    CloseHandle(h);
    errno = code;
    return result;
}

gint radios_fs_stat(const gchar *path, gboolean follow_links, RadioSFileInfo *info)
{
    return stat_target(RADIOS_AT_FDCWD, path, follow_links, info);
}

gint radios_fs_statat(gint dirfd, const gchar *name, gboolean follow_links, RadioSFileInfo *info)
{
    return stat_target(dirfd, name, follow_links, info);
}

gint radios_fd_stat(gint fd, RadioSFileInfo *info)
{
    if (!info) return fail(EINVAL);
    HANDLE h = fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    return handle_info(h, info);
}

/* ------------------------------------------------------------------ root / no-follow walk */

/* Length of the root prefix of an absolute path ("C:\" = 3, "\\srv\share\" ...),
 * 0 when the path is not absolute. */
static gsize root_length(const gchar *path)
{
    if (!path) return 0;
    if (g_ascii_isalpha(path[0]) && path[1] == ':' && G_IS_DIR_SEPARATOR(path[2])) return 3;
    if (G_IS_DIR_SEPARATOR(path[0]) && G_IS_DIR_SEPARATOR(path[1])) {
        const gchar *server = path + 2, *p = server;
        while (*p && !G_IS_DIR_SEPARATOR(*p)) ++p;
        if (p == server || !*p) return 0;
        if ((p - server == 1 && (server[0] == '?' || server[0] == '.'))) return 0;
        const gchar *share = p + 1, *q = share;
        while (*q && !G_IS_DIR_SEPARATOR(*q)) ++q;
        if (q == share) return 0;
        return (gsize)(q - path) + (*q ? 1 : 0);
    }
    return 0;
}

gint radios_fs_open_root(const gchar *absolute_path, gchar ***components)
{
    radios_platform_init();
    if (components) *components = NULL;
    gsize length = root_length(absolute_path);
    if (!length) return fail(EINVAL);
    gchar *root = g_strndup(absolute_path, length);
    if (!G_IS_DIR_SEPARATOR(root[length - 1])) { gchar *with = g_strconcat(root, "\\", NULL); g_free(root); root = with; }
    gchar **parts = radios_path_split(absolute_path + length);
    for (guint i = 0; parts[i]; ++i)
        if (g_str_equal(parts[i], ".") || g_str_equal(parts[i], "..")) { g_strfreev(parts); g_free(root); return fail(EINVAL); }
    gint fd = radios_fs_open(root, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY, 0);
    g_free(root);
    if (fd < 0) { g_strfreev(parts); return -1; }
    if (components) *components = parts; else g_strfreev(parts);
    return fd;
}

gint radios_fs_open_nofollow_path(const gchar *absolute_path, gint flags, guint mode)
{
    gchar **parts = NULL;
    gint directory = radios_fs_open_root(absolute_path, &parts);
    if (directory < 0) return -1;
    guint count = g_strv_length(parts);
    if (count == 0) {
        g_strfreev(parts);
        if ((flags & RADIOS_O_ACCMODE) != RADIOS_O_RDONLY) { close(directory); return fail(EISDIR); }
        return directory;
    }
    for (guint i = 0; i + 1 < count; ++i) {
        gint next = radios_fs_openat(directory, parts[i], RADIOS_O_RDONLY | RADIOS_O_DIRECTORY | RADIOS_O_NOFOLLOW, 0);
        gint code = errno;
        close(directory);
        if (next < 0) { g_strfreev(parts); errno = code; return -1; }
        directory = next;
    }
    gint fd = radios_fs_openat(directory, parts[count - 1], flags | RADIOS_O_NOFOLLOW, mode);
    gint code = errno;
    close(directory);
    g_strfreev(parts);
    errno = code;
    return fd;
}

/* ------------------------------------------------------------------ create / delete / rename */

gint radios_fs_mkdirat(gint dirfd, const gchar *name, guint mode)
{
    radios_platform_init();
    gboolean private_dir = (mode & 077) == 0;
    if (private_dir && !security.directory_sd) return fail(EACCES);
    FsTarget target;
    if (!fs_target(dirfd, name, &target)) return -1;
    HANDLE h = NULL;
    NTSTATUS status = nt_create(target.root, target.name, FILE_LIST_DIRECTORY | SYNCHRONIZE, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_CREATE,
                                FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT,
                                private_dir ? security.directory_sd : NULL, &h);
    fs_target_clear(&target);
    if (!NT_SUCCESS(status)) return fail(errno_from_status(status));
    CloseHandle(h);
    return 0;
}

static gboolean fallback_info_class(DWORD code)
{
    return code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION;
}

static DWORD delete_handle(HANDLE h)
{
    FILE_DISPOSITION_INFO_EX ex = { FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                                    FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE };
    if (SetFileInformationByHandle(h, FileDispositionInfoEx, &ex, sizeof ex)) return ERROR_SUCCESS;
    DWORD code = GetLastError();
    if (!fallback_info_class(code)) return code;
    FILE_DISPOSITION_INFO classic = { TRUE };   /* FAT/exFAT/SMB: classic delete-on-close */
    return SetFileInformationByHandle(h, FileDispositionInfo, &classic, sizeof classic) ? ERROR_SUCCESS : GetLastError();
}

gint radios_fs_unlinkat(gint dirfd, const gchar *name, gint flags)
{
    radios_platform_init();
    if (flags & ~RADIOS_AT_REMOVEDIR) return fail(EINVAL);
    FsTarget target;
    if (!fs_target(dirfd, name, &target)) return -1;
    gint64 started = g_get_monotonic_time();
    guint attempt = 0;
    gint result = -1, code = EIO;
    for (;;) {
        HANDLE h = NULL;
        NTSTATUS status = nt_create(target.root, target.name, DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES, 0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                    FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT, NULL, &h);
        if (!NT_SUCCESS(status)) {
            if (status_retryable(status) && retry_allowed(started)) { retry_sleep(&attempt, started); continue; }
            code = errno_from_status(status);
            break;
        }
        FILE_ATTRIBUTE_TAG_INFO tag;
        if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag)) {
            code = radios_platform_errno_from_win32(GetLastError()); CloseHandle(h); break;
        }
        gboolean link = (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag.ReparseTag);
        gboolean directory = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!(flags & RADIOS_AT_REMOVEDIR) && directory && !link) { CloseHandle(h); code = EISDIR; break; }
        if ((flags & RADIOS_AT_REMOVEDIR) && !directory) { CloseHandle(h); code = ENOTDIR; break; }
        DWORD error = delete_handle(h);
        CloseHandle(h);
        if (error == ERROR_SUCCESS) { result = 0; break; }
        if ((error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) && retry_allowed(started)) {
            retry_sleep(&attempt, started);
            continue;
        }
        code = radios_platform_errno_from_win32(error);
        break;
    }
    fs_target_clear(&target);
    if (result != 0) errno = code;
    return result;
}

gint radios_fs_unlink(const gchar *path)
{
    return radios_fs_unlinkat(RADIOS_AT_FDCWD, path, 0);
}

/* NtSetInformationFile classes (kernel32's SetFileInformationByHandle rejects a
 * RootDirectory for renames with ERROR_INVALID_PARAMETER, so the native call is
 * used: it gives real renameat semantics relative to the directory handle). */
#define NT_FILE_RENAME_INFORMATION 10
#define NT_FILE_RENAME_INFORMATION_EX 65

static NTSTATUS nt_rename(HANDLE h, HANDLE root, const WCHAR *name, ULONG flags, gboolean extended)
{
    /* FILE_RENAME_INFORMATION(_EX) has the layout of the Win32 FILE_RENAME_INFO:
     * BOOLEAN ReplaceIfExists / ULONG Flags, HANDLE RootDirectory, ULONG length, name. */
    gsize bytes = wcslen(name) * sizeof(WCHAR);
    gsize size = G_STRUCT_OFFSET(FILE_RENAME_INFO, FileName) + bytes + sizeof(WCHAR);
    FILE_RENAME_INFO *info = g_malloc0(size);
    if (extended) info->Flags = flags;
    else info->ReplaceIfExists = TRUE;
    info->RootDirectory = root;
    info->FileNameLength = (DWORD)bytes;
    memcpy(info->FileName, name, bytes);
    IO_STATUS_BLOCK io;
    memset(&io, 0, sizeof io);
    NTSTATUS status = NtSetInformationFile(h, &io, info, (ULONG)size,
                                           (FILE_INFORMATION_CLASS)(extended ? NT_FILE_RENAME_INFORMATION_EX
                                                                             : NT_FILE_RENAME_INFORMATION));
    g_free(info);
    return status;
}

static gboolean status_unsupported(NTSTATUS status)
{
    return status == STATUS_INVALID_PARAMETER || status == STATUS_NOT_SUPPORTED || status == STATUS_INVALID_INFO_CLASS ||
           status == STATUS_INVALID_DEVICE_REQUEST || status == STATUS_NOT_IMPLEMENTED;
}

static DWORD rename_handle(HANDLE h, HANDLE root, const WCHAR *name)
{
    static const ULONG attempts[] = {
        FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS | FILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE,
        FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS,
    };
    for (guint i = 0; i < G_N_ELEMENTS(attempts); ++i) {
        NTSTATUS status = nt_rename(h, root, name, attempts[i], TRUE);
        if (NT_SUCCESS(status)) return ERROR_SUCCESS;
        if (!status_unsupported(status)) return RtlNtStatusToDosError(status);
    }
    /* FAT/exFAT/SMB servers without POSIX semantics: classic replace. */
    NTSTATUS status = nt_rename(h, root, name, 0, FALSE);
    return NT_SUCCESS(status) ? ERROR_SUCCESS : RtlNtStatusToDosError(status);
}

gint radios_fs_renameat(gint olddirfd, const gchar *oldname, gint newdirfd, const gchar *newname)
{
    radios_platform_init();
    FsTarget from, to;
    if (!fs_target(olddirfd, oldname, &from)) return -1;
    if (!fs_target(newdirfd, newname, &to)) { gint code = errno; fs_target_clear(&from); errno = code; return -1; }
    gint64 started = g_get_monotonic_time();
    guint attempt = 0;
    gint result = -1, code = EIO;
    for (;;) {
        HANDLE h = NULL;
        NTSTATUS status = nt_create(from.root, from.name, DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES, 0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                    FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT, NULL, &h);
        if (!NT_SUCCESS(status)) {
            if (status_retryable(status) && retry_allowed(started)) { retry_sleep(&attempt, started); continue; }
            code = errno_from_status(status);
            break;
        }
        DWORD error = rename_handle(h, to.root, to.name);
        CloseHandle(h);
        if (error == ERROR_SUCCESS) { result = 0; break; }
        gboolean retry = error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
        if (error == ERROR_ACCESS_DENIED || error == ERROR_ALREADY_EXISTS) {
            /* Classify the destination once: a directory is never replaced; a
             * regular file is usually held by a reader without FILE_SHARE_DELETE. */
            HANDLE dest = NULL;
            NTSTATUS probe = nt_create(to.root, to.name, FILE_READ_ATTRIBUTES | SYNCHRONIZE, 0,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                       FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT, NULL, &dest);
            if (NT_SUCCESS(probe)) {
                FILE_ATTRIBUTE_TAG_INFO tag;
                gboolean directory = GetFileInformationByHandleEx(dest, FileAttributeTagInfo, &tag, sizeof tag) &&
                                     (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                                     !((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag.ReparseTag));
                CloseHandle(dest);
                if (directory) { code = EISDIR; break; }
                retry = TRUE;
            } else if (probe == STATUS_DELETE_PENDING || status_retryable(probe)) retry = TRUE;
        }
        if (retry && retry_allowed(started)) { retry_sleep(&attempt, started); continue; }
        code = radios_platform_errno_from_win32(error);
        break;
    }
    fs_target_clear(&from);
    fs_target_clear(&to);
    if (result != 0) errno = code;
    return result;
}

gint radios_fs_rename(const gchar *from, const gchar *to)
{
    return radios_fs_renameat(RADIOS_AT_FDCWD, from, RADIOS_AT_FDCWD, to);
}

gint radios_fs_mkdir_with_parents(const gchar *path, guint mode)
{
    radios_platform_init();
    if (!path || !*path) return fail(path ? ENOENT : EINVAL);
    gchar *normal = radios_path_normalize(path);
    if (!normal) return fail(EINVAL);
    RadioSFileInfo info;
    if (radios_fs_stat(normal, TRUE, &info) == 0) {
        g_free(normal);
        return info.type == RADIOS_FILE_TYPE_DIRECTORY ? 0 : fail(ENOTDIR);
    }
    gsize root = root_length(normal);
    if (!root) { g_free(normal); return fail(EINVAL); }
    gint result = 0;
    for (gchar *p = normal + root; result == 0; ++p) {
        if (*p != '\\' && *p != '\0') continue;
        gchar saved = *p;
        *p = '\0';
        if (radios_fs_mkdirat(RADIOS_AT_FDCWD, normal, mode) != 0) {
            if (errno != EEXIST) result = -1;
            else if (radios_fs_stat(normal, TRUE, &info) != 0) result = -1;
            else if (info.type != RADIOS_FILE_TYPE_DIRECTORY) { errno = ENOTDIR; result = -1; }
        }
        *p = saved;
        if (saved == '\0') break;
    }
    gint code = errno;
    g_free(normal);
    errno = code;
    return result;
}

/* ------------------------------------------------------------------ realpath, access, space */

static gchar *final_path(HANDLE h)
{
    DWORD flags[] = { FILE_NAME_NORMALIZED | VOLUME_NAME_DOS, FILE_NAME_NORMALIZED | VOLUME_NAME_GUID };
    for (guint f = 0; f < G_N_ELEMENTS(flags); ++f) {
        DWORD capacity = MAX_PATH;
        for (guint attempt = 0; attempt < 4; ++attempt) {
            WCHAR *buffer = g_new(WCHAR, capacity);
            DWORD length = GetFinalPathNameByHandleW(h, buffer, capacity, flags[f]);
            if (length == 0) {
                DWORD code = GetLastError();
                g_free(buffer);
                if (f == 0 && code == ERROR_PATH_NOT_FOUND) break;
                errno = radios_platform_errno_from_win32(code);
                return NULL;
            }
            if (length >= capacity) { g_free(buffer); capacity = length + 1; continue; }
            WCHAR *start = buffer;
            gchar *result = NULL;
            if (f == 0 && wcsncmp(buffer, L"\\\\?\\UNC\\", 8) == 0) {
                start = buffer + 6;
                start[0] = L'\\';
                start[1] = L'\\';
            } else if (f == 0 && wcsncmp(buffer, L"\\\\?\\", 4) == 0) start = buffer + 4;
            result = utf8(start, -1);
            g_free(buffer);
            if (!result) { errno = EINVAL; return NULL; }
            if (g_ascii_isalpha(result[0]) && result[1] == ':') result[0] = g_ascii_toupper(result[0]);
            gsize length8 = strlen(result), root = root_length(result);
            while (length8 > 1 && length8 > root && G_IS_DIR_SEPARATOR(result[length8 - 1])) result[--length8] = '\0';
            return result;
        }
    }
    errno = ENOENT;
    return NULL;
}

gchar *radios_fs_realpath(const gchar *path, GError **error)
{
    radios_platform_init();
    WCHAR *wide = path && *path ? win32_path(path) : NULL;
    if (!wide) {
        radios_platform_set_io_error(error, path && *path ? errno : ENOENT, "%s", path ? path : "(null)");
        return NULL;
    }
    HANDLE h = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    g_free(wide);
    if (h == INVALID_HANDLE_VALUE) {
        radios_platform_set_io_error(error, radios_platform_errno_from_win32(GetLastError()), "%s", path);
        return NULL;
    }
    gchar *result = final_path(h);
    gint code = errno;
    CloseHandle(h);
    if (!result) radios_platform_set_io_error(error, code, "%s", path);
    return result;
}

gint radios_fs_dir_check_access(const gchar *directory, gboolean need_write)
{
    gint fd = radios_fs_open(directory, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY, 0);
    if (fd < 0) return -1;
    HANDLE h = fd_handle(fd);
    gsize size = 4096;
    gpointer buffer = g_malloc(size);
    gint result = 0;
    if (!GetFileInformationByHandleEx(h, FileIdBothDirectoryRestartInfo, buffer, (DWORD)size) &&
        GetLastError() != ERROR_NO_MORE_FILES) {
        result = fail_win32();
    }
    g_free(buffer);
    if (result == 0 && need_write) {
        gchar *probe = g_strdup_printf(".radios-access-%lu-%08x", (gulong)GetCurrentProcessId(), g_random_int());
        WCHAR *name = utf16(probe);
        HANDLE file = NULL;
        NTSTATUS status = nt_create(h, name, FILE_GENERIC_WRITE | DELETE, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,
                                    0, FILE_CREATE, FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, NULL, &file);
        if (NT_SUCCESS(status)) CloseHandle(file);
        else result = fail(status == STATUS_MEDIA_WRITE_PROTECTED ? EROFS : errno_from_status(status));
        g_free(name); g_free(probe);
    }
    gint code = errno;
    close(fd);
    errno = code;
    return result;
}

gboolean radios_fs_disk_space(const gchar *directory, gint64 *free_bytes, gint64 *total_bytes)
{
    radios_platform_init();
    WCHAR *wide = win32_path(directory);
    if (!wide) return FALSE;
    gsize length = wcslen(wide);
    WCHAR *with_slash = g_new(WCHAR, length + 2);
    wcscpy(with_slash, wide);
    if (length && !wide_separator(with_slash[length - 1])) wcscat(with_slash, L"\\");
    ULARGE_INTEGER available, total;
    BOOL ok = GetDiskFreeSpaceExW(with_slash, &available, &total, NULL);
    DWORD code = GetLastError();
    g_free(wide); g_free(with_slash);
    if (!ok) { errno = radios_platform_errno_from_win32(code); return FALSE; }
    if (free_bytes) *free_bytes = (gint64)MIN(available.QuadPart, (ULONGLONG)G_MAXINT64);
    if (total_bytes) *total_bytes = (gint64)MIN(total.QuadPart, (ULONGLONG)G_MAXINT64);
    return TRUE;
}

/* ------------------------------------------------------------------ descriptor ops */

GPtrArray *radios_fd_list_names(gint dirfd, GError **error)
{
    HANDLE h = fd_handle(dirfd);
    if (h == INVALID_HANDLE_VALUE) { radios_platform_set_io_error(error, EBADF, "dizin listelenemedi"); return NULL; }
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const gsize size = 64 * 1024;
    guint64 *buffer = g_malloc(size);  /* 8-byte aligned */
    FILE_INFO_BY_HANDLE_CLASS info_class = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(h, info_class, buffer, (DWORD)size)) {
            DWORD code = GetLastError();
            if (code == ERROR_NO_MORE_FILES) break;
            g_free(buffer);
            g_ptr_array_unref(names);
            radios_platform_set_io_error(error, radios_platform_errno_from_win32(code), "dizin listelenemedi");
            return NULL;
        }
        info_class = FileIdBothDirectoryInfo;
        const guint8 *entry = (const guint8 *)buffer;
        for (;;) {
            const FILE_ID_BOTH_DIR_INFO *item = (const FILE_ID_BOTH_DIR_INFO *)entry;
            glong units = (glong)(item->FileNameLength / sizeof(WCHAR));
            gboolean dot = (units == 1 && item->FileName[0] == L'.') ||
                           (units == 2 && item->FileName[0] == L'.' && item->FileName[1] == L'.');
            if (!dot) {
                gchar *name = utf8(item->FileName, units);
                if (name) g_ptr_array_add(names, name);
            }
            if (item->NextEntryOffset == 0) break;
            entry += item->NextEntryOffset;
        }
    }
    g_free(buffer);
    return names;
}

static gssize positional_io(gint fd, gpointer buffer, gsize count, gint64 offset, gboolean write)
{
    if (offset < 0) { errno = EINVAL; return -1; }
    HANDLE h = fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    gint64 position = _lseeki64(fd, 0, SEEK_CUR);
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof overlapped);
    overlapped.Offset = (DWORD)((guint64)offset & 0xFFFFFFFFu);
    overlapped.OffsetHigh = (DWORD)((guint64)offset >> 32);
    DWORD amount = (DWORD)MIN(count, (gsize)0x7FFFF000u), done = 0;
    BOOL ok = write ? WriteFile(h, buffer, amount, &done, &overlapped) : ReadFile(h, buffer, amount, &done, &overlapped);
    DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    if (position >= 0) _lseeki64(fd, position, SEEK_SET);
    if (!ok) {
        if (!write && code == ERROR_HANDLE_EOF) return 0;
        errno = radios_platform_errno_from_win32(code);
        return -1;
    }
    return (gssize)done;
}

gssize radios_fd_pread(gint fd, gpointer buffer, gsize count, gint64 offset)
{
    return positional_io(fd, buffer, count, offset, FALSE);
}

gssize radios_fd_pwrite(gint fd, gconstpointer buffer, gsize count, gint64 offset)
{
    return positional_io(fd, (gpointer)buffer, count, offset, TRUE);
}

gint64 radios_fd_seek(gint fd, gint64 offset, gint whence)
{
    return _lseeki64(fd, offset, whence);
}

gint radios_fd_truncate(gint fd, gint64 length)
{
    if (length < 0) return fail(EINVAL);
    HANDLE h = fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    /* O(1) like ftruncate (the CRT's _chsize_s writes zeros when extending);
     * the file position is not moved. */
    FILE_END_OF_FILE_INFO end;
    end.EndOfFile.QuadPart = length;
    return SetFileInformationByHandle(h, FileEndOfFileInfo, &end, sizeof end) ? 0 : fail_win32();
}

gint radios_fd_sync(gint fd)
{
    HANDLE h = fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (GetFileType(h) != FILE_TYPE_DISK) return 0;
    if (FlushFileBuffers(h)) return 0;
    DWORD code = GetLastError();
    if (code == ERROR_ACCESS_DENIED || code == ERROR_INVALID_HANDLE) return 0;  /* read-only fd / directory */
    return fail(radios_platform_errno_from_win32(code));
}

gint radios_fd_sync_dir(gint dirfd)
{
    return fd_handle(dirfd) == INVALID_HANDLE_VALUE ? -1 : 0;
}

gint radios_fd_close(gint fd)
{
    HANDLE h = fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof overlapped);
    overlapped.OffsetHigh = (DWORD)(RADIOS_LOCK_REGION_OFFSET >> 32);
    overlapped.Offset = (DWORD)(RADIOS_LOCK_REGION_OFFSET & 0xFFFFFFFFu);
    UnlockFileEx(h, 0, RADIOS_LOCK_REGION_LENGTH, 0, &overlapped);
    return close(fd);
}

gint radios_fd_lock(gint fd, gint operation)
{
    HANDLE h = fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    gint kind = operation & (RADIOS_LOCK_SH | RADIOS_LOCK_EX | RADIOS_LOCK_UN);
    if (kind != RADIOS_LOCK_SH && kind != RADIOS_LOCK_EX && kind != RADIOS_LOCK_UN) return fail(EINVAL);
    if (operation & ~(RADIOS_LOCK_SH | RADIOS_LOCK_EX | RADIOS_LOCK_UN | RADIOS_LOCK_NB)) return fail(EINVAL);
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof overlapped);
    overlapped.OffsetHigh = (DWORD)(RADIOS_LOCK_REGION_OFFSET >> 32);
    overlapped.Offset = (DWORD)(RADIOS_LOCK_REGION_OFFSET & 0xFFFFFFFFu);
    if (!UnlockFileEx(h, 0, RADIOS_LOCK_REGION_LENGTH, 0, &overlapped)) {
        DWORD code = GetLastError();
        if (code != ERROR_NOT_LOCKED) return fail(radios_platform_errno_from_win32(code));
    }
    if (kind == RADIOS_LOCK_UN) return 0;
    DWORD flags = (kind == RADIOS_LOCK_EX ? LOCKFILE_EXCLUSIVE_LOCK : 0) | ((operation & RADIOS_LOCK_NB) ? LOCKFILE_FAIL_IMMEDIATELY : 0);
    memset(&overlapped, 0, sizeof overlapped);
    overlapped.OffsetHigh = (DWORD)(RADIOS_LOCK_REGION_OFFSET >> 32);
    overlapped.Offset = (DWORD)(RADIOS_LOCK_REGION_OFFSET & 0xFFFFFFFFu);
    if (LockFileEx(h, flags, 0, RADIOS_LOCK_REGION_LENGTH, 0, &overlapped)) return 0;
    DWORD code = GetLastError();
    if (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING) return fail(EWOULDBLOCK);
    return fail(radios_platform_errno_from_win32(code));
}

/* ------------------------------------------------------------------ privacy */

static gboolean sid_equal(PSID a, PSID b)
{
    return a && b && IsValidSid(a) && IsValidSid(b) && EqualSid(a, b);
}

static gboolean volume_has_acls(HANDLE h)
{
    DWORD flags = 0;
    if (!GetVolumeInformationByHandleW(h, NULL, 0, NULL, NULL, &flags, NULL, 0)) {
        errno = radios_platform_errno_from_win32(GetLastError());
        return FALSE;
    }
    if (!(flags & FILE_PERSISTENT_ACLS)) { errno = ENOTSUP; return FALSE; }
    return TRUE;
}

static gboolean dacl_private(PACL dacl, PSID owner)
{
    if (!dacl) return FALSE;  /* NULL DACL grants everyone everything */
    const ACCESS_MASK sensitive = FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_READ_EA | FILE_WRITE_EA |
                                  FILE_EXECUTE | FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC |
                                  WRITE_OWNER | GENERIC_ALL | GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        ACE_HEADER *header = NULL;
        if (!GetAce(dacl, i, (LPVOID *)&header)) return FALSE;
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;
        PSID sid = NULL;
        ACCESS_MASK mask = 0;
        switch (header->AceType) {
        case ACCESS_ALLOWED_ACE_TYPE: case ACCESS_ALLOWED_CALLBACK_ACE_TYPE: {
            ACCESS_ALLOWED_ACE *ace = (ACCESS_ALLOWED_ACE *)header;
            mask = ace->Mask; sid = (PSID)&ace->SidStart;
            break;
        }
        case ACCESS_ALLOWED_OBJECT_ACE_TYPE: case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE: {
            ACCESS_ALLOWED_OBJECT_ACE *ace = (ACCESS_ALLOWED_OBJECT_ACE *)header;
            guint8 *p = (guint8 *)&ace->ObjectType;
            if (ace->Flags & ACE_OBJECT_TYPE_PRESENT) p += sizeof(GUID);
            if (ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) p += sizeof(GUID);
            mask = ace->Mask; sid = (PSID)p;
            break;
        }
        default:
            continue;  /* deny/audit ACEs never widen access */
        }
        if (!(mask & sensitive)) continue;
        if (sid_equal(sid, security.user) || sid_equal(sid, owner) || sid_equal(sid, (PSID)security.system_sid) ||
            sid_equal(sid, (PSID)security.admins_sid) || sid_equal(sid, (PSID)security.owner_rights_sid))
            continue;
        return FALSE;
    }
    return TRUE;
}

/* The DACL grants the current user full control (and denies nothing to it):
 * together with OWNER|NO_OTHERS this is what fchmod(0600/0700) produces. */
static gboolean dacl_user_full(PACL dacl)
{
    if (!dacl) return FALSE;
    ACCESS_MASK granted = 0;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        ACE_HEADER *header = NULL;
        if (!GetAce(dacl, i, (LPVOID *)&header)) return FALSE;
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE || header->AceType == ACCESS_DENIED_ACE_TYPE) {
            ACCESS_ALLOWED_ACE *ace = (ACCESS_ALLOWED_ACE *)header;   /* same layout for deny */
            if (!sid_equal((PSID)&ace->SidStart, security.user)) continue;
            ACCESS_MASK mask = (ace->Mask & GENERIC_ALL) ? FILE_ALL_ACCESS : ace->Mask;
            if (header->AceType == ACCESS_DENIED_ACE_TYPE) return FALSE;
            granted |= mask;
        }
    }
    return (granted & FILE_ALL_ACCESS) == FILE_ALL_ACCESS;
}

static gboolean handle_is_canonical_private(HANDLE h)
{
    PSID owner = NULL;
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, NULL, &dacl, NULL, &sd) != ERROR_SUCCESS)
        return FALSE;
    gboolean canonical = sid_equal(owner, security.user) && dacl_private(dacl, owner) && dacl_user_full(dacl);
    LocalFree(sd);
    return canonical;
}

static gboolean handle_is_private(HANDLE h, guint checks)
{
    radios_platform_init();
    if (!security.user) { errno = EACCES; return FALSE; }
    if (!volume_has_acls(h)) return FALSE;
    PSID owner = NULL;
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    DWORD code = GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                 &owner, NULL, &dacl, NULL, &sd);
    if (code != ERROR_SUCCESS) { errno = radios_platform_errno_from_win32(code); return FALSE; }
    gboolean ok = TRUE;
    if (checks & RADIOS_PRIVATE_OWNER)
        ok = sid_equal(owner, security.user) || (security.administrator && sid_equal(owner, (PSID)security.admins_sid));
    if (ok && (checks & (RADIOS_PRIVATE_NO_OTHERS | RADIOS_PRIVATE_EXACT_FILE_MODE))) ok = dacl_private(dacl, owner);
    LocalFree(sd);
    if (ok && (checks & RADIOS_PRIVATE_EXACT_FILE_MODE)) {
        RadioSFileInfo info;
        if (handle_info(h, &info) != 0) return FALSE;
        ok = info.type == RADIOS_FILE_TYPE_REGULAR;
    }
    if (!ok) errno = EACCES;
    return ok;
}

gboolean radios_fd_is_private(gint fd, guint checks)
{
    HANDLE h = fd_handle(fd);
    return h != INVALID_HANDLE_VALUE && handle_is_private(h, checks);
}

static HANDLE open_for_security(const gchar *path, gboolean follow_links, ACCESS_MASK access)
{
    WCHAR *name = nt_path(path);
    if (!name) return NULL;
    HANDLE h = NULL;
    NTSTATUS status = nt_create(NULL, name, access | FILE_READ_ATTRIBUTES | SYNCHRONIZE, 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                FILE_OPEN_FOR_BACKUP_INTENT | (follow_links ? 0 : FILE_OPEN_REPARSE_POINT), NULL, &h);
    g_free(name);
    if (!NT_SUCCESS(status)) { errno = errno_from_status(status); return NULL; }
    return h;
}

gboolean radios_fs_path_is_private(const gchar *path, gboolean follow_links, guint checks)
{
    radios_platform_init();
    HANDLE h = open_for_security(path, follow_links, READ_CONTROL);
    if (!h) return FALSE;
    gboolean ok = handle_is_private(h, checks);
    gint code = errno;
    CloseHandle(h);
    errno = code;
    return ok;
}

/* Runs SetSecurityInfo on h, or on a reopened handle with the needed rights when
 * h lacks them (descriptors opened for data access have no WRITE_DAC/WRITE_OWNER). */
static DWORD set_security(HANDLE h, ACCESS_MASK rights, SECURITY_INFORMATION what, PSID owner, PACL dacl)
{
    DWORD code = SetSecurityInfo(h, SE_FILE_OBJECT, what, owner, NULL, dacl, NULL);
    if (code != ERROR_ACCESS_DENIED) return code;
    HANDLE again = ReOpenFile(h, rights | READ_CONTROL | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_BACKUP_SEMANTICS);
    if (again == INVALID_HANDLE_VALUE) return code;
    code = SetSecurityInfo(again, SE_FILE_OBJECT, what, owner, NULL, dacl, NULL);
    CloseHandle(again);
    return code;
}

static gint make_private_handle(HANDLE h)
{
    if (!security.user) return fail(EACCES);
    if (!volume_has_acls(h)) return -1;   /* FAT/exFAT: cannot be made private (ENOTSUP) */
    if (handle_is_canonical_private(h)) return 0;
    BY_HANDLE_FILE_INFORMATION by;
    if (!GetFileInformationByHandle(h, &by)) return fail_win32();
    PACL dacl = (by.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? security.directory_dacl : security.file_dacl;
    if (!dacl) return fail(EACCES);
    /* 1. The DACL (owner rights include WRITE_DAC). */
    DWORD code = set_security(h, WRITE_DAC, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, NULL, dacl);
    if (code != ERROR_SUCCESS) return fail(radios_platform_errno_from_win32(code));
    /* 2. The owner: the new DACL grants the user WRITE_OWNER, so a reopen works
     *    even for files that an elevated run left owned by Administrators. */
    PSID owner = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    gboolean owner_ok = FALSE, owner_user = FALSE;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &sd) == ERROR_SUCCESS) {
        owner_user = sid_equal(owner, security.user);
        owner_ok = owner_user || (security.administrator && sid_equal(owner, (PSID)security.admins_sid));
    }
    if (sd) LocalFree(sd);
    if (owner_user) return 0;
    code = set_security(h, WRITE_OWNER, OWNER_SECURITY_INFORMATION, security.user, NULL);
    if (code == ERROR_SUCCESS || owner_ok) return 0;
    return fail(radios_platform_errno_from_win32(code));
}

gint radios_fd_make_private(gint fd)
{
    radios_platform_init();
    HANDLE h = fd_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    return make_private_handle(h);
}

gboolean radios_fs_path_make_private(const gchar *path, GError **error)
{
    radios_platform_init();
    HANDLE h = open_for_security(path, TRUE, READ_CONTROL | WRITE_DAC);
    if (!h && errno == EACCES) h = open_for_security(path, TRUE, READ_CONTROL);
    if (!h) return radios_platform_set_io_error(error, errno, "%s", path ? path : "(null)");
    gint result = make_private_handle(h);
    gint code = errno;
    CloseHandle(h);
    if (result != 0) return radios_platform_set_io_error(error, code, "%s", path);
    return TRUE;
}

/* ------------------------------------------------------------------ path strings */

static WCHAR *unified_wide(const gchar *path)
{
    WCHAR *wide = utf16(path);
    if (wide) for (WCHAR *p = wide; *p; ++p) if (*p == L'/') *p = L'\\';
    return wide;
}

gboolean radios_path_is_absolute(const gchar *path)
{
    return root_length(path) > 0;
}

gboolean radios_path_is_root(const gchar *path)
{
    gsize root = root_length(path);
    if (!root) return FALSE;
    gsize length = strlen(path);
    if (length == root) return TRUE;
    /* "\\server\share" without the trailing separator */
    return FALSE;
}

gboolean radios_path_equal(const gchar *a, const gchar *b)
{
    if (!a || !b) return FALSE;
    WCHAR *wa = unified_wide(a), *wb = unified_wide(b);
    gboolean equal = wa && wb && CompareStringOrdinal(wa, -1, wb, -1, TRUE) == CSTR_EQUAL;
    g_free(wa); g_free(wb);
    return equal;
}

gboolean radios_path_is_inside(const gchar *root, const gchar *path)
{
    if (!root || !path || !*root) return FALSE;
    WCHAR *wr = unified_wide(root), *wp = unified_wide(path);
    gboolean inside = FALSE;
    if (wr && wp) {
        gsize lr = wcslen(wr), lp = wcslen(wp);
        gboolean root_is_root = radios_path_is_root(root);
        if (lr > 1 && wr[lr - 1] == L'\\' && !root_is_root) wr[--lr] = L'\0';
        if (CompareStringOrdinal(wr, (int)lr, wp, (int)lp, TRUE) == CSTR_EQUAL) inside = TRUE;
        else if (lp > lr && CompareStringOrdinal(wr, (int)lr, wp, (int)lr, TRUE) == CSTR_EQUAL)
            inside = wr[lr - 1] == L'\\' || wp[lr] == L'\\';
    }
    g_free(wr); g_free(wp);
    return inside;
}

gchar *radios_path_normalize(const gchar *path)
{
    if (!path) return NULL;
    gchar *copy = g_strdup(path);
    g_strdelimit(copy, "/", '\\');
    const gchar *start = copy;
    gchar *unc = NULL;
    if (g_str_has_prefix(copy, "\\\\?\\UNC\\")) { unc = g_strconcat("\\\\", copy + 8, NULL); start = unc; }
    else if (g_str_has_prefix(copy, "\\\\?\\")) start = copy + 4;
    gchar *canonical = g_canonicalize_filename(start, NULL);
    g_free(copy); g_free(unc);
    if (!canonical) return NULL;
    g_strdelimit(canonical, "/", '\\');
    if (g_ascii_isalpha(canonical[0]) && canonical[1] == ':') canonical[0] = g_ascii_toupper(canonical[0]);
    gsize length = strlen(canonical), root = root_length(canonical);
    while (length > root && length > 1 && canonical[length - 1] == '\\') canonical[--length] = '\0';
    if (root == 0 && g_ascii_isalpha(canonical[0]) && canonical[1] == ':' && canonical[2] == '\0') {
        gchar *with = g_strconcat(canonical, "\\", NULL);
        g_free(canonical);
        canonical = with;
    }
    return canonical;
}

static gboolean reserved_device_name(const gchar *name)
{
    static const gchar *const plain[] = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };
    gsize stem = strcspn(name, ".");
    while (stem > 0 && name[stem - 1] == ' ') --stem;
    gchar *base = g_ascii_strup(name, (gssize)stem);
    gboolean reserved = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(plain) && !reserved; ++i) reserved = g_str_equal(base, plain[i]);
    if (!reserved && (g_str_has_prefix(base, "COM") || g_str_has_prefix(base, "LPT"))) {
        const gchar *suffix = base + 3;
        reserved = (g_ascii_isdigit(suffix[0]) && suffix[1] == '\0') ||
                   g_str_equal(suffix, "\xc2\xb9") || g_str_equal(suffix, "\xc2\xb2") || g_str_equal(suffix, "\xc2\xb3");
    }
    g_free(base);
    return reserved;
}

gboolean radios_path_component_valid(const gchar *name)
{
    if (!name || !*name || g_str_equal(name, ".") || g_str_equal(name, "..")) return FALSE;
    if (!g_utf8_validate(name, -1, NULL)) return FALSE;
    for (const guchar *p = (const guchar *)name; *p; ++p) {
        if (*p < 0x20 || *p == 0x7f) return FALSE;
        if (strchr("\\/:*?\"<>|", *p)) return FALSE;
    }
    gsize length = strlen(name);
    if (name[length - 1] == '.' || name[length - 1] == ' ') return FALSE;
    if (reserved_device_name(name)) return FALSE;
    glong units = 0;
    gunichar2 *wide = g_utf8_to_utf16(name, -1, NULL, &units, NULL);
    g_free(wide);
    return wide != NULL && units <= 255;
}

gchar **radios_path_split(const gchar *relative)
{
    GPtrArray *parts = g_ptr_array_new();
    if (relative) {
        const gchar *p = relative;
        while (*p) {
            while (*p && G_IS_DIR_SEPARATOR(*p)) ++p;
            const gchar *start = p;
            while (*p && !G_IS_DIR_SEPARATOR(*p)) ++p;
            if (p > start) g_ptr_array_add(parts, g_strndup(start, (gsize)(p - start)));
        }
    }
    g_ptr_array_add(parts, NULL);
    return (gchar **)g_ptr_array_free(parts, FALSE);
}

gchar *radios_path_to_portable(const gchar *relative)
{
    return relative ? g_strdelimit(g_strdup(relative), "\\", '/') : NULL;
}

gchar *radios_path_from_portable(const gchar *relative)
{
    return relative ? g_strdelimit(g_strdup(relative), "/", '\\') : NULL;
}

/* ------------------------------------------------------------------ test support */

typedef struct {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    WCHAR PathBuffer[1];
} MountPointReparseBuffer;

static gint make_junction(const gchar *target, const gchar *link_path, GError **error)
{
    gchar *parent = g_path_get_dirname(link_path);
    gchar *absolute = g_canonicalize_filename(target, parent);
    gchar *normal = radios_path_normalize(absolute);
    g_free(parent); g_free(absolute);
    WCHAR *print = utf16(normal);
    WCHAR *substitute = nt_path(normal);
    WCHAR *link = win32_path(link_path);
    g_free(normal);
    gint result = -1;
    if (!print || !substitute || !link) {
        radios_platform_set_io_error(error, EINVAL, "%s", link_path);
        goto out;
    }
    if (!CreateDirectoryW(link, NULL)) {
        radios_platform_set_io_error(error, radios_platform_errno_from_win32(GetLastError()), "%s", link_path);
        goto out;
    }
    HANDLE h = CreateFileW(link, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        radios_platform_set_io_error(error, radios_platform_errno_from_win32(GetLastError()), "%s", link_path);
        RemoveDirectoryW(link);
        goto out;
    }
    gsize sub_bytes = wcslen(substitute) * sizeof(WCHAR), print_bytes = wcslen(print) * sizeof(WCHAR);
    gsize data_length = 8 + sub_bytes + sizeof(WCHAR) + print_bytes + sizeof(WCHAR);
    gsize total = 8 + data_length;
    MountPointReparseBuffer *buffer = g_malloc0(total + sizeof(WCHAR));
    buffer->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer->ReparseDataLength = (USHORT)data_length;
    buffer->SubstituteNameOffset = 0;
    buffer->SubstituteNameLength = (USHORT)sub_bytes;
    buffer->PrintNameOffset = (USHORT)(sub_bytes + sizeof(WCHAR));
    buffer->PrintNameLength = (USHORT)print_bytes;
    memcpy(buffer->PathBuffer, substitute, sub_bytes);
    memcpy((guint8 *)buffer->PathBuffer + sub_bytes + sizeof(WCHAR), print, print_bytes);
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buffer, (DWORD)total, NULL, 0, &returned, NULL);
    DWORD code = GetLastError();
    g_free(buffer);
    CloseHandle(h);
    if (!ok) {
        RemoveDirectoryW(link);
        radios_platform_set_io_error(error, radios_platform_errno_from_win32(code), "%s", link_path);
        goto out;
    }
    result = 0;
out:
    g_free(print); g_free(substitute); g_free(link);
    return result;
}

gint radios_platform_test_symlink(const gchar *target, const gchar *link_path, gboolean directory, GError **error)
{
    radios_platform_init();
    gchar *native = target ? g_strdelimit(g_strdup(target), "/", '\\') : NULL;
    WCHAR *wtarget = utf16(native);
    WCHAR *wlink = link_path ? win32_path(link_path) : NULL;
    g_free(native);
    if (!wtarget || !wlink) {
        g_free(wtarget); g_free(wlink);
        radios_platform_set_io_error(error, EINVAL, "%s", link_path ? link_path : "(null)");
        return -1;
    }
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE | (directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
    BOOLEAN ok = CreateSymbolicLinkW(wlink, wtarget, flags);
    DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    g_free(wtarget); g_free(wlink);
    if (ok) return 0;
    if (code != ERROR_PRIVILEGE_NOT_HELD && code != ERROR_INVALID_PARAMETER) {
        radios_platform_set_io_error(error, radios_platform_errno_from_win32(code), "%s", link_path);
        return -1;
    }
    if (directory) return make_junction(target, link_path, error);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "%s: dosya bağlantısı oluşturma yetkisi yok (Geliştirici Modu kapalı)", link_path);
    errno = ENOTSUP;
    return -1;
}

gboolean radios_platform_test_make_public(const gchar *path, GError **error)
{
    radios_platform_init();
    WCHAR *wide = win32_path(path);
    if (!wide) return radios_platform_set_io_error(error, errno, "%s", path ? path : "(null)");
    PACL old = NULL, updated = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    DWORD code = GetNamedSecurityInfoW(wide, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &old, NULL, &sd);
    if (code == ERROR_SUCCESS) {
        EXPLICIT_ACCESS_W access;
        memset(&access, 0, sizeof access);
        access.grfAccessPermissions = FILE_GENERIC_READ;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = (LPWSTR)security.users_sid;
        code = SetEntriesInAclW(1, &access, old, &updated);
        if (code == ERROR_SUCCESS)
            code = SetNamedSecurityInfoW(wide, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, updated, NULL);
    }
    if (updated) LocalFree(updated);
    if (sd) LocalFree(sd);
    g_free(wide);
    if (code != ERROR_SUCCESS) return radios_platform_set_io_error(error, radios_platform_errno_from_win32(code), "%s", path);
    return TRUE;
}
