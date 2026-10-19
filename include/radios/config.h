#ifndef RADIOS_CONFIG_H
#define RADIOS_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

#define RADIOS_CONFIG_ERROR (radios_config_error_quark())

typedef enum {
    RADIOS_CONFIG_ERROR_IO,
    RADIOS_CONFIG_ERROR_INVALID
} RadioSConfigError;

typedef struct {
    gchar *project_root;
    gchar *config_file;
    gchar *name;
    gchar *initial_profile;
    gchar *assets_root;
    gchar *database_file;
    gchar *endpoint_root;
    gchar *import_root;
    gchar *cache_root;
    gchar *log_root;
    gchar *recording_root;
    gchar *backup_root;
    gchar *database_source_dir;
    gchar *service_url;
    gchar *python_command;
    gchar *token_environment;
    gint connect_timeout_seconds;
    gint request_timeout_seconds;
    gboolean auto_start_playback;
    /* config/storage.ini moved the database outside the project folder. */
    gboolean database_external;
} RadioSConfig;

GQuark radios_config_error_quark(void);
gboolean radios_config_profile_is_valid(const gchar *profile);

/* The result is owned by the caller. Explicit invalid roots never fall back. */
gchar *radios_project_root_find(const gchar *explicit_root,
                               const gchar *executable_path,
                               GError **error);

/* out must be zero-initialized or cleared. No directories/files are created.
 * On failure out is unchanged. All returned filesystem paths are absolute,
 * resolved against the project root, and constrained to that root. */
gboolean radios_config_load(const gchar *project_root,
                            const gchar *config_path,
                            RadioSConfig *out,
                            GError **error);
void radios_config_clear(RadioSConfig *config);

G_END_DECLS

#endif
