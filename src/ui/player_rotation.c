#include "radios/player_rotation.h"
#include "radios/automation_calendar_plan.h"
#include "radios/playout.h"
#include "radios/playout_ui.h"
#include "radios/audio_ui.h"
#include "radios/weekly_programme_plan.h"
#include "radios/programme_model.h"
#include "radios/widgets.h"
#include "radios/broadcast_header.h"
#include "radios/player_workspace.h"
#include "radios/platform.h"

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *dialog, *list, *preview, *status, *load;
    RadioSDataClient *client;
    JsonNode *record, *prepared;
    gchar *document_kind, *document_id, *rotation_id, *rotation_name, *first_playlist_id;
    gint64 revision;
    guint generation, pending_lists;
    gboolean rendering, closed, accepting, catalogue_failed;
    RadioSPlayerRotationDone done;
    gpointer user;
} RotationLoader;

typedef struct {
    gint references;
    RadioSViewContext *ctx;
    RadioSDataClient *client;
    GWeakRef owner;
    gboolean modal, completed, handoff;
    guint generation;
    gchar *document_kind, *document_id, *rotation_id, *rotation_name, *first_playlist_id;
    gint64 revision;
    JsonNode *event, *selection, *prepared;
    RadioSDataCallback callback;
    gpointer user;
    GDestroyNotify destroy;
} RotationJob;

static JsonObject *object(JsonObject *parent, const gchar *name)
{
    JsonNode *node = parent ? json_object_get_member(parent, name) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array(JsonObject *parent, const gchar *name)
{
    JsonNode *node = parent ? json_object_get_member(parent, name) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static JsonObject *at(JsonArray *items, guint index)
{
    JsonNode *node = items && index < json_array_get_length(items) ? json_array_get_element(items, index) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static const gchar *text(JsonObject *parent, const gchar *name)
{ return radios_json_string(parent, name, ""); }
static JsonNode *copy_object(JsonObject *source)
{
    if (!source) return NULL;
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, radios_programme_clone_object(source)); return node;
}
static RotationLoader *loader_get(GtkWidget *dialog)
{ return g_object_get_data(G_OBJECT(dialog), "radios-player-rotation"); }
static gboolean same_client(RadioSViewContext *ctx, RadioSDataClient *anchor)
{
    RadioSDataClient *current = radios_data_client_acquire(ctx);
    gboolean same = anchor && current == anchor && radios_data_connected(ctx);
    if (current) radios_data_client_unref(current);
    return same;
}
static RotationJob *job_ref(RotationJob *job) { ++job->references; return job; }
static void job_free(gpointer value)
{
    RotationJob *job = value;
    if (--job->references) return;
    if (job->destroy) job->destroy(job->user);
    radios_data_client_unref(job->client); g_weak_ref_clear(&job->owner);
    g_free(job->document_kind); g_free(job->document_id); g_free(job->rotation_id); g_free(job->rotation_name); g_free(job->first_playlist_id);
    g_clear_pointer(&job->event, json_node_free);
    g_clear_pointer(&job->selection, json_node_free);
    g_clear_pointer(&job->prepared, json_node_free);
    g_free(job);
}
static RotationJob *job_new(RadioSViewContext *ctx, GtkWidget *owner, RadioSDataClient *client)
{
    RotationJob *job = g_new0(RotationJob, 1); job->references = 1;
    job->ctx = ctx; job->client = radios_data_client_ref(client); g_weak_ref_init(&job->owner, owner); return job;
}
static gboolean job_current(RotationJob *job, GtkWidget *owner)
{
    if (job->completed || gtk_widget_in_destruction(owner)) return FALSE;
    if (job->modal) {
        RotationLoader *loader = loader_get(owner);
        if (!loader || loader->closed || loader->generation != job->generation) return FALSE;
    }
    return TRUE;
}
static void job_fail(RotationJob *job, GtkWidget *owner, const GError *error, const gchar *fallback)
{
    if (!job_current(job, owner)) return;
    job->completed = TRUE;
    GError *failure = error ? g_error_copy(error) : g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, fallback);
    if (job->modal) {
        RotationLoader *loader = loader_get(owner);
        loader->accepting = FALSE;
        gtk_label_set_text(GTK_LABEL(loader->status), failure->message);
        gtk_widget_set_sensitive(loader->load, FALSE);
    } else if (job->callback) job->callback(job->ctx, owner, NULL, failure, job->user);
    g_error_free(failure);
}
static void job_invalid(RotationJob *job, GtkWidget *owner, const GError *error, const gchar *message)
{
    GError *failure = error ? g_error_copy(error) : g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    job_fail(job, owner, failure, message); g_error_free(failure);
}
static gboolean job_check(RotationJob *job, GtkWidget *owner)
{
    if (!job_current(job, owner)) return FALSE;
    if (same_client(job->ctx, job->client)) return TRUE;
    job_fail(job, owner, NULL, "Veri oturumu değişti. Rotasyonu yeniden yükleyin."); return FALSE;
}
static JsonNode *body_new(RotationJob *job)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *body = json_object_new();
    json_node_take_object(node, body);
    json_object_set_string_member(body, "station_id", radios_data_client_station_id(job->client)); return node;
}
static gboolean send_request(RotationJob *job, GtkWidget *owner, const gchar *path,
    JsonNode *body, RadioSDataCallback callback)
{
    gboolean accepted = job_check(job, owner) && radios_data_request(job->ctx, owner, "POST", path,
        body, callback, job_ref(job), job_free);
    json_node_free(body); return accepted;
}
static gboolean document_kind_valid(const gchar *kind)
{ return kind && (!strcmp(kind, "automation_calendar") || !strcmp(kind, "weekly_programme")); }
static JsonObject *record_calendar(JsonObject *record)
{
    JsonObject *config = object(record, "config");
    return !strcmp(text(record, "kind"), "weekly_programme") ? object(config, "calendar") : config;
}
static gboolean definition_is_schema(JsonObject *definition)
{
    JsonObject *settings = object(definition, "settings");
    JsonNode *node = settings ? json_object_get_member(settings, "automation_schema") : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}
static JsonObject *definition_find(JsonObject *record, const gchar *id)
{
    JsonArray *definitions = array(record_calendar(record), "rotation_library");
    for (guint i = 0; definitions && i < json_array_get_length(definitions); ++i) {
        JsonObject *definition = at(definitions, i);
        if (definition && g_str_equal(text(definition, "id"), id)) return definition;
    }
    return NULL;
}
static gboolean record_valid(RotationJob *job, JsonObject *record)
{
    return record && g_str_equal(text(record, "_id"), job->document_id) &&
        radios_json_int(record, "_rev", 0) == job->revision && document_kind_valid(job->document_kind) &&
        g_str_equal(text(record, "kind"), job->document_kind) &&
        (strcmp(job->document_kind, "automation_calendar") || g_str_equal(text(record, "name"), "main")) &&
        g_str_equal(text(record, "status"), "active") &&
        g_str_equal(text(record, "station_id"), radios_data_client_station_id(job->client)) &&
        definition_find(record, job->rotation_id) != NULL;
}
static void verified_document(RadioSViewContext *, GtkWidget *, JsonNode *, const GError *, gpointer);
static void programme_received(RadioSViewContext *, GtkWidget *, JsonNode *, const GError *, gpointer);
static gboolean document_request(RotationJob *job, GtkWidget *owner, RadioSDataCallback callback)
{
    JsonNode *body = body_new(job); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "kind", job->document_kind);
    json_object_set_string_member(o, "document_id", job->document_id);
    return send_request(job, owner, "/v1/workflows/desktop/get", body, callback);
}
static gboolean programme_request(RotationJob *job, GtkWidget *owner)
{
    JsonObject *selected = json_node_get_object(job->selection), *config = object(selected, "config");
    if (!config) return FALSE;
    JsonNode *body = body_new(job); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "kind", "programme"); json_object_set_member(o, "config", copy_object(config));
    return send_request(job, owner, "/v1/workflows/desktop-playout/prepare", body, programme_received);
}
static GtkListStore *store(GtkWidget *tree) { return GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(tree))); }
static void preview_render(RotationLoader *loader, JsonObject *prepared)
{
    gtk_list_store_clear(store(loader->preview)); JsonArray *tracks = array(prepared, "tracks");
    for (guint i = 0; tracks && i < json_array_get_length(tracks); ++i) {
        JsonObject *row = at(tracks, i), *metadata = object(row, "metadata"); GtkTreeIter iter;
        gint64 milliseconds = radios_json_int(metadata, "duration_ms", 0);
        gchar *duration = g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, milliseconds / 60000, milliseconds / 1000 % 60);
        gtk_list_store_append(store(loader->preview), &iter);
        gtk_list_store_set(store(loader->preview), &iter, 0, i + 1, 1, text(metadata, "title"), 2, text(metadata, "artist"), 3, duration, -1);
        g_free(duration);
    }
    gchar *message = g_strdup_printf("%u parça hazır. Yükleme sesi başlatmaz; bu sıra en fazla 200 parçadır.", tracks ? json_array_get_length(tracks) : 0);
    gtk_label_set_text(GTK_LABEL(loader->status), message); g_free(message);
    gtk_widget_set_sensitive(loader->load, tracks && json_array_get_length(tracks) > 0);
}
static void verified_document(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; RotationJob *job = value; if (!job_check(job, owner)) return;
    JsonObject *record = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    if (error || !record_valid(job, record)) { job_invalid(job, owner, error, "Rotasyon kaydı değişti veya kaldırıldı. Yeniden yükleyin."); return; }
    /* Document verification precedes publication; never tag a selection or
     * a stale retained card response as the currently accepted origin. */
    json_object_set_string_member(json_node_get_object(job->prepared), "loudness_automation_document_id", job->document_id);
    json_object_set_string_member(json_node_get_object(job->prepared), "rotation_document_kind", job->document_kind);
    job->completed = TRUE;
    if (job->modal) {
        RotationLoader *loader = loader_get(owner); g_clear_pointer(&loader->prepared, json_node_free);
        loader->prepared = copy_object(json_node_get_object(job->prepared));
        g_free(loader->document_kind); loader->document_kind = g_strdup(job->document_kind);
        g_free(loader->document_id); loader->document_id = g_strdup(job->document_id); loader->revision = job->revision;
        g_free(loader->rotation_id); loader->rotation_id = g_strdup(job->rotation_id);
        g_free(loader->rotation_name); loader->rotation_name = g_strdup(job->rotation_name);
        if (job->handoff) {
            g_object_ref(owner);
            if (loader->done) loader->done(json_node_get_object(loader->prepared), loader->document_id, loader->revision,
                loader->rotation_id, loader->rotation_name, loader->client, loader->user);
            gtk_widget_destroy(owner); g_object_unref(owner);
        } else preview_render(loader, json_node_get_object(loader->prepared));
    } else if (job->callback) job->callback(job->ctx, owner, job->prepared, NULL, job->user);
}
static void programme_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; RotationJob *job = value; if (!job_check(job, owner)) return;
    GError *failure = error ? g_error_copy(error) : NULL;
    JsonNode *merged = !failure && result && JSON_NODE_HOLDS_OBJECT(result) ?
        radios_automation_calendar_merge_selection(json_node_get_object(result), json_node_get_object(job->selection), &failure) : NULL;
    if (!merged) { job_fail(job, owner, failure, "Rotasyon medyası yeniden doğrulanamadı."); g_clear_error(&failure); return; }
    RadioSPlayoutRequest request = {.station_id = radios_data_client_station_id(job->client),
        .database_file = job->ctx->config->database_file, .now_utc_us = g_get_real_time(), .trim_calendar_end = TRUE};
    RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(merged), &request, &failure);
    if (!plan) {
        json_node_free(merged); job_fail(job, owner, failure, "Rotasyon planı geçersiz."); g_clear_error(&failure); return;
    }
    radios_playout_plan_free(plan);
    job->prepared = merged;
    if (!document_request(job, owner, verified_document)) job_fail(job, owner, NULL, "Rotasyon kaydı yeniden okunamadı.");
}
static void automation_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; RotationJob *job = value; if (!job_check(job, owner)) return;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { job_fail(job, owner, error, "Rotasyon kaynakları hazırlanamadı."); return; }
    GError *failure = NULL;
    job->selection = radios_automation_calendar_prepared_programme(json_node_get_object(job->event), json_node_get_object(result),
        radios_data_client_station_id(job->client), job->rotation_id, 200, &failure);
    if (!job->selection) { job_fail(job, owner, failure, "Rotasyon kurallarına uyan parça yok."); g_clear_error(&failure); return; }
    if (!programme_request(job, owner)) job_fail(job, owner, NULL, "Rotasyon medyası doğrulanamadı.");
}
static JsonNode *event_new(RotationJob *job, JsonObject *calendar)
{
    const gchar *name = json_object_has_member(calendar, "timezone") ? text(calendar, "timezone") : "UTC";
    GTimeZone *zone = radios_time_zone_new(name);
    if (!zone) return NULL;
    GDateTime *now = g_date_time_new_now(zone); g_time_zone_unref(zone);
    gchar *date = g_date_time_format(now, "%Y-%m-%d");
    gint minute = g_date_time_get_hour(now) * 60 + g_date_time_get_minute(now);
    JsonObject *event = json_object_new(), *assignment = json_object_new(); JsonArray *assignments = json_array_new();
    gchar *id = g_uuid_string_random(), *assignment_id = g_uuid_string_random();
    json_object_set_string_member(event, "id", id); json_object_set_string_member(event, "title", job->rotation_name);
    json_object_set_string_member(event, "date", date); json_object_set_string_member(event, "color", "#abd6e4");
    json_object_set_int_member(event, "start_minute", minute); json_object_set_int_member(event, "end_minute", 1440);
    json_object_set_string_member(assignment, "id", assignment_id); json_object_set_string_member(assignment, "rotation_id", job->rotation_id);
    json_array_add_object_element(assignments, assignment); json_object_set_array_member(event, "rotations", assignments);
    g_free(id); g_free(assignment_id); g_free(date); g_date_time_unref(now);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, event); return node;
}
static void selected_document(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; RotationJob *job = value; if (!job_check(job, owner)) return;
    JsonObject *record = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    if (error || !record_valid(job, record)) { job_invalid(job, owner, error, "Rotasyon kaydı değişti veya kaldırıldı. Yeniden yükleyin."); return; }
    if (!job->modal || job->handoff) {
        if (!programme_request(job, owner)) job_fail(job, owner, NULL, "Yüklü rotasyon yeniden doğrulanamadı.");
        return;
    }
    JsonObject *definition = definition_find(record, job->rotation_id); JsonArray *sources = array(definition, "sources");
    if (!sources || !json_array_get_length(sources)) {
        job_fail(job, owner, NULL, "Bu rotasyonda playlist yok. Otomasyon'da rotasyonu düzenleyip playlist ekleyin."); return;
    }
    g_free(job->rotation_name); job->rotation_name = g_strdup(text(definition, "name"));
    JsonNode *calendar = copy_object(record_calendar(record));
    JsonNode *event = calendar ? event_new(job, json_node_get_object(calendar)) : NULL; GError *failure = NULL;
    if (!event) { g_clear_pointer(&calendar, json_node_free); job_fail(job, owner, NULL, "Şemanın saat dilimi geçersiz."); return; }
    if (job->first_playlist_id) {
        JsonObject *schema = definition_find(record, job->rotation_id);
        if (!definition_is_schema(schema)) {
            json_node_free(event); json_node_free(calendar); job_fail(job, owner, NULL, "Seçilen kayıt bir otomasyon şeması değil."); return;
        }
        JsonArray *definitions = array(json_node_get_object(calendar), "rotation_library");
        JsonObject *copy = NULL;
        for (guint i = 0; definitions && i < json_array_get_length(definitions); ++i)
            if (!g_strcmp0(text(at(definitions, i), "id"), job->rotation_id)) copy = at(definitions, i);
        JsonArray *old = array(copy, "sources"), *ordered = json_array_new(); JsonObject *current = NULL;
        for (guint i = 0; old && i < json_array_get_length(old); ++i)
            if (!current && !g_strcmp0(text(at(old, i), "playlist_id"), job->first_playlist_id)) current = at(old, i);
        if (current) json_array_add_object_element(ordered, radios_programme_clone_object(current));
        else {
            if (old && json_array_get_length(old) >= 50) { json_node_free(event); json_node_free(calendar); json_array_unref(ordered); job_fail(job, owner, NULL, "Şemaya mevcut playlist eklenince 50 kaynak sınırı aşılır."); return; }
            JsonObject *source = json_object_new(); gchar *id = g_uuid_string_random();
            json_object_set_string_member(source, "id", id); g_free(id); json_object_set_string_member(source, "playlist_id", job->first_playlist_id);
            json_object_set_int_member(source, "frequency", 1); json_object_set_int_member(source, "count", 1); json_object_set_int_member(source, "weight", 100);
            json_object_set_int_member(source, "start_minute", 0); json_object_set_int_member(source, "end_minute", 1440); json_object_set_int_member(source, "interval_minutes", 0);
            const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
            for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(source, flags[i], FALSE);
            json_object_set_string_member(source, "media_kind", "all"); json_array_add_object_element(ordered, source);
        }
        for (guint i = 0; old && i < json_array_get_length(old); ++i) if (at(old, i) != current) json_array_add_object_element(ordered, radios_programme_clone_object(at(old, i)));
        json_object_set_array_member(copy, "sources", ordered);
    }
    job->event = radios_automation_calendar_resolve_event(json_node_get_object(calendar), json_node_get_object(event), &failure);
    json_node_free(calendar);
    if (job->event && job->first_playlist_id) json_object_set_string_member(json_node_get_object(job->event), "schema_first_playlist_id", job->first_playlist_id);
    json_node_free(event);
    JsonNode *configuration = job->event ? radios_automation_calendar_prepare_config(json_node_get_object(job->event), NULL, &failure) : NULL;
    if (!configuration) { job_fail(job, owner, failure, "Rotasyon tanımı hazırlanamadı."); g_clear_error(&failure); return; }
    JsonNode *body = body_new(job); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "kind", "automation"); json_object_set_member(o, "config", configuration);
    if (!send_request(job, owner, "/v1/workflows/desktop-playout/prepare", body, automation_received))
        job_fail(job, owner, NULL, "Rotasyon kaynakları okunamadı.");
}
static void source_selected(GtkTreeSelection *selection, gpointer value)
{
    RotationLoader *loader = value; if (loader->rendering || loader->closed || loader->pending_lists || loader->catalogue_failed) return;
    ++loader->generation; loader->accepting = FALSE; g_clear_pointer(&loader->prepared, json_node_free); gtk_list_store_clear(store(loader->preview));
    gtk_widget_set_sensitive(loader->load, FALSE);
    GtkTreeModel *model = NULL; GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
    if (!same_client(loader->ctx, loader->client)) { gtk_label_set_text(GTK_LABEL(loader->status), "Veri oturumu değişti. Pencereyi kapatıp yeniden açın."); return; }
    RotationJob *job = job_new(loader->ctx, loader->dialog, loader->client); job->modal = TRUE; job->generation = loader->generation;
    job->first_playlist_id = g_strdup(loader->first_playlist_id);
    gtk_tree_model_get(model, &iter, 0, &job->rotation_name, 2, &job->rotation_id,
        3, &job->document_kind, 4, &job->document_id, 5, &job->revision, -1);
    gtk_label_set_text(GTK_LABEL(loader->status), "Rotasyon kuralları ve medya doğrulanıyor…");
    if (!document_request(job, loader->dialog, selected_document)) job_fail(job, loader->dialog, NULL, "Rotasyon kaydı okunamadı.");
    job_free(job);
}
static void append_definitions(RotationLoader *loader, JsonObject *record)
{
    JsonArray *definitions = array(record_calendar(record), "rotation_library");
    for (guint i = 0; definitions && i < json_array_get_length(definitions); ++i) {
        JsonObject *definition = at(definitions, i); JsonArray *sources = array(definition, "sources");
        if (!definition || !g_uuid_string_is_valid(text(definition, "id")) || !*text(definition, "name") ||
            (loader->first_playlist_id && !definition_is_schema(definition))) continue;
        gchar *absent = radios_automation_calendar_excluded_kinds_text(object(definition, "settings"));
        gchar *caption = loader->first_playlist_id ? g_strdup_printf("%s · %s%s%s",
            !strcmp(text(record, "kind"), "automation_calendar") ? "Ana yayın takvimi" : text(record, "name"), text(definition, "name"), *absent ? " · yok: " : "", absent) :
            g_strdup_printf("%s%s%s", text(definition, "name"), *absent ? " · yok: " : "", absent);
        g_free(absent);
        GtkTreeIter iter; gtk_list_store_append(store(loader->list), &iter);
        gtk_list_store_set(store(loader->list), &iter, 0, caption, 1, sources ? json_array_get_length(sources) : 0,
            2, text(definition, "id"), 3, text(record, "kind"), 4, text(record, "_id"), 5, radios_json_int(record, "_rev", 0), -1);
        g_free(caption);
    }
}
static void catalogue_finished(RotationLoader *loader, gboolean failed)
{
    loader->catalogue_failed |= failed;
    if (loader->pending_lists) --loader->pending_lists;
    if (loader->pending_lists) return;
    loader->rendering = TRUE;
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(loader->list)));
    loader->rendering = FALSE;
    gtk_widget_set_sensitive(loader->list, !loader->catalogue_failed);
    if (loader->catalogue_failed) {
        gtk_label_set_text(GTK_LABEL(loader->status), "Kayıtlı programlar tamamen okunamadı. Rotasyonları yeniden yükleyin.");
        gtk_widget_set_sensitive(loader->load, FALSE); return;
    }
    gtk_label_set_text(GTK_LABEL(loader->status), gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store(loader->list)), NULL) ?
        (loader->first_playlist_id ? "Ana takvim veya kayıtlı programdan bir şema seçin." : "Bir rotasyon seçin.") :
        "Kayıtlı rotasyon yok. Önce Otomasyon'da bir rotasyon kaydedin.");
}
static void list_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; RotationJob *job = value; if (!job_check(job, owner)) return;
    RotationLoader *loader = loader_get(owner); JsonObject *response = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    JsonArray *records = array(response, "records");
    if (loader->first_playlist_id) {
        if (error || !records) { catalogue_finished(loader, TRUE); return; }
        loader->rendering = TRUE;
        for (guint i = 0; i < json_array_get_length(records); ++i) {
            JsonObject *record = at(records, i);
            if (!record || strcmp(text(record, "kind"), job->document_kind) || !g_uuid_string_is_valid(text(record, "_id")) ||
                radios_json_int(record, "_rev", 0) < 1 || strcmp(text(record, "status"), "active") ||
                strcmp(text(record, "station_id"), radios_data_client_station_id(job->client)) ||
                (!strcmp(job->document_kind, "automation_calendar") && strcmp(text(record, "name"), "main"))) continue;
            append_definitions(loader, record);
        }
        loader->rendering = FALSE; catalogue_finished(loader, FALSE); return;
    }
    JsonObject *record = NULL;
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *candidate = at(records, i);
        if (candidate && g_str_equal(text(candidate, "name"), "main") && g_str_equal(text(candidate, "kind"), "automation_calendar") &&
            g_str_equal(text(candidate, "status"), "active") && g_str_equal(text(candidate, "station_id"), radios_data_client_station_id(job->client))) {
            if (record) { loader->pending_lists = 0; job_fail(job, owner, NULL, "Birden fazla ana takvim kaydı var; rotasyon kaydı belirsiz."); return; }
            record = candidate;
        }
    }
    if (error || !record || !g_uuid_string_is_valid(text(record, "_id")) || radios_json_int(record, "_rev", 0) < 1) {
        loader->pending_lists = 0; job_fail(job, owner, error, "Kayıtlı rotasyon yok. Önce Otomasyon'da bir rotasyon kaydedin."); return;
    }
    g_clear_pointer(&loader->record, json_node_free); loader->record = copy_object(record);
    loader->rendering = TRUE; append_definitions(loader, record); loader->rendering = FALSE;
    catalogue_finished(loader, FALSE);
}
static void reload_clicked(GtkButton *button, gpointer value)
{
    (void)button; RotationLoader *loader = value; ++loader->generation; loader->accepting = FALSE;
    g_clear_pointer(&loader->record, json_node_free); g_clear_pointer(&loader->prepared, json_node_free);
    loader->rendering = TRUE; gtk_list_store_clear(store(loader->list)); loader->rendering = FALSE;
    gtk_list_store_clear(store(loader->preview)); gtk_widget_set_sensitive(loader->load, FALSE);
    loader->catalogue_failed = FALSE; loader->pending_lists = loader->first_playlist_id ? 2 : 1;
    gtk_widget_set_sensitive(loader->list, FALSE);
    if (!same_client(loader->ctx, loader->client)) { loader->pending_lists = 0; gtk_label_set_text(GTK_LABEL(loader->status), "Veri oturumu değişti. Pencereyi kapatıp yeniden açın."); return; }
    gtk_label_set_text(GTK_LABEL(loader->status), "Kayıtlı rotasyonlar okunuyor…");
    const gchar *kinds[] = {"automation_calendar", "weekly_programme"}; guint requests = loader->pending_lists;
    for (guint i = 0; i < requests; ++i) {
        RotationJob *job = job_new(loader->ctx, loader->dialog, loader->client); job->modal = TRUE; job->generation = loader->generation;
        job->document_kind = g_strdup(kinds[i]);
        JsonNode *body = body_new(job); json_object_set_string_member(json_node_get_object(body), "kind", kinds[i]);
        if (!send_request(job, loader->dialog, "/v1/workflows/desktop/list", body, list_received)) catalogue_finished(loader, TRUE);
        job_free(job);
    }
}
static void loader_destroyed(GtkWidget *widget, gpointer value)
{
    (void)widget; RotationLoader *loader = value; loader->closed = TRUE; ++loader->generation;
}
static void loader_free(gpointer value)
{
    RotationLoader *loader = value; radios_data_client_unref(loader->client);
    g_clear_pointer(&loader->record, json_node_free); g_clear_pointer(&loader->prepared, json_node_free);
    g_free(loader->document_kind); g_free(loader->document_id); g_free(loader->rotation_id); g_free(loader->rotation_name); g_free(loader->first_playlist_id); g_free(loader);
}
static void loader_response(GtkDialog *dialog, gint response, gpointer value)
{
    RotationLoader *loader = value;
    if (response == GTK_RESPONSE_ACCEPT) {
        if (loader->accepting) return;
        if (!loader->prepared || !same_client(loader->ctx, loader->client)) {
            gtk_label_set_text(GTK_LABEL(loader->status), "Rotasyon hazır değil. Yeniden yükleyin."); return;
        }
        loader->accepting = TRUE; ++loader->generation; gtk_widget_set_sensitive(loader->load, FALSE);
        RotationJob *job = job_new(loader->ctx, GTK_WIDGET(dialog), loader->client);
        job->modal = job->handoff = TRUE; job->generation = loader->generation;
        job->document_kind = g_strdup(loader->document_kind);
        job->document_id = g_strdup(loader->document_id); job->revision = loader->revision;
        job->rotation_id = g_strdup(loader->rotation_id); job->rotation_name = g_strdup(loader->rotation_name);
        job->selection = copy_object(json_node_get_object(loader->prepared));
        gtk_label_set_text(GTK_LABEL(loader->status), "Rotasyon yüklenmeden önce yeniden doğrulanıyor…");
        if (!document_request(job, GTK_WIDGET(dialog), selected_document)) job_fail(job, GTK_WIDGET(dialog), NULL, "Rotasyon yeniden doğrulanamadı.");
        job_free(job); return;
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static GtkWidget *tree_new(const gchar *name, const gchar *const *titles, guint columns, const GType *types, guint model_columns)
{
    GtkListStore *model = gtk_list_store_newv((gint)model_columns, (GType *)types); GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(model)); g_object_unref(model);
    gtk_widget_set_name(tree, name);
    for (guint i = 0; i < columns; ++i) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_expand(column, i == 0 || i == 1); gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
    }
    return tree;
}
static GtkWidget *scroll_new(GtkWidget *tree)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tree); return scroll;
}
static GtkWidget *rotation_open(RadioSViewContext *ctx, GtkWidget *owner, const gchar *first_playlist_id, RadioSPlayerRotationDone done, gpointer user)
{
    RadioSDataClient *client = ctx && owner && !gtk_widget_in_destruction(owner) && radios_data_connected(ctx) ? radios_data_client_acquire(ctx) : NULL;
    if (!client) return NULL;
    RotationLoader *loader = g_new0(RotationLoader, 1); loader->ctx = ctx; loader->client = client; loader->done = done; loader->user = user;
    loader->first_playlist_id = g_strdup(first_playlist_id);
    loader->dialog = gtk_dialog_new_with_buttons(first_playlist_id ? "Otomasyon şeması yükle" : "Rotasyon yükle", ctx->parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Yükle", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(loader->dialog, "player-rotation-dialog"); gtk_window_set_default_size(GTK_WINDOW(loader->dialog), 960, 540);
    gtk_style_context_add_class(gtk_widget_get_style_context(loader->dialog), "radios"); gtk_style_context_add_class(gtk_widget_get_style_context(loader->dialog), "task-dialog");
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(loader->dialog)); gtk_container_set_border_width(GTK_CONTAINER(body), 14); gtk_box_set_spacing(GTK_BOX(body), 10);
    const gchar *source_titles[] = {"Rotasyon", "Kaynak"}, *preview_titles[] = {"#", "Parça", "Sanatçı", "Süre"};
    const GType source_types[] = {G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64}, preview_types[] = {G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING};
    loader->list = tree_new("player-rotation-list", source_titles, 2, source_types, 6); loader->preview = tree_new("player-rotation-preview", preview_titles, 4, preview_types, 4);
    GtkWidget *rows = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL); gtk_paned_pack1(GTK_PANED(rows), scroll_new(loader->list), FALSE, FALSE); gtk_paned_pack2(GTK_PANED(rows), scroll_new(loader->preview), TRUE, FALSE); gtk_paned_set_position(GTK_PANED(rows), 280);
    gtk_box_pack_start(GTK_BOX(body), rows, TRUE, TRUE, 0);
    GtkWidget *reload = gtk_button_new_with_label("Rotasyonları yeniden yükle"); gtk_widget_set_name(reload, "player-rotation-reload"); gtk_widget_set_halign(reload, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(body), reload, FALSE, FALSE, 0);
    loader->status = radios_ui_label("", "application-status"); gtk_widget_set_name(loader->status, "player-rotation-status"); gtk_label_set_line_wrap(GTK_LABEL(loader->status), TRUE);
    gtk_box_pack_start(GTK_BOX(body), loader->status, FALSE, FALSE, 0);
    loader->load = gtk_dialog_get_widget_for_response(GTK_DIALOG(loader->dialog), GTK_RESPONSE_ACCEPT); gtk_widget_set_name(loader->load, "player-rotation-load"); gtk_widget_set_sensitive(loader->load, FALSE);
    g_object_set_data_full(G_OBJECT(loader->dialog), "radios-player-rotation", loader, loader_free);
    g_signal_connect(loader->dialog, "destroy", G_CALLBACK(loader_destroyed), loader); g_signal_connect(loader->dialog, "response", G_CALLBACK(loader_response), loader);
    g_signal_connect(reload, "clicked", G_CALLBACK(reload_clicked), loader); g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(loader->list)), "changed", G_CALLBACK(source_selected), loader);
    g_signal_connect_object(owner, "destroy", G_CALLBACK(gtk_widget_destroy), loader->dialog, G_CONNECT_SWAPPED);
    gtk_widget_show_all(loader->dialog); reload_clicked(NULL, loader); return loader->dialog;
}
GtkWidget *radios_player_rotation_open(RadioSViewContext *ctx, GtkWidget *owner, RadioSPlayerRotationDone done, gpointer user)
{ return rotation_open(ctx, owner, NULL, done, user); }
GtkWidget *radios_player_rotation_open_schema(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *current_playlist_id, RadioSPlayerRotationDone done, gpointer user)
{
    if (!g_uuid_string_is_valid(current_playlist_id)) return NULL;
    return rotation_open(ctx, owner, current_playlist_id, done, user);
}
gboolean radios_player_rotation_prepare_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *prepared,
    const gchar *document_id, gint64 revision, const gchar *rotation_id, RadioSDataClient *client,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{
    if (!ctx || !owner || gtk_widget_in_destruction(owner) || !prepared || !document_id || !rotation_id || !g_uuid_string_is_valid(document_id) || revision < 1 ||
        !g_uuid_string_is_valid(rotation_id) || !same_client(ctx, client) || !g_str_equal(text(prepared, "kind"), "programme") ||
        !g_str_equal(text(prepared, "station_id"), radios_data_client_station_id(client))) {
        if (destroy) destroy(user);
        return FALSE;
    }
    const gchar *kind = json_object_has_member(prepared, "rotation_document_kind") ? text(prepared, "rotation_document_kind") : "automation_calendar";
    if (!document_kind_valid(kind)) { if (destroy) destroy(user); return FALSE; }
    RotationJob *job = job_new(ctx, owner, client); job->document_kind = g_strdup(kind); job->document_id = g_strdup(document_id); job->rotation_id = g_strdup(rotation_id);
    job->revision = revision; job->selection = copy_object(prepared); job->callback = callback; job->user = user; job->destroy = destroy;
    gboolean accepted = document_request(job, owner, selected_document); job_free(job); return accepted;
}

/* The UTC scheduler owns only the global intent it installed. Worker results
 * cannot regain ownership after Stop, replacement, logout or card destruction. */
struct RadioSPlayerWeekly {
    gint references;
    RadioSViewContext *ctx;
    GWeakRef owner;
    RadioSDataClient *client;
    JsonNode *record;
    RadioSPlayerWeeklySnapshot snapshot;
    guint source, target, timer;
    guint64 serial, run_intent;
    gint64 retry_utc_us, stop_after_utc_us, completed_end_utc_us;
    gboolean closed, pending, owns_run, general;
    JsonNode *schedule, *preview_config;
    RadioSGeneralProgrammeQueue queue;
    gpointer queue_user;
    gint64 general_floor_utc_us, general_end_utc_us, general_after_utc_us, discover_at;
    guint retry_failures;
    GHashTable *unavailable_events;
    gint64 unavailable_until;
    gchar *general_event_id;
    RadioSWeeklyProgrammeSelection *general_following;
    gint64 general_following_floor;
    GCancellable *general_selection_cancel;
    /* Kılavuz §02/§03: schema applied to the current slot's event and the
     * entry rule carried from the preceding programme. Preserve alternatives
     * and their assigned automation across retries of the same occurrence. */
    gchar *general_schema_id, *general_schema_event_id, *general_entry_key;
    JsonNode *general_entry_settings, *general_entry_active;
    /* Transactional runtime-only schema replacement. Until the verified
     * future queue is accepted, a failure restores this schedule. */
    JsonNode *schema_replace_backup;
    gchar *schema_replace_id;
};
typedef struct {
    gint references;
    RadioSPlayerWeekly *weekly;
    guint64 serial, intent, generations[32];
    GWeakRef cards[32];
    RadioSWeeklyProgrammeOccurrence *occurrence;
    JsonNode *selection, *prepared, *sources;
    guint reselections;
    gboolean preview, replace_remaining;
    RadioSAutomationBinding loudness_binding;
    gboolean loudness_binding_captured;
    JsonArray *records;
} WeeklyJob;
static gboolean general_discover(RadioSPlayerWeekly *weekly, GtkWidget *owner);
static gboolean general_tick(RadioSPlayerWeekly *weekly);
static void general_source_changed(RadioSPlayerWeekly *weekly);
static void general_skip_slot(RadioSPlayerWeekly *weekly, const gchar *message);
static JsonArray *general_available_records(RadioSPlayerWeekly *weekly, JsonArray *records);
typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *dialog, *list, *preview, *status, *load;
    RadioSDataClient *client;
    JsonNode *records;
    guint generation;
    gboolean updating, accepting;
    RadioSPlayerWeeklyDone done;
    gpointer user;
} WeeklyPicker;
typedef struct {
    RadioSDataClient *client;
    guint generation;
    JsonNode *record;
} WeeklyPickJob;
static void weekly_unref(RadioSPlayerWeekly *weekly)
{
    if (--weekly->references) return;
    if (weekly->client) radios_data_client_unref(weekly->client);
    g_clear_pointer(&weekly->record, json_node_free);
    g_clear_pointer(&weekly->schedule, json_node_free); g_clear_pointer(&weekly->preview_config, json_node_free);
    g_clear_pointer(&weekly->unavailable_events, g_hash_table_unref);
    g_clear_pointer(&weekly->general_following, radios_weekly_programme_selection_free);
    g_clear_object(&weekly->general_selection_cancel);
    g_free(weekly->general_event_id);
    g_free(weekly->general_schema_id); g_free(weekly->general_schema_event_id); g_free(weekly->general_entry_key);
    g_clear_pointer(&weekly->general_entry_settings, json_node_free); g_clear_pointer(&weekly->general_entry_active, json_node_free);
    g_clear_pointer(&weekly->schema_replace_backup, json_node_free); g_free(weekly->schema_replace_id);
    g_weak_ref_clear(&weekly->owner); g_free(weekly);
}
static void weekly_fail(RadioSPlayerWeekly *weekly, const GError *error, const gchar *message)
{
    weekly->pending = FALSE;
    if (weekly->schema_replace_id) {
        if (weekly->schema_replace_backup) {
            g_clear_pointer(&weekly->schedule, json_node_free);
            weekly->schedule = g_steal_pointer(&weekly->schema_replace_backup);
        }
        g_clear_pointer(&weekly->schema_replace_id, g_free);
        weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_PLAYING;
        if (weekly->owns_run && weekly->run_intent != radios_playout_ui_intent(weekly->ctx)) {
            weekly->owns_run = FALSE; weekly->snapshot.armed = FALSE; weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_ERROR;
        }
        g_strlcpy(weekly->snapshot.error, error ? error->message : message, sizeof weekly->snapshot.error);
        if (!weekly->closed && weekly->ctx->notify) weekly->ctx->notify(weekly->ctx->notify_data, weekly->snapshot.error);
        return;
    }
    gboolean transient = error && ((error->domain == RADIOS_DATA_CLIENT_ERROR &&
        (error->code == RADIOS_DATA_CLIENT_ERROR_TRANSPORT || error->code == RADIOS_DATA_CLIENT_ERROR_TIMEOUT ||
         error->code == RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED || error->code == RADIOS_DATA_CLIENT_ERROR_UNAVAILABLE)) ||
        (error->domain == G_IO_ERROR && (error->code == G_IO_ERROR_TIMED_OUT || error->code == G_IO_ERROR_NOT_CONNECTED)));
    if (weekly->general && transient) {
        guint delay = MIN(30u, 1u << MIN(weekly->retry_failures++, 5u));
        weekly->retry_utc_us = g_get_real_time() + delay * G_USEC_PER_SEC;
        weekly->discover_at = g_get_monotonic_time() + delay * G_USEC_PER_SEC;
        weekly->snapshot.state = weekly->snapshot.armed ? RADIOS_PLAYER_WEEKLY_WAITING : RADIOS_PLAYER_WEEKLY_LOADED;
    } else { weekly->snapshot.armed = FALSE; weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_ERROR; }
    g_strlcpy(weekly->snapshot.error, error ? error->message : message, sizeof weekly->snapshot.error);
    if (!weekly->closed && weekly->ctx->notify)
        weekly->ctx->notify(weekly->ctx->notify_data, weekly->snapshot.error);
}
static gboolean weekly_current(RadioSPlayerWeekly *weekly, GtkWidget *owner)
{
    if (weekly->closed || !owner || gtk_widget_in_destruction(owner)) return FALSE;
    if (weekly->general) return TRUE;
    if (!same_client(weekly->ctx, weekly->client)) return FALSE;
    gchar *key = g_strdup_printf("radios-deck-owner-%u", weekly->source);
    gboolean current = g_object_get_data(G_OBJECT(weekly->ctx->parent), key) == owner;
    g_free(key); return current;
}
static JsonObject *weekly_config(RadioSPlayerWeekly *weekly)
{ return weekly->general ? (weekly->schedule ? json_node_get_object(weekly->schedule) : NULL) :
    object(weekly->record ? json_node_get_object(weekly->record) : NULL, "config"); }
static gboolean weekly_bounds(JsonObject *config, gint64 *anchor, gint64 *period)
{
    gchar *iso = g_strconcat(text(config, "week_anchor"), "T00:00:00Z", NULL);
    GDateTime *date = g_date_time_new_from_iso8601(iso, NULL); g_free(iso);
    gint64 weeks = radios_json_int(config, "week_count", 0);
    if (!date || weeks < 1 || weeks > 52) { if (date) g_date_time_unref(date); return FALSE; }
    *anchor = g_date_time_to_unix(date) * G_USEC_PER_SEC;
    *period = weeks * 7 * 24 * 60 * 60 * G_USEC_PER_SEC; g_date_time_unref(date); return TRUE;
}
static RadioSWeeklyProgrammeOccurrence *weekly_next(RadioSPlayerWeekly *weekly, gint64 now, GError **error)
{
    if (weekly->general) return weekly->schedule ?
        radios_weekly_programme_next(json_node_get_object(weekly->schedule), now, error) : NULL;
    JsonNode *config = copy_object(weekly_config(weekly));
    if (!config) return NULL;
    gint64 anchor = 0, period = 0;
    gboolean valid = weekly_bounds(json_node_get_object(config), &anchor, &period);
    json_object_set_boolean_member(json_node_get_object(config), "repeat", weekly->snapshot.repeat ||
        (valid && weekly->stop_after_utc_us > anchor + period));
    RadioSWeeklyProgrammeOccurrence *occurrence = radios_weekly_programme_next(json_node_get_object(config), now, error);
    json_node_free(config);
    if (occurrence && !weekly->snapshot.repeat && occurrence->start_utc_us >= weekly->stop_after_utc_us) {
        radios_weekly_programme_occurrence_free(occurrence); return NULL;
    }
    return occurrence;
}
static void weekly_display(RadioSPlayerWeekly *weekly, RadioSWeeklyProgrammeOccurrence *occurrence, gint64 now)
{
    weekly->snapshot.current_event_name[0] = weekly->snapshot.next_event_name[0] = '\0';
    weekly->snapshot.start_utc_us = weekly->snapshot.end_utc_us = weekly->snapshot.next_start_utc_us = weekly->snapshot.remaining_us = 0;
    if (!occurrence) return;
    if (occurrence->start_utc_us <= now && now < occurrence->end_utc_us) {
        g_strlcpy(weekly->snapshot.current_event_name, text(json_node_get_object(occurrence->event), "title"), sizeof weekly->snapshot.current_event_name);
        weekly->snapshot.start_utc_us = occurrence->start_utc_us; weekly->snapshot.end_utc_us = occurrence->end_utc_us;
        weekly->snapshot.remaining_us = MAX((gint64)0, occurrence->end_utc_us - now);
        /* A record scan validates every saved calendar and may take longer
         * than this 250 ms display tick. Discovery computes the following slot
         * once, against the same immutable records and this exact end floor. */
        RadioSWeeklyProgrammeSelection *following = weekly->general_following_floor == occurrence->end_utc_us ? weekly->general_following : NULL;
        RadioSWeeklyProgrammeOccurrence *next = weekly->general ? (following ? following->occurrence : NULL) : weekly_next(weekly, occurrence->end_utc_us, NULL);
        if (next) {
            weekly->snapshot.next_start_utc_us = next->start_utc_us;
            g_strlcpy(weekly->snapshot.next_event_name, text(json_node_get_object(next->event), "title"), sizeof weekly->snapshot.next_event_name);
            if (!weekly->general) radios_weekly_programme_occurrence_free(next);
        }
    } else {
        weekly->snapshot.next_start_utc_us = occurrence->start_utc_us;
        weekly->snapshot.remaining_us = MAX((gint64)0, occurrence->start_utc_us - now);
        g_strlcpy(weekly->snapshot.next_event_name, text(json_node_get_object(occurrence->event), "title"), sizeof weekly->snapshot.next_event_name);
    }
}
static WeeklyJob *weekly_job_ref(WeeklyJob *job) { ++job->references; return job; }
static void weekly_job_free(gpointer value)
{
    WeeklyJob *job = value; if (--job->references) return;
    for (guint i = 0; i < 32; ++i) g_weak_ref_clear(&job->cards[i]);
    radios_weekly_programme_occurrence_free(job->occurrence);
    g_clear_pointer(&job->selection, json_node_free); g_clear_pointer(&job->prepared, json_node_free); g_clear_pointer(&job->sources, json_node_free);
    g_clear_pointer(&job->records, json_array_unref);
    weekly_unref(job->weekly); g_free(job);
}
static gboolean weekly_job_current(WeeklyJob *job, GtkWidget *owner)
{
    RadioSPlayerWeekly *weekly = job->weekly;
    if (weekly->serial != job->serial || (!weekly->snapshot.armed && !(weekly->general && job->preview)) || weekly->closed) return FALSE;
    if (weekly->general && !same_client(weekly->ctx, weekly->client)) {
        weekly->pending = FALSE; weekly->discover_at = 0; return FALSE;
    }
    if (!weekly_current(weekly, owner) || radios_playout_ui_intent(weekly->ctx) != job->intent) {
        weekly_fail(weekly, NULL, "Program, veri oturumu veya player sahipliği değişti. Yeniden başlatın."); return FALSE;
    }
    return TRUE;
}
static JsonNode *weekly_body(RadioSPlayerWeekly *weekly)
{
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(body, json_object_new());
    json_object_set_string_member(json_node_get_object(body), "station_id", radios_data_client_station_id(weekly->client)); return body;
}
static gboolean weekly_request(WeeklyJob *job, GtkWidget *owner, const gchar *path, JsonNode *body, RadioSDataCallback callback)
{
    gboolean accepted = weekly_job_current(job, owner) && radios_data_request(job->weekly->ctx, owner, "POST", path,
        body, callback, weekly_job_ref(job), weekly_job_free);
    json_node_free(body); return accepted;
}
static gboolean weekly_record_valid(RadioSPlayerWeekly *weekly, JsonObject *record)
{
    JsonObject *original = weekly->record ? json_node_get_object(weekly->record) : NULL;
    JsonNode *config = record ? json_object_get_member(record, "config") : NULL;
    JsonNode *original_config = original ? json_object_get_member(original, "config") : NULL;
    return record && original && g_str_equal(text(record, "_id"), text(original, "_id")) &&
        radios_json_int(record, "_rev", 0) == radios_json_int(original, "_rev", 0) &&
        g_str_equal(text(record, "kind"), text(original, "kind")) &&
        (g_str_equal(text(record, "kind"), "weekly_programme") || (weekly->general && g_str_equal(text(record, "kind"), "automation_calendar") &&
        g_str_equal(text(record, "name"), "main"))) && g_str_equal(text(record, "status"), "active") &&
        g_str_equal(text(record, "station_id"), radios_data_client_station_id(weekly->client)) &&
        config && original_config && json_node_equal(config, original_config);
}
static gboolean general_record_changed(RadioSPlayerWeekly *weekly, JsonObject *record)
{
    JsonObject *original = weekly->record ? json_node_get_object(weekly->record) : NULL;
    if (!record || !original || !g_str_equal(text(record, "_id"), text(original, "_id")) ||
        !g_str_equal(text(record, "kind"), text(original, "kind")) || !*text(record, "name") ||
        !g_str_equal(text(record, "station_id"), radios_data_client_station_id(weekly->client)) ||
        radios_json_int(record, "_rev", 0) <= radios_json_int(original, "_rev", 0) || !object(record, "config")) return FALSE;
    if (g_str_equal(text(record, "status"), "disabled")) return TRUE;
    if (!g_str_equal(text(record, "status"), "active")) return FALSE;
    JsonArray *records = json_array_new(); json_array_add_element(records, copy_object(record));
    GError *error = NULL; RadioSWeeklyProgrammeSelection *probe = radios_weekly_programme_select_next(records, g_get_real_time(), &error);
    radios_weekly_programme_selection_free(probe); json_array_unref(records);
    gboolean valid = error == NULL; g_clear_error(&error); return valid;
}
static gboolean weekly_document_request(WeeklyJob *job, GtkWidget *owner, RadioSDataCallback callback)
{
    JsonNode *body = weekly_body(job->weekly); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "kind", text(json_node_get_object(job->weekly->record), "kind"));
    json_object_set_string_member(o, "document_id", text(json_node_get_object(job->weekly->record), "_id"));
    return weekly_request(job, owner, "/v1/workflows/desktop/get", body, callback);
}
static void weekly_automation_received(RadioSViewContext *, GtkWidget *, JsonNode *, const GError *, gpointer);
static gboolean weekly_remaining_retry(WeeklyJob *job, GtkWidget *owner, GError *error)
{
    if (!error || !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) return FALSE;
    RadioSPlayerWeekly *weekly = job->weekly;
    if (!job->reselections && job->sources && g_get_real_time() < job->occurrence->end_utc_us) {
        ++job->reselections; g_clear_pointer(&job->selection, json_node_free); g_clear_pointer(&job->prepared, json_node_free);
        weekly_automation_received(weekly->ctx, owner, job->sources, NULL, job);
    } else {
        gint64 now = g_get_real_time(); weekly->pending = FALSE; weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_WAITING;
        weekly->retry_utc_us = MIN(job->occurrence->end_utc_us, (now / (60 * G_USEC_PER_SEC) + 1) * (60 * G_USEC_PER_SEC));
        g_strlcpy(weekly->snapshot.error, "Tekrar ve zaman kurallarına uygun parçayı bekliyor.", sizeof weekly->snapshot.error);
    }
    return TRUE;
}
/* The queue is a projection of the verified configuration and the accepted
 * planner result (core radios_weekly_programme_queue_projection). The calendar
 * programme's own live settings are the default of its live blocks. */
static JsonNode *general_queue_projection(JsonObject *configuration, JsonObject *display, RadioSWeeklyProgrammeOccurrence *occurrence)
{
    JsonObject *event = occurrence && occurrence->event ? json_node_get_object(occurrence->event) : NULL;
    return radios_weekly_programme_queue_projection(configuration, display, object(event, "live"));
}
/* Kılavuz §03 "Giriş: N. sıra": the planner chooses an entry from the
 * preceding programme's complete rule, otherwise from the new programme's
 * own schema. Retries keep that rule and the same occurrence seed. */
static void general_entry_text(RadioSPlayerWeekly *weekly, guint order)
{
    weekly->snapshot.entry_source_order = order;
    if (order) g_snprintf(weekly->snapshot.entry_text, sizeof weekly->snapshot.entry_text, "Giriş: %u. sıra", order);
    else weekly->snapshot.entry_text[0] = '\0';
}
static JsonObject *schedule_calendar(RadioSPlayerWeekly *weekly)
{ return weekly->schedule ? object(json_node_get_object(weekly->schedule), "calendar") : NULL; }
static JsonObject *schedule_event(RadioSPlayerWeekly *weekly)
{
    JsonArray *events = array(schedule_calendar(weekly), "events");
    for (guint i = 0; weekly->general_event_id && events && i < json_array_get_length(events); ++i)
        if (g_str_equal(text(at(events, i), "id"), weekly->general_event_id)) return at(events, i);
    return NULL;
}
static JsonNode *general_entry_settings(JsonObject *calendar, JsonObject *event, const gchar *schema_id)
{
    JsonObject *definition = radios_weekly_programme_schema_definition(calendar, event, schema_id);
    JsonObject *settings = object(definition, "settings"), *entry = object(settings, "programme_entry");
    if (!entry) return NULL;
    JsonObject *carried = json_object_new();
    json_object_set_member(carried, "programme_entry", copy_object(entry));
    if (array(settings, "autodj_presets"))
        json_object_set_member(carried, "autodj_presets", json_node_copy(json_object_get_member(settings, "autodj_presets")));
    JsonNode *result = copy_object(carried); json_object_unref(carried); return result;
}
static JsonNode *general_entry_for(RadioSPlayerWeekly *weekly, WeeklyJob *job)
{
    if (!weekly->general || !job->occurrence) return NULL;
    if (weekly->general_entry_key && g_str_equal(weekly->general_entry_key, job->occurrence->occurrence_key)) return weekly->general_entry_active;
    g_clear_pointer(&weekly->general_entry_active, json_node_free);
    weekly->general_entry_active = g_steal_pointer(&weekly->general_entry_settings);
    if (!weekly->general_entry_active) {
        JsonObject *calendar = job->occurrence->calendar ? json_node_get_object(job->occurrence->calendar) : NULL;
        weekly->general_entry_active = general_entry_settings(calendar, json_node_get_object(job->occurrence->event), NULL);
    }
    g_free(weekly->general_entry_key); weekly->general_entry_key = g_strdup(job->occurrence->occurrence_key);
    return weekly->general_entry_active;
}
/* A slot ends: its governing schema (applied, else assigned) decides the
 * entry into the next programme. The applied schema belongs to that slot only. */
static void general_slot_ended(RadioSPlayerWeekly *weekly)
{
    JsonObject *event = schedule_event(weekly);
    const gchar *applied = weekly->general_schema_id && weekly->general_schema_event_id && weekly->general_event_id &&
        g_str_equal(weekly->general_schema_event_id, weekly->general_event_id) ? weekly->general_schema_id : NULL;
    g_clear_pointer(&weekly->general_entry_settings, json_node_free);
    if (event) weekly->general_entry_settings = general_entry_settings(schedule_calendar(weekly), event, applied);
    g_clear_pointer(&weekly->general_schema_id, g_free); g_clear_pointer(&weekly->general_schema_event_id, g_free);
    weekly->snapshot.schema_name[0] = '\0';
}
static void general_schema_reset(RadioSPlayerWeekly *weekly)
{
    g_clear_pointer(&weekly->general_schema_id, g_free); g_clear_pointer(&weekly->general_schema_event_id, g_free);
    g_clear_pointer(&weekly->general_entry_key, g_free);
    g_clear_pointer(&weekly->general_entry_settings, json_node_free); g_clear_pointer(&weekly->general_entry_active, json_node_free);
    weekly->snapshot.schema_name[0] = '\0'; general_entry_text(weekly, 0);
}
static void general_entry_reselect(RadioSPlayerWeekly *weekly)
{
    g_clear_pointer(&weekly->general_entry_key, g_free);
    g_clear_pointer(&weekly->general_entry_settings, json_node_free);
    g_clear_pointer(&weekly->general_entry_active, json_node_free);
}
/* Kılavuz §02: the schema leads the programme's order as its first assignment
 * (kept once) and names the live programme's automation. The record itself is
 * untouched: only the runtime weekly configuration changes. */
static gboolean schema_apply_event(JsonObject *calendar, JsonObject *event, const gchar *schema_id, GError **error)
{
    JsonArray *library = array(calendar, "rotation_library"), *rows = array(event, "rotations"); JsonObject *definition = NULL;
    for (guint i = 0; library && i < json_array_get_length(library); ++i)
        if (!g_strcmp0(text(at(library, i), "id"), schema_id)) { definition = at(library, i); break; }
    if (!definition || !definition_is_schema(definition)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Seçilen tanım çalan programın takvimindeki bir rotasyon şeması değil."); return FALSE;
    }
    if (!rows) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Çalan programın rotasyon listesi geçersiz."); return FALSE; }
    JsonObject *existing = NULL; guint expanded = json_array_get_length(array(definition, "sources"));
    for (guint i = 0; i < json_array_get_length(rows); ++i) {
        JsonObject *row = at(rows, i); if (!row) continue;
        if (!existing && !g_strcmp0(text(row, "rotation_id"), schema_id)) { existing = row; continue; }
        JsonObject *other = NULL;
        for (guint j = 0; library && *text(row, "rotation_id") && j < json_array_get_length(library); ++j)
            if (!g_strcmp0(text(at(library, j), "id"), text(row, "rotation_id"))) { other = at(library, j); break; }
        expanded += other ? json_array_get_length(array(other, "sources")) : 1;
    }
    if (expanded > 50) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Şema eklenince programın 50 playlist sınırı aşılır."); return FALSE; }
    JsonArray *ordered = json_array_new(); JsonObject *assignment = existing ? radios_programme_clone_object(existing) : json_object_new();
    if (!existing) {
        gchar *id = g_uuid_string_random(); json_object_set_string_member(assignment, "id", id); g_free(id);
        json_object_set_string_member(assignment, "rotation_id", schema_id);
    }
    json_array_add_object_element(ordered, assignment);
    for (guint i = 0; i < json_array_get_length(rows); ++i)
        if (at(rows, i) && at(rows, i) != existing) json_array_add_object_element(ordered, radios_programme_clone_object(at(rows, i)));
    json_object_set_array_member(event, "rotations", ordered);
    JsonObject *live = object(event, "live");
    if (live) json_object_set_string_member(live, "automation_id", schema_id);
    return TRUE;
}
/* Re-apply the slot's schema to a freshly selected runtime configuration of
 * the same programme, so discovery neither drops nor churns on it. */
static void general_schema_reapply(RadioSPlayerWeekly *weekly, RadioSWeeklyProgrammeSelection *selected, gint64 now)
{
    if (!weekly->general_schema_id || !selected || !selected->occurrence) return;
    if (g_strcmp0(weekly->general_schema_event_id, selected->occurrence->original_event_id)) { general_schema_reset(weekly); return; }
    JsonObject *config = json_node_get_object(selected->config), *calendar = object(config, "calendar");
    JsonArray *events = array(calendar, "events"); JsonObject *event = NULL;
    for (guint i = 0; events && i < json_array_get_length(events); ++i)
        if (g_str_equal(text(at(events, i), "id"), selected->occurrence->original_event_id)) { event = at(events, i); break; }
    JsonNode *backup = copy_object(config);
    RadioSWeeklyProgrammeOccurrence *again = event && schema_apply_event(calendar, event, weekly->general_schema_id, NULL) ?
        radios_weekly_programme_next(config, MAX(now, selected->occurrence->start_utc_us), NULL) : NULL;
    if (again && g_str_equal(again->original_event_id, selected->occurrence->original_event_id) && again->start_utc_us == selected->occurrence->start_utc_us) {
        radios_weekly_programme_occurrence_free(selected->occurrence); selected->occurrence = again; json_node_free(backup); return;
    }
    radios_weekly_programme_occurrence_free(again);
    json_node_free(selected->config); selected->config = backup; general_schema_reset(weekly);
}
static void weekly_document_final(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    WeeklyJob *job = value; RadioSPlayerWeekly *weekly = job->weekly;
    if (!weekly_job_current(job, owner)) return;
    JsonObject *record = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    if (error || !weekly_record_valid(weekly, record)) {
        if (!job->replace_remaining && weekly->general && ((!error && general_record_changed(weekly, record)) ||
            (error && error->domain == RADIOS_DATA_CLIENT_ERROR && error->code == RADIOS_DATA_CLIENT_ERROR_NOT_FOUND)))
            general_source_changed(weekly);
        else weekly_fail(weekly, error, "Kayıtlı program değişti. Yeniden yükleyin.");
        return;
    }
    gint64 now = g_get_real_time();
    if (job->replace_remaining) {
        GError *failure = NULL;
        if (now >= job->occurrence->end_utc_us || !weekly->owns_run || !weekly->schema_replace_id) {
            weekly_fail(weekly, NULL, "Şema hazırlanırken program tamamlandı; çalan kaynak korundu."); return;
        }
        if (!radios_playout_ui_replace_remaining_prepared(ctx, owner, json_node_get_object(job->prepared), job->intent, &failure)) {
            weekly_fail(weekly, failure, "Şema kalan yayın sırasına uygulanamadı; mevcut yayın korundu.");
            g_clear_error(&failure); return;
        }
        weekly->run_intent = radios_playout_ui_intent(ctx);
        g_free(weekly->general_schema_id); weekly->general_schema_id = g_steal_pointer(&weekly->schema_replace_id);
        g_free(weekly->general_schema_event_id); weekly->general_schema_event_id = g_strdup(weekly->general_event_id);
        g_clear_pointer(&weekly->schema_replace_backup, json_node_free);
        JsonObject *definition = radios_weekly_programme_schema_definition(schedule_calendar(weekly), schedule_event(weekly), weekly->general_schema_id);
        g_strlcpy(weekly->snapshot.schema_name, text(definition, "name"), sizeof weekly->snapshot.schema_name);
        general_entry_reselect(weekly);
        JsonObject *first = at(array(json_node_get_object(job->selection), "tracks"), 0);
        general_entry_text(weekly, (guint)radios_json_int(object(first, "programme_entry"), "source_order", 0));
        weekly->pending = FALSE; weekly->snapshot.error[0] = '\0';
        weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_PLAYING;
        if (weekly->queue) {
            JsonNode *accepted = radios_playout_ui_plan_snapshot(ctx);
            JsonNode *projection = general_queue_projection(object(json_node_get_object(job->prepared), "config"), accepted ? json_node_get_object(accepted) : NULL, job->occurrence);
            if (projection) weekly->queue(json_node_get_object(projection), TRUE, weekly->queue_user);
            g_clear_pointer(&projection, json_node_free); g_clear_pointer(&accepted, json_node_free);
        }
        return;
    }
    if (now >= job->occurrence->end_utc_us) { weekly->pending = FALSE; weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_WAITING; return; }
    if (weekly->general && !job->preview && weekly->snapshot.paused) {
        /* Pause may arrive during a due HTTP preparation. Resume prepares
         * afresh rather than installing audio behind the paused controls. */
        weekly->pending = FALSE; weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_WAITING; return;
    }
    if (weekly->general && job->preview) {
        RadioSPlayoutRequest request = {.station_id = radios_data_client_station_id(weekly->client),
            .database_file = ctx->config->database_file, .now_utc_us = MAX(now, job->occurrence->start_utc_us)};
        request.has_default_policy = radios_broadcast_transition_policy(ctx, &request.default_mode,
            &request.default_overlap_seconds, &request.default_beat_sync);
        GError *failure = NULL; RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(job->prepared), &request, &failure);
        if (!plan) { weekly_fail(weekly, failure, "Genel yayın önizlemesi geçersiz."); g_clear_error(&failure); return; }
        g_clear_pointer(&weekly->preview_config, json_node_free);
        weekly->preview_config = general_queue_projection(object(json_node_get_object(job->prepared), "config"),
            json_node_get_object(plan->display), job->occurrence);
        radios_playout_plan_free(plan); weekly->pending = FALSE;
        weekly->snapshot.state = weekly->snapshot.armed ? RADIOS_PLAYER_WEEKLY_WAITING : RADIOS_PLAYER_WEEKLY_LOADED;
        weekly->snapshot.error[0] = 0; weekly->discover_at = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
        weekly_display(weekly, job->occurrence, now);
        if (weekly->queue) weekly->queue(json_node_get_object(weekly->preview_config), FALSE, weekly->queue_user);
        return;
    }
    guint decks[32], count = 0; gboolean routed = FALSE; GError *failure = NULL;
    if (!radios_playout_prepared_weekly_pool(ctx, json_node_get_object(job->prepared), weekly->source, weekly->target,
            decks, &count, &routed, &failure)) {
        if (!weekly_remaining_retry(job, owner, failure)) weekly_fail(weekly, failure, "Program player dağılımı geçersiz.");
        g_clear_error(&failure); return;
    }
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx); RadioSAudioSnapshot snapshot = {0};
    if (engine) { radios_audio_engine_snapshot(engine, &snapshot); radios_audio_engine_unref(engine); }
    gboolean same = engine != NULL;
    for (guint i = 0; i < count && same; ++i) {
        guint deck = decks[i]; gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
        GtkWidget *current = g_object_get_data(G_OBJECT(ctx->parent), key); g_free(key);
        GtkWidget *original = g_weak_ref_get(&job->cards[deck]);
        same = original && current == original && !gtk_widget_in_destruction(current) && snapshot.decks[deck].generation == job->generations[deck];
        g_clear_object(&original);
    }
    if (!same) { weekly_fail(weekly, NULL, "Program hazırlanırken bir player değişti; mevcut kaynak korundu."); return; }
    const gchar *automation_id = text(record, "_id");
    gboolean assigned = job->loudness_binding_captured && ctx->players &&
        g_str_equal(job->loudness_binding.automation_id, automation_id) &&
        radios_automation_scope_binding_current(radios_player_workspace_topology(ctx->players), &job->loudness_binding, NULL);
    if (!radios_playout_ui_start_prepared_weekly_with_loudness_origin(ctx, owner, json_node_get_object(job->prepared),
            weekly->source, weekly->target, automation_id, assigned, &failure)) {
        if (!weekly_remaining_retry(job, owner, failure)) weekly_fail(weekly, failure, "Program başlatılamadı.");
        g_clear_error(&failure); return;
    }
    weekly->pending = FALSE; weekly->owns_run = TRUE;
    weekly->run_intent = radios_playout_ui_intent(ctx); weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_PLAYING;
    weekly->snapshot.error[0] = '\0'; weekly->retry_utc_us = 0; weekly->retry_failures = 0;
    weekly_display(weekly, job->occurrence, now);
    if (weekly->general && weekly->queue) {
        JsonNode *accepted = radios_playout_ui_plan_snapshot(ctx);
        JsonNode *projection = general_queue_projection(object(json_node_get_object(job->prepared), "config"),
            accepted ? json_node_get_object(accepted) : NULL, job->occurrence);
        if (projection) weekly->queue(json_node_get_object(projection), TRUE, weekly->queue_user);
        g_clear_pointer(&projection, json_node_free); g_clear_pointer(&accepted, json_node_free);
    }
}
static void weekly_programme_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; WeeklyJob *job = value; if (!weekly_job_current(job, owner)) return;
    GError *failure = error ? g_error_copy(error) : NULL;
    job->prepared = !failure && result && JSON_NODE_HOLDS_OBJECT(result) ?
        radios_automation_calendar_merge_selection(json_node_get_object(result), json_node_get_object(job->selection), &failure) : NULL;
    if (!job->prepared) { weekly_fail(job->weekly, failure, "Program medyası yeniden doğrulanamadı."); g_clear_error(&failure); return; }
    if (!weekly_document_request(job, owner, weekly_document_final)) weekly_fail(job->weekly, NULL, "Program kaydı yeniden doğrulanamadı.");
}
/* Prove actual prepared availability before classifying an unavailable slot.
 * Missing source/binding/type evidence remains a fatal validation failure. */
static gboolean general_unavailable_source(WeeklyJob *job)
{
    JsonNode *probe_event = copy_object(json_node_get_object(job->occurrence->event));
    JsonArray *probe_rules = array(json_node_get_object(probe_event), "rotations");
    for (guint i = 0; probe_rules && i < json_array_get_length(probe_rules); ++i) json_object_set_int_member(at(probe_rules, i), "weight", 0);
    GError *validation = NULL; JsonNode *probe = radios_automation_calendar_prepared_programme_at(json_node_get_object(probe_event),
        json_node_get_object(job->sources), radios_data_client_station_id(job->weekly->client), "general-availability-validation", 1,
        MAX(g_get_real_time(), job->occurrence->start_utc_us), &validation);
    gboolean strict = validation && g_error_matches(validation, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
    g_clear_pointer(&probe, json_node_free); json_node_free(probe_event); g_clear_error(&validation);
    if (!strict) return FALSE;
    JsonArray *rules = array(json_node_get_object(job->occurrence->event), "rotations");
    JsonArray *sources = array(job->sources ? json_node_get_object(job->sources) : NULL, "sources");
    gboolean unavailable = FALSE;
    for (guint i = 0; rules && i < json_array_get_length(rules); ++i) {
        JsonObject *rule = at(rules, i), *matched = NULL;
        if (radios_json_int(rule, "weight", 1) == 0 || radios_json_int(rule, "calendar_group_weight", 100) == 0) continue;
        for (guint j = 0; sources && j < json_array_get_length(sources); ++j) {
            JsonObject *candidate = at(sources, j);
            if (g_str_equal(text(candidate, "source_id"), text(rule, "id"))) { matched = candidate; break; }
        }
        JsonArray *tracks = array(matched, "tracks"), *reasons = array(matched, "reasons");
        if (!matched || !g_str_equal(text(matched, "playlist_id"), text(rule, "playlist_id")) || !tracks || !reasons) return FALSE;
        for (guint j = 0; j < json_array_get_length(reasons); ++j) {
            JsonNode *reason = json_array_get_element(reasons, j);
            if (!reason || !JSON_NODE_HOLDS_VALUE(reason) || json_node_get_value_type(reason) != G_TYPE_STRING ||
                (!g_str_equal(json_node_get_string(reason), "playlist_disabled") && !g_str_equal(json_node_get_string(reason), "playlist_draft_unavailable"))) return FALSE;
        }
        gboolean eligible = FALSE;
        const gchar *kind = radios_json_string(rule, "media_kind", "all");
        for (guint j = 0; j < json_array_get_length(tracks); ++j) {
            JsonObject *row = at(tracks, j); JsonNode *available = row ? json_object_get_member(row, "eligible") : NULL;
            if (!available || !JSON_NODE_HOLDS_VALUE(available) || json_node_get_value_type(available) != G_TYPE_BOOLEAN) return FALSE;
            eligible |= json_node_get_boolean(available) && (g_str_equal(kind, "all") || g_str_equal(kind, text(object(row, "metadata"), "kind")));
        }
        unavailable |= !eligible;
    }
    return unavailable;
}
static void weekly_automation_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; WeeklyJob *job = value; RadioSPlayerWeekly *weekly = job->weekly;
    if (!weekly_job_current(job, owner)) return;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        if (!job->replace_remaining && weekly->general && error && error->domain == RADIOS_DATA_CLIENT_ERROR && error->code == RADIOS_DATA_CLIENT_ERROR_NOT_FOUND)
            general_skip_slot(weekly, "Program kaynağı artık yok; sonraki program bekleniyor.");
        else weekly_fail(weekly, error, "Program kaynakları doğrulanamadı.");
        return;
    }
    gint64 now = g_get_real_time();
    if (now >= job->occurrence->end_utc_us) {
        if (job->replace_remaining) weekly_fail(weekly, NULL, "Program zamanı tamamlandı; şema değiştirilmedi.");
        else { weekly->pending = FALSE; weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_WAITING; }
        return;
    }
    if (!job->sources) job->sources = copy_object(json_node_get_object(result));
    JsonNode *sources = copy_object(json_node_get_object(result));
    JsonNode *turns = radios_playout_ui_calendar_turns(weekly->ctx);
    if (turns) json_object_set_member(json_node_get_object(sources), "calendar_turns", turns);
    JsonNode *checkpoint = radios_playout_ui_probability_checkpoint(weekly->ctx, TRUE);
    if (checkpoint) json_object_set_member(json_node_get_object(sources), "policy_selection_checkpoint", checkpoint);
    JsonNode *history = radios_playout_ui_history(weekly->ctx);
    if (history) {
        JsonObject *record = json_object_new(); json_object_set_member(record, "config", history);
        json_object_set_object_member(json_node_get_object(sources), "history", record);
    }
    /* Actual receipts may begin during the two captures above. Fence their
     * timestamps against the current clock, then recheck the real window. */
    now = g_get_real_time();
    if (now >= job->occurrence->end_utc_us) {
        json_node_free(sources);
        if (job->replace_remaining) { weekly_fail(weekly, NULL, "Program zamanı tamamlandı; şema değiştirilmedi."); return; }
        weekly->pending = FALSE;
        weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_WAITING; return;
    }
    GError *failure = NULL;
    /* Preserve the schema's weighted alternatives and assigned presets. A
     * fixed source-order override here would bypass every probability path. */
    JsonNode *replacement_entry = job->replace_remaining ? general_entry_settings(schedule_calendar(weekly), schedule_event(weekly), weekly->schema_replace_id) : NULL;
    JsonNode *entry = job->replace_remaining ? replacement_entry : general_entry_for(weekly, job);
    if (entry) json_object_set_member(json_node_get_object(job->occurrence->event), "schema_entry_settings", copy_object(json_node_get_object(entry)));
    g_clear_pointer(&replacement_entry, json_node_free);
    gchar *seed = g_compute_checksum_for_string(G_CHECKSUM_SHA256, job->occurrence->occurrence_key, -1);
    gint64 effective = MAX(now, job->occurrence->start_utc_us);
    if (job->replace_remaining) {
        RadioSPlayoutHeaderSnapshot header = {0};
        if (radios_playout_ui_header_snapshot(weekly->ctx, &header)) effective += MAX((gint64)0, header.track_remaining_us);
    }
    job->selection = radios_automation_calendar_prepared_programme_at(json_node_get_object(job->occurrence->event),
        json_node_get_object(sources), radios_data_client_station_id(weekly->client), seed, 200,
        effective, &failure);
    g_free(seed); json_node_free(sources);
    if (!job->selection) {
        gboolean unavailable = failure && g_error_matches(failure, G_IO_ERROR, G_IO_ERROR_INVALID_DATA) &&
            (g_str_equal(failure->message, "Rotasyon playlisti değişti veya çalınabilir kaynak hazırlığı yok.") ||
             g_str_equal(failure->message, "Rotasyon playlistinde çalınabilir doğrulanmış medya yok.") ||
             g_str_equal(failure->message, "Rotasyon playlistinde seçilen medya türünde çalınabilir doğrulanmış medya yok."));
        if (job->replace_remaining) weekly_fail(weekly, failure, "Yeni şema için uygun medya hazırlanamadı; mevcut yayın korundu.");
        else if (weekly->general && unavailable && general_unavailable_source(job)) general_skip_slot(weekly, "Bu programın doğrulanmış medyası yok; sonraki program bekleniyor.");
        else if (failure && g_error_matches(failure, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            weekly->pending = FALSE; weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_WAITING;
            weekly->retry_utc_us = MIN(job->occurrence->end_utc_us, (now / (60 * G_USEC_PER_SEC) + 1) * (60 * G_USEC_PER_SEC));
            g_strlcpy(weekly->snapshot.error, "Tekrar ve zaman kurallarına uygun parçayı bekliyor.", sizeof weekly->snapshot.error);
        } else weekly_fail(weekly, failure, "Program kurallarına uyan parça yok.");
        g_clear_error(&failure); return;
    }
    if (weekly->general) {
        JsonObject *first = at(array(json_node_get_object(job->selection), "tracks"), 0);
        general_entry_text(weekly, (guint)radios_json_int(object(first, "programme_entry"), "source_order", 0));
    }
    JsonNode *body = weekly_body(weekly); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "kind", "programme"); json_object_set_member(o, "config", copy_object(object(json_node_get_object(job->selection), "config")));
    if (!weekly_request(job, owner, "/v1/workflows/desktop-playout/prepare", body, weekly_programme_received)) weekly_fail(weekly, NULL, "Program medyası hazırlanamadı.");
}
/* The general programme assigned to a player cluster (or to one player)
 * plays on that scope's players, its rotations' per-kind AutoDJ channels
 * mapped onto them exactly as the scope's own flow maps them. NULL without
 * an error: the programme is assigned nowhere and keeps the general players. */
static JsonNode *general_scope_event(RadioSPlayerWeekly *weekly, JsonObject *event, GError **error)
{
    const gchar *id = weekly->record ? text(json_node_get_object(weekly->record), "_id") : NULL;
    if (!weekly->general || !id || !*id || !weekly->ctx->players || !radios_player_workspace_ready(weekly->ctx->players)) return NULL;
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(weekly->ctx->players);
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER}; guint found = 0;
    for (guint i = 0; i < topology->cluster_count; ++i)
        if (g_str_equal(topology->clusters[i].automation_id, id) && !found++)
            g_strlcpy(scope.cluster_id, topology->clusters[i].id, sizeof scope.cluster_id);
    if (!found)
        for (guint deck = 0; deck < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++deck) {
            const RadioSPlayerDefinition *player = radios_player_topology_player_const(topology, deck);
            if (player && player->enabled && g_str_equal(player->automation_id, id) && !found++)
                scope = (RadioSAutomationScope){.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = deck};
        }
    if (found > 1) {
        /* An ambiguous assignment keeps today's general players, said once per occurrence. */
        if (!weekly->closed && weekly->ctx->notify)
            weekly->ctx->notify(weekly->ctx->notify_data, "Program birden fazla kümeye ya da oynatıcıya atanmış; genel yayın oynatıcılarıyla çalıyor.");
        return NULL;
    }
    RadioSAutomationBinding binding;
    if (!found || !radios_automation_scope_resolve(topology, &scope, &binding, error)) return NULL;
    if (!binding.count) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Programın atandığı kümede oynatıcı yok."); return NULL; }
    gchar *notice = NULL; JsonNode *routed = radios_automation_scope_route_event_notice(&binding, event, &notice, error);
    if (routed && notice && !weekly->closed && weekly->ctx->notify) weekly->ctx->notify(weekly->ctx->notify_data, notice);
    g_free(notice); return routed;
}
static void weekly_document_first(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; WeeklyJob *job = value; RadioSPlayerWeekly *weekly = job->weekly;
    if (!weekly_job_current(job, owner)) return;
    JsonObject *record = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    if (error || !weekly_record_valid(weekly, record)) {
        if (!job->replace_remaining && weekly->general && ((!error && general_record_changed(weekly, record)) ||
            (error && error->domain == RADIOS_DATA_CLIENT_ERROR && error->code == RADIOS_DATA_CLIENT_ERROR_NOT_FOUND)))
            general_source_changed(weekly);
        else weekly_fail(weekly, error, "Kayıtlı program değişti veya kaldırıldı. Yeniden yükleyin.");
        return;
    }
    GError *failure = NULL;
    if (!job->occurrence) job->occurrence = weekly_next(weekly,
        MAX(MAX(g_get_real_time(), weekly->completed_end_utc_us), weekly->general_floor_utc_us), &failure);
    if (!job->occurrence || (!job->occurrence->active && !(weekly->general && job->preview))) {
        if (job->replace_remaining) { weekly_fail(weekly, failure, "Program zamanı tamamlandı; şema değiştirilmedi."); g_clear_error(&failure); return; }
        weekly->pending = FALSE;
        if (failure) weekly_fail(weekly, failure, "Program zamanı geçersiz.");
        else weekly->snapshot.state = job->occurrence ? RADIOS_PLAYER_WEEKLY_WAITING : RADIOS_PLAYER_WEEKLY_FINISHED;
        g_clear_error(&failure); return;
    }
    JsonNode *scoped = general_scope_event(weekly, json_node_get_object(job->occurrence->event), &failure);
    if (!scoped && failure) { weekly_fail(weekly, failure, "Programın atandığı kümeye yerleşemedi."); g_clear_error(&failure); return; }
    /* Later stages read each row's player map from the occurrence's event,
     * as the scope execution does once it routed it. */
    if (scoped) { json_node_free(job->occurrence->event); job->occurrence->event = scoped; }
    JsonNode *configuration = radios_automation_calendar_prepare_config(json_node_get_object(job->occurrence->event), NULL, &failure);
    if (!configuration) { weekly_fail(weekly, failure, "Program rotasyonları geçersiz."); g_clear_error(&failure); return; }
    JsonNode *body = weekly_body(weekly); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "kind", "automation"); json_object_set_member(o, "config", configuration);
    if (!weekly_request(job, owner, "/v1/workflows/desktop-playout/prepare", body, weekly_automation_received)) weekly_fail(weekly, NULL, "Program kaynak isteği gönderilemedi.");
}
static gboolean weekly_prepare(RadioSPlayerWeekly *weekly, GtkWidget *owner)
{
    WeeklyJob *job = g_new0(WeeklyJob, 1); job->references = 1; job->weekly = weekly; ++weekly->references;
    job->serial = weekly->serial; job->intent = radios_playout_ui_intent(weekly->ctx);
    job->replace_remaining = weekly->schema_replace_id != NULL;
    if (weekly->source < 32 && weekly->ctx->players && radios_player_workspace_ready(weekly->ctx->players)) {
        RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = weekly->source};
        job->loudness_binding_captured = radios_automation_scope_resolve(
            radios_player_workspace_topology(weekly->ctx->players), &scope, &job->loudness_binding, NULL) &&
            weekly->record && g_str_equal(job->loudness_binding.automation_id, text(json_node_get_object(weekly->record), "_id"));
    }
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(weekly->ctx); RadioSAudioSnapshot snapshot = {0};
    if (engine) { radios_audio_engine_snapshot(engine, &snapshot); radios_audio_engine_unref(engine); }
    for (guint deck = 0; deck < 32; ++deck) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
        GtkWidget *card = g_object_get_data(G_OBJECT(weekly->ctx->parent), key); g_free(key);
        g_weak_ref_init(&job->cards[deck], card); job->generations[deck] = snapshot.decks[deck].generation;
    }
    weekly->pending = TRUE; weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_PREPARING;
    gboolean accepted = engine && weekly_document_request(job, owner, weekly_document_first);
    if (!accepted) weekly_fail(weekly, NULL, "Program hazırlığı başlatılamadı.");
    weekly_job_free(job); return accepted;
}
static void weekly_release_run(RadioSPlayerWeekly *weekly)
{
    if (weekly->schema_replace_id) { ++weekly->serial; weekly->pending = FALSE; }
    g_clear_pointer(&weekly->schema_replace_backup, json_node_free);
    g_clear_pointer(&weekly->schema_replace_id, g_free);
    if (weekly->owns_run && radios_playout_ui_intent(weekly->ctx) == weekly->run_intent)
        radios_playout_ui_action(weekly->ctx, RADIOS_PLAYOUT_STOP, NULL);
    weekly->owns_run = FALSE; weekly->run_intent = radios_playout_ui_intent(weekly->ctx);
}
static gboolean weekly_tick(gpointer value)
{
    RadioSPlayerWeekly *weekly = value;
    if (weekly->closed) return G_SOURCE_REMOVE;
    if (weekly->general) return general_tick(weekly);
    if (!weekly->snapshot.armed) {
        if (weekly->record && weekly->snapshot.state == RADIOS_PLAYER_WEEKLY_LOADED) {
            RadioSWeeklyProgrammeOccurrence *next = weekly_next(weekly, g_get_real_time(), NULL);
            weekly_display(weekly, next, g_get_real_time()); radios_weekly_programme_occurrence_free(next);
        }
        return G_SOURCE_CONTINUE;
    }
    GtkWidget *owner = g_weak_ref_get(&weekly->owner);
    if (!weekly_current(weekly, owner)) {
        weekly_release_run(weekly); weekly_fail(weekly, NULL, "Veri oturumu veya rotasyon değişti. Programı yeniden yükleyin.");
        g_clear_object(&owner); return G_SOURCE_CONTINUE;
    }
    gint64 now = g_get_real_time();
    if (weekly->owns_run) {
        if (radios_playout_ui_intent(weekly->ctx) != weekly->run_intent) {
            weekly->owns_run = FALSE; weekly_fail(weekly, NULL, "Program yürütücüsü değişti; yeni yayın korundu."); g_object_unref(owner); return G_SOURCE_CONTINUE;
        }
        RadioSPlayoutSnapshot run = {0}; radios_playout_ui_get_snapshot(weekly->ctx, &run);
        weekly->snapshot.paused = run.state == RADIOS_PLAYOUT_PAUSED;
        weekly->snapshot.remaining_us = MAX((gint64)0, weekly->snapshot.end_utc_us - now);
        if (now >= weekly->snapshot.end_utc_us) {
            weekly->completed_end_utc_us = MAX(weekly->completed_end_utc_us, weekly->snapshot.end_utc_us); weekly_release_run(weekly);
        } else if (run.state == RADIOS_PLAYOUT_ERROR || run.state == RADIOS_PLAYOUT_IDLE) {
            weekly_release_run(weekly); weekly_fail(weekly, NULL, run.error[0] ? run.error : "Program oynatımı kesildi.");
            g_object_unref(owner); return G_SOURCE_CONTINUE;
        } else if (run.state == RADIOS_PLAYOUT_FINISHED) weekly_release_run(weekly);
        else { g_object_unref(owner); return G_SOURCE_CONTINUE; }
    }
    if (!weekly->owns_run && radios_playout_ui_intent(weekly->ctx) != weekly->run_intent) {
        weekly_fail(weekly, NULL, "Beklenen program sırasında başka bir yayın komutu verildi; yeni yayın korundu.");
        g_object_unref(owner); return G_SOURCE_CONTINUE;
    }
    if (weekly->pending) { g_object_unref(owner); return G_SOURCE_CONTINUE; }
    GError *error = NULL;
    RadioSWeeklyProgrammeOccurrence *occurrence = weekly_next(weekly, MAX(now, weekly->completed_end_utc_us), &error);
    weekly_display(weekly, occurrence, now);
    if (error) { weekly_fail(weekly, error, "Program zamanı geçersiz."); g_clear_error(&error); }
    else if (!occurrence) { weekly->snapshot.armed = FALSE; weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_FINISHED; }
    else if (occurrence->active && now >= weekly->retry_utc_us) weekly_prepare(weekly, owner);
    else weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_WAITING;
    radios_weekly_programme_occurrence_free(occurrence); g_object_unref(owner); return G_SOURCE_CONTINUE;
}
RadioSPlayerWeekly *radios_player_weekly_new(RadioSViewContext *ctx, GtkWidget *owner, guint source)
{
    if (!ctx || !owner || source >= 32) return NULL;
    RadioSPlayerWeekly *weekly = g_new0(RadioSPlayerWeekly, 1); weekly->references = 1;
    weekly->ctx = ctx; weekly->source = source; weekly->target = G_MAXUINT; g_weak_ref_init(&weekly->owner, owner);
    weekly->timer = g_timeout_add(250, weekly_tick, weekly); return weekly;
}
void radios_player_weekly_stop(RadioSPlayerWeekly *weekly)
{
    if (!weekly || weekly->closed) return;
    ++weekly->serial; weekly_release_run(weekly); weekly->pending = FALSE;
    if (weekly->general_selection_cancel) g_cancellable_cancel(weekly->general_selection_cancel);
    g_clear_object(&weekly->general_selection_cancel);
    if (weekly->general) general_schema_reset(weekly);
    weekly->snapshot.armed = weekly->snapshot.paused = FALSE; weekly->retry_utc_us = weekly->completed_end_utc_us = 0;
    weekly->snapshot.state = weekly->record ? RADIOS_PLAYER_WEEKLY_LOADED : RADIOS_PLAYER_WEEKLY_EMPTY;
}
void radios_player_weekly_free(RadioSPlayerWeekly *weekly)
{
    if (!weekly || weekly->closed) return;
    radios_player_weekly_stop(weekly); weekly->closed = TRUE;
    if (weekly->timer) { g_source_remove(weekly->timer); weekly->timer = 0; }
    weekly_unref(weekly);
}
gboolean radios_player_weekly_load(RadioSPlayerWeekly *weekly, JsonObject *record, RadioSDataClient *client, GError **error)
{
    JsonObject *config = object(record, "config");
    if (!weekly || weekly->closed || !record || !config || !same_client(weekly->ctx, client) ||
        !g_uuid_string_is_valid(text(record, "_id")) || radios_json_int(record, "_rev", 0) < 1 ||
        !g_str_equal(text(record, "kind"), "weekly_programme") || !g_str_equal(text(record, "status"), "active") ||
        !g_str_equal(text(record, "station_id"), radios_data_client_station_id(client)))
        return g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Kayıtlı program kimliği veya veri oturumu geçersiz."), FALSE;
    GError *failure = NULL; RadioSWeeklyProgrammeOccurrence *check = radios_weekly_programme_next(config, g_get_real_time(), &failure);
    radios_weekly_programme_occurrence_free(check);
    if (failure) { g_propagate_error(error, failure); return FALSE; }
    radios_player_weekly_stop(weekly); g_clear_pointer(&weekly->record, json_node_free);
    if (weekly->client) radios_data_client_unref(weekly->client);
    weekly->record = copy_object(record); weekly->client = radios_data_client_ref(client);
    weekly->snapshot = (RadioSPlayerWeeklySnapshot){.state = RADIOS_PLAYER_WEEKLY_LOADED,
        .repeat = json_object_get_boolean_member(config, "repeat")};
    g_strlcpy(weekly->snapshot.programme_name, text(record, "name"), sizeof weekly->snapshot.programme_name);
    gint64 anchor = 0, period = 0; weekly_bounds(config, &anchor, &period); weekly->stop_after_utc_us = anchor + period;
    RadioSWeeklyProgrammeOccurrence *next = weekly_next(weekly, g_get_real_time(), NULL); weekly_display(weekly, next, g_get_real_time());
    radios_weekly_programme_occurrence_free(next); return TRUE;
}
void radios_player_weekly_set_repeat(RadioSPlayerWeekly *weekly, gboolean repeat)
{
    if (!weekly || weekly->closed || !weekly->record) return;
    weekly->snapshot.repeat = repeat;
    gint64 anchor = 0, period = 0;
    if (weekly_bounds(weekly_config(weekly), &anchor, &period)) {
        gint64 now = g_get_real_time();
        gint64 reference = weekly->snapshot.armed && weekly->snapshot.start_utc_us > 0 &&
            (weekly->owns_run || weekly->pending) ? weekly->snapshot.start_utc_us : now;
        gint64 cycle = weekly->snapshot.armed && reference > anchor ? (reference - anchor) / period : 0;
        weekly->stop_after_utc_us = anchor + (cycle + 1) * period;
    }
    RadioSWeeklyProgrammeOccurrence *next = weekly_next(weekly, g_get_real_time(), NULL); weekly_display(weekly, next, g_get_real_time());
    radios_weekly_programme_occurrence_free(next);
}
gboolean radios_player_weekly_start(RadioSPlayerWeekly *weekly, guint target, GError **error)
{
    GtkWidget *owner = weekly ? g_weak_ref_get(&weekly->owner) : NULL;
    gboolean current = weekly && weekly->record && weekly_current(weekly, owner) && weekly->ctx->playout;
    if (!current || weekly->snapshot.armed) {
        g_clear_object(&owner); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Program hazır değil veya zaten başlatıldı."); return FALSE;
    }
    RadioSPlayoutSnapshot run = {0}; radios_playout_ui_get_snapshot(weekly->ctx, &run);
    if (run.state != RADIOS_PLAYOUT_IDLE && run.state != RADIOS_PLAYOUT_FINISHED && run.state != RADIOS_PLAYOUT_ERROR) {
        g_object_unref(owner); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Genel yayın sırası kullanımda; mevcut yayın korundu."); return FALSE;
    }
    ++weekly->serial; weekly->target = target; weekly->snapshot.armed = TRUE;
    weekly->snapshot.error[0] = '\0'; weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_WAITING;
    weekly->run_intent = radios_playout_ui_intent(weekly->ctx); weekly_tick(weekly); g_object_unref(owner); return TRUE;
}
gboolean radios_player_weekly_get_snapshot(RadioSPlayerWeekly *weekly, RadioSPlayerWeeklySnapshot *snapshot)
{
    if (!weekly || weekly->closed || !snapshot) return FALSE;
    *snapshot = weekly->snapshot; return TRUE;
}

gboolean radios_player_weekly_header_snapshot(RadioSPlayerWeekly *weekly, RadioSPlayerWeeklySnapshot *snapshot)
{
    if (!snapshot) return FALSE;
    memset(snapshot, 0, sizeof *snapshot);
    if (!weekly || weekly->closed || !weekly->owns_run ||
        weekly->run_intent != radios_playout_ui_intent(weekly->ctx) ||
        !weekly->snapshot.current_event_name[0] || !weekly->snapshot.end_utc_us) return FALSE;
    *snapshot = weekly->snapshot; return TRUE;
}

static void general_source_changed(RadioSPlayerWeekly *weekly)
{
    weekly->pending = FALSE; weekly->general_after_utc_us = weekly->completed_end_utc_us;
    weekly->general_floor_utc_us = weekly->general_end_utc_us = 0;
    g_clear_pointer(&weekly->record, json_node_free); g_clear_pointer(&weekly->schedule, json_node_free);
    weekly->discover_at = 0; weekly->retry_utc_us = 0;
    weekly->snapshot.state = weekly->snapshot.armed ? RADIOS_PLAYER_WEEKLY_WAITING : RADIOS_PLAYER_WEEKLY_LOADED;
    g_strlcpy(weekly->snapshot.error, "Program güncellendi; takvim yeniden okunuyor.", sizeof weekly->snapshot.error);
}
static void general_skip_slot(RadioSPlayerWeekly *weekly, const gchar *message)
{
    weekly->pending = FALSE; weekly->general_after_utc_us = weekly->completed_end_utc_us;
    if (!weekly->unavailable_events) weekly->unavailable_events = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    JsonObject *record = weekly->record ? json_node_get_object(weekly->record) : NULL;
    gchar *key = g_strdup_printf("%s:%s:%" G_GINT64_FORMAT ":%s", text(record, "kind"), text(record, "_id"),
        radios_json_int(record, "_rev", 0), weekly->general_event_id ? weekly->general_event_id : "");
    g_hash_table_add(weekly->unavailable_events, key); weekly->unavailable_until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    g_clear_pointer(&weekly->record, json_node_free); g_clear_pointer(&weekly->schedule, json_node_free);
    weekly->discover_at = 0; weekly->retry_utc_us = 0;
    weekly->snapshot.state = weekly->snapshot.armed ? RADIOS_PLAYER_WEEKLY_WAITING : RADIOS_PLAYER_WEEKLY_LOADED;
    g_strlcpy(weekly->snapshot.error, message, sizeof weekly->snapshot.error);
}
static void general_projection_clear(RadioSPlayerWeekly *weekly)
{
    g_clear_pointer(&weekly->preview_config, json_node_free);
    if (weekly->queue) weekly->queue(NULL, FALSE, weekly->queue_user);
}
static JsonArray *general_available_records(RadioSPlayerWeekly *weekly, JsonArray *records)
{
    JsonArray *available = json_array_new();
    if (weekly->unavailable_events && g_get_monotonic_time() >= weekly->unavailable_until) g_hash_table_remove_all(weekly->unavailable_events);
    for (guint i = 0; i < json_array_get_length(records); ++i) {
        JsonObject *original = at(records, i); JsonNode *copy = copy_object(original); JsonObject *record = json_node_get_object(copy);
        JsonObject *config = object(record, "config"); JsonArray *events = array(g_str_equal(text(record, "kind"), "weekly_programme") ? object(config, "calendar") : config, "events");
        for (guint j = 0; weekly->unavailable_events && events && j < json_array_get_length(events);) {
            gchar *key = g_strdup_printf("%s:%s:%" G_GINT64_FORMAT ":%s", text(record, "kind"), text(record, "_id"),
                radios_json_int(record, "_rev", 0), text(at(events, j), "id"));
            gboolean excluded = g_hash_table_contains(weekly->unavailable_events, key); g_free(key);
            if (excluded) json_array_remove_element(events, j); else ++j;
        }
        json_array_add_element(available, copy);
    }
    return available;
}
static gboolean general_records_append(WeeklyJob *job, JsonNode *result, const GError *error)
{
    JsonArray *records = result && JSON_NODE_HOLDS_OBJECT(result) ? array(json_node_get_object(result), "records") : NULL;
    if (error || !records) { weekly_fail(job->weekly, error, "Genel yayın takvim kayıtları okunamadı."); return FALSE; }
    for (guint i = 0; i < json_array_get_length(records); ++i) {
        JsonObject *record = at(records, i);
        if (!record || !g_str_equal(text(record, "station_id"), radios_data_client_station_id(job->weekly->client))) {
            weekly_fail(job->weekly, NULL, "Genel yayın takvim kaydının istasyon kimliği değişti."); return FALSE;
        }
        json_array_add_element(job->records, copy_object(record));
    }
    return TRUE;
}
typedef struct {
    JsonArray *records;
    gint64 now, floor;
    RadioSWeeklyProgrammeSelection *selected, *following;
} GeneralSelection;
static void general_selection_free(gpointer value)
{
    GeneralSelection *selection = value;
    json_array_unref(selection->records);
    radios_weekly_programme_selection_free(selection->selected);
    radios_weekly_programme_selection_free(selection->following);
    g_free(selection);
}
static void general_selection_work(GTask *task, gpointer source, gpointer value, GCancellable *cancel)
{
    (void)source; GeneralSelection *selection = value; GError *failure = NULL;
    if (g_task_return_error_if_cancelled(task)) return;
    selection->selected = radios_weekly_programme_select_running(selection->records, selection->now, selection->floor, &failure);
    if (!failure && selection->selected && !g_cancellable_is_cancelled(cancel)) {
        gint64 floor = selection->selected->occurrence->end_utc_us;
        selection->following = radios_weekly_programme_select_running(selection->records, floor, floor, NULL);
    }
    if (failure) g_task_return_error(task, failure);
    else if (!g_task_return_error_if_cancelled(task)) g_task_return_boolean(task, TRUE);
}
static void general_selection_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; WeeklyJob *job = data; RadioSPlayerWeekly *weekly = job->weekly;
    GTask *task = G_TASK(result); GeneralSelection *selection = g_task_get_task_data(task);
    GtkWidget *owner = g_weak_ref_get(&weekly->owner);
    GError *failure = NULL; gboolean valid = g_task_propagate_boolean(task, &failure);
    if (!owner || gtk_widget_in_destruction(owner) || !weekly_job_current(job, owner)) goto done;
    g_clear_object(&weekly->general_selection_cancel);
    if (!valid) { weekly_fail(weekly, failure, "Genel yayın takvimi geçersiz."); goto done; }
    gint64 now = g_get_real_time();
    /* A delayed result may cross the selected slot's end, or a system clock
     * correction may expose a different active slot. Re-select honestly. */
    if (now < selection->now || (selection->selected && now >= selection->selected->occurrence->end_utc_us)) {
        weekly->pending = FALSE; weekly->discover_at = 0; goto done;
    }
    g_clear_pointer(&weekly->general_following, radios_weekly_programme_selection_free);
    weekly->general_following_floor = selection->selected ? selection->selected->occurrence->end_utc_us : 0;
    weekly->general_following = selection->following; selection->following = NULL;
    RadioSWeeklyProgrammeSelection *selected = selection->selected; selection->selected = NULL;
    if (selected) {
        JsonObject *chosen = json_node_get_object(selected->record);
        for (guint i = 0; i < json_array_get_length(job->records); ++i) {
            JsonObject *original = at(job->records, i);
            if (g_str_equal(text(original, "kind"), text(chosen, "kind")) && g_str_equal(text(original, "_id"), text(chosen, "_id")) &&
                radios_json_int(original, "_rev", 0) == radios_json_int(chosen, "_rev", 0)) {
                json_node_free(selected->record); selected->record = copy_object(original); break;
            }
        }
    }
    weekly->discover_at = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    if (!selected) {
        weekly->pending = FALSE; g_clear_pointer(&weekly->record, json_node_free); g_clear_pointer(&weekly->schedule, json_node_free);
        weekly->snapshot.programme_name[0] = 0; weekly->snapshot.repeat = FALSE;
        weekly->snapshot.state = weekly->snapshot.armed ? RADIOS_PLAYER_WEEKLY_WAITING : RADIOS_PLAYER_WEEKLY_EMPTY;
        weekly->snapshot.error[0] = 0; weekly_display(weekly, NULL, now); general_projection_clear(weekly); goto done;
    }
    general_schema_reapply(weekly, selected, now);
    gboolean unchanged = weekly->record && weekly->schedule && weekly->preview_config &&
        json_node_equal(weekly->record, selected->record) && json_node_equal(weekly->schedule, selected->config) &&
        weekly->general_floor_utc_us == selected->occurrence->start_utc_us;
    if (unchanged) {
        weekly->pending = FALSE; weekly->snapshot.state = weekly->snapshot.armed ? RADIOS_PLAYER_WEEKLY_WAITING : RADIOS_PLAYER_WEEKLY_LOADED;
        weekly->snapshot.error[0] = 0;
        weekly_display(weekly, selected->occurrence, now); radios_weekly_programme_selection_free(selected); goto done;
    }
    g_clear_pointer(&weekly->record, json_node_free); g_clear_pointer(&weekly->schedule, json_node_free);
    weekly->record = selected->record; selected->record = NULL;
    weekly->schedule = selected->config; selected->config = NULL;
    weekly->general_floor_utc_us = selected->occurrence->start_utc_us;
    weekly->general_end_utc_us = selected->occurrence->end_utc_us;
    g_free(weekly->general_event_id); weekly->general_event_id = g_strdup(selected->occurrence->original_event_id);
    weekly->snapshot.repeat = json_object_get_boolean_member(json_node_get_object(weekly->schedule), "repeat");
    gint64 anchor = 0, period = 0; weekly_bounds(json_node_get_object(weekly->schedule), &anchor, &period); weekly->stop_after_utc_us = anchor + period;
    g_strlcpy(weekly->snapshot.programme_name, text(json_node_get_object(weekly->record), "name"), sizeof weekly->snapshot.programme_name);
    weekly_display(weekly, selected->occurrence, now);
    job->occurrence = selected->occurrence; selected->occurrence = NULL; radios_weekly_programme_selection_free(selected);
    if (!weekly_document_request(job, owner, weekly_document_first)) weekly_fail(weekly, NULL, "Genel yayın kaydı yeniden okunamadı.");
done:
    g_clear_error(&failure); g_clear_object(&owner); weekly_job_free(job);
}
static void general_weekly_records(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; WeeklyJob *job = data; RadioSPlayerWeekly *weekly = job->weekly;
    if (!weekly_job_current(job, owner) || !general_records_append(job, result, error)) return;
    GeneralSelection *selection = g_new0(GeneralSelection, 1);
    selection->records = general_available_records(weekly, job->records);
    selection->now = g_get_real_time();
    selection->floor = weekly->general_after_utc_us ? weekly->general_after_utc_us : selection->now - G_TIME_SPAN_DAY;
    weekly->general_selection_cancel = g_cancellable_new();
    g_signal_connect_object(owner, "destroy", G_CALLBACK(g_cancellable_cancel), weekly->general_selection_cancel, G_CONNECT_SWAPPED);
    GTask *task = g_task_new(NULL, weekly->general_selection_cancel, general_selection_ready, weekly_job_ref(job));
    g_task_set_task_data(task, selection, general_selection_free);
    g_task_run_in_thread(task, general_selection_work); g_object_unref(task);
}
static void general_calendar_records(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; WeeklyJob *job = data;
    if (!weekly_job_current(job, owner) || !general_records_append(job, result, error)) return;
    JsonNode *body = weekly_body(job->weekly); json_object_set_string_member(json_node_get_object(body), "kind", "weekly_programme");
    if (!weekly_request(job, owner, "/v1/workflows/desktop/list", body, general_weekly_records))
        weekly_fail(job->weekly, NULL, "Kayıtlı haftalık programlar okunamadı.");
}
static gboolean general_discover(RadioSPlayerWeekly *weekly, GtkWidget *owner)
{
    if (weekly->pending || weekly->owns_run || !owner || gtk_widget_in_destruction(owner) || !radios_data_connected(weekly->ctx)) return FALSE;
    RadioSDataClient *client = radios_data_client_acquire(weekly->ctx); if (!client) return FALSE;
    if (weekly->client != client) {
        gboolean same_scope = radios_data_client_same_identity(weekly->client, client);
        if (!same_scope) {
            weekly->general_after_utc_us = weekly->general_floor_utc_us = weekly->general_end_utc_us = weekly->completed_end_utc_us = 0;
            if (weekly->unavailable_events) g_hash_table_remove_all(weekly->unavailable_events);
            g_clear_pointer(&weekly->general_following, radios_weekly_programme_selection_free);
            weekly->general_following_floor = 0;
        }
        if (weekly->client) radios_data_client_unref(weekly->client);
        weekly->client = radios_data_client_ref(client);
        g_clear_pointer(&weekly->record, json_node_free); g_clear_pointer(&weekly->schedule, json_node_free); general_projection_clear(weekly);
    }
    radios_data_client_unref(client); ++weekly->serial;
    if (weekly->general_selection_cancel) g_cancellable_cancel(weekly->general_selection_cancel);
    g_clear_object(&weekly->general_selection_cancel);
    WeeklyJob *job = g_new0(WeeklyJob, 1); job->references = 1; job->weekly = weekly; ++weekly->references;
    job->serial = weekly->serial; job->intent = radios_playout_ui_intent(weekly->ctx); job->preview = TRUE;
    job->records = json_array_new(); for (guint i = 0; i < 32; ++i) g_weak_ref_init(&job->cards[i], NULL);
    weekly->pending = TRUE; weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_PREPARING;
    weekly->discover_at = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    JsonNode *body = weekly_body(weekly); json_object_set_string_member(json_node_get_object(body), "kind", "automation_calendar");
    gboolean accepted = weekly_request(job, owner, "/v1/workflows/desktop/list", body, general_calendar_records);
    if (!accepted) weekly_fail(weekly, NULL, "Genel yayın takvimi isteği gönderilemedi.");
    weekly_job_free(job); return accepted;
}
static gboolean general_tick(RadioSPlayerWeekly *weekly)
{
    GtkWidget *owner = g_weak_ref_get(&weekly->owner);
    if (!owner || gtk_widget_in_destruction(owner)) { radios_player_weekly_stop(weekly); g_clear_object(&owner); return G_SOURCE_CONTINUE; }
    if (!weekly->snapshot.armed) {
        if (!weekly->pending && g_get_monotonic_time() >= weekly->discover_at) general_discover(weekly, owner);
        if (weekly->record) {
            RadioSWeeklyProgrammeOccurrence *future = weekly_next(weekly, MAX(g_get_real_time(), weekly->general_floor_utc_us), NULL);
            weekly_display(weekly, future, g_get_real_time()); radios_weekly_programme_occurrence_free(future);
        }
        g_object_unref(owner); return G_SOURCE_CONTINUE;
    }
    gboolean connected = radios_data_connected(weekly->ctx);
    if (!connected && weekly->pending) {
        if (weekly->schema_replace_id) weekly_fail(weekly, NULL, "Veri bağlantısı kesildi; mevcut şema korundu.");
        ++weekly->serial; weekly->pending = FALSE;
    }
    if (connected) {
        RadioSDataClient *current = radios_data_client_acquire(weekly->ctx);
        if (current && current != weekly->client) {
            if (weekly->schema_replace_id) weekly_fail(weekly, NULL, "Veri oturumu değişti; mevcut şema korundu.");
            if (!radios_data_client_same_identity(current, weekly->client)) {
                weekly->owns_run = FALSE; weekly_fail(weekly, NULL, "Genel program veri kimliği değişti; mevcut ses korundu.");
                radios_data_client_unref(current); g_object_unref(owner); return G_SOURCE_CONTINUE;
            }
            ++weekly->serial; weekly->pending = FALSE;
            radios_data_client_unref(weekly->client); weekly->client = radios_data_client_ref(current);
            weekly->discover_at = 0; weekly->retry_utc_us = 0;
        }
        if (current) radios_data_client_unref(current);
    }
    if (radios_playout_ui_intent(weekly->ctx) != weekly->run_intent) {
        weekly->owns_run = FALSE; weekly_fail(weekly, NULL, "Başka bir yayın komutu genel programı devraldı; yeni yayın korundu.");
        g_object_unref(owner); return G_SOURCE_CONTINUE;
    }
    gint64 now = g_get_real_time();
    if (weekly->owns_run) {
        RadioSPlayoutSnapshot run = {0}; radios_playout_ui_get_snapshot(weekly->ctx, &run);
        weekly->snapshot.paused = run.state == RADIOS_PLAYOUT_PAUSED;
        weekly->snapshot.remaining_us = MAX((gint64)0, weekly->snapshot.end_utc_us - now);
        if (now >= weekly->snapshot.end_utc_us) {
            weekly->general_after_utc_us = weekly->snapshot.end_utc_us;
            weekly->completed_end_utc_us = MAX(weekly->completed_end_utc_us, weekly->snapshot.end_utc_us);
            general_slot_ended(weekly);
            weekly_release_run(weekly); g_clear_pointer(&weekly->record, json_node_free); g_clear_pointer(&weekly->schedule, json_node_free);
            weekly->discover_at = 0;
        } else if (run.state == RADIOS_PLAYOUT_FINISHED) weekly_release_run(weekly);
        else if (run.state == RADIOS_PLAYOUT_ERROR || run.state == RADIOS_PLAYOUT_IDLE) {
            weekly_release_run(weekly); weekly_fail(weekly, NULL, run.error[0] ? run.error : "Genel program oynatımı kesildi.");
            g_object_unref(owner); return G_SOURCE_CONTINUE;
        } else { g_object_unref(owner); return G_SOURCE_CONTINUE; }
    }
    if (!connected) {
        weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_WAITING;
        g_strlcpy(weekly->snapshot.error, "Veri bağlantısı bekleniyor; otomasyon açık.", sizeof weekly->snapshot.error);
        g_object_unref(owner); return G_SOURCE_CONTINUE;
    }
    if (weekly->pending || weekly->snapshot.paused) { g_object_unref(owner); return G_SOURCE_CONTINUE; }
    if (weekly->record && now >= weekly->general_end_utc_us) {
        weekly->general_after_utc_us = weekly->general_end_utc_us;
        weekly->completed_end_utc_us = MAX(weekly->completed_end_utc_us, weekly->general_end_utc_us);
        general_slot_ended(weekly);
        g_clear_pointer(&weekly->record, json_node_free); g_clear_pointer(&weekly->schedule, json_node_free); weekly->discover_at = 0;
    }
    if (!weekly->record) {
        if (g_get_monotonic_time() >= weekly->discover_at) general_discover(weekly, owner);
        g_object_unref(owner); return G_SOURCE_CONTINUE;
    }
    RadioSWeeklyProgrammeOccurrence *occurrence = weekly_next(weekly, MAX(now, weekly->general_floor_utc_us), NULL);
    weekly_display(weekly, occurrence, now);
    if (!occurrence || (now >= weekly->general_floor_utc_us && occurrence->start_utc_us > weekly->general_floor_utc_us)) {
        /* The chosen slot ended without owning PCM (for example all source
         * repeat intervals were blocked). Still advance every global source. */
        weekly->general_after_utc_us = MAX(weekly->general_after_utc_us, weekly->general_end_utc_us);
        general_slot_ended(weekly);
        g_clear_pointer(&weekly->record, json_node_free); g_clear_pointer(&weekly->schedule, json_node_free); weekly->discover_at = 0;
        radios_weekly_programme_occurrence_free(occurrence); general_discover(weekly, owner);
    } else if (now >= occurrence->start_utc_us && now >= weekly->retry_utc_us) {
        weekly_prepare(weekly, owner); radios_weekly_programme_occurrence_free(occurrence);
    } else {
        weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_WAITING;
        radios_weekly_programme_occurrence_free(occurrence);
        if (g_get_monotonic_time() >= weekly->discover_at && now < weekly->general_floor_utc_us) general_discover(weekly, owner);
    }
    g_object_unref(owner); return G_SOURCE_CONTINUE;
}
RadioSPlayerWeekly *radios_player_weekly_new_general(RadioSViewContext *ctx, GtkWidget *owner)
{
    if (!ctx || !ctx->playout || !owner) return NULL;
    RadioSPlayerWeekly *weekly = g_new0(RadioSPlayerWeekly, 1); weekly->references = 1;
    weekly->ctx = ctx; weekly->source = weekly->target = G_MAXUINT; weekly->general = TRUE;
    g_weak_ref_init(&weekly->owner, owner); weekly->timer = g_timeout_add(250, weekly_tick, weekly); return weekly;
}
void radios_player_weekly_general_set_queue(RadioSPlayerWeekly *weekly, RadioSGeneralProgrammeQueue callback, gpointer user)
{
    if (!weekly || !weekly->general || weekly->closed) return;
    weekly->queue = callback; weekly->queue_user = user;
    if (callback && weekly->preview_config) callback(json_node_get_object(weekly->preview_config), weekly->owns_run, user);
}
void radios_player_weekly_general_refresh(RadioSPlayerWeekly *weekly)
{
    if (weekly && weekly->general && !weekly->closed && !weekly->owns_run) {
        weekly->discover_at = 0;
        if (weekly->unavailable_events) g_hash_table_remove_all(weekly->unavailable_events);
        if (!weekly->record) weekly->general_after_utc_us = weekly->completed_end_utc_us;
    }
}
gboolean radios_player_weekly_start_general(RadioSPlayerWeekly *weekly, GError **error)
{
    GtkWidget *owner = weekly ? g_weak_ref_get(&weekly->owner) : NULL;
    RadioSPlayoutSnapshot run = {0}; if (weekly) radios_playout_ui_get_snapshot(weekly->ctx, &run);
    if (!weekly || !weekly->general || weekly->closed || !owner || gtk_widget_in_destruction(owner) ||
        !radios_data_connected(weekly->ctx) || weekly->snapshot.armed ||
        (run.state != RADIOS_PLAYOUT_IDLE && run.state != RADIOS_PLAYOUT_FINISHED && run.state != RADIOS_PLAYOUT_ERROR)) {
        g_clear_object(&owner); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Genel program hazır değil veya yayın sırası kullanımda."); return FALSE;
    }
    ++weekly->serial; weekly->pending = FALSE; weekly->snapshot.armed = TRUE; weekly->snapshot.paused = FALSE;
    weekly->snapshot.state = RADIOS_PLAYER_WEEKLY_WAITING; weekly->snapshot.error[0] = 0;
    weekly->general_after_utc_us = 0; weekly->general_floor_utc_us = 0; weekly->retry_utc_us = 0; weekly->completed_end_utc_us = 0;
    if (weekly->unavailable_events) g_hash_table_remove_all(weekly->unavailable_events);
    weekly->run_intent = radios_playout_ui_intent(weekly->ctx);
    g_clear_pointer(&weekly->record, json_node_free); g_clear_pointer(&weekly->schedule, json_node_free);
    general_discover(weekly, owner); g_object_unref(owner); return TRUE;
}
gboolean radios_player_weekly_general_action(RadioSPlayerWeekly *weekly, guint action, GError **error)
{
    if (!weekly || !weekly->general || weekly->closed) return FALSE;
    if (action == RADIOS_PLAYOUT_STOP) { radios_player_weekly_stop(weekly); return TRUE; }
    if (action == RADIOS_PLAYOUT_RESUME && !weekly->snapshot.armed) return radios_player_weekly_start_general(weekly, error);
    if (!weekly->snapshot.armed) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Genel program başlatılmadı."); return FALSE; }
    if (!weekly->owns_run && (action == RADIOS_PLAYOUT_PAUSE || action == RADIOS_PLAYOUT_RESUME)) {
        weekly->snapshot.paused = action == RADIOS_PLAYOUT_PAUSE; return TRUE;
    }
    return weekly->owns_run && radios_playout_ui_action(weekly->ctx, (RadioSPlayoutAction)action, error);
}
gboolean radios_player_weekly_general_apply_schema(RadioSPlayerWeekly *weekly, const gchar *schema_id, GError **error)
{
    if (!weekly || !weekly->general || weekly->closed) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "Genel program sürücüsü yok."); return FALSE;
    }
    if (!g_uuid_string_is_valid(schema_id)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Uygulanacak rotasyon şemasını seçin."); return FALSE;
    }
    if (weekly->pending || weekly->schema_replace_id) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Program hazırlığı sürüyor; tamamlanınca yeniden deneyin."); return FALSE;
    }
    if (weekly->owns_run && weekly->run_intent != radios_playout_ui_intent(weekly->ctx)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Yayının sahibi değişti; mevcut yayın korundu."); return FALSE;
    }
    JsonObject *calendar = schedule_calendar(weekly), *event = weekly->snapshot.armed && weekly->record ? schedule_event(weekly) : NULL;
    if (!calendar || !event) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Çalan genel program yok."); return FALSE; }
    JsonObject *config = json_node_get_object(weekly->schedule); JsonNode *backup = copy_object(config);
    if (!schema_apply_event(calendar, event, schema_id, error)) { json_node_free(backup); return FALSE; }
    GError *failure = NULL; RadioSWeeklyProgrammeOccurrence *check = radios_weekly_programme_next(config, g_get_real_time(), &failure);
    radios_weekly_programme_occurrence_free(check);
    if (failure) {
        json_node_free(weekly->schedule); weekly->schedule = backup; g_propagate_error(error, failure); return FALSE;
    }
    if (weekly->owns_run) {
        GtkWidget *owner = g_weak_ref_get(&weekly->owner);
        if (!owner || gtk_widget_in_destruction(owner)) {
            g_clear_object(&owner); json_node_free(weekly->schedule); weekly->schedule = backup;
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Yayın görünümü kapandı."); return FALSE;
        }
        weekly->schema_replace_backup = backup; weekly->schema_replace_id = g_strdup(schema_id); ++weekly->serial;
        gboolean accepted = weekly_prepare(weekly, owner); g_object_unref(owner);
        if (!accepted) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Şema hazırlığı başlatılamadı; mevcut yayın korundu.");
        return accepted;
    }
    json_node_free(backup);
    JsonArray *library = array(calendar, "rotation_library"); const gchar *name = "";
    for (guint i = 0; library && i < json_array_get_length(library); ++i)
        if (!g_strcmp0(text(at(library, i), "id"), schema_id)) { name = text(at(library, i), "name"); break; }
    g_free(weekly->general_schema_id); weekly->general_schema_id = g_strdup(schema_id);
    g_free(weekly->general_schema_event_id); weekly->general_schema_event_id = g_strdup(weekly->general_event_id);
    g_strlcpy(weekly->snapshot.schema_name, name, sizeof weekly->snapshot.schema_name);
    general_entry_reselect(weekly);
    general_entry_text(weekly, radios_weekly_programme_entry_order(calendar, event, schema_id));
    /* A slot without its own PCM picks up the schema on its next prepare. */
    if (!weekly->owns_run && !weekly->pending) weekly->retry_utc_us = 0;
    return TRUE;
}

/* The saved-programme picker reads identities and the complete snapshot only.
 * Real media preparation is deferred to a due event, after the operator starts. */
static WeeklyPicker *weekly_picker(GtkWidget *owner)
{ return g_object_get_data(G_OBJECT(owner), "radios-player-weekly-picker"); }
static void weekly_pick_job_free(gpointer value)
{
    WeeklyPickJob *job = value; radios_data_client_unref(job->client); g_clear_pointer(&job->record, json_node_free); g_free(job);
}
static void weekly_picker_free(gpointer value)
{
    WeeklyPicker *picker = value; radios_data_client_unref(picker->client); g_clear_pointer(&picker->records, json_node_free); g_free(picker);
}
static void weekly_picker_selected(GtkTreeSelection *selection, gpointer value)
{
    WeeklyPicker *picker = value; if (picker->updating || picker->accepting) return;
    gtk_list_store_clear(store(picker->preview)); GtkTreeModel *model = NULL; GtkTreeIter iter;
    gboolean selected = gtk_tree_selection_get_selected(selection, &model, &iter);
    gtk_widget_set_sensitive(picker->load, selected); if (!selected) return;
    guint index = 0; gtk_tree_model_get(model, &iter, 2, &index, -1);
    JsonObject *record = at(array(picker->records ? json_node_get_object(picker->records) : NULL, "records"), index);
    JsonObject *config = object(record, "config"); JsonArray *events = array(object(config, "calendar"), "events");
    for (guint i = 0; events && i < json_array_get_length(events); ++i) {
        JsonObject *event = at(events, i); GtkTreeIter row;
        gchar *time = g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT "–%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
            radios_json_int(event, "start_minute", 0) / 60, radios_json_int(event, "start_minute", 0) % 60,
            radios_json_int(event, "end_minute", 0) / 60, radios_json_int(event, "end_minute", 0) % 60);
        gtk_list_store_append(store(picker->preview), &row);
        gtk_list_store_set(store(picker->preview), &row, 0, text(event, "date"), 1, time, 2, text(event, "title"), -1); g_free(time);
    }
    gchar *message = g_strdup_printf("%" G_GINT64_FORMAT " haftalık program · %u blok. Yükleme ses başlatmaz; zamanlar UTC'dir.",
        radios_json_int(config, "week_count", 0), events ? json_array_get_length(events) : 0);
    gtk_label_set_text(GTK_LABEL(picker->status), message); g_free(message);
}
static void weekly_picker_list_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; WeeklyPickJob *job = value; WeeklyPicker *picker = weekly_picker(owner);
    if (!picker || picker->generation != job->generation) return;
    if (!same_client(picker->ctx, job->client) || error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        gtk_label_set_text(GTK_LABEL(picker->status), error ? error->message : "Programlar okunamadı veya oturum değişti."); return;
    }
    g_clear_pointer(&picker->records, json_node_free); picker->records = copy_object(json_node_get_object(result));
    JsonArray *records = array(json_node_get_object(result), "records"); picker->updating = TRUE;
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = at(records, i), *config = object(record, "config");
        if (!g_str_equal(text(record, "kind"), "weekly_programme") || !g_str_equal(text(record, "status"), "active") ||
            !g_str_equal(text(record, "station_id"), radios_data_client_station_id(job->client)) ||
            !g_uuid_string_is_valid(text(record, "_id")) || radios_json_int(record, "_rev", 0) < 1 || !config) continue;
        GError *invalid = NULL; RadioSWeeklyProgrammeOccurrence *check = radios_weekly_programme_next(config, g_get_real_time(), &invalid);
        radios_weekly_programme_occurrence_free(check); if (invalid) { g_error_free(invalid); continue; }
        GtkTreeIter row; gtk_list_store_append(store(picker->list), &row);
        gtk_list_store_set(store(picker->list), &row, 0, text(record, "name"), 1, radios_json_int(config, "week_count", 0), 2, i, -1);
    }
    picker->updating = FALSE; gtk_label_set_text(GTK_LABEL(picker->status), "Kaydedilmiş programı seçin.");
}
static void weekly_picker_reload(GtkButton *button, gpointer value)
{
    (void)button; WeeklyPicker *picker = value; ++picker->generation; picker->accepting = FALSE;
    gtk_list_store_clear(store(picker->list)); gtk_list_store_clear(store(picker->preview)); gtk_widget_set_sensitive(picker->load, FALSE);
    if (!same_client(picker->ctx, picker->client)) { gtk_label_set_text(GTK_LABEL(picker->status), "Veri oturumu değişti; pencereyi yeniden açın."); return; }
    WeeklyPickJob *job = g_new0(WeeklyPickJob, 1); job->client = radios_data_client_ref(picker->client); job->generation = picker->generation;
    JsonNode *body = radios_data_body(picker->ctx); json_object_set_string_member(json_node_get_object(body), "kind", "weekly_programme");
    gtk_label_set_text(GTK_LABEL(picker->status), "Kayıtlı programlar okunuyor…");
    radios_data_request(picker->ctx, picker->dialog, "POST", "/v1/workflows/desktop/list", body, weekly_picker_list_received, job, weekly_pick_job_free);
    json_node_free(body);
}
static void weekly_picker_verified(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; WeeklyPickJob *job = value; WeeklyPicker *picker = weekly_picker(owner);
    if (!picker || picker->generation != job->generation) return;
    picker->accepting = FALSE;
    JsonObject *record = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    JsonObject *original = job->record ? json_node_get_object(job->record) : NULL;
    if (!same_client(picker->ctx, job->client) || error || !record || !original ||
        !g_str_equal(text(record, "_id"), text(original, "_id")) || radios_json_int(record, "_rev", 0) != radios_json_int(original, "_rev", 0) ||
        !g_str_equal(text(record, "station_id"), radios_data_client_station_id(job->client)) ||
        !g_str_equal(text(record, "kind"), "weekly_programme") || !g_str_equal(text(record, "status"), "active")) {
        gtk_label_set_text(GTK_LABEL(picker->status), error ? error->message : "Program değişti; listeyi yeniden yükleyin."); return;
    }
    g_object_ref(owner); if (picker->done) picker->done(record, picker->client, picker->user);
    gtk_widget_destroy(owner); g_object_unref(owner);
}
static void weekly_picker_response(GtkDialog *dialog, gint response, gpointer value)
{
    WeeklyPicker *picker = value; if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    if (picker->accepting) return;
    GtkTreeModel *model = NULL; GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(picker->list)), &model, &iter)) return;
    guint index = 0; gtk_tree_model_get(model, &iter, 2, &index, -1);
    JsonObject *record = at(array(picker->records ? json_node_get_object(picker->records) : NULL, "records"), index); if (!record) return;
    WeeklyPickJob *job = g_new0(WeeklyPickJob, 1); job->client = radios_data_client_ref(picker->client); job->generation = picker->generation; job->record = copy_object(record);
    JsonNode *body = radios_data_body(picker->ctx); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "kind", "weekly_programme"); json_object_set_string_member(o, "document_id", text(record, "_id"));
    picker->accepting = TRUE; gtk_widget_set_sensitive(picker->load, FALSE);
    radios_data_request(picker->ctx, picker->dialog, "POST", "/v1/workflows/desktop/get", body, weekly_picker_verified, job, weekly_pick_job_free); json_node_free(body);
}
GtkWidget *radios_player_weekly_open(RadioSViewContext *ctx, GtkWidget *owner, RadioSPlayerWeeklyDone done, gpointer user)
{
    if (!ctx || !owner || !radios_data_connected(ctx) || gtk_widget_in_destruction(owner)) return NULL;
    WeeklyPicker *picker = g_new0(WeeklyPicker, 1); picker->ctx = ctx; picker->client = radios_data_client_acquire(ctx); picker->done = done; picker->user = user;
    picker->dialog = gtk_dialog_new_with_buttons("Program yükle", ctx->parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Yükle", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(picker->dialog, "player-weekly-dialog"); gtk_window_set_default_size(GTK_WINDOW(picker->dialog), 960, 540);
    gtk_style_context_add_class(gtk_widget_get_style_context(picker->dialog), "radios"); gtk_style_context_add_class(gtk_widget_get_style_context(picker->dialog), "task-dialog");
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(picker->dialog)); gtk_container_set_border_width(GTK_CONTAINER(body), 14); gtk_box_set_spacing(GTK_BOX(body), 10);
    const gchar *titles[] = {"Program", "Hafta"}, *preview[] = {"Tarih · UTC", "Saat", "Blok"};
    const GType types[] = {G_TYPE_STRING, G_TYPE_INT64, G_TYPE_UINT}, preview_types[] = {G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING};
    picker->list = tree_new("player-weekly-list", titles, 2, types, 3); picker->preview = tree_new("player-weekly-preview", preview, 3, preview_types, 3);
    GtkWidget *rows = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL); gtk_paned_pack1(GTK_PANED(rows), scroll_new(picker->list), FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(rows), scroll_new(picker->preview), TRUE, FALSE); gtk_paned_set_position(GTK_PANED(rows), 260);
    gtk_box_pack_start(GTK_BOX(body), rows, TRUE, TRUE, 0);
    GtkWidget *reload = gtk_button_new_with_label("Programları yeniden yükle"); gtk_widget_set_name(reload, "player-weekly-reload"); gtk_widget_set_halign(reload, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(body), reload, FALSE, FALSE, 0);
    picker->status = radios_ui_label("", "application-status"); gtk_widget_set_name(picker->status, "player-weekly-status"); gtk_label_set_line_wrap(GTK_LABEL(picker->status), TRUE);
    gtk_box_pack_start(GTK_BOX(body), picker->status, FALSE, FALSE, 0);
    picker->load = gtk_dialog_get_widget_for_response(GTK_DIALOG(picker->dialog), GTK_RESPONSE_ACCEPT); gtk_widget_set_name(picker->load, "player-weekly-load"); gtk_widget_set_sensitive(picker->load, FALSE);
    g_object_set_data_full(G_OBJECT(picker->dialog), "radios-player-weekly-picker", picker, weekly_picker_free);
    g_signal_connect(picker->dialog, "response", G_CALLBACK(weekly_picker_response), picker); g_signal_connect(reload, "clicked", G_CALLBACK(weekly_picker_reload), picker);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(picker->list)), "changed", G_CALLBACK(weekly_picker_selected), picker);
    g_signal_connect_object(owner, "destroy", G_CALLBACK(gtk_widget_destroy), picker->dialog, G_CONNECT_SWAPPED);
    gtk_widget_show_all(picker->dialog); weekly_picker_reload(NULL, picker); return picker->dialog;
}
