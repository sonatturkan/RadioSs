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
 * resolved against the project root, and constrained to that root (Windows
 * rules: case-insensitive, both separators, links and junctions resolved,
 * reserved names such as NUL or "x:stream" rejected).
 * python_command is returned as the absolute interpreter path (DECISIONS D4):
 * an explicit absolute or root-relative .exe path, or for the default names
 * python3/python the bundled <root>\runtime\python\python.exe (else
 * <root>\python\python.exe); PATH is never searched. Its existence is checked
 * when spawning (radios_config_python_executable), so a configured but
 * missing interpreter fails closed. */
gboolean radios_config_load(const gchar *project_root,
                            const gchar *config_path,
                            RadioSConfig *out,
                            GError **error);
void radios_config_clear(RadioSConfig *config);

/* Arguments every Python child of RadioS starts with (D4): ignore PYTHON*
 * variables and the user site, write no .pyc, UTF-8 I/O. */
#define RADIOS_PYTHON_FLAGS "-E", "-s", "-B", "-X", "utf8"

/* config->python_command checked for a spawn: an absolute path to an existing
 * regular file. NULL with G_IO_ERROR_NOT_FOUND (missing interpreter) or
 * G_IO_ERROR_INVALID_FILENAME (not an absolute Windows path) otherwise. */
gchar *radios_config_python_executable(const RadioSConfig *config, GError **error);
/* { interpreter, RADIOS_PYTHON_FLAGS, script, arguments..., NULL } for
 * radios_process_spawn(); g_strfreev(). NULL with the error above. */
gchar **radios_config_python_argv(const RadioSConfig *config, const gchar *script,
                                  const gchar *const *arguments, GError **error);
/* Overrides for radios_process_environment() when spawning Python:
 * PYTHONUTF8=1, PYTHONDONTWRITEBYTECODE=1, PYTHONIOENCODING=utf-8 and removal
 * of PYTHONHOME/PYTHONPATH. Static, NULL-terminated. */
const gchar *const *radios_config_python_environment_overrides(void);

G_END_DECLS

#endif
