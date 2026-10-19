/* Production settings synchronizer with real private files and an isolated
 * document transport. No service, user preferences or audio output is opened. */
#include "radios/settings_sync.h"
#include "radios/data.h"
#include "radios/audio.h"
#include "radios/audio_ui.h"
#include "radios/header_status.h"
#include "radios/local_ini.h"
#include <glib/gstdio.h>
#include <string.h>

struct RadioSDocument {
    RadioSDocumentApply apply;
    gpointer user;
    RadioSViewContext *ctx;
    GtkWidget *owner;
};
typedef struct {
    gchar *root;
    RadioSConfig config;
    RadioSViewContext ctx;
    RadioSDocument document;
    JsonNode *sent;
    guint saves, dirty, hooks, hidden;
    RadioSUIPreferences applied;
} Fixture;
static Fixture *active;

RadioSDocument *radios_data_document_new(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *kind, const gchar *name, RadioSDocumentApply apply, gpointer user)
{
    g_assert_cmpstr(kind, ==, "desktop_preferences");
    g_assert_nonnull(name);
    active->document = (RadioSDocument){apply, user, ctx, owner};
    return &active->document;
}
gboolean radios_data_document_ready(RadioSDocument *document)
{ g_assert_true(document == &active->document); return TRUE; }
const gchar *radios_data_document_id(RadioSDocument *document)
{ g_assert_true(document == &active->document); return "private-existing-document"; }
void radios_data_document_dirty(RadioSDocument *document, gboolean dirty)
{ g_assert_true(document == &active->document); g_assert_true(dirty); ++active->dirty; }
gboolean radios_data_document_autosave(RadioSDocument *document, JsonNode *config)
{
    g_assert_true(document == &active->document);
    g_clear_pointer(&active->sent, json_node_free);
    active->sent = json_node_copy(config);
    ++active->saves;
    return TRUE;
}
RadioSAudioEngine *radios_audio_ui_engine_acquire(RadioSViewContext *ctx)
{ (void)ctx; g_assert_not_reached(); return NULL; }
gboolean radios_audio_engine_configure_output(RadioSAudioEngine *engine,
    guint rate, guint frames, GError **error)
{ (void)engine; (void)rate; (void)frames; (void)error; g_assert_not_reached(); return FALSE; }
void radios_audio_engine_unref(RadioSAudioEngine *engine)
{ (void)engine; g_assert_not_reached(); }
void radios_header_status_apply_hidden(RadioSViewContext *ctx, guint mask)
{ g_assert_true(ctx == &active->ctx); active->hidden = mask; }

static void window_apply(gpointer data, const RadioSUIPreferences *preferences)
{
    Fixture *fixture = data;
    fixture->applied = *preferences;
    ++fixture->hooks;
}
static void setup(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GError *error = NULL;
    fixture->root = g_dir_make_tmp("radios-settings-scale-XXXXXX", &error);
    g_assert_no_error(error);
    gchar *config = g_build_filename(fixture->root, "config", NULL);
    g_assert_cmpint(g_mkdir(config, 0700), ==, 0);
    g_free(config);
    RadioSUIPreferences ui = {2, 8, 6, 5, 125, 110, 90, 105, 95};
    RadioSPreferencesAudio audio;
    radios_preferences_audio_default(&audio);
    g_assert_true(radios_ui_preferences_save(fixture->root, &ui, &error));
    g_assert_no_error(error);
    g_assert_true(radios_preferences_audio_save(fixture->root, &audio, &error));
    g_assert_no_error(error);
    fixture->config.project_root = fixture->root;
    fixture->ctx.config = &fixture->config;
    fixture->ctx.data = (RadioSData *)fixture;
    fixture->ctx.parent = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
    g_object_ref_sink(fixture->ctx.parent);
    active = fixture;
    radios_settings_sync_bind(&fixture->ctx, GTK_WIDGET(fixture->ctx.parent));
    radios_settings_sync_set_window_hook(fixture->ctx.parent, window_apply, fixture);
}
static void teardown(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gtk_widget_destroy(GTK_WIDGET(fixture->ctx.parent));
    g_object_unref(fixture->ctx.parent);
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_clear_pointer(&fixture->sent, json_node_free);
    const gchar *names[] = {"ui-preferences.ini", "audio-panel.ini"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        gchar *path = g_build_filename(fixture->root, "config", names[i], NULL);
        g_assert_cmpint(g_remove(path), ==, 0);
        g_free(path);
    }
    gchar *config = g_build_filename(fixture->root, "config", NULL);
    g_assert_cmpint(g_rmdir(config), ==, 0);
    g_free(config);
    g_assert_cmpint(g_rmdir(fixture->root), ==, 0);
    g_free(fixture->root);
    active = NULL;
}
static void deliver(Fixture *fixture, const gchar *json)
{
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
    g_assert_no_error(error);
    fixture->document.apply(&fixture->ctx, GTK_WIDGET(fixture->ctx.parent),
        json_node_get_object(json_parser_get_root(parser)), fixture->document.user);
    g_object_unref(parser);
}
static RadioSUIPreferences local_ui(Fixture *fixture)
{
    RadioSUIPreferences ui = {0};
    GError *error = NULL;
    g_assert_true(radios_ui_preferences_load(fixture->root, &ui, &error));
    g_assert_no_error(error);
    return ui;
}
static void remote_scale_and_legacy(Fixture *fixture, gconstpointer data)
{
    (void)data;
    deliver(fixture, "{\"schema_version\":1,\"ui\":{\"text_size\":0,\"default_profile\":3,\"header_hidden_fields\":2,\"content_scale_percent\":85,\"mixer_scale_percent\":75,\"effector_scale_percent\":140,\"mixer_height_percent\":65,\"effector_height_percent\":135}}");
    RadioSUIPreferences saved = local_ui(fixture);
    g_assert_cmpuint(saved.content_scale_percent, ==, 85);
    g_assert_cmpuint(saved.mixer_scale_percent, ==, 75);
    g_assert_cmpuint(saved.effector_scale_percent, ==, 140);
    g_assert_cmpuint(saved.mixer_height_percent, ==, 65);
    g_assert_cmpuint(saved.effector_height_percent, ==, 135);
    g_assert_cmpuint(saved.text_size, ==, 0);
    g_assert_cmpuint(saved.default_profile, ==, 3);
    g_assert_cmpuint(saved.last_profile, ==, 6);
    g_assert_cmpuint(saved.header_hidden_fields, ==, 2);
    g_assert_cmpuint(fixture->hooks, ==, 1);
    g_assert_cmpuint(fixture->applied.content_scale_percent, ==, 85);
    g_assert_cmpuint(fixture->applied.mixer_scale_percent, ==, 75);
    g_assert_cmpuint(fixture->applied.effector_scale_percent, ==, 140);
    g_assert_cmpuint(fixture->applied.mixer_height_percent, ==, 65);
    g_assert_cmpuint(fixture->applied.effector_height_percent, ==, 135);
    g_assert_cmpuint(fixture->hidden, ==, 2);
    g_assert_cmpuint(fixture->saves, ==, 0);
    /* An older workstation changes text/header preferences without resetting
     * a scale that its optional JSON schema did not know about. */
    deliver(fixture, "{\"schema_version\":1,\"ui\":{\"text_size\":1,\"default_profile\":8,\"header_hidden_fields\":7}}");
    saved = local_ui(fixture);
    g_assert_cmpuint(saved.content_scale_percent, ==, 85);
    g_assert_cmpuint(saved.mixer_scale_percent, ==, 75);
    g_assert_cmpuint(saved.effector_scale_percent, ==, 140);
    g_assert_cmpuint(saved.mixer_height_percent, ==, 65);
    g_assert_cmpuint(saved.effector_height_percent, ==, 135);
    g_assert_cmpuint(saved.last_profile, ==, 6);
    g_assert_cmpuint(saved.text_size, ==, 1);
    g_assert_cmpuint(saved.header_hidden_fields, ==, 7);
    g_assert_cmpuint(fixture->hooks, ==, 2);
    g_assert_cmpuint(fixture->applied.content_scale_percent, ==, 85);
    g_assert_cmpuint(fixture->applied.mixer_scale_percent, ==, 75);
    g_assert_cmpuint(fixture->applied.effector_scale_percent, ==, 140);
    g_assert_cmpuint(fixture->applied.mixer_height_percent, ==, 65);
    g_assert_cmpuint(fixture->applied.effector_height_percent, ==, 135);
    g_assert_cmpuint(fixture->saves, ==, 0);
    /* A workstation from before the separate heights sends widths only: the
     * local heights stay as they are (the width fallback is for INI files). */
    deliver(fixture, "{\"schema_version\":1,\"ui\":{\"text_size\":1,\"default_profile\":8,\"header_hidden_fields\":7,\"content_scale_percent\":85,\"mixer_scale_percent\":90,\"effector_scale_percent\":110}}");
    saved = local_ui(fixture);
    g_assert_cmpuint(saved.mixer_scale_percent, ==, 90);
    g_assert_cmpuint(saved.effector_scale_percent, ==, 110);
    g_assert_cmpuint(saved.mixer_height_percent, ==, 65);
    g_assert_cmpuint(saved.effector_height_percent, ==, 135);
    g_assert_cmpuint(fixture->hooks, ==, 3);
    g_assert_cmpuint(fixture->saves, ==, 0);
    /* Replaying an identical remote snapshot does not rewrite files/hooks. */
    deliver(fixture, "{\"schema_version\":1,\"ui\":{\"text_size\":1,\"default_profile\":8,\"header_hidden_fields\":7,\"content_scale_percent\":85,\"mixer_scale_percent\":90,\"effector_scale_percent\":110,\"mixer_height_percent\":65,\"effector_height_percent\":135}}");
    g_assert_cmpuint(fixture->hooks, ==, 3);
    g_assert_cmpuint(fixture->saves, ==, 0);
}
static void remote_panel_scales_only(Fixture *fixture, gconstpointer data)
{
    (void)data;
    deliver(fixture, "{\"schema_version\":1,\"ui\":{\"mixer_scale_percent\":147,\"effector_scale_percent\":53}}");
    RadioSUIPreferences saved = local_ui(fixture);
    g_assert_cmpuint(saved.content_scale_percent, ==, 125);
    g_assert_cmpuint(saved.mixer_scale_percent, ==, 147);
    g_assert_cmpuint(saved.effector_scale_percent, ==, 53);
    g_assert_cmpuint(saved.text_size, ==, 2);
    g_assert_cmpuint(saved.default_profile, ==, 8);
    g_assert_cmpuint(saved.last_profile, ==, 6);
    g_assert_cmpuint(saved.header_hidden_fields, ==, 5);
    g_assert_cmpuint(saved.mixer_height_percent, ==, 105);
    g_assert_cmpuint(saved.effector_height_percent, ==, 95);
    g_assert_cmpuint(fixture->hooks, ==, 1);
    g_assert_cmpuint(fixture->applied.mixer_scale_percent, ==, 147);
    g_assert_cmpuint(fixture->applied.effector_scale_percent, ==, 53);
    g_assert_cmpuint(fixture->applied.mixer_height_percent, ==, 105);
    g_assert_cmpuint(fixture->applied.effector_height_percent, ==, 95);
    g_assert_cmpuint(fixture->saves, ==, 0);
    /* Each optional field preserves the other local scale independently. */
    deliver(fixture, "{\"schema_version\":1,\"ui\":{\"mixer_scale_percent\":80}}");
    saved = local_ui(fixture);
    g_assert_cmpuint(saved.content_scale_percent, ==, 125);
    g_assert_cmpuint(saved.mixer_scale_percent, ==, 80);
    g_assert_cmpuint(saved.effector_scale_percent, ==, 53);
    g_assert_cmpuint(fixture->hooks, ==, 2);
    deliver(fixture, "{\"schema_version\":1,\"ui\":{}}");
    g_assert_cmpuint(fixture->hooks, ==, 2);
    g_assert_cmpuint(fixture->saves, ==, 0);
    deliver(fixture, "{\"schema_version\":1,\"ui\":{\"mixer_scale_percent\":49,\"effector_scale_percent\":151}}");
    saved = local_ui(fixture);
    g_assert_cmpuint(saved.content_scale_percent, ==, 125);
    g_assert_cmpuint(saved.mixer_scale_percent, ==, 80);
    g_assert_cmpuint(saved.effector_scale_percent, ==, 53);
    g_assert_cmpuint(fixture->hooks, ==, 2);
    g_assert_cmpuint(fixture->saves, ==, 0);
    /* Heights travel on their own keys: a remote height changes no width,
     * each height is optional on its own, and an out-of-range height is
     * rejected without touching the files. */
    deliver(fixture, "{\"schema_version\":1,\"ui\":{\"mixer_height_percent\":140,\"effector_height_percent\":60}}");
    saved = local_ui(fixture);
    g_assert_cmpuint(saved.mixer_scale_percent, ==, 80);
    g_assert_cmpuint(saved.effector_scale_percent, ==, 53);
    g_assert_cmpuint(saved.mixer_height_percent, ==, 140);
    g_assert_cmpuint(saved.effector_height_percent, ==, 60);
    g_assert_cmpuint(fixture->hooks, ==, 3);
    g_assert_cmpuint(fixture->applied.mixer_scale_percent, ==, 80);
    g_assert_cmpuint(fixture->applied.mixer_height_percent, ==, 140);
    g_assert_cmpuint(fixture->applied.effector_height_percent, ==, 60);
    deliver(fixture, "{\"schema_version\":1,\"ui\":{\"effector_height_percent\":75}}");
    saved = local_ui(fixture);
    g_assert_cmpuint(saved.mixer_height_percent, ==, 140);
    g_assert_cmpuint(saved.effector_height_percent, ==, 75);
    g_assert_cmpuint(fixture->hooks, ==, 4);
    deliver(fixture, "{\"schema_version\":1,\"ui\":{\"mixer_height_percent\":49,\"effector_height_percent\":151}}");
    saved = local_ui(fixture);
    g_assert_cmpuint(saved.mixer_scale_percent, ==, 80);
    g_assert_cmpuint(saved.effector_scale_percent, ==, 53);
    g_assert_cmpuint(saved.mixer_height_percent, ==, 140);
    g_assert_cmpuint(saved.effector_height_percent, ==, 75);
    g_assert_cmpuint(fixture->hooks, ==, 4);
    g_assert_cmpuint(fixture->saves, ==, 0);
}
static void local_scale_payload_and_invalid(Fixture *fixture, gconstpointer data)
{
    (void)data;
    RadioSUIPreferences ui = local_ui(fixture);
    ui.content_scale_percent = 143;
    ui.mixer_scale_percent = 67;
    ui.effector_scale_percent = 149;
    ui.mixer_height_percent = 58;
    ui.effector_height_percent = 137;
    ui.last_profile = 1; /* The actual local last profile stays authoritative. */
    RadioSPreferencesAudio audio;
    GError *error = NULL;
    g_assert_true(radios_preferences_audio_load(fixture->root, &audio, &error));
    g_assert_no_error(error);
    g_assert_true(radios_settings_sync_apply(&fixture->ctx, &ui, &audio, &error));
    g_assert_no_error(error);
    RadioSUIPreferences saved = local_ui(fixture);
    g_assert_cmpuint(saved.content_scale_percent, ==, 143);
    g_assert_cmpuint(saved.mixer_scale_percent, ==, 67);
    g_assert_cmpuint(saved.effector_scale_percent, ==, 149);
    g_assert_cmpuint(saved.mixer_height_percent, ==, 58);
    g_assert_cmpuint(saved.effector_height_percent, ==, 137);
    g_assert_cmpuint(saved.last_profile, ==, 6);
    g_assert_cmpuint(saved.header_hidden_fields, ==, 5);
    g_assert_cmpuint(fixture->saves, ==, 1);
    g_assert_cmpuint(fixture->dirty, ==, 1);
    JsonObject *root = json_node_get_object(fixture->sent);
    JsonObject *remote_ui = json_object_get_object_member(root, "ui");
    g_assert_cmpint(json_object_get_int_member(root, "schema_version"), ==, 1);
    g_assert_cmpint(json_object_get_int_member(remote_ui, "content_scale_percent"), ==, 143);
    g_assert_cmpint(json_object_get_int_member(remote_ui, "mixer_scale_percent"), ==, 67);
    g_assert_cmpint(json_object_get_int_member(remote_ui, "effector_scale_percent"), ==, 149);
    g_assert_cmpint(json_object_get_int_member(remote_ui, "mixer_height_percent"), ==, 58);
    g_assert_cmpint(json_object_get_int_member(remote_ui, "effector_height_percent"), ==, 137);
    g_assert_cmpint(json_object_get_int_member(remote_ui, "text_size"), ==, 2);
    g_assert_cmpint(json_object_get_int_member(remote_ui, "default_profile"), ==, 8);
    g_assert_cmpint(json_object_get_int_member(remote_ui, "header_hidden_fields"), ==, 5);
    g_assert_false(json_object_has_member(remote_ui, "last_profile"));
    g_assert_cmpuint(fixture->applied.content_scale_percent, ==, 143);
    g_assert_cmpuint(fixture->applied.mixer_scale_percent, ==, 67);
    g_assert_cmpuint(fixture->applied.effector_scale_percent, ==, 149);
    g_assert_cmpuint(fixture->applied.mixer_height_percent, ==, 58);
    g_assert_cmpuint(fixture->applied.effector_height_percent, ==, 137);
    g_assert_cmpuint(fixture->applied.last_profile, ==, 6);
    const guint invalid[] = {0, 49, 151, G_MAXUINT};
    for (guint field = 0; field < 5; ++field) {
        for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
            RadioSUIPreferences rejected = ui;
            guint *scales[] = {&rejected.content_scale_percent, &rejected.mixer_scale_percent, &rejected.effector_scale_percent,
                &rejected.mixer_height_percent, &rejected.effector_height_percent};
            *scales[field] = invalid[i];
            g_assert_false(radios_settings_sync_apply(&fixture->ctx, &rejected, &audio, &error));
            g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
            saved = local_ui(fixture);
            g_assert_cmpuint(saved.content_scale_percent, ==, 143);
            g_assert_cmpuint(saved.mixer_scale_percent, ==, 67);
            g_assert_cmpuint(saved.effector_scale_percent, ==, 149);
            g_assert_cmpuint(saved.mixer_height_percent, ==, 58);
            g_assert_cmpuint(saved.effector_height_percent, ==, 137);
            g_assert_cmpuint(fixture->hooks, ==, 1);
            g_assert_cmpuint(fixture->saves, ==, 1);
        }
    }
}
static void local_package_round_trip_and_factory(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL;
    JsonObject *files = radios_settings_local_export(fixture->root, &error);
    g_assert_no_error(error); g_assert_nonnull(files);
    g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(files, "media-archive.ini")));
    json_object_set_string_member(files, "media-archive.ini", "[archive]\nversion=1\n");
    json_object_set_string_member(files, "library-analysis.ini", "[analysis]\nversion=1\nworkers=3\n");
    json_object_set_string_member(files, "storage.ini", "[storage]\nversion=1\n");
    g_assert_true(radios_settings_local_import(&fixture->ctx, files, &error));
    g_assert_no_error(error);
    JsonObject *saved = radios_settings_local_export(fixture->root, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(json_object_get_string_member(saved, "media-archive.ini"), ==, "[archive]\nversion=1\n");
    json_object_unref(saved);
    json_object_set_string_member(files, "media-archive.ini", "[archive]\nversion=1\nwatch_minutes=45\n");
    json_object_set_string_member(files, "library-analysis.ini", "[analysis]\nversion=1\nworkers=999\n");
    g_assert_false(radios_settings_local_import(&fixture->ctx, files, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    saved = radios_settings_local_export(fixture->root, &error); g_assert_no_error(error);
    g_assert_cmpstr(json_object_get_string_member(saved, "media-archive.ini"), ==, "[archive]\nversion=1\n");
    g_assert_cmpstr(json_object_get_string_member(saved, "library-analysis.ini"), ==, "[analysis]\nversion=1\nworkers=3\n");
    json_object_unref(saved);
    json_object_set_string_member(files, "../outside.ini", "[bad]\nx=1\n");
    g_assert_false(radios_settings_local_import(&fixture->ctx, files, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    json_object_unref(files);
    g_assert_true(radios_settings_local_reset(&fixture->ctx, &error));
    g_assert_no_error(error);
    saved = radios_settings_local_export(fixture->root, &error); g_assert_no_error(error);
    g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(saved, "media-archive.ini")));
    g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(saved, "library-analysis.ini")));
    g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(saved, "storage.ini")));
    RadioSUIPreferences ui; radios_ui_preferences_default(&ui);
    RadioSUIPreferences actual = local_ui(fixture);
    g_assert_cmpuint(actual.content_scale_percent, ==, ui.content_scale_percent);
    json_object_unref(saved);
}

/* R3.4 named views: the database copy carries views and active_view; only
 * well-formed views are kept, the INI file keeps the active settings alone,
 * and documents without views keep their earlier form. */
static void views_round_trip_active_and_legacy(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL;
    deliver(fixture, "{\"schema_version\":1,\"ui\":{\"text_size\":2,\"default_profile\":8,\"header_hidden_fields\":6,"
        "\"content_scale_percent\":125,\"mixer_scale_percent\":110,\"effector_scale_percent\":90,\"mixer_height_percent\":105,"
        "\"effector_height_percent\":95},\"views\":["
        "{\"name\":\"Sabah\",\"ui\":{\"text_size\":1,\"default_profile\":1,\"header_hidden_fields\":0,\"content_scale_percent\":80}},"
        "{\"name\":\" boşluklu \",\"ui\":{\"text_size\":1,\"default_profile\":1,\"header_hidden_fields\":0}},"
        "{\"name\":\"Sabah\",\"ui\":{\"text_size\":0,\"default_profile\":0,\"header_hidden_fields\":0}},"
        "{\"name\":\"Taşan\",\"ui\":{\"text_size\":1,\"default_profile\":1,\"header_hidden_fields\":0,\"content_scale_percent\":151}},"
        "{\"name\":\"Sekme\\tadı\",\"ui\":{}},{\"name\":\"\",\"ui\":{}},{\"ui\":{}},{\"name\":\"Nesnesiz\",\"ui\":3},7,"
        "{\"name\":\"Akşam\",\"ui\":{\"text_size\":2,\"default_profile\":3,\"header_hidden_fields\":5,\"content_scale_percent\":73,"
        "\"mixer_scale_percent\":102}}],\"active_view\":\"Taşan\"}");
    gchar *active = NULL; gchar **names = radios_settings_views_list(&fixture->ctx, &active);
    g_assert_cmpuint(g_strv_length(names), ==, 2);
    g_assert_cmpstr(names[0], ==, "Sabah"); g_assert_cmpstr(names[1], ==, "Akşam");
    g_assert_null(active); g_strfreev(names);
    RadioSUIPreferences evening;
    g_assert_true(radios_settings_views_get(&fixture->ctx, "Akşam", &evening));
    g_assert_cmpuint(evening.content_scale_percent, ==, 73); g_assert_cmpuint(evening.mixer_scale_percent, ==, 102);
    g_assert_cmpuint(evening.effector_height_percent, ==, 100); g_assert_cmpuint(evening.header_hidden_fields, ==, 5);
    g_assert_false(radios_settings_views_get(&fixture->ctx, "Taşan", NULL));
    guint saves = fixture->saves, hooks = fixture->hooks;
    g_assert_true(radios_settings_views_apply(&fixture->ctx, "Akşam", &error)); g_assert_no_error(error);
    g_assert_cmpuint(fixture->hooks, ==, hooks + 1); g_assert_cmpuint(fixture->saves, ==, saves + 1);
    g_assert_cmpuint(fixture->applied.content_scale_percent, ==, 73); g_assert_cmpuint(fixture->hidden, ==, 5);
    RadioSUIPreferences saved = local_ui(fixture);
    g_assert_cmpuint(saved.content_scale_percent, ==, 73); g_assert_cmpuint(saved.text_size, ==, 2);
    g_assert_cmpuint(saved.default_profile, ==, 3); g_assert_cmpuint(saved.mixer_scale_percent, ==, 102);
    gchar *path = g_build_filename(fixture->root, "config", "ui-preferences.ini", NULL), *contents = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error)); g_assert_no_error(error);
    g_assert_null(strstr(contents, "Akşam")); g_assert_null(strstr(contents, "Sabah")); g_assert_null(strstr(contents, "view"));
    g_free(contents); g_free(path);
    JsonObject *root = json_node_get_object(fixture->sent);
    JsonArray *views = json_object_get_array_member(root, "views");
    g_assert_cmpuint(json_array_get_length(views), ==, 2);
    JsonObject *morning = json_array_get_object_element(views, 0);
    g_assert_cmpstr(json_object_get_string_member(morning, "name"), ==, "Sabah");
    JsonObject *morning_ui = json_object_get_object_member(morning, "ui");
    g_assert_cmpint(json_object_get_int_member(morning_ui, "content_scale_percent"), ==, 80);
    g_assert_cmpint(json_object_get_int_member(morning_ui, "mixer_scale_percent"), ==, 100);
    g_assert_cmpuint(json_object_get_size(morning_ui), ==, 8);
    g_assert_cmpstr(json_object_get_string_member(root, "active_view"), ==, "Akşam");
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(root, "ui"), "content_scale_percent"), ==, 73);
    /* Changing a shown setting clears active_view, the views stay. */
    RadioSUIPreferences changed = saved; RadioSPreferencesAudio audio; radios_preferences_audio_default(&audio);
    changed.content_scale_percent = 88;
    g_assert_true(radios_settings_sync_apply(&fixture->ctx, &changed, &audio, &error)); g_assert_no_error(error);
    root = json_node_get_object(fixture->sent);
    g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(root, "active_view")));
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root, "views")), ==, 2);
    /* Deleting every view returns the document to its earlier form. */
    g_assert_true(radios_settings_views_delete(&fixture->ctx, "Sabah", &error)); g_assert_no_error(error);
    g_assert_true(radios_settings_views_delete(&fixture->ctx, "Akşam", &error)); g_assert_no_error(error);
    g_assert_false(radios_settings_views_delete(&fixture->ctx, "Akşam", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND); g_clear_error(&error);
    root = json_node_get_object(fixture->sent);
    g_assert_false(json_object_has_member(root, "views")); g_assert_false(json_object_has_member(root, "active_view"));
    /* A document from before views: no views, and the payload keeps that form. */
    deliver(fixture, "{\"schema_version\":1,\"ui\":{\"text_size\":1,\"default_profile\":0,\"header_hidden_fields\":0,\"content_scale_percent\":100}}");
    names = radios_settings_views_list(&fixture->ctx, &active); g_assert_null(names[0]); g_assert_null(active); g_strfreev(names);
    changed = local_ui(fixture); changed.mixer_scale_percent = 140;
    g_assert_true(radios_settings_sync_apply(&fixture->ctx, &changed, &audio, &error)); g_assert_no_error(error);
    root = json_node_get_object(fixture->sent);
    g_assert_false(json_object_has_member(root, "views")); g_assert_false(json_object_has_member(root, "active_view"));
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(root, "ui"), "mixer_scale_percent"), ==, 140);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check(&argc, &argv)) return 77;
    g_test_add("/settings-sync/remote-scale-legacy-preservation", Fixture, NULL, setup, remote_scale_and_legacy, teardown);
    g_test_add("/settings-sync/remote-panel-scales-independent", Fixture, NULL, setup, remote_panel_scales_only, teardown);
    g_test_add("/settings-sync/local-scale-payload-invalid-preservation", Fixture, NULL, setup, local_scale_payload_and_invalid, teardown);
    g_test_add("/settings-sync/local-package-round-trip-and-factory", Fixture, NULL, setup, local_package_round_trip_and_factory, teardown);
    g_test_add("/settings-sync/views-round-trip-active-and-legacy", Fixture, NULL, setup, views_round_trip_active_and_legacy, teardown);
    return g_test_run();
}
