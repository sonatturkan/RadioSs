#include "radios/storage_locations.h"
#include "radios/local_ini.h"
#include "radios/platform.h"

#include <errno.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdarg.h>
#include <string.h>
#include <windows.h>

#define PATH_LIMIT 4000u
/* Without Windows long-path support a folder may use at most 240 UTF-16 units,
 * leaving room below MAX_PATH (260) for the file names RadioS, FFmpeg and the
 * Python service create inside it. */
#define SHORT_PATH_LIMIT 240
#define MARKER_FORMAT "radios-storage-v1"
#define MARKER_LIMIT 4096

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

/* RtlAreLongPathsEnabled() (Windows 10 1607+): TRUE only when the registry
 * switch LongPathsEnabled is on and the executable's manifest is
 * longPathAware (RadioS.exe and the bundled python.exe both are), i.e. when
 * every RadioS process can open paths beyond MAX_PATH. */
static gboolean long_paths_enabled(void)
{
    static gsize initialised = 0;
    static gboolean enabled = FALSE;
    if (g_once_init_enter(&initialised)) {
        typedef BOOLEAN (WINAPI *LongPathsFunc)(void);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        FARPROC address = ntdll ? GetProcAddress(ntdll, "RtlAreLongPathsEnabled") : NULL;
        LongPathsFunc query = address ? (LongPathsFunc)(void (*)(void))address : NULL;
        enabled = query && query();
        g_once_init_leave(&initialised, 1);
    }
    return enabled;
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

/* Every name of the path (UNC server and share included) must be a valid
 * Windows file name: no ':' streams, wildcards, reserved device names such as
 * NUL or COM1, or trailing dots/spaces that Win32 would silently strip. */
static gboolean components_valid(const gchar *directory)
{
    const gchar *rest = g_ascii_isalpha(directory[0]) && directory[1] == ':' ? directory + 2 : directory;
    gchar **parts = radios_path_split(rest);
    gboolean valid = parts[0] != NULL;
    for (guint i = 0; valid && parts[i]; ++i) valid = radios_path_component_valid(parts[i]);
    g_strfreev(parts);
    return valid;
}

/* Storage folder rules on Windows (identical in tools/radios_database.py):
 * absolute drive ("D:\...") or UNC ("\\server\share\...") path in canonical
 * form (backslashes, upper-case drive letter, no ".", "..", doubled or
 * trailing separator), not a root, valid names only. */
gboolean radios_storage_path_check_syntax(const gchar *directory, GError **error)
{
    if (!directory || !*directory) return fail(error, "Klasör yolu boş.");
    if (strlen(directory) > PATH_LIMIT) return fail(error, "Klasör yolu çok uzun.");
    if (!g_utf8_validate(directory, -1, NULL)) return fail(error, "Klasör yolu geçerli UTF-8 metin olmalı.");
    for (const gchar *p = directory; *p; ++p)
        if ((guchar)*p < 0x20 || *p == 0x7f) return fail(error, "Klasör yolu denetim karakteri içeremez.");
    glong units = 0;
    gunichar2 *wide = g_utf8_to_utf16(directory, -1, NULL, &units, NULL);
    gboolean converted = wide != NULL;
    g_free(wide);
    if (!converted) return fail(error, "Klasör yolu geçerli UTF-8 metin olmalı.");
    if (units > SHORT_PATH_LIMIT && !long_paths_enabled())
        return fail(error, "Klasör yolu çok uzun; Windows uzun yol desteği kapalıyken en fazla %d karakter olabilir.",
                    SHORT_PATH_LIMIT);
    if (!radios_path_is_absolute(directory))
        return fail(error, "Klasör yolu mutlak olmalı (örn. D:\\RadioS\\Kayıtlar).");
    if (radios_path_is_root(directory)) return fail(error, "Kök dizin seçilemez.");
    gchar *canonical = radios_path_normalize(directory);
    gboolean same = canonical && g_str_equal(canonical, directory);
    g_free(canonical);
    if (!same)
        return fail(error, "Klasör yolu sadeleştirilmiş olmalı (\\ ayırıcı ve büyük sürücü harfi; . , .. , / , çift \\ "
                           "veya sondaki \\ içermemeli).");
    if (!components_valid(directory))
        return fail(error, "Klasör yolunda Windows'ta geçersiz bir ad var (ayrılmış aygıt adı, : * ? \" < > | "
                           "veya sonda nokta/boşluk).");
    return TRUE;
}

/* The physical project folders RadioS itself owns. Compared after realpath,
 * case-insensitively; "database\SDBASE" is joined as two components. A
 * project root that cannot be resolved right now still protects its
 * normalised spelling instead of letting every folder through. */
static gboolean protected_location(const gchar *project_root, const gchar *directory)
{
    static const gchar *const protected_names[] = {
        "config", "src", "include", "tools", "tests", "docs", "design", "assets", "vendor", "packaging", "cmake", NULL};
    gchar *root = radios_fs_realpath(project_root, NULL);
    if (!root) root = radios_path_normalize(project_root);
    if (!root) return FALSE;
    gboolean blocked = FALSE;
    for (guint i = 0; protected_names[i] && !blocked; ++i) {
        gchar *protected_path = g_build_filename(root, protected_names[i], NULL);
        blocked = radios_path_is_inside(protected_path, directory);
        g_free(protected_path);
    }
    if (!blocked) {
        gchar *service = g_build_filename(root, "database", "SDBASE", NULL);
        blocked = radios_path_is_inside(service, directory);
        g_free(service);
    }
    g_free(root);
    return blocked;
}

static const gchar *string_member(JsonObject *object, const gchar *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}

/* Reads the marker without following a link swapped in after the check and
 * never more than MARKER_LIMIT bytes. */
static gboolean marker_read(const gchar *path, gchar **text, gsize *length)
{
    *text = NULL; *length = 0;
    gint descriptor = radios_fs_open(path, RADIOS_O_RDONLY | RADIOS_O_NOFOLLOW | RADIOS_O_CLOEXEC, 0);
    if (descriptor < 0) return FALSE;
    RadioSFileInfo info;
    if (radios_fd_stat(descriptor, &info) != 0 || info.type != RADIOS_FILE_TYPE_REGULAR ||
        info.size <= 0 || info.size > MARKER_LIMIT) {
        close(descriptor);
        return FALSE;
    }
    gchar *buffer = g_malloc(MARKER_LIMIT + 1);
    gsize used = 0;
    gboolean ok = TRUE;
    while (used < MARKER_LIMIT + 1) {
        gint amount = read(descriptor, buffer + used, (guint)(MARKER_LIMIT + 1 - used));
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) { ok = FALSE; break; }
        if (amount == 0) break;
        used += (gsize)amount;
    }
    close(descriptor);
    if (!ok || used == 0 || used > MARKER_LIMIT) { g_free(buffer); return FALSE; }
    buffer[used] = '\0';
    *text = buffer; *length = used;
    return TRUE;
}

static gboolean marker_check(const gchar *directory, RadioSStorageRole role, GError **error)
{
    gchar *path = g_build_filename(directory, RADIOS_STORAGE_MARKER, NULL);
    RadioSFileInfo info;
    if (radios_fs_stat(path, FALSE, &info) != 0) {
        g_free(path);
        return fail(error, "%s klasöründe RadioS işareti yok. Disk takılı olmayabilir veya klasör RadioS ile hazırlanmamış.",
                    role_labels[role]);
    }
    gboolean ok = info.type == RADIOS_FILE_TYPE_REGULAR && info.size > 0 && info.size <= MARKER_LIMIT;
    gchar *text = NULL; gsize length = 0;
    if (ok) ok = marker_read(path, &text, &length);
    g_free(path);
    JsonParser *parser = ok ? json_parser_new() : NULL;
    if (ok) ok = json_parser_load_from_data(parser, text, (gssize)length, NULL);
    JsonNode *root = ok ? json_parser_get_root(parser) : NULL;
    JsonObject *object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
    const gchar *format = string_member(object, "format");
    const gchar *kind = string_member(object, "role");
    gboolean good = ok && g_strcmp0(format, MARKER_FORMAT) == 0;
    gboolean same_role = good && g_strcmp0(kind, role_ids[role]) == 0;
    gchar *other = good && !same_role ? g_strdup(kind ? kind : "?") : NULL;
    g_clear_object(&parser); g_free(text);
    if (!good) return fail(error, "%s klasöründeki RadioS işareti okunamadı veya geçersiz.", role_labels[role]);
    if (!same_role) {
        fail(error, "Bu klasör RadioS tarafından başka amaçla (%s) kullanılıyor.", other);
        g_free(other);
        return FALSE;
    }
    return TRUE;
}

gboolean radios_storage_directory_check(const gchar *project_root, RadioSStorageRole role,
                                        const gchar *directory, gboolean require_marker, GError **error)
{
    g_return_val_if_fail(project_root && role < RADIOS_STORAGE_ROLE_COUNT, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (!radios_storage_path_check_syntax(directory, error)) return FALSE;
    RadioSFileInfo info;
    if (radios_fs_stat(directory, FALSE, &info) != 0)
        return fail(error, errno == ENOENT ? "Klasör bulunamadı: %s" : "Klasör incelenemedi: %s", directory);
    /* Symbolic links and junctions (directory links any user can create). */
    if (info.type == RADIOS_FILE_TYPE_LINK)
        return fail(error, "Klasör sembolik bağlantı veya dizin bağlantısı (junction) olamaz: %s", directory);
    if (info.type != RADIOS_FILE_TYPE_DIRECTORY) return fail(error, "Seçilen yol bir klasör değil: %s", directory);
    /* realpath also expands 8.3 short names and resolves subst and mapped
     * network drives (to UNC), so only the physical spelling passes. */
    gchar *physical = radios_fs_realpath(directory, NULL);
    gboolean plain = physical && radios_path_equal(physical, directory);
    g_free(physical);
    if (!plain)
        return fail(error, "Klasör yolunda sembolik bağlantı, dizin bağlantısı, kısa ad veya eşlenmiş sürücü var; "
                           "gerçek yolu seçin: %s", directory);
    if (radios_fs_dir_check_access(directory, TRUE) != 0) return fail(error, "Klasöre yazma izni yok: %s", directory);
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
    RadioSFileInfo info;
    /* Recordings inherit the parent's ACL (0755); the database and backup
     * folders RadioS creates are private to the user (0700). */
    if (radios_fs_stat(directory, FALSE, &info) != 0 && errno == ENOENT &&
        radios_fs_mkdir_with_parents(directory, role == RADIOS_STORAGE_RECORDINGS ? 0755 : 0700) != 0) {
        gint code = errno;
        return fail(error, "Klasör oluşturulamadı: %s (%s)", directory, g_strerror(code));
    }
    if (!radios_storage_directory_check(project_root, role, directory, FALSE, error)) return FALSE;
    gchar *marker = g_build_filename(directory, RADIOS_STORAGE_MARKER, NULL);
    /* 0600: created with the private owner-only descriptor. */
    gint descriptor = radios_fs_open(marker, RADIOS_O_WRONLY | RADIOS_O_CREAT | RADIOS_O_EXCL | RADIOS_O_NOFOLLOW | RADIOS_O_CLOEXEC, 0600);
    if (descriptor < 0 && errno == EEXIST) {
        g_free(marker);
        return marker_check(directory, role, error);
    }
    if (descriptor < 0) {
        gint code = errno;
        gboolean result = fail(error, "RadioS işareti yazılamadı: %s (%s)", directory, g_strerror(code));
        g_free(marker); return result;
    }
    GDateTime *now = g_date_time_new_now_utc();
    gchar *stamp = g_date_time_format_iso8601(now); g_date_time_unref(now);
    gchar *body = g_strdup_printf("{\"format\":\"%s\",\"role\":\"%s\",\"created\":\"%s\"}\n", MARKER_FORMAT, role_ids[role], stamp);
    gsize length = strlen(body), written = 0;
    gboolean ok = TRUE;
    gint code = 0;
    while (written < length) {
        gint amount = write(descriptor, body + written, (guint)(length - written));
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) { code = amount < 0 ? errno : EIO; ok = FALSE; break; }
        written += (gsize)amount;
    }
    if (ok && radios_fd_sync(descriptor) != 0) { code = errno; ok = FALSE; }
    if (close(descriptor) != 0 && ok) { code = errno; ok = FALSE; }
    if (!ok) {
        (void)radios_fs_unlink(marker);
        fail(error, "RadioS işareti yazılamadı: %s (%s)", directory, g_strerror(code));
    }
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
        ok = g_key_file_load_from_data(file, contents, (gsize)-1, G_KEY_FILE_NONE, &local);
        if (ok) {
            gint version = g_key_file_get_integer(file, "storage", "version", &local);
            if (local) ok = FALSE;
            else if (version != 1) ok = fail(&local, "storage.ini version=1 olmalı.");
        }
        for (guint i = 0; ok && i < RADIOS_STORAGE_ROLE_COUNT; ++i) {
            if (!g_key_file_has_key(file, "storage", role_keys[i], NULL)) continue;
            /* GKeyFile unescapes "\\" back to the Windows separator. */
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
        /* Backslashes are written escaped ("D:\\Radyo"); the Python reader
         * unescapes them like GKeyFile. */
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
                "diski takın ya da config\\storage.ini dosyasını kaldırın.", local->message);
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

/* The configuration file's own value for a role, resolved against the project
 * like config.c does (native separators). */
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
    gchar *resolved = NULL;
    if (value && *value) {
        gchar *joined = config->project_root && radios_path_is_absolute(config->project_root)
            ? g_canonicalize_filename(value, config->project_root) : g_canonicalize_filename(value, NULL);
        resolved = radios_path_normalize(joined);
        g_free(joined);
    }
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

/* GetDiskFreeSpaceExW for an existing path (statvfs semantics: a missing
 * path or a drive that is not ready is unknown); a file stands for the
 * volume of its folder. Free bytes are the caller's quota-aware share. */
static gboolean disk_space(const gchar *directory, gint64 *free_bytes, gint64 *total_bytes)
{
    RadioSFileInfo info;
    if (!directory || !*directory || radios_fs_stat(directory, TRUE, &info) != 0) return FALSE;
    if (info.type == RADIOS_FILE_TYPE_DIRECTORY) return radios_fs_disk_space(directory, free_bytes, total_bytes);
    gchar *parent = g_path_get_dirname(directory);
    gboolean known = radios_fs_disk_space(parent, free_bytes, total_bytes);
    g_free(parent);
    return known;
}

gint64 radios_storage_free_bytes(const gchar *directory)
{
    gint64 free_bytes = -1;
    return disk_space(directory, &free_bytes, NULL) ? free_bytes : -1;
}

gint64 radios_storage_total_bytes(const gchar *directory)
{
    gint64 total_bytes = -1;
    return disk_space(directory, NULL, &total_bytes) ? total_bytes : -1;
}

/* Links (symbolic links and junctions) are never followed; cloud placeholders
 * count with their logical size. Depth and entry budget bound the walk. */
static gint64 tree_walk(const gchar *directory, guint depth, guint *budget)
{
    GDir *dir = depth > 32 ? NULL : g_dir_open(directory, 0, NULL);
    gint64 total = 0;
    const gchar *name;
    while (dir && *budget && (name = g_dir_read_name(dir))) {
        gchar *path = g_build_filename(directory, name, NULL);
        RadioSFileInfo info;
        --*budget;
        if (radios_fs_stat(path, FALSE, &info) == 0) {
            if (info.type == RADIOS_FILE_TYPE_REGULAR && info.size > 0) total += info.size;
            else if (info.type == RADIOS_FILE_TYPE_DIRECTORY) total += tree_walk(path, depth + 1, budget);
        }
        g_free(path);
    }
    if (dir) g_dir_close(dir);
    return total;
}

gint64 radios_storage_tree_bytes(const gchar *directory)
{
    if (!directory) return -1;
    RadioSFileInfo info;
    if (radios_fs_stat(directory, FALSE, &info) != 0 || info.type != RADIOS_FILE_TYPE_DIRECTORY) return -1;
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
