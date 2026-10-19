#ifndef RADIOS_STORAGE_LOCATIONS_H
#define RADIOS_STORAGE_LOCATIONS_H

#include <glib.h>
#include "radios/config.h"

G_BEGIN_DECLS

/* Ayarlar › Medya arşivi › Radyo verileri: where RadioS writes its data.
 * <project>/config/storage.ini may move three locations out of the project:
 *   database_dir    the database file with its managed media copies
 *   recording_root  WAV recordings
 *   backup_root     database backups
 * Every chosen folder carries a marker file written by RadioS, so a disk that
 * is not mounted is noticed instead of silently replaced by an empty folder.
 * A missing recording/backup folder falls back to the project default with a
 * notice; a missing database folder is an error, because starting on a stale
 * copy would split the station's data. Source files are never touched. */
typedef enum {
    RADIOS_STORAGE_DATABASE,
    RADIOS_STORAGE_RECORDINGS,
    RADIOS_STORAGE_BACKUPS,
    RADIOS_STORAGE_ROLE_COUNT
} RadioSStorageRole;

#define RADIOS_STORAGE_MARKER ".radios-storage.json"
#define RADIOS_STORAGE_FILE "storage.ini"

typedef struct {
    gchar *path[RADIOS_STORAGE_ROLE_COUNT]; /* NULL: the project default */
} RadioSStorageSettings;

void radios_storage_settings_init(RadioSStorageSettings *settings);
void radios_storage_settings_clear(RadioSStorageSettings *settings);
void radios_storage_settings_copy(RadioSStorageSettings *destination, const RadioSStorageSettings *source);
const gchar *radios_storage_role_key(RadioSStorageRole role);
const gchar *radios_storage_role_label(RadioSStorageRole role);

/* A missing file yields all defaults without writing. */
gboolean radios_storage_settings_load(const gchar *project_root, RadioSStorageSettings *out, GError **error);
gboolean radios_storage_settings_save(const gchar *project_root, const RadioSStorageSettings *settings, GError **error);

/* Shared path rule (identical in tools/radios_database.py): an absolute drive
 * ("D:\\Radyo") or UNC ("\\\\server\\share\\Radyo") path in canonical form
 * (backslashes, upper-case drive letter, no ".", "..", doubled or trailing
 * separator), valid Windows names only, valid UTF-8 without control
 * characters, not a root; at most 240 UTF-16 units unless Windows long-path
 * support is enabled. */
gboolean radios_storage_path_check_syntax(const gchar *path, GError **error);

/* The folder rules: the path rule above, an existing directory that is not a
 * link (symbolic link or junction) and equals its physical path (no link,
 * 8.3 short name, subst or mapped drive on the way), readable and writable
 * (probed), outside the project's source and config folders. With
 * require_marker the RadioS marker for this role has to be present too. */
gboolean radios_storage_directory_check(const gchar *project_root, RadioSStorageRole role,
                                        const gchar *directory, gboolean require_marker, GError **error);
/* Creates the folder when absent and writes the role's marker. */
gboolean radios_storage_directory_prepare(const gchar *project_root, RadioSStorageRole role,
                                          const gchar *directory, GError **error);

/* Applies storage.ini to a freshly loaded configuration: database_file,
 * recording_root, backup_root and database_external. */
gboolean radios_storage_apply(RadioSConfig *config, GError **error);

/* Effective folder right now (storage.ini re-read), so a changed recording or
 * backup folder reaches a running application. Owned; falls back to the
 * configuration file's own value. database: the folder of the database file. */
gchar *radios_storage_current_dir(const RadioSConfig *config, RadioSStorageRole role);
/* NULL when every configured location is in use; otherwise the Turkish notice
 * (owned) about a folder that fell back to its default. */
gchar *radios_storage_notice(const RadioSConfig *config);

/* Bytes free (for this user, quota-aware) and total on the volume holding
 * directory (-1 when unknown, e.g. a missing path or a drive that is not
 * ready), and the bytes of regular files below it (links and junctions are
 * not followed). */
gint64 radios_storage_free_bytes(const gchar *directory);
gint64 radios_storage_total_bytes(const gchar *directory);
gint64 radios_storage_tree_bytes(const gchar *directory);
gchar *radios_storage_format_bytes(gint64 bytes);

G_END_DECLS

#endif
