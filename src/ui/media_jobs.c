#include "radios/media_jobs.h"
#include "radios/media.h"
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    RadioSViewContext *ctx;
    RadioSDataClient *client;
    RadioSConfig config;
    GWeakRef owner, parent;
    gchar *source, *source_root, *kind, *title, *retry_key, *snapshot;
    GCancellable *cancel;
    JsonNode *body;
    RadioSDataCallback callback;
    gpointer user;
    GDestroyNotify destroy;
    gboolean completed, sent, replaying, reserved, staged;
} MediaJob;
typedef struct { JsonNode *body; gchar *kind, *title; } MediaRetry;
static void media_retry_free(gpointer value)
{ MediaRetry *retry = value; json_node_free(retry->body); g_free(retry->kind); g_free(retry->title); g_free(retry); }

static void media_owner_destroyed(GtkWidget *owner, gpointer unused)
{ (void)unused; g_object_set_data(G_OBJECT(owner), "radios-media-destroyed", GINT_TO_POINTER(1)); }
static void guard_media_owner(GtkWidget *owner)
{
    if (!g_object_get_data(G_OBJECT(owner), "radios-media-guard")) {
        g_object_set_data(G_OBJECT(owner), "radios-media-guard", GINT_TO_POINTER(1));
        g_signal_connect(owner, "destroy", G_CALLBACK(media_owner_destroyed), NULL);
    }
}
static void media_job_free(gpointer value)
{
    MediaJob *job = value;
    g_weak_ref_clear(&job->owner); g_weak_ref_clear(&job->parent);
    radios_data_client_unref(job->client); radios_config_clear(&job->config);
    g_free(job->source); g_free(job->kind); g_free(job->title); g_free(job->retry_key); g_free(job->snapshot);
    g_free(job->source_root);
    g_clear_object(&job->cancel); g_clear_pointer(&job->body, json_node_free); g_free(job);
}
static gboolean uncertain_error(const GError *error)
{
    if (!error || error->domain != RADIOS_DATA_CLIENT_ERROR) return FALSE;
    switch (error->code) {
    case RADIOS_DATA_CLIENT_ERROR_TRANSPORT: case RADIOS_DATA_CLIENT_ERROR_TIMEOUT:
    case RADIOS_DATA_CLIENT_ERROR_CANCELLED: case RADIOS_DATA_CLIENT_ERROR_INVALID_RESPONSE:
    case RADIOS_DATA_CLIENT_ERROR_RESPONSE_TOO_LARGE: case RADIOS_DATA_CLIENT_ERROR_UNAVAILABLE:
    case RADIOS_DATA_CLIENT_ERROR_COMMITTED: case RADIOS_DATA_CLIENT_ERROR_HTTP:
    case RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED: return TRUE;
    default: return FALSE;
    }
}
static void media_job_done(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)source; (void)unused; GTask *task = G_TASK(result); MediaJob *job = g_task_get_task_data(task);
    GError *error = NULL; JsonNode *node = g_task_propagate_pointer(task, &error);
    GtkWidget *owner = g_weak_ref_get(&job->owner), *parent = g_weak_ref_get(&job->parent);
    gboolean parent_live = parent && !g_object_get_data(G_OBJECT(parent), "radios-media-destroyed");
    if (parent_live) {
        GHashTable *sources = g_object_get_data(G_OBJECT(parent), "radios-media-active-sources");
        if (sources) g_hash_table_remove(sources, job->retry_key);
        if (job->reserved) {
            guint active = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(parent), "radios-media-active"));
            g_object_set_data(G_OBJECT(parent), "radios-media-active", GUINT_TO_POINTER(active > 0 ? active - 1 : 0));
        }
        GHashTable *retry = g_object_get_data(G_OBJECT(parent), "radios-media-retries");
        if (job->body && ((job->sent && uncertain_error(error)) || (!job->sent && job->replaying))) {
            MediaRetry *pending = g_new0(MediaRetry, 1); pending->body = json_node_copy(job->body);
            pending->kind = g_strdup(job->kind); pending->title = g_strdup(job->title);
            g_hash_table_replace(retry, g_strdup(job->retry_key), pending);
        } else g_hash_table_remove(retry, job->retry_key);
    }
    if (job->snapshot && (!error || (!job->replaying && !job->sent) ||
                          (job->sent && !uncertain_error(error)))) g_unlink(job->snapshot);
    if (parent_live) {
        if (!error) g_object_set_data(G_OBJECT(parent), "radios-media-refresh-needed", GINT_TO_POINTER(1));
        if (!GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(parent), "radios-media-active")) &&
            g_object_get_data(G_OBJECT(parent), "radios-media-refresh-needed")) {
            g_object_set_data(G_OBJECT(parent), "radios-media-refresh-needed", NULL);
            radios_data_refresh(job->ctx);
        }
    }
    if (parent_live && owner && !g_object_get_data(G_OBJECT(owner), "radios-media-destroyed")) {
        if (job->ctx->notify && error) job->ctx->notify(job->ctx->notify_data, error->message);
        if (job->callback) job->callback(job->ctx, owner, node, error, job->user);
    }
    if (job->destroy) { job->destroy(job->user); job->destroy = NULL; }
    g_clear_object(&owner); g_clear_object(&parent); g_clear_error(&error); g_clear_pointer(&node, json_node_free);
}
static gchar *media_job_work(gpointer value, const RadioSCancelToken *cancel, GError **unused)
{
    (void)unused; GTask *task = value; MediaJob *job = g_task_get_task_data(task); GError *error = NULL;
    if (!job->body) {
        RadioSMediaAnalysis *analysis = job->staged ? radios_media_stage_probe(&job->config, job->source, cancel, job->cancel, &error) :
            radios_media_stage_analyze(&job->config, job->source, cancel, job->cancel, &error);
        if (analysis) {
            job->snapshot = g_strdup(analysis->path);
            job->body = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(job->body, object);
            json_object_set_string_member(object, "station_id", radios_data_client_station_id(job->client));
            gchar *filename = g_path_get_basename(analysis->path), *key = g_uuid_string_random();
            json_object_set_string_member(object, "import_path", filename); g_free(filename);
            json_object_set_string_member(object, "source_name", analysis->source_name);
            json_object_set_string_member(object, "idempotency_key", key); g_free(key);
            JsonObject *metadata = json_object_new();
            const gchar *title = *job->title ? job->title : analysis->title;
            gchar *fallback = g_strdup(analysis->source_name); gchar *extension = strrchr(fallback, '.');
            if (extension && extension != fallback) *extension = '\0';
            if (!title || !*title) title = fallback;
            json_object_set_string_member(metadata, "title", title);
            g_free(fallback);
            json_object_set_string_member(metadata, "kind", job->kind);
            json_object_set_string_member(metadata, "artist", analysis->artist ? analysis->artist : "");
            json_object_set_string_member(metadata, "album", analysis->album ? analysis->album : "");
            json_object_set_string_member(metadata, "genre", analysis->genre ? analysis->genre : "");
            json_object_set_string_member(metadata, "year", ""); json_object_set_string_member(metadata, "comment", "");
            json_object_set_array_member(metadata, "tags", json_array_new());
            json_object_set_object_member(object, "metadata", metadata);
            if (job->staged) {
                JsonObject *probe = json_object_new();
                json_object_set_string_member(probe, "sha256", analysis->sha256);
                json_object_set_string_member(probe, "codec", analysis->codec);
                json_object_set_string_member(probe, "format", analysis->format);
                json_object_set_int_member(probe, "size_bytes", analysis->size_bytes);
                json_object_set_int_member(probe, "duration_ms", analysis->duration_ms);
                json_object_set_int_member(probe, "sample_rate", analysis->sample_rate);
                json_object_set_int_member(probe, "channels", analysis->channels);
                json_object_set_int_member(probe, "bitrate", analysis->bitrate);
                json_object_set_object_member(object, "probe", probe);
            } else json_object_set_member(object, "analysis", radios_media_analysis_to_json(analysis));
            gchar *source = g_canonicalize_filename(job->source, NULL);
            gchar *root = job->source_root ? g_canonicalize_filename(job->source_root, NULL) : g_path_get_dirname(source);
            json_object_set_string_member(object, "source_path", source);
            json_object_set_string_member(object, "source_root", root);
            g_free(source); g_free(root);
            radios_media_analysis_free(analysis);
        }
    } else {
        const gchar *filename = radios_json_string(json_node_get_object(job->body), "import_path", "");
        job->snapshot = g_build_filename(job->config.import_root, filename, NULL);
    }
    JsonNode *result = NULL;
    const gchar *endpoint = job->body && json_object_has_member(json_node_get_object(job->body), "probe") ?
        "/v1/workflows/desktop-media/import-staged" : "/v1/workflows/desktop-media/import";
    if (!error && job->body) {
        if (radios_cancelled(cancel) || g_cancellable_is_cancelled(job->cancel))
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Medya içe aktarma iptal edildi.");
        else if (radios_data_client_prepare(job->client, "POST", endpoint, job->body, &error)) {
            job->sent = TRUE;
            result = radios_data_client_request_cancellable_sync(job->client, "POST", endpoint,
                                                                  job->body, cancel, job->cancel, &error);
        }
    }
    job->completed = TRUE;
    if (error) g_task_return_error(task, error);
    else if (result) g_task_return_pointer(task, result, (GDestroyNotify)json_node_free);
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Medya sonucu alınamadı.");
    return NULL;
}
static void media_task_destroy(gpointer value)
{
    GTask *task = value; MediaJob *job = g_task_get_task_data(task);
    if (!job->completed) {
        job->completed = TRUE; g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Medya işi iptal edildi.");
    }
    g_object_unref(task);
}
static GCancellable *import_async(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *source, const gchar *source_root, gboolean staged, const gchar *kind, const gchar *title,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{
    RadioSDataClient *client = radios_data_client_acquire(ctx);
    if (!client || !source || !*source || !owner) {
        if (client) radios_data_client_unref(client);
        if (ctx && ctx->notify) ctx->notify(ctx->notify_data, "Medya içe aktarma için veritabanı bağlantısı ve dosya seçimi gerekli.");
        if (destroy) destroy(user);
        return NULL;
    }
    GtkWidget *parent = GTK_WIDGET(ctx->parent); guard_media_owner(owner); guard_media_owner(parent);
    GHashTable *retry = g_object_get_data(G_OBJECT(parent), "radios-media-retries");
    if (!retry) {
        retry = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, media_retry_free);
        g_object_set_data_full(G_OBJECT(parent), "radios-media-retries", retry, (GDestroyNotify)g_hash_table_unref);
    }
    GHashTable *sources = g_object_get_data(G_OBJECT(parent), "radios-media-active-sources");
    if (!sources) {
        sources = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_object_set_data_full(G_OBJECT(parent), "radios-media-active-sources", sources, (GDestroyNotify)g_hash_table_unref);
    }
    gchar *identity = g_canonicalize_filename(source, NULL);
    if (g_hash_table_contains(sources, identity)) {
        if (ctx->notify) ctx->notify(ctx->notify_data, "Bu dosyanın içe aktarma işlemi sürüyor; sonucu bekleyin.");
        radios_data_client_unref(client); g_free(identity); if (destroy) destroy(user); return NULL;
    }
    MediaRetry *previous = g_hash_table_lookup(retry, identity);
    const gchar *requested_kind = kind ? kind : "music", *requested_title = title ? title : "";
    guint active = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(parent), "radios-media-active"));
    if ((previous && (g_strcmp0(previous->kind, requested_kind) || g_strcmp0(previous->title, requested_title))) ||
        (!previous && g_hash_table_size(retry) + active >= 64)) {
        if (ctx->notify) ctx->notify(ctx->notify_data, previous ?
            "Bu dosyanın önceki kaydının sonucu belirsiz. Önce aynı tür ve başlıkla tekrar deneyin; ardından etiketlerini düzenleyin." :
            "Bekleyen medya işlemleri dolu. Önce sonucu belirsiz içe aktarmaları aynı dosyaları seçerek tamamlayın.");
        radios_data_client_unref(client); g_free(identity); if (destroy) destroy(user); return NULL;
    }
    MediaJob *job = g_new0(MediaJob, 1); job->ctx = ctx; job->client = client;
    job->config.project_root = g_strdup(ctx->config->project_root); job->config.import_root = g_strdup(ctx->config->import_root);
    job->source = g_strdup(source); job->kind = g_strdup(kind ? kind : "music"); job->title = g_strdup(title ? title : "");
    job->source_root = g_strdup(source_root); job->staged = staged;
    job->retry_key = identity; job->body = previous ? json_node_copy(previous->body) : NULL;
    job->replaying = previous != NULL;
    g_hash_table_add(sources, g_strdup(identity));
    if (!previous) { job->reserved = TRUE; g_object_set_data(G_OBJECT(parent), "radios-media-active", GUINT_TO_POINTER(active + 1)); }
    job->callback = callback; job->user = user; job->destroy = destroy;
    job->cancel = g_cancellable_new(); g_weak_ref_init(&job->owner, owner); g_weak_ref_init(&job->parent, parent);
    g_signal_connect_object(owner, "destroy", G_CALLBACK(g_cancellable_cancel), job->cancel, G_CONNECT_SWAPPED);
    GTask *task = g_task_new(NULL, NULL, media_job_done, NULL); g_task_set_task_data(task, job, media_job_free);
    GError *error = NULL; GCancellable *handle = g_object_ref(job->cancel);
    if (!radios_runtime_submit(ctx->runtime, media_job_work, task, media_task_destroy, &error)) {
        job->completed = TRUE; g_task_return_error(task, error); g_object_unref(task);
    }
    return handle;
}

GCancellable *radios_media_import_async(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *source, const gchar *kind, const gchar *title,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{ return import_async(ctx, owner, source, NULL, FALSE, kind, title, callback, user, destroy); }

GCancellable *radios_media_import_staged_async(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *source, const gchar *source_root, const gchar *kind, const gchar *title,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{ return import_async(ctx, owner, source, source_root, TRUE, kind, title, callback, user, destroy); }
