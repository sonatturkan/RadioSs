#ifndef RADIOS_SETTINGS_SYNC_H
#define RADIOS_SETTINGS_SYNC_H
#include "radios/views.h"
#include <json-glib/json-glib.h>
#include "radios/ui_preferences.h"
#include "radios/preferences_storage.h"

/* Workstation preferences (config/ui-preferences.ini and config/audio-panel.ini)
 * mirrored to the station's "desktop_preferences" document. The database copy
 * wins when it loads; every local change is saved back automatically. */
void radios_settings_sync_bind(RadioSViewContext *ctx, GtkWidget *owner);
/* Writes both files, applies them to the open window (text size, content
 * scale, start profile, header fields, output format) and saves them to the database. */
gboolean radios_settings_sync_apply(RadioSViewContext *ctx, const RadioSUIPreferences *ui,
    const RadioSPreferencesAudio *audio, GError **error);
/* .st workstation settings: fixed basenames, bounded UTF-8 INI, no caller paths.
 * Missing files are JSON null and restore application defaults. */
JsonObject *radios_settings_local_export(const gchar *project_root, GError **error);
gboolean radios_settings_local_import(RadioSViewContext *ctx, JsonObject *files, GError **error);
gboolean radios_settings_local_reset(RadioSViewContext *ctx, GError **error);

/* The main window applies text size, content scale and start profile. */
typedef void (*RadioSSettingsWindowApply)(gpointer data, const RadioSUIPreferences *ui);
void radios_settings_sync_set_window_hook(GtkWindow *window, RadioSSettingsWindowApply apply, gpointer data);

/* Named views: complete RadioSUIPreferences sets (text size, start profile,
 * hidden header fields, workspace and mixer/effector scales) saved under a
 * name in the same "desktop_preferences" document, as
 * views: [{name, ui: {...}}] and active_view (a name or null). At most
 * RADIOS_SETTINGS_VIEWS_MAX views; a name has 1..RADIOS_SETTINGS_VIEW_NAME_MAX
 * characters, no surrounding space or control characters, and is unique.
 * The views live only in the database: config/ui-preferences.ini keeps the
 * active settings alone. active_view names the view whose settings are shown;
 * it is cleared once the shown settings change. Changing views needs the
 * document to be loaded (available). */
#define RADIOS_SETTINGS_VIEWS_MAX 16
#define RADIOS_SETTINGS_VIEW_NAME_MAX 40
gboolean radios_settings_views_available(RadioSViewContext *ctx);
/* NULL-terminated names in saved order (g_strfreev); *active is a copy or NULL. */
gchar **radios_settings_views_list(RadioSViewContext *ctx, gchar **active);
gboolean radios_settings_views_get(RadioSViewContext *ctx, const gchar *name, RadioSUIPreferences *out);
/* Saves the current workstation settings as @name: a new view, or the
 * existing view of that name overwritten. The saved view becomes active. */
gboolean radios_settings_views_save(RadioSViewContext *ctx, const gchar *name, GError **error);
/* Applies the view to the files and the open window and makes it active. */
gboolean radios_settings_views_apply(RadioSViewContext *ctx, const gchar *name, GError **error);
gboolean radios_settings_views_delete(RadioSViewContext *ctx, const gchar *name, GError **error);
/* Called after the views, the active view or their availability change,
 * while @owner lives. FALSE: this window has no database document at all. */
typedef void (*RadioSSettingsViewsChanged)(gpointer data);
gboolean radios_settings_views_watch(RadioSViewContext *ctx, GtkWidget *owner, RadioSSettingsViewsChanged changed, gpointer data);
#endif
