#include "radios/media_ui.h"
#include "radios/media_jobs.h"
#include "radios/editors.h"
#include "radios/widgets.h"
#include "radios/audio_ui.h"
#include "radios/media_routing.h"
#include "radios/player_transport.h"
#include <string.h>

typedef struct PlaylistDrop PlaylistDrop;
typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *dialog, *tree, *search, *status, *chooser, *import_status;
    GtkWidget *fields[8], *save, *import, *cancel, *previous, *next;
    GtkWidget *marker_host, *marker_panel, *routing, *pick;
    GWeakRef pick_owner;
    gulong pick_handler;
    RadioSMediaPick pick_callback;
    gpointer pick_user;
    GtkListStore *model;
    GHashTable *records, *selected_ids;
    JsonNode *detail, *retry;
    JsonArray *batch_records;
    gchar *selected, *import_kind, *last_import_error, *query, *import_root, *folder_root;
    GPtrArray *paths;
    GArray *page_history;
    GCancellable *cancellable;
    guint generation, list_generation, offset, next_offset, importing, imported, failed;
    gboolean updating, dirty, busy, has_next, closed;
    PlaylistDrop *playlist_drop;
    guint drop_generation;
    GQueue *drop_jobs;
    gboolean creating; /* a folder playlist is being created */
    RadioSMediaImportDone batch_done; /* programmatic batch (Ayarlar › Medya arşivi) */
    gpointer batch_user;
    /* Saved library views (/v1/views): kind filter, visible columns, name. */
    GtkWidget *kind_filter, *view_combo, *view_save, *view_dialog, *column_menu;
    GtkWidget *column_toggles[4], *column_items[4];
    gchar *kind, *views_station, *view_pending;
    JsonNode *views;
    guint views_generation;
    gboolean view_updating, view_saving;
} MediaLibrary;
struct PlaylistDrop {
    MediaLibrary *library;
    GtkWidget *dialog, *combo, *status;
    JsonNode *records, *pending;
    GPtrArray *paths;
    gchar *target, *reason;
    gint64 revision;
    guint rejected, added;
    gboolean starting, started, saving, finished;
    gboolean headless; /* a window drop: no dialog, results go to notifications */
    gchar *label;
};
/* Window drops wait here while another import runs: a folder becomes its
 * own playlist; loose files go to the visible playlist or the library. */
typedef struct { gchar *folder; GPtrArray *paths; } DropJob;
typedef struct {
    RadioSViewContext *ctx;
    gulong drop_handler, received_handler;
} MediaDropBinding;

enum { TITLE, KIND, ARTIST, ALBUM, GENRE, TAGS, YEAR, COMMENT };
static const gchar *const kinds[] = { "music", "advertisement", "jingle", "sweeper", "news", "program", "announcement", "effect", "voice_track", "other", NULL };
static const gchar *const kind_labels[] = { "Müzik", "Reklam", "Jingle", "Sweeper", "Haber", "Program", "Duyuru", "Efekt", "Ses kaydı", "Diğer", NULL };
/* media-table columns in order, keyed as /v1/views profile columns. */
static const gchar *const column_keys[] = { "title", "artist", "kind", "duration_ms" };
static const gchar *const column_labels[] = { "Başlık", "Sanatçı", "Tür", "Ölçülen süre" };
G_STATIC_ASSERT(G_N_ELEMENTS(column_keys) == 4 && G_N_ELEMENTS(column_labels) == 4);
static void library_refresh(RadioSViewContext *ctx, GtkWidget *owner);
static void views_ensure(MediaLibrary *library);
static void detail_request(MediaLibrary *library);
static void import_next(MediaLibrary *library);
static void import_cancel(GtkButton *widget, gpointer value);
static void playlist_drop_save(MediaLibrary *library);
static void drop_jobs_next(MediaLibrary *library);
static void library_notify(MediaLibrary *library, const gchar *text)
{ if (library->ctx->notify) library->ctx->notify(library->ctx->notify_data, text); }
static void selection_routes(MediaLibrary *library);
static gboolean markers_dirty(MediaLibrary *library);
static void pick_clear(MediaLibrary *library)
{
    GObject *owner = g_weak_ref_get(&library->pick_owner);
    if (owner) {
        if (library->pick_handler && g_signal_handler_is_connected(owner, library->pick_handler))
            g_signal_handler_disconnect(owner, library->pick_handler);
        g_object_unref(owner);
    }
    library->pick_handler = 0; library->pick_callback = NULL; library->pick_user = NULL;
    g_weak_ref_set(&library->pick_owner, NULL);
    if (library->pick) { gtk_widget_set_sensitive(library->pick, FALSE); gtk_widget_hide(library->pick); }
}
static void pick_target_destroyed(GtkWidget *widget, gpointer value)
{ (void)widget; pick_clear(value); }

static JsonNode *copy_node(JsonNode *node)
{
    JsonGenerator *generator = json_generator_new(); json_generator_set_root(generator, node);
    gchar *text = json_generator_to_data(generator, NULL); JsonParser *parser = json_parser_new();
    JsonNode *result = json_parser_load_from_data(parser, text, -1, NULL) ? json_node_copy(json_parser_get_root(parser)) : NULL;
    g_object_unref(parser); g_object_unref(generator); g_free(text); return result;
}
static MediaLibrary *state(GtkWidget *owner) { return g_object_get_data(G_OBJECT(owner), "radios-media-library"); }
static void pack(GtkWidget *box, GtkWidget *child, gboolean expand) { gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static void message(MediaLibrary *library, const gchar *text) { gtk_label_set_text(GTK_LABEL(library->status), text); }
static GtkWidget *button(const gchar *text, const gchar *name)
{
    GtkWidget *widget = radios_ui_button(text); gtk_widget_set_name(widget, name); return widget;
}
static void enable_fields(MediaLibrary *library)
{
    gboolean available = library->detail && !library->busy;
    for (guint i = 0; i < 8; ++i) gtk_widget_set_sensitive(library->fields[i], available);
    gtk_widget_set_sensitive(library->save, available);
    if (library->marker_panel) gtk_widget_set_sensitive(library->marker_panel, available);
    if (library->pick) {
        gtk_widget_set_visible(library->pick, library->pick_callback != NULL);
        gtk_widget_set_sensitive(library->pick, available && library->pick_callback != NULL);
    }
}
static void pick_clicked(GtkButton *button_widget, gpointer value)
{
    (void)button_widget; MediaLibrary *library = value;
    if (!library->detail || library->busy || !library->pick_callback) return;
    if (markers_dirty(library)) { message(library, "Önce parça işaretlerini kaydet; seçili medya korunuyor."); return; }
    GtkWidget *owner = g_weak_ref_get(&library->pick_owner);
    if (!owner || gtk_widget_in_destruction(owner)) { g_clear_object(&owner); pick_clear(library); return; }
    RadioSMediaPick callback = library->pick_callback; gpointer user = library->pick_user;
    JsonObject *latest = radios_marker_editor_detail(library->marker_panel);
    JsonNode *snapshot = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(snapshot, latest ? latest : json_node_get_object(library->detail));
    JsonNode *detail = copy_node(snapshot); json_node_free(snapshot); pick_clear(library);
    /* Clear intent before calling: the target may open another picker. */
    callback(library->ctx, owner, json_node_get_object(detail), user);
    json_node_free(detail); g_object_unref(owner);
}
static gboolean markers_dirty(MediaLibrary *library)
{ return library->marker_panel && radios_marker_editor_dirty(library->marker_panel); }
static void marker_apply(MediaLibrary *library)
{
    if (library->marker_panel && markers_dirty(library)) return;
    if (library->marker_panel) gtk_widget_destroy(library->marker_panel);
    library->marker_panel = radios_marker_editor_embed(library->ctx, library->dialog,
        library->detail ? json_node_get_object(library->detail) : NULL);
    pack(library->marker_host, library->marker_panel, FALSE);
    gtk_widget_show_all(library->marker_host);
}
static void selection_routes(MediaLibrary *library)
{
    if (!library->routing) return;
    /* A valid file drop accepts up to 256 sources, beyond one 200-row page.
     * Keep every imported source available for routing until explicit selection. */
    if (library->batch_records && json_array_get_length(library->batch_records)) {
        radios_media_routing_select(library->routing, library->batch_records); return;
    }
    JsonArray *records = json_array_new();
    GList *paths = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(library->tree)), NULL);
    for (GList *item = paths; item; item = item->next) {
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(library->model), &iter, item->data)) continue;
        gchar *id = NULL; gtk_tree_model_get(GTK_TREE_MODEL(library->model), &iter, 4, &id, -1);
        JsonNode *record = g_hash_table_lookup(library->records, id);
        if (record) json_array_add_element(records, copy_node(record));
        g_free(id);
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    radios_media_routing_select(library->routing, records); json_array_unref(records);
}
static void details_apply(MediaLibrary *library)
{
    JsonObject *snapshot = library->detail ? json_node_get_object(library->detail) : NULL;
    JsonObject *media = snapshot ? json_object_get_object_member(snapshot, "media") : NULL;
    for (guint i = 0; media && library->batch_records && i < json_array_get_length(library->batch_records); ++i) {
        JsonObject *record = json_array_get_object_element(library->batch_records, i);
        JsonObject *batch_media = json_object_get_object_member(record, "media");
        if (!g_strcmp0(radios_json_string(batch_media, "_id", ""), radios_json_string(media, "_id", ""))) {
            JsonNode *updated = copy_node(library->detail);
            json_node_set_object(json_array_get_element(library->batch_records, i), json_node_get_object(updated)); json_node_free(updated);
            break;
        }
    }
    const gchar *keys[] = { "title", "kind", "artist", "album", "genre", "tags", "year", "comment" };
    library->updating = TRUE;
    for (guint i = 0; i < 8; ++i) {
        if (i == KIND) {
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(library->fields[i]), radios_json_string(media, "kind", "music"));
        } else if (i == TAGS) {
            GString *text = g_string_new(""); JsonArray *tags = media ? json_object_get_array_member(media, "tags") : NULL;
            for (guint j = 0; tags && j < json_array_get_length(tags); ++j) {
                if (j) g_string_append(text, ", ");
                g_string_append(text, json_array_get_string_element(tags, j));
            }
            gtk_entry_set_text(GTK_ENTRY(library->fields[i]), text->str); g_string_free(text, TRUE);
        } else gtk_entry_set_text(GTK_ENTRY(library->fields[i]), radios_json_string(media, keys[i], ""));
    }
    library->updating = FALSE; library->dirty = FALSE; marker_apply(library); enable_fields(library);
}
static void changed(GtkWidget *widget, gpointer value)
{
    (void)widget; MediaLibrary *library = value;
    if (!library->updating) { library->dirty = TRUE; message(library, "Etiket değişiklikleri henüz kaydedilmedi."); }
}
static void select_id(MediaLibrary *library, const gchar *id)
{
    GtkTreeIter iter; gboolean found = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(library->model), &iter);
    while (found) {
        gchar *row = NULL; gtk_tree_model_get(GTK_TREE_MODEL(library->model), &iter, 4, &row, -1);
        gboolean matches = g_strcmp0(row, id) == 0; g_free(row);
        if (matches) { gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(library->tree)), &iter); return; }
        found = gtk_tree_model_iter_next(GTK_TREE_MODEL(library->model), &iter);
    }
}
static void detail_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; MediaLibrary *library = state(owner); guint generation = GPOINTER_TO_UINT(value);
    if (generation != library->generation) return;
    library->busy = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { message(library, error ? error->message : "Medya ayrıntısı alınamadı."); enable_fields(library); return; }
    g_clear_pointer(&library->detail, json_node_free); library->detail = copy_node(result);
    g_clear_pointer(&library->retry, json_node_free); details_apply(library); message(library, "Parça hazır.");
}
static void detail_request(MediaLibrary *library)
{
    if (!library->selected) return;
    JsonNode *body = radios_data_body(library->ctx); json_object_set_string_member(json_node_get_object(body), "media_id", library->selected);
    library->busy = TRUE; enable_fields(library); ++library->generation;
    if (!radios_data_request(library->ctx, library->dialog, "POST", "/v1/workflows/desktop-media/get", body, detail_done, GUINT_TO_POINTER(library->generation), NULL)) {
        library->busy = FALSE; enable_fields(library); message(library, "Veri servisi bağlı değil; kayıt seçimi korunuyor.");
    }
    json_node_free(body);
}
static void selected(GtkTreeSelection *selection, gpointer value)
{
    MediaLibrary *library = value; if (library->updating) return;
    GList *paths = gtk_tree_selection_get_selected_rows(selection, NULL);
    gchar *id = NULL;
    /* Keep the current detail while extending a multiple selection. */
    for (GList *item = paths; item; item = item->next) {
        GtkTreeIter iter; if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(library->model), &iter, item->data)) continue;
        gchar *candidate = NULL; gtk_tree_model_get(GTK_TREE_MODEL(library->model), &iter, 4, &candidate, -1);
        if (!id || !g_strcmp0(candidate, library->selected)) { g_free(id); id = g_strdup(candidate); }
        g_free(candidate);
    }
    gboolean change = g_strcmp0(id, library->selected) != 0;
    if ((change && (library->dirty || library->busy || markers_dirty(library))) || radios_media_routing_busy(library->routing)) {
        library->updating = TRUE; gtk_tree_selection_unselect_all(selection);
        GHashTableIter saved; gpointer key; g_hash_table_iter_init(&saved, library->selected_ids);
        while (g_hash_table_iter_next(&saved, &key, NULL)) select_id(library, key);
        library->updating = FALSE; message(library, "Önce mevcut etiket/işaret değişikliklerini kaydet veya işlemi tamamla.");
        g_free(id); g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free); return;
    }
    g_clear_pointer(&library->batch_records, json_array_unref);
    g_hash_table_remove_all(library->selected_ids);
    for (GList *item = paths; item; item = item->next) {
        GtkTreeIter iter; if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(library->model), &iter, item->data)) continue;
        gchar *candidate = NULL; gtk_tree_model_get(GTK_TREE_MODEL(library->model), &iter, 4, &candidate, -1);
        g_hash_table_add(library->selected_ids, candidate);
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free); selection_routes(library);
    if (!change) { g_free(id); return; }
    g_free(library->selected); library->selected = id;
    g_clear_pointer(&library->detail, json_node_free); g_clear_pointer(&library->retry, json_node_free);
    details_apply(library); detail_request(library);
}
static void list_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer unused)
{
    (void)ctx; MediaLibrary *library = state(owner);
    if (GPOINTER_TO_UINT(unused) != library->list_generation) return;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { message(library, error ? error->message : "Medya listesi alınamadı."); return; }
    JsonObject *object = json_node_get_object(result); JsonArray *records = json_object_get_array_member(object, "records");
    library->updating = TRUE; gtk_list_store_clear(library->model); g_hash_table_remove_all(library->records);
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonNode *record = json_array_get_element(records, i); JsonObject *snapshot = json_node_get_object(record);
        JsonObject *media = json_object_get_object_member(snapshot, "media"), *version = json_object_get_object_member(snapshot, "version");
        const gchar *id = radios_json_string(media, "_id", ""); GtkTreeIter iter;
        gchar *duration = g_strdup_printf("%.3f sn", radios_json_int(version, "duration_ms", 0) / 1000.0);
        gtk_list_store_append(library->model, &iter); gtk_list_store_set(library->model, &iter,
            0, radios_json_string(media, "title", ""), 1, radios_json_string(media, "artist", ""),
            2, radios_json_string(media, "kind", ""), 3, duration, 4, id, -1);
        g_hash_table_insert(library->records, g_strdup(id), copy_node(record)); g_free(duration);
    }
    GHashTableIter saved; gpointer key; g_hash_table_iter_init(&saved, library->selected_ids);
    while (g_hash_table_iter_next(&saved, &key, NULL)) select_id(library, key);
    library->updating = FALSE; selection_routes(library);
    JsonNode *next = json_object_get_member(object, "next_offset"); library->has_next = next && !JSON_NODE_HOLDS_NULL(next);
    library->next_offset = library->has_next ? (guint)json_node_get_int(next) : 0;
    gtk_widget_set_sensitive(library->previous, library->page_history->len > 0); gtk_widget_set_sensitive(library->next, library->has_next);
    gchar *text = g_strdup_printf("%u kayıt gösteriliyor · toplam %" G_GINT64_FORMAT, records ? json_array_get_length(records) : 0, radios_json_int(object, "total", 0));
    message(library, text); g_free(text);
}
static void library_refresh(RadioSViewContext *ctx, GtkWidget *owner)
{
    MediaLibrary *library = state(owner); if (!library || !radios_data_connected(ctx)) return;
    JsonNode *body = radios_data_body(ctx); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "query", library->query);
    json_object_set_int_member(object, "offset", library->offset); json_object_set_int_member(object, "limit", 200);
    if (library->kind && *library->kind) json_object_set_string_member(object, "kind", library->kind);
    ++library->list_generation;
    radios_data_request(ctx, owner, "POST", "/v1/workflows/desktop-media/list", body, list_done, GUINT_TO_POINTER(library->list_generation), NULL); json_node_free(body);
    views_ensure(library);
}
static void search_activate(GtkEntry *entry, gpointer value)
{
    MediaLibrary *library = value; library->offset = 0; g_array_set_size(library->page_history, 0);
    g_free(library->query); library->query = g_strdup(gtk_entry_get_text(entry)); library_refresh(library->ctx, library->dialog);
}
static void page_clicked(GtkButton *button_widget, gpointer value)
{
    MediaLibrary *library = value;
    if (GTK_WIDGET(button_widget) == library->next) {
        if (!library->has_next) return;
        g_array_append_val(library->page_history, library->offset); library->offset = library->next_offset;
    } else {
        if (!library->page_history->len) return;
        library->offset = g_array_index(library->page_history, guint, library->page_history->len - 1);
        g_array_set_size(library->page_history, library->page_history->len - 1);
    }
    library_refresh(library->ctx, library->dialog);
}
/* Saved library views (/v1/views): the visible columns and the kind filter
 * under a name, kept per user and station. */
static GtkTreeViewColumn *table_column(MediaLibrary *library, guint index)
{ return gtk_tree_view_get_column(GTK_TREE_VIEW(library->tree), (gint)index); }
static guint columns_visible(MediaLibrary *library)
{
    guint visible = 0;
    for (guint i = 0; i < G_N_ELEMENTS(column_keys); ++i) visible += gtk_tree_view_column_get_visible(table_column(library, i)) ? 1 : 0;
    return visible;
}
/* Sütunlar and the header menu follow the table; the last visible column
 * cannot be switched off. */
static void columns_sync(MediaLibrary *library)
{
    if (library->closed) return;
    gboolean updating = library->view_updating; guint visible = columns_visible(library); library->view_updating = TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(column_keys); ++i) {
        gboolean shown = gtk_tree_view_column_get_visible(table_column(library, i));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(library->column_toggles[i]), shown);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(library->column_items[i]), shown);
        gtk_widget_set_sensitive(library->column_toggles[i], !shown || visible > 1);
        gtk_widget_set_sensitive(library->column_items[i], !shown || visible > 1);
    }
    library->view_updating = updating;
}
static void column_toggled(GtkToggleButton *toggle, gpointer value)
{
    MediaLibrary *library = value; if (library->view_updating) return;
    gboolean shown = gtk_toggle_button_get_active(toggle);
    for (guint i = 0; i < G_N_ELEMENTS(column_keys); ++i)
        if (library->column_toggles[i] == GTK_WIDGET(toggle) && (shown || columns_visible(library) > 1))
            gtk_tree_view_column_set_visible(table_column(library, i), shown);
    columns_sync(library);
}
static void column_item_toggled(GtkCheckMenuItem *item, gpointer value)
{
    MediaLibrary *library = value; if (library->view_updating) return;
    for (guint i = 0; i < G_N_ELEMENTS(column_keys); ++i)
        if (library->column_items[i] == GTK_WIDGET(item))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(library->column_toggles[i]), gtk_check_menu_item_get_active(item));
    columns_sync(library);
}
/* A header of a non-clickable column consumes its presses before
 * button-press-event, so a capture-phase gesture opens the column menu. */
static void header_pressed(GtkGestureMultiPress *gesture, gint presses, gdouble x, gdouble y, gpointer value)
{
    (void)presses; (void)x; (void)y; MediaLibrary *library = value;
    GdkEventSequence *sequence = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(gesture));
    const GdkEvent *event = gtk_gesture_get_last_event(GTK_GESTURE(gesture), sequence);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    columns_sync(library); gtk_menu_popup_at_pointer(GTK_MENU(library->column_menu), event);
}
static void columns_bind(MediaLibrary *library)
{
    for (guint i = 0; i < G_N_ELEMENTS(column_keys); ++i) {
        GtkWidget *header = gtk_tree_view_column_get_button(table_column(library, i));
        if (!header) continue;
        GtkGesture *gesture = gtk_gesture_multi_press_new(header);
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_SECONDARY);
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture), GTK_PHASE_CAPTURE);
        g_signal_connect(gesture, "pressed", G_CALLBACK(header_pressed), library);
        g_object_set_data_full(G_OBJECT(header), "radios-media-header-menu", gesture, g_object_unref);
    }
    gtk_menu_attach_to_widget(GTK_MENU(library->column_menu), library->tree, NULL);
}
static gboolean kind_known(const gchar *kind)
{
    for (guint i = 0; kinds[i]; ++i) if (!g_strcmp0(kinds[i], kind)) return TRUE;
    return FALSE;
}
/* Sets the kind filter without reading the list; callers refresh. */
static void kind_filter_set(MediaLibrary *library, const gchar *kind)
{
    const gchar *id = kind_known(kind) ? kind : ""; gboolean updating = library->view_updating;
    library->view_updating = TRUE; gtk_combo_box_set_active_id(GTK_COMBO_BOX(library->kind_filter), id); library->view_updating = updating;
    gchar *previous = library->kind; library->kind = g_strdup(id); g_free(previous);
}
static void list_restart(MediaLibrary *library)
{ library->offset = 0; g_array_set_size(library->page_history, 0); library_refresh(library->ctx, library->dialog); }
static void kind_filter_changed(GtkComboBox *combo, gpointer value)
{
    MediaLibrary *library = value; if (library->view_updating) return;
    kind_filter_set(library, gtk_combo_box_get_active_id(combo)); list_restart(library);
}
static JsonArray *member_array(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static JsonObject *member_object(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static const gchar *string_element(JsonArray *array, guint index)
{
    JsonNode *node = json_array_get_element(array, index);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}
static gint column_index(const gchar *key)
{
    for (guint i = 0; i < G_N_ELEMENTS(column_keys); ++i) if (!g_strcmp0(column_keys[i], key)) return (gint)i;
    return -1;
}
/* One of this user's profiles by identity or, without one, by its unique name. */
static JsonObject *view_find(MediaLibrary *library, const gchar *id, const gchar *name)
{
    JsonArray *views = library->views ? json_node_get_array(library->views) : NULL;
    for (guint i = 0; views && i < json_array_get_length(views); ++i) {
        JsonNode *node = json_array_get_element(views, i);
        JsonObject *view = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        if (view && !g_strcmp0(radios_json_string(view, id ? "_id" : "name", ""), id ? id : name)) return view;
    }
    return NULL;
}
static gint view_compare(gconstpointer left, gconstpointer right)
{
    return g_utf8_collate(radios_json_string(*(JsonObject *const *)left, "name", ""), radios_json_string(*(JsonObject *const *)right, "name", ""));
}
/* Lists the library views and keeps the chosen (or the just saved) view
 * selected without applying it again. */
static void views_fill(MediaLibrary *library)
{
    const gchar *active = gtk_combo_box_get_active_id(GTK_COMBO_BOX(library->view_combo)); /* interned */
    const gchar *keep = library->view_pending ? library->view_pending : active ? active : "";
    GPtrArray *rows = g_ptr_array_new(); JsonArray *views = library->views ? json_node_get_array(library->views) : NULL;
    for (guint i = 0; views && i < json_array_get_length(views); ++i) {
        JsonNode *node = json_array_get_element(views, i);
        JsonObject *view = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        if (view && !g_strcmp0(radios_json_string(view, "view_kind", ""), "library") && g_uuid_string_is_valid(radios_json_string(view, "_id", "")))
            g_ptr_array_add(rows, view);
    }
    g_ptr_array_sort(rows, view_compare);
    gboolean updating = library->view_updating; library->view_updating = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(library->view_combo));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(library->view_combo), "", "Varsayılan görünüm");
    for (guint i = 0; i < rows->len; ++i)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(library->view_combo), radios_json_string(g_ptr_array_index(rows, i), "_id", ""),
            radios_json_string(g_ptr_array_index(rows, i), "name", ""));
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(library->view_combo), keep)) gtk_combo_box_set_active(GTK_COMBO_BOX(library->view_combo), 0);
    library->view_updating = updating; g_clear_pointer(&library->view_pending, g_free); g_ptr_array_unref(rows);
}
static void views_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; MediaLibrary *library = state(owner);
    if (GPOINTER_TO_UINT(value) != library->views_generation) return;
    if (error || !result || !JSON_NODE_HOLDS_ARRAY(result)) {
        gchar *text = g_strdup_printf("Kayıtlı görünümler alınamadı: %s", error ? error->message : "geçersiz yanıt.");
        message(library, text); g_free(text); return;
    }
    g_clear_pointer(&library->views, json_node_free); library->views = copy_node(result); views_fill(library);
}
static void views_request(MediaLibrary *library)
{
    const gchar *station = radios_data_station_id(library->ctx); if (!*station) return;
    g_free(library->views_station); library->views_station = g_strdup(station);
    gchar *escaped = g_uri_escape_string(station, NULL, FALSE), *path = g_strconcat("/v1/views?station_id=", escaped, NULL);
    ++library->views_generation;
    radios_data_request(library->ctx, library->dialog, "GET", path, NULL, views_done, GUINT_TO_POINTER(library->views_generation), NULL);
    g_free(path); g_free(escaped);
}
/* Saved views belong to this user and station: a shown window reads them
 * once per station; a library built unseen for a drop does not. */
static void views_ensure(MediaLibrary *library)
{
    if (!library->closed && gtk_widget_get_visible(library->dialog) && radios_data_connected(library->ctx) &&
        g_strcmp0(library->views_station, radios_data_station_id(library->ctx))) views_request(library);
}
static void library_shown(GtkWidget *widget, gpointer value) { (void)widget; views_ensure(value); }
/* Applies the chosen view; Varsayılan görünüm shows every column and kind. */
static void view_changed(GtkComboBox *combo, gpointer value)
{
    MediaLibrary *library = value; if (library->view_updating) return;
    g_clear_pointer(&library->view_pending, g_free); /* this choice wins over a pending reread */
    const gchar *id = gtk_combo_box_get_active_id(combo);
    JsonObject *view = id && *id ? view_find(library, id, NULL) : NULL;
    JsonArray *columns = member_array(view, "columns"); gboolean shown[G_N_ELEMENTS(column_keys)] = { FALSE }; guint count = 0;
    for (guint i = 0; columns && i < json_array_get_length(columns); ++i) {
        gint index = column_index(string_element(columns, i));
        if (index >= 0 && !shown[index]) { shown[index] = TRUE; ++count; }
    }
    gboolean updating = library->view_updating; library->view_updating = TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(column_keys); ++i) gtk_tree_view_column_set_visible(table_column(library, i), !count || shown[i]);
    library->view_updating = updating; columns_sync(library);
    kind_filter_set(library, radios_json_string(member_object(view, "filters"), "kind", "")); list_restart(library);
}
static void view_saved(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer unused)
{
    (void)ctx; (void)unused; MediaLibrary *library = state(owner);
    library->view_saving = FALSE; gtk_widget_set_sensitive(library->view_save, TRUE);
    JsonObject *saved = !error && result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    gboolean conflict = error && error->domain == RADIOS_DATA_CLIENT_ERROR && error->code == RADIOS_DATA_CLIENT_ERROR_CONFLICT;
    gchar *text = saved ? g_strdup_printf("\"%s\" görünümü kaydedildi.", radios_json_string(saved, "name", "")) :
        conflict ? g_strdup("Görünüm başka bir oturumda değişti veya bu ad kullanımda; güncel liste okunuyor, yeniden kaydet.") :
        g_strdup_printf("Görünüm kaydedilemedi: %s", error ? error->message : "veri servisi yanıtı doğrulanamadı.");
    message(library, text); g_free(text);
    if (saved) { g_free(library->view_pending); library->view_pending = g_strdup(radios_json_string(saved, "_id", "")); }
    if (saved || conflict) views_request(library);
}
/* A name of an existing library view updates that profile at its revision. */
static void view_save(MediaLibrary *library, const gchar *name)
{
    JsonObject *existing = view_find(library, NULL, name);
    if (existing && g_strcmp0(radios_json_string(existing, "view_kind", ""), "library")) {
        message(library, "Bu ad başka bir görünüm türünde kullanılıyor; başka bir ad seç."); return;
    }
    JsonNode *body = radios_data_body(library->ctx); JsonObject *object = json_node_get_object(body), *filters = json_object_new();
    JsonArray *columns = json_array_new(), *kept = member_array(existing, "columns");
    for (guint i = 0; i < G_N_ELEMENTS(column_keys); ++i)
        if (gtk_tree_view_column_get_visible(table_column(library, i))) json_array_add_string_element(columns, column_keys[i]);
    /* An update keeps the columns and filters this window does not show. */
    for (guint i = 0; kept && i < json_array_get_length(kept); ++i) {
        const gchar *key = string_element(kept, i);
        if (key && column_index(key) < 0) json_array_add_string_element(columns, key);
    }
    JsonObject *previous = member_object(existing, "filters"); GList *keys = previous ? json_object_get_members(previous) : NULL;
    for (GList *key = keys; key; key = key->next)
        if (g_strcmp0(key->data, "kind")) json_object_set_member(filters, key->data, json_node_copy(json_object_get_member(previous, key->data)));
    g_list_free(keys);
    if (*library->kind) json_object_set_string_member(filters, "kind", library->kind);
    json_object_set_string_member(object, "name", name); json_object_set_string_member(object, "view_kind", "library");
    json_object_set_array_member(object, "columns", columns); json_object_set_object_member(object, "filters", filters);
    if (existing) {
        json_object_set_string_member(object, "profile_id", radios_json_string(existing, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(existing, "_rev", 0));
    }
    library->view_saving = TRUE; gtk_widget_set_sensitive(library->view_save, FALSE);
    if (!radios_data_request(library->ctx, library->dialog, "POST", "/v1/views", body, view_saved, NULL, NULL)) {
        library->view_saving = FALSE; gtk_widget_set_sensitive(library->view_save, TRUE);
        message(library, "Veri servisi bağlı değil; görünüm kaydedilmedi.");
    }
    json_node_free(body);
}
static void view_name_changed(GtkEditable *entry, gpointer dialog)
{
    gchar *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, *name != '\0'); g_free(name);
}
static void view_dialog_response(GtkDialog *dialog, gint response, gpointer value)
{
    MediaLibrary *library = value; GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "media-view-name");
    gchar *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
    gtk_widget_destroy(GTK_WIDGET(dialog));
    if (response == GTK_RESPONSE_ACCEPT && *name && !library->closed && !library->view_saving) view_save(library, name);
    g_free(name);
}
static void view_dialog_destroyed(GtkWidget *widget, gpointer value)
{ MediaLibrary *library = value; if (library->view_dialog == widget) library->view_dialog = NULL; }
static void view_save_clicked(GtkButton *widget, gpointer value)
{
    (void)widget; MediaLibrary *library = value;
    if (library->view_dialog) { gtk_window_present(GTK_WINDOW(library->view_dialog)); return; }
    if (library->view_saving) return;
    library->view_dialog = gtk_dialog_new_with_buttons("Görünümü kaydet", GTK_WINDOW(library->dialog),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(library->view_dialog, "media-view-dialog");
    gtk_style_context_add_class(gtk_widget_get_style_context(library->view_dialog), "radios");
    GtkWidget *area = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, NULL), *entry = radios_ui_entry("Görünüm adı");
    gtk_container_set_border_width(GTK_CONTAINER(area), 18); gtk_widget_set_name(entry, "media-view-name");
    gtk_entry_set_max_length(GTK_ENTRY(entry), 120); gtk_entry_set_width_chars(GTK_ENTRY(entry), 28); gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    const gchar *active = gtk_combo_box_get_active_id(GTK_COMBO_BOX(library->view_combo));
    JsonObject *view = active && *active ? view_find(library, active, NULL) : NULL;
    if (view) gtk_entry_set_text(GTK_ENTRY(entry), radios_json_string(view, "name", ""));
    pack(area, radios_ui_label("Görünüm adı", "form-label"), FALSE); pack(area, entry, FALSE);
    pack(gtk_dialog_get_content_area(GTK_DIALOG(library->view_dialog)), area, TRUE);
    g_object_set_data(G_OBJECT(library->view_dialog), "media-view-name", entry);
    gtk_dialog_set_default_response(GTK_DIALOG(library->view_dialog), GTK_RESPONSE_ACCEPT);
    g_signal_connect(entry, "changed", G_CALLBACK(view_name_changed), library->view_dialog);
    view_name_changed(GTK_EDITABLE(entry), library->view_dialog);
    g_signal_connect(library->view_dialog, "response", G_CALLBACK(view_dialog_response), library);
    g_signal_connect(library->view_dialog, "destroy", G_CALLBACK(view_dialog_destroyed), library);
    gtk_widget_show_all(library->view_dialog);
}
/* Second, small toolbar row: kind filter, Sütunlar and the saved views. */
static GtkWidget *view_toolbar(MediaLibrary *library)
{
    GtkWidget *bar = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL); gtk_widget_set_name(bar, "media-view-toolbar");
    library->kind_filter = gtk_combo_box_text_new(); gtk_widget_set_name(library->kind_filter, "media-kind-filter");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(library->kind_filter), "", "Tüm türler");
    for (guint i = 0; kinds[i]; ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(library->kind_filter), kinds[i], kind_labels[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(library->kind_filter), 0); library->kind = g_strdup("");
    gtk_widget_set_tooltip_text(library->kind_filter, "Tür süzgeci");
    GtkWidget *columns = gtk_menu_button_new(), *popover = gtk_popover_new(columns), *items = radios_ui_box(GTK_ORIENTATION_VERTICAL, 2, NULL);
    gtk_button_set_label(GTK_BUTTON(columns), "Sütunlar ▾"); gtk_widget_set_name(columns, "media-columns");
    gtk_style_context_add_class(gtk_widget_get_style_context(columns), "ld-btn"); gtk_widget_set_tooltip_text(columns, "Görünür sütunlar");
    gtk_container_set_border_width(GTK_CONTAINER(items), 6); gtk_container_add(GTK_CONTAINER(popover), items);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(columns), popover);
    library->column_menu = g_object_ref_sink(gtk_menu_new()); gtk_widget_set_name(library->column_menu, "media-columns-menu");
    for (guint i = 0; i < G_N_ELEMENTS(column_keys); ++i) {
        gchar *name = g_strdup_printf("media-column-%s", column_keys[i]), *item_name = g_strdup_printf("media-columns-menu-%s", column_keys[i]);
        library->column_toggles[i] = gtk_check_button_new_with_label(column_labels[i]); gtk_widget_set_name(library->column_toggles[i], name);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(library->column_toggles[i]), TRUE); pack(items, library->column_toggles[i], FALSE);
        g_signal_connect(library->column_toggles[i], "toggled", G_CALLBACK(column_toggled), library);
        library->column_items[i] = gtk_check_menu_item_new_with_label(column_labels[i]); gtk_widget_set_name(library->column_items[i], item_name);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(library->column_items[i]), TRUE);
        gtk_menu_shell_append(GTK_MENU_SHELL(library->column_menu), library->column_items[i]);
        g_signal_connect(library->column_items[i], "toggled", G_CALLBACK(column_item_toggled), library);
        g_free(name); g_free(item_name);
    }
    gtk_widget_show_all(items); gtk_widget_show_all(library->column_menu);
    library->view_combo = gtk_combo_box_text_new(); gtk_widget_set_name(library->view_combo, "media-view");
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(library->view_combo));
    if (cells) g_object_set(cells->data, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 14, "max-width-chars", 28, NULL);
    g_list_free(cells);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(library->view_combo), "", "Varsayılan görünüm");
    gtk_combo_box_set_active(GTK_COMBO_BOX(library->view_combo), 0); gtk_widget_set_tooltip_text(library->view_combo, "Kayıtlı görünüm");
    library->view_save = button("Görünümü kaydet", "media-view-save");
    gtk_widget_set_tooltip_text(library->view_save, "Görünür sütunları ve tür süzgecini adla kaydet");
    pack(bar, library->kind_filter, FALSE); pack(bar, columns, FALSE); pack(bar, library->view_combo, FALSE); pack(bar, library->view_save, FALSE);
    g_signal_connect(library->kind_filter, "changed", G_CALLBACK(kind_filter_changed), library);
    g_signal_connect(library->view_combo, "changed", G_CALLBACK(view_changed), library);
    g_signal_connect(library->view_save, "clicked", G_CALLBACK(view_save_clicked), library);
    g_signal_connect(library->dialog, "show", G_CALLBACK(library_shown), library);
    return bar;
}
static void reload_response(GtkDialog *dialog, gint response, gpointer value)
{
    MediaLibrary *library = value;
    if (response == GTK_RESPONSE_ACCEPT && !library->closed) {
        library->dirty = FALSE;
        if (library->marker_panel) { gtk_widget_destroy(library->marker_panel); library->marker_panel = NULL; }
        detail_request(library);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void reload_offer(MediaLibrary *library)
{
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(library->dialog), GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "Etiket girdileri korunuyor.");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "Veritabanındaki güncel kaydı yeniden yüklemek yerel etiket ve parça işareti değişikliklerini geri alır.");
    gtk_widget_set_name(dialog, "media-reload-dialog"); gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Girdileri koru", GTK_RESPONSE_CANCEL, "Yeniden yükle", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    g_signal_connect(dialog, "response", G_CALLBACK(reload_response), library); gtk_widget_show_all(dialog);
}
static JsonObject *metadata_read(MediaLibrary *library)
{
    JsonObject *metadata = json_object_new(); const gchar *keys[] = { "title", "kind", "artist", "album", "genre", "tags", "year", "comment" };
    for (guint i = 0; i < 8; ++i) {
        if (i == KIND) {
            const gchar *kind = gtk_combo_box_get_active_id(GTK_COMBO_BOX(library->fields[i])); json_object_set_string_member(metadata, keys[i], kind ? kind : "music");
        } else if (i == TAGS) {
            JsonArray *tags = json_array_new(); gchar **parts = g_strsplit(gtk_entry_get_text(GTK_ENTRY(library->fields[i])), ",", -1);
            GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
            for (guint j = 0; parts[j]; ++j) { gchar *tag = g_strstrip(parts[j]); if (*tag && !g_hash_table_contains(seen, tag)) { json_array_add_string_element(tags, tag); g_hash_table_add(seen, tag); } }
            g_hash_table_unref(seen); g_strfreev(parts); json_object_set_array_member(metadata, keys[i], tags);
        } else json_object_set_string_member(metadata, keys[i], gtk_entry_get_text(GTK_ENTRY(library->fields[i])));
    }
    return metadata;
}
static void tags_saved(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer unused)
{
    (void)ctx; (void)unused; MediaLibrary *library = state(owner); library->busy = FALSE; enable_fields(library);
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        message(library, error ? error->message : "Etiket kaydı doğrulanamadı; girdiler korunuyor.");
        if (error && error->domain == RADIOS_DATA_CLIENT_ERROR && (error->code == RADIOS_DATA_CLIENT_ERROR_CONFLICT || error->code == RADIOS_DATA_CLIENT_ERROR_NOT_FOUND)) {
            g_clear_pointer(&library->retry, json_node_free); reload_offer(library);
        } else if (error && error->domain == RADIOS_DATA_CLIENT_ERROR && (error->code == RADIOS_DATA_CLIENT_ERROR_VALIDATION || error->code == RADIOS_DATA_CLIENT_ERROR_FORBIDDEN || error->code == RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED)) g_clear_pointer(&library->retry, json_node_free);
        return;
    }
    g_clear_pointer(&library->retry, json_node_free); g_clear_pointer(&library->detail, json_node_free);
    library->detail = copy_node(result); details_apply(library); library_refresh(library->ctx, owner); message(library, "Etiketler veritabanına kaydedildi.");
}
static void tags_save(GtkButton *widget, gpointer value)
{
    (void)widget; MediaLibrary *library = value; if (!library->detail || library->busy) return;
    if (markers_dirty(library)) { message(library, "Önce parça işaretlerini kaydet; etiket girdileri korunuyor."); return; }
    JsonObject *metadata = metadata_read(library);
    if (!*radios_json_string(metadata, "title", "")) { message(library, "Medya başlığı gerekli."); json_object_unref(metadata); return; }
    JsonNode *body = radios_data_body(library->ctx); JsonObject *object = json_node_get_object(body);
    json_object_set_object_member(object, "metadata", metadata);
    if (library->retry) {
        if (!json_node_equal(json_object_get_member(json_node_get_object(library->retry), "metadata"), json_object_get_member(object, "metadata"))) { json_node_free(body); reload_offer(library); return; }
        json_node_free(body); body = copy_node(library->retry);
    } else {
        JsonObject *latest = radios_marker_editor_detail(library->marker_panel);
        JsonObject *media = json_object_get_object_member(latest ? latest : json_node_get_object(library->detail), "media");
        json_object_set_string_member(object, "media_id", library->selected);
        json_object_set_int_member(object, "expected_revision", radios_json_int(media, "_rev", 0)); gchar *key = g_uuid_string_random();
        json_object_set_string_member(object, "idempotency_key", key); g_free(key); library->retry = copy_node(body);
    }
    library->busy = TRUE; enable_fields(library);
    if (!radios_data_request(library->ctx, library->dialog, "POST", "/v1/workflows/desktop-media/metadata-save", body, tags_saved, NULL, NULL)) { library->busy = FALSE; enable_fields(library); message(library, "Veri servisi bağlı değil; etiket girdileri korunuyor."); }
    json_node_free(body);
}
static void import_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer unused)
{
    (void)ctx; (void)unused; MediaLibrary *library = state(owner); g_clear_object(&library->cancellable);
    if (!error && result) {
        ++library->imported;
        if (library->batch_records && JSON_NODE_HOLDS_OBJECT(result))
            json_array_add_element(library->batch_records, copy_node(result));
        if (JSON_NODE_HOLDS_OBJECT(result)) {
            JsonObject *media = json_object_get_object_member(json_node_get_object(result), "media");
            const gchar *id = radios_json_string(media, "_id", "");
            if (*id) {
                g_hash_table_add(library->selected_ids, g_strdup(id));
                if (!library->busy && !library->dirty && !markers_dirty(library)) {
                    g_free(library->selected); library->selected = g_strdup(id);
                    g_clear_pointer(&library->detail, json_node_free); library->detail = copy_node(result); details_apply(library);
                }
            }
        }
        PlaylistDrop *drop = library->playlist_drop;
        if (drop && drop->started && JSON_NODE_HOLDS_OBJECT(result)) {
            JsonObject *detail = json_node_get_object(result);
            JsonObject *media = json_object_get_object_member(detail, "media");
            JsonObject *version = json_object_get_object_member(detail, "version");
            const gchar *media_id = radios_json_string(media, "_id", "");
            const gchar *version_id = radios_json_string(version, "_id", "");
            const gchar *file_id = radios_json_string(version, "media_file_id", "");
            const gchar *digest = radios_json_string(version, "sha256", "");
            if (g_uuid_string_is_valid(media_id) && g_uuid_string_is_valid(version_id) &&
                g_uuid_string_is_valid(file_id) && strlen(digest) == 64) {
                drop->pending = radios_data_body(library->ctx);
                JsonObject *body = json_node_get_object(drop->pending), *metadata = json_object_new();
                const gchar *keys[] = { "title", "artist", "album", "genre", "comment" };
                for (guint i = 0; i < G_N_ELEMENTS(keys); ++i)
                    json_object_set_string_member(metadata, keys[i], radios_json_string(media, keys[i], ""));
                json_object_set_string_member(metadata, "key", "");
                json_object_set_string_member(metadata, "location", radios_json_string(version, "managed_path", ""));
                json_object_set_double_member(metadata, "bpm", 0);
                json_object_set_int_member(metadata, "duration_ms", radios_json_int(version, "duration_ms", 0));
                json_object_set_string_member(metadata, "media_id", media_id);
                json_object_set_string_member(metadata, "version_id", version_id);
                json_object_set_string_member(metadata, "media_file_id", file_id);
                json_object_set_string_member(metadata, "media_sha256", digest);
                json_object_set_string_member(body, "playlist_id", drop->target);
                json_object_set_int_member(body, "expected_revision", drop->revision);
                gchar *key = g_uuid_string_random();
                json_object_set_string_member(body, "idempotency_key", key); g_free(key);
                json_object_set_object_member(body, "metadata", metadata);
                playlist_drop_save(library); return;
            }
            ++library->failed;
            g_free(library->last_import_error);
            library->last_import_error = g_strdup("Doğrulanmış medya kimliği eksik; playlist kaydı oluşturulmadı.");
        }
    } else {
        ++library->failed; g_free(library->last_import_error); library->last_import_error = g_strdup(error ? error->message : "Medya kaydı doğrulanamadı.");
        gtk_label_set_text(GTK_LABEL(library->import_status), library->last_import_error);
    }
    ++library->importing; import_next(library);
}
static void import_next(MediaLibrary *library)
{
    if (library->closed) return;
    while (library->paths && library->importing < library->paths->len) {
        const gchar *path = g_ptr_array_index(library->paths, library->importing); gchar *title = g_path_get_basename(path);
        gchar *text = g_strdup_printf("Dosya içe alınıyor · %u/%u · %s", library->importing + 1, library->paths->len, title);
        gtk_label_set_text(GTK_LABEL(library->import_status), text); g_free(text); g_free(title);
        /* Preserve the source's actual title tag; the worker supplies a basename
         * only when the decoded file has no title. */
        library->cancellable = library->paths->len > 1 || library->folder_root ?
            radios_media_import_staged_async(library->ctx, library->dialog, path, library->import_root, library->import_kind, "", import_done, NULL, NULL) :
            radios_media_import_async(library->ctx, library->dialog, path, library->import_kind, "", import_done, NULL, NULL);
        if (library->cancellable) return;
        ++library->failed; ++library->importing;
    }
    gboolean catalogue_changed = library->paths && library->imported > 0;
    gchar *text = g_strdup_printf("İçe aktarma tamamlandı · %u kayıt doğrulandı · %u sonuç doğrulanamadı. Yayın onayı verilmedi.%s%s",
        library->imported, library->failed, library->last_import_error ? " " : "", library->last_import_error ? library->last_import_error : "");
    gtk_label_set_text(GTK_LABEL(library->import_status), text); g_free(text); g_clear_pointer(&library->paths, g_ptr_array_unref);
    g_object_set_data(G_OBJECT(library->ctx->parent), "radios-media-import-batch", NULL);
    g_clear_pointer(&library->import_root, g_free); g_clear_pointer(&library->folder_root, g_free);
    gtk_widget_set_sensitive(library->import, TRUE); gtk_widget_set_sensitive(library->cancel, FALSE); library_refresh(library->ctx, library->dialog);
    if (catalogue_changed) {
        guint generation = radios_media_catalogue_generation(library->ctx) + 1;
        g_object_set_data(G_OBJECT(library->ctx->parent), "radios-media-catalogue-generation", GUINT_TO_POINTER(generation));
        radios_data_refresh(library->ctx);
    }
    gboolean visible = gtk_widget_get_visible(library->dialog);
    if (library->playlist_drop && library->playlist_drop->started) {
        PlaylistDrop *drop = library->playlist_drop; drop->finished = TRUE;
        gchar *summary = g_strdup_printf("%u şarkı playliste eklendi · %u sonuç doğrulanamadı.%s%s",
            drop->added, library->failed, library->last_import_error ? " " : "", library->last_import_error ? library->last_import_error : "");
        gtk_label_set_text(GTK_LABEL(drop->status), summary);
        gtk_dialog_set_response_sensitive(GTK_DIALOG(drop->dialog), GTK_RESPONSE_ACCEPT, FALSE);
        if (drop->headless) {
            gchar *text = g_strdup_printf("%s: %s", drop->label ? drop->label : "Playlist", summary);
            library_notify(library, text); g_free(text); gtk_widget_destroy(drop->dialog);
        }
        g_free(summary);
    } else if (!visible && library->imported + library->failed) {
        gchar *text2 = g_strdup_printf("Bırakılan dosyalar içe aktarıldı · %u kayıt doğrulandı · %u doğrulanamadı.%s%s", library->imported, library->failed,
            library->last_import_error ? " " : "", library->last_import_error ? library->last_import_error : "");
        library_notify(library, text2); g_free(text2);
    }
    drop_jobs_next(library);
    if (library->batch_done) {
        /* One report per programmatic batch, after the library settled. */
        RadioSMediaImportDone done = library->batch_done; gpointer user = library->batch_user;
        library->batch_done = NULL; library->batch_user = NULL;
        done(library->ctx, library->imported, library->failed, library->last_import_error, user);
    }
}
static gboolean import_available(MediaLibrary *library)
{
    const gchar *reason = NULL;
    if (!radios_data_connected(library->ctx)) reason = "Veri servisi bağlı değil; dosya içe aktarılmadı. Önce veri servisine bağlan.";
    else if (library->paths || library->cancellable) reason = "İçe aktarma sürüyor; tamamlanmasını bekle veya iptal et.";
    else if (library->chooser) reason = "Dosya seçim penceresi açık; önce seçimi tamamla veya kapat.";
    else if (library->playlist_drop && !library->playlist_drop->starting) reason = "Playliste ekle penceresi açık; önce işlemi tamamla veya kapat.";
    else if (library->busy) reason = "Medya işlemi sürüyor; tamamlanmasını bekle.";
    if (reason) gtk_label_set_text(GTK_LABEL(library->import_status), reason);
    return reason == NULL;
}
/* Takes ownership even when the batch is rejected. Both entry points retain
 * the same sequential worker, durable receipt, cancellation and refresh. */
static gboolean import_begin(MediaLibrary *library, GPtrArray *paths, const gchar *kind,
    guint rejected, const gchar *reason)
{
    if (!import_available(library)) { g_ptr_array_unref(paths); return FALSE; }
    if (!paths->len) {
        gchar *text = g_strdup_printf("İçe aktarılacak yerel ses dosyası yok · %u dosya kabul edilmedi.%s%s",
            rejected, reason ? " " : "", reason ? reason : "");
        gtk_label_set_text(GTK_LABEL(library->import_status), text); g_free(text); g_ptr_array_unref(paths); return FALSE;
    }
    if (!library->dirty && !markers_dirty(library)) {
        g_hash_table_remove_all(library->selected_ids);
        g_clear_pointer(&library->batch_records, json_array_unref); library->batch_records = json_array_new();
        library->offset = 0; g_array_set_size(library->page_history, 0);
        g_free(library->query); library->query = g_strdup(""); gtk_entry_set_text(GTK_ENTRY(library->search), "");
        /* Imported records must show: a kind filter of another kind is cleared. */
        if (*library->kind && g_strcmp0(library->kind, kind && *kind ? kind : "music")) kind_filter_set(library, "");
    }
    library->paths = paths; library->importing = library->imported = 0; library->failed = rejected;
    g_object_set_data(G_OBJECT(library->ctx->parent), "radios-media-import-batch", GINT_TO_POINTER(1));
    radios_media_rhythm_library_cancel(library->ctx);
    g_free(library->import_root);
    library->import_root = library->folder_root ? g_strdup(library->folder_root) : g_path_get_dirname(g_ptr_array_index(paths, 0));
    for (guint i = 0; i < paths->len && !library->folder_root; ++i) {
        const gchar *path = g_ptr_array_index(paths, i);
        while (g_strcmp0(library->import_root, "/") &&
            (!g_str_has_prefix(path, library->import_root) || path[strlen(library->import_root)] != G_DIR_SEPARATOR)) {
            gchar *parent = g_path_get_dirname(library->import_root); g_free(library->import_root); library->import_root = parent;
        }
    }
    g_free(library->last_import_error); library->last_import_error = g_strdup(reason);
    g_free(library->import_kind); library->import_kind = g_strdup(kind && *kind ? kind : "music");
    gtk_widget_set_sensitive(library->import, FALSE); gtk_widget_set_sensitive(library->cancel, TRUE);
    import_next(library); return TRUE;
}
static void playlist_drop_saved(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result,
    const GError *error, gpointer generation)
{
    MediaLibrary *library = state(owner); PlaylistDrop *drop = library->playlist_drop;
    if (!drop || GPOINTER_TO_UINT(generation) != library->drop_generation) {
        ++library->importing; import_next(library); return;
    }
    drop->saving = FALSE;
    if (drop->headless && (error || !result || !JSON_NODE_HOLDS_OBJECT(result))) {
        /* Nobody can press retry on a window drop: keep the request durable and move on. */
        if (drop->pending) radios_data_preserve(library->ctx, "/v1/workflows/desktop-playlists/entry-save", drop->pending);
        g_clear_pointer(&drop->pending, json_node_free); ++library->failed;
        g_free(library->last_import_error); library->last_import_error = g_strdup(error ? error->message : "Playlist kaydı doğrulanamadı.");
        ++library->importing; import_next(library); return;
    }
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        gtk_label_set_text(GTK_LABEL(drop->status), error ? error->message : "Playlist kaydı doğrulanamadı; yeniden dene.");
        gtk_dialog_set_response_sensitive(GTK_DIALOG(drop->dialog), GTK_RESPONSE_ACCEPT, TRUE);
        GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(drop->dialog), GTK_RESPONSE_ACCEPT);
        gtk_button_set_label(GTK_BUTTON(accept), "Kaydı yeniden dene"); return;
    }
    JsonObject *playlist = json_node_get_object(result);
    if (g_strcmp0(radios_json_string(playlist, "_id", ""), drop->target) || radios_json_int(playlist, "_rev", 0) <= 0) {
        gtk_label_set_text(GTK_LABEL(drop->status), "Playlist yanıtı seçilen kayıtla eşleşmiyor; yeniden dene.");
        gtk_dialog_set_response_sensitive(GTK_DIALOG(drop->dialog), GTK_RESPONSE_ACCEPT, TRUE); return;
    }
    drop->revision = radios_json_int(playlist, "_rev", 0); ++drop->added;
    g_clear_pointer(&drop->pending, json_node_free);
    radios_data_refresh(ctx);
    ++library->importing; import_next(library);
}
static void playlist_drop_save(MediaLibrary *library)
{
    PlaylistDrop *drop = library->playlist_drop;
    if (!drop || !drop->pending || drop->saving) return;
    drop->saving = TRUE;
    gtk_label_set_text(GTK_LABEL(drop->status), "Doğrulanmış şarkı seçilen playliste kaydediliyor…");
    gtk_dialog_set_response_sensitive(GTK_DIALOG(drop->dialog), GTK_RESPONSE_ACCEPT, FALSE);
    if (!radios_data_request(library->ctx, library->dialog, "POST", "/v1/workflows/desktop-playlists/entry-save",
        drop->pending, playlist_drop_saved, GUINT_TO_POINTER(library->drop_generation), NULL)) {
        drop->saving = FALSE;
        gtk_label_set_text(GTK_LABEL(drop->status), "Veri servisi bağlı değil; doğrulanmış şarkı ve kayıt isteği korunuyor.");
        gtk_dialog_set_response_sensitive(GTK_DIALOG(drop->dialog), GTK_RESPONSE_ACCEPT, TRUE);
    }
}
static void playlist_drop_destroyed(GtkWidget *widget, gpointer value)
{
    (void)widget; PlaylistDrop *drop = value; MediaLibrary *library = drop->library;
    if (drop->started && !drop->finished && library->paths) {
        import_cancel(NULL, library);
        if (drop->pending) radios_data_preserve(library->ctx, "/v1/workflows/desktop-playlists/entry-save", drop->pending);
        /* A paused failed save has no callback left to advance its batch. */
        if (drop->pending && !drop->saving) { ++library->importing; import_next(library); }
    }
    library->playlist_drop = NULL; ++library->drop_generation;
    g_clear_pointer(&drop->records, json_node_free); g_clear_pointer(&drop->pending, json_node_free);
    g_clear_pointer(&drop->paths, g_ptr_array_unref); g_free(drop->target); g_free(drop->reason); g_free(drop->label); g_free(drop);
}
static gboolean drop_headless(MediaLibrary *library, GPtrArray *paths, const gchar *kind, const gchar *target, gint64 revision, const gchar *label)
{
    PlaylistDrop *drop = g_new0(PlaylistDrop, 1); drop->library = library; drop->headless = TRUE; drop->label = g_strdup(label);
    drop->dialog = gtk_dialog_new_with_buttons("Playliste ekle", library->ctx->parent, GTK_DIALOG_DESTROY_WITH_PARENT,
        "Vazgeç / Kapat", GTK_RESPONSE_CANCEL, "Playliste ekle", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(drop->dialog));
    drop->combo = gtk_combo_box_text_new(); drop->status = gtk_label_new(""); pack(body, drop->combo, FALSE); pack(body, drop->status, FALSE);
    g_object_set_data(G_OBJECT(drop->dialog), "radios-playlist-drop", drop);
    g_signal_connect(drop->dialog, "destroy", G_CALLBACK(playlist_drop_destroyed), drop);
    library->playlist_drop = drop; ++library->drop_generation;
    drop->target = g_strdup(target); drop->revision = revision; drop->started = drop->starting = TRUE;
    gboolean queued = import_begin(library, paths, kind, 0, NULL);
    drop->starting = FALSE;
    if (!queued) { drop->finished = TRUE; gtk_widget_destroy(drop->dialog); }
    return queued;
}
static const gchar *const audio_extensions[] = {".wav", ".mp3", ".flac", ".ogg", ".opus", ".m4a", ".aac", NULL};
static gboolean audio_file(const gchar *name)
{
    gchar *lower = g_ascii_strdown(name, -1); gboolean match = FALSE;
    for (guint i = 0; audio_extensions[i] && !match; ++i) match = g_str_has_suffix(lower, audio_extensions[i]);
    g_free(lower); return match;
}
static void folder_collect(const gchar *folder, guint depth, GPtrArray *paths)
{
    GDir *dir = depth > 6 ? NULL : g_dir_open(folder, 0, NULL); const gchar *name; GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    while (dir && (name = g_dir_read_name(dir))) if (name[0] != '.') g_ptr_array_add(names, g_strdup(name));
    if (dir) g_dir_close(dir);
    g_ptr_array_sort(names, (GCompareFunc)g_strcmp0);
    for (guint i = 0; i < names->len && paths->len < 256; ++i) {
        gchar *path = g_build_filename(folder, g_ptr_array_index(names, i), NULL);
        if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) folder_collect(path, depth + 1, paths);
        else if (g_file_test(path, G_FILE_TEST_IS_REGULAR) && audio_file(path)) { g_ptr_array_add(paths, g_canonicalize_filename(path, NULL)); path = NULL; }
        g_free(path);
    }
    g_ptr_array_unref(names);
}
static void drop_job_free(gpointer value) { DropJob *job = value; g_free(job->folder); if (job->paths) g_ptr_array_unref(job->paths); g_free(job); }
typedef struct { MediaLibrary *library; GPtrArray *paths; gchar *name; } FolderCreate;
static void folder_created(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; FolderCreate *create = value; MediaLibrary *library = create->library; library->creating = FALSE;
    JsonObject *playlist = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    const gchar *id = radios_json_string(playlist, "_id", ""); gint64 revision = radios_json_int(playlist, "_rev", 0);
    if (error || !g_uuid_string_is_valid(id) || revision <= 0) {
        gchar *text = g_strdup_printf("\"%s\" klasör playlisti oluşturulamadı: %s", create->name, error ? error->message : "geçersiz yanıt");
        library_notify(library, text); g_free(text); g_ptr_array_unref(create->paths);
    } else {
        gchar *label = g_strdup_printf("\"%s\" klasör playlisti", create->name);
        if (!drop_headless(library, create->paths, "music", id, revision, label)) library_notify(library, gtk_label_get_text(GTK_LABEL(library->import_status)));
        g_free(label);
    }
    g_free(create->name); g_free(create);
    if (!library->paths && !library->playlist_drop) drop_jobs_next(library);
}
/* Starts the next queued window drop once the library is idle. */
static void drop_jobs_next(MediaLibrary *library)
{
    if (library->closed || !library->drop_jobs || library->creating || library->paths || library->cancellable || library->playlist_drop || library->busy) return;
    DropJob *job = g_queue_pop_head(library->drop_jobs); if (!job) return;
    if (job->folder) {
        g_free(library->folder_root); library->folder_root = g_strdup(job->folder);
        GPtrArray *paths = g_ptr_array_new_with_free_func(g_free); folder_collect(job->folder, 0, paths);
        gchar *name = g_path_get_basename(job->folder);
        if (!paths->len) {
            gchar *text = g_strdup_printf("\"%s\" klasöründe ses dosyası yok; playlist oluşturulmadı.", name);
            library_notify(library, text); g_free(text); g_free(name); g_ptr_array_unref(paths); drop_job_free(job); drop_jobs_next(library); return;
        }
        FolderCreate *create = g_new0(FolderCreate, 1); create->library = library; create->paths = paths; create->name = name;
        JsonNode *body = radios_data_body(library->ctx); JsonObject *object = json_node_get_object(body);
        gchar *trimmed = g_utf8_substring(name, 0, MIN(120, g_utf8_strlen(name, -1))), *key = g_uuid_string_random();
        json_object_set_string_member(object, "name", trimmed); json_object_set_string_member(object, "idempotency_key", key); g_free(trimmed); g_free(key);
        /* Later drops wait for this playlist's identity. */
        library->creating = radios_data_request(library->ctx, library->dialog, "POST", "/v1/workflows/desktop-playlists/create", body, folder_created, create, NULL);
        json_node_free(body);
        if (!library->creating) { library_notify(library, "Veri servisi bağlı değil; klasör içe aktarılmadı."); g_ptr_array_unref(paths); g_free(name); g_free(create); }
        drop_job_free(job); return;
    }
    GPtrArray *paths = job->paths; job->paths = NULL; drop_job_free(job);
    gchar *target = NULL; gint64 revision = 0;
    if (radios_playlist_visible_target(library->ctx, &target, &revision)) {
        if (!drop_headless(library, paths, "music", target, revision, "Açık playlist")) library_notify(library, gtk_label_get_text(GTK_LABEL(library->import_status)));
    } else if (!import_begin(library, paths, "music", 0, NULL)) library_notify(library, gtk_label_get_text(GTK_LABEL(library->import_status)));
    g_free(target);
}
static void chooser_response(GtkDialog *dialog, gint response, gpointer value)
{
    MediaLibrary *library = value; GPtrArray *paths = NULL; gchar *kind = NULL;
    if (response == GTK_RESPONSE_ACCEPT) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog)); paths = g_ptr_array_new_with_free_func(g_free);
        for (GSList *file = files; file; file = file->next) g_ptr_array_add(paths, file->data);
        g_slist_free(files); GtkWidget *combo = g_object_get_data(G_OBJECT(dialog), "media-import-kind");
        kind = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)));
    }
    library->chooser = NULL; gtk_widget_destroy(GTK_WIDGET(dialog));
    if (paths) import_begin(library, paths, kind, 0, NULL);
    g_free(kind);
}
static GtkWidget *kind_combo(const gchar *name)
{
    GtkWidget *combo = gtk_combo_box_text_new(); gtk_widget_set_name(combo, name);
    for (guint i = 0; kinds[i]; ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), kinds[i], kind_labels[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0); return combo;
}
static void import_clicked(GtkButton *widget, gpointer value)
{
    (void)widget; MediaLibrary *library = value;
    if (library->chooser) { gtk_window_present(GTK_WINDOW(library->chooser)); return; }
    if (!import_available(library)) return;
    library->chooser = gtk_file_chooser_dialog_new("Ses dosyalarını içe aktar", GTK_WINDOW(library->dialog), GTK_FILE_CHOOSER_ACTION_OPEN,
        "Vazgeç", GTK_RESPONSE_CANCEL, "İçe aktar", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(library->chooser), TRUE); gtk_widget_set_name(library->chooser, "media-import-chooser");
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(library->chooser), TRUE); gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(library->chooser), TRUE);
    GtkFileFilter *filter = gtk_file_filter_new(); gtk_file_filter_set_name(filter, "Ses dosyaları");
    const gchar *extensions[] = { "*.wav", "*.mp3", "*.flac", "*.ogg", "*.opus", "*.m4a", "*.aac", NULL };
    for (guint i = 0; extensions[i]; ++i) gtk_file_filter_add_pattern(filter, extensions[i]);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(library->chooser), filter);
    GtkWidget *kind = kind_combo("media-import-kind"); gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(library->chooser), kind);
    g_object_set_data(G_OBJECT(library->chooser), "media-import-kind", kind); g_signal_connect(library->chooser, "response", G_CALLBACK(chooser_response), library); gtk_widget_show_all(library->chooser);
}
static void import_cancel(GtkButton *widget, gpointer value)
{
    (void)widget; MediaLibrary *library = value;
    if (library->paths) g_ptr_array_set_size(library->paths, library->importing + 1);
    if (library->cancellable) g_cancellable_cancel(library->cancellable);
    gtk_widget_set_sensitive(library->cancel, FALSE); gtk_label_set_text(GTK_LABEL(library->import_status), "İçe aktarma iptal ediliyor; tamamlanmış kayıtlar korunur.");
}
static void destroyed(GtkWidget *widget, gpointer value)
{
    (void)widget; MediaLibrary *library = value; library->closed = TRUE;
    pick_clear(library);
    radios_audio_ui_preview_stop(library->ctx, library->dialog);
    if (library->cancellable) g_cancellable_cancel(library->cancellable);
    if (library->chooser) gtk_widget_destroy(library->chooser);
    if (library->view_dialog) gtk_widget_destroy(library->view_dialog);
    if (library->column_menu) gtk_widget_destroy(library->column_menu);
    if (library->playlist_drop) gtk_widget_destroy(library->playlist_drop->dialog);
    g_object_set_data(G_OBJECT(library->ctx->parent), "radios-media-library-dialog", NULL);
}
static void library_free(gpointer value)
{
    MediaLibrary *library = value; g_clear_object(&library->cancellable);
    library->pick = NULL; pick_clear(library); g_weak_ref_clear(&library->pick_owner);
    g_clear_pointer(&library->batch_records, json_array_unref);
    if (library->drop_jobs) g_queue_free_full(library->drop_jobs, drop_job_free);
    g_clear_pointer(&library->paths, g_ptr_array_unref); g_clear_pointer(&library->detail, json_node_free); g_clear_pointer(&library->retry, json_node_free);
    g_hash_table_unref(library->records); g_hash_table_unref(library->selected_ids); g_array_unref(library->page_history); g_free(library->selected); g_free(library->import_kind); g_free(library->last_import_error); g_free(library->query);
    g_free(library->kind); g_free(library->views_station); g_free(library->view_pending);
    g_clear_pointer(&library->views, json_node_free); g_clear_object(&library->column_menu);
    g_free(library->import_root); g_free(library->folder_root); g_free(library);
}
static void close_response(GtkDialog *dialog, gint response, gpointer unused) { (void)response; (void)unused; gtk_widget_destroy(GTK_WIDGET(dialog)); }
static void library_open(RadioSViewContext *ctx, gboolean show)
{
    GtkWidget *existing = g_object_get_data(G_OBJECT(ctx->parent), "radios-media-library-dialog");
    if (existing) { if (show) gtk_window_present(GTK_WINDOW(existing)); return; }
    MediaLibrary *library = g_new0(MediaLibrary, 1); library->ctx = ctx; library->query = g_strdup(""); library->page_history = g_array_new(FALSE, FALSE, sizeof(guint));
    g_weak_ref_init(&library->pick_owner, NULL);
    library->records = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_free);
    library->selected_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    library->dialog = gtk_dialog_new_with_buttons("Medya kütüphanesi", ctx->parent, GTK_DIALOG_DESTROY_WITH_PARENT, "Kapat", GTK_RESPONSE_CLOSE, NULL);
    gtk_widget_set_name(library->dialog, "media-library-dialog"); gtk_window_set_default_size(GTK_WINDOW(library->dialog), 1040, 760);
    gtk_style_context_add_class(gtk_widget_get_style_context(library->dialog), "radios");
    /* Two toolbar rows share the toolbar margins; views take the second row. */
    GtkWidget *body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "pw-body"), *toolbars = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "pw-toolbar");
    GtkWidget *toolbar = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name(scroll, "media-library-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), body);
    gtk_container_set_border_width(GTK_CONTAINER(body), 18); pack(gtk_dialog_get_content_area(GTK_DIALOG(library->dialog)), scroll, TRUE);
    library->search = radios_ui_entry("Başlık, sanatçı veya etiket ara · Enter"); gtk_widget_set_name(library->search, "media-search"); pack(toolbar, library->search, TRUE);
    library->import = button("+ Ses dosyaları", "media-import"); library->cancel = button("İçe aktarmayı iptal et", "media-import-cancel"); gtk_widget_set_sensitive(library->cancel, FALSE);
    library->previous = button("Önceki", "media-page-previous"); library->next = button("Sonraki", "media-page-next");
    gtk_widget_set_sensitive(library->previous, FALSE); gtk_widget_set_sensitive(library->next, FALSE);
    GtkWidget *rekordbox = button("rekordbox USB / SD oku", "media-import-rekordbox");
    gtk_widget_set_tooltip_text(rekordbox, "rekordbox USB / SD tracklist veya XML dosyasından içe aktar");
    g_signal_connect_swapped(rekordbox, "clicked", G_CALLBACK(radios_playlist_rekordbox_open), ctx);
    pack(toolbar, library->previous, FALSE); pack(toolbar, library->next, FALSE); pack(toolbar, library->import, FALSE); pack(toolbar, rekordbox, FALSE); pack(toolbar, library->cancel, FALSE);
    pack(toolbars, toolbar, FALSE); pack(toolbars, view_toolbar(library), FALSE); pack(body, toolbars, FALSE);
    const gchar *headers[] = { "Başlık", "Sanatçı", "Tür", "Ölçülen süre" }; GtkWidget *table = radios_ui_table(headers, 4);
    library->tree = g_object_get_data(G_OBJECT(table), "tree-view"); gtk_widget_set_name(library->tree, "media-table");
    library->model = gtk_list_store_new(5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    gtk_tree_view_set_model(GTK_TREE_VIEW(library->tree), GTK_TREE_MODEL(library->model)); g_object_unref(library->model);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(library->tree)), GTK_SELECTION_MULTIPLE);
    gtk_widget_set_size_request(table, -1, 180); pack(body, table, FALSE); columns_bind(library);
    GtkWidget *details = gtk_expander_new("Parça bilgileri"); gtk_widget_set_name(details, "media-metadata-expander");
    GtkWidget *metadata = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL);
    gtk_container_add(GTK_CONTAINER(details), metadata); pack(body, details, FALSE);
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(grid), 12); gtk_grid_set_row_spacing(GTK_GRID(grid), 8); gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    const gchar *labels[] = { "Başlık", "Tür", "Sanatçı", "Albüm", "Tarz", "Etiketler · virgülle ayır", "Yıl", "Not" };
    const gchar *names[] = { "media-title", "media-kind", "media-artist", "media-album", "media-genre", "media-tags", "media-year", "media-comment" };
    for (guint i = 0; i < 8; ++i) {
        GtkWidget *field = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "qwf-field"); pack(field, radios_ui_label(labels[i], "form-label"), FALSE);
        library->fields[i] = i == KIND ? kind_combo(names[i]) : radios_ui_entry(labels[i]); gtk_widget_set_name(library->fields[i], names[i]);
        if (GTK_IS_ENTRY(library->fields[i])) gtk_entry_set_max_length(GTK_ENTRY(library->fields[i]), i == COMMENT ? 2000 : i == YEAR ? 4 : i == TAGS ? 2592 : i == GENRE ? 120 : 200);
        pack(field, library->fields[i], FALSE); gtk_grid_attach(GTK_GRID(grid), field, i % 4, i / 4, 1, 1);
        g_signal_connect(library->fields[i], "changed", G_CALLBACK(changed), library);
    }
    pack(metadata, grid, FALSE); GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "pw-actions");
    library->save = button("Etiketleri kaydet", "media-tags-save");
    pack(actions, library->save, FALSE); pack(metadata, actions, FALSE);
    library->routing = radios_media_routing_new(ctx); pack(body, library->routing, FALSE);
    library->pick = button("Seçili medyayı ata", "media-pick-accept");
    gtk_widget_set_no_show_all(library->pick, TRUE); gtk_widget_hide(library->pick);
    pack(body, library->pick, FALSE); g_signal_connect(library->pick, "clicked", G_CALLBACK(pick_clicked), library);
    library->marker_host = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "media-marker-host");
    gtk_widget_set_name(library->marker_host, "media-marker-host"); pack(body, library->marker_host, FALSE);
    marker_apply(library);
    library->status = radios_ui_label("Medya kütüphanesi yükleniyor.", "application-status"); gtk_widget_set_name(library->status, "media-status"); pack(body, library->status, FALSE);
    library->import_status = radios_ui_label("", "application-status"); gtk_widget_set_name(library->import_status, "media-import-status"); pack(body, library->import_status, FALSE);
    g_object_set_data_full(G_OBJECT(library->dialog), "radios-media-library", library, library_free); g_object_set_data(G_OBJECT(ctx->parent), "radios-media-library-dialog", library->dialog);
    g_signal_connect(library->dialog, "destroy", G_CALLBACK(destroyed), library); g_signal_connect(library->dialog, "response", G_CALLBACK(close_response), NULL);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(library->tree)), "changed", G_CALLBACK(selected), library);
    g_signal_connect(library->search, "activate", G_CALLBACK(search_activate), library); g_signal_connect(library->previous, "clicked", G_CALLBACK(page_clicked), library); g_signal_connect(library->next, "clicked", G_CALLBACK(page_clicked), library);
    g_signal_connect(library->save, "clicked", G_CALLBACK(tags_save), library);
    g_signal_connect(library->import, "clicked", G_CALLBACK(import_clicked), library); g_signal_connect(library->cancel, "clicked", G_CALLBACK(import_cancel), library);
    radios_ui_bind_media_drop(library->dialog, ctx);
    enable_fields(library);
    /* A window drop builds the library unseen; presenting it later shows all. */
    GList *children = gtk_container_get_children(GTK_CONTAINER(library->dialog));
    for (GList *child = children; child; child = child->next) gtk_widget_show_all(child->data);
    g_list_free(children);
    if (show) gtk_widget_show(library->dialog);
    radios_data_register_refresh(ctx, library->dialog, library_refresh);
    if (radios_data_connected(ctx)) library_refresh(ctx, library->dialog); else message(library, "Medya kütüphanesi için veri servisine bağlan.");
}
void radios_media_library_open(RadioSViewContext *ctx) { library_open(ctx, TRUE); }
void radios_media_import_open(RadioSViewContext *ctx)
{
    if (!ctx || !GTK_IS_WINDOW(ctx->parent) || gtk_widget_in_destruction(GTK_WIDGET(ctx->parent))) return;
    radios_media_library_open(ctx);
    GtkWidget *dialog = g_object_get_data(G_OBJECT(ctx->parent), "radios-media-library-dialog");
    if (!dialog || gtk_widget_in_destruction(dialog)) return;
    MediaLibrary *library = state(dialog);
    if (library && !library->closed) import_clicked(NULL, library);
}
/* Chosen folders follow the window-drop path: each becomes a playlist named
 * after the folder and its audio files are imported into the library. */
static void folder_chooser_response(GtkDialog *dialog, gint response, gpointer value)
{
    RadioSViewContext *ctx = value;
    GSList *folders = response == GTK_RESPONSE_ACCEPT ? gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog)) : NULL;
    gtk_widget_destroy(GTK_WIDGET(dialog));
    if (folders && GTK_IS_WINDOW(ctx->parent) && !gtk_widget_in_destruction(GTK_WIDGET(ctx->parent))) {
        library_open(ctx, FALSE);
        GtkWidget *owner = g_object_get_data(G_OBJECT(ctx->parent), "radios-media-library-dialog");
        MediaLibrary *library = owner ? state(owner) : NULL;
        if (library && !library->closed) {
            if (!library->drop_jobs) library->drop_jobs = g_queue_new();
            for (GSList *folder = folders; folder; folder = folder->next) {
                DropJob *job = g_new0(DropJob, 1); job->folder = g_canonicalize_filename(folder->data, NULL);
                g_queue_push_tail(library->drop_jobs, job);
            }
            if (library->paths || library->cancellable || library->playlist_drop || library->busy || library->creating)
                library_notify(library, "Klasörler sıraya alındı; süren içe aktarma bitince başlayacak.");
            drop_jobs_next(library);
        }
    }
    g_slist_free_full(folders, g_free);
}
static void folder_chooser_destroyed(GtkWidget *widget, gpointer value)
{
    RadioSViewContext *ctx = value;
    if (GTK_IS_WINDOW(ctx->parent) && g_object_get_data(G_OBJECT(ctx->parent), "radios-media-folder-chooser") == widget)
        g_object_set_data(G_OBJECT(ctx->parent), "radios-media-folder-chooser", NULL);
}
void radios_media_folder_import_open(RadioSViewContext *ctx)
{
    if (!ctx || !GTK_IS_WINDOW(ctx->parent) || gtk_widget_in_destruction(GTK_WIDGET(ctx->parent))) return;
    GtkWidget *existing = g_object_get_data(G_OBJECT(ctx->parent), "radios-media-folder-chooser");
    if (existing) { gtk_window_present(GTK_WINDOW(existing)); return; }
    GtkWidget *chooser = gtk_file_chooser_dialog_new("Müzik klasörü ekle", ctx->parent, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Klasörü ekle", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(chooser, "media-folder-chooser"); gtk_window_set_destroy_with_parent(GTK_WINDOW(chooser), TRUE);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(chooser), TRUE); gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(chooser), TRUE);
    g_object_set_data(G_OBJECT(ctx->parent), "radios-media-folder-chooser", chooser);
    g_signal_connect(chooser, "destroy", G_CALLBACK(folder_chooser_destroyed), ctx);
    g_signal_connect(chooser, "response", G_CALLBACK(folder_chooser_response), ctx);
    gtk_widget_show_all(chooser);
}
gboolean radios_media_import_busy(RadioSViewContext *ctx)
{
    if (!ctx || !GTK_IS_WINDOW(ctx->parent)) return FALSE;
    MediaLibrary *library = state(g_object_get_data(G_OBJECT(ctx->parent), "radios-media-library-dialog"));
    return library && !library->closed && (library->paths || library->cancellable || library->chooser ||
        library->playlist_drop || library->busy || library->creating);
}
void radios_media_import_cancel(RadioSViewContext *ctx)
{
    if (!ctx || !GTK_IS_WINDOW(ctx->parent)) return;
    MediaLibrary *library = state(g_object_get_data(G_OBJECT(ctx->parent), "radios-media-library-dialog"));
    if (library && !library->closed && library->paths) import_cancel(NULL, library);
}
gboolean radios_media_import_files(RadioSViewContext *ctx, GPtrArray *paths, const gchar *kind,
    const gchar *playlist_id, gint64 playlist_revision, const gchar *label,
    RadioSMediaImportDone done, gpointer user)
{
    if (!paths) return FALSE;
    if (!ctx || !GTK_IS_WINDOW(ctx->parent) || gtk_widget_in_destruction(GTK_WIDGET(ctx->parent))) { g_ptr_array_unref(paths); return FALSE; }
    library_open(ctx, FALSE);
    MediaLibrary *library = state(g_object_get_data(G_OBJECT(ctx->parent), "radios-media-library-dialog"));
    if (!library || library->closed || !import_available(library)) { g_ptr_array_unref(paths); return FALSE; }
    library->batch_done = done; library->batch_user = user;
    gboolean queued = playlist_id && *playlist_id ?
        drop_headless(library, paths, kind, playlist_id, playlist_revision, label ? label : "Playlist") :
        import_begin(library, paths, kind, 0, NULL);
    if (!queued) { library->batch_done = NULL; library->batch_user = NULL; }
    return queued;
}
guint radios_media_catalogue_generation(RadioSViewContext *ctx)
{
    return ctx && GTK_IS_WINDOW(ctx->parent) ?
        GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(ctx->parent), "radios-media-catalogue-generation")) : 0;
}
static gboolean media_drag_drop(GtkWidget *widget, GdkDragContext *context, gint x, gint y, guint time, gpointer value)
{
    (void)x; (void)y; MediaDropBinding *binding = value;
    if (!binding->ctx) return FALSE;
    GdkAtom target = gtk_drag_dest_find_target(widget, context, NULL);
    if (target == GDK_NONE) return FALSE;
    gtk_drag_get_data(widget, context, target, time); return TRUE;
}
static void media_drop_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
    GtkSelectionData *selection, guint info, guint time, gpointer value)
{
    (void)x; (void)y; MediaDropBinding *binding = value; gboolean accepted = FALSE;
    RadioSViewContext *ctx = binding->ctx;
    if (!ctx) goto finished;
    GtkWidget *dialog = g_object_get_data(G_OBJECT(ctx->parent), "radios-media-library-dialog");
    gboolean window_drop = !dialog || widget != dialog;
    library_open(ctx, !window_drop);
    dialog = g_object_get_data(G_OBJECT(ctx->parent), "radios-media-library-dialog");
    MediaLibrary *library = state(dialog);
    if (!window_drop && !import_available(library)) goto finished;
    if (!selection || info != 0 || gtk_selection_data_get_format(selection) != 8 ||
        gtk_selection_data_get_target(selection) != gdk_atom_intern_static_string("text/uri-list") ||
        gtk_selection_data_get_length(selection) <= 0 || gtk_selection_data_get_length(selection) > 1024 * 1024) {
        gtk_label_set_text(GTK_LABEL(library->import_status), "Bırakılan dosya listesi okunamadı veya çok büyük; yerel ses dosyalarını bırak.");
        goto finished;
    }
    const guchar *bytes = gtk_selection_data_get_data(selection);
    if (!bytes || memchr(bytes, '\0', (gsize)gtk_selection_data_get_length(selection))) {
        gtk_label_set_text(GTK_LABEL(library->import_status), "Bırakılan dosya listesi bozuk; yerel ses dosyalarını yeniden bırak."); goto finished;
    }
    gchar **uris = gtk_selection_data_get_uris(selection);
    if (!uris || !uris[0]) {
        g_strfreev(uris); gtk_label_set_text(GTK_LABEL(library->import_status), "Bırakılan dosya listesi boş; yerel ses dosyalarını bırak."); goto finished;
    }
    guint count = g_strv_length(uris);
    if (count > 256) {
        g_strfreev(uris); gtk_label_set_text(GTK_LABEL(library->import_status), "Tek seferde en fazla 256 ses dosyası bırakabilirsin."); goto finished;
    }
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint rejected = 0, folders = 0; const gchar *reason = NULL;
    if (!library->drop_jobs) library->drop_jobs = g_queue_new();
    for (guint i = 0; i < count; ++i) {
        gchar *host = NULL; gchar *path = g_filename_from_uri(uris[i], &host, NULL);
        if (!path || (host && *host && g_ascii_strcasecmp(host, "localhost") != 0) || !g_path_is_absolute(path)) {
            ++rejected; reason = "Yalnız bu bilgisayardaki dosyalar kabul edilir; bağlantılar alınmadı."; g_free(path);
        } else if (window_drop && g_file_test(path, G_FILE_TEST_IS_DIR)) {
            DropJob *job = g_new0(DropJob, 1); job->folder = g_canonicalize_filename(path, NULL); g_free(path);
            g_queue_push_tail(library->drop_jobs, job); ++folders;
        } else if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            ++rejected; reason = "Klasörler ve bulunamayan dosyalar alınmadı."; g_free(path);
        } else {
            gchar *canonical = g_canonicalize_filename(path, NULL); g_free(path);
            if (g_hash_table_contains(seen, canonical)) {
                ++rejected; reason = "Aynı dosya bir kez içe aktarılır."; g_free(canonical);
            } else {
                g_hash_table_add(seen, g_strdup(canonical)); g_ptr_array_add(paths, canonical);
            }
        }
        g_free(host);
    }
    g_hash_table_unref(seen); g_strfreev(uris);
    if (!window_drop) accepted = import_begin(library, paths, "music", rejected, reason);
    else {
        /* Window drops never open a page: folders become playlists, loose
         * files are imported at once (into the open playlist, if any). */
        if (!paths->len && !folders) { accepted = import_begin(library, paths, "music", rejected, reason); goto finished; }
        if (paths->len) { DropJob *job = g_new0(DropJob, 1); job->paths = paths; g_queue_push_tail(library->drop_jobs, job); }
        else g_ptr_array_unref(paths);
        accepted = TRUE;
        if (rejected && reason) library_notify(library, reason);
        if (library->paths || library->cancellable || library->playlist_drop || library->busy || library->creating)
            library_notify(library, "Bırakılan dosyalar sıraya alındı; süren içe aktarma bitince başlayacak.");
        drop_jobs_next(library);
    }
finished:
    /* Acknowledges a queued COPY, never a MOVE: source files remain intact.
     * A NULL context is also safe for programmatic native signal dispatch. */
    if (context) gtk_drag_finish(context, accepted, FALSE, time);
}
static void media_drop_unbind(GtkWidget *widget, gpointer value)
{
    MediaDropBinding *binding = value; binding->ctx = NULL;
    if (binding->drop_handler && g_signal_handler_is_connected(widget, binding->drop_handler))
        g_signal_handler_disconnect(widget, binding->drop_handler);
    if (binding->received_handler && g_signal_handler_is_connected(widget, binding->received_handler))
        g_signal_handler_disconnect(widget, binding->received_handler);
    binding->drop_handler = binding->received_handler = 0;
    gtk_drag_dest_unset(widget);
}
void radios_ui_bind_media_drop(GtkWidget *widget, RadioSViewContext *ctx)
{
    if (g_object_get_data(G_OBJECT(widget), "radios-media-drop-bound")) return;
    static const GtkTargetEntry targets[] = { { "text/uri-list", 0, 0 } };
    gtk_drag_dest_set(widget, GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_HIGHLIGHT, targets, G_N_ELEMENTS(targets), GDK_ACTION_COPY);
    MediaDropBinding *binding = g_new0(MediaDropBinding, 1); binding->ctx = ctx;
    g_object_set_data_full(G_OBJECT(widget), "radios-media-drop-bound", binding, g_free);
    binding->drop_handler = g_signal_connect(widget, "drag-drop", G_CALLBACK(media_drag_drop), binding);
    binding->received_handler = g_signal_connect(widget, "drag-data-received", G_CALLBACK(media_drop_received), binding);
    g_signal_connect(widget, "destroy", G_CALLBACK(media_drop_unbind), binding);
}
static void bound_open(GtkButton *widget, gpointer value) { (void)widget; radios_media_library_open(value); }
static void deck_picked(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail, gpointer value)
{
    guint deck = GPOINTER_TO_UINT(value); GError *error = NULL;
    if (!radios_audio_ui_load_media(ctx, owner, detail, deck, FALSE, &error)) {
        if (ctx->notify) ctx->notify(ctx->notify_data, error ? error->message : "Medya yüklenemedi.");
        g_clear_error(&error); return;
    }
    radios_broadcast_player_library_loaded(ctx, deck);
    GtkWidget *main = g_object_get_data(G_OBJECT(owner), "radios-deck-main-widget");
    radios_audio_ui_set_deck_main(ctx, deck, main ? gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(main)) : TRUE, NULL);
    GtkWidget *title = g_object_get_data(G_OBJECT(owner), "radios-deck-title-widget");
    if (title) gtk_label_set_text(GTK_LABEL(title), radios_json_string(json_object_get_object_member(detail, "media"), "title", "—"));
}
void radios_media_library_open_for_deck(RadioSViewContext *ctx, guint deck)
{
    if (deck >= 32) return;
    gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
    GtkWidget *owner = g_object_get_data(G_OBJECT(ctx->parent), key); g_free(key);
    if (owner && !gtk_widget_in_destruction(owner)) radios_media_library_pick(ctx, owner, deck_picked, GUINT_TO_POINTER(deck));
    else radios_media_library_open(ctx);
}
void radios_ui_bind_media_library(GtkWidget *widget, RadioSViewContext *ctx) { g_signal_connect(widget, "clicked", G_CALLBACK(bound_open), ctx); }
void radios_media_library_pick(RadioSViewContext *ctx, GtkWidget *owner, RadioSMediaPick callback, gpointer user)
{
    if (!ctx || !GTK_IS_WIDGET(owner) || gtk_widget_in_destruction(owner) || !callback) return;
    radios_media_library_open(ctx);
    MediaLibrary *library = state(g_object_get_data(G_OBJECT(ctx->parent), "radios-media-library-dialog"));
    GtkWidget *toplevel = gtk_widget_get_toplevel(owner);
    if (GTK_IS_WINDOW(toplevel)) gtk_window_set_transient_for(GTK_WINDOW(library->dialog), GTK_WINDOW(toplevel));
    pick_clear(library); g_weak_ref_set(&library->pick_owner, owner);
    library->pick_callback = callback; library->pick_user = user;
    library->pick_handler = g_signal_connect(owner, "destroy", G_CALLBACK(pick_target_destroyed), library);
    enable_fields(library);
    message(library, "Doğrulanmış kaydı seçip Seçili medyayı ata düğmesini kullan.");
}
gboolean radios_media_library_get(RadioSViewContext *ctx, GtkWidget *owner, const gchar *media_id,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{
    if (!g_uuid_string_is_valid(media_id)) { if (destroy) destroy(user); return FALSE; }
    JsonNode *body = radios_data_body(ctx);
    json_object_set_string_member(json_node_get_object(body), "media_id", media_id);
    gboolean queued = radios_data_request(ctx, owner, "POST", "/v1/workflows/desktop-media/get", body, callback, user, destroy);
    json_node_free(body); return queued;
}
void radios_media_library_open_selected(RadioSViewContext *ctx, const gchar *media_id)
{
    radios_media_library_open(ctx);
    GtkWidget *dialog = g_object_get_data(G_OBJECT(ctx->parent), "radios-media-library-dialog"); MediaLibrary *library = state(dialog);
    if (!media_id || !*media_id || library->busy || radios_media_routing_busy(library->routing)) return;
    if ((library->dirty || markers_dirty(library)) && g_strcmp0(library->selected, media_id) != 0) { message(library, "Önce mevcut etiket değişikliklerini kaydet veya geri al."); return; }
    if (g_strcmp0(library->selected, media_id) == 0 && (library->dirty || markers_dirty(library))) return;
    g_free(library->selected); library->selected = g_strdup(media_id);
    library->updating = TRUE; gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(library->tree)));
    g_clear_pointer(&library->batch_records, json_array_unref);
    g_hash_table_remove_all(library->selected_ids); g_hash_table_add(library->selected_ids, g_strdup(media_id)); select_id(library, media_id);
    library->updating = FALSE; selection_routes(library); detail_request(library);
}
