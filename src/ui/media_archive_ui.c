#include "radios/media_archive_ui.h"
#include "radios/media_archive.h"
#include "radios/storage_locations.h"
#include "radios/media_ui.h"
#include "radios/data.h"
#include "radios/reset_ui.h"
#include "radios/platform.h"
#include <string.h>

#define RUNNER_KEY "radios-media-archive-runner"
#define PAGE_KEY "radios-media-archive-page"
#define INDEX_FILE "media-archive-index.json"
#define WATCH_TICK_SECONDS 30
#define RETRY_SECONDS 3

static const gchar *const kinds[] = {"music", "advertisement", "jingle", "sweeper", "news", "program",
                                     "announcement", "effect", "voice_track", "other", NULL};
static const gchar *const kind_labels[] = {"Müzik", "Reklam", "Jingle", "Sweeper", "Haber", "Program",
                                           "Duyuru", "Efekt", "Ses kaydı", "Diğer", NULL};

/* Storage roles (config/storage.ini): widget ids, chooser titles and what a
 * change means. The data service reads the same file (tools/radios_database.py). */
static const gchar *const storage_ids[RADIOS_STORAGE_ROLE_COUNT] = {"database", "recordings", "backups"};
static const gchar *const storage_titles[RADIOS_STORAGE_ROLE_COUNT] = {
    "Veritabanı klasörü seç", "Kayıt klasörü seç", "Yedek klasörü seç"};
static const gchar *const storage_chosen_texts[RADIOS_STORAGE_ROLE_COUNT] = {
    "Veritabanı ve yönetilen medya yeniden başlatmada şuraya taşınacak: %s (RadioS işareti bırakıldı). "
    "Yeni klasörde veritabanı yoksa açılışta son kullanılan kopya oraya kopyalanır; eski kopya yerinde kalır.",
    "WAV kayıtları artık şuraya yazılacak: %s (RadioS işareti bırakıldı). Değişiklik RadioS yeniden başlatılınca geçerli olur.",
    "Veritabanı yedekleri artık şuraya yazılacak: %s (RadioS işareti bırakıldı). Veri servisi sonraki yedekten itibaren bu klasörü kullanır."};
static const gchar *const storage_default_texts[RADIOS_STORAGE_ROLE_COUNT] = {
    "Veritabanı yeniden başlatmada varsayılan klasöre (radios.ini › database_file) döner: orada kopya varsa o açılır, "
    "yoksa son kullanılan klasörden kopyalanır. Dış klasördeki kopya silinmez.",
    "WAV kayıtları yeniden varsayılan klasöre (radios.ini › recording_root) yazılacak. Değişiklik RadioS yeniden başlatılınca geçerli olur.",
    "Veritabanı yedekleri yeniden varsayılan klasöre (radios.ini › backup_root) yazılacak."};

/* New files of one archive subfolder; name NULL means no playlist. */
typedef struct { gchar *name; GPtrArray *paths; } ArchiveGroup;

/* One runner per application window: the Preferences page and the timed
 * watch share it, so a manual and an automatic run never overlap. */
typedef struct {
    RadioSViewContext *ctx;
    RadioSMediaArchiveSettings settings;
    gchar *settings_error;      /* media-archive.ini could not be read */
    GCancellable *cancel;       /* scan in progress */
    gboolean importing, manual, scanned;
    guint generation;           /* bumps per run; late callbacks are dropped */
    GPtrArray *groups;          /* ArchiveGroup* waiting for import */
    guint group_index, retry_timer, watch_timer;
    gchar *pending_playlist; gint64 pending_revision;
    guint found, fresh, duplicates, imported, failed, folders_missing, unreadable, library_records;
    guint64 bytes;
    gboolean truncated;
    gchar *status, *last_error;
    gint64 last_run_us;         /* monotonic end of the last run */
    GtkWidget *page;            /* the open Preferences page, if any */
} ArchiveRunner;

typedef struct {
    RadioSViewContext *ctx;
    ArchiveRunner *runner;
    GtkWidget *page, *folders, *add, *remove, *kind, *folder_playlists, *watch, *minutes;
    GtkWidget *scan, *stop, *progress, *summary, *settings_status;
    GtkWidget *storage_path[RADIOS_STORAGE_ROLE_COUNT], *storage_size[RADIOS_STORAGE_ROLE_COUNT];
    GtkWidget *storage_choose[RADIOS_STORAGE_ROLE_COUNT], *storage_default[RADIOS_STORAGE_ROLE_COUNT];
    GtkWidget *storage_status, *storage_notice, *restart;
    guint timer;
    gboolean updating, disposed;
} ArchivePage;

static void pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static GtkWidget *styled(GtkWidget *widget, const gchar *style)
{ gtk_style_context_add_class(gtk_widget_get_style_context(widget), style); return widget; }
static GtkWidget *named(GtkWidget *widget, const gchar *name) { gtk_widget_set_name(widget, name); return widget; }
static GtkWidget *text(const gchar *value, const gchar *style)
{
    GtkWidget *label = gtk_label_new(value); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE); gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 90);
    return style ? styled(label, style) : label;
}
static GtkWidget *section_new(const gchar *title)
{
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); styled(section, "preferences-section");
    pack(section, text(title, "preferences-heading"), FALSE);
    return section;
}
static void group_free(gpointer value)
{ ArchiveGroup *group = value; g_free(group->name); g_ptr_array_unref(group->paths); g_free(group); }
static void page_refresh(ArchivePage *page);

/* ------------------------------------------------------------------ runner */

static void runner_free(gpointer value)
{
    ArchiveRunner *runner = value;
    if (runner->cancel) g_cancellable_cancel(runner->cancel);
    g_clear_object(&runner->cancel);
    if (runner->retry_timer) g_source_remove(runner->retry_timer);
    if (runner->watch_timer) g_source_remove(runner->watch_timer);
    g_clear_pointer(&runner->groups, g_ptr_array_unref);
    radios_media_archive_settings_clear(&runner->settings);
    g_free(runner->settings_error); g_free(runner->pending_playlist); g_free(runner->status); g_free(runner->last_error);
    g_free(runner);
}

static void runner_load_settings(ArchiveRunner *runner)
{
    GError *error = NULL;
    g_clear_pointer(&runner->settings_error, g_free);
    const gchar *root = runner->ctx->config ? runner->ctx->config->project_root : NULL;
    if (!root) { runner->settings_error = g_strdup("Proje kökü bilinmiyor; medya arşivi ayarları okunamadı."); return; }
    if (!radios_media_archive_settings_load(root, &runner->settings, &error)) {
        runner->settings_error = g_strdup(error ? error->message : "Medya arşivi ayarları okunamadı.");
        g_clear_error(&error);
    }
}

static ArchiveRunner *runner_of(RadioSViewContext *ctx)
{
    if (!ctx || !GTK_IS_WINDOW(ctx->parent)) return NULL;
    ArchiveRunner *runner = g_object_get_data(G_OBJECT(ctx->parent), RUNNER_KEY);
    if (runner) return runner;
    runner = g_new0(ArchiveRunner, 1); runner->ctx = ctx;
    radios_media_archive_settings_init(&runner->settings);
    runner_load_settings(runner);
    runner->last_run_us = g_get_monotonic_time();
    runner->status = g_strdup("Arşiv henüz taranmadı.");
    g_object_set_data_full(G_OBJECT(ctx->parent), RUNNER_KEY, runner, runner_free);
    return runner;
}

static gboolean runner_busy(const ArchiveRunner *runner) { return runner->cancel != NULL || runner->importing; }

static void runner_status(ArchiveRunner *runner, const gchar *message)
{
    g_free(runner->status); runner->status = g_strdup(message);
    if (runner->page) page_refresh(g_object_get_data(G_OBJECT(runner->page), PAGE_KEY));
}

static void runner_notify(ArchiveRunner *runner, const gchar *message)
{ if (runner->ctx->notify) runner->ctx->notify(runner->ctx->notify_data, message); }

static void run_finish(ArchiveRunner *runner, const gchar *failure)
{
    gboolean ran_scan = runner->scanned;
    runner->importing = FALSE;
    g_clear_object(&runner->cancel);
    ++runner->generation;
    if (runner->retry_timer) { g_source_remove(runner->retry_timer); runner->retry_timer = 0; }
    g_clear_pointer(&runner->groups, g_ptr_array_unref);
    g_clear_pointer(&runner->pending_playlist, g_free);
    runner->last_run_us = g_get_monotonic_time();
    g_free(runner->last_error); runner->last_error = g_strdup(failure);
    GString *summary = g_string_new(NULL);
    if (failure) g_string_append(summary, failure);
    if (ran_scan) {
        gchar *size = radios_storage_format_bytes((gint64)runner->bytes);
        if (summary->len) g_string_append_c(summary, ' ');
        g_string_append_printf(summary, "Arşiv: %u ses dosyası (%s) · %u yeni · %u içe aktarıldı · %u doğrulanamadı · %u kopya atlandı · kütüphanede %u kayıt.",
            runner->found, size, runner->fresh, runner->imported, runner->failed, runner->duplicates, runner->library_records);
        g_free(size);
        if (runner->folders_missing) g_string_append_printf(summary, " %u arşiv klasörü okunamadı.", runner->folders_missing);
        if (runner->unreadable) g_string_append_printf(summary, " %u dosya okunamadı.", runner->unreadable);
        if (runner->truncated) g_string_append_printf(summary, " Dosya sınırına (%u) ulaşıldı; kalanlar sonraki taramada.", RADIOS_MEDIA_ARCHIVE_MAX_FILES);
    }
    runner_status(runner, summary->str);
    if (runner->manual || runner->imported || runner->failed || failure) {
        gchar *message = g_strdup_printf("Medya arşivi: %s", summary->str);
        runner_notify(runner, message); g_free(message);
    }
    g_string_free(summary, TRUE);
}

/* ---- scan: worker thread ---- */

typedef struct {
    GWeakRef parent; guint generation;
    gchar **folders; gchar *index_path; RadioSDataClient *client; gboolean folder_playlists;
    GPtrArray *files, *fresh; GHashTable *library;
    RadioSMediaArchiveScanStats stats; guint records, duplicates;
} ScanTask;

static void scan_task_free(gpointer value)
{
    ScanTask *task = value;
    g_weak_ref_clear(&task->parent);
    g_strfreev(task->folders); g_free(task->index_path);
    if (task->client) radios_data_client_unref(task->client);
    g_clear_pointer(&task->fresh, g_ptr_array_unref);
    g_clear_pointer(&task->files, g_ptr_array_unref);
    g_clear_pointer(&task->library, g_hash_table_unref);
    g_free(task);
}

typedef struct { GWeakRef parent; guint generation, done, total; gchar *name; } Progress;

static gboolean progress_idle(gpointer value)
{
    Progress *progress = value;
    GtkWidget *parent = g_weak_ref_get(&progress->parent);
    ArchiveRunner *runner = parent ? g_object_get_data(G_OBJECT(parent), RUNNER_KEY) : NULL;
    if (runner && runner->generation == progress->generation && runner->cancel) {
        gchar *message = progress->total ?
            g_strdup_printf("Arşiv taranıyor · %u/%u · %s", progress->done, progress->total, progress->name) :
            g_strdup("Arşiv klasörleri okunuyor…");
        runner_status(runner, message); g_free(message);
    }
    g_clear_object(&parent); g_weak_ref_clear(&progress->parent); g_free(progress->name); g_free(progress);
    return G_SOURCE_REMOVE;
}

/* Scanning thread: a status line every 25 files and at both ends. */
static gboolean scan_progress(guint done, guint total, const gchar *path, gpointer user)
{
    ScanTask *task = user;
    if (done != 0 && done != total && done % 25) return TRUE;
    Progress *progress = g_new0(Progress, 1);
    GtkWidget *parent = g_weak_ref_get(&task->parent);
    g_weak_ref_init(&progress->parent, parent); g_clear_object(&parent);
    progress->generation = task->generation; progress->done = done; progress->total = total;
    progress->name = g_path_get_basename(path && *path ? path : "");
    g_idle_add(progress_idle, progress);
    return TRUE;
}

static void scan_thread(GTask *task, gpointer source, gpointer value, GCancellable *cancel)
{
    (void)source; ScanTask *data = value; GError *error = NULL;
    data->files = radios_media_archive_scan((const gchar *const *)data->folders, data->index_path, cancel,
                                            scan_progress, data, &data->stats, &error);
    if (!data->files) {
        if (!error) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Arşiv taranamadı.");
        g_task_return_error(task, error); return;
    }
    data->library = radios_media_archive_library_digests(data->client, cancel, &data->records, &error);
    if (!data->library) {
        if (!error) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Medya kütüphanesi okunamadı.");
        g_task_return_error(task, error); return;
    }
    data->fresh = radios_media_archive_new_files(data->files, data->library, &data->duplicates);
    g_task_return_boolean(task, TRUE);
}

/* ---- import: main thread ---- */

static void group_next(ArchiveRunner *runner);

static void groups_build(ArchiveRunner *runner, GPtrArray *fresh, gboolean folder_playlists)
{
    g_clear_pointer(&runner->groups, g_ptr_array_unref);
    runner->groups = g_ptr_array_new_with_free_func(group_free);
    GHashTable *index = g_hash_table_new(g_str_hash, g_str_equal);
    ArchiveGroup *loose = NULL;
    for (guint i = 0; fresh && i < fresh->len; ++i) {
        const RadioSMediaArchiveFile *file = g_ptr_array_index(fresh, i);
        const gchar *name = folder_playlists && file->group && *file->group ? file->group : NULL;
        ArchiveGroup *group = name ? g_hash_table_lookup(index, name) : loose;
        if (!group) {
            group = g_new0(ArchiveGroup, 1); group->name = g_strdup(name);
            group->paths = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(runner->groups, group);
            if (name) g_hash_table_insert(index, group->name, group); else loose = group;
        }
        g_ptr_array_add(group->paths, g_strdup(file->path));
    }
    g_hash_table_unref(index);
}

static void import_done(RadioSViewContext *ctx, guint imported, guint failed, const gchar *last_error, gpointer user)
{
    ArchiveRunner *runner = runner_of(ctx);
    if (!runner || runner->generation != GPOINTER_TO_UINT(user) || !runner->importing) return;
    runner->imported += imported; runner->failed += failed;
    if (last_error && *last_error) { g_free(runner->last_error); runner->last_error = g_strdup(last_error); }
    ++runner->group_index;
    group_next(runner);
}

static gboolean retry_tick(gpointer value);

static void group_import(ArchiveRunner *runner, const gchar *playlist_id, gint64 revision)
{
    ArchiveGroup *group = g_ptr_array_index(runner->groups, runner->group_index);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < group->paths->len; ++i) g_ptr_array_add(paths, g_strdup(g_ptr_array_index(group->paths, i)));
    gchar *label = group->name ? g_strdup_printf("\"%s\" klasör playlisti", group->name) : NULL;
    gchar *message = g_strdup_printf("İçe aktarılıyor · %u/%u grup · %u dosya%s%s", runner->group_index + 1, runner->groups->len,
        group->paths->len, label ? " · " : "", label ? label : "");
    runner_status(runner, message); g_free(message);
    gboolean queued = radios_media_import_files(runner->ctx, paths, runner->settings.kind, playlist_id, revision, label,
                                                import_done, GUINT_TO_POINTER(runner->generation));
    g_free(label);
    if (queued) return;
    if (!radios_data_connected(runner->ctx)) { run_finish(runner, "Veri servisi bağlantısı kesildi; içe aktarma durdu."); return; }
    /* The library is busy with an operator action: wait and try again. */
    g_free(runner->pending_playlist); runner->pending_playlist = g_strdup(playlist_id); runner->pending_revision = revision;
    runner_status(runner, "Medya kütüphanesi meşgul (içe aktarma veya seçim sürüyor); arşiv içe aktarması bekliyor…");
    if (!runner->retry_timer) runner->retry_timer = g_timeout_add_seconds(RETRY_SECONDS, retry_tick, runner);
}

static gboolean retry_tick(gpointer value)
{
    ArchiveRunner *runner = value; runner->retry_timer = 0;
    if (!runner->importing || !runner->groups || runner->group_index >= runner->groups->len) return G_SOURCE_REMOVE;
    gchar *playlist = g_strdup(runner->pending_playlist); gint64 revision = runner->pending_revision;
    group_import(runner, playlist, revision); g_free(playlist);
    return G_SOURCE_REMOVE;
}

static void playlist_created(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer user)
{
    (void)owner; ArchiveRunner *runner = runner_of(ctx);
    if (!runner || runner->generation != GPOINTER_TO_UINT(user) || !runner->importing) return;
    JsonObject *playlist = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    const gchar *id = radios_json_string(playlist, "_id", ""); gint64 revision = radios_json_int(playlist, "_rev", 0);
    if (error || !g_uuid_string_is_valid(id) || revision <= 0) {
        gchar *message = g_strdup_printf("Klasör playlisti oluşturulamadı: %s", error ? error->message : "geçersiz yanıt");
        run_finish(runner, message); g_free(message); return;
    }
    group_import(runner, id, revision);
}

static void playlists_listed(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer user)
{
    (void)owner; ArchiveRunner *runner = runner_of(ctx);
    if (!runner || runner->generation != GPOINTER_TO_UINT(user) || !runner->importing) return;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        gchar *message = g_strdup_printf("Playlistler okunamadı: %s", error ? error->message : "geçersiz yanıt");
        run_finish(runner, message); g_free(message); return;
    }
    ArchiveGroup *group = g_ptr_array_index(runner->groups, runner->group_index);
    gchar *wanted = g_utf8_casefold(group->name, -1);
    JsonNode *rows = json_object_get_member(json_node_get_object(result), "playlists");
    const gchar *found = NULL; gint64 revision = 0;
    for (guint i = 0; rows && JSON_NODE_HOLDS_ARRAY(rows) && !found && i < json_array_get_length(json_node_get_array(rows)); ++i) {
        JsonObject *row = json_array_get_object_element(json_node_get_array(rows), i);
        gchar *name = g_utf8_casefold(radios_json_string(row, "name", ""), -1);
        if (g_str_equal(name, wanted) && g_uuid_string_is_valid(radios_json_string(row, "_id", ""))) {
            found = radios_json_string(row, "_id", ""); revision = radios_json_int(row, "_rev", 0);
        }
        g_free(name);
    }
    g_free(wanted);
    if (found) { group_import(runner, found, revision); return; }
    JsonNode *body = radios_data_body(ctx); JsonObject *object = json_node_get_object(body);
    gchar *trimmed = g_utf8_substring(group->name, 0, MIN(120, g_utf8_strlen(group->name, -1))), *key = g_uuid_string_random();
    json_object_set_string_member(object, "name", trimmed);
    json_object_set_string_member(object, "purpose", radios_media_archive_playlist_purpose(runner->settings.kind));
    json_object_set_string_member(object, "idempotency_key", key); g_free(trimmed); g_free(key);
    gboolean sent = radios_data_request(ctx, GTK_WIDGET(ctx->parent), "POST", "/v1/workflows/desktop-playlists/create", body,
                                        playlist_created, GUINT_TO_POINTER(runner->generation), NULL);
    json_node_free(body);
    if (!sent) run_finish(runner, "Veri servisi bağlı değil; klasör playlisti oluşturulmadı.");
}

static void group_next(ArchiveRunner *runner)
{
    if (!runner->importing) return;
    if (!runner->groups || runner->group_index >= runner->groups->len) { run_finish(runner, NULL); return; }
    ArchiveGroup *group = g_ptr_array_index(runner->groups, runner->group_index);
    if (!group->name) { group_import(runner, NULL, 0); return; }
    JsonNode *body = radios_data_body(runner->ctx);
    gboolean sent = radios_data_request(runner->ctx, GTK_WIDGET(runner->ctx->parent), "POST", "/v1/workflows/desktop-playlists/list",
                                        body, playlists_listed, GUINT_TO_POINTER(runner->generation), NULL);
    json_node_free(body);
    if (!sent) run_finish(runner, "Veri servisi bağlı değil; içe aktarma durdu.");
}

static void scan_finished(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)source; (void)unused; GTask *task = G_TASK(result); ScanTask *data = g_task_get_task_data(task);
    GError *error = NULL; gboolean ok = g_task_propagate_boolean(task, &error);
    GtkWidget *parent = g_weak_ref_get(&data->parent);
    ArchiveRunner *runner = parent ? g_object_get_data(G_OBJECT(parent), RUNNER_KEY) : NULL;
    if (!runner || runner->generation != data->generation || !runner->cancel) { g_clear_error(&error); g_clear_object(&parent); return; }
    g_clear_object(&runner->cancel);
    if (!ok) {
        gboolean cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        run_finish(runner, cancelled ? "Tarama durduruldu." : error ? error->message : "Arşiv taranamadı.");
        g_clear_error(&error); g_clear_object(&parent); return;
    }
    runner->found = data->stats.files; runner->bytes = data->stats.bytes; runner->fresh = data->fresh ? data->fresh->len : 0;
    runner->duplicates = data->duplicates; runner->folders_missing = data->stats.folders_missing;
    runner->unreadable = data->stats.unreadable; runner->truncated = data->stats.truncated; runner->library_records = data->records;
    runner->scanned = TRUE;
    groups_build(runner, data->fresh, data->folder_playlists);
    if (!runner->groups->len) { run_finish(runner, NULL); g_clear_object(&parent); return; }
    runner->importing = TRUE; runner->group_index = 0;
    group_next(runner);
    g_clear_object(&parent);
}

static gboolean run_start(ArchiveRunner *runner, gboolean manual)
{
    if (runner_busy(runner)) return FALSE;
    if (runner->settings_error) { runner_status(runner, runner->settings_error); return FALSE; }
    if (!runner->settings.folders || !runner->settings.folders->len) { runner_status(runner, "Önce bir arşiv klasörü ekleyin."); return FALSE; }
    if (!radios_data_connected(runner->ctx)) { runner_status(runner, "Arşiv taraması için veri servisi bağlantısı gerekiyor."); return FALSE; }
    RadioSDataClient *client = radios_data_client_acquire(runner->ctx);
    if (!client) { runner_status(runner, "Veri servisi kimliği hazır değil."); return FALSE; }
    ++runner->generation; runner->manual = manual;
    runner->found = runner->fresh = runner->duplicates = runner->imported = runner->failed = 0;
    runner->folders_missing = runner->unreadable = runner->library_records = 0; runner->bytes = 0; runner->truncated = FALSE; runner->scanned = FALSE;
    g_clear_pointer(&runner->last_error, g_free);
    ScanTask *data = g_new0(ScanTask, 1);
    g_weak_ref_init(&data->parent, runner->ctx->parent);
    data->generation = runner->generation; data->client = client; data->folder_playlists = runner->settings.folder_playlists;
    data->folders = g_new0(gchar *, runner->settings.folders->len + 1);
    for (guint i = 0; i < runner->settings.folders->len; ++i) data->folders[i] = g_strdup(g_ptr_array_index(runner->settings.folders, i));
    const RadioSConfig *config = runner->ctx->config;
    gchar *cache = config && config->cache_root ? g_strdup(config->cache_root) : g_build_filename(config->project_root, "data", "cache", NULL);
    data->index_path = g_build_filename(cache, INDEX_FILE, NULL); g_free(cache);
    runner->cancel = g_cancellable_new();
    GTask *task = g_task_new(NULL, runner->cancel, scan_finished, NULL);
    g_task_set_task_data(task, data, scan_task_free);
    g_task_run_in_thread(task, scan_thread);
    g_object_unref(task);
    runner_status(runner, "Arşiv klasörleri okunuyor…");
    return TRUE;
}

static void run_stop(ArchiveRunner *runner)
{
    if (runner->cancel) { g_cancellable_cancel(runner->cancel); return; }  /* scan_finished reports the stop */
    if (!runner->importing) return;
    radios_media_import_cancel(runner->ctx);
    run_finish(runner, "İçe aktarma durduruldu; tamamlanmış kayıtlar korunur.");
}

static gboolean watch_tick(gpointer value)
{
    ArchiveRunner *runner = value;
    if (!runner->settings.watch || runner->settings_error || runner_busy(runner) || !radios_data_connected(runner->ctx)) return G_SOURCE_CONTINUE;
    gint64 due = runner->last_run_us + (gint64)runner->settings.watch_minutes * 60 * G_USEC_PER_SEC;
    if (g_get_monotonic_time() >= due) run_start(runner, FALSE);
    return G_SOURCE_CONTINUE;
}

void radios_media_archive_bind(RadioSViewContext *ctx, GtkWidget *window)
{
    g_return_if_fail(ctx && GTK_IS_WINDOW(window));
    ArchiveRunner *runner = runner_of(ctx);
    if (!runner || runner->watch_timer) return;
    runner->last_run_us = g_get_monotonic_time();
    runner->watch_timer = g_timeout_add_seconds(WATCH_TICK_SECONDS, watch_tick, runner);
}

/* -------------------------------------------------------------------- page */

static ArchivePage *page_of(GtkWidget *widget) { return widget ? g_object_get_data(G_OBJECT(widget), PAGE_KEY) : NULL; }

static void page_status(ArchivePage *page, const gchar *message)
{
    gtk_label_set_text(GTK_LABEL(page->settings_status), message);
    if (page->ctx->notify) page->ctx->notify(page->ctx->notify_data, message);
}

static gboolean settings_store(ArchivePage *page, RadioSMediaArchiveSettings *proposed)
{
    GError *error = NULL;
    const gchar *root = page->ctx->config ? page->ctx->config->project_root : NULL;
    if (!root || !radios_media_archive_settings_save(root, proposed, &error)) {
        gchar *message = g_strdup_printf("Medya arşivi ayarları kaydedilemedi: %s", error ? error->message : "proje kökü bilinmiyor");
        page_status(page, message); g_free(message); g_clear_error(&error);
        return FALSE;
    }
    radios_media_archive_settings_copy(&page->runner->settings, proposed);
    g_clear_pointer(&page->runner->settings_error, g_free);
    page_status(page, "Medya arşivi ayarları kaydedildi (config/media-archive.ini).");
    return TRUE;
}

static void folders_rebuild(ArchivePage *page)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(page->folders));
    for (GList *child = children; child; child = child->next) gtk_widget_destroy(child->data);
    g_list_free(children);
    GPtrArray *folders = page->runner->settings.folders;
    for (guint i = 0; folders && i < folders->len; ++i) {
        const gchar *folder = g_ptr_array_index(folders, i);
        GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gchar *name = g_strdup_printf("preferences-archive-folder-%u", i); named(row, name); g_free(name);
        g_object_set_data_full(G_OBJECT(row), "path", g_strdup(folder), g_free);
        GtkWidget *path = gtk_label_new(folder); gtk_label_set_xalign(GTK_LABEL(path), 0);
        gtk_label_set_ellipsize(GTK_LABEL(path), PANGO_ELLIPSIZE_MIDDLE); gtk_widget_set_hexpand(path, TRUE);
        gtk_widget_set_tooltip_text(path, folder);
        RadioSMediaArchiveFolderState state = radios_media_archive_folder_state(folder);
        GtkWidget *status = gtk_label_new(radios_media_archive_folder_state_label(state));
        styled(status, state == RADIOS_MEDIA_ARCHIVE_FOLDER_READY ? "preferences-archive-ready" : "preferences-archive-problem");
        pack(box, path, TRUE); pack(box, status, FALSE);
        gtk_container_add(GTK_CONTAINER(row), box); gtk_container_add(GTK_CONTAINER(page->folders), row);
    }
    gtk_widget_show_all(page->folders);
}

static void page_refresh(ArchivePage *page)
{
    if (!page || page->disposed) return;
    ArchiveRunner *runner = page->runner;
    gboolean busy = runner_busy(runner), connected = radios_data_connected(page->ctx);
    gboolean folders = runner->settings.folders && runner->settings.folders->len > 0;
    gtk_widget_set_sensitive(page->scan, !busy && connected && folders && !runner->settings_error);
    gtk_widget_set_sensitive(page->stop, busy);
    gtk_widget_set_sensitive(page->remove, !busy && gtk_list_box_get_selected_row(GTK_LIST_BOX(page->folders)) != NULL);
    gtk_widget_set_sensitive(page->add, !busy && !runner->settings_error &&
        (!runner->settings.folders || runner->settings.folders->len < RADIOS_MEDIA_ARCHIVE_MAX_FOLDERS));
    gtk_widget_set_sensitive(page->minutes, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page->watch)));
    gtk_label_set_text(GTK_LABEL(page->progress), runner->status ? runner->status : "");
    if (runner->settings_error) gtk_label_set_text(GTK_LABEL(page->settings_status), runner->settings_error);
    else if (!connected && !busy) gtk_label_set_text(GTK_LABEL(page->settings_status), "Tarama ve içe aktarma için veri servisi bağlantısı gerekiyor; ayarlar yerel dosyada saklanır.");
    gchar *summary = runner->settings.watch ?
        g_strdup_printf("Otomatik izleme açık: RadioS açıkken her %u dakikada bir yeni dosyalar içe aktarılır.", runner->settings.watch_minutes) :
        g_strdup("Otomatik izleme kapalı: yeni dosyalar yalnız \"Şimdi tara\" ile içe aktarılır.");
    gtk_label_set_text(GTK_LABEL(page->summary), summary); g_free(summary);
}

static gboolean page_tick(gpointer value)
{
    ArchivePage *page = value; if (page->disposed) return G_SOURCE_REMOVE;
    page_refresh(page); return G_SOURCE_CONTINUE;
}

static void scan_clicked(GtkButton *button, gpointer value)
{ (void)button; ArchivePage *page = value; run_start(page->runner, TRUE); page_refresh(page); }
static void stop_clicked(GtkButton *button, gpointer value)
{ (void)button; ArchivePage *page = value; run_stop(page->runner); page_refresh(page); }

static void option_changed(GtkWidget *widget, gpointer value)
{
    (void)widget; ArchivePage *page = value; if (page->updating) return;
    RadioSMediaArchiveSettings proposed; radios_media_archive_settings_init(&proposed);
    radios_media_archive_settings_copy(&proposed, &page->runner->settings);
    const gchar *kind = gtk_combo_box_get_active_id(GTK_COMBO_BOX(page->kind));
    g_free(proposed.kind); proposed.kind = g_strdup(kind ? kind : "music");
    proposed.folder_playlists = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page->folder_playlists));
    proposed.watch = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page->watch));
    proposed.watch_minutes = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(page->minutes));
    settings_store(page, &proposed);
    radios_media_archive_settings_clear(&proposed);
    page_refresh(page);
}

/* The physical folder: links and junctions resolved, on-disk case, upper-case
 * drive letter, mapped network drives as UNC (a drive letter mapped only in
 * this logon session would not exist for the service).  Falls back to the
 * normalized chosen path when it cannot be resolved (then validation reports
 * the real problem). */
static gchar *chosen_folder(const gchar *chosen)
{
    gchar *physical = radios_fs_realpath(chosen, NULL);
    return physical ? physical : radios_path_normalize(chosen);
}

static void folder_chosen(GtkDialog *dialog, gint response, gpointer value)
{
    ArchivePage *page = value;
    gchar *chosen = response == GTK_RESPONSE_ACCEPT ? gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)) : NULL;
    gtk_widget_destroy(GTK_WIDGET(dialog));
    if (!chosen || page->disposed) { g_free(chosen); return; }
    gchar *folder = chosen_folder(chosen); g_free(chosen);
    RadioSMediaArchiveSettings proposed; radios_media_archive_settings_init(&proposed);
    radios_media_archive_settings_copy(&proposed, &page->runner->settings);
    g_ptr_array_add(proposed.folders, folder);
    GError *error = NULL;
    if (!radios_media_archive_settings_validate(&proposed, &error)) {
        gchar *message = g_strdup_printf("Klasör eklenmedi: %s", error ? error->message : "geçersiz klasör");
        page_status(page, message); g_free(message); g_clear_error(&error);
    } else if (settings_store(page, &proposed)) {
        folders_rebuild(page);
    }
    radios_media_archive_settings_clear(&proposed);
    page_refresh(page);
}

static void add_clicked(GtkButton *button, gpointer value)
{
    (void)button; ArchivePage *page = value;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Arşiv klasörü seç", GTK_WINDOW(gtk_widget_get_toplevel(page->page)),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Vazgeç", GTK_RESPONSE_CANCEL, "Klasörü ekle", GTK_RESPONSE_ACCEPT, NULL);
    named(dialog, "preferences-archive-folder-chooser");
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), TRUE);
    g_signal_connect(dialog, "response", G_CALLBACK(folder_chosen), page);
    gtk_widget_show_all(dialog);
}

static void remove_clicked(GtkButton *button, gpointer value)
{
    (void)button; ArchivePage *page = value;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(page->folders));
    const gchar *path = row ? g_object_get_data(G_OBJECT(row), "path") : NULL;
    if (!path) return;
    RadioSMediaArchiveSettings proposed; radios_media_archive_settings_init(&proposed);
    radios_media_archive_settings_copy(&proposed, &page->runner->settings);
    for (guint i = 0; i < proposed.folders->len; ++i)
        if (g_str_equal(g_ptr_array_index(proposed.folders, i), path)) { g_ptr_array_remove_index(proposed.folders, i); break; }
    if (settings_store(page, &proposed)) folders_rebuild(page);
    radios_media_archive_settings_clear(&proposed);
    page_refresh(page);
}

static void folder_selected(GtkListBox *list, GtkListBoxRow *row, gpointer value)
{ (void)list; (void)row; page_refresh(value); }

/* ---- storage locations ---- */

typedef struct { GWeakRef page; gchar *dirs[RADIOS_STORAGE_ROLE_COUNT]; gint64 used[RADIOS_STORAGE_ROLE_COUNT], free_bytes[RADIOS_STORAGE_ROLE_COUNT], total[RADIOS_STORAGE_ROLE_COUNT]; } SizeTask;

static void size_task_free(gpointer value)
{
    SizeTask *task = value; g_weak_ref_clear(&task->page);
    for (guint i = 0; i < RADIOS_STORAGE_ROLE_COUNT; ++i) g_free(task->dirs[i]);
    g_free(task);
}

static void size_thread(GTask *task, gpointer source, gpointer value, GCancellable *cancel)
{
    (void)source; (void)cancel; SizeTask *data = value;
    for (guint i = 0; i < RADIOS_STORAGE_ROLE_COUNT; ++i) {
        data->used[i] = radios_storage_tree_bytes(data->dirs[i]);
        data->free_bytes[i] = radios_storage_free_bytes(data->dirs[i]);
        data->total[i] = radios_storage_total_bytes(data->dirs[i]);
    }
    g_task_return_boolean(task, TRUE);
}

static void size_finished(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)source; (void)unused; SizeTask *data = g_task_get_task_data(G_TASK(result));
    GtkWidget *widget = g_weak_ref_get(&data->page); ArchivePage *page = page_of(widget);
    if (page && !page->disposed) {
        for (guint i = 0; i < RADIOS_STORAGE_ROLE_COUNT; ++i) {
            gchar *used = radios_storage_format_bytes(data->used[i]), *free_text = radios_storage_format_bytes(data->free_bytes[i]);
            gchar *total = radios_storage_format_bytes(data->total[i]);
            gchar *line = data->dirs[i] ? g_strdup_printf("Kullanılan %s · Diskte boş %s / %s", used, free_text, total) : g_strdup("—");
            gtk_label_set_text(GTK_LABEL(page->storage_size[i]), line);
            g_free(used); g_free(free_text); g_free(total); g_free(line);
        }
    }
    g_clear_object(&widget);
}

static void storage_refresh(ArchivePage *page)
{
    const RadioSConfig *config = page->ctx->config;
    SizeTask *data = g_new0(SizeTask, 1); g_weak_ref_init(&data->page, page->page);
    for (guint role = 0; role < RADIOS_STORAGE_ROLE_COUNT; ++role) {
        gchar *dir = NULL;
        if (config && config->project_root && (role != RADIOS_STORAGE_DATABASE || config->database_file))
            dir = radios_storage_current_dir(config, (RadioSStorageRole)role);
        gtk_label_set_text(GTK_LABEL(page->storage_path[role]), dir ? dir : "—");
        gtk_widget_set_tooltip_text(page->storage_path[role], dir);
        gtk_label_set_text(GTK_LABEL(page->storage_size[role]), dir ? "Boyut hesaplanıyor…" : "—");
        data->dirs[role] = dir;
    }
    gchar *notice = config && config->project_root ? radios_storage_notice(config) : NULL;
    gtk_label_set_text(GTK_LABEL(page->storage_notice), notice ? notice : "");
    gtk_widget_set_visible(page->storage_notice, notice != NULL);
    g_free(notice);
    GTask *task = g_task_new(NULL, NULL, size_finished, NULL);
    g_task_set_task_data(task, data, size_task_free);
    g_task_run_in_thread(task, size_thread);
    g_object_unref(task);
}

static gboolean storage_save(ArchivePage *page, RadioSStorageRole role, const gchar *folder)
{
    const gchar *root = page->ctx->config ? page->ctx->config->project_root : NULL;
    RadioSStorageSettings settings; radios_storage_settings_init(&settings);
    GError *error = NULL; gboolean ok = root != NULL;
    if (ok && !radios_storage_settings_load(root, &settings, &error)) ok = FALSE;
    if (ok) {
        g_free(settings.path[role]);
        settings.path[role] = g_strdup(folder);
        ok = radios_storage_settings_save(root, &settings, &error);
    }
    radios_storage_settings_clear(&settings);
    if (!ok) {
        gchar *message = g_strdup_printf("Depolama ayarı kaydedilemedi: %s", error ? error->message : "proje kökü bilinmiyor");
        gtk_label_set_text(GTK_LABEL(page->storage_status), message); g_free(message); g_clear_error(&error);
        return FALSE;
    }
    return TRUE;
}

static RadioSStorageRole storage_role_of(gpointer object)
{
    guint role = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(object), "storage-role"));
    return role < RADIOS_STORAGE_ROLE_COUNT ? (RadioSStorageRole)role : RADIOS_STORAGE_RECORDINGS;
}

/* Backups are read by the data service per backup; the other roles need a restart. */
static void storage_changed(ArchivePage *page, RadioSStorageRole role, const gchar *message)
{
    gtk_label_set_text(GTK_LABEL(page->storage_status), message);
    if (page->ctx->notify) page->ctx->notify(page->ctx->notify_data, message);
    if (role != RADIOS_STORAGE_BACKUPS) gtk_widget_set_sensitive(page->restart, TRUE);
    storage_refresh(page);
}

static void storage_chosen(GtkDialog *dialog, gint response, gpointer value)
{
    ArchivePage *page = value; RadioSStorageRole role = storage_role_of(dialog);
    gchar *chosen = response == GTK_RESPONSE_ACCEPT ? gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)) : NULL;
    gtk_widget_destroy(GTK_WIDGET(dialog));
    if (!chosen || page->disposed) { g_free(chosen); return; }
    gchar *folder = chosen_folder(chosen); g_free(chosen);
    GError *error = NULL;
    const gchar *root = page->ctx->config ? page->ctx->config->project_root : NULL;
    if (!root || !radios_storage_directory_prepare(root, role, folder, &error)) {
        gchar *message = g_strdup_printf("%s klasörü kullanılamıyor: %s", radios_storage_role_label(role), error ? error->message : "proje kökü bilinmiyor");
        gtk_label_set_text(GTK_LABEL(page->storage_status), message); g_free(message); g_clear_error(&error);
    } else if (storage_save(page, role, folder)) {
        gchar *message = g_strdup_printf(storage_chosen_texts[role], folder);
        storage_changed(page, role, message); g_free(message);
    }
    g_free(folder);
}

static void storage_choose_clicked(GtkButton *button, gpointer value)
{
    ArchivePage *page = value; RadioSStorageRole role = storage_role_of(button);
    GtkWidget *dialog = gtk_file_chooser_dialog_new(storage_titles[role], GTK_WINDOW(gtk_widget_get_toplevel(page->page)),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Vazgeç", GTK_RESPONSE_CANCEL, "Bu klasörü kullan", GTK_RESPONSE_ACCEPT, NULL);
    named(dialog, "preferences-storage-folder-chooser");
    g_object_set_data(G_OBJECT(dialog), "storage-role", GUINT_TO_POINTER(role));
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), TRUE);
    g_signal_connect(dialog, "response", G_CALLBACK(storage_chosen), page);
    gtk_widget_show_all(dialog);
}

static void storage_default_clicked(GtkButton *button, gpointer value)
{
    ArchivePage *page = value; RadioSStorageRole role = storage_role_of(button);
    if (storage_save(page, role, NULL)) storage_changed(page, role, storage_default_texts[role]);
}

static void restart_response(GtkDialog *dialog, gint response, gpointer value)
{
    ArchivePage *page = value; gtk_widget_destroy(GTK_WIDGET(dialog));
    if (response != GTK_RESPONSE_ACCEPT || page->disposed) return;
    GError *error = NULL;
    if (!radios_reset_ui_restart(page->ctx, &error)) {
        gchar *message = g_strdup_printf("RadioS yeniden başlatılamadı: %s. Uygulamayı kapatıp yeniden açın.", error ? error->message : "");
        gtk_label_set_text(GTK_LABEL(page->storage_status), message); g_free(message);
    }
    g_clear_error(&error);
}

static void restart_clicked(GtkButton *button, gpointer value)
{
    (void)button; ArchivePage *page = value;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(page->page)), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "RadioS şimdi kapatılıp yeniden açılsın mı? Süren yayın ve kayıtlar durur.");
    named(dialog, "preferences-storage-restart-confirm");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Vazgeç", GTK_RESPONSE_CANCEL, "Yeniden başlat", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    g_signal_connect(dialog, "response", G_CALLBACK(restart_response), page);
    gtk_widget_show_all(dialog);
}

static void page_destroyed(GtkWidget *widget, gpointer value)
{
    (void)widget; ArchivePage *page = value; page->disposed = TRUE;
    if (page->timer) { g_source_remove(page->timer); page->timer = 0; }
    if (page->runner && page->runner->page == page->page) page->runner->page = NULL;
}

static GtkWidget *storage_row(ArchivePage *page, GtkWidget *section, RadioSStorageRole role, const gchar *id)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gchar *name = g_strdup_printf("preferences-storage-%s", id); named(box, name); g_free(name);
    pack(box, text(radios_storage_role_label(role), "preferences-storage-role"), FALSE);
    page->storage_path[role] = text("—", "preferences-storage-path");
    name = g_strdup_printf("preferences-storage-%s-path", id); named(page->storage_path[role], name); g_free(name);
    gtk_label_set_ellipsize(GTK_LABEL(page->storage_path[role]), PANGO_ELLIPSIZE_MIDDLE); gtk_label_set_line_wrap(GTK_LABEL(page->storage_path[role]), FALSE);
    gtk_label_set_selectable(GTK_LABEL(page->storage_path[role]), TRUE);
    page->storage_size[role] = text("—", "preferences-storage-size");
    name = g_strdup_printf("preferences-storage-%s-size", id); named(page->storage_size[role], name); g_free(name);
    pack(box, page->storage_path[role], FALSE); pack(box, page->storage_size[role], FALSE);
    pack(section, box, FALSE);
    return box;
}

GtkWidget *radios_media_archive_ui_page_new(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx && GTK_IS_WINDOW(ctx->parent), NULL);
    ArchivePage *page = g_new0(ArchivePage, 1); page->ctx = ctx; page->runner = runner_of(ctx); page->updating = TRUE;
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16); gtk_container_set_border_width(GTK_CONTAINER(body), 30);
    named(body, "preferences-archive-page"); page->page = body;
    g_object_set_data_full(G_OBJECT(body), PAGE_KEY, page, g_free);
    g_signal_connect(body, "destroy", G_CALLBACK(page_destroyed), page);
    pack(body, text("Medya arşivi", "preferences-heading"), FALSE);
    pack(body, text("Arşiv klasörleri yalnız okunur: yeni ses dosyaları doğrulanıp medya kütüphanesine kopyalanır, kaynak dosyalara dokunulmaz. "
        "Daha önce içe aktarılmış dosyalar (aynı SHA-256) atlanır.", "preferences-subtitle"), FALSE);

    /* Archive folders. */
    GtkWidget *folders = section_new("Arşiv klasörleri (ses dosyalarının okunduğu yer)");
    GtkWidget *list_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(list_scroll, -1, 120);
    page->folders = named(gtk_list_box_new(), "preferences-archive-folders");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page->folders), GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(list_scroll), page->folders); pack(folders, list_scroll, FALSE);
    GtkWidget *folder_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    page->add = named(gtk_button_new_with_label("Klasör ekle…"), "preferences-archive-add");
    page->remove = named(gtk_button_new_with_label("Seçili klasörü kaldır"), "preferences-archive-remove");
    pack(folder_actions, page->add, FALSE); pack(folder_actions, page->remove, FALSE); pack(folders, folder_actions, FALSE);
    pack(folders, text("Harici disk veya ağ klasörü seçilebilir; takılı olmayan klasör taramada atlanır ve burada \"Bulunamadı\" olarak görünür. "
        "En çok 16 klasör; klasörler iç içe olamaz.", "preferences-subtitle"), FALSE);
    pack(body, folders, FALSE);

    /* Import options. */
    GtkWidget *options = section_new("İçe aktarma");
    GtkWidget *kind_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    pack(kind_row, text("Yeni dosyaların türü:", NULL), FALSE);
    page->kind = named(gtk_combo_box_text_new(), "preferences-archive-kind");
    for (guint i = 0; kinds[i]; ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(page->kind), kinds[i], kind_labels[i]);
    pack(kind_row, page->kind, FALSE); pack(options, kind_row, FALSE);
    page->folder_playlists = named(gtk_check_button_new_with_label("Her alt klasör için aynı adlı playlist oluştur ve yeni dosyaları ona ekle"), "preferences-archive-folder-playlists");
    pack(options, page->folder_playlists, FALSE);
    GtkWidget *watch_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    page->watch = named(gtk_check_button_new_with_label("RadioS açıkken yeni dosyaları izle; her"), "preferences-archive-watch");
    page->minutes = named(gtk_spin_button_new_with_range(5, 1440, 5), "preferences-archive-minutes");
    pack(watch_row, page->watch, FALSE); pack(watch_row, page->minutes, FALSE); pack(watch_row, text("dakikada bir içe aktar", NULL), FALSE);
    pack(options, watch_row, FALSE);
    page->summary = named(text("", "preferences-subtitle"), "preferences-archive-summary"); pack(options, page->summary, FALSE);
    GtkWidget *scan_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    page->scan = named(gtk_button_new_with_label("Şimdi tara ve içe aktar"), "preferences-archive-scan");
    page->stop = named(gtk_button_new_with_label("Durdur"), "preferences-archive-stop");
    pack(scan_row, page->scan, FALSE); pack(scan_row, page->stop, FALSE); pack(options, scan_row, FALSE);
    page->progress = named(text("", NULL), "preferences-archive-progress"); pack(options, page->progress, FALSE);
    page->settings_status = named(text("", "preferences-subtitle"), "preferences-archive-status"); pack(options, page->settings_status, FALSE);
    pack(body, options, FALSE);

    /* Storage locations. */
    GtkWidget *storage = section_new("Radyo verileri (RadioS'un yazdığı yer)");
    static const RadioSStorageRole order[] = {RADIOS_STORAGE_RECORDINGS, RADIOS_STORAGE_BACKUPS, RADIOS_STORAGE_DATABASE};
    for (guint i = 0; i < G_N_ELEMENTS(order); ++i) {
        RadioSStorageRole role = order[i];
        GtkWidget *row = storage_row(page, storage, role, storage_ids[role]), *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gchar *name = g_strdup_printf("preferences-storage-%s-choose", storage_ids[role]);
        page->storage_choose[role] = named(gtk_button_new_with_label("Seç…"), name); g_free(name);
        name = g_strdup_printf("preferences-storage-%s-default", storage_ids[role]);
        page->storage_default[role] = named(gtk_button_new_with_label("Varsayılana dön"), name); g_free(name);
        g_object_set_data(G_OBJECT(page->storage_choose[role]), "storage-role", GUINT_TO_POINTER(role));
        g_object_set_data(G_OBJECT(page->storage_default[role]), "storage-role", GUINT_TO_POINTER(role));
        pack(actions, page->storage_choose[role], FALSE); pack(actions, page->storage_default[role], FALSE); pack(row, actions, FALSE);
        g_signal_connect(page->storage_choose[role], "clicked", G_CALLBACK(storage_choose_clicked), page);
        g_signal_connect(page->storage_default[role], "clicked", G_CALLBACK(storage_default_clicked), page);
    }
    page->restart = named(gtk_button_new_with_label("Şimdi yeniden başlat"), "preferences-storage-restart");
    gtk_widget_set_sensitive(page->restart, FALSE); gtk_widget_set_halign(page->restart, GTK_ALIGN_START);
    pack(storage, page->restart, FALSE);
    page->storage_status = named(text("", "preferences-subtitle"), "preferences-storage-status"); pack(storage, page->storage_status, FALSE);
    pack(storage, text("Seçilen klasöre RadioS bir işaret dosyası yazar; işareti olmayan (takılı olmayan) disk yerine boş bir klasör kullanılmaz. "
        "Kayıt ve veritabanı klasörü RadioS yeniden başlatılınca geçerli olur; veritabanı yeni klasörde yoksa açılışta son kullanılan kopya oraya kopyalanır. "
        "Yedek klasörünü veri servisi sonraki yedekte kullanır. Ulaşılamayan veritabanı klasörü hata demektir: eski kopyayla açılmaz.", "preferences-subtitle"), FALSE);
    page->storage_notice = named(text("", "preferences-storage-notice"), "preferences-storage-notice");
    gtk_widget_set_no_show_all(page->storage_notice, TRUE); pack(storage, page->storage_notice, FALSE);
    pack(body, storage, FALSE);

    /* Values. */
    ArchiveRunner *runner = page->runner;
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(page->kind), runner->settings.kind);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(page->kind)) < 0) gtk_combo_box_set_active(GTK_COMBO_BOX(page->kind), 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->folder_playlists), runner->settings.folder_playlists);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->watch), runner->settings.watch);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(page->minutes), runner->settings.watch_minutes);
    folders_rebuild(page);
    storage_refresh(page);
    page->updating = FALSE;
    runner->page = body;
    g_signal_connect(page->kind, "changed", G_CALLBACK(option_changed), page);
    g_signal_connect(page->folder_playlists, "toggled", G_CALLBACK(option_changed), page);
    g_signal_connect(page->watch, "toggled", G_CALLBACK(option_changed), page);
    g_signal_connect(page->minutes, "value-changed", G_CALLBACK(option_changed), page);
    g_signal_connect(page->add, "clicked", G_CALLBACK(add_clicked), page);
    g_signal_connect(page->remove, "clicked", G_CALLBACK(remove_clicked), page);
    g_signal_connect(page->folders, "row-selected", G_CALLBACK(folder_selected), page);
    g_signal_connect(page->scan, "clicked", G_CALLBACK(scan_clicked), page);
    g_signal_connect(page->stop, "clicked", G_CALLBACK(stop_clicked), page);
    g_signal_connect(page->restart, "clicked", G_CALLBACK(restart_clicked), page);
    page_refresh(page);
    page->timer = g_timeout_add(1000, page_tick, page);
    GtkWidget *scroll = named(gtk_scrolled_window_new(NULL, NULL), "preferences-archive-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), body);
    return scroll;
}
