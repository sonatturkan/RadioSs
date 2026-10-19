#include "radios/media_ui.h"
#include "radios/media.h"
#include "radios/programme_model.h"
#include "radios/autodj_marker_jobs.h"
#include "radios/library_analysis.h"
#include <string.h>

typedef enum { RHYTHM_ENSURE, RHYTHM_ANALYZE, RHYTHM_SAVE, RHYTHM_LIBRARY } RhythmKind;
typedef struct {
    RadioSViewContext *ctx;
    RadioSDataClient *client;
    GWeakRef owner, parent;
    gchar *media_id;
    RadioSDataCallback callback;
    gpointer user;
    GDestroyNotify destroy;
    RhythmKind kind;
    JsonNode *request;
    GHashTable *settled; /* library pass: media whose AutoDJ points cannot be calculated */
    guint workers;       /* library pass: tracks analysed at the same time */
} RhythmJob;
static void rhythm_owner_destroyed(GtkWidget *widget, gpointer unused)
{ (void)unused; g_object_set_data(G_OBJECT(widget), "radios-rhythm-destroyed", GINT_TO_POINTER(1)); }
static void rhythm_owner_guard(GtkWidget *widget)
{
    if (g_object_get_data(G_OBJECT(widget), "radios-rhythm-guard")) return;
    g_object_set_data(G_OBJECT(widget), "radios-rhythm-guard", GINT_TO_POINTER(1));
    g_signal_connect(widget, "destroy", G_CALLBACK(rhythm_owner_destroyed), NULL);
}
static void rhythm_cancel(GtkWidget *widget, GCancellable *cancel)
{ (void)widget; g_cancellable_cancel(cancel); }
static void rhythm_job_free(gpointer data)
{
    RhythmJob *job = data;
    if (job->destroy) job->destroy(job->user);
    g_weak_ref_clear(&job->owner); g_weak_ref_clear(&job->parent);
    radios_data_client_unref(job->client); g_free(job->media_id);
    g_clear_pointer(&job->request, json_node_free); g_clear_pointer(&job->settled, g_hash_table_unref); g_free(job);
}
static JsonObject *analysis_of(JsonObject *detail)
{
    JsonNode *analysis = detail ? json_object_get_member(detail, "analysis") : NULL;
    return analysis && JSON_NODE_HOLDS_OBJECT(analysis) ? json_node_get_object(analysis) : NULL;
}
static JsonObject *rhythm_of(JsonObject *detail)
{
    JsonObject *analysis = analysis_of(detail);
    JsonNode *rhythm = analysis ? json_object_get_member(analysis, "rhythm") : NULL;
    return rhythm && JSON_NODE_HOLDS_OBJECT(rhythm) ? json_node_get_object(rhythm) : NULL;
}
static gboolean wait_cancellable(GCancellable *cancel, guint milliseconds);
/* A library pass shares the service's request budget with the operator. A
 * rate-limited request was rejected before any change, so it waits the
 * server's Retry-After and is sent again unchanged: a large import is
 * analysed in one pass instead of leaving songs failed until the next one. */
#define RATE_LIMIT_RETRIES 60u
static gboolean rate_limited_wait(RadioSDataClient *client, GCancellable *cancel, const GError *error, guint *attempts)
{
    if (!g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED) || ++*attempts > RATE_LIMIT_RETRIES) return FALSE;
    return wait_cancellable(cancel, CLAMP(radios_data_client_retry_delay(client), 1u, 30u) * 1000u);
}
static JsonNode *request_paced(RadioSDataClient *client, const gchar *path, JsonNode *body, GCancellable *cancel, GError **error)
{
    GError *local = NULL; guint attempts = 0;
    for (;;) {
        g_clear_error(&local);
        JsonNode *result = radios_data_client_request_cancellable_sync(client, "POST", path, body, NULL, cancel, &local);
        if (result || !rate_limited_wait(client, cancel, local, &attempts)) {
            if (local) g_propagate_error(error, local);
            return result;
        }
    }
}
static JsonNode *media_get(RadioSDataClient *client, const gchar *media_id, GCancellable *cancel, GError **error)
{
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); JsonObject *request = json_object_new(); json_node_take_object(body, request);
    json_object_set_string_member(request, "station_id", radios_data_client_station_id(client));
    json_object_set_string_member(request, "media_id", media_id);
    JsonNode *detail = request_paced(client, "/v1/workflows/desktop-media/get", body, cancel, error);
    json_node_free(body);
    if (detail && !JSON_NODE_HOLDS_OBJECT(detail)) {
        json_node_free(detail); detail = NULL;
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Medya analizi için geçerli kayıt gerekli.");
    }
    return detail;
}
/* Saves `analysis` (consumed) for the stored version in `detail`. A conflicting
 * save by another player or the library scan counts as done when the stored
 * grid is now current, so concurrent analysis never surfaces as an operator
 * error. A commit by another worker between this save's preparation and its
 * commit is rejected as a conflict too: the record is read again and the save
 * repeated, as long as it is still the same version (and, for an operator's
 * edit, the same revisions). */
static JsonNode *analysis_save(RadioSDataClient *client, JsonObject *detail, JsonNode *analysis, gboolean accept_current,
    GCancellable *cancel, GError **error)
{
    JsonObject *first_media = json_object_get_object_member(detail, "media"), *first_version = json_object_get_object_member(detail, "version");
    gchar *media_id = g_strdup(radios_json_string(first_media, "_id", "")), *version_id = g_strdup(radios_json_string(first_version, "_id", ""));
    gchar *sha = g_strdup(radios_json_string(first_version, "sha256", ""));
    gint64 first_revision = radios_json_int(first_media, "_rev", 0), first_analysis_revision = radios_json_int(detail, "analysis_revision", 0);
    JsonObject *source = detail; JsonNode *fresh = NULL, *result = NULL; GError *local = NULL;
    for (guint attempt = 0; ; ++attempt) {
        JsonObject *media = json_object_get_object_member(source, "media");
        JsonNode *body = json_node_new(JSON_NODE_OBJECT); JsonObject *request = json_object_new(); json_node_take_object(body, request);
        json_object_set_string_member(request, "station_id", radios_data_client_station_id(client));
        json_object_set_string_member(request, "media_id", media_id);
        json_object_set_string_member(request, "version_id", version_id);
        json_object_set_int_member(request, "expected_revision", radios_json_int(media, "_rev", 0));
        json_object_set_int_member(request, "expected_analysis_revision", radios_json_int(source, "analysis_revision", 0));
        gchar *key = g_uuid_string_random(); json_object_set_string_member(request, "idempotency_key", key); g_free(key);
        json_object_set_member(request, "analysis", json_node_copy(analysis));
        g_clear_error(&local);
        result = request_paced(client, "/v1/workflows/desktop-media/analysis-save", body, cancel, &local);
        json_node_free(body);
        if (result || !g_error_matches(local, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CONFLICT) ||
            attempt + 1 >= 6 || g_cancellable_is_cancelled(cancel)) break;
        g_usleep((gulong)g_random_int_range(20000, 100000) * (attempt + 1));
        g_clear_pointer(&fresh, json_node_free);
        fresh = media_get(client, media_id, cancel, NULL);
        if (!fresh) break;
        JsonObject *latest = json_node_get_object(fresh);
        JsonObject *latest_media = json_object_get_object_member(latest, "media"), *latest_version = json_object_get_object_member(latest, "version");
        if (accept_current && radios_media_analysis_complete(analysis_of(latest))) { g_clear_error(&local); result = fresh; fresh = NULL; break; }
        if (g_strcmp0(radios_json_string(latest_version, "_id", ""), version_id) || g_strcmp0(radios_json_string(latest_version, "sha256", ""), sha)) break;
        if (!accept_current && (radios_json_int(latest_media, "_rev", 0) != first_revision ||
                                radios_json_int(latest, "analysis_revision", 0) != first_analysis_revision)) break;
        source = latest;
    }
    json_node_free(analysis); g_clear_pointer(&fresh, json_node_free);
    g_free(media_id); g_free(version_id); g_free(sha);
    if (!result && local) g_propagate_error(error, local); else g_clear_error(&local);
    return result;
}
/* Reads the stored analysis; decodes and saves when `ensure` is false or the
 * stored grid is not current. Returns the detail or the saved snapshot. */
static JsonNode *rhythm_sync(RadioSDataClient *client, const gchar *media_id, gboolean ensure, GCancellable *cancel, GError **error)
{
    JsonNode *detail = media_get(client, media_id, cancel, error);
    if (!detail) return NULL;
    JsonObject *object = json_node_get_object(detail), *version = json_object_get_object_member(object, "version");
    /* A completed analysis with no reliable BPM is still complete. Avoid
     * repeatedly decoding speech/silence each time a player refreshes. */
    if (ensure && radios_media_analysis_complete(analysis_of(object))) return detail;
    const gchar *path = radios_json_string(version, "managed_path", ""), *sha = radios_json_string(version, "sha256", "");
    RadioSMediaAnalysis *analysis = NULL; GError *local = NULL;
    if (g_path_is_absolute(path) && g_str_equal(radios_json_string(json_object_get_object_member(object, "media"), "station_id", ""), radios_data_client_station_id(client)))
        analysis = radios_media_analyze(path, NULL, cancel, &local);
    else g_set_error_literal(&local, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Doğrulanmış istasyon medyası gerekli.");
    if (analysis && !g_str_equal(analysis->sha256, sha))
        g_set_error_literal(&local, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Analiz sırasında medya kimliği değişti.");
    JsonNode *measured = analysis && !local ? radios_media_analysis_to_json(analysis) : NULL;
    /* Upkeep (a missing loudness measurement) never discards an operator's grid. */
    JsonObject *stored = rhythm_of(object);
    if (measured && ensure && stored && g_str_equal(radios_json_string(stored, "algorithm", ""), RADIOS_RHYTHM_ALGORITHM_MANUAL))
        json_object_set_object_member(json_node_get_object(measured), "rhythm", radios_programme_clone_object(stored));
    JsonNode *saved = measured ? analysis_save(client, object, measured, ensure, cancel, &local) : NULL;
    radios_media_analysis_free(analysis); json_node_free(detail);
    if (local) { g_clear_pointer(&saved, json_node_free); g_propagate_error(error, local); }
    return saved;
}
static gboolean wait_cancellable(GCancellable *cancel, guint milliseconds)
{
    for (guint waited = 0; waited < milliseconds; waited += 50) {
        if (g_cancellable_is_cancelled(cancel)) return FALSE;
        g_usleep(50000);
    }
    return !g_cancellable_is_cancelled(cancel);
}
static gchar *settled_key(JsonObject *row)
{
    return g_strdup_printf("%s:%s", radios_json_string(json_object_get_object_member(row, "media"), "_id", ""),
        radios_json_string(json_object_get_object_member(row, "version"), "sha256", ""));
}
/* One library pass. Several worker threads analyse tracks at the same time
 * (Ayarlar › Kütüphane analizi); `lock` guards the counters, the first error
 * and the sets of tracks whose AutoDJ points cannot be calculated. */
typedef struct {
    RadioSDataClient *client;
    GHashTable *settled;
    GCancellable *cancel;
    JsonArray *records, *newly_settled;
    GMutex lock;
    guint analyzed, failed, unknown, marked;
    GError *first;
} LibraryPass;
static void pass_failed(LibraryPass *pass, GError *error)
{
    g_mutex_lock(&pass->lock);
    ++pass->failed;
    if (!pass->first) pass->first = error; else g_error_free(error);
    g_mutex_unlock(&pass->lock);
}
/* Calculates the AutoDJ points of one music track. A new beat grid also
 * recalculates earlier automatic points; manual points always stay. Tracks
 * the backend cannot mark (silent, unavailable) are remembered as settled.
 * Workers commit to the same store, and a commit that lands between another
 * assignment's preparation and its own rejects that one with a conflict: the
 * track is read again and retried a few times before it is left to the next
 * pass. */
static gboolean library_points(LibraryPass *pass, JsonObject *row, gboolean regrid, GError **error)
{
    gchar *key = settled_key(row);
    g_mutex_lock(&pass->lock);
    gboolean settled = g_hash_table_contains(pass->settled, key);
    g_mutex_unlock(&pass->lock);
    if (settled) { g_free(key); return TRUE; }
    GError *local = NULL; JsonNode *answer = NULL, *fresh = NULL; JsonObject *current = row;
    for (guint attempt = 0; attempt < 6; ++attempt) {
        g_clear_pointer(&answer, json_node_free); g_clear_error(&local);
        guint limited = 0;
        do {
            g_clear_pointer(&answer, json_node_free); g_clear_error(&local);
            answer = radios_autodj_points_assign_sync(pass->client, current, regrid, -45, 0, NULL, pass->cancel, &local);
        } while (!answer && rate_limited_wait(pass->client, pass->cancel, local, &limited));
        if (answer || !g_error_matches(local, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CONFLICT) ||
            g_cancellable_is_cancelled(pass->cancel)) break;
        g_usleep((gulong)g_random_int_range(20000, 100000) * (attempt + 1));
        if (g_cancellable_is_cancelled(pass->cancel)) break;
        JsonNode *reread = media_get(pass->client, radios_json_string(json_object_get_object_member(row, "media"), "_id", ""), pass->cancel, NULL);
        if (!reread) break;
        g_clear_pointer(&fresh, json_node_free); fresh = reread; current = json_node_get_object(fresh);
    }
    JsonObject *response = answer ? json_node_get_object(answer) : NULL;
    const gchar *status = radios_json_string(response, "status", ""), *reason = radios_json_string(response, "reason", "");
    g_mutex_lock(&pass->lock);
    if (!g_strcmp0(status, "updated")) ++pass->marked;
    else if (response && g_strcmp0(reason, "unchanged") && g_strcmp0(reason, "existing_markers")) {
        g_hash_table_add(pass->settled, g_strdup(key)); json_array_add_string_element(pass->newly_settled, key);
    }
    g_mutex_unlock(&pass->lock);
    g_clear_pointer(&answer, json_node_free); g_clear_pointer(&fresh, json_node_free); g_free(key);
    /* A concurrent edit wins; the next pass reads the new revisions. */
    if (g_error_matches(local, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CONFLICT)) g_clear_error(&local);
    if (local) { g_propagate_error(error, local); return FALSE; }
    return TRUE;
}
/* Completes the grid and the AutoDJ points of one library row. */
static void library_track(LibraryPass *pass, JsonObject *row)
{
    JsonObject *media = row ? json_object_get_object_member(row, "media") : NULL;
    JsonObject *version = row ? json_object_get_object_member(row, "version") : NULL;
    const gchar *media_id = radios_json_string(media, "_id", "");
    gboolean verified = version && json_object_has_member(version, "verified") && json_object_get_boolean_member(version, "verified");
    if (!verified || !g_uuid_string_is_valid(media_id)) return;
    gboolean grid_due = !radios_media_analysis_complete(analysis_of(row)), points_due = radios_autodj_points_due(row);
    if (points_due) {
        gchar *key = settled_key(row);
        g_mutex_lock(&pass->lock);
        points_due = !g_hash_table_contains(pass->settled, key);
        g_mutex_unlock(&pass->lock);
        g_free(key);
    }
    if (!grid_due && !points_due) return;
    if (!wait_cancellable(pass->cancel, 150)) return;
    JsonNode *saved = NULL; GError *local = NULL;
    if (grid_due) {
        saved = rhythm_sync(pass->client, media_id, TRUE, pass->cancel, &local);
        if (saved) {
            RadioSRhythmAnalysis grid;
            g_mutex_lock(&pass->lock);
            if (!radios_rhythm_analysis_read(rhythm_of(json_node_get_object(saved)), &grid)) ++pass->unknown;
            ++pass->analyzed;
            g_mutex_unlock(&pass->lock);
        } else if (g_cancellable_is_cancelled(pass->cancel)) {
            g_clear_error(&local); return;
        } else {
            pass_failed(pass, local); return;
        }
    }
    JsonObject *current = saved ? json_node_get_object(saved) : row;
    gboolean music = !g_strcmp0(radios_json_string(json_object_get_object_member(current, "media"), "kind", ""), "music");
    if ((saved && music) || radios_autodj_points_due(current)) {
        if (!library_points(pass, current, saved != NULL, &local)) {
            if (g_cancellable_is_cancelled(pass->cancel)) g_clear_error(&local);
            else pass_failed(pass, local);
        }
    }
    g_clear_pointer(&saved, json_node_free);
}
static void library_record(guint index, gpointer data)
{
    LibraryPass *pass = data;
    JsonNode *node = json_array_get_element(pass->records, index);
    library_track(pass, node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL);
    radios_library_analysis_activity_step();
}
/* Walks the library page by page: completes every grid that is missing or
 * came from an older analyzer (manual grids stay untouched), then gives every
 * music track automatic AutoDJ IN/OUT points where they are missing or stale.
 * Points the operator placed are never replaced. `workers` tracks are
 * analysed at the same time. */
static JsonNode *library_scan_pages(RadioSDataClient *client, GHashTable *settled, guint workers, GCancellable *cancel, GError **error)
{
    LibraryPass pass = {.client = client, .settled = settled, .cancel = cancel, .newly_settled = json_array_new()};
    g_mutex_init(&pass.lock);
    gint64 offset = 0; GError *failure = NULL; gboolean cancelled = FALSE;
    for (;;) {
        JsonNode *body = json_node_new(JSON_NODE_OBJECT); JsonObject *request = json_object_new(); json_node_take_object(body, request);
        json_object_set_string_member(request, "station_id", radios_data_client_station_id(client));
        json_object_set_int_member(request, "limit", 200); json_object_set_int_member(request, "offset", offset);
        GError *local = NULL;
        JsonNode *page = radios_data_client_request_cancellable_sync(client, "POST", "/v1/workflows/desktop-media/list", body, NULL, cancel, &local);
        json_node_free(body);
        if (!page || !JSON_NODE_HOLDS_OBJECT(page)) {
            g_clear_pointer(&page, json_node_free);
            if (!local) g_set_error_literal(&local, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Medya listesi okunamadı.");
            failure = local; break;
        }
        JsonObject *object = json_node_get_object(page);
        if (!offset) radios_library_analysis_activity_add_total((guint)MAX(0, radios_json_int(object, "total", 0)));
        JsonNode *records_node = json_object_get_member(object, "records");
        pass.records = records_node && JSON_NODE_HOLDS_ARRAY(records_node) ? json_node_get_array(records_node) : NULL;
        if (pass.records) radios_library_analysis_run(json_array_get_length(pass.records), workers, cancel, library_record, &pass);
        pass.records = NULL;
        JsonNode *next = json_object_get_member(object, "next_offset");
        gint64 next_offset = next && JSON_NODE_HOLDS_VALUE(next) && json_node_get_value_type(next) == G_TYPE_INT64 ? json_node_get_int(next) : -1;
        json_node_free(page);
        if (g_cancellable_is_cancelled(cancel)) { cancelled = TRUE; break; }
        if (next_offset <= offset) break;
        offset = next_offset;
    }
    JsonNode *result = NULL;
    if (failure) g_propagate_error(error, failure);
    else if (cancelled) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "İptal edildi.");
    else {
        result = json_node_new(JSON_NODE_OBJECT); JsonObject *summary = json_object_new(); json_node_take_object(result, summary);
        json_object_set_int_member(summary, "analyzed", pass.analyzed); json_object_set_int_member(summary, "unknown", pass.unknown);
        json_object_set_int_member(summary, "failed", pass.failed); json_object_set_int_member(summary, "marked", pass.marked);
        json_object_set_array_member(summary, "settled", pass.newly_settled); pass.newly_settled = NULL;
        json_object_set_string_member(summary, "first_error", pass.first ? pass.first->message : "");
    }
    g_clear_pointer(&pass.newly_settled, json_array_unref); g_clear_error(&pass.first); g_mutex_clear(&pass.lock);
    return result;
}
static JsonNode *library_scan(RadioSDataClient *client, GHashTable *settled, guint workers, GCancellable *cancel, GError **error)
{
    radios_library_analysis_activity_begin(workers);
    JsonNode *result = library_scan_pages(client, settled, workers, cancel, error);
    radios_library_analysis_activity_end();
    return result;
}
static void rhythm_work(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; RhythmJob *job = data; GError *error = NULL; JsonNode *result = NULL;
    if (job->kind == RHYTHM_LIBRARY) result = library_scan(job->client, job->settled, job->workers, cancel, &error);
    else if (job->kind == RHYTHM_SAVE) {
        JsonNode *detail = media_get(job->client, job->media_id, cancel, &error);
        JsonObject *object = detail ? json_node_get_object(detail) : NULL, *requested = json_node_get_object(job->request);
        JsonObject *stored = object ? json_object_get_object_member(object, "analysis") : NULL;
        JsonObject *media = object ? json_object_get_object_member(object, "media") : NULL;
        /* The grid belongs to the version and analysis revision the operator
         * edited; a newer analysis or media change must be reviewed first. */
        if (object && (!stored || !g_str_equal(radios_json_string(media, "version_id", ""), radios_json_string(requested, "version_id", "!")) ||
                radios_json_int(object, "analysis_revision", -1) != radios_json_int(requested, "analysis_revision", -2)))
            g_set_error_literal(&error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CONFLICT,
                "Parçanın analizi bu sırada değişti. Izgarayı yeniden açıp tekrar kaydedin.");
        if (object && !error) {
            JsonObject *analysis = radios_programme_clone_object(stored);
            json_object_set_object_member(analysis, "rhythm", radios_programme_clone_object(json_object_get_object_member(requested, "rhythm")));
            JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, analysis);
            result = analysis_save(job->client, object, node, FALSE, cancel, &error);
        }
        g_clear_pointer(&detail, json_node_free);
    } else result = rhythm_sync(job->client, job->media_id, job->kind == RHYTHM_ENSURE, cancel, &error);
    if (error) { g_clear_pointer(&result, json_node_free); g_task_return_error(task, error); }
    else g_task_return_pointer(task, result, (GDestroyNotify)json_node_free);
}
static void rhythm_done(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)source; (void)unused; GTask *task = G_TASK(result); RhythmJob *job = g_task_get_task_data(task);
    GError *error = NULL; JsonNode *node = g_task_propagate_pointer(task, &error);
    GtkWidget *owner = g_weak_ref_get(&job->owner), *parent = g_weak_ref_get(&job->parent);
    if (owner && parent && !g_object_get_data(G_OBJECT(owner), "radios-rhythm-destroyed") && !g_object_get_data(G_OBJECT(parent), "radios-rhythm-destroyed")) {
        RadioSDataClient *current = radios_data_client_acquire(job->ctx);
        if (current == job->client && job->callback) job->callback(job->ctx, owner, node, error, job->user);
        if (current) radios_data_client_unref(current);
    }
    g_clear_object(&owner); g_clear_object(&parent); g_clear_pointer(&node, json_node_free); g_clear_error(&error);
}
static GCancellable *rhythm_start_settled(RadioSViewContext *ctx, GtkWidget *owner, const gchar *media_id,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy, RhythmKind kind, JsonNode *request, GHashTable *settled);
static GCancellable *rhythm_start(RadioSViewContext *ctx, GtkWidget *owner, const gchar *media_id,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy, RhythmKind kind, JsonNode *request)
{ return rhythm_start_settled(ctx, owner, media_id, callback, user, destroy, kind, request, NULL); }
/* `settled` (library passes only) is copied; NULL starts empty. */
static GCancellable *rhythm_start_settled(RadioSViewContext *ctx, GtkWidget *owner, const gchar *media_id,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy, RhythmKind kind, JsonNode *request, GHashTable *settled)
{
    RadioSDataClient *client = radios_data_client_acquire(ctx);
    if (!client || !owner || (kind != RHYTHM_LIBRARY && !g_uuid_string_is_valid(media_id))) {
        if (client) radios_data_client_unref(client);
        if (destroy) destroy(user);
        if (request) json_node_free(request);
        return NULL;
    }
    RhythmJob *job = g_new0(RhythmJob, 1); job->ctx = ctx; job->client = client; job->media_id = g_strdup(media_id);
    GtkWidget *parent = ctx->parent ? GTK_WIDGET(ctx->parent) : owner;
    rhythm_owner_guard(owner); rhythm_owner_guard(parent);
    job->callback = callback; job->user = user; job->destroy = destroy; job->kind = kind; job->request = request;
    if (kind == RHYTHM_LIBRARY) {
        RadioSLibraryAnalysisSettings analysis;
        radios_library_analysis_settings_current(ctx->config ? ctx->config->project_root : NULL, &analysis);
        job->workers = analysis.workers;
        job->settled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        GHashTableIter iter; gpointer key;
        if (settled) { g_hash_table_iter_init(&iter, settled); while (g_hash_table_iter_next(&iter, &key, NULL)) g_hash_table_add(job->settled, g_strdup(key)); }
    }
    g_weak_ref_init(&job->owner, owner); g_weak_ref_init(&job->parent, parent);
    GCancellable *cancel = g_cancellable_new(); GTask *task = g_task_new(NULL, cancel, rhythm_done, NULL);
    g_signal_connect_object(owner, "destroy", G_CALLBACK(rhythm_cancel), cancel, 0);
    g_task_set_task_data(task, job, rhythm_job_free); g_task_run_in_thread(task, rhythm_work); g_object_unref(task); return cancel;
}
GCancellable *radios_media_rhythm_analyze_async(RadioSViewContext *ctx, GtkWidget *owner, const gchar *media_id,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{ return rhythm_start(ctx, owner, media_id, callback, user, destroy, RHYTHM_ANALYZE, NULL); }
GCancellable *radios_media_rhythm_ensure_async(RadioSViewContext *ctx, GtkWidget *owner, const gchar *media_id,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{ return rhythm_start(ctx, owner, media_id, callback, user, destroy, RHYTHM_ENSURE, NULL); }
GCancellable *radios_media_rhythm_save_async(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail,
    const RadioSRhythmAnalysis *grid, RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{
    JsonObject *media = detail ? json_object_get_object_member(detail, "media") : NULL;
    if (!media || !grid) { if (destroy) destroy(user); return NULL; }
    JsonNode *request = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(request, object);
    json_object_set_string_member(object, "version_id", radios_json_string(media, "version_id", ""));
    json_object_set_int_member(object, "analysis_revision", radios_json_int(detail, "analysis_revision", 0));
    json_object_set_object_member(object, "rhythm", radios_rhythm_analysis_json(grid));
    return rhythm_start(ctx, owner, radios_json_string(media, "_id", ""), callback, user, destroy, RHYTHM_SAVE, request);
}
GCancellable *radios_media_rhythm_library_scan_async(RadioSViewContext *ctx, GtkWidget *owner,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{ return rhythm_start(ctx, owner, NULL, callback, user, destroy, RHYTHM_LIBRARY, NULL); }

/* Background upkeep: one library pass shortly after the database connects,
 * another once the library has been quiet for the configured time after an
 * import batch (the songs are taken in first and then analysed together),
 * otherwise at the configured interval; a reconnect to another client
 * restarts it. Everything follows Ayarlar › Kütüphane analizi
 * (config/library-analysis.ini): parallel workers, automatic scans on or
 * off, quiet time, interval. */
typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *owner;
    GCancellable *running;
    RadioSDataClient *client;
    gint64 next_run, quiet_until;
    guint timer, generation;
    gboolean reported_failure, run_now;
    GHashTable *settled; /* media:sha whose AutoDJ points cannot be calculated */
    gchar *last_result;  /* one line for the settings page */
} LibraryUpkeep;
static void upkeep_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *error, gpointer data)
{
    (void)owner; LibraryUpkeep *upkeep = data;
    g_clear_object(&upkeep->running);
    g_free(upkeep->last_result); upkeep->last_result = NULL;
    if (error) {
        upkeep->last_result = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ?
            g_strdup("Tarama durduruldu; tamamlanan parçalar korunur.") : g_strdup_printf("Tarama tamamlanamadı: %s", error->message);
        return;
    }
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) return;
    JsonObject *summary = json_node_get_object(node);
    gint64 analyzed = radios_json_int(summary, "analyzed", 0), unknown = radios_json_int(summary, "unknown", 0), failed = radios_json_int(summary, "failed", 0);
    gint64 marked = radios_json_int(summary, "marked", 0);
    upkeep->last_result = g_strdup_printf("Son tarama: %" G_GINT64_FORMAT " parçada ritim/BPM, %" G_GINT64_FORMAT " parçada AutoDJ IN/OUT hesaplandı, %" G_GINT64_FORMAT " parça başarısız.",
        analyzed, marked, failed);
    JsonNode *settled_node = json_object_get_member(summary, "settled");
    JsonArray *settled = settled_node && JSON_NODE_HOLDS_ARRAY(settled_node) ? json_node_get_array(settled_node) : NULL;
    for (guint i = 0; settled && i < json_array_get_length(settled); ++i) {
        JsonNode *item = json_array_get_element(settled, i);
        if (JSON_NODE_HOLDS_VALUE(item) && json_node_get_value_type(item) == G_TYPE_STRING)
            g_hash_table_add(upkeep->settled, g_strdup(json_node_get_string(item)));
    }
    if (marked && ctx->notify) {
        gchar *text = g_strdup_printf("AutoDJ IN/OUT: %" G_GINT64_FORMAT " parçaya otomatik geçiş noktası atandı (elle konan noktalar korunur).", marked);
        ctx->notify(ctx->notify_data, text); g_free(text);
    }
    if (marked) radios_data_refresh(ctx);
    if (analyzed && ctx->notify) {
        gchar *text = unknown ? g_strdup_printf("BPM analizi: %" G_GINT64_FORMAT " parça güncellendi; %" G_GINT64_FORMAT " parçada güvenilir tempo bulunamadı (BPM'i oynatıcıdan elle girebilirsiniz).", analyzed, unknown) :
            g_strdup_printf("BPM analizi: %" G_GINT64_FORMAT " parça güncellendi.", analyzed);
        ctx->notify(ctx->notify_data, text); g_free(text);
    }
    if (failed && !upkeep->reported_failure && ctx->notify) {
        gchar *text = g_strdup_printf("BPM analizi %" G_GINT64_FORMAT " parçada tamamlanamadı: %s", failed, radios_json_string(summary, "first_error", ""));
        ctx->notify(ctx->notify_data, text); g_free(text); upkeep->reported_failure = TRUE;
    }
}
static gboolean upkeep_tick(gpointer data)
{
    LibraryUpkeep *upkeep = data;
    RadioSDataClient *client = radios_data_client_acquire(upkeep->ctx);
    gint64 now = g_get_monotonic_time();
    if (client != upkeep->client) {
        if (upkeep->running) g_cancellable_cancel(upkeep->running);
        g_clear_object(&upkeep->running);
        if (upkeep->client) radios_data_client_unref(upkeep->client);
        upkeep->client = client ? radios_data_client_ref(client) : NULL;
        upkeep->next_run = now + 20 * G_TIME_SPAN_SECOND; upkeep->reported_failure = FALSE;
    }
    RadioSLibraryAnalysisSettings analysis;
    radios_library_analysis_settings_current(upkeep->ctx->config ? upkeep->ctx->config->project_root : NULL, &analysis);
    guint generation = radios_media_catalogue_generation(upkeep->ctx);
    if (generation != upkeep->generation) {
        upkeep->generation = generation;
        /* First the songs are taken in, then analysed: every import batch
         * restarts the quiet time before the scan. */
        upkeep->quiet_until = now + (gint64)analysis.settle_seconds * G_TIME_SPAN_SECOND;
        upkeep->next_run = MIN(upkeep->next_run, upkeep->quiet_until);
    }
    gboolean due = upkeep->run_now || (analysis.auto_scan && now >= upkeep->next_run && now >= upkeep->quiet_until);
    if (client && !upkeep->running && due &&
        !g_object_get_data(G_OBJECT(upkeep->ctx->parent), "radios-media-import-batch") && radios_data_connected(upkeep->ctx)) {
        upkeep->run_now = FALSE;
        upkeep->next_run = now + (gint64)analysis.interval_minutes * G_TIME_SPAN_MINUTE;
        upkeep->running = rhythm_start_settled(upkeep->ctx, upkeep->owner, NULL, upkeep_done, upkeep, NULL, RHYTHM_LIBRARY, NULL, upkeep->settled);
    }
    if (client) radios_data_client_unref(client);
    return G_SOURCE_CONTINUE;
}
static void upkeep_free(gpointer data)
{
    LibraryUpkeep *upkeep = data;
    if (upkeep->timer) g_source_remove(upkeep->timer);
    if (upkeep->running) g_cancellable_cancel(upkeep->running);
    g_clear_object(&upkeep->running);
    if (upkeep->client) radios_data_client_unref(upkeep->client);
    g_clear_pointer(&upkeep->settled, g_hash_table_unref);
    g_free(upkeep->last_result);
    g_free(upkeep);
}
void radios_media_rhythm_library_bind(RadioSViewContext *ctx, GtkWidget *owner)
{
    g_return_if_fail(ctx && GTK_IS_WIDGET(owner));
    if (g_object_get_data(G_OBJECT(owner), "radios-rhythm-upkeep")) return;
    LibraryUpkeep *upkeep = g_new0(LibraryUpkeep, 1); upkeep->ctx = ctx; upkeep->owner = owner;
    upkeep->settled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    upkeep->generation = radios_media_catalogue_generation(ctx);
    upkeep->timer = g_timeout_add_seconds(5, upkeep_tick, upkeep);
    g_object_set_data_full(G_OBJECT(owner), "radios-rhythm-upkeep", upkeep, upkeep_free);
}
static LibraryUpkeep *upkeep_of(RadioSViewContext *ctx)
{ return ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-rhythm-upkeep") : NULL; }
gboolean radios_media_rhythm_library_run_now(RadioSViewContext *ctx)
{
    LibraryUpkeep *upkeep = upkeep_of(ctx);
    if (!upkeep || upkeep->running || !radios_data_connected(ctx)) return FALSE;
    upkeep->run_now = TRUE;
    upkeep_tick(upkeep); /* start at once instead of on the next five-second tick */
    return TRUE;
}
void radios_media_rhythm_library_cancel(RadioSViewContext *ctx)
{
    LibraryUpkeep *upkeep = upkeep_of(ctx);
    if (upkeep && upkeep->running) g_cancellable_cancel(upkeep->running);
}
gchar *radios_media_rhythm_library_status(RadioSViewContext *ctx, gboolean *running)
{
    LibraryUpkeep *upkeep = upkeep_of(ctx);
    if (running) *running = upkeep && upkeep->running != NULL;
    return g_strdup(upkeep && upkeep->last_result ? upkeep->last_result : "");
}
