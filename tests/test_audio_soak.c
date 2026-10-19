#define _POSIX_C_SOURCE 200809L
#include "radios/audio_runner.h"
#include "radios/audio_output.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ASan deliberately retains freed decoder buffers in its quarantine. Leak
 * detection still runs at exit; its instrumentation needs a separate RSS
 * allowance from the uninstrumented real-time acceptance process. */
#if defined(__SANITIZE_ADDRESS__)
#define SOAK_RSS_ALLOWANCE (384u * 1024u * 1024u)
#else
#define SOAK_RSS_ALLOWANCE (64u * 1024u * 1024u)
#endif

/* Real Pulse consumption drives every cursor. This test never calls render().
 * The wrapper owns the isolated daemon, monitor capture and HTTP receiver. */
static void append16(GByteArray *b, guint16 n)
{ guint8 v[] = {(guint8)n, (guint8)(n >> 8)}; g_byte_array_append(b, v, 2); }
static void append32(GByteArray *b, guint32 n)
{ guint8 v[] = {(guint8)n, (guint8)(n >> 8), (guint8)(n >> 16), (guint8)(n >> 24)}; g_byte_array_append(b, v, 4); }
static void require(gboolean okay, GError **error, const gchar *action)
{ if (!okay) g_error("%s: %s", action, *error ? (*error)->message : "failed"); g_clear_error(error); }
static guint64 rss_bytes(void)
{
    gchar *contents = NULL; guint64 resident = 0;
    if (g_file_get_contents("/proc/self/statm", &contents, NULL, NULL)) {
        gchar **fields = g_strsplit(contents, " ", 3);
        if (fields[1]) resident = g_ascii_strtoull(fields[1], NULL, 10) * (guint64)sysconf(_SC_PAGESIZE);
        g_strfreev(fields); g_free(contents);
    }
    return resident;
}
static guint fd_count(void)
{
    GDir *dir = g_dir_open("/proc/self/fd", 0, NULL); guint n = 0;
    if (dir) { while (g_dir_read_name(dir)) ++n; g_dir_close(dir); }
    return n;
}
static RadioSAudioOutputSnapshot wait_output(RadioSAudioOutput *output, RadioSAudioOutputState wanted)
{
    RadioSAudioOutputSnapshot s; gint64 deadline = g_get_monotonic_time() + 15000000;
    do {
        radios_audio_output_snapshot(output, &s);
        if (s.state == wanted) return s;
        if (s.state == RADIOS_AUDIO_OUTPUT_FAILED) g_error("Output failed: %s", s.error);
        g_usleep(5000);
    } while (g_get_monotonic_time() < deadline);
    g_error("Output timeout, wanted%d actual%d", wanted, s.state); return s;
}
static void member_int(JsonBuilder *b, const gchar *name, guint64 value)
{ json_builder_set_member_name(b, name); json_builder_add_int_value(b, (gint64)value); }
static void member_bool(JsonBuilder *b, const gchar *name, gboolean value)
{ json_builder_set_member_name(b, name); json_builder_add_boolean_value(b, value); }
int main(int argc, char **argv)
{
    if (argc != 3) { g_printerr("Usage: test_audio_soak PRIVATE_DIRECTORY SECONDS\n"); return 2; }
    guint seconds = (guint)g_ascii_strtoull(argv[2], NULL, 10);
    if (seconds < 8 || seconds > 600 || !g_getenv("RADIOS_TEST_PULSE_SERVER") || !g_getenv("RADIOS_TEST_STREAM_URL")) return 2;
    gchar *paths[2], *sha[2], *media[2], *versions[2];
    for (guint file = 0; file < 2; ++file) {
        gchar *name = g_strdup_printf("source-%u.wav", file); paths[file] = g_build_filename(argv[1], name, NULL); g_free(name);
        GByteArray *b = g_byte_array_new(); g_byte_array_append(b, (guint8 *)"RIFF", 4); append32(b, 96000 * 4 + 36);
        g_byte_array_append(b, (guint8 *)"WAVEfmt ", 8); append32(b, 16); append16(b, 1); append16(b, 2);
        append32(b, 48000); append32(b, 192000); append16(b, 4); append16(b, 16); g_byte_array_append(b, (guint8 *)"data", 4); append32(b, 96000 * 4);
        for (guint i = 0; i < 96000; ++i) {
            gint16 sample = (gint16)lrint(6000 * sin(2 * G_PI * (file ? 660 : 440) * i / 48000.0));
            append16(b, (guint16)sample); append16(b, (guint16)(-sample / 2));
        }
        g_assert_true(g_file_set_contents(paths[file], (gchar *)b->data, b->len, NULL));
        sha[file] = g_compute_checksum_for_data(G_CHECKSUM_SHA256, b->data, b->len); g_byte_array_unref(b);
        media[file] = g_uuid_string_random(); versions[file] = g_uuid_string_random();
    }
    RadioSAudioRunItem items[400] = {0}; guint total = MIN(400u, seconds / 1 + 8u);
    for (guint i = 0; i < total; ++i) {
        guint file = i % 2; items[i].entry_id = g_uuid_string_random();
        items[i].media = (RadioSAudioLoadSpec){paths[file], sha[file], media[file], versions[file], 0, 96000};
        items[i].duration_frames = items[i].auto_next_frame = items[i].fade_start_frame = items[i].fade_end_frame = 96000;
        items[i].preferred_deck = -1; items[i].mode = i % 2 ? RADIOS_AUDIO_RUN_EOF : RADIOS_AUDIO_RUN_CROSSFADE;
        items[i].overlap_frames = i % 2 ? 0 : 12000;
        items[i].fade_in_frames = i && (i - 1) % 2 == 0 ? 12000 : 0;
    }
    RadioSAudioEngine *engine = radios_audio_engine_new(TRUE, g_getenv("RADIOS_TEST_PULSE_SERVER"));
    RadioSAudioPreferences preferences; radios_audio_preferences_default(&preferences);
    g_strlcpy(preferences.main_sink, "radios_soak_main", sizeof preferences.main_sink);
    g_strlcpy(preferences.cue_sink, "radios_soak_cue", sizeof preferences.cue_sink);
    GError *error = NULL; require(radios_audio_engine_apply_preferences(engine, &preferences, &error), &error, "route private devices");
    RadioSAudioSnapshot audio; gint64 deadline = g_get_monotonic_time() + 15000000;
    do { radios_audio_engine_snapshot(engine, &audio); if (audio.connected && audio.buses[0].ready) break; g_usleep(5000); } while (g_get_monotonic_time() < deadline);
    g_assert_true(audio.connected); g_assert_true(audio.buses[0].ready); g_assert_false(audio.microphone_gate);
    RadioSAudioOutput *record = radios_audio_engine_output_acquire(engine, RADIOS_AUDIO_OUTPUT_RECORD);
    RadioSAudioOutput *stream = radios_audio_engine_output_acquire(engine, RADIOS_AUDIO_OUTPUT_STREAM);
    gchar *record_path = g_build_filename(argv[1], "programme.wav", NULL);
    require(radios_audio_output_start_record(record, record_path, &error), &error, "start WAV"); wait_output(record, RADIOS_AUDIO_OUTPUT_ACTIVE);
    require(radios_audio_output_start_stream(stream, g_getenv("RADIOS_TEST_STREAM_URL"), "source", "isolated-fixture", 128, &error), &error, "start MP3");
    wait_output(stream, RADIOS_AUDIO_OUTPUT_ACTIVE);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    require(radios_audio_engine_runner_configure(engine, items, 4, &options, &error), &error, "configure programme");
    require(radios_audio_engine_runner_start(engine, &error), &error, "start programme");
    RadioSAudioRunSnapshot run; deadline = g_get_monotonic_time() + 15000000;
    do { radios_audio_engine_runner_snapshot(engine, &run); if (run.actual_current_frame > 4800) break; g_usleep(5000); } while (g_get_monotonic_time() < deadline);
    g_assert_cmpuint(run.actual_current_frame, >, 4800);
    gint64 started = g_get_monotonic_time(), until = started + (gint64)seconds * G_USEC_PER_SEC;
    guint64 epoch = run.epoch, previous_frame = run.actual_current_frame, consumed_start, previous_consumed, rss_start = rss_bytes(), rss_max = rss_start, rss_warm = 0;
    guint previous_index = run.index, appended = 4, append_calls = 0, transitions = 0, overlap_observations = 0, samples = 0, fd_start = fd_count(), fd_max = fd_start;
    radios_audio_engine_snapshot(engine, &audio); consumed_start = previous_consumed = audio.buses[0].consumed_frames;
    guint64 initial_loss_epoch = audio.loss_epoch; gint64 next_metrics = started;
    JsonBuilder *metrics = json_builder_new(); json_builder_begin_array(metrics);
    while (g_get_monotonic_time() < until) {
        radios_audio_engine_snapshot(engine, &audio); radios_audio_engine_runner_snapshot(engine, &run);
        if (!audio.connected || !audio.buses[0].ready || run.state == RADIOS_AUDIO_RUN_ERROR) g_error("Soak device/programme failure: %s %s", audio.error, run.error);
        g_assert_false(audio.microphone_gate); g_assert_cmpuint(audio.loss_epoch, ==, initial_loss_epoch);
        g_assert_cmpuint(audio.buses[0].consumed_frames, >=, previous_consumed); previous_consumed = audio.buses[0].consumed_frames;
        g_assert_cmpuint(run.epoch, ==, epoch); g_assert_cmpuint(run.index, >=, previous_index);
        if (run.index == previous_index) g_assert_cmpuint(run.actual_current_frame, >=, previous_frame);
        else transitions += run.index - previous_index;
        previous_index = run.index; previous_frame = run.actual_current_frame;
        if (run.state == RADIOS_AUDIO_RUN_OVERLAP) ++overlap_observations;
        if (appended < total && appended - run.index <= 3) {
            guint count = MIN(4u, total - appended);
            require(radios_audio_engine_runner_append(engine, items + appended, count, &error), &error, "live queue append");
            appended += count; ++append_calls;
        }
        RadioSAudioOutputSnapshot rec, net; radios_audio_output_snapshot(record, &rec); radios_audio_output_snapshot(stream, &net);
        if (rec.state != RADIOS_AUDIO_OUTPUT_ACTIVE || net.state != RADIOS_AUDIO_OUTPUT_ACTIVE) g_error("Soak output failed: record%s stream%s", rec.error, net.error);
        g_assert_cmpuint(rec.overrun_frames, ==, 0); g_assert_cmpuint(net.overrun_frames, ==, 0);
        gint64 now = g_get_monotonic_time();
        if (now >= next_metrics) {
            guint64 rss = rss_bytes(); guint fds = fd_count(); rss_max = MAX(rss_max, rss); fd_max = MAX(fd_max, fds);
            if (!rss_warm && now - started >= 30000000) rss_warm = rss;
            json_builder_begin_object(metrics); member_int(metrics, "elapsed_ms", (guint64)((now - started) / 1000));
            member_int(metrics, "rss_bytes", rss); member_int(metrics, "fds", fds); member_int(metrics, "queue_index", run.index);
            member_int(metrics, "consumed_frames", audio.buses[0].consumed_frames - consumed_start); json_builder_end_object(metrics);
            ++samples; next_metrics = now + G_USEC_PER_SEC;
            if (samples % 30 == 0) { g_print("Soak elapsed %" G_GINT64_FORMAT "s, consumed %" G_GUINT64_FORMAT " frames, transitions %u, RSS %" G_GUINT64_FORMAT "\n", (now - started) / G_USEC_PER_SEC, audio.buses[0].consumed_frames - consumed_start, transitions, rss); fflush(stdout); }
        }
        g_usleep(20000);
    }
    gint64 elapsed = g_get_monotonic_time() - started; radios_audio_engine_snapshot(engine, &audio);
    guint64 consumed = audio.buses[0].consumed_frames - consumed_start;
    g_assert_cmpuint(consumed, >, (guint64)seconds * 48000 * 9 / 10);
    g_assert_cmpuint(consumed, <, (guint64)seconds * 48000 * 11 / 10);
    g_assert_cmpuint(transitions, >=, seconds / 3); g_assert_cmpuint(append_calls, >, 0); g_assert_cmpuint(overlap_observations, >, 0);
    require(radios_audio_engine_runner_stop(engine, &error), &error, "explicit programme stop");
    radios_audio_output_stop(record); radios_audio_output_stop(stream);
    RadioSAudioOutputSnapshot rec = wait_output(record, RADIOS_AUDIO_OUTPUT_STOPPED), net = wait_output(stream, RADIOS_AUDIO_OUTPUT_STOPPED);
    g_assert_true(rec.recording_finalized); g_assert_cmpuint(rec.frames_received, ==, rec.frames_processed); g_assert_cmpuint(rec.bytes_written, ==, rec.frames_processed * 4);
    g_assert_cmpuint(rec.overrun_frames, ==, 0); g_assert_cmpuint(net.overrun_frames, ==, 0); g_assert_cmpuint(net.bytes_written, >, (guint64)seconds * 12000);
    RadioSAudioRunReceipt receipts[RADIOS_AUDIO_RUN_MAX_RECEIPTS]; guint receipt_count = radios_audio_engine_runner_receipts(engine, receipts, G_N_ELEMENTS(receipts));
    guint completed = 0, interrupted = 0; guint64 receipt_frames = 0;
    for (guint i = 0; i < receipt_count; ++i) {
        g_assert_cmpuint(receipts[i].epoch, ==, epoch); g_assert_cmpuint(receipts[i].index, <, appended);
        g_assert_cmpuint(receipts[i].position_frames, <=, 96000); g_assert_cmpint(receipts[i].first_utc_us, >, 0);
        if (receipts[i].terminal == RADIOS_AUDIO_RUN_TERMINAL_COMPLETE) { ++completed; g_assert_cmpuint(receipts[i].position_frames, ==, 96000); }
        else if (receipts[i].terminal == RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED) ++interrupted;
        else g_error("Unexpected terminal receipt%d", receipts[i].terminal);
        receipt_frames += receipts[i].position_frames;
    }
    g_assert_cmpuint(completed, >=, seconds / 3); g_assert_cmpuint(interrupted, >=, 1); g_assert_cmpuint(interrupted, <=, 2);
    guint64 rss_end = rss_bytes(); guint fd_end = fd_count();
    g_assert_cmpuint(fd_max, <=, fd_start + 12); g_assert_cmpuint(fd_end, <=, fd_start + 6);
    g_assert_cmpuint(rss_end, <=, (rss_warm ? rss_warm : rss_start) + SOAK_RSS_ALLOWANCE);
    json_builder_end_array(metrics); JsonNode *metrics_node = json_builder_get_root(metrics); g_object_unref(metrics);
    JsonBuilder *b = json_builder_new(); json_builder_begin_object(b); member_bool(b, "passed", TRUE); member_bool(b, "realtime", TRUE);
    member_int(b, "requested_seconds", seconds); member_int(b, "elapsed_us", (guint64)elapsed); member_int(b, "main_consumed_frames", consumed);
    member_int(b, "transitions", transitions); member_int(b, "append_calls", append_calls); member_int(b, "overlap_observations", overlap_observations);
    member_int(b, "receipt_count", receipt_count); member_int(b, "completed_receipts", completed); member_int(b, "interrupted_receipts", interrupted); member_int(b, "receipt_frames", receipt_frames);
    member_int(b, "record_frames", rec.frames_processed); member_int(b, "record_bytes", rec.bytes_written); member_bool(b, "record_finalized", rec.recording_finalized);
    member_int(b, "stream_frames", net.frames_processed); member_int(b, "stream_bytes", net.bytes_written); member_int(b, "stream_http_status", (guint64)net.http_status); member_bool(b, "stream_response_confirmed", net.response_confirmed);
    member_int(b, "record_overrun_frames", rec.overrun_frames); member_int(b, "stream_overrun_frames", net.overrun_frames);
    member_int(b, "rss_start_bytes", rss_start); member_int(b, "rss_warm_bytes", rss_warm); member_int(b, "rss_max_bytes", rss_max); member_int(b, "rss_end_bytes", rss_end);
    member_int(b, "fd_start", fd_start); member_int(b, "fd_max", fd_max); member_int(b, "fd_end", fd_end); member_bool(b, "microphone_opened", FALSE);
    member_bool(b, "engine_underrun_counter_available", FALSE);
    json_builder_set_member_name(b, "metrics"); json_builder_add_value(b, metrics_node); json_builder_end_object(b);
    JsonNode *root = json_builder_get_root(b); JsonGenerator *generator = json_generator_new(); json_generator_set_root(generator, root); json_generator_set_pretty(generator, TRUE);
    gchar *report = g_build_filename(argv[1], "engine-proof.json", NULL); g_assert_true(json_generator_to_file(generator, report, NULL));
    g_print("Completed actual %.3fs, %u transitions, %u confirmed receipts, %" G_GUINT64_FORMAT " MAIN frames\n", elapsed / 1000000.0, transitions, receipt_count, consumed);
    g_free(report); json_node_unref(root); g_object_unref(generator); g_object_unref(b);
    radios_audio_engine_unref(engine); radios_audio_output_unref(record); radios_audio_output_unref(stream); g_free(record_path);
    for (guint file = 0; file < 2; ++file) { g_free(paths[file]); g_free(sha[file]); g_free(media[file]); g_free(versions[file]); }
    for (guint i = 0; i < total; ++i) g_free((gpointer)items[i].entry_id);
    return 0;
}
