#include "radios/preferences_storage.h"
#include "radios/local_ini.h"

#include <gio/gio.h>

#define PANEL_FILE "audio-panel.ini"
#define PANEL_LIMIT 8192u
#define SUBJECT "Ses tercihleri: "

static gboolean invalid(GError **error, const gchar *message)
{
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    return FALSE;
}

void radios_preferences_audio_default(RadioSPreferencesAudio *preferences)
{
    g_return_if_fail(preferences != NULL);
    *preferences = (RadioSPreferencesAudio){44100, 256, 0, 1};
}

gboolean radios_preferences_audio_validate(const RadioSPreferencesAudio *preferences,
                                           GError **error)
{
    g_return_val_if_fail(preferences != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (preferences->sample_rate != 44100 && preferences->sample_rate != 48000)
        return invalid(error, "Örnekleme hızı 44100 veya 48000 Hz olmalı.");
    guint frames = preferences->buffer_frames;
    if (frames < 64 || frames > 4096 || (frames & (frames - 1)) != 0)
        return invalid(error, "Tampon 64–4096 arasında ikinin kuvveti olmalı.");
    if (preferences->click_sound > 2 || preferences->click_volume > 2)
        return invalid(error, "Metronom ses ve seviye seçimleri 0–2 arasında olmalı.");
    return TRUE;
}

static gboolean read_integer(GKeyFile *file, const gchar *key, guint *value, GError **error)
{
    GError *field_error = NULL;
    gint64 parsed = g_key_file_get_int64(file, "audio", key, &field_error);
    if (field_error) { g_propagate_error(error, field_error); return FALSE; }
    if (parsed < 0 || (guint64)parsed > G_MAXUINT)
        return invalid(error, "Ses tercih değeri negatif olmayan bir tam sayı olmalı.");
    *value = (guint)parsed;
    return TRUE;
}

gboolean radios_preferences_audio_load(const gchar *project_root,
                                      RadioSPreferencesAudio *out, GError **error)
{
    g_return_val_if_fail(project_root && *project_root, FALSE);
    g_return_val_if_fail(out != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    /* config/audio-panel.ini through the shared workstation-file rules:
     * physical config directory inside the project, no links, bounded
     * NUL-free UTF-8, private atomic replacement. */
    gchar *buffer = NULL;
    if (!radios_local_ini_read(project_root, PANEL_FILE, PANEL_LIMIT, &buffer, error)) {
        g_prefix_error(error, SUBJECT);
        return FALSE;
    }
    RadioSPreferencesAudio proposed;
    radios_preferences_audio_default(&proposed);
    if (!buffer) { *out = proposed; return TRUE; }
    GKeyFile *file = g_key_file_new();
    gboolean success = g_key_file_load_from_data(file, buffer, (gsize)-1, G_KEY_FILE_NONE, error);
    g_free(buffer);
    guint version = 0;
    if (success) success = read_integer(file, "version", &version, error);
    if (success && version != 1) success = invalid(error, "Ses tercihleri version=1 olmalı.");
    const gchar *keys[] = {"sample_rate", "buffer_frames", "click_sound", "click_volume"};
    guint *values[] = {&proposed.sample_rate, &proposed.buffer_frames, &proposed.click_sound, &proposed.click_volume};
    for (guint i = 0; success && i < G_N_ELEMENTS(keys); ++i)
        success = read_integer(file, keys[i], values[i], error);
    if (success) success = radios_preferences_audio_validate(&proposed, error);
    g_key_file_unref(file);
    if (success) *out = proposed;
    return success;
}

gboolean radios_preferences_audio_save(const gchar *project_root,
                                      const RadioSPreferencesAudio *preferences,
                                      GError **error)
{
    g_return_val_if_fail(project_root && *project_root, FALSE);
    g_return_val_if_fail(preferences != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (!radios_preferences_audio_validate(preferences, error)) return FALSE;
    gchar *contents = g_strdup_printf("[audio]\nversion=1\nsample_rate=%u\nbuffer_frames=%u\nclick_sound=%u\nclick_volume=%u\n",
        preferences->sample_rate, preferences->buffer_frames, preferences->click_sound, preferences->click_volume);
    gboolean success = radios_local_ini_write(project_root, PANEL_FILE, contents, error);
    if (!success) g_prefix_error(error, SUBJECT);
    g_free(contents);
    return success;
}
