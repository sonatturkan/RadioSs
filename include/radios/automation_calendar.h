#ifndef RADIOS_AUTOMATION_CALENDAR_H
#define RADIOS_AUTOMATION_CALENDAR_H
#include "radios/views.h"
#include <json-glib/json-glib.h>
G_BEGIN_DECLS
GtkWidget *radios_automation_calendar_new(RadioSViewContext *ctx, GtkWidget *legacy_rules);
typedef void (*RadioSAutomationCalendarChanged)(GtkWidget *page, JsonNode *snapshot, gpointer user);
/* Detached production calendar editor. The parent owns persistence and receives
 * borrowed full snapshots; no automation_calendar/main write is made here. */
GtkWidget *radios_automation_calendar_editor_new(RadioSViewContext *ctx,
    JsonObject *snapshot, RadioSAutomationCalendarChanged changed, gpointer user);
void radios_automation_calendar_set_snapshot(GtkWidget *page, JsonObject *snapshot);
/* Owned existing AutoDJ defaults; used by the calendar's verified preparation. */
JsonNode *radios_automation_rules_configuration(GtkWidget *legacy_rules);
/* Select a visible week column, using minute coordinates. extend adds saved
 * blocks to the current multiple selection. No write or playback is started. */
void radios_automation_calendar_select(GtkWidget *page, guint day,
    guint start_minute, guint end_minute, gboolean extend);
/* False while visible block properties contain incomplete/invalid input. */
gboolean radios_automation_calendar_snapshot_ready(GtkWidget *page);
/* Bind a separately persisted saved programme to the one visible grid.
 * NULL callback restores the preserved main calendar. Invalid drafts block
 * switching. Global ACKs never replace a bound programme's in-progress edit. */
gboolean radios_automation_calendar_bind_editor(GtkWidget *page,
    RadioSAutomationCalendarChanged changed, gpointer user, const gchar *title);
/* Owned main calendar even while a saved programme occupies the same grid. */
JsonNode *radios_automation_calendar_main_snapshot(GtkWidget *page);
/* Owned current calendar configuration, including unsaved edits. */
JsonNode *radios_automation_calendar_snapshot(GtkWidget *page);
/* Flush coalesced edits and checkpoint the exact pending CAS before closing data. */
void radios_automation_calendar_prepare_close(GtkWidget *page);
gboolean radios_automation_calendar_create_player_schema(RadioSViewContext *ctx,
    const gchar *playlist_id, GError **error);
G_END_DECLS
#endif
