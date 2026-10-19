#ifndef RADIOS_MEDIA_ARCHIVE_H
#define RADIOS_MEDIA_ARCHIVE_H

#include <gio/gio.h>
#include "radios/data_client.h"

G_BEGIN_DECLS

/* Ayarlar › Medya arşivi: the folders the station's audio archive is read
 * from. The workstation file config/media-archive.ini holds them (0600,
 * atomic replacement, links rejected). Source files are only ever read. */
#define RADIOS_MEDIA_ARCHIVE_MAX_FOLDERS 16u
#define RADIOS_MEDIA_ARCHIVE_MAX_FILES 50000u
#define RADIOS_MEDIA_ARCHIVE_MAX_DEPTH 16u

typedef struct {
    GPtrArray *folders;        /* owned canonical absolute folder paths */
    gchar *kind;               /* library kind of new imports ("music") */
    gboolean folder_playlists; /* add imports to a playlist per subfolder */
    gboolean watch;            /* import new files while RadioS runs */
    guint watch_minutes;       /* 5..1440 */
} RadioSMediaArchiveSettings;

void radios_media_archive_settings_init(RadioSMediaArchiveSettings *settings);
void radios_media_archive_settings_clear(RadioSMediaArchiveSettings *settings);
void radios_media_archive_settings_copy(RadioSMediaArchiveSettings *destination,
                                        const RadioSMediaArchiveSettings *source);
gboolean radios_media_archive_kind_is_valid(const gchar *kind);
/* Folders must be absolute, distinct and not inside one another. */
gboolean radios_media_archive_settings_validate(const RadioSMediaArchiveSettings *settings,
                                                GError **error);
/* A missing file yields the defaults without writing. */
gboolean radios_media_archive_settings_load(const gchar *project_root,
                                            RadioSMediaArchiveSettings *out, GError **error);
gboolean radios_media_archive_settings_save(const gchar *project_root,
                                            const RadioSMediaArchiveSettings *settings,
                                            GError **error);

typedef enum {
    RADIOS_MEDIA_ARCHIVE_FOLDER_READY,
    RADIOS_MEDIA_ARCHIVE_FOLDER_MISSING,
    RADIOS_MEDIA_ARCHIVE_FOLDER_NOT_DIRECTORY,
    RADIOS_MEDIA_ARCHIVE_FOLDER_UNREADABLE
} RadioSMediaArchiveFolderState;

RadioSMediaArchiveFolderState radios_media_archive_folder_state(const gchar *folder);
const gchar *radios_media_archive_folder_state_label(RadioSMediaArchiveFolderState state);

/* WAV, MP3, FLAC, Ogg (OGG/OGA/OPUS), M4A and AAC by file name. */
gboolean radios_media_archive_is_audio_name(const gchar *name);

typedef struct {
    gchar *path;      /* canonical absolute path */
    gchar *folder;    /* configured archive folder it was found under */
    gchar *group;     /* first folder below the archive folder; "" at the top */
    gint64 size;
    gint64 mtime_us;
    gchar sha256[65]; /* lowercase digest */
} RadioSMediaArchiveFile;

typedef struct {
    guint folders_missing; /* configured folders that could not be opened */
    guint files;           /* audio files with a digest */
    guint hashed;          /* digests computed now (the rest came from the index) */
    guint unreadable;      /* audio files that could not be read */
    guint skipped_links;   /* symbolic links that were not followed */
    gboolean truncated;    /* RADIOS_MEDIA_ARCHIVE_MAX_FILES reached */
    guint64 bytes;
} RadioSMediaArchiveScanStats;

/* Runs on the scanning thread; FALSE stops the scan as a cancellation. */
typedef gboolean (*RadioSMediaArchiveProgress)(guint done, guint total, const gchar *path, gpointer user);

void radios_media_archive_file_free(gpointer file);

/* Walks the folders without following symbolic links, skipping hidden
 * entries, to RADIOS_MEDIA_ARCHIVE_MAX_DEPTH levels. A digest is reused from
 * index_path (a cache, may be NULL) while size and modification time match;
 * the rest are hashed and the index is rewritten atomically with only the
 * files seen now. Returns RadioSMediaArchiveFile* sorted by path, or NULL on
 * cancellation or when no folder could be read. */
GPtrArray *radios_media_archive_scan(const gchar *const *folders, const gchar *index_path,
    GCancellable *cancellable, RadioSMediaArchiveProgress progress, gpointer user,
    RadioSMediaArchiveScanStats *stats, GError **error);

/* SHA-256 digests (owned strings) of every active library record, read page by
 * page from desktop-media/list. Blocking; for worker threads. */
GHashTable *radios_media_archive_library_digests(RadioSDataClient *client, GCancellable *cancellable,
                                                 guint *records, GError **error);

/* Files whose digest is neither in the library nor earlier in the list. */
GPtrArray *radios_media_archive_new_files(GPtrArray *files, GHashTable *library, guint *duplicates);

/* The playlist purpose matching a library kind. */
const gchar *radios_media_archive_playlist_purpose(const gchar *kind);

G_END_DECLS

#endif
