/* Settings › Görünüm saved views (R3.4): the production section and settings
 * synchronizer with private preference files on a private display and an
 * isolated document transport. No service, user preferences or audio output
 * is opened. */
#include "radios/view_presets_ui.h"
#include "radios/settings_sync.h"
#include "radios/data.h"
#include "radios/audio.h"
#include "radios/audio_ui.h"
#include "radios/header_status.h"
#include <glib/gstdio.h>
#include <string.h>

struct RadioSDocument { RadioSDocumentApply apply; gpointer user; gboolean ready; };
typedef struct {
    gchar *root;
    RadioSConfig config;
    RadioSViewContext ctx;
    RadioSDocument document;
    JsonNode *sent;
    guint saves, hooks;
    RadioSUIPreferences applied;
} Fixture;
static Fixture *active;

RadioSDocument *radios_data_document_new(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *kind, const gchar *name, RadioSDocumentApply apply, gpointer user)
{
    (void)ctx; (void)owner; (void)name; g_assert_cmpstr(kind, ==, "desktop_preferences");
    active->document = (RadioSDocument){apply, user, FALSE};
    return &active->document;
}
gboolean radios_data_document_ready(RadioSDocument *document)
{ g_assert_true(document == &active->document); return document->ready; }
const gchar *radios_data_document_id(RadioSDocument *document)
{ g_assert_true(document == &active->document); return "private-existing-document"; }
void radios_data_document_dirty(RadioSDocument *document, gboolean dirty)
{ g_assert_true(document == &active->document); g_assert_true(dirty); }
gboolean radios_data_document_autosave(RadioSDocument *document, JsonNode *config)
{
    g_assert_true(document == &active->document); g_assert_true(document->ready);
    g_clear_pointer(&active->sent, json_node_free); active->sent = json_node_copy(config); ++active->saves;
    return TRUE;
}
RadioSAudioEngine *radios_audio_ui_engine_acquire(RadioSViewContext *ctx)
{ (void)ctx; g_assert_not_reached(); return NULL; }
gboolean radios_audio_engine_configure_output(RadioSAudioEngine *engine, guint rate, guint frames, GError **error)
{ (void)engine; (void)rate; (void)frames; (void)error; g_assert_not_reached(); return FALSE; }
void radios_audio_engine_unref(RadioSAudioEngine *engine) { (void)engine; g_assert_not_reached(); }
void radios_header_status_apply_hidden(RadioSViewContext *ctx, guint mask) { (void)ctx; (void)mask; }

static void pump(guint ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    do { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); } while (g_get_monotonic_time() < end);
}
static void window_apply(gpointer data, const RadioSUIPreferences *preferences)
{ Fixture *f = data; f->applied = *preferences; ++f->hooks; }
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find(item->data, name);
    g_list_free(children); return found;
}
static GtkWidget *required(GtkWidget *root, const gchar *name)
{ GtkWidget *widget = find(root, name); g_assert_nonnull(widget); return widget; }
static void setup(Fixture *f, gconstpointer data)
{
    (void)data; GError *error = NULL;
    f->root = g_dir_make_tmp("radios-view-presets-XXXXXX", &error); g_assert_no_error(error);
    gchar *config = g_build_filename(f->root, "config", NULL); g_assert_cmpint(g_mkdir(config, 0700), ==, 0); g_free(config);
    RadioSUIPreferences ui = {1, 0, 7, 0, 73, 102, 100, 100, 100};
    RadioSPreferencesAudio audio; radios_preferences_audio_default(&audio);
    g_assert_true(radios_ui_preferences_save(f->root, &ui, &error)); g_assert_no_error(error);
    g_assert_true(radios_preferences_audio_save(f->root, &audio, &error)); g_assert_no_error(error);
    f->config.project_root = f->root; f->ctx.config = &f->config; f->ctx.data = (RadioSData *)f;
    f->ctx.parent = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL)); g_object_ref_sink(f->ctx.parent);
    active = f;
    radios_settings_sync_bind(&f->ctx, GTK_WIDGET(f->ctx.parent));
    radios_settings_sync_set_window_hook(f->ctx.parent, window_apply, f);
}
static void teardown(Fixture *f, gconstpointer data)
{
    (void)data;
    gtk_widget_destroy(GTK_WIDGET(f->ctx.parent)); g_object_unref(f->ctx.parent); pump(20);
    g_clear_pointer(&f->sent, json_node_free);
    const gchar *names[] = {"ui-preferences.ini", "audio-panel.ini"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        gchar *path = g_build_filename(f->root, "config", names[i], NULL); g_assert_cmpint(g_remove(path), ==, 0); g_free(path);
    }
    gchar *config = g_build_filename(f->root, "config", NULL); g_assert_cmpint(g_rmdir(config), ==, 0); g_free(config);
    g_assert_cmpint(g_rmdir(f->root), ==, 0); g_free(f->root); active = NULL;
}
/* The database copy arrives and the document becomes ready. */
static void deliver(Fixture *f, const gchar *json)
{
    JsonParser *parser = json_parser_new(); GError *error = NULL;
    g_assert_true(json_parser_load_from_data(parser, json, -1, &error)); g_assert_no_error(error);
    f->document.ready = TRUE;
    f->document.apply(&f->ctx, GTK_WIDGET(f->ctx.parent), json_node_get_object(json_parser_get_root(parser)), f->document.user);
    g_object_unref(parser);
}
static RadioSUIPreferences local_ui(Fixture *f)
{
    RadioSUIPreferences ui = {0}; GError *error = NULL;
    g_assert_true(radios_ui_preferences_load(f->root, &ui, &error)); g_assert_no_error(error); return ui;
}
/* The user changes a setting in the window: files and database follow. */
static void user_changes(Fixture *f, guint content, guint mixer)
{
    RadioSUIPreferences ui = local_ui(f); RadioSPreferencesAudio audio; GError *error = NULL;
    radios_preferences_audio_default(&audio); ui.content_scale_percent = content; ui.mixer_scale_percent = mixer;
    g_assert_true(radios_settings_sync_apply(&f->ctx, &ui, &audio, &error)); g_assert_no_error(error);
}
static JsonArray *sent_views(Fixture *f)
{
    g_assert_nonnull(f->sent); JsonObject *root = json_node_get_object(f->sent);
    return json_object_has_member(root, "views") ? json_object_get_array_member(root, "views") : NULL;
}
static const gchar *sent_active(Fixture *f)
{
    JsonObject *root = json_node_get_object(f->sent);
    JsonNode *node = json_object_get_member(root, "active_view");
    return node && !JSON_NODE_HOLDS_NULL(node) ? json_node_get_string(node) : NULL;
}
static JsonObject *sent_view(Fixture *f, guint index, const gchar *name)
{
    JsonObject *view = json_array_get_object_element(sent_views(f), index);
    g_assert_cmpstr(json_object_get_string_member(view, "name"), ==, name);
    return json_object_get_object_member(view, "ui");
}
static const gchar *status(GtkWidget *section)
{ return gtk_label_get_text(GTK_LABEL(required(section, "preferences-view-presets-status"))); }

static void save_apply_overwrite_delete(Fixture *f, gconstpointer data)
{
    (void)data; GError *error = NULL;
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL), *section = radios_view_presets_section_new(&f->ctx);
    gtk_container_add(GTK_CONTAINER(window), section); gtk_widget_show_all(window); pump(50);
    GtkWidget *list = required(section, "preferences-view-presets-list"), *name = required(section, "preferences-view-presets-name");
    GtkWidget *save = required(section, "preferences-view-presets-save"), *remove = required(section, "preferences-view-presets-delete");
    /* Until the database copy is loaded nothing can be saved over it. */
    g_assert_false(gtk_widget_get_sensitive(save)); g_assert_cmpstr(status(section), ==, "Görünümler yükleniyor…");
    g_assert_false(radios_settings_views_save(&f->ctx, "Erken", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED); g_clear_error(&error);
    /* An earlier document without views stays valid. */
    deliver(f, "{\"schema_version\":1,\"ui\":{\"text_size\":1,\"default_profile\":0,\"header_hidden_fields\":7,\"content_scale_percent\":73,\"mixer_scale_percent\":102}}");
    pump(20);
    g_assert_true(gtk_widget_get_sensitive(save)); g_assert_false(gtk_widget_get_sensitive(remove));
    g_assert_null(gtk_combo_box_get_active_id(GTK_COMBO_BOX(list)));
    /* Kaydet: the current settings under the typed name, active at once. */
    gtk_entry_set_text(GTK_ENTRY(name), "  Gece yayını  "); gtk_button_clicked(GTK_BUTTON(save)); pump(20);
    g_assert_cmpstr(status(section), ==, "“Gece yayını” kaydedildi.");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(list)), ==, "Gece yayını");
    g_assert_cmpuint(json_array_get_length(sent_views(f)), ==, 1); g_assert_cmpstr(sent_active(f), ==, "Gece yayını");
    JsonObject *night = sent_view(f, 0, "Gece yayını");
    g_assert_cmpint(json_object_get_int_member(night, "content_scale_percent"), ==, 73);
    g_assert_cmpint(json_object_get_int_member(night, "mixer_scale_percent"), ==, 102);
    g_assert_cmpint(json_object_get_int_member(night, "header_hidden_fields"), ==, 7);
    g_assert_true(gtk_widget_get_sensitive(remove));
    /* A changed setting is no longer that view. */
    user_changes(f, 125, 90); pump(20);
    g_assert_null(sent_active(f)); g_assert_null(gtk_combo_box_get_active_id(GTK_COMBO_BOX(list)));
    gtk_entry_set_text(GTK_ENTRY(name), "Gündüz"); gtk_button_clicked(GTK_BUTTON(save)); pump(20);
    g_assert_cmpuint(json_array_get_length(sent_views(f)), ==, 2); g_assert_cmpstr(sent_active(f), ==, "Gündüz");
    g_assert_cmpint(json_object_get_int_member(sent_view(f, 1, "Gündüz"), "content_scale_percent"), ==, 125);
    /* Choosing a view applies it to the files and the window at once. */
    guint hooks = f->hooks;
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(list), "Gece yayını"); pump(20);
    g_assert_cmpuint(f->hooks, ==, hooks + 1); g_assert_cmpuint(f->applied.content_scale_percent, ==, 73);
    g_assert_cmpuint(f->applied.mixer_scale_percent, ==, 102);
    RadioSUIPreferences shown = local_ui(f);
    g_assert_cmpuint(shown.content_scale_percent, ==, 73); g_assert_cmpuint(shown.mixer_scale_percent, ==, 102);
    g_assert_cmpstr(sent_active(f), ==, "Gece yayını"); g_assert_cmpstr(status(section), ==, "“Gece yayını” uygulandı.");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(name)), ==, "Gece yayını");
    /* Kaydet with the chosen view's name overwrites it. */
    user_changes(f, 73, 120); pump(20); g_assert_null(sent_active(f));
    gtk_combo_box_set_active(GTK_COMBO_BOX(list), -1); gtk_entry_set_text(GTK_ENTRY(name), "Gece yayını");
    gtk_button_clicked(GTK_BUTTON(save)); pump(20);
    g_assert_cmpuint(json_array_get_length(sent_views(f)), ==, 2);
    g_assert_cmpint(json_object_get_int_member(sent_view(f, 0, "Gece yayını"), "mixer_scale_percent"), ==, 120);
    g_assert_cmpstr(sent_active(f), ==, "Gece yayını");
    /* Sil removes the chosen view; the remaining one is kept. */
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(list), "Gündüz"); pump(20);
    g_assert_cmpuint(local_ui(f).content_scale_percent, ==, 125);
    gtk_button_clicked(GTK_BUTTON(remove)); pump(20);
    g_assert_cmpstr(status(section), ==, "“Gündüz” silindi.");
    g_assert_cmpuint(json_array_get_length(sent_views(f)), ==, 1); g_assert_null(sent_active(f));
    g_assert_null(gtk_combo_box_get_active_id(GTK_COMBO_BOX(list)));
    g_assert_cmpuint(local_ui(f).content_scale_percent, ==, 125); /* deleting never changes the shown settings */
    /* Invalid names and the limit. */
    gtk_entry_set_text(GTK_ENTRY(name), "   "); gtk_button_clicked(GTK_BUTTON(save)); pump(20);
    g_assert_cmpstr(status(section), ==, "Görünüm adı 1–40 karakter olmalı.");
    gchar *long_name = g_strnfill(41, 'a');
    g_assert_false(radios_settings_views_save(&f->ctx, long_name, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error); g_free(long_name);
    g_assert_false(radios_settings_views_save(&f->ctx, "a\tb", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    for (guint i = 1; i < RADIOS_SETTINGS_VIEWS_MAX; ++i) {
        gchar *label = g_strdup_printf("Görünüm %u", i);
        g_assert_true(radios_settings_views_save(&f->ctx, label, &error)); g_assert_no_error(error); g_free(label);
    }
    g_assert_cmpuint(json_array_get_length(sent_views(f)), ==, RADIOS_SETTINGS_VIEWS_MAX);
    g_assert_false(radios_settings_views_save(&f->ctx, "Fazla", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE); g_clear_error(&error);
    g_assert_true(radios_settings_views_save(&f->ctx, "Görünüm 3", &error)); g_assert_no_error(error); /* overwrite at the limit */
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(list));
    g_assert_cmpint(gtk_tree_model_iter_n_children(model, NULL), ==, RADIOS_SETTINGS_VIEWS_MAX);
    gtk_widget_destroy(window); pump(20);
}
/* Views saved on another workstation arrive with the database copy. */
static void remote_views_and_active(Fixture *f, gconstpointer data)
{
    (void)data;
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL), *section = radios_view_presets_section_new(&f->ctx);
    gtk_container_add(GTK_CONTAINER(window), section); gtk_widget_show_all(window); pump(50);
    GtkWidget *list = required(section, "preferences-view-presets-list");
    deliver(f, "{\"schema_version\":1,\"ui\":{\"text_size\":2,\"default_profile\":3,\"header_hidden_fields\":1,\"content_scale_percent\":110,"
        "\"mixer_scale_percent\":100,\"effector_scale_percent\":100,\"mixer_height_percent\":100,\"effector_height_percent\":100},"
        "\"views\":[{\"name\":\"Stüdyo\",\"ui\":{\"text_size\":2,\"default_profile\":3,\"header_hidden_fields\":1,\"content_scale_percent\":110,"
        "\"mixer_scale_percent\":100,\"effector_scale_percent\":100,\"mixer_height_percent\":100,\"effector_height_percent\":100}},"
        "{\"name\":\"Kompakt\",\"ui\":{\"text_size\":0,\"default_profile\":1,\"header_hidden_fields\":0,\"content_scale_percent\":60}}],"
        "\"active_view\":\"Stüdyo\"}");
    pump(20);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(list)), ==, "Stüdyo");
    g_assert_cmpuint(local_ui(f).content_scale_percent, ==, 110); g_assert_cmpuint(local_ui(f).text_size, ==, 2);
    RadioSUIPreferences compact;
    g_assert_true(radios_settings_views_get(&f->ctx, "Kompakt", &compact));
    g_assert_cmpuint(compact.content_scale_percent, ==, 60); g_assert_cmpuint(compact.mixer_scale_percent, ==, 100);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(list), "Kompakt"); pump(20);
    g_assert_cmpuint(f->applied.content_scale_percent, ==, 60); g_assert_cmpuint(f->applied.text_size, ==, 0);
    g_assert_cmpuint(local_ui(f).default_profile, ==, 1);
    g_assert_cmpuint(json_array_get_length(sent_views(f)), ==, 2); g_assert_cmpstr(sent_active(f), ==, "Kompakt");
    gtk_widget_destroy(window); pump(20);
}
/* Without a database document the section stays inert and says so. */
static void without_database(void)
{
    RadioSConfig config = {0}; RadioSViewContext ctx = {0}; ctx.config = &config;
    ctx.parent = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL)); g_object_ref_sink(ctx.parent);
    GtkWidget *section = radios_view_presets_section_new(&ctx);
    gtk_container_add(GTK_CONTAINER(ctx.parent), section); gtk_widget_show_all(GTK_WIDGET(ctx.parent)); pump(30);
    g_assert_cmpstr(status(section), ==, "Veritabanı bağlantısı yok.");
    g_assert_false(gtk_widget_get_sensitive(required(section, "preferences-view-presets-save")));
    g_assert_false(gtk_widget_get_sensitive(required(section, "preferences-view-presets-list")));
    gchar **names = radios_settings_views_list(&ctx, NULL); g_assert_null(names[0]); g_strfreev(names);
    gtk_widget_destroy(GTK_WIDGET(ctx.parent)); g_object_unref(ctx.parent); pump(20);
}
int main(int argc, char **argv)
{
    gtk_test_init(&argc, &argv, NULL);
    g_test_add("/view-presets-ui/save-apply-overwrite-delete-limits", Fixture, NULL, setup, save_apply_overwrite_delete, teardown);
    g_test_add("/view-presets-ui/remote-views-active-and-apply", Fixture, NULL, setup, remote_views_and_active, teardown);
    g_test_add_func("/view-presets-ui/without-database", without_database);
    return g_test_run();
}
