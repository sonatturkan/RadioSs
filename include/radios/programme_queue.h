#ifndef RADIOS_PROGRAMME_QUEUE_H
#define RADIOS_PROGRAMME_QUEUE_H

#include <gtk/gtk.h>
#include "radios/programme_model.h"
#include "radios/playout_ui.h"

/* Drag and drop inside RadioS: catalogue rows, flow rows and player cards. */
#define RADIOS_DRAG_ROW "application/x-radios-programme-row"   /* flow item id */
#define RADIOS_DRAG_MEDIA "application/x-radios-media"         /* catalogue media record JSON */
#define RADIOS_DRAG_PROGRAMME "application/x-radios-programme" /* saved programme record JSON */
#define RADIOS_DRAG_PLAYLIST "application/x-radios-playlist"
typedef enum {
    RADIOS_DRAG_INFO_ROW = 1, RADIOS_DRAG_INFO_MEDIA, RADIOS_DRAG_INFO_PROGRAMME,
    RADIOS_DRAG_INFO_PLAYLIST
} RadioSDragInfo;
const GtkTargetEntry *radios_drag_targets(guint *count);
void radios_drag_set_payload(GtkSelectionData *selection, const gchar *payload);
/* Owned copy of a received payload, or NULL when empty. */
gchar *radios_drag_payload(GtkSelectionData *selection);

/* Where an insert or drop lands: inside block at position, or a new block at
 * root position when block is negative. */
typedef struct { gint block; guint position; } RadioSProgrammePlacement;

/* Row controls are S · V · R · L · × (R in the former Ö place, L next to
 * it). Ö (properties) has no button: the
 * action stays reachable from the row's context menu. L toggles the event's
 * live flow; R is push-to-talk (press/release) and never advances the flow. */
typedef enum {
    RADIOS_PROGRAMME_ROW_PLAY, RADIOS_PROGRAMME_ROW_WAVEFORM,
    RADIOS_PROGRAMME_ROW_PROPERTIES, RADIOS_PROGRAMME_ROW_REMOVE,
    RADIOS_PROGRAMME_ROW_LIVE, RADIOS_PROGRAMME_ROW_TALK_PRESS, RADIOS_PROGRAMME_ROW_TALK_RELEASE
} RadioSProgrammeRowAction;

typedef struct {
    gpointer data;
    /* The operator selected an event or content row (id), or cleared the
     * selection: id is NULL after Esc in the queue, a click on the list's
     * empty area, a second click on the selected content row or the list
     * box's own unselect. With NULL the page shows the sounding content's
     * remaining time again instead of the selected item's duration (PDF s.8). */
    void (*selected)(gpointer data, const gchar *id);
    void (*action)(gpointer data, const gchar *id, RadioSProgrammeRowAction action);
    /* payload is the flow item id for RADIOS_DRAG_INFO_ROW. */
    gboolean (*drop)(gpointer data, RadioSDragInfo info, const gchar *payload, RadioSProgrammePlacement placement);
} RadioSProgrammeQueueHooks;

/* The "No · Event · Süre · Kontroller" list of the broadcast flow, named
 * programme-queue-table; its GtkListBox is object data "list-box". */
GtkWidget *radios_programme_queue_new(const RadioSProgrammeQueueHooks *hooks);
/* Rebuilds the rows from a programme config and selects selected. Music leaves
 * may carry accepted planned_jingles [{title, placement: overlay|before|after}];
 * each becomes the song's own 1.1 sub-row (name, kind and relation); other
 * placements are ignored. */
void radios_programme_queue_render(GtkWidget *queue, JsonObject *config, const gchar *selected);
/* The sounding leaf and its state; NULL when the flow is not playing. */
void radios_programme_queue_set_playing(GtkWidget *queue, const gchar *leaf, gboolean paused);
/* The next leaf uses the sounding leaf's measured progress (an accelerating
 * black-to-white fade); a zero/unknown duration stays black. Pause freezes it. */
void radios_programme_queue_set_progress(GtkWidget *queue, const gchar *playing,
    const gchar *next, guint64 position_ms, guint64 duration_ms, gboolean paused);
/* Deep-copies the accepted run's event ownership and boundaries. Draft edits
 * and render calls cannot retarget its completion state. NULL clears it. */
void radios_programme_queue_set_run_config(GtkWidget *queue, JsonObject *accepted_config);
/* Call only for a genuine COMPLETE receipt. An event's final playable leaf
 * gives it a stable finished state and clears its active indication; duplicate
 * receipts are harmless. Completion never starts a blinking warning. */
void radios_programme_queue_complete(GtkWidget *queue, const gchar *leaf);
/* Separate display-only badges for independent event runs, keyed by
 * origin_block_id. The array is borrowed; matching rows are deeply copied.
 * This never changes the normal queue's playing leaf or completion state. */
void radios_programme_queue_set_event_runs(GtkWidget *queue, JsonArray *runs);
/* A genuine independent-event terminal receipt. Retains at most 64 labels;
 * a new run, event removal or reset clears its previous terminal label. */
void radios_programme_queue_complete_event_run(GtkWidget *queue, const gchar *origin, guint source);
/* Clears transient progress, the frozen run and completed-event state. */
void radios_programme_queue_reset_progress(GtkWidget *queue);
/* Each row shows its player(s) at the right. A leaf's actual player (1..32,
 * recorded while it sounds) wins over a leaf/accepted-plan "player" member,
 * which wins over default_player (0: automation chooses). names holds 32
 * display names or NULL; it is copied. */
void radios_programme_queue_set_player(GtkWidget *queue, const gchar *leaf, guint player);
void radios_programme_queue_set_player_context(GtkWidget *queue, guint default_player, const gchar *const *names);
void radios_programme_queue_set_expanded(GtkWidget *queue, const gchar *block, gboolean expanded);
/* Item id of a list row, NULL for an empty-block row. */
const gchar *radios_programme_queue_row_id(GtkListBoxRow *row);

/* ---- Kılavuz 2026-10-18 · yayın sırası görünümü ------------------------- */
/* Display window from the current position (PDF §06 "gösterme adedi"):
 * at most event_show main event rows in the event view and content_show
 * content rows in an opened event; 0 means unlimited. Rows of completed
 * events/contents leave the active list; the programme itself is unchanged.
 * A transition keeps both its rows inside the window. */
void radios_programme_queue_set_window(GtkWidget *queue, guint event_show, guint content_show);
/* A transition in progress from source_leaf to target_leaf: both rows (and
 * their events in the event view) show the green G instead of S until the
 * source completes. NULL/NULL clears. */
void radios_programme_queue_set_transition(GtkWidget *queue, const gchar *source_leaf, const gchar *target_leaf);
/* The active event enters its end/transition-preparation zone: only that
 * event's area blinks; FALSE (or completion) clears the warning. */
void radios_programme_queue_set_ending(GtkWidget *queue, const gchar *event_id, gboolean approaching);
/* Live state of an event row: L lit while its live flow is active; R shown
 * held while push-to-talk is pressed. */
void radios_programme_queue_set_live(GtkWidget *queue, const gchar *event_id, gboolean live_active, gboolean talk_held);
/* R is pressable only for the current on-air event (the live step the flow
 * waits at, or the event of the sounding content); other rows keep it
 * insensitive. NULL event clears every row. */
void radios_programme_queue_set_talk_available(GtkWidget *queue, const gchar *event_id);
/* The live step the flow is waiting at (on air); NULL when none. Its event
 * row reads like the sounding row: is-playing, S lit and "● Yayında", with or
 * without the L latch. Reaching a finished step again revives it; a limited
 * event view keeps it in the window. reset_progress keeps this state: the
 * caller clears it when the flow leaves the step. */
void radios_programme_queue_set_live_step(GtkWidget *queue, const gchar *event_id);
/* The flow finished that live step (the outer order advanced): its row is a
 * finished event ("✓ Tamamlandı"), which a limited view drops before filling
 * up again (PDF s.11). It also ends the step's on-air and ending state.
 * reset_progress clears it with the other finished events. */
void radios_programme_queue_complete_live_step(GtkWidget *queue, const gchar *event_id);
/* Run-time label of a live row's subtitle: "dj", "talk" or "music", from the
 * mixer channels bound at run time (PDF s.4 step 5); NULL or "" returns to
 * the event's own live.label. The row text is set only when it changes. */
void radios_programme_queue_set_live_label(GtkWidget *queue, const gchar *event_id, const gchar *label);
/* A planned jingle/sweeper of a music content (its 1.1 sub-row in the
 * opened event) is ready, playing, paused or waiting. */
void radios_programme_queue_set_overlay(GtkWidget *queue, const gchar *leaf_id, const gchar *title, RadioSPlayoutContentState state);
void radios_programme_queue_set_overlay_identity(GtkWidget *queue, const gchar *leaf_id,
    const gchar *overlay_id, const gchar *title, RadioSPlayoutContentState state);
/* Row kinds for tests and callers: a main row, a sub-row (1.1) attached to
 * the main row above it, or a note row. */
typedef enum { RADIOS_PROGRAMME_ROW_MAIN, RADIOS_PROGRAMME_ROW_SUB, RADIOS_PROGRAMME_ROW_NOTE } RadioSProgrammeRowLevel;
RadioSProgrammeRowLevel radios_programme_queue_row_level(GtkListBoxRow *row);
/* The displayed number of a row ("1", "1.1"); borrowed, NULL for note rows. */
const gchar *radios_programme_queue_row_number(GtkListBoxRow *row);

#endif
