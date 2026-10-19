#include "radios/playlist_browser.h"
#include "radios/widgets.h"
#include "radios/data.h"
#include "radios/media_ui.h"
#include "radios/catalogue_playlist.h"
#include <math.h>
#include <string.h>

enum { SOURCE_ARTIST, SOURCE_TITLE, SOURCE_ALBUM, SOURCE_DURATION, SOURCE_ROW, SOURCE_COLUMNS };

typedef struct {
    gchar *title, *artist, *album;
    gint64 duration_ms;
    JsonObject *metadata;
} BrowserRow;

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *widget, *tree, *query, *status, *empty, *add, *heading;
    GtkWidget *previous, *next, *page_label;
    GtkListStore *store;
    GPtrArray *rows;
    GArray *page_history;
    RadioSDataClient *scope;
    GtkTreePath *anchor, *pressed;
    gint press_x, press_y;
    guint generation, search_timer, offset, next_offset, media_generation;
    gboolean updating, closed, has_next, collapse_on_release, dragging, open_on_release;
} PlaylistBrowser;

typedef struct {
    RadioSDataClient *client;
    gchar *query;
    guint generation;
} BrowserRequest;

static void browser_load(PlaylistBrowser *browser);

static JsonObject *object_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

static JsonArray *array_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

static JsonObject *array_object(JsonArray *array, guint index)
{
    JsonNode *node = array ? json_array_get_element(array, index) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

static gboolean bool_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}

static JsonObject *object_copy(JsonObject *source)
{
    JsonObject *copy = json_object_new();
    JsonObjectIter iter;
    const gchar *key;
    JsonNode *value;
    json_object_iter_init(&iter, source);
    while (json_object_iter_next(&iter, &key, &value))
        json_object_set_member(copy, key, json_node_copy(value));
    return copy;
}

static PlaylistBrowser *browser_state(GtkWidget *widget)
{
    return widget ? g_object_get_data(G_OBJECT(widget), "radios-playlist-browser") : NULL;
}

static GtkWidget *named_button(const gchar *label, const gchar *name)
{
    GtkWidget *button = radios_ui_button(label);
    gtk_widget_set_name(button, name);
    return button;
}

static void pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{
    gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0);
}

static void row_free(gpointer data)
{
    BrowserRow *row = data;
    if (!row) return;
    g_free(row->title); g_free(row->artist); g_free(row->album);
    if (row->metadata) json_object_unref(row->metadata);
    g_free(row);
}

static gboolean valid_binding(JsonObject *metadata)
{
    const gchar *ids[] = {"media_id", "version_id", "media_file_id"};
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i)
        if (!g_uuid_string_is_valid(radios_json_string(metadata, ids[i], ""))) return FALSE;
    const gchar *digest = radios_json_string(metadata, "media_sha256", "");
    if (strlen(digest) != 64) return FALSE;
    for (guint i = 0; i < 64; ++i)
        if (!g_ascii_isxdigit(digest[i]) || g_ascii_isupper(digest[i])) return FALSE;
    return TRUE;
}

static BrowserRow *local_row(JsonObject *record)
{
    JsonObject *version = object_member(record, "version");
    if (!version || !bool_member(version, "verified")) return NULL;
    JsonObject *metadata = radios_catalogue_playlist_track(record);
    if (!metadata || !valid_binding(metadata)) {
        if (metadata) json_object_unref(metadata);
        return NULL;
    }
    JsonNode *bpm = json_object_get_member(metadata, "bpm");
    gdouble tempo = bpm && JSON_NODE_HOLDS_VALUE(bpm) ? json_node_get_double(bpm) : 0;
    if (!isfinite(tempo) || tempo < 0 || tempo > 1000) json_object_set_double_member(metadata, "bpm", 0);
    BrowserRow *row = g_new0(BrowserRow, 1);
    row->metadata = metadata;
    row->title = g_strdup(radios_json_string(metadata, "title", ""));
    row->artist = g_strdup(radios_json_string(metadata, "artist", ""));
    row->album = g_strdup(radios_json_string(metadata, "album", ""));
    row->duration_ms = radios_json_int(metadata, "duration_ms", 0);
    return row;
}

static gchar *duration_text(gint64 duration)
{
    if (duration <= 0) return g_strdup("—");
    guint64 seconds = (guint64)duration / 1000;
    return g_strdup_printf("%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT, seconds / 60, seconds % 60);
}

static gboolean identity_current(PlaylistBrowser *browser, RadioSDataClient *identity)
{
    RadioSDataClient *current = radios_data_client_acquire(browser->ctx);
    gboolean same = current && identity && current == identity &&
        !g_strcmp0(radios_data_client_station_id(current), radios_data_client_station_id(identity)) &&
        !g_strcmp0(radios_data_client_user_id(current), radios_data_client_user_id(identity)) &&
        !g_strcmp0(radios_data_client_store_id(current), radios_data_client_store_id(identity));
    if (current) radios_data_client_unref(current);
    return same && radios_data_connected(browser->ctx);
}

static JsonArray *selected_rows(PlaylistBrowser *browser)
{
    JsonArray *entries = json_array_new();
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(browser->tree));
    GList *paths = gtk_tree_selection_get_selected_rows(selection, NULL);
    for (GList *item = paths; item; item = item->next) {
        GtkTreeIter iter;
        BrowserRow *row = NULL;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(browser->store), &iter, item->data))
            gtk_tree_model_get(GTK_TREE_MODEL(browser->store), &iter, SOURCE_ROW, &row, -1);
        if (row && row->metadata) json_array_add_object_element(entries, object_copy(row->metadata));
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return entries;
}

JsonArray *radios_playlist_browser_selected(GtkWidget *widget)
{
    PlaylistBrowser *browser = browser_state(widget);
    if (!browser || browser->closed || !identity_current(browser, browser->scope)) return json_array_new();
    return selected_rows(browser);
}

static void selection_changed(GtkTreeSelection *selection, gpointer data)
{
    PlaylistBrowser *browser = data;
    guint selected = (guint)gtk_tree_selection_count_selected_rows(selection);
    gtk_widget_set_sensitive(browser->add, selected > 0);
}

static gboolean select_song(GtkTreeSelection *selection, GtkTreeModel *model,
    GtkTreePath *path, gboolean selected, gpointer data)
{
    (void)selection; (void)selected; (void)data;
    GtkTreeIter iter; BrowserRow *row = NULL;
    if (gtk_tree_model_get_iter(model, &iter, path)) gtk_tree_model_get(model, &iter, SOURCE_ROW, &row, -1);
    return row && row->metadata;
}

static void clear_rows(PlaylistBrowser *browser)
{
    gtk_list_store_clear(browser->store);
    g_ptr_array_set_size(browser->rows, 0);
    g_clear_pointer(&browser->anchor, gtk_tree_path_free);
    g_clear_pointer(&browser->pressed, gtk_tree_path_free);
    browser->collapse_on_release = FALSE;
    browser->open_on_release = FALSE;
    gtk_widget_set_sensitive(browser->add, FALSE);
    gtk_widget_show(browser->empty);
}

static void render_rows(PlaylistBrowser *browser, GPtrArray *rows)
{
    clear_rows(browser);
    for (guint i = 0; i < rows->len; ++i) {
        BrowserRow *row = g_ptr_array_index(rows, i);
        if (!row) continue;
        g_ptr_array_index(rows, i) = NULL;
        g_ptr_array_add(browser->rows, row);
        GtkTreeIter iter;
        gchar *duration = duration_text(row->duration_ms);
        gtk_list_store_append(browser->store, &iter);
        gtk_list_store_set(browser->store, &iter, SOURCE_ARTIST, row->artist,
            SOURCE_TITLE, row->title, SOURCE_ALBUM, row->album, SOURCE_DURATION, duration,
            SOURCE_ROW, row, -1);
        g_free(duration);
    }
    gtk_widget_set_visible(browser->empty, browser->rows->len == 0);
    if (browser->rows->len == 0) gtk_label_set_text(GTK_LABEL(browser->empty), "Aramaya uyan içerik bulunamadı.");
}

static void pagination_update(PlaylistBrowser *browser, gint64 total)
{
    gtk_widget_set_visible(browser->previous, TRUE);
    gtk_widget_set_visible(browser->next, TRUE);
    gtk_widget_set_sensitive(browser->previous, browser->page_history->len > 0);
    gtk_widget_set_sensitive(browser->next, browser->has_next);
    gchar *text = total >= 0 ? g_strdup_printf("%u içerik · toplam %" G_GINT64_FORMAT, browser->rows->len, total) :
        g_strdup_printf("%u içerik", browser->rows->len);
    gtk_label_set_text(GTK_LABEL(browser->page_label), text); g_free(text);
}

static void request_free(gpointer data)
{
    BrowserRequest *request = data;
    if (request->client) radios_data_client_unref(request->client);
    g_free(request->query);
    g_free(request);
}

static BrowserRequest *request_new(PlaylistBrowser *browser)
{
    BrowserRequest *request = g_new0(BrowserRequest, 1);
    request->client = radios_data_client_acquire(browser->ctx);
    request->generation = browser->generation;
    request->query = g_strdup(gtk_entry_get_text(GTK_ENTRY(browser->query)));
    g_strstrip(request->query);
    return request;
}

static void local_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *error, gpointer data)
{
    (void)ctx;
    BrowserRequest *request = data;
    PlaylistBrowser *browser = browser_state(owner);
    if (!browser || browser->closed || request->generation != browser->generation) return;
    if (!identity_current(browser, request->client)) {
        clear_rows(browser); gtk_label_set_text(GTK_LABEL(browser->status), "RadioS oturumu değişti; kaynakları yenileyin."); return;
    }
    if (error || !node || !JSON_NODE_HOLDS_OBJECT(node)) {
        gtk_label_set_text(GTK_LABEL(browser->status), error ? error->message : "Medya arşivi alınamadı."); return;
    }
    JsonObject *response = json_node_get_object(node);
    JsonArray *records = array_member(response, "records");
    GPtrArray *rows = g_ptr_array_new_with_free_func(row_free);
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        BrowserRow *row = local_row(array_object(records, i));
        if (row) g_ptr_array_add(rows, row);
    }
    render_rows(browser, rows); g_ptr_array_unref(rows);
    JsonNode *next = json_object_get_member(response, "next_offset");
    browser->has_next = next && !JSON_NODE_HOLDS_NULL(next);
    browser->next_offset = browser->has_next ? (guint)json_node_get_int(next) : 0;
    pagination_update(browser, radios_json_int(response, "total", 0));
    gtk_label_set_text(GTK_LABEL(browser->status), "");
    if (!browser->rows->len) gtk_label_set_text(GTK_LABEL(browser->empty), "Ana medyada içerik yok.");
}

static void browser_load(PlaylistBrowser *browser)
{
    if (browser->closed) return;
    browser->media_generation = radios_media_catalogue_generation(browser->ctx);
    if (browser->search_timer) { g_source_remove(browser->search_timer); browser->search_timer = 0; }
    ++browser->generation;
    clear_rows(browser);
    browser->has_next = FALSE; pagination_update(browser, -1);
    if (browser->scope) radios_data_client_unref(browser->scope);
    browser->scope = radios_data_client_acquire(browser->ctx);
    if (!radios_data_connected(browser->ctx) || !browser->scope) {
        gtk_label_set_text(GTK_LABEL(browser->status), "İçerikleri görmek için RadioS veri bağlantısı gerekiyor.");
        gtk_label_set_text(GTK_LABEL(browser->empty), "Medya arşivi bağlantısı bekleniyor."); return;
    }
    BrowserRequest *request = request_new(browser);
    if (g_utf8_strlen(request->query, -1) > 200) {
        gtk_label_set_text(GTK_LABEL(browser->status), "Arama en fazla 200 karakter olabilir.");
        request_free(request); return;
    }
    gtk_label_set_text(GTK_LABEL(browser->status), "Kaynaklar yükleniyor…");
    gtk_label_set_text(GTK_LABEL(browser->empty), "Yükleniyor…");
    JsonNode *body = radios_data_body(browser->ctx);
    JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "query", request->query);
    json_object_set_int_member(object, "offset", browser->offset);
    json_object_set_int_member(object, "limit", 200);
    if (!radios_data_request(browser->ctx, browser->widget, "POST", "/v1/workflows/desktop-media/list", body, local_done, request, request_free))
        gtk_label_set_text(GTK_LABEL(browser->status), "Medya arşivi isteği başlatılamadı.");
    json_node_free(body);
}

void radios_playlist_browser_refresh(GtkWidget *widget)
{
    PlaylistBrowser *browser = browser_state(widget);
    if (browser) browser_load(browser);
}

static void refresh_clicked(GtkButton *button, gpointer data)
{
    (void)button; browser_load(data);
}

static gboolean search_timeout(gpointer data)
{
    PlaylistBrowser *browser = data;
    browser->search_timer = 0;
    browser_load(browser); return G_SOURCE_REMOVE;
}

static void search_changed(GtkEditable *entry, gpointer data)
{
    (void)entry; PlaylistBrowser *browser = data;
    if (browser->updating || browser->closed) return;
    if (browser->search_timer) g_source_remove(browser->search_timer);
    ++browser->generation;
    clear_rows(browser);
    browser->offset = 0; g_array_set_size(browser->page_history, 0);
    browser->search_timer = g_timeout_add(350, search_timeout, browser);
}

static void search_activate(GtkEntry *entry, gpointer data)
{
    (void)entry; browser_load(data);
}

static void page_clicked(GtkButton *button, gpointer data)
{
    PlaylistBrowser *browser = data;
    if (GTK_WIDGET(button) == browser->next && browser->has_next) {
        g_array_append_val(browser->page_history, browser->offset); browser->offset = browser->next_offset;
    } else if (GTK_WIDGET(button) == browser->previous && browser->page_history->len) {
        browser->offset = g_array_index(browser->page_history, guint, browser->page_history->len - 1);
        g_array_set_size(browser->page_history, browser->page_history->len - 1);
    } else return;
    browser_load(browser);
}

static BrowserRow *row_at_path(PlaylistBrowser *browser, GtkTreePath *path)
{
    GtkTreeIter iter; BrowserRow *row = NULL;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(browser->store), &iter, path))
        gtk_tree_model_get(GTK_TREE_MODEL(browser->store), &iter, SOURCE_ROW, &row, -1);
    return row;
}

static void media_open(PlaylistBrowser *browser, BrowserRow *row)
{
    if (browser->closed || !row ||
        !row->metadata || !valid_binding(row->metadata) || !identity_current(browser, browser->scope)) return;
    gchar *media_id = g_strdup(radios_json_string(row->metadata, "media_id", ""));
    radios_media_library_open_selected(browser->ctx, media_id);
    g_free(media_id);
}

static gboolean row_pressed(GtkWidget *tree, GdkEventButton *event, gpointer data)
{
    PlaylistBrowser *browser = data;
    if (event->button != 1 || event->type != GDK_BUTTON_PRESS) return FALSE;
    browser->open_on_release = FALSE;
    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree), (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL)) return FALSE;
    BrowserRow *row = row_at_path(browser, path);
    if (!row || !row->metadata) {
        gtk_tree_path_free(path); return TRUE;
    }
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    gboolean selected = gtk_tree_selection_path_is_selected(selection, path);
    browser->collapse_on_release = FALSE;
    browser->open_on_release = !(event->state & (GDK_SHIFT_MASK | GDK_MOD1_MASK | GDK_CONTROL_MASK));
    if (event->state & GDK_SHIFT_MASK) {
        if (!browser->anchor) browser->anchor = gtk_tree_path_copy(path);
        if (!(event->state & (GDK_MOD1_MASK | GDK_CONTROL_MASK))) gtk_tree_selection_unselect_all(selection);
        gtk_tree_selection_select_range(selection, browser->anchor, path);
    } else if (event->state & (GDK_MOD1_MASK | GDK_CONTROL_MASK)) {
        if (selected) gtk_tree_selection_unselect_path(selection, path);
        else gtk_tree_selection_select_path(selection, path);
        g_clear_pointer(&browser->anchor, gtk_tree_path_free); browser->anchor = gtk_tree_path_copy(path);
    } else {
        if (!selected) { gtk_tree_selection_unselect_all(selection); gtk_tree_selection_select_path(selection, path); }
        else browser->collapse_on_release = gtk_tree_selection_count_selected_rows(selection) > 1;
        g_clear_pointer(&browser->anchor, gtk_tree_path_free); browser->anchor = gtk_tree_path_copy(path);
    }
    g_clear_pointer(&browser->pressed, gtk_tree_path_free);
    if (gtk_tree_selection_path_is_selected(selection, path)) browser->pressed = gtk_tree_path_copy(path);
    browser->press_x = (gint)event->x; browser->press_y = (gint)event->y;
    gtk_widget_grab_focus(tree); gtk_tree_path_free(path); return TRUE;
}

static gboolean row_released(GtkWidget *tree, GdkEventButton *event, gpointer data)
{
    PlaylistBrowser *browser = data;
    if (event->button != 1) return FALSE;
    if (browser->pressed && browser->collapse_on_release && !browser->dragging) {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
        gtk_tree_selection_unselect_all(selection); gtk_tree_selection_select_path(selection, browser->pressed);
    }
    BrowserRow *row = NULL;
    if (browser->pressed && browser->open_on_release && !browser->dragging &&
        !(event->state & (GDK_SHIFT_MASK | GDK_MOD1_MASK | GDK_CONTROL_MASK)) &&
        !gtk_drag_check_threshold(tree, browser->press_x, browser->press_y, (gint)event->x, (gint)event->y)) {
        GtkTreePath *released = NULL;
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree), (gint)event->x, (gint)event->y, &released, NULL, NULL, NULL)) {
            if (gtk_tree_path_compare(browser->pressed, released) == 0) row = row_at_path(browser, released);
            gtk_tree_path_free(released);
        }
    }
    g_clear_pointer(&browser->pressed, gtk_tree_path_free);
    browser->collapse_on_release = browser->open_on_release = FALSE;
    if (row) media_open(browser, row);
    return TRUE;
}

static gboolean row_motion(GtkWidget *tree, GdkEventMotion *event, gpointer data)
{
    PlaylistBrowser *browser = data;
    if (!browser->pressed || !(event->state & GDK_BUTTON1_MASK) || browser->dragging) return FALSE;
    if (!gtk_drag_check_threshold(tree, browser->press_x, browser->press_y, (gint)event->x, (gint)event->y)) return FALSE;
    browser->dragging = TRUE; browser->collapse_on_release = browser->open_on_release = FALSE;
    GtkTargetList *targets = gtk_drag_source_get_target_list(tree);
    gtk_drag_begin_with_coordinates(tree, targets, GDK_ACTION_COPY, 1, (GdkEvent *)event, browser->press_x, browser->press_y);
    g_clear_pointer(&browser->pressed, gtk_tree_path_free); return TRUE;
}

static void drag_finished(GtkWidget *tree, GdkDragContext *context, gpointer data)
{
    (void)tree; (void)context; PlaylistBrowser *browser = data;
    browser->dragging = FALSE; browser->collapse_on_release = browser->open_on_release = FALSE;
    g_clear_pointer(&browser->pressed, gtk_tree_path_free);
}

static void drag_get(GtkWidget *tree, GdkDragContext *context, GtkSelectionData *selection,
    guint info, guint time, gpointer data)
{
    (void)tree; (void)context; (void)info; (void)time;
    PlaylistBrowser *browser = data;
    JsonArray *entries = radios_playlist_browser_selected(browser->widget);
    if (json_array_get_length(entries)) {
        JsonNode *node = json_node_new(JSON_NODE_ARRAY); json_node_take_array(node, entries);
        gchar *text = json_to_string(node, FALSE);
        gtk_selection_data_set(selection, gdk_atom_intern_static_string(RADIOS_PLAYLIST_ENTRIES_MIME), 8,
            (const guchar *)text, (gint)strlen(text));
        g_free(text); json_node_free(node);
    } else json_array_unref(entries);
}

static void row_activated(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column, gpointer data)
{
    (void)tree; (void)column; PlaylistBrowser *browser = data;
    media_open(browser, row_at_path(browser, path));
}

static void data_refreshed(RadioSViewContext *ctx, GtkWidget *owner)
{
    (void)ctx; PlaylistBrowser *browser = browser_state(owner);
    if (!browser || browser->closed) return;
    /* Ordinary target writes must not reset source rows or the user's selection. */
    if (!identity_current(browser, browser->scope) ||
            browser->media_generation != radios_media_catalogue_generation(browser->ctx)) browser_load(browser);
}

static void browser_destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; PlaylistBrowser *browser = data;
    browser->closed = TRUE; ++browser->generation;
    if (browser->search_timer) { g_source_remove(browser->search_timer); browser->search_timer = 0; }
}

static void browser_free(gpointer data)
{
    PlaylistBrowser *browser = data;
    if (browser->search_timer) g_source_remove(browser->search_timer);
    if (browser->scope) radios_data_client_unref(browser->scope);
    g_clear_pointer(&browser->anchor, gtk_tree_path_free); g_clear_pointer(&browser->pressed, gtk_tree_path_free);
    g_ptr_array_unref(browser->rows); g_array_unref(browser->page_history); g_object_unref(browser->store);
    g_free(browser);
}

GtkWidget *radios_playlist_browser_new(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx, NULL);
    PlaylistBrowser *browser = g_new0(PlaylistBrowser, 1);
    browser->ctx = ctx;
    browser->rows = g_ptr_array_new_with_free_func(row_free);
    browser->page_history = g_array_new(FALSE, FALSE, sizeof(guint));
    browser->widget = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "pw-source-panel");
    gtk_widget_set_name(browser->widget, "pw-source-browser");
    g_object_set_data_full(G_OBJECT(browser->widget), "radios-playlist-browser", browser, browser_free);
    g_signal_connect(browser->widget, "destroy", G_CALLBACK(browser_destroyed), browser);
    GtkWidget *strip = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "pw-source-strip");
    browser->query = radios_ui_entry("Ana medyada parça, sanatçı veya albüm ara…");
    gtk_widget_set_name(browser->query, "pw-source-search"); pack(strip, browser->query, TRUE);
    g_signal_connect(browser->query, "changed", G_CALLBACK(search_changed), browser);
    g_signal_connect(browser->query, "activate", G_CALLBACK(search_activate), browser);
    GtkWidget *refresh = named_button("Yenile", "pw-source-refresh");
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked), browser); pack(strip, refresh, FALSE); pack(browser->widget, strip, FALSE);
    browser->heading = radios_ui_label("Ana medya", "strong"); gtk_widget_set_name(browser->heading, "pw-source-heading"); pack(browser->widget, browser->heading, FALSE);
    browser->store = gtk_list_store_new(SOURCE_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER);
    browser->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(browser->store)); gtk_widget_set_name(browser->tree, "pw-source-table");
    const gchar *headers[] = {"SANATÇI", "PARÇA", "ALBÜM", "SÜRE"};
    for (guint i = 0; i < G_N_ELEMENTS(headers); ++i) {
        GtkCellRenderer *cell = gtk_cell_renderer_text_new();
        g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 6u, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(headers[i], cell, "text", (gint)i, NULL);
        gtk_tree_view_column_set_expand(column, i < 3); gtk_tree_view_column_set_min_width(column, i < 3 ? 140 : 90);
        gtk_tree_view_append_column(GTK_TREE_VIEW(browser->tree), column);
    }
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(browser->tree), FALSE);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(browser->tree));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
    gtk_tree_selection_set_select_function(selection, select_song, browser, NULL);
    g_signal_connect(selection, "changed", G_CALLBACK(selection_changed), browser);
    g_signal_connect(browser->tree, "button-press-event", G_CALLBACK(row_pressed), browser);
    g_signal_connect(browser->tree, "button-release-event", G_CALLBACK(row_released), browser);
    g_signal_connect(browser->tree, "motion-notify-event", G_CALLBACK(row_motion), browser);
    g_signal_connect(browser->tree, "row-activated", G_CALLBACK(row_activated), browser);
    GtkTargetEntry target = {(gchar *)RADIOS_PLAYLIST_ENTRIES_MIME, GTK_TARGET_SAME_APP, 0};
    gtk_drag_source_set(browser->tree, 0, &target, 1, GDK_ACTION_COPY);
    g_signal_connect(browser->tree, "drag-data-get", G_CALLBACK(drag_get), browser);
    g_signal_connect(browser->tree, "drag-end", G_CALLBACK(drag_finished), browser);
    gtk_widget_add_events(browser->tree, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), browser->tree);
    GtkWidget *overlay = gtk_overlay_new(); gtk_container_add(GTK_CONTAINER(overlay), scroll);
    gtk_widget_set_size_request(overlay, -1, 120);
    browser->empty = radios_ui_label("Medya arşivi bağlantısı bekleniyor.", "pw-empty"); gtk_widget_set_name(browser->empty, "pw-source-empty");
    gtk_widget_set_halign(browser->empty, GTK_ALIGN_CENTER); gtk_widget_set_valign(browser->empty, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all(browser->empty, TRUE); gtk_overlay_add_overlay(GTK_OVERLAY(overlay), browser->empty);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), browser->empty, TRUE); pack(browser->widget, overlay, TRUE);
    GtkWidget *footer = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "pw-bottom");
    browser->add = named_button("Seçilenleri aşağıdaki playliste ekle ↓", "pw-source-add");
    gtk_widget_set_sensitive(browser->add, FALSE); pack(footer, browser->add, FALSE); pack(browser->widget, footer, FALSE);
    GtkWidget *pagination = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "pw-pagination");
    browser->page_label = radios_ui_label("0 içerik", NULL); gtk_widget_set_name(browser->page_label, "pw-source-count"); pack(pagination, browser->page_label, TRUE);
    browser->previous = named_button("Önceki", "pw-source-previous"); browser->next = named_button("Sonraki", "pw-source-next");
    gtk_widget_set_no_show_all(browser->previous, TRUE); gtk_widget_set_no_show_all(browser->next, TRUE);
    g_signal_connect(browser->previous, "clicked", G_CALLBACK(page_clicked), browser); g_signal_connect(browser->next, "clicked", G_CALLBACK(page_clicked), browser);
    pack(pagination, browser->previous, FALSE); pack(pagination, browser->next, FALSE); pack(browser->widget, pagination, FALSE);
    browser->status = radios_ui_label("", "pw-status"); gtk_widget_set_name(browser->status, "pw-source-status");
    gtk_label_set_line_wrap(GTK_LABEL(browser->status), TRUE); pack(browser->widget, browser->status, FALSE);
    radios_data_register_refresh(ctx, browser->widget, data_refreshed);
    browser_load(browser);
    return browser->widget;
}
