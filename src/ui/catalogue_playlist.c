#include "radios/catalogue_playlist.h"
#include "radios/widgets.h"

static JsonObject *object_member(JsonObject *object, const gchar *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

JsonObject *radios_catalogue_playlist_track(JsonObject *record)
{
    JsonObject *media = object_member(record, "media"), *version = object_member(record, "version");
    if (!media || !version) return NULL;
    JsonObject *track = json_object_new();
    const gchar *fields[] = {"title", "artist", "album", "genre", "key", "comment"};
    const glong limits[] = {160, 120, 120, 120, 120, 2000};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) {
        const gchar *value = radios_json_string(media, fields[i], "");
        gchar *bounded = g_utf8_substring(value, 0, MIN(g_utf8_strlen(value, -1), limits[i]));
        if (i == 0) g_strstrip(bounded);
        json_object_set_string_member(track, fields[i], bounded); g_free(bounded);
    }
    if (!*radios_json_string(track, "title", "")) { json_object_unref(track); return NULL; }
    gint64 duration = radios_json_int(version, "duration_ms", 0);
    if (duration > 0 && duration <= 86400000) json_object_set_int_member(track, "duration_ms", duration);
    else json_object_set_null_member(track, "duration_ms");
    JsonObject *analysis = object_member(record, "analysis"), *rhythm = object_member(analysis, "rhythm");
    json_object_set_double_member(track, "bpm", rhythm && json_object_has_member(rhythm, "bpm") ? json_object_get_double_member(rhythm, "bpm") : 0);
    json_object_set_string_member(track, "location", "");
    json_object_set_string_member(track, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_string_member(track, "version_id", radios_json_string(version, "_id", ""));
    json_object_set_string_member(track, "media_file_id", radios_json_string(version, "media_file_id", ""));
    json_object_set_string_member(track, "media_sha256", radios_json_string(version, "sha256", ""));
    return track;
}

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *owner, *button, *dialog;
    RadioSCataloguePlaylistSelection selected;
    RadioSCataloguePlaylistCreated created;
    gpointer data;
    gboolean disposed;
} PlaylistButton;

typedef struct {
    PlaylistButton *button;
    GtkWidget *name, *status;
    JsonArray *entries;
    JsonNode *request;
    RadioSDataClient *identity;
    gboolean busy;
} PlaylistCreate;

static gboolean same_identity(PlaylistCreate *create)
{
    RadioSDataClient *current = radios_data_client_acquire(create->button->ctx);
    gboolean same = current && create->identity &&
        !g_strcmp0(radios_data_client_station_id(current), radios_data_client_station_id(create->identity)) &&
        !g_strcmp0(radios_data_client_user_id(current), radios_data_client_user_id(create->identity)) &&
        !g_strcmp0(radios_data_client_store_id(current), radios_data_client_store_id(create->identity));
    if (current) radios_data_client_unref(current);
    return same;
}

static void create_free(gpointer value)
{
    PlaylistCreate *create = value;
    json_array_unref(create->entries);
    g_clear_pointer(&create->request, json_node_free);
    if (create->identity) radios_data_client_unref(create->identity);
    g_free(create);
}

static void dialog_destroyed(GtkWidget *dialog, gpointer value)
{
    PlaylistCreate *create = value;
    if (create->button->dialog == dialog) create->button->dialog = NULL;
}

static gboolean entries_match(JsonArray *expected, JsonArray *actual)
{
    if (json_array_get_length(expected) != json_array_get_length(actual)) return FALSE;
    for (guint i = 0; i < json_array_get_length(expected); ++i) {
        JsonNode *node = json_array_get_element(actual, i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) return FALSE;
        JsonObject *row = json_node_get_object(node);
        if (radios_json_int(row, "ordinal", -1) != (gint64)i) return FALSE;
        JsonObjectIter iter; const gchar *key; JsonNode *value;
        json_object_iter_init(&iter, json_array_get_object_element(expected, i));
        while (json_object_iter_next(&iter, &key, &value)) {
            JsonNode *saved = json_object_get_member(row, key);
            if (!saved || !json_node_equal(value, saved)) return FALSE;
        }
    }
    return TRUE;
}

static void playlist_created(RadioSViewContext *ctx, GtkWidget *dialog,
    JsonNode *result, const GError *error, gpointer value)
{
    PlaylistCreate *create = value;
    create->busy = FALSE;
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, TRUE);
    if (!same_identity(create)) {
        gtk_label_set_text(GTK_LABEL(create->status), "Veri kimliği değişti. Kaydı doğrulamak için önceki kullanıcı ve istasyonla yeniden bağlanın.");
        return;
    }
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        gtk_label_set_text(GTK_LABEL(create->status), error ? error->message : "Playlist kaydı doğrulanamadı. Aynı işlemi yeniden deneyin.");
        return;
    }
    JsonObject *playlist = json_node_get_object(result);
    JsonNode *tracks = json_object_get_member(playlist, "tracks");
    if (!g_uuid_string_is_valid(radios_json_string(playlist, "_id", "")) ||
        g_strcmp0(radios_json_string(playlist, "station_id", ""), radios_data_client_station_id(create->identity)) ||
        g_strcmp0(radios_json_string(playlist, "name", ""), radios_json_string(json_node_get_object(create->request), "name", "")) ||
        !tracks || !JSON_NODE_HOLDS_ARRAY(tracks) ||
        !entries_match(create->entries, json_node_get_array(tracks))) {
        gtk_label_set_text(GTK_LABEL(create->status), "Playlist yanıtı seçilen içerikle eşleşmedi. Aynı işlemi yeniden doğrulayın.");
        return;
    }
    PlaylistButton *button = create->button;
    GtkWidget *control = g_object_ref(button->button);
    gtk_widget_destroy(dialog);
    if (!button->disposed) {
        if (button->created) button->created(playlist, button->data);
        else if (ctx->notify) ctx->notify(ctx->notify_data, "Seçilen içeriklerle playlist oluşturuldu.");
    }
    g_object_unref(control);
}

static void create_response(GtkDialog *dialog, gint response, gpointer value)
{
    PlaylistCreate *create = value;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    if (create->busy) return;
    if (!same_identity(create)) {
        gtk_label_set_text(GTK_LABEL(create->status), "Veri bağlantısı veya kullanıcı/istasyon değişti. Önceki kimlikle yeniden bağlanın.");
        return;
    }
    if (!create->request) {
        gchar *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(create->name))));
        if (!*name) {
            gtk_label_set_text(GTK_LABEL(create->status), "Playlist adını girin.");
            gtk_widget_grab_focus(create->name); g_free(name); return;
        }
        create->request = radios_data_body(create->button->ctx);
        JsonObject *body = json_node_get_object(create->request);
        gchar *key = g_uuid_string_random();
        json_object_set_string_member(body, "name", name);
        json_object_set_string_member(body, "idempotency_key", key);
        json_object_set_array_member(body, "entries", json_array_ref(create->entries));
        g_free(key); g_free(name);
    }
    create->busy = TRUE;
    gtk_widget_set_sensitive(create->name, FALSE);
    gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, FALSE);
    gtk_label_set_text(GTK_LABEL(create->status), "Playlist kaydediliyor…");
    if (!radios_data_request(create->button->ctx, GTK_WIDGET(dialog), "POST",
        "/v1/workflows/desktop-playlists/create", create->request, playlist_created, create, NULL)) {
        create->busy = FALSE;
        gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, TRUE);
        gtk_label_set_text(GTK_LABEL(create->status), "Kayıt başlatılamadı. Aynı işlemi yeniden deneyin.");
    }
}

void radios_catalogue_playlist_button_refresh(GtkWidget *widget)
{
    PlaylistButton *button = g_object_get_data(G_OBJECT(widget), "catalogue-playlist-button");
    if (!button) return;
    if (button->disposed) { gtk_widget_set_sensitive(widget, FALSE); return; }
    JsonArray *entries = button->selected(button->data);
    guint count = entries ? json_array_get_length(entries) : 0;
    gtk_widget_set_sensitive(widget, radios_data_connected(button->ctx) && count > 0 && count <= 500);
    if (entries) json_array_unref(entries);
}

static void selection_changed(GtkTreeSelection *selection, gpointer value)
{ (void)selection; radios_catalogue_playlist_button_refresh(value); }
static void connection_changed(RadioSViewContext *ctx, GtkWidget *widget)
{ (void)ctx; radios_catalogue_playlist_button_refresh(widget); }
static void owner_closed(GtkWidget *owner, gpointer value)
{
    (void)owner; GtkWidget *widget = value;
    PlaylistButton *button = g_object_get_data(G_OBJECT(widget), "catalogue-playlist-button");
    button->disposed = TRUE;
    gtk_widget_set_sensitive(widget, FALSE);
    if (button->dialog) gtk_widget_destroy(button->dialog);
}

static void create_clicked(GtkButton *widget, gpointer value)
{
    (void)widget; PlaylistButton *button = value;
    if (button->disposed) return;
    if (button->dialog) { gtk_window_present(GTK_WINDOW(button->dialog)); return; }
    if (!radios_data_connected(button->ctx)) return;
    JsonArray *entries = button->selected(button->data);
    guint count = entries ? json_array_get_length(entries) : 0;
    if (!count || count > 500) { if (entries) json_array_unref(entries); return; }
    PlaylistCreate *create = g_new0(PlaylistCreate, 1);
    create->button = button; create->entries = entries;
    create->identity = radios_data_client_acquire(button->ctx);
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Seçilenlerle playlist oluştur", button->ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, "Vazgeç", GTK_RESPONSE_CANCEL,
        "Oluştur", GTK_RESPONSE_ACCEPT, NULL);
    button->dialog = dialog; gtk_widget_set_name(dialog, "broadcast-selection-playlist-dialog");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "task-dialog");
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(body), 16);
    gtk_box_set_spacing(GTK_BOX(body), 10);
    create->name = radios_ui_entry("Playlist adı");
    gtk_widget_set_name(create->name, "broadcast-selection-playlist-name");
    gtk_entry_set_max_length(GTK_ENTRY(create->name), 120);
    gtk_entry_set_activates_default(GTK_ENTRY(create->name), TRUE);
    gtk_box_pack_start(GTK_BOX(body), create->name, FALSE, FALSE, 0);
    gchar *summary = g_strdup_printf("%u seçili içerik", count);
    create->status = radios_ui_label(summary, "application-status"); g_free(summary);
    gtk_widget_set_name(create->status, "broadcast-selection-playlist-status");
    gtk_label_set_line_wrap(GTK_LABEL(create->status), TRUE);
    gtk_box_pack_start(GTK_BOX(body), create->status, FALSE, FALSE, 0);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_object_set_data_full(G_OBJECT(dialog), "catalogue-playlist-create", create, create_free);
    g_signal_connect(dialog, "response", G_CALLBACK(create_response), create);
    g_signal_connect(dialog, "destroy", G_CALLBACK(dialog_destroyed), create);
    g_signal_connect_object(button->owner, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    g_signal_connect_object(button->button, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    gtk_widget_show_all(dialog); gtk_widget_grab_focus(create->name);
}

GtkWidget *radios_catalogue_playlist_button_new(RadioSViewContext *ctx,
    GtkWidget *owner, GtkTreeSelection *selection,
    RadioSCataloguePlaylistSelection selected, RadioSCataloguePlaylistCreated created, gpointer data)
{
    g_return_val_if_fail(ctx && GTK_IS_WIDGET(owner) && GTK_IS_TREE_SELECTION(selection) && selected, NULL);
    PlaylistButton *button = g_new0(PlaylistButton, 1);
    button->ctx = ctx; button->owner = owner; button->selected = selected; button->created = created; button->data = data;
    button->button = radios_ui_button("Seçilenlerle playlist oluştur");
    gtk_widget_set_name(button->button, "broadcast-catalog-create-playlist");
    gtk_style_context_add_class(gtk_widget_get_style_context(button->button), "ld-btn--primary");
    g_object_set_data_full(G_OBJECT(button->button), "catalogue-playlist-button", button, g_free);
    g_signal_connect(button->button, "clicked", G_CALLBACK(create_clicked), button);
    g_signal_connect_object(owner, "destroy", G_CALLBACK(owner_closed), button->button, 0);
    g_signal_connect_object(selection, "changed", G_CALLBACK(selection_changed), button->button, 0);
    radios_data_register_refresh(ctx, button->button, connection_changed);
    radios_catalogue_playlist_button_refresh(button->button);
    return button->button;
}
