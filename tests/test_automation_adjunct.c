#include "radios/audio_runner.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#ifndef RADIOS_TEST_PROJECT_ROOT
#define RADIOS_TEST_PROJECT_ROOT "."
#endif
typedef struct { gchar *directory, *path[2], *sha[2], *media[2], *version[2], *entry[3]; } Fixture;
static void append16(GByteArray *bytes, guint16 n)
{ guint8 b[] = {(guint8)n, (guint8)(n >> 8)}; g_byte_array_append(bytes, b, 2); }
static void append32(GByteArray *bytes, guint32 n)
{ guint8 b[] = {(guint8)n, (guint8)(n >> 8), (guint8)(n >> 16), (guint8)(n >> 24)}; g_byte_array_append(bytes, b, 4); }
static void setup(Fixture *f, gconstpointer unused)
{
    (void)unused; gchar *root = g_canonicalize_filename(RADIOS_TEST_PROJECT_ROOT, NULL), *build = g_build_filename(root, "build", NULL);
    g_assert_cmpint(g_mkdir_with_parents(build, 0700), ==, 0); f->directory = g_build_filename(build, "audio-runner-XXXXXX", NULL); g_assert_nonnull(g_mkdtemp(f->directory));
    for (guint file = 0; file < 2; ++file) {
        gchar *name = g_strdup_printf("source-%u.wav", file); f->path[file] = g_build_filename(f->directory, name, NULL); g_free(name);
        GByteArray *b = g_byte_array_new(); g_byte_array_append(b, (guint8 *)"RIFF", 4); append32(b, 96000 * 4 + 36);
        g_byte_array_append(b, (guint8 *)"WAVEfmt ", 8); append32(b, 16); append16(b, 1); append16(b, 2); append32(b, 48000); append32(b, 192000); append16(b, 4); append16(b, 16);
        g_byte_array_append(b, (guint8 *)"data", 4); append32(b, 96000 * 4);
        for (guint i = 0; i < 96000; ++i) { append16(b, file ? 4096 : 8192); append16(b, file ? (guint16)-4096 : (guint16)-8192); }
        g_assert_true(g_file_set_contents(f->path[file], (gchar *)b->data, b->len, NULL)); f->sha[file] = g_compute_checksum_for_data(G_CHECKSUM_SHA256, b->data, b->len); g_byte_array_unref(b);
        f->media[file] = g_uuid_string_random(); f->version[file] = g_uuid_string_random();
    }
    for (guint i = 0; i < 3; ++i) f->entry[i] = g_uuid_string_random();
    g_free(root); g_free(build);
}
static void teardown(Fixture *f, gconstpointer unused)
{
    (void)unused; for (guint i = 0; i < 2; ++i) { g_assert_cmpint(g_unlink(f->path[i]), ==, 0); g_free(f->path[i]); g_free(f->sha[i]); g_free(f->media[i]); g_free(f->version[i]); }
    for (guint i = 0; i < 3; ++i) g_free(f->entry[i]);
    g_assert_cmpint(g_rmdir(f->directory), ==, 0); g_free(f->directory);
}
static RadioSAudioRunItem item(Fixture *f, guint file, guint entry, guint64 in, guint64 out)
{
    RadioSAudioRunItem result = {0}; result.entry_id = f->entry[entry]; result.media = (RadioSAudioLoadSpec){f->path[file], f->sha[file], f->media[file], f->version[file], in, out};
    result.duration_frames = 96000; result.auto_next_frame = result.fade_start_frame = result.fade_end_frame = out; result.preferred_deck = -1; return result;
}
static void near(gdouble actual, gdouble expected)
{ if (fabs(actual - expected) >= .00001) g_test_message("Actual PCM %.9f, expected %.9f", actual, expected); g_assert_cmpfloat(fabs(actual - expected), <, .00001); }
static void render(RadioSAudioEngine *engine, guint frames, gfloat output[3][8192])
{ GError *error = NULL; g_assert_true(radios_audio_engine_render(engine, frames, output[0], output[1], output[2], &error)); g_assert_no_error(error); }
static void ready_after(RadioSAudioEngine *engine, guint deck, const gchar *media, guint buffered, guint64 old_generation)
{
    gint64 until = g_get_monotonic_time() + 10000000; RadioSAudioSnapshot s;
    do {
        radios_audio_engine_snapshot(engine, &s); RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(engine, &r);
        if (r.state == RADIOS_AUDIO_RUN_ERROR) g_error("Runner: %s", r.error);
        if (g_str_equal(s.decks[deck].media_id, media) && s.decks[deck].generation > old_generation &&
            s.decks[deck].state != RADIOS_AUDIO_LOADING && s.decks[deck].buffered_frames >= buffered) return;
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
    g_error("Runner deck%u did not become ready (state%d, buffers%" G_GUINT64_FORMAT ")", deck, s.decks[deck].state, s.decks[deck].buffered_frames);
}
static void ready(RadioSAudioEngine *engine, guint deck, const gchar *media, guint buffered)
{ ready_after(engine, deck, media, buffered, 0); }
static void start(RadioSAudioEngine *engine, RadioSAudioRunItem *items, guint count, RadioSAudioRunOptions *options)
{
    GError *error = NULL; g_assert_true(radios_audio_engine_runner_configure(engine, items, count, options, &error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_runner_start(engine, &error)); g_assert_no_error(error);
}

static RadioSAudioRunItem adjunct_item(Fixture *f, guint64 trigger, guint64 fade, gdouble duck)
{
    RadioSAudioRunItem q=item(f,0,0,100,8100);
    q.adjunct=(RadioSAudioRunAdjunct){.enabled=TRUE,.player=2,
        .media={f->path[1],f->sha[1],f->media[1],f->version[1],200,1200},
        .entry_id=f->entry[2],.title="Doğrulanmış jingle",.artist="Anons",
        .start_output_frame=trigger,.fade_frames=fade,.gain_db=0,.duck_db=duck};
    return q;
}
static void adjunct_start(Fixture *f, RadioSAudioEngine *e, RadioSAudioRunItem *q)
{
    RadioSAudioRunOptions o; radios_audio_run_options_default(&o); start(e,q,1,&o);
    ready(e,0,f->media[0],4800); ready(e,2,f->media[1],1000);
}
static void sample_exact_overlay(Fixture *f,gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);
    RadioSAudioRunItem q=adjunct_item(f,1000,0,0); adjunct_start(f,e,&q);
    gfloat output[3][8192]; render(e,1000,output); near(output[0][0],.25); near(output[0][1998],.25);
    RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(e,&run); g_assert_cmpuint(run.index,==,0);
    render(e,1001,output); near(output[0][0],.375); near(output[0][1998],.375); near(output[0][2000],.25);
    radios_audio_engine_runner_snapshot(e,&run); g_assert_cmpuint(run.index,==,0); g_assert_cmpint(run.deck_current,==,0);
    RadioSAudioSnapshot s; radios_audio_engine_snapshot(e,&s); g_assert_cmpstr(s.decks[0].media_id,==,f->media[0]);
    near(output[1][0],0); radios_audio_engine_unref(e);
}
static void duck_fade_envelope(Fixture *f,gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);
    RadioSAudioRunItem q=adjunct_item(f,1000,100,-6); adjunct_start(f,e,&q);
    gfloat output[3][8192]; render(e,1000,output); render(e,1000,output);
    const gdouble duck=pow(10,-6.0/20);
    near(output[0][0],.25); near(output[0][100],.25*(1+(duck-1)*.5)+.125*.5);
    near(output[0][200],.25*duck+.125); near(output[0][1800],.25*duck+.125);
    near(output[0][1900],.25*(1+(duck-1)*.5)+.125*.5);
    render(e,1,output); near(output[0][0],.25); radios_audio_engine_unref(e);
}
static void pause_stop_seek_lifecycle(Fixture *f,gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);
    RadioSAudioRunItem q=adjunct_item(f,1000,0,0); adjunct_start(f,e,&q);
    gfloat output[3][8192]; render(e,1100,output); near(output[0][2198],.375);
    RadioSAudioSnapshot before,after; radios_audio_engine_snapshot(e,&before);
    g_assert_true(radios_audio_engine_runner_pause(e,NULL)); render(e,300,output); near(output[0][0],0);
    radios_audio_engine_snapshot(e,&after); g_assert_cmpuint(after.decks[0].cursor_frames,==,before.decks[0].cursor_frames);
    g_assert_cmpuint(after.decks[2].cursor_frames,==,before.decks[2].cursor_frames);
    g_assert_true(radios_audio_engine_runner_resume(e,NULL)); render(e,1,output); near(output[0][0],.375);
    g_assert_true(radios_audio_engine_seek(e,0,3000,NULL)); ready(e,0,f->media[0],4800); render(e,1,output); near(output[0][0],.25);
    RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(e,&run); g_assert_cmpuint(run.index,==,0);
    g_assert_true(radios_audio_engine_runner_stop(e,NULL)); render(e,300,output); near(output[0][0],0);
    radios_audio_engine_snapshot(e,&after); g_assert_cmpint(after.decks[2].state,==,RADIOS_AUDIO_EMPTY);
    radios_audio_engine_unref(e);
}
static void occupied_cue_private_safety(Fixture *f,gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL); RadioSAudioRunOptions o;radios_audio_run_options_default(&o);
    RadioSAudioRunItem q=adjunct_item(f,1000,0,0); g_assert_true(radios_audio_engine_runner_configure(e,&q,1,&o,NULL));
    g_assert_true(radios_audio_engine_load(e,2,&q.adjunct.media,NULL));ready(e,2,f->media[1],1000);
    RadioSAudioSnapshot before,after;radios_audio_engine_snapshot(e,&before); GError *error=NULL;
    g_assert_false(radios_audio_engine_runner_start(e,&error));g_assert_error(error,RADIOS_AUDIO_ERROR_DOMAIN,RADIOS_AUDIO_ERROR_STATE);g_clear_error(&error);
    radios_audio_engine_snapshot(e,&after);g_assert_cmpuint(after.decks[2].generation,==,before.decks[2].generation);g_assert_cmpstr(after.decks[2].media_id,==,f->media[1]);
    g_assert_cmpint(after.decks[0].state,==,RADIOS_AUDIO_EMPTY);g_assert_true(radios_audio_engine_unload(e,2,NULL));
    RadioSAudioMixerConfig mixer;radios_audio_engine_get_mixer(e,&mixer);mixer.channels[2].pfl=TRUE;g_assert_true(radios_audio_engine_apply_mixer(e,&mixer,NULL));
    g_assert_false(radios_audio_engine_runner_start(e,&error));g_assert_error(error,RADIOS_AUDIO_ERROR_DOMAIN,RADIOS_AUDIO_ERROR_STATE);g_clear_error(&error);
    mixer.channels[2].pfl=FALSE;g_assert_true(radios_audio_engine_apply_mixer(e,&mixer,NULL));
    RadioSAudioRunItem private_item=item(f,1,1,200,2200);private_item.preferred_deck=2;o.pool_count=1;o.deck_pool[0]=2;
    g_assert_true(radios_audio_engine_player_runner_configure(e,2,&private_item,1,&o,NULL));
    g_assert_true(radios_audio_engine_player_runner_action(e,2,RADIOS_AUDIO_PLAYER_RUN_START,NULL));ready(e,2,f->media[1],2000);
    g_assert_false(radios_audio_engine_runner_start(e,&error));g_assert_error(error,RADIOS_AUDIO_ERROR_DOMAIN,RADIOS_AUDIO_ERROR_STATE);g_clear_error(&error);
    RadioSAudioRunSnapshot private_run;radios_audio_engine_player_runner_snapshot(e,2,&private_run);g_assert_cmpuint(private_run.count,==,1);
    radios_audio_engine_unref(e);
}
static void manual_aux_only_and_copyright(Fixture *f,gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);RadioSAudioRunItem q=adjunct_item(f,1000,0,0);
    q.adjunct.copyright=(RadioSAudioCopyrightWindow){.restricted=TRUE,.denied=TRUE};adjunct_start(f,e,&q);
    gfloat output[3][8192];render(e,2000,output);near(output[0][2200],.25);
    g_assert_true(radios_audio_engine_runner_stop(e,NULL));q.adjunct.copyright=(RadioSAudioCopyrightWindow){0};adjunct_start(f,e,&q);
    render(e,1100,output);near(output[0][2198],.375);g_assert_true(radios_audio_engine_unload(e,2,NULL));
    render(e,1,output);near(output[0][0],.25);RadioSAudioRunSnapshot run;radios_audio_engine_runner_snapshot(e,&run);
    g_assert_cmpint(run.state,==,RADIOS_AUDIO_RUN_PLAYING);g_assert_cmpuint(run.index,==,0);radios_audio_engine_unref(e);
}
static void live_jingle_copyright_primary_continues(Fixture *f,gconstpointer unused)
{
    (void)unused;RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);RadioSAudioRunItem q=adjunct_item(f,1000,0,0);
    adjunct_start(f,e,&q);gfloat output[3][8192];render(e,1100,output);near(output[0][2198],.375);
    RadioSAudioRunSnapshot before,after;radios_audio_engine_runner_snapshot(e,&before);
    RadioSAudioCopyrightWindow denied={.restricted=TRUE,.denied=TRUE};
    g_assert_true(radios_audio_engine_runner_adjunct_copyright(e,-1,&denied,1,NULL));render(e,1,output);near(output[0][0],.25);
    radios_audio_engine_runner_snapshot(e,&after);g_assert_cmpuint(after.epoch,==,before.epoch);g_assert_cmpuint(after.index,==,before.index);g_assert_cmpint(after.state,==,RADIOS_AUDIO_RUN_PLAYING);
    g_assert_true(radios_audio_engine_runner_stop(e,NULL));adjunct_start(f,e,&q);render(e,1100,output);
    RadioSAudioCopyrightWindow expires={.restricted=TRUE,.ends_at_utc_us=g_get_real_time()+20000};
    g_assert_true(radios_audio_engine_runner_adjunct_copyright(e,-1,&expires,1,NULL));render(e,1,output);near(output[0][0],.375);
    g_usleep(30000);render(e,1,output);near(output[0][0],.25);radios_audio_engine_runner_snapshot(e,&after);
    g_assert_cmpint(after.state,==,RADIOS_AUDIO_RUN_PLAYING);g_assert_cmpuint(after.index,==,0);radios_audio_engine_unref(e);
}
static void incoming_zero_anchor_disjoint_overlay(Fixture *f,gconstpointer unused)
{
    (void)unused;RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);RadioSAudioRunOptions o;radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[2]={adjunct_item(f,1500,0,0),adjunct_item(f,0,0,0)};
    q[0].media.cue_out_frame=q[0].auto_next_frame=q[0].fade_start_frame=q[0].fade_end_frame=3100;
    q[0].mode=RADIOS_AUDIO_RUN_CROSSFADE;q[0].overlap_frames=1000;q[0].preferred_deck=0;
    q[1].entry_id=f->entry[1];q[1].preferred_deck=1;q[1].adjunct.player=3;q[1].fade_in_frames=1000;
    start(e,q,2,&o);ready(e,0,f->media[0],3000);ready(e,1,f->media[0],4800);ready(e,2,f->media[1],1000);ready(e,3,f->media[1],1000);
    gfloat output[3][8192];render(e,1500,output);near(output[0][2998],.25);render(e,500,output);near(output[0][0],.375);
    render(e,1,output);near(output[0][0],.5);RadioSAudioRunSnapshot run;radios_audio_engine_runner_snapshot(e,&run);
    g_assert_cmpint(run.state,==,RADIOS_AUDIO_RUN_OVERLAP);g_assert_cmpuint(run.index,==,0);
    render(e,500,output);near(output[0][998],.25*(cos(G_PI/4)+sin(G_PI/4))+.125);
    render(e,500,output);near(output[0][998],.25);radios_audio_engine_runner_snapshot(e,&run);g_assert_cmpuint(run.index,==,1);
    radios_audio_engine_unref(e);
}
static void same_aux_overlap_rejected(Fixture *f,gconstpointer unused)
{
    (void)unused;RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);RadioSAudioRunOptions o;radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[2]={adjunct_item(f,1500,0,0),adjunct_item(f,0,0,0)};q[1].entry_id=f->entry[1];
    q[0].media.cue_out_frame=q[0].auto_next_frame=q[0].fade_start_frame=q[0].fade_end_frame=3100;q[0].mode=RADIOS_AUDIO_RUN_CROSSFADE;q[0].overlap_frames=1000;
    GError *error=NULL;g_assert_false(radios_audio_engine_runner_configure(e,q,2,&o,&error));g_assert_error(error,RADIOS_AUDIO_ERROR_DOMAIN,RADIOS_AUDIO_ERROR_INVALID);g_clear_error(&error);
    RadioSAudioSnapshot s;radios_audio_engine_snapshot(e,&s);g_assert_cmpint(s.decks[0].state,==,RADIOS_AUDIO_EMPTY);g_assert_cmpint(s.decks[2].state,==,RADIOS_AUDIO_EMPTY);
    radios_audio_engine_unref(e);
}
static void manual_aux_stop_play_preserves_verified_media(Fixture *f,gconstpointer unused)
{
    (void)unused;RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);RadioSAudioRunItem q=adjunct_item(f,1000,0,0);adjunct_start(f,e,&q);
    gfloat output[3][8192];render(e,1100,output);near(output[0][2198],.375);
    g_assert_true(radios_audio_engine_stop(e,2,NULL));ready(e,2,f->media[1],1000);render(e,1,output);near(output[0][0],.25);
    RadioSAudioSnapshot s;radios_audio_engine_snapshot(e,&s);g_assert_cmpstr(s.decks[2].media_id,==,f->media[1]);g_assert_cmpuint(s.decks[2].cursor_frames,==,200);
    g_assert_true(radios_audio_engine_play(e,2,NULL));render(e,1,output);near(output[0][0],.375);
    RadioSAudioRunSnapshot run;radios_audio_engine_runner_snapshot(e,&run);g_assert_cmpint(run.state,==,RADIOS_AUDIO_RUN_PLAYING);g_assert_cmpuint(run.index,==,0);
    g_assert_true(radios_audio_engine_pause(e,2,NULL));render(e,1,output);near(output[0][0],.25);radios_audio_engine_unref(e);
}
static void auxiliary_binding_fence_and_mapping_retirement(Fixture *f,gconstpointer unused)
{
    (void)unused;RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);RadioSAudioRunItem q=adjunct_item(f,1000,0,0);adjunct_start(f,e,&q);
    RadioSAudioPlayerBinding binding={.deck=2};g_strlcpy(binding.playlist_id,f->entry[0],sizeof binding.playlist_id);g_strlcpy(binding.entry_id,f->entry[1],sizeof binding.entry_id);
    g_strlcpy(binding.provider,"probe",sizeof binding.provider);g_strlcpy(binding.source_id,"unrelated-source",sizeof binding.source_id);g_strlcpy(binding.title,"Unrelated title",sizeof binding.title);
    GError *error=NULL;g_assert_false(radios_audio_engine_bind_external_player(e,&binding,&error));g_assert_error(error,RADIOS_AUDIO_ERROR_DOMAIN,RADIOS_AUDIO_ERROR_STATE);g_clear_error(&error);
    gfloat output[3][8192];render(e,1100,output);near(output[0][2198],.375);
    RadioSAudioMixerConfig mixer;radios_audio_engine_get_mixer(e,&mixer);mixer.channels[2].source_deck=5;g_assert_true(radios_audio_engine_apply_mixer(e,&mixer,NULL));
    render(e,1,output);near(output[0][0],.25);RadioSAudioRunSnapshot run;radios_audio_engine_runner_snapshot(e,&run);g_assert_cmpint(run.state,==,RADIOS_AUDIO_RUN_PLAYING);g_assert_cmpuint(run.index,==,0);
    RadioSAudioSnapshot s;radios_audio_engine_snapshot(e,&s);g_assert_cmpstr(s.decks[2].media_id,==,f->media[1]);g_assert_cmpint(s.decks[2].state,==,RADIOS_AUDIO_PAUSED);radios_audio_engine_unref(e);
}
/* Optional auxiliary acquisition must never make the music queue depend on
 * a stopped, cued, unmapped or non-MAIN player becoming available. */
static void optional_unavailable_auxiliary_preserves_players(Fixture *f,gconstpointer mode)
{
    RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);
    RadioSAudioRunOptions o;radios_audio_run_options_default(&o);o.skip_unavailable_adjuncts=TRUE;
    RadioSAudioRunItem q[2]={adjunct_item(f,1000,0,0),item(f,0,1,100,8100)};
    q[1].preferred_deck=1;
    guint kind=GPOINTER_TO_UINT(mode);
    if (!kind) { g_assert_true(radios_audio_engine_load(e,2,&q[0].adjunct.media,NULL));ready(e,2,f->media[1],1000); }
    RadioSAudioMixerConfig mixer;radios_audio_engine_get_mixer(e,&mixer);
    if (kind==1) mixer.channels[2].pfl=TRUE;
    if (kind==2) g_assert_true(radios_audio_engine_set_deck_main(e,2,FALSE,NULL));
    if (kind==3) mixer.channels[2].source_deck=4;
    g_assert_true(radios_audio_engine_apply_mixer(e,&mixer,NULL));
    RadioSAudioSnapshot before,after;radios_audio_engine_snapshot(e,&before);
    start(e,q,2,&o);ready(e,0,f->media[0],4800);ready(e,1,f->media[0],4800);
    g_assert_true(q[0].adjunct.enabled);gfloat output[3][8192];
    render(e,1500,output);near(output[0][0],.25);near(output[0][2998],.25);
    render(e,4096,output);render(e,3000,output);near(output[0][5998],.25);
    RadioSAudioRunSnapshot run;radios_audio_engine_runner_snapshot(e,&run);
    g_assert_cmpuint(run.index,==,1);g_assert_cmpint(run.deck_current,==,1);
    radios_audio_engine_snapshot(e,&after);
    g_assert_cmpuint(after.decks[2].generation,==,before.decks[2].generation);
    g_assert_cmpint(after.decks[2].state,==,before.decks[2].state);
    g_assert_cmpstr(after.decks[2].media_id,==,before.decks[2].media_id);
    g_assert_cmpuint(after.decks[2].cursor_frames,==,before.decks[2].cursor_frames);
    g_assert_cmpmem(&after.mixer,sizeof after.mixer,&before.mixer,sizeof before.mixer);
    gint owner=123;g_assert_false(radios_audio_engine_deck_runner_owner(e,2,&owner));g_assert_cmpint(owner,==,123);
    radios_audio_engine_unref(e);
}
static void optional_auxiliary_keeps_cue_pcm(Fixture *f,gconstpointer unused)
{
    (void)unused;RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);
    RadioSAudioRunOptions o;radios_audio_run_options_default(&o);o.skip_unavailable_adjuncts=TRUE;
    RadioSAudioRunItem q=adjunct_item(f,1000,0,0);
    RadioSAudioPreferences preferences;radios_audio_preferences_default(&preferences);preferences.cue_source=-1;
    g_assert_true(radios_audio_engine_apply_preferences(e,&preferences,NULL));
    RadioSAudioMixerConfig mixer;radios_audio_engine_get_mixer(e,&mixer);mixer.channels[2].pfl=TRUE;
    g_assert_true(radios_audio_engine_apply_mixer(e,&mixer,NULL));
    RadioSAudioLoadSpec cue=q.adjunct.media;cue.cue_out_frame=8100;
    g_assert_true(radios_audio_engine_load(e,2,&cue,NULL));ready(e,2,f->media[1],4800);
    g_assert_true(radios_audio_engine_play(e,2,NULL));
    g_assert_true(radios_audio_engine_set_deck_main(e,2,FALSE,NULL));
    RadioSAudioSnapshot before,after;radios_audio_engine_snapshot(e,&before);
    start(e,&q,1,&o);ready(e,0,f->media[0],4800);
    gfloat output[3][8192];render(e,1100,output);near(output[0][2198],.25);near(output[1][2198],.125);
    radios_audio_engine_snapshot(e,&after);g_assert_cmpuint(after.decks[2].generation,==,before.decks[2].generation);
    g_assert_cmpint(after.decks[2].state,==,RADIOS_AUDIO_PLAYING);g_assert_cmpstr(after.decks[2].media_id,==,f->media[1]);
    g_assert_cmpuint(after.decks[2].cursor_frames,==,cue.cue_in_frame+1100);radios_audio_engine_unref(e);
}
static void optional_auxiliary_keeps_valid_overlay_and_lease_query(Fixture *f,gconstpointer unused)
{
    (void)unused;RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);
    RadioSAudioRunOptions o;radios_audio_run_options_default(&o);o.skip_unavailable_adjuncts=TRUE;
    RadioSAudioRunItem q=adjunct_item(f,1000,0,0);gint owner=123;
    g_assert_false(radios_audio_engine_deck_runner_owner(e,2,&owner));g_assert_cmpint(owner,==,123);
    start(e,&q,1,&o);ready(e,0,f->media[0],4800);ready(e,2,f->media[1],1000);
    g_assert_true(radios_audio_engine_deck_runner_owner(e,0,&owner));g_assert_cmpint(owner,==,-1);
    owner=123;g_assert_true(radios_audio_engine_deck_runner_owner(e,2,&owner));g_assert_cmpint(owner,==,-1);
    g_assert_true(radios_audio_engine_deck_runner_owner(e,2,NULL));
    gfloat output[3][8192];render(e,1100,output);near(output[0][2198],.375);
    g_assert_true(radios_audio_engine_runner_stop(e,NULL));
    g_assert_false(radios_audio_engine_deck_runner_owner(e,2,&owner));
    g_assert_false(radios_audio_engine_deck_runner_owner(e,32,&owner));
    g_assert_false(radios_audio_engine_deck_runner_owner(NULL,2,&owner));radios_audio_engine_unref(e);
}
static void optional_auxiliary_preserves_foreign_silent_lease(Fixture *f,gconstpointer unused)
{
    (void)unused;RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);
    RadioSAudioRunItem private_q[3]={item(f,0,0,100,8100),item(f,0,1,100,8100),adjunct_item(f,1000,0,0)};
    private_q[2].entry_id=f->entry[2];
    RadioSAudioRunOptions private_o;radios_audio_run_options_default(&private_o);
    private_o.pool_count=1;private_o.deck_pool[0]=3;private_o.start_utc_us=g_get_real_time()+60000000;
    g_assert_true(radios_audio_engine_player_runner_configure(e,3,private_q,3,&private_o,NULL));
    g_assert_true(radios_audio_engine_player_runner_action(e,3,RADIOS_AUDIO_PLAYER_RUN_START,NULL));ready(e,3,f->media[0],4800);
    RadioSAudioSnapshot before,after;radios_audio_engine_snapshot(e,&before);g_assert_cmpint(before.decks[2].state,==,RADIOS_AUDIO_EMPTY);
    RadioSAudioRunSnapshot private_before,private_after;radios_audio_engine_player_runner_snapshot(e,3,&private_before);
    gint owner=123;g_assert_true(radios_audio_engine_deck_runner_owner(e,2,&owner));g_assert_cmpint(owner,==,3);
    g_assert_true(radios_audio_engine_deck_runner_owner(e,3,&owner));g_assert_cmpint(owner,==,3);
    RadioSAudioRunItem q=adjunct_item(f,1000,0,0);RadioSAudioRunOptions o;radios_audio_run_options_default(&o);o.skip_unavailable_adjuncts=TRUE;
    start(e,&q,1,&o);ready(e,0,f->media[0],4800);gfloat output[3][8192];render(e,1100,output);near(output[0][2198],.25);
    radios_audio_engine_snapshot(e,&after);g_assert_cmpint(after.decks[2].state,==,RADIOS_AUDIO_EMPTY);
    g_assert_cmpuint(after.decks[2].generation,==,before.decks[2].generation);
    g_assert_cmpuint(after.decks[3].generation,==,before.decks[3].generation);
    g_assert_cmpuint(after.decks[3].cursor_frames,==,before.decks[3].cursor_frames);
    g_assert_true(radios_audio_engine_deck_runner_owner(e,2,&owner));g_assert_cmpint(owner,==,3);
    radios_audio_engine_player_runner_snapshot(e,3,&private_after);g_assert_cmpuint(private_after.epoch,==,private_before.epoch);
    g_assert_cmpuint(private_after.count,==,3);g_assert_true(private_after.owned[3]);radios_audio_engine_unref(e);
}
static void optional_append_skips_only_auxiliary_and_validates_atomically(Fixture *f,gconstpointer unused)
{
    (void)unused;RadioSAudioEngine *e=radios_audio_engine_new(FALSE,NULL);
    RadioSAudioRunOptions o;radios_audio_run_options_default(&o);o.skip_unavailable_adjuncts=TRUE;
    RadioSAudioRunItem q=item(f,0,0,100,6100);q.preferred_deck=0;start(e,&q,1,&o);ready(e,0,f->media[0],4800);
    RadioSAudioRunItem extra=adjunct_item(f,1000,0,0);extra.entry_id=f->entry[1];extra.preferred_deck=1;
    g_assert_true(radios_audio_engine_load(e,2,&extra.adjunct.media,NULL));ready(e,2,f->media[1],1000);
    RadioSAudioSnapshot before,after;radios_audio_engine_snapshot(e,&before);
    RadioSAudioRunItem invalid[2]={extra,extra};invalid[0].adjunct.player=3;invalid[1].entry_id=f->entry[2];invalid[1].media.sha256="bad";
    GError *error=NULL;g_assert_false(radios_audio_engine_runner_append(e,invalid,2,&error));
    g_assert_error(error,RADIOS_AUDIO_ERROR_DOMAIN,RADIOS_AUDIO_ERROR_INVALID);g_clear_error(&error);
    gint owner=123;g_assert_false(radios_audio_engine_deck_runner_owner(e,3,&owner));g_assert_cmpint(owner,==,123);
    RadioSAudioRunSnapshot run;radios_audio_engine_runner_snapshot(e,&run);g_assert_cmpuint(run.count,==,1);
    g_assert_true(radios_audio_engine_runner_append(e,&extra,1,&error));g_assert_no_error(error);g_assert_true(extra.adjunct.enabled);
    ready(e,1,f->media[0],4800);gfloat output[3][8192];render(e,4096,output);near(output[0][8190],.25);
    render(e,3100,output);near(output[0][6198],.25);radios_audio_engine_runner_snapshot(e,&run);
    g_assert_cmpuint(run.count,==,2);g_assert_cmpuint(run.index,==,1);g_assert_cmpint(run.deck_current,==,1);
    radios_audio_engine_snapshot(e,&after);g_assert_cmpuint(after.decks[2].generation,==,before.decks[2].generation);
    g_assert_cmpint(after.decks[2].state,==,RADIOS_AUDIO_READY);g_assert_cmpstr(after.decks[2].media_id,==,f->media[1]);
    g_assert_cmpuint(after.decks[2].cursor_frames,==,before.decks[2].cursor_frames);
    g_assert_false(radios_audio_engine_deck_runner_owner(e,2,&owner));radios_audio_engine_unref(e);
}
int main(int argc,char **argv)
{
    g_test_init(&argc,&argv,NULL);
    g_test_add("/automation-adjunct/sample-exact-overlay",Fixture,NULL,setup,sample_exact_overlay,teardown);
    g_test_add("/automation-adjunct/duck-fade-envelope",Fixture,NULL,setup,duck_fade_envelope,teardown);
    g_test_add("/automation-adjunct/pause-stop-seek-lifecycle",Fixture,NULL,setup,pause_stop_seek_lifecycle,teardown);
    g_test_add("/automation-adjunct/occupied-cue-private-safety",Fixture,NULL,setup,occupied_cue_private_safety,teardown);
    g_test_add("/automation-adjunct/manual-aux-only-copyright",Fixture,NULL,setup,manual_aux_only_and_copyright,teardown);
    g_test_add("/automation-adjunct/live-jingle-copyright-primary-continues",Fixture,NULL,setup,live_jingle_copyright_primary_continues,teardown);
    g_test_add("/automation-adjunct/incoming-zero-anchor-disjoint-overlay",Fixture,NULL,setup,incoming_zero_anchor_disjoint_overlay,teardown);
    g_test_add("/automation-adjunct/same-aux-overlap-rejected",Fixture,NULL,setup,same_aux_overlap_rejected,teardown);
    g_test_add("/automation-adjunct/manual-aux-STOP-PLAY-keeps-verified-source",Fixture,NULL,setup,manual_aux_stop_play_preserves_verified_media,teardown);
    g_test_add("/automation-adjunct/auxiliary-binding-fence-mapping-retirement",Fixture,NULL,setup,auxiliary_binding_fence_and_mapping_retirement,teardown);
    g_test_add("/automation-adjunct/optional-ready-aux-primary-continues",Fixture,GUINT_TO_POINTER(0),setup,optional_unavailable_auxiliary_preserves_players,teardown);
    g_test_add("/automation-adjunct/optional-PFL-aux-primary-continues",Fixture,GUINT_TO_POINTER(1),setup,optional_unavailable_auxiliary_preserves_players,teardown);
    g_test_add("/automation-adjunct/optional-nonMAIN-aux-primary-continues",Fixture,GUINT_TO_POINTER(2),setup,optional_unavailable_auxiliary_preserves_players,teardown);
    g_test_add("/automation-adjunct/optional-unmapped-aux-primary-continues",Fixture,GUINT_TO_POINTER(3),setup,optional_unavailable_auxiliary_preserves_players,teardown);
    g_test_add("/automation-adjunct/optional-aux-keeps-CUE-PCM",Fixture,NULL,setup,optional_auxiliary_keeps_cue_pcm,teardown);
    g_test_add("/automation-adjunct/optional-aux-valid-overlay-owner-query",Fixture,NULL,setup,optional_auxiliary_keeps_valid_overlay_and_lease_query,teardown);
    g_test_add("/automation-adjunct/optional-aux-foreign-empty-reservation",Fixture,NULL,setup,optional_auxiliary_preserves_foreign_silent_lease,teardown);
    g_test_add("/automation-adjunct/optional-append-aux-and-validation-atomicity",Fixture,NULL,setup,optional_append_skips_only_auxiliary_and_validates_atomically,teardown);
    return g_test_run();
}
