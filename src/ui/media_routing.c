#include "radios/media_routing.h"
#include "radios/widgets.h"
#include <string.h>

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *panel, *rows, *rows_scroll, *status, *choices, *choices_scroll, *summary, *apply;
    JsonArray *selected, *playlists;
    GPtrArray *checks, *jobs;
    GHashTable *targets;
    JsonNode *retry;
    guint index, completed, generation;
    gboolean busy, loading, updating, outcome;
} Routing;

static void routing_refresh(RadioSViewContext *ctx, GtkWidget *owner);
static void route_next(Routing *routing);
static Routing *state(GtkWidget *panel) { return g_object_get_data(G_OBJECT(panel), "radios-media-routing"); }
static void pack(GtkWidget *box, GtkWidget *child, gboolean expand) { gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static void message(Routing *routing, const gchar *text)
{ gtk_label_set_text(GTK_LABEL(routing->status), text); gtk_widget_set_visible(routing->status, text && *text); }
static JsonNode *copy(JsonNode *node)
{
    JsonGenerator *generator = json_generator_new(); json_generator_set_root(generator, node);
    gchar *text = json_generator_to_data(generator, NULL); JsonParser *parser = json_parser_new();
    JsonNode *result = json_parser_load_from_data(parser, text, -1, NULL) ? json_node_copy(json_parser_get_root(parser)) : NULL;
    g_object_unref(parser); g_object_unref(generator); g_free(text); return result;
}
static JsonObject *playlist(Routing *routing, const gchar *id)
{
    for (guint i = 0; routing->playlists && i < json_array_get_length(routing->playlists); ++i) {
        JsonObject *row = json_array_get_object_element(routing->playlists, i);
        if (!g_strcmp0(radios_json_string(row, "_id", ""), id)) return row;
    }
    return NULL;
}
static gboolean contains(JsonObject *list, const gchar *media_id)
{
    JsonArray *tracks = list ? json_object_get_array_member(list, "tracks") : NULL;
    for (guint i = 0; tracks && i < json_array_get_length(tracks); ++i)
        if (!g_strcmp0(radios_json_string(json_array_get_object_element(tracks, i), "media_id", ""), media_id)) return TRUE;
    return FALSE;
}
static gchar *membership(Routing *routing, const gchar *id)
{
    GString *names = g_string_new("");
    for (guint i = 0; routing->playlists && i < json_array_get_length(routing->playlists); ++i) {
        JsonObject *row = json_array_get_object_element(routing->playlists, i);
        if (contains(row, id)) {
            if (names->len) g_string_append(names, ", ");
            g_string_append(names, radios_json_string(row, "name", ""));
        }
    }
    if (!names->len) g_string_append(names, routing->playlists ? "Hiçbir playlistte değil" : "Playlist bilgisi yükleniyor");
    return g_string_free(names, FALSE);
}
static void enable(Routing *routing)
{
    gboolean available = !routing->busy && !routing->loading && radios_data_connected(routing->ctx);
    guint selected = routing->selected ? json_array_get_length(routing->selected) : 0;
    guint targets = g_hash_table_size(routing->targets);
    gtk_widget_set_sensitive(routing->apply, available && selected && (targets || routing->retry));
    for (guint i = 0; i < routing->checks->len; ++i)
        gtk_widget_set_sensitive(g_ptr_array_index(routing->checks, i), available && selected && !routing->retry);
    gchar *text = g_strdup_printf("%u parça · %u playlist seçili", selected, targets);
    gtk_label_set_text(GTK_LABEL(routing->summary), text); g_free(text);
}
static void target_changed(GtkToggleButton *check, gpointer value)
{
    Routing *routing = value; if (routing->updating) return;
    const gchar *id = g_object_get_data(G_OBJECT(check), "playlist-id");
    if (id) {
        if (gtk_toggle_button_get_active(check)) g_hash_table_add(routing->targets, g_strdup(id));
        else g_hash_table_remove(routing->targets, id);
        routing->outcome = FALSE; message(routing, ""); enable(routing);
    }
}
static void clear_children(GtkWidget *parent)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(parent));
    for (GList *item = children; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(children);
}
static void rows_rebuild(Routing *routing)
{
    routing->updating = TRUE;
    clear_children(routing->rows); clear_children(routing->choices); g_ptr_array_set_size(routing->checks, 0);
    for (guint i = 0; routing->playlists && i < json_array_get_length(routing->playlists); ++i) {
        JsonObject *list = json_array_get_object_element(routing->playlists, i);
        const gchar *id = radios_json_string(list, "_id", "");
        GtkWidget *check = gtk_check_button_new_with_label(radios_json_string(list, "name", ""));
        GtkWidget *label = gtk_bin_get_child(GTK_BIN(check)); gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 32); gtk_widget_set_tooltip_text(check, radios_json_string(list, "name", ""));
        gchar *name = g_strdup_printf("media-playlist-choice-%u", i); gtk_widget_set_name(check, name); g_free(name);
        g_object_set_data_full(G_OBJECT(check), "playlist-id", g_strdup(id), g_free);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), g_hash_table_contains(routing->targets, id));
        g_signal_connect(check, "toggled", G_CALLBACK(target_changed), routing);
        gtk_container_add(GTK_CONTAINER(routing->choices), check); g_ptr_array_add(routing->checks, check);
    }
    for (guint i = 0; routing->selected && i < json_array_get_length(routing->selected); ++i) {
        JsonObject *detail = json_array_get_object_element(routing->selected, i), *media = json_object_get_object_member(detail, "media");
        const gchar *id = radios_json_string(media, "_id", "");
        GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 12, "media-playlist-route-row");
        GtkWidget *title = radios_ui_label(radios_json_string(media, "title", ""), "form-label");
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(title), 24);
        gtk_widget_set_tooltip_text(title, radios_json_string(media, "title", "")); pack(row, title, FALSE);
        gchar *names = membership(routing, id); GtkWidget *label = radios_ui_label(names, "pw-info");
        gchar *name = g_strdup_printf("media-playlist-membership-%u", i); gtk_widget_set_name(label, name); g_free(name); g_free(names);
        g_object_set_data_full(G_OBJECT(label), "media-id", g_strdup(id), g_free);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END); gtk_widget_set_tooltip_text(label, gtk_label_get_text(GTK_LABEL(label)));
        pack(row, label, TRUE); pack(routing->rows, row, FALSE);
    }
    routing->updating = FALSE;
    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(routing->rows_scroll)));
    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(routing->choices_scroll)));
    /* The surrounding editor is taller than its viewport. Give each inner
     * scroller a real minimum so GTK cannot collapse it to a clipped sliver. */
    if (routing->checks->len) {
        GtkWidget *first = g_ptr_array_index(routing->checks, 0); gint row_height = 0;
        gtk_widget_get_preferred_height(gtk_widget_get_parent(first), NULL, &row_height);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(routing->choices_scroll), MIN(120, MAX(40, row_height)));
    }
    if (routing->selected && json_array_get_length(routing->selected)) {
        gint rows_height = 0; gtk_widget_get_preferred_height(routing->rows, NULL, &rows_height);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(routing->rows_scroll), MIN(90, rows_height));
    }
    gtk_widget_set_visible(routing->choices_scroll, routing->checks->len > 0);
    gtk_widget_set_visible(routing->rows_scroll, routing->selected && json_array_get_length(routing->selected) > 0);
    enable(routing);
}
static void list_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer generation)
{
    (void)ctx; Routing *routing = state(owner);
    if (GPOINTER_TO_UINT(generation) != routing->generation) return;
    routing->loading = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { message(routing, error ? error->message : "Playlist listesi alınamadı."); enable(routing); return; }
    JsonNode *lists = json_object_get_member(json_node_get_object(result), "playlists");
    if (!lists || !JSON_NODE_HOLDS_ARRAY(lists)) { message(routing, "Playlist listesi alınamadı."); enable(routing); return; }
    JsonNode *snapshot = copy(lists);
    g_clear_pointer(&routing->playlists, json_array_unref);
    routing->playlists = json_array_ref(json_node_get_array(snapshot)); json_node_free(snapshot);
    GHashTableIter iter; gpointer id; g_hash_table_iter_init(&iter, routing->targets);
    while (g_hash_table_iter_next(&iter, &id, NULL)) if (!playlist(routing, id)) g_hash_table_iter_remove(&iter);
    rows_rebuild(routing);
    if (!routing->retry && !routing->outcome) message(routing, json_array_get_length(routing->playlists) ? "" : "Henüz playlist yok.");
}
static void routing_refresh(RadioSViewContext *ctx, GtkWidget *owner)
{
    Routing *routing = state(owner); if (!routing || routing->busy || routing->loading || routing->retry || !radios_data_connected(ctx)) return;
    JsonNode *body = radios_data_body(ctx); routing->loading = TRUE; ++routing->generation; enable(routing);
    if (!radios_data_request(ctx, owner, "POST", "/v1/workflows/desktop-playlists/list", body, list_done, GUINT_TO_POINTER(routing->generation), NULL)) { routing->loading = FALSE; enable(routing); }
    json_node_free(body);
}
static void saved(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer unused)
{
    (void)ctx; (void)unused; Routing *routing = state(owner);
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        routing->busy = FALSE; routing->outcome = TRUE;
        /* An uncertain reply keeps exactly the same receipt/body for retry. */
        if (error && error->domain == RADIOS_DATA_CLIENT_ERROR &&
            (error->code == RADIOS_DATA_CLIENT_ERROR_CONFLICT || error->code == RADIOS_DATA_CLIENT_ERROR_VALIDATION ||
             error->code == RADIOS_DATA_CLIENT_ERROR_FORBIDDEN || error->code == RADIOS_DATA_CLIENT_ERROR_NOT_FOUND || error->code == RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED)) {
            g_clear_pointer(&routing->retry, json_node_free); g_ptr_array_set_size(routing->jobs, 0);
        }
        gchar *text = g_strdup_printf("%u atama tamamlandı. %s%s", routing->completed,
            error ? error->message : "Kayıt doğrulanamadı.", routing->retry ? " Aynı işlemi yeniden denemek için Seçili medyayı ata." : " Tekrar deneyebilirsin.");
        message(routing, text); g_free(text); enable(routing);
        if (!routing->retry) routing_refresh(routing->ctx, routing->panel);
        return;
    }
    JsonObject *row = json_node_get_object(result); const gchar *id = radios_json_string(row, "_id", "");
    JsonObject *current = playlist(routing, id);
    if (current) {
        GList *members = json_object_get_members(row);
        for (GList *item = members; item; item = item->next) json_object_set_member(current, item->data, copy(json_object_get_member(row, item->data)));
        g_list_free(members);
    }
    g_clear_pointer(&routing->retry, json_node_free); ++routing->completed; ++routing->index; route_next(routing);
}
static JsonObject *metadata(JsonObject *detail)
{
    JsonObject *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version");
    JsonObject *result = json_object_new();
    const gchar *keys[] = {"title", "artist", "album", "genre", "key", "comment"};
    const guint limits[] = {160, 120, 120, 120, 120, 2000};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) {
        const gchar *text = radios_json_string(media, keys[i], ""); gchar *bounded = g_utf8_substring(text, 0, MIN((glong)limits[i], g_utf8_strlen(text, -1)));
        json_object_set_string_member(result, keys[i], bounded); g_free(bounded);
    }
    json_object_set_string_member(result, "location", radios_json_string(version, "managed_path", ""));
    json_object_set_int_member(result, "duration_ms", radios_json_int(version, "duration_ms", 0));
    json_object_set_double_member(result, "bpm", 0);
    json_object_set_string_member(result, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_string_member(result, "version_id", radios_json_string(version, "_id", ""));
    json_object_set_string_member(result, "media_file_id", radios_json_string(version, "media_file_id", ""));
    json_object_set_string_member(result, "media_sha256", radios_json_string(version, "sha256", ""));
    return result;
}
static void route_next(Routing *routing)
{
    while (routing->index < routing->jobs->len) {
        JsonNode *job = g_ptr_array_index(routing->jobs, routing->index); JsonObject *object = json_node_get_object(job);
        const gchar *target = radios_json_string(object, "target", "");
        JsonObject *list = playlist(routing, target), *detail = json_object_get_object_member(object, "detail"), *media = json_object_get_object_member(detail, "media");
        if (!routing->retry && contains(list, radios_json_string(media, "_id", ""))) { ++routing->index; continue; }
        if (!routing->retry) {
            routing->retry = radios_data_body(routing->ctx); JsonObject *request = json_node_get_object(routing->retry);
            json_object_set_string_member(request, "playlist_id", target);
            json_object_set_int_member(request, "expected_revision", radios_json_int(list, "_rev", 0));
            gchar *key = g_uuid_string_random(); json_object_set_string_member(request, "idempotency_key", key); g_free(key);
            json_object_set_object_member(request, "metadata", metadata(detail));
        }
        if (!radios_data_request(routing->ctx, routing->panel, "POST", "/v1/workflows/desktop-playlists/entry-save", routing->retry, saved, NULL, NULL)) {
            routing->busy = FALSE; message(routing, "Bağlantı yok; seçim korundu. Seçili medyayı ata ile yeniden dene."); enable(routing);
        }
        return;
    }
    routing->busy = FALSE; routing->outcome = TRUE; g_ptr_array_set_size(routing->jobs, 0); rows_rebuild(routing);
    gchar *text = g_strdup_printf("%u atama tamamlandı.", routing->completed); message(routing, text); g_free(text);
    radios_data_refresh(routing->ctx);
}
static void apply_clicked(GtkButton *button, gpointer value)
{
    (void)button; Routing *routing = value; if (routing->busy || routing->loading) return;
    if (!routing->retry) {
        g_ptr_array_set_size(routing->jobs, 0); routing->index = routing->completed = 0;
        for (guint p = 0; routing->playlists && p < json_array_get_length(routing->playlists); ++p) {
            const gchar *target = radios_json_string(json_array_get_object_element(routing->playlists, p), "_id", "");
            if (!g_hash_table_contains(routing->targets, target)) continue;
            for (guint i = 0; routing->selected && i < json_array_get_length(routing->selected); ++i) {
                JsonObject *detail = json_array_get_object_element(routing->selected, i);
                JsonObject *object = json_object_new(); JsonNode *job = json_node_new(JSON_NODE_OBJECT); json_node_take_object(job, object);
                json_object_set_string_member(object, "target", target);
                /* The selected snapshot is immutable for the entire batch.
                 * Share it between targets instead of copying its waveform. */
                json_object_set_object_member(object, "detail", json_object_ref(detail)); g_ptr_array_add(routing->jobs, job);
            }
        }
        if (!routing->jobs->len) { message(routing, "Parçaları ve en az bir hedef playlist seç."); return; }
    }
    routing->busy = TRUE; routing->outcome = FALSE; enable(routing); message(routing, "Seçilen parçalar hedef playlistlere ekleniyor…"); route_next(routing);
}
static void free_routing(gpointer value)
{
    Routing *routing = value; g_clear_pointer(&routing->selected, json_array_unref); g_clear_pointer(&routing->playlists, json_array_unref);
    g_clear_pointer(&routing->retry, json_node_free); g_ptr_array_unref(routing->checks); g_ptr_array_unref(routing->jobs); g_hash_table_unref(routing->targets); g_free(routing);
}
GtkWidget *radios_media_routing_new(RadioSViewContext *ctx)
{
    Routing *routing = g_new0(Routing, 1); routing->ctx = ctx;
    routing->checks = g_ptr_array_new(); routing->jobs = g_ptr_array_new_with_free_func((GDestroyNotify)json_node_free);
    routing->targets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    routing->panel = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "media-playlist-routing"); gtk_widget_set_name(routing->panel, "media-playlist-routing");
    GtkWidget *toolbar = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "pw-actions");
    routing->summary = radios_ui_label("", "pw-info"); gtk_widget_set_name(routing->summary, "media-playlist-route-summary");
    routing->apply = radios_ui_button("Seçili medyayı ata"); gtk_widget_set_name(routing->apply, "media-playlist-route-apply");
    pack(toolbar, radios_ui_label("Seçili medya ata", "heading"), FALSE); pack(toolbar, routing->summary, TRUE); pack(toolbar, routing->apply, FALSE); pack(routing->panel, toolbar, FALSE);
    routing->choices = gtk_flow_box_new(); gtk_widget_set_name(routing->choices, "media-playlist-choices");
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(routing->choices), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(routing->choices), 1); gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(routing->choices), 4);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(routing->choices), 4); gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(routing->choices), 12);
    routing->choices_scroll = gtk_scrolled_window_new(NULL, NULL); gtk_widget_set_name(routing->choices_scroll, "media-playlist-choices-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(routing->choices_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(routing->choices_scroll), TRUE); gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(routing->choices_scroll), 120);
    gtk_widget_set_no_show_all(routing->choices_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(routing->choices_scroll), routing->choices); pack(routing->panel, routing->choices_scroll, FALSE);
    routing->rows = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "media-playlist-route-rows");
    routing->rows_scroll = gtk_scrolled_window_new(NULL, NULL); gtk_widget_set_name(routing->rows_scroll, "media-playlist-membership-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(routing->rows_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(routing->rows_scroll), TRUE); gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(routing->rows_scroll), 90);
    gtk_widget_set_no_show_all(routing->rows_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(routing->rows_scroll), routing->rows); pack(routing->panel, routing->rows_scroll, FALSE);
    routing->status = radios_ui_label("", "application-status"); gtk_widget_set_name(routing->status, "media-playlist-route-status"); pack(routing->panel, routing->status, FALSE);
    gtk_widget_set_no_show_all(routing->status, TRUE);
    g_object_set_data_full(G_OBJECT(routing->panel), "radios-media-routing", routing, free_routing);
    g_signal_connect(routing->apply, "clicked", G_CALLBACK(apply_clicked), routing);
    radios_data_register_refresh(ctx, routing->panel, routing_refresh); enable(routing);
    if (radios_data_connected(ctx)) routing_refresh(ctx, routing->panel);
    return routing->panel;
}
void radios_media_routing_select(GtkWidget *panel, JsonArray *records)
{
    Routing *routing = state(panel); if (!routing || routing->busy || routing->retry) return;
    gboolean same = routing->selected && json_array_get_length(routing->selected) == json_array_get_length(records);
    for (guint i = 0; same && i < json_array_get_length(records); ++i) {
        JsonObject *a = json_object_get_object_member(json_array_get_object_element(routing->selected, i), "media");
        JsonObject *b = json_object_get_object_member(json_array_get_object_element(records, i), "media");
        same = !g_strcmp0(radios_json_string(a, "_id", ""), radios_json_string(b, "_id", ""));
    }
    if (!same) { routing->outcome = FALSE; message(routing, ""); }
    g_clear_pointer(&routing->selected, json_array_unref);
    JsonNode *node = json_node_new(JSON_NODE_ARRAY); json_node_set_array(node, records); JsonNode *snapshot = copy(node); json_node_free(node);
    routing->selected = json_array_ref(json_node_get_array(snapshot)); json_node_free(snapshot); rows_rebuild(routing);
    if (!same) routing_refresh(routing->ctx, routing->panel);
}
gboolean radios_media_routing_busy(GtkWidget *panel)
{ Routing *routing = panel ? state(panel) : NULL; return routing && (routing->busy || routing->retry != NULL); }
