#include "radios/header_progress.h"
#include "radios/neon_progress.h"
#include "radios/audio_ui.h"
#include "radios/playout_ui.h"

#include <string.h>

typedef struct {
    gint deck;
    RadioSAudioUiSnapshot audio;
    guint64 cue_in_ms, cue_out_ms;
    gchar entry_id[37];
} HeaderTrack;

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *widget;
    HeaderTrack selected, gesture, preferred;
    gboolean prefer_local;
    guint timer;
    gboolean gesture_active, disposed;
    /* Every normal deck from one engine snapshot per selection pass. */
    RadioSAudioUiSnapshot *decks;
    gboolean decks_valid;
} HeaderProgress;

static gboolean
track_loaded(const RadioSAudioUiSnapshot *audio)
{
    return audio->main_enabled && audio->generation != 0 &&
        audio->duration_ms != 0 && audio->media_id[0] && audio->version_id[0] &&
        (audio->state == RADIOS_AUDIO_UI_PLAYING ||
         audio->state == RADIOS_AUDIO_UI_PAUSED ||
         audio->state == RADIOS_AUDIO_UI_READY);
}

static gboolean
playout_active(RadioSPlayoutState state)
{
    return state == RADIOS_PLAYOUT_PLAYING || state == RADIOS_PLAYOUT_OVERLAP ||
        state == RADIOS_PLAYOUT_PAUSED || state == RADIOS_PLAYOUT_WAITING ||
        state == RADIOS_PLAYOUT_MANUAL_WAIT;
}

static HeaderTrack
track_from_audio(guint deck, const RadioSAudioUiSnapshot *audio)
{
    HeaderTrack track = { .deck = (gint)deck, .audio = *audio,
        .cue_out_ms = audio->duration_ms };
    if (track.audio.runner_paused && track.audio.state == RADIOS_AUDIO_UI_PLAYING)
        track.audio.state = RADIOS_AUDIO_UI_PAUSED;
    if (audio->cue_out_ms != 0)
        track.cue_out_ms = MIN(track.cue_out_ms, audio->cue_out_ms);
    track.cue_in_ms = MIN(audio->cue_in_ms, track.cue_out_ms);
    return track;
}

static HeaderTrack
select_track(HeaderProgress *state)
{
    RadioSViewContext *ctx = state->ctx;
    HeaderTrack selected = { .deck = -1 };
    RadioSPlayoutSnapshot playout = {0};
    if (!ctx)
        return selected;
    gboolean have_run = radios_playout_ui_get_snapshot(ctx, &playout) && playout_active(playout.state);
    gboolean run_quiet = have_run && playout.state != RADIOS_PLAYOUT_PLAYING &&
        playout.state != RADIOS_PLAYOUT_OVERLAP;
    /* The 50 ms tick scans every normal deck; one engine snapshot covers them
     * all instead of taking the engine lock once per deck. */
    state->decks_valid = radios_audio_ui_get_snapshots(ctx, state->decks, RADIOS_AUDIO_NORMAL_DECKS);
    if (!state->decks_valid)
        return selected;
    if (have_run && playout.deck >= 0 &&
        playout.deck < (gint)RADIOS_AUDIO_NORMAL_DECKS &&
        track_loaded(&state->decks[playout.deck])) {
        selected.audio = state->decks[playout.deck];
        selected = track_from_audio((guint)playout.deck, &selected.audio);
        if (playout.duration_ms != 0)
            selected.cue_out_ms = MIN(selected.cue_out_ms, playout.duration_ms);
        selected.cue_in_ms = MIN(MAX(selected.cue_in_ms, playout.cue_in_ms), selected.cue_out_ms);
        g_strlcpy(selected.entry_id, playout.entry_id, sizeof selected.entry_id);
        /* A paused runner keeps its decoder PLAYING but stops consuming PCM.
         * Logical run state controls visual pulse and source priority. */
        if (run_quiet)
            selected.audio.state = RADIOS_AUDIO_UI_PAUSED;
        if (selected.audio.state == RADIOS_AUDIO_UI_PLAYING)
            return selected;
    }

    /* Preview32 and sampler33..65 are excluded from every normal-deck scan. */
    guint best_rank = selected.deck >= 0 ?
        (selected.audio.state == RADIOS_AUDIO_UI_PAUSED ? 2 : 3) : 4;
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) {
        /* Do not reinterpret the selected run's physical decoder state, or
         * its paused overlap partner, as an independently playing deck. */
        if ((gint)deck == selected.deck ||
            (run_quiet && (gint)deck == playout.next_deck))
            continue;
        const RadioSAudioUiSnapshot *audio = &state->decks[deck];
        if (!track_loaded(audio))
            continue;
        HeaderTrack candidate = track_from_audio(deck, audio);
        guint rank = candidate.audio.state == RADIOS_AUDIO_UI_PLAYING ? 1 :
            candidate.audio.state == RADIOS_AUDIO_UI_PAUSED ? 2 : 3;
        if (rank < best_rank) {
            best_rank = rank;
            selected = candidate;
            if (rank == 1)
                return selected;
        }
    }
    /* Seeking a paused source recreates its decoder as LOADING then READY.
     * Preserve that explicit source intent across this readiness gap, while
     * every active playing source above keeps its ordinary priority. */
    if (state->prefer_local) {
        guint deck = (guint)state->preferred.deck;
        const RadioSAudioUiSnapshot *audio = deck < RADIOS_AUDIO_NORMAL_DECKS ? &state->decks[deck] : NULL;
        if (audio &&
            audio->generation == state->preferred.audio.generation &&
            g_str_equal(audio->media_id, state->preferred.audio.media_id) &&
            g_str_equal(audio->version_id, state->preferred.audio.version_id) &&
            audio->main_enabled && audio->duration_ms && !audio->error[0] &&
            (audio->state == RADIOS_AUDIO_UI_LOADING || audio->state == RADIOS_AUDIO_UI_READY ||
             audio->state == RADIOS_AUDIO_UI_PAUSED))
            return track_from_audio(deck, audio);
        state->prefer_local = FALSE;
    }
    return selected;
}

static gboolean
same_track(const HeaderTrack *left, const HeaderTrack *right)
{
    return left->deck == right->deck &&
        left->audio.generation == right->audio.generation &&
        g_str_equal(left->audio.media_id, right->audio.media_id) &&
        g_str_equal(left->audio.version_id, right->audio.version_id) &&
        g_str_equal(left->entry_id, right->entry_id) &&
        left->cue_in_ms == right->cue_in_ms && left->cue_out_ms == right->cue_out_ms;
}

static gboolean
track_seekable(const HeaderTrack *track)
{
    return track->deck >= 0 && track_loaded(&track->audio) &&
        track->audio.output_ready && !track->audio.error[0] &&
        track->cue_out_ms > track->cue_in_ms;
}

static void
display_track(HeaderProgress *state, const HeaderTrack *track, gboolean allow_seek)
{
    guint64 duration = track->cue_out_ms - track->cue_in_ms;
    guint64 position = track->audio.position_ms > track->cue_in_ms ?
        MIN(track->audio.position_ms - track->cue_in_ms, duration) : 0;
    radios_neon_progress_set_state(state->widget, position, duration,
        track->deck >= 0 && track->audio.state == RADIOS_AUDIO_UI_PLAYING,
        allow_seek && track_seekable(track));
    gchar *description = NULL;
    if (track->deck < 0)
        description = g_strdup("Yayın çıkışında yüklü bir şarkı yok.");
    else if (!track->audio.output_ready)
        description = g_strdup("Şarkı konumunu değiştirmek için yayın çıkışını bağla.");
    else if (track->audio.error[0])
        description = g_strdup(track->audio.error);
    else
        description = g_strdup_printf("%s%s%s · Şarkı konumunu değiştirmek için sürükle.",
            track->audio.title[0] ? track->audio.title : "Şarkı",
            track->audio.artist[0] ? " · " : "", track->audio.artist);
    gtk_widget_set_tooltip_text(state->widget, description);
    g_free(description);
}

static gboolean
progress_tick(gpointer data)
{
    HeaderProgress *state = data;
    if (state->disposed)
        return G_SOURCE_REMOVE;
    HeaderTrack current = select_track(state);
    gboolean changed_during_gesture = state->gesture_active &&
        !same_track(&current, &state->gesture);
    if (changed_during_gesture || !track_seekable(&current))
        state->gesture_active = FALSE;
    state->selected = current;
    /* A false update releases a mouse grab and aborts the old song's preview.
     * The next tick enables the new song; its stale release cannot seek it. */
    display_track(state, &current, !changed_during_gesture);
    return G_SOURCE_CONTINUE;
}

static void
seek_begin(GtkWidget *widget, gpointer data)
{
    HeaderProgress *state = data;
    (void)widget;
    if (state->disposed)
        return;
    HeaderTrack current = select_track(state);
    state->gesture_active = track_seekable(&current) &&
        same_track(&current, &state->selected);
    state->gesture = current;
    state->selected = current;
    display_track(state, &current, state->gesture_active);
}

static gboolean
seek_commit(guint64 position_ms, gpointer data)
{
    HeaderProgress *state = data;
    if (state->disposed || !state->gesture_active)
        return FALSE;
    HeaderTrack current = select_track(state);
    HeaderTrack gesture = state->gesture;
    state->gesture_active = FALSE;
    guint64 span = gesture.cue_out_ms - gesture.cue_in_ms;
    if (!same_track(&current, &gesture) || !track_seekable(&current) || position_ms > span)
        return FALSE;

    /* The engine keeps the current AutoDJ queue and rejects ambiguous seeks
     * into an overlap or a preloaded successor. */
    GError *error = NULL;
    gboolean accepted = radios_audio_ui_seek_if_generation(state->ctx, (guint)gesture.deck,
        gesture.audio.generation, gesture.cue_in_ms + position_ms, &error);
    if (accepted) {
        RadioSAudioUiSnapshot observed = {0};
        if (radios_audio_ui_get_snapshot(state->ctx, (guint)gesture.deck, &observed) &&
            observed.generation && g_str_equal(observed.media_id, gesture.audio.media_id) &&
            g_str_equal(observed.version_id, gesture.audio.version_id)) {
            state->preferred = track_from_audio((guint)gesture.deck, &observed);
            state->prefer_local = TRUE;
        }
    }
    if (!accepted && state->ctx && state->ctx->notify)
        state->ctx->notify(state->ctx->notify_data,
            error ? error->message : "Şarkı konumu değiştirilemedi.");
    g_clear_error(&error);
    return accepted;
}

static void
progress_destroyed(GtkWidget *widget, gpointer data)
{
    HeaderProgress *state = data;
    state->disposed = TRUE;
    state->gesture_active = FALSE;
    if (state->timer) {
        g_source_remove(state->timer);
        state->timer = 0;
    }
    radios_neon_progress_set_seek_callback(widget, NULL, NULL);
    g_signal_handlers_disconnect_by_data(widget, state);
    g_clear_pointer(&state->decks, g_free);
}

GtkWidget *
radios_header_progress_new(RadioSViewContext *ctx)
{
    GtkWidget *widget = radios_neon_progress_new();
    HeaderProgress *state = g_new0(HeaderProgress, 1);
    state->ctx = ctx;
    state->widget = widget;
    state->decks = g_new0(RadioSAudioUiSnapshot, RADIOS_AUDIO_NORMAL_DECKS);
    state->selected.deck = state->gesture.deck = -1;
    gtk_widget_set_name(widget, "header-song-progress");
    g_object_set_data_full(G_OBJECT(widget), "radios-header-progress", state, g_free);
    radios_neon_progress_set_seek_callback(widget, seek_commit, state);
    g_signal_connect(widget, "seek-begin", G_CALLBACK(seek_begin), state);
    g_signal_connect(widget, "destroy", G_CALLBACK(progress_destroyed), state);
    progress_tick(state);
    state->timer = g_timeout_add(50, progress_tick, state);
    return widget;
}
