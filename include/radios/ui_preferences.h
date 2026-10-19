#ifndef RADIOS_UI_PREFERENCES_H
#define RADIOS_UI_PREFERENCES_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    RADIOS_HEADER_HIDE_PROGRAMME = 1,
    RADIOS_HEADER_HIDE_TRACK = 2,
    RADIOS_HEADER_HIDE_REMAINING = 4
} RadioSHeaderHiddenField;

typedef struct {
    guint text_size;       /* 0 compact, 1 standard, 2 large */
    guint default_profile; /* 0 last used, 1 broadcast ... 8 routing */
    guint last_profile;    /* 0 broadcast ... 7 routing */
    guint header_hidden_fields; /* 0 shows all; RadioSHeaderHiddenField bitmask */
    guint content_scale_percent; /* 50..150; scales only content below header */
    guint mixer_scale_percent; /* 50..150; mixer width: scales its horizontal lengths only */
    guint effector_scale_percent; /* 50..150; effector width: scales its horizontal lengths only */
    guint mixer_height_percent; /* 50..150; mixer height: scales its vertical lengths only */
    guint effector_height_percent; /* 50..150; effector height: scales its vertical lengths only */
} RadioSUIPreferences;

/* Only config/ui-preferences.ini is read/written. config must already exist.
 * Missing files load defaults {1, 0, 0, 0, 100, 100, 100, 100, 100}; version=1
 * files without header_hidden_fields show all fields, and each missing scale
 * key means 100 percent, except that a missing mixer/effector height key
 * follows its width key (files from before the separate height settings kept
 * one uniform scale). Errors leave out unchanged.
 * Files are limited to 8 KiB; external directory links and all file links are
 * rejected. Save replaces the file atomically with private (0600) permissions.
 * No application config, credentials, database or audio state is modified. */
void radios_ui_preferences_default(RadioSUIPreferences *out);
gboolean radios_ui_preferences_load(const gchar *project_root,
                                   RadioSUIPreferences *out, GError **error);
gboolean radios_ui_preferences_save(const gchar *project_root,
                                   const RadioSUIPreferences *preferences,
                                   GError **error);

G_END_DECLS

#endif
