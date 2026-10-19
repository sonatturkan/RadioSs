#include "radios/window.h"
#include "radios/data.h"
#include "radios/media_jobs.h"
#include "radios/media_ui.h"
#include "radios/editors.h"
#include "radios/playout_ui.h"

#include <string.h>
#include <glib/gstdio.h>
#include <sys/stat.h>

static RadioSConfig config;
static GtkApplication *application;
static RadioSDataClient *reader;

typedef struct {RadioSWindow *window; RadioSRuntime *runtime; GtkWidget *root;} Window;
static gboolean dialog_closed(gpointer data);
static void playlist_create(Window *window, const gchar *name);
static gchar *media_fixture_audio(const gchar *name);
static gboolean media_import_finished(gpointer dialog);

static GtkWidget *
find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (GTK_IS_MENU_BUTTON(root)) {
        GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(root)));
        GtkWidget *result = popover ? find(popover, name) : NULL;
        if (result) return result;
    }
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
/* Kılavuz 18.10 §07: the Ö button is gone from the event row; Özellikler is
 * the first item of the row's context menu (right click on the row body). */
static void data_row_properties(GtkWidget *row)
{
    GtkWidget *handle = gtk_bin_get_child(GTK_BIN(row)); g_assert_true(GTK_IS_EVENT_BOX(handle));
    GdkEvent *event = gdk_event_new(GDK_BUTTON_PRESS);
    event->button.window = g_object_ref(gtk_widget_get_window(handle)); event->button.button = 3;
    event->button.time = GDK_CURRENT_TIME; event->button.x = event->button.y = 4;
    gdk_event_set_device(event, gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(handle))));
    g_assert_true(gtk_widget_event(handle, event)); gdk_event_free(event);
    GtkWidget *menu = g_object_get_data(G_OBJECT(row), "programme-row-menu"); g_assert_true(GTK_IS_MENU(menu));
    GtkWidget *item = find(menu, "programme-menu-properties"); g_assert_true(GTK_IS_MENU_ITEM(item));
    gtk_menu_shell_deactivate(GTK_MENU_SHELL(menu)); gtk_menu_item_activate(GTK_MENU_ITEM(item));
}

static GtkWidget *
find_data(GtkWidget *root, const gchar *key)
{
    if (g_object_get_data(G_OBJECT(root), key)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find_data(item->data, key);
    g_list_free(children); return result;
}

static GtkWidget *
find_text(GtkWidget *root, const gchar *text)
{
    if (GTK_IS_LABEL(root) && g_str_equal(gtk_label_get_text(GTK_LABEL(root)), text)) return root;
    if (GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), text) == 0) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find_text(item->data, text);
    g_list_free(children); return result;
}

static GtkWidget *
dialog_find(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), name)) result = item->data;
    g_list_free(windows); return result;
}

static void
pump(void)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
}

typedef gboolean (*Condition)(gpointer);

static void
wait_until(Condition condition, gpointer data, const gchar *description)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(data) && g_get_monotonic_time() < deadline) pump();
    if (!condition(data)) g_error("Native data acceptance timed out: %s", description);
}

static gboolean
documents_ready(GtkWidget *root)
{
    GPtrArray *documents = g_object_get_data(G_OBJECT(root), "radios-data-documents");
    for (guint i = 0; documents && i < documents->len; ++i)
        if (!radios_data_document_ready(g_ptr_array_index(documents, i))) return FALSE;
    if (!GTK_IS_CONTAINER(root)) return TRUE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); gboolean ready = TRUE;
    for (GList *item = children; item && ready; item = item->next) ready = documents_ready(item->data);
    g_list_free(children); return ready;
}

static gboolean
window_ready(gpointer data)
{
    Window *window = data; GtkWidget *label = find(window->root, "footer-station");
    return label && g_str_equal(gtk_label_get_text(GTK_LABEL(label)), "İstasyon: GUI Kabul İstasyonu") && documents_ready(window->root);
}

static Window
window_open(gboolean connected)
{
    Window window = {0}; GError *error = NULL;
    window.runtime = radios_runtime_new(64, 128);
    window.window = radios_window_new(application, &config, window.runtime, FALSE, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(window.window);
    GList *windows = gtk_application_get_windows(application); g_assert_nonnull(windows);
    window.root = windows->data; radios_window_present(window.window);
    if (connected) wait_until(window_ready, &window, "connected station and loaded native documents");
    return window;
}

static void
window_close(Window *window)
{
    radios_window_free(window->window); radios_runtime_free(window->runtime);
    window->window = NULL; window->runtime = NULL; window->root = NULL;
    while (g_main_context_iteration(NULL, FALSE)) {}
}

static void
set(GtkWidget *root, const gchar *name, const gchar *value)
{
    GtkWidget *entry = find(root, name);
    if (!GTK_IS_ENTRY(entry)) g_error("Native text entry '%s' is missing from this screen", name);
    gtk_entry_set_text(GTK_ENTRY(entry), value);
}

static void
click(GtkWidget *root, const gchar *name)
{
    GtkWidget *button = find(root, name); g_assert_true(GTK_IS_BUTTON(button));
    g_assert_true(gtk_widget_is_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button));
}

static void
navigate(Window *window, const gchar *profile)
{
    gchar *name = g_strdup_printf("profile-%s-tab", profile); click(window->root, name); g_free(name); pump();
}

static GtkWidget *
playlist_page(Window *window)
{
    GtkWidget *page = find(window->root, "playlist-task-view");
    g_assert_nonnull(page); g_assert_true(gtk_widget_get_mapped(page));
    return page;
}

static void
click_text(GtkWidget *root, const gchar *text)
{
    GtkWidget *button = find_text(root, text); g_assert_true(GTK_IS_BUTTON(button));
    g_assert_true(gtk_widget_is_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button));
}

static GtkWidget *
field_find(GtkWidget *root, const gchar *name)
{
    if (g_strcmp0(g_object_get_data(G_OBJECT(root), "em-field-name"), name) == 0)
        return g_object_get_data(G_OBJECT(root), "em-field-control");
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = field_find(item->data, name);
    g_list_free(children); return result;
}

typedef struct {RadioSDataClient *client; const gchar *path; JsonNode *body; JsonNode *result; GError *error; gint done;} Read;

static gpointer
read_work(gpointer data)
{
    Read *read = data;
    read->result = radios_data_client_request_sync(read->client, "POST", read->path, read->body, NULL, &read->error);
    g_atomic_int_set(&read->done, 1); return NULL;
}

static gboolean read_finished(gpointer data) {return g_atomic_int_get(&((Read *)data)->done) != 0;}

static JsonNode *
request(const gchar *path, JsonNode *body)
{
    Read read = {reader, path, body, NULL, NULL, 0}; GThread *thread = g_thread_new("native-acceptance-read", read_work, &read);
    wait_until(read_finished, &read, "database readback"); g_thread_join(thread);
    g_assert_no_error(read.error); g_assert_nonnull(read.result); return read.result;
}

static JsonNode *
body_new(void)
{
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(body, json_object_new());
    json_object_set_string_member(json_node_get_object(body), "station_id", radios_data_client_station_id(reader)); return body;
}

static JsonNode *
playlist_read(const gchar *name)
{
    JsonNode *body = body_new(); JsonNode *result = request("/v1/workflows/desktop-playlists/list", body); json_node_free(body);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(result), "playlists"); JsonNode *found = NULL;
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (g_str_equal(radios_json_string(row, "name", ""), name)) {found = json_node_copy(json_array_get_element(rows, i)); break;}
    }
    json_node_free(result); return found;
}

static JsonNode *
document_read(const gchar *kind, const gchar *name)
{
    JsonNode *body = body_new();
    if (kind) json_object_set_string_member(json_node_get_object(body), "kind", kind);
    JsonNode *result = request(kind ? "/v1/workflows/desktop/list" : "/v1/workflows/desktop/campaign-list", body);
    json_node_free(body); JsonArray *rows = json_object_get_array_member(json_node_get_object(result), "records"); JsonNode *found = NULL;
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (g_str_equal(radios_json_string(row, "name", ""), name)) {found = json_node_copy(json_array_get_element(rows, i)); break;}
    }
    json_node_free(result); return found;
}

typedef gboolean (*RecordCondition)(JsonObject *);

static JsonNode *
document_wait(const gchar *kind, const gchar *name, RecordCondition condition)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline) {
        JsonNode *record = document_read(kind, name);
        if (record && (!condition || condition(json_node_get_object(record)))) return record;
        g_clear_pointer(&record, json_node_free);
        gint64 next = g_get_monotonic_time() + 100 * 1000;
        while (g_get_monotonic_time() < next) pump();
    }
    if (g_strcmp0(kind, "emergency") == 0) {
        JsonNode *record = document_read(kind, name);
        if (record) {
            JsonObject *value = json_object_get_object_member(json_node_get_object(record), "config");
            JsonArray *queues = json_object_get_array_member(value, "queues");
            JsonObject *preparation = json_object_get_object_member(value, "preparation");
            g_test_message("Emergency readback: queue rows %u/%u; takeover name '%s'; contact name '%s'",
                json_array_get_length(json_array_get_array_element(queues, 0)),
                json_array_get_length(json_array_get_array_element(queues, 1)),
                radios_json_string(preparation, "takeover/Acil liste adı", ""),
                radios_json_string(preparation, "handover draft 1/Ad", ""));
            json_node_free(record);
        } else g_test_message("Emergency readback: no record was created");
    }
    g_error("Persisted native document did not reach expected state: %s", kind ? kind : "campaign"); return NULL;
}

static gboolean
programme_metadata_saved(JsonObject *record)
{
    JsonObject *value = json_object_get_object_member(record, "config");
    return g_str_equal(radios_json_string(value, "title", ""), "UI Kalıcı Program") &&
        g_str_equal(radios_json_string(value, "date", ""), "2026-10-06");
}

static gboolean
programme_complete(JsonObject *record)
{
    JsonObject *value = json_object_get_object_member(record, "config");
    JsonArray *blocks = json_object_has_member(value, "blocks") ? json_object_get_array_member(value, "blocks") : NULL;
    return g_str_equal(radios_json_string(value, "title", ""), "UI Kalıcı Program") && blocks && json_array_get_length(blocks) == 1 &&
        g_str_equal(radios_json_string(json_array_get_object_element(blocks, 0), "title", ""), "UI Kalıcı Event");
}

static gboolean
programme_has_block(JsonObject *record)
{
    JsonObject *value = json_object_get_object_member(record, "config");
    return json_object_has_member(value, "blocks") && json_array_get_length(json_object_get_array_member(value, "blocks")) == 1;
}

static gboolean
transition_complete(JsonObject *record)
{
    JsonObject *value = json_object_get_object_member(record, "config");
    return g_strcmp0(json_array_get_string_element(json_object_get_array_member(value, "values"), 0), "180") == 0 &&
        json_object_get_double_member(value, "preview_level") == -9;
}

static gboolean
sampler_complete(JsonObject *record)
{
    JsonArray *pads = json_object_get_array_member(json_object_get_object_member(record, "config"), "pads");
    gboolean first = FALSE, custom = FALSE;
    for (guint i = 0; i < json_array_get_length(pads); ++i) {
        JsonObject *pad = json_array_get_object_element(pads, i);
        if (g_str_equal(radios_json_string(pad, "title", ""), "UI Kalıcı Sampler")) first = TRUE;
        if (g_str_equal(radios_json_string(pad, "title", ""), "UI Dokuzuncu Kanal Sampler") &&
                g_str_equal(radios_json_string(pad, "key", ""), "channel-9-pad-1")) custom = TRUE;
    }
    return json_array_get_length(pads) == 2 && first && custom;
}

static gboolean channels_complete(JsonObject *record)
{
    return radios_json_int(json_object_get_object_member(record, "config"), "deck_count", 0) == 9;
}

static gboolean
emergency_complete(JsonObject *record)
{
    JsonObject *value = json_object_get_object_member(record, "config");
    JsonArray *queues = json_object_get_array_member(value, "queues");
    return json_array_get_length(json_array_get_array_element(queues, 0)) == 1 &&
        g_str_equal(radios_json_string(json_object_get_object_member(value, "preparation"), "takeover/Acil liste adı", ""), "UI Acil Liste Adı") &&
        g_str_equal(radios_json_string(json_object_get_object_member(value, "preparation"), "handover draft 1/Ad", ""), "UI Acil Yetkili");
}

static gboolean
automation_complete(JsonObject *record)
{
    JsonObject *value = json_object_get_object_member(record, "config");
    return g_str_equal(radios_json_string(value, "name", ""), "UI Kalıcı Rotasyon") &&
        json_array_get_length(json_object_get_array_member(value, "sources")) == 1;
}

static gboolean
person_saved(gpointer data)
{
    GtkWidget *dialog = data; GtkWidget *form = g_object_get_data(G_OBJECT(dialog), "technical-form");
    GtkWidget *label = g_object_get_data(G_OBJECT(form), "technical-status-label");
    if (!label) return FALSE;
    const gchar *message = gtk_label_get_text(GTK_LABEL(label));
    if (g_str_has_prefix(message, "Kullanıcı ve istasyon izinleri kaydedildi.")) return TRUE;
    /* A completed, rejected write cannot become successful by waiting. Report
     * its actual form error; retain the single-send mutation and all readback
     * assertions instead of turning a useful failure into a 15-second timeout. */
    if (*message && !g_str_equal(message, "Veritabanına kaydediliyor…") &&
            gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT)))
        g_error("Native user creation was rejected: %s", message);
    return FALSE;
}

static void
test_profiles_persistence(void)
{
    Window window = window_open(TRUE);
    JsonNode *existing_playlist = playlist_read("UI Kalıcı Playlist");
    if (!existing_playlist) playlist_create(&window, "UI Kalıcı Playlist");
    g_clear_pointer(&existing_playlist, json_node_free); navigate(&window, "broadcast");
    /* The genuine add-event action creates the current document in a fresh
     * fixture; perform it once before reading and saving its metadata. */
    click(window.root, "programme-add-music");
    /* Programme metadata now has no broadcast toolbar editor. Keep this
     * persistence scenario on the genuine native document save/apply path. */
    RadioSViewContext *ctx = radios_window_view_context(window.window);
    RadioSDocument *programme = radios_broadcast_programme_document(ctx); g_assert_nonnull(programme);
    JsonNode *record = document_wait("programme", "Mevcut program", programme_has_block);
    JsonNode *programme_config = json_node_copy(json_object_get_member(json_node_get_object(record), "config"));
    json_node_free(record);
    json_object_set_string_member(json_node_get_object(programme_config), "title", "UI Kalıcı Program");
    json_object_set_string_member(json_node_get_object(programme_config), "date", "2026-10-06");
    g_assert_true(radios_data_document_save(programme, programme_config)); json_node_free(programme_config);
    record = document_wait("programme", "Mevcut program", programme_metadata_saved); json_node_free(record);
    GtkWidget *dialog;
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(window.root, "programme-title"))), ==, "UI Kalıcı Program");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(window.root, "programme-date"))), ==, "2026-10-06");
    /* The new event opens empty; its Özellikler is on the event overview. */
    if (gtk_widget_is_visible(find(window.root, "programme-queue-back"))) click(window.root, "programme-queue-back");
    data_row_properties(GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(g_object_get_data(G_OBJECT(find(window.root, "programme-queue-table")), "list-box")), 0)));
    dialog = dialog_find("programme-block-editor"); g_assert_nonnull(dialog);
    set(dialog, "programme-block-title", "UI Kalıcı Event"); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    record = document_wait("programme", "Mevcut program", programme_complete); json_node_free(record);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    click_text(find(window.root, "flow-workspace"), "⚙"); dialog = dialog_find("transition-editor"); g_assert_nonnull(dialog);
    wait_until(window_ready, &window, "transition document initial load");
    const gchar *fields[] = {"transition-duration", "transition-cue-in", "transition-intro", "transition-out", "transition-b-in", "transition-fade-in", "transition-overlap", "transition-ducking"};
    const gchar *values[] = {"180", "5", "10", "175", "0", "1", "3", "-6"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) set(dialog, fields[i], values[i]);
    gtk_range_set_value(GTK_RANGE(find(dialog, "transition-preview-level")), -9);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    record = document_wait("transition", "Geçiş ayarları", transition_complete); json_node_free(record);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    navigate(&window, "dj"); GtkWidget *pad = find_data(find(window.root, "dj-task-view"), "dj-pad-button"); g_assert_nonnull(pad);
    gtk_button_clicked(GTK_BUTTON(g_object_get_data(G_OBJECT(pad), "dj-pad-edit")));
    dialog = dialog_find("dj-sampler-dialog"); g_assert_nonnull(dialog); set(dialog, "dj-sampler-title", "UI Kalıcı Sampler");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); wait_until(dialog_closed, "dj-sampler-dialog", "native sampler assignment");
    record = document_wait("sampler", "DJ sampler", NULL); json_node_free(record);
    click(window.root, "dj-add-channel");
    record = document_wait("channel", "DJ kanalları", channels_complete); json_node_free(record);
    click(window.root, "dj-sampler-channel-9-pad-1-edit"); dialog = dialog_find("dj-sampler-dialog"); g_assert_nonnull(dialog);
    set(dialog, "dj-sampler-title", "UI Dokuzuncu Kanal Sampler"); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    wait_until(dialog_closed, "dj-sampler-dialog", "custom channel sampler assignment");
    record = document_wait("sampler", "DJ sampler", sampler_complete); json_node_free(record);
    navigate(&window, "emergency"); set(window.root, "em-editor-title", "UI Kalıcı Acil Öğe"); set(window.root, "em-editor-duration", "00:12");
    click(window.root, "em-editor-save");
    GList *items = gtk_container_get_children(GTK_CONTAINER(find(window.root, "em-list-items")));
    g_assert_cmpint(g_list_length(items), ==, 1); g_list_free(items);
    GtkWidget *name = field_find(find(window.root, "emergency-task-view"), "Acil liste adı");
    g_assert_true(GTK_IS_ENTRY(name)); gtk_entry_set_text(GTK_ENTRY(name), "UI Acil Liste Adı");
    GtkWidget *emergency = find_data(find(window.root, "emergency-task-view"), "em-state");
    GtkWidget *settings = g_object_get_data(G_OBJECT(emergency), "em-settings-stack"); gtk_stack_set_visible_child_name(GTK_STACK(settings), "handover");
    gtk_entry_set_text(GTK_ENTRY(field_find(emergency, "Ad")), "UI Acil Yetkili");
    gtk_entry_set_text(GTK_ENTRY(field_find(emergency, "Görev")), "Vardiya sorumlusu");
    gtk_entry_set_text(GTK_ENTRY(field_find(emergency, "Telefon")), "+90 555 000 0000");
    click(window.root, "em-contact-add"); click(window.root, "em-log-save");
    record = document_wait("emergency", "Acil hazırlık", emergency_complete); json_node_free(record);
    navigate(&window, "automation"); set(window.root, "automation-name", "UI Kalıcı Rotasyon"); click(window.root, "automation-source-add");
    JsonNode *playlist = playlist_read("UI Kalıcı Playlist"); g_assert_nonnull(playlist);
    GtkWidget *selector = find(window.root, "automation-source-selector"); g_assert_true(GTK_IS_COMBO_BOX(selector));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(selector), radios_json_string(json_node_get_object(playlist), "_id", "")));
    json_node_free(playlist); click(window.root, "automation-save");
    record = document_wait("automation", "main", automation_complete); json_node_free(record);
    navigate(&window, "technical"); click_text(window.root, "+ Sunucu ekle"); dialog = dialog_find("technical-server-dialog"); g_assert_nonnull(dialog);
    set(dialog, "technical-server-name", "UI Kalıcı Sunucu"); set(dialog, "technical-server-station", "GUI Kabul İstasyonu");
    set(dialog, "technical-server-address", "https://server.example.invalid"); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    wait_until(dialog_closed, "technical-server-dialog", "actual server metadata save");
    record = document_wait("server", "UI Kalıcı Sunucu", NULL); json_node_free(record);
    GtkWidget *people = find(window.root, "technical-people-pane"); gtk_stack_set_visible_child_name(GTK_STACK(gtk_widget_get_parent(people)), "people");
    click_text(window.root, "+ Kişi ata"); dialog = dialog_find("technical-person-dialog"); g_assert_nonnull(dialog);
    set(dialog, "technical-person-name", "UI Kalıcı Kullanıcı"); set(dialog, "technical-person-login", "ui_operator");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); wait_until(person_saved, dialog, "actual user and station permission save");
    g_assert_false(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT)));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    JsonNode *context_body = body_new(); JsonNode *context = request("/v1/workflows/desktop/context", context_body); json_node_free(context_body);
    JsonObject *context_object = json_node_get_object(context); JsonArray *users = json_object_get_array_member(context_object, "users");
    gchar *created_user = NULL;
    for (guint i = 0; i < json_array_get_length(users); ++i) {
        JsonObject *user = json_array_get_object_element(users, i);
        if (g_str_equal(radios_json_string(user, "username", ""), "ui_operator")) created_user = g_strdup(radios_json_string(user, "_id", ""));
    }
    g_assert_nonnull(created_user); JsonArray *permissions = json_object_get_array_member(context_object, "permissions"); gboolean view_granted = FALSE;
    for (guint i = 0; i < json_array_get_length(permissions); ++i) {
        JsonObject *grant = json_array_get_object_element(permissions, i);
        if (g_str_equal(radios_json_string(grant, "user_id", ""), created_user) &&
                g_str_equal(radios_json_string(grant, "station_id", ""), radios_data_client_station_id(reader)) &&
                g_str_equal(radios_json_string(grant, "permission", ""), "view")) view_granted = TRUE;
    }
    g_assert_true(view_granted); g_free(created_user); json_node_free(context);
    gchar *database_dir = g_path_get_dirname(config.database_file); gchar *keys = g_build_filename(database_dir, "private-user-keys", NULL);
    GDir *key_directory = g_dir_open(keys, 0, NULL); g_assert_nonnull(key_directory); const gchar *key_name = g_dir_read_name(key_directory); g_assert_nonnull(key_name);
    gchar *key_file = g_build_filename(keys, key_name, NULL); GStatBuf info; g_assert_cmpint(g_stat(key_file, &info), ==, 0); g_assert_cmpint(info.st_mode & 0777, ==, 0600);
    g_dir_close(key_directory); g_free(key_file); g_free(keys); g_free(database_dir);
    navigate(&window, "advert"); click(window.root, "advert-campaign-add"); dialog = dialog_find("advert-campaign-dialog"); g_assert_nonnull(dialog);
    set(dialog, "advert-campaign-name", "UI Kalıcı Kampanya"); set(dialog, "advert-campaign-client", "UI Reklamveren");
    set(dialog, "advert-campaign-start", "2026-10-06"); set(dialog, "advert-campaign-end", "2026-11-06"); set(dialog, "advert-campaign-spot", "UI Spot");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); wait_until(dialog_closed, "advert-campaign-dialog", "actual draft campaign save");
    record = document_wait(NULL, "UI Kalıcı Kampanya", NULL); g_assert_cmpstr(radios_json_string(json_node_get_object(record), "status", ""), ==, "draft"); json_node_free(record);
    window_close(&window); window = window_open(TRUE);
    navigate(&window, "broadcast");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(window.root, "programme-title"))), ==, "UI Kalıcı Program");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(window.root, "programme-date"))), ==, "2026-10-06");
    GtkListBox *queue = g_object_get_data(G_OBJECT(find(window.root, "programme-queue-table")), "list-box");
    g_assert_nonnull(gtk_list_box_get_row_at_index(queue, 0)); g_assert_null(gtk_list_box_get_row_at_index(queue, 1));
    g_assert_nonnull(find_text(find(window.root, "programme-queue-table"), "UI Kalıcı Event"));
    click_text(find(window.root, "flow-workspace"), "⚙"); dialog = dialog_find("transition-editor"); wait_until(window_ready, &window, "restored transition document");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "transition-duration"))), ==, "180");
    g_assert_cmpfloat(gtk_range_get_value(GTK_RANGE(find(dialog, "transition-preview-level"))), ==, -9); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    navigate(&window, "dj"); g_assert_nonnull(find_text(window.root, "UI Kalıcı Sampler"));
    g_assert_nonnull(find(window.root, "dj-channel-9"));
    g_assert_nonnull(find_text(find(window.root, "dj-channel-9"), "UI Dokuzuncu Kanal Sampler"));
    navigate(&window, "emergency"); GtkWidget *list = find(window.root, "em-list-items"); GList *rows = gtk_container_get_children(GTK_CONTAINER(list));
    g_assert_cmpint(g_list_length(rows), ==, 1); g_list_free(rows);
    name = field_find(find(window.root, "emergency-task-view"), "Acil liste adı"); g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(name)), ==, "UI Acil Liste Adı");
    GtkWidget *contact = find(window.root, "em-contact-add"); GtkWidget *contacts = g_object_get_data(G_OBJECT(contact), "em-draft-list");
    rows = gtk_container_get_children(GTK_CONTAINER(contacts)); g_assert_cmpint(g_list_length(rows), ==, 1); g_list_free(rows);
    navigate(&window, "automation"); g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(window.root, "automation-name"))), ==, "UI Kalıcı Rotasyon");
    g_assert_nonnull(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "automation-source-selector"))));
    navigate(&window, "technical"); g_assert_nonnull(find_text(window.root, "UI Kalıcı Sunucu"));
    g_assert_nonnull(find_text(window.root, "UI Kalıcı Kullanıcı · ui_operator"));
    navigate(&window, "advert"); GtkTreeView *tree = GTK_TREE_VIEW(find(window.root, "advert-campaign-table"));
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_tree_view_get_model(tree), NULL), ==, 1);
    window_close(&window);
}

static gboolean
playlist_created(gpointer data)
{
    GtkWidget *selector = find(data, "pw-target-playlist"), *tabs = find(data, "pw-playlists");
    return !dialog_find("pw-create-dialog") && selector && tabs &&
        gtk_combo_box_get_active(GTK_COMBO_BOX(selector)) >= 0 && gtk_widget_is_sensitive(tabs);
}

static void
playlist_create(Window *window, const gchar *name)
{
    navigate(window, "playlist");
    GtkWidget *page = playlist_page(window);
    click(page, "pw-new-playlist"); GtkWidget *dialog = dialog_find("pw-create-dialog"); g_assert_nonnull(dialog);
    set(dialog, "pw-new-name", name);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(dialog, "pw-new-purpose")), "music"));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    wait_until(playlist_created, page, "named playlist saved from the new creation dialog");
    gchar *selected = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(find(page, "pw-target-playlist")));
    g_assert_cmpstr(selected, ==, name); g_free(selected);
}

static gboolean dialog_closed(gpointer data) {return dialog_find(data) == NULL;}

typedef struct { GtkWidget *chooser; const gchar *path; } PlaylistFileChoice;
static gboolean playlist_file_selected(gpointer data)
{
    PlaylistFileChoice *choice = data;
    gchar *selected = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(choice->chooser));
    gboolean same = !g_strcmp0(selected, choice->path); g_free(selected); return same;
}
typedef struct { GtkWidget *page; const gchar *title; GtkTreePath *path; } PlaylistSource;
static gboolean playlist_source_ready(gpointer data)
{
    PlaylistSource *source = data; GtkWidget *tree = find(source->page, "pw-source-table");
    if (!tree) return FALSE;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        gchar *title = NULL; gtk_tree_model_get(model, &iter, 1, &title, -1);
        gboolean same = !g_strcmp0(title, source->title); g_free(title);
        if (same) {
            g_clear_pointer(&source->path, gtk_tree_path_free);
            source->path = gtk_tree_model_get_path(model, &iter); return TRUE;
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return FALSE;
}
static gboolean playlist_track_saved(gpointer page)
{
    GtkWidget *tree = find(page, "pw-builder-table"), *tabs = find(page, "pw-playlists");
    return tree && tabs && gtk_widget_is_sensitive(tabs) &&
        gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(tree)), NULL) == 1;
}

static void
test_playlist_persistence(void)
{
    Window window = window_open(TRUE); playlist_create(&window, "UI Kalıcı Playlist");
    GtkWidget *page = playlist_page(&window); gchar *path = media_fixture_audio("ui-playlist-import.wav");
    click(page, "pw-track-new"); GtkWidget *library = dialog_find("media-library-dialog"), *chooser = dialog_find("media-import-chooser");
    g_assert_nonnull(library); g_assert_true(GTK_IS_FILE_CHOOSER(chooser)); g_assert_null(dialog_find("pw-track-dialog"));
    g_assert_true(gtk_file_chooser_get_select_multiple(GTK_FILE_CHOOSER(chooser)));
    PlaylistFileChoice choice = {chooser, path}; g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), path));
    wait_until(playlist_file_selected, &choice, "generated WAV selected in real multi-file import chooser");
    gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_ACCEPT);
    wait_until(media_import_finished, library, "playlist add-track action imports and verifies actual audio");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(library, "media-import-status"))), "1 kayıt doğrulandı"));
    gtk_dialog_response(GTK_DIALOG(library), GTK_RESPONSE_CLOSE);
    PlaylistSource source = {page, "ui-playlist-import", NULL};
    wait_until(playlist_source_ready, &source, "upper source browser refreshes to show the new verified import");
    GtkWidget *source_tree = find(page, "pw-source-table");
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(source_tree)), source.path); gtk_tree_path_free(source.path);
    click(page, "pw-source-add"); wait_until(playlist_track_saved, page, "selected verified source appended to the lower target");
    GtkTreeView *tree = GTK_TREE_VIEW(find(page, "pw-builder-table")); GtkTreePath *first = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(tree), first); gtk_tree_path_free(first);
    click(page, "pw-track-edit"); GtkWidget *dialog = dialog_find("pw-track-dialog"); g_assert_nonnull(dialog);
    set(dialog, "pw-edit-title", "UI Gerçek Parça Kaydı"); set(dialog, "pw-edit-artist", "Türkçe Sanatçı");
    set(dialog, "pw-edit-bpm", "128");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    wait_until(dialog_closed, "pw-track-dialog", "track dialog closes after confirmed database save");
    JsonNode *record = playlist_read("UI Kalıcı Playlist"); g_assert_nonnull(record);
    JsonArray *tracks = json_object_get_array_member(json_node_get_object(record), "tracks");
    g_assert_cmpuint(json_array_get_length(tracks), ==, 1); JsonObject *track = json_array_get_object_element(tracks, 0);
    g_assert_cmpstr(radios_json_string(track, "title", ""), ==, "UI Gerçek Parça Kaydı");
    g_assert_cmpstr(radios_json_string(track, "artist", ""), ==, "Türkçe Sanatçı");
    g_assert_cmpstr(radios_json_string(track, "media_state", ""), ==, "unverified");
    g_assert_true(g_uuid_string_is_valid(radios_json_string(track, "media_id", "")));
    JsonNode *media_body = body_new();
    json_object_set_string_member(json_node_get_object(media_body), "media_id", radios_json_string(track, "media_id", ""));
    JsonNode *media_detail = request("/v1/workflows/desktop-media/get", media_body); json_node_free(media_body);
    JsonObject *version = json_object_get_object_member(json_node_get_object(media_detail), "version");
    g_assert_true(json_object_get_boolean_member(version, "verified"));
    g_assert_cmpstr(radios_json_string(track, "version_id", ""), ==, radios_json_string(version, "_id", ""));
    g_assert_cmpstr(radios_json_string(track, "media_file_id", ""), ==, radios_json_string(version, "media_file_id", ""));
    g_assert_cmpstr(radios_json_string(track, "media_sha256", ""), ==, radios_json_string(version, "sha256", ""));
    json_node_free(media_detail);
    g_assert_cmpint(g_unlink(path), ==, 0); g_free(path);
    json_node_free(record); window_close(&window); window = window_open(TRUE); navigate(&window, "playlist");
    page = playlist_page(&window);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!find_text(page, "UI Kalıcı Playlist") && g_get_monotonic_time() < deadline) pump();
    g_assert_nonnull(find_text(page, "UI Kalıcı Playlist"));
    tree = GTK_TREE_VIEW(find(page, "pw-builder-table")); g_assert_true(GTK_IS_TREE_VIEW(tree));
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_tree_view_get_model(tree), NULL), ==, 1);
    window_close(&window);
}

static gboolean
rename_failed(gpointer data)
{
    GtkWidget *dialog = data; GtkWidget *label = g_object_get_data(G_OBJECT(dialog), "pw-save-status");
    return label && *gtk_label_get_text(GTK_LABEL(label)) != '\0' &&
        gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT)) &&
        !g_str_has_prefix(gtk_label_get_text(GTK_LABEL(label)), "Oturum");
}

static void
test_conflict_input_retention(void)
{
    Window window = window_open(TRUE); playlist_create(&window, "UI Çakışma Listesi");
    click(playlist_page(&window), "pw-playlist-options");
    click(playlist_page(&window), "pw-rename"); GtkWidget *dialog = dialog_find("pw-name-dialog"); g_assert_nonnull(dialog);
    set(dialog, "pw-edit-name", "Kullanıcının korunacak girdisi");
    JsonNode *row = playlist_read("UI Çakışma Listesi"); g_assert_nonnull(row); JsonObject *record = json_node_get_object(row);
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "playlist_id", radios_json_string(record, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(record, "_rev", 0));
    json_object_set_string_member(object, "name", "Başka oturumun yeni adı"); gchar *key = g_uuid_string_random();
    json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonNode *changed = request("/v1/workflows/desktop-playlists/rename", body);
    json_node_free(changed); json_node_free(body); json_node_free(row);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); wait_until(rename_failed, dialog, "409 preserves native form input");
    g_assert_true(gtk_widget_get_visible(dialog));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "pw-edit-name"))), ==, "Kullanıcının korunacak girdisi");
    row = playlist_read("Başka oturumun yeni adı"); g_assert_nonnull(row); json_node_free(row);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); window_close(&window);
}

static void
service_command(const gchar *operation)
{
    gchar *script = g_build_filename(config.project_root, "tools/radios_database.py", NULL);
    gchar *args[] = {config.python_command, script, "--project-root", config.project_root, (gchar *)operation, NULL};
    gchar *output = NULL, *diagnostic = NULL; gint status = 0; GError *error = NULL;
    gboolean spawned = g_spawn_sync(config.project_root, args, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                                   &output, &diagnostic, &status, &error);
    g_assert_true(spawned); g_assert_no_error(error); g_assert_true(g_spawn_check_wait_status(status, &error)); g_assert_no_error(error);
    g_free(script); g_free(output); g_free(diagnostic);
}

static void
test_transport_input_retention(void)
{
    Window window = window_open(TRUE); playlist_create(&window, "UI Bağlantı Listesi");
    click(playlist_page(&window), "pw-playlist-options");
    click(playlist_page(&window), "pw-rename"); GtkWidget *dialog = dialog_find("pw-name-dialog"); g_assert_nonnull(dialog);
    set(dialog, "pw-edit-name", "Bağlantı dönünce kaydedilen ad"); service_command("stop");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); wait_until(rename_failed, dialog, "transport failure preserves input");
    g_assert_true(gtk_widget_get_visible(dialog));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "pw-edit-name"))), ==, "Bağlantı dönünce kaydedilen ad");
    service_command("start"); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    wait_until(dialog_closed, "pw-name-dialog", "explicit retry after service recovery");
    JsonNode *row = playlist_read("Bağlantı dönünce kaydedilen ad"); g_assert_nonnull(row); json_node_free(row);
    window_close(&window);
}

static void
test_shutdown_during_connect(void)
{
    for (guint i = 0; i < 3; ++i) {Window window = window_open(FALSE); if (i) pump(); window_close(&window);}
    Window window = window_open(TRUE); window_close(&window);
}

static gchar *
sentinel_work(gpointer data, const RadioSCancelToken *cancel, GError **error)
{
    (void)cancel; (void)error; g_atomic_int_set((gint *)data, 1); return NULL;
}

static gboolean sentinel_done(gpointer data) {return g_atomic_int_get((gint *)data) != 0;}
static gboolean file_exists(gpointer data) {return g_file_test(data, G_FILE_TEST_IS_REGULAR);}
typedef struct { GtkWidget *selector; const gchar *id; } SourceChoice;
static gboolean source_choice_ready(gpointer value)
{
    SourceChoice *choice = value;
    return gtk_combo_box_set_active_id(GTK_COMBO_BOX(choice->selector), choice->id);
}
typedef struct { RadioSDocument *document; const gchar *id; gint64 revision; } DocumentAck;
static gboolean document_acknowledged(gpointer value)
{
    DocumentAck *ack = value;
    return radios_data_document_revision(ack->document) == ack->revision &&
        g_str_equal(radios_data_document_id(ack->document), ack->id);
}

static void
test_refresh_edit_revision_race(void)
{
    /* This case owns its own prerequisite, including the native save ACK.
     * It must not depend on all-profiles-persistence having run first. */
    Window window = window_open(TRUE); playlist_create(&window, "Yarış öncesi kaynak playlist");
    JsonNode *playlist = playlist_read("Yarış öncesi kaynak playlist"); g_assert_nonnull(playlist);
    navigate(&window, "automation"); set(window.root, "automation-name", "UI Kalıcı Rotasyon");
    click(window.root, "automation-source-add");
    SourceChoice choice = {find(window.root, "automation-source-selector"),
        radios_json_string(json_node_get_object(playlist), "_id", "")};
    g_assert_true(GTK_IS_COMBO_BOX(choice.selector));
    wait_until(source_choice_ready, &choice, "new native automation source selects its own saved playlist");
    json_node_free(playlist); click(window.root, "automation-save");
    JsonNode *record = document_wait("automation", "main", automation_complete);
    JsonObject *stored = json_node_get_object(record);
    GtkWidget *page = find(window.root, "automation-task-view"); g_assert_nonnull(page);
    GtkWidget *document_owner = find_data(page, "automation-view"); g_assert_nonnull(document_owner);
    GPtrArray *documents = g_object_get_data(G_OBJECT(document_owner), "radios-data-documents");
    g_assert_nonnull(documents); g_assert_cmpuint(documents->len, ==, 1);
    DocumentAck ack = {g_ptr_array_index(documents, 0), radios_json_string(stored, "_id", ""),
        radios_json_int(stored, "_rev", 0)};
    g_assert_cmpint(ack.revision, >, 0);
    wait_until(document_acknowledged, &ack, "clean native automation identity/revision ACK before remote CAS");
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "automation"); json_object_set_string_member(object, "name", "main");
    json_object_set_string_member(object, "document_id", radios_json_string(stored, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(stored, "_rev", 0));
    JsonObject *remote_config = json_object_get_object_member(stored, "config");
    json_object_set_string_member(remote_config, "name", "Harici oturumun rotasyonu");
    JsonNode *remote_node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(remote_node, remote_config);
    json_object_set_member(object, "config", remote_node); gchar *key = g_uuid_string_random();
    json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonNode *changed = request("/v1/workflows/desktop/put", body); json_node_free(changed); json_node_free(body); json_node_free(record);
    gchar *directory = g_path_get_dirname(config.database_file); gchar *gate = g_build_filename(directory, ".acceptance-list-hold", NULL);
    gchar *ready = g_build_filename(directory, ".acceptance-list-ready", NULL); g_unlink(ready);
    g_assert_true(g_file_set_contents(gate, "automation", -1, NULL));
    click_text(window.root, "Verileri yenile"); wait_until(file_exists, ready, "newer automation LIST is held before GTK completion");
    set(window.root, "automation-name", "Korunacak yerel rotasyon girdisi"); g_assert_cmpint(g_unlink(gate), ==, 0);
    gint done = 0; GError *error = NULL;
    g_assert_cmpuint(radios_runtime_submit(window.runtime, sentinel_work, &done, NULL, &error), >, 0); g_assert_no_error(error);
    wait_until(sentinel_done, &done, "all refresh jobs complete before save"); pump();
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(window.root, "automation-name"))), ==, "Korunacak yerel rotasyon girdisi");
    click(window.root, "automation-save");
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (!dialog_find("database-reload-dialog") && g_get_monotonic_time() < deadline) pump();
    GtkWidget *conflict = dialog_find("database-reload-dialog"); g_assert_nonnull(conflict);
    record = document_read("automation", "main"); g_assert_nonnull(record);
    g_assert_cmpstr(radios_json_string(json_object_get_object_member(json_node_get_object(record), "config"), "name", ""), ==, "Harici oturumun rotasyonu");
    json_node_free(record);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(window.root, "automation-name"))), ==, "Korunacak yerel rotasyon girdisi");
    gtk_dialog_response(GTK_DIALOG(conflict), GTK_RESPONSE_CANCEL); window_close(&window);
    g_unlink(ready); g_free(ready); g_free(gate); g_free(directory);
}

typedef struct { RadioSViewContext ctx; GtkWidget *parent; } MediaHarness;
typedef struct { guint callbacks, destroyed; gboolean done; JsonNode *result; GError *error; } MediaResult;
static gboolean media_connected(gpointer value) { return radios_data_connected(&((MediaHarness *)value)->ctx); }
static gboolean media_finished(gpointer value) { return ((MediaResult *)value)->done; }
static void media_callback(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; MediaResult *output = value; ++output->callbacks;
    output->result = result ? json_node_copy(result) : NULL; output->error = error ? g_error_copy(error) : NULL; output->done = TRUE;
}
static void media_destroy(gpointer value) { ++((MediaResult *)value)->destroyed; }
static MediaHarness *media_harness_open(void)
{
    MediaHarness *harness = g_new0(MediaHarness, 1); harness->parent = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(harness->parent);
    harness->ctx.config = &config; harness->ctx.parent = GTK_WINDOW(harness->parent); harness->ctx.runtime = radios_runtime_new(16, 32);
    harness->ctx.data = radios_data_new(&harness->ctx); radios_data_connect(&harness->ctx);
    wait_until(media_connected, harness, "connected media harness"); return harness;
}
static void media_harness_close(MediaHarness *harness)
{
    gtk_widget_destroy(harness->parent); radios_data_close(harness->ctx.data); radios_runtime_free(harness->ctx.runtime);
    while (g_main_context_iteration(NULL, FALSE)) {}
    radios_data_unref(harness->ctx.data); g_object_unref(harness->parent); g_free(harness);
}
typedef struct {
    RadioSDocument *document;
    GtkWidget *entry;
    guint applied;
    gint expected;
    gint64 revision;
} DocumentReadField;
typedef struct {
    DocumentReadField *first, *second;
    const gchar *counter;
    guint requests;
} DocumentReadWait;
static void document_read_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *value, gpointer data)
{
    (void)ctx; (void)owner; DocumentReadField *field = data;
    gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(value, "deck_count", 0));
    gtk_entry_set_text(GTK_ENTRY(field->entry), text); g_free(text); ++field->applied;
}
static guint document_read_count(const gchar *path)
{
    gchar *text = NULL; guint count = 0;
    if (g_file_get_contents(path, &text, NULL, NULL)) count = (guint)g_ascii_strtoull(text, NULL, 10);
    g_free(text); return count;
}
static gboolean document_read_field_ready(gpointer data)
{
    DocumentReadField *field = data;
    return field->applied && radios_data_document_revision(field->document) == field->revision &&
        (gint)g_ascii_strtoll(gtk_entry_get_text(GTK_ENTRY(field->entry)), NULL, 10) == field->expected;
}
static gboolean document_read_both_ready(gpointer data)
{
    DocumentReadWait *wait = data;
    return document_read_field_ready(wait->first) && document_read_field_ready(wait->second) &&
        document_read_count(wait->counter) >= wait->requests;
}
static void document_read_quiet(DocumentReadWait *wait)
{
    gint64 until = g_get_monotonic_time() + 350 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < until) pump();
    g_assert_cmpuint(document_read_count(wait->counter), ==, wait->requests);
    g_assert_true(document_read_field_ready(wait->first)); g_assert_true(document_read_field_ready(wait->second));
}
static JsonNode *document_read_config(gint decks)
{
    JsonNode *value = json_node_new(JSON_NODE_OBJECT); json_node_take_object(value, json_object_new());
    json_object_set_int_member(json_node_get_object(value), "deck_count", decks); return value;
}
static JsonNode *document_read_put(const gchar *name, gint decks, JsonNode *previous)
{
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "channel"); json_object_set_string_member(object, "name", name);
    json_object_set_member(object, "config", document_read_config(decks));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    if (previous) {
        JsonObject *stored = json_node_get_object(previous);
        json_object_set_string_member(object, "document_id", radios_json_string(stored, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(stored, "_rev", 0));
    }
    JsonNode *record = request("/v1/workflows/desktop/put", body); json_node_free(body); return record;
}
static void test_document_read_order(void)
{
    const gchar *first_name = "Okuma sırası bir", *second_name = "Okuma sırası iki";
    gint timeout = config.request_timeout_seconds; config.request_timeout_seconds = 12;
    MediaHarness *harness = media_harness_open(); config.request_timeout_seconds = timeout;
    JsonNode *first_record = document_read_put(first_name, 5, NULL);
    JsonNode *second_record = document_read_put(second_name, 6, NULL);
    GtkWidget *owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_container_add(GTK_CONTAINER(harness->parent), owner);
    DocumentReadField first = {.entry = gtk_entry_new(), .expected = 5, .revision = 1};
    DocumentReadField second = {.entry = gtk_entry_new(), .expected = 6, .revision = 1};
    gtk_box_pack_start(GTK_BOX(owner), first.entry, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(owner), second.entry, FALSE, FALSE, 0);
    gchar *directory = g_path_get_dirname(config.database_file);
    gchar *gate = g_build_filename(directory, ".acceptance-document-read-hold", NULL);
    gchar *ready = g_build_filename(directory, ".acceptance-document-read-ready", NULL);
    gchar *counter = g_build_filename(directory, ".acceptance-document-read-count-channel", NULL);
    guint baseline = document_read_count(counter);
    first.document = radios_data_document_new(&harness->ctx, owner, "channel", first_name, document_read_apply, &first);
    second.document = radios_data_document_new(&harness->ctx, owner, "channel", second_name, document_read_apply, &second);
    DocumentReadWait wait = {&first, &second, counter, baseline + 2};
    wait_until(document_read_both_ready, &wait, "two documents load and the shared-owner initial refresh coalesces");
    gint64 settle = g_get_monotonic_time() + 350 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < settle) pump();
    wait.requests = document_read_count(counter); g_assert_cmpuint(wait.requests, <=, baseline + 4);
    document_read_quiet(&wait);

    /* Runtime workers are normally serial. A second private worker forces a
     * genuine save ACK to arrive before the earlier computed LIST response,
     * without mocking the document callbacks or production data source. */
    g_unlink(ready); g_assert_true(g_file_set_contents(gate, "channel", -1, NULL));
    baseline = document_read_count(counter); radios_data_refresh(&harness->ctx);
    wait_until(file_exists, ready, "older channel read computed before the native save");
    RadioSRuntime *original = harness->ctx.runtime, *save_runtime = radios_runtime_new(16, 32);
    harness->ctx.runtime = save_runtime; gtk_entry_set_text(GTK_ENTRY(first.entry), "9");
    JsonNode *saved_config = document_read_config(9);
    g_assert_true(radios_data_document_save(first.document, saved_config)); json_node_free(saved_config);
    first.expected = 9; first.revision = 2;
    wait_until(document_read_field_ready, &first, "newer native save ACK updates revision and form before the held read");
    guint save_applied = first.applied;
    harness->ctx.runtime = original; radios_runtime_free(save_runtime);
    g_assert_cmpint(g_unlink(gate), ==, 0); wait.requests = baseline + 1;
    wait_until(document_read_both_ready, &wait, "older LIST finishes after the newer acknowledged save");
    document_read_quiet(&wait); g_assert_cmpuint(first.applied, ==, save_applied);
    g_assert_cmpstr(radios_data_document_id(first.document), ==, radios_json_string(json_node_get_object(first_record), "_id", ""));
    g_clear_pointer(&first_record, json_node_free); first_record = document_read("channel", first_name);
    g_assert_cmpint(radios_json_int(json_node_get_object(first_record), "_rev", 0), ==, 2);

    /* Many refresh clicks while both reads are in flight must yield just one
     * additional shared snapshot, and eventually observe external CAS puts. */
    g_unlink(ready); g_assert_true(g_file_set_contents(gate, "channel", -1, NULL));
    baseline = document_read_count(counter); radios_data_refresh(&harness->ctx);
    wait_until(file_exists, ready, "old snapshot held before external commits and explicit refresh");
    JsonNode *changed = document_read_put(first_name, 13, first_record);
    json_node_free(first_record); first_record = changed;
    changed = document_read_put(second_name, 17, second_record);
    json_node_free(second_record); second_record = changed;
    for (guint i = 0; i < 8; ++i) radios_data_refresh(&harness->ctx);
    first.expected = 13; first.revision = 3; second.expected = 17; second.revision = 2;
    g_assert_cmpint(g_unlink(gate), ==, 0); wait.requests = baseline + 2;
    wait_until(document_read_both_ready, &wait, "coalesced refresh reads both external revisions after the held snapshot");
    document_read_quiet(&wait);
    g_test_message("Old read preserved save revision/form; eight refreshes coalesced to two shared snapshots for both documents.");
    media_harness_close(harness); json_node_free(first_record); json_node_free(second_record);
    g_unlink(ready); g_unlink(gate); g_free(directory); g_free(gate); g_free(ready); g_free(counter);
}
static void document_identity_save_status(RadioSDocument *document, const gchar *message, gpointer data)
{
    (void)document;
    if (g_str_equal(message, "Kaydedildi")) *(gboolean *)data = TRUE;
}
static gboolean document_identity_saved(gpointer data) { return *(gboolean *)data; }
static void test_document_identity_fence(void)
{
    const gchar *name = "Okuma kimliği kabulü";
    gint timeout = config.request_timeout_seconds; config.request_timeout_seconds = 12;
    MediaHarness *harness = media_harness_open(); config.request_timeout_seconds = timeout;
    JsonNode *record = document_read_put(name, 5, NULL);
    GtkWidget *owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(harness->parent), owner);
    DocumentReadField field = {.entry = gtk_entry_new(), .expected = 5, .revision = 1};
    gtk_box_pack_start(GTK_BOX(owner), field.entry, FALSE, FALSE, 0);
    field.document = radios_data_document_new(&harness->ctx, owner, "channel", name, document_read_apply, &field);
    wait_until(document_read_field_ready, &field, "document identity baseline loaded");
    gint64 settle = g_get_monotonic_time() + 450 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < settle) pump();

    gchar *directory = g_path_get_dirname(config.database_file);
    gchar *gate = g_build_filename(directory, ".acceptance-document-read-hold", NULL);
    gchar *ready = g_build_filename(directory, ".acceptance-document-read-ready", NULL);
    g_unlink(ready); g_assert_true(g_file_set_contents(gate, "channel", -1, NULL));
    radios_data_refresh(&harness->ctx);
    wait_until(file_exists, ready, "old identity response computed and held");
    JsonNode *changed = document_read_put(name, 13, record); json_node_free(record); record = changed;
    guint applied = field.applied;
    RadioSDataClient *old_client = radios_data_client_acquire(&harness->ctx);
    gchar *original_url = config.service_url;
    const gchar *port = strrchr(original_url, ':'); g_assert_nonnull(port);
    /* Both loopback URLs serve the private fixture, but URL is part of the
     * production store/station identity fence. No credentials are rewritten. */
    config.service_url = g_strdup_printf("http://localhost%s", port);
    RadioSRuntime *original_runtime = harness->ctx.runtime;
    RadioSRuntime *connect_runtime = radios_runtime_new(16, 32);
    harness->ctx.runtime = connect_runtime; radios_data_connect(&harness->ctx);
    wait_until(media_connected, harness, "different client identity connected before old response");
    RadioSDataClient *new_client = radios_data_client_acquire(&harness->ctx);
    g_assert_false(radios_data_client_same_identity(old_client, new_client));
    radios_data_client_unref(old_client); radios_data_client_unref(new_client);
    harness->ctx.runtime = original_runtime; radios_runtime_free(connect_runtime);
    field.expected = 13; field.revision = 2; g_assert_cmpint(g_unlink(gate), ==, 0);
    wait_until(document_read_field_ready, &field, "new identity obtains its own fresh CAS baseline");
    /* An old snapshot must never reach apply, even transiently. Otherwise it
     * could be pinned to the current client's station by the policy UI. */
    g_assert_cmpuint(field.applied, ==, applied + 1);
    g_assert_cmpstr(radios_data_document_id(field.document), ==,
                    radios_json_string(json_node_get_object(record), "_id", ""));
    /* Queue an automatic write behind another held read. Reconnect before
     * its real ACK arrives: consume the committed revision without applying
     * the previous client's form under the current identity. */
    g_unlink(ready); g_assert_true(g_file_set_contents(gate, "channel", -1, NULL));
    radios_data_refresh(&harness->ctx);
    wait_until(file_exists, ready, "read held before old identity automatic write");
    gboolean saved = FALSE;
    radios_data_document_set_status_callback(field.document, document_identity_save_status, &saved);
    JsonNode *saved_config = document_read_config(17);
    radios_data_document_dirty(field.document, TRUE);
    g_assert_true(radios_data_document_autosave(field.document, saved_config)); json_node_free(saved_config);
    applied = field.applied;
    g_free(config.service_url); config.service_url = original_url;
    connect_runtime = radios_runtime_new(16, 32); harness->ctx.runtime = connect_runtime;
    radios_data_connect(&harness->ctx);
    wait_until(media_connected, harness, "original identity restored before pending write ACK");
    harness->ctx.runtime = original_runtime; radios_runtime_free(connect_runtime);
    g_assert_cmpint(g_unlink(gate), ==, 0);
    wait_until(document_identity_saved, &saved, "previous client write committed and ACK consumed");
    g_assert_cmpint(radios_data_document_revision(field.document), ==, 3);
    g_assert_cmpuint(field.applied, ==, applied);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field.entry)), ==, "13");
    field.expected = 17; field.revision = 3; radios_data_refresh(&harness->ctx);
    wait_until(document_read_field_ready, &field, "current identity rereads committed configuration");
    g_test_message("Old-client read and save ACK did not apply a foreign baseline; current client reread committed revision 3.");
    media_harness_close(harness); json_node_free(record);
    g_unlink(ready); g_unlink(gate); g_free(directory); g_free(gate); g_free(ready);
}
static void media_result_clear(MediaResult *output)
{ g_clear_pointer(&output->result, json_node_free); g_clear_error(&output->error); }
static void write_u16(guchar *bytes, guint16 value) { value = GUINT16_TO_LE(value); memcpy(bytes, &value, sizeof value); }
static void write_u32(guchar *bytes, guint32 value) { value = GUINT32_TO_LE(value); memcpy(bytes, &value, sizeof value); }
static gchar *media_fixture_audio(const gchar *name)
{
    const guint frames = 16000, bytes = frames * 2; guchar *wav = g_malloc0(44 + bytes);
    memcpy(wav, "RIFF", 4); write_u32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8); write_u32(wav + 16, 16);
    write_u16(wav + 20, 1); write_u16(wav + 22, 1); write_u32(wav + 24, 8000); write_u32(wav + 28, 16000);
    write_u16(wav + 32, 2); write_u16(wav + 34, 16); memcpy(wav + 36, "data", 4); write_u32(wav + 40, bytes);
    for (guint i = 0; i < frames; ++i) write_u16(wav + 44 + i * 2, (guint16)(i % 8 < 4 ? 16384 : -16384));
    gchar *path = g_build_filename(config.project_root, name, NULL); g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL));
    g_free(wav); return path;
}
static JsonNode *media_read(const gchar *id)
{
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "media_id", id);
    JsonNode *result = request("/v1/workflows/desktop-media/get", body); json_node_free(body); return result;
}
static guint media_count(void)
{
    JsonNode *body = body_new(), *result = request("/v1/workflows/desktop-media/list", body);
    guint total = (guint)radios_json_int(json_node_get_object(result), "total", -1); json_node_free(body); json_node_free(result); return total;
}
static guint import_files(void)
{
    GDir *directory = g_dir_open(config.import_root, 0, NULL); g_assert_nonnull(directory); guint count = 0;
    while (g_dir_read_name(directory)) ++count;
    g_dir_close(directory); return count;
}
static gboolean media_tags_ready(gpointer value)
{ GtkWidget *button = find(value, "media-tags-save"); return button && gtk_widget_is_sensitive(button); }
static gboolean media_inline_ready(gpointer value)
{
    GtkWidget *panel = find(value, "media-marker-panel");
    return panel && radios_marker_editor_detail(panel) != NULL && media_tags_ready(value);
}
static gboolean media_markers_saved(gpointer value)
{
    JsonObject *detail = radios_marker_editor_detail(value);
    JsonNode *markers = detail ? json_object_get_member(detail, "markers") : NULL;
    return !radios_marker_editor_dirty(value) && markers && JSON_NODE_HOLDS_OBJECT(markers) &&
        radios_json_int(json_node_get_object(markers), "_rev", 0) > 0;
}
static gboolean dialog_opened(gpointer name) { return dialog_find(name) != NULL; }
static void test_media_native_persistence(void)
{
    MediaHarness *harness = media_harness_open(); gchar *path = media_fixture_audio("gerçek-ses.wav");
    guint before = media_count(), staged = import_files(); MediaResult output = {0};
    GCancellable *handle = radios_media_import_async(&harness->ctx, harness->parent, path, "music", "Native Medya", media_callback, &output, media_destroy);
    g_assert_nonnull(handle); wait_until(media_finished, &output, "real PCM staged, analyzed and atomically imported");
    g_assert_no_error(output.error); g_assert_nonnull(output.result); g_assert_cmpuint(output.callbacks, ==, 1); g_assert_cmpuint(output.destroyed, ==, 1);
    g_assert_cmpuint(media_count(), ==, before + 1); g_assert_cmpuint(import_files(), ==, staged);
    JsonObject *snapshot = json_node_get_object(output.result), *analysis = json_object_get_object_member(snapshot, "analysis");
    JsonObject *version = json_object_get_object_member(snapshot, "version");
    gchar *id = g_strdup(radios_json_string(json_object_get_object_member(snapshot, "media"), "_id", ""));
    gchar *source = NULL; gsize size = 0; g_assert_true(g_file_get_contents(path, &source, &size, NULL));
    gchar *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *)source, size); g_free(source);
    g_assert_cmpstr(radios_json_string(analysis, "sha256", ""), ==, sha); g_free(sha);
    g_assert_cmpint(radios_json_int(analysis, "duration_ms", 0), ==, 2000); g_assert_cmpint(radios_json_int(analysis, "sample_rate", 0), ==, 8000);
    g_assert_cmpfloat_with_epsilon(json_object_get_double_member(analysis, "peak"), 0.5, 0.0001);
    JsonArray *waveform = json_object_get_array_member(analysis, "waveform"); g_assert_cmpuint(json_array_get_length(waveform), >, 0);
    g_assert_cmpuint(json_array_get_length(waveform), <=, 4096); g_assert_true(json_object_get_boolean_member(version, "verified"));
    g_assert_cmpstr(radios_json_string(version, "status", ""), ==, "draft");
    const gchar *managed = radios_json_string(version, "managed_path", ""); g_assert_true(g_file_test(managed, G_FILE_TEST_IS_REGULAR));
    g_assert_cmpint(g_unlink(path), ==, 0); /* Managed bytes survive removal of the original source. */
    radios_media_library_open_selected(&harness->ctx, id); GtkWidget *dialog = dialog_find("media-library-dialog"); g_assert_nonnull(dialog);
    wait_until(media_tags_ready, dialog, "selected full waveform and editable metadata");
    set(dialog, "media-title", "Kalıcı Medya Başlığı"); set(dialog, "media-artist", "İzmir Sanatçısı");
    set(dialog, "media-album", "Native Albüm"); set(dialog, "media-tags", "Gece, Rock"); click(dialog, "media-tags-save");
    wait_until(media_tags_ready, dialog, "native metadata CAS save completes");
    JsonNode *saved_tags = media_read(id);
    g_assert_cmpstr(radios_json_string(json_object_get_object_member(json_node_get_object(saved_tags), "media"), "title", ""), ==, "Kalıcı Medya Başlığı");
    json_node_free(saved_tags);
    wait_until(media_inline_ready, dialog, "fresh selected media marker panel below metadata");
    GtkWidget *editor = find(dialog, "media-marker-panel"); g_assert_nonnull(editor); g_assert_null(dialog_find("queue-waveform-dialog"));
    g_assert_false(gtk_editable_get_editable(GTK_EDITABLE(find(editor, "qwf-duration"))));
    set(editor, "qwf-cue-in", "0.1"); set(editor, "qwf-intro", "0.3"); set(editor, "qwf-out", "1.9");
    set(editor, "qwf-fade-start", "1.6"); set(editor, "qwf-fade-end", "1.9"); set(editor, "qwf-auto-next", "1.8"); set(editor, "qwf-overlap", "0.1");
    click(editor, "qwf-markers-save"); wait_until(media_markers_saved, editor, "native SHA-bound inline cue draft saved");
    GtkWidget *timeline = find(editor, "qwf-timeline"); g_assert_nonnull(g_object_get_data(G_OBJECT(timeline), "radios-waveform-analysis"));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); g_assert_null(dialog_find("media-library-dialog"));
    radios_media_library_open_selected(&harness->ctx, id); dialog = dialog_find("media-library-dialog"); g_assert_nonnull(dialog);
    wait_until(media_inline_ready, dialog, "reopened library reads the saved inline marker draft");
    editor = find(dialog, "media-marker-panel"); g_assert_cmpfloat(g_ascii_strtod(gtk_entry_get_text(GTK_ENTRY(find(editor, "qwf-cue-in"))), NULL), ==, 0.1);
    media_harness_close(harness); service_command("stop"); service_command("start");
    JsonNode *loaded = media_read(id); JsonObject *record = json_node_get_object(loaded), *media = json_object_get_object_member(record, "media");
    g_assert_cmpstr(radios_json_string(media, "title", ""), ==, "Kalıcı Medya Başlığı");
    g_assert_cmpstr(radios_json_string(media, "artist", ""), ==, "İzmir Sanatçısı");
    JsonObject *markers = json_object_get_object_member(record, "markers"); g_assert_nonnull(markers);
    g_assert_cmpint(radios_json_int(markers, "cue_in_ms", 0), ==, 100); g_assert_cmpint(radios_json_int(markers, "out_ms", 0), ==, 1900);
    g_assert_cmpstr(radios_json_string(markers, "status", ""), ==, "draft");
    g_assert_cmpstr(radios_json_string(markers, "media_sha256", ""), ==, radios_json_string(json_object_get_object_member(record, "version"), "sha256", ""));
    Window window = window_open(TRUE); navigate(&window, "broadcast"); radios_media_library_open(radios_window_view_context(window.window));
    dialog = dialog_find("media-library-dialog"); g_assert_nonnull(dialog); pump(); window_close(&window);
    g_assert_null(dialog_find("media-library-dialog"));
    json_node_free(loaded); media_result_clear(&output); g_object_unref(handle); g_free(id); g_free(path);
}
static void test_media_lost_reply_replay(void)
{
    MediaHarness *harness = media_harness_open(); gchar *path = media_fixture_audio("cevap-kesilen.wav"); guint before = media_count();
    gchar *directory = g_path_get_dirname(config.database_file), *gate = g_build_filename(directory, ".acceptance-import-drop", NULL);
    g_assert_true(g_file_set_contents(gate, "drop", -1, NULL)); MediaResult first = {0};
    GCancellable *handle = radios_media_import_async(&harness->ctx, harness->parent, path, "music", "Kaybolan Cevap", media_callback, &first, media_destroy);
    wait_until(media_finished, &first, "real commit followed by dropped HTTP acknowledgement"); g_assert_nonnull(first.error); g_assert_null(first.result);
    g_assert_cmpuint(media_count(), ==, before + 1); GHashTable *retries = g_object_get_data(G_OBJECT(harness->parent), "radios-media-retries");
    g_assert_cmpuint(g_hash_table_size(retries), ==, 1); g_assert_cmpuint(import_files(), ==, 1);
    MediaResult altered = {0};
    GCancellable *rejected = radios_media_import_async(&harness->ctx, harness->parent, path, "jingle", "Değişen Girdi", media_callback, &altered, media_destroy);
    g_assert_null(rejected); g_assert_cmpuint(altered.callbacks, ==, 0); g_assert_cmpuint(altered.destroyed, ==, 1);
    g_assert_cmpuint(g_hash_table_size(retries), ==, 1); g_assert_cmpuint(media_count(), ==, before + 1);
    g_assert_cmpint(g_unlink(path), ==, 0); /* Retry must replay the same snapshot/key without reading this missing source. */
    MediaResult second = {0}; GCancellable *retry = radios_media_import_async(&harness->ctx, harness->parent, path, "music", "Kaybolan Cevap", media_callback, &second, media_destroy);
    wait_until(media_finished, &second, "explicit retry replays durable receipt"); g_assert_no_error(second.error); g_assert_nonnull(second.result);
    g_assert_cmpuint(media_count(), ==, before + 1); g_assert_cmpuint(g_hash_table_size(retries), ==, 0); g_assert_cmpuint(import_files(), ==, 0);
    g_assert_cmpuint(first.destroyed, ==, 1); g_assert_cmpuint(second.destroyed, ==, 1);
    media_harness_close(harness); g_object_unref(handle); g_object_unref(retry); media_result_clear(&first); media_result_clear(&second);
    g_free(directory); g_free(gate); g_free(path);
}
typedef struct { gint started, release; } MediaBlock;
static gchar *media_block(gpointer value, const RadioSCancelToken *cancel, GError **error)
{
    (void)error; MediaBlock *block = value; g_atomic_int_set(&block->started, 1);
    while (!g_atomic_int_get(&block->release) && !radios_cancelled(cancel)) g_usleep(1000);
    return NULL;
}
static gboolean media_block_started(gpointer value) { return g_atomic_int_get(&((MediaBlock *)value)->started) != 0; }
static void test_media_cancel_and_close(void)
{
    MediaHarness *harness = media_harness_open(); gchar *path = media_fixture_audio("iptal.wav"); guint before = media_count();
    MediaBlock block = {0}; GError *error = NULL; g_assert_cmpuint(radios_runtime_submit(harness->ctx.runtime, media_block, &block, NULL, &error), >, 0); g_assert_no_error(error);
    wait_until(media_block_started, &block, "deterministic queued media cancellation"); MediaResult output = {0};
    GCancellable *handle = radios_media_import_async(&harness->ctx, harness->parent, path, "music", "İptal", media_callback, &output, media_destroy);
    MediaResult duplicate = {0};
    GCancellable *rejected = radios_media_import_async(&harness->ctx, harness->parent, path, "music", "İptal", media_callback, &duplicate, media_destroy);
    g_assert_null(rejected); g_assert_cmpuint(duplicate.callbacks, ==, 0); g_assert_cmpuint(duplicate.destroyed, ==, 1);
    g_cancellable_cancel(handle); g_atomic_int_set(&block.release, 1); wait_until(media_finished, &output, "individual queued import cancelled");
    g_assert_nonnull(output.error); g_assert_null(output.result); g_assert_cmpuint(media_count(), ==, before); g_assert_cmpuint(import_files(), ==, 0);
    media_result_clear(&output); g_object_unref(handle);
    block = (MediaBlock){0}; g_assert_cmpuint(radios_runtime_submit(harness->ctx.runtime, media_block, &block, NULL, &error), >, 0); g_assert_no_error(error);
    wait_until(media_block_started, &block, "media job queued before parent shutdown"); MediaResult closing = {0};
    handle = radios_media_import_async(&harness->ctx, harness->parent, path, "music", "Kapanış", media_callback, &closing, media_destroy);
    media_harness_close(harness); g_assert_cmpuint(closing.callbacks, ==, 0); g_assert_cmpuint(closing.destroyed, ==, 1);
    g_assert_cmpuint(media_count(), ==, before); g_assert_cmpuint(import_files(), ==, 0); g_object_unref(handle); g_unlink(path); g_free(path);
}
typedef struct { GtkWidget *chooser; guint count; } ChooserSelection;
static gboolean chooser_selected(gpointer value)
{
    ChooserSelection *selection = value; GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(selection->chooser));
    guint count = g_slist_length(files); g_slist_free_full(files, g_free); return count == selection->count;
}
static gboolean media_import_finished(gpointer dialog)
{ GtkWidget *button = find(dialog, "media-import"); return button && gtk_widget_is_sensitive(button); }
typedef struct { GtkWidget *dialog; guint count; } MediaTable;
static gboolean media_rows(gpointer value)
{
    MediaTable *table = value; GtkWidget *tree = find(table->dialog, "media-table");
    return tree && (guint)gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(tree)), NULL) == table->count;
}
static void test_media_chooser_search(void)
{
    Window window = window_open(TRUE); gchar *valid = media_fixture_audio("chooser-good.wav"), *invalid = g_build_filename(config.project_root, "chooser-broken.wav", NULL);
    g_assert_true(g_file_set_contents(invalid, "not an audio stream", -1, NULL)); guint before = media_count();
    navigate(&window, "broadcast"); radios_media_library_open(radios_window_view_context(window.window)); GtkWidget *dialog = dialog_find("media-library-dialog"); g_assert_nonnull(dialog);
    click(dialog, "media-import"); GtkWidget *chooser = dialog_find("media-import-chooser"); g_assert_nonnull(chooser);
    g_assert_true(gtk_file_chooser_get_select_multiple(GTK_FILE_CHOOSER(chooser))); ChooserSelection selection = {chooser, 1};
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), valid)); wait_until(chooser_selected, &selection, "first file selected in native GTK chooser");
    gtk_file_chooser_select_all(GTK_FILE_CHOOSER(chooser)); selection.count = 2;
    wait_until(chooser_selected, &selection, "two real files selected for sequential import");
    gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_ACCEPT); wait_until(media_import_finished, dialog, "mixed valid and broken files finish without false records");
    g_assert_cmpuint(media_count(), ==, before + 1); g_assert_cmpuint(import_files(), ==, 0);
    GtkWidget *status = find(dialog, "media-import-status"); g_assert_nonnull(status);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(status)), "1 kayıt")); g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(status)), "1 sonuç doğrulanamadı"));
    set(dialog, "media-search", "chooser-good"); g_signal_emit_by_name(find(dialog, "media-search"), "activate");
    MediaTable table = {dialog, 1}; wait_until(media_rows, &table, "native library searches the imported title");
    GtkWidget *tree = find(dialog, "media-table"); GtkTreeIter iter; GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
    g_assert_true(gtk_tree_model_get_iter_first(model, &iter)); gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), &iter);
    wait_until(media_tags_ready, dialog, "chooser-imported record is editable with its real analysis");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "media-title"))), ==, "chooser-good");
    set(dialog, "media-search", "olmayan-müzik-xyz"); g_signal_emit_by_name(find(dialog, "media-search"), "activate"); table.count = 0;
    wait_until(media_rows, &table, "native search returns an actual empty result");
    window_close(&window); g_unlink(valid); g_unlink(invalid); g_free(valid); g_free(invalid);
}
static JsonObject *copy_media_metadata(JsonObject *media)
{
    const gchar *keys[] = {"title", "kind", "artist", "album", "genre", "tags", "year", "comment"}; JsonObject *metadata = json_object_new();
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) json_object_set_member(metadata, keys[i], json_node_copy(json_object_get_member(media, keys[i])));
    return metadata;
}
static void test_media_native_conflicts(void)
{
    MediaHarness *harness = media_harness_open(); gchar *path = media_fixture_audio("çakışma.wav"); MediaResult output = {0};
    GCancellable *handle = radios_media_import_async(&harness->ctx, harness->parent, path, "music", "Çakışma Medyası", media_callback, &output, media_destroy);
    wait_until(media_finished, &output, "conflict fixture imported"); g_assert_no_error(output.error);
    JsonObject *initial = json_object_get_object_member(json_node_get_object(output.result), "media"); gchar *id = g_strdup(radios_json_string(initial, "_id", ""));
    radios_media_library_open_selected(&harness->ctx, id); GtkWidget *dialog = dialog_find("media-library-dialog"); wait_until(media_tags_ready, dialog, "original media CAS base loaded");
    set(dialog, "media-title", "Korunacak Yerel Başlık"); JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *metadata = copy_media_metadata(initial);
    json_object_set_string_member(metadata, "title", "Harici Başlık"); json_object_set_object_member(object, "metadata", metadata);
    json_object_set_string_member(object, "media_id", id); json_object_set_int_member(object, "expected_revision", radios_json_int(initial, "_rev", 0));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonNode *remote = request("/v1/workflows/desktop-media/metadata-save", body); json_node_free(body);
    click(dialog, "media-tags-save"); wait_until(dialog_opened, "media-reload-dialog", "native metadata 409 offers explicit reload");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "media-title"))), ==, "Korunacak Yerel Başlık");
    gtk_dialog_response(GTK_DIALOG(dialog_find("media-reload-dialog")), GTK_RESPONSE_CANCEL);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "media-title"))), ==, "Korunacak Yerel Başlık");
    /* Explicitly accept the earlier remote metadata before editing markers;
     * the local-title preservation assertions above still exercise conflict protection. */
    click(dialog, "media-tags-save"); wait_until(dialog_opened, "media-reload-dialog", "retrying conflicted metadata offers the current version without a reload button");
    gtk_dialog_response(GTK_DIALOG(dialog_find("media-reload-dialog")), GTK_RESPONSE_ACCEPT);
    wait_until(media_inline_ready, dialog, "fresh remote revision is bound to the inline marker panel");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "media-title"))), ==, "Harici Başlık");
    GtkWidget *editor = find(dialog, "media-marker-panel"); g_assert_nonnull(editor); g_assert_null(dialog_find("queue-waveform-dialog"));
    set(editor, "qwf-cue-in", "0.2"); set(editor, "qwf-intro", "0.3"); set(editor, "qwf-out", "1.9");
    set(editor, "qwf-fade-start", "1.6"); set(editor, "qwf-fade-end", "1.9"); set(editor, "qwf-auto-next", "1.8"); set(editor, "qwf-overlap", "0.1");
    body = body_new(); object = json_node_get_object(body); json_object_set_string_member(object, "media_id", id);
    json_object_set_int_member(object, "expected_revision", radios_json_int(json_object_get_object_member(json_node_get_object(remote), "media"), "_rev", 0));
    json_object_set_null_member(object, "expected_marker_revision"); key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonObject *markers = json_object_new(); const gchar *names[] = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms"};
    const gint values[] = {100, 300, 1900, 1600, 1900, 1800, 100};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) json_object_set_int_member(markers, names[i], values[i]);
    json_object_set_boolean_member(markers, "automatic", FALSE); json_object_set_object_member(object, "markers", markers);
    JsonNode *saved = request("/v1/workflows/desktop-media/markers-save", body); json_node_free(body);
    click(editor, "qwf-markers-save"); wait_until(dialog_opened, "media-marker-reload-dialog", "native inline marker 409 preserves local cue input");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(editor, "qwf-cue-in"))), ==, "0.2");
    gtk_dialog_response(GTK_DIALOG(dialog_find("media-marker-reload-dialog")), GTK_RESPONSE_CANCEL);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(editor, "qwf-cue-in"))), ==, "0.2");
    JsonNode *loaded = media_read(id); g_assert_cmpint(radios_json_int(json_object_get_object_member(json_node_get_object(loaded), "markers"), "cue_in_ms", 0), ==, 100);
    media_harness_close(harness);
    json_node_free(loaded); json_node_free(saved); json_node_free(remote); media_result_clear(&output); g_object_unref(handle); g_unlink(path); g_free(path); g_free(id);
}
/* Saved library views: Tür süzgeci, Sütunlar and named /v1/views profiles
 * in the native library window, read back from the private real service. */
static gpointer read_get_work(gpointer data)
{
    Read *read = data;
    read->result = radios_data_client_request_sync(read->client, "GET", read->path, NULL, NULL, &read->error);
    g_atomic_int_set(&read->done, 1); return NULL;
}
static JsonNode *library_view_read(const gchar *name, guint *named, guint *total)
{
    gchar *station = g_uri_escape_string(radios_data_client_station_id(reader), NULL, FALSE);
    gchar *path = g_strconcat("/v1/views?station_id=", station, NULL);
    Read read = {reader, path, NULL, NULL, NULL, 0}; GThread *thread = g_thread_new("native-acceptance-views", read_get_work, &read);
    wait_until(read_finished, &read, "view profile readback"); g_thread_join(thread);
    g_assert_no_error(read.error); g_assert_nonnull(read.result); g_assert_true(JSON_NODE_HOLDS_ARRAY(read.result));
    JsonArray *rows = json_node_get_array(read.result); JsonNode *found = NULL; *named = *total = 0;
    for (guint i = 0; i < json_array_get_length(rows); ++i) {
        ++*total; if (!g_str_equal(radios_json_string(json_array_get_object_element(rows, i), "name", ""), name)) continue;
        ++*named; g_clear_pointer(&found, json_node_free); found = json_node_copy(json_array_get_element(rows, i));
    }
    json_node_free(read.result); g_free(path); g_free(station); return found;
}
static JsonNode *library_view_wait(GtkWidget *dialog, const gchar *name, gint64 revision, guint total)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND; guint named = 0, rows = 0;
    while (g_get_monotonic_time() < deadline) {
        JsonNode *view = library_view_read(name, &named, &rows);
        if (view && named == 1 && rows == total && radios_json_int(json_node_get_object(view), "_rev", 0) == revision) return view;
        g_clear_pointer(&view, json_node_free);
        gint64 next = g_get_monotonic_time() + 100 * 1000; while (g_get_monotonic_time() < next) pump();
    }
    g_error("Library view '%s' did not reach revision %" G_GINT64_FORMAT " (%u named, %u rows): %s", name, revision, named, rows,
            gtk_label_get_text(GTK_LABEL(find(dialog, "media-status")))); return NULL;
}
static gchar *library_view_put(const gchar *name, const gchar *view_kind, const gchar *kind, const gchar *const *columns,
                               const gchar *profile_id, gint64 revision)
{
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *filters = json_object_new(); JsonArray *list = json_array_new();
    for (guint i = 0; columns[i]; ++i) json_array_add_string_element(list, columns[i]);
    if (kind) json_object_set_string_member(filters, "kind", kind);
    json_object_set_string_member(object, "name", name); json_object_set_string_member(object, "view_kind", view_kind);
    json_object_set_array_member(object, "columns", list); json_object_set_object_member(object, "filters", filters);
    if (profile_id) { json_object_set_string_member(object, "profile_id", profile_id); json_object_set_int_member(object, "expected_revision", revision); }
    JsonNode *saved = request("/v1/views", body); json_node_free(body);
    gchar *id = g_strdup(radios_json_string(json_node_get_object(saved), "_id", "")); json_node_free(saved);
    g_assert_true(g_uuid_string_is_valid(id)); return id;
}
typedef struct { GtkWidget *dialog; const gchar *kind; guint count; } MediaKindRows;
static gboolean media_kind_rows(gpointer value)
{
    MediaKindRows *rows = value; GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(find(rows->dialog, "media-table")));
    GtkTreeIter iter; guint count = 0; gboolean more = gtk_tree_model_get_iter_first(model, &iter);
    while (more) {
        gchar *kind = NULL; gtk_tree_model_get(model, &iter, 2, &kind, -1);
        gboolean other = rows->kind && g_strcmp0(kind, rows->kind); g_free(kind);
        if (other) return FALSE;
        ++count; more = gtk_tree_model_iter_next(model, &iter);
    }
    return count == rows->count;
}
static gboolean media_column_shown(GtkWidget *dialog, guint index)
{ return gtk_tree_view_column_get_visible(gtk_tree_view_get_column(GTK_TREE_VIEW(find(dialog, "media-table")), (gint)index)); }
typedef struct { GtkWidget *combo; const gchar *active; guint rows; } MediaViewChoice;
static gboolean media_view_listed(gpointer value)
{
    MediaViewChoice *choice = value; gchar *active = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(choice->combo));
    gboolean listed = (guint)gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(choice->combo)), NULL) == choice->rows &&
        !g_strcmp0(active, choice->active);
    g_free(active); return listed;
}
static void media_view_wait(MediaViewChoice *choice, GtkWidget *dialog, const gchar *description)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!media_view_listed(choice) && g_get_monotonic_time() < deadline) pump();
    if (media_view_listed(choice)) return;
    gchar *active = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(choice->combo));
    g_error("Saved library views not listed (%s): %d rows, active '%s'; status: %s", description,
            gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(choice->combo)), NULL), active ? active : "",
            gtk_label_get_text(GTK_LABEL(find(dialog, "media-status"))));
}
typedef struct { GtkWidget *dialog; const gchar *text; } MediaStatus;
static gboolean media_status_has(gpointer value)
{ MediaStatus *status = value; return strstr(gtk_label_get_text(GTK_LABEL(find(status->dialog, "media-status"))), status->text) != NULL; }
static gboolean media_header_ready(gpointer header)
{
    GdkWindow *window = gtk_button_get_event_window(GTK_BUTTON(header));
    return gtk_widget_get_mapped(header) && window && gdk_window_is_viewable(window);
}
/* A right click on a column header opens the column menu at the pointer.
 * It is dispatched like a real press on the header's input window. */
static GtkWidget *media_header_menu(GtkWidget *tree, guint column)
{
    GtkWidget *header = gtk_tree_view_column_get_button(gtk_tree_view_get_column(GTK_TREE_VIEW(tree), (gint)column));
    g_assert_true(GTK_IS_BUTTON(header)); wait_until(media_header_ready, header, "library table header shown");
    GdkEvent *event = gdk_event_new(GDK_BUTTON_PRESS);
    event->button.window = g_object_ref(gtk_button_get_event_window(GTK_BUTTON(header))); event->button.button = 3;
    event->button.time = GDK_CURRENT_TIME; event->button.x = event->button.y = 4;
    GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(header)));
    gdk_event_set_device(event, pointer); gdk_event_set_source_device(event, pointer);
    gtk_main_do_event(event); gdk_event_free(event);
    GList *menus = gtk_menu_get_for_attach_widget(tree); g_assert_nonnull(menus);
    GtkWidget *menu = menus->data; g_assert_true(gtk_widget_get_mapped(menu)); return menu;
}
/* The media harness has no main window draining its bounded runtime event
 * queue; this longer scenario drains it the way the window does. */
static gboolean media_runtime_drain(gpointer runtime)
{
    RadioSEvent *event;
    while ((event = radios_runtime_pop_event(runtime)) != NULL) radios_event_free(event);
    return G_SOURCE_CONTINUE;
}
static void test_media_library_views(void)
{
    MediaHarness *harness = media_harness_open(); MediaResult music = {0}, jingle = {0};
    guint drain = g_timeout_add(10, media_runtime_drain, harness->ctx.runtime);
    gchar *music_path = media_fixture_audio("görünüm-müzik.wav"), *jingle_path = media_fixture_audio("görünüm-jingle.wav");
    GCancellable *music_handle = radios_media_import_async(&harness->ctx, harness->parent, music_path, "music", "Görünüm Profili Müzik", media_callback, &music, media_destroy);
    wait_until(media_finished, &music, "music record imported for library views"); g_assert_no_error(music.error);
    GCancellable *jingle_handle = radios_media_import_async(&harness->ctx, harness->parent, jingle_path, "jingle", "Görünüm Profili Jingle", media_callback, &jingle, media_destroy);
    wait_until(media_finished, &jingle, "jingle record imported for library views"); g_assert_no_error(jingle.error);
    /* Let the imports' own catalogue refresh pass before the window opens. */
    gint64 settle = g_get_monotonic_time() + 700 * G_TIME_SPAN_MILLISECOND; while (g_get_monotonic_time() < settle) pump();
    radios_media_library_open(&harness->ctx); GtkWidget *dialog = dialog_find("media-library-dialog"); g_assert_nonnull(dialog);
    GtkWidget *tree = find(dialog, "media-table"), *kinds = find(dialog, "media-kind-filter"), *views = find(dialog, "media-view");
    GtkWidget *bar = find(dialog, "media-view-toolbar"), *first_row = gtk_widget_get_parent(find(dialog, "media-search"));
    g_assert_true(GTK_IS_COMBO_BOX(kinds)); g_assert_true(GTK_IS_COMBO_BOX(views)); g_assert_true(GTK_IS_MENU_BUTTON(find(dialog, "media-columns")));
    g_assert_true(gtk_widget_get_parent(kinds) == bar && gtk_widget_get_parent(find(dialog, "media-view-save")) == bar);
    g_assert_true(gtk_widget_get_parent(find(dialog, "media-import")) == first_row && gtk_widget_get_parent(find(dialog, "media-import-rekordbox")) == first_row);
    gint bar_width = 0, row_width = 0; gtk_widget_get_preferred_width(bar, &bar_width, NULL); gtk_widget_get_preferred_width(first_row, &row_width, NULL);
    g_assert_cmpint(bar_width, <=, row_width); /* the second row never widens the window */
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(kinds)), ==, "");
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(kinds)), NULL), ==, 11);
    MediaViewChoice choice = {views, "Varsayılan görünüm", 1}; g_assert_true(media_view_listed(&choice));

    /* Tür süzgeci asks the service again for the chosen kind only. */
    set(dialog, "media-search", "Görünüm Profili"); g_signal_emit_by_name(find(dialog, "media-search"), "activate");
    MediaKindRows rows = {dialog, NULL, 2}; wait_until(media_kind_rows, &rows, "both kinds listed without a kind filter");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(kinds), "jingle"));
    rows = (MediaKindRows){dialog, "jingle", 1}; wait_until(media_kind_rows, &rows, "kind filter lists only the jingle");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(kinds), "music"));
    rows = (MediaKindRows){dialog, "music", 1}; wait_until(media_kind_rows, &rows, "kind filter lists only the music");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(kinds), "jingle"));
    rows = (MediaKindRows){dialog, "jingle", 1}; wait_until(media_kind_rows, &rows, "kind filter returns to the jingle");

    /* Header right click hides Sanatçı; Sütunlar shows the same state. */
    GtkWidget *menu = media_header_menu(tree, 0), *artist_item = find(menu, "media-columns-menu-artist");
    g_assert_true(GTK_IS_CHECK_MENU_ITEM(artist_item)); g_assert_true(gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(artist_item)));
    gtk_menu_shell_deactivate(GTK_MENU_SHELL(menu)); gtk_menu_item_activate(GTK_MENU_ITEM(artist_item));
    g_assert_false(media_column_shown(dialog, 1)); g_assert_true(media_column_shown(dialog, 0) && media_column_shown(dialog, 2) && media_column_shown(dialog, 3));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(dialog, "media-column-artist"))));

    /* Görünümü kaydet stores the visible columns and the kind filter. */
    click(dialog, "media-view-save"); GtkWidget *namer = dialog_find("media-view-dialog"); g_assert_nonnull(namer);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(namer, "media-view-name"))), ==, "");
    g_assert_false(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(namer), GTK_RESPONSE_ACCEPT)));
    set(namer, "media-view-name", "Jingle görünümü");
    g_assert_true(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(namer), GTK_RESPONSE_ACCEPT)));
    gtk_dialog_response(GTK_DIALOG(namer), GTK_RESPONSE_ACCEPT); g_assert_null(dialog_find("media-view-dialog"));
    JsonNode *saved = library_view_wait(dialog, "Jingle görünümü", 1, 1); JsonObject *profile = json_node_get_object(saved);
    g_assert_cmpstr(radios_json_string(profile, "view_kind", ""), ==, "library");
    JsonArray *columns = json_object_get_array_member(profile, "columns"); g_assert_cmpuint(json_array_get_length(columns), ==, 3);
    g_assert_cmpstr(json_array_get_string_element(columns, 0), ==, "title"); g_assert_cmpstr(json_array_get_string_element(columns, 1), ==, "kind");
    g_assert_cmpstr(json_array_get_string_element(columns, 2), ==, "duration_ms");
    JsonObject *filters = json_object_get_object_member(profile, "filters");
    g_assert_cmpuint(json_object_get_size(filters), ==, 1); g_assert_cmpstr(radios_json_string(filters, "kind", ""), ==, "jingle");
    gchar *profile_id = g_strdup(radios_json_string(profile, "_id", ""));
    MediaStatus status = {dialog, "\"Jingle görünümü\" görünümü kaydedildi."}; wait_until(media_status_has, &status, "saved view reported");
    choice = (MediaViewChoice){views, "Jingle görünümü", 2}; media_view_wait(&choice, dialog, "saved view reread, listed and kept selected");

    /* Varsayılan görünüm shows every column and kind; the saved view returns. */
    gtk_combo_box_set_active(GTK_COMBO_BOX(views), 0);
    for (guint i = 0; i < 4; ++i) g_assert_true(media_column_shown(dialog, i));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(dialog, "media-column-artist"))));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(kinds)), ==, "");
    rows = (MediaKindRows){dialog, NULL, 2}; wait_until(media_kind_rows, &rows, "default view lists every kind");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(views), profile_id));
    g_assert_false(media_column_shown(dialog, 1)); g_assert_true(media_column_shown(dialog, 0) && media_column_shown(dialog, 2) && media_column_shown(dialog, 3));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(dialog, "media-column-artist"))));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(kinds)), ==, "jingle");
    rows = (MediaKindRows){dialog, "jingle", 1}; wait_until(media_kind_rows, &rows, "saved view filters the jingle again");

    /* Another session changes the profile and adds views: the stale save
     * reports the conflict and rereads; archive views stay out of the list. */
    const gchar *title_only[] = {"title", NULL}, *title_artist[] = {"title", "artist", NULL};
    g_free(library_view_put("Jingle görünümü", "library", NULL, title_only, profile_id, 1));
    gchar *external_id = library_view_put("Harici görünüm", "library", "music", title_artist, NULL, 0);
    g_free(library_view_put("Arşiv görünümü", "archive", NULL, title_only, NULL, 0));
    click(dialog, "media-view-save"); namer = dialog_find("media-view-dialog"); g_assert_nonnull(namer);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(namer, "media-view-name"))), ==, "Jingle görünümü");
    gtk_dialog_response(GTK_DIALOG(namer), GTK_RESPONSE_ACCEPT);
    status = (MediaStatus){dialog, "başka bir oturumda değişti"}; wait_until(media_status_has, &status, "stale view revision reports the conflict");
    choice = (MediaViewChoice){views, "Jingle görünümü", 3}; media_view_wait(&choice, dialog, "conflict rereads the views");

    /* Saving the same name again updates that profile: revision up, no new record. */
    click(dialog, "media-view-save"); namer = dialog_find("media-view-dialog"); g_assert_nonnull(namer);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(namer, "media-view-name"))), ==, "Jingle görünümü");
    gtk_dialog_response(GTK_DIALOG(namer), GTK_RESPONSE_ACCEPT);
    JsonNode *updated = library_view_wait(dialog, "Jingle görünümü", 3, 3); profile = json_node_get_object(updated);
    g_assert_cmpstr(radios_json_string(profile, "_id", ""), ==, profile_id); g_assert_cmpint(radios_json_int(profile, "config_version", 0), ==, 3);
    columns = json_object_get_array_member(profile, "columns"); g_assert_cmpuint(json_array_get_length(columns), ==, 3);
    g_assert_cmpstr(json_array_get_string_element(columns, 1), ==, "kind");
    g_assert_cmpstr(radios_json_string(json_object_get_object_member(profile, "filters"), "kind", ""), ==, "jingle");
    status = (MediaStatus){dialog, "\"Jingle görünümü\" görünümü kaydedildi."}; wait_until(media_status_has, &status, "update reported");

    /* A view saved by another session applies its own columns and kind. */
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(views), external_id));
    g_assert_true(media_column_shown(dialog, 0) && media_column_shown(dialog, 1)); g_assert_false(media_column_shown(dialog, 2) || media_column_shown(dialog, 3));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(kinds)), ==, "music");
    rows = (MediaKindRows){dialog, "music", 1}; wait_until(media_kind_rows, &rows, "external view filters the music");

    /* A name another view kind uses is refused before any request. */
    click(dialog, "media-view-save"); namer = dialog_find("media-view-dialog"); g_assert_nonnull(namer);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(namer, "media-view-name"))), ==, "Harici görünüm");
    set(namer, "media-view-name", "Arşiv görünümü"); gtk_dialog_response(GTK_DIALOG(namer), GTK_RESPONSE_ACCEPT);
    status = (MediaStatus){dialog, "başka bir görünüm türünde"}; g_assert_true(media_status_has(&status));
    guint named = 0, total = 0; JsonNode *archive = library_view_read("Arşiv görünümü", &named, &total);
    g_assert_nonnull(archive); g_assert_cmpuint(named, ==, 1); g_assert_cmpuint(total, ==, 3);
    g_assert_cmpstr(radios_json_string(json_node_get_object(archive), "view_kind", ""), ==, "archive");
    g_assert_cmpint(radios_json_int(json_node_get_object(archive), "_rev", 0), ==, 1);

    /* At least one column stays visible in Sütunlar and in the header menu. */
    GtkWidget *title = find(dialog, "media-column-title"), *artist = find(dialog, "media-column-artist");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(title), FALSE);
    g_assert_false(media_column_shown(dialog, 0)); g_assert_true(media_column_shown(dialog, 1));
    g_assert_false(gtk_widget_get_sensitive(artist)); g_assert_false(gtk_widget_get_sensitive(artist_item));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(artist), FALSE);
    g_assert_true(media_column_shown(dialog, 1)); g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(artist)));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(title), TRUE); g_assert_true(gtk_widget_get_sensitive(artist));
    g_test_message("Kind filter, header/Sütunlar columns and /v1/views save, update, conflict and reread verified.");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); g_assert_null(dialog_find("media-library-dialog"));
    g_source_remove(drain); media_harness_close(harness);
    json_node_free(archive); json_node_free(updated); json_node_free(saved); g_free(external_id); g_free(profile_id);
    media_result_clear(&music); media_result_clear(&jingle); g_object_unref(music_handle); g_object_unref(jingle_handle);
    g_unlink(music_path); g_unlink(jingle_path); g_free(music_path); g_free(jingle_path);
}

int
main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *project_root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i)
        argv[i] = argv[i + 1];
    --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    GError *error = NULL; g_assert_true(radios_config_load(project_root, NULL, &config, &error)); g_assert_no_error(error); g_free(project_root);
    application = gtk_application_new("org.radios.NativeDataAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-data/playlist-persistence", test_playlist_persistence);
    g_test_add_func("/native-data/conflict-input-retention", test_conflict_input_retention);
    g_test_add_func("/native-data/transport-input-retention", test_transport_input_retention);
    g_test_add_func("/native-data/shutdown-during-connect", test_shutdown_during_connect);
    g_test_add_func("/native-data/all-profiles-persistence", test_profiles_persistence);
    g_test_add_func("/native-data/refresh-edit-revision-race", test_refresh_edit_revision_race);
    g_test_add_func("/native-data/document-read-order", test_document_read_order);
    g_test_add_func("/native-data/document-identity-fence", test_document_identity_fence);
    g_test_add_func("/native-media/persistence-metadata-cue", test_media_native_persistence);
    g_test_add_func("/native-media/lost-reply-replay", test_media_lost_reply_replay);
    g_test_add_func("/native-media/cancel-and-close", test_media_cancel_and_close);
    g_test_add_func("/native-media/chooser-search-broken-file", test_media_chooser_search);
    g_test_add_func("/native-media/metadata-marker-conflicts", test_media_native_conflicts);
    g_test_add_func("/native-media/library-view-profiles", test_media_library_views);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
