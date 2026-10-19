#include "radios/audio_dsp.h"
#include <math.h>
#include <string.h>

typedef struct {
    gfloat sources[RADIOS_AUDIO_DECKS * 2], microphone[2], main[2], cue[2];
    gboolean routed[RADIOS_AUDIO_DECKS];
} Frame;
static void near(gdouble actual, gdouble expected)
{ g_assert_cmpfloat(fabs(actual - expected), <, .000002); }
static void frame_init(Frame *frame)
{
    memset(frame, 0, sizeof *frame);
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) frame->routed[i] = i != 32;
}
static void config_init(RadioSAudioMixerConfig *config)
{
    radios_audio_mixer_config_default(config); config->speech_ducking = FALSE;
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i) {
        config->channels[i].source_deck = -1; config->channels[i].return_effect = -1;
    }
    config->master.return_effect = -1;
    for (guint i = 0; i < RADIOS_AUDIO_FX_SLOTS; ++i) {
        config->effects[i].type = RADIOS_FX_FILTER; config->effects[i].enabled = TRUE;
        config->effects[i].depth = 1; config->effects[i].return_db = 0;
    }
}
static void process(RadioSAudioDsp *dsp, Frame *frame, gboolean gate, gboolean cue)
{ radios_audio_dsp_process_frame(dsp, frame->sources, frame->routed, frame->microphone, gate, cue, frame->main, frame->cue); }
static void configure(RadioSAudioDsp *dsp, const RadioSAudioMixerConfig *config)
{
    GError *error = NULL; g_assert_true(radios_audio_dsp_configure(dsp, config, &error)); g_assert_no_error(error);
    Frame frame; frame_init(&frame);
    for (guint i = 0; i < 240; ++i) process(dsp, &frame, FALSE, FALSE);
}
static void settle(RadioSAudioDsp *dsp, Frame *frame)
{ for (guint i = 0; i < 3000; ++i) process(dsp, frame, FALSE, TRUE); }
/* Since 18.10.2026 the DSP ramps the microphone gate over 240 frames (5 ms)
 * by default, so L/R never click on air (PDF p.8, p.15). Closing, the
 * microphone and its own effects fade out and are completely silent after at
 * most GATE_RAMP frames; opening reaches full level within GATE_RAMP frames
 * (every opening below is checked only after speech_settle). Expectations
 * that held on the first closed frame now hold once this ramp has run. */
#define GATE_RAMP 240u
static void gate_close_ramp(RadioSAudioDsp *dsp, Frame *frame, gboolean cue)
{ for (guint i = 0; i < GATE_RAMP; ++i) process(dsp, frame, FALSE, cue); }
static void validation(void)
{
    RadioSAudioMixerConfig config; radios_audio_mixer_config_default(&config);
    g_assert_cmpint(config.channels[0].return_effect, ==, -2); g_assert_cmpint(config.master.return_effect, ==, -2);
    RadioSAudioDsp *dsp = radios_audio_dsp_new();
    for (gint value = -2; value < (gint)RADIOS_AUDIO_FX_SLOTS; ++value) {
        config.channels[31].return_effect = config.master.return_effect = value;
        g_assert_true(radios_audio_dsp_configure(dsp, &config, NULL));
    }
    GError *error = NULL;
    for (guint i = 0; i < 4; ++i) {
        RadioSAudioMixerConfig invalid = config;
        if (i < 2) invalid.channels[0].return_effect = i == 0 ? -3 : 5;
        else invalid.master.return_effect = i == 2 ? -3 : 5;
        g_assert_false(radios_audio_dsp_configure(dsp, &invalid, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
        RadioSAudioMixerConfig current; radios_audio_dsp_get_config(dsp, &current);
        g_assert_cmpint(current.channels[0].return_effect, ==, -2); g_assert_cmpint(current.master.return_effect, ==, 4);
    }
    radios_audio_dsp_free(dsp);
}
static void cross_source_and_independent_slots(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; config_init(&config);
    config.channels[31].source_deck = 0; config.channels[31].fx_slot = 0; config.channels[31].fx_return_db = 6;
    config.channels[30].source_deck = 1; config.channels[30].fx_slot = 1;
    config.channels[0].return_effect = 1; configure(dsp, &config);
    Frame frame; frame_init(&frame); frame.sources[0] = .1f; frame.sources[2] = .2f;
    settle(dsp, &frame); near(frame.main[0], .5);
    config.channels[0].return_effect = 0; configure(dsp, &config); settle(dsp, &frame); near(frame.main[0], .4);
    config.channels[0].return_effect = -1; configure(dsp, &config); settle(dsp, &frame); near(frame.main[0], .3);
    config.channels[0].return_effect = 4; config.channels[31].fx_slot = 4;
    configure(dsp, &config); settle(dsp, &frame); near(frame.main[0], .4);
    config.channels[0].return_db = -6.020599913279624;
    configure(dsp, &config); settle(dsp, &frame); near(frame.main[0], .35);
    radios_audio_dsp_free(dsp);
}
static void same_source_and_no_feedback(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; config_init(&config);
    config.channels[31].source_deck = 0; config.channels[31].fx_slot = 0; config.channels[31].return_effect = 0;
    configure(dsp, &config); Frame frame; frame_init(&frame); frame.sources[0] = .1f;
    settle(dsp, &frame); near(frame.main[0], .2);
    config.channels[31].return_effect = -1; config.channels[0].return_effect = 0; config.channels[0].fx_slot = 0;
    configure(dsp, &config); settle(dsp, &frame); near(frame.main[0], .2);
    config.master.fx_slot = 0; configure(dsp, &config); settle(dsp, &frame); near(frame.main[0], .3);
    for (guint i = 0; i < 24000; ++i) { process(dsp, &frame, FALSE, TRUE); near(frame.main[0], .3); }
    radios_audio_dsp_begin_block(dsp); process(dsp, &frame, FALSE, TRUE);
    RadioSAudioDspMeters meters; radios_audio_dsp_get_meters(dsp, &meters);
    near(meters.channel_fx_send_peak[31][0], .1); near(meters.channel_fx_send_peak[0][0], 0);
    near(meters.master_fx_send_peak[0], .1); near(meters.channel_peak[0][0], .2);
    /* Only wet echoes remain. Neither the empty receiver nor MASTER sends
     * these returned tails back into the effect even while its output is live. */
    config.effects[0].type = RADIOS_FX_ECHO; configure(dsp, &config); radios_audio_dsp_reset(dsp);
    process(dsp, &frame, FALSE, TRUE); frame.sources[0] = 0;
    for (guint i = 1; i < 12000; ++i) process(dsp, &frame, FALSE, TRUE);
    radios_audio_dsp_begin_block(dsp); process(dsp, &frame, FALSE, TRUE);
    radios_audio_dsp_get_meters(dsp, &meters); near(frame.main[0], .2); near(meters.channel_peak[0][0], .2);
    near(meters.channel_fx_send_peak[31][0], 0); near(meters.channel_fx_send_peak[0][0], 0); near(meters.master_fx_send_peak[0], 0);
    radios_audio_dsp_free(dsp);
}
static void monitor_pfl_meter_and_mute(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; config_init(&config);
    config.channels[31].source_deck = 0; config.channels[31].fx_slot = 0;
    config.channels[0].return_effect = 0; config.channels[0].fx_slot = 0; config.channels[0].return_db = -6.020599913279624;
    config.channels[0].gain_db = -6.020599913279624; config.channels[0].pan = 1; config.channels[0].pfl = TRUE;
    config.effects[0].cue = TRUE; configure(dsp, &config);
    Frame frame; frame_init(&frame); frame.sources[0] = .1f; frame.sources[1] = .2f; settle(dsp, &frame);
    radios_audio_dsp_begin_block(dsp); process(dsp, &frame, FALSE, TRUE);
    near(frame.main[0], .1); near(frame.main[1], .25); near(frame.cue[0], .1); near(frame.cue[1], .25);
    RadioSAudioDspMeters meters; radios_audio_dsp_get_meters(dsp, &meters);
    near(meters.fx_send_peak[0][0], .1); near(meters.fx_return_peak[0][0], .1);
    near(meters.channel_fx_send_peak[31][0], .1); near(meters.channel_fx_send_peak[0][0], 0); near(meters.channel_fx_send_peak[0][1], 0);
    near(meters.fx_return_peak[0][1], .2); near(meters.channel_peak[0][0], 0); near(meters.channel_peak[0][1], .05);
    config.channels[0].mute = TRUE; configure(dsp, &config); settle(dsp, &frame);
    near(frame.main[1], .2); near(frame.cue[1], .25);
    config.effects[0].enabled = FALSE; configure(dsp, &config); settle(dsp, &frame);
    near(frame.main[1], .2); near(frame.cue[1], 0);
    config.effects[0].enabled = TRUE; config.channels[31].mute = TRUE; configure(dsp, &config); settle(dsp, &frame);
    near(frame.main[1], 0); near(frame.cue[1], 0);
    config.channels[31].mute = FALSE; config.channels[0].mute = FALSE; config.channels[0].solo = TRUE;
    configure(dsp, &config); settle(dsp, &frame); near(frame.main[1], 0); near(frame.cue[1], 0);
    g_assert_cmpuint(radios_audio_dsp_contributing_solo_mask(dsp), ==, 1);
    config.channels[31].solo = TRUE; configure(dsp, &config); settle(dsp, &frame); near(frame.main[1], .25);
    config.channels[0].solo = FALSE; configure(dsp, &config); settle(dsp, &frame); near(frame.main[1], .2);
    config.channels[31].solo = FALSE; config.channels[0].solo = TRUE; config.effects[0].enabled = FALSE;
    configure(dsp, &config); settle(dsp, &frame); near(frame.main[1], .2);
    g_assert_cmpuint(radios_audio_dsp_contributing_solo_mask(dsp), ==, 0);
    radios_audio_dsp_free(dsp);
}
static void master_return_and_crossfade(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; config_init(&config);
    config.channels[31].source_deck = 0; config.channels[31].fx_slot = 1;
    config.master.return_effect = 1; config.master.return_db = -6.020599913279624;
    configure(dsp, &config); Frame frame; frame_init(&frame); frame.sources[0] = .1f; settle(dsp, &frame); near(frame.main[0], .15);
    config.master.return_effect = -1; config.master.fx_slot = 0; config.channels[0].return_effect = 0;
    configure(dsp, &config); settle(dsp, &frame); near(frame.main[0], .2);
    config.channels[0].crossfade_group = 1; config.crossfader = 1;
    configure(dsp, &config); settle(dsp, &frame); near(frame.main[0], .1);
    config.master.return_effect = 0; config.master.return_db = 0;
    configure(dsp, &config); settle(dsp, &frame); near(frame.main[0], .2);
    config.master.mute = TRUE; configure(dsp, &config); settle(dsp, &frame); near(frame.main[0], 0);
    radios_audio_dsp_free(dsp);
}
static gdouble returned_tone_energy(RadioSAudioDsp *dsp, RadioSAudioMixerConfig *config)
{
    configure(dsp, config); radios_audio_dsp_reset(dsp); Frame frame; frame_init(&frame); gdouble energy = 0;
    for (guint i = 0; i < 18000; ++i) {
        frame.sources[0] = (gfloat)(.1 * sin(2 * G_PI * 1000 * i / 48000)); process(dsp, &frame, FALSE, FALSE);
        if (i >= 6000) energy += (frame.main[0] - frame.sources[0]) * (frame.main[0] - frame.sources[0]);
    }
    return energy;
}
static void recipient_equalizer(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; config_init(&config);
    config.channels[31].source_deck = 0; config.channels[31].fx_slot = 0; config.channels[0].return_effect = 0;
    gdouble full = returned_tone_energy(dsp, &config); config.channels[0].eq_mid_db = -26;
    g_assert_cmpfloat(returned_tone_energy(dsp, &config), <, full / 100); radios_audio_dsp_free(dsp);
}
static void speech_ducking_and_gate_tail(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; config_init(&config);
    config.speech_ducking = TRUE; config.speech_bed_db = -20;
    config.channels[31].source_deck = 0; config.channels[31].fx_slot = 0;
    config.channels[30].source_deck = -2; config.channels[30].fx_slot = 0; config.channels[0].return_effect = 0;
    configure(dsp, &config); Frame frame; frame_init(&frame); frame.sources[0] = .5f; frame.microphone[0] = .1f;
    for (guint i = 0; i < 4000; ++i) process(dsp, &frame, TRUE, FALSE);
    near(frame.main[0], .3); gate_close_ramp(dsp, &frame, FALSE);
    RadioSAudioDspMeters meters; radios_audio_dsp_get_meters(dsp, &meters); near(frame.main[0], meters.speech_gain);
    config.speech_ducking = FALSE; config.effects[0].type = RADIOS_FX_ECHO;
    configure(dsp, &config); radios_audio_dsp_reset(dsp); frame.sources[0] = .1f; frame.microphone[0] = .2f;
    process(dsp, &frame, TRUE, FALSE); frame.sources[0] = 0; frame.microphone[0] = 0;
    gdouble peak = 0;
    for (guint i = 1; i < 18000; ++i) { process(dsp, &frame, FALSE, FALSE); peak = MAX(peak, fabs(frame.main[0])); }
    near(peak, .1); /* Music echo survives; the closed microphone contributes no tail. */
    radios_audio_dsp_free(dsp);
}

static void speech_send_validation(void)
{
    RadioSAudioMixerConfig config; radios_audio_mixer_config_default(&config);
    g_assert_cmpint(config.speech_fx_slot, ==, -1); RadioSAudioDsp *dsp = radios_audio_dsp_new();
    for (gint slot = -1; slot < (gint)RADIOS_AUDIO_FX_SLOTS; ++slot) {
        config.speech_fx_slot = slot; g_assert_true(radios_audio_dsp_configure(dsp, &config, NULL));
    }
    const gint invalid_slots[] = {-2, (gint)RADIOS_AUDIO_FX_SLOTS};
    for (guint i = 0; i < G_N_ELEMENTS(invalid_slots); ++i) {
        RadioSAudioMixerConfig invalid = config; invalid.speech_fx_slot = invalid_slots[i]; GError *error = NULL;
        g_assert_false(radios_audio_dsp_configure(dsp, &invalid, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
        RadioSAudioMixerConfig actual; radios_audio_dsp_get_config(dsp, &actual);
        g_assert_cmpint(actual.speech_fx_slot, ==, config.speech_fx_slot);
    }
    radios_audio_dsp_free(dsp);
}
static void speech_settle(RadioSAudioDsp *dsp, Frame *frame)
{ for (guint i = 0; i < 3000; ++i) process(dsp, frame, TRUE, TRUE); }
static void speech_send_independent_returns(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; config_init(&config);
    config.channels[31].source_deck = 0; config.speech_fx_slot = 0;
    Frame frame; frame_init(&frame); frame.sources[0] = .25f; frame.sources[1] = .125f;
    frame.microphone[0] = .125f; frame.microphone[1] = .0625f;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .375); near(frame.main[1], .1875);
    radios_audio_dsp_begin_block(dsp); process(dsp, &frame, TRUE, TRUE);
    RadioSAudioDspMeters meters; radios_audio_dsp_get_meters(dsp, &meters);
    near(meters.speech_fx_send_peak[0], .125); near(meters.speech_fx_send_peak[1], .0625);
    near(meters.fx_send_peak[0][0], .125); near(meters.fx_return_peak[0][0], .125);
    /* Selecting a different, un-fed return does not add microphone or music. */
    config.master.return_effect = 1; configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .375);
    config.master.return_effect = 0; configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .5);
    config.master.return_effect = -1; config.channels[0].return_effect = 0; config.channels[0].fx_slot = 0;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .5);
    for (guint i = 0; i < 24000; ++i) { process(dsp, &frame, TRUE, TRUE); near(frame.main[0], .5); }
    radios_audio_dsp_begin_block(dsp); process(dsp, &frame, TRUE, TRUE); radios_audio_dsp_get_meters(dsp, &meters);
    near(meters.channel_fx_send_peak[0][0], 0); /* RETURN is never re-sent. */
    config.speech_fx_slot = 3; configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .375);
    config.channels[0].return_effect = 3; configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .5);
    config.effects[3].enabled = FALSE; configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .375);
    radios_audio_dsp_begin_block(dsp); process(dsp, &frame, TRUE, TRUE); radios_audio_dsp_get_meters(dsp, &meters);
    near(meters.speech_fx_send_peak[0], 0);
    config.effects[3].enabled = TRUE; config.speech_fx_slot = -1;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .375);
    gate_close_ramp(dsp, &frame, TRUE); near(frame.main[0], .25); near(frame.main[1], .125);
    radios_audio_dsp_free(dsp);
}
static void speech_send_once_with_channel_assignments(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; config_init(&config);
    config.channels[31].source_deck = 0; config.channels[30].source_deck = -2;
    config.speech_fx_slot = 0; config.master.return_effect = 0;
    Frame frame; frame_init(&frame); frame.sources[0] = .25f; frame.microphone[0] = .125f;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .5);
    /* Two intentional microphone strips duplicate their dry paths, but the
     * independent microphone SEND remains a single .125 contribution. */
    config.channels[29].source_deck = -2; configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .625);
    radios_audio_dsp_begin_block(dsp); process(dsp, &frame, TRUE, TRUE);
    RadioSAudioDspMeters meters; radios_audio_dsp_get_meters(dsp, &meters);
    near(meters.speech_fx_send_peak[0], .125); near(meters.fx_send_peak[0][0], .125);
    /* Existing strip SEND is an additional deliberate route, not a replacement. */
    config.channels[29].source_deck = -1; config.channels[30].fx_slot = 0;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .625);
    radios_audio_dsp_begin_block(dsp); process(dsp, &frame, TRUE, TRUE); radios_audio_dsp_get_meters(dsp, &meters);
    near(meters.speech_fx_send_peak[0], .125); near(meters.fx_send_peak[0][0], .25);
    config.channels[30].fx_slot = -1; config.effects[0].send_db = -6.020599913279624;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .4375);
    config.effects[0].return_db = -6.020599913279624;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .40625);
    config.channels[31].solo = TRUE; configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .25);
    config.channels[30].solo = TRUE; config.channels[31].solo = FALSE;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .15625);
    config.channels[30].mute = TRUE; configure(dsp, &config); speech_settle(dsp, &frame);
    /* A muted SOLO microphone strip cannot suppress the live music and its
     * independent wet return. The saved SOLO selection itself remains set. */
    near(frame.main[0], .28125); g_assert_cmpuint(radios_audio_dsp_contributing_solo_mask(dsp), ==, 0);
    radios_audio_dsp_free(dsp);
}
static void speech_send_gate_route_and_music_tails(void)
{
    /* Every microphone bypass/route transition is checked against a second
     * DSP receiving only the same music impulse. Its echo must remain exact. */
    for (guint transition = 0; transition < 6; ++transition) {
        RadioSAudioDsp *dsp = radios_audio_dsp_new(), *music_dsp = radios_audio_dsp_new();
        RadioSAudioMixerConfig config; config_init(&config);
        config.channels[31].source_deck = 0; config.channels[31].fx_slot = 1;
        config.channels[0].return_effect = 1; config.master.return_effect = 0;
        config.effects[0].type = config.effects[1].type = config.effects[2].type = RADIOS_FX_ECHO;
        config.speech_fx_slot = 0;
        RadioSAudioMixerConfig music_config = config; music_config.speech_fx_slot = -1;
        configure(dsp, &config); configure(music_dsp, &music_config);
        Frame frame, music; frame_init(&frame); frame_init(&music);
        frame.sources[0] = music.sources[0] = .1f; frame.microphone[0] = .2f;
        process(dsp, &frame, TRUE, FALSE); process(music_dsp, &music, TRUE, FALSE);
        frame.sources[0] = music.sources[0] = frame.microphone[0] = 0;
        gboolean gate = TRUE;
        switch (transition) {
        case 0: gate = FALSE; break;
        case 1: config.speech_fx_slot = -1; break;
        case 2: config.speech_fx_slot = 2; config.master.return_effect = 2; break;
        case 3: config.master.mute = TRUE; break;
        case 4: config.effects[0].enabled = FALSE; break;
        case 5: config.channels[30].source_deck = -2; break;
        }
        g_assert_true(radios_audio_dsp_configure(dsp, &config, NULL));
        gdouble peak = 0;
        for (guint i = 1; i < 30000; ++i) {
            if (i == 480) { /* Reopen before the pending music echo arrives. */
                gate = TRUE; config.master.mute = FALSE; config.effects[0].enabled = TRUE;
                if (transition == 1) config.speech_fx_slot = 0;
                g_assert_true(radios_audio_dsp_configure(dsp, &config, NULL));
            }
            process(dsp, &frame, gate, FALSE); process(music_dsp, &music, gate, FALSE);
            near(frame.main[0], music.main[0]); near(frame.main[1], music.main[1]);
            peak = MAX(peak, fabs(frame.main[0]));
        }
        near(peak, .1); radios_audio_dsp_free(dsp); radios_audio_dsp_free(music_dsp);
    }
}
static void speech_send_remains_speech_during_ducking(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; config_init(&config);
    config.channels[31].source_deck = 0; config.speech_fx_slot = 0; config.master.return_effect = 0;
    config.speech_ducking = TRUE; config.speech_bed_db = -20;
    configure(dsp, &config); Frame frame; frame_init(&frame); frame.sources[0] = .5f; frame.microphone[0] = .1f;
    for (guint i = 0; i < 4000; ++i) process(dsp, &frame, TRUE, FALSE);
    near(frame.main[0], .25); /* .05 music bed + .1 dry speech + .1 wet speech. */
    gate_close_ramp(dsp, &frame, FALSE); RadioSAudioDspMeters meters; radios_audio_dsp_get_meters(dsp, &meters);
    near(frame.main[0], .5 * meters.speech_gain);
    for (guint i = 0; i < 24000; ++i) {
        process(dsp, &frame, FALSE, FALSE); radios_audio_dsp_get_meters(dsp, &meters);
        near(frame.main[0], .5 * meters.speech_gain);
    }
    radios_audio_dsp_free(dsp);
}


static void speech_send_legacy_music_history(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(), *baseline = radios_audio_dsp_new();
    RadioSAudioMixerConfig config; radios_audio_mixer_config_default(&config); config.speech_ducking = FALSE;
    config.channels[0].fx_slot = 0; config.channels[0].fx_return_db = 0;
    config.effects[0].enabled = config.effects[1].enabled = TRUE;
    config.effects[0].type = config.effects[1].type = RADIOS_FX_ECHO;
    config.effects[0].depth = config.effects[1].depth = 1;
    config.effects[0].return_db = config.effects[1].return_db = 0;
    config.effects[1].cue = TRUE; config.effects[1].monitor_path = 1;
    configure(dsp, &config); configure(baseline, &config);
    Frame frame, reference; frame_init(&frame); frame_init(&reference);
    frame.sources[0] = reference.sources[0] = .1f;
    process(dsp, &frame, TRUE, TRUE); process(baseline, &reference, TRUE, TRUE);
    frame.sources[0] = reference.sources[0] = 0; gdouble peak = 0;
    for (guint i = 1; i < 30000; ++i) {
        if (i == 1 || i == 2000) {
            config.speech_fx_slot = 1; g_assert_true(radios_audio_dsp_configure(dsp, &config, NULL));
        } else if (i == 1000) {
            config.speech_fx_slot = -1; g_assert_true(radios_audio_dsp_configure(dsp, &config, NULL));
        }
        frame.microphone[0] = reference.microphone[0] = i == 480 ? .2f : 0;
        process(dsp, &frame, TRUE, TRUE); process(baseline, &reference, TRUE, TRUE);
        near(frame.main[0], reference.main[0]); near(frame.main[1], reference.main[1]);
        near(frame.cue[0], reference.cue[0]); /* Bypassed microphone echo never returns. */
        if (i != 480) peak = MAX(peak, fabs(frame.main[0]));
    }
    near(peak, .1); radios_audio_dsp_free(dsp); radios_audio_dsp_free(baseline);
}


static void speech_destination_validation(void)
{
    RadioSAudioMixerConfig config; radios_audio_mixer_config_default(&config);
    g_assert_true(config.speech_to_master); g_assert_cmpint(config.speech_channel, ==, -1);
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); config.speech_to_master = FALSE;
    for (gint channel = -1; channel < (gint)RADIOS_AUDIO_MIXER_CHANNELS; ++channel) {
        config.speech_channel = channel; g_assert_true(radios_audio_dsp_configure(dsp, &config, NULL));
    }
    for (guint kind = 0; kind < 3; ++kind) {
        RadioSAudioMixerConfig invalid = config; GError *error = NULL;
        if (kind == 0) invalid.speech_to_master = 2;
        else invalid.speech_channel = kind == 1 ? -2 : (gint)RADIOS_AUDIO_MIXER_CHANNELS;
        g_assert_false(radios_audio_dsp_configure(dsp, &invalid, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
        RadioSAudioMixerConfig actual; radios_audio_dsp_get_config(dsp, &actual);
        g_assert_false(actual.speech_to_master); g_assert_cmpint(actual.speech_channel, ==, 31);
    }
    radios_audio_dsp_free(dsp);
}
static void speech_destination_legacy_and_exclusive(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; config_init(&config);
    config.channels[31].source_deck = 0; Frame frame; frame_init(&frame);
    frame.sources[0] = .25f; frame.microphone[0] = .125f;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .375);
    config.speech_channel = 30; configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .375);
    config.channels[30].source_deck = -2; config.channels[30].gain_db = -6.020599913279624;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .3125);
    config.channels[29].source_deck = -2; configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .4375);
    /* A selected channel already assigned the microphone still receives it
     * once; other old microphone strips cannot bypass this destination. */
    config.speech_to_master = FALSE; configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .3125);
    config.channels[30].mute = TRUE; configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .25);
    config.channels[30].mute = FALSE; config.speech_to_master = TRUE;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .4375);
    RadioSAudioMixerConfig actual; radios_audio_dsp_get_config(dsp, &actual);
    g_assert_true(actual.speech_to_master); g_assert_cmpint(actual.speech_channel, ==, 30);
    g_assert_cmpint(actual.channels[29].source_deck, ==, -2); g_assert_cmpint(actual.channels[30].source_deck, ==, -2);
    radios_audio_dsp_free(dsp);
}
static void speech_destination_controls_and_gate(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; config_init(&config);
    config.channels[31].source_deck = 0; config.speech_to_master = FALSE;
    Frame frame; frame_init(&frame); frame.sources[0] = frame.sources[1] = .25f;
    frame.microphone[0] = frame.microphone[1] = .125f;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .25); near(frame.main[1], .25);
    config.speech_channel = 0; config.channels[0].gain_db = -6.020599913279624; config.channels[0].pan = 1;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .25); near(frame.main[1], .3125);
    config.channels[0].mute = TRUE; configure(dsp, &config); speech_settle(dsp, &frame);
    near(frame.main[0], .25); near(frame.main[1], .25);
    config.channels[0].mute = FALSE; config.channels[0].solo = TRUE;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], 0); near(frame.main[1], .0625);
    config.channels[0].solo = FALSE; config.channels[31].solo = TRUE;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .25); near(frame.main[1], .25);
    config.channels[31].solo = FALSE; config.channels[0].mute = config.channels[0].pfl = TRUE;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.cue[0], 0); near(frame.cue[1], .0625);
    radios_audio_dsp_begin_block(dsp); process(dsp, &frame, TRUE, TRUE);
    RadioSAudioDspMeters meters; radios_audio_dsp_get_meters(dsp, &meters); near(meters.channel_peak[0][1], .0625);
    gate_close_ramp(dsp, &frame, TRUE); near(frame.cue[0], 0); near(frame.cue[1], 0); near(frame.main[0], .25);
    /* A configured legacy microphone PFL cannot circumvent FALSE routing. */
    config.channels[29].source_deck = -2; config.channels[29].pfl = TRUE;
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.cue[1], .0625);
    config.speech_channel = -1; configure(dsp, &config); speech_settle(dsp, &frame);
    near(frame.main[0], .25); near(frame.main[1], .25); near(frame.cue[0], 0); near(frame.cue[1], 0);
    radios_audio_dsp_free(dsp);
}
static gdouble speech_destination_tone_energy(RadioSAudioDsp *dsp, RadioSAudioMixerConfig *config)
{
    configure(dsp, config); radios_audio_dsp_reset(dsp); Frame frame; frame_init(&frame); gdouble energy = 0;
    for (guint i = 0; i < 18000; ++i) {
        frame.microphone[0] = (gfloat)(.1 * sin(2 * G_PI * 1000 * i / 48000)); process(dsp, &frame, TRUE, FALSE);
        if (i >= 6000) energy += frame.main[0] * frame.main[0];
    }
    return energy;
}
static void speech_destination_pre_equalizer(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; config_init(&config);
    config.speech_to_master = FALSE; config.speech_channel = 0;
    gdouble full = speech_destination_tone_energy(dsp, &config); g_assert_cmpfloat(full, >, 1);
    config.channels[0].eq_mid_db = -26;
    g_assert_cmpfloat(speech_destination_tone_energy(dsp, &config), <, full / 100);
    radios_audio_dsp_free(dsp);
}
static void speech_destination_preserves_music_tails(void)
{
    for (guint transition = 0; transition < 3; ++transition) {
        RadioSAudioDsp *dsp = radios_audio_dsp_new(), *baseline = radios_audio_dsp_new();
        RadioSAudioMixerConfig config; config_init(&config);
        config.speech_to_master = FALSE; config.speech_channel = 0; config.speech_fx_slot = 2;
        config.channels[31].source_deck = 0; config.channels[31].fx_slot = 1;
        config.channels[0].fx_slot = 0; config.channels[2].return_effect = 0;
        config.channels[3].return_effect = 1; config.master.return_effect = 2;
        config.effects[0].type = config.effects[1].type = config.effects[2].type = RADIOS_FX_ECHO;
        configure(dsp, &config); configure(baseline, &config);
        Frame frame, reference; frame_init(&frame); frame_init(&reference);
        frame.sources[0] = reference.sources[0] = .1f; frame.microphone[0] = .2f;
        process(dsp, &frame, TRUE, FALSE); process(baseline, &reference, TRUE, FALSE);
        frame.sources[0] = reference.sources[0] = frame.microphone[0] = 0;
        if (transition == 0) config.speech_channel = 1;
        else if (transition == 1) config.speech_to_master = TRUE;
        else config.speech_channel = -1;
        g_assert_true(radios_audio_dsp_configure(dsp, &config, NULL));
        g_assert_true(radios_audio_dsp_configure(baseline, &config, NULL));
        RadioSAudioMixerConfig actual; radios_audio_dsp_get_config(dsp, &actual);
        g_assert_cmpint(actual.speech_fx_slot, ==, 2);
        gdouble peak = 0;
        for (guint i = 1; i < 30000; ++i) {
            process(dsp, &frame, TRUE, FALSE); process(baseline, &reference, TRUE, FALSE);
            near(frame.main[0], reference.main[0]); near(frame.main[1], reference.main[1]);
            peak = MAX(peak, fabs(frame.main[0]));
        }
        near(peak, .1); radios_audio_dsp_free(dsp); radios_audio_dsp_free(baseline);
    }
}
static void speech_destination_ducking_and_independent_aux(void)
{
    RadioSAudioDsp *dsp = radios_audio_dsp_new(); RadioSAudioMixerConfig config; config_init(&config);
    config.speech_to_master = FALSE; config.speech_channel = 0; config.speech_fx_slot = 3;
    config.channels[0].gain_db = -6.020599913279624; config.channels[31].source_deck = 0;
    config.master.return_effect = 3; config.speech_ducking = TRUE; config.speech_bed_db = -20;
    configure(dsp, &config); Frame frame; frame_init(&frame); frame.sources[0] = .5f; frame.microphone[0] = .1f;
    for (guint i = 0; i < 4000; ++i) process(dsp, &frame, TRUE, FALSE);
    near(frame.main[0], .2); /* .05 music bed + .05 channel dry + .1 independent wet. */
    config.speech_to_master = TRUE; configure(dsp, &config);
    for (guint i = 0; i < 4000; ++i) process(dsp, &frame, TRUE, FALSE);
    near(frame.main[0], .25);
    config.speech_to_master = FALSE; config.speech_channel = -1; configure(dsp, &config);
    for (guint i = 0; i < 4000; ++i) process(dsp, &frame, TRUE, FALSE);
    near(frame.main[0], .15);
    config.speech_channel = 0; config.channels[0].solo = TRUE; configure(dsp, &config); speech_settle(dsp, &frame);
    near(frame.main[0], .15); radios_audio_dsp_begin_block(dsp); process(dsp, &frame, TRUE, FALSE);
    RadioSAudioDspMeters meters; radios_audio_dsp_get_meters(dsp, &meters); near(meters.speech_fx_send_peak[0], .1);
    config.channels[0].solo = FALSE; config.channels[29].source_deck = -2; config.channels[29].solo = TRUE;
    /* Speech is routed exclusively to channel 0: the unused legacy source on
     * channel 29 is dormant, so its SOLO leaves that path and music audible. */
    configure(dsp, &config); speech_settle(dsp, &frame); near(frame.main[0], .2);
    g_assert_cmpuint(radios_audio_dsp_contributing_solo_mask(dsp), ==, 0);
    gate_close_ramp(dsp, &frame, FALSE);
    radios_audio_dsp_begin_block(dsp); process(dsp, &frame, FALSE, FALSE); radios_audio_dsp_get_meters(dsp, &meters);
    near(meters.speech_fx_send_peak[0], 0);
    radios_audio_dsp_free(dsp);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/effect-return/validation-atomic", validation);
    g_test_add_func("/effect-return/cross-source-independent-slots", cross_source_and_independent_slots);
    g_test_add_func("/effect-return/same-source-no-feedback", same_source_and_no_feedback);
    g_test_add_func("/effect-return/cue-pfl-meters-mute", monitor_pfl_meter_and_mute);
    g_test_add_func("/effect-return/master-crossfade", master_return_and_crossfade);
    g_test_add_func("/effect-return/recipient-equalizer", recipient_equalizer);
    g_test_add_func("/effect-return/speech-duck-gate-tail", speech_ducking_and_gate_tail);
    g_test_add_func("/effect-return/speech-send-validation", speech_send_validation);
    g_test_add_func("/effect-return/speech-send-independent-returns", speech_send_independent_returns);
    g_test_add_func("/effect-return/speech-send-once-channel-assignments", speech_send_once_with_channel_assignments);
    g_test_add_func("/effect-return/speech-send-gate-route-music-tails", speech_send_gate_route_and_music_tails);
    g_test_add_func("/effect-return/speech-send-ducking", speech_send_remains_speech_during_ducking);
    g_test_add_func("/effect-return/speech-send-legacy-music-history", speech_send_legacy_music_history);
    g_test_add_func("/effect-return/speech-destination-validation", speech_destination_validation);
    g_test_add_func("/effect-return/speech-destination-legacy-exclusive", speech_destination_legacy_and_exclusive);
    g_test_add_func("/effect-return/speech-destination-controls-gate", speech_destination_controls_and_gate);
    g_test_add_func("/effect-return/speech-destination-pre-equalizer", speech_destination_pre_equalizer);
    g_test_add_func("/effect-return/speech-destination-preserve-music-tails", speech_destination_preserves_music_tails);
    g_test_add_func("/effect-return/speech-destination-ducking-independent-aux", speech_destination_ducking_and_independent_aux);
    return g_test_run();
}
