/* LUFS normalization end to end, offline and without an audio device: the
 * real BS.1770 analyzer measures each file, the measurement travels in the
 * backend-shaped prepared detail through the planner into a scope runner on
 * players 7 and 8 with the live station's strip levels (player 7's saved
 * fader -52.1 dB, player 8's 0 dB), and MAIN is measured with the same
 * BS.1770 meter. OTO SES drives the strip fader to the correction while it
 * applies; the saved manual level returns for an unmeasured source. */
#include "radios/audio_runner.h"
#include "radios/media.h"
#include "radios/playout.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#ifndef RADIOS_TEST_PROJECT_ROOT
#define RADIOS_TEST_PROJECT_ROOT "."
#endif
#define RATE 48000u
#define TRACK_FRAMES (RATE * 4u)
#define BLOCK 1024u
#define TRACKS 5u
#define LOW_DECK 6u   /* player 7 */
#define LOW_FADER_DB (-52.1)
#define UNITY_DECK 7u /* player 8 */
static const gchar *station = "e0000000-0000-4000-8000-0000000000c5";

typedef struct { const gchar *name; gdouble sine_dbfs, click; gboolean measured; guint deck; } Track;
static const Track tracks[TRACKS] = {
    {"loud", -8, 0, TRUE, LOW_DECK}, {"quiet", -20, 0, TRUE, UNITY_DECK}, {"quiet-peak", -20, .5, TRUE, LOW_DECK},
    {"unmeasured", -8, 0, FALSE, UNITY_DECK}, {"unmeasured-low-fader", -8, 0, FALSE, LOW_DECK}};
typedef struct {
    gchar *directory, *database, *objects, *path[TRACKS], *ids[TRACKS][5];
    JsonNode *analysis[TRACKS];
    RadioSLoudness measured[TRACKS];
} Fixture;
typedef struct {
    RadioSLoudnessMeter *meter;
    gdouble squares, peak, gain_db, fader_db, applied_db;
    guint64 frames;
    gboolean seen, snapshot_measured, active;
} Heard;

static void append16(GByteArray *bytes, guint16 value)
{ guint8 data[] = {(guint8)value, (guint8)(value >> 8)}; g_byte_array_append(bytes, data, 2); }
static void append32(GByteArray *bytes, guint32 value)
{ guint8 data[] = {(guint8)value, (guint8)(value >> 8), (guint8)(value >> 16), (guint8)(value >> 24)}; g_byte_array_append(bytes, data, 4); }
/* Stereo 1 kHz sine: its BS.1770 loudness equals its peak level (EBU Tech
 * 3341), so -8/-20 dBFS files are -8/-20 LUFS. One optional click raises only
 * the sample peak, which caps the allowed boost. */
static void wav(const gchar *path, gdouble sine_dbfs, gdouble click)
{
    GByteArray *bytes = g_byte_array_new(); gdouble amplitude = pow(10, sine_dbfs / 20);
    g_byte_array_append(bytes, (guint8 *)"RIFF", 4); append32(bytes, TRACK_FRAMES * 4 + 36);
    g_byte_array_append(bytes, (guint8 *)"WAVEfmt ", 8); append32(bytes, 16); append16(bytes, 1); append16(bytes, 2);
    append32(bytes, RATE); append32(bytes, RATE * 4); append16(bytes, 4); append16(bytes, 16);
    g_byte_array_append(bytes, (guint8 *)"data", 4); append32(bytes, TRACK_FRAMES * 4);
    for (guint i = 0; i < TRACK_FRAMES; ++i) {
        gdouble value = i == TRACK_FRAMES / 2 && click > 0 ? click : amplitude * sin(2 * G_PI * 1000.0 * i / RATE);
        gint16 sample = (gint16)lround(value * 32767);
        append16(bytes, (guint16)sample); append16(bytes, (guint16)sample);
    }
    g_assert_true(g_file_set_contents(path, (gchar *)bytes->data, bytes->len, NULL)); g_byte_array_unref(bytes);
}
static void setup(Fixture *f, gconstpointer unused)
{
    (void)unused; gchar *root = g_canonicalize_filename(RADIOS_TEST_PROJECT_ROOT, NULL), *build = g_build_filename(root, "build", NULL);
    g_assert_cmpint(g_mkdir_with_parents(build, 0700), ==, 0);
    f->directory = g_build_filename(build, "loudness-engine-XXXXXX", NULL); g_assert_nonnull(g_mkdtemp(f->directory));
    f->database = g_build_filename(f->directory, "RadioS.sdb", NULL); f->objects = g_strconcat(f->database, ".objects", NULL);
    g_assert_cmpint(g_mkdir(f->objects, 0700), ==, 0);
    for (guint i = 0; i < TRACKS; ++i) {
        gchar *name = g_strdup_printf("%s.wav", tracks[i].name); f->path[i] = g_build_filename(f->objects, name, NULL); g_free(name);
        wav(f->path[i], tracks[i].sine_dbfs, tracks[i].click);
        GError *error = NULL; RadioSMediaAnalysis *analysis = radios_media_analyze(f->path[i], NULL, NULL, &error);
        g_assert_no_error(error); g_assert_nonnull(analysis);
        f->analysis[i] = radios_media_analysis_to_json(analysis); radios_media_analysis_free(analysis);
        JsonObject *object = json_node_get_object(f->analysis[i]);
        json_object_remove_member(object, "waveform"); /* as desktop-playout/prepare sends it */
        if (!tracks[i].measured) json_object_remove_member(object, "loudness"); /* never measured */
        g_assert_cmpint(radios_loudness_read(object, &f->measured[i]), ==, tracks[i].measured);
        for (guint id = 0; id < 5; ++id) f->ids[i][id] = g_uuid_string_random();
    }
    g_free(root); g_free(build);
}
static void teardown(Fixture *f, gconstpointer unused)
{
    (void)unused;
    for (guint i = 0; i < TRACKS; ++i) {
        g_assert_cmpint(g_unlink(f->path[i]), ==, 0); g_free(f->path[i]); json_node_free(f->analysis[i]);
        for (guint id = 0; id < 5; ++id) g_free(f->ids[i][id]);
    }
    g_assert_cmpint(g_rmdir(f->objects), ==, 0); g_assert_cmpint(g_rmdir(f->directory), ==, 0);
    g_free(f->objects); g_free(f->database); g_free(f->directory);
}
/* One desktop-playout/prepare track row: the media detail carries the stored
 * analysis exactly where the planner reads `analysis.loudness` and `peak`. */
static JsonObject *row(Fixture *f, guint i)
{
    enum { MEDIA, VERSION, FILE_ID, ITEM, ENTRY };
    JsonObject *row = json_object_new(), *detail = json_object_new(), *media = json_object_new(), *version = json_object_new(), *metadata = json_object_new();
    JsonObject *analysis = json_node_get_object(f->analysis[i]);
    gint64 duration = json_object_get_int_member(analysis, "duration_ms");
    json_object_set_string_member(row, "entry_id", f->ids[i][ENTRY]); json_object_set_int_member(row, "ordinal", i);
    json_object_set_boolean_member(row, "eligible", TRUE);
    json_object_set_string_member(media, "_id", f->ids[i][MEDIA]); json_object_set_string_member(media, "station_id", station);
    json_object_set_string_member(media, "version_id", f->ids[i][VERSION]); json_object_set_string_member(media, "media_file_id", f->ids[i][FILE_ID]);
    json_object_set_string_member(media, "item_id", f->ids[i][ITEM]); json_object_set_string_member(media, "title", tracks[i].name);
    json_object_set_string_member(media, "kind", "music");
    json_object_set_string_member(version, "_id", f->ids[i][VERSION]); json_object_set_string_member(version, "item_id", f->ids[i][ITEM]);
    json_object_set_string_member(version, "media_file_id", f->ids[i][FILE_ID]);
    json_object_set_string_member(version, "sha256", json_object_get_string_member(analysis, "sha256"));
    json_object_set_string_member(version, "managed_path", f->path[i]); json_object_set_int_member(version, "duration_ms", duration);
    json_object_set_boolean_member(version, "verified", TRUE); json_object_set_boolean_member(version, "object_available", TRUE);
    json_object_set_string_member(metadata, "title", tracks[i].name); json_object_set_string_member(metadata, "artist", "C5");
    json_object_set_string_member(metadata, "album", ""); json_object_set_string_member(metadata, "kind", "music");
    json_object_set_int_member(metadata, "duration_ms", duration);
    json_object_set_object_member(detail, "media", media); json_object_set_object_member(detail, "version", version);
    json_object_set_object_member(detail, "analysis", json_object_ref(analysis));
    json_object_set_object_member(row, "detail", detail); json_object_set_object_member(row, "metadata", metadata);
    return row;
}
static RadioSPlayoutPlan *plan_build(Fixture *f, const guint *order, guint count)
{
    JsonObject *prepared = json_object_new(); JsonArray *rows = json_array_new();
    json_object_set_int_member(prepared, "contract_version", 1); json_object_set_string_member(prepared, "station_id", station);
    json_object_set_string_member(prepared, "kind", "playlist"); json_object_set_string_member(prepared, "timezone", "UTC");
    for (guint i = 0; i < count; ++i) json_array_add_object_element(rows, row(f, order[i]));
    json_object_set_array_member(prepared, "tracks", rows); json_object_set_array_member(prepared, "sources", json_array_new());
    RadioSPlayoutRequest request = {.station_id = station, .database_file = f->database, .mode = RADIOS_AUDIO_RUN_EOF,
        .now_utc_us = g_get_real_time(), .live = TRUE};
    GError *error = NULL; RadioSPlayoutPlan *plan = radios_playout_plan_build(prepared, &request, &error);
    g_assert_no_error(error); g_assert_nonnull(plan); g_assert_cmpuint(plan->entries->len, ==, count);
    json_object_unref(prepared); return plan;
}
static RadioSAudioEngine *engine_new(gboolean normalize, gdouble low_fader_db)
{
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); GError *error = NULL;
    RadioSAudioPreferences prefs; radios_audio_preferences_default(&prefs);
    g_assert_true(prefs.loudness_normalize); g_assert_cmpfloat(prefs.loudness_target_lufs, ==, -14);
    prefs.loudness_normalize = normalize; g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, &error)); g_assert_no_error(error);
    RadioSAudioMixerConfig mixer; radios_audio_engine_get_mixer(engine, &mixer);
    for (guint c = 0; c < RADIOS_AUDIO_MIXER_CHANNELS; ++c) { mixer.channels[c].source_deck = -1; mixer.channels[c].gain_db = 0; }
    mixer.channels[LOW_DECK].source_deck = LOW_DECK; mixer.channels[LOW_DECK].gain_db = low_fader_db;
    mixer.channels[UNITY_DECK].source_deck = UNITY_DECK; mixer.channels[UNITY_DECK].gain_db = 0;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, &error)); g_assert_no_error(error);
    return engine;
}
/* Plays the plan on a players 7+8 scope and attributes every MAIN frame to
 * the single item audible in it; the decks' and strips' state is sampled. */
static void play(RadioSAudioEngine *engine, RadioSPlayoutPlan *plan, const guint *order, Heard *heard)
{
    guint count = plan->entries->len; RadioSAudioRunItem *items = radios_playout_plan_items(plan);
    for (guint i = 0; i < count; ++i) { items[i].preferred_deck = (gint)tracks[order[i]].deck; heard[i] = (Heard){.meter = radios_loudness_meter_new(RATE, 2)}; }
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    options.pool_count = 2; options.deck_pool[0] = LOW_DECK; options.deck_pool[1] = UNITY_DECK;
    options.loudness_context = RADIOS_AUDIO_LOUDNESS_AUTODJ | RADIOS_AUDIO_LOUDNESS_AUTOMATION;
    GError *error = NULL;
    g_assert_true(radios_audio_engine_scope_runner_configure(engine, LOW_DECK, items, count, &options, &error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_scope_runner_start_replacing(engine, LOW_DECK, -1, 0, &error)); g_assert_no_error(error);
    static gfloat output[3][BLOCK * 2]; gint64 deadline = g_get_monotonic_time() + 90 * G_USEC_PER_SEC;
    for (;;) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000); /* decoder workers refill between offline renders */
        g_assert_true(radios_audio_engine_render(engine, BLOCK, output[0], output[1], output[2], &error)); g_assert_no_error(error);
        RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
        RadioSAudioRunSnapshot run; radios_audio_engine_player_runner_snapshot(engine, LOW_DECK, &run);
        g_assert_cmpint(run.state, !=, RADIOS_AUDIO_RUN_ERROR);
        gint audible = -1; guint playing = 0;
        for (guint deck = LOW_DECK; deck <= UNITY_DECK; ++deck) {
            if (snapshot.decks[deck].state != RADIOS_AUDIO_PLAYING) continue;
            ++playing;
            for (guint i = 0; i < count; ++i) if (g_str_equal(snapshot.decks[deck].media_id, items[i].media.media_id)) audible = (gint)i;
        }
        if (playing == 1 && audible >= 0) {
            Heard *h = &heard[audible]; guint deck = tracks[order[audible]].deck;
            if (!h->seen) {
                h->seen = TRUE; h->snapshot_measured = snapshot.decks[deck].loudness_measured; h->gain_db = snapshot.decks[deck].loudness_gain_db;
                h->fader_db = snapshot.mixer.channels[deck].gain_db; h->active = snapshot.dsp_meters.channel_loudness_active[deck];
            }
            h->applied_db = snapshot.dsp_meters.channel_loudness_gain_db[deck]; /* settled value at the item's end */
            for (guint frame = 0; frame < BLOCK; ++frame) {
                radios_loudness_meter_push_frame(h->meter, &output[0][frame * 2]);
                for (guint c = 0; c < 2; ++c) { gdouble v = output[0][frame * 2 + c]; h->squares += v * v; h->peak = MAX(h->peak, fabs(v)); }
                ++h->frames;
            }
        }
        if (run.state == RADIOS_AUDIO_RUN_FINISHED || run.state == RADIOS_AUDIO_RUN_IDLE) break;
    }
    g_free(items);
}
static gdouble heard_lufs(Heard *h)
{
    gdouble lufs = -999; gboolean measured = radios_loudness_meter_finish(h->meter, &lufs);
    radios_loudness_meter_free(h->meter); h->meter = NULL; g_assert_true(measured); return lufs;
}
static gdouble heard_rms_db(const Heard *h) { return 10 * log10(h->squares / (2.0 * h->frames)); }
static gdouble manual_of(guint i) { return tracks[i].deck == LOW_DECK ? LOW_FADER_DB : 0; }

static void normalized_equal_level_peak_cap_unmeasured(Fixture *f, gconstpointer unused)
{
    (void)unused; const guint order[TRACKS] = {0, 1, 2, 3, 4};
    RadioSPlayoutPlan *plan = plan_build(f, order, TRACKS);
    /* The load path: the stored measurement reaches every run item unchanged. */
    for (guint i = 0; i < TRACKS; ++i) {
        const RadioSLoudness *item = &((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, i))->run.loudness;
        g_assert_cmpint(item->measured, ==, tracks[i].measured);
        if (!tracks[i].measured) continue;
        g_assert_cmpfloat(item->integrated_lufs, ==, f->measured[i].integrated_lufs); g_assert_cmpfloat(item->peak, ==, f->measured[i].peak);
        g_assert_cmpfloat(fabs(item->integrated_lufs - tracks[i].sine_dbfs), <, .1);
    }
    gdouble expected[TRACKS];
    for (guint i = 0; i < TRACKS; ++i) expected[i] = radios_loudness_gain_db(&f->measured[i], -14);
    g_assert_cmpfloat(fabs(expected[0] + 6), <, .1); g_assert_cmpfloat(fabs(expected[1] - 6), <, .1);
    /* The click allows only the boost that keeps the sample peak at -1 dBFS. */
    g_assert_cmpfloat(fabs(expected[2] - (-1 - 20 * log10(f->measured[2].peak))), <, 1e-9); g_assert_cmpfloat(expected[2], <, 5.5);
    RadioSAudioEngine *engine = engine_new(TRUE, LOW_FADER_DB); Heard heard[TRACKS];
    play(engine, plan, order, heard);
    gdouble lufs[TRACKS];
    for (guint i = 0; i < TRACKS; ++i) {
        g_assert_true(heard[i].seen); g_assert_cmpuint(heard[i].frames, >, RATE * 3);
        lufs[i] = heard_lufs(&heard[i]);
        g_test_message("%s: file %.2f LUFS, MAIN %.2f LUFS, RMS %.2f dBFS, peak %.4f, correction %.2f dB, strip applies %.2f dB, fader %.2f dB",
            tracks[i].name, tracks[i].measured ? f->measured[i].integrated_lufs : NAN, lufs[i], heard_rms_db(&heard[i]),
            heard[i].peak, heard[i].gain_db, heard[i].applied_db, heard[i].fader_db);
        g_assert_cmpint(heard[i].snapshot_measured, ==, tracks[i].measured);
        g_assert_cmpint(heard[i].active, ==, tracks[i].measured);
        g_assert_cmpfloat(fabs(heard[i].gain_db - (tracks[i].measured ? expected[i] : 0)), <, 1e-6);
        /* OTO SES: the visible fader carries the correction while it applies;
         * otherwise the strip's saved manual level is restored. */
        g_assert_cmpfloat(fabs(heard[i].fader_db - (tracks[i].measured ? expected[i] : manual_of(i))), <, 1e-6);
        if (tracks[i].measured) g_assert_cmpfloat(fabs(heard[i].applied_db - expected[i]), <, .01);
    }
    /* -8 and -20 LUFS files both air at the -14 target with equal RMS, the
     * loud one through player 7's strip although its saved fader is -52.1 dB. */
    g_assert_cmpfloat(fabs(lufs[0] + 14), <, .25); g_assert_cmpfloat(fabs(lufs[1] + 14), <, .25);
    g_assert_cmpfloat(fabs(heard_rms_db(&heard[0]) - heard_rms_db(&heard[1])), <, .25);
    /* Peak cap: boosted as far as a -1 dBFS sample peak, never further. */
    g_assert_cmpfloat(fabs(lufs[2] - (f->measured[2].integrated_lufs + expected[2])), <, .25);
    g_assert_cmpfloat(heard[2].peak, <=, pow(10, -1.0 / 20) * 1.002); g_assert_cmpfloat(heard[2].peak, >, pow(10, -1.0 / 20) * .98);
    /* Unmeasured: no correction; the file plays at its own level through the
     * strip's saved fader, so player 7's -52.1 dB strip is almost silent. */
    g_assert_cmpfloat(fabs(lufs[3] - tracks[3].sine_dbfs), <, .25);
    g_assert_cmpfloat(fabs(lufs[4] - (tracks[4].sine_dbfs + LOW_FADER_DB)), <, .5);
    radios_audio_engine_unref(engine); radios_playout_plan_free(plan);
}
/* Control: with the switch off the same files air at their own levels, so
 * the equal levels above can only come from the normalization. */
static void disabled_plays_own_levels(Fixture *f, gconstpointer unused)
{
    (void)unused; const guint order[2] = {0, 1};
    RadioSPlayoutPlan *plan = plan_build(f, order, 2);
    RadioSAudioEngine *engine = engine_new(FALSE, 0); Heard heard[2];
    play(engine, plan, order, heard);
    for (guint i = 0; i < 2; ++i) {
        g_assert_true(heard[i].seen); g_assert_true(heard[i].snapshot_measured); g_assert_false(heard[i].active);
        g_assert_cmpfloat(heard[i].gain_db, ==, 0); g_assert_cmpfloat(heard[i].fader_db, ==, 0);
        g_assert_cmpfloat(fabs(heard_lufs(&heard[i]) - tracks[order[i]].sine_dbfs), <, .25);
    }
    radios_audio_engine_unref(engine); radios_playout_plan_free(plan);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/loudness-engine/prepared-detail-planner-scope-equal-level-peak-cap-unmeasured", Fixture, NULL,
        setup, normalized_equal_level_peak_cap_unmeasured, teardown);
    g_test_add("/loudness-engine/disabled-plays-own-levels", Fixture, NULL, setup, disabled_plays_own_levels, teardown);
    return g_test_run();
}
