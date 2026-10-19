#ifndef RADIOS_PLAYOUT_UI_H
#define RADIOS_PLAYOUT_UI_H
#include "radios/data.h"
#include "radios/player_topology.h"

typedef struct RadioSPlayoutController RadioSPlayoutController;
typedef struct RadioSPlayoutResume RadioSPlayoutResume;
typedef enum {
    RADIOS_PLAYOUT_IDLE, RADIOS_PLAYOUT_PREPARING, RADIOS_PLAYOUT_SCHEDULED,
    RADIOS_PLAYOUT_PLAYING, RADIOS_PLAYOUT_OVERLAP, RADIOS_PLAYOUT_PAUSED,
    RADIOS_PLAYOUT_WAITING, RADIOS_PLAYOUT_MANUAL_WAIT,
    RADIOS_PLAYOUT_FINISHED, RADIOS_PLAYOUT_ERROR
} RadioSPlayoutState;
typedef enum {
    RADIOS_PLAYOUT_PAUSE, RADIOS_PLAYOUT_RESUME, RADIOS_PLAYOUT_STOP,
    RADIOS_PLAYOUT_NEXT, RADIOS_PLAYOUT_PREVIOUS
} RadioSPlayoutAction;
typedef struct {
    RadioSPlayoutState state;
    guint index, total;
    gint deck, next_deck;
    guint transition;
    guint64 overlap_ms;
    guint64 position_ms, duration_ms, cue_in_ms;
    gint64 start_utc_us;
    gchar entry_id[37], next_entry_id[37], title[161], error[256];
    gboolean automation;
    gboolean ending; /* actual cursor entered the accepted cue/transition preparation region */
    /* A live step with a duration rule: the transport waits (MANUAL_WAIT)
     * from the previous row's actual end and continues at this UTC; 0 none. */
    gint64 live_wait_until_utc_us;
} RadioSPlayoutSnapshot;
/* Observed running programme name from the immutable accepted plan.
 * Missing, stopped, scheduled and finished programmes leave a cleared result. */
typedef struct {
    gchar programme_title[801], playlist_title[801];
    gboolean programme_active;
    /* Finite accepted queue/block estimates use the observed PCM cursor and
     * full tempo curve. Zero means unknown; paused/manual/media waits have no
     * estimated wall endpoint. An explicit programme deadline is retained. */
    gint64 programme_end_utc_us, playlist_end_utc_us;
    gint64 programme_remaining_us, playlist_remaining_us, track_remaining_us;
    /* Accepted runtime geometry; FALSE across unknown/manual media waits. */
    gboolean advert_timing_known;
    gint64 advert_remaining_us;
} RadioSPlayoutHeaderSnapshot;
typedef struct {
    gchar title[801], artist[801], album[801], source_name[801];
    gchar next_title[801], next_artist[801];
    guint64 duration_ms;
    gchar kind[41]; /* immutable accepted entry classification */
} RadioSPlayoutDeckMetadata;
/* Display-only accepted classification. Primary and auxiliary sources are
 * checked against their current native lease, binding and PCM generation.
 * A planned overlay is distinct from one actually playing or paused. */
typedef enum {
    RADIOS_PLAYOUT_CONTENT_NONE, RADIOS_PLAYOUT_CONTENT_READY,
    RADIOS_PLAYOUT_CONTENT_PLAYING, RADIOS_PLAYOUT_CONTENT_PAUSED,
    RADIOS_PLAYOUT_CONTENT_WAITING
} RadioSPlayoutContentState;
typedef struct {
    gchar kind[41], title[801], artist[801];
    RadioSPlayoutContentState state, overlay_state;
    gboolean auxiliary;
    gchar overlay_kind[41], overlay_title[801], overlay_id[37];
    guint overlay_player; /* one-based; zero when absent */
} RadioSPlayoutDeckContent;

RadioSPlayoutController *radios_playout_ui_new(RadioSViewContext *ctx);
RadioSPlayoutController *radios_playout_ui_new_player(RadioSViewContext *ctx, guint deck, GtkWidget *owner);
/* Scope runners own explicit pools independently from the visible player card
 * and from the global programme runner. ctx must outlive the controller. */
RadioSPlayoutController *radios_playout_ui_new_scope(RadioSViewContext *ctx, guint source, GtkWidget *owner);
gboolean radios_playout_ui_ready(RadioSViewContext *ctx);
/* Owned immutable accepted order plus current runtime selection. NULL when
 * this controller does not own an accepted sequence. No editor mutation. */
JsonNode *radios_playout_ui_plan_snapshot(RadioSViewContext *ctx);
gboolean radios_playout_ui_start_prepared_scope(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared, const guint *decks, guint count, RadioSAutoDJMode mode, GError **error);
/* Origin is captured from a freshly fenced saved automation/binding. These
 * entry points keep it immutable through asynchronous preparation and load. */
gboolean radios_playout_ui_start_prepared_scope_with_loudness_origin(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared, const guint *decks, guint count, RadioSAutoDJMode mode,
    const gchar *automation_id, gboolean assigned, GError **error);
gboolean radios_playout_ui_start_prepared_weekly_with_loudness_origin(RadioSViewContext *ctx,
    GtkWidget *owner, JsonObject *prepared, guint source, guint target,
    const gchar *automation_id, gboolean assigned, GError **error);
gboolean radios_playout_ui_start_automation_with_loudness_origin(RadioSViewContext *ctx,
    GtkWidget *owner, JsonObject *config, const gchar *automation_id, gboolean assigned, GError **error);
void radios_playout_ui_close(RadioSPlayoutController *playout);
void radios_playout_ui_unref(RadioSPlayoutController *playout);
/* Synchronously adopts a freshly authenticated, immutable programme prepare
 * response using the same C identity/cue checks as ordinary preparation. */
gboolean radios_playout_ui_start_prepared(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared, gint64 start_utc_us, GError **error);
gboolean radios_playout_ui_start_prepared_with_loudness_origin(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared, gint64 start_utc_us, const gchar *automation_id, gboolean assigned, GError **error);
/* Pure, strict prepare validation; returns the exact planned zero-based union.
 * Explicit per-row routing overrides the legacy source/target pair. */
gboolean radios_playout_prepared_rotation_pool(RadioSViewContext *ctx,
    JsonObject *prepared, guint source, guint target, guint decks[32],
    guint *count, gboolean *routed, GError **error);
/* Adopt a freshly reauthenticated finite named rotation on the chosen two
 * live MAIN decks. Keep its EOF/CUE/crossfade settings; no forced BPM SYNC.
 * Caller must fence document/client and the card's assignment/generations. */
gboolean radios_playout_ui_start_prepared_player_rotation(RadioSViewContext *ctx,
    GtkWidget *owner, JsonObject *prepared, guint source, guint target,
    GError **error);
guint64 radios_playout_ui_intent(RadioSViewContext *ctx);
/* Validated future-only programme. The first future item's transition policy
 * becomes the outgoing policy of the current item. An active overlap defers
 * the commit until it drains. TRUE also accepts a controller-owned deferred
 * commit; stale intent/epoch and invalid input preserve the old queue. */
gboolean radios_playout_ui_replace_remaining_prepared(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared, guint64 expected_intent, GError **error);
/* The operator's × on unheard rows of the owned run: those entries leave the
 * executor's future in one step; the sounding content, its own exit policy,
 * cursor, epoch and the finished entries stay. The sounding or a finished
 * entry and the row a live step hangs on are refused, an unknown one is
 * G_IO_ERROR_NOT_FOUND, a running transition G_IO_ERROR_BUSY. */
gboolean radios_playout_ui_remove_future_entries(RadioSViewContext *ctx, const gchar *const *entry_ids, guint count, GError **error);
/* Owned accepted programme metadata, including the preserved current content
 * and a queued transition-safe replacement. Used with plan_snapshot for the
 * runtime queue projection; never a saved programme edit. */
JsonNode *radios_playout_ui_programme_config_snapshot(RadioSViewContext *ctx);
/* Same-session emergency checkpoint: preserve the remaining verified source
 * queue and actual PCM cursor; no automatic return or crash recovery. */
gboolean radios_playout_ui_interrupt_prepared(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared, RadioSPlayoutResume **resume, GError **error);
gboolean radios_playout_ui_restore(RadioSViewContext *ctx, GtkWidget *owner,
    RadioSPlayoutResume *resume, GError **error);
void radios_playout_ui_resume_free(RadioSPlayoutResume *resume);
/* Repeat the same verified finite queue only after its exact epoch has fully
 * drained. Source files are reopened by the decoder; manual takeover and a
 * different accepted plan cannot be replaced by this command. */
gboolean radios_playout_ui_repeat_completed(RadioSViewContext *ctx,
    guint64 expected_epoch, GError **error);
gboolean radios_playout_ui_start_playlist(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *playlist_id, JsonArray *entry_ids, guint mode, gdouble overlap,
    GError **error);
gboolean radios_playout_ui_start_programme(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *config, guint start_index, guint mode, gdouble overlap,
    GError **error);
gboolean radios_playout_ui_start_player_autodj(RadioSViewContext *ctx, GtkWidget *owner,
    JsonArray *items, guint source, guint target, GError **error);
gboolean radios_playout_ui_schedule_programme(RadioSViewContext *ctx,
    GtkWidget *owner, JsonObject *config, gint64 start_unix_ms, guint mode,
    gdouble overlap, GError **error);
gboolean radios_playout_ui_start_automation(RadioSViewContext *ctx,
    GtkWidget *owner, JsonObject *config, GError **error);
/* Read-only plan delivery. Callback data follows the existing data-request
 * lifetime contract; preview never loads a deck or opens an output gate. */
gboolean radios_playout_ui_preview_automation(RadioSViewContext *ctx,
    GtkWidget *owner, JsonObject *config, RadioSDataCallback callback,
    gpointer user, GDestroyNotify destroy, GError **error);
gboolean radios_playout_ui_action(RadioSViewContext *ctx,
    RadioSPlayoutAction action, GError **error);
gboolean radios_playout_ui_get_snapshot(RadioSViewContext *ctx,
    RadioSPlayoutSnapshot *snapshot);
/* Owned array of newly observed COMPLETE entry IDs for the current run.
 * Interrupted/failed receipts and receipts from previous epochs are excluded. */
JsonArray *radios_playout_ui_take_completed(RadioSViewContext *ctx);
gboolean radios_playout_ui_header_snapshot(RadioSViewContext *ctx,
    RadioSPlayoutHeaderSnapshot *snapshot);
gboolean radios_playout_ui_deck_metadata(RadioSViewContext *ctx, guint deck,
    RadioSPlayoutDeckMetadata *metadata);
gboolean radios_playout_ui_deck_content(RadioSViewContext *ctx, guint deck,
    RadioSPlayoutDeckContent *content);
/* Includes empty/loading preloads and drained deck pool slots. */
gboolean radios_playout_ui_owns_deck(RadioSViewContext *ctx, guint deck);
/* Owned bounded source UUID -> actual first consumed UTC microsecond map.
 * Same-controller memory only; scheduled/unconsumed entries are excluded. */
JsonNode *radios_playout_ui_calendar_turns(RadioSViewContext *ctx);
JsonNode *radios_playout_ui_probability_checkpoint(RadioSViewContext *ctx, gboolean selection);
JsonNode *radios_playout_ui_history(RadioSViewContext *ctx); /* owned, local PCM observations */
const gchar *radios_playout_ui_state_name(RadioSPlayoutState state);
/* Kılavuz 2026-10-18 §06/§08 "çekme adedi": a programme run prepares only
 * event_fetch events (each at most content_fetch contents) ahead of the
 * current position and appends the remaining programme in the background
 * before the prepared items run out. 0/0 keeps the whole-programme behaviour
 * (a programme above 500 contents is still windowed by that per-request cap).
 * GTK thread; applies to runs started afterwards on this controller and to
 * the continuation windows of the programme already running. */
void radios_playout_ui_set_programme_window(RadioSViewContext *ctx, guint event_fetch, guint content_fetch);
/* Pure window selection behind that setting: the ids of the contiguous
 * playback-order window starting at start_index. Events are the blocks of
 * config; an event longer than content_fetch ends the window inside it, so
 * the next window resumes exactly there and the programme order is kept. At
 * most RADIOS_AUDIO_RUN_MAX_ITEMS (500) ids, the backend's per-call limit; a
 * zero count leaves that dimension unbounded. *next_index receives the start
 * of the following window. FALSE with *ids NULL when both counts are zero
 * (whole programme) or nothing remains. The caller unrefs *ids. */
gboolean radios_playout_programme_window_entries(JsonObject *config, guint start_index,
    guint event_fetch, guint content_fetch, JsonArray **ids, guint *next_index);
/* U3 (C10) header playlist picker. The playlists (event blocks) of the
 * started plan from the playing one on, in plan order: the playing one
 * first, then exactly the targets that repeated "Sonraki playlist" presses
 * reach within the accepted window (same bounds and ownership). Owned array
 * of objects {epoch, index, entry_id, title, kind, first_title, first_artist,
 * current}; index is the playlist's first row (the playing row for the
 * current one). NULL when the plan cannot be navigated by playlist. */
JsonArray *radios_playout_ui_playlist_choices(RadioSViewContext *ctx);
/* Jump to the first song of a listed following playlist through the same
 * bounded epoch jump as radios_playout_ui_next_playlist(). Refused when the
 * epoch changed or index/entry_id is no longer a following playlist start. */
gboolean radios_playout_ui_jump_playlist(RadioSViewContext *ctx, guint64 epoch,
    guint index, const gchar *entry_id, GError **error);

/* Broadcast view owns the persistent programme draft and its CAS save. */
gboolean radios_broadcast_append_playlist(RadioSViewContext *ctx,
    JsonArray *tracks, GError **error);
/* Loads a list of card inputs on the first live player whose inputs are idle
 * and plays it there (its deck), never directly on MAIN. */
gboolean radios_broadcast_play_on_player(RadioSViewContext *ctx, JsonArray *items,
    const gchar *name, gchar **player, GError **error);
/* The same append as one event titled title (NULL: by content type). */
gboolean radios_broadcast_append_event(RadioSViewContext *ctx,
    JsonArray *tracks, const gchar *title, GError **error);
/* Borrowed; use its acknowledged identity/revision to guard delayed appends. */
RadioSDocument *radios_broadcast_programme_document(RadioSViewContext *ctx);
/* Formal approval/distribution panel of the Broadcast page's selected
 * programme queue, mounted by another page. It follows queue switches and is
 * destroyed with the Broadcast page. NULL when that page does not exist. */
GtkWidget *radios_broadcast_programme_lifecycle_new(RadioSViewContext *ctx);
/* Owned "queue · programme title" of that selected queue; NULL without one. */
gchar *radios_broadcast_programme_label(RadioSViewContext *ctx);
/* Deep-copy ownership when a runner accepts a new programme request. NULL
 * clears the previous model for starts that do not use the broadcast draft. */
void radios_broadcast_queue_run_config(RadioSViewContext *ctx, JsonObject *config);

/* Strict weekly-batch validation accepts a single authenticated entry. The
 * selected source/optional partner never override saved per-kind routing. */
gboolean radios_playout_prepared_weekly_pool(RadioSViewContext *ctx,
    JsonObject *prepared, guint source, guint target, guint decks[32],
    guint *count, gboolean *routed, GError **error);
gboolean radios_playout_ui_start_prepared_weekly(RadioSViewContext *ctx,
    GtkWidget *owner, JsonObject *prepared, guint source, guint target,
    GError **error);
/* Acknowledged settings only. Deep-copies and propagates to station/player
 * runners sharing the window; the getter is borrowed until the next update. */
void radios_playout_ui_copyright_policy(RadioSViewContext *ctx, JsonObject *policy);
JsonObject *radios_playout_ui_copyright_policy_get(RadioSViewContext *ctx);
gboolean radios_playout_ui_copyright_policy_record(RadioSViewContext *ctx,
    JsonObject *policy, const gchar *document_id, gint64 revision);

#endif
