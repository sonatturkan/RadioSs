#include "radios/local_event_ui.h"
#include "radios/data.h"
#include "radios/programme_model.h"
#include "radios/player_workspace.h"
#include <string.h>

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *dialog, *saved, *name, *kind, *source_type, *source, *list, *status;
    GtkWidget *fresh, *save, *insert, *add, *up, *down, *remove, *record_owner;
    GtkWidget *play, *playback_controls, *player, *after;
    RadioSDocument *document;
    RadioSDataClient *client;
    RadioSLocalEventInsert callback, play_callback;
    gpointer user, play_user;
    JsonNode *config, *catalogues[4];
    gchar *id;
    guint generation, pending;
    gboolean updating, dirty, busy, disposed;
} LocalEvent;
typedef struct { guint generation, catalogue; } LocalRead;
static const gchar *const source_types[] = {"playlist", "automation_calendar", "weekly_programme"};
static const gchar *const source_labels[] = {"Playlist", "Otomasyon şeması", "Haftalık program"};
static void refresh(LocalEvent *view);
static void controls(LocalEvent *view);
static void render_sources(LocalEvent *view, gint selected);
static void playback_choices(LocalEvent *view);
static void playback_changed(GtkComboBox *combo, gpointer user);
static JsonObject *object(JsonObject *parent, const gchar *key)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array(JsonObject *parent, const gchar *key)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static const gchar *text(JsonObject *parent, const gchar *key)
{ return radios_json_string(parent, key, ""); }
static JsonObject *configuration(LocalEvent *view)
{ return view->config ? json_node_get_object(view->config) : NULL; }
static GtkWidget *named(GtkWidget *widget, const gchar *name)
{ gtk_widget_set_name(widget, name); return widget; }
static void pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static void status(LocalEvent *view, const gchar *message)
{ gtk_label_set_text(GTK_LABEL(view->status), message ? message : ""); }
static gboolean identity(LocalEvent *view)
{
    RadioSDataClient *current = radios_data_client_acquire(view->ctx);
    gboolean same = current && view->client && radios_data_client_same_identity(current, view->client);
    if (current) radios_data_client_unref(current);
    return same;
}
static JsonArray *records(LocalEvent *view, guint index)
{
    return view->catalogues[index] ? array(json_node_get_object(view->catalogues[index]), index == 0 ? "playlists" : "records") : NULL;
}
static gboolean active_record(LocalEvent *view, JsonObject *record)
{
    const gchar *record_status = radios_json_string(record, "status", "active");
    return g_uuid_string_is_valid(text(record, "_id")) && view->client &&
        !g_strcmp0(text(record, "station_id"), radios_data_client_station_id(view->client)) &&
        g_str_equal(record_status, "active");
}
static const gchar *record_title(JsonObject *record, gboolean local)
{
    const gchar *title = local ? text(object(record, "config"), "title") : "";
    return *title ? title : text(record, "name");
}
static JsonObject *record_find(LocalEvent *view, guint index, const gchar *id)
{
    JsonArray *items = records(view, index);
    for (guint i = 0; items && i < json_array_get_length(items); ++i) {
        JsonObject *item = json_array_get_object_element(items, i);
        if (!g_strcmp0(text(item, "_id"), id)) return item;
    }
    return NULL;
}
static guint source_index(const gchar *type)
{
    for (guint i = 0; i < G_N_ELEMENTS(source_types); ++i)
        if (!g_strcmp0(type, source_types[i])) return i;
    return G_N_ELEMENTS(source_types);
}
static void source_choices(LocalEvent *view)
{
    const gchar *type = gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->source_type));
    guint index = source_index(type);
    gchar *selected = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->source)));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(view->source));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->source), "", "Kaynak seçin");
    JsonArray *items = index < 3 ? records(view, index) : NULL;
    for (guint i = 0; items && i < json_array_get_length(items); ++i) {
        JsonObject *item = json_array_get_object_element(items, i);
        if (active_record(view, item))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->source), text(item, "_id"), record_title(item, FALSE));
    }
    if (!selected || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->source), selected))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->source), "");
    g_free(selected);
}
static void saved_choices(LocalEvent *view)
{
    gboolean was_updating = view->updating;
    view->updating = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(view->saved));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->saved), "", "Kaydedilmiş yerel event seçin");
    JsonArray *items = records(view, 3);
    for (guint i = 0; items && i < json_array_get_length(items); ++i) {
        JsonObject *item = json_array_get_object_element(items, i);
        if (active_record(view, item))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->saved), text(item, "_id"), record_title(item, TRUE));
    }
    if (!view->id || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->saved), view->id))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->saved), "");
    view->updating = was_updating;
}
static void read_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer user)
{
    (void)ctx;
    LocalRead *read = user;
    LocalEvent *view = g_object_get_data(G_OBJECT(owner), "radios-local-event");
    if (!view || view->disposed || read->generation != view->generation) return;
    if (view->pending) --view->pending;
    if (!identity(view)) status(view, "Veri oturumu değişti. Event penceresini yeniden açın.");
    else if (error || !result || !JSON_NODE_HOLDS_OBJECT(result))
        status(view, error ? error->message : "Event kaynakları okunamadı.");
    else {
        g_clear_pointer(&view->catalogues[read->catalogue], json_node_free);
        view->catalogues[read->catalogue] = json_node_copy(result);
        if (read->catalogue == 3) saved_choices(view);
        else { source_choices(view); render_sources(view, -1); }
        if (!view->pending && !view->busy && !view->dirty) status(view, "Yerel event hazır.");
    }
    controls(view);
}
static void refresh(LocalEvent *view)
{
    if (view->disposed || !radios_data_connected(view->ctx)) { status(view, "Veritabanı bağlantısı bekleniyor."); return; }
    if (!view->client) view->client = radios_data_client_acquire(view->ctx);
    if (!identity(view)) { status(view, "Veri oturumu değişti. Event penceresini yeniden açın."); controls(view); return; }
    playback_choices(view);
    ++view->generation;
    view->pending = 0;
    for (guint i = 0; i < 4; ++i) {
        JsonNode *body = radios_data_body(view->ctx);
        if (i) json_object_set_string_member(json_node_get_object(body), "kind", i == 3 ? "local_event" : source_types[i]);
        LocalRead *read = g_new0(LocalRead, 1); read->generation = view->generation; read->catalogue = i;
        if (radios_data_request(view->ctx, view->dialog, "POST", i == 0 ? "/v1/workflows/desktop-playlists/list" : "/v1/workflows/desktop/list", body, read_done, read, g_free)) ++view->pending;
        json_node_free(body);
    }
    controls(view);
}
static void selected(GtkListBox *list, GtkListBoxRow *row, gpointer user)
{ (void)list; (void)row; controls(user); }
static void controls(LocalEvent *view)
{
    gboolean ready = !view->disposed && identity(view) && (!view->document || radios_data_document_ready(view->document));
    gboolean edit = ready && !view->busy;
    gchar *title = g_strdup(gtk_entry_get_text(GTK_ENTRY(view->name))); g_strstrip(title);
    JsonArray *sources = array(configuration(view), "sources");
    guint count = sources ? json_array_get_length(sources) : 0;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(view->list));
    gint index = row ? gtk_list_box_row_get_index(row) : -1;
    gtk_widget_set_sensitive(view->name, edit); gtk_widget_set_sensitive(view->kind, edit);
    gtk_widget_set_sensitive(view->source_type, edit); gtk_widget_set_sensitive(view->source, edit);
    gtk_widget_set_sensitive(view->save, edit && *title && view->dirty);
    gtk_widget_set_sensitive(view->insert, edit && *title && count && view->callback);
    gtk_widget_set_sensitive(view->play, edit && *title && count && view->play_callback);
    gtk_widget_set_sensitive(view->playback_controls, edit && view->play_callback);
    gtk_widget_set_sensitive(view->fresh, edit && !view->dirty);
    gtk_widget_set_sensitive(view->saved, edit && !view->dirty);
    const gchar *source = gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->source));
    gtk_widget_set_sensitive(view->add, edit && source && *source && count < 100);
    gtk_widget_set_sensitive(view->up, edit && index > 0);
    gtk_widget_set_sensitive(view->down, edit && index >= 0 && (guint)index + 1 < count);
    gtk_widget_set_sensitive(view->remove, edit && index >= 0);
    g_free(title);
}
static void changed(LocalEvent *view)
{
    if (view->updating) return;
    view->dirty = TRUE;
    if (view->document) radios_data_document_dirty(view->document, TRUE);
    status(view, "Kaydedilmemiş değişiklikler var.");
    controls(view);
}
static void fields_changed(GtkWidget *widget, gpointer user)
{
    (void)widget; LocalEvent *view = user;
    if (view->updating) return;
    json_object_set_string_member(configuration(view), "title", gtk_entry_get_text(GTK_ENTRY(view->name)));
    const gchar *kind = gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->kind));
    json_object_set_string_member(configuration(view), "kind", kind ? kind : "music");
    changed(view);
}
static void choices_changed(GtkWidget *widget, gpointer user)
{
    LocalEvent *view = user;
    if (widget == view->source_type) source_choices(view);
    controls(view);
}
static void render_sources(LocalEvent *view, gint selected_index)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(view->list));
    for (GList *item = children; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(children);
    JsonArray *sources = array(configuration(view), "sources");
    for (guint i = 0; sources && i < json_array_get_length(sources); ++i) {
        JsonObject *source = json_array_get_object_element(sources, i);
        guint index = source_index(text(source, "type"));
        JsonObject *record = index < 3 ? record_find(view, index, text(source, "source_id")) : NULL;
        const gchar *title = record ? record_title(record, FALSE) : text(source, "source_id");
        gchar *label = g_strdup_printf("%u. %s · %s%s", i + 1, index < 3 ? source_labels[index] : "Kaynak", title,
            record && !active_record(view, record) ? " (devre dışı)" : "");
        GtkWidget *row = gtk_list_box_row_new(), *name = gtk_label_new(label);
        gtk_label_set_xalign(GTK_LABEL(name), 0); gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_widget_set_margin_start(name, 8); gtk_widget_set_margin_end(name, 8);
        gtk_widget_set_margin_top(name, 8); gtk_widget_set_margin_bottom(name, 8);
        gtk_widget_set_tooltip_text(name, label);
        gtk_container_add(GTK_CONTAINER(row), name); gtk_container_add(GTK_CONTAINER(view->list), row);
        if ((gint)i == selected_index) gtk_list_box_select_row(GTK_LIST_BOX(view->list), GTK_LIST_BOX_ROW(row));
        g_free(label);
    }
    gtk_widget_show_all(view->list);
    controls(view);
}
static void add_source(GtkButton *button, gpointer user)
{
    (void)button; LocalEvent *view = user;
    const gchar *type = gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->source_type));
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->source));
    guint index = source_index(type);
    JsonArray *sources = array(configuration(view), "sources");
    if (!identity(view) || !g_uuid_string_is_valid(id) || index >= 3 || !active_record(view, record_find(view, index, id)) || json_array_get_length(sources) >= 100) return;
    JsonObject *source = json_object_new(); gchar *occurrence = g_uuid_string_random();
    json_object_set_string_member(source, "id", occurrence); g_free(occurrence);
    json_object_set_string_member(source, "type", type); json_object_set_string_member(source, "source_id", id);
    json_array_add_object_element(sources, source); changed(view);
    render_sources(view, (gint)json_array_get_length(sources) - 1);
}
static void move_source(GtkButton *button, gpointer user)
{
    LocalEvent *view = user;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(view->list));
    if (!row || !identity(view) || view->busy) return;
    gint selected_index = gtk_list_box_row_get_index(row);
    JsonArray *sources = array(configuration(view), "sources");
    gint count = (gint)json_array_get_length(sources), direction = GTK_WIDGET(button) == view->up ? -1 : 1;
    gboolean remove = GTK_WIDGET(button) == view->remove;
    gint target = selected_index + direction;
    if (!remove && (target < 0 || target >= count)) return;
    JsonArray *next = json_array_new();
    for (gint i = 0; i < count; ++i) {
        if (remove && i == selected_index) continue;
        gint origin = !remove && i == target ? selected_index : !remove && i == selected_index ? target : i;
        json_array_add_element(next, json_node_copy(json_array_get_element(sources, (guint)origin)));
    }
    json_object_set_array_member(configuration(view), "sources", next); changed(view);
    render_sources(view, remove ? MIN(selected_index, count - 2) : target);
}
static void apply_config(LocalEvent *view, JsonObject *config)
{
    JsonNode *copy = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(copy, radios_programme_clone_object(config));
    g_clear_pointer(&view->config, json_node_free); view->config = copy;
    view->updating = TRUE;
    gtk_entry_set_text(GTK_ENTRY(view->name), text(config, "title"));
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->kind), text(config, "kind")))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->kind), "other");
    playback_choices(view);
    JsonObject *playback = object(config, "playback");
    gint64 player = radios_json_int(playback, "player", 0);
    gchar *player_id = player ? g_strdup_printf("%" G_GINT64_FORMAT, player) : g_strdup("auto");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->player), player_id); g_free(player_id);
    const gchar *after = text(playback, "next_cluster_id");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->after), *after ? after : "stop");
    view->updating = FALSE; view->dirty = FALSE;
    render_sources(view, -1); controls(view);
}
static void document_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer user)
{
    (void)ctx; LocalEvent *view = user;
    if (!view->disposed && owner == view->record_owner && config) {
        apply_config(view, config);
        status(view, "Yerel event hazır.");
    }
}
static void document_status(RadioSDocument *document, const gchar *message, gpointer user)
{
    LocalEvent *view = user;
    if (view->disposed || view->document != document) return;
    view->busy = g_str_has_prefix(message ? message : "", "Kaydediliyor");
    if (radios_data_document_revision(document) > 0) { view->dirty = FALSE; refresh(view); }
    status(view, message); controls(view);
}
static void reset_record(LocalEvent *view)
{
    if (view->record_owner) gtk_widget_destroy(view->record_owner);
    view->record_owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_no_show_all(view->record_owner, TRUE);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(view->dialog))), view->record_owner, FALSE, FALSE, 0);
    view->document = NULL; g_clear_pointer(&view->id, g_free);
}
static void fresh_clicked(GtkButton *button, gpointer user)
{
    (void)button; LocalEvent *view = user;
    if (view->dirty || view->busy) return;
    reset_record(view);
    JsonObject *config = json_object_new();
    json_object_set_int_member(config, "schema", 1); json_object_set_string_member(config, "title", "");
    json_object_set_string_member(config, "kind", "music"); json_object_set_array_member(config, "sources", json_array_new());
    apply_config(view, config); json_object_unref(config); saved_choices(view);
    status(view, "Event adını yazın; playlist veya şema ekleyin.");
    gtk_widget_grab_focus(view->name);
}
static void saved_changed(GtkComboBox *combo, gpointer user)
{
    LocalEvent *view = user;
    if (view->updating || view->dirty || view->busy || !identity(view)) return;
    gchar *id = g_strdup(gtk_combo_box_get_active_id(combo));
    if (!g_uuid_string_is_valid(id) || !g_strcmp0(id, view->id)) { g_free(id); return; }
    JsonObject *record = record_find(view, 3, id);
    if (!record) { g_free(id); return; }
    reset_record(view); view->id = id;
    view->document = radios_data_document_new_for_id(view->ctx, view->record_owner, "local_event", text(record, "name"), id, document_apply, view);
    radios_data_document_set_status_callback(view->document, document_status, view);
    status(view, "Event yükleniyor…"); controls(view);
}
static JsonNode *snapshot(LocalEvent *view)
{
    JsonNode *node = radios_programme_clone(view->config);
    gchar *title = g_strdup(gtk_entry_get_text(GTK_ENTRY(view->name))); g_strstrip(title);
    JsonObject *config = json_node_get_object(node);
    json_object_set_string_member(config, "title", title); g_free(title);
    if (view->play_callback || json_object_has_member(config, "playback")) {
        guint deck = G_MAXUINT; gchar cluster[37];
        if (radios_local_event_ui_playback(view->dialog, &deck, cluster)) {
            JsonObject *playback = json_object_new();
            json_object_set_int_member(playback, "player", deck == G_MAXUINT ? 0 : (gint64)deck + 1);
            json_object_set_string_member(playback, "next_cluster_id", cluster);
            json_object_set_object_member(config, "playback", playback);
        }
    }
    return node;
}
static void save_clicked(GtkButton *button, gpointer user)
{
    (void)button; LocalEvent *view = user;
    if (view->busy || !identity(view)) return;
    JsonNode *config = snapshot(view); const gchar *title = text(json_node_get_object(config), "title");
    if (!*title) { status(view, "Event adını yazın."); json_node_free(config); return; }
    if (!view->document) {
        g_free(view->id); view->id = g_uuid_string_random();
        view->document = radios_data_document_new_record(view->ctx, view->record_owner, "local_event", title, view->id, document_apply, view);
        radios_data_document_set_status_callback(view->document, document_status, view);
    }
    view->busy = TRUE;
    if (!radios_data_document_save(view->document, config)) { view->busy = FALSE; status(view, "Event kaydedilemedi; değişiklikler pencerede korunuyor."); }
    json_node_free(config); controls(view);
}
static void insert_clicked(GtkButton *button, gpointer user)
{
    LocalEvent *view = user;
    gboolean playing = GTK_WIDGET(button) == view->play;
    RadioSLocalEventInsert callback = playing ? view->play_callback : view->callback;
    gpointer callback_user = playing ? view->play_user : view->user;
    if (!callback || view->busy || !identity(view)) return;
    JsonNode *config = snapshot(view); JsonArray *sources = array(json_node_get_object(config), "sources");
    if (!*text(json_node_get_object(config), "title") || !sources || !json_array_get_length(sources)) { json_node_free(config); return; }
    GError *error = NULL;
    if (callback(view->ctx, json_node_get_object(config), callback_user, &error))
        status(view, playing ? "Event kümesiz oynatıcıya Yarı AutoDJ ile gönderildi." : "Event kaynakları hazırlanıyor…");
    else status(view, error ? error->message : "Event eklenemedi.");
    g_clear_error(&error); json_node_free(config);
}
static void playback_changed(GtkComboBox *combo, gpointer user)
{
    (void)combo; LocalEvent *view = user;
    if (!view->updating) changed(view);
}
static void playback_choices(LocalEvent *view)
{
    gboolean was_updating = view->updating; view->updating = TRUE;
    gchar *player = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->player)));
    gchar *after = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->after)));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(view->player));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->player), "auto", "Otomatik · uygun kümesiz oynatıcı");
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(view->after));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->after), "stop", "Dur");
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(view->ctx->players);
    for (guint deck = 0; topology && deck < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++deck) {
        const RadioSPlayerDefinition *definition = radios_player_topology_player_const(topology, deck);
        if (!definition || *definition->cluster_id) continue;
        gchar *id = g_strdup_printf("%u", deck + 1);
        gchar *name = *definition->name ? g_strdup_printf("%u · %s", deck + 1, definition->name) : g_strdup_printf("Oynatıcı %u", deck + 1);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->player), id, name); g_free(id); g_free(name);
    }
    for (guint i = 0; topology && i < topology->cluster_count; ++i)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->after), topology->clusters[i].id, topology->clusters[i].name);
    /* Keep saved selections inspectable when their topology was removed.
     * Starting still validates current independent membership and ownership. */
    JsonObject *playback = object(configuration(view), "playback");
    gint64 saved_player = radios_json_int(playback, "player", 0);
    const RadioSPlayerDefinition *saved_definition = saved_player >= 1 && saved_player <= 32 && topology ?
        radios_player_topology_player_const(topology, (guint)saved_player - 1) : NULL;
    if (saved_player >= 1 && saved_player <= 32 && (!saved_definition || *saved_definition->cluster_id)) {
        gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, saved_player);
        gchar *name = g_strdup_printf("Oynatıcı %" G_GINT64_FORMAT " · kullanılamıyor", saved_player);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->player), id, name); g_free(id); g_free(name);
    }
    const gchar *saved_after = text(playback, "next_cluster_id");
    gboolean cluster_exists = FALSE;
    for (guint i = 0; topology && i < topology->cluster_count; ++i)
        cluster_exists |= !g_strcmp0(saved_after, topology->clusters[i].id);
    if (*saved_after && !cluster_exists) {
        gchar *name = g_strdup_printf("Kullanılamayan küme · %s", saved_after);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->after), saved_after, name); g_free(name);
    }
    if (!player || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->player), player))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->player), "auto");
    if (!after || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->after), after))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->after), "stop");
    g_free(player); g_free(after); view->updating = was_updating;
}
static void refresh_clicked(GtkButton *button, gpointer user)
{ (void)button; refresh(user); }
static void response(GtkDialog *dialog, gint response_id, gpointer user)
{ (void)response_id; (void)user; gtk_widget_destroy(GTK_WIDGET(dialog)); }
static void disposed(GtkWidget *widget, gpointer user)
{
    (void)widget; LocalEvent *view = user; view->disposed = TRUE;
    if (view->document) radios_data_document_preserve_pending(view->document);
}
static void free_view(gpointer user)
{
    LocalEvent *view = user;
    g_clear_pointer(&view->config, json_node_free);
    for (guint i = 0; i < 4; ++i) g_clear_pointer(&view->catalogues[i], json_node_free);
    g_clear_pointer(&view->client, radios_data_client_unref); g_free(view->id); g_free(view);
}
static GtkWidget *button(GtkWidget *box, const gchar *label, const gchar *name, GCallback callback, LocalEvent *view)
{
    GtkWidget *widget = named(gtk_button_new_with_label(label), name);
    pack(box, widget, FALSE); g_signal_connect(widget, "clicked", callback, view); return widget;
}
GtkWidget *radios_local_event_ui_open(RadioSViewContext *ctx, GtkWindow *parent, RadioSLocalEventInsert insert, gpointer user)
{
    g_return_val_if_fail(ctx != NULL, NULL);
    LocalEvent *view = g_new0(LocalEvent, 1); view->ctx = ctx; view->callback = insert; view->user = user;
    view->client = radios_data_client_acquire(ctx);
    view->dialog = named(gtk_dialog_new_with_buttons("Yerel Event oluşturucu", parent,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Kapat", GTK_RESPONSE_CLOSE, NULL), "local-event-dialog");
    gtk_window_set_default_size(GTK_WINDOW(view->dialog), 780, 560);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(view->dialog));
    gtk_box_set_spacing(GTK_BOX(content), 10); gtk_container_set_border_width(GTK_CONTAINER(content), 14);
    GtkWidget *saved_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); pack(content, saved_row, FALSE);
    view->saved = named(gtk_combo_box_text_new(), "local-event-saved"); pack(saved_row, view->saved, TRUE);
    view->fresh = button(saved_row, "Yeni event", "local-event-new", G_CALLBACK(fresh_clicked), view);
    button(saved_row, "Yenile", "local-event-refresh", G_CALLBACK(refresh_clicked), view);
    GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); pack(content, name_row, FALSE);
    view->name = named(gtk_entry_new(), "local-event-name"); gtk_entry_set_placeholder_text(GTK_ENTRY(view->name), "Event adı");
    gtk_entry_set_max_length(GTK_ENTRY(view->name), 160); pack(name_row, view->name, TRUE);
    view->kind = named(gtk_combo_box_text_new(), "local-event-kind"); pack(name_row, view->kind, FALSE);
    const gchar *kinds[] = {"music", "jingle", "sweeper", "advertisement", "program", "announcement", "news", "effect", "voice_track", "other"};
    const gchar *labels[] = {"Müzik", "Jingle", "Sweeper", "Reklam", "Karma program", "Anons", "Haber", "Efekt", "Ses kaydı", "Diğer"};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->kind), kinds[i], labels[i]);
    GtkWidget *source_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); pack(content, source_row, FALSE);
    view->source_type = named(gtk_combo_box_text_new(), "local-event-source-type"); pack(source_row, view->source_type, FALSE);
    for (guint i = 0; i < 3; ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->source_type), source_types[i], source_labels[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(view->source_type), 0);
    view->source = named(gtk_combo_box_text_new(), "local-event-source"); pack(source_row, view->source, TRUE);
    view->add = button(source_row, "Evente ekle", "local-event-add", G_CALLBACK(add_source), view);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC); pack(content, scroll, TRUE);
    view->list = named(gtk_list_box_new(), "local-event-sources"); gtk_container_add(GTK_CONTAINER(scroll), view->list);
    GtkWidget *edit_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); pack(content, edit_row, FALSE);
    view->up = button(edit_row, "Yukarı", "local-event-up", G_CALLBACK(move_source), view);
    view->down = button(edit_row, "Aşağı", "local-event-down", G_CALLBACK(move_source), view);
    view->remove = button(edit_row, "Çıkar", "local-event-remove", G_CALLBACK(move_source), view);
    view->playback_controls = named(gtk_grid_new(), "local-event-playback-controls");
    gtk_grid_set_column_spacing(GTK_GRID(view->playback_controls), 8);
    gtk_grid_set_row_spacing(GTK_GRID(view->playback_controls), 8);
    GtkWidget *player_label = gtk_label_new("Kümesiz oynatıcı"), *after_label = gtk_label_new("Event sonrası");
    gtk_label_set_xalign(GTK_LABEL(player_label), 0); gtk_label_set_xalign(GTK_LABEL(after_label), 0);
    view->player = named(gtk_combo_box_text_new(), "local-event-player");
    view->after = named(gtk_combo_box_text_new(), "local-event-after");
    gtk_widget_set_hexpand(view->player, TRUE); gtk_widget_set_hexpand(view->after, TRUE);
    gtk_widget_set_tooltip_text(view->player, "Event bu kümesiz oynatıcıda Yarı AutoDJ ile çalar.");
    gtk_widget_set_tooltip_text(view->after, "Event tamamlandığında dur veya seçili kümenin yayın sırasından devam et.");
    gtk_grid_attach(GTK_GRID(view->playback_controls), player_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(view->playback_controls), view->player, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(view->playback_controls), after_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(view->playback_controls), view->after, 1, 1, 1, 1);
    pack(content, view->playback_controls, FALSE);
    gtk_widget_show_all(view->playback_controls); gtk_widget_hide(view->playback_controls);
    gtk_widget_set_no_show_all(view->playback_controls, TRUE);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); pack(content, actions, FALSE);
    view->save = button(actions, "Eventi kaydet", "local-event-save", G_CALLBACK(save_clicked), view);
    view->insert = button(actions, "Seçili sıraya ekle", "local-event-insert", G_CALLBACK(insert_clicked), view);
    view->play = button(actions, "Kümesizde çal", "local-event-play", G_CALLBACK(insert_clicked), view);
    gtk_widget_set_no_show_all(view->play, TRUE);
    view->status = named(gtk_label_new(""), "local-event-status"); gtk_label_set_xalign(GTK_LABEL(view->status), 0);
    gtk_label_set_line_wrap(GTK_LABEL(view->status), TRUE); pack(content, view->status, FALSE);
    g_object_set_data_full(G_OBJECT(view->dialog), "radios-local-event", view, free_view);
    g_signal_connect(view->dialog, "destroy", G_CALLBACK(disposed), view);
    g_signal_connect(view->dialog, "response", G_CALLBACK(response), view);
    g_signal_connect(view->saved, "changed", G_CALLBACK(saved_changed), view);
    g_signal_connect(view->name, "changed", G_CALLBACK(fields_changed), view);
    g_signal_connect(view->kind, "changed", G_CALLBACK(fields_changed), view);
    g_signal_connect(view->source_type, "changed", G_CALLBACK(choices_changed), view);
    g_signal_connect(view->source, "changed", G_CALLBACK(choices_changed), view);
    g_signal_connect(view->player, "changed", G_CALLBACK(playback_changed), view);
    g_signal_connect(view->after, "changed", G_CALLBACK(playback_changed), view);
    g_signal_connect(view->list, "row-selected", G_CALLBACK(selected), view);
    fresh_clicked(NULL, view); source_choices(view); playback_choices(view); refresh(view);
    gtk_widget_show_all(view->dialog); return view->dialog;
}
JsonNode *radios_local_event_ui_snapshot(GtkWidget *dialog)
{
    LocalEvent *view = dialog ? g_object_get_data(G_OBJECT(dialog), "radios-local-event") : NULL;
    return view && !view->disposed ? snapshot(view) : NULL;
}

void radios_local_event_ui_insert_result(GtkWidget *dialog, const gchar *message)
{
    LocalEvent *view = dialog ? g_object_get_data(G_OBJECT(dialog), "radios-local-event") : NULL;
    if (view && !view->disposed) status(view, message);
}

void radios_local_event_ui_set_play_handler(GtkWidget *dialog, RadioSLocalEventInsert play, gpointer user)
{
    LocalEvent *view = dialog ? g_object_get_data(G_OBJECT(dialog), "radios-local-event") : NULL;
    if (!view || view->disposed) return;
    view->play_callback = play; view->play_user = user; playback_choices(view);
    gtk_widget_set_visible(view->playback_controls, play != NULL);
    gtk_widget_set_visible(view->play, play != NULL); controls(view);
}
gboolean radios_local_event_ui_playback(GtkWidget *dialog, guint *deck, gchar cluster_id[37])
{
    if (deck) *deck = G_MAXUINT;
    if (cluster_id) cluster_id[0] = 0;
    LocalEvent *view = dialog ? g_object_get_data(G_OBJECT(dialog), "radios-local-event") : NULL;
    if (!view || view->disposed || !deck || !cluster_id) return FALSE;
    const gchar *player = gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->player));
    const gchar *after = gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->after));
    if (!player || !after) return FALSE;
    if (g_strcmp0(player, "auto")) {
        gchar *end = NULL; guint64 value = g_ascii_strtoull(player, &end, 10);
        if (!*player || !end || *end || value < 1 || value > RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS) return FALSE;
        *deck = (guint)value - 1;
    }
    if (g_strcmp0(after, "stop")) {
        if (!g_uuid_string_is_valid(after)) return FALSE;
        g_strlcpy(cluster_id, after, 37);
    }
    return TRUE;
}

gboolean radios_local_event_ui_set_event(GtkWidget *dialog, const gchar *title, const gchar *kind)
{
    LocalEvent *view = dialog ? g_object_get_data(G_OBJECT(dialog), "radios-local-event") : NULL;
    if (!view || view->disposed) return FALSE;
    if (view->busy || view->dirty || view->document) {
        status(view, "Açık Event taslağı korundu; farklı bir Event için Yeni event seçin."); return FALSE;
    }
    if (kind && radios_programme_kind_valid(kind))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->kind), kind);
    gtk_entry_set_text(GTK_ENTRY(view->name), title ? title : ""); return TRUE;
}
static gboolean choice_available(GtkComboBox *combo, const gchar *id)
{
    GtkTreeModel *model = gtk_combo_box_get_model(combo); GtkTreeIter iter;
    gint column = gtk_combo_box_get_id_column(combo);
    if (!model || column < 0 || !gtk_tree_model_get_iter_first(model, &iter)) return FALSE;
    do {
        gchar *value = NULL; gtk_tree_model_get(model, &iter, column, &value, -1);
        gboolean same = !g_strcmp0(value, id); g_free(value); if (same) return TRUE;
    } while (gtk_tree_model_iter_next(model, &iter));
    return FALSE;
}
gboolean radios_local_event_ui_set_playback(GtkWidget *dialog, guint deck, const gchar *cluster_id)
{
    LocalEvent *view = dialog ? g_object_get_data(G_OBJECT(dialog), "radios-local-event") : NULL;
    if (!view || view->disposed || view->busy || (deck != G_MAXUINT && deck >= RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS)) return FALSE;
    gchar *player = deck == G_MAXUINT ? g_strdup("auto") : g_strdup_printf("%u", deck + 1);
    const gchar *after = cluster_id && *cluster_id ? cluster_id : "stop";
    gboolean available = choice_available(GTK_COMBO_BOX(view->player), player) && choice_available(GTK_COMBO_BOX(view->after), after);
    if (available) {
        gboolean different = g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->player)), player) ||
            g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->after)), after);
        gboolean was_updating = view->updating; view->updating = TRUE;
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->player), player);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->after), after);
        view->updating = was_updating;
        if (different) changed(view);
    }
    g_free(player); return available;
}
gboolean radios_local_event_ui_set_configuration(GtkWidget *dialog, JsonObject *recipe)
{
    LocalEvent *view = dialog ? g_object_get_data(G_OBJECT(dialog), "radios-local-event") : NULL;
    if (!view || view->disposed || !recipe) return FALSE;
    if (view->busy || view->dirty || view->document) {
        status(view, "Açık Event taslağı korundu; farklı bir Event için Yeni event seçin."); return FALSE;
    }
    if (radios_json_int(recipe, "schema", 0) != 1 || !*text(recipe, "title") ||
        !radios_programme_kind_valid(text(recipe, "kind")) || !array(recipe, "sources")) return FALSE;
    apply_config(view, recipe);
    /* This is the selected programme event's persisted recipe, not a new or
     * changed local document. Editing it requires explicit Assign or Save. */
    status(view, "Seçili Event kaynakları yüklendi."); return TRUE;
}
void radios_local_event_ui_set_insert_label(GtkWidget *dialog, const gchar *label)
{
    LocalEvent *view = dialog ? g_object_get_data(G_OBJECT(dialog), "radios-local-event") : NULL;
    if (view && !view->disposed && label && *label) gtk_button_set_label(GTK_BUTTON(view->insert), label);
}
