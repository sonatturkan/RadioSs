#include "radios/header_navigation.h"
#include "radios/playout.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#ifndef RADIOS_TEST_PROJECT_ROOT
#define RADIOS_TEST_PROJECT_ROOT "."
#endif

typedef struct {
    gchar *directory, *database, *objects;
    gchar *path[3], *sha[3], *media[3], *version[3], *file[3], *item[3], *entry[3], *block[3];
} Fixture;
static const gchar *station = "50000000-0000-4000-8000-000000000001";
static void append16(GByteArray *bytes, guint16 value)
{ guint8 data[] = {(guint8)value, (guint8)(value >> 8)}; g_byte_array_append(bytes, data, 2); }
static void append32(GByteArray *bytes, guint32 value)
{ guint8 data[] = {(guint8)value, (guint8)(value >> 8), (guint8)(value >> 16), (guint8)(value >> 24)}; g_byte_array_append(bytes, data, 4); }
static void setup(Fixture *f, gconstpointer unused)
{
    (void)unused;
    gchar *root = g_canonicalize_filename(RADIOS_TEST_PROJECT_ROOT, NULL);
    gchar *build = g_build_filename(root, "build-prompt5", NULL);
    g_assert_cmpint(g_mkdir_with_parents(build, 0700), ==, 0);
    f->directory = g_build_filename(build, "navigation-backend-XXXXXX", NULL);
    g_assert_nonnull(g_mkdtemp(f->directory));
    f->database = g_build_filename(f->directory, "fixture.sdb", NULL);
    f->objects = g_strconcat(f->database, ".objects", NULL);
    g_assert_cmpint(g_mkdir(f->objects, 0700), ==, 0);
    const guint16 amplitudes[] = {8192, 4096, 12288};
    for (guint i = 0; i < 3; ++i) {
        f->media[i] = g_uuid_string_random(); f->version[i] = g_uuid_string_random();
        f->file[i] = g_uuid_string_random(); f->item[i] = g_uuid_string_random();
        f->entry[i] = g_uuid_string_random(); f->block[i] = g_uuid_string_random();
        gchar *name = g_strdup_printf("%s.wav", f->media[i]);
        f->path[i] = g_build_filename(f->objects, name, NULL); g_free(name);
        GByteArray *bytes = g_byte_array_new();
        g_byte_array_append(bytes, (const guint8 *)"RIFF", 4); append32(bytes, 96000 * 4 + 36);
        g_byte_array_append(bytes, (const guint8 *)"WAVEfmt ", 8); append32(bytes, 16);
        append16(bytes, 1); append16(bytes, 2); append32(bytes, 48000); append32(bytes, 192000);
        append16(bytes, 4); append16(bytes, 16);
        g_byte_array_append(bytes, (const guint8 *)"data", 4); append32(bytes, 96000 * 4);
        for (guint sample = 0; sample < 96000; ++sample) {
            append16(bytes, amplitudes[i]); append16(bytes, (guint16)-amplitudes[i]);
        }
        g_assert_true(g_file_set_contents(f->path[i], (const gchar *)bytes->data, bytes->len, NULL));
        f->sha[i] = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes->data, bytes->len);
        g_byte_array_unref(bytes);
    }
    g_free(build); g_free(root);
}
static void teardown(Fixture *f, gconstpointer unused)
{
    (void)unused;
    for (guint i = 0; i < 3; ++i) {
        g_assert_cmpint(g_unlink(f->path[i]), ==, 0);
        g_free(f->path[i]); g_free(f->sha[i]); g_free(f->media[i]); g_free(f->version[i]);
        g_free(f->file[i]); g_free(f->item[i]); g_free(f->entry[i]); g_free(f->block[i]);
    }
    g_assert_cmpint(g_rmdir(f->objects), ==, 0); g_assert_cmpint(g_rmdir(f->directory), ==, 0);
    g_free(f->objects); g_free(f->database); g_free(f->directory);
}
static RadioSAudioRunItem input(Fixture *f, guint i)
{
    RadioSAudioRunItem item = {0};
    item.entry_id = f->entry[i]; item.media = (RadioSAudioLoadSpec){f->path[i], f->sha[i], f->media[i], f->version[i], 0, 60000};
    item.duration_frames = 96000; item.auto_next_frame = item.fade_start_frame = item.fade_end_frame = 60000;
    item.preferred_deck = -1; return item;
}
static void render(RadioSAudioEngine *engine, guint frames, gfloat output[3][8192])
{
    GError *error = NULL;
    g_assert_true(radios_audio_engine_render(engine, frames, output[0], output[1], output[2], &error));
    g_assert_no_error(error);
}
static void ready(RadioSAudioEngine *engine, guint deck, const gchar *media)
{
    gint64 until = g_get_monotonic_time() + 10000000;
    do {
        RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
        if (g_str_equal(snapshot.decks[deck].media_id, media) &&
            snapshot.decks[deck].state != RADIOS_AUDIO_LOADING && snapshot.decks[deck].buffered_frames >= 4800) return;
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
    g_error("Navigation fixture deck %u did not load %s", deck, media);
}
static void start(RadioSAudioEngine *engine, RadioSAudioRunItem *items, RadioSAudioRunOptions *options)
{
    GError *error = NULL;
    g_assert_true(radios_audio_engine_runner_configure(engine, items, 3, options, &error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_runner_start(engine, &error)); g_assert_no_error(error);
}
static void rejected_jump(RadioSAudioEngine *engine, guint64 epoch, guint index)
{
    GError *error = NULL;
    g_assert_false(radios_audio_engine_runner_jump_to(engine, epoch, index, &error));
    g_assert_nonnull(error); g_clear_error(&error);
}
static void test_pcm_jump_and_fences(Fixture *f, gconstpointer unused)
{
    (void)unused;
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem items[] = {input(f, 0), input(f, 1), input(f, 2)};
    start(engine, items, &options); ready(engine, 0, f->media[0]); ready(engine, 1, f->media[1]);
    gfloat output[3][8192]; render(engine, 100, output);
    g_assert_cmpfloat(fabs(output[0][0] - .25), <, .00001);
    RadioSAudioRunSnapshot before; radios_audio_engine_runner_snapshot(engine, &before);
    rejected_jump(engine, 0, 2); rejected_jump(engine, before.epoch + 1, 2); rejected_jump(engine, before.epoch, 3);
    RadioSAudioRunSnapshot unchanged; radios_audio_engine_runner_snapshot(engine, &unchanged);
    g_assert_cmpuint(unchanged.index, ==, 0); g_assert_cmpuint(unchanged.actual_current_frame, ==, 100);
    g_assert_true(radios_audio_engine_runner_pause(engine, NULL)); rejected_jump(engine, before.epoch, 2);
    g_assert_true(radios_audio_engine_runner_resume(engine, NULL));
    g_assert_true(radios_audio_engine_runner_jump_to(engine, before.epoch, 2, NULL));
    rejected_jump(engine, before.epoch, 1); /* A second intent cannot overwrite the pending cut. */
    render(engine, 241, output); ready(engine, 0, f->media[2]); render(engine, 1, output);
    RadioSAudioRunSnapshot after; radios_audio_engine_runner_snapshot(engine, &after);
    g_assert_cmpuint(after.epoch, ==, before.epoch); g_assert_cmpuint(after.index, ==, 2);
    g_assert_cmpstr(after.entry_id, ==, f->entry[2]); g_assert_cmpfloat(fabs(output[0][0] - .375), <, .00001);
    RadioSAudioRunReceipt receipts[8]; guint count = radios_audio_engine_runner_receipts(engine, receipts, 8);
    gboolean interrupted = FALSE, target = FALSE;
    for (guint i = 0; i < count; ++i) {
        g_assert_cmpuint(receipts[i].index, !=, 1); /* The skipped song never sounded. */
        interrupted |= receipts[i].index == 0 && receipts[i].terminal == RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED;
        target |= receipts[i].index == 2 && receipts[i].position_frames > 0;
    }
    g_assert_true(interrupted); g_assert_true(target);
    g_assert_true(radios_audio_engine_runner_stop(engine, NULL));
    start(engine, items, &options); ready(engine, 0, f->media[0]); render(engine, 1, output);
    rejected_jump(engine, before.epoch, 2); /* A replacement queue rejects an old epoch. */
    radios_audio_engine_unref(engine);
}
static void test_scheduled_rejection(Fixture *f, gconstpointer unused)
{
    (void)unused;
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options); options.start_utc_us = g_get_real_time() + 60000000;
    RadioSAudioRunItem items[] = {input(f, 0), input(f, 1), input(f, 2)};
    start(engine, items, &options); ready(engine, 0, f->media[0]);
    gfloat output[3][8192]; render(engine, 1, output);
    RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_SCHEDULED); rejected_jump(engine, run.epoch, 2);
    g_assert_cmpfloat(output[0][0], ==, 0); g_assert_cmpuint(run.actual_current_frame, ==, 0);
    radios_audio_engine_unref(engine);
}
static void test_manual_wait_jump(Fixture *f, gconstpointer unused)
{
    (void)unused;
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem items[] = {input(f, 0), input(f, 1), input(f, 2)};
    items[0].mode = RADIOS_AUDIO_RUN_MANUAL; items[0].media.cue_out_frame = 100;
    items[0].auto_next_frame = items[0].fade_start_frame = items[0].fade_end_frame = 100;
    start(engine, items, &options);
    /* The short first source legitimately buffers fewer than ready() needs. */
    gint64 until = g_get_monotonic_time() + 10000000;
    RadioSAudioSnapshot loaded;
    do { radios_audio_engine_snapshot(engine, &loaded); if (loaded.decks[0].buffered_frames >= 100) break; g_usleep(1000); } while (g_get_monotonic_time() < until);
    g_assert_cmpuint(loaded.decks[0].buffered_frames, >=, 100);
    gfloat output[3][8192]; render(engine, 101, output);
    RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_MANUAL_WAIT);
    g_assert_true(radios_audio_engine_runner_jump_to(engine, run.epoch, 2, NULL));
    render(engine, 241, output); ready(engine, 0, f->media[2]); render(engine, 1, output);
    radios_audio_engine_runner_snapshot(engine, &run); g_assert_cmpuint(run.index, ==, 2);
    g_assert_cmpfloat(fabs(output[0][0] - .375), <, .00001); radios_audio_engine_unref(engine);
}
static void test_player_owner_isolation(Fixture *f, gconstpointer unused)
{
    (void)unused;
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem items[] = {input(f, 0), input(f, 1), input(f, 2)}, other = input(f, 1);
    g_assert_true(radios_audio_engine_player_runner_configure(engine, 0, items, 3, &options, NULL));
    g_assert_true(radios_audio_engine_player_runner_configure(engine, 1, &other, 1, &options, NULL));
    g_assert_true(radios_audio_engine_player_runner_action(engine, 0, RADIOS_AUDIO_PLAYER_RUN_START, NULL));
    g_assert_true(radios_audio_engine_player_runner_action(engine, 1, RADIOS_AUDIO_PLAYER_RUN_START, NULL));
    ready(engine, 0, f->media[0]); ready(engine, 1, f->media[1]);
    gfloat output[3][8192]; render(engine, 100, output);
    RadioSAudioRunSnapshot mine, before, after; radios_audio_engine_player_runner_snapshot(engine, 0, &mine);
    radios_audio_engine_player_runner_snapshot(engine, 1, &before);
    g_assert_true(radios_audio_engine_player_runner_jump_to(engine, 0, mine.epoch, 2, NULL));
    render(engine, 241, output); ready(engine, 0, f->media[2]); render(engine, 1, output);
    radios_audio_engine_player_runner_snapshot(engine, 0, &mine); g_assert_cmpuint(mine.index, ==, 2);
    radios_audio_engine_player_runner_snapshot(engine, 1, &after);
    g_assert_cmpuint(after.epoch, ==, before.epoch); g_assert_cmpuint(after.index, ==, before.index);
    g_assert_cmpuint(after.actual_current_frame, >, before.actual_current_frame);
    g_assert_cmpint(after.state, ==, RADIOS_AUDIO_RUN_PLAYING);
    g_assert_cmpfloat(fabs(output[0][0] - .5), <, .00001);
    RadioSAudioRunSnapshot global; radios_audio_engine_runner_snapshot(engine, &global);
    g_assert_cmpint(global.state, ==, RADIOS_AUDIO_RUN_IDLE); radios_audio_engine_unref(engine);
}
static void test_realtime_audible_rendered_eof_jump(Fixture *f, gconstpointer unused)
{
    (void)unused;
    const gchar *main_sink = g_getenv("RADIOS_TEST_MAIN_SINK");
    const gchar *cue_sink = g_getenv("RADIOS_TEST_CUE_SINK");
    const gchar *monitor_sink = g_getenv("RADIOS_TEST_MONITOR_SINK");
    if (!main_sink || !cue_sink || !monitor_sink) {
        g_test_skip("Run through run_audio_ui.py --engine-only for owned Pulse outputs"); return;
    }
    RadioSAudioEngine *engine = radios_audio_engine_new(TRUE, NULL);
    RadioSAudioPreferences preferences; radios_audio_preferences_default(&preferences);
    g_strlcpy(preferences.main_sink, main_sink, sizeof preferences.main_sink);
    g_strlcpy(preferences.cue_sink, cue_sink, sizeof preferences.cue_sink);
    g_strlcpy(preferences.monitor_sink, monitor_sink, sizeof preferences.monitor_sink);
    GError *error = NULL;
    g_assert_true(radios_audio_engine_apply_preferences(engine, &preferences, &error)); g_assert_no_error(error);
    gint64 until = g_get_monotonic_time() + 10000000;
    RadioSAudioSnapshot audio;
    do {
        radios_audio_engine_snapshot(engine, &audio);
        if (audio.buses[0].ready && audio.buses[1].ready) break;
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
    g_assert_true(audio.buses[0].ready); g_assert_true(audio.buses[1].ready);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem items[] = {input(f, 0), input(f, 1), input(f, 2)};
    for (guint i = 0; i < 3; ++i) {
        items[i].media.cue_out_frame = items[i].auto_next_frame =
            items[i].fade_start_frame = items[i].fade_end_frame = 4800;
    }
    start(engine, items, &options);
    gboolean rendered_eof_while_playing = FALSE;
    RadioSAudioRunSnapshot run = {0}; until = g_get_monotonic_time() + 10000000;
    do {
        radios_audio_engine_snapshot(engine, &audio); radios_audio_engine_runner_snapshot(engine, &run);
        if (run.state == RADIOS_AUDIO_RUN_ERROR) g_error("Realtime navigation source: %s", run.error);
        for (guint deck = 0; deck < 2; ++deck) {
            const RadioSAudioDeckSnapshot *source = &audio.decks[deck];
            if (run.state == RADIOS_AUDIO_RUN_PLAYING && run.index == 2 &&
                g_str_equal(source->media_id, f->media[2]) && source->state == RADIOS_AUDIO_PLAYING &&
                source->buffered_frames == 0 && source->cursor_frames > 0 && source->cursor_frames < 4800) {
                rendered_eof_while_playing = TRUE; break;
            }
        }
        if (rendered_eof_while_playing) break;
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
    g_assert_true(rendered_eof_while_playing);
    /* Let the next output callback record final renderer EOF while the queued
     * source tail still sounds; physical consumption must remain PLAYING. */
    g_usleep(20000); radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_PLAYING);
    g_assert_cmpuint(run.index, ==, 2); g_assert_cmpuint(run.actual_current_frame, <, 4800);
    g_test_message("Actual audible index%u, frame%" G_GUINT64_FORMAT "; target PCM rendered to EOF", run.index, run.actual_current_frame);
    /* The renderer has reached final EOF, but actual output still owns a
     * PLAYING queue. A bounded jump to its first entry remains valid. */
    g_assert_true(radios_audio_engine_runner_jump_to(engine, run.epoch, 0, &error)); g_assert_no_error(error);
    gboolean target_consumed = FALSE; until = g_get_monotonic_time() + 10000000;
    do {
        RadioSAudioRunSnapshot after; radios_audio_engine_runner_snapshot(engine, &after);
        target_consumed = after.epoch == run.epoch && after.index == 0 && after.actual_current_frame > 0;
        if (target_consumed) break;
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
    g_assert_true(target_consumed); radios_audio_engine_unref(engine);
}
static JsonObject *prepared_row(Fixture *f, guint i)
{
    JsonObject *row = json_object_new(), *detail = json_object_new(), *media = json_object_new();
    JsonObject *version = json_object_new(), *analysis = json_object_new(), *metadata = json_object_new();
    json_object_set_string_member(row, "entry_id", f->entry[i]); json_object_set_boolean_member(row, "eligible", TRUE);
    json_object_set_string_member(media, "_id", f->media[i]); json_object_set_string_member(media, "station_id", station);
    json_object_set_string_member(media, "version_id", f->version[i]); json_object_set_string_member(media, "media_file_id", f->file[i]);
    json_object_set_string_member(media, "item_id", f->item[i]);
    json_object_set_string_member(version, "_id", f->version[i]); json_object_set_string_member(version, "item_id", f->item[i]);
    json_object_set_string_member(version, "media_file_id", f->file[i]); json_object_set_string_member(version, "sha256", f->sha[i]);
    json_object_set_string_member(version, "managed_path", f->path[i]); json_object_set_int_member(version, "duration_ms", 2000);
    json_object_set_boolean_member(version, "verified", TRUE); json_object_set_boolean_member(version, "object_available", TRUE);
    json_object_set_string_member(analysis, "sha256", f->sha[i]); json_object_set_int_member(analysis, "sample_rate", 48000);
    json_object_set_int_member(analysis, "sample_frames", 96000);
    json_object_set_string_member(metadata, "title", "Fixture track"); json_object_set_string_member(metadata, "kind", "music");
    json_object_set_object_member(detail, "media", media); json_object_set_object_member(detail, "version", version);
    json_object_set_object_member(detail, "analysis", analysis); json_object_set_object_member(row, "detail", detail);
    json_object_set_object_member(row, "metadata", metadata); return row;
}
static void test_immutable_grouping_by_identity(Fixture *f, gconstpointer unused)
{
    (void)unused;
    JsonObject *prepared = json_object_new(), *config = json_object_new();
    json_object_set_int_member(prepared, "contract_version", 1); json_object_set_string_member(prepared, "station_id", station);
    json_object_set_string_member(prepared, "kind", "programme");
    JsonArray *tracks = json_array_new(), *events = json_array_new(), *blocks = json_array_new();
    for (guint i = 0; i < 3; ++i) json_array_add_object_element(tracks, prepared_row(f, i));
    /* Reordered config entries prove metadata is joined by identity, not index. */
    const guint order[] = {2, 0, 1};
    for (guint j = 0; j < 3; ++j) {
        guint i = order[j]; JsonObject *event = json_object_new();
        json_object_set_string_member(event, "id", f->entry[i]);
        json_object_set_string_member(event, "block_id", f->block[i == 2 ? 1 : 0]);
        json_object_set_string_member(event, "kind", "music"); json_array_add_object_element(events, event);
    }
    for (guint i = 0; i < 3; ++i) {
        JsonObject *block = json_object_new(); json_object_set_string_member(block, "id", f->block[i]);
        json_object_set_string_member(block, "kind", "music"); json_object_set_string_member(block, "title", i == 1 ? "Second playlist" : "First playlist");
        json_array_add_object_element(blocks, block); /* Third block is empty. */
    }
    json_object_set_array_member(config, "events", events); json_object_set_array_member(config, "blocks", blocks);
    json_object_set_array_member(prepared, "tracks", tracks); json_object_set_object_member(prepared, "config", config);
    RadioSPlayoutRequest request = {.station_id = station, .database_file = f->database, .now_utc_us = g_get_real_time(), .start_index = 1};
    GError *error = NULL; RadioSPlayoutPlan *plan = radios_playout_plan_build(prepared, &request, &error);
    g_assert_no_error(error); g_assert_nonnull(plan); g_assert_cmpuint(plan->entries->len, ==, 2);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(plan->display), "entries");
    JsonObject *first = json_array_get_object_element(rows, 0), *second = json_array_get_object_element(rows, 1);
    g_assert_cmpstr(json_object_get_string_member(first, "entry_id"), ==, f->entry[1]);
    g_assert_cmpstr(json_object_get_string_member(first, "block_id"), ==, f->block[0]);
    g_assert_cmpstr(json_object_get_string_member(second, "entry_id"), ==, f->entry[2]);
    g_assert_cmpstr(json_object_get_string_member(second, "block_id"), ==, f->block[1]);
    json_object_set_string_member(json_array_get_object_element(events, 0), "block_id", f->block[0]);
    json_object_set_string_member(json_array_get_object_element(blocks, 1), "title", "Edited draft");
    json_object_unref(prepared);
    g_assert_cmpstr(json_object_get_string_member(second, "block_id"), ==, f->block[1]);
    g_assert_cmpstr(json_object_get_string_member(second, "block_title"), ==, "Second playlist");
    radios_playout_plan_free(plan);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/prompt5/backend/PCM-jump-fences-paused-pending-consumption", Fixture, NULL, setup, test_pcm_jump_and_fences, teardown);
    g_test_add("/prompt5/backend/scheduled-rejection", Fixture, NULL, setup, test_scheduled_rejection, teardown);
    g_test_add("/prompt5/backend/manual-wait-jump", Fixture, NULL, setup, test_manual_wait_jump, teardown);
    g_test_add("/prompt5/backend/independent-player-owner-isolation", Fixture, NULL, setup, test_player_owner_isolation, teardown);
    g_test_add("/prompt5/backend/immutable-block-identity-start-offset", Fixture, NULL, setup, test_immutable_grouping_by_identity, teardown);
    g_test_add("/prompt5/backend/realtime-audible-rendered-EOF-jump", Fixture, NULL, setup, test_realtime_audible_rendered_eof_jump, teardown);
    return g_test_run();
}
