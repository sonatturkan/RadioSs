#include "radios/library_analysis.h"
#include "radios/local_ini.h"
#include "radios/platform.h"

#include <stdarg.h>

#define SETTINGS_FILE "library-analysis.ini"
#define SETTINGS_GROUP "analysis"

static gboolean fail(GError **error, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
static gboolean fail(GError **error, const gchar *format, ...)
{
    va_list args; va_start(args, format);
    gchar *message = g_strdup_vprintf(format, args);
    va_end(args);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    g_free(message);
    return FALSE;
}

guint radios_library_analysis_default_workers(void)
{
    return CLAMP(g_get_num_processors() / 2, 1u, 4u);
}

void radios_library_analysis_settings_init(RadioSLibraryAnalysisSettings *settings)
{
    g_return_if_fail(settings);
    settings->workers = radios_library_analysis_default_workers();
    settings->auto_scan = TRUE;
    settings->settle_seconds = 20;
    settings->interval_minutes = 10;
}

gboolean radios_library_analysis_settings_validate(const RadioSLibraryAnalysisSettings *settings, GError **error)
{
    g_return_val_if_fail(settings, FALSE);
    if (settings->workers < 1 || settings->workers > RADIOS_LIBRARY_ANALYSIS_MAX_WORKERS)
        return fail(error, "Paralel analiz sayısı 1–%u olmalı.", RADIOS_LIBRARY_ANALYSIS_MAX_WORKERS);
    if (settings->settle_seconds < 3 || settings->settle_seconds > 600) return fail(error, "İçe aktarma sonrası bekleme 3–600 saniye olmalı.");
    if (settings->interval_minutes < 1 || settings->interval_minutes > 1440) return fail(error, "Tarama aralığı 1–1440 dakika olmalı.");
    return TRUE;
}

static gboolean read_integer(GKeyFile *file, const gchar *key, guint *value, GError **error)
{
    if (!g_key_file_has_key(file, SETTINGS_GROUP, key, NULL)) return TRUE;
    gint number = g_key_file_get_integer(file, SETTINGS_GROUP, key, error);
    if (error && *error) return FALSE;
    *value = number < 0 ? 0 : (guint)number; /* 0 fails validation below */
    return TRUE;
}

gboolean radios_library_analysis_settings_load(const gchar *project_root, RadioSLibraryAnalysisSettings *out, GError **error)
{
    g_return_val_if_fail(project_root && out, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    gchar *contents = NULL;
    if (!radios_local_ini_read(project_root, SETTINGS_FILE, 4096, &contents, error)) return FALSE;
    RadioSLibraryAnalysisSettings proposed; radios_library_analysis_settings_init(&proposed);
    GError *local = NULL; gboolean ok = TRUE;
    if (contents) {
        GKeyFile *file = g_key_file_new();
        ok = g_key_file_load_from_data(file, contents, -1, G_KEY_FILE_NONE, &local);
        if (ok) {
            gint version = g_key_file_get_integer(file, SETTINGS_GROUP, "version", &local);
            if (local) ok = FALSE; else if (version != 1) ok = fail(&local, "library-analysis.ini version=1 olmalı.");
        }
        if (ok) ok = read_integer(file, "workers", &proposed.workers, &local);
        if (ok) ok = read_integer(file, "settle_seconds", &proposed.settle_seconds, &local);
        if (ok) ok = read_integer(file, "interval_minutes", &proposed.interval_minutes, &local);
        if (ok && g_key_file_has_key(file, SETTINGS_GROUP, "auto", NULL)) {
            proposed.auto_scan = g_key_file_get_boolean(file, SETTINGS_GROUP, "auto", &local);
            if (local) ok = FALSE;
        }
        g_key_file_unref(file);
        if (ok && !radios_library_analysis_settings_validate(&proposed, &local)) ok = FALSE;
    }
    g_free(contents);
    if (!ok) {
        g_propagate_prefixed_error(error, local, "Kütüphane analizi ayarları (library-analysis.ini) okunamadı: ");
        return FALSE;
    }
    *out = proposed;
    return TRUE;
}

gboolean radios_library_analysis_settings_save(const gchar *project_root, const RadioSLibraryAnalysisSettings *settings, GError **error)
{
    g_return_val_if_fail(project_root && settings, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (!radios_library_analysis_settings_validate(settings, error)) return FALSE;
    GKeyFile *file = g_key_file_new();
    g_key_file_set_integer(file, SETTINGS_GROUP, "version", 1);
    g_key_file_set_integer(file, SETTINGS_GROUP, "workers", (gint)settings->workers);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "auto", settings->auto_scan);
    g_key_file_set_integer(file, SETTINGS_GROUP, "settle_seconds", (gint)settings->settle_seconds);
    g_key_file_set_integer(file, SETTINGS_GROUP, "interval_minutes", (gint)settings->interval_minutes);
    gchar *data = g_key_file_to_data(file, NULL, NULL);
    g_key_file_unref(file);
    gboolean saved = radios_local_ini_write(project_root, SETTINGS_FILE, data, error);
    g_free(data);
    return saved;
}

void radios_library_analysis_settings_current(const gchar *project_root, RadioSLibraryAnalysisSettings *out)
{
    g_return_if_fail(out);
    GError *error = NULL;
    if (!project_root || !radios_library_analysis_settings_load(project_root, out, &error)) radios_library_analysis_settings_init(out);
    g_clear_error(&error);
}

/* ------------------------------------------------------------------ runner */

typedef struct {
    gint next;
    guint count;
    GCancellable *cancel;
    RadioSLibraryAnalysisTask task;
    gpointer user;
    gboolean lower_priority;
} RunState;

static void run_indices(RunState *state)
{
    for (;;) {
        if (state->cancel && g_cancellable_is_cancelled(state->cancel)) return;
        gint index = g_atomic_int_add(&state->next, 1);
        if (index < 0 || (guint)index >= state->count) return;
        state->task((guint)index, state->user);
    }
}

static gpointer run_thread(gpointer data)
{
    RunState *state = data;
    /* Analysis must never delay audio: workers run below normal priority
     * (per thread, THREAD_PRIORITY_BELOW_NORMAL). */
    if (state->lower_priority) radios_thread_lower_priority();
    run_indices(state);
    return NULL;
}

void radios_library_analysis_run(guint count, guint workers, GCancellable *cancel, RadioSLibraryAnalysisTask task, gpointer user)
{
    if (!count || !task) return;
    workers = CLAMP(workers, 1u, RADIOS_LIBRARY_ANALYSIS_MAX_WORKERS);
    if (workers > count) workers = count;
    RunState state = {.next = 0, .count = count, .cancel = cancel, .task = task, .user = user, .lower_priority = workers > 1};
    GThread *threads[RADIOS_LIBRARY_ANALYSIS_MAX_WORKERS] = {NULL};
    guint started = 0;
    if (workers > 1) {
        for (guint i = 0; i < workers; ++i) {
            threads[i] = g_thread_try_new("radios-analysis", run_thread, &state, NULL);
            if (threads[i]) ++started;
        }
    }
    /* One worker, or no thread could start: run on the caller. */
    if (!started) run_indices(&state);
    for (guint i = 0; i < workers; ++i) if (threads[i]) g_thread_join(threads[i]);
}

/* ---------------------------------------------------------------- activity */

static gint activity_scans, activity_done, activity_total, activity_workers;

void radios_library_analysis_activity_begin(guint workers)
{
    /* A race with the first scan's reset can only blur a progress bar. */
    if (g_atomic_int_add(&activity_scans, 1) == 0) {
        g_atomic_int_set(&activity_done, 0); g_atomic_int_set(&activity_total, 0);
    }
    g_atomic_int_set(&activity_workers, (gint)workers);
}

void radios_library_analysis_activity_add_total(guint count)
{ g_atomic_int_add(&activity_total, (gint)MIN(count, (guint)G_MAXINT / 2)); }

void radios_library_analysis_activity_step(void)
{ g_atomic_int_inc(&activity_done); }

void radios_library_analysis_activity_end(void)
{
    /* Never below zero, even when two scans end at the same time: a separate
     * check and decrement could both pass at 1 and leave -1, after which the
     * activity of every later scan would be reported as idle. */
    for (;;) {
        gint scans = g_atomic_int_get(&activity_scans);
        if (scans <= 0 || g_atomic_int_compare_and_exchange(&activity_scans, scans, scans - 1)) return;
    }
}

gboolean radios_library_analysis_activity(guint *done, guint *total, guint *workers)
{
    if (done) *done = (guint)MAX(0, g_atomic_int_get(&activity_done));
    if (total) *total = (guint)MAX(0, g_atomic_int_get(&activity_total));
    if (workers) *workers = (guint)MAX(0, g_atomic_int_get(&activity_workers));
    return g_atomic_int_get(&activity_scans) > 0;
}
