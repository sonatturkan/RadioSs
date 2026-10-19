#include "radios/automation_calendar.h"
#include "radios/automation_calendar_plan.h"
#include "radios/rotation_editor.h"
#include "radios/probability_editor.h"
#include "radios/weekly_programme_ui.h"
#include "radios/data.h"
#include "radios/station_time.h"
#include "radios/playout.h"
#include "radios/playout_ui.h"
#include "radios/player_rotation.h"
#include "radios/widgets.h"
#include "radios/audio_ui.h"
#include "radios/platform.h"
#include <math.h>
#include <string.h>

/* The reference has dates in columns and time down the vertical axis. The
 * drawing area shares the same minute coordinates as the persisted blocks. */
typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *page, *grid, *header, *date, *selection, *status, *editor, *name, *color;
    GtkWidget *rows, *save_selection, *copy, *paste, *define, *rotation_dialog;
    GtkWidget *independent, *schema, *library_choice, *library_edit, *library_assign, *library_dialog, *library_rows, *schema_dialog;
    GtkListStore *library_store;
    GtkWidget *apply_schema, *rotation_title, *properties_item;
    JsonNode *mixer, *channel_names; /* dj_mixer / channel document configs for the live panel */
    GtkWidget *live_channels; /* the open programme dialog's channel tree (weak) */
    guint mixer_generation;
    GtkWidget *preview_button, *append_button, *schedule_button;
    GtkListStore *store;
    GtkWidget *detail_title, *detail_subtitle, *empty, *rotation_count, *content_warning;
    GtkWidget *settings_stack, *probability_page;
    GtkWidget *programme_date, *programme_start, *programme_end, *week_range, *save_status;
    GtkWidget *block_menu, *row_menu, *rotation_mode;
    RadioSAutomationCalendarChanged editor_changed;
    gpointer editor_user;
    gboolean detached, bound_editor, saved_main_dirty;
    JsonNode *saved_main;
    gchar *programme_title;
    const gchar *schema_playlist; /* synchronous schema create only */
    guint autosave_source, reorder_source;
    GtkWidget *timezone_label;
    gboolean drag_blocks;
    gint drag_day, drag_minute;
    gdouble press_x, press_y;
    GtkCssProvider *style;
    GdkScreen *style_screen;
    gboolean split_initialized;
    RadioSDocument *document;
    JsonNode *config, *clipboard, *row_clipboard, *playlists;
    JsonNode *prepared, *preview_event;
    gchar *preview_notes;
    RadioSDataClient *preview_client;
    RadioSDataClient *playlists_client, *playlist_request_client;
    gchar *playlists_station, *playlists_endpoint;
    guint playlist_generation;
    RadioSPlayoutPlan *plan;
    guint preview_generation;
    gboolean preview_busy;
    GDateTime *week;
    GHashTable *selected, *source_problems;
    gchar *current, *inspected_id;
    gboolean inspector_invalid, closing;
    guint day_first, day_last, start, end, anchor_day, anchor_minute, step;
    gboolean has_selection, dragging, moved, updating, dirty, disposed, loading;
} Calendar;

typedef struct {
    RadioSDataClient *client;
    gchar *station, *endpoint;
    guint generation;
} PlaylistRequest;

static void load_playlists(RadioSViewContext *ctx, GtkWidget *owner);
static void refresh(Calendar *view);
static void show_programme(Calendar *view);
static void copy_blocks(GtkButton *button, gpointer data);
static void paste_blocks(GtkButton *button, gpointer data);
static void remove_programme(GtkButton *button, gpointer data);
static void row_action(GtkButton *button, gpointer data);
static void block_popup(Calendar *view, GdkEvent *event);
static gboolean rows_press(GtkWidget *widget, GdkEventButton *event, gpointer data);
static void edit_rotation(Calendar *view);
static gboolean move_blocks(Calendar *view, gint day_delta, gint minute_delta);
static void update_detail_heading(Calendar *view);
static void commit(Calendar *view);
static gboolean sync_row_order(Calendar *view, GtkTreeModel *model);
static void programme_dialog(GtkButton *button, gpointer data);
static void rotation_dialog(GtkButton *button, gpointer data);
static void preview_selected(GtkButton *button, gpointer data);
static void preview_append(GtkButton *button, gpointer data);
static void preview_schedule(GtkButton *button, gpointer data);
static void library_open(Calendar *view, const gchar *id);
static void library_reconcile_editor(Calendar *view);
static void library_dialog_response(GtkDialog *dialog, gint response, gpointer data);
static void library_edit(GtkButton *button, gpointer data);
static void library_choices(Calendar *view);
static void library_create(GtkButton *button, gpointer data);
static void schema_settings(GtkButton *button, gpointer data);
static gchar *apply_schema_choice(Calendar *view);
static void library_selection_changed(GtkTreeSelection *selection, gpointer data);
static void event_probability(GtkButton *button, gpointer data);
static JsonObject *library_by_id(Calendar *view, const gchar *id);
static void probability_return(GtkButton *button, gpointer data);
static void load_mixer(Calendar *view);
static JsonArray *library_array(Calendar *view, gboolean create);
static gboolean library_add_assignment(Calendar *view, JsonObject *event, const gchar *id);
static gboolean definition_schema(JsonObject *definition);
static void apply_schema(GtkButton *button, gpointer data);
static void programme_properties(GtkMenuItem *item, gpointer data);

static RadioSDocument *calendar_active_document(Calendar *view)
{
    return view->bound_editor ? g_object_get_data(G_OBJECT(view->page), "radios-weekly-programme-document") : view->document;
}
static JsonNode *object_node(JsonObject *object)
{ JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, object); return node; }
/* JSON-GLib node_copy retains object/array containers. Calendar clipboard
 * and snapshots must own independent descendants before IDs or times change. */
static JsonNode *deep_copy(JsonNode *node)
{
    gchar *text = json_to_string(node, FALSE);
    JsonParser *parser = json_parser_new();
    gboolean loaded = json_parser_load_from_data(parser, text, -1, NULL);
    g_free(text);
    JsonNode *copy = loaded ? json_parser_steal_root(parser) : NULL;
    g_object_unref(parser); return copy;
}
static const gchar *calendar_timezone(Calendar *view)
{ return radios_json_string(json_node_get_object(view->config), "timezone", "UTC"); }
static GDateTime *calendar_now(Calendar *view)
{
    GTimeZone *zone = radios_time_zone_new(calendar_timezone(view));
    if (!zone) zone = g_time_zone_new_utc();
    GDateTime *now = g_date_time_new_now(zone); g_time_zone_unref(zone); return now;
}
static void timezone_render(Calendar *view)
{
    const gchar *zone = calendar_timezone(view);
    if (view->timezone_label) { gchar *label = g_strdup_printf("Tarih · %s", zone); gtk_label_set_text(GTK_LABEL(view->timezone_label), label); g_free(label); }
    if (view->date) { gchar *tip = g_strdup_printf("Görüntülenecek hafta · %s", zone); gtk_widget_set_tooltip_text(view->date, tip); g_free(tip); }
}
static JsonArray *events(Calendar *view)
{ return json_object_get_array_member(json_node_get_object(view->config), "events"); }
static JsonObject *event_by_id(Calendar *view, const gchar *id)
{
    JsonArray *array = events(view);
    for (guint i = 0; id && i < json_array_get_length(array); ++i) {
        JsonObject *event = json_array_get_object_element(array, i);
        if (g_str_equal(radios_json_string(event, "id", ""), id)) return event;
    }
    return NULL;
}
/* Kılavuz §04 adım 1: a programme marked "Live event" (contract §1b). */
static JsonObject *event_live(JsonObject *event)
{
    JsonNode *node = event ? json_object_get_member(event, "live") : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static gchar *date_at(Calendar *view, guint day)
{
    GDateTime *date = g_date_time_add_days(view->week, day);
    if (!date) return g_strdup("");
    gchar *text = g_date_time_format(date, "%Y-%m-%d"); g_date_time_unref(date); return text;
}
static gint event_day(Calendar *view, JsonObject *event)
{
    for (guint day = 0; day < 7; ++day) {
        gchar *date = date_at(view, day);
        gboolean match = g_str_equal(date, radios_json_string(event, "date", ""));
        g_free(date); if (match) return (gint)day;
    }
    return -1;
}
static void notice(Calendar *view, const gchar *message)
{
    gtk_label_set_text(GTK_LABEL(view->status), message);
    if (view->ctx->notify) view->ctx->notify(view->ctx->notify_data, message);
}
static gboolean overlaps(JsonArray *array, const gchar *date, guint start, guint end, const gchar *ignore)
{
    for (guint i = 0; i < json_array_get_length(array); ++i) {
        JsonObject *event = json_array_get_object_element(array, i);
        if (ignore && g_str_equal(ignore, radios_json_string(event, "id", ""))) continue;
        if (g_str_equal(date, radios_json_string(event, "date", "")) &&
            start < (guint)radios_json_int(event, "end_minute", 0) &&
            end > (guint)radios_json_int(event, "start_minute", 0)) return TRUE;
    }
    return FALSE;
}
static void free_view(gpointer data)
{
    Calendar *view = data;
    g_clear_pointer(&view->config, json_node_free);
    g_clear_pointer(&view->saved_main, json_node_free); g_free(view->programme_title);
    g_clear_pointer(&view->clipboard, json_node_free);
    g_clear_pointer(&view->row_clipboard, json_node_free);
    g_clear_pointer(&view->playlists, json_node_free);
    g_clear_pointer(&view->mixer, json_node_free); g_clear_pointer(&view->channel_names, json_node_free);
    if (view->live_channels) g_object_remove_weak_pointer(G_OBJECT(view->live_channels), (gpointer *)&view->live_channels);
    g_clear_pointer(&view->prepared, json_node_free); g_clear_pointer(&view->preview_event, json_node_free);
    g_clear_pointer(&view->playlists_client, radios_data_client_unref);
    g_clear_pointer(&view->playlist_request_client, radios_data_client_unref);
    g_free(view->playlists_station); g_free(view->playlists_endpoint);
    g_free(view->preview_notes);
    if (view->preview_client) radios_data_client_unref(view->preview_client);
    if (view->plan) radios_playout_plan_free(view->plan);
    g_date_time_unref(view->week); g_hash_table_unref(view->selected); g_hash_table_unref(view->source_problems);
    g_clear_object(&view->style); g_clear_object(&view->style_screen);
    g_clear_object(&view->block_menu); g_clear_object(&view->row_menu);
    g_free(view->current); g_free(view->inspected_id); g_free(view);
}
static void destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; Calendar *view = data;
    radios_automation_calendar_prepare_close(view->page);
    view->disposed = TRUE;
    if (view->reorder_source) { g_source_remove(view->reorder_source); view->reorder_source = 0; }
    if (view->block_menu) gtk_widget_destroy(view->block_menu);
    if (view->row_menu) gtk_widget_destroy(view->row_menu);
    GtkWidget *legacy = g_object_get_data(G_OBJECT(view->page), "legacy-rules");
    if (legacy) gtk_widget_destroy(legacy);
    if (view->rotation_dialog) gtk_widget_destroy(view->rotation_dialog);
    if (view->library_dialog) gtk_widget_destroy(view->library_dialog);
    if (view->schema_dialog) gtk_widget_destroy(view->schema_dialog);
    if (view->style && view->style_screen)
        gtk_style_context_remove_provider_for_screen(view->style_screen, GTK_STYLE_PROVIDER(view->style));
}
static void invalidate_preview(Calendar *view)
{
    ++view->preview_generation; view->preview_busy = FALSE;
    g_clear_pointer(&view->prepared, json_node_free); g_clear_pointer(&view->preview_event, json_node_free);
    g_clear_pointer(&view->preview_client, radios_data_client_unref); g_clear_pointer(&view->plan, radios_playout_plan_free);
    g_clear_pointer(&view->preview_notes, g_free);
    if (view->preview_button) gtk_widget_set_sensitive(view->preview_button, TRUE);
    if (view->append_button) gtk_widget_set_sensitive(view->append_button, FALSE);
    if (view->schedule_button) gtk_widget_set_sensitive(view->schedule_button, FALSE);
}
static void set_week(Calendar *view, GDateTime *date)
{
    GDateTime *monday = g_date_time_add_days(date, 1 - g_date_time_get_day_of_week(date));
    if (!monday) { notice(view, "Hafta desteklenen tarih aralığının dışında."); return; }
    invalidate_preview(view);
    g_clear_pointer(&view->week, g_date_time_unref); view->week = monday;
    gchar *text = g_date_time_format(monday, "%Y-%m-%d");
    gtk_entry_set_text(GTK_ENTRY(view->date), text); g_free(text);
    GDateTime *sunday = g_date_time_add_days(monday, 6);
    gchar *first = g_date_time_format(monday, "%d.%m.%Y");
    gchar *last = sunday ? g_date_time_format(sunday, "%d.%m.%Y") : g_strdup("—");
    gchar *range = g_strdup_printf("%s – %s", first, last);
    gtk_label_set_text(GTK_LABEL(view->week_range), range);
    g_free(first); g_free(last); g_free(range); if (sunday) g_date_time_unref(sunday);
    view->has_selection = FALSE; g_hash_table_remove_all(view->selected);
    g_clear_pointer(&view->current, g_free); refresh(view);
}
static GDateTime *parse_date(const gchar *text)
{
    if (!g_regex_match_simple("^[0-9]{4}-[0-9]{2}-[0-9]{2}$", text, 0, 0)) return NULL;
    gchar *iso = g_strdup_printf("%sT00:00:00Z", text);
    GDateTime *date = g_date_time_new_from_iso8601(iso, NULL); g_free(iso); return date;
}
static void date_changed(GtkEntry *entry, gpointer data)
{
    Calendar *view = data; GDateTime *date = parse_date(gtk_entry_get_text(entry));
    if (!date) { notice(view, "Geçerli bir tarih girin: YYYY-AA-GG."); return; }
    set_week(view, date); g_date_time_unref(date);
}
static void navigate(GtkButton *button, gpointer data)
{
    Calendar *view = data; gint direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "direction"));
    GDateTime *date = direction ? g_date_time_add_days(view->week, direction * 7) : calendar_now(view);
    if (date) { set_week(view, date); g_date_time_unref(date); }
}
/* A catalogue belongs to the exact authenticated session that fetched it.
 * Reconnects must not relabel an older station's cache as the new session. */
static gboolean playlist_identity_matches(Calendar *view, RadioSDataClient *client,
    const gchar *station, const gchar *endpoint)
{
    RadioSDataClient *current = radios_data_client_acquire(view->ctx);
    gboolean match = current && current == client &&
        !g_strcmp0(station, radios_data_station_id(view->ctx)) &&
        !g_strcmp0(endpoint, radios_data_endpoint_id(view->ctx));
    g_clear_pointer(&current, radios_data_client_unref); return match;
}
static JsonArray *calendar_playlists(Calendar *view)
{
    if (!view->playlists || !JSON_NODE_HOLDS_OBJECT(view->playlists) ||
        !playlist_identity_matches(view, view->playlists_client, view->playlists_station, view->playlists_endpoint)) return NULL;
    JsonNode *node = json_object_get_member(json_node_get_object(view->playlists), "playlists");
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static gchar *playlist_name(Calendar *view, const gchar *id)
{
    JsonArray *array = calendar_playlists(view);
    for (guint i = 0; array && i < json_array_get_length(array); ++i) {
        JsonObject *playlist = json_array_get_object_element(array, i);
        if (!g_strcmp0(radios_json_string(playlist, "_id", ""), id))
            return g_strdup(radios_json_string(playlist, "name", "Playlist"));
    }
    return array && id && *id ? g_strdup_printf("Kullanılamayan playlist · %s", id) : g_strdup(id ? id : "Playlist seçilmedi");
}
/* This is a read-only preflight of the authenticated catalogue, never a
 * replacement for prepare's file, copyright and scheduling validation. A
 * metadata draft alone is not evidence that the block can produce audio. */
static gchar *source_problem_uncached(Calendar *view, JsonObject *source)
{
    const gchar *id = radios_json_string(source, "playlist_id", "");
    if (!*id) return g_strdup("playlist seçilmemiş");
    JsonArray *catalogue = calendar_playlists(view);
    if (!catalogue) return g_strdup("playlist içeriği henüz okunmadı; yayın uygunluğu doğrulanamadı");
    JsonObject *playlist = NULL;
    for (guint i = 0; i < json_array_get_length(catalogue); ++i) {
        JsonObject *candidate = json_array_get_object_element(catalogue, i);
        if (!g_strcmp0(radios_json_string(candidate, "_id", ""), id)) { playlist = candidate; break; }
    }
    if (!playlist) return g_strdup_printf("playlist kaldırılmış veya devre dışı: %s", id);
    const gchar *name = radios_json_string(playlist, "name", "Playlist");
    JsonNode *node = json_object_get_member(playlist, "tracks");
    JsonArray *tracks = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
    if (!tracks || !json_array_get_length(tracks)) return g_strdup_printf("'%s' playlisti boş", name);
    for (guint i = 0; i < json_array_get_length(tracks); ++i) {
        JsonObject *track = json_array_get_object_element(tracks, i);
        if (g_uuid_string_is_valid(radios_json_string(track, "media_id", "")) &&
            g_uuid_string_is_valid(radios_json_string(track, "version_id", "")) &&
            g_uuid_string_is_valid(radios_json_string(track, "media_file_id", "")) &&
            strlen(radios_json_string(track, "media_sha256", "")) == 64) return NULL;
    }
    return g_strdup_printf("'%s' playlistinde doğrulanmış yerel ses kaydı yok", name);
}
static gchar *source_problem(Calendar *view, JsonObject *source)
{
    /* A live event step has no playlist to verify (contract §2). */
    if (!g_strcmp0(radios_json_string(source, "source_type", "playlist"), "live")) return NULL;
    const gchar *id = radios_json_string(source, "playlist_id", "");
    /* Several blocks often share a playlist. Never rescan every track on each
     * canvas repaint; refresh invalidates this read-only catalogue summary. */
    const gchar *cached = calendar_playlists(view) ? g_hash_table_lookup(view->source_problems, id) : NULL;
    if (cached) return *cached ? g_strdup(cached) : NULL;
    gchar *issue = source_problem_uncached(view, source);
    g_hash_table_replace(view->source_problems, g_strdup(id), g_strdup(issue ? issue : ""));
    return issue;
}
static gchar *event_problem(Calendar *view, JsonObject *event)
{
    JsonNode *node = event ? json_object_get_member(event, "rotations") : NULL;
    JsonArray *rows = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
    if (!rows || !json_array_get_length(rows)) return g_strdup("rotasyon atanmamış");
    for (guint i = 0; i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        const gchar *id = radios_json_string(row, "rotation_id", "");
        if (!*id) {
            gchar *issue = source_problem(view, row); if (issue) return issue;
            continue;
        }
        JsonObject *definition = library_by_id(view, id);
        if (!definition) return g_strdup("atanan rotasyon bulunamadı");
        const gchar *name = radios_json_string(definition, "name", "Rotasyon");
        JsonArray *sources = json_object_get_array_member(definition, "sources");
        if (!sources || !json_array_get_length(sources)) return g_strdup_printf("'%s' rotasyonunda playlist yok", name);
        for (guint j = 0; j < json_array_get_length(sources); ++j) {
            gchar *issue = source_problem(view, json_array_get_object_element(sources, j));
            if (issue) { gchar *text = g_strdup_printf("'%s': %s", name, issue); g_free(issue); return text; }
        }
    }
    return NULL;
}
static gchar *event_warning(Calendar *view, JsonObject *event)
{
    gchar *problem = event_problem(view, event); if (!problem) return NULL;
    guint from = (guint)radios_json_int(event, "start_minute", 0), to = (guint)radios_json_int(event, "end_minute", 0);
    gchar *text = g_strdup_printf("Uyarı: %s.\n%s · %s %02u:%02u–%02u:%02u. Taslak kaydedilebilir; yayın öncesinde içeriği tamamlayın.",
        problem, radios_json_string(event, "title", "Blok"), radios_json_string(event, "date", ""),
        from / 60, from % 60, to / 60, to % 60);
    g_free(problem); return text;
}
static void refresh(Calendar *view)
{
    g_hash_table_remove_all(view->source_problems);
    timezone_render(view);
    /* Background playlist names and save receipts may rebuild the table while
     * the operator selects rows. Retain selection by stable rotation UUID. */
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view->rows));
    GHashTable *chosen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GList *paths = gtk_tree_selection_get_selected_rows(selection, NULL);
    for (GList *item = paths; item; item = item->next) {
        GtkTreeIter iter; gchar *id = NULL;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(view->store), &iter, item->data)) {
            gtk_tree_model_get(GTK_TREE_MODEL(view->store), &iter, 0, &id, -1); if (id) g_hash_table_add(chosen, id);
        }
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    view->updating = TRUE;
    if (view->has_selection) {
        gchar *first = date_at(view, view->day_first), *last = date_at(view, view->day_last);
        gchar *text = g_strdup_printf("%s%s%s  ·  %02u:%02u – %02u:%02u  ·  %u event seçili",
            first, view->day_first == view->day_last ? "" : " → ", view->day_first == view->day_last ? "" : last,
            view->start / 60, view->start % 60, view->end / 60, view->end % 60, g_hash_table_size(view->selected));
        gtk_label_set_text(GTK_LABEL(view->selection), text); g_free(first); g_free(last); g_free(text);
    } else gtk_label_set_text(GTK_LABEL(view->selection), "Takvimde saat aralığı seçin");
    gtk_widget_set_sensitive(view->save_selection, view->has_selection && !g_hash_table_size(view->selected) &&
        (!calendar_active_document(view) || radios_data_document_ready(calendar_active_document(view))));
    JsonObject *event = event_by_id(view, view->current);
    gchar *warning = event ? event_warning(view, event) : NULL;
    gchar *problem = event ? event_problem(view, event) : NULL;
    gchar *summary = problem ? g_strdup_printf("Uyarı: %s", problem) : NULL;
    gtk_label_set_text(GTK_LABEL(view->content_warning), summary ? summary : "");
    gtk_widget_set_tooltip_text(view->content_warning, warning);
    gtk_widget_set_visible(view->content_warning, warning != NULL);
    g_free(summary); g_free(problem); g_free(warning);
    library_choices(view);
    gboolean ready = !calendar_active_document(view) || radios_data_document_ready(calendar_active_document(view));
    gtk_widget_set_sensitive(view->independent, ready);
    gtk_widget_set_sensitive(view->schema, ready);
    gtk_widget_set_sensitive(view->library_edit, ready && gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->library_choice)));
    gtk_widget_set_sensitive(view->library_assign, ready && event && view->library_rows && gtk_tree_selection_count_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(view->library_rows))) > 0);
    gtk_widget_set_sensitive(view->editor, event != NULL);
    if (event) gtk_widget_show(view->editor); else gtk_widget_hide(view->editor);
    gtk_widget_set_visible(view->empty, event == NULL);
    update_detail_heading(view);
    /* Kılavuz §04 adım 2: a live programme assigns rotation schemas. */
    if (view->rotation_title) gtk_label_set_text(GTK_LABEL(view->rotation_title), event_live(event) ? "2  Rotasyon şemaları" : "2  Rotasyonlar");
    GtkWidget *focus = gtk_window_get_focus(view->ctx->parent);
    gboolean keep_input = event && !g_strcmp0(view->inspected_id, view->current) &&
        !g_object_get_data(G_OBJECT(view->page), "radios-data-explicit-reload") &&
        (view->inspector_invalid || focus == view->name || focus == view->programme_date || focus == view->programme_start || focus == view->programme_end);
    if (!keep_input) {
    view->inspector_invalid = FALSE;
    gtk_entry_set_text(GTK_ENTRY(view->name), event ? radios_json_string(event, "title", "") : "");
    gtk_combo_box_set_active(GTK_COMBO_BOX(view->color), event &&
        g_str_equal(radios_json_string(event, "color", ""), "#abfe33") ? 1 : 0);
    gtk_entry_set_text(GTK_ENTRY(view->programme_date), event ? radios_json_string(event, "date", "") : "");
    guint from = event ? (guint)radios_json_int(event, "start_minute", 0) : 0;
    guint to = event ? (guint)radios_json_int(event, "end_minute", 0) : 0;
    gchar *time = event ? g_strdup_printf("%02u:%02u", from / 60, from % 60) : g_strdup("");
    gtk_entry_set_text(GTK_ENTRY(view->programme_start), time); g_free(time);
    time = event ? g_strdup_printf("%02u:%02u", to / 60, to % 60) : g_strdup("");
    gtk_entry_set_text(GTK_ENTRY(view->programme_end), time); g_free(time);
    }
    g_free(view->inspected_id); view->inspected_id = g_strdup(view->current);
    gtk_list_store_clear(view->store);
    JsonArray *rotations = event ? json_object_get_array_member(event, "rotations") : NULL;
    gtk_combo_box_set_active(GTK_COMBO_BOX(view->rotation_mode), event &&
        !g_strcmp0(radios_json_string(event, "rotation_selection_mode", "linear"), "weighted") ? 1 : 0);
    gchar *count = g_strdup_printf("%u rotasyon", rotations ? json_array_get_length(rotations) : 0);
    gtk_label_set_text(GTK_LABEL(view->rotation_count), count); g_free(count);
    for (guint i = 0; rotations && i < json_array_get_length(rotations); ++i) {
        JsonObject *row = json_array_get_object_element(rotations, i); GtkTreeIter iter;
        const gchar *reference = radios_json_string(row, "rotation_id", "");
        if (*reference) {
            JsonObject *definition = library_by_id(view, reference);
            JsonArray *sources = definition ? json_object_get_array_member(definition, "sources") : NULL;
            JsonObject *settings = definition ? json_object_get_object_member(definition, "settings") : NULL;
            guint unavailable = 0; JsonArray *catalog = calendar_playlists(view);
            for (guint j = 0; catalog && sources && j < json_array_get_length(sources); ++j) {
                const gchar *playlist_id = radios_json_string(json_array_get_object_element(sources, j), "playlist_id", "");
                gboolean found = FALSE;
                for (guint k = 0; k < json_array_get_length(catalog); ++k)
                    found |= !g_strcmp0(radios_json_string(json_array_get_object_element(catalog, k), "_id", ""), playlist_id);
                if (!found) ++unavailable;
            }
            gchar *amount = unavailable ? g_strdup_printf("%u playlist · %u kullanılamıyor", sources ? json_array_get_length(sources) : 0, unavailable) :
                g_strdup_printf("%u playlist", sources ? json_array_get_length(sources) : 0);
            gchar *absent = radios_automation_calendar_excluded_kinds_text(settings);
            gchar *title = g_strdup_printf("%s%s%s", definition ? radios_json_string(definition, "name", "Rotasyon") : "Rotasyon bulunamadı", *absent ? " · yok: " : "", absent);
            gtk_list_store_append(view->store, &iter);
            gtk_list_store_set(view->store, &iter, 0, radios_json_string(row, "id", ""), 1, title,
                2, amount, 3, settings && !g_strcmp0(radios_json_string(settings, "selection_mode", "linear"), "weighted") ? "Olasılıklı" : "Doğrusal",
                4, "Olasılık", 5, "Ayarlar", 6, "Ayarlar", 7, (gint)radios_json_int(row, "weight", 100), -1);
            if (g_hash_table_contains(chosen, radios_json_string(row, "id", ""))) gtk_tree_selection_select_iter(selection, &iter);
            g_free(amount); g_free(title); g_free(absent); continue;
        }
        gchar *name = playlist_name(view, radios_json_string(row, "playlist_id", ""));
        const gchar *kind = radios_json_string(row, "media_kind", "all");
        if (strcmp(kind, "all")) {
            const gchar *caption = !strcmp(kind, "music") ? "Müzik" : !strcmp(kind, "jingle") ? "Jingle" : !strcmp(kind, "advertisement") ? "Reklam" : "Sweeper";
            gchar *typed = g_strdup_printf("%s · %s", caption, name); g_free(name); name = typed;
        }
        gchar *frequency = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(row, "frequency", 1));
        gtk_list_store_append(view->store, &iter);
        gtk_list_store_set(view->store, &iter, 0, radios_json_string(row, "id", ""), 1, name, 2, frequency,
            3, json_object_get_boolean_member(row, "random") ? "Rastgele" : "Sırayla",
            4, json_object_get_boolean_member(row, "track_random") ? "Rastgele" : "Sırayla",
            5, json_object_get_boolean_member(row, "advert_random") ? "Rastgele" : "Sırayla",
            6, json_object_get_boolean_member(row, "sweeper_random") ? "Rastgele" : "Sırayla", 7, -1, -1);
        if (g_hash_table_contains(chosen, radios_json_string(row, "id", ""))) gtk_tree_selection_select_iter(selection, &iter);
        g_free(name); g_free(frequency);
    }
    g_hash_table_unref(chosen);
    view->updating = FALSE; gtk_widget_queue_draw(view->grid); gtk_widget_queue_draw(view->header);
}
static gboolean autosave_tick(gpointer data);
static void submit(Calendar *view)
{
    if (view->editor_changed) {
        /* The parent already received the edit synchronously in changed(). */
        gtk_label_set_text(GTK_LABEL(view->save_status), "Program taslağı güncellendi");
        return;
    }
    if (view->document) {
        if (!radios_data_document_ready(view->document)) {
            gtk_label_set_text(GTK_LABEL(view->save_status), "Bağlantı bekleniyor");
            notice(view, "Değişiklikler henüz kaydedilmedi; veri bağlantısı bekleniyor.");
            if (!view->autosave_source) view->autosave_source = g_timeout_add_seconds(1, autosave_tick, view);
            return;
        }
        gboolean submitted = radios_data_document_autosave(view->document, view->config);
        if (!radios_data_connected(view->ctx)) {
            gtk_label_set_text(GTK_LABEL(view->save_status), "Bağlantı bekleniyor");
            notice(view, "Değişiklikler otomatik kayıt kuyruğunda; veri bağlantısı bekleniyor.");
            if (!view->autosave_source) view->autosave_source = g_timeout_add_seconds(1, autosave_tick, view);
            return;
        }
        gtk_label_set_text(GTK_LABEL(view->save_status), submitted ? "Kaydediliyor…" : "Kayıt doğrulanamadı");
        notice(view, submitted ? "Değişiklikler otomatik kaydediliyor…" :
            "Otomatik kayıt doğrulanamadı; değişiklikler bu oturumda korunuyor.");
    } else {
        gtk_label_set_text(GTK_LABEL(view->save_status), "Kaydedilmedi");
        notice(view, "Veritabanı bağlantısı olmadan kalıcı kayıt yapılmadı.");
    }
}
static gboolean autosave_tick(gpointer data)
{
    Calendar *view = data; view->autosave_source = 0;
    if (!view->disposed && view->dirty) submit(view);
    return G_SOURCE_REMOVE;
}
static void changed(Calendar *view)
{
    invalidate_preview(view); view->dirty = TRUE;
    if (view->document && !view->bound_editor) radios_data_document_dirty(view->document, TRUE);
    if (view->editor_changed) {
        /* Fence an older ACK before the next main-loop iteration. Database
         * transport remains debounced by the parent, while a valid visible
         * edit must immediately make its owning document dirty. */
        JsonNode *snapshot = deep_copy(view->config);
        view->editor_changed(view->page, snapshot, view->editor_user);
        json_node_free(snapshot);
    }
    gtk_label_set_text(GTK_LABEL(view->save_status), "Kaydediliyor…");
}
static void commit(Calendar *view)
{
    if (view->autosave_source) { g_source_remove(view->autosave_source); view->autosave_source = 0; }
    changed(view); submit(view); refresh(view);
}
void radios_automation_calendar_prepare_close(GtkWidget *page)
{
    GtkWidget *programmes = page ? g_object_get_data(G_OBJECT(page), "weekly-programme-workspace") : NULL;
    if (programmes) radios_weekly_programme_prepare_close(programmes);
    Calendar *view = page ? g_object_get_data(G_OBJECT(page), "automation-calendar") : NULL;
    if (!view || view->disposed || view->closing) return;
    if (view->reorder_source) {
        g_source_remove(view->reorder_source); view->reorder_source = 0;
        if (sync_row_order(view, GTK_TREE_MODEL(view->store))) changed(view);
    }
    view->closing = TRUE;
    /* Close owned popup surfaces before their parent window starts unmapping. */
    if (view->block_menu) gtk_menu_popdown(GTK_MENU(view->block_menu));
    if (view->row_menu) gtk_menu_popdown(GTK_MENU(view->row_menu));
    if (view->autosave_source) { g_source_remove(view->autosave_source); view->autosave_source = 0; }
    /* While the shared grid edits a weekly programme, its dirty flag belongs
     * to that programme. Only a preserved unsaved Main draft may enqueue a
     * Main write; closing a weekly CAS conflict must not advance Main's rev. */
    gboolean main_dirty = view->bound_editor ? view->saved_main_dirty : view->dirty;
    if (view->document && main_dirty) {
        radios_data_document_autosave(view->document, view->bound_editor && view->saved_main ? view->saved_main : view->config);
        if (!radios_data_document_preserve_pending(view->document))
            g_printerr("Rotasyon otomatik kayıt kuyruğu korunamadı.\n");
    }
}
static void save_status_changed(RadioSDocument *document, const gchar *text, gpointer data)
{
    (void)document; Calendar *view = data;
    if (!view->disposed && !view->closing && !view->bound_editor) gtk_label_set_text(GTK_LABEL(view->save_status), text);
}
static void applied(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; Calendar *view = data;
    JsonNode *incoming = object_node(config);
    if (view->bound_editor) {
        /* Global calendar ACKs belong to the preserved main draft, while the
         * single visible grid is editing a separately pinned programme. */
        gboolean reload = g_object_get_data(G_OBJECT(owner), "radios-data-explicit-reload") != NULL;
        if (reload || !view->saved_main_dirty || json_node_equal(view->saved_main, incoming)) {
            g_clear_pointer(&view->saved_main, json_node_free); view->saved_main = deep_copy(incoming);
            view->saved_main_dirty = FALSE;
        }
        json_node_free(incoming); return;
    }
    if (view->dirty && !g_object_get_data(G_OBJECT(owner), "radios-data-explicit-reload") &&
        !json_node_equal(view->config, incoming)) { json_node_free(incoming); return; }
    g_clear_pointer(&view->config, json_node_free); view->config = deep_copy(incoming); json_node_free(incoming);
    invalidate_preview(view);
    library_reconcile_editor(view);
    if (view->library_dialog && g_object_get_data(G_OBJECT(owner), "radios-data-explicit-reload"))
        gtk_widget_destroy(view->library_dialog);
    view->dirty = FALSE;
    refresh(view);
    gtk_label_set_text(GTK_LABEL(view->save_status), view->inspector_invalid ? "Alanları tamamlayın" : "Kaydedildi");
    notice(view, view->inspector_invalid ? "Geçerli değişiklikler kaydedildi; düzenlediğiniz alanı tamamlayın." : "Değişiklikler veritabanına kaydedildi.");
}

static void text_at(cairo_t *cr, GtkWidget *widget, const gchar *text, gdouble x, gdouble y, gint width)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, text);
    PangoFontDescription *font = radios_ui_drawn_font_new(10.0 * 96.0 / 72.0, PANGO_WEIGHT_NORMAL);   /* 10 pt at 96 dpi */
    pango_layout_set_font_description(layout, font); pango_font_description_free(font);
    if (width > 0) { pango_layout_set_width(layout, width * PANGO_SCALE); pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END); }
    cairo_move_to(cr, x, y); pango_cairo_show_layout(cr, layout); g_object_unref(layout);
}
static gdouble row_height(Calendar *view) { return view->step == 15 ? 28.0 : view->step == 30 ? 34.0 : 46.0; }
static gdouble minute_y(Calendar *view, guint minute) { return minute * row_height(view) / view->step; }
static gboolean draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    Calendar *view = data; GtkAllocation allocation; gtk_widget_get_allocation(view->grid, &allocation);
    gdouble col = (allocation.width - 60.0) / 7.0, height = row_height(view);
    cairo_set_source_rgb(cr, .09, .11, .14); cairo_paint(cr);
    const gchar *days[] = { "Pzt", "Sal", "Çar", "Per", "Cum", "Cmt", "Paz" };
    if (widget == view->header) {
      GtkAdjustment *horizontal = g_object_get_data(G_OBJECT(view->header), "horizontal");
      cairo_translate(cr, -gtk_adjustment_get_value(horizontal), 0);
      for (guint day = 0; day < 7; ++day) {
        GDateTime *date = g_date_time_add_days(view->week, day); gchar *stamp = date ? g_date_time_format(date, "%d.%m.%Y") : g_strdup("—");
        if (date) g_date_time_unref(date);
        cairo_set_source_rgb(cr, .27, .28, .29); cairo_rectangle(cr, 60 + col * day, 0, col - 1, 41); cairo_fill(cr);
        cairo_set_source_rgb(cr, .95, .95, .95);
        text_at(cr, widget, days[day], 65 + col * day, 2, (gint)col - 10);
        text_at(cr, widget, stamp, 65 + col * day, 21, (gint)col - 10); g_free(stamp);
      }
      return FALSE;
    }
    for (guint minute = 0; minute < 1440; minute += view->step) {
        gdouble y = minute_y(view, minute);
        cairo_set_source_rgb(cr, .25, .28, .32); cairo_set_line_width(cr, minute % 60 ? .5 : 1);
        cairo_move_to(cr, 60, y); cairo_line_to(cr, allocation.width, y); cairo_stroke(cr);
        if (!(minute % 60)) {
            gchar *time = g_strdup_printf("%02u:00", minute / 60); cairo_set_source_rgb(cr, .8, .83, .86);
            text_at(cr, widget, time, 4, y + 2, 55); g_free(time);
        }
    }
    for (guint day = 0; day <= 7; ++day) {
        cairo_set_source_rgb(cr, .4, .42, .44); cairo_move_to(cr, 60 + col * day, 0);
        cairo_line_to(cr, 60 + col * day, minute_y(view, 1440)); cairo_stroke(cr);
    }
    JsonArray *array = events(view);
    for (guint i = 0; i < json_array_get_length(array); ++i) {
        JsonObject *event = json_array_get_object_element(array, i); gint day = event_day(view, event); if (day < 0) continue;
        guint start = (guint)radios_json_int(event, "start_minute", 0), end = (guint)radios_json_int(event, "end_minute", 0);
        gdouble x = 62 + day * col, y = minute_y(view, start), h = minute_y(view, end) - y;
        GdkRGBA color; gdk_rgba_parse(&color, radios_json_string(event, "color", "#abd6e4"));
        cairo_set_source_rgba(cr, color.red, color.green, color.blue, 1); cairo_rectangle(cr, x, y + 1, col - 4, h - 2); cairo_fill(cr);
        cairo_save(cr); cairo_rectangle(cr, x + 2, y + 1, col - 8, MAX(0.0, h - 2)); cairo_clip(cr);
        cairo_set_source_rgb(cr, .06, .12, .15); gchar *time = g_strdup_printf("%02u:%02u–%02u:%02u", start / 60, start % 60, end / 60, end % 60);
        text_at(cr, widget, time, x + 4, y + 3, (gint)col - 12); g_free(time);
        text_at(cr, widget, radios_json_string(event, "title", ""), x + 4, y + 20, (gint)col - 12); cairo_restore(cr);
        gchar *problem = event_problem(view, event);
        if (problem) {
            cairo_save(cr); const gdouble dash[] = {5, 3}; cairo_set_dash(cr, dash, G_N_ELEMENTS(dash), 0);
            cairo_set_source_rgb(cr, .84, .12, .15); cairo_set_line_width(cr, 2);
            cairo_rectangle(cr, x + 1, y + 2, col - 6, MAX(0.0, h - 4)); cairo_stroke(cr);
            cairo_restore(cr);
        }
        if (g_hash_table_contains(view->selected, radios_json_string(event, "id", ""))) {
            gdouble inset = problem ? 4 : 1;
            cairo_set_source_rgb(cr, .07, .77, .87); cairo_set_line_width(cr, 3);
            cairo_rectangle(cr, x + inset, y + inset + 1, col - 4 - 2 * inset, MAX(0.0, h - 2 * inset - 2)); cairo_stroke(cr);
        }
        g_free(problem);
    }
    if (view->drag_blocks && view->moved) {
        for (guint i = 0; i < json_array_get_length(array); ++i) {
            JsonObject *event = json_array_get_object_element(array, i);
            if (!g_hash_table_contains(view->selected, radios_json_string(event, "id", ""))) continue;
            gint day = event_day(view, event) + view->drag_day;
            gint start = (gint)radios_json_int(event, "start_minute", 0) + view->drag_minute;
            gint end = (gint)radios_json_int(event, "end_minute", 0) + view->drag_minute;
            if (day < 0 || day > 6 || start < 0 || end > 1440) continue;
            cairo_set_source_rgba(cr, .07, .77, .87, .35);
            cairo_rectangle(cr, 62 + day * col, minute_y(view, (guint)start), col - 4, minute_y(view, (guint)(end - start)));
            cairo_fill_preserve(cr); cairo_set_source_rgb(cr, .07, .77, .87);
            cairo_set_line_width(cr, 2); cairo_stroke(cr);
        }
    }
    if (view->has_selection && (!g_hash_table_size(view->selected) || (view->dragging && !view->drag_blocks))) {
        gdouble x = 61 + view->day_first * col, y = minute_y(view, view->start), w = (view->day_last - view->day_first + 1) * col - 2;
        cairo_set_source_rgba(cr, .07, .77, .87, .22); cairo_rectangle(cr, x, y, w, minute_y(view, view->end) - y); cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, .07, .77, .87); cairo_set_line_width(cr, 2); cairo_stroke(cr);
    }
    (void)height; return FALSE;
}
static gboolean coordinates(Calendar *view, gdouble x, gdouble y, guint *day, guint *minute)
{
    GtkAllocation a; gtk_widget_get_allocation(view->grid, &a);
    if (x < 60 || x >= a.width || y < 0 || y >= minute_y(view, 1440)) return FALSE;
    *day = MIN(6u, (guint)((x - 60) / ((a.width - 60.0) / 7.0)));
    gchar *date = date_at(view, *day); gboolean valid = *date != '\0'; g_free(date);
    if (!valid) return FALSE;
    *minute = MIN(1440u - view->step, (guint)(y / row_height(view)) * view->step); return TRUE;
}
static JsonObject *event_at(Calendar *view, guint day, guint minute)
{
    JsonArray *array = events(view); gchar *date = date_at(view, day); JsonObject *found = NULL;
    for (guint i = 0; i < json_array_get_length(array); ++i) {
        JsonObject *event = json_array_get_object_element(array, i);
        if (g_str_equal(date, radios_json_string(event, "date", "")) &&
            minute >= (guint)radios_json_int(event, "start_minute", 0) && minute < (guint)radios_json_int(event, "end_minute", 0)) { found = event; break; }
    }
    g_free(date); return found;
}
static gboolean grid_tooltip(GtkWidget *widget, gint x, gint y, gboolean keyboard, GtkTooltip *tooltip, gpointer data)
{
    (void)widget; Calendar *view = data; guint day, minute;
    JsonObject *event = keyboard ? event_by_id(view, view->current) : NULL;
    if (!keyboard && coordinates(view, x, y, &day, &minute)) {
        minute = MIN(1439u, (guint)(y * view->step / row_height(view)));
        event = event_at(view, day, minute);
    }
    gchar *warning = event ? event_warning(view, event) : NULL;
    if (warning) gtk_tooltip_set_text(tooltip, warning);
    gboolean shown = warning != NULL; g_free(warning); return shown;
}
void radios_automation_calendar_select(GtkWidget *page, guint day, guint start, guint end, gboolean extend)
{
    Calendar *view = g_object_get_data(G_OBJECT(page), "automation-calendar");
    if (!view || day > 6 || start >= end || end > 1440) return;
    gchar *date = date_at(view, day); gboolean valid = *date != '\0'; g_free(date);
    if (!valid) return;
    invalidate_preview(view);
    if (!extend) g_hash_table_remove_all(view->selected);
    view->has_selection = TRUE; view->day_first = view->day_last = day; view->start = start; view->end = end;
    JsonObject *event = event_at(view, day, start);
    g_clear_pointer(&view->current, g_free);
    if (event) {
        view->current = g_strdup(radios_json_string(event, "id", ""));
        if (extend && g_hash_table_contains(view->selected, view->current)) g_hash_table_remove(view->selected, view->current);
        else g_hash_table_add(view->selected, g_strdup(view->current));
        view->start = (guint)radios_json_int(event, "start_minute", start); view->end = (guint)radios_json_int(event, "end_minute", end);
    }
    show_programme(view); refresh(view);
}
JsonNode *radios_automation_calendar_snapshot(GtkWidget *page)
{
    Calendar *view = g_object_get_data(G_OBJECT(page), "automation-calendar");
    return view ? deep_copy(view->config) : NULL;
}
static gboolean press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget; Calendar *view = data; guint day, minute;
    if (!coordinates(view, event->x, event->y, &day, &minute)) return FALSE;
    if (calendar_active_document(view) && !radios_data_document_ready(calendar_active_document(view))) {
        notice(view, "Takvim yükleniyor; kayıt hazır olduğunda düzenleyebilirsiniz."); return TRUE;
    }
    if (event->button != 1 && event->button != 3) return FALSE;
    gtk_widget_grab_focus(view->grid);
    guint hit_minute = MIN(1439u, (guint)(event->y * view->step / row_height(view)));
    JsonObject *hit = event_at(view, day, hit_minute);
    guint select_first = hit ? (guint)radios_json_int(hit, "start_minute", minute) : minute;
    guint select_last = hit ? (guint)radios_json_int(hit, "end_minute", minute + view->step) : minute + view->step;
    gboolean already = hit && g_hash_table_contains(view->selected, radios_json_string(hit, "id", ""));
    if (event->button == 3) {
        if (!already) radios_automation_calendar_select(view->page, day, select_first, select_last, FALSE);
        block_popup(view, (GdkEvent *)event); return TRUE;
    }
    if (!(event->state & GDK_SHIFT_MASK && view->has_selection && !hit)) { view->anchor_day = day; view->anchor_minute = minute; }
    view->press_x = event->x; view->press_y = event->y;
    view->drag_day = 0; view->drag_minute = 0; view->moved = FALSE; view->drag_blocks = FALSE;
    if (event->state & GDK_SHIFT_MASK && view->has_selection && !hit) {
        view->dragging = TRUE; view->moved = TRUE;
    } else {
        if (!already || event->state & GDK_CONTROL_MASK)
            radios_automation_calendar_select(view->page, day, select_first, select_last, event->state & GDK_CONTROL_MASK);
        else {
            g_free(view->current); view->current = g_strdup(radios_json_string(hit, "id", ""));
            show_programme(view); refresh(view);
        }
        view->dragging = !(event->state & GDK_CONTROL_MASK);
        view->drag_blocks = hit != NULL && view->dragging;
    }
    if (event->type == GDK_2BUTTON_PRESS && view->current) gtk_widget_grab_focus(view->name);
    return TRUE;
}
static gboolean motion(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    (void)widget; Calendar *view = data; guint day, minute;
    if (!view->dragging || !coordinates(view, event->x, event->y, &day, &minute)) return FALSE;
    if (view->drag_blocks) {
        if (view->moved || gtk_drag_check_threshold(view->grid, (gint)view->press_x, (gint)view->press_y, (gint)event->x, (gint)event->y)) {
            view->moved = TRUE; view->drag_day = (gint)day - (gint)view->anchor_day;
            view->drag_minute = (gint)minute - (gint)view->anchor_minute;
            gtk_widget_queue_draw(view->grid);
        }
    } else if (day != view->anchor_day || minute != view->anchor_minute || view->moved) {
        view->moved = TRUE; view->has_selection = TRUE; g_hash_table_remove_all(view->selected); g_clear_pointer(&view->current, g_free);
        view->day_first = MIN(day, view->anchor_day); view->day_last = MAX(day, view->anchor_day);
        view->start = MIN(minute, view->anchor_minute); view->end = MAX(minute, view->anchor_minute) + view->step;
        refresh(view);
    }
    return TRUE;
}
static gboolean release(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget; Calendar *view = data;
    if (event->button != 1) return FALSE;
    guint released_day, released_minute;
    gboolean move = view->dragging && view->drag_blocks && view->moved &&
        coordinates(view, event->x, event->y, &released_day, &released_minute);
    view->dragging = FALSE; view->drag_blocks = FALSE;
    if (move) {
        view->drag_day = (gint)released_day - (gint)view->anchor_day;
        view->drag_minute = (gint)released_minute - (gint)view->anchor_minute;
        move_blocks(view, view->drag_day, view->drag_minute);
    }
    else if (!view->moved && view->current && !(event->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))) gtk_widget_grab_focus(view->name);
    view->moved = FALSE; refresh(view); return TRUE;
}
static void zoom(GtkButton *button, gpointer data)
{
    Calendar *view = data; gint direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "direction"));
    view->step = direction > 0 ? MAX(15u, view->step / 2) : MIN(60u, view->step * 2);
    gtk_widget_set_size_request(view->grid, 700, (gint)minute_y(view, 1440)); gtk_widget_queue_draw(view->grid);
}
static void header_scrolled(GtkAdjustment *adjustment, gpointer data)
{ (void)adjustment; gtk_widget_queue_draw(GTK_WIDGET(data)); }
static void grid_resized(GtkWidget *widget, GtkAllocation *allocation, gpointer data)
{ (void)widget; (void)allocation; Calendar *view = data; gtk_widget_queue_draw(view->header); }
static JsonObject *new_event(const gchar *title, const gchar *date, guint start, guint end, const gchar *color)
{
    JsonObject *event = json_object_new(); gchar *id = g_uuid_string_random();
    json_object_set_string_member(event, "id", id); g_free(id);
    json_object_set_string_member(event, "title", title); json_object_set_string_member(event, "date", date);
    json_object_set_int_member(event, "start_minute", start); json_object_set_int_member(event, "end_minute", end);
    json_object_set_string_member(event, "color", color); json_object_set_array_member(event, "rotations", json_array_new()); return event;
}
/* radyo.pdf 1: the programme name is picked from existing programmes: block
 * names already used in this calendar and programmes saved in the Program
 * tab. Picking a block name also brings that programme's rotations. */
static GtkWidget *named_descendant(GtkWidget *widget, const gchar *name)
{
    if (!widget || !g_strcmp0(gtk_widget_get_name(widget), name)) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = named_descendant(item->data, name);
    g_list_free(children); return found;
}
static gint name_compare(gconstpointer a, gconstpointer b)
{ return g_utf8_collate(*(const gchar *const *)a, *(const gchar *const *)b); }
static gboolean name_add(GPtrArray *names, GHashTable *seen, const gchar *title)
{
    gchar *clean = g_strstrip(g_strdup(title ? title : "")), *key = g_utf8_casefold(clean, -1);
    gboolean added = *clean && !g_hash_table_contains(seen, key);
    if (added) { g_hash_table_add(seen, key); g_ptr_array_add(names, clean); } else { g_free(key); g_free(clean); }
    return added;
}
static GPtrArray *programme_names(Calendar *view)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free); GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    JsonArray *array = events(view);
    for (guint i = 0; array && i < json_array_get_length(array); ++i) name_add(names, seen, radios_json_string(json_array_get_object_element(array, i), "title", ""));
    GtkWidget *workspace = NULL;
    for (GtkWidget *widget = view->page; widget && !workspace; widget = gtk_widget_get_parent(widget)) workspace = g_object_get_data(G_OBJECT(widget), "weekly-programme-workspace");
    /* Saved programmes are read from the Program tab's own list (text, id). */
    GtkWidget *choice = named_descendant(workspace, "weekly-programme-choice");
    GtkTreeModel *model = GTK_IS_COMBO_BOX(choice) ? gtk_combo_box_get_model(GTK_COMBO_BOX(choice)) : NULL; GtkTreeIter iter;
    for (gboolean more = model && gtk_tree_model_get_iter_first(model, &iter); more; more = gtk_tree_model_iter_next(model, &iter)) {
        gchar *title = NULL, *id = NULL; gtk_tree_model_get(model, &iter, 0, &title, 1, &id, -1);
        if (id && g_uuid_string_is_valid(id)) name_add(names, seen, title);
        g_free(title); g_free(id);
    }
    g_hash_table_unref(seen); g_ptr_array_sort(names, name_compare); return names;
}
static JsonObject *programme_template(Calendar *view, const gchar *title)
{
    gchar *clean = g_strstrip(g_strdup(title ? title : "")), *key = g_utf8_casefold(clean, -1); JsonObject *best = NULL;
    JsonArray *array = events(view);
    for (guint i = 0; *key && array && i < json_array_get_length(array); ++i) {
        JsonObject *event = json_array_get_object_element(array, i);
        gchar *name = g_strstrip(g_strdup(radios_json_string(event, "title", ""))), *other = g_utf8_casefold(name, -1);
        gboolean match = !g_strcmp0(key, other); g_free(name); g_free(other);
        if (!match) continue;
        gint order = best ? g_strcmp0(radios_json_string(event, "date", ""), radios_json_string(best, "date", "")) : 1;
        if (!best || order > 0 || (!order && radios_json_int(event, "start_minute", 0) > radios_json_int(best, "start_minute", 0))) best = event;
    }
    g_free(clean); g_free(key); return best;
}
static void policy_reidentify(JsonObject *policy, guint depth)
{
    const gchar *keys[] = {"transitions", "jingles", "children"};
    for (guint k = 0; policy && depth <= 8 && k < G_N_ELEMENTS(keys); ++k) {
        JsonNode *node = json_object_get_member(policy, keys[k]); JsonArray *items = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
        for (guint i = 0; items && i < json_array_get_length(items); ++i) {
            JsonNode *element = json_array_get_element(items, i); if (!JSON_NODE_HOLDS_OBJECT(element)) continue;
            JsonObject *item = json_node_get_object(element); gchar *id = g_uuid_string_random(); json_object_set_string_member(item, "id", id); g_free(id);
            JsonNode *child = k == 2 ? json_object_get_member(item, "policy") : NULL;
            if (child && JSON_NODE_HOLDS_OBJECT(child)) policy_reidentify(json_node_get_object(child), depth + 1);
        }
    }
}
/* Assignment and policy identities are unique per calendar: copies get new ones. */
static void programme_copy_content(JsonObject *target, JsonObject *template)
{
    JsonNode *rotations = json_object_get_member(template, "rotations");
    if (rotations && JSON_NODE_HOLDS_ARRAY(rotations)) {
        JsonNode *copy = deep_copy(rotations); JsonArray *rows = json_node_get_array(copy);
        for (guint i = 0; i < json_array_get_length(rows); ++i) {
            JsonNode *element = json_array_get_element(rows, i); if (!JSON_NODE_HOLDS_OBJECT(element)) continue;
            gchar *id = g_uuid_string_random(); json_object_set_string_member(json_node_get_object(element), "id", id); g_free(id);
        }
        json_object_set_member(target, "rotations", copy);
    }
    if (json_object_has_member(template, "rotation_selection_mode"))
        json_object_set_string_member(target, "rotation_selection_mode", radios_json_string(template, "rotation_selection_mode", "linear"));
    JsonNode *policy = json_object_get_member(template, "probability_policy");
    if (policy && JSON_NODE_HOLDS_OBJECT(policy)) {
        JsonNode *copy = deep_copy(policy); policy_reidentify(json_node_get_object(copy), 0); json_object_set_member(target, "probability_policy", copy);
    }
}
/* Kılavuz §04 adım 1 (contract §1b/§5): the "Live event" side panel of the
 * programme dialog. */
static GtkWidget *live_caption(const gchar *text)
{ GtkWidget *label = radios_ui_label(text, "small"); gtk_label_set_xalign(GTK_LABEL(label), 0); return label; }
/* Kılavuz s.4 adım 5 / s.7: the DJ/Konuşma/Müzik label of a mixer strip
 * ("dj"/"talk"/"music" in dj_mixer) or NULL when the strip has none. */
static gchar *mixer_channel_label(Calendar *view, guint number)
{
    JsonObject *mixer = view->mixer && JSON_NODE_HOLDS_OBJECT(view->mixer) ? json_node_get_object(view->mixer) : NULL;
    JsonNode *strips = mixer ? json_object_get_member(mixer, "channels") : NULL;
    JsonArray *channels = strips && JSON_NODE_HOLDS_ARRAY(strips) ? json_node_get_array(strips) : NULL;
    JsonNode *strip = channels && number >= 1 && number - 1 < json_array_get_length(channels) ? json_array_get_element(channels, number - 1) : NULL;
    const gchar *label = strip && JSON_NODE_HOLDS_OBJECT(strip) ? radios_json_string(json_node_get_object(strip), "label", "") : "";
    return !strcmp(label, "dj") || !strcmp(label, "talk") || !strcmp(label, "music") ? g_strdup(label) : NULL;
}
/* Kullanıcı isteği (U4): the channel tree of the schema and step editors
 * lists the strips the station defined — the DJ page's channels and every
 * strip with a label, a source or its own name. NULL until the mixer
 * documents arrive, so the editor keeps offering every strip meanwhile. */
static JsonArray *defined_channels(Calendar *view)
{
    if (!view->mixer && !view->channel_names) return NULL;
    JsonObject *channel_config = view->channel_names && JSON_NODE_HOLDS_OBJECT(view->channel_names) ? json_node_get_object(view->channel_names) : NULL;
    JsonNode *names_node = channel_config ? json_object_get_member(channel_config, "names") : NULL;
    JsonObject *names = names_node && JSON_NODE_HOLDS_OBJECT(names_node) ? json_node_get_object(names_node) : NULL;
    JsonNode *slots_node = channel_config ? json_object_get_member(channel_config, "slots") : NULL;
    JsonObject *mixer = view->mixer && JSON_NODE_HOLDS_OBJECT(view->mixer) ? json_node_get_object(view->mixer) : NULL;
    JsonNode *strips_node = mixer ? json_object_get_member(mixer, "channels") : NULL;
    JsonArray *strips = strips_node && JSON_NODE_HOLDS_ARRAY(strips_node) ? json_node_get_array(strips_node) : NULL;
    guint32 shown = 0;
    if (slots_node && JSON_NODE_HOLDS_ARRAY(slots_node)) {
        JsonArray *slots = json_node_get_array(slots_node);
        for (guint i = 0; i < json_array_get_length(slots); ++i) {
            JsonNode *slot = json_array_get_element(slots, i);
            gint64 index = JSON_NODE_HOLDS_VALUE(slot) && json_node_get_value_type(slot) == G_TYPE_INT64 ? json_node_get_int(slot) : -1;
            if (index >= 0 && index < 32) shown |= 1u << index;
        }
    } else {
        /* Without its document the DJ page shows its eight default strips. */
        gint64 count = channel_config ? CLAMP(radios_json_int(channel_config, "deck_count", 8), 1, 32) : 8;
        shown = count >= 32 ? G_MAXUINT32 : (1u << count) - 1;
    }
    JsonArray *channels = json_array_new();
    for (guint number = 1; number <= 32; ++number) {
        gchar key[8]; g_snprintf(key, sizeof key, "%u", number);
        gchar *fallback = g_strdup_printf("Kanal %u", number);
        const gchar *name = radios_json_string(names, key, "");
        gchar *label = mixer_channel_label(view, number);
        const gchar *audio = label ? NULL : radios_audio_ui_get_channel_label(view->ctx, number);
        if (audio && (!strcmp(audio, "dj") || !strcmp(audio, "talk") || !strcmp(audio, "music"))) label = g_strdup(audio);
        JsonNode *strip_node = strips && number - 1 < json_array_get_length(strips) ? json_array_get_element(strips, number - 1) : NULL;
        JsonObject *strip = strip_node && JSON_NODE_HOLDS_OBJECT(strip_node) ? json_node_get_object(strip_node) : NULL;
        gint64 deck = radios_json_int(strip, "source_deck", -1);
        JsonNode *input_node = strip ? json_object_get_member(strip, "input") : NULL;
        const gchar *input = input_node && JSON_NODE_HOLDS_OBJECT(input_node) ? radios_json_string(json_node_get_object(input_node), "name", "") : "";
        gchar *source = deck == -2 ? g_strdup("Stüdyo mikrofonu") : deck >= 0 && deck < 32 ? g_strdup_printf("Oynatıcı %" G_GINT64_FORMAT, deck + 1) : g_strdup(input);
        if ((shown & (1u << (number - 1))) || label || *source || (*name && g_strcmp0(name, fallback))) {
            JsonObject *row = json_object_new();
            json_object_set_int_member(row, "number", number); json_object_set_string_member(row, "name", *name ? name : fallback);
            json_object_set_string_member(row, "label", label ? label : ""); json_object_set_string_member(row, "source", source);
            json_array_add_object_element(channels, row);
        }
        g_free(fallback); g_free(label); g_free(source);
    }
    return channels;
}
/* Every strip while the mixer documents are still on their way. */
static JsonArray *live_channel_rows(Calendar *view)
{
    JsonArray *channels = defined_channels(view); if (channels) return channels;
    channels = json_array_new();
    for (guint number = 1; number <= 32; ++number) {
        JsonObject *row = json_object_new(); gchar *name = g_strdup_printf("Kanal %u", number);
        json_object_set_int_member(row, "number", number); json_object_set_string_member(row, "name", name);
        json_object_set_string_member(row, "label", ""); json_object_set_string_member(row, "source", "");
        json_array_add_object_element(channels, row); g_free(name);
    }
    return channels;
}
static void library_channels(Calendar *view)
{
    GtkWidget *editor = view->library_dialog ? g_object_get_data(G_OBJECT(view->library_dialog), "rotation-editor") : NULL;
    if (!editor) return;
    JsonArray *channels = defined_channels(view); radios_rotation_editor_set_channels(editor, channels);
    if (channels) json_array_unref(channels);
}
/* The chosen strips' label decides what the event's L does: when every
 * labelled strip among the chosen ones carries the same label, the event's
 * label follows it; unlabelled or mixed choices leave the operator's label. */
static void live_channels_changed(GtkWidget *picker, gpointer data)
{
    GtkWidget *dialog = data, *combo = g_object_get_data(G_OBJECT(dialog), "live-label");
    if (!combo || g_object_get_data(G_OBJECT(dialog), "live-loading")) return;
    const gchar *agreed = radios_ui_channel_picker_agreed_label(picker);
    if (agreed) gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), agreed);
}
static void live_end_changed(GtkComboBox *combo, gpointer data)
{ gtk_widget_set_sensitive(GTK_WIDGET(data), !g_strcmp0(gtk_combo_box_get_active_id(combo), "duration")); }
static void live_toggled(GtkToggleButton *button, gpointer data)
{
    GtkWidget *dialog = data, *panel = g_object_get_data(G_OBJECT(dialog), "live-panel");
    gboolean active = gtk_toggle_button_get_active(button);
    gtk_widget_set_visible(panel, active);
    /* The live settings open beside the programme fields: widen, then narrow. */
    gtk_window_resize(GTK_WINDOW(dialog), active ? 960 : 480, MAX(1, gtk_widget_get_allocated_height(dialog)));
}
static GtkWidget *live_panel_new(Calendar *view, GtkWidget *dialog)
{
    GtkWidget *panel = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "ac-live-panel");
    gtk_widget_set_name(panel, "automation-calendar-event-live-panel");
    gtk_box_pack_start(GTK_BOX(panel), radios_ui_label("Canlı yayın", "ac-section-title"), FALSE, FALSE, 0);
    const gchar *label_ids[] = {"dj", "talk", "music"}, *label_names[] = {"DJ", "Konuşma", "Müzik"};
    GtkWidget *label = gtk_combo_box_text_new(); gtk_widget_set_name(label, "automation-calendar-event-live-label");
    for (guint i = 0; i < G_N_ELEMENTS(label_ids); ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(label), label_ids[i], label_names[i]);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(label), "talk");
    gtk_box_pack_start(GTK_BOX(panel), live_caption("Etiket"), FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(panel), label, FALSE, FALSE, 0);
    /* Kullanıcı isteği U4: the defined strips in a folding tree by label. */
    GtkWidget *channels = radios_ui_channel_picker_new("automation-calendar-event-live-channels");
    JsonArray *strips = live_channel_rows(view); radios_ui_channel_picker_set_channels(channels, strips); json_array_unref(strips);
    radios_ui_channel_picker_set_changed(channels, live_channels_changed, dialog);
    if (view->live_channels) g_object_remove_weak_pointer(G_OBJECT(view->live_channels), (gpointer *)&view->live_channels);
    view->live_channels = channels; g_object_add_weak_pointer(G_OBJECT(channels), (gpointer *)&view->live_channels);
    gtk_box_pack_start(GTK_BOX(panel), live_caption("Yayına verilecek mixer kanalları"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panel), channels, FALSE, FALSE, 0);
    GtkWidget *schema = gtk_combo_box_text_new(); gtk_widget_set_name(schema, "automation-calendar-event-live-schema");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(schema), "", "Şema yok");
    JsonArray *library = library_array(view, FALSE);
    for (guint i = 0; library && i < json_array_get_length(library); ++i) {
        JsonObject *definition = json_array_get_object_element(library, i);
        if (definition_schema(definition))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(schema), radios_json_string(definition, "id", ""), radios_json_string(definition, "name", "Şema"));
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(schema), 0);
    gtk_box_pack_start(GTK_BOX(panel), live_caption("Live event şeması"), FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(panel), schema, FALSE, FALSE, 0);
    GtkWidget *end = gtk_combo_box_text_new(); gtk_widget_set_name(end, "automation-calendar-event-live-end");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(end), "duration", "Süre ile"); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(end), "manual", "Elle");
    gtk_combo_box_set_active(GTK_COMBO_BOX(end), 0);
    GtkWidget *duration = gtk_spin_button_new_with_range(1, 1440, 1); gtk_widget_set_name(duration, "automation-calendar-event-live-duration");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(duration), 30);
    GtkWidget *end_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    gtk_box_pack_start(GTK_BOX(end_row), end, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(end_row), live_caption("Süre (dk)"), FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(end_row), duration, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panel), live_caption("Bitiş kuralı"), FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(panel), end_row, FALSE, FALSE, 0);
    g_signal_connect(end, "changed", G_CALLBACK(live_end_changed), duration);
    g_object_set_data(G_OBJECT(dialog), "live-panel", panel); g_object_set_data(G_OBJECT(dialog), "live-label", label);
    g_object_set_data(G_OBJECT(dialog), "live-channels", channels); g_object_set_data(G_OBJECT(dialog), "live-schema", schema);
    g_object_set_data(G_OBJECT(dialog), "live-end", end); g_object_set_data(G_OBJECT(dialog), "live-duration", duration);
    return panel;
}
static void live_panel_load(GtkWidget *dialog, JsonObject *live)
{
    GtkWidget *label = g_object_get_data(G_OBJECT(dialog), "live-label"), *schema = g_object_get_data(G_OBJECT(dialog), "live-schema");
    GtkWidget *end = g_object_get_data(G_OBJECT(dialog), "live-end"), *duration = g_object_get_data(G_OBJECT(dialog), "live-duration");
    GtkWidget *channels = g_object_get_data(G_OBJECT(dialog), "live-channels");
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(label), radios_json_string(live, "label", "talk"))) gtk_combo_box_set_active_id(GTK_COMBO_BOX(label), "talk");
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(schema), radios_json_string(live, "automation_id", ""))) gtk_combo_box_set_active(GTK_COMBO_BOX(schema), 0);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(end), radios_json_string(live, "end_rule", "duration"))) gtk_combo_box_set_active(GTK_COMBO_BOX(end), 0);
    gint64 milliseconds = radios_json_int(live, "duration_ms", 0);
    if (milliseconds > 0) gtk_spin_button_set_value(GTK_SPIN_BUTTON(duration), (gdouble)MAX((gint64)1, milliseconds / 60000));
    JsonNode *chosen_node = json_object_get_member(live, "channels");
    JsonArray *chosen = chosen_node && JSON_NODE_HOLDS_ARRAY(chosen_node) ? json_node_get_array(chosen_node) : NULL;
    /* The saved label stays as saved: only the operator's own choice of
     * strips moves it. */
    g_object_set_data(G_OBJECT(dialog), "live-loading", GINT_TO_POINTER(1));
    radios_ui_channel_picker_set_selected(channels, chosen);
    g_object_set_data(G_OBJECT(dialog), "live-loading", NULL);
}
/* Owned live object (contract §1b) when the box is checked; NULL otherwise. */
static gboolean live_settings(GtkWidget *dialog, JsonObject **live, GError **error)
{
    *live = NULL; GtkWidget *check = g_object_get_data(G_OBJECT(dialog), "live");
    if (!check || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check))) return TRUE;
    GtkWidget *label = g_object_get_data(G_OBJECT(dialog), "live-label"), *schema = g_object_get_data(G_OBJECT(dialog), "live-schema");
    GtkWidget *end = g_object_get_data(G_OBJECT(dialog), "live-end"), *duration = g_object_get_data(G_OBJECT(dialog), "live-duration");
    GtkWidget *channels = g_object_get_data(G_OBJECT(dialog), "live-channels");
    const gchar *rule = gtk_combo_box_get_active_id(GTK_COMBO_BOX(end)); if (!rule) rule = "manual";
    gtk_spin_button_update(GTK_SPIN_BUTTON(duration)); gint64 minutes = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(duration));
    if (!strcmp(rule, "duration") && minutes <= 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Süre ile bitiş için dakika girin."); return FALSE;
    }
    JsonObject *object = json_object_new(); const gchar *tag = gtk_combo_box_get_active_id(GTK_COMBO_BOX(label));
    json_object_set_string_member(object, "label", tag ? tag : "talk");
    json_object_set_array_member(object, "channels", radios_ui_channel_picker_selected(channels));
    const gchar *automation = gtk_combo_box_get_active_id(GTK_COMBO_BOX(schema));
    json_object_set_string_member(object, "automation_id", automation && g_uuid_string_is_valid(automation) ? automation : "");
    json_object_set_string_member(object, "end_rule", rule);
    json_object_set_int_member(object, "duration_ms", !strcmp(rule, "duration") ? minutes * 60000 : 0);
    *live = object; return TRUE;
}
/* The chosen live event schema is assigned to the programme (its list then
 * shows schemas only); an existing assignment is kept once. */
static void live_assign_schema(Calendar *view, JsonObject *event, JsonObject *live)
{
    const gchar *id = radios_json_string(live, "automation_id", "");
    if (!g_uuid_string_is_valid(id)) return;
    JsonArray *rows = json_object_get_array_member(event, "rotations");
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i)
        if (!g_strcmp0(radios_json_string(json_array_get_object_element(rows, i), "rotation_id", ""), id)) return;
    library_add_assignment(view, event, id);
}
static void programme_name_chosen(GtkComboBox *combo, gpointer data)
{
    GtkWidget *dialog = data; if (gtk_combo_box_get_active(combo) < 0) return;
    Calendar *view = g_object_get_data(G_OBJECT(dialog), "calendar");
    gchar *title = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo)); JsonObject *template = view ? programme_template(view, title) : NULL;
    if (template) gtk_combo_box_set_active(GTK_COMBO_BOX(g_object_get_data(G_OBJECT(dialog), "color")),
        g_str_equal(radios_json_string(template, "color", ""), "#abfe33") ? 1 : 0);
    /* Picking an existing live programme also brings its live settings. */
    JsonObject *live = event_live(template);
    if (live) { live_panel_load(dialog, live); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_object_get_data(G_OBJECT(dialog), "live")), TRUE); }
    g_free(title);
}
static void programme_response(GtkDialog *dialog, gint response, gpointer data)
{
    Calendar *view = data;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    if (calendar_active_document(view) && !radios_data_document_ready(calendar_active_document(view))) {
        notice(view, "Takvim yükleniyor; kayıt hazır olduğunda düzenleyebilirsiniz."); return;
    }
    GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "name"), *color = g_object_get_data(G_OBJECT(dialog), "color");
    gchar *title = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))); g_strstrip(title);
    if (!*title) { notice(view, "Program adı gerekli."); g_free(title); return; }
    /* Kılavuz §04 adım 1: a checked "Live event" box persists events[].live. */
    JsonObject *live = NULL; GError *invalid = NULL;
    if (!live_settings(GTK_WIDGET(dialog), &live, &invalid)) { notice(view, invalid->message); g_clear_error(&invalid); g_free(title); return; }
    const gchar *paint = gtk_combo_box_get_active(GTK_COMBO_BOX(color)) == 1 ? "#abfe33" : "#abd6e4";
    const gchar *edit_id = g_object_get_data(G_OBJECT(dialog), "edit-id");
    JsonObject *current = edit_id ? event_by_id(view, edit_id) : NULL;
    if (edit_id && !current) { notice(view, "Program değişti veya kaldırıldı; yeniden seçin."); g_free(title); g_clear_pointer(&live, json_object_unref); return; }
    if (current) {
        json_object_set_string_member(current, "title", title); json_object_set_string_member(current, "color", paint);
        if (live) { json_object_set_object_member(current, "live", json_object_ref(live)); live_assign_schema(view, current, live); }
        else if (json_object_has_member(current, "live")) json_object_remove_member(current, "live");
    } else {
        guint first = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(dialog), "day-first"));
        guint last = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(dialog), "day-last"));
        guint start = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(dialog), "start"));
        guint end = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(dialog), "end"));
        const gchar *week = g_object_get_data(G_OBJECT(dialog), "week");
        GDateTime *base = parse_date(week); JsonArray *array = events(view);
        if (json_array_get_length(array) + last - first + 1 > 500) {
            notice(view, "Takvim en fazla 500 program içerebilir."); g_date_time_unref(base); g_free(title); g_clear_pointer(&live, json_object_unref); return;
        }
        for (guint day = first; day <= last; ++day) {
            GDateTime *date = g_date_time_add_days(base, day); gchar *stamp = g_date_time_format(date, "%Y-%m-%d"); g_date_time_unref(date);
            gboolean conflict = overlaps(array, stamp, start, end, NULL); g_free(stamp);
            if (conflict) {
                notice(view, "Seçim mevcut bir programla çakışıyor; boş bir aralık seçin."); g_date_time_unref(base); g_free(title);
                g_clear_pointer(&live, json_object_unref); return;
            }
        }
        g_hash_table_remove_all(view->selected);
        JsonObject *template = programme_template(view, title);
        for (guint day = first; day <= last; ++day) {
            GDateTime *date = g_date_time_add_days(base, day); gchar *stamp = g_date_time_format(date, "%Y-%m-%d"); g_date_time_unref(date);
            JsonObject *event = new_event(title, stamp, start, end, paint); g_free(stamp);
            if (template) programme_copy_content(event, template);
            if (live) { json_object_set_object_member(event, "live", json_object_ref(live)); live_assign_schema(view, event, live); }
            g_free(view->current); view->current = g_strdup(radios_json_string(event, "id", ""));
            g_hash_table_add(view->selected, g_strdup(view->current)); json_array_add_object_element(array, event);
        }
        g_date_time_unref(base);
    }
    g_clear_pointer(&live, json_object_unref);
    g_free(title); gtk_widget_destroy(GTK_WIDGET(dialog)); show_programme(view); commit(view);
}
static void style_calendar_dialog(GtkWidget *dialog)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "task-dialog");
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_style_context_add_class(gtk_widget_get_style_context(content), "automation-calendar");
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    if (accept) {
        gtk_style_context_add_class(gtk_widget_get_style_context(accept), "task-primary");
        GtkWidget *actions = gtk_widget_get_parent(accept);
        if (actions) gtk_style_context_add_class(gtk_widget_get_style_context(actions), "automation-calendar");
    }
    gtk_window_set_default_size(GTK_WINDOW(dialog), 480, -1);
}
static void programme_dialog(GtkButton *button, gpointer data)
{
    Calendar *view = data; gboolean edit = button == NULL || g_object_get_data(G_OBJECT(button), "edit") != NULL;
    if (calendar_active_document(view) && !radios_data_document_ready(calendar_active_document(view))) {
        notice(view, "Takvim yükleniyor; kayıt hazır olduğunda düzenleyebilirsiniz."); return;
    }
    JsonObject *current = edit ? event_by_id(view, view->current) : NULL;
    if ((edit && !current) || (!edit && !view->has_selection)) return;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(edit ? "Program özellikleri" : "Program oluştur", view->ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, "Vazgeç", GTK_RESPONSE_CANCEL, edit ? "Uygula" : "Oluştur", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(dialog, "automation-calendar-event-dialog"); style_calendar_dialog(dialog);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    /* Kılavuz §04 adım 1: programme fields on the left; the live settings open beside them. */
    GtkWidget *columns = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 16, "ac-event-columns"), *left = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, NULL);
    GtkWidget *programme = gtk_combo_box_text_new_with_entry(); gtk_widget_set_name(programme, "automation-calendar-event-programme");
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(programme)); gtk_widget_set_name(entry, "automation-calendar-event-name"); gtk_entry_set_max_length(GTK_ENTRY(entry), 160);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Listeden program seçin ya da yeni program adı yazın");
    GPtrArray *names = programme_names(view);
    for (guint i = 0; i < names->len; ++i) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(programme), g_ptr_array_index(names, i));
    g_ptr_array_unref(names);
    gtk_entry_set_text(GTK_ENTRY(entry), current ? radios_json_string(current, "title", "") : "");
    const gchar *colors[] = { "Açık mavi", "Yeşil", NULL }; GtkWidget *color = radios_ui_combo(colors, current && g_str_equal(radios_json_string(current, "color", ""), "#abfe33") ? 1 : 0);
    gtk_widget_set_name(color, "automation-calendar-event-color");
    gtk_box_pack_start(GTK_BOX(left), radios_ui_label("Program adı", NULL), FALSE, FALSE, 4); gtk_box_pack_start(GTK_BOX(left), programme, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(left), color, FALSE, FALSE, 4);
    GtkWidget *live_check = gtk_check_button_new_with_label("Live event"); gtk_widget_set_name(live_check, "automation-calendar-event-live");
    gtk_box_pack_start(GTK_BOX(left), live_check, FALSE, FALSE, 4);
    GtkWidget *panel = live_panel_new(view, dialog);
    gtk_box_pack_start(GTK_BOX(columns), left, TRUE, TRUE, 0); gtk_box_pack_start(GTK_BOX(columns), panel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), columns, TRUE, TRUE, 0);
    gtk_widget_show_all(panel); gtk_widget_set_no_show_all(panel, TRUE); gtk_widget_hide(panel);
    g_object_set_data(G_OBJECT(dialog), "name", entry); g_object_set_data(G_OBJECT(dialog), "color", color); g_object_set_data(G_OBJECT(dialog), "calendar", view);
    g_object_set_data(G_OBJECT(dialog), "live", live_check);
    g_signal_connect(programme, "changed", G_CALLBACK(programme_name_chosen), dialog);
    g_signal_connect(live_check, "toggled", G_CALLBACK(live_toggled), dialog);
    if (edit) g_object_set_data_full(G_OBJECT(dialog), "edit-id", g_strdup(view->current), g_free);
    g_object_set_data(G_OBJECT(dialog), "day-first", GUINT_TO_POINTER(view->day_first)); g_object_set_data(G_OBJECT(dialog), "day-last", GUINT_TO_POINTER(view->day_last));
    g_object_set_data(G_OBJECT(dialog), "start", GUINT_TO_POINTER(view->start)); g_object_set_data(G_OBJECT(dialog), "end", GUINT_TO_POINTER(view->end));
    g_object_set_data_full(G_OBJECT(dialog), "week", date_at(view, 0), g_free);
    /* Owner destruction disconnects the response before the view is freed. */
    g_signal_connect_object(view->page, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    g_signal_connect(dialog, "response", G_CALLBACK(programme_response), view); gtk_widget_show_all(dialog);
    /* "Program özellikleri" reopens the same panel with the saved values. */
    JsonObject *live = event_live(current);
    if (live) { live_panel_load(dialog, live); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(live_check), TRUE); }
    if (view->ctx->data) load_mixer(view);
}
static gboolean parse_time(const gchar *text, gboolean allow_end, guint *minute)
{
    if (!g_regex_match_simple("^[0-9]{2}:[0-9]{2}$", text, 0, 0)) return FALSE;
    guint hour = (guint)((text[0] - '0') * 10 + text[1] - '0');
    guint min = (guint)((text[3] - '0') * 10 + text[4] - '0');
    if (min > 59 || hour > 24 || (hour == 24 && (!allow_end || min))) return FALSE;
    *minute = hour * 60 + min; return TRUE;
}
static void rotation_mode_changed(GtkComboBox *combo, gpointer data)
{
    Calendar *view = data; if (view->updating || view->closing || view->disposed) return;
    JsonObject *event = event_by_id(view, view->current); if (!event) return;
    json_object_set_string_member(event, "rotation_selection_mode",
        gtk_combo_box_get_active(combo) == 1 ? "weighted" : "linear");
    commit(view);
}
static void rotation_weight_cell(GtkTreeViewColumn *column, GtkCellRenderer *cell,
    GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)column; (void)data; gint weight = -1;
    gtk_tree_model_get(model, iter, 7, &weight, -1);
    gchar *text = weight < 0 ? g_strdup("—") : g_strdup_printf("%d", weight);
    g_object_set(cell, "text", text, "editable", weight >= 0, NULL); g_free(text);
}
static void rotation_weight_edited(GtkCellRendererText *cell, const gchar *path, const gchar *text, gpointer data)
{
    (void)cell; Calendar *view = data;
    if (view->updating || view->closing || view->disposed || !*text || strlen(text) > 3) return;
    for (const gchar *c = text; *c; ++c) if (!g_ascii_isdigit(*c)) { notice(view, "Rotasyon ağırlığı 0–100 arasında tam sayı olmalı."); return; }
    gint weight = (gint)g_ascii_strtoll(text, NULL, 10);
    if (weight > 100) { notice(view, "Rotasyon ağırlığı 0–100 arasında olmalı."); return; }
    GtkTreeIter iter; if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(view->store), &iter, path)) return;
    gchar *id = NULL; gtk_tree_model_get(GTK_TREE_MODEL(view->store), &iter, 0, &id, -1);
    JsonObject *event = event_by_id(view, view->current);
    JsonArray *rows = event ? json_object_get_array_member(event, "rotations") : NULL;
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (!g_strcmp0(radios_json_string(row, "id", ""), id) && json_object_has_member(row, "rotation_id")) {
            json_object_set_int_member(row, "weight", weight); commit(view); break;
        }
    }
    g_free(id);
}
static void inspector_changed(GtkWidget *widget, gpointer data)
{
    (void)widget; Calendar *view = data; if (view->updating) return;
    JsonObject *event = event_by_id(view, view->current); if (!event) return;
    const gchar *title = gtk_entry_get_text(GTK_ENTRY(view->name));
    gchar *trimmed = g_strdup(title); g_strstrip(trimmed);
    const gchar *date = gtk_entry_get_text(GTK_ENTRY(view->programme_date));
    GDateTime *stamp = parse_date(date); guint first = 0, last = 0;
    gboolean valid = *trimmed && stamp &&
        parse_time(gtk_entry_get_text(GTK_ENTRY(view->programme_start)), FALSE, &first) &&
        parse_time(gtk_entry_get_text(GTK_ENTRY(view->programme_end)), TRUE, &last) && first < last;
    g_clear_pointer(&stamp, g_date_time_unref);
    if (!valid) {
        view->inspector_invalid = TRUE;
        g_free(trimmed); gtk_label_set_text(GTK_LABEL(view->save_status), "Alanları tamamlayın");
        notice(view, "Ad, tarih (YYYY-AA-GG) ve saatleri (SS:DD) tamamlayın; bitiş başlangıçtan sonra olmalı."); return;
    }
    if (overlaps(events(view), date, first, last, view->current)) {
        view->inspector_invalid = TRUE;
        g_free(trimmed); gtk_label_set_text(GTK_LABEL(view->save_status), "Saatler çakışıyor");
        notice(view, "Tarih ve saat başka bir blokla çakışıyor; kayıt değiştirilmedi."); return;
    }
    view->inspector_invalid = FALSE;
    json_object_set_string_member(event, "title", trimmed); g_free(trimmed);
    json_object_set_string_member(event, "date", date);
    json_object_set_int_member(event, "start_minute", first); json_object_set_int_member(event, "end_minute", last);
    json_object_set_string_member(event, "color", gtk_combo_box_get_active(GTK_COMBO_BOX(view->color)) == 1 ? "#abfe33" : "#abd6e4");
    view->start = first; view->end = last; gint day = event_day(view, event);
    if (day < 0) {
        GDateTime *event_date = parse_date(date);
        GDateTime *monday = event_date ? g_date_time_add_days(event_date, 1 - g_date_time_get_day_of_week(event_date)) : NULL;
        if (monday) {
            g_clear_pointer(&view->week, g_date_time_unref); view->week = monday;
            gchar *week = g_date_time_format(monday, "%Y-%m-%d"); gtk_entry_set_text(GTK_ENTRY(view->date), week); g_free(week);
            GDateTime *sunday = g_date_time_add_days(monday, 6);
            gchar *a = g_date_time_format(monday, "%d.%m.%Y"); gchar *b = sunday ? g_date_time_format(sunday, "%d.%m.%Y") : g_strdup("—");
            gchar *range = g_strdup_printf("%s – %s", a, b); gtk_label_set_text(GTK_LABEL(view->week_range), range);
            g_free(a); g_free(b); g_free(range); g_clear_pointer(&sunday, g_date_time_unref);
            day = event_day(view, event);
            g_hash_table_remove_all(view->selected); g_hash_table_add(view->selected, g_strdup(view->current));
            gtk_widget_queue_draw(view->header);
        }
        g_clear_pointer(&event_date, g_date_time_unref);
    }
    if (day >= 0) view->day_first = view->day_last = (guint)day;
    changed(view); update_detail_heading(view); gtk_widget_queue_draw(view->grid);
    if (view->autosave_source) g_source_remove(view->autosave_source);
    view->autosave_source = g_timeout_add(250, autosave_tick, view);
}
static void remove_programme(GtkButton *button, gpointer data)
{
    (void)button; Calendar *view = data; if (!g_hash_table_size(view->selected)) return;
    JsonArray *array = events(view);
    for (gint i = (gint)json_array_get_length(array) - 1; i >= 0; --i)
        if (g_hash_table_contains(view->selected, radios_json_string(json_array_get_object_element(array, (guint)i), "id", "")))
            json_array_remove_element(array, (guint)i);
    g_hash_table_remove_all(view->selected); g_clear_pointer(&view->current, g_free); commit(view);
}
static void new_ids(JsonObject *event)
{
    gchar *id = g_uuid_string_random(); json_object_set_string_member(event, "id", id); g_free(id);
    JsonArray *array = json_object_get_array_member(event, "rotations");
    for (guint i = 0; i < json_array_get_length(array); ++i) {
        id = g_uuid_string_random(); json_object_set_string_member(json_array_get_object_element(array, i), "id", id); g_free(id);
    }
}
static void copy_blocks(GtkButton *button, gpointer data)
{
    (void)button; Calendar *view = data; JsonArray *copy = json_array_new(), *array = events(view);
    for (guint i = 0; i < json_array_get_length(array); ++i) {
        JsonObject *event = json_array_get_object_element(array, i);
        if (g_hash_table_contains(view->selected, radios_json_string(event, "id", "")))
            json_array_add_element(copy, deep_copy(json_array_get_element(array, i)));
    }
    g_clear_pointer(&view->clipboard, json_node_free); view->clipboard = json_node_new(JSON_NODE_ARRAY); json_node_take_array(view->clipboard, copy);
    refresh(view); notice(view, "Seçili bloklar kopyalandı. Hedef hücreye sağ tıklayıp Yapıştır seçin.");
}
static gint64 absolute_minute(const gchar *date, guint minute)
{
    GDateTime *stamp = parse_date(date); if (!stamp) return -1;
    gint64 result = g_date_time_to_unix(stamp) / 60 + minute; g_date_time_unref(stamp); return result;
}
static void paste_blocks(GtkButton *button, gpointer data)
{
    (void)button; Calendar *view = data; if (!view->clipboard || !view->has_selection) return;
    JsonArray *source = json_node_get_array(view->clipboard), *array = events(view);
    if (!json_array_get_length(source)) return;
    if (json_array_get_length(array) + json_array_get_length(source) > 500) { notice(view, "Takvim en fazla 500 program içerebilir."); return; }
    gint64 base = G_MAXINT64;
    for (guint i = 0; i < json_array_get_length(source); ++i) {
        JsonObject *event = json_array_get_object_element(source, i);
        base = MIN(base, absolute_minute(radios_json_string(event, "date", ""), (guint)radios_json_int(event, "start_minute", 0)));
    }
    gchar *target_date = date_at(view, view->day_first); gint64 target = absolute_minute(target_date, view->start); g_free(target_date);
    JsonArray *pending = json_array_new();
    for (guint i = 0; i < json_array_get_length(source); ++i) {
        JsonNode *copy = deep_copy(json_array_get_element(source, i)); JsonObject *event = json_node_get_object(copy);
        guint old_start = (guint)radios_json_int(event, "start_minute", 0), duration = (guint)radios_json_int(event, "end_minute", 0) - old_start;
        gint64 moment = absolute_minute(radios_json_string(event, "date", ""), old_start) - base + target;
        GDateTime *stamp = g_date_time_new_from_unix_utc(moment * 60);
        if (!stamp) { json_node_free(copy); json_array_unref(pending); notice(view, "Hedef tarih desteklenen aralığın dışında."); return; }
        gchar *date = g_date_time_format(stamp, "%Y-%m-%d"); guint start = (guint)(g_date_time_get_hour(stamp) * 60 + g_date_time_get_minute(stamp)); g_date_time_unref(stamp);
        if (start + duration > 1440 || overlaps(array, date, start, start + duration, NULL) || overlaps(pending, date, start, start + duration, NULL)) {
            g_free(date); json_node_free(copy); json_array_unref(pending); notice(view, "Yapıştırma çakışıyor veya gece yarısını aşıyor; hiçbir event eklenmedi."); return;
        }
        json_object_set_string_member(event, "date", date); g_free(date); json_object_set_int_member(event, "start_minute", start);
        json_object_set_int_member(event, "end_minute", start + duration); new_ids(event); json_array_add_element(pending, copy);
    }
    g_hash_table_remove_all(view->selected);
    for (guint i = 0; i < json_array_get_length(pending); ++i) {
        JsonObject *event = json_array_get_object_element(pending, i);
        g_free(view->current); view->current = g_strdup(radios_json_string(event, "id", "")); g_hash_table_add(view->selected, g_strdup(view->current));
        json_array_add_element(array, deep_copy(json_array_get_element(pending, i)));
    }
    json_array_unref(pending); commit(view);
}
static gboolean move_blocks(Calendar *view, gint day_delta, gint minute_delta)
{
    if ((!day_delta && !minute_delta) || !g_hash_table_size(view->selected)) return FALSE;
    JsonArray *array = events(view), *others = json_array_new(), *pending = json_array_new();
    for (guint i = 0; i < json_array_get_length(array); ++i) {
        JsonObject *event = json_array_get_object_element(array, i);
        if (!g_hash_table_contains(view->selected, radios_json_string(event, "id", "")))
            json_array_add_element(others, deep_copy(json_array_get_element(array, i)));
    }
    gboolean valid = TRUE;
    for (guint i = 0; i < json_array_get_length(array) && valid; ++i) {
        JsonObject *event = json_array_get_object_element(array, i);
        if (!g_hash_table_contains(view->selected, radios_json_string(event, "id", ""))) continue;
        gint first = (gint)radios_json_int(event, "start_minute", 0) + minute_delta;
        gint last = (gint)radios_json_int(event, "end_minute", 0) + minute_delta;
        GDateTime *original = parse_date(radios_json_string(event, "date", ""));
        GDateTime *target = original ? g_date_time_add_days(original, day_delta) : NULL;
        g_clear_pointer(&original, g_date_time_unref);
        if (!target || first < 0 || last > 1440 || first >= last) { g_clear_pointer(&target, g_date_time_unref); valid = FALSE; break; }
        gchar *date = g_date_time_format(target, "%Y-%m-%d"); g_date_time_unref(target);
        if (overlaps(others, date, (guint)first, (guint)last, NULL) || overlaps(pending, date, (guint)first, (guint)last, NULL)) valid = FALSE;
        if (valid) {
            JsonNode *copy = deep_copy(json_array_get_element(array, i)); JsonObject *moved = json_node_get_object(copy);
            json_object_set_string_member(moved, "date", date); json_object_set_int_member(moved, "start_minute", first);
            json_object_set_int_member(moved, "end_minute", last); json_array_add_element(pending, copy);
        }
        g_free(date);
    }
    if (valid) {
        for (guint i = 0; i < json_array_get_length(pending); ++i) {
            JsonObject *next = json_array_get_object_element(pending, i);
            JsonObject *event = event_by_id(view, radios_json_string(next, "id", ""));
            json_object_set_string_member(event, "date", radios_json_string(next, "date", ""));
            json_object_set_int_member(event, "start_minute", radios_json_int(next, "start_minute", 0));
            json_object_set_int_member(event, "end_minute", radios_json_int(next, "end_minute", 0));
        }
        JsonObject *current = event_by_id(view, view->current);
        if (current) {
            gint day = event_day(view, current);
            if (day >= 0) view->day_first = view->day_last = (guint)day;
            view->start = (guint)radios_json_int(current, "start_minute", 0); view->end = (guint)radios_json_int(current, "end_minute", 0);
        }
        commit(view);
    } else notice(view, "Taşıma başka bir blokla çakışıyor veya gün sınırını aşıyor; hiçbir blok taşınmadı.");
    json_array_unref(others); json_array_unref(pending); return valid;
}
static GtkWidget *menu_item(GtkWidget *menu, const gchar *label, const gchar *name, GCallback callback, Calendar *view)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label); gtk_widget_set_name(item, name);
    if (callback) g_signal_connect(item, "activate", callback, view);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item); return item;
}
static void reload_calendar(GtkMenuItem *item, gpointer data)
{
    (void)item; Calendar *view = data;
    if (calendar_active_document(view)) radios_data_document_reload(calendar_active_document(view));
}
static void block_popup(Calendar *view, GdkEvent *event)
{
    gtk_widget_set_sensitive(view->copy, g_hash_table_size(view->selected) > 0);
    gtk_widget_set_sensitive(view->paste, view->clipboard && view->has_selection);
    GtkWidget *remove = g_object_get_data(G_OBJECT(view->block_menu), "remove");
    gtk_widget_set_sensitive(remove, g_hash_table_size(view->selected) > 0);
    if (view->properties_item) gtk_widget_set_sensitive(view->properties_item, event_by_id(view, view->current) != NULL);
    gtk_widget_show_all(view->block_menu);
    if (event && event->type == GDK_BUTTON_PRESS) gtk_menu_popup_at_pointer(GTK_MENU(view->block_menu), event);
    else gtk_menu_popup_at_widget(GTK_MENU(view->block_menu), view->grid, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, event);
}
static gboolean key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)widget; Calendar *view = data;
    if (event->keyval == GDK_KEY_Menu || (event->keyval == GDK_KEY_F10 && event->state & GDK_SHIFT_MASK)) { block_popup(view, (GdkEvent *)event); return TRUE; }
    if (event->keyval == GDK_KEY_Delete) { remove_programme(NULL, view); return TRUE; }
    /* Ctrl+Alt is AltGr on Windows keyboards (Turkish Q: AltGr+C/V are not copy/paste). */
    if ((event->state & GDK_CONTROL_MASK) && !(event->state & GDK_MOD1_MASK)) {
        if (event->keyval == GDK_KEY_c || event->keyval == GDK_KEY_C) { copy_blocks(NULL, view); return TRUE; }
        if (event->keyval == GDK_KEY_v || event->keyval == GDK_KEY_V) { paste_blocks(NULL, view); return TRUE; }
    }
    if (event->keyval == GDK_KEY_Return) { programme_dialog(view->save_selection == NULL ? NULL : GTK_BUTTON(view->save_selection), view); return TRUE; }
    if (!view->has_selection) return FALSE;
    gint day = (gint)view->day_first, minute = (gint)view->start;
    if (event->keyval == GDK_KEY_Left) --day; else if (event->keyval == GDK_KEY_Right) ++day;
    else if (event->keyval == GDK_KEY_Up) minute -= (gint)view->step; else if (event->keyval == GDK_KEY_Down) minute += (gint)view->step; else return FALSE;
    day = CLAMP(day, 0, 6); minute = CLAMP(minute, 0, 1440 - (gint)view->step);
    if (event->state & GDK_SHIFT_MASK) {
        g_hash_table_remove_all(view->selected); g_clear_pointer(&view->current, g_free);
        view->day_last = (guint)day; view->day_first = MIN(view->anchor_day, (guint)day); view->day_last = MAX(view->anchor_day, (guint)day);
        view->start = MIN(view->anchor_minute, (guint)minute); view->end = MAX(view->anchor_minute, (guint)minute) + view->step; refresh(view);
    } else { view->anchor_day = (guint)day; view->anchor_minute = (guint)minute; radios_automation_calendar_select(view->page, (guint)day, (guint)minute, (guint)minute + view->step, FALSE); }
    return TRUE;
}

static void playlist_choices(Calendar *view)
{
    if (view->library_dialog) {
        GtkWidget *editor = g_object_get_data(G_OBJECT(view->library_dialog), "rotation-editor");
        JsonArray *playlists = calendar_playlists(view);
        radios_rotation_editor_set_playlists(editor, playlists);
    }
    if (!view->rotation_dialog) return;
    GtkComboBoxText *combo = g_object_get_data(G_OBJECT(view->rotation_dialog), "playlist");
    gchar *current = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)));
    if (!current) current = g_strdup(g_object_get_data(G_OBJECT(view->rotation_dialog), "desired-playlist"));
    gtk_combo_box_text_remove_all(combo);
    JsonArray *array = calendar_playlists(view);
    for (guint i = 0; array && i < json_array_get_length(array); ++i) {
        JsonObject *row = json_array_get_object_element(array, i);
        gtk_combo_box_text_append(combo, radios_json_string(row, "_id", ""), radios_json_string(row, "name", "Playlist"));
    }
    if (current && *current) gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), current);
    g_free(current);
}
static void playlist_request_free(gpointer data)
{
    PlaylistRequest *request = data;
    g_clear_pointer(&request->client, radios_data_client_unref);
    g_free(request->station); g_free(request->endpoint); g_free(request);
}
static void playlists_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    PlaylistRequest *request = data;
    Calendar *view = g_object_get_data(G_OBJECT(owner), "automation-calendar");
    if (!view || view->disposed || view->closing || request->generation != view->playlist_generation) return;
    view->loading = FALSE; g_clear_pointer(&view->playlist_request_client, radios_data_client_unref);
    if (!playlist_identity_matches(view, request->client, request->station, request->endpoint)) {
        playlist_choices(view); refresh(view);
        if (radios_data_connected(ctx)) load_playlists(ctx, owner);
        return;
    }
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { if (error) notice(view, error->message); return; }
    g_clear_pointer(&view->playlists, json_node_free); view->playlists = deep_copy(result);
    g_clear_pointer(&view->playlists_client, radios_data_client_unref);
    view->playlists_client = radios_data_client_ref(request->client);
    g_free(view->playlists_station); view->playlists_station = g_strdup(request->station);
    g_free(view->playlists_endpoint); view->playlists_endpoint = g_strdup(request->endpoint);
    playlist_choices(view); refresh(view);
    if (view->dirty && !view->autosave_source) submit(view);
}
/* Kılavuz §04 adım 1 / contract §5: the live panel lists mixer channels from
 * the dj_mixer document (labels, sources) and the channel document (names).
 * Both are read-only catalogues of this authenticated session. */
typedef struct {
    RadioSDataClient *client;
    gchar *kind;
    guint generation;
} MixerRequest;
static void mixer_request_free(gpointer data)
{
    MixerRequest *request = data;
    g_clear_pointer(&request->client, radios_data_client_unref); g_free(request->kind); g_free(request);
}
static void mixer_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; MixerRequest *request = data; Calendar *view = g_object_get_data(G_OBJECT(owner), "automation-calendar");
    if (!view || view->disposed || view->closing || request->generation != view->mixer_generation ||
        error || !result || !JSON_NODE_HOLDS_OBJECT(result)) return;
    JsonNode *records = json_object_get_member(json_node_get_object(result), "records");
    JsonArray *rows = records && JSON_NODE_HOLDS_ARRAY(records) ? json_node_get_array(records) : NULL; JsonObject *chosen = NULL;
    gboolean channel = g_str_equal(request->kind, "channel");
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonNode *element = json_array_get_element(rows, i); if (!JSON_NODE_HOLDS_OBJECT(element)) continue;
        JsonObject *record = json_node_get_object(element); JsonNode *config = json_object_get_member(record, "config");
        if (g_strcmp0(radios_json_string(record, "kind", ""), request->kind) || g_strcmp0(radios_json_string(record, "status", "active"), "active") ||
            !config || !JSON_NODE_HOLDS_OBJECT(config)) continue;
        JsonObject *candidate = json_node_get_object(config);
        if (channel) {
            /* The DJ page's channel document names the mixer channels; other
             * channel documents name players. */
            if (!json_object_has_member(candidate, "names")) continue;
            if (!chosen || g_str_equal(radios_json_string(record, "name", ""), "DJ kanalları")) chosen = candidate;
        } else if (!chosen) chosen = candidate;
    }
    JsonNode **slot = channel ? &view->channel_names : &view->mixer;
    g_clear_pointer(slot, json_node_free);
    if (chosen) { JsonNode *node = object_node(chosen); *slot = deep_copy(node); json_node_free(node); }
    library_channels(view);
    if (view->live_channels) { JsonArray *list = live_channel_rows(view); radios_ui_channel_picker_set_channels(view->live_channels, list); json_array_unref(list); }
}
static void load_mixer(Calendar *view)
{
    if (!view->ctx->data || view->disposed || view->closing || !radios_data_connected(view->ctx)) return;
    RadioSDataClient *client = radios_data_client_acquire(view->ctx); if (!client) return;
    const gchar *kinds[] = {"dj_mixer", "channel"}; ++view->mixer_generation;
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        MixerRequest *request = g_new0(MixerRequest, 1); request->client = radios_data_client_ref(client);
        request->kind = g_strdup(kinds[i]); request->generation = view->mixer_generation;
        JsonNode *body = radios_data_body(view->ctx); json_object_set_string_member(json_node_get_object(body), "kind", kinds[i]);
        /* A refused request already ran mixer_request_free through destroy. */
        radios_data_request(view->ctx, view->page, "POST", "/v1/workflows/desktop/list", body, mixer_received, request, mixer_request_free);
        json_node_free(body);
    }
    radios_data_client_unref(client);
}
static void load_playlists(RadioSViewContext *ctx, GtkWidget *owner)
{
    Calendar *view = g_object_get_data(G_OBJECT(owner), "automation-calendar");
    if (!view || view->disposed || view->closing) return;
    load_mixer(view);
    if (!view->dirty && !view->detached && !view->bound_editor && !*radios_data_document_id(view->document) && !json_array_get_length(events(view))) {
        const gchar *zone = radios_station_timezone(ctx);
        gboolean moved = g_strcmp0(zone, calendar_timezone(view)) != 0;
        json_object_set_string_member(json_node_get_object(view->config), "timezone", zone);
        timezone_render(view);
        if (moved && !view->has_selection) { GDateTime *today = calendar_now(view); set_week(view, today); g_date_time_unref(today); }
    }
    if (view->dirty && !view->autosave_source && radios_data_document_ready(view->document)) submit(view);
    if (view->playlists && !calendar_playlists(view)) playlist_choices(view);
    RadioSDataClient *client = radios_data_client_acquire(ctx);
    if (!client) return;
    if (view->loading && client == view->playlist_request_client) { radios_data_client_unref(client); return; }
    PlaylistRequest *request = g_new0(PlaylistRequest, 1); request->client = client;
    request->station = g_strdup(radios_data_station_id(ctx)); request->endpoint = g_strdup(radios_data_endpoint_id(ctx));
    request->generation = ++view->playlist_generation;
    g_clear_pointer(&view->playlist_request_client, radios_data_client_unref);
    view->playlist_request_client = radios_data_client_ref(client);
    JsonNode *body = radios_data_body(ctx);
    view->loading = radios_data_request(ctx, owner, "POST", "/v1/workflows/desktop-playlists/list", body,
        playlists_received, request, playlist_request_free);
    json_node_free(body);
    if (!view->loading) g_clear_pointer(&view->playlist_request_client, radios_data_client_unref);
}
/* Definitions belong to the same station-scoped, revision-checked document as
 * their assignments. An assignment keeps the definition identity; it never
 * duplicates a playlist or takes a mutable reference into a saved snapshot. */
static JsonArray *library_array(Calendar *view, gboolean create)
{
    JsonObject *config = json_node_get_object(view->config);
    if (!json_object_has_member(config, "rotation_library")) {
        if (!create) return NULL;
        json_object_set_array_member(config, "rotation_library", json_array_new());
    }
    return json_object_get_array_member(config, "rotation_library");
}
static JsonObject *library_by_id(Calendar *view, const gchar *id)
{
    JsonArray *array = library_array(view, FALSE);
    for (guint i = 0; id && array && i < json_array_get_length(array); ++i) {
        JsonObject *definition = json_array_get_object_element(array, i);
        if (!g_strcmp0(id, radios_json_string(definition, "id", ""))) return definition;
    }
    return NULL;
}
static gboolean definition_schema(JsonObject *definition)
{
    JsonNode *settings = definition ? json_object_get_member(definition, "settings") : NULL;
    JsonNode *flag = settings && JSON_NODE_HOLDS_OBJECT(settings) ? json_object_get_member(json_node_get_object(settings), "automation_schema") : NULL;
    return flag && JSON_NODE_HOLDS_VALUE(flag) && json_node_get_value_type(flag) == G_TYPE_BOOLEAN && json_node_get_boolean(flag);
}
static void library_choices(Calendar *view)
{
    if (!view->library_choice) return;
    gchar *selected = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->library_choice)));
    GHashTable *chosen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GtkTreeSelection *table_selection = view->library_rows ? gtk_tree_view_get_selection(GTK_TREE_VIEW(view->library_rows)) : NULL;
    GList *paths = table_selection ? gtk_tree_selection_get_selected_rows(table_selection, NULL) : NULL;
    for (GList *it = paths; it; it = it->next) {
        GtkTreeIter row; gchar *id = NULL;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(view->library_store), &row, it->data)) {
            gtk_tree_model_get(GTK_TREE_MODEL(view->library_store), &row, 0, &id, -1); if (id) g_hash_table_add(chosen, id);
        }
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    gboolean was_updating = view->updating; view->updating = TRUE;
    if (view->library_store) gtk_list_store_clear(view->library_store);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(view->library_choice));
    JsonArray *array = library_array(view, FALSE); guint choices = 0;
    /* Kılavuz §04 adım 2: a live programme chooses among rotation schemas only. */
    gboolean live_only = event_live(event_by_id(view, view->current)) != NULL;
    for (guint i = 0; array && i < json_array_get_length(array); ++i) {
        JsonObject *definition = json_array_get_object_element(array, i);
        /* Schemas are marked; "Seçim" follows the planner: percentages, weights, then turns. */
        gboolean schema = definition_schema(definition);
        if (live_only && !schema) continue;
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->library_choice), radios_json_string(definition, "id", ""),
            radios_json_string(definition, "name", "Rotasyon")); ++choices;
        if (view->library_store) {
            GtkTreeIter row; JsonObject *settings = json_object_get_object_member(definition, "settings");
            JsonArray *sources = json_object_get_array_member(definition, "sources");
            gchar *count = g_strdup_printf("%u playlist", sources ? json_array_get_length(sources) : 0);
            gchar *absent = radios_automation_calendar_excluded_kinds_text(settings);
            gchar *name = g_strdup_printf("%s%s%s%s", radios_json_string(definition, "name", "Rotasyon"), schema ? " · Şema" : "", *absent ? " · yok: " : "", absent);
            g_free(absent);
            JsonNode *policy = settings ? json_object_get_member(settings, "probability_policy") : NULL; gboolean percentages = FALSE;
            JsonNode *groups = policy && JSON_NODE_HOLDS_OBJECT(policy) ? json_object_get_member(json_node_get_object(policy), "children") : NULL;
            for (guint g = 0; groups && JSON_NODE_HOLDS_ARRAY(groups) && g < json_array_get_length(json_node_get_array(groups)); ++g)
                percentages |= !g_strcmp0(radios_json_string(json_array_get_object_element(json_node_get_array(groups), g), "scope", ""), "playlist");
            gtk_list_store_append(view->library_store, &row);
            gtk_list_store_set(view->library_store, &row, 0, radios_json_string(definition, "id", ""),
                1, name, 2, count,
                3, percentages ? "Olasılık %" : settings && !g_strcmp0(radios_json_string(settings, "selection_mode", "linear"), "weighted") ? "Ağırlıklı" : "Sırayla",
                4, "Olasılık", 5, "Ayarlar", 6, "Ayarlar", 7, "100", -1);
            if (g_hash_table_contains(chosen, radios_json_string(definition, "id", ""))) gtk_tree_selection_select_iter(table_selection, &row);
            g_free(count); g_free(name);
        }
    }
    JsonArray *programmes = events(view);
    for (guint i = 0; !live_only && i < json_array_get_length(programmes); ++i) {
        JsonObject *event = json_array_get_object_element(programmes, i);
        JsonArray *rows = json_object_get_array_member(event, "rotations");
        for (guint j = 0; j < json_array_get_length(rows); ++j) {
            JsonObject *row = json_array_get_object_element(rows, j);
            if (!json_object_has_member(row, "playlist_id")) continue;
            gchar *playlist = playlist_name(view, radios_json_string(row, "playlist_id", ""));
            gchar *caption = g_strdup_printf("%s · %s", playlist, radios_json_string(event, "title", "Program"));
            gchar *key = g_strconcat("legacy/", radios_json_string(row, "id", ""), NULL);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->library_choice), key, caption); ++choices;
            if (view->library_store) {
                GtkTreeIter item; gchar *frequency = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(row, "frequency", 1));
                gtk_list_store_append(view->library_store, &item);
                gtk_list_store_set(view->library_store, &item, 0, key, 1, caption, 2, frequency,
                    3, json_object_get_boolean_member(row, "random") ? "Rastgele" : "Doğrusal",
                    4, "Olasılık", 5, "Ayarlar", 6, "Ayarlar", 7, "100", -1);
                if (g_hash_table_contains(chosen, key)) gtk_tree_selection_select_iter(table_selection, &item);
                g_free(frequency);
            }
            g_free(playlist); g_free(caption); g_free(key);
        }
    }
    if (!selected || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->library_choice), selected))
        gtk_combo_box_set_active(GTK_COMBO_BOX(view->library_choice), choices ? 0 : -1);
    view->updating = was_updating; g_free(selected); g_hash_table_unref(chosen);
}
static gboolean library_can_edit(Calendar *view)
{
    if (view->closing || view->disposed || (calendar_active_document(view) && !radios_data_document_ready(calendar_active_document(view)))) {
        notice(view, "Rotasyon kayıtları yükleniyor; bağlantı hazır olduğunda düzenleyebilirsiniz."); return FALSE;
    }
    return TRUE;
}
static gboolean library_add_assignment(Calendar *view, JsonObject *event, const gchar *id)
{
    if (!event || !library_by_id(view, id)) return FALSE;
    JsonArray *rows = json_object_get_array_member(event, "rotations");
    if (json_array_get_length(rows) >= 50) { notice(view, "Bir program en fazla 50 rotasyon içerebilir."); return FALSE; }
    /* Keep the preparation contract bounded even with nested definitions. */
    guint total = 0;
    for (guint i = 0; i <= json_array_get_length(rows); ++i) {
        JsonObject *row = i < json_array_get_length(rows) ? json_array_get_object_element(rows, i) : NULL;
        const gchar *ref = row ? radios_json_string(row, "rotation_id", "") : id;
        JsonObject *definition = *ref ? library_by_id(view, ref) : NULL;
        total += definition ? json_array_get_length(json_object_get_array_member(definition, "sources")) : 1;
    }
    if (total > 50) { notice(view, "Programın rotasyonlarında toplam en fazla 50 playlist olabilir."); return FALSE; }
    JsonObject *assignment = json_object_new(); gchar *uuid = g_uuid_string_random();
    json_object_set_string_member(assignment, "id", uuid); g_free(uuid);
    json_object_set_string_member(assignment, "rotation_id", id); json_array_add_object_element(rows, assignment); return TRUE;
}
static JsonObject *library_default_settings(void)
{
    JsonObject *settings = json_object_new();
    json_object_set_string_member(settings, "selection_mode", "linear");
    const gchar *gaps[] = { "track_gap_minutes", "artist_gap_minutes", "album_gap_minutes" };
    for (guint i = 0; i < G_N_ELEMENTS(gaps); ++i) json_object_set_int_member(settings, gaps[i], 0);
    JsonArray *transitions = json_array_new();
    for (guint i = 0; i < 3; ++i) {
        JsonObject *transition = json_object_new(); json_object_set_int_member(transition, "mode", i);
        json_object_set_int_member(transition, "weight", i ? 0 : 100); json_object_set_int_member(transition, "overlap_ms", i ? 3000 : 0);
        json_array_add_object_element(transitions, transition);
    }
    json_object_set_array_member(settings, "transitions", transitions); return settings;
}
/* Historic flat rotations remain selectable. Reusing one creates a named
 * independent definition, retaining its playlist and selection flags while
 * keeping the original programme and source identities untouched. */
static gchar *library_definition_from_choice(Calendar *view, const gchar *choice, gboolean *created)
{
    *created = FALSE;
    if (library_by_id(view, choice)) return g_strdup(choice);
    if (!choice || !g_str_has_prefix(choice, "legacy/") || !g_uuid_string_is_valid(choice + 7)) return NULL;
    JsonObject *original = NULL; JsonArray *programmes = events(view);
    for (guint i = 0; !original && i < json_array_get_length(programmes); ++i) {
        JsonArray *rows = json_object_get_array_member(json_array_get_object_element(programmes, i), "rotations");
        for (guint j = 0; j < json_array_get_length(rows); ++j) {
            JsonObject *row = json_array_get_object_element(rows, j);
            if (json_object_has_member(row, "playlist_id") && !g_strcmp0(choice + 7, radios_json_string(row, "id", ""))) { original = row; break; }
        }
    }
    if (!original) { notice(view, "Seçili eski rotasyon değişti veya kaldırıldı; yeniden seçin."); return NULL; }
    JsonArray *library = library_array(view, FALSE);
    if (library && json_array_get_length(library) >= 200) { notice(view, "En fazla 200 kayıtlı rotasyon olabilir."); return NULL; }
    JsonNode *node = object_node(original), *copy = deep_copy(node); json_node_free(node);
    if (!copy) return NULL;
    JsonObject *source = json_node_get_object(copy); gchar *source_id = g_uuid_string_random();
    json_object_set_string_member(source, "id", source_id); g_free(source_id);
    json_object_set_int_member(source, "count", radios_json_int(original, "frequency", 1));
    json_object_set_int_member(source, "frequency", 1); json_object_set_int_member(source, "weight", 1);
    json_object_set_int_member(source, "start_minute", 0); json_object_set_int_member(source, "end_minute", 1440);
    json_object_set_int_member(source, "interval_minutes", 0);
    JsonObject *definition = json_object_new(); gchar *id = g_uuid_string_random();
    gchar *name = playlist_name(view, radios_json_string(source, "playlist_id", ""));
    gchar *bounded_name = g_utf8_substring(name, 0, MIN(g_utf8_strlen(name, -1), 160)); g_free(name);
    json_object_set_string_member(definition, "id", id); json_object_set_string_member(definition, "name", *bounded_name ? bounded_name : "Rotasyon"); g_free(bounded_name);
    JsonArray *sources = json_array_new(); json_array_add_element(sources, copy);
    json_object_set_array_member(definition, "sources", sources); json_object_set_object_member(definition, "settings", library_default_settings());
    json_array_add_object_element(library_array(view, TRUE), definition); *created = TRUE; return id;
}
static void library_choice_changed(GtkComboBox *combo, gpointer data)
{
    Calendar *view = data; if (view->updating) return;
    gboolean ready = !calendar_active_document(view) || radios_data_document_ready(calendar_active_document(view));
    const gchar *id = gtk_combo_box_get_active_id(combo); gboolean selected = id != NULL;
    /* Legacy callers select by UUID through the hidden adapter. Reflect that
     * selection in the real table, while preserving a visible multi-selection
     * when its first selected row already has this identity. */
    if (selected && view->library_rows) {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view->library_rows));
        GList *paths = gtk_tree_selection_get_selected_rows(selection, NULL); gboolean matches = FALSE;
        if (paths) {
            GtkTreeIter iter; gchar *first = NULL;
            if (gtk_tree_model_get_iter(GTK_TREE_MODEL(view->library_store), &iter, paths->data)) {
                gtk_tree_model_get(GTK_TREE_MODEL(view->library_store), &iter, 0, &first, -1); matches = !g_strcmp0(first, id); g_free(first);
            }
        }
        g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
        if (!matches) {
            GtkTreeIter iter; gboolean more = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(view->library_store), &iter);
            view->updating = TRUE; gtk_tree_selection_unselect_all(selection);
            while (more) {
                gchar *key = NULL; gtk_tree_model_get(GTK_TREE_MODEL(view->library_store), &iter, 0, &key, -1);
                gboolean match = !g_strcmp0(key, id); g_free(key);
                if (match) { gtk_tree_selection_select_iter(selection, &iter); break; }
                more = gtk_tree_model_iter_next(GTK_TREE_MODEL(view->library_store), &iter);
            }
            view->updating = FALSE;
        }
        selected = gtk_tree_selection_count_selected_rows(selection) > 0;
    }
    gtk_widget_set_sensitive(view->library_edit, ready && selected);
    gtk_widget_set_sensitive(view->library_assign, ready && selected && event_by_id(view, view->current));
}
static void library_selection_changed(GtkTreeSelection *selection, gpointer data)
{
    Calendar *view = data; if (view->updating) return;
    GList *paths = gtk_tree_selection_get_selected_rows(selection, NULL);
    if (paths) {
        GtkTreeIter iter; gchar *id = NULL;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(view->library_store), &iter, paths->data)) {
            gtk_tree_model_get(GTK_TREE_MODEL(view->library_store), &iter, 0, &id, -1);
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->library_choice), id); g_free(id);
        }
    }
    gtk_widget_set_sensitive(view->library_assign, paths && event_by_id(view, view->current) && library_can_edit(view));
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
}
static void library_row_activated(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column, gpointer data)
{
    (void)tree; (void)column; Calendar *view = data; GtkTreeIter iter; gchar *id = NULL;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(view->library_store), &iter, path)) return;
    gtk_tree_model_get(GTK_TREE_MODEL(view->library_store), &iter, 0, &id, -1);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->library_choice), id); g_free(id);
    library_edit(NULL, view);
}
static void library_assign(GtkButton *button, gpointer data)
{
    (void)button; Calendar *view = data; if (!library_can_edit(view)) return;
    JsonObject *event = event_by_id(view, view->current); if (!event) return;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view->library_rows));
    GList *paths = gtk_tree_selection_get_selected_rows(selection, NULL);
    GPtrArray *choices = g_ptr_array_new_with_free_func(g_free);
    for (GList *it = paths; it; it = it->next) {
        GtkTreeIter iter; gchar *id = NULL;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(view->library_store), &iter, it->data)) {
            gtk_tree_model_get(GTK_TREE_MODEL(view->library_store), &iter, 0, &id, -1); if (id) g_ptr_array_add(choices, id);
        }
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    if (!choices->len) { notice(view, "Atamak için tablodan en az bir rotasyon seç."); g_ptr_array_unref(choices); return; }
    JsonNode *before = deep_copy(view->config); gboolean success = choices->len > 0;
    for (guint i = 0; success && i < choices->len; ++i) {
        gboolean created = FALSE; gchar *id = library_definition_from_choice(view, g_ptr_array_index(choices, i), &created);
        success = id && library_add_assignment(view, event, id); g_free(id);
    }
    if (success) commit(view);
    else { json_node_free(view->config); view->config = before; before = NULL; refresh(view); }
    g_clear_pointer(&before, json_node_free); g_ptr_array_unref(choices);
}
static void event_probability_changed(GtkWidget *editor, JsonNode *snapshot, gpointer data)
{
    Calendar *view = data;
    if (view->closing || view->disposed || !snapshot || (!JSON_NODE_HOLDS_OBJECT(snapshot) && !JSON_NODE_HOLDS_NULL(snapshot))) return;
    const gchar *id = g_object_get_data(G_OBJECT(editor), "probability-event-id");
    JsonObject *event = event_by_id(view, id);
    JsonNode *base = g_object_get_data(G_OBJECT(editor), "probability-event-base");
    JsonNode *current = event ? object_node(event) : NULL;
    RadioSDataClient *opened = g_object_get_data(G_OBJECT(editor), "probability-client");
    RadioSDataClient *client = radios_data_client_acquire(view->ctx);
    gboolean allowed = current && base && json_node_equal(current, base) && radios_data_client_same_identity(client, opened);
    if (client) radios_data_client_unref(client);
    g_clear_pointer(&current, json_node_free);
    if (!allowed) { gtk_widget_set_sensitive(editor, FALSE); notice(view, "Program veya veri oturumu değişti; olasılıkları yeniden açın."); return; }
    /* Nothing left: no policy, so weights and linear order apply again. */
    if (JSON_NODE_HOLDS_NULL(snapshot)) json_object_remove_member(event, "probability_policy");
    else json_object_set_member(event, "probability_policy", deep_copy(snapshot));
    JsonNode *node = object_node(event);
    g_object_set_data_full(G_OBJECT(editor), "probability-event-base", deep_copy(node), (GDestroyNotify)json_node_free); json_node_free(node);
    commit(view);
}
static void probability_return(GtkButton *button, gpointer data)
{
    (void)button; Calendar *view = data;
    gtk_stack_set_visible_child_name(GTK_STACK(view->settings_stack), "calendar");
    GtkWidget *page = view->probability_page; view->probability_page = NULL;
    if (page) gtk_widget_destroy(page);
}
static void event_probability(GtkButton *button, gpointer data)
{
    (void)button; Calendar *view = data; JsonObject *event = event_by_id(view, view->current);
    if (!event || !library_can_edit(view)) return;
    if (view->probability_page) probability_return(NULL, view);
    GtkWidget *page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "ac-inline-settings");
    view->probability_page = page; gtk_widget_set_name(page, "automation-calendar-probability-page");
    GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 12, "ac-inline-heading");
    GtkWidget *back = radios_ui_button("← Programa dön");
    gtk_widget_set_name(back, "automation-calendar-probability-back");
    g_signal_connect(back, "clicked", G_CALLBACK(probability_return), view);
    gtk_box_pack_start(GTK_BOX(heading), back, FALSE, FALSE, 0);
    gchar *title = g_strdup_printf("Olasılık · %s", radios_json_string(event, "title", "Program"));
    GtkWidget *caption = radios_ui_label(title, "heading"); g_free(title);
    gtk_widget_set_name(caption, "automation-calendar-probability-title");
    gtk_box_pack_start(GTK_BOX(heading), caption, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);
    RadioSProbabilityEditorHooks hooks = {.changed = event_probability_changed, .user = view};
    GtkWidget *editor = radios_probability_editor_new(view->ctx, &hooks);
    /* Only this programme's rotations and their playlists are listed; other
     * jingle playlists remain jingle sources. */
    JsonNode *rows = json_object_get_member(event, "rotations");
    JsonArray *targets = json_array_new(), *assignments = rows && JSON_NODE_HOLDS_ARRAY(rows) ? json_node_get_array(rows) : NULL, *playlists = calendar_playlists(view);
    GHashTable *members = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; assignments && i < json_array_get_length(assignments); ++i) {
        JsonObject *assignment = json_array_get_object_element(assignments, i);
        JsonObject *definition = library_by_id(view, radios_json_string(assignment, "rotation_id", ""));
        if (!definition) { if (*radios_json_string(assignment, "playlist_id", "")) g_hash_table_add(members, (gpointer)radios_json_string(assignment, "playlist_id", "")); continue; }
        if (g_hash_table_contains(members, radios_json_string(definition, "id", ""))) continue;
        g_hash_table_add(members, (gpointer)radios_json_string(definition, "id", ""));
        JsonObject *target = json_object_new(); json_object_set_string_member(target, "id", radios_json_string(definition, "id", ""));
        json_object_set_string_member(target, "scope", "automation"); json_object_set_string_member(target, "name", radios_json_string(definition, "name", "Rotasyon"));
        json_array_add_object_element(targets, target);
        JsonNode *list = json_object_get_member(definition, "sources"); JsonArray *sources = list && JSON_NODE_HOLDS_ARRAY(list) ? json_node_get_array(list) : NULL;
        for (guint j = 0; sources && j < json_array_get_length(sources); ++j)
            g_hash_table_add(members, (gpointer)radios_json_string(json_array_get_object_element(sources, j), "playlist_id", ""));
    }
    for (guint i = 0; playlists && i < json_array_get_length(playlists); ++i) {
        JsonObject *playlist = json_array_get_object_element(playlists, i); const gchar *id = radios_json_string(playlist, "_id", "");
        const gchar *purpose = radios_json_string(playlist, "purpose", "music"); gboolean member = g_hash_table_contains(members, id);
        if (!member && g_strcmp0(purpose, "jingle")) continue;
        JsonObject *target = json_object_new(); json_object_set_string_member(target, "id", id);
        json_object_set_string_member(target, "scope", "playlist"); json_object_set_string_member(target, "name", radios_json_string(playlist, "name", "Playlist"));
        json_object_set_string_member(target, "purpose", purpose); json_object_set_boolean_member(target, "listed", member);
        json_array_add_object_element(targets, target);
    }
    g_hash_table_unref(members);
    radios_probability_editor_set_targets(editor, targets); json_array_unref(targets);
    JsonNode *policy = json_object_get_member(event, "probability_policy");
    radios_probability_editor_set_policy(editor, policy && JSON_NODE_HOLDS_OBJECT(policy) ? json_node_get_object(policy) : NULL);
    g_object_set_data_full(G_OBJECT(editor), "probability-event-id", g_strdup(view->current), g_free);
    JsonNode *node = object_node(event); g_object_set_data_full(G_OBJECT(editor), "probability-event-base", deep_copy(node), (GDestroyNotify)json_node_free); json_node_free(node);
    RadioSDataClient *client = radios_data_client_acquire(view->ctx);
    g_object_set_data_full(G_OBJECT(editor), "probability-client", client, (GDestroyNotify)radios_data_client_unref);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_name(scroll, "automation-calendar-probability-scroll");
    gtk_container_add(GTK_CONTAINER(scroll), editor); gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);
    gtk_stack_add_named(GTK_STACK(view->settings_stack), page, "probability");
    gtk_widget_show_all(page);
    gtk_stack_set_visible_child_name(GTK_STACK(view->settings_stack), "probability");
}
static void library_closed(GtkWidget *dialog, gpointer data)
{ Calendar *view = data; if (view->library_dialog == dialog) view->library_dialog = NULL; }
static void library_reconcile_editor(Calendar *view)
{
    if (view->schema_dialog) {
        JsonNode *base = g_object_get_data(G_OBJECT(view->schema_dialog), "schema-library-base");
        JsonNode *current = json_object_get_member(json_node_get_object(view->config), "rotation_library");
        if ((base || current) && !(base && current && json_node_equal(base, current))) gtk_widget_destroy(view->schema_dialog);
    }
    if (!view->library_dialog) return;
    const gchar *id = g_object_get_data(G_OBJECT(view->library_dialog), "rotation-id");
    JsonNode *base = g_object_get_data(G_OBJECT(view->library_dialog), "editor-base");
    JsonObject *definition = library_by_id(view, id);
    JsonNode *current = definition ? object_node(definition) : NULL;
    gboolean matches = current && base && json_node_equal(current, base);
    g_clear_pointer(&current, json_node_free);
    /* Keep an unchanged save acknowledgement transparent. A replacement or
     * deletion closes the obsolete draft before the next field can mutate it. */
    if (!matches) gtk_widget_destroy(view->library_dialog);
}
static void library_changed(GtkWidget *editor, JsonNode *snapshot, gpointer data)
{
    Calendar *view = data;
    if (!view->library_dialog || view->closing || view->disposed || !snapshot || !JSON_NODE_HOLDS_OBJECT(snapshot)) return;
    RadioSDataClient *opened = g_object_get_data(G_OBJECT(view->library_dialog), "editor-client");
    RadioSDataClient *current = radios_data_client_acquire(view->ctx);
    /* A detached draft without a data adapter has no remote identity to
     * compare. A real session still must match the one that opened it. */
    gboolean same = (!view->ctx->data && !current && !opened) || radios_data_client_same_identity(current, opened);
    if (current) radios_data_client_unref(current);
    if (!same || !library_can_edit(view)) { gtk_widget_set_sensitive(editor, FALSE); notice(view, "Veri oturumu değişti; rotasyonu yeniden açın."); return; }
    const gchar *id = g_object_get_data(G_OBJECT(view->library_dialog), "rotation-id");
    JsonObject *existing = library_by_id(view, id); JsonNode *base = g_object_get_data(G_OBJECT(view->library_dialog), "editor-base");
    JsonNode *existing_node = existing ? object_node(existing) : NULL;
    gboolean matches = existing_node && base && json_node_equal(existing_node, base);
    g_clear_pointer(&existing_node, json_node_free);
    if (!matches || g_strcmp0(radios_json_string(json_node_get_object(snapshot), "id", ""), id)) {
        gtk_widget_set_sensitive(editor, FALSE); notice(view, "Rotasyon başka bir işlemle değişti; yeniden açın."); return;
    }
    /* A definition edit can affect several programmes. Reject the complete
     * draft if any assignment would exceed the authenticated prepare limit. */
    guint replacement_count = json_array_get_length(json_object_get_array_member(json_node_get_object(snapshot), "sources"));
    JsonArray *programmes = events(view);
    for (guint i = 0; i < json_array_get_length(programmes); ++i) {
        JsonArray *rows = json_object_get_array_member(json_array_get_object_element(programmes, i), "rotations"); guint count = 0;
        for (guint j = 0; j < json_array_get_length(rows); ++j) {
            JsonObject *row = json_array_get_object_element(rows, j); const gchar *ref = radios_json_string(row, "rotation_id", "");
            JsonObject *definition = *ref ? library_by_id(view, ref) : NULL;
            count += !g_strcmp0(ref, id) ? replacement_count : definition ? json_array_get_length(json_object_get_array_member(definition, "sources")) : 1;
        }
        if (count > 50) {
            radios_rotation_editor_set_config(editor, existing);
            notice(view, "Bu değişiklik programdaki 50 playlist sınırını aşar; ekleme kaydedilmedi."); return;
        }
    }
    JsonArray *library = library_array(view, FALSE);
    for (guint i = 0; i < json_array_get_length(library); ++i) {
        if (json_array_get_object_element(library, i) != existing) continue;
        /* JSON arrays have no replace setter; rebuild with independent nodes. */
        JsonArray *replacement = json_array_new();
        for (guint j = 0; j < json_array_get_length(library); ++j)
            json_array_add_element(replacement, deep_copy(j == i ? snapshot : json_array_get_element(library, j)));
        json_object_set_array_member(json_node_get_object(view->config), "rotation_library", replacement); break;
    }
    g_object_set_data_full(G_OBJECT(view->library_dialog), "editor-base", deep_copy(snapshot), (GDestroyNotify)json_node_free);
    /* Bound frequent spins/typing to the same autosave coalescing path as the
     * inspector. The document preserves latest pending edits on window close. */
    if (view->autosave_source) g_source_remove(view->autosave_source);
    changed(view); view->autosave_source = g_timeout_add(250, autosave_tick, view); refresh(view);
}
static void library_open(Calendar *view, const gchar *id)
{
    if (!library_can_edit(view)) return;
    JsonObject *definition = library_by_id(view, id); if (!definition) return;
    if (view->library_dialog) { gtk_window_present(GTK_WINDOW(view->library_dialog)); return; }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Rotasyon düzenleyici", view->ctx->parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Kapat", GTK_RESPONSE_CLOSE, NULL);
    view->library_dialog = dialog; gtk_widget_set_name(dialog, "automation-calendar-library-dialog"); style_calendar_dialog(dialog);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 1180, 820);
    RadioSRotationEditorHooks hooks = { .changed = library_changed, .user = view };
    GtkWidget *editor = radios_rotation_editor_new(view->ctx, &hooks);
    g_object_set_data(G_OBJECT(dialog), "rotation-editor", editor);
    g_object_set_data_full(G_OBJECT(dialog), "rotation-id", g_strdup(id), g_free);
    JsonNode *node = object_node(definition);
    g_object_set_data_full(G_OBJECT(dialog), "editor-base", deep_copy(node), (GDestroyNotify)json_node_free); json_node_free(node);
    RadioSDataClient *client = radios_data_client_acquire(view->ctx);
    if (client) g_object_set_data_full(G_OBJECT(dialog), "editor-client", client, (GDestroyNotify)radios_data_client_unref);
    radios_rotation_editor_set_config(editor, definition);
    JsonArray *playlists = calendar_playlists(view);
    radios_rotation_editor_set_playlists(editor, playlists);
    radios_rotation_editor_set_schemas(editor, library_array(view, FALSE));
    library_channels(view);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), editor, TRUE, TRUE, 0);
    g_signal_connect(dialog, "destroy", G_CALLBACK(library_closed), view);
    g_signal_connect(dialog, "response", G_CALLBACK(library_dialog_response), view);
    g_signal_connect_object(view->page, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    gtk_widget_show_all(dialog);
    if (view->ctx->data) load_playlists(view->ctx, view->page);
}
static void library_edit(GtkButton *button, gpointer data)
{
    (void)button; Calendar *view = data; if (!library_can_edit(view)) return;
    if (view->library_dialog) { gtk_window_present(GTK_WINDOW(view->library_dialog)); return; }
    gboolean created = FALSE;
    gchar *id = library_definition_from_choice(view, gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->library_choice)), &created);
    if (created) { commit(view); gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->library_choice), id); }
    if (id) library_open(view, id);
    g_free(id);
}
static void library_create(GtkButton *button, gpointer data)
{
    Calendar *view = data; if (!library_can_edit(view)) return;
    if (view->library_dialog) { gtk_window_present(GTK_WINDOW(view->library_dialog)); return; }
    JsonArray *library = library_array(view, TRUE);
    if (json_array_get_length(library) >= 200) { notice(view, "En fazla 200 kayıtlı rotasyon olabilir."); return; }
    JsonObject *event = GTK_WIDGET(button) == view->define ? event_by_id(view, view->current) : NULL;
    if (GTK_WIDGET(button) == view->define && !event) return;
    if (event && json_array_get_length(json_object_get_array_member(event, "rotations")) >= 50) {
        notice(view, "Bir program en fazla 50 rotasyon içerebilir."); return;
    }
    JsonObject *definition = json_object_new(); gchar *id = g_uuid_string_random();
    json_object_set_string_member(definition, "id", id);
    gchar *name = g_strdup_printf(GTK_WIDGET(button) == view->schema ? "Yeni şema %u" : "Yeni rotasyon %u", json_array_get_length(library) + 1);
    json_object_set_string_member(definition, "name", name); g_free(name);
    JsonArray *sources = json_array_new();
    if (GTK_WIDGET(button) == view->schema && view->schema_playlist) {
        JsonObject *source = json_object_new(); gchar *source_id = g_uuid_string_random();
        json_object_set_string_member(source, "id", source_id); g_free(source_id);
        json_object_set_string_member(source, "playlist_id", view->schema_playlist);
        json_object_set_int_member(source, "frequency", 1); json_object_set_int_member(source, "count", 1);
        json_object_set_int_member(source, "weight", 100); json_object_set_int_member(source, "start_minute", 0);
        json_object_set_int_member(source, "end_minute", 1440); json_object_set_int_member(source, "interval_minutes", 0);
        const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
        for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(source, flags[i], FALSE);
        json_object_set_string_member(source, "media_kind", "all"); json_array_add_object_element(sources, source);
    }
    json_object_set_array_member(definition, "sources", sources);
    JsonObject *settings = library_default_settings();
    if (GTK_WIDGET(button) == view->schema) json_object_set_boolean_member(settings, "automation_schema", TRUE);
    json_object_set_object_member(definition, "settings", settings);
    json_array_add_object_element(library, definition);
    if (event && !library_add_assignment(view, event, id)) { json_array_remove_element(library, json_array_get_length(library) - 1); g_free(id); return; }
    commit(view); gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->library_choice), id); library_open(view, id); g_free(id);
}
static void schema_settings_editor(Calendar *view, const gchar *id)
{
    if (!definition_schema(library_by_id(view, id))) return;
    if (view->library_dialog && g_strcmp0(id, g_object_get_data(G_OBJECT(view->library_dialog), "rotation-id")))
        gtk_widget_destroy(view->library_dialog);
    library_open(view, id);
    if (!view->library_dialog) return;
    gtk_window_set_title(GTK_WINDOW(view->library_dialog), "Şema Ayarları");
    if (!g_object_get_data(G_OBJECT(view->library_dialog), "schema-new-button")) {
        GtkWidget *create = gtk_dialog_add_button(GTK_DIALOG(view->library_dialog), "Yeni şema", GTK_RESPONSE_APPLY);
        gtk_widget_set_name(create, "automation-calendar-new-schema");
        g_object_set_data(G_OBJECT(view->library_dialog), "schema-new-button", create); gtk_widget_show(create);
    }
    radios_rotation_editor_open_schema_settings(g_object_get_data(G_OBJECT(view->library_dialog), "rotation-editor"));
}
static void library_dialog_response(GtkDialog *dialog, gint response, gpointer data)
{
    Calendar *view = data;
    gboolean create = response == GTK_RESPONSE_APPLY && definition_schema(library_by_id(view,
        g_object_get_data(G_OBJECT(dialog), "rotation-id")));
    gtk_widget_destroy(GTK_WIDGET(dialog));
    if (create) {
        library_create(GTK_BUTTON(view->schema), view);
        if (view->library_dialog) schema_settings_editor(view, g_object_get_data(G_OBJECT(view->library_dialog), "rotation-id"));
    }
}
static void schema_dialog_closed(GtkWidget *dialog, gpointer data)
{ Calendar *view = data; if (view->schema_dialog == dialog) view->schema_dialog = NULL; }
enum { SCHEMA_RESPONSE_DELETE = 1 };
/* Programmes and live steps that name a schema keep it from being deleted. */
static guint schema_uses(Calendar *view, const gchar *id)
{
    guint uses = 0; JsonArray *programmes = events(view), *library = library_array(view, FALSE);
    for (guint i = 0; programmes && i < json_array_get_length(programmes); ++i) {
        JsonObject *event = json_array_get_object_element(programmes, i);
        JsonArray *rows = json_object_has_member(event, "rotations") ? json_object_get_array_member(event, "rotations") : NULL;
        for (guint j = 0; rows && j < json_array_get_length(rows); ++j) uses += !g_strcmp0(radios_json_string(json_array_get_object_element(rows, j), "rotation_id", ""), id);
        JsonNode *live = json_object_get_member(event, "live");
        if (live && JSON_NODE_HOLDS_OBJECT(live)) uses += !g_strcmp0(radios_json_string(json_node_get_object(live), "automation_id", ""), id);
    }
    for (guint i = 0; library && i < json_array_get_length(library); ++i) {
        JsonObject *definition = json_array_get_object_element(library, i);
        JsonArray *sources = json_object_has_member(definition, "sources") ? json_object_get_array_member(definition, "sources") : NULL;
        for (guint j = 0; sources && j < json_array_get_length(sources); ++j) {
            JsonNode *live = json_object_get_member(json_array_get_object_element(sources, j), "live");
            if (live && JSON_NODE_HOLDS_OBJECT(live)) uses += !g_strcmp0(radios_json_string(json_node_get_object(live), "automation_id", ""), id);
        }
    }
    return uses;
}
static void schema_settings(GtkButton *button, gpointer data);
static void schema_delete(Calendar *view, GtkDialog *dialog)
{
    GtkComboBox *choice = g_object_get_data(G_OBJECT(dialog), "schema-choice");
    const gchar *id = gtk_combo_box_get_active_id(choice); JsonArray *library = library_array(view, FALSE);
    if (!id || !definition_schema(library_by_id(view, id)) || !library) return;
    guint uses = schema_uses(view, id);
    if (uses) {
        gchar *message = g_strdup_printf("Bu şema %u yerde kullanılıyor; önce programlardan ve live adımlarından kaldırın.", uses);
        gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(dialog), "schema-status")), message); notice(view, message); g_free(message); return;
    }
    /* JSON arrays have no remove-by-identity; rebuild with independent nodes. */
    JsonArray *replacement = json_array_new();
    for (guint i = 0; i < json_array_get_length(library); ++i)
        if (g_strcmp0(radios_json_string(json_array_get_object_element(library, i), "id", ""), id))
            json_array_add_element(replacement, deep_copy(json_array_get_element(library, i)));
    json_object_set_array_member(json_node_get_object(view->config), "rotation_library", replacement);
    gtk_widget_destroy(GTK_WIDGET(dialog)); commit(view); notice(view, "Şema silindi.");
    schema_settings(NULL, view);
}
static void schema_dialog_response(GtkDialog *dialog, gint response, gpointer data)
{
    Calendar *view = data;
    if (response == GTK_RESPONSE_ACCEPT) {
        GtkComboBox *choice = g_object_get_data(G_OBJECT(dialog), "schema-choice");
        gchar *id = g_strdup(gtk_combo_box_get_active_id(choice));
        if (!definition_schema(library_by_id(view, id))) { g_free(id); return; }
        gtk_widget_destroy(GTK_WIDGET(dialog)); schema_settings_editor(view, id); g_free(id);
    } else if (response == SCHEMA_RESPONSE_DELETE) {
        schema_delete(view, dialog);
    } else if (response == GTK_RESPONSE_APPLY) {
        gtk_widget_destroy(GTK_WIDGET(dialog)); library_create(GTK_BUTTON(view->schema), view);
        if (view->library_dialog) {
            const gchar *id = g_object_get_data(G_OBJECT(view->library_dialog), "rotation-id");
            schema_settings_editor(view, id);
        }
    } else gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void schema_settings(GtkButton *button, gpointer data)
{
    (void)button; Calendar *view = data; if (!library_can_edit(view)) return;
    if (view->schema_dialog) { gtk_window_present(GTK_WINDOW(view->schema_dialog)); return; }
    gchar *chosen = apply_schema_choice(view);
    const gchar *selected = gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->library_choice));
    if (!chosen && definition_schema(library_by_id(view, selected))) chosen = g_strdup(selected);
    JsonArray *library = library_array(view, FALSE); guint count = 0; const gchar *only = NULL;
    for (guint i = 0; library && i < json_array_get_length(library); ++i) {
        JsonObject *definition = json_array_get_object_element(library, i);
        if (definition_schema(definition)) { ++count; only = radios_json_string(definition, "id", ""); }
    }
    if (!chosen && count == 1) chosen = g_strdup(only);
    if (chosen) { schema_settings_editor(view, chosen); g_free(chosen); return; }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Şema Ayarları", view->ctx->parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Kapat", GTK_RESPONSE_CLOSE, "Şemayı sil", SCHEMA_RESPONSE_DELETE,
        "Yeni şema", GTK_RESPONSE_APPLY, "Ayarları aç", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), SCHEMA_RESPONSE_DELETE), "automation-calendar-schema-delete");
    view->schema_dialog = dialog; gtk_widget_set_name(dialog, "automation-calendar-schema-settings-dialog"); style_calendar_dialog(dialog);
    JsonNode *base = json_object_get_member(json_node_get_object(view->config), "rotation_library");
    if (base) g_object_set_data_full(G_OBJECT(dialog), "schema-library-base", deep_copy(base), (GDestroyNotify)json_node_free);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 500, 180);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    /* No guide text in the window (user rule): the hint is the choice's tooltip. */
    GtkWidget *choice = gtk_combo_box_text_new(); gtk_widget_set_name(choice, "automation-calendar-schema-settings-choice");
    gtk_widget_set_tooltip_text(choice, count ? "Ayarlarını düzenlemek için kayıtlı şemayı seçin." : "Henüz kayıtlı şema yok; Yeni şema ile yayın akışını tanımlayın.");
    for (guint i = 0; library && i < json_array_get_length(library); ++i) {
        JsonObject *definition = json_array_get_object_element(library, i); if (!definition_schema(definition)) continue;
        JsonArray *steps = json_object_has_member(definition, "sources") ? json_object_get_array_member(definition, "sources") : NULL;
        gchar *caption = g_strdup_printf("%s · %u adım", radios_json_string(definition, "name", "Şema"), steps ? json_array_get_length(steps) : 0);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(choice), radios_json_string(definition, "id", ""), caption); g_free(caption);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(choice), count ? 0 : -1);
    gtk_widget_set_sensitive(choice, count > 0); gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, count > 0);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), SCHEMA_RESPONSE_DELETE, count > 0);
    gtk_box_pack_start(GTK_BOX(content), choice, FALSE, FALSE, 8); g_object_set_data(G_OBJECT(dialog), "schema-choice", choice);
    GtkWidget *status = radios_ui_label("", "application-status"); gtk_widget_set_name(status, "automation-calendar-schema-status");
    gtk_label_set_line_wrap(GTK_LABEL(status), TRUE); gtk_box_pack_start(GTK_BOX(content), status, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(dialog), "schema-status", status);
    g_signal_connect(dialog, "destroy", G_CALLBACK(schema_dialog_closed), view);
    g_signal_connect(dialog, "response", G_CALLBACK(schema_dialog_response), view);
    g_signal_connect_object(view->page, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    gtk_widget_show_all(dialog);
}
static GtkWidget *calendar_find_main(GtkWidget *root)
{
    if (!g_strcmp0(gtk_widget_get_name(root), "automation-calendar-main")) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *it = children; it && !result; it = it->next) result = calendar_find_main(it->data);
    g_list_free(children); return result;
}
gboolean radios_automation_calendar_create_player_schema(RadioSViewContext *ctx,
    const gchar *playlist_id, GError **error)
{
    GtkWidget *main = ctx && ctx->parent ? calendar_find_main(GTK_WIDGET(ctx->parent)) : NULL;
    Calendar *view = main ? g_object_get_data(G_OBJECT(main), "automation-calendar") : NULL;
    if (!playlist_id || !g_uuid_string_is_valid(playlist_id)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Şema için kayıtlı bir playlist seçin."); return FALSE;
    }
    if (!view || !library_can_edit(view)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "Şema için kayıtlı player playlisti ve yüklü otomasyon takvimi gerekli."); return FALSE;
    }
    if (view->library_dialog) {
        gtk_window_present(GTK_WINDOW(view->library_dialog));
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Yeni şema oluşturmadan önce açık düzenleyiciyi kapatın."); return FALSE;
    }
    if (view->schema_dialog) gtk_widget_destroy(view->schema_dialog);
    view->schema_playlist = playlist_id; library_create(GTK_BUTTON(view->schema), view); view->schema_playlist = NULL;
    if (!view->library_dialog) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, gtk_label_get_text(GTK_LABEL(view->status))); return FALSE;
    }
    schema_settings_editor(view, g_object_get_data(G_OBJECT(view->library_dialog), "rotation-id")); return TRUE;
}
static void rotation_closed(GtkWidget *dialog, gpointer data)
{ Calendar *view = data; if (view->rotation_dialog == dialog) view->rotation_dialog = NULL; }
static void rotation_response(GtkDialog *dialog, gint response, gpointer data)
{
    Calendar *view = data;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    const gchar *event_id = g_object_get_data(G_OBJECT(dialog), "event-id"); JsonObject *event = event_by_id(view, event_id);
    if (!event) { notice(view, "Hedef program değişti veya kaldırıldı."); return; }
    GtkComboBox *playlist = g_object_get_data(G_OBJECT(dialog), "playlist"); const gchar *id = gtk_combo_box_get_active_id(playlist);
    if (!id || !g_uuid_string_is_valid(id)) { notice(view, "Rotasyon için kayıtlı bir playlist seçin."); return; }
    JsonArray *array = json_object_get_array_member(event, "rotations");
    const gchar *edit_id = g_object_get_data(G_OBJECT(dialog), "rotation-id"); JsonObject *row = NULL;
    for (guint i = 0; edit_id && i < json_array_get_length(array); ++i) {
        JsonObject *candidate = json_array_get_object_element(array, i);
        if (g_str_equal(edit_id, radios_json_string(candidate, "id", ""))) { row = candidate; break; }
    }
    if (edit_id && !row) { notice(view, "Düzenlenen rotasyon kaldırıldı; yeniden seçin."); return; }
    if (!row && json_array_get_length(array) >= 50) { notice(view, "Bir program en fazla 50 rotasyon içerebilir."); return; }
    gboolean adding = row == NULL;
    if (adding) { row = json_object_new(); gchar *row_id = g_uuid_string_random(); json_object_set_string_member(row, "id", row_id); g_free(row_id); }
    GtkComboBox *kind = g_object_get_data(G_OBJECT(dialog), "media-kind");
    json_object_set_string_member(row, "media_kind", gtk_combo_box_get_active_id(kind) ? gtk_combo_box_get_active_id(kind) : "all");
    json_object_set_string_member(row, "playlist_id", id);
    GtkSpinButton *frequency = g_object_get_data(G_OBJECT(dialog), "frequency"); gtk_spin_button_update(frequency);
    json_object_set_int_member(row, "frequency", gtk_spin_button_get_value_as_int(frequency));
    const gchar *fields[] = { "random", "track_random", "advert_random", "sweeper_random" };
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i)
        json_object_set_boolean_member(row, fields[i], gtk_toggle_button_get_active(g_object_get_data(G_OBJECT(dialog), fields[i])));
    if (adding) json_array_add_object_element(array, row);
    gtk_widget_destroy(GTK_WIDGET(dialog));
    commit(view);
}
static void rotation_open(Calendar *view, const gchar *rotation_id)
{
    JsonObject *event = event_by_id(view, view->current); if (!event) return;
    JsonObject *existing = NULL; JsonArray *rotations = json_object_get_array_member(event, "rotations");
    for (guint i = 0; rotation_id && i < json_array_get_length(rotations); ++i) {
        JsonObject *row = json_array_get_object_element(rotations, i);
        if (g_str_equal(rotation_id, radios_json_string(row, "id", ""))) { existing = row; break; }
    }
    if (rotation_id && !existing) return;
    if (existing && json_object_has_member(existing, "rotation_id")) {
        library_open(view, radios_json_string(existing, "rotation_id", "")); return;
    }
    if (view->rotation_dialog) { gtk_window_present(GTK_WINDOW(view->rotation_dialog)); return; }
    GtkWidget *dialog = gtk_dialog_new_with_buttons(existing ? "Rotasyon düzenle" : "Yeni rotasyon", view->ctx->parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL, existing ? "Uygula" : "Rotasyon ekle", GTK_RESPONSE_ACCEPT, NULL);
    view->rotation_dialog = dialog; gtk_widget_set_name(dialog, "automation-calendar-rotation-dialog"); style_calendar_dialog(dialog);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    GtkWidget *playlist = gtk_combo_box_text_new(); gtk_widget_set_name(playlist, "automation-calendar-rotation-playlist");
    GtkWidget *frequency = gtk_spin_button_new_with_range(1, 100, 1); gtk_widget_set_name(frequency, "automation-calendar-rotation-frequency");
    gtk_widget_set_tooltip_text(frequency, "Her rotasyon turunda bu playlistten seçilecek parça adedi");
    GtkWidget *kind = gtk_combo_box_text_new(); gtk_widget_set_name(kind, "automation-calendar-rotation-media-kind");
    const gchar *kind_ids[] = { "all", "music", "jingle", "advertisement", "sweeper" };
    const gchar *kind_labels[] = { "Playlist · tüm içerikler", "Müzik", "Jingle", "Reklam", "Sweeper" };
    for (guint i = 0; i < G_N_ELEMENTS(kind_ids); ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(kind), kind_ids[i], kind_labels[i]);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(kind), existing ? radios_json_string(existing, "media_kind", "all") : "all");
    gtk_box_pack_start(GTK_BOX(box), radios_ui_label("İçerik türü", NULL), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), kind, FALSE, FALSE, 4);
    g_object_set_data(G_OBJECT(dialog), "media-kind", kind);
    if (existing) gtk_spin_button_set_value(GTK_SPIN_BUTTON(frequency), (gdouble)radios_json_int(existing, "frequency", 1));
    gtk_box_pack_start(GTK_BOX(box), radios_ui_label("Hangi playlist?", NULL), FALSE, FALSE, 4); gtk_box_pack_start(GTK_BOX(box), playlist, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), radios_ui_label("Sıklık", NULL), FALSE, FALSE, 4); gtk_box_pack_start(GTK_BOX(box), frequency, FALSE, FALSE, 4);
    g_object_set_data(G_OBJECT(dialog), "playlist", playlist); g_object_set_data(G_OBJECT(dialog), "frequency", frequency);
    const gchar *fields[] = { "random", "track_random", "advert_random", "sweeper_random" };
    const gchar *names[] = { "random", "track-random", "advert-random", "sweeper-random" };
    const gchar *labels[] = { "Playlistler arasından rastgele seç", "Playlist içindeki parçaları rastgele çal", "Reklamları rastgele seç", "Sweeper'ları rastgele seç" };
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) {
        GtkWidget *check = gtk_check_button_new_with_label(labels[i]); gchar *name = g_strdup_printf("automation-calendar-rotation-%s", names[i]);
        gtk_widget_set_name(check, name); g_free(name); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), existing ? json_object_get_boolean_member(existing, fields[i]) : TRUE);
        GtkWidget *label = gtk_bin_get_child(GTK_BIN(check));
        if (GTK_IS_LABEL(label)) { gtk_label_set_line_wrap(GTK_LABEL(label), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(label), 42); }
        g_object_set_data(G_OBJECT(dialog), fields[i], check); gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 4);
    }
    g_object_set_data_full(G_OBJECT(dialog), "event-id", g_strdup(view->current), g_free);
    g_signal_connect(dialog, "destroy", G_CALLBACK(rotation_closed), view);
    g_signal_connect(dialog, "response", G_CALLBACK(rotation_response), view);
    if (existing) {
        g_object_set_data_full(G_OBJECT(dialog), "rotation-id", g_strdup(rotation_id), g_free);
        g_object_set_data_full(G_OBJECT(dialog), "desired-playlist", g_strdup(radios_json_string(existing, "playlist_id", "")), g_free);
    }
    playlist_choices(view);
    if (existing) gtk_combo_box_set_active_id(GTK_COMBO_BOX(playlist), radios_json_string(existing, "playlist_id", ""));
    if (view->ctx->data) load_playlists(view->ctx, view->page);
    gtk_widget_show_all(dialog);
}
static void rotation_dialog(GtkButton *button, gpointer data)
{ library_create(button, data); }
static void edit_rotation(Calendar *view)
{
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view->rows));
    GList *paths = gtk_tree_selection_get_selected_rows(selection, NULL);
    if (g_list_length(paths) == 1) {
        GtkTreeIter iter; gchar *id = NULL;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(view->store), &iter, paths->data)) {
            gtk_tree_model_get(GTK_TREE_MODEL(view->store), &iter, 0, &id, -1); rotation_open(view, id); g_free(id);
        }
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
}
static void row_edit(GtkMenuItem *item, gpointer data)
{ (void)item; edit_rotation(data); }
static gboolean rows_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    Calendar *view = data;
    if (event->button != 3 && event->type != GDK_2BUTTON_PRESS) return FALSE;
    GtkTreePath *path = NULL; GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL)) {
        if (!gtk_tree_selection_path_is_selected(selection, path)) { gtk_tree_selection_unselect_all(selection); gtk_tree_selection_select_path(selection, path); }
        gtk_tree_path_free(path);
    }
    if (event->type == GDK_2BUTTON_PRESS && event->button == 1) { edit_rotation(view); return TRUE; }
    guint count = (guint)gtk_tree_selection_count_selected_rows(selection);
    GList *children = gtk_container_get_children(GTK_CONTAINER(view->row_menu));
    for (GList *child = children; child; child = child->next) {
        GtkWidget *item = child->data; const gchar *name = gtk_widget_get_name(item);
        if (!strcmp(name, "automation-calendar-row-edit")) gtk_widget_set_sensitive(item, count == 1);
        else if (!strcmp(name, "automation-calendar-row-paste")) gtk_widget_set_sensitive(item, view->row_clipboard != NULL);
        else if (!strcmp(name, "automation-calendar-row-up") || !strcmp(name, "automation-calendar-row-down")) gtk_widget_set_sensitive(item, count == 1);
        else if (!strcmp(name, "automation-calendar-row-remove") || !strcmp(name, "automation-calendar-row-copy")) gtk_widget_set_sensitive(item, count > 0);
    }
    g_list_free(children); gtk_widget_show_all(view->row_menu); gtk_menu_popup_at_pointer(GTK_MENU(view->row_menu), (GdkEvent *)event); return TRUE;
}
static void row_deleted(GtkTreeModel *model, GtkTreePath *path, gpointer data);
static gboolean sync_row_order(Calendar *view, GtkTreeModel *model)
{
    JsonObject *event = event_by_id(view, view->current); if (!event) return FALSE;
    JsonArray *old = json_object_get_array_member(event, "rotations"), *replacement = json_array_new(); GtkTreeIter cursor;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gboolean valid = gtk_tree_model_get_iter_first(model, &cursor), different = FALSE; guint index = 0;
    while (valid) {
        gchar *id = NULL; gtk_tree_model_get(model, &cursor, 0, &id, -1);
        for (guint i = 0; i < json_array_get_length(old); ++i)
            if (id && !g_hash_table_contains(seen, id) && !g_strcmp0(id, radios_json_string(json_array_get_object_element(old, i), "id", ""))) {
                json_array_add_element(replacement, deep_copy(json_array_get_element(old, i))); g_hash_table_add(seen, g_strdup(id));
                different |= i != index;
            }
        g_free(id); ++index; valid = gtk_tree_model_iter_next(model, &cursor);
    }
    g_hash_table_unref(seen);
    if (json_array_get_length(replacement) != json_array_get_length(old) || !different) { json_array_unref(replacement); return FALSE; }
    json_object_set_array_member(event, "rotations", replacement); return TRUE;
}
static void rows_reordered(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gint *order, gpointer data)
{
    (void)path; (void)iter; (void)order; Calendar *view = data;
    if (view->updating || view->disposed || view->closing) return;
    if (sync_row_order(view, model)) commit(view);
}
static gboolean reorder_idle(gpointer data)
{
    GtkWidget *page = data; Calendar *view = g_object_get_data(G_OBJECT(page), "automation-calendar");
    view->reorder_source = 0;
    if (!view->disposed && !view->closing && view->dirty) {
        if (view->autosave_source) { g_source_remove(view->autosave_source); view->autosave_source = 0; }
        submit(view); refresh(view);
    }
    return G_SOURCE_REMOVE;
}
static void row_deleted(GtkTreeModel *model, GtkTreePath *path, gpointer data)
{
    (void)path; Calendar *view = data;
    if (view->updating || view->disposed || view->closing || !sync_row_order(view, model)) return;
    /* Capture the completed order now, before a new selection can rebuild the
     * table. Rendering waits until GtkListStore finishes its delete operation. */
    if (!view->reorder_source)
        view->reorder_source = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, reorder_idle, g_object_ref(view->page), g_object_unref);
    changed(view);
}
static guint expanded_rows(Calendar *view, JsonArray *rows)
{
    guint count = 0;
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        const gchar *ref = radios_json_string(row, "rotation_id", "");
        JsonObject *definition = *ref ? library_by_id(view, ref) : NULL;
        if (*ref && !definition) return 51;
        JsonArray *sources = definition ? json_object_get_array_member(definition, "sources") : NULL;
        count += sources ? json_array_get_length(sources) : 1;
        if (count > 50) break;
    }
    return count;
}
static void row_action(GtkButton *button, gpointer data)
{
    Calendar *view = data; JsonObject *event = event_by_id(view, view->current); if (!event) return;
    gint action = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "action"));
    JsonArray *array = json_object_get_array_member(event, "rotations"); GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view->rows));
    GList *paths = gtk_tree_selection_get_selected_rows(selection, NULL);
    if (action == 3) {
        g_clear_pointer(&view->row_clipboard, json_node_free); view->row_clipboard = json_node_new(JSON_NODE_ARRAY); JsonArray *copy = json_array_new(); json_node_take_array(view->row_clipboard, copy);
        for (GList *item = paths; item; item = item->next) {
            gint index = gtk_tree_path_get_indices(item->data)[0]; json_array_add_element(copy, deep_copy(json_array_get_element(array, (guint)index)));
        }
        notice(view, "Seçili rotasyon eventleri kopyalandı; hedef programda Eventleri yapıştır'a basın.");
    } else if (action == 4) {
        JsonArray *copy = view->row_clipboard ? json_node_get_array(view->row_clipboard) : NULL;
        if (copy && json_array_get_length(copy) + json_array_get_length(array) <= 50 &&
            expanded_rows(view, copy) + expanded_rows(view, array) <= 50) {
            for (guint i = 0; i < json_array_get_length(copy); ++i) {
                JsonNode *node = deep_copy(json_array_get_element(copy, i)); gchar *id = g_uuid_string_random();
                json_object_set_string_member(json_node_get_object(node), "id", id); g_free(id); json_array_add_element(array, node);
            }
            commit(view);
        } else notice(view, "Yapıştırılacak event yok veya toplam 50 playlist sınırı aşılıyor.");
    } else if (paths) {
        if (action == 0) {
            for (GList *item = g_list_last(paths); item; item = item->prev) json_array_remove_element(array, (guint)gtk_tree_path_get_indices(item->data)[0]);
            commit(view);
        } else {
            guint index = (guint)gtk_tree_path_get_indices(paths->data)[0]; gint other = (gint)index + action;
            if (g_list_length(paths) == 1 && other >= 0 && other < (gint)json_array_get_length(array)) {
                JsonArray *replacement = json_array_new();
                for (guint i = 0; i < json_array_get_length(array); ++i)
                    json_array_add_element(replacement, deep_copy(json_array_get_element(array, i == index ? (guint)other : i == (guint)other ? index : i)));
                json_object_set_array_member(event, "rotations", replacement); commit(view);
                GtkTreePath *path = gtk_tree_path_new_from_indices(other, -1); gtk_tree_selection_select_path(selection, path); gtk_tree_path_free(path);
            }
        }
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
}
typedef struct {
    JsonNode *event, *diagnostics, *selection;
    RadioSDataClient *client;
    guint generation;
    gboolean programme;
    gchar loudness_document_id[37];
    gint64 loudness_revision;
} PreviewJob;
static void preview_job_free(gpointer data)
{
    PreviewJob *job = data; json_node_free(job->event);
    g_clear_pointer(&job->diagnostics, json_node_free); g_clear_pointer(&job->selection, json_node_free);
    radios_data_client_unref(job->client); g_free(job);
}
static void append_diagnostics(JsonArray *messages, JsonNode *node)
{
    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) return;
    JsonArray *array = json_node_get_array(node);
    for (guint i = 0; i < json_array_get_length(array); ++i)
        json_array_add_element(messages, deep_copy(json_array_get_element(array, i)));
}
static gchar *preview_notes(JsonNode *selection, JsonNode *verified, JsonNode *planned)
{
    guint unavailable = 0, boundary = 0, blocked = 0, other = 0;
    JsonNode *groups[] = { selection, verified, planned };
    for (guint group = 0; group < G_N_ELEMENTS(groups); ++group) {
        JsonNode *node = groups[group]; if (!node || !JSON_NODE_HOLDS_ARRAY(node)) continue;
        JsonArray *array = json_node_get_array(node);
        for (guint i = 0; i < json_array_get_length(array); ++i) {
            JsonNode *item = json_array_get_element(array, i);
            if (!JSON_NODE_HOLDS_OBJECT(item)) { ++other; continue; }
            JsonObject *message = json_node_get_object(item);
            const gchar *reason = radios_json_string(message, "reason", radios_json_string(message, "code", ""));
            if (g_str_equal(reason, "calendar_media_unavailable")) ++unavailable;
            else if (g_str_equal(reason, "calendar_end_boundary")) ++boundary;
            else if (g_str_equal(reason, "calendar_no_legal_candidate")) ++blocked;
            else ++other;
        }
    }
    if (!unavailable && !boundary && !blocked && !other) return NULL;
    GString *notes = g_string_new(NULL);
    if (unavailable) g_string_append_printf(notes, "%u medya kaydı doğrulanamadığı için atlandı. ", unavailable);
    if (boundary) g_string_append_printf(notes, "%u sonraki öğe blok sonunu aşacağı için eklenmedi. ", boundary);
    if (blocked) g_string_append(notes, "Tekrar, saat, sıklık veya playlist aralığı kuralları nedeniyle uygun parça kalmadı. ");
    if (other) g_string_append_printf(notes, "%u ek hazırlık bildirimi var.", other);
    g_strchomp(notes->str); return g_string_free(notes, FALSE);
}
static gboolean preview_input_matches(Calendar *view, JsonNode *event, RadioSDataClient *client)
{
    JsonObject *current = event_by_id(view, view->current); if (!current || !radios_data_connected(view->ctx)) return FALSE;
    JsonNode *node = radios_automation_calendar_resolve_event(json_node_get_object(view->config), current, NULL);
    gboolean equal = node && json_node_equal(node, event); g_clear_pointer(&node, json_node_free);
    RadioSDataClient *actual = radios_data_client_acquire(view->ctx);
    gboolean identity = actual == client; if (actual) radios_data_client_unref(actual); return equal && identity;
}
static void preview_dialog(Calendar *view)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Program önizlemesi", view->ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Kapat", GTK_RESPONSE_CLOSE, NULL);
    gtk_widget_set_name(dialog, "automation-calendar-preview-dialog"); gtk_window_set_default_size(GTK_WINDOW(dialog), 900, 600);
    const gchar *columns[] = { "Sıra / saat", "Parça", "Sanatçı", "Tür", "Süre" };
    GtkWidget *table = radios_ui_table(columns, G_N_ELEMENTS(columns));
    GtkListStore *store = g_object_get_data(G_OBJECT(table), "list-store");
    JsonArray *entries = json_object_get_array_member(json_node_get_object(view->plan->display), "entries");
    GTimeZone *zone = radios_time_zone_new(calendar_timezone(view));
    for (guint i = 0; entries && i < json_array_get_length(entries); ++i) {
        JsonObject *entry = json_array_get_object_element(entries, i); GtkTreeIter iter;
        GDateTime *utc = g_date_time_new_from_iso8601(radios_json_string(entry, "start_utc", ""), NULL);
        GDateTime *local = utc && zone ? g_date_time_to_timezone(utc, zone) : NULL;
        gchar *stamp = local ? g_date_time_format(local, "%d.%m.%Y %H:%M:%S") : g_strdup(radios_json_string(entry, "start_utc", ""));
        gchar *order = g_strdup_printf("%u · %s", i + 1, stamp); g_free(stamp);
        if (local) g_date_time_unref(local);
        if (utc) g_date_time_unref(utc);
        gint64 duration = radios_json_int(entry, "duration_ms", 0);
        gchar *time = g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, duration / 60000, duration / 1000 % 60);
        gtk_list_store_append(store, &iter); gtk_list_store_set(store, &iter, 0, order,
            1, radios_json_string(entry, "title", ""), 2, radios_json_string(entry, "artist", ""),
            3, radios_json_string(entry, "kind", ""), 4, time, -1); g_free(order); g_free(time);
    }
    if (zone) g_time_zone_unref(zone);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    if (view->preview_notes) {
        GtkWidget *notes = radios_ui_label(view->preview_notes, "task-help");
        gtk_widget_set_name(notes, "automation-calendar-preview-notes");
        gtk_label_set_line_wrap(GTK_LABEL(notes), TRUE);
        gtk_box_pack_start(GTK_BOX(box), notes, FALSE, FALSE, 8);
    }
    gtk_box_pack_start(GTK_BOX(box), table, TRUE, TRUE, 0);
    g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);
    g_signal_connect_object(view->page, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED); gtk_widget_show_all(dialog);
}
static gint64 event_start_us(JsonObject *event)
{ return radios_automation_calendar_event_time(event, (guint)radios_json_int(event, "start_minute", 0)); }
static void preview_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    Calendar *view = g_object_get_data(G_OBJECT(owner), "automation-calendar"); PreviewJob *job = data;
    if (job->generation != view->preview_generation) return;
    if (!preview_input_matches(view, job->event, job->client)) {
        invalidate_preview(view); notice(view, "Program veya veri oturumu değişti; önizlemeyi yeniden hazırlayın."); return;
    }
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        invalidate_preview(view); notice(view, error ? error->message : "Program hazırlığı alınamadı."); return;
    }
    GError *failure = NULL; JsonObject *event = json_node_get_object(job->event);
    if (!job->programme) {
        JsonNode *sources = deep_copy(result), *checkpoint = radios_playout_ui_probability_checkpoint(ctx, TRUE);
        if (checkpoint) json_object_set_member(json_node_get_object(sources), "policy_selection_checkpoint", checkpoint);
        JsonNode *selection = radios_automation_calendar_prepared_programme(event, json_node_get_object(sources),
            radios_data_station_id(ctx), radios_json_string(event, "id", ""), 200, &failure);
        json_node_free(sources);
        if (!selection) { invalidate_preview(view); notice(view, failure ? failure->message : "Program sırası üretilemedi."); g_clear_error(&failure); return; }
        JsonObject *selected = json_node_get_object(selection);
        JsonNode *programme = deep_copy(json_object_get_member(selected, "config"));
        JsonNode *body = radios_data_body(ctx); JsonObject *object = json_node_get_object(body);
        json_object_set_string_member(object, "kind", "programme"); json_object_set_member(object, "config", programme);
        PreviewJob *next = g_new0(PreviewJob, 1); next->event = deep_copy(job->event); next->client = radios_data_client_ref(job->client);
        next->generation = job->generation; next->programme = TRUE;
        g_strlcpy(next->loudness_document_id, job->loudness_document_id, sizeof next->loudness_document_id);
        next->loudness_revision = job->loudness_revision;
        next->diagnostics = deep_copy(json_object_get_member(selected, "diagnostics"));
        JsonArray *messages = json_node_get_array(next->diagnostics);
        append_diagnostics(messages, json_object_get_member(json_node_get_object(result), "diagnostics"));
        next->selection = selection;
        gboolean accepted = radios_data_request(ctx, owner, "POST", "/v1/workflows/desktop-playout/prepare", body, preview_received, next, preview_job_free);
        json_node_free(body);
        if (!accepted) { invalidate_preview(view); notice(view, "Program medyası yeniden doğrulanamadı."); }
        return;
    }
    RadioSPlayoutRequest request = { .station_id = radios_data_station_id(ctx), .database_file = ctx->config->database_file,
        .mode = 0, .now_utc_us = g_get_real_time(), .start_utc_us = event_start_us(event) };
    JsonNode *merged = radios_automation_calendar_merge_selection(json_node_get_object(result), json_node_get_object(job->selection), &failure);
    JsonNode *checkpoint = radios_playout_ui_probability_checkpoint(ctx, FALSE);
    if (merged && checkpoint) json_object_set_member(json_node_get_object(merged), "probability_checkpoint", checkpoint); else g_clear_pointer(&checkpoint, json_node_free);
    RadioSPlayoutPlan *plan = merged ? radios_playout_plan_build(json_node_get_object(merged), &request, &failure) : NULL;
    if (!plan) { g_clear_pointer(&merged, json_node_free); invalidate_preview(view); notice(view, failure ? failure->message : "Doğrulanmış önizleme üretilemedi."); g_clear_error(&failure); return; }
    if (job->loudness_revision > 0 &&
        radios_data_document_revision(calendar_active_document(view)) == job->loudness_revision &&
        g_str_equal(radios_data_document_id(calendar_active_document(view)), job->loudness_document_id)) {
        /* Local provenance only: capture before the async prepare, then bind
         * it to the same acknowledged record. Editor drafts stay untagged. */
        json_object_set_string_member(json_node_get_object(merged), "loudness_automation_document_id", job->loudness_document_id);
        json_object_set_int_member(json_node_get_object(merged), "loudness_automation_document_revision", job->loudness_revision);
    }
    view->plan = plan; view->prepared = merged; view->preview_event = deep_copy(job->event);
    view->preview_client = radios_data_client_ref(job->client); view->preview_busy = FALSE;
    view->preview_notes = preview_notes(job->diagnostics, json_object_get_member(json_node_get_object(result), "diagnostics"),
        json_object_get_member(json_node_get_object(plan->display), "diagnostics"));
    gtk_widget_set_sensitive(view->preview_button, TRUE); gtk_widget_set_sensitive(view->append_button, plan->entries->len > 0);
    gtk_widget_set_sensitive(view->schedule_button, ctx->playout != NULL && plan->entries->len > 0);
    gchar *message = g_strdup_printf("%u öğe hazır. Rotasyon sıklığı ve ayrı parça/reklam/sweeper seçimleri uygulandı.", plan->entries->len);
    notice(view, message); g_free(message); preview_dialog(view);
}
static void preview_selected(GtkButton *button, gpointer data)
{
    (void)button; Calendar *view = data; JsonObject *event = event_by_id(view, view->current); if (!event || view->preview_busy) return;
    if (!radios_data_connected(view->ctx)) { notice(view, "Önizleme için veritabanı bağlantısı gerekli."); return; }
    invalidate_preview(view);
    GtkWidget *legacy = g_object_get_data(G_OBJECT(view->page), "legacy-rules");
    JsonNode *defaults = legacy ? radios_automation_rules_configuration(legacy) : NULL; GError *error = NULL;
    JsonNode *resolved = radios_automation_calendar_resolve_event(json_node_get_object(view->config), event, &error);
    JsonNode *config = resolved ? radios_automation_calendar_prepare_config(json_node_get_object(resolved), defaults ? json_node_get_object(defaults) : NULL, &error) : NULL;
    g_clear_pointer(&defaults, json_node_free);
    if (!config) { g_clear_pointer(&resolved, json_node_free); notice(view, error ? error->message : "Rotasyon hazırlığı geçersiz."); g_clear_error(&error); return; }
    JsonNode *body = radios_data_body(view->ctx); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "automation"); json_object_set_member(object, "config", config);
    PreviewJob *job = g_new0(PreviewJob, 1); job->event = resolved;
    job->generation = view->preview_generation; job->client = radios_data_client_acquire(view->ctx);
    job->loudness_revision = radios_data_document_revision(calendar_active_document(view));
    if (job->loudness_revision > 0)
        g_strlcpy(job->loudness_document_id, radios_data_document_id(calendar_active_document(view)), sizeof job->loudness_document_id);
    view->preview_busy = TRUE; gtk_widget_set_sensitive(view->preview_button, FALSE);
    gboolean accepted = radios_data_request(view->ctx, view->page, "POST", "/v1/workflows/desktop-playout/prepare", body, preview_received, job, preview_job_free);
    json_node_free(body); if (!accepted) invalidate_preview(view); else notice(view, "Programın playlist ve medya kayıtları doğrulanıyor…");
}
static void preview_append(GtkButton *button, gpointer data)
{
    (void)button; Calendar *view = data; if (!view->prepared || !view->preview_event || !view->plan) return;
    if (!preview_input_matches(view, view->preview_event, view->preview_client)) { invalidate_preview(view); notice(view, "Önizleme değişti; yeniden hazırlayın."); return; }
    GError *error = NULL; JsonArray *entries = json_object_get_array_member(json_node_get_object(view->plan->display), "entries");
    if (!radios_broadcast_append_event(view->ctx, entries, radios_json_string(json_node_get_object(view->preview_event), "title", ""), &error))
        notice(view, error ? error->message : "Yayın sırası hazır değil.");
    else notice(view, "Doğrulanmış takvim programı yayın sırasına ekleniyor.");
    g_clear_error(&error);
}
static void preview_schedule(GtkButton *button, gpointer data)
{
    (void)button; Calendar *view = data; if (!view->prepared || !view->preview_event) return;
    if (!preview_input_matches(view, view->preview_event, view->preview_client)) { invalidate_preview(view); notice(view, "Önizleme değişti; yeniden hazırlayın."); return; }
    gint64 start = event_start_us(json_node_get_object(view->preview_event));
    if (start <= g_get_real_time()) { notice(view, "Zamanlama için gelecekteki bir program aralığı seçin."); return; }
    GError *error = NULL;
    JsonObject *prepared = json_node_get_object(view->prepared);
    const gchar *origin = radios_json_string(prepared, "loudness_automation_document_id", "");
    gint64 revision = radios_json_int(prepared, "loudness_automation_document_revision", 0);
    if (revision <= 0 || revision != radios_data_document_revision(calendar_active_document(view)) ||
        g_strcmp0(origin, radios_data_document_id(calendar_active_document(view)))) origin = NULL;
    if (!radios_playout_ui_start_prepared_with_loudness_origin(view->ctx, view->page, prepared, start, origin, FALSE, &error))
        notice(view, error ? error->message : "Program zamanlanamadı.");
    else notice(view, "Program seçili takvim başlangıç saati için zamanlandı.");
    g_clear_error(&error);
}
/* Kılavuz §02 "Mevcut şemayı uygula": the broadcast page keeps the general
 * programme driver on its root widget; find it through the shared window. */
static GtkWidget *descendant_with_data(GtkWidget *root, const gchar *key)
{
    GQueue pending = G_QUEUE_INIT; GtkWidget *found = NULL;
    if (root) g_queue_push_tail(&pending, root);
    while (!found && !g_queue_is_empty(&pending)) {
        GtkWidget *widget = g_queue_pop_head(&pending);
        if (g_object_get_data(G_OBJECT(widget), key)) { found = widget; break; }
        if (!GTK_IS_CONTAINER(widget)) continue;
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *it = children; it; it = it->next) g_queue_push_tail(&pending, it->data);
        g_list_free(children);
    }
    g_queue_clear(&pending); return found;
}
static RadioSPlayerWeekly *general_driver(Calendar *view)
{
    GtkWidget *root = view->ctx->parent ? GTK_WIDGET(view->ctx->parent) : NULL;
    GtkWidget *owner = descendant_with_data(root, "radios-general-programme-driver");
    return owner ? g_object_get_data(G_OBJECT(owner), "radios-general-programme-driver") : NULL;
}
/* The selected library schema first, then the selected programme's schema
 * (its live event schema or its first assigned schema definition). Owned. */
static gchar *apply_schema_choice(Calendar *view)
{
    GtkTreeSelection *selection = view->library_rows ? gtk_tree_view_get_selection(GTK_TREE_VIEW(view->library_rows)) : NULL;
    GList *paths = selection ? gtk_tree_selection_get_selected_rows(selection, NULL) : NULL; gchar *chosen = NULL;
    for (GList *it = paths; it && !chosen; it = it->next) {
        GtkTreeIter iter; gchar *id = NULL;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(view->library_store), &iter, it->data)) gtk_tree_model_get(GTK_TREE_MODEL(view->library_store), &iter, 0, &id, -1);
        if (definition_schema(library_by_id(view, id))) chosen = id; else g_free(id);
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    JsonObject *event = event_by_id(view, view->current), *live = event_live(event);
    const gchar *automation = radios_json_string(live, "automation_id", "");
    if (!chosen && definition_schema(library_by_id(view, automation))) chosen = g_strdup(automation);
    JsonArray *rows = event ? json_object_get_array_member(event, "rotations") : NULL;
    for (guint i = 0; !chosen && rows && i < json_array_get_length(rows); ++i) {
        const gchar *id = radios_json_string(json_array_get_object_element(rows, i), "rotation_id", "");
        if (definition_schema(library_by_id(view, id))) chosen = g_strdup(id);
    }
    return chosen;
}
static void apply_schema(GtkButton *button, gpointer data)
{
    (void)button; Calendar *view = data;
    RadioSPlayerWeekly *driver = general_driver(view);
    if (!driver) { notice(view, "Çalan genel program yok."); return; }
    gchar *id = apply_schema_choice(view);
    if (!id) { notice(view, "Uygulanacak rotasyon şemasını seçin."); return; }
    GError *error = NULL;
    if (!radios_player_weekly_general_apply_schema(driver, id, &error)) notice(view, error ? error->message : "Şema çalan programa uygulanamadı.");
    else {
        RadioSPlayerWeeklySnapshot snapshot = {0}; radios_player_weekly_get_snapshot(driver, &snapshot);
        gchar *message = g_strdup_printf("Şema çalan programa uygulandı: %s%s%s", snapshot.schema_name,
            snapshot.entry_text[0] ? " · " : "", snapshot.entry_text);
        notice(view, message); g_free(message);
    }
    g_clear_error(&error); g_free(id);
}
static void programme_properties(GtkMenuItem *item, gpointer data)
{ (void)item; programme_dialog(NULL, data); }
static GtkWidget *button_new(const gchar *label, const gchar *name, GCallback callback, Calendar *view)
{
    GtkWidget *button = radios_ui_button(label); gtk_widget_set_name(button, name);
    if (callback) g_signal_connect(button, "clicked", callback, view);
    return button;
}
static void G_GNUC_UNUSED detail_columns_changed(GtkToggleButton *button, gpointer data)
{
    Calendar *view = data;
    GtkWidget *tables[] = {view->rows, view->library_rows};
    for (guint i = 0; i < G_N_ELEMENTS(tables); ++i) {
        GList *columns = gtk_tree_view_get_columns(GTK_TREE_VIEW(tables[i]));
        guint index = 0;
        for (GList *it = columns; it; it = it->next, ++index)
            if (index >= 2) gtk_tree_view_column_set_visible(it->data, gtk_toggle_button_get_active(button));
        g_list_free(columns);
    }
}
static GtkWidget *simple_section(const gchar *title, const gchar *name)
{
    GtkWidget *section = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "ac-simple-section");
    gtk_widget_set_name(section, name);
    gtk_box_pack_start(GTK_BOX(section), radios_ui_label(title, "ac-section-title"), FALSE, FALSE, 0);
    return section;
}
static void update_detail_heading(Calendar *view)
{
    if (!view->detail_title) return;
    gtk_label_set_text(GTK_LABEL(view->detail_title), "Özellikler");
    JsonObject *event = event_by_id(view, view->current);
    gchar *caption = event && view->programme_title && *view->programme_title ?
        g_strdup_printf("%s · %s", view->programme_title, radios_json_string(event, "title", "")) :
        g_strdup(event ? radios_json_string(event, "title", "") : view->programme_title && *view->programme_title ? view->programme_title : "Takvimden bir blok veya saat aralığı seçin.");
    gtk_label_set_text(GTK_LABEL(view->detail_subtitle), caption); g_free(caption);
}
static void show_programme(Calendar *view)
{ update_detail_heading(view); }
static void split_mapped(GtkWidget *widget, gpointer data)
{
    /* GtkStack also allocates hidden pages; initialize from the first visible
     * allocation and retain the operator's handle position on later maps. */
    Calendar *view = data; gint width = gtk_widget_get_allocated_width(widget);
    if (view->split_initialized || width < 400) return;
    view->split_initialized = TRUE;
    gtk_paned_set_position(GTK_PANED(widget), MAX(260, width - 500));
}
static void calendar_style(Calendar *view)
{
    if (!view->ctx->config || !view->ctx->config->assets_root) return;
    gchar *path = g_build_filename(view->ctx->config->assets_root, "themes", "automation-calendar.css", NULL);
    GtkCssProvider *provider = gtk_css_provider_new(); GError *error = NULL;
    if (gtk_css_provider_load_from_path(provider, path, &error)) {
        view->style = provider; view->style_screen = g_object_ref(gtk_widget_get_screen(view->page));
        gtk_style_context_add_provider_for_screen(view->style_screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    } else {
        g_warning("Automation calendar theme: %s", error->message); g_clear_error(&error); g_object_unref(provider);
    }
    g_free(path);
}
static GtkWidget *calendar_new(RadioSViewContext *ctx, GtkWidget *legacy_rules, gboolean detached)
{
    Calendar *view = g_new0(Calendar, 1); view->ctx = ctx; view->step = 30; view->detached = detached;
    view->selected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    view->source_problems = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    JsonObject *config = json_object_new(); json_object_set_int_member(config, "schema", 1); json_object_set_array_member(config, "events", json_array_new());
    json_object_set_string_member(config, "timezone", radios_station_timezone(ctx));
    view->config = object_node(config); json_object_unref(config);
    GtkWidget *page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "task-view automation-calendar"); view->page = page;
    gtk_widget_set_name(page, "automation-calendar"); g_object_set_data_full(G_OBJECT(page), "automation-calendar", view, free_view);
    g_signal_connect(page, "destroy", G_CALLBACK(destroyed), view);
    GtkWidget *workspace = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "automation-calendar-workspace");
    gtk_widget_set_name(workspace, "automation-calendar-workspace");
    GtkWidget *bar = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "ac-calendar-bar");
    const gchar *nav_labels[] = { "‹", "Bugün", "›", "+", "−" }, *nav_names[] = {
        "automation-calendar-prev", "automation-calendar-today", "automation-calendar-next", "automation-calendar-zoom-in", "automation-calendar-zoom-out" };
    const gint directions[] = { -1, 0, 1, 1, -1 };
    for (guint i = 0; i < G_N_ELEMENTS(nav_labels); ++i) {
        GtkWidget *button = button_new(nav_labels[i], nav_names[i], i < 3 ? G_CALLBACK(navigate) : G_CALLBACK(zoom), view);
        g_object_set_data(G_OBJECT(button), "direction", GINT_TO_POINTER(directions[i])); gtk_box_pack_start(GTK_BOX(bar), button, FALSE, FALSE, 0);
    }
    view->date = radios_ui_entry("YYYY-AA-GG"); gtk_widget_set_name(view->date, "automation-calendar-date"); gtk_entry_set_width_chars(GTK_ENTRY(view->date), 10);
    gtk_entry_set_max_length(GTK_ENTRY(view->date), 10); gtk_widget_set_tooltip_text(view->date, "Görüntülenecek hafta · UTC");
    g_signal_connect(view->date, "activate", G_CALLBACK(date_changed), view); gtk_box_pack_start(GTK_BOX(bar), view->date, FALSE, FALSE, 0);
    gtk_widget_set_name(bar, "automation-calendar-tools");
    view->week_range = radios_ui_label("", "ac-week-range"); gtk_widget_set_name(view->week_range, "automation-calendar-week-range");
    gtk_label_set_ellipsize(GTK_LABEL(view->week_range), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(bar), view->week_range, TRUE, TRUE, 4);
    GtkWidget *selection_bar = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "ac-selection-bar");
    gtk_widget_set_name(selection_bar, "automation-calendar-selection-tools");
    view->selection = radios_ui_label("", NULL); gtk_label_set_ellipsize(GTK_LABEL(view->selection), PANGO_ELLIPSIZE_END);
    gtk_widget_set_name(view->selection, "automation-calendar-selection"); gtk_box_pack_start(GTK_BOX(selection_bar), view->selection, TRUE, TRUE, 0);
    view->save_selection = button_new("Program oluştur", "automation-calendar-save-selection", G_CALLBACK(programme_dialog), view);
    view->independent = button_new("Bağımsız rotasyon oluştur", "automation-calendar-independent-rotation", G_CALLBACK(library_create), view);
    view->schema = button_new("Şema Ayarları", "automation-calendar-create-rotation-schema", G_CALLBACK(schema_settings), view);
    view->apply_schema = button_new("Mevcut şemayı uygula", "automation-calendar-apply-schema", G_CALLBACK(apply_schema), view);
    gtk_widget_set_tooltip_text(view->apply_schema, "Seçili rotasyon şemasını çalan genel programa uygular.");
    gtk_widget_set_tooltip_text(view->save_selection, "Takvimde seçtiğiniz saat aralığına bir program bloğu ekler. Program adı listeden seçilebilir.");
    gtk_widget_set_tooltip_text(view->independent, "Takvim saatine bağlı olmayan bir rotasyon tanımlar. Kayıtlı rotasyonlar listesine eklenir; istediğiniz programa atarsınız.");
    gtk_widget_set_tooltip_text(view->schema, "Playlist ve live event sırasını, oynatıcı kümelerini ve adımlar arasında atanmış otomasyonları düzenler.");
    gtk_box_pack_end(GTK_BOX(selection_bar), view->apply_schema, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(selection_bar), view->schema, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(selection_bar), view->independent, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(selection_bar), view->save_selection, FALSE, FALSE, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(view->save_selection), "ac-primary");
    gtk_box_pack_start(GTK_BOX(workspace), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(workspace), selection_bar, FALSE, FALSE, 0);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL); gtk_widget_set_name(paned, "automation-calendar-split"); gtk_widget_set_vexpand(paned, TRUE);
    g_signal_connect(paned, "map", G_CALLBACK(split_mapped), view);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    view->grid = gtk_drawing_area_new(); gtk_widget_set_name(view->grid, "automation-calendar-grid"); gtk_widget_set_size_request(view->grid, 700, (gint)minute_y(view, 1440));
    gtk_widget_set_can_focus(view->grid, TRUE); atk_object_set_name(gtk_widget_get_accessible(view->grid), "Haftalık otomasyon takvimi · gün ve saat hücreleri");
    gtk_widget_add_events(view->grid, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(view->grid, "draw", G_CALLBACK(draw), view); g_signal_connect(view->grid, "button-press-event", G_CALLBACK(press), view);
    g_signal_connect(view->grid, "motion-notify-event", G_CALLBACK(motion), view); g_signal_connect(view->grid, "button-release-event", G_CALLBACK(release), view);
    g_signal_connect(view->grid, "key-press-event", G_CALLBACK(key_press), view); gtk_container_add(GTK_CONTAINER(scroll), view->grid);
    GtkWidget *calendar_box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, NULL);
    view->header = gtk_drawing_area_new(); gtk_widget_set_name(view->header, "automation-calendar-days"); gtk_widget_set_size_request(view->header, -1, 42);
    GtkAdjustment *horizontal = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroll));
    g_object_set_data(G_OBJECT(view->header), "horizontal", horizontal);
    g_signal_connect(view->header, "draw", G_CALLBACK(draw), view);
    g_signal_connect_object(horizontal, "value-changed", G_CALLBACK(header_scrolled), view->header, 0);
    g_signal_connect(view->grid, "size-allocate", G_CALLBACK(grid_resized), view);
    gtk_box_pack_start(GTK_BOX(calendar_box), view->header, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(calendar_box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(workspace), calendar_box, TRUE, TRUE, 0);
    GtkWidget *side = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, "ac-side-panel");
    gtk_widget_set_name(side, "automation-calendar-side-panel"); gtk_widget_set_size_request(side, 480, -1);
    g_object_set_data(G_OBJECT(page), "automation-calendar-side-panel", side);
    GtkWidget *panel_heading = radios_ui_box(GTK_ORIENTATION_VERTICAL, 5, "ac-panel-heading");
    view->detail_title = radios_ui_label("", "heading");
    gtk_widget_set_name(view->detail_title, "automation-calendar-detail-title");
    gtk_label_set_ellipsize(GTK_LABEL(view->detail_title), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(view->detail_title), 32);
    view->detail_subtitle = radios_ui_label("", "small"); gtk_widget_set_name(view->detail_subtitle, "automation-calendar-detail-subtitle");
    gtk_label_set_line_wrap(GTK_LABEL(view->detail_subtitle), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(view->detail_subtitle), 45);
    gtk_box_pack_start(GTK_BOX(panel_heading), view->detail_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panel_heading), view->detail_subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(side), panel_heading, FALSE, FALSE, 0);
    GtkWidget *library_bar = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "ac-library-picker");
    gtk_widget_set_name(library_bar, "automation-calendar-available-rotations-panel");
    GtkWidget *library_heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
    gtk_box_pack_start(GTK_BOX(library_heading), radios_ui_label("Bir veya daha fazla rotasyon seçin", "small"), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(library_bar), library_heading, FALSE, FALSE, 0);
    GtkWidget *library_buttons = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, NULL);
    view->library_choice = gtk_combo_box_text_new(); gtk_widget_set_name(view->library_choice, "automation-calendar-existing-rotation");
    gtk_widget_set_hexpand(view->library_choice, TRUE);
    GList *library_cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(view->library_choice));
    for (GList *cell = library_cells; cell; cell = cell->next)
        if (GTK_IS_CELL_RENDERER_TEXT(cell->data)) g_object_set(cell->data, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 18, NULL);
    g_list_free(library_cells);
    view->library_edit = button_new("Düzenle", "automation-calendar-manage-rotation", G_CALLBACK(library_edit), view);
    view->library_assign = button_new("Programa ekle", "automation-calendar-assign-rotation", G_CALLBACK(library_assign), view);
    g_signal_connect(view->library_choice, "changed", G_CALLBACK(library_choice_changed), view);
    gtk_box_pack_start(GTK_BOX(library_buttons), view->library_choice, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(library_buttons), view->library_edit, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(view->library_choice, TRUE); gtk_widget_hide(view->library_choice);
    gtk_widget_set_no_show_all(view->library_edit, TRUE); gtk_widget_hide(view->library_edit);
    gtk_box_pack_end(GTK_BOX(library_heading), view->library_assign, FALSE, FALSE, 0);
    gtk_widget_set_tooltip_text(view->library_rows ? view->library_rows : view->library_assign, "Birden fazla rotasyonu Ctrl veya Shift ile seçip programa ata.");
    view->library_store = gtk_list_store_new(8, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    view->library_rows = gtk_tree_view_new_with_model(GTK_TREE_MODEL(view->library_store)); g_object_unref(view->library_store);
    gtk_widget_set_name(view->library_rows, "automation-calendar-available-rotations");
    gtk_widget_set_tooltip_text(view->library_rows, "Birden fazla rotasyonu Ctrl veya Shift ile seçip programa ata; çift tıklayarak düzenle.");
    GtkTreeSelection *available_selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view->library_rows));
    gtk_tree_selection_set_mode(available_selection, GTK_SELECTION_MULTIPLE);
    g_signal_connect(available_selection, "changed", G_CALLBACK(library_selection_changed), view);
    g_signal_connect(view->library_rows, "row-activated", G_CALLBACK(library_row_activated), view);
    const gchar *available_columns[] = {"Rotasyon", "İçerik"};
    for (guint i = 0; i < G_N_ELEMENTS(available_columns); ++i) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(available_columns[i], renderer, "text", (gint)i + 1, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE); gtk_tree_view_column_set_min_width(column, i ? 68 : 140);
        gtk_tree_view_column_set_expand(column, i == 0);
        gtk_tree_view_column_set_visible(column, i < 2);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view->library_rows), column);
    }
    GtkWidget *available_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(available_scroll, -1, 100);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(available_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(available_scroll), view->library_rows);
    gtk_box_pack_start(GTK_BOX(library_bar), available_scroll, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(library_bar), library_buttons, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(library_buttons, TRUE); gtk_widget_hide(library_buttons);
    GtkWidget *programme_page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, NULL);
    gtk_widget_set_vexpand(programme_page, TRUE);
    gtk_box_pack_start(GTK_BOX(side), programme_page, TRUE, TRUE, 0);
    view->empty = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "ac-empty-state");
    gtk_widget_set_valign(view->empty, GTK_ALIGN_CENTER); gtk_widget_set_vexpand(view->empty, TRUE);
    GtkWidget *empty_title = radios_ui_label("Önce saat seçin", "heading");
    gtk_box_pack_start(GTK_BOX(view->empty), empty_title, FALSE, FALSE, 0);
    gtk_widget_show_all(view->empty); gtk_widget_set_no_show_all(view->empty, TRUE);
    gtk_box_pack_start(GTK_BOX(programme_page), view->empty, TRUE, TRUE, 0);
    view->editor = radios_ui_box(GTK_ORIENTATION_VERTICAL, 10, "automation-calendar-programme ac-programme-editor");
    gtk_widget_set_name(view->editor, "automation-calendar-programme-editor"); gtk_widget_set_vexpand(view->editor, TRUE);
    GtkWidget *programme_section = simple_section("1  Program ve saat", "automation-calendar-programme-section");
    GtkWidget *identity = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "ac-programme-identity");
    GtkWidget *name_field = radios_ui_box(GTK_ORIENTATION_VERTICAL, 5, NULL), *color_field = radios_ui_box(GTK_ORIENTATION_VERTICAL, 5, NULL);
    gtk_box_pack_start(GTK_BOX(name_field), radios_ui_label("Program adı", "small"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(color_field), radios_ui_label("Takvim rengi", "small"), FALSE, FALSE, 0);
    view->name = radios_ui_entry("Program adı"); gtk_widget_set_name(view->name, "automation-calendar-programme-name"); gtk_entry_set_max_length(GTK_ENTRY(view->name), 160);
    const gchar *colors[] = { "Açık mavi", "Yeşil", NULL }; view->color = radios_ui_combo(colors, 0);
    g_signal_connect(view->name, "changed", G_CALLBACK(inspector_changed), view); g_signal_connect(view->color, "changed", G_CALLBACK(inspector_changed), view);
    gtk_entry_set_width_chars(GTK_ENTRY(view->name), 16);
    gtk_box_pack_start(GTK_BOX(name_field), view->name, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(color_field), view->color, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(identity), name_field, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(programme_section), identity, FALSE, FALSE, 0);
    GtkWidget *times = gtk_grid_new(); gtk_style_context_add_class(gtk_widget_get_style_context(times), "ac-time-fields");
    gtk_grid_set_column_spacing(GTK_GRID(times), 10); gtk_grid_set_row_spacing(GTK_GRID(times), 5);
    const gchar *captions[] = { "Tarih", "Başlangıç", "Bitiş" };
    const gchar *field_names[] = { "automation-calendar-programme-date", "automation-calendar-programme-start", "automation-calendar-programme-end" };
    GtkWidget **fields[] = { &view->programme_date, &view->programme_start, &view->programme_end };
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) {
        GtkWidget *caption = radios_ui_label(captions[i], "small");
        if (!i) view->timezone_label = caption;
        gtk_grid_attach(GTK_GRID(times), caption, (gint)i, 0, 1, 1);
        *fields[i] = radios_ui_entry(i ? "SS:DD" : "YYYY-AA-GG"); gtk_widget_set_name(*fields[i], field_names[i]);
        gtk_entry_set_width_chars(GTK_ENTRY(*fields[i]), i ? 5 : 10); gtk_entry_set_max_length(GTK_ENTRY(*fields[i]), i ? 5 : 10);
        gtk_widget_set_hexpand(*fields[i], TRUE); g_signal_connect(*fields[i], "changed", G_CALLBACK(inspector_changed), view);
        gtk_grid_attach(GTK_GRID(times), *fields[i], (gint)i, 1, 1, 1);
    }
    gtk_widget_set_tooltip_text(view->programme_end, "Gün sonu için 24:00 kullanılabilir.");
    gtk_box_pack_start(GTK_BOX(programme_section), times, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(view->editor), programme_section, FALSE, FALSE, 0);
    view->content_warning = radios_ui_label("", "error");
    gtk_widget_set_name(view->content_warning, "automation-calendar-content-warning");
    gtk_label_set_xalign(GTK_LABEL(view->content_warning), 0); gtk_label_set_line_wrap(GTK_LABEL(view->content_warning), FALSE);
    gtk_label_set_max_width_chars(GTK_LABEL(view->content_warning), 48);
    gtk_label_set_lines(GTK_LABEL(view->content_warning), 2); gtk_label_set_ellipsize(GTK_LABEL(view->content_warning), PANGO_ELLIPSIZE_END);
    gtk_widget_set_no_show_all(view->content_warning, TRUE);
    gtk_box_pack_start(GTK_BOX(view->editor), view->content_warning, FALSE, FALSE, 0);
    GtkWidget *rotation_section = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "ac-simple-section");
    gtk_widget_set_name(rotation_section, "automation-calendar-rotation-section");
    GtkWidget *rotation_heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "ac-rotation-heading");
    view->rotation_title = radios_ui_label("2  Rotasyonlar", "ac-section-title"); gtk_widget_set_name(view->rotation_title, "automation-calendar-rotation-title");
    gtk_box_pack_start(GTK_BOX(rotation_heading), view->rotation_title, TRUE, TRUE, 0);
    view->rotation_count = radios_ui_label("", "small"); gtk_widget_set_name(view->rotation_count, "automation-calendar-rotation-count");
    gtk_box_pack_end(GTK_BOX(rotation_heading), view->rotation_count, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rotation_section), rotation_heading, FALSE, FALSE, 0);
    GtkWidget *mode_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
    gtk_box_pack_start(GTK_BOX(mode_row), radios_ui_label("Rotasyonlar arası seçim", "small"), FALSE, FALSE, 0);
    const gchar *modes[] = {"Doğrusal sıra", "Ağırlıklı olasılıklı", NULL};
    view->rotation_mode = radios_ui_combo(modes, 0); gtk_widget_set_name(view->rotation_mode, "automation-calendar-rotation-mode");
    gtk_box_pack_start(GTK_BOX(mode_row), view->rotation_mode, TRUE, TRUE, 0);
    g_signal_connect(view->rotation_mode, "changed", G_CALLBACK(rotation_mode_changed), view);
    GtkWidget *probability = button_new("Olasılık", "automation-calendar-programme-probability", G_CALLBACK(event_probability), view);
    gtk_style_context_add_class(gtk_widget_get_style_context(probability), "task-primary");
    gtk_widget_set_tooltip_text(view->rotation_mode, "Ağırlıklı seçimde Diğer seçenekler'den Ağırlık sütununu açıp çift tıklayarak payı düzenle. 0 rotasyonu kapatır; zaman ve tekrar kuralları uygun adayları belirler.");
    view->store = gtk_list_store_new(8, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    view->rows = gtk_tree_view_new_with_model(GTK_TREE_MODEL(view->store)); g_object_unref(view->store); gtk_widget_set_name(view->rows, "automation-calendar-rotations");
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(view->rows)), GTK_SELECTION_MULTIPLE);
    /* Selection, track, advert and sweeper behaviour belong to the rotation
     * editor; the table names the rotation and its content only. */
    const gchar *columns[] = { "Rotasyon / Playlist", "İçerik / Sıklık" };
    for (guint i = 0; i < G_N_ELEMENTS(columns); ++i) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new(); g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(columns[i], renderer, "text", (gint)i + 1, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE); gtk_tree_view_column_set_min_width(column, i ? 68 : 120); gtk_tree_view_column_set_expand(column, i == 0); gtk_tree_view_append_column(GTK_TREE_VIEW(view->rows), column);
        gtk_tree_view_column_set_visible(column, i < 2);
    }
    GtkCellRenderer *weight_cell = gtk_cell_renderer_text_new();
    g_object_set(weight_cell, "editable", TRUE, NULL);
    g_object_set_data(G_OBJECT(view->rows), "automation-calendar-rotation-weight", weight_cell);
    GtkTreeViewColumn *weight_column = gtk_tree_view_column_new(); gtk_tree_view_column_set_title(weight_column, "Ağırlık");
    gtk_tree_view_column_pack_start(weight_column, weight_cell, TRUE);
    gtk_tree_view_column_set_cell_data_func(weight_column, weight_cell, rotation_weight_cell, view, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view->rows), weight_column);
    gtk_tree_view_column_set_visible(weight_column, FALSE);
    g_signal_connect(weight_cell, "edited", G_CALLBACK(rotation_weight_edited), view);
    g_signal_connect(view->rows, "button-press-event", G_CALLBACK(rows_press), view);
    gtk_tree_view_set_reorderable(GTK_TREE_VIEW(view->rows), TRUE); g_signal_connect(view->store, "rows-reordered", G_CALLBACK(rows_reordered), view);
    g_signal_connect(view->store, "row-deleted", G_CALLBACK(row_deleted), view);
    GtkWidget *row_scroll = gtk_scrolled_window_new(NULL, NULL); gtk_widget_set_size_request(row_scroll, -1, 100);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(row_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(row_scroll, TRUE); gtk_widget_set_name(row_scroll, "automation-calendar-rotation-scroll");
    gtk_container_add(GTK_CONTAINER(row_scroll), view->rows); gtk_box_pack_start(GTK_BOX(rotation_section), row_scroll, TRUE, TRUE, 0);
    view->row_menu = gtk_menu_new(); g_object_ref_sink(view->row_menu);
    gtk_menu_attach_to_widget(GTK_MENU(view->row_menu), view->rows, NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(view->row_menu), "radios");
    gtk_style_context_add_class(gtk_widget_get_style_context(view->row_menu), "automation-calendar-menu");
    menu_item(view->row_menu, "Düzenle", "automation-calendar-row-edit", G_CALLBACK(row_edit), view);
    const gchar *action_labels[] = { "Yukarı taşı", "Aşağı taşı", "Sil", "Kopyala", "Yapıştır" };
    const gchar *action_names[] = { "automation-calendar-row-up", "automation-calendar-row-down", "automation-calendar-row-remove", "automation-calendar-row-copy", "automation-calendar-row-paste" };
    const gint action_ids[] = { -1, 1, 0, 3, 4 };
    for (guint i = 0; i < G_N_ELEMENTS(action_ids); ++i) {
        GtkWidget *item = menu_item(view->row_menu, action_labels[i], action_names[i], G_CALLBACK(row_action), view);
        g_object_set_data(G_OBJECT(item), "action", GINT_TO_POINTER(action_ids[i]));
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(view->row_menu), gtk_separator_menu_item_new());
    view->preview_button = menu_item(view->row_menu, "Önizle", "automation-calendar-preview", G_CALLBACK(preview_selected), view);
    view->append_button = menu_item(view->row_menu, "Yayın sırasına ekle", "automation-calendar-append", G_CALLBACK(preview_append), view);
    view->schedule_button = menu_item(view->row_menu, "Zamanla", "automation-calendar-schedule", G_CALLBACK(preview_schedule), view);
    gtk_widget_set_sensitive(view->append_button, FALSE); gtk_widget_set_sensitive(view->schedule_button, FALSE);
    GtkWidget *bottom = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, "ac-panel-footer");
    view->define = button_new("Yeni rotasyon", "automation-calendar-define-rotation", G_CALLBACK(rotation_dialog), view);
    gtk_style_context_add_class(gtk_widget_get_style_context(view->define), "ac-primary");
    gtk_box_pack_end(GTK_BOX(bottom), view->define, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rotation_section), bottom, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(view->editor), rotation_section, TRUE, TRUE, 0);
    GtkWidget *rules = simple_section("3  Kurallar", "automation-calendar-rules-section");
    GtkWidget *rules_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, NULL);
    gtk_box_pack_end(GTK_BOX(rules_row), probability, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rules), rules_row, FALSE, FALSE, 0);
    GtkWidget *details = gtk_expander_new("Diğer seçenekler");
    gtk_widget_set_name(details, "automation-calendar-details");
    GtkWidget *details_body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "ac-details-body");
    gtk_box_pack_start(GTK_BOX(details_body), mode_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(details_body), color_field, FALSE, FALSE, 0);
    GtkWidget *detail_columns = gtk_check_button_new_with_label("Rotasyon ağırlığı sütununu göster");
    gtk_widget_set_name(detail_columns, "automation-calendar-detail-columns");
    g_signal_connect(detail_columns, "toggled", G_CALLBACK(detail_columns_changed), view);
    gtk_box_pack_start(GTK_BOX(details_body), detail_columns, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(details), details_body);
    gtk_box_pack_start(GTK_BOX(rules), details, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(view->editor), rules, FALSE, FALSE, 0);
    gtk_widget_show_all(view->editor); gtk_widget_set_no_show_all(view->editor, TRUE);
    gtk_box_pack_start(GTK_BOX(programme_page), view->editor, TRUE, TRUE, 0);
    gtk_paned_pack1(GTK_PANED(paned), workspace, TRUE, FALSE);
    if (legacy_rules) { g_object_ref_sink(legacy_rules); g_object_set_data_full(G_OBJECT(page), "legacy-rules", legacy_rules, g_object_unref); }
    view->block_menu = gtk_menu_new(); g_object_ref_sink(view->block_menu);
    gtk_menu_attach_to_widget(GTK_MENU(view->block_menu), view->grid, NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(view->block_menu), "radios");
    gtk_style_context_add_class(gtk_widget_get_style_context(view->block_menu), "automation-calendar-menu");
    view->copy = menu_item(view->block_menu, "Kopyala", "automation-calendar-copy", G_CALLBACK(copy_blocks), view);
    view->paste = menu_item(view->block_menu, "Yapıştır", "automation-calendar-paste", G_CALLBACK(paste_blocks), view);
    view->properties_item = menu_item(view->block_menu, "Program özellikleri", "automation-calendar-programme-properties", G_CALLBACK(programme_properties), view);
    GtkWidget *remove = menu_item(view->block_menu, "Sil", "automation-calendar-remove-programme", G_CALLBACK(remove_programme), view);
    g_object_set_data(G_OBJECT(view->block_menu), "remove", remove);
    gtk_menu_shell_append(GTK_MENU_SHELL(view->block_menu), gtk_separator_menu_item_new());
    menu_item(view->block_menu, "Veritabanından yeniden yükle", "automation-calendar-reload", G_CALLBACK(reload_calendar), view);
    GtkWidget *side_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name(side_scroll, "automation-calendar-properties-scroll");
    GtkWidget *library_expander = gtk_expander_new("Kayıtlı rotasyonlardan ekle");
    gtk_widget_set_name(library_expander, "automation-calendar-library-picker");
    gtk_style_context_add_class(gtk_widget_get_style_context(library_expander), "ac-library-expander");
    gtk_container_add(GTK_CONTAINER(library_expander), library_bar);
    gtk_box_pack_start(GTK_BOX(side), library_expander, FALSE, FALSE, 0);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(side_scroll), side);
    gtk_paned_pack2(GTK_PANED(paned), side_scroll, FALSE, FALSE); gtk_paned_set_position(GTK_PANED(paned), 840);
    show_programme(view);
    view->settings_stack = gtk_stack_new();
    gtk_widget_set_name(view->settings_stack, "automation-calendar-pages");
    gtk_stack_set_hhomogeneous(GTK_STACK(view->settings_stack), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(view->settings_stack), FALSE);
    gtk_stack_add_named(GTK_STACK(view->settings_stack), paned, "calendar");
    gtk_box_pack_start(GTK_BOX(page), view->settings_stack, TRUE, TRUE, 0);
    gtk_widget_set_tooltip_text(view->grid, "Boş hücrelerde sürükleyerek saat aralığı seçin. Ctrl ile blokları birlikte seçip sürükleyin. Sağ tık: sil, kopyala, yapıştır.");
    gtk_widget_set_has_tooltip(view->grid, TRUE); g_signal_connect(view->grid, "query-tooltip", G_CALLBACK(grid_tooltip), view);
    view->save_status = radios_ui_label("Otomatik kayıt", "ac-save-status"); gtk_widget_set_name(view->save_status, "automation-calendar-save-status");
    gtk_box_pack_end(GTK_BOX(bar), view->save_status, FALSE, FALSE, 0);
    view->status = radios_ui_label("", "task-feedback"); gtk_widget_set_name(view->status, "automation-calendar-status"); gtk_box_pack_start(GTK_BOX(page), view->status, FALSE, FALSE, 0);
    calendar_style(view);
    GDateTime *today = calendar_now(view); set_week(view, today); g_date_time_unref(today);
    timezone_render(view);
    if (ctx->data) { if (!detached) view->document = radios_data_document_new(ctx, page, "automation_calendar", "main", applied, view);
        if (view->document) radios_data_document_set_status_callback(view->document, save_status_changed, view);
        radios_data_register_refresh(ctx, page, load_playlists); }
    return page;
}

void radios_automation_calendar_set_snapshot(GtkWidget *page, JsonObject *snapshot)
{
    Calendar *view = page ? g_object_get_data(G_OBJECT(page), "automation-calendar") : NULL;
    if (!view || view->disposed || view->closing || !snapshot) return;
    JsonNode *incoming = object_node(snapshot);
    g_clear_pointer(&view->config, json_node_free); view->config = deep_copy(incoming); json_node_free(incoming);
    view->dirty = FALSE; invalidate_preview(view);
    library_reconcile_editor(view);
    if (!event_by_id(view, view->current)) { g_clear_pointer(&view->current, g_free); g_hash_table_remove_all(view->selected); }
    refresh(view);
    if (view->detached && !view->inspector_invalid) {
        gtk_label_set_text(GTK_LABEL(view->save_status), "Güncel");
        gtk_label_set_text(GTK_LABEL(view->status), "Program taslağı güncellendi.");
    }
}
gboolean radios_automation_calendar_snapshot_ready(GtkWidget *page)
{
    Calendar *view = page ? g_object_get_data(G_OBJECT(page), "automation-calendar") : NULL;
    return view && !view->disposed && !view->closing && !view->inspector_invalid;
}
gboolean radios_automation_calendar_bind_editor(GtkWidget *page,
    RadioSAutomationCalendarChanged callback, gpointer user, const gchar *title)
{
    Calendar *view = page ? g_object_get_data(G_OBJECT(page), "automation-calendar") : NULL;
    if (!view || view->disposed || view->closing || view->inspector_invalid) return FALSE;
    /* Programme copies retain definition identities. An editor belongs to
     * the programme that opened it, even if the next draft has identical IDs. */
    if (view->library_dialog) gtk_widget_destroy(view->library_dialog);
    if (view->schema_dialog) gtk_widget_destroy(view->schema_dialog);
    if (view->rotation_dialog) gtk_widget_destroy(view->rotation_dialog);
    if (callback) {
        if (!view->bound_editor) {
            if (view->dirty && view->document) submit(view);
            g_clear_pointer(&view->saved_main, json_node_free); view->saved_main = deep_copy(view->config);
            view->saved_main_dirty = view->dirty;
        }
        view->bound_editor = TRUE; view->editor_changed = callback; view->editor_user = user; view->dirty = FALSE;
    } else {
        view->bound_editor = FALSE; view->editor_changed = NULL; view->editor_user = NULL;
        if (view->saved_main) {
            JsonNode *saved = view->saved_main; view->saved_main = NULL;
            radios_automation_calendar_set_snapshot(page, json_node_get_object(saved)); json_node_free(saved);
            view->dirty = view->saved_main_dirty;
        }
        view->saved_main_dirty = FALSE;
    }
    g_free(view->programme_title); view->programme_title = g_strdup(title ? title : "");
    update_detail_heading(view); return TRUE;
}
JsonNode *radios_automation_calendar_main_snapshot(GtkWidget *page)
{
    Calendar *view = page ? g_object_get_data(G_OBJECT(page), "automation-calendar") : NULL;
    return view ? deep_copy(view->bound_editor && view->saved_main ? view->saved_main : view->config) : NULL;
}
GtkWidget *radios_automation_calendar_editor_new(RadioSViewContext *ctx, JsonObject *snapshot,
    RadioSAutomationCalendarChanged callback, gpointer user)
{
    GtkWidget *page = calendar_new(ctx, NULL, TRUE);
    Calendar *view = g_object_get_data(G_OBJECT(page), "automation-calendar");
    view->editor_changed = callback; view->editor_user = user;
    if (snapshot) radios_automation_calendar_set_snapshot(page, snapshot);
    return page;
}
static void save_as_programme(GtkButton *button, gpointer data)
{
    (void)button; Calendar *view = data;
    if (view->closing || view->disposed || view->inspector_invalid) {
        notice(view, "Kaydetmeden önce düzenlediğiniz program alanlarını tamamlayın."); return;
    }
    gchar *week = g_date_time_format(view->week, "%Y-%m-%d");
    radios_weekly_programme_save_snapshot(view->ctx, view->page, json_node_get_object(view->config), week);
    g_free(week);
}
GtkWidget *radios_automation_calendar_new(RadioSViewContext *ctx, GtkWidget *legacy_rules)
{
    GtkWidget *main = calendar_new(ctx, legacy_rules, FALSE);
    gtk_widget_set_name(main, "automation-calendar-main");
    Calendar *view = g_object_get_data(G_OBJECT(main), "automation-calendar");
    GtkWidget *bar = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
    GtkWidget *save = button_new("Program olarak kaydet", "automation-calendar-save-programme", G_CALLBACK(save_as_programme), view);
    gtk_box_pack_end(GTK_BOX(bar), save, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(main), bar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(main), bar, 0);
    GtkWidget *programmes = radios_weekly_programme_controls_new(ctx, main);
    GtkWidget *side = g_object_get_data(G_OBJECT(main), "automation-calendar-side-panel");
    gtk_box_pack_start(GTK_BOX(side), programmes, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(side), programmes, 1);
    g_object_set_data(G_OBJECT(main), "weekly-programme-workspace", programmes);
    return main;
}
