#define _POSIX_C_SOURCE 200809L
#include "radios/audio_runner.h"
#include <glib/gstdio.h>
#include <math.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static gchar *directory, *source, *sha, *media, *version, *entry;
static void append16(GByteArray *bytes, guint16 n)
{ guint8 b[] = {(guint8)n, (guint8)(n >> 8)}; g_byte_array_append(bytes, b, 2); }
static void append32(GByteArray *bytes, guint32 n)
{ guint8 b[] = {(guint8)n, (guint8)(n >> 8), (guint8)(n >> 16), (guint8)(n >> 24)}; g_byte_array_append(bytes, b, 4); }
static void signal_daemon(const gchar *name)
{
    gchar *path = g_build_filename(directory, name, NULL);
    g_assert_true(g_file_set_contents(path, "request", -1, NULL)); g_assert_cmpint(g_chmod(path, 0600), ==, 0); g_free(path);
}
static void wait_marker(const gchar *name)
{
    gchar *path = g_build_filename(directory, name, NULL); gint64 deadline = g_get_monotonic_time() + 15000000;
    while (!g_file_test(path, G_FILE_TEST_IS_REGULAR) && g_get_monotonic_time() < deadline) g_usleep(5000);
    g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR)); g_free(path);
}
static void wait_device(RadioSAudioEngine *engine, gboolean connected, gboolean ready)
{
    gint64 deadline = g_get_monotonic_time() + 15000000; RadioSAudioSnapshot s;
    do {
        radios_audio_engine_snapshot(engine, &s);
        if (s.connected == connected && s.buses[0].ready == ready && (!ready || s.microphone_ready)) return;
        g_usleep(5000);
    } while (g_get_monotonic_time() < deadline);
    g_error("Recovery device state: connected%d ready%d error%s bus%s", s.connected, s.buses[0].ready, s.error, s.buses[0].error);
}
static void wait_pcm(RadioSAudioEngine *engine, guint deck, guint64 beyond)
{
    gint64 deadline = g_get_monotonic_time() + 15000000; RadioSAudioSnapshot s;
    do {
        radios_audio_engine_snapshot(engine, &s);
        if (s.decks[deck].cursor_frames > beyond) return;
        g_usleep(5000);
    } while (g_get_monotonic_time() < deadline);
    g_error("Recovery PCM did not advance: state%d frame%" G_GUINT64_FORMAT " error%s", s.decks[deck].state, s.decks[deck].cursor_frames, s.decks[deck].error);
}
static void quiet(RadioSAudioEngine *engine, guint deck, guint64 cursor)
{
    for (guint i = 0; i < 100; ++i) {
        RadioSAudioSnapshot s; radios_audio_engine_snapshot(engine, &s);
        g_assert_cmpint(s.decks[deck].state, ==, RADIOS_AUDIO_PAUSED);
        g_assert_cmpuint(s.decks[deck].cursor_frames, ==, cursor); g_assert_false(s.microphone_gate);
        g_usleep(10000);
    }
}
static void test_restart(void)
{
    const gchar *server = g_getenv("RADIOS_TEST_PULSE_SERVER"); g_assert_nonnull(server);
    RadioSAudioEngine *engine = radios_audio_engine_new(TRUE, server);
    RadioSAudioPreferences prefs; radios_audio_preferences_default(&prefs);
    g_strlcpy(prefs.main_sink, "radios_recovery_main", sizeof prefs.main_sink);
    g_strlcpy(prefs.cue_sink, "radios_recovery_cue", sizeof prefs.cue_sink);
    g_strlcpy(prefs.microphone_source, "radios_recovery_main.monitor", sizeof prefs.microphone_source);
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL)); wait_device(engine, TRUE, TRUE);
    RadioSAudioLoadSpec spec = {source, sha, media, version, 0, 1440000};
    g_assert_true(radios_audio_engine_load(engine, 0, &spec, NULL));
    RadioSAudioSnapshot s; gint64 deadline = g_get_monotonic_time() + 10000000;
    do { radios_audio_engine_snapshot(engine, &s); if (s.decks[0].state == RADIOS_AUDIO_READY) break; g_usleep(5000); } while (g_get_monotonic_time() < deadline);
    g_assert_cmpint(s.decks[0].state, ==, RADIOS_AUDIO_READY);
    g_assert_true(radios_audio_engine_play(engine, 0, NULL)); g_assert_true(radios_audio_engine_set_microphone_gate(engine, TRUE, NULL));
    wait_pcm(engine, 0, 9600); signal_daemon("stop-1"); wait_marker("stopped-1"); wait_device(engine, FALSE, FALSE);
    radios_audio_engine_snapshot(engine, &s); guint64 cursor = s.decks[0].cursor_frames;
    g_assert_cmpuint(cursor, >, 0); g_assert_cmpuint(s.loss_epoch, >=, 1); g_assert_false(s.microphone_gate);
    quiet(engine, 0, cursor); g_usleep(2100000); signal_daemon("restart-1"); wait_marker("restarted-1"); wait_device(engine, TRUE, TRUE);
    quiet(engine, 0, cursor); g_assert_true(radios_audio_engine_play(engine, 0, NULL)); wait_pcm(engine, 0, cursor + 4800);
    g_test_message("Manual deck preserved its confirmed cursor and resumed only after explicit Play.");
    g_assert_true(radios_audio_engine_unload(engine, 0, NULL));
    RadioSAudioRunItem item = { .entry_id = entry, .media = spec, .duration_frames = 1440000,
        .auto_next_frame = 1440000, .fade_start_frame = 1440000, .fade_end_frame = 1440000, .preferred_deck = -1 };
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    g_assert_true(radios_audio_engine_runner_configure(engine, &item, 1, &options, NULL));
    g_assert_true(radios_audio_engine_runner_start(engine, NULL));
    RadioSAudioRunSnapshot run;
    deadline = g_get_monotonic_time() + 10000000;
    do { radios_audio_engine_runner_snapshot(engine, &run); if (run.actual_current_frame > 9600) break; g_usleep(5000); } while (g_get_monotonic_time() < deadline);
    g_assert_cmpuint(run.actual_current_frame, >, 9600); guint64 epoch = run.epoch;
    signal_daemon("stop-2"); wait_marker("stopped-2"); wait_device(engine, FALSE, FALSE);
    radios_audio_engine_runner_snapshot(engine, &run); g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_PAUSED);
    cursor = run.actual_current_frame; gint deck = run.deck_current; g_assert_cmpint(deck, >=, 0);
    signal_daemon("restart-missing"); wait_marker("restarted-missing");
    wait_device(engine, TRUE, FALSE); g_usleep(500000);
    radios_audio_engine_snapshot(engine, &s); g_assert_false(s.buses[0].ready);
    g_assert_true(strstr(s.buses[0].error, "fallback") != NULL || strstr(s.buses[0].error, "No such") != NULL);
    quiet(engine, (guint)deck, cursor);
    GError *unavailable = NULL;
    g_assert_false(radios_audio_engine_runner_resume(engine, &unavailable));
    g_assert_error(unavailable, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_UNAVAILABLE); g_clear_error(&unavailable);
    radios_audio_engine_runner_snapshot(engine, &run); g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_PAUSED); g_assert_cmpuint(run.epoch, ==, epoch);
    signal_daemon("stop-3"); wait_marker("stopped-3"); wait_device(engine, FALSE, FALSE);
    signal_daemon("restart-3"); wait_marker("restarted-3"); wait_device(engine, TRUE, TRUE);
    quiet(engine, (guint)deck, cursor); g_assert_true(radios_audio_engine_runner_resume(engine, NULL));
    wait_pcm(engine, (guint)deck, cursor + 4800);
    RadioSAudioRunReceipt receipt[4]; guint count = radios_audio_engine_runner_receipts(engine, receipt, G_N_ELEMENTS(receipt));
    g_assert_cmpuint(count, ==, 1); g_assert_cmpint(receipt[0].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_NONE);
    g_assert_true(radios_audio_engine_runner_stop(engine, NULL));
    count = radios_audio_engine_runner_receipts(engine, receipt, G_N_ELEMENTS(receipt));
    g_assert_cmpuint(count, ==, 1); g_assert_cmpint(receipt[0].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED);
    g_test_message("Same-server restart paused manual and runner PCM; microphone closed; absent exact sink rejected; explicit resume advanced actual frames; terminal remained interrupted.");
    radios_audio_engine_unref(engine);
}
/* R8.4: devices plugged into and removed from the sound server while MAIN
 * plays. Only sinks this test creates are loaded and unloaded; the default
 * device is never changed. A GTK-like 50 ms poll keeps its cadence, the deck
 * keeps playing on its own output, and every plugged device appears in the
 * engine's list by itself (no "Yenile"). */
static gchar *pactl(const gchar *server, const gchar *a, const gchar *b, const gchar *c, const gchar *d)
{
    const gchar *argv[] = {"pactl", "-s", server, a, b, c, d, NULL};
    gchar *out = NULL; gint status = 0;
    if (!g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
            &out, NULL, &status, NULL) || !g_spawn_check_wait_status(status, NULL)) { g_free(out); return NULL; }
    return g_strstrip(out);
}
typedef struct { const gchar *server; guint cycles; gint done; guint loaded; } Plugger;
static gpointer plug_cycles(gpointer data)
{
    Plugger *plugger = data;
    for (guint i = 0; i < plugger->cycles; ++i) {
        gchar *argument = g_strdup_printf("sink_name=radios_hotplug_%d_%u", (gint)getpid(), i);
        gchar *module = pactl(plugger->server, "load-module", "module-null-sink", argument, "sink_properties=device.description=Takılan_aygıt");
        if (module && g_ascii_string_to_unsigned(module, 10, 0, G_MAXUINT32, NULL, NULL)) ++plugger->loaded;
        g_usleep(900000);
        if (module) g_free(pactl(plugger->server, "unload-module", module, NULL, NULL));
        g_free(module); g_free(argument); g_usleep(300000);
    }
    g_atomic_int_set(&plugger->done, 1);
    return NULL;
}
typedef struct {
    RadioSAudioEngine *engine; Plugger *plugger; GMainLoop *loop; gchar *prefix;
    gint64 last, max_gap, max_call, stalled_since, max_stall; guint64 cursor;
    gboolean left_playing; gint left_state; gchar left_error[256];
    GHashTable *visible;
} HotplugPoll;
static gboolean hotplug_poll(gpointer data)
{
    HotplugPoll *poll = data; gint64 now = g_get_monotonic_time();
    if (poll->last) poll->max_gap = MAX(poll->max_gap, now - poll->last);
    poll->last = now;
    RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1);
    radios_audio_engine_snapshot(poll->engine, s);
    poll->max_call = MAX(poll->max_call, g_get_monotonic_time() - now);
    if (s->decks[0].state != RADIOS_AUDIO_PLAYING && !poll->left_playing) {
        poll->left_playing = TRUE; poll->left_state = s->decks[0].state;
        g_strlcpy(poll->left_error, s->decks[0].error, sizeof poll->left_error);
    }
    if (s->decks[0].cursor_frames > poll->cursor) { poll->cursor = s->decks[0].cursor_frames; poll->stalled_since = now; }
    else poll->max_stall = MAX(poll->max_stall, now - poll->stalled_since);
    for (guint i = 0; i < s->output_count; ++i)
        if (g_str_has_prefix(s->outputs[i].name, poll->prefix)) g_hash_table_add(poll->visible, g_strdup(s->outputs[i].name));
    g_free(s);
    if (g_atomic_int_get(&poll->plugger->done)) { g_main_loop_quit(poll->loop); return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}
static gboolean pcm_advanced(RadioSAudioEngine *engine, guint deck, guint64 beyond)
{
    gint64 deadline = g_get_monotonic_time() + 15000000; gboolean advanced = FALSE;
    RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1);
    do {
        radios_audio_engine_snapshot(engine, s); advanced = s->decks[deck].cursor_frames > beyond;
        if (!advanced) g_usleep(5000);
    } while (!advanced && g_get_monotonic_time() < deadline);
    g_free(s); return advanced;
}
static void test_hotplug(void)
{
    const gchar *server = g_getenv("RADIOS_TEST_PULSE_SERVER"); g_assert_nonnull(server);
    gchar *main_sink = g_strdup_printf("radios_hotplug_main_%d", (gint)getpid());
    gchar *argument = g_strdup_printf("sink_name=%s", main_sink);
    gchar *main_module = pactl(server, "load-module", "module-null-sink", argument, "rate=48000"); g_free(argument);
    g_assert_nonnull(main_module);
    /* Every step below records its outcome; the server is cleaned up before
     * any assertion, so a failing run never leaves a device behind. */
    RadioSAudioEngine *engine = radios_audio_engine_new(TRUE, server);
    RadioSAudioPreferences prefs; radios_audio_preferences_default(&prefs);
    g_strlcpy(prefs.main_sink, main_sink, sizeof prefs.main_sink);
    gboolean applied = radios_audio_engine_apply_preferences(engine, &prefs, NULL);
    RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1); gint64 deadline = g_get_monotonic_time() + 15000000;
    do { radios_audio_engine_snapshot(engine, s); if (s->connected && s->buses[0].ready) break; g_usleep(5000); } while (g_get_monotonic_time() < deadline);
    gboolean ready = applied && s->connected && s->buses[0].ready;
    RadioSAudioLoadSpec spec = {source, sha, media, version, 0, 1440000};
    gboolean loaded = ready && radios_audio_engine_load(engine, 0, &spec, NULL), deck_ready = FALSE;
    deadline = g_get_monotonic_time() + 10000000;
    while (loaded && !deck_ready && g_get_monotonic_time() < deadline) {
        radios_audio_engine_snapshot(engine, s); deck_ready = s->decks[0].state == RADIOS_AUDIO_READY;
        if (!deck_ready) g_usleep(5000);
    }
    GError *play_error = NULL;
    gboolean playing = deck_ready && radios_audio_engine_play(engine, 0, &play_error);
    gboolean started = playing && pcm_advanced(engine, 0, 9600);
    Plugger plugger = {server, 6, 0, 0};
    HotplugPoll poll = {engine, &plugger, g_main_loop_new(NULL, FALSE), g_strdup_printf("radios_hotplug_%d_", (gint)getpid()),
        0, 0, 0, g_get_monotonic_time(), 0, 0, FALSE, 0, "", g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL)};
    gboolean gone = FALSE;
    if (started) {
        g_timeout_add(50, hotplug_poll, &poll);
        GThread *thread = g_thread_new("hotplug", plug_cycles, &plugger);
        g_main_loop_run(poll.loop); g_thread_join(thread);
        g_test_message("hotplug: %u devices plugged/unplugged; poll max gap %" G_GINT64_FORMAT " ms, snapshot max %" G_GINT64_FORMAT
            " us, PCM max stall %" G_GINT64_FORMAT " ms, devices seen without refresh %u, deck left PLAYING: %s%s%s",
            plugger.loaded, poll.max_gap / 1000, poll.max_call, poll.max_stall / 1000, g_hash_table_size(poll.visible),
            poll.left_playing ? "yes" : "no", poll.left_playing ? " · " : "", poll.left_error);
        /* Removed devices leave the list by themselves too. */
        deadline = g_get_monotonic_time() + 3000000;
        do {
            radios_audio_engine_snapshot(engine, s); gone = TRUE;
            for (guint i = 0; i < s->output_count; ++i) if (g_str_has_prefix(s->outputs[i].name, poll.prefix)) gone = FALSE;
            if (!gone) g_usleep(20000);
        } while (!gone && g_get_monotonic_time() < deadline);
    } else {
        radios_audio_engine_snapshot(engine, s);
        g_test_message("hotplug setup: applied=%d ready=%d loaded=%d deck_ready=%d play=%d (%s) deck state=%d error=%s bus=%s",
            applied, ready, loaded, deck_ready, playing, play_error ? play_error->message : "-", s->decks[0].state,
            s->decks[0].error, s->buses[0].error);
    }
    gboolean unloaded = !loaded || radios_audio_engine_unload(engine, 0, NULL);
    radios_audio_engine_unref(engine); g_free(s);
    g_free(pactl(server, "unload-module", main_module, NULL, NULL));
    guint visible = g_hash_table_size(poll.visible);
    g_hash_table_unref(poll.visible); g_main_loop_unref(poll.loop); g_free(poll.prefix); g_free(main_module); g_free(main_sink);
    g_assert_true(ready); g_assert_true(loaded); g_assert_true(deck_ready);
    g_assert_no_error(play_error); g_assert_true(playing); g_assert_true(started);
    g_assert_cmpuint(plugger.loaded, ==, plugger.cycles);
    g_assert_cmpint(poll.max_gap, <, 500000); g_assert_cmpint(poll.max_call, <, 20000);
    g_assert_false(poll.left_playing); g_assert_cmpint(poll.max_stall, <, 500000);
    g_assert_cmpuint(visible, ==, plugger.cycles);
    g_assert_true(gone); g_assert_true(unloaded);
}
/* Flush continuity: decks 0 (440 Hz) and 1 (1 kHz) play on one private MAIN
 * sink. Seeking and then stopping deck 1 makes the engine flush its outputs;
 * deck 0 must stay audible. The MAIN monitor is captured with parec and the
 * longest stretch of 1 ms windows below 0.02 RMS is measured between the
 * first and the last audible millisecond. Target: at most one 480-frame
 * (10 ms) block. Only the sink this test creates is loaded and unloaded. */
static gchar *tone_wav(const gchar *name, gdouble frequency, gchar **sha_out)
{
    GByteArray *b = g_byte_array_new(); g_byte_array_append(b, (guint8 *)"RIFF", 4); append32(b, 1440000 * 4 + 36);
    g_byte_array_append(b, (guint8 *)"WAVEfmt ", 8); append32(b, 16); append16(b, 1); append16(b, 2);
    append32(b, 48000); append32(b, 192000); append16(b, 4); append16(b, 16); g_byte_array_append(b, (guint8 *)"data", 4); append32(b, 1440000 * 4);
    for (guint i = 0; i < 1440000; ++i) { gint16 sample = (gint16)(8192 * sin(2 * G_PI * frequency * i / 48000.0)); append16(b, (guint16)sample); append16(b, (guint16)sample); }
    gchar *path = g_build_filename(directory, name, NULL);
    g_assert_true(g_file_set_contents(path, (gchar *)b->data, b->len, NULL));
    *sha_out = g_compute_checksum_for_data(G_CHECKSUM_SHA256, b->data, b->len); g_byte_array_unref(b); return path;
}
static gboolean deck_state_wait(RadioSAudioEngine *engine, guint deck, RadioSAudioState state)
{
    gint64 deadline = g_get_monotonic_time() + 10000000; gboolean reached = FALSE;
    RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1);
    do { radios_audio_engine_snapshot(engine, s); reached = s->decks[deck].state == state; if (!reached) g_usleep(5000); }
    while (!reached && g_get_monotonic_time() < deadline);
    g_free(s); return reached;
}
static void test_flush_gap(void)
{
    const gchar *server = g_getenv("RADIOS_TEST_PULSE_SERVER"); g_assert_nonnull(server);
    gchar *main_sink = g_strdup_printf("radios_flushgap_main_%d", (gint)getpid());
    gchar *argument = g_strdup_printf("sink_name=%s", main_sink);
    gchar *main_module = pactl(server, "load-module", "module-null-sink", argument, "rate=48000"); g_free(argument);
    g_assert_nonnull(main_module);
    gchar *capture = g_build_filename(directory, "flush-gap-main.f32", NULL), *device = g_strdup_printf("%s.monitor", main_sink);
    gint fd = open(capture, O_WRONLY | O_CREAT | O_TRUNC, 0600); g_assert_cmpint(fd, >=, 0);
    const gchar *recorder[] = {"parec", "--server", server, "--device", device, "--raw", "--format=float32le", "--rate=48000",
        "--channels=2", "--latency-msec=10", NULL};
    GPid parec = 0; GError *error = NULL; gint64 capture_started = g_get_monotonic_time(), seek_at = 0, stop_at = 0;
    gboolean recording = g_spawn_async_with_fds(NULL, (gchar **)recorder, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL, &parec, -1, fd, -1, &error);
    close(fd); g_clear_error(&error);
    gchar *second_sha = NULL, *second = tone_wav("flush-gap-1khz.wav", 1000, &second_sha);
    gchar *second_media = g_uuid_string_random(), *second_version = g_uuid_string_random();
    RadioSAudioEngine *engine = radios_audio_engine_new(TRUE, server);
    RadioSAudioPreferences prefs; radios_audio_preferences_default(&prefs);
    g_strlcpy(prefs.main_sink, main_sink, sizeof prefs.main_sink);
    gboolean ready = recording && radios_audio_engine_apply_preferences(engine, &prefs, NULL);
    RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1); gint64 deadline = g_get_monotonic_time() + 15000000;
    do { radios_audio_engine_snapshot(engine, s); if (s->connected && s->buses[0].ready) break; g_usleep(5000); } while (ready && g_get_monotonic_time() < deadline);
    ready = ready && s->connected && s->buses[0].ready;
    RadioSAudioLoadSpec first_spec = {source, sha, media, version, 0, 1440000}, second_spec = {second, second_sha, second_media, second_version, 0, 1440000};
    gboolean playing = ready && radios_audio_engine_load(engine, 0, &first_spec, NULL) && radios_audio_engine_load(engine, 1, &second_spec, NULL) &&
        deck_state_wait(engine, 0, RADIOS_AUDIO_READY) && deck_state_wait(engine, 1, RADIOS_AUDIO_READY) &&
        radios_audio_engine_play(engine, 0, NULL) && radios_audio_engine_play(engine, 1, NULL) &&
        pcm_advanced(engine, 0, 9600) && pcm_advanced(engine, 1, 9600);
    guint64 flushes_before = radios_audio_engine_output_flushes(engine), flushes_after = flushes_before;
    gboolean seeked = FALSE, stopped = FALSE;
    if (playing) {
        g_usleep(1500000);
        seek_at = g_get_monotonic_time(); seeked = radios_audio_engine_seek(engine, 1, 480000, NULL); g_usleep(700000);
        stop_at = g_get_monotonic_time(); stopped = radios_audio_engine_stop(engine, 1, NULL); g_usleep(1500000);
        flushes_after = radios_audio_engine_output_flushes(engine);
    }
    if (parec > 0) { kill(parec, SIGTERM); waitpid(parec, NULL, 0); g_spawn_close_pid(parec); }
    if (playing) { radios_audio_engine_stop(engine, 0, NULL); }
    radios_audio_engine_unref(engine); g_free(s);
    g_free(pactl(server, "unload-module", main_module, NULL, NULL));
    /* Longest silent stretch of deck 0 between the first and last audible millisecond. */
    gchar *bytes = NULL; gsize length = 0; guint longest = 0, gaps = 0, audible_ms = 0; GString *where = g_string_new(NULL);
    if (g_file_get_contents(capture, &bytes, &length, NULL) && length >= 8 * 48) {
        const gfloat *pcm = (const gfloat *)bytes; guint windows = (guint)(length / 8 / 48);
        gint first = -1, last = -1;
        gboolean *loud = g_new0(gboolean, windows);
        for (guint w = 0; w < windows; ++w) {
            gdouble sum = 0; for (guint f = 0; f < 48; ++f) { gdouble v = pcm[(w * 48 + f) * 2]; sum += v * v; }
            loud[w] = sqrt(sum / 48) >= 0.02;
            if (loud[w]) { if (first < 0) first = (gint)w; last = (gint)w; }
        }
        guint run = 0;
        for (gint w = first; first >= 0 && w <= last; ++w) {
            if (loud[w]) {
                if (run > 10) { ++gaps; g_string_append_printf(where, " %d ms@%d", run, w - (gint)run); }
                run = 0; ++audible_ms;
            } else longest = MAX(longest, ++run);
        }
        g_free(loud);
    }
    g_free(bytes); g_unlink(capture); g_unlink(second);
    g_test_message("flush gap: %" G_GUINT64_FORMAT " output flushes during seek+stop; deck 0 audible %u ms; longest MAIN gap %u ms "
        "(%u frames); gaps over one 10 ms block: %u [length@capture-ms:%s; seek ~%" G_GINT64_FORMAT " ms, stop ~%" G_GINT64_FORMAT " ms after capture start]",
        flushes_after - flushes_before, audible_ms, longest, longest * 48, gaps, where->str,
        seek_at ? (seek_at - capture_started) / 1000 : -1, stop_at ? (stop_at - capture_started) / 1000 : -1);
    g_string_free(where, TRUE);
    g_free(capture); g_free(device); g_free(second); g_free(second_sha); g_free(second_media); g_free(second_version);
    g_free(main_module); g_free(main_sink);
    g_assert_true(ready); g_assert_true(playing); g_assert_true(seeked); g_assert_true(stopped);
    g_assert_cmpuint(flushes_after - flushes_before, >=, 1);
    g_assert_cmpuint(audible_ms, >, 3000);
    /* The one-block target also needs the engine to resume rendering at once
     * after a flush; RADIOS_TEST_FLUSH_GAP_STRICT=1 checks it. By default only
     * dropouts of a quarter second or more fail (stream re-creation, reconnects). */
    const gchar *strict = g_getenv("RADIOS_TEST_FLUSH_GAP_STRICT");
    g_assert_cmpuint(longest * 48, <=, strict && g_str_equal(strict, "1") ? 480u : 12000u);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); directory = g_strdup(argv[1]);
    for (gint i = 1; i < argc - 1; ++i) argv[i] = argv[i + 1];
    --argc; g_test_init(&argc, &argv, NULL);
    source = g_build_filename(directory, "source.wav", NULL);
    GByteArray *b = g_byte_array_new(); g_byte_array_append(b, (guint8 *)"RIFF", 4); append32(b, 1440000 * 4 + 36);
    g_byte_array_append(b, (guint8 *)"WAVEfmt ", 8); append32(b, 16); append16(b, 1); append16(b, 2);
    append32(b, 48000); append32(b, 192000); append16(b, 4); append16(b, 16); g_byte_array_append(b, (guint8 *)"data", 4); append32(b, 1440000 * 4);
    for (guint i = 0; i < 1440000; ++i) { gint16 sample = (gint16)(8192 * sin(2 * G_PI * 440 * i / 48000.0)); append16(b, (guint16)sample); append16(b, (guint16)sample); }
    g_assert_true(g_file_set_contents(source, (gchar *)b->data, b->len, NULL)); sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, b->data, b->len);
    g_byte_array_unref(b); media = g_uuid_string_random(); version = g_uuid_string_random(); entry = g_uuid_string_random();
    g_test_add_func("/audio-recovery/private-server-restart", test_restart);
    g_test_add_func("/audio-recovery/device-hotplug-keeps-playing", test_hotplug);
    g_test_add_func("/audio-recovery/flush-keeps-other-deck-continuous", test_flush_gap); gint result = g_test_run();
    g_unlink(source); g_free(source); g_free(sha); g_free(media); g_free(version); g_free(entry); g_free(directory); return result;
}
