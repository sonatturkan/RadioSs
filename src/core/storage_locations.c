#define _XOPEN_SOURCE 700

#include "radios/storage_locations.h"
#include "radios/local_ini.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define PATH_LIMIT 4000u
#define MARKER_FORMAT "radios-storage-v1"

static const gchar *const role_keys[RADIOS_STORAGE_ROLE_COUNT] = {"database_dir", "recording_root", "backup_root"};
static const gchar *const role_ids[RADIOS_STORAGE_ROLE_COUNT] = {"database", "recordings", "backups"};
static const gchar *const role_labels[RADIOS_STORAGE_ROLE_COUNT] = {
    "Veritabanı ve yönetilen medya", "Kayıtlar", "Yedekler"};

static gboolean fail(GError **error, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
static gboolean fail(GError **error, const gchar *format, ...)
{
    va_list arguments; va_start(arguments, format);
    gchar *message = g_strdup_vprintf(format, arguments); va_end(arguments);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    g_free(message);
    return FALSE;
}

void radios_storage_settings_init(RadioSStorageSettings *settings)
{
    g_return_if_fail(settings != NULL);
    memset(settings, 0, sizeof *settings);
}

void radios_storage_settings_clear(RadioSStorageSettings *settings)
{
    g_return_if_fail(settings != NULL);
    for (guint i = 0; i < RADIOS_STORAGE_ROLE_COUNT; ++i) g_clear_pointer(&settings->path[i], g_free);
}

void radios_storage_settings_copy(RadioSStorageSettings *destination, const RadioSStorageSettings *source)
{
    g_return_if_fail(destination && source && destination != source);
    radios_storage_settings_clear(destination);
    for (guint i = 0; i < RADIOS_STORAGE_ROLE_COUNT; ++i) destination->path[i] = g_strdup(source->path[i]);
}

const gchar *radios_storage_role_key(RadioSStorageRole role)
{ return role < RADIOS_STORAGE_ROLE_COUNT ? role_keys[role] : ""; }

const gchar *radios_storage_role_label(RadioSStorageRole role)
{ return role < RADIOS_STORAGE_ROLE_COUNT ? role_labels[role] : ""; }

gboolean radios_storage_path_check_syntax(const gchar *directory, GError **error)
{
    if (!directory || !*directory) return fail(error, "Klasör yolu boş.");
    if (strlen(directory) > PATH_LIMIT) return fail(error, "Klasör yolu çok uzun.");
    if (!g_utf8_validate(directory, -1, NULL)) return fail(error, "Klasör yolu geçerli UTF-8 metin olmalı.");
    for (const gchar *p = directory; *p; ++p)
        if ((guchar)*p < 0x20 || *p == 0x7f) return fail(error, "Klasör yolu denetim karakteri içeremez.");
    if (!g_path_is_absolute(directory)) return fail(error, "Klasör yolu mutlak olmalı (/ ile başlamalı).");
    gchar *canonical = g_canonicalize_filename(directory, NULL);
    gboolean same = g_str_equal(canonical, directory);
    g_free(canonical);
    if (!same) return fail(error, "Klasör yolu sadeleştirilmiş olmalı (.. , // veya sondaki / içermemeli).");
    if (g_str_equal(directory, "/")) return fail(error, "Kök dizin seçilemez.");
    return TRUE;
}

static gboolean path_inside(const gchar *root, const gchar *path)
{
    gsize length = strlen(root);
    return g_str_equal(root, path) ||
           (g_str_has_prefix(path, root) && (length == 1 || path[length] == G_DIR_SEPARATOR));
}

static gboolean protected_location(const gchar *project_root, const gchar *directory)
{
    static const gchar *const protected_names[] = {
        "config", "src", "include", "tools", "tests", "docs", "design", "assets", "vendor", "packaging", "cmake",
        "database/SDBASE", NULL};
    gchar *root = realpath(project_root, NULL);
    gboolean blocked = FALSE;
    for (guint i = 0; root && protected_names[i] && !blocked; ++i) {
        gchar *protected_path = g_build_filename(root, protected_names[i], NULL);
        blocked = path_inside(protected_path, directory);
        g_free(protected_path);
    }
    free(root);
    return blocked;
}

static const gchar *string_member(JsonObject *object, const gchar *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}

static gboolean marker_check(const gchar *directory, RadioSStorageRole role, GError **error)
{
    gchar *path = g_build_filename(directory, RADIOS_STORAGE_MARKER, NULL);
    struct stat info;
    if (lstat(path, &info) != 0) {
        g_free(path);
        return fail(error, "%s klasöründe RadioS işareti yok. Disk takılı olmayabilir veya klasör RadioS ile hazırlanmamış.",
                    role_labels[role]);
    }
    gboolean ok = S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= 4096;
    gchar *text = NULL; gsize length = 0;
    if (ok) ok = g_file_get_contents(path, &text, &length, NULL);
    g_free(path);
    JsonParser *parser = ok ? json_parser_new() : NULL;
    if (ok) ok = json_parser_load_from_data(parser, text, (gssize)length, NULL);
    JsonNode *root = ok ? json_parser_get_root(parser) : NULL;
    JsonObject *object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
    const gchar *format = string_member(object, "format");
    const gchar *kind = string_member(object, "role");
    gboolean good = ok && g_strcmp0(format, MARKER_FORMAT) == 0;
    gboolean same_role = good && g_strcmp0(kind, role_ids[role]) == 0;
    g_clear_object(&parser); g_free(text);
    if (!good) return fail(error, "%s klasöründeki RadioS işareti okunamadı veya geçersiz.", role_labels[role]);
    if (!same_role) return fail(error, "Bu klasör RadioS tarafından başka amaçla (%s) kullanılıyor.", kind ? kind : "?");
    return TRUE;
}

gboolean radios_storage_directory_check(const gchar *project_root, RadioSStorageRole role,
                                        const gchar *directory, gboolean require_marker, GError **error)
{
    g_return_val_if_fail(project_root && role < RADIOS_STORAGE_ROLE_COUNT, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (!radios_storage_path_check_syntax(directory, error)) return FALSE;
    struct stat info;
    if (lstat(directory, &info) != 0)
        return fail(error, errno == ENOENT ? "Klasör bulunamadı: %s" : "Klasör incelenemedi: %s", directory);
    if (S_ISLNK(info.st_mode)) return fail(error, "Klasör sembolik bağlantı olamaz: %s", directory);
    if (!S_ISDIR(info.st_mode)) return fail(error, "Seçilen yol bir klasör değil: %s", directory);
    gchar *physical = realpath(directory, NULL);
    gboolean plain = physical && g_str_equal(physical, directory);
    free(physical);
    if (!plain) return fail(error, "Klasör yolunda sembolik bağlantı var; gerçek yolu seçin: %s", directory);
    if (access(directory, R_OK | W_OK | X_OK) != 0) return fail(error, "Klasöre yazma izni yok: %s", directory);
    if (protected_location(project_root, directory))
        return fail(error, "RadioS'un kendi kaynak veya ayar klasörü seçilemez: %s", directory);
    return require_marker ? marker_check(directory, role, error) : TRUE;
}

gboolean radios_storage_directory_prepare(const gchar *project_root, RadioSStorageRole role,
                                          const gchar *directory, GError **error)
{
    g_return_val_if_fail(project_root && role < RADIOS_STORAGE_ROLE_COUNT, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (!radios_storage_path_check_syntax(directory, error)) return FALSE;
    struct stat info;
    if (lstat(directory, &info) != 0 && errno == ENOENT &&
        g_mkdir_with_parents(directory, role == RADIOS_STORAGE_RECORDINGS ? 0755 : 0700) != 0)
        return fail(error, "Klasör oluşturulamadı: %s (%s)", directory, g_strerror(errno));
    if (!radios_storage_directory_check(project_root, role, directory, FALSE, error)) return FALSE;
    gchar *marker = g_build_filename(directory, RADIOS_STORAGE_MARKER, NULL);
    gint descriptor = open(marker, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0 && errno == EEXIST) {
        g_free(marker);
        return marker_check(directory, role, error);
    }
    if (descriptor < 0) {
        gboolean result = fail(error, "RadioS işareti yazılamadı: %s (%s)", directory, g_strerror(errno));
        g_free(marker); return result;
    }
    GDateTime *now = g_date_time_new_now_utc();
    gchar *stamp = g_date_time_format_iso8601(now); g_date_time_unref(now);
    gchar *body = g_strdup_printf("{\"format\":\"%s\",\"role\":\"%s\",\"created\":\"%s\"}\n", MARKER_FORMAT, role_ids[role], stamp);
    gsize length = strlen(body), written = 0;
    gboolean ok = TRUE;
    while (written < length) {
        ssize_t amount = write(descriptor, body + written, length - written);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) { ok = FALSE; break; }
        written += (gsize)amount;
    }
    if (ok && fsync(descriptor) != 0) ok = FALSE;
    if (close(descriptor) != 0) ok = FALSE;
    if (!ok) { unlink(marker); fail(error, "RadioS işareti yazılamadı: %s", directory); }
    g_free(stamp); g_free(body); g_free(marker);
    return ok;
}

gboolean radios_storage_settings_load(const gchar *project_root, RadioSStorageSettings *out, GError **error)
{
    g_return_val_if_fail(project_root && out, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    gchar *contents = NULL;
    if (!radios_local_ini_read(project_root, RADIOS_STORAGE_FILE, 16384, &contents, error)) return FALSE;
    RadioSStorageSettings proposed; radios_storage_settings_init(&proposed);
    gboolean ok = TRUE;
    if (contents) {
        GKeyFile *file = g_key_file_new();
        GError *local = NULL;
        ok = g_key_file_load_from_data(file, contents, -1, G_KEY_FILE_NONE, &local);
        if (ok) {
            gint version = g_key_file_get_integer(file, "storage", "version", &local);
            if (local) ok = FALSE;
            else if (version != 1) ok = fail(&local, "storage.ini version=1 olmalı.");
        }
        for (guint i = 0; ok && i < RADIOS_STORAGE_ROLE_COUNT; ++i) {
            if (!g_key_file_has_key(file, "storage", role_keys[i], NULL)) continue;
            gchar *value = g_key_file_get_string(file, "storage", role_keys[i], &local);
            if (!value) { ok = FALSE; break; }
            if (!*value) { g_free(value); continue; }
            if (!radios_storage_path_check_syntax(value, &local)) { g_free(value); ok = FALSE; break; }
            proposed.path[i] = value;
        }
        if (!ok) g_propagate_prefixed_error(error, local, "Depolama ayarları (storage.ini) okunamadı: ");
        g_key_file_unref(file);
    }
    g_free(contents);
    if (!ok) { radios_storage_settings_clear(&proposed); return FALSE; }
    radios_storage_settings_clear(out);
    *out = proposed;
    return TRUE;
}

gboolean radios_storage_settings_save(const gchar *project_root, const RadioSStorageSettings *settings, GError **error)
{
    g_return_val_if_fail(project_root && settings, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    GKeyFile *file = g_key_file_new();
    g_key_file_set_integer(file, "storage", "version", 1);
    for (guint i = 0; i < RADIOS_STORAGE_ROLE_COUNT; ++i) {
        if (!settings->path[i]) continue;
        if (!radios_storage_path_check_syntax(settings->path[i], error)) { g_key_file_unref(file); return FALSE; }
        g_key_file_set_string(file, "storage", role_keys[i], settings->path[i]);
    }
    gchar *data = g_key_file_to_data(file, NULL, NULL);
    g_key_file_unref(file);
    gboolean saved = radios_local_ini_write(project_root, RADIOS_STORAGE_FILE, data, error);
    g_free(data);
    return saved;
}

static gboolean config_error(GError **error, const gchar *message)
{
    g_set_error_literal(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID, message);
    return FALSE;
}

gboolean radios_storage_apply(RadioSConfig *config, GError **error)
{
    g_return_val_if_fail(config && config->project_root && config->database_file, FALSE);
    RadioSStorageSettings settings; radios_storage_settings_init(&settings);
    GError *local = NULL;
    if (!radios_storage_settings_load(config->project_root, &settings, &local)) {
        gboolean result = config_error(error, local ? local->message : "Depolama ayarları okunamadı.");
        g_clear_error(&local); return result;
    }
    gboolean ok = TRUE;
    if (settings.path[RADIOS_STORAGE_DATABASE]) {
        if (!radios_storage_directory_check(config->project_root, RADIOS_STORAGE_DATABASE,
                                            settings.path[RADIOS_STORAGE_DATABASE], TRUE, &local)) {
            gchar *message = g_strdup_printf("Veritabanı klasörü kullanılamıyor: %s Eski bir kopyayla başlamamak için uygulama açılmadı; "
                "diski takın ya da config/storage.ini dosyasını kaldırın.", local->message);
            ok = config_error(error, message);
            g_free(message); g_clear_error(&local);
        } else {
            gchar *name = g_path_get_basename(config->database_file);
            gchar *moved = g_build_filename(settings.path[RADIOS_STORAGE_DATABASE], name, NULL);
            g_free(name); g_free(config->database_file);
            config->database_file = moved; config->database_external = TRUE;
        }
    }
    for (guint role = RADIOS_STORAGE_RECORDINGS; ok && role <= RADIOS_STORAGE_BACKUPS; ++role) {
        const gchar *path = settings.path[role];
        if (!path || !radios_storage_directory_check(config->project_root, (RadioSStorageRole)role, path, TRUE, NULL)) continue;
        gchar **target = role == RADIOS_STORAGE_RECORDINGS ? &config->recording_root : &config->backup_root;
        g_free(*target); *target = g_strdup(path);
    }
    radios_storage_settings_clear(&settings);
    return ok;
}

/* The configuration file's own value for a role, resolved like config.c does. */
static gchar *default_dir(const RadioSConfig *config, RadioSStorageRole role)
{
    gchar *value = NULL;
    if (config->config_file) {
        GKeyFile *file = g_key_file_new();
        if (g_key_file_load_from_file(file, config->config_file, G_KEY_FILE_NONE, NULL)) {
            const gchar *key = role == RADIOS_STORAGE_DATABASE ? "database_file" : role_keys[role];
            value = g_key_file_get_string(file, "paths", key, NULL);
        }
        g_key_file_unref(file);
    }
    gchar *resolved = value && *value ? g_canonicalize_filename(value, config->project_root) : NULL;
    g_free(value);
    if (resolved && role == RADIOS_STORAGE_DATABASE) {
        gchar *directory = g_path_get_dirname(resolved); g_free(resolved); resolved = directory;
    }
    if (resolved) return resolved;
    if (role == RADIOS_STORAGE_DATABASE) return g_path_get_dirname(config->database_file);
    return g_strdup(role == RADIOS_STORAGE_RECORDINGS ? config->recording_root : config->backup_root);
}

gchar *radios_storage_current_dir(const RadioSConfig *config, RadioSStorageRole role)
{
    g_return_val_if_fail(config && role < RADIOS_STORAGE_ROLE_COUNT, NULL);
    if (role == RADIOS_STORAGE_DATABASE) return g_path_get_dirname(config->database_file);
    RadioSStorageSettings settings; radios_storage_settings_init(&settings);
    gchar *result = NULL;
    if (config->project_root && radios_storage_settings_load(config->project_root, &settings, NULL) && settings.path[role] &&
        radios_storage_directory_check(config->project_root, role, settings.path[role], TRUE, NULL))
        result = g_strdup(settings.path[role]);
    radios_storage_settings_clear(&settings);
    return result ? result : default_dir(config, role);
}

gchar *radios_storage_notice(const RadioSConfig *config)
{
    g_return_val_if_fail(config && config->project_root, NULL);
    RadioSStorageSettings settings; radios_storage_settings_init(&settings);
    GError *error = NULL;
    GString *notice = NULL;
    if (!radios_storage_settings_load(config->project_root, &settings, &error)) {
        notice = g_string_new(error ? error->message : "Depolama ayarları okunamadı.");
        g_clear_error(&error);
    }
    for (guint role = 0; role < RADIOS_STORAGE_ROLE_COUNT; ++role) {
        if (!settings.path[role] ||
            radios_storage_directory_check(config->project_root, (RadioSStorageRole)role, settings.path[role], TRUE, &error)) {
            g_clear_error(&error); continue;
        }
        if (!notice) notice = g_string_new(NULL); else g_string_append_c(notice, '\n');
        g_string_append_printf(notice, "%s: %s%s", role_labels[role], error->message,
                               role == RADIOS_STORAGE_DATABASE ? "" : " Varsayılan klasör kullanılıyor.");
        g_clear_error(&error);
    }
    radios_storage_settings_clear(&settings);
    return notice ? g_string_free(notice, FALSE) : NULL;
}

gint64 radios_storage_free_bytes(const gchar *directory)
{
    struct statvfs info;
    if (!directory || statvfs(directory, &info) != 0) return -1;
    return (gint64)info.f_bavail * (gint64)info.f_frsize;
}

gint64 radios_storage_total_bytes(const gchar *directory)
{
    struct statvfs info;
    if (!directory || statvfs(directory, &info) != 0) return -1;
    return (gint64)info.f_blocks * (gint64)info.f_frsize;
}

static gint64 tree_walk(const gchar *directory, guint depth, guint *budget)
{
    GDir *dir = depth > 32 ? NULL : g_dir_open(directory, 0, NULL);
    gint64 total = 0;
    const gchar *name;
    while (dir && *budget && (name = g_dir_read_name(dir))) {
        gchar *path = g_build_filename(directory, name, NULL);
        struct stat info;
        --*budget;
        if (lstat(path, &info) == 0) {
            if (S_ISREG(info.st_mode)) total += info.st_size;
            else if (S_ISDIR(info.st_mode)) total += tree_walk(path, depth + 1, budget);
        }
        g_free(path);
    }
    if (dir) g_dir_close(dir);
    return total;
}

gint64 radios_storage_tree_bytes(const gchar *directory)
{
    if (!directory) return -1;
    struct stat info;
    if (lstat(directory, &info) != 0 || !S_ISDIR(info.st_mode)) return -1;
    guint budget = 2000000u;
    return tree_walk(directory, 0, &budget);
}

gchar *radios_storage_format_bytes(gint64 bytes)
{
    if (bytes < 0) return g_strdup("—");
    static const gchar *const units[] = {"B", "KB", "MB", "GB", "TB"};
    gdouble value = (gdouble)bytes;
    guint unit = 0;
    while (value >= 1024.0 && unit + 1 < G_N_ELEMENTS(units)) { value /= 1024.0; ++unit; }
    gchar number[32];
    if (unit == 0) g_snprintf(number, sizeof number, "%" G_GINT64_FORMAT, bytes);
    else g_snprintf(number, sizeof number, "%.1f", value);
    for (gchar *p = number; *p; ++p) if (*p == '.') *p = ',';
    return g_strdup_printf("%s %s", number, units[unit]);
}
