#define _XOPEN_SOURCE 700

#include "radios/media_archive.h"
#include "radios/local_ini.h"
#include "radios/storage_locations.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SETTINGS_FILE "media-archive.ini"
#define INDEX_LIMIT (64u * 1024u * 1024u)
#define HASH_CHUNK (256u * 1024u)

static const gchar *const audio_extensions[] = {".wav", ".mp3", ".flac", ".ogg", ".oga", ".opus", ".m4a", ".aac", NULL};
static const gchar *const kinds[] = {"music", "advertisement", "jingle", "sweeper", "news", "program",
                                     "announcement", "effect", "voice_track", "other", NULL};

static gboolean fail(GError **error, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
static gboolean fail(GError **error, const gchar *format, ...)
{
    va_list arguments; va_start(arguments, format);
    gchar *message = g_strdup_vprintf(format, arguments); va_end(arguments);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    g_free(message);
    return FALSE;
}

/* ---------------------------------------------------------------- settings */

void radios_media_archive_settings_init(RadioSMediaArchiveSettings *settings)
{
    g_return_if_fail(settings != NULL);
    settings->folders = g_ptr_array_new_with_free_func(g_free);
    settings->kind = g_strdup("music");
    settings->folder_playlists = FALSE;
    settings->watch = FALSE;
    settings->watch_minutes = 30;
}

void radios_media_archive_settings_clear(RadioSMediaArchiveSettings *settings)
{
    g_return_if_fail(settings != NULL);
    g_clear_pointer(&settings->folders, g_ptr_array_unref);
    g_clear_pointer(&settings->kind, g_free);
}

void radios_media_archive_settings_copy(RadioSMediaArchiveSettings *destination, const RadioSMediaArchiveSettings *source)
{
    g_return_if_fail(destination && source && destination != source);
    radios_media_archive_settings_clear(destination);
    radios_media_archive_settings_init(destination);
    g_free(destination->kind); destination->kind = g_strdup(source->kind);
    for (guint i = 0; source->folders && i < source->folders->len; ++i)
        g_ptr_array_add(destination->folders, g_strdup(g_ptr_array_index(source->folders, i)));
    destination->folder_playlists = source->folder_playlists;
    destination->watch = source->watch;
    destination->watch_minutes = source->watch_minutes;
}

gboolean radios_media_archive_kind_is_valid(const gchar *kind)
{
    for (guint i = 0; kind && kinds[i]; ++i) if (g_str_equal(kind, kinds[i])) return TRUE;
    return FALSE;
}

const gchar *radios_media_archive_playlist_purpose(const gchar *kind)
{
    if (!kind) return "music";
    if (g_str_equal(kind, "advertisement")) return "advert";
    if (g_str_equal(kind, "jingle")) return "jingle";
    if (g_str_equal(kind, "sweeper")) return "sweeper";
    if (g_str_equal(kind, "news") || g_str_equal(kind, "announcement") || g_str_equal(kind, "voice_track")) return "announcement";
    if (g_str_equal(kind, "program")) return "programme";
    if (g_str_equal(kind, "effect") || g_str_equal(kind, "other")) return "other";
    return "music";
}

static gboolean path_inside(const gchar *root, const gchar *path)
{
    gsize length = strlen(root);
    return g_str_equal(root, path) ||
           (g_str_has_prefix(path, root) && (length == 1 || path[length] == G_DIR_SEPARATOR));
}

gboolean radios_media_archive_settings_validate(const RadioSMediaArchiveSettings *settings, GError **error)
{
    g_return_val_if_fail(settings != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    guint count = settings->folders ? settings->folders->len : 0;
    if (count > RADIOS_MEDIA_ARCHIVE_MAX_FOLDERS) return fail(error, "En çok %u arşiv klasörü tanımlanabilir.", RADIOS_MEDIA_ARCHIVE_MAX_FOLDERS);
    for (guint i = 0; i < count; ++i) {
        const gchar *folder = g_ptr_array_index(settings->folders, i);
        if (!radios_storage_path_check_syntax(folder, error)) return FALSE;
        for (guint j = 0; j < i; ++j) {
            const gchar *other = g_ptr_array_index(settings->folders, j);
            if (g_str_equal(folder, other)) return fail(error, "Klasör iki kez eklenmiş: %s", folder);
            if (path_inside(other, folder) || path_inside(folder, other))
                return fail(error, "Arşiv klasörleri iç içe olamaz: %s ve %s", other, folder);
        }
    }
    if (!radios_media_archive_kind_is_valid(settings->kind)) return fail(error, "Arşiv içe aktarma türü geçersiz.");
    if (settings->watch_minutes < 5 || settings->watch_minutes > 1440) return fail(error, "İzleme aralığı 5–1440 dakika olmalı.");
    return TRUE;
}

gboolean radios_media_archive_settings_load(const gchar *project_root, RadioSMediaArchiveSettings *out, GError **error)
{
    g_return_val_if_fail(project_root && out, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    gchar *contents = NULL;
    if (!radios_local_ini_read(project_root, SETTINGS_FILE, 65536, &contents, error)) return FALSE;
    RadioSMediaArchiveSettings proposed; radios_media_archive_settings_init(&proposed);
    gboolean ok = TRUE;
    GError *local = NULL;
    if (contents) {
        GKeyFile *file = g_key_file_new();
        ok = g_key_file_load_from_data(file, contents, -1, G_KEY_FILE_NONE, &local);
        if (ok) {
            gint version = g_key_file_get_integer(file, "archive", "version", &local);
            if (local) ok = FALSE; else if (version != 1) ok = fail(&local, "media-archive.ini version=1 olmalı.");
        }
        if (ok && g_key_file_has_key(file, "archive", "folders", NULL)) {
            gsize length = 0;
            gchar **folders = g_key_file_get_string_list(file, "archive", "folders", &length, &local);
            if (!folders) ok = FALSE;
            for (gsize i = 0; folders && i < length; ++i) {
                if (!folders[i][0]) continue;
                if (proposed.folders->len >= RADIOS_MEDIA_ARCHIVE_MAX_FOLDERS) { ok = fail(&local, "Çok fazla arşiv klasörü tanımlı."); break; }
                g_ptr_array_add(proposed.folders, g_strdup(folders[i]));
            }
            g_strfreev(folders);
        }
        if (ok && g_key_file_has_key(file, "archive", "kind", NULL)) {
            gchar *kind = g_key_file_get_string(file, "archive", "kind", &local);
            if (!kind) ok = FALSE;
            else { g_free(proposed.kind); proposed.kind = kind; }
        }
        if (ok && g_key_file_has_key(file, "archive", "folder_playlists", NULL)) {
            proposed.folder_playlists = g_key_file_get_boolean(file, "archive", "folder_playlists", &local); if (local) ok = FALSE;
        }
        if (ok && g_key_file_has_key(file, "archive", "watch", NULL)) {
            proposed.watch = g_key_file_get_boolean(file, "archive", "watch", &local); if (local) ok = FALSE;
        }
        if (ok && g_key_file_has_key(file, "archive", "watch_minutes", NULL)) {
            gint minutes = g_key_file_get_integer(file, "archive", "watch_minutes", &local);
            if (local) ok = FALSE; else proposed.watch_minutes = minutes < 0 ? 0 : (guint)minutes;
        }
        g_key_file_unref(file);
        if (ok && !radios_media_archive_settings_validate(&proposed, &local)) ok = FALSE;
    }
    g_free(contents);
    if (!ok) {
        g_propagate_prefixed_error(error, local, "Medya arşivi ayarları (media-archive.ini) okunamadı: ");
        radios_media_archive_settings_clear(&proposed);
        return FALSE;
    }
    radios_media_archive_settings_clear(out);
    *out = proposed;
    return TRUE;
}

gboolean radios_media_archive_settings_save(const gchar *project_root, const RadioSMediaArchiveSettings *settings, GError **error)
{
    g_return_val_if_fail(project_root && settings, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (!radios_media_archive_settings_validate(settings, error)) return FALSE;
    GKeyFile *file = g_key_file_new();
    g_key_file_set_integer(file, "archive", "version", 1);
    guint count = settings->folders->len;
    if (count) {
        const gchar **list = g_new0(const gchar *, count + 1);
        for (guint i = 0; i < count; ++i) list[i] = g_ptr_array_index(settings->folders, i);
        g_key_file_set_string_list(file, "archive", "folders", list, count);
        g_free(list);
    }
    g_key_file_set_string(file, "archive", "kind", settings->kind);
    g_key_file_set_boolean(file, "archive", "folder_playlists", settings->folder_playlists);
    g_key_file_set_boolean(file, "archive", "watch", settings->watch);
    g_key_file_set_integer(file, "archive", "watch_minutes", (gint)settings->watch_minutes);
    gchar *data = g_key_file_to_data(file, NULL, NULL);
    g_key_file_unref(file);
    gboolean saved = radios_local_ini_write(project_root, SETTINGS_FILE, data, error);
    g_free(data);
    return saved;
}

/* ------------------------------------------------------------------- folders */

RadioSMediaArchiveFolderState radios_media_archive_folder_state(const gchar *folder)
{
    GStatBuf info;
    if (!folder || g_stat(folder, &info) != 0) return RADIOS_MEDIA_ARCHIVE_FOLDER_MISSING;
    if (!S_ISDIR(info.st_mode)) return RADIOS_MEDIA_ARCHIVE_FOLDER_NOT_DIRECTORY;
    return access(folder, R_OK | X_OK) == 0 ? RADIOS_MEDIA_ARCHIVE_FOLDER_READY : RADIOS_MEDIA_ARCHIVE_FOLDER_UNREADABLE;
}

const gchar *radios_media_archive_folder_state_label(RadioSMediaArchiveFolderState state)
{
    switch (state) {
    case RADIOS_MEDIA_ARCHIVE_FOLDER_READY: return "Hazır";
    case RADIOS_MEDIA_ARCHIVE_FOLDER_MISSING: return "Bulunamadı (disk takılı mı?)";
    case RADIOS_MEDIA_ARCHIVE_FOLDER_NOT_DIRECTORY: return "Klasör değil";
    case RADIOS_MEDIA_ARCHIVE_FOLDER_UNREADABLE: return "Okunamıyor (izin yok)";
    }
    return "?";
}

gboolean radios_media_archive_is_audio_name(const gchar *name)
{
    if (!name || !*name) return FALSE;
    gchar *lower = g_ascii_strdown(name, -1);
    gboolean match = FALSE;
    for (guint i = 0; audio_extensions[i] && !match; ++i) match = g_str_has_suffix(lower, audio_extensions[i]);
    g_free(lower);
    return match;
}

void radios_media_archive_file_free(gpointer value)
{
    RadioSMediaArchiveFile *file = value;
    if (!file) return;
    g_free(file->path); g_free(file->folder); g_free(file->group); g_free(file);
}

/* --------------------------------------------------------------------- scan */

typedef struct { gint64 size, mtime_us; gchar sha256[65]; } IndexEntry;

typedef struct {
    GPtrArray *files;
    GHashTable *seen;
    RadioSMediaArchiveScanStats *stats;
    const gchar *folder;
    GCancellable *cancellable;
} Walk;

static gint64 mtime_us(const GStatBuf *info)
{ return (gint64)info->st_mtim.tv_sec * G_USEC_PER_SEC + (gint64)info->st_mtim.tv_nsec / 1000; }

static gint compare_names(gconstpointer a, gconstpointer b)
{ return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b); }

static void walk_directory(Walk *walk, const gchar *directory, guint depth, const gchar *group)
{
    GDir *dir = g_dir_open(directory, 0, NULL);
    if (!dir) { if (depth) ++walk->stats->unreadable; return; }
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const gchar *name;
    while ((name = g_dir_read_name(dir))) if (name[0] != '.') g_ptr_array_add(names, g_strdup(name));
    g_dir_close(dir);
    g_ptr_array_sort(names, compare_names);
    for (guint i = 0; i < names->len; ++i) {
        if (g_cancellable_is_cancelled(walk->cancellable)) break;
        const gchar *entry = g_ptr_array_index(names, i);
        gchar *path = g_build_filename(directory, entry, NULL);
        GStatBuf info;
        if (g_lstat(path, &info) != 0) { ++walk->stats->unreadable; g_free(path); continue; }
        if (S_ISLNK(info.st_mode)) { ++walk->stats->skipped_links; g_free(path); continue; }
        if (S_ISDIR(info.st_mode)) {
            if (depth + 1 <= RADIOS_MEDIA_ARCHIVE_MAX_DEPTH) walk_directory(walk, path, depth + 1, depth == 0 ? entry : group);
            g_free(path); continue;
        }
        if (!S_ISREG(info.st_mode) || !radios_media_archive_is_audio_name(entry) || g_hash_table_contains(walk->seen, path)) {
            g_free(path); continue;
        }
        if (walk->files->len >= RADIOS_MEDIA_ARCHIVE_MAX_FILES) { walk->stats->truncated = TRUE; g_free(path); break; }
        RadioSMediaArchiveFile *file = g_new0(RadioSMediaArchiveFile, 1);
        file->path = path; file->folder = g_strdup(walk->folder); file->group = g_strdup(group);
        file->size = (gint64)info.st_size; file->mtime_us = mtime_us(&info);
        g_hash_table_add(walk->seen, g_strdup(path));
        g_ptr_array_add(walk->files, file);
    }
    g_ptr_array_unref(names);
}

static GHashTable *index_load(const gchar *path)
{
    GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GStatBuf info;
    if (!path || g_stat(path, &info) != 0 || !S_ISREG(info.st_mode) || (guint64)info.st_size > INDEX_LIMIT) return table;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_file(parser, path, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        JsonObject *object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
        JsonNode *rows = object ? json_object_get_member(object, "entries") : NULL;
        JsonNode *version = object ? json_object_get_member(object, "version") : NULL;
        if (rows && JSON_NODE_HOLDS_ARRAY(rows) && version && JSON_NODE_HOLDS_VALUE(version) && json_node_get_int(version) == 1) {
            JsonArray *array = json_node_get_array(rows);
            for (guint i = 0; i < json_array_get_length(array); ++i) {
                JsonNode *node = json_array_get_element(array, i);
                JsonObject *row = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
                if (!row || !json_object_has_member(row, "p") || !json_object_has_member(row, "h")) continue;
                JsonNode *p = json_object_get_member(row, "p"), *h = json_object_get_member(row, "h");
                JsonNode *s = json_object_get_member(row, "s"), *m = json_object_get_member(row, "m");
                if (!JSON_NODE_HOLDS_VALUE(p) || json_node_get_value_type(p) != G_TYPE_STRING ||
                    !JSON_NODE_HOLDS_VALUE(h) || json_node_get_value_type(h) != G_TYPE_STRING ||
                    !s || !JSON_NODE_HOLDS_VALUE(s) || !m || !JSON_NODE_HOLDS_VALUE(m)) continue;
                const gchar *digest = json_node_get_string(h);
                if (!digest || strlen(digest) != 64) continue;
                IndexEntry *entry = g_new0(IndexEntry, 1);
                entry->size = json_node_get_int(s); entry->mtime_us = json_node_get_int(m);
                g_strlcpy(entry->sha256, digest, sizeof entry->sha256);
                g_hash_table_replace(table, g_strdup(json_node_get_string(p)), entry);
            }
        }
    }
    g_object_unref(parser);
    return table;
}

static void index_save(const gchar *path, GPtrArray *files)
{
    if (!path) return;
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "version"); json_builder_add_int_value(builder, 1);
    json_builder_set_member_name(builder, "entries"); json_builder_begin_array(builder);
    for (guint i = 0; i < files->len; ++i) {
        const RadioSMediaArchiveFile *file = g_ptr_array_index(files, i);
        if (!file->sha256[0]) continue;
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "p"); json_builder_add_string_value(builder, file->path);
        json_builder_set_member_name(builder, "s"); json_builder_add_int_value(builder, file->size);
        json_builder_set_member_name(builder, "m"); json_builder_add_int_value(builder, file->mtime_us);
        json_builder_set_member_name(builder, "h"); json_builder_add_string_value(builder, file->sha256);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder); json_builder_end_object(builder);
    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gsize length = 0;
    gchar *text = json_generator_to_data(generator, &length);
    gchar *directory = g_path_get_dirname(path);
    g_mkdir_with_parents(directory, 0700);
    g_free(directory);
    /* The index is only a cache: a failed write costs one re-hash. */
    g_file_set_contents_full(path, text, (gssize)length, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
    g_free(text); json_node_unref(root); g_object_unref(generator); g_object_unref(builder);
}

static gboolean hash_file(const gchar *path, const GStatBuf *expected, GCancellable *cancellable,
                          gchar digest[65], GError **error)
{
    gint descriptor = g_open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0);
    if (descriptor < 0) { g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "%s: %s", path, g_strerror(errno)); return FALSE; }
    GStatBuf opened;
    if (fstat(descriptor, &opened) != 0 || !S_ISREG(opened.st_mode)) {
        close(descriptor); return fail(error, "%s: normal dosya değil.", path);
    }
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    guchar *buffer = g_malloc(HASH_CHUNK);
    gint64 total = 0;
    gboolean ok = TRUE;
    for (;;) {
        if (g_cancellable_is_cancelled(cancellable)) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Tarama iptal edildi."); ok = FALSE; break; }
        ssize_t amount = read(descriptor, buffer, HASH_CHUNK);
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) { g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "%s: %s", path, g_strerror(errno)); ok = FALSE; break; }
        if (amount == 0) break;
        g_checksum_update(checksum, buffer, amount); total += amount;
    }
    GStatBuf after;
    if (ok && (fstat(descriptor, &after) != 0 || after.st_size != expected->st_size || total != (gint64)expected->st_size ||
               mtime_us(&after) != mtime_us(expected)))
        ok = fail(error, "%s: okunurken değişti (kopyalama sürüyor olabilir).", path);
    if (ok) g_strlcpy(digest, g_checksum_get_string(checksum), 65);
    g_free(buffer); g_checksum_free(checksum); close(descriptor);
    return ok;
}

static gint compare_files(gconstpointer a, gconstpointer b)
{
    const RadioSMediaArchiveFile *x = *(RadioSMediaArchiveFile *const *)a, *y = *(RadioSMediaArchiveFile *const *)b;
    return g_strcmp0(x->path, y->path);
}

GPtrArray *radios_media_archive_scan(const gchar *const *folders, const gchar *index_path, GCancellable *cancellable,
    RadioSMediaArchiveProgress progress, gpointer user, RadioSMediaArchiveScanStats *stats, GError **error)
{
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    RadioSMediaArchiveScanStats local_stats = {0};
    if (!stats) stats = &local_stats;
    memset(stats, 0, sizeof *stats);
    GPtrArray *files = g_ptr_array_new_with_free_func(radios_media_archive_file_free);
    Walk walk = {files, g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), stats, NULL, cancellable};
    guint configured = 0;
    for (guint i = 0; folders && folders[i]; ++i) {
        ++configured;
        if (radios_media_archive_folder_state(folders[i]) != RADIOS_MEDIA_ARCHIVE_FOLDER_READY) { ++stats->folders_missing; continue; }
        walk.folder = folders[i];
        walk_directory(&walk, folders[i], 0, "");
    }
    g_hash_table_unref(walk.seen);
    gboolean cancelled = g_cancellable_is_cancelled(cancellable);
    if (!cancelled && configured && stats->folders_missing == configured) {
        fail(error, "Arşiv klasörlerinden hiçbiri okunamadı. Diskin takılı olduğundan emin olun.");
        g_ptr_array_unref(files); return NULL;
    }
    g_ptr_array_sort(files, compare_files);
    GHashTable *index = cancelled ? NULL : index_load(index_path);
    guint total = files->len;
    if (progress && !cancelled && !progress(0, total, "", user)) cancelled = TRUE;
    for (guint i = 0; i < files->len && !cancelled; ++i) {
        RadioSMediaArchiveFile *file = g_ptr_array_index(files, i);
        IndexEntry *known = g_hash_table_lookup(index, file->path);
        GError *hash_error = NULL;
        if (known && known->size == file->size && known->mtime_us == file->mtime_us) {
            g_strlcpy(file->sha256, known->sha256, sizeof file->sha256);
        } else {
            GStatBuf info;
            if (g_lstat(file->path, &info) != 0 || !S_ISREG(info.st_mode) || (gint64)info.st_size != file->size) {
                ++stats->unreadable;
            } else if (hash_file(file->path, &info, cancellable, file->sha256, &hash_error)) {
                ++stats->hashed;
            } else {
                cancelled = g_error_matches(hash_error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
                if (!cancelled) ++stats->unreadable;
                file->sha256[0] = '\0';
            }
        }
        g_clear_error(&hash_error);
        if (progress && !progress(i + 1, total, file->path, user)) cancelled = TRUE;
    }
    if (cancelled) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Tarama iptal edildi.");
        if (index) g_hash_table_unref(index);
        g_ptr_array_unref(files); return NULL;
    }
    /* Failed files are dropped from the result; the rest keep their digests. */
    for (guint i = files->len; i > 0; --i) {
        RadioSMediaArchiveFile *file = g_ptr_array_index(files, i - 1);
        if (!file->sha256[0]) { g_ptr_array_remove_index(files, i - 1); continue; }
        stats->bytes += (guint64)file->size;
    }
    stats->files = files->len;
    index_save(index_path, files);
    g_hash_table_unref(index);
    return files;
}

/* ------------------------------------------------------------------ library */

static const gchar *string_of(JsonObject *object, const gchar *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}

GHashTable *radios_media_archive_library_digests(RadioSDataClient *client, GCancellable *cancellable,
                                                 guint *records, GError **error)
{
    g_return_val_if_fail(client != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    GHashTable *digests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint seen = 0;
    gint64 offset = 0;
    for (guint page = 0; page < 20000; ++page) {
        JsonNode *body = json_node_new(JSON_NODE_OBJECT);
        JsonObject *request = json_object_new(); json_node_take_object(body, request);
        json_object_set_string_member(request, "station_id", radios_data_client_station_id(client));
        json_object_set_string_member(request, "query", "");
        json_object_set_int_member(request, "limit", 200);
        json_object_set_int_member(request, "offset", offset);
        JsonNode *result = radios_data_client_request_cancellable_sync(client, "POST", "/v1/workflows/desktop-media/list",
                                                                      body, NULL, cancellable, error);
        json_node_free(body);
        if (!result) { g_hash_table_unref(digests); return NULL; }
        JsonObject *object = JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
        JsonNode *rows = object ? json_object_get_member(object, "records") : NULL;
        if (!rows || !JSON_NODE_HOLDS_ARRAY(rows)) {
            fail(error, "Medya kütüphanesi yanıtı geçersiz."); json_node_free(result); g_hash_table_unref(digests); return NULL;
        }
        JsonArray *array = json_node_get_array(rows);
        for (guint i = 0; i < json_array_get_length(array); ++i) {
            JsonObject *row = json_array_get_object_element(array, i);
            JsonNode *version = row ? json_object_get_member(row, "version") : NULL;
            const gchar *digest = version && JSON_NODE_HOLDS_OBJECT(version) ? string_of(json_node_get_object(version), "sha256") : NULL;
            ++seen;
            if (digest && strlen(digest) == 64) g_hash_table_add(digests, g_strdup(digest));
        }
        JsonNode *next = json_object_get_member(object, "next_offset");
        gboolean more = next && JSON_NODE_HOLDS_VALUE(next) && json_node_get_value_type(next) == G_TYPE_INT64 &&
                        json_node_get_int(next) > offset && json_array_get_length(array) > 0;
        if (more) offset = json_node_get_int(next);
        json_node_free(result);
        if (!more) break;
    }
    if (records) *records = seen;
    return digests;
}

GPtrArray *radios_media_archive_new_files(GPtrArray *files, GHashTable *library, guint *duplicates)
{
    GPtrArray *fresh = g_ptr_array_new();
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    guint repeated = 0;
    for (guint i = 0; files && i < files->len; ++i) {
        RadioSMediaArchiveFile *file = g_ptr_array_index(files, i);
        if (!file->sha256[0] || (library && g_hash_table_contains(library, file->sha256))) continue;
        if (g_hash_table_contains(seen, file->sha256)) { ++repeated; continue; }
        g_hash_table_add(seen, file->sha256);
        g_ptr_array_add(fresh, file);
    }
    g_hash_table_unref(seen);
    if (duplicates) *duplicates = repeated;
    return fresh;
}
