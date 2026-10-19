#include "radios/ui_preferences.h"
#include "radios/local_ini.h"

#include <gio/gio.h>

#define PREFERENCES_FILE "ui-preferences.ini"
#define PREFERENCES_LIMIT 8192
#define SUBJECT "GUI tercihleri: "

static gboolean
invalid(GError **error, const gchar *message)
{
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    return FALSE;
}

static gboolean
preferences_valid(const RadioSUIPreferences *preferences, GError **error)
{
    return (preferences->text_size <= 2 && preferences->default_profile <= 8 &&
            preferences->last_profile <= 7 && preferences->header_hidden_fields <= 7 &&
            preferences->content_scale_percent >= 50 && preferences->content_scale_percent <= 150 &&
            preferences->mixer_scale_percent >= 50 && preferences->mixer_scale_percent <= 150 &&
            preferences->effector_scale_percent >= 50 && preferences->effector_scale_percent <= 150 &&
            preferences->mixer_height_percent >= 50 && preferences->mixer_height_percent <= 150 &&
            preferences->effector_height_percent >= 50 && preferences->effector_height_percent <= 150) ||
        invalid(error, "GUI tercihleri aralık dışında: yazı 0–2, başlangıç profili 0–8, son profil 0–7, üst bar gizli alanları 0–7, içerik, mikser ve efektör ölçekleri (genişlik ve yükseklik) %50–150 olmalı.");
}

void
radios_ui_preferences_default(RadioSUIPreferences *out)
{
    g_return_if_fail(out != NULL);
    *out = (RadioSUIPreferences){.text_size = 1, .content_scale_percent = 100,
        .mixer_scale_percent = 100, .effector_scale_percent = 100,
        .mixer_height_percent = 100, .effector_height_percent = 100};
}

gboolean
radios_ui_preferences_load(const gchar *project_root,
                          RadioSUIPreferences *out, GError **error)
{
    g_return_val_if_fail(project_root != NULL && *project_root != '\0', FALSE);
    g_return_val_if_fail(out != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    /* config/ui-preferences.ini through the shared workstation-file rules:
     * physical config directory inside the project, no links, bounded
     * NUL-free UTF-8, private atomic replacement. */
    gchar *buffer = NULL;
    if (!radios_local_ini_read(project_root, PREFERENCES_FILE, PREFERENCES_LIMIT, &buffer, error)) {
        g_prefix_error(error, SUBJECT);
        return FALSE;
    }
    RadioSUIPreferences proposed;
    radios_ui_preferences_default(&proposed);
    if (buffer == NULL) {
        *out = proposed;
        return TRUE;
    }
    GKeyFile *key_file = g_key_file_new();
    gboolean success = g_key_file_load_from_data(key_file, buffer, (gsize)-1,
                                                G_KEY_FILE_NONE, error);
    g_free(buffer);
    if (success) {
        gint version = g_key_file_get_integer(key_file, "ui", "version", error);
        success = error == NULL || *error == NULL;
        if (success && version != 1)
            success = invalid(error, "Desteklenmeyen GUI tercih sürümü; version=1 olmalı.");
        const gchar *keys[] = {"text_size", "default_profile", "last_profile", "header_hidden_fields", "content_scale_percent",
                               "mixer_scale_percent", "effector_scale_percent", "mixer_height_percent", "effector_height_percent"};
        guint *values[] = {&proposed.text_size, &proposed.default_profile, &proposed.last_profile,
                           &proposed.header_hidden_fields, &proposed.content_scale_percent,
                           &proposed.mixer_scale_percent, &proposed.effector_scale_percent,
                           &proposed.mixer_height_percent, &proposed.effector_height_percent};
        for (guint i = 0; i < G_N_ELEMENTS(keys) && success; ++i) {
            if (i >= 3 && !g_key_file_has_key(key_file, "ui", keys[i], NULL))
                continue;
            GError *field_error = NULL;
            gint64 value = g_key_file_get_int64(key_file, "ui", keys[i], &field_error);
            if (field_error != NULL) {
                g_propagate_error(error, field_error);
                success = FALSE;
            } else if (value < 0 || (guint64)value > G_MAXUINT) {
                success = invalid(error, "GUI tercih değeri negatif olmayan bir tam sayı olmalı.");
            } else {
                *values[i] = (guint)value;
            }
        }
        /* Older files have one uniform scale: a height that was never saved follows its width. */
        if (success && !g_key_file_has_key(key_file, "ui", "mixer_height_percent", NULL))
            proposed.mixer_height_percent = proposed.mixer_scale_percent;
        if (success && !g_key_file_has_key(key_file, "ui", "effector_height_percent", NULL))
            proposed.effector_height_percent = proposed.effector_scale_percent;
        if (success)
            success = preferences_valid(&proposed, error);
    }
    g_key_file_unref(key_file);
    if (success)
        *out = proposed;
    return success;
}

gboolean
radios_ui_preferences_save(const gchar *project_root,
                          const RadioSUIPreferences *preferences, GError **error)
{
    g_return_val_if_fail(project_root != NULL && *project_root != '\0', FALSE);
    g_return_val_if_fail(preferences != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (!preferences_valid(preferences, error))
        return FALSE;
    gchar *contents = g_strdup_printf("[ui]\nversion=1\ntext_size=%u\ndefault_profile=%u\nlast_profile=%u\nheader_hidden_fields=%u\ncontent_scale_percent=%u\nmixer_scale_percent=%u\neffector_scale_percent=%u\nmixer_height_percent=%u\neffector_height_percent=%u\n",
        preferences->text_size, preferences->default_profile, preferences->last_profile,
        preferences->header_hidden_fields, preferences->content_scale_percent,
        preferences->mixer_scale_percent, preferences->effector_scale_percent,
        preferences->mixer_height_percent, preferences->effector_height_percent);
    gboolean success = radios_local_ini_write(project_root, PREFERENCES_FILE, contents, error);
    if (!success)
        g_prefix_error(error, SUBJECT);
    g_free(contents);
    return success;
}
