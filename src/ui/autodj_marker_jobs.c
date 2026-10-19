#include "radios/autodj_marker_jobs.h"
#include "radios/programme_model.h"
#include "radios/library_analysis.h"
#include <math.h>
#include <string.h>

typedef struct {
    RadioSViewContext *ctx;
    RadioSDataClient *client;
    GWeakRef owner, parent;
    GMainContext *context;
    gchar *playlist_id;
    gboolean overwrite;
    gdouble threshold;
    guint lead;
    guint workers; /* tracks assigned at the same time (Ayarlar › Kütüphane analizi) */
    gint finished;
    RadioSAutoDJMarkersProgress progress;
    RadioSDataCallback callback;
    gpointer user;
    GDestroyNotify destroy;
} MarkerJob;
typedef struct { GTask *task; guint completed, total; } MarkerProgress;
static JsonObject *object_at(JsonObject *o, const gchar *key)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
}
static JsonArray *array_at(JsonObject *o, const gchar *key)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL;
}
static JsonNode *body_new(MarkerJob *job)
{
    JsonNode *n = json_node_new(JSON_NODE_OBJECT); json_node_take_object(n, json_object_new());
    json_object_set_string_member(json_node_get_object(n), "station_id", radios_data_client_station_id(job->client)); return n;
}
static JsonNode *call(MarkerJob *job, const gchar *path, JsonNode *body, GCancellable *cancel, GError **error)
{
    JsonNode *result = radios_data_client_request_cancellable_sync(job->client, "POST", path, body, NULL, cancel, error);
    if (result && !JSON_NODE_HOLDS_OBJECT(result)) {
        json_node_free(result); result = NULL;
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Auto DJ için geçerli veritabanı yanıtı gerekli.");
    }
    return result;
}
static void owner_dead(GtkWidget *owner, gpointer unused)
{ (void)unused; g_object_set_data(G_OBJECT(owner), "autodj-marker-owner-dead", GINT_TO_POINTER(1)); }
static void guard_owner(GtkWidget *owner)
{
    if (g_object_get_data(G_OBJECT(owner), "autodj-marker-owner-guard")) return;
    g_object_set_data(G_OBJECT(owner), "autodj-marker-owner-guard", GINT_TO_POINTER(1));
    g_signal_connect(owner, "destroy", G_CALLBACK(owner_dead), NULL);
}
static void cancel_owner(GtkWidget *owner, GCancellable *cancel)
{ (void)owner; g_cancellable_cancel(cancel); }
static GtkWidget *live_owner(MarkerJob *job)
{
    GtkWidget *parent = g_weak_ref_get(&job->parent), *owner = g_weak_ref_get(&job->owner);
    gboolean live = parent && owner && !g_object_get_data(G_OBJECT(parent), "autodj-marker-owner-dead") &&
        !g_object_get_data(G_OBJECT(owner), "autodj-marker-owner-dead") && !gtk_widget_in_destruction(parent) && !gtk_widget_in_destruction(owner);
    if (live) {
        RadioSDataClient *current = radios_data_client_acquire(job->ctx);
        live = radios_data_client_same_identity(current, job->client);
        if (current) radios_data_client_unref(current);
    }
    g_clear_object(&parent);
    if (!live) g_clear_object(&owner);
    return owner;
}
static gboolean report_progress(gpointer data)
{
    MarkerProgress *p = data; MarkerJob *job = g_task_get_task_data(p->task);
    if (!g_atomic_int_get(&job->finished)) {
        GtkWidget *owner = live_owner(job);
        if (owner && job->progress) job->progress(p->completed, p->total, job->user);
        else g_cancellable_cancel(g_task_get_cancellable(p->task));
        g_clear_object(&owner);
    }
    return G_SOURCE_REMOVE;
}
static void progress_free(gpointer data)
{ MarkerProgress *p = data; g_object_unref(p->task); g_free(p); }
static void progress_post(GTask *task, guint completed, guint total)
{
    MarkerJob *job = g_task_get_task_data(task); if (!job->progress) return;
    MarkerProgress *p = g_new0(MarkerProgress, 1); p->task = g_object_ref(task); p->completed = completed; p->total = total;
    GSource *source = g_idle_source_new(); g_source_set_callback(source, report_progress, p, progress_free);
    g_source_attach(source, job->context); g_source_unref(source);
}
static void add_target(GPtrArray *ids, GHashTable *seen, const gchar *id, guint *omitted)
{
    if (!id || !g_uuid_string_is_valid(id)) { ++*omitted; return; }
    if (g_hash_table_contains(seen, id)) return;
    gchar *copy = g_strdup(id); g_ptr_array_add(ids, copy); g_hash_table_add(seen, copy);
}
static GPtrArray *targets(MarkerJob *job, guint *omitted, GCancellable *cancel, GError **error)
{
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free); GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    if (job->playlist_id) {
        JsonNode *body = body_new(job), *result = call(job, "/v1/workflows/desktop-playlists/list", body, cancel, error); json_node_free(body);
        JsonArray *rows = result ? array_at(json_node_get_object(result), "playlists") : NULL; gboolean found = FALSE;
        for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
            JsonObject *row = json_array_get_object_element(rows, i);
            if (g_strcmp0(radios_json_string(row, "_id", ""), job->playlist_id)) continue;
            found = TRUE; JsonArray *tracks = array_at(row, "tracks");
            for (guint j = 0; tracks && j < json_array_get_length(tracks); ++j) {
                JsonObject *track = json_array_get_object_element(tracks, j);
                add_target(ids, seen, radios_json_string(track, "media_id", ""), omitted);
            }
            break;
        }
        if (result && !found) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Seçilen playlist kaldırılmış veya bu istasyonda bulunamadı.");
        g_clear_pointer(&result, json_node_free);
    } else {
        gint64 offset = 0, sequence = -1;
        while (!g_cancellable_is_cancelled(cancel)) {
            JsonNode *body = body_new(job); JsonObject *request = json_node_get_object(body);
            json_object_set_string_member(request, "kind", "music"); json_object_set_int_member(request, "limit", 200); json_object_set_int_member(request, "offset", offset);
            JsonNode *result = call(job, "/v1/workflows/desktop-media/list", body, cancel, error); json_node_free(body); if (!result) break;
            JsonObject *page = json_node_get_object(result); gint64 current = radios_json_int(page, "sequence", -1);
            if (sequence >= 0 && current != sequence) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Kütüphane tarama sırasında değişti; hiçbir işaret yazılmadı. Yeniden başlatın."); json_node_free(result); break;
            }
            sequence = current; JsonArray *rows = array_at(page, "records");
            if (!rows) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Medya listesi geçersiz."); json_node_free(result); break; }
            for (guint i = 0; i < json_array_get_length(rows); ++i) {
                JsonObject *media = object_at(json_array_get_object_element(rows, i), "media");
                add_target(ids, seen, radios_json_string(media, "_id", ""), omitted);
            }
            JsonNode *next = json_object_get_member(page, "next_offset");
            gint64 following = next && JSON_NODE_HOLDS_VALUE(next) && json_node_get_value_type(next) == G_TYPE_INT64 ? json_node_get_int(next) : -1;
            json_node_free(result); if (following < 0) break;
            if (following <= offset) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Kütüphane sayfalaması ilerlemiyor."); break; }
            offset = following;
        }
    }
    g_hash_table_unref(seen);
    if (error && *error) { g_ptr_array_unref(ids); return NULL; }
    return ids;
}
/* An unchanged calculation is a successful analysis, but must acknowledge the
 * same recording and untouched marker draft before it is counted as such. */
static gboolean marker_ack_valid(JsonObject *response, JsonObject *record, const gchar *id, gboolean changed)
{
    JsonObject *ack = object_at(response, "detail"), *media = object_at(record, "media"), *version = object_at(record, "version");
    JsonObject *m = object_at(ack, "media"), *v = object_at(ack, "version");
    JsonObject *old_points = object_at(record, "markers"), *points = object_at(ack, "markers");
    guint revision_step = changed ? 1 : 0;
    if (!m || !v || !points || !media || !version ||
        g_strcmp0(radios_json_string(m, "_id", ""), id) ||
        g_strcmp0(radios_json_string(v, "_id", ""), radios_json_string(version, "_id", "!")) ||
        g_strcmp0(radios_json_string(v, "sha256", ""), radios_json_string(version, "sha256", "!")) ||
        radios_json_int(points, "autodj_in_ms", -1) < 0 ||
        radios_json_int(points, "autodj_out_ms", -1) <= radios_json_int(points, "autodj_in_ms", -1) ||
        radios_json_int(m, "_rev", 0) != radios_json_int(media, "_rev", 0) + revision_step ||
        radios_json_int(points, "_rev", 0) != radios_json_int(old_points, "_rev", 0) + revision_step) return FALSE;
    return changed || (old_points && json_node_equal(json_object_get_member(record, "markers"), json_object_get_member(ack, "markers")));
}
gboolean radios_autodj_points_due(JsonObject *row)
{
    JsonObject *media = object_at(row, "media"), *version = object_at(row, "version"), *markers = object_at(row, "markers");
    JsonNode *verified = version ? json_object_get_member(version, "verified") : NULL;
    if (g_strcmp0(radios_json_string(media, "kind", ""), "music") || !object_at(row, "analysis") || !verified ||
        !JSON_NODE_HOLDS_VALUE(verified) || json_node_get_value_type(verified) != G_TYPE_BOOLEAN || !json_node_get_boolean(verified)) return FALSE;
    JsonObject *source = object_at(markers, "autodj_source");
    const gchar *keys[] = {"autodj_in_ms", "autodj_out_ms"};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) {
        JsonNode *point = markers ? json_object_get_member(markers, keys[i]) : NULL;
        const gchar *by = radios_json_string(source, keys[i], "");
        if (!point || JSON_NODE_HOLDS_NULL(point) || (g_strcmp0(by, "manual") && g_strcmp0(by, RADIOS_AUTODJ_POINTS_ALGORITHM))) return TRUE;
    }
    return FALSE;
}
JsonNode *radios_autodj_points_assign_sync(RadioSDataClient *client, JsonObject *row, gboolean overwrite,
    gdouble threshold_db, guint out_lead_ms, const gchar *playlist_id, GCancellable *cancel, GError **error)
{
    JsonObject *media = object_at(row, "media"), *version = object_at(row, "version"), *markers = object_at(row, "markers");
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); JsonObject *request = json_object_new(); json_node_take_object(body, request);
    json_object_set_string_member(request, "station_id", radios_data_client_station_id(client));
    json_object_set_string_member(request, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_string_member(request, "version_id", radios_json_string(version, "_id", ""));
    json_object_set_string_member(request, "media_sha256", radios_json_string(version, "sha256", ""));
    json_object_set_int_member(request, "expected_revision", radios_json_int(media, "_rev", 0));
    if (markers) json_object_set_int_member(request, "expected_marker_revision", radios_json_int(markers, "_rev", 0));
    else json_object_set_null_member(request, "expected_marker_revision");
    gchar *key = g_uuid_string_random(); json_object_set_string_member(request, "idempotency_key", key); g_free(key);
    json_object_set_boolean_member(request, "overwrite", overwrite); json_object_set_double_member(request, "threshold_db", threshold_db);
    json_object_set_int_member(request, "out_lead_ms", out_lead_ms);
    if (playlist_id) json_object_set_string_member(request, "playlist_id", playlist_id);
    JsonNode *result = radios_data_client_request_cancellable_sync(client, "POST", "/v1/workflows/desktop-autodj/markers-assign", body, NULL, cancel, error);
    json_node_free(body);
    if (result && !JSON_NODE_HOLDS_OBJECT(result)) {
        json_node_free(result); result = NULL;
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Auto DJ için geçerli veritabanı yanıtı gerekli.");
    }
    return result;
}
static gboolean connection_error(const GError *error)
{
    return error && ((error->domain == G_IO_ERROR && (error->code == G_IO_ERROR_TIMED_OUT || error->code == G_IO_ERROR_CONNECTION_CLOSED ||
        error->code == G_IO_ERROR_HOST_UNREACHABLE || error->code == G_IO_ERROR_CANCELLED)) ||
        (error->domain == RADIOS_DATA_CLIENT_ERROR && error->code != RADIOS_DATA_CLIENT_ERROR_CONFLICT && error->code != RADIOS_DATA_CLIENT_ERROR_VALIDATION));
}
/* One marker pass. `job->workers` tracks are assigned at the same time
 * (Ayarlar › Kütüphane analizi); `lock` guards the counters and messages. */
typedef struct {
    MarkerJob *job;
    GTask *task;
    GCancellable *cancel;
    GPtrArray *ids;
    GMutex lock;
    guint updated, unchanged, skipped, failed, completed, total;
    gchar *first, *skip_reason;
    gint stopped; /* a connection error ended the pass */
} MarkerPass;
static void marker_track(guint index, gpointer data)
{
    MarkerPass *pass = data; MarkerJob *job = pass->job;
    if (g_atomic_int_get(&pass->stopped)) return;
    const gchar *id = g_ptr_array_index(pass->ids, index);
    GError *error = NULL; JsonNode *detail = NULL; gchar *skip_reason = NULL;
    guint updated = 0, unchanged = 0, skipped = 0;
    /* Workers commit to the same store, and a commit that lands between
     * another assignment's preparation and its own rejects that one with a
     * conflict: the track is read again and retried a few times. */
    for (guint attempt = 0; ; ++attempt) {
        g_clear_error(&error); g_clear_pointer(&detail, json_node_free);
        JsonNode *body = body_new(job);
        json_object_set_string_member(json_node_get_object(body), "media_id", id);
        detail = call(job, "/v1/workflows/desktop-media/get", body, pass->cancel, &error); json_node_free(body);
        JsonObject *record = detail ? json_node_get_object(detail) : NULL, *media = object_at(record, "media");
        if (!record) break;
        if (g_strcmp0(radios_json_string(media, "kind", ""), "music")) { ++skipped; break; }
        JsonNode *saved = radios_autodj_points_assign_sync(job->client, record, job->overwrite, job->threshold, job->lead,
            job->playlist_id, pass->cancel, &error);
        if (saved) {
            JsonObject *response = json_node_get_object(saved); const gchar *status = radios_json_string(response, "status", "");
            if (!g_strcmp0(status, "updated")) {
                if (marker_ack_valid(response, record, id, TRUE)) ++updated;
                else g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Kaydedilen IN/OUT yanıtının parça kimliği veya revizyonu eşleşmedi.");
            } else if (!g_strcmp0(status, "skipped") && !g_strcmp0(radios_json_string(response, "reason", ""), "unchanged")) {
                if (marker_ack_valid(response, record, id, FALSE)) ++unchanged;
                else g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Yeniden hesaplanan IN/OUT yanıtının parça kimliği veya korunan noktaları eşleşmedi.");
            } else if (!g_strcmp0(status, "skipped")) {
                ++skipped;
                if (*radios_json_string(response, "reason_text", "")) skip_reason = g_strdup(radios_json_string(response, "reason_text", ""));
            } else g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Auto DJ işlem sonucu geçersiz.");
            json_node_free(saved);
            break;
        }
        if (!g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CONFLICT) || attempt + 1 >= 6 ||
            g_cancellable_is_cancelled(pass->cancel)) break;
        g_usleep((gulong)g_random_int_range(20000, 100000) * (attempt + 1));
    }
    gboolean stop = connection_error(error);
    g_mutex_lock(&pass->lock);
    pass->updated += updated; pass->unchanged += unchanged; pass->skipped += skipped;
    if (error) { ++pass->failed; if (!pass->first) pass->first = g_strdup(error->message); }
    if (skip_reason && !pass->skip_reason) { pass->skip_reason = skip_reason; skip_reason = NULL; }
    ++pass->completed; progress_post(pass->task, pass->completed, pass->total);
    g_mutex_unlock(&pass->lock);
    radios_library_analysis_activity_step();
    if (stop) g_atomic_int_set(&pass->stopped, 1);
    g_free(skip_reason); g_clear_error(&error); g_clear_pointer(&detail, json_node_free);
}
static void marker_work(GTask *task, gpointer unused, gpointer data, GCancellable *cancel)
{
    (void)unused; MarkerJob *job = data; GError *error = NULL; guint omitted = 0;
    GPtrArray *ids = targets(job, &omitted, cancel, &error);
    if (!ids) { g_task_return_error(task, error); return; }
    MarkerPass pass = {.job = job, .task = task, .cancel = cancel, .ids = ids, .skipped = omitted, .completed = omitted, .total = ids->len + omitted};
    g_mutex_init(&pass.lock);
    progress_post(task, pass.completed, pass.total);
    radios_library_analysis_activity_begin(job->workers);
    radios_library_analysis_activity_add_total(ids->len);
    radios_library_analysis_run(ids->len, job->workers, cancel, marker_track, &pass);
    radios_library_analysis_activity_end();
    g_ptr_array_unref(ids);
    JsonNode *result = json_node_new(JSON_NODE_OBJECT); JsonObject *summary = json_object_new(); json_node_take_object(result, summary);
    json_object_set_int_member(summary, "updated", pass.updated); json_object_set_int_member(summary, "unchanged", pass.unchanged);
    json_object_set_int_member(summary, "skipped", pass.skipped);
    json_object_set_int_member(summary, "failed", pass.failed); json_object_set_int_member(summary, "completed", pass.completed); json_object_set_int_member(summary, "total", pass.total);
    json_object_set_boolean_member(summary, "cancelled", g_cancellable_is_cancelled(cancel)); json_object_set_string_member(summary, "first_error", pass.first ? pass.first : "");
    json_object_set_string_member(summary, "first_skip_reason", pass.skip_reason ? pass.skip_reason : "");
    g_free(pass.first); g_free(pass.skip_reason); g_mutex_clear(&pass.lock);
    g_task_return_pointer(task, result, (GDestroyNotify)json_node_free);
}
static void marker_free(gpointer data)
{
    MarkerJob *job = data; if (job->destroy) job->destroy(job->user);
    radios_data_client_unref(job->client); g_weak_ref_clear(&job->owner); g_weak_ref_clear(&job->parent);
    g_main_context_unref(job->context); g_free(job->playlist_id); g_free(job);
}
static void marker_done(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)source; (void)unused; GTask *task = G_TASK(result); MarkerJob *job = g_task_get_task_data(task); GError *error = NULL;
    g_atomic_int_set(&job->finished, 1); JsonNode *node = g_task_propagate_pointer(task, &error); GtkWidget *owner = live_owner(job);
    if (owner && job->callback) job->callback(job->ctx, owner, node, error, job->user);
    g_clear_object(&owner); g_clear_pointer(&node, json_node_free); g_clear_error(&error);
}
GCancellable *radios_autodj_markers_scan_async(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *playlist_id, gboolean overwrite, gdouble threshold_db, guint out_lead_ms,
    RadioSAutoDJMarkersProgress progress, RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{
    RadioSDataClient *client = radios_data_client_acquire(ctx);
    if (!client || !GTK_IS_WIDGET(owner) || (playlist_id && !g_uuid_string_is_valid(playlist_id)) ||
        !isfinite(threshold_db) || threshold_db < -60 || threshold_db > -6 || out_lead_ms > 30000) {
        if (client) radios_data_client_unref(client);
        if (destroy) destroy(user);
        return NULL;
    }
    MarkerJob *job = g_new0(MarkerJob, 1); job->ctx = ctx; job->client = client; job->playlist_id = g_strdup(playlist_id);
    job->overwrite = overwrite; job->threshold = threshold_db; job->lead = out_lead_ms; job->callback = callback;
    job->progress = progress; job->user = user; job->destroy = destroy; job->context = g_main_context_ref_thread_default();
    RadioSLibraryAnalysisSettings analysis;
    radios_library_analysis_settings_current(ctx->config ? ctx->config->project_root : NULL, &analysis);
    job->workers = analysis.workers;
    GtkWidget *parent = ctx->parent ? GTK_WIDGET(ctx->parent) : owner; guard_owner(owner); guard_owner(parent);
    g_weak_ref_init(&job->owner, owner); g_weak_ref_init(&job->parent, parent);
    GCancellable *cancel = g_cancellable_new(); GTask *task = g_task_new(NULL, cancel, marker_done, NULL); g_task_set_check_cancellable(task, FALSE);
    g_signal_connect_object(owner, "destroy", G_CALLBACK(cancel_owner), cancel, 0);
    g_signal_connect_object(parent, "destroy", G_CALLBACK(cancel_owner), cancel, 0);
    g_task_set_task_data(task, job, marker_free); g_task_run_in_thread(task, marker_work); g_object_unref(task); return cancel;
}
