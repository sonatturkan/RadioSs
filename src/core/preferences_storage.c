#define _XOPEN_SOURCE 700

#include "radios/preferences_storage.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PANEL_FILE "audio-panel.ini"
#define PANEL_LIMIT 8192u

static gboolean invalid(GError **error, const gchar *message)
{
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    return FALSE;
}

static gboolean io_error(GError **error, const gchar *operation)
{
    gint code = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(code),
                "Ses tercihleri %s: %s", operation, g_strerror(code));
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

static gint config_directory(const gchar *project_root, GError **error)
{
    gchar *root = realpath(project_root, NULL);
    if (!root) {
        io_error(error, "proje kökü çözülemedi");
        return -1;
    }
    gchar *requested = g_build_filename(root, "config", NULL);
    gchar *directory = realpath(requested, NULL);
    g_free(requested);
    if (!directory) {
        free(root);
        io_error(error, "mevcut config dizini çözülemedi");
        return -1;
    }
    gsize length = strlen(root);
    gboolean inside = g_str_equal(root, directory) ||
        (g_str_has_prefix(directory, root) && (length == 1 || directory[length] == G_DIR_SEPARATOR));
    free(root);
    if (!inside) {
        free(directory);
        invalid(error, "Ses tercihleri config bağlantısı RadioS dışına çıkıyor.");
        return -1;
    }
    gint descriptor = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    free(directory);
    if (descriptor < 0) io_error(error, "config dizini açılamadı");
    return descriptor;
}

static gboolean destination_valid(gint directory, gboolean *missing, GError **error)
{
    struct stat info;
    *missing = FALSE;
    if (fstatat(directory, PANEL_FILE, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) { *missing = TRUE; return TRUE; }
        return io_error(error, "dosya incelenemedi");
    }
    if (!S_ISREG(info.st_mode))
        return invalid(error, "Ses tercihleri normal dosya olmalı; dosya bağlantıları kabul edilmez.");
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
    gint directory = config_directory(project_root, error);
    if (directory < 0) return FALSE;
    gboolean missing;
    if (!destination_valid(directory, &missing, error)) { close(directory); return FALSE; }
    RadioSPreferencesAudio proposed;
    radios_preferences_audio_default(&proposed);
    if (missing) { close(directory); *out = proposed; return TRUE; }
    gint descriptor = openat(directory, PANEL_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    close(directory);
    if (descriptor < 0) return io_error(error, "dosya açılamadı");
    struct stat info;
    if (fstat(descriptor, &info) != 0) {
        close(descriptor); return io_error(error, "açılmış dosya incelenemedi");
    }
    if (!S_ISREG(info.st_mode) || info.st_size > PANEL_LIMIT) {
        close(descriptor); return invalid(error, "Ses tercihleri normal dosya olmalı ve 8 KiB sınırını aşmamalı.");
    }
    gchar buffer[PANEL_LIMIT + 1];
    gsize used = 0;
    while (used < sizeof buffer) {
        ssize_t amount = read(descriptor, buffer + used, sizeof buffer - used);
        if (amount < 0) {
            if (errno == EINTR) continue;
            close(descriptor); return io_error(error, "dosya okunamadı");
        }
        if (amount == 0) break;
        used += (gsize)amount;
    }
    close(descriptor);
    if (used > PANEL_LIMIT || memchr(buffer, '\0', used) || !g_utf8_validate(buffer, (gssize)used, NULL))
        return invalid(error, "Ses tercihleri 8 KiB sınırında geçerli UTF-8 metin olmalı.");
    GKeyFile *file = g_key_file_new();
    gboolean success = g_key_file_load_from_data(file, buffer, used, G_KEY_FILE_NONE, error);
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
    gint directory = config_directory(project_root, error);
    if (directory < 0) return FALSE;
    gboolean missing;
    if (!destination_valid(directory, &missing, error)) { close(directory); return FALSE; }
    gchar *temporary = NULL;
    gint descriptor = -1;
    for (guint attempt = 0; attempt < 20 && descriptor < 0; ++attempt) {
        g_free(temporary);
        temporary = g_strdup_printf(".audio-panel-%08x-%08x.tmp", g_random_int(), g_random_int());
        descriptor = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor < 0 && errno != EEXIST) break;
    }
    if (descriptor < 0) {
        io_error(error, "geçici dosya oluşturulamadı");
        g_free(temporary); close(directory); return FALSE;
    }
    gchar *contents = g_strdup_printf("[audio]\nversion=1\nsample_rate=%u\nbuffer_frames=%u\nclick_sound=%u\nclick_volume=%u\n",
        preferences->sample_rate, preferences->buffer_frames, preferences->click_sound, preferences->click_volume);
    gsize length = strlen(contents), written = 0;
    gboolean success = TRUE;
    while (written < length) {
        ssize_t amount = write(descriptor, contents + written, length - written);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) { success = io_error(error, "geçici dosya yazılamadı"); break; }
        written += (gsize)amount;
    }
    if (success && fsync(descriptor) != 0) success = io_error(error, "geçici dosya diske aktarılamadı");
    if (close(descriptor) != 0 && success) success = io_error(error, "geçici dosya kapatılamadı");
    if (success && renameat(directory, temporary, directory, PANEL_FILE) != 0)
        success = io_error(error, "dosya atomik değiştirilemedi");
    if (!success) unlinkat(directory, temporary, 0);
    /* Rename commits the replacement; a later directory sync cannot turn the
     * already committed write into a reported no-change failure. */
    if (success) (void)fsync(directory);
    close(directory); g_free(contents); g_free(temporary);
    return success;
}
