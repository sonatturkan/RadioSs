#ifndef RADIOS_PLAYER_ROTATION_H
#define RADIOS_PLAYER_ROTATION_H

#include "radios/data.h"

/* All arguments are borrowed for this callback. The prepared response is a
 * finite (at most 200 entries), authenticated named-rotation snapshot.
 * Loading never starts audio. Copy prepared and ref client to keep them. */
typedef void (*RadioSPlayerRotationDone)(JsonObject *prepared,
    const gchar *document_id, gint64 document_revision,
    const gchar *rotation_id, const gchar *rotation_name,
    RadioSDataClient *client, gpointer user);

/* Real saved definitions from automation_calendar/main. NULL if unavailable.
 * done runs only while owner is alive, before the returned dialog is destroyed.
 * Stable controls: player-rotation-dialog/list/preview/status/reload. */
GtkWidget *radios_player_rotation_open(RadioSViewContext *ctx, GtkWidget *owner,
    RadioSPlayerRotationDone done, gpointer user);

/* Schemas from Main and every active saved weekly programme. The current
 * playlist is moved/inserted first in a read-only preparation clone. Document
 * kind/id/revision are pinned per selected row and retained for revalidation. */
GtkWidget *radios_player_rotation_open_schema(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *current_playlist_id, RadioSPlayerRotationDone done, gpointer user);

/* Re-read the exact active definition/revision, then freshly authenticate the
 * loaded programme and merge only unchanged media/cue identities. The caller
 * must still fence its card assignment/generation before starting playback.
 * The client anchor must remain the current connected authenticated client.
 * destroy(user) runs exactly once, including a synchronous FALSE return.
 * An accepted operation calls callback only while owner remains alive; final
 * callback data is borrowed and destroy runs after it. This never plays audio. */
gboolean radios_player_rotation_prepare_loaded(RadioSViewContext *ctx,
    GtkWidget *owner, JsonObject *prepared, const gchar *document_id,
    gint64 document_revision, const gchar *rotation_id, RadioSDataClient *client,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy);

/* A saved complete UTC programme is distinct from a finite named rotation.
 * Picker and loader callbacks borrow their validated record and client. */
typedef void (*RadioSPlayerWeeklyDone)(JsonObject *record, RadioSDataClient *client, gpointer user);
typedef struct RadioSPlayerWeekly RadioSPlayerWeekly;
typedef enum {
    RADIOS_PLAYER_WEEKLY_EMPTY, RADIOS_PLAYER_WEEKLY_LOADED,
    RADIOS_PLAYER_WEEKLY_WAITING, RADIOS_PLAYER_WEEKLY_PREPARING,
    RADIOS_PLAYER_WEEKLY_PLAYING, RADIOS_PLAYER_WEEKLY_FINISHED,
    RADIOS_PLAYER_WEEKLY_ERROR
} RadioSPlayerWeeklyState;
typedef struct {
    RadioSPlayerWeeklyState state;
    gchar programme_name[801], current_event_name[801], next_event_name[801], error[256];
    gint64 start_utc_us, end_utc_us, next_start_utc_us, remaining_us;
    gboolean repeat, armed, paused;
    /* Kılavuz §02/§03: the schema applied to the running general programme
     * ("Mevcut şemayı uygula") and the programme_entry source order handed to
     * the planner for the current preparation ("Giriş: N. sıra", 0 = none). */
    gchar schema_name[161], entry_text[64];
    guint entry_source_order;
} RadioSPlayerWeeklySnapshot;
GtkWidget *radios_player_weekly_open(RadioSViewContext *ctx, GtkWidget *owner,
    RadioSPlayerWeeklyDone done, gpointer user);
RadioSPlayerWeekly *radios_player_weekly_new(RadioSViewContext *ctx,
    GtkWidget *owner, guint source);
/* The general owner discovers the nearest future main-calendar or saved weekly
 * slot while idle. Projection is borrowed and never persists an editor draft.
 * running=TRUE identifies a freshly authenticated accepted runner batch. */
typedef void (*RadioSGeneralProgrammeQueue)(JsonObject *programme, gboolean running, gpointer user);
RadioSPlayerWeekly *radios_player_weekly_new_general(RadioSViewContext *ctx, GtkWidget *owner);
void radios_player_weekly_general_set_queue(RadioSPlayerWeekly *weekly,
    RadioSGeneralProgrammeQueue callback, gpointer user);
void radios_player_weekly_general_refresh(RadioSPlayerWeekly *weekly);
gboolean radios_player_weekly_start_general(RadioSPlayerWeekly *weekly, GError **error);
/* action uses the RadioSPlayoutAction values; waiting pause/stop are owned by
 * this driver even before a PCM runner exists. */
gboolean radios_player_weekly_general_action(RadioSPlayerWeekly *weekly, guint action, GError **error);
/* Kılavuz §02 "Mevcut şemayı uygula": apply a saved automation schema
 * (an automation_schema definition of the running record's own calendar) to
 * the general driver's current programme. The runtime slot keeps its record,
 * revision and identity fences; only its event gains the schema as the first
 * assignment (and live.automation_id). Every following preparation of that
 * slot uses the schema: a slot that owns no PCM yet re-prepares at once; a
 * running programme keeps the item on air while its remaining queue is
 * re-prepared with the schema and replaced at a transition-safe point
 * (radios_playout_ui_replace_remaining_prepared); any failure keeps the
 * current broadcast. Snapshot schema_name and entry_text/entry_source_order
 * report the result. Never starts audio. */
gboolean radios_player_weekly_general_apply_schema(RadioSPlayerWeekly *weekly,
    const gchar *schema_id, GError **error);
/* Loading validates and copies; it never starts audio or changes routing. */
gboolean radios_player_weekly_load(RadioSPlayerWeekly *weekly, JsonObject *record,
    RadioSDataClient *client, GError **error);
/* target may be G_MAXUINT for a single deck; an overlap needs a second deck
 * or the programme's explicit saved per-kind player distribution. */
gboolean radios_player_weekly_start(RadioSPlayerWeekly *weekly, guint target, GError **error);
/* A runtime override; turning repeat off finishes the current complete cycle. */
void radios_player_weekly_set_repeat(RadioSPlayerWeekly *weekly, gboolean repeat);
void radios_player_weekly_stop(RadioSPlayerWeekly *weekly);
gboolean radios_player_weekly_get_snapshot(RadioSPlayerWeekly *weekly,
    RadioSPlayerWeeklySnapshot *snapshot);
/* Only the driver's still-owned accepted MAIN intent may supply a fixed
 * current programme deadline. Future previews and replaced runs return FALSE. */
gboolean radios_player_weekly_header_snapshot(RadioSPlayerWeekly *weekly,
    RadioSPlayerWeeklySnapshot *snapshot);
void radios_player_weekly_free(RadioSPlayerWeekly *weekly);

#endif
