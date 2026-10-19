#include "radios/diagnostics.h"
#include <gio/gio.h>

gchar *
radios_diagnostics_run(gpointer data, const RadioSCancelToken *cancel, GError **error)
{
    const RadioSConfig *config = data;
    gchar *database_dir = g_path_get_dirname(config->database_file);
    const gchar *directories[] = {
        config->assets_root, config->database_source_dir, database_dir,
        config->endpoint_root, config->import_root, config->cache_root,
        config->log_root, config->recording_root, config->backup_root
    };
    for (guint i = 0; i < G_N_ELEMENTS(directories); ++i) {
        if (radios_cancelled(cancel)) {
            g_free(database_dir);
            return NULL;
        }
        if (!g_file_test(directories[i], G_FILE_TEST_IS_DIR)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                        "Dizin bulunamadı: %s", directories[i]);
            g_free(database_dir);
            return NULL;
        }
    }
    g_free(database_dir);
    return g_strdup_printf("%u dizin hazır. %s", (guint)G_N_ELEMENTS(directories),
        g_file_test(config->database_file, G_FILE_TEST_IS_REGULAR)
            ? "Veritabanı dosyası mevcut."
            : "Veritabanı dosyası henüz oluşturulmadı.");
}
