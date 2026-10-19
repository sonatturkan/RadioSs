#ifndef RADIOS_BROADCAST_HEADER_H
#define RADIOS_BROADCAST_HEADER_H

#include "radios/views.h"
#include "radios/playout_ui.h"

G_BEGIN_DECLS

typedef struct {
    gboolean programme_active, current, paused, waiting;
    guint active_sources;
    /* Exact observed local owner; false when no source is audible. */
    gboolean local_deck;
    guint deck;
    gchar entry_id[37];
    gchar playlist_title[801];
    /* Calendar programme end is its fixed slot boundary. Other endpoints are
     * observed finite-queue estimates; zero means an unknown wall endpoint. */
    gint64 programme_end_utc_us, playlist_end_utc_us;
    gint64 programme_remaining_us, playlist_remaining_us, track_remaining_us;
    /* Accepted runtime geometry; FALSE across unknown/manual media waits. */
    gboolean advert_timing_known;
    gint64 advert_remaining_us;
    gchar programme_title[801], title[801], artist[801];
    guint64 position_ms, duration_ms;
    gdouble tempo_ratio;
    GtkWidget *card; /* borrowed, the live player card that owns the current deck */
    /* Kılavuz 2026-10-18 A-4: the flow item selected in the broadcast queue.
     * selected is FALSE without a selection (all other fields then unset);
     * selected_content tells a content row from an event row; selected_playing
     * means the selection is the sounding item, so the header keeps showing
     * the remaining time. selected_duration_ms is -1 when unknown. */
    gboolean selected, selected_content, selected_playing;
    gint64 selected_duration_ms;
    gchar selected_title[161];
} RadioSBroadcastHeaderSnapshot;

/* GTK-thread read-only observations. TRUE is authoritative even when no
 * source is current; FALSE means the context is absent.
 * Snapshots never start playback or load/edit the selected programme draft. */
gboolean radios_broadcast_header_snapshot(RadioSViewContext *ctx,
    RadioSBroadcastHeaderSnapshot *snapshot);
/* Read-only accepted metadata for one private player or the general runner.
 * TRUE is authoritative for a managed deck, including a cleared result when
 * no current/overlapping accepted entry matches. FALSE means no managed owner.
 * Does not select a queue, load media or start playback. */
gboolean radios_broadcast_player_deck_metadata(RadioSViewContext *ctx, guint deck,
    RadioSPlayoutDeckMetadata *metadata);
/* Station-wide defaults edited in the general broadcast queue. Capture on
 * the GTK thread before preparing a run; event/rotation rules take priority. */
gboolean radios_broadcast_transition_policy(RadioSViewContext *ctx, guint *mode,
    gdouble *overlap_seconds, gboolean *beat_sync);
/* Preferences shares the same acknowledged station document and defaults.
 * Setter does not change transport or the selected queue. GTK thread only. */
gboolean radios_broadcast_transition_policy_ready(RadioSViewContext *ctx);
const gchar *radios_broadcast_transition_policy_status(RadioSViewContext *ctx);
gboolean radios_broadcast_transition_policy_set(RadioSViewContext *ctx, guint mode,
    gdouble overlap_seconds, gboolean beat_sync, GError **error);
/* Checkpoint pending automation defaults before closing the data client. */
void radios_broadcast_prepare_close(RadioSViewContext *ctx);

G_END_DECLS
#endif
