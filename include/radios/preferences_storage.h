#ifndef RADIOS_PREFERENCES_STORAGE_H
#define RADIOS_PREFERENCES_STORAGE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    guint sample_rate;
    guint buffer_frames;
    guint click_sound;  /* 0..2 correspond to Click Sound 01..03. */
    guint click_volume; /* 0 Small, 1 Middle, 2 Large. */
} RadioSPreferencesAudio;

/* Panel settings are distinct from the engine's internal 48 kHz PCM format.
 * Callers must apply output settings to the engine before reporting success. */
void radios_preferences_audio_default(RadioSPreferencesAudio *preferences);
gboolean radios_preferences_audio_validate(const RadioSPreferencesAudio *preferences,
                                           GError **error);

/* Only config/audio-panel.ini is accessed. The config directory must exist.
 * A missing file returns defaults {44100, 256, 0, 1} without writing. Invalid
 * files leave out unchanged. Reads are bounded to 8 KiB; file links and config
 * links escaping the project are rejected. Save commits by atomic replacement
 * and creates the file private (owner-only ACL, the Windows form of 0600). No DB, identity or general UI preference is changed. */
gboolean radios_preferences_audio_load(const gchar *project_root,
                                      RadioSPreferencesAudio *out, GError **error);
gboolean radios_preferences_audio_save(const gchar *project_root,
                                      const RadioSPreferencesAudio *preferences,
                                      GError **error);

G_END_DECLS

#endif
