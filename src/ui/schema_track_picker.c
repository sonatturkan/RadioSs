#include "radios/schema_track_picker.h"
#include "radios/audio_ui.h"
#include "radios/automation_calendar_plan.h"
#include "radios/automation_scope.h"
#include "radios/data.h"
#include "radios/media_geometry.h"
#include "radios/player_transport.h"
#include "radios/player_workspace.h"
#include "radios/playout_ui.h"
#include "radios/programme_model.h"
#include "radios/weekly_programme_plan.h"
#include "radios/widgets.h"
#include "radios/platform.h"
#include <string.h>

/* Kullanıcı isteği U2: "Şemadan yükle…" of one player card. Nothing is
 * written: schema list, prepare(automation) and the local calendar selection
 * only; the chosen song loads like "Kütüphaneden yükle". */

enum { COL_NUMBER, COL_TITLE, COL_DURATION, COL_KIND, COL_START, COL_INDEX, COL_KIND_ID, COL_SEARCH, COLUMNS };

typedef struct {
    RadioSViewContext *ctx;
    RadioSDataClient *client;
    GWeakRef owner;
    guint deck, generation;
    GtkWidget *dialog, *schema, *search, *kind, *tree, *status, *load;
    GtkListStore *store;
    GtkTreeModel *filter;
    JsonNode *records, *rows;
    gchar assigned[37];
    gboolean filling;
} Picker;
typedef struct { guint generation; JsonNode *event; gchar *title; gint64 start_utc_us, end_utc_us; gboolean single; guint source; gchar *seed; } Job;

static JsonObject *member_object(JsonObject *object, const gchar *key)
{ JsonNode *node = object ? json_object_get_member(object, key) : NULL; return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL; }
static JsonArray *member_array(JsonObject *object, const gchar *key)
{ JsonNode *node = object ? json_object_get_member(object, key) : NULL; return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL; }
static gboolean member_true(JsonObject *object, const gchar *key)
{ JsonNode *node = object ? json_object_get_member(object, key) : NULL; return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node); }

JsonArray *radios_schema_track_picker_rows(JsonObject *selection, gint64 start_utc_us, gboolean single_player)
{
    JsonArray *rows = json_array_new(), *tracks = member_array(selection, "tracks");
    gint64 start = start_utc_us;
    for (guint i = 0; tracks && i < json_array_get_length(tracks); ++i) {
        JsonNode *node = json_array_get_element(tracks, i);
        JsonObject *track = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        JsonObject *detail = member_object(track, "detail"), *metadata = member_object(track, "metadata");
        JsonObject *media = member_object(detail, "media"), *version = member_object(detail, "version");
        if (!track || !media || !version) continue;
        /* The selection's own handover geometry (automation_calendar_plan.c). */
        RadioSMediaGeometry geometry = radios_media_geometry(detail, radios_json_int(version, "duration_ms", 0), TRUE);
        guint64 in = single_player ? geometry.cue_in : geometry.autodj_in;
        guint64 out = radios_media_geometry_sound_out(detail, &geometry, in, !single_player && geometry.has_autodj_out ? geometry.autodj_out : 0);
        guint64 handover = !single_player && geometry.has_autodj_out && geometry.autodj_out > in ? geometry.autodj_out - in : out > in ? out - in : 0;
        if (member_true(track, "calendar_window_wait"))
            start = MAX(start, radios_json_int(member_object(track, "calendar_constraints"), "earliest_utc_us", start));
        JsonObject *row = json_object_new();
        json_object_set_int_member(row, "number", json_array_get_length(rows) + 1);
        const gchar *keys[] = {"title", "artist"};
        for (guint k = 0; k < G_N_ELEMENTS(keys); ++k)
            json_object_set_string_member(row, keys[k], radios_json_string(metadata, keys[k], radios_json_string(media, keys[k], "")));
        json_object_set_string_member(row, "kind", radios_json_string(metadata, "kind", radios_json_string(media, "kind", "music")));
        json_object_set_int_member(row, "duration_ms", (gint64)((out > in ? out - in : 0) / 48));
        json_object_set_int_member(row, "start_utc_us", start);
        json_object_set_string_member(row, "media_id", radios_json_string(media, "_id", ""));
        json_object_set_string_member(row, "version_id", radios_json_string(version, "_id", ""));
        json_object_set_string_member(row, "entry_id", radios_json_string(track, "entry_id", ""));
        json_array_add_object_element(rows, row);
        start += (gint64)(handover * G_USEC_PER_SEC / 48000);
    }
    return rows;
}

static void job_free(gpointer value)
{
    Job *job = value; if (!job) return;
    if (job->event) json_node_free(job->event);
    g_free(job->title); g_free(job->seed); g_free(job);
}
static Picker *picker_get(GtkWidget *dialog)
{ return dialog ? g_object_get_data(G_OBJECT(dialog), "radios-schema-track-picker") : NULL; }
static void picker_free(gpointer value)
{
    Picker *picker = value;
    if (picker->client) radios_data_client_unref(picker->client);
    g_clear_pointer(&picker->records, json_node_free); g_clear_pointer(&picker->rows, json_node_free);
    g_weak_ref_clear(&picker->owner); g_free(picker);
}
static void status_set(Picker *picker, const gchar *text) { gtk_label_set_text(GTK_LABEL(picker->status), text ? text : ""); }
static gboolean same_session(Picker *picker)
{
    RadioSDataClient *current = radios_data_client_acquire(picker->ctx);
    gboolean same = current && current == picker->client && radios_data_connected(picker->ctx);
    if (current) radios_data_client_unref(current);
    return same;
}
static gchar *clock_text(gint64 utc_us, const gchar *zone_name)
{
    GTimeZone *zone = radios_time_zone_new(zone_name && *zone_name ? zone_name : "UTC");
    if (!zone) zone = g_time_zone_new_utc();
    GDateTime *utc = g_date_time_new_from_unix_utc(utc_us / G_USEC_PER_SEC), *local = utc ? g_date_time_to_timezone(utc, zone) : NULL;
    gchar *text = local ? g_date_time_format(local, "%H:%M:%S") : g_strdup("—");
    if (local) g_date_time_unref(local);
    if (utc) g_date_time_unref(utc);
    g_time_zone_unref(zone); return text;
}
static void actions_update(Picker *picker)
{
    GtkTreeModel *model = NULL; GtkTreeIter iter;
    gboolean chosen = gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(picker->tree)), &model, &iter);
    gtk_widget_set_sensitive(picker->load, chosen && same_session(picker));
}
static void rows_show(Picker *picker)
{
    picker->filling = TRUE; gtk_list_store_clear(picker->store);
    const gchar *zone = picker->client ? radios_data_client_station_timezone(picker->client) : "UTC";
    JsonArray *rows = picker->rows ? json_node_get_array(picker->rows) : NULL;
    GtkComboBoxText *kinds = GTK_COMBO_BOX_TEXT(picker->kind);
    gchar *active = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(picker->kind)));
    gtk_combo_box_text_remove_all(kinds); gtk_combo_box_text_append(kinds, "", "Tüm türler");
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        const gchar *title = radios_json_string(row, "title", ""), *artist = radios_json_string(row, "artist", "");
        const gchar *kind = radios_json_string(row, "kind", "music");
        gchar *number = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(row, "number", i + 1));
        gchar *name = *artist ? g_strdup_printf("%s – %s", artist, *title ? title : "—") : g_strdup(*title ? title : "—");
        gint64 ms = radios_json_int(row, "duration_ms", 0);
        gchar *length = g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, ms / 60000, ms / 1000 % 60);
        gchar *start = clock_text(radios_json_int(row, "start_utc_us", 0), zone);
        gchar *folded = g_utf8_casefold(name, -1);
        GtkTreeIter iter; gtk_list_store_append(picker->store, &iter);
        gtk_list_store_set(picker->store, &iter, COL_NUMBER, number, COL_TITLE, name, COL_DURATION, length,
            COL_KIND, radios_programme_kind_label(kind), COL_START, start, COL_INDEX, (gint)i, COL_KIND_ID, kind, COL_SEARCH, folded, -1);
        if (!g_hash_table_contains(seen, kind)) {
            g_hash_table_add(seen, (gpointer)kind); gtk_combo_box_text_append(kinds, kind, radios_programme_kind_label(kind));
        }
        g_free(number); g_free(name); g_free(length); g_free(start); g_free(folded);
    }
    g_hash_table_unref(seen);
    if (!active || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(picker->kind), active)) gtk_combo_box_set_active_id(GTK_COMBO_BOX(picker->kind), "");
    g_free(active); picker->filling = FALSE;
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(picker->filter)); actions_update(picker);
}
static gboolean row_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    Picker *picker = data; gchar *kind = NULL, *folded = NULL; gboolean visible = TRUE;
    gtk_tree_model_get(model, iter, COL_KIND_ID, &kind, COL_SEARCH, &folded, -1);
    const gchar *wanted = gtk_combo_box_get_active_id(GTK_COMBO_BOX(picker->kind));
    if (wanted && *wanted && g_strcmp0(kind, wanted)) visible = FALSE;
    const gchar *query = gtk_entry_get_text(GTK_ENTRY(picker->search));
    if (visible && query && *query) {
        gchar *needle = g_utf8_casefold(query, -1); visible = folded && strstr(folded, needle) != NULL; g_free(needle);
    }
    g_free(kind); g_free(folded); return visible;
}
static void filter_changed(GtkWidget *widget, gpointer data)
{
    (void)widget; Picker *picker = data; if (picker->filling) return;
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(picker->filter)); actions_update(picker);
}

/* Selection: the same chain the scope runner uses (player_scope_execution.c
 * schema_resolve → automation_received), without installing anything. */
static void prepared_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; Job *job = value; Picker *picker = picker_get(owner);
    if (!picker || job->generation != picker->generation) return;
    if (!same_session(picker)) { status_set(picker, "Veri oturumu değişti; pencereyi yeniden açın."); return; }
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { status_set(picker, error ? error->message : "Şema kaynakları okunamadı."); return; }
    JsonNode *sources = json_node_copy(result);
    RadioSViewContext *run = picker->ctx->players ? radios_player_workspace_source_context(picker->ctx->players, job->source) : NULL;
    JsonNode *turns = run ? radios_playout_ui_calendar_turns(run) : NULL;
    if (turns) json_object_set_member(json_node_get_object(sources), "calendar_turns", turns);
    JsonNode *history = run ? radios_playout_ui_history(run) : NULL;
    if (history) {
        JsonObject *record = json_object_new(); json_object_set_member(record, "config", history);
        json_object_set_object_member(json_node_get_object(sources), "history", record);
    }
    gint64 start = MAX(g_get_real_time(), job->start_utc_us);
    GError *failure = NULL;
    JsonNode *selection = start < job->end_utc_us ? radios_automation_calendar_prepared_programme_at_mode(json_node_get_object(job->event),
        json_node_get_object(sources), radios_data_client_station_id(picker->client), job->seed, 200, start, job->single, &failure) : NULL;
    json_node_free(sources);
    if (!selection) {
        status_set(picker, failure ? failure->message : "Kuşağın süresi doldu."); g_clear_error(&failure);
        g_clear_pointer(&picker->rows, json_node_free); rows_show(picker); return;
    }
    JsonArray *rows = radios_schema_track_picker_rows(json_node_get_object(selection), start, job->single);
    json_node_free(selection);
    g_clear_pointer(&picker->rows, json_node_free); picker->rows = json_node_new(JSON_NODE_ARRAY); json_node_take_array(picker->rows, rows);
    rows_show(picker);
    const gchar *zone = radios_data_client_station_timezone(picker->client);
    gchar *from = clock_text(job->start_utc_us, zone), *to = clock_text(job->end_utc_us, zone);
    gchar *text = g_strdup_printf("%s · %.5s–%.5s · %u parça", job->title, from, to, json_array_get_length(rows));
    status_set(picker, text); g_free(text); g_free(from); g_free(to);
}
static JsonObject *record_find(Picker *picker, const gchar *id)
{
    JsonArray *records = picker->records ? member_array(json_node_get_object(picker->records), "records") : NULL;
    for (guint i = 0; id && records && i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i);
        if (g_str_equal(radios_json_string(record, "_id", ""), id)) return record;
    }
    return NULL;
}
static void selection_request(Picker *picker)
{
    ++picker->generation; g_clear_pointer(&picker->rows, json_node_free); rows_show(picker);
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(picker->schema));
    JsonObject *record = record_find(picker, id), *config = member_object(record, "config");
    if (!config) { status_set(picker, "Şema seçin."); return; }
    if (!same_session(picker)) { status_set(picker, "Veri oturumu değişti; pencereyi yeniden açın."); return; }
    if (!picker->ctx->players || !radios_player_workspace_ready(picker->ctx->players)) { status_set(picker, "Oynatıcı tanımları henüz hazır değil."); return; }
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(picker->ctx->players);
    const RadioSPlayerDefinition *player = radios_player_topology_player_const(topology, picker->deck);
    if (!player) { status_set(picker, "Oynatıcı artık tanımlı değil."); return; }
    /* A clustered player draws from its cluster's flow; others from their own. */
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = picker->deck};
    if (*player->cluster_id) { scope.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER; g_strlcpy(scope.cluster_id, player->cluster_id, sizeof scope.cluster_id); }
    RadioSAutomationBinding binding; GError *error = NULL;
    if (!radios_automation_scope_resolve(topology, &scope, &binding, &error)) { status_set(picker, error->message); g_error_free(error); return; }
    RadioSWeeklyProgrammeOccurrence *occurrence = radios_weekly_programme_next(config, g_get_real_time(), &error);
    if (!occurrence) { status_set(picker, error ? error->message : "Şemada yaklaşan kuşak yok."); g_clear_error(&error); return; }
    JsonNode *event = radios_automation_scope_route_event(&binding, json_node_get_object(occurrence->event), &error);
    JsonNode *prepare = event ? radios_automation_calendar_prepare_config(json_node_get_object(event), NULL, &error) : NULL;
    if (!prepare) {
        status_set(picker, error ? error->message : "Şema kuşağı hazırlanamadı."); g_clear_error(&error);
        if (event) json_node_free(event);
        radios_weekly_programme_occurrence_free(occurrence); return;
    }
    Job *job = g_new0(Job, 1); job->generation = picker->generation; job->event = event; job->source = binding.source;
    JsonObject *event_object = json_node_get_object(occurrence->event);
    job->title = g_strdup(radios_json_string(event_object, "title", radios_json_string(event_object, "name", radios_json_string(record, "name", "Şema"))));
    job->start_utc_us = occurrence->start_utc_us; job->end_utc_us = occurrence->end_utc_us;
    RadioSAutoDJMode mode = binding.autodj_mode == RADIOS_AUTODJ_OFF ? RADIOS_AUTODJ_SEMI : binding.autodj_mode;
    job->single = mode != RADIOS_AUTODJ_TRANSITIONS || binding.count < 2;
    job->seed = g_strdup_printf("%s:0", occurrence->occurrence_key ? occurrence->occurrence_key : radios_json_string(record, "_id", ""));
    radios_weekly_programme_occurrence_free(occurrence);
    JsonNode *body = radios_data_body(picker->ctx); json_object_set_string_member(json_node_get_object(body), "kind", "automation");
    json_object_set_member(json_node_get_object(body), "config", prepare);
    status_set(picker, "Şema parçaları hazırlanıyor…");
    if (!radios_data_request(picker->ctx, picker->dialog, "POST", "/v1/workflows/desktop-playout/prepare", body, prepared_received, job, job_free))
        status_set(picker, "Şema kaynakları istenemedi.");
    json_node_free(body);
}
static void schema_changed(GtkComboBox *combo, gpointer data)
{ (void)combo; Picker *picker = data; if (!picker->filling) selection_request(picker); }
static void schemas_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; Picker *picker = picker_get(owner); guint generation = GPOINTER_TO_UINT(value);
    if (!picker || generation != picker->generation) return;
    if (!same_session(picker) || error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        status_set(picker, error ? error->message : "Şemalar okunamadı veya oturum değişti."); return;
    }
    g_clear_pointer(&picker->records, json_node_free); picker->records = json_node_copy(result);
    JsonArray *records = member_array(json_node_get_object(result), "records");
    picker->filling = TRUE; gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(picker->schema));
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i);
        if (!g_str_equal(radios_json_string(record, "kind", ""), "weekly_programme") ||
            !g_str_equal(radios_json_string(record, "status", ""), "active") || !member_object(record, "config") ||
            !g_str_equal(radios_json_string(record, "station_id", ""), radios_data_client_station_id(picker->client)) ||
            !g_uuid_string_is_valid(radios_json_string(record, "_id", ""))) continue;
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(picker->schema), radios_json_string(record, "_id", ""), radios_json_string(record, "name", "Şema"));
    }
    gboolean assigned = *picker->assigned && gtk_combo_box_set_active_id(GTK_COMBO_BOX(picker->schema), picker->assigned);
    picker->filling = FALSE;
    if (assigned) selection_request(picker); else status_set(picker, "Şema seçin.");
}

/* Load: the song is re-read and loaded exactly like the library's
 * "Kütüphaneden yükle" (media.c deck_picked). */
static void media_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    gchar *wanted = value; Picker *picker = picker_get(owner);
    if (!picker) return;
    GtkWidget *card = g_weak_ref_get(&picker->owner);
    if (!card || gtk_widget_in_destruction(card)) { if (card) g_object_unref(card); gtk_widget_destroy(picker->dialog); return; }
    if (!same_session(picker)) { status_set(picker, "Veri oturumu değişti; pencereyi yeniden açın."); g_object_unref(card); return; }
    JsonObject *detail = !error && result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    if (!detail || !g_str_equal(radios_json_string(member_object(detail, "version"), "_id", ""), wanted)) {
        status_set(picker, error ? error->message : "Parçanın sürümü değişmiş; şemayı yeniden seçin."); g_object_unref(card); return;
    }
    GError *failure = NULL;
    if (!radios_audio_ui_load_media(ctx, card, detail, picker->deck, FALSE, &failure)) {
        status_set(picker, failure ? failure->message : "Medya yüklenemedi."); g_clear_error(&failure); g_object_unref(card); return;
    }
    radios_broadcast_player_library_loaded(ctx, picker->deck);
    GtkWidget *main = g_object_get_data(G_OBJECT(card), "radios-deck-main-widget");
    radios_audio_ui_set_deck_main(ctx, picker->deck, main ? gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(main)) : TRUE, NULL);
    GtkWidget *title = g_object_get_data(G_OBJECT(card), "radios-deck-title-widget");
    if (title) gtk_label_set_text(GTK_LABEL(title), radios_json_string(member_object(detail, "media"), "title", "—"));
    g_object_unref(card); gtk_widget_destroy(picker->dialog);
}
static void load_selected(Picker *picker)
{
    GtkTreeModel *model = NULL; GtkTreeIter iter; gint index = -1;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(picker->tree)), &model, &iter)) return;
    gtk_tree_model_get(model, &iter, COL_INDEX, &index, -1);
    JsonArray *rows = picker->rows ? json_node_get_array(picker->rows) : NULL;
    if (!rows || index < 0 || (guint)index >= json_array_get_length(rows)) return;
    JsonObject *row = json_array_get_object_element(rows, (guint)index);
    if (!same_session(picker)) { status_set(picker, "Veri oturumu değişti; pencereyi yeniden açın."); return; }
    JsonNode *body = radios_data_body(picker->ctx);
    json_object_set_string_member(json_node_get_object(body), "media_id", radios_json_string(row, "media_id", ""));
    status_set(picker, "Parça yükleniyor…"); gtk_widget_set_sensitive(picker->load, FALSE);
    if (!radios_data_request(picker->ctx, picker->dialog, "POST", "/v1/workflows/desktop-media/get", body, media_received,
            g_strdup(radios_json_string(row, "version_id", "")), g_free)) { status_set(picker, "Parça istenemedi."); actions_update(picker); }
    json_node_free(body);
}
static void row_activated(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column, gpointer data)
{ (void)tree; (void)path; (void)column; load_selected(data); }
static void selection_changed(GtkTreeSelection *selection, gpointer data) { (void)selection; actions_update(data); }
static void response(GtkDialog *dialog, gint id, gpointer data)
{
    if (id == GTK_RESPONSE_ACCEPT) { load_selected(data); return; }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static GtkWidget *column_view(Picker *picker)
{
    picker->store = gtk_list_store_new(COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    picker->filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(picker->store), NULL);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(picker->filter), row_visible, picker, NULL);
    picker->tree = gtk_tree_view_new_with_model(picker->filter);
    g_object_unref(picker->filter); g_object_unref(picker->store);
    gtk_widget_set_name(picker->tree, "schema-track-picker-list");
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(picker->tree), GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(picker->tree), "ld-table");
    static const gchar *const headers[] = {"No", "Sanatçı – Başlık", "Süre", "Tür", "Başlangıç"};
    for (guint i = 0; i < G_N_ELEMENTS(headers); ++i) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ellipsize", i == COL_TITLE ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(headers[i], renderer, "text", (gint)i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE); gtk_tree_view_column_set_expand(column, i == COL_TITLE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(picker->tree), column);
    }
    g_signal_connect(picker->tree, "row-activated", G_CALLBACK(row_activated), picker);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(picker->tree)), "changed", G_CALLBACK(selection_changed), picker);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE); gtk_container_add(GTK_CONTAINER(scroll), picker->tree);
    return scroll;
}
typedef struct { RadioSViewContext *ctx; GWeakRef owner; guint deck; } Binding;
static void binding_free(gpointer data, GClosure *closure)
{ (void)closure; Binding *binding = data; g_weak_ref_clear(&binding->owner); g_free(binding); }
static void bound_clicked(GtkButton *button, gpointer data)
{
    (void)button; Binding *binding = data; GtkWidget *owner = g_weak_ref_get(&binding->owner);
    if (owner) { radios_schema_track_picker_open(binding->ctx, owner, binding->deck); g_object_unref(owner); }
}
void radios_schema_track_picker_bind(GtkWidget *button, RadioSViewContext *ctx, GtkWidget *owner, guint deck)
{
    if (!GTK_IS_BUTTON(button) || !ctx || !GTK_IS_WIDGET(owner)) return;
    Binding *binding = g_new0(Binding, 1); binding->ctx = ctx; binding->deck = deck; g_weak_ref_init(&binding->owner, owner);
    g_signal_connect_data(button, "clicked", G_CALLBACK(bound_clicked), binding, binding_free, 0);
}
void radios_schema_track_picker_open(RadioSViewContext *ctx, GtkWidget *owner, guint deck)
{
    if (!ctx || !ctx->parent || !GTK_IS_WIDGET(owner) || gtk_widget_in_destruction(owner) || deck >= 32) return;
    if (!radios_data_connected(ctx)) { if (ctx->notify) ctx->notify(ctx->notify_data, "Veri bağlantısı hazır değil."); return; }
    Picker *picker = g_new0(Picker, 1); picker->ctx = ctx; picker->deck = deck; picker->client = radios_data_client_acquire(ctx);
    g_weak_ref_init(&picker->owner, owner);
    if (ctx->players && radios_player_workspace_ready(ctx->players))
        g_strlcpy(picker->assigned, radios_player_topology_automation(radios_player_workspace_topology(ctx->players), deck), sizeof picker->assigned);
    const gchar *name = g_object_get_data(G_OBJECT(owner), "radios-player-name");
    gchar *title = name ? g_strdup_printf("%s · Şemadan yükle", name) : g_strdup_printf("Oynatıcı %u · Şemadan yükle", deck + 1);
    picker->dialog = gtk_dialog_new_with_buttons(title, ctx->parent, GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Yükle", GTK_RESPONSE_ACCEPT, NULL);
    g_free(title);
    gtk_widget_set_name(picker->dialog, "schema-track-picker-dialog");
    gtk_style_context_add_class(gtk_widget_get_style_context(picker->dialog), "radios");
    gtk_style_context_add_class(gtk_widget_get_style_context(picker->dialog), "task-dialog");
    gtk_window_set_default_size(GTK_WINDOW(picker->dialog), 900, 560);
    picker->load = gtk_dialog_get_widget_for_response(GTK_DIALOG(picker->dialog), GTK_RESPONSE_ACCEPT);
    gtk_widget_set_name(picker->load, "schema-track-picker-load");
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(picker->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(body), 14); gtk_box_set_spacing(GTK_BOX(body), 10);
    GtkWidget *bar = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    picker->schema = gtk_combo_box_text_new(); gtk_widget_set_name(picker->schema, "schema-track-picker-schema");
    gtk_widget_set_tooltip_text(picker->schema, "Şema");
    picker->search = gtk_search_entry_new(); gtk_widget_set_name(picker->search, "schema-track-picker-search");
    gtk_entry_set_placeholder_text(GTK_ENTRY(picker->search), "Ara");
    picker->kind = gtk_combo_box_text_new(); gtk_widget_set_name(picker->kind, "schema-track-picker-kind");
    gtk_widget_set_tooltip_text(picker->kind, "Tür");
    gtk_box_pack_start(GTK_BOX(bar), picker->schema, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), picker->search, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bar), picker->kind, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), column_view(picker), TRUE, TRUE, 0);
    picker->status = radios_ui_label("", "application-status"); gtk_widget_set_name(picker->status, "schema-track-picker-status");
    gtk_box_pack_start(GTK_BOX(body), picker->status, FALSE, FALSE, 0);
    g_object_set_data_full(G_OBJECT(picker->dialog), "radios-schema-track-picker", picker, picker_free);
    g_signal_connect(picker->schema, "changed", G_CALLBACK(schema_changed), picker);
    g_signal_connect(picker->search, "search-changed", G_CALLBACK(filter_changed), picker);
    g_signal_connect(picker->kind, "changed", G_CALLBACK(filter_changed), picker);
    g_signal_connect(picker->dialog, "response", G_CALLBACK(response), picker);
    g_signal_connect_object(owner, "destroy", G_CALLBACK(gtk_widget_destroy), picker->dialog, G_CONNECT_SWAPPED);
    rows_show(picker);
    gtk_widget_show_all(picker->dialog);
    status_set(picker, "Şemalar okunuyor…");
    JsonNode *list = radios_data_body(ctx); json_object_set_string_member(json_node_get_object(list), "kind", "weekly_programme");
    if (!radios_data_request(ctx, picker->dialog, "POST", "/v1/workflows/desktop/list", list, schemas_received,
            GUINT_TO_POINTER(picker->generation), NULL)) status_set(picker, "Şemalar istenemedi.");
    json_node_free(list);
}
