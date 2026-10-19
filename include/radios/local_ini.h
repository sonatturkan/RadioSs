#ifndef RADIOS_LOCAL_INI_H
#define RADIOS_LOCAL_INI_H

#include <glib.h>

G_BEGIN_DECLS

/* Workstation files that live directly in <project>/config (for example
 * media-archive.ini and storage.ini). The config directory must exist and
 * stay inside the project. File links are rejected; reads are bounded;
 * writes replace the file atomically with 0600 permissions. */

/* A missing file succeeds with *contents == NULL. Otherwise *contents is a
 * NUL-terminated, valid UTF-8 copy owned by the caller. */
gboolean radios_local_ini_read(const gchar *project_root, const gchar *file_name, gsize limit,
                               gchar **contents, GError **error);
gboolean radios_local_ini_write(const gchar *project_root, const gchar *file_name,
                                const gchar *contents, GError **error);
gboolean radios_local_ini_remove(const gchar *project_root, const gchar *file_name, GError **error);

G_END_DECLS

#endif
