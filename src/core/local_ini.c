#define _XOPEN_SOURCE 700

#include "radios/local_ini.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static gboolean invalid(GError **error, const gchar *message)
{
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    return FALSE;
}

static gboolean io_error(GError **error, const gchar *file_name, const gchar *operation)
{
    gint code = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(code), "%s %s: %s", file_name, operation, g_strerror(code));
    return FALSE;
}

static gboolean name_valid(const gchar *name)
{
    return name && *name && name[0] != '.' && !strchr(name, '/') && strlen(name) < 100;
}

/* Opens <project>/config without following links; the physical directory has
 * to stay inside the physical project root. */
static gint config_directory(const gchar *project_root, const gchar *file_name, GError **error)
{
    gchar *root = realpath(project_root, NULL);
    if (!root) { io_error(error, file_name, "proje kökü çözülemedi"); return -1; }
    gchar *requested = g_build_filename(root, "config", NULL);
    gchar *directory = realpath(requested, NULL);
    g_free(requested);
    if (!directory) { free(root); io_error(error, file_name, "config dizini çözülemedi"); return -1; }
    gsize length = strlen(root);
    gboolean inside = g_str_equal(root, directory) ||
        (g_str_has_prefix(directory, root) && (length == 1 || directory[length] == G_DIR_SEPARATOR));
    free(root);
    if (!inside) { free(directory); invalid(error, "config dizini RadioS dışına çıkıyor."); return -1; }
    gint descriptor = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    free(directory);
    if (descriptor < 0) io_error(error, file_name, "config dizini açılamadı");
    return descriptor;
}

gboolean radios_local_ini_read(const gchar *project_root, const gchar *file_name, gsize limit,
                               gchar **contents, GError **error)
{
    g_return_val_if_fail(project_root && *project_root && contents, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    *contents = NULL;
    if (!name_valid(file_name) || limit == 0 || limit > 1024u * 1024u) return invalid(error, "Yerel ayar dosyası adı veya sınırı geçersiz.");
    gint directory = config_directory(project_root, file_name, error);
    if (directory < 0) return FALSE;
    struct stat info;
    if (fstatat(directory, file_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        gboolean missing = errno == ENOENT;
        if (!missing) io_error(error, file_name, "incelenemedi");
        close(directory);
        return missing;
    }
    if (!S_ISREG(info.st_mode)) { close(directory); return invalid(error, "Yerel ayar dosyası normal dosya olmalı; bağlantılar kabul edilmez."); }
    gint descriptor = openat(directory, file_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    close(directory);
    if (descriptor < 0) return io_error(error, file_name, "açılamadı");
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || (guint64)info.st_size > limit) {
        close(descriptor);
        return invalid(error, "Yerel ayar dosyası normal dosya olmalı ve boyut sınırını aşmamalı.");
    }
    gchar *buffer = g_malloc(limit + 1);
    gsize used = 0;
    while (used < limit + 1) {
        ssize_t amount = read(descriptor, buffer + used, limit + 1 - used);
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) { close(descriptor); g_free(buffer); return io_error(error, file_name, "okunamadı"); }
        if (amount == 0) break;
        used += (gsize)amount;
    }
    close(descriptor);
    if (used > limit || memchr(buffer, '\0', used) || !g_utf8_validate(buffer, (gssize)used, NULL)) {
        g_free(buffer);
        return invalid(error, "Yerel ayar dosyası boyut sınırı içinde geçerli UTF-8 metin olmalı.");
    }
    buffer[used] = '\0';
    *contents = buffer;
    return TRUE;
}

gboolean radios_local_ini_remove(const gchar *project_root, const gchar *file_name, GError **error)
{
    if (!project_root || !name_valid(file_name)) return invalid(error, "Yerel ayar dosyası adı geçersiz.");
    gint directory = config_directory(project_root, file_name, error);
    if (directory < 0) return FALSE;
    struct stat info;
    if (fstatat(directory, file_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        gboolean missing = errno == ENOENT;
        if (!missing) io_error(error, file_name, "incelenemedi");
        close(directory); return missing;
    }
    if (!S_ISREG(info.st_mode)) { close(directory); return invalid(error, "Yerel ayar dosyası normal dosya olmalı."); }
    gboolean removed = unlinkat(directory, file_name, 0) == 0;
    if (!removed) io_error(error, file_name, "kaldırılamadı");
    else fsync(directory);
    close(directory); return removed;
}

gboolean radios_local_ini_write(const gchar *project_root, const gchar *file_name,
                                const gchar *contents, GError **error)
{
    g_return_val_if_fail(project_root && *project_root && contents, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (!name_valid(file_name)) return invalid(error, "Yerel ayar dosyası adı geçersiz.");
    gint directory = config_directory(project_root, file_name, error);
    if (directory < 0) return FALSE;
    struct stat info;
    if (fstatat(directory, file_name, &info, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(info.st_mode)) {
        close(directory);
        return invalid(error, "Yerel ayar dosyası normal dosya olmalı; bağlantılar kabul edilmez.");
    }
    gchar *temporary = NULL;
    gint descriptor = -1;
    for (guint attempt = 0; attempt < 20 && descriptor < 0; ++attempt) {
        g_free(temporary);
        temporary = g_strdup_printf(".%s-%08x-%08x.tmp", file_name, g_random_int(), g_random_int());
        descriptor = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor < 0 && errno != EEXIST) break;
    }
    if (descriptor < 0) {
        io_error(error, file_name, "geçici dosya oluşturulamadı");
        g_free(temporary); close(directory); return FALSE;
    }
    gsize length = strlen(contents), written = 0;
    gboolean success = TRUE;
    while (written < length) {
        ssize_t amount = write(descriptor, contents + written, length - written);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) { success = io_error(error, file_name, "geçici dosya yazılamadı"); break; }
        written += (gsize)amount;
    }
    if (success && fsync(descriptor) != 0) success = io_error(error, file_name, "diske aktarılamadı");
    if (close(descriptor) != 0 && success) success = io_error(error, file_name, "kapatılamadı");
    if (success && renameat(directory, temporary, directory, file_name) != 0)
        success = io_error(error, file_name, "atomik değiştirilemedi");
    if (!success) unlinkat(directory, temporary, 0);
    else (void)fsync(directory);
    close(directory); g_free(temporary);
    return success;
}
