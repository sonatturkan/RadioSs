#ifndef RADIOS_HEADER_NAVIGATION_H
#define RADIOS_HEADER_NAVIGATION_H

#include "radios/views.h"
#include "radios/audio_runner.h"

G_BEGIN_DECLS

typedef struct RadioSAudioEngine RadioSAudioEngine;

/* A bounded manual jump within the same accepted immutable runner epoch.
 * Paused, scheduled, preparing and finished runners reject navigation. */
gboolean radios_audio_engine_runner_jump_to(RadioSAudioEngine *engine,
    guint64 expected_epoch, guint index, GError **error);
gboolean radios_audio_engine_player_runner_jump_to(RadioSAudioEngine *engine,
    guint deck, guint64 expected_epoch, guint index, GError **error);

/* Latest engine observation for owned navigation, without a GTK poll delay. */
gboolean radios_playout_ui_navigation_snapshot(RadioSViewContext *ctx,
    RadioSAudioRunSnapshot *snapshot);

/* Playlist boundaries belong to the started plan, never a mutable editor. */
gboolean radios_playout_ui_can_next_playlist(RadioSViewContext *ctx);
gboolean radios_playout_ui_next_playlist(RadioSViewContext *ctx, GError **error);

/* Actions: 0 previous, 1 play/resume, 2 pause, 3 stop, 4 next track,
 * 5 next playlist. The broadcast facade preserves the selected queue owner. */
void radios_broadcast_navigation_status(RadioSViewContext *ctx,
    gboolean *previous, gboolean *next, gboolean *playlist);
gboolean radios_broadcast_navigation_available(RadioSViewContext *ctx, guint action);
/* Whether selected queue content or an existing transport can accept Play.
 * Unlike button sensitivity, an idle empty draft is not playable content. */
gboolean radios_broadcast_navigation_queue_playable(RadioSViewContext *ctx);
/* An existing selected queue/flow owns transport, including paused media. */
gboolean radios_broadcast_navigation_queue_active(RadioSViewContext *ctx);
/* The broadcast page shows its general queue in continuous calendar mode
 * ("Takvime göre sürekli yayın") with the calendar's programme in it: the
 * header's Play starts that queue, so its event list follows the transport. */
gboolean radios_broadcast_navigation_general_continuous(RadioSViewContext *ctx);
gboolean radios_broadcast_navigation_action(RadioSViewContext *ctx, guint action, GError **error);
/* U3 (C10) header playlist picker on the same selected queue owner as
 * action 5. Under the general driver's controls only its own run is listed
 * (NULL otherwise) or jumped (refused with a reason). Choices and the jump
 * follow radios_playout_ui_playlist_choices / radios_playout_ui_jump_playlist. */
JsonArray *radios_broadcast_navigation_playlists(RadioSViewContext *ctx);
gboolean radios_broadcast_navigation_jump_playlist(RadioSViewContext *ctx, guint64 epoch,
    guint index, const gchar *entry_id, GError **error);

GtkWidget *radios_header_navigation_new(RadioSViewContext *ctx);
GtkWidget *radios_header_health_new(RadioSViewContext *ctx);

G_END_DECLS
#endif
