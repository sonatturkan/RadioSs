#include "radios/settings_sync.h"
#include "radios/data.h"
#include "radios/audio.h"
#include "radios/audio_ui.h"
#include "radios/header_status.h"
#include "radios/local_ini.h"
#include "radios/media_archive.h"
#include "radios/library_analysis.h"
#include "radios/storage_locations.h"
#include <string.h>

static const gchar *const local_files[] = {"ui-preferences.ini", "audio-panel.ini", "media-archive.ini", "library-analysis.ini", "storage.ini"};

JsonObject *radios_settings_local_export(const gchar *project_root, GError **error)
{
    JsonObject *files = json_object_new();
    for (guint i = 0; i < G_N_ELEMENTS(local_files); ++i) {
        gchar *contents = NULL;
        if (!radios_local_ini_read(project_root, local_files[i], 65536, &contents, error)) { json_object_unref(files); return NULL; }
        if (contents) json_object_set_string_member(files, local_files[i], contents);
        else json_object_set_null_member(files, local_files[i]);
        g_free(contents);
    }
    return files;
}

gboolean radios_settings_local_import(RadioSViewContext *ctx, JsonObject *files, GError **error)
{
    if (!ctx || !ctx->config || !ctx->config->project_root || !files) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Yerel ayar kapsamı eksik."); return FALSE;
    }
    const gchar *root = ctx->config->project_root;
    GList *members = json_object_get_members(files);
    for (GList *item = members; item; item = item->next) {
        gboolean known = FALSE;
        for (guint i = 0; i < G_N_ELEMENTS(local_files); ++i) known |= g_str_equal(item->data, local_files[i]);
        JsonNode *node = json_object_get_member(files, item->data);
        if (!known || (!JSON_NODE_HOLDS_NULL(node) && (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING))) {
            g_list_free(members); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Pakette desteklenmeyen yerel ayar var."); return FALSE;
        }
        if (!JSON_NODE_HOLDS_NULL(node)) {
            const gchar *text = json_node_get_string(node); GKeyFile *ini = g_key_file_new();
            gboolean valid = text && strlen(text) <= 65536 && g_key_file_load_from_data(ini, text, -1, G_KEY_FILE_NONE, error);
            g_key_file_unref(ini);
            if (!valid) { g_list_free(members); if (error && !*error) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Yerel ayar dosyası geçersiz."); return FALSE; }
        }
    }
    JsonObject *before = radios_settings_local_export(root, error);
    if (!before) { g_list_free(members); return FALSE; }
    gboolean ok = TRUE;
    for (GList *item = members; item && ok; item = item->next) {
        JsonNode *node = json_object_get_member(files, item->data);
        ok = JSON_NODE_HOLDS_NULL(node) ? radios_local_ini_remove(root, item->data, error) :
            radios_local_ini_write(root, item->data, json_node_get_string(node), error);
    }
    RadioSUIPreferences ui; RadioSPreferencesAudio audio;
    RadioSMediaArchiveSettings archive; radios_media_archive_settings_init(&archive);
    RadioSStorageSettings storage; radios_storage_settings_init(&storage);
    RadioSLibraryAnalysisSettings analysis;
    if (ok) ok = radios_media_archive_settings_load(root, &archive, error) &&
                 radios_library_analysis_settings_load(root, &analysis, error) &&
                 radios_storage_settings_load(root, &storage, error);
    radios_media_archive_settings_clear(&archive); radios_storage_settings_clear(&storage);
    if (ok) ok = radios_ui_preferences_load(root, &ui, error) && radios_preferences_audio_load(root, &audio, error) &&
                 radios_settings_sync_apply(ctx, &ui, &audio, error);
    if (!ok) {
        for (guint i = 0; i < G_N_ELEMENTS(local_files); ++i) {
            JsonNode *node = json_object_get_member(before, local_files[i]);
            if (JSON_NODE_HOLDS_NULL(node)) radios_local_ini_remove(root, local_files[i], NULL);
            else radios_local_ini_write(root, local_files[i], json_node_get_string(node), NULL);
        }
    }
    json_object_unref(before); g_list_free(members);
    return ok;
}

gboolean radios_settings_local_reset(RadioSViewContext *ctx, GError **error)
{
    JsonObject *defaults = json_object_new();
    for (guint i = 0; i < G_N_ELEMENTS(local_files); ++i) json_object_set_null_member(defaults, local_files[i]);
    gboolean ok = radios_settings_local_import(ctx, defaults, error); json_object_unref(defaults); return ok;
}

#define SYNC_KEY "radios-settings-sync"
#define WINDOW_HOOK_KEY "radios-settings-window-hook"

typedef struct { RadioSSettingsWindowApply apply; gpointer data; } WindowHook;
typedef struct { gchar *name; RadioSUIPreferences ui; } View;
typedef struct {
    RadioSViewContext *ctx;
    RadioSDocument *document;
    GFileMonitor *monitors[2];
    JsonNode *remote;
    guint debounce, timer;
    gboolean applying, disposed;
    GPtrArray *views; /* View, saved order; only in the database document */
    gchar *active_view;
    GSList *watches;
} Sync;
typedef struct { Sync *sync; GtkWidget *owner; RadioSSettingsViewsChanged changed; gpointer data; } ViewWatch;

static void view_free(gpointer data) { View *view = data; g_free(view->name); g_free(view); }
static View *view_find(Sync *s, const gchar *name)
{
    for (guint i = 0; s && name && i < s->views->len; ++i)
        if (g_str_equal(((View *)s->views->pdata[i])->name, name)) return s->views->pdata[i];
    return NULL;
}
/* The settings a view restores; last_profile follows the workstation. */
static gboolean view_shown(const RadioSUIPreferences *view, const RadioSUIPreferences *ui)
{
    return view->text_size == ui->text_size && view->default_profile == ui->default_profile &&
        (view->header_hidden_fields & 7) == (ui->header_hidden_fields & 7) &&
        view->content_scale_percent == ui->content_scale_percent && view->mixer_scale_percent == ui->mixer_scale_percent &&
        view->effector_scale_percent == ui->effector_scale_percent && view->mixer_height_percent == ui->mixer_height_percent &&
        view->effector_height_percent == ui->effector_height_percent;
}
/* A trimmed, bounded, printable name, or NULL with the reason. */
static gchar *view_name(const gchar *text, GError **error)
{
    glong length = text && g_utf8_validate(text, -1, NULL) ? g_utf8_strlen(text, -1) : 0;
    gboolean printable = length > 0;
    for (const gchar *c = text; printable && *c; c = g_utf8_next_char(c)) printable = !g_unichar_iscntrl(g_utf8_get_char(c));
    gchar *name = printable ? g_strstrip(g_strdup(text)) : NULL;
    gboolean visible = FALSE; /* not only (Unicode) space */
    for (const gchar *c = name; c && *c && !visible; c = g_utf8_next_char(c)) visible = !g_unichar_isspace(g_utf8_get_char(c));
    if (!visible || g_utf8_strlen(name, -1) > RADIOS_SETTINGS_VIEW_NAME_MAX || length > 4 * RADIOS_SETTINGS_VIEW_NAME_MAX) {
        g_free(name);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Görünüm adı 1–%d karakter olmalı.", RADIOS_SETTINGS_VIEW_NAME_MAX);
        return NULL;
    }
    return name;
}
static JsonObject *ui_object(const RadioSUIPreferences *ui)
{
    JsonObject *u = json_object_new();
    json_object_set_int_member(u, "text_size", ui->text_size);
    json_object_set_int_member(u, "default_profile", ui->default_profile);
    json_object_set_int_member(u, "header_hidden_fields", ui->header_hidden_fields & 7);
    json_object_set_int_member(u, "content_scale_percent", ui->content_scale_percent);
    json_object_set_int_member(u, "mixer_scale_percent", ui->mixer_scale_percent);
    json_object_set_int_member(u, "effector_scale_percent", ui->effector_scale_percent);
    json_object_set_int_member(u, "mixer_height_percent", ui->mixer_height_percent);
    json_object_set_int_member(u, "effector_height_percent", ui->effector_height_percent);
    return u;
}
static gboolean ui_value(JsonObject *object, const gchar *key, guint low, guint high, guint *value)
{
    if (!json_object_has_member(object, key)) return TRUE;
    JsonNode *node = json_object_get_member(object, key);
    if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) return FALSE;
    gint64 number = json_node_get_int(node);
    if (number < low || number > high) return FALSE;
    *value = (guint)number; return TRUE;
}
/* A saved view's settings; anything out of range rejects the view. */
static gboolean ui_parse(JsonObject *object, RadioSUIPreferences *ui)
{
    radios_ui_preferences_default(ui);
    return object && ui_value(object, "text_size", 0, 2, &ui->text_size) && ui_value(object, "default_profile", 0, 8, &ui->default_profile) &&
        ui_value(object, "header_hidden_fields", 0, 7, &ui->header_hidden_fields) &&
        ui_value(object, "content_scale_percent", 50, 150, &ui->content_scale_percent) &&
        ui_value(object, "mixer_scale_percent", 50, 150, &ui->mixer_scale_percent) &&
        ui_value(object, "effector_scale_percent", 50, 150, &ui->effector_scale_percent) &&
        ui_value(object, "mixer_height_percent", 50, 150, &ui->mixer_height_percent) &&
        ui_value(object, "effector_height_percent", 50, 150, &ui->effector_height_percent);
}
static void views_changed(Sync *s)
{
    GSList *watches = g_slist_copy(s->watches);
    for (GSList *item = watches; item; item = item->next) {
        ViewWatch *watch = item->data;
        if (g_slist_find(s->watches, watch)) watch->changed(watch->data);
    }
    g_slist_free(watches);
}

static JsonNode *config_node(Sync *s, const RadioSUIPreferences *ui, const RadioSPreferencesAudio *audio)
{
    JsonObject *root = json_object_new(), *a = json_object_new();
    json_object_set_int_member(root, "schema_version", 1);
    json_object_set_object_member(root, "ui", ui_object(ui));
    /* Documents without views keep their earlier form. */
    if (s->views->len || s->active_view) {
        JsonArray *views = json_array_new();
        for (guint i = 0; i < s->views->len; ++i) {
            const View *view = s->views->pdata[i]; JsonObject *item = json_object_new();
            json_object_set_string_member(item, "name", view->name);
            json_object_set_object_member(item, "ui", ui_object(&view->ui));
            json_array_add_object_element(views, item);
        }
        json_object_set_array_member(root, "views", views);
        if (s->active_view) json_object_set_string_member(root, "active_view", s->active_view);
        else json_object_set_null_member(root, "active_view");
    }
    json_object_set_int_member(a, "sample_rate", audio->sample_rate);
    json_object_set_int_member(a, "buffer_frames", audio->buffer_frames);
    json_object_set_int_member(a, "click_sound", audio->click_sound);
    json_object_set_int_member(a, "click_volume", audio->click_volume);
    json_object_set_object_member(root, "audio_panel", a);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, root); return node;
}
static void local_read(Sync *s, RadioSUIPreferences *ui, RadioSPreferencesAudio *audio)
{
    const gchar *root = s->ctx->config->project_root;
    radios_ui_preferences_default(ui);
    radios_preferences_audio_default(audio);
    if (root) { radios_ui_preferences_load(root, ui, NULL); radios_preferences_audio_load(root, audio, NULL); }
}
static void push(Sync *s)
{
    if (s->disposed || !s->document || !radios_data_document_ready(s->document)) return;
    RadioSUIPreferences ui; RadioSPreferencesAudio audio; local_read(s, &ui, &audio);
    View *active = view_find(s, s->active_view);
    gboolean cleared = s->active_view && (!active || !view_shown(&active->ui, &ui));
    if (cleared) g_clear_pointer(&s->active_view, g_free);
    JsonNode *local = config_node(s, &ui, &audio);
    if (cleared) views_changed(s);
    if (s->remote && json_node_equal(s->remote, local)) { json_node_free(local); return; }
    radios_data_document_dirty(s->document, TRUE);
    radios_data_document_autosave(s->document, local);
    g_clear_pointer(&s->remote, json_node_free); s->remote = local;
}
static gboolean debounced(gpointer data)
{ Sync *s = data; s->debounce = 0; if (!s->applying) push(s); return G_SOURCE_REMOVE; }
static void file_changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data)
{
    (void)monitor; (void)file; (void)other; Sync *s = data;
    if (s->disposed || s->applying || event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED) return;
    if (s->debounce) g_source_remove(s->debounce);
    s->debounce = g_timeout_add(600, debounced, s);
}
static guint member(JsonObject *object, const gchar *name, guint fallback)
{ return object && json_object_has_member(object, name) ? (guint)json_object_get_int_member(object, name) : fallback; }
static void remote_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; (void)owner; Sync *s = data;
    if (s->disposed || !config) return;
    RadioSUIPreferences ui, current_ui; RadioSPreferencesAudio audio, current_audio;
    local_read(s, &current_ui, &current_audio); ui = current_ui; audio = current_audio;
    JsonObject *u = json_object_has_member(config, "ui") ? json_object_get_object_member(config, "ui") : NULL;
    JsonObject *a = json_object_has_member(config, "audio_panel") ? json_object_get_object_member(config, "audio_panel") : NULL;
    ui.text_size = member(u, "text_size", ui.text_size); ui.default_profile = member(u, "default_profile", ui.default_profile);
    ui.header_hidden_fields = member(u, "header_hidden_fields", ui.header_hidden_fields);
    ui.content_scale_percent = member(u, "content_scale_percent", ui.content_scale_percent);
    ui.mixer_scale_percent = member(u, "mixer_scale_percent", ui.mixer_scale_percent);
    ui.effector_scale_percent = member(u, "effector_scale_percent", ui.effector_scale_percent);
    ui.mixer_height_percent = member(u, "mixer_height_percent", ui.mixer_height_percent);
    ui.effector_height_percent = member(u, "effector_height_percent", ui.effector_height_percent);
    audio.sample_rate = member(a, "sample_rate", audio.sample_rate); audio.buffer_frames = member(a, "buffer_frames", audio.buffer_frames);
    audio.click_sound = member(a, "click_sound", audio.click_sound); audio.click_volume = member(a, "click_volume", audio.click_volume);
    g_ptr_array_set_size(s->views, 0); g_clear_pointer(&s->active_view, g_free);
    JsonNode *views = json_object_get_member(config, "views");
    JsonArray *list = views && JSON_NODE_HOLDS_ARRAY(views) ? json_node_get_array(views) : NULL;
    for (guint i = 0; list && i < json_array_get_length(list) && s->views->len < RADIOS_SETTINGS_VIEWS_MAX; ++i) {
        JsonNode *node = json_array_get_element(list, i);
        JsonObject *item = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        JsonNode *name_node = item ? json_object_get_member(item, "name") : NULL;
        JsonNode *ui_node = item ? json_object_get_member(item, "ui") : NULL;
        const gchar *raw = name_node && JSON_NODE_HOLDS_VALUE(name_node) && json_node_get_value_type(name_node) == G_TYPE_STRING ?
            json_node_get_string(name_node) : NULL;
        gchar *name = raw ? view_name(raw, NULL) : NULL;
        View *view = g_new0(View, 1);
        if (name && g_str_equal(name, raw) && !view_find(s, name) && ui_node && JSON_NODE_HOLDS_OBJECT(ui_node) &&
            ui_parse(json_node_get_object(ui_node), &view->ui)) {
            view->name = name; g_ptr_array_add(s->views, view);
        } else { g_free(name); g_free(view); }
    }
    JsonNode *active = json_object_get_member(config, "active_view");
    if (active && JSON_NODE_HOLDS_VALUE(active) && json_node_get_value_type(active) == G_TYPE_STRING &&
        view_find(s, json_node_get_string(active)))
        s->active_view = g_strdup(json_node_get_string(active));
    g_clear_pointer(&s->remote, json_node_free); s->remote = config_node(s, &ui, &audio);
    gboolean differs = ui.text_size != current_ui.text_size || ui.default_profile != current_ui.default_profile ||
        ui.header_hidden_fields != current_ui.header_hidden_fields || ui.content_scale_percent != current_ui.content_scale_percent ||
        ui.mixer_scale_percent != current_ui.mixer_scale_percent || ui.effector_scale_percent != current_ui.effector_scale_percent ||
        ui.mixer_height_percent != current_ui.mixer_height_percent || ui.effector_height_percent != current_ui.effector_height_percent ||
        audio.sample_rate != current_audio.sample_rate ||
        audio.buffer_frames != current_audio.buffer_frames || audio.click_sound != current_audio.click_sound ||
        audio.click_volume != current_audio.click_volume;
    if (differs) radios_settings_sync_apply(s->ctx, &ui, &audio, NULL);
    views_changed(s);
}
/* No saved document yet: the first ready moment uploads this workstation's files. */
static gboolean first_upload(gpointer data)
{
    Sync *s = data;
    if (s->disposed) return G_SOURCE_REMOVE;
    if (!s->document || !radios_data_document_ready(s->document)) return G_SOURCE_CONTINUE;
    const gchar *id = radios_data_document_id(s->document);
    if (!s->remote && (!id || !*id)) push(s);
    s->timer = 0; views_changed(s); return G_SOURCE_REMOVE;
}
static void watch_owner_gone(gpointer data, GObject *owner)
{
    (void)owner; ViewWatch *watch = data;
    watch->sync->watches = g_slist_remove(watch->sync->watches, watch); g_free(watch);
}
static void sync_free(gpointer data)
{
    Sync *s = data; s->disposed = TRUE;
    for (GSList *item = s->watches; item; item = item->next) {
        ViewWatch *watch = item->data; g_object_weak_unref(G_OBJECT(watch->owner), watch_owner_gone, watch); g_free(watch);
    }
    g_slist_free(s->watches); s->watches = NULL;
    g_ptr_array_unref(s->views); g_free(s->active_view);
    if (s->debounce) g_source_remove(s->debounce);
    if (s->timer) g_source_remove(s->timer);
    for (guint i = 0; i < 2; ++i) if (s->monitors[i]) { g_file_monitor_cancel(s->monitors[i]); g_object_unref(s->monitors[i]); }
    g_clear_pointer(&s->remote, json_node_free); g_free(s);
}
void radios_settings_sync_bind(RadioSViewContext *ctx, GtkWidget *owner)
{
    g_return_if_fail(ctx && GTK_IS_WIDGET(owner));
    if (!ctx->data || !ctx->config || !ctx->config->project_root || g_object_get_data(G_OBJECT(owner), SYNC_KEY)) return;
    Sync *s = g_new0(Sync, 1); s->ctx = ctx; s->views = g_ptr_array_new_with_free_func(view_free);
    g_object_set_data_full(G_OBJECT(owner), SYNC_KEY, s, sync_free);
    /* Replacing the owning entry with a borrowed entry would call sync_free,
     * even when both entries contain the same pointer. */
    if (ctx->parent && GTK_WIDGET(ctx->parent) != owner)
        g_object_set_data(G_OBJECT(ctx->parent), SYNC_KEY, s);
    const gchar *files[] = {"ui-preferences.ini", "audio-panel.ini"};
    for (guint i = 0; i < 2; ++i) {
        gchar *path = g_build_filename(ctx->config->project_root, "config", files[i], NULL);
        GFile *file = g_file_new_for_path(path);
        s->monitors[i] = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
        if (s->monitors[i]) g_signal_connect(s->monitors[i], "changed", G_CALLBACK(file_changed), s);
        g_object_unref(file); g_free(path);
    }
    s->document = radios_data_document_new(ctx, owner, "desktop_preferences", "Masaüstü tercihleri", remote_loaded, s);
    s->timer = g_timeout_add_seconds(2, first_upload, s);
}
void radios_settings_sync_set_window_hook(GtkWindow *window, RadioSSettingsWindowApply apply, gpointer data)
{
    g_return_if_fail(GTK_IS_WINDOW(window));
    WindowHook *hook = g_new0(WindowHook, 1); hook->apply = apply; hook->data = data;
    g_object_set_data_full(G_OBJECT(window), WINDOW_HOOK_KEY, hook, g_free);
}
gboolean radios_settings_sync_apply(RadioSViewContext *ctx, const RadioSUIPreferences *ui,
    const RadioSPreferencesAudio *audio, GError **error)
{
    g_return_val_if_fail(ctx && ctx->config && ui && audio, FALSE);
    const gchar *root = ctx->config->project_root;
    Sync *s = ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), SYNC_KEY) : NULL;
    RadioSUIPreferences current; RadioSPreferencesAudio current_audio;
    if (!root || !radios_ui_preferences_load(root, &current, error) || !radios_preferences_audio_load(root, &current_audio, error))
        return FALSE;
    RadioSUIPreferences next = *ui; next.last_profile = current.last_profile; next.header_hidden_fields &= 7;
    if (s) s->applying = TRUE;
    gboolean saved = radios_ui_preferences_save(root, &next, error) && radios_preferences_audio_save(root, audio, error);
    if (saved) {
        WindowHook *hook = ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), WINDOW_HOOK_KEY) : NULL;
        if (hook && hook->apply) hook->apply(hook->data, &next);
        radios_header_status_apply_hidden(ctx, next.header_hidden_fields);
        if (audio->sample_rate != current_audio.sample_rate || audio->buffer_frames != current_audio.buffer_frames) {
            RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx);
            if (engine) { radios_audio_engine_configure_output(engine, audio->sample_rate, audio->buffer_frames, NULL); radios_audio_engine_unref(engine); }
        }
    }
    if (s) {
        s->applying = FALSE;
        if (saved) push(s);
    }
    return saved;
}

static Sync *views_sync(RadioSViewContext *ctx)
{
    Sync *s = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), SYNC_KEY) : NULL;
    return s && !s->disposed ? s : NULL;
}
gboolean radios_settings_views_available(RadioSViewContext *ctx)
{
    Sync *s = views_sync(ctx);
    return s && s->document && radios_data_document_ready(s->document);
}
static Sync *views_ready(RadioSViewContext *ctx, GError **error)
{
    if (radios_settings_views_available(ctx)) return views_sync(ctx);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
        views_sync(ctx) ? "Görünümler veritabanından henüz yüklenmedi." : "Görünümler için veritabanı bağlantısı yok.");
    return NULL;
}
gchar **radios_settings_views_list(RadioSViewContext *ctx, gchar **active)
{
    Sync *s = views_sync(ctx); GPtrArray *names = g_ptr_array_new();
    for (guint i = 0; s && i < s->views->len; ++i) g_ptr_array_add(names, g_strdup(((View *)s->views->pdata[i])->name));
    g_ptr_array_add(names, NULL);
    if (active) *active = s ? g_strdup(s->active_view) : NULL;
    return (gchar **)g_ptr_array_free(names, FALSE);
}
gboolean radios_settings_views_get(RadioSViewContext *ctx, const gchar *name, RadioSUIPreferences *out)
{
    View *view = view_find(views_sync(ctx), name);
    if (view && out) *out = view->ui;
    return view != NULL;
}
gboolean radios_settings_views_save(RadioSViewContext *ctx, const gchar *name, GError **error)
{
    Sync *s = views_ready(ctx, error);
    gchar *clean = s ? view_name(name, error) : NULL;
    if (!clean) return FALSE;
    RadioSUIPreferences ui;
    if (!radios_ui_preferences_load(ctx->config->project_root, &ui, error)) { g_free(clean); return FALSE; }
    View *view = view_find(s, clean);
    if (!view && s->views->len >= RADIOS_SETTINGS_VIEWS_MAX) {
        g_free(clean);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "En çok %d görünüm kaydedilebilir.", RADIOS_SETTINGS_VIEWS_MAX);
        return FALSE;
    }
    if (!view) { view = g_new0(View, 1); view->name = g_strdup(clean); g_ptr_array_add(s->views, view); }
    view->ui = ui; view->ui.header_hidden_fields &= 7;
    g_free(s->active_view); s->active_view = clean;
    push(s); views_changed(s);
    return TRUE;
}
gboolean radios_settings_views_apply(RadioSViewContext *ctx, const gchar *name, GError **error)
{
    Sync *s = views_ready(ctx, error);
    if (!s) return FALSE;
    View *view = view_find(s, name);
    if (!view) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Görünüm bulunamadı."); return FALSE; }
    RadioSPreferencesAudio audio; RadioSUIPreferences ui = view->ui;
    if (!radios_preferences_audio_load(ctx->config->project_root, &audio, error)) return FALSE;
    gchar *previous = s->active_view; s->active_view = g_strdup(view->name);
    gboolean applied = radios_settings_sync_apply(ctx, &ui, &audio, error);
    if (applied) g_free(previous); else { g_free(s->active_view); s->active_view = previous; }
    views_changed(s);
    return applied;
}
gboolean radios_settings_views_delete(RadioSViewContext *ctx, const gchar *name, GError **error)
{
    Sync *s = views_ready(ctx, error);
    if (!s) return FALSE;
    View *view = view_find(s, name);
    if (!view) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Görünüm bulunamadı."); return FALSE; }
    if (!g_strcmp0(s->active_view, view->name)) g_clear_pointer(&s->active_view, g_free);
    g_ptr_array_remove(s->views, view);
    push(s); views_changed(s);
    return TRUE;
}
gboolean radios_settings_views_watch(RadioSViewContext *ctx, GtkWidget *owner, RadioSSettingsViewsChanged changed, gpointer data)
{
    g_return_val_if_fail(GTK_IS_WIDGET(owner) && changed, FALSE);
    Sync *s = views_sync(ctx);
    if (!s) return FALSE;
    ViewWatch *watch = g_new0(ViewWatch, 1); *watch = (ViewWatch){s, owner, changed, data};
    s->watches = g_slist_prepend(s->watches, watch);
    g_object_weak_ref(G_OBJECT(owner), watch_owner_gone, watch);
    return TRUE;
}
