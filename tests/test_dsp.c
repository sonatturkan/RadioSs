#include "radios/audio.h"
#include <math.h>
#include <string.h>

/* Default microphone gate ramp (5 ms): opening reaches full level and closing
 * reaches silence within this many frames (radios_audio_dsp_set_microphone_ramp). */
#define GATE_RAMP 240u
typedef struct { gfloat sources[RADIOS_AUDIO_DECKS * 2], microphone[2], main[2], cue[2]; gboolean routed[RADIOS_AUDIO_DECKS]; } Frame;
static void frame_init(Frame *frame)
{ memset(frame, 0, sizeof *frame); for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) frame->routed[i] = i != 32; }
static void simple_config(RadioSAudioMixerConfig *config)
{ radios_audio_mixer_config_default(config); for (guint i = 0; i < 32; ++i) config->channels[i].source_deck = -1; config->channels[0].source_deck = 0; config->speech_ducking = FALSE; }
static void configure(RadioSAudioDsp *dsp, RadioSAudioMixerConfig *config)
{
    GError *error = NULL; g_assert_true(radios_audio_dsp_configure(dsp, config, &error)); g_assert_no_error(error);
    Frame silence; frame_init(&silence);
    for (guint i = 0; i < 240; ++i) radios_audio_dsp_process_frame(dsp, silence.sources, silence.routed, silence.microphone, FALSE, FALSE, silence.main, silence.cue);
}
static void process(RadioSAudioDsp *dsp, Frame *frame, gboolean gate, gboolean cue)
{ radios_audio_dsp_process_frame(dsp, frame->sources, frame->routed, frame->microphone, gate, cue, frame->main, frame->cue); }
static void near(gdouble actual, gdouble expected, gdouble tolerance)
{ g_assert_cmpfloat(fabs(actual - expected), <, tolerance); }
static void validation(void)
{
    RadioSAudioMixerConfig config; radios_audio_mixer_config_default(&config); GError *error = NULL;
    g_assert_true(radios_audio_mixer_config_validate(&config, &error)); g_assert_no_error(error);
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); config.channels[31].source_deck = 32;
    g_assert_false(radios_audio_dsp_configure(dsp, &config, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    RadioSAudioMixerConfig unchanged; radios_audio_dsp_get_config(dsp, &unchanged); g_assert_cmpint(unchanged.channels[31].source_deck, ==, 31);
    config = unchanged; config.effects[4].depth = NAN; g_assert_false(radios_audio_dsp_configure(dsp, &config, &error)); g_clear_error(&error);
    config = unchanged; config.channels[0].eq_low_db = -26; config.channels[31].eq_high_db = 6; configure(dsp, &config);
    config.channels[0].pan = 1.01; g_assert_false(radios_audio_dsp_configure(dsp, &config, &error)); g_clear_error(&error);
    config = unchanged; config.master_fx.type = (RadioSAudioFxType)14; g_assert_false(radios_audio_dsp_configure(dsp, &config, &error)); g_clear_error(&error);
    config = unchanged; config.crossfader = INFINITY; g_assert_false(radios_audio_dsp_configure(dsp, &config, &error)); g_clear_error(&error);
    g_assert_cmpstr(radios_audio_fx_name(RADIOS_FX_REV_ROLL), ==, "Rev Roll"); radios_audio_dsp_free(dsp);
}
static void routing(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    config.channels[1].source_deck = 1; configure(dsp, &config); Frame frame; frame_init(&frame);
    frame.sources[0] = .1f; frame.sources[2] = .2f; frame.sources[33 * 2] = .05f; frame.sources[34 * 2] = .025f;
    frame.sources[65 * 2] = .04f; frame.sources[32 * 2] = .9f;
    process(dsp, &frame, FALSE, TRUE); near(frame.main[0], .415, .000001); near(frame.cue[0], 0, .000001);
    config.channels[0].mute = TRUE; config.channels[0].pfl = TRUE; configure(dsp, &config);
    process(dsp, &frame, FALSE, TRUE); near(frame.main[0], .265, .000001); near(frame.cue[0], .15, .000001);
    config.channels[0].mute = FALSE; config.channels[1].solo = TRUE; configure(dsp, &config);
    process(dsp, &frame, FALSE, TRUE); near(frame.main[0], .265, .000001); near(frame.cue[0], .15, .000001);
    process(dsp, &frame, FALSE, FALSE); near(frame.cue[0], 0, .000001);
    config.channels[1].solo = FALSE; config.channels[1].source_deck = 0; configure(dsp, &config);
    process(dsp, &frame, FALSE, TRUE); near(frame.main[0], .315, .000001);
    radios_audio_dsp_free(dsp);
}
static void pan_gain_crossfade(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config); Frame frame; frame_init(&frame);
    frame.sources[0] = .25f; frame.sources[1] = .5f; config.channels[0].pan = 1; configure(dsp, &config);
    process(dsp, &frame, FALSE, FALSE); near(frame.main[0], 0, .000001); near(frame.main[1], .5, .000001);
    config.channels[0].pan = 0; config.channels[0].gain_db = -6.020599913279624; configure(dsp, &config);
    process(dsp, &frame, FALSE, FALSE); near(frame.main[0], .125, .000001); near(frame.main[1], .25, .000001);
    config.channels[0].gain_db = 0; config.channels[0].crossfade_group = 1; config.channels[1].source_deck = 1; config.channels[1].crossfade_group = 2;
    frame.sources[0] = .1f; frame.sources[1] = 0; frame.sources[2] = 0; frame.sources[3] = .1f;
    for (guint i = 0; i <= 40; ++i) {
        config.crossfader = -1 + i / 20.0; configure(dsp, &config); process(dsp, &frame, FALSE, FALSE);
        near(frame.main[0] * frame.main[0] + frame.main[1] * frame.main[1], .01, .00000001);
    }
    radios_audio_dsp_free(dsp);
}
static gdouble tone_rms(RadioSAudioDsp *dsp, gdouble frequency)
{
    Frame frame; frame_init(&frame); gdouble energy = 0; radios_audio_dsp_reset(dsp);
    for (guint i = 0; i < 12000; ++i) {
        frame.sources[0] = .05f * (gfloat)sin(2 * G_PI * frequency * i / 48000); process(dsp, &frame, FALSE, FALSE);
        if (i >= 6000) energy += frame.main[0] * frame.main[0];
    }
    return sqrt(energy / 6000);
}
static void equalizer(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    const gdouble frequencies[] = {60, 1000, 15000};
    for (guint band = 0; band < 3; ++band) {
        simple_config(&config); configure(dsp, &config); gdouble dry = tone_rms(dsp, frequencies[band]);
        gdouble *value = band == 0 ? &config.channels[0].eq_low_db : band == 1 ? &config.channels[0].eq_mid_db : &config.channels[0].eq_high_db;
        *value = 6; configure(dsp, &config); gdouble boost = tone_rms(dsp, frequencies[band]); g_assert_cmpfloat(boost / dry, >, 1.8);
        *value = -26; configure(dsp, &config); gdouble cut = tone_rms(dsp, frequencies[band]); g_assert_cmpfloat(cut / dry, <, .15);
    }
    simple_config(&config); configure(dsp, &config); gdouble dry = tone_rms(dsp, 1000);
    config.master.eq_mid_db = -26; configure(dsp, &config); g_assert_cmpfloat(tone_rms(dsp, 1000) / dry, <, .1);
    radios_audio_dsp_free(dsp);
}
static void all_effects(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    config.channels[0].fx_slot = 0; config.channels[0].fx_return_db = 0;
    config.effects[0].enabled = TRUE; config.effects[0].cue = TRUE; config.effects[0].return_db = 0; config.effects[0].depth = .75;
    for (gint type = 0; type < RADIOS_FX_COUNT; ++type) {
        config.effects[0].type = (RadioSAudioFxType)type; configure(dsp, &config); radios_audio_dsp_reset(dsp);
        Frame frame; frame_init(&frame); gdouble energy = 0, late_energy = 0;
        for (guint i = 0; i < 30000; ++i) {
            frame.sources[0] = i < 6000 ? (gfloat)(.2 * sin(2 * G_PI * (233.0 * i / 48000 + .0000002 * i * i))) : 0;
            process(dsp, &frame, FALSE, TRUE); g_assert_true(isfinite(frame.main[0])); g_assert_true(isfinite(frame.cue[0]));
            energy += frame.cue[0] * frame.cue[0]; if (i > 22000) late_energy += frame.cue[0] * frame.cue[0];
        }
        g_test_message("%s measured wet energy %.5f", radios_audio_fx_name((RadioSAudioFxType)type), energy);
        g_assert_cmpfloat(energy, >, .01);
        if (type == RADIOS_FX_SLIP_ROLL || type == RADIOS_FX_REV_ROLL) g_assert_cmpfloat(late_energy, >, 1);
        if (type == RADIOS_FX_ROLL) near(late_energy, 0, .00001);
        config.effects[0].enabled = FALSE; configure(dsp, &config); frame.sources[0] = .2f; process(dsp, &frame, FALSE, TRUE); near(frame.cue[0], 0, .000001);
        config.effects[0].enabled = TRUE;
    }
    radios_audio_dsp_free(dsp);
}
static gdouble delayed_impulse(RadioSAudioDsp *dsp, RadioSAudioMixerConfig *config)
{
    configure(dsp, config); radios_audio_dsp_reset(dsp); Frame frame; frame_init(&frame); gdouble peak = 0;
    for (guint i = 0; i < 30000; ++i) {
        frame.sources[0] = i == 0 ? .25f : 0; process(dsp, &frame, FALSE, TRUE); peak = MAX(peak, fabs(frame.cue[0]));
    }
    return peak;
}
static void send_return(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    config.channels[0].fx_slot = 0; config.channels[0].fx_return_db = 0;
    config.effects[0].enabled = config.effects[0].cue = TRUE; config.effects[0].return_db = 0; config.effects[0].depth = 1;
    gdouble full = delayed_impulse(dsp, &config); near(full, .25, .000001);
    gdouble *gains[] = {&config.channels[0].fx_send_db, &config.channels[0].fx_return_db, &config.effects[0].send_db, &config.effects[0].return_db};
    for (guint i = 0; i < 4; ++i) { *gains[i] = -6.020599913279624; near(delayed_impulse(dsp, &config), full / 2, .000001); *gains[i] = 0; }
    config.effects[0].depth = 0; near(delayed_impulse(dsp, &config), 0, .000001); radios_audio_dsp_free(dsp);
}
static void five_slots_master(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    for (guint i = 0; i < 5; ++i) {
        config.channels[i].source_deck = 0; config.channels[i].fx_slot = (gint)i; config.channels[i].fx_return_db = 0;
        config.effects[i].type = RADIOS_FX_FILTER; config.effects[i].enabled = config.effects[i].cue = TRUE; config.effects[i].depth = .5; config.effects[i].return_db = 0;
    }
    configure(dsp, &config); Frame frame; frame_init(&frame); frame.sources[0] = .1f;
    for (guint i = 0; i < 200; ++i) process(dsp, &frame, FALSE, TRUE);
    near(frame.main[0], .75, .000001); near(frame.cue[0], .25, .000001);
    config.effects[2].enabled = FALSE; configure(dsp, &config);
    for (guint i = 0; i < 200; ++i) process(dsp, &frame, FALSE, TRUE);
    near(frame.main[0], .7, .000001); near(frame.cue[0], .2, .000001);
    config.master_fx.type = RADIOS_FX_FILTER; config.master_fx.depth = 1; config.master_fx.enabled = config.master_fx.cue = TRUE;
    config.master_fx.return_db = config.master.fx_return_db = 0; configure(dsp, &config);
    for (guint i = 0; i < 2000; ++i) process(dsp, &frame, FALSE, TRUE);
    g_assert_cmpfloat(frame.main[0], >, 1.3); config.master.mute = TRUE; configure(dsp, &config);
    for (guint i = 0; i < 2000; ++i) process(dsp, &frame, FALSE, TRUE);
    near(frame.main[0], 0, .000001); g_assert_cmpfloat(frame.cue[0], >, .8);
    radios_audio_dsp_free(dsp);
}
static void master_aux(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    g_assert_cmpint(config.master.fx_slot, ==, -1);
    config.master.fx_slot = 2; config.master.fx_return_db = 0;
    config.effects[2].type = RADIOS_FX_FILTER; config.effects[2].depth = 1;
    config.effects[2].enabled = config.effects[2].cue = TRUE; config.effects[2].return_db = 0;
    configure(dsp, &config); Frame frame; frame_init(&frame); frame.sources[0] = .1f;
    for (guint i = 0; i < 2000; ++i) process(dsp, &frame, FALSE, TRUE);
    near(frame.main[0], .2, .000001); near(frame.cue[0], .1, .000001);
    radios_audio_dsp_begin_block(dsp); process(dsp, &frame, FALSE, TRUE);
    RadioSAudioDspMeters meters; radios_audio_dsp_get_meters(dsp, &meters);
    near(meters.fx_send_peak[2][0], .1, .000001); near(meters.fx_return_peak[2][0], .1, .000001);
    gdouble *gains[] = {&config.master.fx_send_db, &config.master.fx_return_db, &config.effects[2].send_db, &config.effects[2].return_db};
    for (guint gain = 0; gain < 4; ++gain) {
        *gains[gain] = -6.020599913279624; configure(dsp, &config);
        for (guint i = 0; i < 2000; ++i) process(dsp, &frame, FALSE, TRUE);
        near(frame.main[0], .15, .000001); near(frame.cue[0], .05, .000001); *gains[gain] = 0;
    }
    config.master_fx.type = RADIOS_FX_FILTER; config.master_fx.enabled = TRUE;
    config.master_fx.depth = 1; config.master_fx.return_db = 0; configure(dsp, &config);
    for (guint i = 0; i < 2000; ++i) process(dsp, &frame, FALSE, TRUE);
    near(frame.main[0], .4, .000001); near(frame.cue[0], .1, .000001);
    config.master.mute = TRUE; configure(dsp, &config);
    for (guint i = 0; i < 2000; ++i) process(dsp, &frame, FALSE, TRUE);
    near(frame.main[0], 0, .000001); near(frame.cue[0], .1, .000001);
    config.effects[2].monitor_path = 2; configure(dsp, &config);
    for (guint i = 0; i < 2000; ++i) process(dsp, &frame, FALSE, TRUE);
    near(frame.cue[0], .2, .000001);
    GError *error = NULL; config.master.fx_slot = 5;
    g_assert_false(radios_audio_dsp_configure(dsp, &config, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    RadioSAudioMixerConfig current; radios_audio_dsp_get_config(dsp, &current); g_assert_cmpint(current.master.fx_slot, ==, 2);
    config.master.fx_slot = -1; config.master.mute = FALSE; config.master_fx.enabled = FALSE; configure(dsp, &config);
    process(dsp, &frame, FALSE, TRUE); near(frame.main[0], .1, .000001); near(frame.cue[0], 0, .000001);
    radios_audio_dsp_free(dsp);
}
static void ducking(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    config.speech_ducking = TRUE; config.speech_bed_db = -20; configure(dsp, &config); Frame frame; frame_init(&frame);
    frame.sources[0] = .5f; frame.microphone[0] = .1f;
    for (guint i = 0; i < 4000; ++i) process(dsp, &frame, TRUE, FALSE);
    /* Closing ramps the microphone out within GATE_RAMP frames (5 ms); the bed
     * is still ducked then, because the designed release is slower (~50 ms). */
    near(frame.main[0], .15, .00001);
    for (guint i = 0; i < GATE_RAMP; ++i) process(dsp, &frame, FALSE, FALSE);
    g_assert_cmpfloat(frame.main[0], <, .1);
    for (guint i = 0; i < 30000; ++i) process(dsp, &frame, FALSE, FALSE);
    near(frame.main[0], .5, .00001);
    config.speech_ducking = FALSE; config.channels[0].source_deck = -2; config.channels[0].fx_slot = 0; config.channels[0].fx_return_db = 0;
    config.effects[0].type = RADIOS_FX_ECHO; config.effects[0].enabled = TRUE; config.effects[0].depth = 1; config.effects[0].return_db = 0;
    configure(dsp, &config); frame.sources[0] = 0;
    for (guint i = 0; i < 16000; ++i) process(dsp, &frame, TRUE, FALSE);
    /* The voice and its echo only fade during the ramp; then no tail at all. */
    gdouble previous = fabs(frame.main[0]);
    for (guint i = 0; i < 20000; ++i) {
        process(dsp, &frame, FALSE, FALSE);
        if (i < GATE_RAMP) { g_assert_cmpfloat(fabs(frame.main[0]), <=, previous + .000001); previous = fabs(frame.main[0]); }
        else near(frame.main[0], 0, .000001);
    }
    radios_audio_dsp_free(dsp);
}
static void external_master(void)
{
    RadioSAudioDsp *external = radios_audio_dsp_new(), *reference = radios_audio_dsp_new(), *plain = radios_audio_dsp_new();
    RadioSAudioMixerConfig config; simple_config(&config); config.master.eq_mid_db = 6;
    config.master_fx.type = RADIOS_FX_FILTER; config.master_fx.enabled = TRUE; config.master_fx.depth = .5;
    config.master.fx_return_db = config.master_fx.return_db = 0;
    configure(external, &config); configure(reference, &config);
    config.master.eq_mid_db = 0; config.master_fx.enabled = FALSE; configure(plain, &config);
    Frame source, empty; frame_init(&source); frame_init(&empty); gdouble wet = 0, dry = 0;
    for (guint i = 0; i < 5000; ++i) {
        gfloat music[2] = {(gfloat)(.2 * sin(2 * G_PI * 1000 * i / 48000)), 0}; source.sources[0] = music[0];
        process(reference, &source, FALSE, FALSE);
        radios_audio_dsp_process_frame_with_external(external, empty.sources, empty.routed, empty.microphone,
            FALSE, music, FALSE, empty.main, empty.cue);
        near(empty.main[0], source.main[0], .000001);
        if (i > 1000) wet += empty.main[0] * empty.main[0];
        radios_audio_dsp_process_frame_with_external(plain, empty.sources, empty.routed, empty.microphone,
            FALSE, music, FALSE, empty.main, empty.cue);
        if (i > 1000) dry += empty.main[0] * empty.main[0];
    }
    g_assert_cmpfloat(wet, >, dry * 3);
    config.master.mute = TRUE; configure(external, &config); gfloat music[2] = {.25f, 0};
    radios_audio_dsp_process_frame_with_external(external, empty.sources, empty.routed, empty.microphone,
        FALSE, music, FALSE, empty.main, empty.cue); near(empty.main[0], 0, .000001);
    config.master.mute = FALSE; config.speech_ducking = TRUE; config.speech_bed_db = -20;
    configure(external, &config); radios_audio_dsp_reset(external); empty.microphone[0] = .1f;
    for (guint i = 0; i < 4000; ++i) radios_audio_dsp_process_frame_with_external(external,
        empty.sources, empty.routed, empty.microphone, TRUE, music, FALSE, empty.main, empty.cue);
    near(empty.main[0], .125, .00001);
    for (guint i = 0; i < 30000; ++i) radios_audio_dsp_process_frame_with_external(external,
        empty.sources, empty.routed, empty.microphone, FALSE, music, FALSE, empty.main, empty.cue);
    near(empty.main[0], .25, .00001);
    radios_audio_dsp_free(external); radios_audio_dsp_free(reference); radios_audio_dsp_free(plain);
}
static void transition(void)
{
    RadioSAudioTransition plan = {48000, 24000, 24000, 0}; GError *error = NULL; g_assert_true(radios_audio_transition_validate(&plan, &error));
    gdouble a, b; radios_audio_transition_gains(&plan, 0, &a, &b); near(a, 1, .000001); near(b, 0, .000001);
    radios_audio_transition_gains(&plan, 36000, &a, &b); near(a, sqrt(.5), .000001); near(b, sqrt(.5), .000001);
    near(a * a + b * b, 1, .000001); radios_audio_transition_gains(&plan, 48000, &a, &b); near(a, 0, .000001); near(b, 1, .000001);
    plan.duck_db = -20; radios_audio_transition_gains(&plan, 36000, &a, &b); near(a, .1 * sqrt(.5), .000001); near(b, sqrt(.5), .000001);
    plan.overlap_frames = 0; plan.fade_in_frames = 0; radios_audio_transition_gains(&plan, 48000, &a, &b); near(a, 0, .000001); near(b, 1, .000001);
    plan.overlap_frames = 48001; g_assert_false(radios_audio_transition_validate(&plan, &error)); g_clear_error(&error);
}
static void click_free_controls(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config); configure(dsp, &config);
    Frame frame; frame_init(&frame); frame.sources[0] = frame.sources[1] = .5f; process(dsp, &frame, FALSE, FALSE);
    config.channels[0].gain_db = -6.020599913279624; config.channels[0].pan = 1; config.channels[0].crossfade_group = 2; config.crossfader = -1;
    g_assert_true(radios_audio_dsp_configure(dsp, &config, NULL)); gfloat previous = .5f;
    for (guint i = 0; i < 240; ++i) {
        process(dsp, &frame, FALSE, FALSE); g_assert_cmpfloat(frame.main[0], <=, previous);
        g_assert_cmpfloat(previous - frame.main[0], <, .0063f); previous = frame.main[0];
    }
    near(frame.main[0], 0, .000001); near(frame.main[1], 0, .000001);
    config.crossfader = 1; g_assert_true(radios_audio_dsp_configure(dsp, &config, NULL)); previous = 0;
    for (guint i = 0; i < 240; ++i) { process(dsp, &frame, FALSE, FALSE); g_assert_cmpfloat(frame.main[1], >=, previous); g_assert_cmpfloat(frame.main[1] - previous, <, .0011f); previous = frame.main[1]; }
    near(frame.main[1], .25, .000001); radios_audio_dsp_free(dsp);
}
static void roll_distinction(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    config.channels[0].fx_slot = 0; config.channels[0].fx_return_db = 0;
    config.effects[0].enabled = config.effects[0].cue = TRUE; config.effects[0].depth = .75; config.effects[0].return_db = 0;
    gdouble values[3][2];
    for (guint type = 0; type < 3; ++type) {
        config.effects[0].type = (RadioSAudioFxType)(RADIOS_FX_SLIP_ROLL + type); configure(dsp, &config); radios_audio_dsp_reset(dsp);
        Frame frame; frame_init(&frame);
        for (guint i = 0; i <= 6000; ++i) { frame.sources[0] = i * .0001f; process(dsp, &frame, FALSE, TRUE); if (i == 3000) values[type][0] = frame.cue[0]; if (i == 6000) values[type][1] = frame.cue[0]; }
    }
    near(values[0][0], 0, .000001); near(values[2][0], .2999 * .75, .000001);
    near(values[0][1], 0, .000001); near(values[1][1], .3 * .75, .000001); radios_audio_dsp_free(dsp);
}
static void channel_send_return(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    config.channels[0].source_deck = -1; config.channels[7].source_deck = 0;
    config.channels[7].send_channel = 0; config.channels[7].send_db = 0;
    config.channels[0].gain_db = -6.020599913279624; config.channels[0].pfl = TRUE;
    configure(dsp, &config); Frame frame; frame_init(&frame); frame.sources[0] = frame.sources[1] = .2f;
    process(dsp, &frame, FALSE, TRUE); near(frame.main[0], .3, .000001); near(frame.cue[0], .1, .000001);
    config.channels[0].return_channel = 7; configure(dsp, &config);
    process(dsp, &frame, FALSE, TRUE); near(frame.main[0], .3, .000001);
    config.channels[7].send_channel = -1; configure(dsp, &config);
    process(dsp, &frame, FALSE, TRUE); near(frame.main[0], .3, .000001);
    config.channels[0].send_channel = 2; config.channels[0].send_db = 0;
    config.channels[2].gain_db = -6.020599913279624; configure(dsp, &config);
    process(dsp, &frame, FALSE, TRUE); near(frame.main[0], .35, .000001);
    config.channels[2].mute = TRUE; configure(dsp, &config);
    process(dsp, &frame, FALSE, TRUE); near(frame.main[0], .3, .000001);
    config.channels[7].mute = TRUE; configure(dsp, &config);
    process(dsp, &frame, FALSE, TRUE); near(frame.main[0], 0, .000001); near(frame.cue[0], 0, .000001);
    config.channels[7].mute = FALSE; configure(dsp, &config); frame.routed[0] = FALSE;
    process(dsp, &frame, FALSE, TRUE); near(frame.main[0], 0, .000001);
    radios_audio_dsp_free(dsp);
}

static void master_channel_routing(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    g_assert_cmpint(config.master.send_channel, ==, -1); g_assert_cmpint(config.master.return_channel, ==, -1);
    config.channels[0].source_deck = -1; config.channels[7].source_deck = 0;
    config.channels[0].gain_db = -6.020599913279624; config.channels[0].send_db = 0;
    config.master.send_channel = 0; config.master.send_db = 0;
    Frame frame; frame_init(&frame); frame.sources[0] = frame.sources[1] = .2f;
    configure(dsp, &config); process(dsp, &frame, FALSE, FALSE); near(frame.main[0], .3, .000001);
    config.master.return_channel = 0; configure(dsp, &config);
    for (guint sample = 0; sample < 10000; ++sample) {
        process(dsp, &frame, FALSE, FALSE); near(frame.main[0], .3, .000001);
    }
    config.master.return_db = -6.020599913279624; configure(dsp, &config);
    process(dsp, &frame, FALSE, FALSE); near(frame.main[0], .25, .000001);
    config.channels[0].send_db = -6.020599913279624; configure(dsp, &config);
    process(dsp, &frame, FALSE, FALSE); near(frame.main[0], .225, .000001);
    config.master.return_channel = -1; config.master.send_db = -6.020599913279624; configure(dsp, &config);
    process(dsp, &frame, FALSE, FALSE); near(frame.main[0], .25, .000001);
    config.master.send_db = 0; config.channels[0].send_db = 0;
    config.channels[0].send_channel = 2; config.channels[2].gain_db = -6.020599913279624;
    config.channels[31].source_deck = 1; frame.sources[2] = frame.sources[3] = .1f;
    configure(dsp, &config); process(dsp, &frame, FALSE, FALSE); near(frame.main[0], .525, .000001);
    config.channels[0].source_deck = 3; frame.sources[6] = frame.sources[7] = .05f;
    configure(dsp, &config);
    for (guint sample = 0; sample < 10000; ++sample) {
        process(dsp, &frame, FALSE, FALSE); near(frame.main[0], .5625, .000001);
    }
    frame.microphone[0] = frame.microphone[1] = .02f;
    frame.sources[RADIOS_AUDIO_MASTER_SAMPLER * 2] = frame.sources[RADIOS_AUDIO_MASTER_SAMPLER * 2 + 1] = .01f;
    gfloat external[2] = {.04f, .04f};
    /* The opening ramp reaches the full microphone level within GATE_RAMP frames. */
    for (guint sample = 0; sample < GATE_RAMP; ++sample)
        radios_audio_dsp_process_frame_with_external(dsp, frame.sources, frame.routed, frame.microphone, TRUE,
            external, FALSE, frame.main, frame.cue);
    near(frame.main[0], .685, .000001);
    config.master.mute = TRUE; configure(dsp, &config); process(dsp, &frame, FALSE, FALSE);
    near(frame.main[0], 0, .000001);
    config.master.mute = FALSE; config.master.send_channel = -1; config.channels[0].send_channel = -1;
    configure(dsp, &config); process(dsp, &frame, FALSE, FALSE); near(frame.main[0], .335, .000001);
    radios_audio_dsp_free(dsp);
}
static void master_routes_speech_ducking(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    config.channels[0].source_deck = -1; config.channels[7].source_deck = 0;
    config.channels[0].gain_db = -6.020599913279624; config.channels[0].send_db = 0;
    config.master.send_channel = config.master.return_channel = 0; config.master.send_db = 0;
    config.speech_ducking = TRUE; config.speech_bed_db = -20; configure(dsp, &config);
    Frame frame; frame_init(&frame); frame.sources[0] = frame.sources[1] = .2f;
    frame.microphone[0] = frame.microphone[1] = .02f;
    frame.sources[RADIOS_AUDIO_MASTER_SAMPLER * 2] = frame.sources[RADIOS_AUDIO_MASTER_SAMPLER * 2 + 1] = .01f;
    gfloat external[2] = {.04f, .04f};
    for (guint sample = 0; sample < 10000; ++sample)
        radios_audio_dsp_process_frame_with_external(dsp, frame.sources, frame.routed, frame.microphone, TRUE,
            external, FALSE, frame.main, frame.cue);
    near(frame.main[0], .081, .000001); near(frame.main[1], .081, .000001);
    for (guint sample = 0; sample < 60000; ++sample)
        radios_audio_dsp_process_frame_with_external(dsp, frame.sources, frame.routed, frame.microphone, FALSE,
            external, FALSE, frame.main, frame.cue);
    near(frame.main[0], .375, .000001);
    radios_audio_dsp_free(dsp);
}
static void master_route_validation(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    config.master.send_channel = config.master.return_channel = 31; configure(dsp, &config);
    RadioSAudioMixerConfig valid = config;
    for (guint failure = 0; failure < 6; ++failure) {
        config = valid;
        switch (failure) {
        case 0: config.master.send_channel = 32; break;
        case 1: config.master.return_channel = -2; break;
        case 2: config.master.send_db = NAN; break;
        case 3: config.master.return_db = INFINITY; break;
        case 4: config.master.send_db = -61; break;
        case 5: config.master.return_db = 7; break;
        }
        GError *error = NULL;
        g_assert_false(radios_audio_dsp_configure(dsp, &config, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
        RadioSAudioMixerConfig current; radios_audio_dsp_get_config(dsp, &current);
        g_assert_cmpint(current.master.send_channel, ==, 31); g_assert_cmpint(current.master.return_channel, ==, 31);
        near(current.master.send_db, -6, .000001); near(current.master.return_db, 0, .000001);
    }
    radios_audio_dsp_free(dsp);
}
static void routing_validation(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    config.channels[0].send_channel = 1; config.channels[1].return_channel = 0; configure(dsp, &config);
    RadioSAudioMixerConfig valid = config;
    config.channels[1].send_channel = 0; GError *error = NULL;
    g_assert_false(radios_audio_dsp_configure(dsp, &config, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    radios_audio_dsp_get_config(dsp, &config); g_assert_cmpint(config.channels[1].send_channel, ==, -1);
    config.channels[2].return_channel = 1; config.channels[2].send_channel = 0;
    g_assert_false(radios_audio_mixer_config_validate(&config, &error)); g_clear_error(&error);
    config = valid; config.channels[0].send_channel = 0;
    g_assert_false(radios_audio_mixer_config_validate(&config, &error)); g_clear_error(&error);
    config = valid; config.channels[0].return_channel = 32;
    g_assert_false(radios_audio_mixer_config_validate(&config, &error)); g_clear_error(&error);
    config = valid; config.channels[0].send_db = NAN;
    g_assert_false(radios_audio_mixer_config_validate(&config, &error)); g_clear_error(&error);
    config = valid; config.channels[0].return_db = 7;
    g_assert_false(radios_audio_mixer_config_validate(&config, &error)); g_clear_error(&error);
    radios_audio_dsp_free(dsp);
}

static void routed_microphone_gate(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    config.channels[7].source_deck = -2; config.channels[7].send_channel = 1; config.channels[7].send_db = 0;
    config.channels[1].fx_slot = 0; config.channels[1].fx_return_db = 0;
    config.effects[0].enabled = TRUE; config.effects[0].depth = 1; config.effects[0].return_db = 0;
    config.speech_ducking = TRUE; config.speech_bed_db = -20; configure(dsp, &config);
    Frame frame; frame_init(&frame); frame.sources[0] = frame.sources[1] = .5f;
    frame.microphone[0] = frame.microphone[1] = .2f;
    for (guint sample = 0; sample < 6000; ++sample) process(dsp, &frame, TRUE, FALSE);
    near(frame.main[0], .45, .0001);
    /* The routed voice only fades during the closing ramp; afterwards MAIN is
     * exactly the ducked music, channel 1's delay included. */
    RadioSAudioDspMeters meters; gdouble previous = 1;
    for (guint sample = 0; sample < GATE_RAMP + 24000; ++sample) {
        process(dsp, &frame, FALSE, FALSE); radios_audio_dsp_get_meters(dsp, &meters);
        gdouble voice = frame.main[0] - .5 * meters.speech_gain;
        if (sample < GATE_RAMP) {
            g_assert_cmpfloat(voice, <=, previous + .000001); g_assert_cmpfloat(voice, >=, -.000001); previous = voice;
        } else near(frame.main[0], .5 * meters.speech_gain, .000001);
    }
    near(frame.main[0], .5, .0001); radios_audio_dsp_free(dsp);
}

static void engine_contract(void)
{
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioMixerConfig config; radios_audio_engine_get_mixer(engine, &config);
    config.channels[0].source_deck = 1; config.channels[0].pfl = TRUE; config.effects[0].type = RADIOS_FX_ROBOT;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &config, NULL)); RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpint(snapshot.mixer.channels[0].source_deck, ==, 1); g_assert_cmpint(snapshot.mixer.effects[0].type, ==, RADIOS_FX_ROBOT);
    GError *error = NULL; config.effects[4].depth = INFINITY; g_assert_false(radios_audio_engine_apply_mixer(engine, &config, &error)); g_clear_error(&error);
    radios_audio_engine_snapshot(engine, &snapshot); near(snapshot.mixer.effects[4].depth, .5, .000001);
    g_assert_true(radios_audio_engine_set_crossfader(engine, -1, NULL)); g_assert_true(radios_audio_engine_set_ducking(engine, FALSE, -20, NULL));
    radios_audio_engine_get_mixer(engine, &config); near(config.crossfader, -1, .000001); g_assert_false(config.speech_ducking);
    radios_audio_engine_unref(engine);
}
static void inactive_solo_does_not_silence_players(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    Frame frame; frame_init(&frame); gboolean active[RADIOS_AUDIO_DECKS] = {FALSE}; active[0] = TRUE;
    frame.sources[0] = .3f; config.channels[1].source_deck = 1; config.channels[1].solo = TRUE;
    radios_audio_dsp_set_source_activity(dsp, active);
    for (guint variant = 0; variant < 4; ++variant) {
        config.channels[1].source_deck = variant == 2 ? -1 : 1;
        config.channels[1].mute = variant == 0; config.channels[1].gain_db = variant == 1 ? -60 : 0;
        configure(dsp, &config);
        for (guint n = 0; n < 300; ++n) process(dsp, &frame, FALSE, FALSE);
        near(frame.main[0], .3, .000001); g_assert_cmpuint(radios_audio_dsp_contributing_solo_mask(dsp), ==, 0);
    }
    config.channels[1].source_deck = 1; configure(dsp, &config); active[1] = TRUE;
    radios_audio_dsp_set_source_activity(dsp, active); frame.sources[2] = .2f;
    for (guint n = 0; n < 300; ++n) process(dsp, &frame, FALSE, FALSE);
    near(frame.main[0], .2, .000001); g_assert_cmpuint(radios_audio_dsp_contributing_solo_mask(dsp), ==, 2);
    /* A real solo stays in force on a decoded zero crossing. */
    frame.sources[2] = 0; process(dsp, &frame, FALSE, FALSE); near(frame.main[0], 0, .000001);
    config.channels[1].mute = TRUE; configure(dsp, &config); process(dsp, &frame, FALSE, FALSE); near(frame.main[0], .3, .000001);
    radios_audio_dsp_free(dsp);
}

/* LUFS trim belongs to the direct player input. A routed channel, microphone
 * or jingle entering the same strip must never inherit a second correction. */
static void channel_loudness(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    config.channels[0].pfl = TRUE; config.channels[0].send_channel = 1; config.channels[0].send_db = 0;
    config.channels[1].return_db = 0; configure(dsp, &config);
    radios_audio_dsp_set_channel_loudness(dsp, 0, -6.020599913279624, TRUE);
    radios_audio_dsp_set_channel_loudness(dsp, 1, -6.020599913279624, TRUE);
    Frame frame; frame_init(&frame); frame.sources[0] = .2f; frame.sources[RADIOS_AUDIO_SAMPLER_BASE * 2] = .05f;
    for (guint i = 0; i < 240; ++i) process(dsp, &frame, FALSE, TRUE);
    near(frame.main[0], .3, .000001); near(frame.cue[0], .15, .000001);
    RadioSAudioDspMeters meters; radios_audio_dsp_get_meters(dsp, &meters);
    near(meters.channel_loudness_gain_db[0], -6.020599913279624, .000001); g_assert_true(meters.channel_loudness_active[0]);
    g_assert_false(meters.channel_loudness_active[1]);
    config.channels[0].loudness_bypass = TRUE; configure(dsp, &config); process(dsp, &frame, FALSE, TRUE);
    near(frame.main[0], .5, .000001); near(frame.cue[0], .25, .000001);
    radios_audio_dsp_get_meters(dsp, &meters); g_assert_false(meters.channel_loudness_active[0]);
    config.channels[0].loudness_bypass = FALSE; config.channels[0].source_deck = -2;
    configure(dsp, &config); frame.sources[0] = 0; frame.microphone[0] = .2f;
    for (guint i = 0; i < GATE_RAMP; ++i) process(dsp, &frame, TRUE, TRUE); /* opening ramp */
    near(frame.main[0], .5, .000001);
    radios_audio_dsp_free(dsp);
}

static void auto_fader_residual_duck_and_manual_restore(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    config.channels[0].gain_db = -18; configure(dsp, &config);
    radios_audio_dsp_set_channel_auto_gain(dsp, 0, -18, -6, TRUE);
    Frame frame; frame_init(&frame); frame.sources[0] = .2f;
    for (guint i = 0; i < 240; ++i) process(dsp, &frame, FALSE, FALSE);
    gdouble factor = pow(10, -6.0 / 20); near(frame.main[0], .2 * factor, .000001);
    RadioSAudioMixerConfig actual; radios_audio_dsp_get_config(dsp, &actual); near(actual.channels[0].gain_db, -6, .000001);
    RadioSAudioDspMeters meters; radios_audio_dsp_get_meters(dsp, &meters);
    near(meters.channel_loudness_gain_db[0], -6, .000001); g_assert_true(meters.channel_loudness_active[0]);
    config = actual; config.speech_ducking = TRUE; config.speech_bed_db = -6; configure(dsp, &config);
    frame.microphone[0] = .1f;
    for (guint i = 0; i < 4000; ++i) process(dsp, &frame, TRUE, FALSE);
    radios_audio_dsp_get_meters(dsp, &meters); near(meters.speech_gain, factor, .000001);
    near(frame.main[0], .2 * factor * factor + .1, .000001);
    /* The positive range checks have no residual speech-release envelope. */
    radios_audio_dsp_free(dsp); dsp = radios_audio_dsp_new();
    config = actual; config.speech_ducking = FALSE; configure(dsp, &config); frame.microphone[0] = 0;
    radios_audio_dsp_set_channel_auto_gain(dsp, 0, 6, 11, TRUE);
    for (guint i = 0; i < 240; ++i) process(dsp, &frame, FALSE, FALSE);
    near(frame.main[0], .2 * pow(10, 11.0 / 20), .000001); g_assert_cmpfloat(frame.main[0], <, 1);
    radios_audio_dsp_get_config(dsp, &actual); near(actual.channels[0].gain_db, 6, .000001);
    radios_audio_dsp_get_meters(dsp, &meters); near(meters.channel_loudness_gain_db[0], 11, .000001);
    radios_audio_dsp_set_channel_auto_gain(dsp, 0, -12, 11, FALSE);
    for (guint i = 0; i < 240; ++i) process(dsp, &frame, FALSE, FALSE);
    near(frame.main[0], .2 * pow(10, -12.0 / 20), .000001);
    radios_audio_dsp_get_config(dsp, &actual); near(actual.channels[0].gain_db, -12, .000001);
    radios_audio_dsp_get_meters(dsp, &meters); g_assert_false(meters.channel_loudness_active[0]);
    radios_audio_dsp_free(dsp);
}

/* L (latch) and R (push-to-talk) while a song plays (PDF p.8, p.15): latch on
 * for 100 ms, ten 10 ms taps, then on/off again inside the echo tail after
 * the song stops at frame 28000 (a zero crossing of both test tones). */
static gboolean gate_schedule(guint frame)
{
    if (frame < 9600) return FALSE;
    if (frame < 14400) return TRUE;
    if (frame < 16800) return FALSE;
    if (frame < 21600) return (frame - 16800) / 480 % 2 == 0;
    if (frame < 30000) return FALSE;
    if (frame < 32400) return TRUE;
    if (frame < 33000) return FALSE;
    if (frame < 40000) return frame / 1200 % 2 == 1;
    return FALSE;
}
static gfloat programme(guint frame, gdouble frequency, gdouble level)
{ return frame < 28000 ? (gfloat)(level * sin(2 * G_PI * frequency * frame / 48000)) : 0; }
static void programme_master_chain(RadioSAudioMixerConfig *config)
{
    simple_config(config);
    config->master.eq_low_db = 6; config->master.eq_mid_db = -6; config->master.eq_high_db = 4;
    config->master_fx.type = RADIOS_FX_ECHO; config->master_fx.enabled = TRUE; config->master_fx.depth = .6;
    config->master_fx.return_db = config->master.fx_return_db = 0;
}
/* The gate must leave the programme alone: master EQ filters, the MASTER
 * inline echo, the legacy MASTER aux, channel effects, a microphone strip's
 * own sampler music (EQ + echo), an explicit RETURN's EQ/PFL and a MASTER
 * sampler jingle in the explicit MASTER aux reverb. Compared with a DSP that
 * never sees the gate, MAIN and CUE stay identical sample by sample, so the
 * song has no step and its effect tails decay as designed. */
static void gate_keeps_programme_histories(void)
{
    for (guint ramp = 0; ramp <= 240; ramp += 240) for (guint variant = 0; variant < 4; ++variant) {
        RadioSAudioDsp *dsp = radios_audio_dsp_new(), *reference = radios_audio_dsp_new();
        radios_audio_dsp_set_microphone_ramp(dsp, ramp); radios_audio_dsp_set_microphone_ramp(reference, ramp);
        RadioSAudioMixerConfig config; programme_master_chain(&config);
        if (variant == 1) {
            config.channels[0].fx_slot = 0; config.channels[0].fx_return_db = 0;
            config.effects[0].type = RADIOS_FX_REVERB; config.effects[0].enabled = TRUE; config.effects[0].return_db = 0;
            config.channels[5].source_deck = -2; config.channels[5].eq_low_db = -6;
            config.channels[5].fx_slot = 1; config.channels[5].fx_return_db = 0;
            config.effects[1].type = RADIOS_FX_ECHO; config.effects[1].enabled = TRUE; config.effects[1].return_db = 0;
            config.master.fx_slot = 2; config.effects[2].type = RADIOS_FX_ECHO; config.effects[2].enabled = TRUE;
            config.effects[2].depth = .4; config.effects[2].return_db = 0;
        } else if (variant == 2) {
            for (guint i = 0; i < 32; ++i) config.channels[i].return_effect = -1;
            config.master.return_effect = -1;
            config.channels[0].fx_slot = 0; config.effects[0].type = RADIOS_FX_ECHO; config.effects[0].enabled = TRUE;
            config.effects[0].depth = .7; config.effects[0].return_db = 0;
            config.channels[1].return_effect = 0; config.channels[1].eq_low_db = 4; config.channels[1].eq_high_db = -3;
            config.channels[1].pfl = TRUE;
        } else if (variant == 3) {
            for (guint i = 0; i < 32; ++i) config.channels[i].return_effect = -1;
            config.master.fx_slot = config.master.return_effect = 3;
            config.effects[3].type = RADIOS_FX_REVERB; config.effects[3].enabled = TRUE;
            config.effects[3].depth = .6; config.effects[3].return_db = 0;
        }
        configure(dsp, &config); configure(reference, &config);
        Frame frame, expected; frame_init(&frame); frame_init(&expected);
        const guint sampler = (RADIOS_AUDIO_SAMPLER_BASE + 5) * 2, jingle = RADIOS_AUDIO_MASTER_SAMPLER * 2;
        gdouble deviation = 0, step = 0, reference_step = 0, tail = 0, reference_tail = 0; guint worst = 0;
        for (guint i = 0; i < 48000; ++i) {
            frame.sources[0] = expected.sources[0] = programme(i, 240, .3);
            frame.sources[sampler] = expected.sources[sampler] = variant == 1 ? programme(i, 360, .1) : 0;
            frame.sources[jingle] = frame.sources[jingle + 1] = expected.sources[jingle] = expected.sources[jingle + 1] =
                variant == 3 ? programme(i, 360, .15) : 0;
            gdouble before = frame.main[0], reference_before = expected.main[0];
            process(dsp, &frame, gate_schedule(i), TRUE); process(reference, &expected, FALSE, TRUE);
            for (guint c = 0; c < 2; ++c) {
                gdouble difference = MAX(fabs(frame.main[c] - expected.main[c]), fabs(frame.cue[c] - expected.cue[c]));
                if (difference > deviation) { deviation = difference; worst = i; }
            }
            if (i) { step = MAX(step, fabs(frame.main[0] - before)); reference_step = MAX(reference_step, fabs(expected.main[0] - reference_before)); }
            if (i >= 30000) { tail += frame.main[0] * frame.main[0]; reference_tail += expected.main[0] * expected.main[0]; }
        }
        g_test_message("ramp %u variant %u: MAIN/CUE deviation %.6f (frame %u); largest step %.6f, gate-free %.6f; tail energy %.3f, gate-free %.3f",
            ramp, variant, deviation, worst, step, reference_step, tail, reference_tail);
        g_assert_cmpfloat(reference_tail, >, 100);
        g_assert_cmpfloat(deviation, <, .000001);
        g_assert_cmpfloat(step, <=, reference_step + .000001);
        g_assert_cmpfloat(fabs(tail - reference_tail), <=, reference_tail * .000001);
        radios_audio_dsp_free(dsp); radios_audio_dsp_free(reference);
    }
}
/* With the engine's 5 ms gate ramp, opening the microphone ducks the song only
 * through the designed one-pole envelope and the microphone fades in and out.
 * Superposition proves it: MAIN minus the same microphone alone equals the
 * song multiplied by the reported duck gain on a DSP that never sees a gate. */
static void gate_ramp_ducks_programme_smoothly(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(), *voice = radios_audio_dsp_new(), *ducked = radios_audio_dsp_new(), *plain = radios_audio_dsp_new();
    RadioSAudioDsp *all[] = {dsp, voice, ducked, plain};
    RadioSAudioMixerConfig config; programme_master_chain(&config); config.speech_ducking = TRUE; config.speech_bed_db = -12;
    for (guint i = 0; i < G_N_ELEMENTS(all); ++i) { radios_audio_dsp_set_microphone_ramp(all[i], 240); configure(all[i], &config); }
    Frame full, spoken, song, dry; frame_init(&full); frame_init(&spoken); frame_init(&song); frame_init(&dry);
    full.microphone[0] = full.microphone[1] = spoken.microphone[0] = spoken.microphone[1] = .05f;
    const gdouble bed = pow(10, -12.0 / 20);
    gdouble previous_gain = 1, gain_step = 0, deviation = 0, voice_step = 0, step = 0, dry_step = 0;
    for (guint i = 0; i < 48000; ++i) {
        gboolean gate = gate_schedule(i);
        gdouble before[] = {full.main[0], spoken.main[0], dry.main[0]};
        full.sources[0] = dry.sources[0] = programme(i, 240, .3);
        process(dsp, &full, gate, FALSE);
        RadioSAudioDspMeters meters; radios_audio_dsp_get_meters(dsp, &meters);
        process(voice, &spoken, gate, FALSE);
        song.sources[0] = (gfloat)(programme(i, 240, .3) * meters.speech_gain); process(ducked, &song, FALSE, FALSE);
        process(plain, &dry, FALSE, FALSE);
        /* Attack while the gate is open, release while it is closed; never a jump. */
        if (gate) g_assert_cmpfloat(meters.speech_gain, <=, previous_gain + 1e-12);
        else g_assert_cmpfloat(meters.speech_gain, >=, previous_gain - 1e-12);
        gain_step = MAX(gain_step, fabs(meters.speech_gain - previous_gain)); previous_gain = meters.speech_gain;
        if (i == 14399) near(meters.speech_gain, bed, .001);
        if (i == 29999) g_assert_cmpfloat(meters.speech_gain, >, .95);
        for (guint c = 0; c < 2; ++c) deviation = MAX(deviation, fabs(full.main[c] - spoken.main[c] - song.main[c]));
        if (i) {
            step = MAX(step, fabs(full.main[0] - before[0])); voice_step = MAX(voice_step, fabs(spoken.main[0] - before[1]));
            dry_step = MAX(dry_step, fabs(dry.main[0] - before[2]));
        }
    }
    g_test_message("song deviation from ducked reference %.7f; duck step %.6f (bound %.6f); microphone step %.6f; MAIN step %.6f, song alone %.6f",
        deviation, gain_step, .004 * (1 - bed), voice_step, step, dry_step);
    g_assert_cmpfloat(deviation, <, .000002);
    g_assert_cmpfloat(gain_step, <=, .004 * (1 - bed) + 1e-12);
    g_assert_cmpfloat(voice_step, <, .002);
    g_assert_cmpfloat(step, <=, dry_step + .002);
    for (guint i = 0; i < G_N_ELEMENTS(all); ++i) radios_audio_dsp_free(all[i]);
}
/* The microphone's own effects (strip echo, speech SEND echo) fade with the
 * closing ramp and are cleared while silent: a closed microphone leaves no
 * tail, and a reopened one starts fresh, exactly like a DSP whose microphone
 * opens for the first time. The song's own echo runs on untouched. */
static void gate_ramp_fades_and_clears_voice_effects(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(), *fresh = radios_audio_dsp_new();
    RadioSAudioMixerConfig config; simple_config(&config);
    for (guint i = 0; i < 32; ++i) config.channels[i].return_effect = -1;
    config.channels[0].fx_slot = 2; config.channels[2].return_effect = 2;
    config.channels[3].source_deck = -2; config.channels[3].fx_slot = 0; config.channels[4].return_effect = 0;
    config.speech_fx_slot = 1; config.channels[5].return_effect = 1;
    for (guint slot = 0; slot < 3; ++slot) {
        config.effects[slot].type = RADIOS_FX_ECHO; config.effects[slot].enabled = TRUE;
        config.effects[slot].depth = .8; config.effects[slot].return_db = 0;
    }
    radios_audio_dsp_set_microphone_ramp(dsp, 240); radios_audio_dsp_set_microphone_ramp(fresh, 240);
    configure(dsp, &config); configure(fresh, &config);
    Frame frame, expected; frame_init(&frame); frame_init(&expected);
    frame.microphone[0] = frame.microphone[1] = expected.microphone[0] = expected.microphone[1] = .2f;
    gdouble voice_peak = 0, voice_step = 0, previous = 0, deviation = 0;
    for (guint i = 0; i < 72000; ++i) {
        frame.sources[0] = expected.sources[0] = (gfloat)(.2 * sin(2 * G_PI * 240 * i / 48000));
        process(dsp, &frame, (i >= 2400 && i < 26400) || i >= 50400, FALSE); process(fresh, &expected, i >= 50400, FALSE);
        gdouble spoken = frame.main[0] - expected.main[0];
        if (i < 26640) { voice_peak = MAX(voice_peak, fabs(spoken)); if (i) voice_step = MAX(voice_step, fabs(spoken - previous)); }
        else deviation = MAX(deviation, MAX(fabs(frame.main[0] - expected.main[0]), fabs(frame.main[1] - expected.main[1])));
        previous = spoken;
    }
    g_test_message("voice peak %.4f, largest voice step %.6f (linear 240-frame fade %.6f); after close/reopen deviation %.7f",
        voice_peak, voice_step, voice_peak / 240, deviation);
    g_assert_cmpfloat(voice_peak, >, .4);
    g_assert_cmpfloat(voice_step, <=, 2 * voice_peak / 240);
    g_assert_cmpfloat(deviation, <, .000002);
    radios_audio_dsp_free(dsp); radios_audio_dsp_free(fresh);
}
/* An output flush (radios_audio_dsp_reset) inside the closing ramp: the voice
 * that still fades in afterwards must not come back as an echo 250 ms later. */
static void gate_ramp_reset_mid_fade_leaves_no_voice_tail(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; simple_config(&config);
    config.channels[3].source_deck = -2; config.channels[3].fx_slot = 0; config.channels[3].fx_return_db = 0;
    config.effects[0].type = RADIOS_FX_ECHO; config.effects[0].enabled = TRUE; config.effects[0].depth = .8; config.effects[0].return_db = 0;
    radios_audio_dsp_set_microphone_ramp(dsp, 240); configure(dsp, &config);
    Frame frame; frame_init(&frame); frame.microphone[0] = frame.microphone[1] = .2f;
    for (guint i = 0; i < 2000; ++i) process(dsp, &frame, TRUE, FALSE);
    near(frame.main[0], .2, .000001);
    gdouble tail = 0;
    for (guint i = 0; i < 30000; ++i) {
        if (i == 100) radios_audio_dsp_reset(dsp);
        process(dsp, &frame, FALSE, FALSE);
        if (i >= 240) tail = MAX(tail, MAX(fabs(frame.main[0]), fabs(frame.main[1])));
    }
    g_test_message("largest voice residue after the closing ramp %.9f", tail);
    g_assert_cmpfloat(tail, <, 1e-9);
    radios_audio_dsp_free(dsp);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/dsp/inactive-muted-empty-solo-preserves-live-players", inactive_solo_does_not_silence_players);
    g_test_add_func("/dsp/validation-atomic", validation);
    g_test_add_func("/dsp/mixer-loudness-player-only-bypass-routes", channel_loudness);
    g_test_add_func("/dsp/auto-fader-residual-duck-once-manual-restore", auto_fader_residual_duck_and_manual_restore);
    g_test_add_func("/dsp/routing-sampler-pfl-solo", routing);
    g_test_add_func("/dsp/pan-gain-equal-power", pan_gain_crossfade);
    g_test_add_func("/dsp/eq-frequency-response", equalizer);
    g_test_add_func("/dsp/all-14-effects", all_effects);
    g_test_add_func("/dsp/send-return", send_return);
    g_test_add_func("/dsp/five-slots-master", five_slots_master);
    g_test_add_func("/dsp/master-aux-routing", master_aux);
    g_test_add_func("/dsp/master-channel-send-return", master_channel_routing);
    g_test_add_func("/dsp/master-channel-speech-ducking", master_routes_speech_ducking);
    g_test_add_func("/dsp/master-channel-validation-atomic", master_route_validation);
    g_test_add_func("/dsp/speech-duck-and-mic-gate", ducking);
    g_test_add_func("/dsp/external-master-eq-fx-mute-duck", external_master);
    g_test_add_func("/dsp/transition-sample-envelope", transition);
    g_test_add_func("/dsp/engine-config-contract", engine_contract);
    g_test_add_func("/dsp/channel-send-return-chain", channel_send_return);
    g_test_add_func("/dsp/channel-routing-feedback-atomic", routing_validation);
    g_test_add_func("/dsp/routed-microphone-gate-and-duck", routed_microphone_gate);
    g_test_add_func("/dsp/click-free-controls", click_free_controls);
    g_test_add_func("/dsp/roll-algorithm-distinction", roll_distinction);
    g_test_add_func("/dsp/mic-gate-keeps-music-eq-fx-tails", gate_keeps_programme_histories);
    g_test_add_func("/dsp/mic-gate-ramp-ducks-music-smoothly", gate_ramp_ducks_programme_smoothly);
    g_test_add_func("/dsp/mic-gate-ramp-voice-effects-fade-clear", gate_ramp_fades_and_clears_voice_effects);
    g_test_add_func("/dsp/mic-gate-ramp-reset-mid-fade-no-voice-tail", gate_ramp_reset_mid_fade_leaves_no_voice_tail);
    return g_test_run();
}
