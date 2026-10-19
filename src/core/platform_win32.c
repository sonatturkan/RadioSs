/* RadioS platform layer: processes, startup of the GUI-subsystem executable,
 * graceful termination and crash dumps (Windows 10 1903+).
 *
 * Child processes are started with CreateProcessW only: no console window
 * (CREATE_NO_WINDOW), an explicit PROC_THREAD_ATTRIBUTE_HANDLE_LIST that holds
 * nothing but the child's std handles (lock handles never leak into children),
 * MSVCRT argument quoting and a sorted UTF-16 environment block. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <sddl.h>

#include "radios/platform.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

#define RELAUNCH_VARIABLE "RADIOS_RELAUNCH_WAIT_HANDLE"
#define PIPE_BUFFER (64 * 1024)
#define GRACE_AFTER_EXIT_MS 1000

/* ------------------------------------------------------------------ small helpers */

static WCHAR *to_wide(const gchar *text)
{
    return text ? (WCHAR *)g_utf8_to_utf16(text, -1, NULL, NULL, NULL) : NULL;
}

static gchar *from_wide(const WCHAR *text, glong length)
{
    return g_utf16_to_utf8((const gunichar2 *)text, length, NULL, NULL, NULL);
}

static gboolean win32_error(GError **error, DWORD code, const gchar *subject)
{
    return radios_platform_set_io_error(error, radios_platform_errno_from_win32(code), "%s", subject);
}

/* "D:P(A;;GA;;;<user>)(A;;GA;;;SY)" for pipes and crash dumps (owner = creator). */
static PSECURITY_DESCRIPTOR owner_only_descriptor(void)
{
    static gsize initialised = 0;
    static PSECURITY_DESCRIPTOR descriptor = NULL;
    if (g_once_init_enter(&initialised)) {
        HANDLE token = NULL;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, NULL, 0, &size);
            TOKEN_USER *user = size ? g_malloc(size) : NULL;
            LPWSTR sid = NULL;
            if (user && GetTokenInformation(token, TokenUser, user, size, &size) && ConvertSidToStringSidW(user->User.Sid, &sid)) {
                gchar *text = from_wide(sid, -1);
                gchar *sddl = g_strdup_printf("O:%sD:P(A;;GA;;;%s)(A;;GA;;;SY)", text, text);
                WCHAR *wide = to_wide(sddl);
                if (wide) ConvertStringSecurityDescriptorToSecurityDescriptorW(wide, SDDL_REVISION_1, &descriptor, NULL);
                g_free(wide); g_free(sddl); g_free(text);
                LocalFree(sid);
            }
            g_free(user);
            CloseHandle(token);
        }
        g_once_init_leave(&initialised, 1);
    }
    return descriptor;
}

static gchar *module_path(HMODULE module, GError **error)
{
    DWORD capacity = MAX_PATH;
    while (capacity <= 32768 * 2) {
        WCHAR *buffer = g_new(WCHAR, capacity);
        DWORD length = GetModuleFileNameW(module, buffer, capacity);
        if (length == 0) {
            DWORD code = GetLastError();
            g_free(buffer);
            win32_error(error, code, "program yolu okunamadı");
            return NULL;
        }
        if (length < capacity) {
            WCHAR *start = buffer;
            if (wcsncmp(buffer, L"\\\\?\\UNC\\", 8) == 0) { start = buffer + 6; start[0] = L'\\'; }
            else if (wcsncmp(buffer, L"\\\\?\\", 4) == 0) start = buffer + 4;
            gchar *path = from_wide(start, -1);
            g_free(buffer);
            if (!path) radios_platform_set_io_error(error, EINVAL, "program yolu okunamadı");
            return path;
        }
        g_free(buffer);
        capacity *= 2;
    }
    radios_platform_set_io_error(error, ENAMETOOLONG, "program yolu okunamadı");
    return NULL;
}

/* ------------------------------------------------------------------ identity, argv, threads */

gchar *radios_process_executable_path(GError **error)
{
    radios_platform_init();
    return module_path(NULL, error);
}

static GMutex argv_lock;
static gchar **remembered_argv;

static gchar **current_argv(gint argc, gchar **argv)
{
    gchar **args = g_win32_get_command_line();
    if (args && args[0]) return args;
    g_strfreev(args);
    GPtrArray *copy = g_ptr_array_new();
    for (gint i = 0; argv && i < argc && argv[i]; ++i) {
        gchar *utf8 = g_locale_to_utf8(argv[i], -1, NULL, NULL, NULL);
        g_ptr_array_add(copy, utf8 ? utf8 : g_strdup(argv[i]));
    }
    g_ptr_array_add(copy, NULL);
    return (gchar **)g_ptr_array_free(copy, FALSE);
}

void radios_process_remember_argv(gint argc, gchar **argv)
{
    gchar **args = current_argv(argc, argv);
    g_mutex_lock(&argv_lock);
    g_strfreev(remembered_argv);
    remembered_argv = args;
    g_mutex_unlock(&argv_lock);
}

gchar **radios_process_argv(void)
{
    g_mutex_lock(&argv_lock);
    gchar **copy = remembered_argv ? g_strdupv(remembered_argv) : NULL;
    g_mutex_unlock(&argv_lock);
    return copy ? copy : current_argv(0, NULL);
}

gchar **radios_platform_get_argv(gint argc, gchar **argv)
{
    return current_argv(argc, argv);
}

guint32 radios_process_id(void)
{
    return (guint32)GetCurrentProcessId();
}

void radios_thread_lower_priority(void)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

/* ------------------------------------------------------------------ graceful termination */

static GMutex termination_lock;
static HANDLE stop_event, teardown_done;
static GHashTable *termination_sources;   /* source id -> GSource* */
static gint termination_watchers;
static gboolean console_handler_installed;

gchar *radios_process_stop_event_name(guint32 pid)
{
    return g_strdup_printf("Local\\RadioS-stop-%lu", (gulong)pid);
}

typedef struct {
    GSource source;
    GPollFD poll;
} StopSource;

static gboolean stop_prepare(GSource *source, gint *timeout)
{
    (void)source;
    *timeout = -1;
    return FALSE;
}

static gboolean stop_check(GSource *source)
{
    return (((StopSource *)source)->poll.revents & G_IO_IN) != 0;
}

static gboolean stop_dispatch(GSource *source, GSourceFunc callback, gpointer data)
{
    ((StopSource *)source)->poll.revents = 0;
    ResetEvent(stop_event);
    return callback ? callback(data) : G_SOURCE_REMOVE;
}

static GSourceFuncs stop_funcs = { stop_prepare, stop_check, stop_dispatch, NULL, NULL, NULL };

static BOOL WINAPI console_handler(DWORD type)
{
    if (g_atomic_int_get(&termination_watchers) <= 0 || !stop_event) return FALSE;
    SetEvent(stop_event);
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        /* Returning ends the process: keep it alive while the GTK thread tears
         * down (WAV finalisation, durable writes), within the OS limit. */
        WaitForSingleObject(teardown_done, 4500);
    }
    return TRUE;
}

static gboolean stop_channel(void)
{
    if (stop_event) return TRUE;
    gchar *name = radios_process_stop_event_name(radios_process_id());
    WCHAR *wide = to_wide(name);
    g_free(name);
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, wide);
    DWORD code = GetLastError();
    g_free(wide);
    if (!event) return FALSE;
    if (code == ERROR_ALREADY_EXISTS) ResetEvent(event);   /* never inherit a stale request */
    stop_event = event;
    teardown_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    return TRUE;
}

guint radios_process_add_termination_watch(GSourceFunc callback, gpointer data, GDestroyNotify notify)
{
    g_return_val_if_fail(callback != NULL, 0);
    radios_platform_init();
    g_mutex_lock(&termination_lock);
    if (!stop_channel()) {
        g_mutex_unlock(&termination_lock);
        g_critical("RadioS durdurma olayı oluşturulamadı (Win32 hata %lu)", (gulong)GetLastError());
        return 0;
    }
    GSource *source = g_source_new(&stop_funcs, sizeof(StopSource));
    StopSource *stop = (StopSource *)source;
    stop->poll.fd = (gintptr)stop_event;
    stop->poll.events = G_IO_IN;
    g_source_add_poll(source, &stop->poll);
    g_source_set_priority(source, G_PRIORITY_HIGH);
    g_source_set_callback(source, callback, data, notify);
    g_source_set_static_name(source, "RadioS termination watch");
    guint id = g_source_attach(source, NULL);
    if (!termination_sources) termination_sources = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(termination_sources, GUINT_TO_POINTER(id), source);
    if (g_atomic_int_add(&termination_watchers, 1) == 0) ResetEvent(teardown_done);
    if (!console_handler_installed) console_handler_installed = SetConsoleCtrlHandler(console_handler, TRUE) != FALSE;
    g_mutex_unlock(&termination_lock);
    return id;
}

void radios_process_remove_termination_watch(guint id)
{
    if (!id) return;
    g_mutex_lock(&termination_lock);
    GSource *source = termination_sources ? g_hash_table_lookup(termination_sources, GUINT_TO_POINTER(id)) : NULL;
    if (source) {
        g_hash_table_remove(termination_sources, GUINT_TO_POINTER(id));
        g_source_destroy(source);
        g_source_unref(source);
        if (g_atomic_int_dec_and_test(&termination_watchers) && teardown_done) SetEvent(teardown_done);
    }
    g_mutex_unlock(&termination_lock);
}

gboolean radios_process_request_stop(guint32 pid, GError **error)
{
    gchar *name = radios_process_stop_event_name(pid);
    WCHAR *wide = to_wide(name);
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, wide);
    DWORD code = event ? ERROR_SUCCESS : GetLastError();
    g_free(wide);
    if (!event) {
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%lu numaralı RadioS sürecinin durdurma kanalı yok (%s)",
                        (gulong)pid, name);
        else win32_error(error, code, name);
        g_free(name);
        return FALSE;
    }
    BOOL ok = SetEvent(event);
    code = GetLastError();
    CloseHandle(event);
    if (!ok) { win32_error(error, code, name); g_free(name); return FALSE; }
    g_free(name);
    return TRUE;
}

/* ------------------------------------------------------------------ command line + environment */

static gboolean append_quoted(GString *line, const gchar *arg)
{
    if (*arg && !strpbrk(arg, " \t\n\v\"")) { g_string_append(line, arg); return TRUE; }
    g_string_append_c(line, '"');
    for (const gchar *p = arg;; ++p) {
        gsize backslashes = 0;
        while (*p == '\\') { ++backslashes; ++p; }
        if (*p == '\0') { for (gsize i = 0; i < backslashes * 2; ++i) g_string_append_c(line, '\\'); break; }
        if (*p == '"') { for (gsize i = 0; i < backslashes * 2 + 1; ++i) g_string_append_c(line, '\\'); }
        else { for (gsize i = 0; i < backslashes; ++i) g_string_append_c(line, '\\'); }
        g_string_append_c(line, *p);
    }
    g_string_append_c(line, '"');
    return TRUE;
}

gchar *radios_process_quote_command_line(const gchar *const *argv)
{
    g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);
    GString *line = g_string_new(NULL);
    /* argv[0] is parsed without backslash escapes by the CRT: plain quotes. */
    if (strpbrk(argv[0], " \t") || !*argv[0]) g_string_append_printf(line, "\"%s\"", argv[0]);
    else g_string_append(line, argv[0]);
    for (guint i = 1; argv[i]; ++i) {
        g_string_append_c(line, ' ');
        append_quoted(line, argv[i]);
    }
    return g_string_free(line, FALSE);
}

/* Length of the name part of "NAME=VALUE" (names may start with '=': "=C:=C:\x"). */
static gsize env_name_length(const gchar *entry)
{
    const gchar *equal = entry[0] ? strchr(entry + 1, '=') : NULL;
    return equal ? (gsize)(equal - entry) : strlen(entry);
}

static gboolean env_names_equal(const gchar *a, gsize la, const gchar *b, gsize lb)
{
    glong wa = 0, wb = 0;
    gunichar2 *ua = g_utf8_to_utf16(a, (glong)la, NULL, &wa, NULL), *ub = g_utf8_to_utf16(b, (glong)lb, NULL, &wb, NULL);
    gboolean equal = ua && ub && CompareStringOrdinal((LPCWCH)ua, (int)wa, (LPCWCH)ub, (int)wb, TRUE) == CSTR_EQUAL;
    g_free(ua); g_free(ub);
    return equal;
}

gchar **radios_process_environment(const gchar *const *overrides)
{
    gchar **current = g_get_environ();
    GPtrArray *result = g_ptr_array_new();
    for (guint i = 0; current[i]; ++i) {
        gsize length = env_name_length(current[i]);
        gboolean replaced = FALSE;
        for (guint j = 0; overrides && overrides[j] && !replaced; ++j)
            replaced = env_names_equal(current[i], length, overrides[j], env_name_length(overrides[j]));
        if (replaced) g_free(current[i]); else g_ptr_array_add(result, current[i]);
    }
    g_free(current);   /* the strings moved into result or were freed */
    for (guint j = 0; overrides && overrides[j]; ++j) {
        if (!overrides[j][0] || !strchr(overrides[j] + 1, '=')) continue;   /* removal only */
        gsize length = env_name_length(overrides[j]);
        gboolean later = FALSE;   /* a later override of the same name wins */
        for (guint k = j + 1; overrides[k] && !later; ++k) later = env_names_equal(overrides[j], length, overrides[k], env_name_length(overrides[k]));
        if (!later) g_ptr_array_add(result, g_strdup(overrides[j]));
    }
    g_ptr_array_add(result, NULL);
    return (gchar **)g_ptr_array_free(result, FALSE);
}

typedef struct {
    WCHAR *text;     /* "NAME=VALUE" */
    gsize name_length;
    guint order;
} EnvEntry;

static gint env_compare(gconstpointer pa, gconstpointer pb)
{
    const EnvEntry *a = *(const EnvEntry *const *)pa, *b = *(const EnvEntry *const *)pb;
    gint result = CompareStringOrdinal(a->text, (int)a->name_length, b->text, (int)b->name_length, TRUE) - CSTR_EQUAL;
    if (result) return result;
    return a->order < b->order ? -1 : a->order > b->order ? 1 : 0;
}

static void env_entry_free(gpointer data)
{
    EnvEntry *entry = data;
    g_free(entry->text);
    g_free(entry);
}

static gboolean env_add(GPtrArray *entries, const WCHAR *text, GError **error)
{
    /* Names may start with '=' (per-drive current directories "=C:=C:\x"). */
    const WCHAR *equal = text[0] ? wcschr(text + 1, L'=') : NULL;
    if (!equal) {
        gchar *shown = from_wide(text, -1);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Geçersiz ortam değişkeni: %s", shown ? shown : "?");
        g_free(shown);
        return FALSE;
    }
    EnvEntry *entry = g_new0(EnvEntry, 1);
    gsize length = wcslen(text);
    entry->text = g_new(WCHAR, length + 1);
    memcpy(entry->text, text, (length + 1) * sizeof(WCHAR));
    entry->name_length = (gsize)(equal - text);
    entry->order = entries->len;
    g_ptr_array_add(entries, entry);
    return TRUE;
}

/* Sorted (case-insensitive, as CreateProcess requires), duplicate-free (last
 * definition wins), double-NUL terminated UTF-16 block. */
static WCHAR *environment_block(const gchar *const *envp, const gchar *extra, GError **error)
{
    GPtrArray *entries = g_ptr_array_new_with_free_func(env_entry_free);
    gboolean ok = TRUE;
    if (envp) {
        for (guint i = 0; ok && envp[i]; ++i) {
            WCHAR *wide = to_wide(envp[i]);
            if (!wide) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Ortam değişkeni UTF-8 değil");
                ok = FALSE;
                break;
            }
            ok = env_add(entries, wide, error);
            g_free(wide);
        }
    } else {
        WCHAR *strings = GetEnvironmentStringsW();
        for (const WCHAR *p = strings; ok && p && *p; p += wcslen(p) + 1)
            if (wcschr(p + 1, L'=')) ok = env_add(entries, p, error);
        if (strings) FreeEnvironmentStringsW(strings);
    }
    if (ok && extra) {
        WCHAR *wide = to_wide(extra);
        ok = wide && env_add(entries, wide, error);
        g_free(wide);
    }
    if (!ok) { g_ptr_array_unref(entries); return NULL; }
    g_ptr_array_sort(entries, env_compare);
    GArray *block = g_array_new(FALSE, FALSE, sizeof(WCHAR));
    for (guint i = 0; i < entries->len; ++i) {
        const EnvEntry *entry = entries->pdata[i];
        if (i + 1 < entries->len) {
            const EnvEntry *next = entries->pdata[i + 1];
            if (CompareStringOrdinal(entry->text, (int)entry->name_length, next->text, (int)next->name_length, TRUE) == CSTR_EQUAL)
                continue;   /* a later definition of the same name follows */
        }
        g_array_append_vals(block, entry->text, (guint)wcslen(entry->text) + 1);
    }
    WCHAR zero = L'\0';
    if (block->len == 0) g_array_append_val(block, zero);
    g_array_append_val(block, zero);
    g_ptr_array_unref(entries);
    return (WCHAR *)g_array_free(block, FALSE);
}

/* ------------------------------------------------------------------ processes */

struct RadioSProcess {
    gint ref_count;
    HANDLE process;
    DWORD pid;
    GMutex lock;
    gboolean communicating;
    HANDLE pipe_in, pipe_out, pipe_err;   /* owned overlapped parent ends, or NULL */
};

static RadioSProcess *process_ref(RadioSProcess *process)
{
    g_atomic_int_inc(&process->ref_count);
    return process;
}

static void close_handle(HANDLE *h)
{
    if (*h && *h != INVALID_HANDLE_VALUE) CloseHandle(*h);
    *h = NULL;
}

static void process_unref(RadioSProcess *process)
{
    if (!process || !g_atomic_int_dec_and_test(&process->ref_count)) return;
    close_handle(&process->pipe_in);
    close_handle(&process->pipe_out);
    close_handle(&process->pipe_err);
    close_handle(&process->process);
    g_mutex_clear(&process->lock);
    g_free(process);
}

static gboolean usable_handle(HANDLE h)
{
    return h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
}

/* Child-side handles: created non-inheritable, made inheritable only for the
 * CreateProcessW call and listed explicitly. */
typedef struct {
    HANDLE child[3];       /* stdin, stdout, stderr for the child */
    HANDLE close_after[8]; /* parent copies of child ends + self handle */
    guint close_count;
    HANDLE parent_sync[3]; /* synchronous parent ends handed out as fds */
    HANDLE parent_owned[3];/* overlapped parent ends kept in the process */
} StdioPlan;

static void plan_close(StdioPlan *plan, HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return;
    for (guint i = 0; i < plan->close_count; ++i) if (plan->close_after[i] == h) return;
    if (plan->close_count < G_N_ELEMENTS(plan->close_after)) plan->close_after[plan->close_count++] = h;
}

static void plan_clear(StdioPlan *plan, gboolean failed)
{
    for (guint i = 0; i < plan->close_count; ++i) CloseHandle(plan->close_after[i]);
    plan->close_count = 0;
    if (failed) {
        for (guint i = 0; i < 3; ++i) { close_handle(&plan->parent_sync[i]); close_handle(&plan->parent_owned[i]); }
    }
}

static HANDLE open_null(DWORD access)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
    return CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
}

static volatile LONG pipe_serial;

/* Overlapped parent end + synchronous child end of a private named pipe. */
static gboolean overlapped_pipe(gboolean parent_reads, HANDLE *parent, HANDLE *child)
{
    gchar *name = g_strdup_printf("\\\\.\\pipe\\RadioS-%lu-%ld-%08x", (gulong)GetCurrentProcessId(),
                                  (long)InterlockedIncrement(&pipe_serial), g_random_int());
    WCHAR *wide = to_wide(name);
    g_free(name);
    SECURITY_ATTRIBUTES sa = { sizeof sa, owner_only_descriptor(), FALSE };
    *parent = CreateNamedPipeW(wide, (parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED |
                               FILE_FLAG_FIRST_PIPE_INSTANCE,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                               1, PIPE_BUFFER, PIPE_BUFFER, 0, &sa);
    if (*parent == INVALID_HANDLE_VALUE) { *parent = NULL; g_free(wide); return FALSE; }
    *child = CreateFileW(wide, parent_reads ? GENERIC_WRITE : GENERIC_READ, 0, &sa, OPEN_EXISTING, 0, NULL);
    g_free(wide);
    if (*child == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        CloseHandle(*parent);
        *parent = *child = NULL;
        SetLastError(code);
        return FALSE;
    }
    return TRUE;
}

static gboolean plan_stream(StdioPlan *plan, guint index, gboolean pipe, gboolean caller_fd, gboolean null_stdio,
                            GError **error)
{
    static const DWORD std_ids[3] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
    gboolean parent_reads = index != 0;
    HANDLE child = NULL;
    if (pipe && caller_fd) {
        SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
        HANDLE read_end = NULL, write_end = NULL;
        if (!CreatePipe(&read_end, &write_end, &sa, PIPE_BUFFER)) return win32_error(error, GetLastError(), "CreatePipe");
        plan->parent_sync[index] = parent_reads ? read_end : write_end;
        child = parent_reads ? write_end : read_end;
    } else if (pipe) {
        HANDLE parent = NULL;
        if (!overlapped_pipe(parent_reads, &parent, &child)) return win32_error(error, GetLastError(), "CreateNamedPipe");
        plan->parent_owned[index] = parent;
    } else {
        HANDLE ours = null_stdio ? NULL : GetStdHandle(std_ids[index]);
        if (usable_handle(ours)) {
            if (!DuplicateHandle(GetCurrentProcess(), ours, GetCurrentProcess(), &child, 0, FALSE, DUPLICATE_SAME_ACCESS))
                child = NULL;
        }
        if (!child) child = open_null(index == 0 ? GENERIC_READ : GENERIC_WRITE);
        if (child == INVALID_HANDLE_VALUE || !child) return win32_error(error, GetLastError(), "NUL");
    }
    plan->child[index] = child;
    plan_close(plan, child);
    return TRUE;
}

static gint fd_from_handle(HANDLE h, gboolean readable)
{
    gint fd = _open_osfhandle((intptr_t)h, _O_BINARY | _O_NOINHERIT | (readable ? _O_RDONLY : _O_WRONLY));
    if (fd < 0) CloseHandle(h);
    return fd;
}

static gboolean has_suffix_ci(const gchar *text, const gchar *suffix)
{
    gsize lt = strlen(text), ls = strlen(suffix);
    return lt >= ls && g_ascii_strcasecmp(text + lt - ls, suffix) == 0;
}

RadioSProcess *radios_process_spawn(const gchar *const *argv, const gchar *working_directory,
                                    const gchar *const *envp, guint flags,
                                    gint *stdin_fd, gint *stdout_fd, gint *stderr_fd, GError **error)
{
    g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    radios_platform_init();
    if (stdin_fd) *stdin_fd = -1;
    if (stdout_fd) *stdout_fd = -1;
    if (stderr_fd) *stderr_fd = -1;
    const gchar *program = argv[0];
    if (!radios_path_is_absolute(program) || strchr(program, '"')) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Program yolu mutlak olmalı: %s", program);
        return NULL;
    }
    if (has_suffix_ci(program, ".bat") || has_suffix_ci(program, ".cmd")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Toplu iş dosyaları doğrudan çalıştırılmaz: %s", program);
        return NULL;
    }
    gchar *native_program = g_strdelimit(g_strdup(program), "/", '\\');
    GPtrArray *native_argv = g_ptr_array_new();
    g_ptr_array_add(native_argv, native_program);
    for (guint i = 1; argv[i]; ++i) g_ptr_array_add(native_argv, (gpointer)argv[i]);
    g_ptr_array_add(native_argv, NULL);
    gchar *line = radios_process_quote_command_line((const gchar *const *)native_argv->pdata);
    g_ptr_array_unref(native_argv);
    WCHAR *application = to_wide(native_program), *command = to_wide(line);
    g_free(line);
    g_free(native_program);
    WCHAR *directory = NULL;
    if (working_directory) {
        gchar *native = g_strdelimit(g_strdup(working_directory), "/", '\\');
        directory = to_wide(native);
        g_free(native);
    }
    if (!application || !command || (working_directory && !directory) || wcslen(command) >= 32767) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Komut satırı geçersiz veya çok uzun: %s", program);
        g_free(application); g_free(command); g_free(directory);
        return NULL;
    }
    gboolean detached = (flags & RADIOS_SPAWN_DETACHED) != 0;
    gboolean null_stdio = detached || (flags & RADIOS_SPAWN_NULL_STDIO);
    StdioPlan plan;
    memset(&plan, 0, sizeof plan);
    gboolean ok = plan_stream(&plan, 0, (flags & RADIOS_SPAWN_STDIN_PIPE) != 0, stdin_fd != NULL, null_stdio, error) &&
                  plan_stream(&plan, 1, (flags & RADIOS_SPAWN_STDOUT_PIPE) != 0, stdout_fd != NULL, null_stdio, error);
    if (ok) {
        if (flags & RADIOS_SPAWN_STDERR_TO_STDOUT) plan.child[2] = plan.child[1];
        else ok = plan_stream(&plan, 2, (flags & RADIOS_SPAWN_STDERR_PIPE) != 0, stderr_fd != NULL, null_stdio, error);
    }
    HANDLE self = NULL;
    gchar *relaunch = NULL;
    if (ok && (flags & RADIOS_SPAWN_PASS_SELF_HANDLE)) {
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self,
                             SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0))
            ok = win32_error(error, GetLastError(), "DuplicateHandle");
        else {
            plan_close(&plan, self);
            relaunch = g_strdup_printf(RELAUNCH_VARIABLE "=%" G_GUINT64_FORMAT, (guint64)(guintptr)self);
        }
    }
    WCHAR *environment = NULL;
    if (ok && (envp || relaunch)) {
        environment = environment_block(envp, relaunch, error);
        ok = environment != NULL;
    }
    g_free(relaunch);
    RadioSProcess *process = NULL;
    if (ok) {
        HANDLE list[4];
        guint count = 0;
        for (guint i = 0; i < 3; ++i) {
            gboolean seen = FALSE;
            for (guint j = 0; j < count; ++j) seen |= list[j] == plan.child[i];
            if (!seen) list[count++] = plan.child[i];
        }
        if (self) list[count++] = self;
        for (guint i = 0; i < count; ++i) SetHandleInformation(list[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &size);
        LPPROC_THREAD_ATTRIBUTE_LIST attributes = g_malloc(size);
        STARTUPINFOEXW info;
        memset(&info, 0, sizeof info);
        info.StartupInfo.cb = sizeof info;
        info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        info.StartupInfo.hStdInput = plan.child[0];
        info.StartupInfo.hStdOutput = plan.child[1];
        info.StartupInfo.hStdError = plan.child[2];
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size) ||
            !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, count * sizeof(HANDLE), NULL, NULL)) {
            ok = win32_error(error, GetLastError(), "PROC_THREAD_ATTRIBUTE_HANDLE_LIST");
        } else {
            info.lpAttributeList = attributes;
            DWORD creation = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
            if (detached) creation |= CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
            PROCESS_INFORMATION pi;
            memset(&pi, 0, sizeof pi);
            BOOL created = CreateProcessW(application, command, NULL, NULL, TRUE, creation, environment, directory,
                                          &info.StartupInfo, &pi);
            DWORD code = created ? ERROR_SUCCESS : GetLastError();
            if (!created && detached && code == ERROR_ACCESS_DENIED) {
                /* The job we run in forbids breakaway: the child stays in it. */
                created = CreateProcessW(application, command, NULL, NULL, TRUE, creation & ~(DWORD)CREATE_BREAKAWAY_FROM_JOB,
                                         environment, directory, &info.StartupInfo, &pi);
                code = created ? ERROR_SUCCESS : GetLastError();
            }
            if (!created) ok = win32_error(error, code, program);
            else {
                CloseHandle(pi.hThread);
                process = g_new0(RadioSProcess, 1);
                process->ref_count = 1;
                process->process = pi.hProcess;
                process->pid = pi.dwProcessId;
                g_mutex_init(&process->lock);
            }
            DeleteProcThreadAttributeList(attributes);
        }
        g_free(attributes);
    }
    plan_clear(&plan, !ok);
    g_free(application); g_free(command); g_free(directory); g_free(environment);
    if (!ok) return NULL;
    gint *fds[3] = { stdin_fd, stdout_fd, stderr_fd };
    for (guint i = 0; i < 3; ++i) {
        if (plan.parent_sync[i]) {
            gint fd = fd_from_handle(plan.parent_sync[i], i != 0);
            if (fds[i]) *fds[i] = fd;
        }
    }
    process->pipe_in = plan.parent_owned[0];
    process->pipe_out = plan.parent_owned[1];
    process->pipe_err = plan.parent_owned[2];
    return process;
}

guint32 radios_process_get_id(RadioSProcess *process)
{
    g_return_val_if_fail(process != NULL, 0);
    return (guint32)process->pid;
}

gboolean radios_process_wait(RadioSProcess *process, guint timeout_ms, gint *exit_code)
{
    g_return_val_if_fail(process != NULL, FALSE);
    DWORD wait = WaitForSingleObject(process->process, timeout_ms == RADIOS_PROCESS_WAIT_INFINITE ? INFINITE : timeout_ms);
    if (wait != WAIT_OBJECT_0) return FALSE;
    DWORD code = 0;
    if (!GetExitCodeProcess(process->process, &code)) code = (DWORD)-1;
    if (exit_code) *exit_code = (gint)code;
    return TRUE;
}

typedef struct {
    RadioSProcess *process;
    RadioSProcessExitFunc callback;
    gpointer data;
} WatchData;

static void watch_exited(GPid pid, gint status, gpointer user_data)
{
    (void)pid;
    WatchData *watch = user_data;
    watch->callback(watch->process, status, watch->data);
}

static void watch_free(gpointer user_data)
{
    WatchData *watch = user_data;
    process_unref(watch->process);
    g_free(watch);
}

guint radios_process_watch(RadioSProcess *process, GMainContext *context, RadioSProcessExitFunc callback, gpointer data)
{
    g_return_val_if_fail(process != NULL && callback != NULL, 0);
    WatchData *watch = g_new0(WatchData, 1);
    watch->process = process_ref(process);
    watch->callback = callback;
    watch->data = data;
    GSource *source = g_child_watch_source_new((GPid)process->process);
    g_source_set_callback(source, G_SOURCE_FUNC(watch_exited), watch, watch_free);
    guint id = g_source_attach(source, context);
    g_source_unref(source);
    return id;
}

gboolean radios_process_terminate(RadioSProcess *process, gint exit_code)
{
    g_return_val_if_fail(process != NULL, FALSE);
    if (WaitForSingleObject(process->process, 0) == WAIT_OBJECT_0) return TRUE;
    if (TerminateProcess(process->process, (UINT)exit_code)) return TRUE;
    DWORD code = GetLastError();
    if (WaitForSingleObject(process->process, 0) == WAIT_OBJECT_0) return TRUE;
    errno = radios_platform_errno_from_win32(code);
    return FALSE;
}

void radios_process_free(RadioSProcess *process)
{
    process_unref(process);
}

/* ------------------------------------------------------------------ communicate */

typedef struct {
    HANDLE handle;
    OVERLAPPED overlapped;
    gboolean pending, done, writer;
    guint8 buffer[16384];
    GByteArray *data;      /* reader: collected bytes */
    gsize limit;
    const guint8 *source;  /* writer: remaining bytes */
    gsize remaining;
} Stream;

static void stream_init(Stream *stream, HANDLE handle, gboolean writer, gsize limit)
{
    memset(stream, 0, sizeof *stream);
    stream->handle = handle;
    stream->writer = writer;
    stream->limit = limit;
    stream->done = handle == NULL;
    if (handle) stream->overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!writer) stream->data = g_byte_array_new();
}

static void stream_cancel(Stream *stream)
{
    if (stream->pending) {
        DWORD amount = 0;
        CancelIoEx(stream->handle, &stream->overlapped);
        GetOverlappedResult(stream->handle, &stream->overlapped, &amount, TRUE);
        stream->pending = FALSE;
    }
}

static void stream_clear(Stream *stream)
{
    stream_cancel(stream);
    if (stream->overlapped.hEvent) CloseHandle(stream->overlapped.hEvent);
    if (stream->data) g_byte_array_unref(stream->data);
    memset(stream, 0, sizeof *stream);
}

static void stream_take(Stream *stream, DWORD amount)
{
    if (stream->writer) {
        stream->source += amount;
        stream->remaining -= MIN((gsize)amount, stream->remaining);
        if (stream->remaining == 0) { CloseHandle(stream->handle); stream->handle = NULL; stream->done = TRUE; }
        return;
    }
    gsize room = stream->data->len < stream->limit ? stream->limit - stream->data->len : 0;
    if (room) g_byte_array_append(stream->data, stream->buffer, (guint)MIN((gsize)amount, room));
}

/* Starts I/O until an operation is pending or the stream is finished.
 * FALSE + *code on a real error. */
static gboolean stream_pump(Stream *stream, DWORD *code)
{
    while (!stream->done && !stream->pending) {
        DWORD amount = 0;
        BOOL ok;
        if (stream->writer) {
            if (stream->remaining == 0) { CloseHandle(stream->handle); stream->handle = NULL; stream->done = TRUE; break; }
            ok = WriteFile(stream->handle, stream->source, (DWORD)MIN(stream->remaining, (gsize)0x7FFFF000u), NULL, &stream->overlapped);
        } else {
            ok = ReadFile(stream->handle, stream->buffer, sizeof stream->buffer, NULL, &stream->overlapped);
        }
        DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error == ERROR_IO_PENDING) { stream->pending = TRUE; break; }
        if (ok) {
            if (!GetOverlappedResult(stream->handle, &stream->overlapped, &amount, FALSE)) error = GetLastError();
            else { stream_take(stream, amount); continue; }
        }
        if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_HANDLE_EOF) {
            /* EOF (reader) or the child closed its stdin (writer): finished. */
            if (stream->writer) { CloseHandle(stream->handle); stream->handle = NULL; }
            stream->done = TRUE;
            break;
        }
        *code = error;
        return FALSE;
    }
    return TRUE;
}

static gboolean stream_complete(Stream *stream, DWORD *code)
{
    DWORD amount = 0;
    stream->pending = FALSE;
    if (GetOverlappedResult(stream->handle, &stream->overlapped, &amount, FALSE)) { stream_take(stream, amount); return TRUE; }
    DWORD error = GetLastError();
    if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_HANDLE_EOF) {
        if (stream->writer) { CloseHandle(stream->handle); stream->handle = NULL; }
        stream->done = TRUE;
        return TRUE;
    }
    *code = error;
    return FALSE;
}

gboolean radios_process_communicate(RadioSProcess *process, GBytes *stdin_bytes, gsize max_bytes, guint timeout_ms,
                                    GBytes **stdout_bytes, GBytes **stderr_bytes, gint *exit_code,
                                    GCancellable *cancellable, GError **error)
{
    g_return_val_if_fail(process != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (stdout_bytes) *stdout_bytes = NULL;
    if (stderr_bytes) *stderr_bytes = NULL;
    g_mutex_lock(&process->lock);
    gboolean busy = process->communicating;
    process->communicating = TRUE;
    HANDLE in = process->pipe_in, out = process->pipe_out, err = process->pipe_err;
    process->pipe_in = process->pipe_out = process->pipe_err = NULL;
    g_mutex_unlock(&process->lock);
    if (busy) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Süreçle iletişim zaten sürüyor");
        return FALSE;
    }
    if (stdin_bytes && g_bytes_get_size(stdin_bytes) > 0 && !in) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Sürecin standart girişi bu süreçte değil");
        close_handle(&out); close_handle(&err);
        g_mutex_lock(&process->lock); process->communicating = FALSE; g_mutex_unlock(&process->lock);
        return FALSE;
    }
    Stream streams[3];
    stream_init(&streams[0], in, TRUE, 0);
    stream_init(&streams[1], out, FALSE, max_bytes);
    stream_init(&streams[2], err, FALSE, max_bytes);
    if (in) {
        gsize size = 0;
        streams[0].source = stdin_bytes ? g_bytes_get_data(stdin_bytes, &size) : NULL;
        streams[0].remaining = size;
    }
    GPollFD cancel_poll = { 0, 0, 0 };
    gboolean cancel_ok = cancellable && g_cancellable_make_pollfd(cancellable, &cancel_poll);
    gint64 now = g_get_monotonic_time();
    gint64 deadline = timeout_ms == RADIOS_PROCESS_WAIT_INFINITE ? G_MAXINT64 : now + (gint64)timeout_ms * 1000;
    gint64 grace = G_MAXINT64;
    gboolean exited = WaitForSingleObject(process->process, 0) == WAIT_OBJECT_0;
    if (exited) grace = now + GRACE_AFTER_EXIT_MS * 1000;
    gboolean success = FALSE;
    DWORD failure = ERROR_SUCCESS;
    gint outcome = 0;  /* 0 running, 1 done, 2 timeout, 3 cancelled, 4 error */
    while (outcome == 0) {
        for (guint i = 0; i < 3 && outcome == 0; ++i)
            if (!stream_pump(&streams[i], &failure)) outcome = 4;
        if (outcome) break;
        gboolean streams_done = streams[0].done && streams[1].done && streams[2].done;
        if (streams_done && exited) { outcome = 1; break; }
        if (g_cancellable_is_cancelled(cancellable)) { outcome = 3; break; }
        HANDLE handles[5];
        gint owners[5];
        DWORD count = 0;
        if (!exited) { handles[count] = process->process; owners[count++] = -1; }
        for (guint i = 0; i < 3; ++i)
            if (streams[i].pending) { handles[count] = streams[i].overlapped.hEvent; owners[count++] = (gint)i; }
        if (cancel_ok) { handles[count] = (HANDLE)(gintptr)cancel_poll.fd; owners[count++] = -2; }
        now = g_get_monotonic_time();
        gint64 until = MIN(deadline, grace);
        if (now >= until) {
            outcome = deadline <= grace ? 2 : 1;   /* grace over: output of the exited child is complete */
            break;
        }
        gint64 wait_ms = (until - now + 999) / 1000;
        DWORD wait = WaitForMultipleObjects(count, handles, FALSE, (DWORD)MIN(wait_ms, (gint64)0x7FFFFFFF));
        if (wait == WAIT_TIMEOUT) continue;
        if (wait >= WAIT_OBJECT_0 + count) { failure = GetLastError(); outcome = 4; break; }
        gint owner = owners[wait - WAIT_OBJECT_0];
        if (owner == -1) { exited = TRUE; grace = g_get_monotonic_time() + GRACE_AFTER_EXIT_MS * 1000; }
        else if (owner == -2) outcome = 3;
        else if (!stream_complete(&streams[owner], &failure)) outcome = 4;
    }
    for (guint i = 0; i < 3; ++i) stream_cancel(&streams[i]);
    if (cancel_ok) g_cancellable_release_fd(cancellable);
    switch (outcome) {
    case 1: {
        DWORD code = 0;
        if (!GetExitCodeProcess(process->process, &code)) code = (DWORD)-1;
        if (exit_code) *exit_code = (gint)code;
        if (stdout_bytes) *stdout_bytes = g_byte_array_free_to_bytes(g_steal_pointer(&streams[1].data));
        if (stderr_bytes) *stderr_bytes = g_byte_array_free_to_bytes(g_steal_pointer(&streams[2].data));
        success = TRUE;
        break;
    }
    case 2:
        radios_process_terminate(process, 1);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Süreç %u ms içinde bitmedi ve sonlandırıldı", timeout_ms);
        break;
    case 3:
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Süreçle iletişim iptal edildi");
        break;
    default:
        win32_error(error, failure, "süreç borusu");
        break;
    }
    for (guint i = 0; i < 3; ++i) {
        HANDLE h = streams[i].handle;
        streams[i].handle = h;
        stream_clear(&streams[i]);
        if (h) CloseHandle(h);
    }
    g_mutex_lock(&process->lock);
    process->communicating = FALSE;
    g_mutex_unlock(&process->lock);
    return success;
}

typedef struct {
    RadioSProcess *process;
    GBytes *stdin_bytes, *stdout_bytes, *stderr_bytes;
    gsize max_bytes;
    guint timeout_ms;
    gint exit_code;
} CommunicateTask;

static void communicate_task_free(gpointer data)
{
    CommunicateTask *task = data;
    if (task->stdin_bytes) g_bytes_unref(task->stdin_bytes);
    if (task->stdout_bytes) g_bytes_unref(task->stdout_bytes);
    if (task->stderr_bytes) g_bytes_unref(task->stderr_bytes);
    process_unref(task->process);
    g_free(task);
}

static void communicate_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
    (void)source;
    CommunicateTask *work = data;
    GError *error = NULL;
    if (radios_process_communicate(work->process, work->stdin_bytes, work->max_bytes, work->timeout_ms,
                                   &work->stdout_bytes, &work->stderr_bytes, &work->exit_code, cancellable, &error))
        g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, error);
}

void radios_process_communicate_async(RadioSProcess *process, GBytes *stdin_bytes, gsize max_bytes, guint timeout_ms,
                                      GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    g_return_if_fail(process != NULL);
    CommunicateTask *work = g_new0(CommunicateTask, 1);
    work->process = process_ref(process);
    work->stdin_bytes = stdin_bytes ? g_bytes_ref(stdin_bytes) : NULL;
    work->max_bytes = max_bytes;
    work->timeout_ms = timeout_ms;
    static gint source_tag;
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, &source_tag);
    g_task_set_task_data(task, work, communicate_task_free);
    g_task_set_return_on_cancel(task, FALSE);
    g_task_run_in_thread(task, communicate_thread);
    g_object_unref(task);
}

gboolean radios_process_communicate_finish(RadioSProcess *process, GAsyncResult *result, GBytes **stdout_bytes,
                                           GBytes **stderr_bytes, gint *exit_code, GError **error)
{
    g_return_val_if_fail(process != NULL, FALSE);
    g_return_val_if_fail(g_task_is_valid(result, NULL), FALSE);
    GTask *task = G_TASK(result);
    CommunicateTask *work = g_task_get_task_data(task);
    if (stdout_bytes) *stdout_bytes = NULL;
    if (stderr_bytes) *stderr_bytes = NULL;
    if (!g_task_propagate_boolean(task, error)) return FALSE;
    if (stdout_bytes) *stdout_bytes = work->stdout_bytes ? g_bytes_ref(work->stdout_bytes) : NULL;
    if (stderr_bytes) *stderr_bytes = work->stderr_bytes ? g_bytes_ref(work->stderr_bytes) : NULL;
    if (exit_code) *exit_code = work->exit_code;
    return TRUE;
}

gboolean radios_process_spawn_detached(const gchar *const *argv, const gchar *working_directory, GError **error)
{
    RadioSProcess *process = radios_process_spawn(argv, working_directory, NULL, RADIOS_SPAWN_DETACHED,
                                                  NULL, NULL, NULL, error);
    if (!process) return FALSE;
    radios_process_free(process);
    return TRUE;
}

/* ------------------------------------------------------------------ console, log sink, relaunch */

static gboolean console_attached;

gboolean radios_platform_stdio_is_usable(gint fd)
{
    if (fd < 0 || fd > 2) return FALSE;
    intptr_t raw = _get_osfhandle(fd);   /* -1 closed, -2 no console (GUI process) */
    return raw > 0 && usable_handle((HANDLE)raw);
}

static void reopen_stream(FILE *stream, gint fd, DWORD std_id, const WCHAR *device, const WCHAR *mode)
{
    FILE *reopened = _wfreopen(device, mode, stream);
    if (!reopened) return;
    if (_fileno(stream) != fd && _fileno(stream) >= 0) _dup2(_fileno(stream), fd);
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(stream));
    if (usable_handle(h)) SetStdHandle(std_id, h);
    setvbuf(stream, NULL, _IONBF, 0);
}

gboolean radios_platform_attach_parent_console(void)
{
    radios_platform_init();
    gboolean out_ok = radios_platform_stdio_is_usable(1), err_ok = radios_platform_stdio_is_usable(2);
    if (out_ok && err_ok) return FALSE;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return FALSE;
    console_attached = TRUE;
    if (!out_ok) reopen_stream(stdout, 1, STD_OUTPUT_HANDLE, L"CONOUT$", L"w");
    if (!err_ok) reopen_stream(stderr, 2, STD_ERROR_HANDLE, L"CONOUT$", L"w");
    return TRUE;
}

/* redirect[0]/[1]: stdout/stderr.  Decided by the caller before h exists, since
 * the new fd may take the number of a closed std fd. */
static gboolean redirect_to_handle(HANDLE h, const gboolean redirect[2])
{
    gint fd = _open_osfhandle((intptr_t)h, _O_APPEND | _O_BINARY | _O_WRONLY);
    if (fd < 0) { CloseHandle(h); return FALSE; }
    FILE *streams[2] = { stdout, stderr };
    const gint fds[2] = { 1, 2 };
    const DWORD ids[2] = { STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
    gboolean fd_in_use = FALSE;   /* fd may itself be 1 or 2 when those were closed */
    for (guint i = 0; i < 2; ++i) {
        if (!redirect[i]) continue;
        fflush(streams[i]);
        if (_fileno(streams[i]) < 0) _wfreopen(L"NUL", L"w", streams[i]);
        gint target = _fileno(streams[i]) >= 0 ? _fileno(streams[i]) : fds[i];
        gint wanted[2] = { target, fds[i] };
        for (guint j = 0; j < (target != fds[i] ? 2u : 1u); ++j) {
            if (wanted[j] == fd) fd_in_use = TRUE;
            else _dup2(fd, wanted[j]);
        }
        clearerr(streams[i]);
        SetStdHandle(ids[i], (HANDLE)_get_osfhandle(target));
        setvbuf(streams[i], NULL, _IONBF, 0);
    }
    if (!fd_in_use) close(fd);
    return TRUE;
}

static HANDLE open_log(const gchar *log_directory, GError **error)
{
    if (radios_fs_mkdir_with_parents(log_directory, 0755) != 0) {
        radios_platform_set_io_error(error, errno, "%s", log_directory);
        return NULL;
    }
    gchar *path = g_build_filename(log_directory, "radios-desktop.log", NULL);
    RadioSFileInfo info;
    if (radios_fs_stat(path, FALSE, &info) == 0 && info.type == RADIOS_FILE_TYPE_REGULAR && info.size > 5 * 1024 * 1024) {
        gchar *previous = g_strconcat(path, ".1", NULL);
        (void)radios_fs_rename(path, previous);   /* best effort: another instance may hold it */
        g_free(previous);
    }
    gchar *native = g_strdelimit(g_strdup(path), "/", '\\');
    WCHAR *wide = to_wide(native);
    g_free(native);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
    HANDLE h = wide ? CreateFileW(wide, FILE_APPEND_DATA | SYNCHRONIZE | READ_CONTROL | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, NULL)
                    : INVALID_HANDLE_VALUE;
    DWORD code = GetLastError();
    g_free(wide);
    if (h == INVALID_HANDLE_VALUE) { win32_error(error, code, path); g_free(path); return NULL; }
    g_free(path);
    return h;
}

gboolean radios_platform_redirect_stdio_to_log(const gchar *log_directory, GError **error)
{
    g_return_val_if_fail(log_directory != NULL, FALSE);
    radios_platform_init();
    const gboolean redirect[2] = { !radios_platform_stdio_is_usable(1), !radios_platform_stdio_is_usable(2) };
    if (!redirect[0] && !redirect[1]) return TRUE;
    HANDLE h = open_log(log_directory, error);
    if (!h) return FALSE;
    if (!redirect_to_handle(h, redirect)) return radios_platform_set_io_error(error, EMFILE, "%s", log_directory);
    return TRUE;
}

void radios_platform_detach_console(const gchar *log_directory)
{
    if (!console_attached) return;
    const gchar *keep = g_getenv("RADIOS_KEEP_CONSOLE");
    if (keep && g_str_equal(keep, "1")) return;
    fflush(stdout);
    fflush(stderr);
    FreeConsole();
    console_attached = FALSE;
    HANDLE h = log_directory ? open_log(log_directory, NULL) : NULL;
    if (!h) h = open_null(GENERIC_WRITE);
    const gboolean both[2] = { TRUE, TRUE };
    if (h && h != INVALID_HANDLE_VALUE) redirect_to_handle(h, both);
}

gboolean radios_platform_relaunch_wait(guint timeout_ms)
{
    radios_platform_init();
    const gchar *value = g_getenv(RELAUNCH_VARIABLE);
    if (!value) return FALSE;
    gchar *copy = g_strdup(value);
    g_unsetenv(RELAUNCH_VARIABLE);
    gchar *end = NULL;
    guint64 number = g_ascii_strtoull(copy, &end, 10);
    gboolean parsed = end && *end == '\0' && end != copy && number != 0;
    g_free(copy);
    if (!parsed) return FALSE;
    HANDLE parent = (HANDLE)(guintptr)number;
    if (GetProcessId(parent) == 0) return FALSE;   /* not a process handle: never touch it */
    /* Only wait for (and close) a handle of a process running this executable. */
    DWORD capacity = 32768, length = capacity;
    WCHAR *image = g_new(WCHAR, capacity);
    gchar *own = radios_process_executable_path(NULL);
    gboolean same = FALSE;
    if (own && QueryFullProcessImageNameW(parent, 0, image, &length)) {
        gchar *theirs = from_wide(image, (glong)length);
        same = theirs && radios_path_equal(theirs, own);
        g_free(theirs);
    } else if (own && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) {
        /* The old instance already exited (its image name may be gone): the
         * handle is a process handle we inherited at start-up, nothing to wait for. */
        same = TRUE;
    }
    g_free(own);
    g_free(image);
    if (!same) return FALSE;
    DWORD wait = WaitForSingleObject(parent, timeout_ms == RADIOS_PROCESS_WAIT_INFINITE ? INFINITE : timeout_ms);
    CloseHandle(parent);
    return wait == WAIT_OBJECT_0;
}

/* ------------------------------------------------------------------ crash dumps */

typedef BOOL (WINAPI *MiniDumpWriteDumpFunc)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                             PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

#define CRASH_KEEP 10

static struct {
    WCHAR *directory;
    WCHAR *path;           /* preallocated: directory + file name */
    gsize path_capacity;
    MiniDumpWriteDumpFunc write_dump;
    HANDLE request, done, thread;
    EXCEPTION_POINTERS *pointers;
    DWORD thread_id;
    LPTOP_LEVEL_EXCEPTION_FILTER previous;
    volatile LONG busy;
    gboolean installed;
} crash;

static DWORD WINAPI crash_writer(LPVOID unused)
{
    (void)unused;
    WaitForSingleObject(crash.request, INFINITE);
    SYSTEMTIME now;
    GetSystemTime(&now);
    _snwprintf(crash.path, crash.path_capacity, L"%ls\\RadioS-%04u%02u%02uT%02u%02u%02uZ-%lu.dmp", crash.directory,
               (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay, (unsigned)now.wHour, (unsigned)now.wMinute,
               (unsigned)now.wSecond, (unsigned long)GetCurrentProcessId());
    crash.path[crash.path_capacity - 1] = L'\0';
    SECURITY_ATTRIBUTES sa = { sizeof sa, owner_only_descriptor(), FALSE };
    CreateDirectoryW(crash.directory, &sa);
    HANDLE file = CreateFileW(crash.path, GENERIC_WRITE, 0, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION information = { crash.thread_id, crash.pointers, FALSE };
        MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithIndirectlyReferencedMemory |
                                             MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        crash.write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, crash.pointers ? &information : NULL, NULL, NULL);
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    SetEvent(crash.done);
    return 0;
}

static void crash_dump(EXCEPTION_POINTERS *pointers)
{
    if (!crash.installed || InterlockedExchange(&crash.busy, 1)) return;
    crash.pointers = pointers;
    crash.thread_id = GetCurrentThreadId();
    SetEvent(crash.request);
    WaitForSingleObject(crash.done, 120000);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *pointers)
{
    crash_dump(pointers);
    return crash.previous ? crash.previous(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

static void crash_abort(int signal_number)
{
    CONTEXT context;
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record;
    memset(&record, 0, sizeof record);
    record.ExceptionCode = 0x40000015;   /* STATUS_FATAL_APP_EXIT */
#if defined(__x86_64__) || defined(_M_X64)
    record.ExceptionAddress = (PVOID)context.Rip;
#endif
    EXCEPTION_POINTERS pointers = { &record, &context };
    crash_dump(&pointers);
    signal(signal_number, SIG_DFL);
}

static gint compare_names(gconstpointer a, gconstpointer b)
{
    return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static void crash_retention(const gchar *directory)
{
    gint fd = radios_fs_open(directory, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY, 0);
    if (fd < 0) return;
    GPtrArray *names = radios_fd_list_names(fd, NULL);
    if (names) {
        GPtrArray *dumps = g_ptr_array_new();
        for (guint i = 0; i < names->len; ++i) {
            const gchar *name = names->pdata[i];
            if (g_str_has_prefix(name, "RadioS-") && g_str_has_suffix(name, ".dmp")) g_ptr_array_add(dumps, (gpointer)name);
        }
        g_ptr_array_sort(dumps, compare_names);   /* the UTC stamp sorts chronologically */
        for (guint i = 0; i + (CRASH_KEEP - 1) < dumps->len; ++i) (void)radios_fs_unlinkat(fd, dumps->pdata[i], 0);
        g_ptr_array_unref(dumps);
        g_ptr_array_unref(names);
    }
    close(fd);
}

gboolean radios_platform_install_crash_handler(const gchar *log_root, GError **error)
{
    g_return_val_if_fail(log_root != NULL, FALSE);
    radios_platform_init();
    if (crash.installed) return TRUE;
    gchar *directory = g_build_filename(log_root, "crash", NULL);
    gchar *normal = radios_path_normalize(directory);
    g_free(directory);
    if (!normal || radios_fs_mkdir_with_parents(normal, 0700) != 0) {
        gboolean result = radios_platform_set_io_error(error, normal ? errno : EINVAL, "%s", normal ? normal : log_root);
        g_free(normal);
        return result;
    }
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    crash.write_dump = dbghelp ? (MiniDumpWriteDumpFunc)(void (*)(void))GetProcAddress(dbghelp, "MiniDumpWriteDump") : NULL;
    if (!crash.write_dump) {
        DWORD code = GetLastError();
        g_free(normal);
        return win32_error(error, code, "dbghelp.dll MiniDumpWriteDump");
    }
    crash_retention(normal);
    crash.directory = to_wide(normal);
    crash.path_capacity = wcslen(crash.directory) + 64;
    crash.path = g_new0(WCHAR, crash.path_capacity);
    crash.request = CreateEventW(NULL, FALSE, FALSE, NULL);
    crash.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    crash.thread = CreateThread(NULL, 256 * 1024, crash_writer, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    g_free(normal);
    if (!crash.request || !crash.done || !crash.thread) return win32_error(error, GetLastError(), "crash dump thread");
    crash.installed = TRUE;
    crash.previous = SetUnhandledExceptionFilter(crash_filter);
    signal(SIGABRT, crash_abort);
    return TRUE;
}
