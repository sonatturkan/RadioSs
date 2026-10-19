#include "radios/local_ini.h"
#include "radios/platform.h"

#include <errno.h>
#include <gio/gio.h>
#include <string.h>

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

/* One plain Windows file name directly in config: no separators, streams,
 * reserved device names or trailing dots/spaces; never hidden ('.'). */
static gboolean name_valid(const gchar *name)
{
    return name && *name && name[0] != '.' && strlen(name) < 100 && radios_path_component_valid(name);
}

/* Opens <project>/config without following links; the physical directory has
 * to stay inside the physical project root. */
static gint config_directory(const gchar *project_root, const gchar *file_name, GError **error)
{
    gchar *root = radios_fs_realpath(project_root, NULL);
    if (!root) { io_error(error, file_name, "proje kökü çözülemedi"); return -1; }
    gchar *requested = g_build_filename(root, "config", NULL);
    gchar *directory = radios_fs_realpath(requested, NULL);
    g_free(requested);
    if (!directory) { g_free(root); io_error(error, file_name, "config dizini çözülemedi"); return -1; }
    gboolean inside = radios_path_is_inside(root, directory);
    g_free(root);
    if (!inside) { g_free(directory); invalid(error, "config dizini RadioS dışına çıkıyor."); return -1; }
    gint descriptor = radios_fs_open(directory, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY | RADIOS_O_NOFOLLOW | RADIOS_O_CLOEXEC, 0);
    if (descriptor < 0) io_error(error, file_name, "config dizini açılamadı");
    g_free(directory);
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
    RadioSFileInfo info;
    if (radios_fs_statat(directory, file_name, FALSE, &info) != 0) {
        gboolean missing = errno == ENOENT;
        if (!missing) io_error(error, file_name, "incelenemedi");
        close(directory);
        return missing;
    }
    if (info.type != RADIOS_FILE_TYPE_REGULAR) { close(directory); return invalid(error, "Yerel ayar dosyası normal dosya olmalı; bağlantılar kabul edilmez."); }
    gint descriptor = radios_fs_openat(directory, file_name, RADIOS_O_RDONLY | RADIOS_O_NOFOLLOW | RADIOS_O_CLOEXEC | RADIOS_O_NONBLOCK, 0);
    if (descriptor < 0) { io_error(error, file_name, "açılamadı"); close(directory); return FALSE; }
    close(directory);
    if (radios_fd_stat(descriptor, &info) != 0 || info.type != RADIOS_FILE_TYPE_REGULAR || info.size < 0 || (guint64)info.size > limit) {
        close(descriptor);
        return invalid(error, "Yerel ayar dosyası normal dosya olmalı ve boyut sınırını aşmamalı.");
    }
    gchar *buffer = g_malloc(limit + 1);
    gsize used = 0;
    while (used < limit + 1) {
        gint amount = read(descriptor, buffer + used, (guint)(limit + 1 - used));
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) { io_error(error, file_name, "okunamadı"); close(descriptor); g_free(buffer); return FALSE; }
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
    RadioSFileInfo info;
    if (radios_fs_statat(directory, file_name, FALSE, &info) != 0) {
        gboolean missing = errno == ENOENT;
        if (!missing) io_error(error, file_name, "incelenemedi");
        close(directory); return missing;
    }
    if (info.type != RADIOS_FILE_TYPE_REGULAR) { close(directory); return invalid(error, "Yerel ayar dosyası normal dosya olmalı."); }
    gboolean removed = radios_fs_unlinkat(directory, file_name, 0) == 0;
    if (!removed) io_error(error, file_name, "kaldırılamadı");
    else (void)radios_fd_sync_dir(directory);
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
    RadioSFileInfo info;
    if (radios_fs_statat(directory, file_name, FALSE, &info) == 0 && info.type != RADIOS_FILE_TYPE_REGULAR) {
        close(directory);
        return invalid(error, "Yerel ayar dosyası normal dosya olmalı; bağlantılar kabul edilmez.");
    }
    gchar *temporary = NULL;
    gint descriptor = -1;
    for (guint attempt = 0; attempt < 20 && descriptor < 0; ++attempt) {
        g_free(temporary);
        temporary = g_strdup_printf(".%s-%08x-%08x.tmp", file_name, g_random_int(), g_random_int());
        /* 0600: created with the private owner-only descriptor. */
        descriptor = radios_fs_openat(directory, temporary,
                                      RADIOS_O_WRONLY | RADIOS_O_CREAT | RADIOS_O_EXCL | RADIOS_O_NOFOLLOW | RADIOS_O_CLOEXEC, 0600);
        if (descriptor < 0 && errno != EEXIST) break;
    }
    if (descriptor < 0) {
        io_error(error, file_name, "geçici dosya oluşturulamadı");
        g_free(temporary); close(directory); return FALSE;
    }
    gsize length = strlen(contents), written = 0;
    gboolean success = TRUE;
    while (written < length) {
        gsize chunk = MIN(length - written, (gsize)G_MAXINT32);
        gint amount = write(descriptor, contents + written, (guint)chunk);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) { success = io_error(error, file_name, "geçici dosya yazılamadı"); break; }
        written += (gsize)amount;
    }
    if (success && radios_fd_sync(descriptor) != 0) success = io_error(error, file_name, "diske aktarılamadı");
    if (close(descriptor) != 0 && success) success = io_error(error, file_name, "kapatılamadı");
    /* Atomic replacement even while a reader holds the old file open with
     * FILE_SHARE_DELETE; sharing violations are retried by the platform. */
    if (success && radios_fs_renameat(directory, temporary, directory, file_name) != 0)
        success = io_error(error, file_name, "atomik değiştirilemedi");
    if (!success) (void)radios_fs_unlinkat(directory, temporary, 0);
    else (void)radios_fd_sync_dir(directory);
    close(directory); g_free(temporary);
    return success;
}
