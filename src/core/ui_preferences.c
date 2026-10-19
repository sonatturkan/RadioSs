#define _XOPEN_SOURCE 700

#include "radios/ui_preferences.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PREFERENCES_FILE "ui-preferences.ini"
#define PREFERENCES_LIMIT 8192

static gboolean
io_error(GError **error, const gchar *operation)
{
    gint saved_errno = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                "GUI tercihleri %s: %s", operation, g_strerror(saved_errno));
    return FALSE;
}

static gboolean
invalid(GError **error, const gchar *message)
{
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    return FALSE;
}

static gboolean
inside(const gchar *root, const gchar *path)
{
    gsize length = strlen(root);
    return g_str_equal(root, path) ||
        (g_str_has_prefix(path, root) &&
         (length == 1 || path[length] == G_DIR_SEPARATOR));
}

static gint
config_directory(const gchar *project_root, GError **error)
{
    gchar *root = realpath(project_root, NULL);
    if (root == NULL) {
        io_error(error, "proje kökü çözülemedi");
        return -1;
    }
    gchar *requested = g_build_filename(root, "config", NULL);
    gchar *directory = realpath(requested, NULL);
    g_free(requested);
    if (directory == NULL) {
        free(root);
        io_error(error, "mevcut config dizini çözülemedi");
        return -1;
    }
    if (!inside(root, directory)) {
        free(root);
        free(directory);
        invalid(error, "GUI tercihleri config bağlantısı RadioS dışına çıkıyor.");
        return -1;
    }
    gint descriptor = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    free(root);
    free(directory);
    if (descriptor < 0)
        io_error(error, "config dizini açılamadı");
    return descriptor;
}

static gboolean
check_destination(gint directory, gboolean *missing, GError **error)
{
    struct stat info;
    *missing = FALSE;
    if (fstatat(directory, PREFERENCES_FILE, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            *missing = TRUE;
            return TRUE;
        }
        return io_error(error, "dosya incelenemedi");
    }
    if (!S_ISREG(info.st_mode))
        return invalid(error, "GUI tercihleri normal dosya olmalı; sembolik bağlantılar kabul edilmez.");
    return TRUE;
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
    gint directory = config_directory(project_root, error);
    if (directory < 0)
        return FALSE;
    gboolean missing;
    if (!check_destination(directory, &missing, error)) {
        close(directory);
        return FALSE;
    }
    RadioSUIPreferences proposed;
    radios_ui_preferences_default(&proposed);
    if (missing) {
        close(directory);
        *out = proposed;
        return TRUE;
    }
    gint descriptor = openat(directory, PREFERENCES_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    close(directory);
    if (descriptor < 0)
        return io_error(error, "dosya açılamadı");
    struct stat info;
    if (fstat(descriptor, &info) != 0) {
        close(descriptor);
        return io_error(error, "açılmış dosya incelenemedi");
    }
    if (!S_ISREG(info.st_mode) || info.st_size > PREFERENCES_LIMIT) {
        close(descriptor);
        return invalid(error, "GUI tercih dosyası normal dosya olmalı ve 8 KiB sınırını aşmamalı.");
    }
    gchar buffer[PREFERENCES_LIMIT + 1];
    gsize used = 0;
    while (used < sizeof buffer) {
        ssize_t amount = read(descriptor, buffer + used, sizeof buffer - used);
        if (amount < 0) {
            if (errno == EINTR)
                continue;
            close(descriptor);
            return io_error(error, "dosya okunamadı");
        }
        if (amount == 0)
            break;
        used += (gsize)amount;
    }
    close(descriptor);
    if (used > PREFERENCES_LIMIT || memchr(buffer, '\0', used) != NULL ||
        !g_utf8_validate(buffer, (gssize)used, NULL))
        return invalid(error, "GUI tercih dosyası geçerli UTF-8 metin olmalı ve 8 KiB sınırını aşmamalı.");
    GKeyFile *key_file = g_key_file_new();
    gboolean success = g_key_file_load_from_data(key_file, buffer, used,
                                                G_KEY_FILE_NONE, error);
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
    gint directory = config_directory(project_root, error);
    if (directory < 0)
        return FALSE;
    gboolean missing;
    if (!check_destination(directory, &missing, error)) {
        close(directory);
        return FALSE;
    }
    gchar *temporary = NULL;
    gint descriptor = -1;
    for (guint attempt = 0; attempt < 20 && descriptor < 0; ++attempt) {
        g_free(temporary);
        temporary = g_strdup_printf(".ui-preferences-%08x-%08x.tmp", g_random_int(), g_random_int());
        descriptor = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor < 0 && errno != EEXIST)
            break;
    }
    if (descriptor < 0) {
        io_error(error, "geçici dosya oluşturulamadı");
        g_free(temporary);
        close(directory);
        return FALSE;
    }
    gchar *contents = g_strdup_printf("[ui]\nversion=1\ntext_size=%u\ndefault_profile=%u\nlast_profile=%u\nheader_hidden_fields=%u\ncontent_scale_percent=%u\nmixer_scale_percent=%u\neffector_scale_percent=%u\nmixer_height_percent=%u\neffector_height_percent=%u\n",
        preferences->text_size, preferences->default_profile, preferences->last_profile,
        preferences->header_hidden_fields, preferences->content_scale_percent,
        preferences->mixer_scale_percent, preferences->effector_scale_percent,
        preferences->mixer_height_percent, preferences->effector_height_percent);
    gsize length = strlen(contents), written = 0;
    gboolean success = TRUE;
    while (written < length) {
        ssize_t amount = write(descriptor, contents + written, length - written);
        if (amount < 0 && errno == EINTR)
            continue;
        if (amount <= 0) {
            success = io_error(error, "geçici dosya yazılamadı");
            break;
        }
        written += (gsize)amount;
    }
    if (success && fsync(descriptor) != 0)
        success = io_error(error, "geçici dosya diske aktarılamadı");
    if (close(descriptor) != 0 && success)
        success = io_error(error, "geçici dosya kapatılamadı");
    if (success && renameat(directory, temporary, directory, PREFERENCES_FILE) != 0)
        success = io_error(error, "dosya atomik değiştirilemedi");
    if (!success)
        unlinkat(directory, temporary, 0);
    /* The replacement is committed at rename; a directory sync is best effort
     * so an already committed write is never reported as an untouched file. */
    if (success)
        (void)fsync(directory);
    close(directory);
    g_free(contents);
    g_free(temporary);
    return success;
}
