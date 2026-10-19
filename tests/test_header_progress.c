#include "radios/header_progress.h"
#include "radios/neon_progress.h"
#include "radios/audio_ui.h"
#include "radios/playout_ui.h"

#include <string.h>

/* Link this test to the adapter/widget sources, without radios_ui. These
 * stubs record the actual controller API invoked by a completed gesture. */
static RadioSAudioUiSnapshot decks[RADIOS_AUDIO_DECKS];
static gboolean available[RADIOS_AUDIO_DECKS];
static RadioSPlayoutSnapshot playout;
static gboolean have_playout, reject_seek, replace_inside_seek, load_after_seek;
static guint seek_calls, accepted_seeks, last_deck, snapshot_calls, preview_queries;
static guint64 last_position, last_generation;

gboolean
radios_audio_ui_get_snapshot(RadioSViewContext *ctx, guint deck, RadioSAudioUiSnapshot *out)
{
    (void)ctx;
    ++snapshot_calls;
    if (deck >= RADIOS_AUDIO_NORMAL_DECKS)
        ++preview_queries;
    if (deck >= RADIOS_AUDIO_DECKS || !available[deck])
        return FALSE;
    *out = decks[deck];
    return TRUE;
}

/* The periodic scan reads every normal deck at once; an unloaded deck is an
 * empty snapshot, exactly as the controller reports it. */
gboolean
radios_audio_ui_get_snapshots(RadioSViewContext *ctx, RadioSAudioUiSnapshot *out, guint count)
{
    (void)ctx;
    ++snapshot_calls;
    if (!out || !count || count > RADIOS_AUDIO_DECKS)
        return FALSE;
    for (guint deck = 0; deck < count; ++deck)
        out[deck] = available[deck] ? decks[deck] : (RadioSAudioUiSnapshot){0};
    return TRUE;
}

gboolean
radios_playout_ui_get_snapshot(RadioSViewContext *ctx, RadioSPlayoutSnapshot *out)
{
    (void)ctx;
    if (!have_playout)
        return FALSE;
    *out = playout;
    return TRUE;
}

gboolean
radios_audio_ui_seek_if_generation(RadioSViewContext *ctx, guint deck,
    guint64 expected_generation, guint64 position, GError **error)
{
    (void)ctx;
    ++seek_calls;
    last_deck = deck;
    last_position = position;
    last_generation = expected_generation;
    if (replace_inside_seek)
        ++decks[deck].generation;
    if (expected_generation != decks[deck].generation) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Şarkı kimliği değişti");
        return FALSE;
    }
    if (reject_seek) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Deneme seek hatası");
        return FALSE;
    }
    ++accepted_seeks;
    if (load_after_seek) {
        ++decks[deck].generation;
        decks[deck].state = RADIOS_AUDIO_UI_LOADING;
        decks[deck].position_ms = position;
        decks[deck].output_ready = FALSE;
    }
    return TRUE;
}

typedef struct {
    GtkWidget *window, *widget;
    RadioSViewContext ctx;
    gchar *notice;
} Fixture;

static void
notify(gpointer data, const gchar *message)
{
    Fixture *fixture = data;
    g_free(fixture->notice);
    fixture->notice = g_strdup(message);
}

static void
pump(guint milliseconds)
{
    gint64 end = g_get_monotonic_time() + milliseconds * 1000;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        if (milliseconds)
            g_usleep(1000);
    } while (g_get_monotonic_time() < end);
}

static void
setup(Fixture *fixture, gconstpointer data)
{
    (void)data;
    memset(decks, 0, sizeof decks);
    memset(available, 0, sizeof available);
    playout = (RadioSPlayoutSnapshot){0};
    have_playout = reject_seek = replace_inside_seek = load_after_seek = FALSE;
    seek_calls = accepted_seeks = snapshot_calls = preview_queries = 0;
    last_deck = G_MAXUINT;
    last_position = 0;
    last_generation = 0;
    fixture->ctx.notify = notify;
    fixture->ctx.notify_data = fixture;
    fixture->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_ref_sink(fixture->window);
    fixture->ctx.parent = GTK_WINDOW(fixture->window);
    gtk_window_set_default_size(GTK_WINDOW(fixture->window), 500, 60);
}

static void
teardown(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gtk_widget_destroy(fixture->window);
    g_object_unref(fixture->window);
    if (fixture->widget)
        g_object_unref(fixture->widget);
    g_free(fixture->notice);
    pump(0);
}

static void
load_deck(guint deck, RadioSAudioUiState state, guint64 position, guint64 duration)
{
    g_assert_cmpuint(deck, <, RADIOS_AUDIO_DECKS);
    available[deck] = TRUE;
    decks[deck] = (RadioSAudioUiSnapshot){ .state = state, .generation = 7,
        .position_ms = position, .duration_ms = duration,
        .main_enabled = TRUE, .output_ready = TRUE };
    g_strlcpy(decks[deck].media_id, "11111111-1111-4111-8111-111111111111", sizeof decks[deck].media_id);
    g_strlcpy(decks[deck].version_id, "22222222-2222-4222-8222-222222222222", sizeof decks[deck].version_id);
    g_strlcpy(decks[deck].title, "Gerçek şarkı", sizeof decks[deck].title);
}

static void
create(Fixture *fixture)
{
    fixture->widget = radios_header_progress_new(&fixture->ctx);
    g_object_ref_sink(fixture->widget);
    gtk_container_add(GTK_CONTAINER(fixture->window), fixture->widget);
    gtk_widget_show_all(fixture->window);
    pump(10);
}

static void
assert_state(Fixture *fixture, guint64 position, guint64 duration, gboolean playing, gboolean seekable)
{
    guint64 actual_position = G_MAXUINT64, actual_duration = G_MAXUINT64;
    gboolean actual_playing = !playing, actual_seekable = !seekable;
    radios_neon_progress_get_state(fixture->widget, &actual_position, &actual_duration,
        &actual_playing, &actual_seekable);
    g_assert_cmpuint(actual_position, ==, position);
    g_assert_cmpuint(actual_duration, ==, duration);
    g_assert_cmpint(actual_playing, ==, playing);
    g_assert_cmpint(actual_seekable, ==, seekable);
}

static void
key(Fixture *fixture, guint keyval)
{
    GdkEventKey event = { .type = GDK_KEY_PRESS, .keyval = keyval };
    gboolean handled = FALSE;
    g_signal_emit_by_name(fixture->widget, "key-press-event", &event, &handled);
}

static void
press(Fixture *fixture)
{
    GdkEventButton event = { .type = GDK_BUTTON_PRESS, .button = 1,
        .x = gtk_widget_get_allocated_width(fixture->widget) / 2.0 };
    gboolean handled = FALSE;
    g_signal_emit_by_name(fixture->widget, "button-press-event", &event, &handled);
    g_assert_true(handled);
}

static void
release(Fixture *fixture)
{
    GdkEventButton event = { .type = GDK_BUTTON_RELEASE, .button = 1,
        .x = gtk_widget_get_allocated_width(fixture->widget) / 2.0 };
    gboolean handled = FALSE;
    g_signal_emit_by_name(fixture->widget, "button-release-event", &event, &handled);
}

static void
no_source(Fixture *fixture, gconstpointer data)
{
    (void)data;
    create(fixture);
    assert_state(fixture, 0, 0, FALSE, FALSE);
    key(fixture, GDK_KEY_End);
    g_assert_cmpuint(seek_calls, ==, 0);
    g_assert_cmpuint(preview_queries, ==, 0);
}

static void
source_priority(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(0, RADIOS_AUDIO_UI_READY, 0, 10000);
    load_deck(1, RADIOS_AUDIO_UI_PAUSED, 1000, 20000);
    load_deck(31, RADIOS_AUDIO_UI_PLAYING, 4000, 30000);
    create(fixture);
    assert_state(fixture, 4000, 30000, TRUE, TRUE);
    key(fixture, GDK_KEY_End);
    g_assert_cmpuint(seek_calls, ==, 1);
    g_assert_cmpuint(last_deck, ==, 31);
    g_assert_cmpuint(last_position, ==, 30000);
    g_assert_cmpuint(last_generation, ==, 7);
    g_assert_cmpuint(accepted_seeks, ==, 1);
    g_assert_cmpuint(preview_queries, ==, 0);
}

static void
paused_over_ready(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(0, RADIOS_AUDIO_UI_READY, 0, 10000);
    load_deck(24, RADIOS_AUDIO_UI_PAUSED, 7000, 21000);
    create(fixture);
    assert_state(fixture, 7000, 21000, FALSE, TRUE);
    key(fixture, GDK_KEY_Right);
    g_assert_cmpuint(last_deck, ==, 24);
    g_assert_cmpuint(last_position, ==, 12000);
    /* A command acknowledgement does not invent observed playback progress. */
    assert_state(fixture, 7000, 21000, FALSE, TRUE);
}

static void
cue_excluded(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(0, RADIOS_AUDIO_UI_PLAYING, 1000, 10000);
    decks[0].main_enabled = FALSE;
    load_deck(32, RADIOS_AUDIO_UI_PLAYING, 3000, 30000);
    load_deck(65, RADIOS_AUDIO_UI_PLAYING, 6000, 60000);
    create(fixture);
    assert_state(fixture, 0, 0, FALSE, FALSE);
    g_assert_cmpuint(preview_queries, ==, 0);
}

static void
playout_priority_and_cues(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(0, RADIOS_AUDIO_UI_PLAYING, 1000, 10000);
    load_deck(9, RADIOS_AUDIO_UI_PLAYING, 12000, 60000);
    have_playout = TRUE;
    playout = (RadioSPlayoutSnapshot){ .state = RADIOS_PLAYOUT_OVERLAP,
        .deck = 9, .cue_in_ms = 10000, .duration_ms = 40000 };
    g_strlcpy(playout.entry_id, "33333333-3333-4333-8333-333333333333", sizeof playout.entry_id);
    create(fixture);
    assert_state(fixture, 2000, 30000, TRUE, TRUE);
    key(fixture, GDK_KEY_Home);
    g_assert_cmpuint(last_deck, ==, 9);
    g_assert_cmpuint(last_position, ==, 10000);
    key(fixture, GDK_KEY_End);
    g_assert_cmpuint(seek_calls, ==, 2);
    g_assert_cmpuint(last_position, ==, 40000);
}

static void
logical_runner_pause(Fixture *fixture, gconstpointer data)
{
    RadioSPlayoutState logical = (RadioSPlayoutState)GPOINTER_TO_UINT(data);
    load_deck(9, RADIOS_AUDIO_UI_PLAYING, 12000, 60000);
    load_deck(10, RADIOS_AUDIO_UI_PLAYING, 4000, 50000);
    have_playout = TRUE;
    playout = (RadioSPlayoutSnapshot){ .state = logical, .deck = 9,
        .next_deck = 10, .cue_in_ms = 10000, .duration_ms = 40000 };
    create(fixture);
    assert_state(fixture, 2000, 30000, FALSE, TRUE);
    pump(130);
    assert_state(fixture, 2000, 30000, FALSE, TRUE);
    load_deck(1, RADIOS_AUDIO_UI_PLAYING, 3000, 20000);
    pump(70);
    assert_state(fixture, 3000, 20000, TRUE, TRUE);
}

static void
private_runner_paused(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(0, RADIOS_AUDIO_UI_READY, 1000, 10000);
    load_deck(7, RADIOS_AUDIO_UI_PLAYING, 4000, 20000);
    decks[7].runner_paused = TRUE;
    create(fixture);
    assert_state(fixture, 4000, 20000, FALSE, TRUE);
    pump(130);
    assert_state(fixture, 4000, 20000, FALSE, TRUE);
    key(fixture, GDK_KEY_End);
    g_assert_cmpuint(last_deck, ==, 7);
    g_assert_cmpuint(last_position, ==, 20000);
    load_deck(2, RADIOS_AUDIO_UI_PLAYING, 2000, 30000);
    pump(70);
    assert_state(fixture, 2000, 30000, TRUE, TRUE);
    available[2] = FALSE;
    decks[7].runner_paused = FALSE;
    pump(70);
    assert_state(fixture, 4000, 20000, TRUE, TRUE);
}

static void
unavailable_output(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(2, RADIOS_AUDIO_UI_PAUSED, 2000, 10000);
    decks[2].output_ready = FALSE;
    create(fixture);
    assert_state(fixture, 2000, 10000, FALSE, FALSE);
    key(fixture, GDK_KEY_End);
    g_assert_cmpuint(seek_calls, ==, 0);
}

static void
manual_cue_span(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(6, RADIOS_AUDIO_UI_PLAYING, 10000, 60000);
    decks[6].cue_in_ms = 5000;
    decks[6].cue_out_ms = 30000;
    create(fixture);
    assert_state(fixture, 5000, 25000, TRUE, TRUE);
    key(fixture, GDK_KEY_Home);
    g_assert_cmpuint(last_position, ==, 5000);
    key(fixture, GDK_KEY_End);
    g_assert_cmpuint(last_position, ==, 30000);
    g_assert_cmpuint(accepted_seeks, ==, 2);
}

static void
playout_physical_cue_intersection(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(0, RADIOS_AUDIO_UI_PLAYING, 15000, 60000);
    decks[0].cue_in_ms = 10000;
    decks[0].cue_out_ms = 40000;
    have_playout = TRUE;
    playout = (RadioSPlayoutSnapshot){ .state = RADIOS_PLAYOUT_PLAYING,
        .deck = 0, .cue_in_ms = 5000, .duration_ms = 50000 };
    create(fixture);
    assert_state(fixture, 5000, 30000, TRUE, TRUE);
    key(fixture, GDK_KEY_Home);
    g_assert_cmpuint(last_position, ==, 10000);
    key(fixture, GDK_KEY_End);
    g_assert_cmpuint(last_position, ==, 40000);
}

static void
invalid_cue_span(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(0, RADIOS_AUDIO_UI_PLAYING, 1000, 30000);
    decks[0].cue_in_ms = 20000;
    decks[0].cue_out_ms = 10000;
    create(fixture);
    assert_state(fixture, 0, 0, TRUE, FALSE);
    key(fixture, GDK_KEY_Home);
    g_assert_cmpuint(seek_calls, ==, 0);
}

static void
source_error(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(3, RADIOS_AUDIO_UI_PLAYING, 3000, 10000);
    g_strlcpy(decks[3].error, "Çıkış hatası", sizeof decks[3].error);
    create(fixture);
    assert_state(fixture, 3000, 10000, TRUE, FALSE);
    key(fixture, GDK_KEY_End);
    g_assert_cmpuint(seek_calls, ==, 0);
}

static void
seek_error(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(1, RADIOS_AUDIO_UI_PLAYING, 3000, 10000);
    create(fixture);
    reject_seek = TRUE;
    key(fixture, GDK_KEY_End);
    g_assert_cmpuint(seek_calls, ==, 1);
    g_assert_cmpstr(fixture->notice, ==, "Deneme seek hatası");
    assert_state(fixture, 3000, 10000, TRUE, TRUE);
}

static void
atomic_generation_guard(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(0, RADIOS_AUDIO_UI_PLAYING, 1000, 10000);
    create(fixture);
    /* Emulate a runner replacement after the adapter's final snapshot but
     * before the engine validates the expected generation under its lock. */
    replace_inside_seek = TRUE;
    key(fixture, GDK_KEY_End);
    g_assert_cmpuint(seek_calls, ==, 1);
    g_assert_cmpuint(last_generation, ==, 7);
    g_assert_cmpuint(accepted_seeks, ==, 0);
    g_assert_cmpstr(fixture->notice, ==, "Şarkı kimliği değişti");
    assert_state(fixture, 1000, 10000, TRUE, TRUE);
}

static void
replace_on_begin(GtkWidget *widget, gpointer data)
{
    (void)widget;
    guint which = GPOINTER_TO_UINT(data);
    if (which == 0)
        ++decks[0].generation;
    else if (which == 1)
        decks[0].media_id[0] = '9';
    else if (which == 2)
        decks[0].version_id[0] = '9';
    else if (which == 3) {
        decks[0].main_enabled = FALSE;
        load_deck(1, RADIOS_AUDIO_UI_PLAYING, 4000, 10000);
    } else if (which == 4)
        decks[0].output_ready = FALSE;
    else
        available[0] = FALSE;
}

static void
identity_at_commit(Fixture *fixture, gconstpointer data)
{
    load_deck(0, RADIOS_AUDIO_UI_PLAYING, 1000, 10000);
    create(fixture);
    g_signal_connect(fixture->widget, "seek-begin", G_CALLBACK(replace_on_begin), (gpointer)data);
    key(fixture, GDK_KEY_End);
    g_assert_cmpuint(seek_calls, ==, 0);
}

static void
identity_between_press_release(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(0, RADIOS_AUDIO_UI_PLAYING, 1000, 10000);
    create(fixture);
    press(fixture);
    g_assert_true(gtk_widget_has_grab(fixture->widget));
    ++decks[0].generation;
    release(fixture);
    g_assert_cmpuint(seek_calls, ==, 0);
    g_assert_false(gtk_widget_has_grab(fixture->widget));
}

static void
identity_poll_cancels_drag(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(0, RADIOS_AUDIO_UI_PLAYING, 1000, 10000);
    create(fixture);
    press(fixture);
    load_deck(0, RADIOS_AUDIO_UI_PLAYING, 2000, 10000);
    ++decks[0].generation;
    pump(70);
    g_assert_false(gtk_widget_has_grab(fixture->widget));
    release(fixture);
    g_assert_cmpuint(seek_calls, ==, 0);
}

static void
valid_drag(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(7, RADIOS_AUDIO_UI_PLAYING, 1000, 10000);
    create(fixture);
    press(fixture);
    /* Ordinary PCM progress during dragging preserves the source identity. */
    decks[7].position_ms = 2000;
    pump(70);
    g_assert_true(gtk_widget_has_grab(fixture->widget));
    release(fixture);
    g_assert_cmpuint(seek_calls, ==, 1);
    g_assert_cmpuint(last_deck, ==, 7);
    g_assert_cmpuint(last_position, >, 4000);
    g_assert_cmpuint(last_position, <, 6000);
}

static void
destroy_stops_poll(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(0, RADIOS_AUDIO_UI_PLAYING, 1000, 10000);
    create(fixture);
    press(fixture);
    gtk_widget_destroy(fixture->widget);
    guint before = snapshot_calls;
    pump(80);
    g_assert_cmpuint(snapshot_calls, ==, before);
    g_assert_false(gtk_widget_has_grab(fixture->widget));
    g_assert_cmpuint(seek_calls, ==, 0);
}

static void
retain_seeked_paused_source(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(0, RADIOS_AUDIO_UI_PAUSED, 2000, 10000);
    load_deck(1, RADIOS_AUDIO_UI_PAUSED, 3000, 20000);
    create(fixture); load_after_seek = TRUE;
    key(fixture, GDK_KEY_Right);
    g_assert_cmpuint(last_deck, ==, 0);
    pump(70);
    assert_state(fixture, 7000, 10000, FALSE, FALSE);
    decks[0].state = RADIOS_AUDIO_UI_READY; decks[0].output_ready = TRUE;
    pump(70);
    assert_state(fixture, 7000, 10000, FALSE, TRUE);
    load_deck(2, RADIOS_AUDIO_UI_PLAYING, 4000, 30000);
    pump(70);
    assert_state(fixture, 4000, 30000, TRUE, TRUE);
}

static void
seek_preference_expires_on_replacement(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_deck(0, RADIOS_AUDIO_UI_PAUSED, 2000, 10000);
    load_deck(1, RADIOS_AUDIO_UI_PAUSED, 3000, 20000);
    create(fixture); load_after_seek = TRUE;
    key(fixture, GDK_KEY_Right);
    decks[0].state = RADIOS_AUDIO_UI_READY; decks[0].output_ready = TRUE;
    ++decks[0].generation;
    pump(70);
    assert_state(fixture, 3000, 20000, FALSE, TRUE);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check(&argc, &argv)) {
        g_printerr("GTK display is unavailable.\n");
        return 77;
    }
    g_test_add("/header-progress/empty", Fixture, NULL, setup, no_source, teardown);
    g_test_add("/header-progress/playing-priority", Fixture, NULL, setup, source_priority, teardown);
    g_test_add("/header-progress/paused-priority", Fixture, NULL, setup, paused_over_ready, teardown);
    g_test_add("/header-progress/cue-excluded", Fixture, NULL, setup, cue_excluded, teardown);
    g_test_add("/header-progress/playout-cue-span", Fixture, NULL, setup, playout_priority_and_cues, teardown);
    g_test_add("/header-progress/logical-runner-paused", Fixture, GUINT_TO_POINTER(RADIOS_PLAYOUT_PAUSED), setup, logical_runner_pause, teardown);
    g_test_add("/header-progress/private-runner-paused", Fixture, NULL, setup, private_runner_paused, teardown);
    g_test_add("/header-progress/logical-runner-waiting", Fixture, GUINT_TO_POINTER(RADIOS_PLAYOUT_WAITING), setup, logical_runner_pause, teardown);
    g_test_add("/header-progress/logical-runner-manual-wait", Fixture, GUINT_TO_POINTER(RADIOS_PLAYOUT_MANUAL_WAIT), setup, logical_runner_pause, teardown);
    g_test_add("/header-progress/unavailable-output", Fixture, NULL, setup, unavailable_output, teardown);
    g_test_add("/header-progress/manual-cue-span", Fixture, NULL, setup, manual_cue_span, teardown);
    g_test_add("/header-progress/playout-physical-cue-intersection", Fixture, NULL, setup, playout_physical_cue_intersection, teardown);
    g_test_add("/header-progress/invalid-cue-span", Fixture, NULL, setup, invalid_cue_span, teardown);
    g_test_add("/header-progress/source-error", Fixture, NULL, setup, source_error, teardown);
    g_test_add("/header-progress/seek-error", Fixture, NULL, setup, seek_error, teardown);
    g_test_add("/header-progress/atomic-generation-guard", Fixture, NULL, setup, atomic_generation_guard, teardown);
    g_test_add("/header-progress/stale-generation", Fixture, GUINT_TO_POINTER(0), setup, identity_at_commit, teardown);
    g_test_add("/header-progress/stale-media", Fixture, GUINT_TO_POINTER(1), setup, identity_at_commit, teardown);
    g_test_add("/header-progress/stale-version", Fixture, GUINT_TO_POINTER(2), setup, identity_at_commit, teardown);
    g_test_add("/header-progress/stale-deck", Fixture, GUINT_TO_POINTER(3), setup, identity_at_commit, teardown);
    g_test_add("/header-progress/lost-output-during-gesture", Fixture, GUINT_TO_POINTER(4), setup, identity_at_commit, teardown);
    g_test_add("/header-progress/unloaded-during-gesture", Fixture, GUINT_TO_POINTER(5), setup, identity_at_commit, teardown);
    g_test_add("/header-progress/stale-drag-release", Fixture, NULL, setup, identity_between_press_release, teardown);
    g_test_add("/header-progress/poll-cancels-stale-drag", Fixture, NULL, setup, identity_poll_cancels_drag, teardown);
    g_test_add("/header-progress/valid-drag", Fixture, NULL, setup, valid_drag, teardown);
    g_test_add("/header-progress/destroy-stops-poll", Fixture, NULL, setup, destroy_stops_poll, teardown);
    g_test_add("/header-progress/retain-seeked-paused-source", Fixture, NULL, setup, retain_seeked_paused_source, teardown);
    g_test_add("/header-progress/seek-preference-expires-on-replacement", Fixture, NULL, setup, seek_preference_expires_on_replacement, teardown);
    return g_test_run();
}
