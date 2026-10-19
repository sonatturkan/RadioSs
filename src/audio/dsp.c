#include "radios/audio_dsp.h"
#include <math.h>
#include <string.h>

#define RATE 48000.0
#define HISTORY 48002u
#define MAX_DELAY 47998u
typedef struct { gdouble b0, b1, b2, a1, a2; } Coefficients;
typedef struct { gdouble z1, z2; } FilterState;
typedef struct { gdouble value, target, step; guint remaining; } Ramp;
static void ramp_target(Ramp *ramp, gdouble target, gboolean initial)
{
    if (initial) { ramp->value = ramp->target = target; ramp->remaining = 0; return; }
    if (ramp->target == target) return;
    ramp->target = target; ramp->remaining = 240; ramp->step = (target - ramp->value) / 240;
}
static gdouble ramp_next(Ramp *ramp)
{
    if (ramp->remaining) { ramp->value += ramp->step; if (!--ramp->remaining) ramp->value = ramp->target; }
    return ramp->value;
}
typedef struct {
    gfloat history[HISTORY][2];
    guint write, age, roll_cursor, roll_length;
    gboolean captured;
    gdouble phase, filter[2], phaser[2][4], last[2];
} EffectState;
typedef struct {
    Coefficients eq[3];
    FilterState states[6][3][2]; /* dry music/PFL/speech, returned music/speech/PFL */
    EffectState effect, speech_effect;
    Ramp gain, loudness, pan[2], crossfade, send, return_gain, channel_send, channel_return;
    gdouble loudness_db;
    gboolean loudness_enabled, auto_fader;
    gboolean bypass_eq;
} ChannelState;
struct RadioSAudioDsp {
    RadioSAudioMixerConfig config;
    ChannelState channels[32];
    guint channel_order[32], route_offsets[33], route_targets[64];
    gboolean master_receivers[32];
    Coefficients master_eq[3];
    FilterState master_states[3][2];
    EffectState master_effect, master_aux, master_aux_speech, speech_aux;
    gboolean speech_send_active;
    FilterState master_aux_states[2][3][2], master_return_states[2][3][2];
    gboolean explicit_returns;
    Ramp fx_send[5], fx_return[5], master_send, master_return, master_fx_send, master_fx_return;
    Ramp master_channel_send, master_channel_return;
    gdouble crossfade[3], duck_gain;
    guint speech_hold;
    gboolean microphone_gate;
    /* Gate ramp (frames, 0 = immediate). microphone_gain scales only the
     * on-air microphone ingress; voice_wet scales the wet output of the
     * gate-owned voice histories (speech SEND, strip voice effects), which are
     * cleared at the bottom of each closing voice_dip. */
    guint microphone_ramp;
    gdouble microphone_gain, voice_wet;
    gboolean voice_dip, voice_fed; /* voice_fed: a voice history got input since its last reset */
    gboolean initialized;
    RadioSAudioDspMeters meters;
    gboolean capture_channels;
    gboolean source_activity_known, source_activity[RADIOS_AUDIO_DECKS];
    guint32 contributing_solo_mask;
    gfloat channel_output[32][2];
};
static const gchar *const effect_names[] = {"Delay", "Echo", "Spiral", "Reverb", "Trans", "Filter",
    "Flanger", "Phaser", "Robot", "Melodic", "Slip Roll", "Roll", "Rev Roll", "MIDI LFO"};
const gchar *radios_audio_fx_name(RadioSAudioFxType type)
{ return type >= 0 && type < RADIOS_FX_COUNT ? effect_names[type] : NULL; }
static gboolean invalid(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, message); return FALSE; }
static gboolean range(gdouble number, gdouble low, gdouble high)
{ return isfinite(number) && number >= low && number <= high; }
static gboolean boolean(gboolean value) { return value == FALSE || value == TRUE; }
static gboolean fx_valid(const RadioSAudioFx *fx)
{
    return fx->type >= 0 && fx->type < RADIOS_FX_COUNT && boolean(fx->enabled) && boolean(fx->cue) &&
        range(fx->depth, 0, 1) && range(fx->send_db, -60, 6) && range(fx->return_db, -60, 6) &&
        range(fx->tempo_bpm, 30, 300) && fx->monitor_path >= 0 && fx->monitor_path <= 2;
}
static gboolean channel_link(const RadioSAudioMixerConfig *config, guint source, guint target)
{
    return config->channels[source].send_channel == (gint)target || config->channels[target].return_channel == (gint)source;
}
static void master_receivers(const RadioSAudioMixerConfig *config, gboolean receivers[32])
{
    memset(receivers, 0, 32 * sizeof *receivers);
    if (config->master.send_channel < 0) return;
    receivers[config->master.send_channel] = TRUE;
    for (guint pass = 0; pass < 32; ++pass) {
        gboolean changed = FALSE;
        for (guint source = 0; source < 32; ++source) if (receivers[source])
            for (guint target = 0; target < 32; ++target)
                if (!receivers[target] && channel_link(config, source, target)) {
                    receivers[target] = TRUE; changed = TRUE;
                }
        if (!changed) break;
    }
}
static gboolean channel_routes(const RadioSAudioMixerConfig *config, guint order[32], const gboolean receivers[32])
{
    guint incoming[32] = {0}; gboolean emitted[32] = {FALSE};
    for (guint source = 0; source < 32; ++source)
        for (guint target = 0; target < 32; ++target)
            if (channel_link(config, source, target))
                ++incoming[target];
    for (guint position = 0; position < 32; ++position) {
        guint source = 0;
        if (receivers)
            while (source < 32 && (emitted[source] || incoming[source] || receivers[source])) ++source;
        if (source == 32) source = 0;
        while (source < 32 && (emitted[source] || incoming[source])) ++source;
        if (source == 32) return FALSE;
        emitted[source] = TRUE;
        if (order) order[position] = source;
        for (guint target = 0; target < 32; ++target)
            if (channel_link(config, source, target))
                --incoming[target];
    }
    return TRUE;
}
void radios_audio_mixer_config_default(RadioSAudioMixerConfig *config)
{
    g_return_if_fail(config); memset(config, 0, sizeof *config);
    for (guint i = 0; i < 32; ++i) {
        config->channels[i].source_deck = (gint)i; config->channels[i].fx_slot = -1;
        config->channels[i].fx_return_db = -6; config->channels[i].return_effect = -2;
        config->channels[i].send_channel = config->channels[i].return_channel = -1;
        config->channels[i].send_db = -6;
    }
    for (guint i = 0; i < 5; ++i) {
        config->effects[i].depth = .5; config->effects[i].return_db = -6;
        config->effects[i].tempo_bpm = 120; config->effects[i].monitor_path = 1;
    }
    config->master_fx = config->effects[0]; config->master.fx_slot = -1; config->master.fx_return_db = -6;
    config->master.return_effect = -2;
    config->master.send_channel = config->master.return_channel = -1; config->master.send_db = -6;
    config->speech_ducking = TRUE; config->speech_bed_db = -18; config->speech_fx_slot = -1;
    config->speech_to_master = TRUE; config->speech_channel = -1;
}
gboolean radios_audio_mixer_config_validate(const RadioSAudioMixerConfig *config, GError **error)
{
    if (!config || !range(config->crossfader, -1, 1) || !boolean(config->speech_ducking) ||
        !range(config->speech_bed_db, -36, 0) || config->speech_fx_slot < -1 || config->speech_fx_slot >= (gint)RADIOS_AUDIO_FX_SLOTS ||
        !boolean(config->speech_to_master) || config->speech_channel < -1 || config->speech_channel >= (gint)RADIOS_AUDIO_MIXER_CHANNELS)
        return invalid(error, "Invalid crossfader or speech ducking configuration");
    for (guint i = 0; i < 32; ++i) {
        const RadioSAudioChannelDsp *channel = &config->channels[i];
        if (channel->source_deck < -2 || channel->source_deck >= 32 || !range(channel->eq_low_db, -26, 6) ||
            !range(channel->eq_mid_db, -26, 6) || !range(channel->eq_high_db, -26, 6) ||
            !range(channel->pan, -1, 1) || !range(channel->gain_db, -60, 6) || !boolean(channel->mute) ||
            !boolean(channel->solo) || !boolean(channel->pfl) || !boolean(channel->loudness_bypass) || channel->crossfade_group < 0 || channel->crossfade_group > 2 ||
            channel->fx_slot < -1 || channel->fx_slot >= 5 || channel->return_effect < -2 || channel->return_effect >= 5 ||
            !range(channel->fx_send_db, -60, 6) || !range(channel->fx_return_db, -60, 6))
            return invalid(error, "Invalid mixer channel configuration");
        if (channel->send_channel < -1 || channel->send_channel >= 32 || channel->send_channel == (gint)i ||
            channel->return_channel < -1 || channel->return_channel >= 32 || channel->return_channel == (gint)i ||
            !range(channel->send_db, -60, 6) || !range(channel->return_db, -60, 6))
            return invalid(error, "Kanal gönderimi geçersiz; kanal kendisine bağlanamaz.");
    }
    if (!channel_routes(config, NULL, NULL)) return invalid(error, "Send/return bağlantısı geri besleme döngüsü oluşturuyor.");
    const RadioSAudioMasterDsp *master = &config->master;
    if (!range(master->eq_low_db, -26, 6) || !range(master->eq_mid_db, -26, 6) || !range(master->eq_high_db, -26, 6) ||
        !boolean(master->mute) || master->fx_slot < -1 || master->fx_slot >= 5 || master->return_effect < -2 || master->return_effect >= 5 ||
        !range(master->fx_send_db, -60, 6) || !range(master->fx_return_db, -60, 6) || !fx_valid(&config->master_fx))
        return invalid(error, "Invalid master DSP configuration");
    if (master->send_channel < -1 || master->send_channel >= 32 || master->return_channel < -1 || master->return_channel >= 32 ||
        !range(master->send_db, -60, 6) || !range(master->return_db, -60, 6))
        return invalid(error, "MASTER kanal gönderimi veya alım seviyesi geçersiz.");
    for (guint i = 0; i < 5; ++i) if (!fx_valid(&config->effects[i])) return invalid(error, "Invalid effect slot configuration");
    return TRUE;
}
/* RBJ peaking/shelving equations: https://www.w3.org/TR/audio-eq-cookbook/ */
static Coefficients coefficients(guint band, gdouble db)
{
    if (db == 0) return (Coefficients){1, 0, 0, 0, 0};
    gdouble frequency = band == 0 ? 200 : band == 1 ? 1000 : 6000;
    gdouble a = pow(10, db / 40), w = 2 * G_PI * frequency / RATE, c = cos(w), s = sin(w);
    gdouble alpha = s / (2 * .7071067811865476), root = 2 * sqrt(a) * alpha;
    gdouble b0, b1, b2, a0, a1, a2;
    if (band == 0) {
        b0 = a * ((a + 1) - (a - 1) * c + root); b1 = 2 * a * ((a - 1) - (a + 1) * c);
        b2 = a * ((a + 1) - (a - 1) * c - root); a0 = (a + 1) + (a - 1) * c + root;
        a1 = -2 * ((a - 1) + (a + 1) * c); a2 = (a + 1) + (a - 1) * c - root;
    } else if (band == 2) {
        b0 = a * ((a + 1) + (a - 1) * c + root); b1 = -2 * a * ((a - 1) + (a + 1) * c);
        b2 = a * ((a + 1) + (a - 1) * c - root); a0 = (a + 1) - (a - 1) * c + root;
        a1 = 2 * ((a - 1) - (a + 1) * c); a2 = (a + 1) - (a - 1) * c - root;
    } else {
        alpha = s / 2; b0 = 1 + alpha * a; b1 = -2 * c; b2 = 1 - alpha * a;
        a0 = 1 + alpha / a; a1 = -2 * c; a2 = 1 - alpha / a;
    }
    return (Coefficients){b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}
static gdouble filter(gdouble input, const Coefficients *c, FilterState *state)
{
    gdouble output = input * c->b0 + state->z1;
    state->z1 = input * c->b1 - output * c->a1 + state->z2;
    state->z2 = input * c->b2 - output * c->a2;
    if (!isfinite(output) || fabs(output) > 1e12) { state->z1 = state->z2 = 0; return 0; }
    if (fabs(state->z1) < 1e-30) state->z1 = 0;
    if (fabs(state->z2) < 1e-30) state->z2 = 0;
    return output;
}
static gdouble clean(gdouble input) { return isfinite(input) ? CLAMP(input, -1e6, 1e6) : 0; }
static gdouble tap(const EffectState *state, guint channel, gdouble delay)
{
    delay = CLAMP(delay, 1, MAX_DELAY); gdouble position = state->write - delay;
    if (position < 0) position += HISTORY;
    guint first = (guint)position, next = first + 1 == HISTORY ? 0 : first + 1;
    gdouble fraction = position - first;
    return state->history[first][channel] * (1 - fraction) + state->history[next][channel] * fraction;
}
static void effect(EffectState *state, const RadioSAudioFx *settings, const gdouble input[2], gdouble output[2])
{
    output[0] = output[1] = 0;
    if (!settings->enabled) return;
    gdouble depth = settings->depth, beat = MIN(RATE * 60 / settings->tempo_bpm, MAX_DELAY);
    gdouble phase = state->phase, stored[2] = {input[0], input[1]};
    for (guint c = 0; c < 2; ++c) {
        gdouble value = 0, delayed;
        switch (settings->type) {
        case RADIOS_FX_DELAY: value = tap(state, c, beat * (.125 + .875 * depth)); break;
        case RADIOS_FX_ECHO:
            value = tap(state, c, beat * .5); stored[c] += value * (.2 + .6 * depth); break;
        case RADIOS_FX_SPIRAL:
            value = tap(state, c, beat * (.25 + .15 * sin(phase * 2 * G_PI)));
            stored[c] += value * (.3 + .5 * depth); value *= .8 + .2 * sin(phase * 2 * G_PI); break;
        case RADIOS_FX_REVERB: {
            const guint delays[] = {1557, 1617, 1491, 1422};
            for (guint i = 0; i < 4; ++i) value += tap(state, c, delays[i] + c * 23);
            value *= .25; stored[c] += value * (.45 + .4 * depth); break;
        }
        case RADIOS_FX_TRANS: value = input[c] * (phase < .2 + .6 * (1 - depth) ? 1 : 0); break;
        case RADIOS_FX_FILTER: {
            gdouble cutoff = 100 + 15000 * (1 - depth), coefficient = 1 - exp(-2 * G_PI * cutoff / RATE);
            state->filter[c] += coefficient * (input[c] - state->filter[c]); value = state->filter[c]; break;
        }
        case RADIOS_FX_FLANGER:
            delayed = tap(state, c, 24 + 216 * (.5 + .5 * sin(2 * G_PI * (phase + c * .25))));
            value = (input[c] + delayed) * .5; stored[c] += delayed * .35; break;
        case RADIOS_FX_PHASER: {
            gdouble frequency = 350 + 2500 * (.5 + .5 * sin(phase * 2 * G_PI));
            gdouble t = tan(G_PI * frequency / RATE), coefficient = (1 - t) / (1 + t); value = input[c];
            for (guint stage = 0; stage < 4; ++stage) {
                gdouble allpass = -coefficient * value + state->phaser[c][stage];
                state->phaser[c][stage] = value + coefficient * allpass; value = allpass;
            }
            value = .5 * (input[c] + value); break;
        }
        case RADIOS_FX_ROBOT: value = input[c] * sin(2 * G_PI * phase); break;
        case RADIOS_FX_MELODIC: {
            gdouble second = phase < .5 ? phase + .5 : phase - .5;
            gdouble first_weight = .5 - .5 * cos(2 * G_PI * phase), second_weight = 1 - first_weight;
            value = tap(state, c, 1 + phase * 2400) * first_weight + tap(state, c, 1 + second * 2400) * second_weight; break;
        }
        case RADIOS_FX_SLIP_ROLL: case RADIOS_FX_ROLL: case RADIOS_FX_REV_ROLL: {
            if (!state->captured) { value = input[c]; break; }
            guint position = settings->type == RADIOS_FX_REV_ROLL ? state->roll_length - 1 - state->roll_cursor : state->roll_cursor;
            value = state->history[position][c]; break;
        }
        case RADIOS_FX_MIDI_LFO: value = input[c] * (.5 + .5 * sin(2 * G_PI * phase)); break;
        default: break;
        }
        output[c] = clean(value * depth);
    }
    if (settings->type >= RADIOS_FX_SLIP_ROLL && settings->type <= RADIOS_FX_REV_ROLL) {
        if (!state->roll_length) state->roll_length = MAX(1u, (guint)(beat / pow(2, floor(depth * 4))));
        if (!state->captured) {
            for (guint c = 0; c < 2; ++c) state->history[state->write][c] = (gfloat)CLAMP(stored[c], -64, 64);
            if (++state->write >= state->roll_length) { state->captured = TRUE; state->roll_cursor = 0; }
        } else {
            if (settings->type == RADIOS_FX_ROLL && state->age / state->roll_length % 2)
                for (guint c = 0; c < 2; ++c) state->history[state->roll_cursor][c] = (gfloat)CLAMP(stored[c], -64, 64);
            if (++state->roll_cursor >= state->roll_length) state->roll_cursor = 0;
        }
    } else {
        for (guint c = 0; c < 2; ++c) state->history[state->write][c] = (gfloat)CLAMP(stored[c], -64, 64);
        if (++state->write >= HISTORY) state->write = 0;
    }
    ++state->age;
    gdouble increment = settings->tempo_bpm / (60 * RATE);
    if (settings->type == RADIOS_FX_ROBOT) increment = (40 + 760 * depth) / RATE;
    else if (settings->type == RADIOS_FX_MELODIC) increment = (1 - pow(2, (depth * 24 - 12) / 12)) / 2400;
    else if (settings->type == RADIOS_FX_TRANS) increment *= 4;
    else if (settings->type == RADIOS_FX_FLANGER || settings->type == RADIOS_FX_PHASER || settings->type == RADIOS_FX_SPIRAL) increment *= .25;
    state->phase += increment; state->phase -= floor(state->phase);
}
RadioSAudioDsp *radios_audio_dsp_new(void)
{
    RadioSAudioDsp *dsp = g_new0(RadioSAudioDsp, 1); dsp->duck_gain = 1; dsp->voice_wet = 1;
    dsp->microphone_ramp = 240; /* click-free 5 ms L/R gate by default */
    RadioSAudioMixerConfig config; radios_audio_mixer_config_default(&config);
    (void)radios_audio_dsp_configure(dsp, &config, NULL); return dsp;
}
void radios_audio_dsp_free(RadioSAudioDsp *dsp) { g_free(dsp); }
void radios_audio_dsp_get_config(const RadioSAudioDsp *dsp, RadioSAudioMixerConfig *config)
{ g_return_if_fail(dsp && config); *config = dsp->config; }
gboolean radios_audio_dsp_configure(RadioSAudioDsp *dsp, const RadioSAudioMixerConfig *config, GError **error)
{
    if (!dsp || !radios_audio_mixer_config_validate(config, error)) return FALSE;
    gboolean explicit_returns = config->master.return_effect != -2;
    for (guint i = 0; i < 32; ++i) explicit_returns |= config->channels[i].return_effect != -2;
    gboolean return_mode_changed = explicit_returns != dsp->explicit_returns;
    gboolean rerouted = config->master.send_channel != dsp->config.master.send_channel;
    gint speech_slot = config->speech_fx_slot;
    gboolean microphone_rerouted = config->speech_to_master != dsp->config.speech_to_master ||
        (!config->speech_to_master && config->speech_channel != dsp->config.speech_channel);
    gboolean speech_rerouted = microphone_rerouted || speech_slot != dsp->config.speech_fx_slot || config->master.mute != dsp->config.master.mute;
    if (speech_slot >= 0)
        speech_rerouted |= dsp->config.effects[speech_slot].type != config->effects[speech_slot].type ||
            dsp->config.effects[speech_slot].enabled != config->effects[speech_slot].enabled;
    for (guint index = 0; index < 32; ++index)
        speech_rerouted |= config->channels[index].return_effect != dsp->config.channels[index].return_effect ||
            config->channels[index].source_deck != dsp->config.channels[index].source_deck;
    speech_rerouted |= config->master.return_effect != dsp->config.master.return_effect;
    for (guint index = 0; index < 32; ++index)
        rerouted |= config->channels[index].send_channel != dsp->config.channels[index].send_channel ||
            config->channels[index].return_channel != dsp->config.channels[index].return_channel;
    if ((speech_slot >= 0 || dsp->config.speech_fx_slot >= 0) && (speech_rerouted || rerouted)) {
        memset(&dsp->speech_aux, 0, sizeof dsp->speech_aux);
        for (guint index = 0; index < 32; ++index)
            memset(dsp->channels[index].states[4], 0, sizeof dsp->channels[index].states[4]);
        memset(dsp->master_return_states[1], 0, sizeof dsp->master_return_states[1]);
        dsp->speech_send_active = FALSE;
    }
    if (microphone_rerouted) {
        /* Only voice histories belong to this ingress; music and source
         * assignments survive destination changes. The live gate is separate. */
        for (guint index = 0; index < RADIOS_AUDIO_MIXER_CHANNELS; ++index) {
            ChannelState *state = &dsp->channels[index];
            memset(&state->speech_effect, 0, sizeof state->speech_effect);
            memset(state->states[2], 0, sizeof state->states[2]);
            memset(state->states[4], 0, sizeof state->states[4]);
        }
        memset(&dsp->speech_aux, 0, sizeof dsp->speech_aux); dsp->speech_send_active = FALSE;
        memset(&dsp->master_aux_speech, 0, sizeof dsp->master_aux_speech);
        memset(dsp->master_aux_states[1], 0, sizeof dsp->master_aux_states[1]);
        memset(dsp->master_return_states[1], 0, sizeof dsp->master_return_states[1]);
    }
    master_receivers(config, dsp->master_receivers);
    channel_routes(config, dsp->channel_order, dsp->master_receivers);
    guint route_count = 0;
    for (guint source = 0; source < 32; ++source) {
        dsp->route_offsets[source] = route_count;
        for (guint target = 0; target < 32; ++target)
            if (channel_link(config, source, target))
                dsp->route_targets[route_count++] = target;
    }
    dsp->route_offsets[32] = route_count;
    for (guint i = 0; i < 32; ++i) {
        ChannelState *state = &dsp->channels[i]; const RadioSAudioChannelDsp *channel = &config->channels[i];
        const RadioSAudioChannelDsp *before = &dsp->config.channels[i];
        if (rerouted || before->source_deck != channel->source_deck || before->fx_slot != channel->fx_slot ||
            (channel->fx_slot >= 0 && (dsp->config.effects[channel->fx_slot].type != config->effects[channel->fx_slot].type ||
             dsp->config.effects[channel->fx_slot].enabled != config->effects[channel->fx_slot].enabled))) {
            memset(state->states, 0, sizeof state->states); memset(&state->effect, 0, sizeof state->effect);
            memset(&state->speech_effect, 0, sizeof state->speech_effect);
        }
        if (before->return_effect != channel->return_effect)
            memset(state->states[3], 0, sizeof state->states[3] * 3);
        state->eq[0] = coefficients(0, channel->eq_low_db); state->eq[1] = coefficients(1, channel->eq_mid_db); state->eq[2] = coefficients(2, channel->eq_high_db);
        state->bypass_eq = channel->eq_low_db == 0 && channel->eq_mid_db == 0 && channel->eq_high_db == 0;
        if (state->bypass_eq) memset(state->states, 0, sizeof state->states);
        ramp_target(&state->gain, pow(10, channel->gain_db / 20), !dsp->initialized);
        ramp_target(&state->loudness, state->loudness_enabled && !channel->loudness_bypass && channel->source_deck >= 0 ?
            pow(10, state->loudness_db / 20) : 1, !dsp->initialized);
        ramp_target(&state->pan[0], channel->pan > 0 ? cos(channel->pan * G_PI / 2) : 1, !dsp->initialized);
        ramp_target(&state->pan[1], channel->pan < 0 ? cos(-channel->pan * G_PI / 2) : 1, !dsp->initialized);
        ramp_target(&state->send, pow(10, channel->fx_send_db / 20), !dsp->initialized);
        ramp_target(&state->return_gain, pow(10, channel->fx_return_db / 20), !dsp->initialized);
        ramp_target(&state->channel_send, pow(10, channel->send_db / 20), !dsp->initialized);
        ramp_target(&state->channel_return, pow(10, channel->return_db / 20), !dsp->initialized);
        gdouble crossfade = channel->crossfade_group == 0 ? 1 : channel->crossfade_group == 1 ?
            cos((config->crossfader + 1) * G_PI / 4) : sin((config->crossfader + 1) * G_PI / 4);
        ramp_target(&state->crossfade, crossfade, !dsp->initialized);
    }
    if (rerouted) memset(dsp->master_states, 0, sizeof dsp->master_states);
    if (rerouted || dsp->config.master_fx.type != config->master_fx.type || dsp->config.master_fx.enabled != config->master_fx.enabled)
        memset(&dsp->master_effect, 0, sizeof dsp->master_effect);
    gint master_slot = config->master.fx_slot;
    if (rerouted || return_mode_changed || dsp->config.master.fx_slot != master_slot || (master_slot >= 0 &&
        (dsp->config.effects[master_slot].type != config->effects[master_slot].type ||
         dsp->config.effects[master_slot].enabled != config->effects[master_slot].enabled)))
    {
        memset(&dsp->master_aux, 0, sizeof dsp->master_aux);
        memset(&dsp->master_aux_speech, 0, sizeof dsp->master_aux_speech);
        memset(dsp->master_aux_states, 0, sizeof dsp->master_aux_states);
    }
    if (rerouted || return_mode_changed || dsp->config.master.return_effect != config->master.return_effect)
        memset(dsp->master_return_states, 0, sizeof dsp->master_return_states);
    dsp->explicit_returns = explicit_returns;
    dsp->master_eq[0] = coefficients(0, config->master.eq_low_db); dsp->master_eq[1] = coefficients(1, config->master.eq_mid_db); dsp->master_eq[2] = coefficients(2, config->master.eq_high_db);
    ramp_target(&dsp->master_send, pow(10, config->master.fx_send_db / 20), !dsp->initialized);
    ramp_target(&dsp->master_return, pow(10, config->master.fx_return_db / 20), !dsp->initialized);
    ramp_target(&dsp->master_channel_send, pow(10, config->master.send_db / 20), !dsp->initialized);
    ramp_target(&dsp->master_channel_return, pow(10, config->master.return_db / 20), !dsp->initialized);
    ramp_target(&dsp->master_fx_send, pow(10, config->master_fx.send_db / 20), !dsp->initialized);
    ramp_target(&dsp->master_fx_return, pow(10, config->master_fx.return_db / 20), !dsp->initialized);
    for (guint i = 0; i < 5; ++i) {
        ramp_target(&dsp->fx_send[i], pow(10, config->effects[i].send_db / 20), !dsp->initialized);
        ramp_target(&dsp->fx_return[i], pow(10, config->effects[i].return_db / 20), !dsp->initialized);
    }
    dsp->crossfade[0] = 1; dsp->crossfade[1] = cos((config->crossfader + 1) * G_PI / 4); dsp->crossfade[2] = sin((config->crossfader + 1) * G_PI / 4);
    dsp->config = *config; dsp->initialized = TRUE; return TRUE;
}
void radios_audio_dsp_reset(RadioSAudioDsp *dsp)
{
    g_return_if_fail(dsp);
    for (guint i = 0; i < 32; ++i) {
        memset(dsp->channels[i].states, 0, sizeof dsp->channels[i].states); memset(&dsp->channels[i].effect, 0, sizeof dsp->channels[i].effect);
        memset(&dsp->channels[i].speech_effect, 0, sizeof dsp->channels[i].speech_effect);
    }
    memset(dsp->master_states, 0, sizeof dsp->master_states); memset(&dsp->master_effect, 0, sizeof dsp->master_effect);
    memset(&dsp->master_aux, 0, sizeof dsp->master_aux);
    memset(&dsp->master_aux_speech, 0, sizeof dsp->master_aux_speech);
    memset(dsp->master_aux_states, 0, sizeof dsp->master_aux_states);
    memset(dsp->master_return_states, 0, sizeof dsp->master_return_states);
    memset(&dsp->speech_aux, 0, sizeof dsp->speech_aux); dsp->speech_send_active = FALSE;
    dsp->duck_gain = 1; dsp->speech_hold = 0; memset(&dsp->meters, 0, sizeof dsp->meters);
    /* Every voice history is clear now. A microphone still fading out keeps
     * feeding them, so that fade ends with its own clearing dip. */
    dsp->voice_wet = 1; dsp->voice_fed = FALSE;
    dsp->voice_dip = !dsp->microphone_gate && dsp->microphone_gain > 0;
}
/* The gate owns only the microphone's own effect histories: the speech SEND
 * and the strips' voice effects. Unassigned strip histories are cleared when
 * a slot is assigned (configure), so only live slots need clearing. Filters
 * (strip and MASTER EQ instances), the MASTER inline/aux effects (fed by the
 * summed programme, MASTER sampler included) and music histories stay. */
static void voice_effects_reset(RadioSAudioDsp *dsp)
{
    for (guint index = 0; index < RADIOS_AUDIO_MIXER_CHANNELS; ++index)
        if (dsp->config.channels[index].fx_slot >= 0)
            memset(&dsp->channels[index].speech_effect, 0, sizeof dsp->channels[index].speech_effect);
    memset(&dsp->speech_aux, 0, sizeof dsp->speech_aux); dsp->voice_fed = FALSE;
}
static gdouble gate_ramp_next(gdouble value, gboolean up, guint frames)
{
    if (!frames) return up ? 1 : 0;
    gdouble step = 1.0 / frames, next = up ? value + step : value - step;
    return next > 1 - step / 2 ? 1 : next < step / 2 ? 0 : next;
}
void radios_audio_dsp_set_microphone_ramp(RadioSAudioDsp *dsp, guint frames)
{
    g_return_if_fail(dsp);
    dsp->microphone_ramp = MIN(frames, 4800u);
    if (dsp->microphone_ramp) return;
    if (dsp->voice_dip) voice_effects_reset(dsp);
    dsp->microphone_gain = dsp->microphone_gate ? 1 : 0; dsp->voice_wet = 1; dsp->voice_dip = FALSE;
}
void radios_audio_dsp_begin_block(RadioSAudioDsp *dsp)
{ g_return_if_fail(dsp); memset(&dsp->meters, 0, sizeof dsp->meters); }
void radios_audio_dsp_get_meters(const RadioSAudioDsp *dsp, RadioSAudioDspMeters *meters)
{
    g_return_if_fail(dsp && meters); *meters = dsp->meters;
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i) {
        const ChannelState *state = &dsp->channels[i];
        meters->channel_loudness_gain_db[i] = 20 * log10(MAX(state->loudness.value, 1e-9));
        if (state->auto_fader) meters->channel_loudness_gain_db[i] += 20 * log10(MAX(state->gain.value, 1e-9));
        meters->channel_loudness_active[i] = dsp->channels[i].loudness_enabled &&
            !dsp->config.channels[i].loudness_bypass && dsp->config.channels[i].source_deck >= 0;
    }
}
void radios_audio_dsp_set_channel_loudness(RadioSAudioDsp *dsp, guint channel, gdouble gain_db, gboolean enabled)
{
    g_return_if_fail(dsp && channel < RADIOS_AUDIO_MIXER_CHANNELS && isfinite(gain_db));
    ChannelState *state = &dsp->channels[channel];
    state->auto_fader = FALSE;
    state->loudness_db = CLAMP(gain_db, -80, 12); state->loudness_enabled = !!enabled;
    ramp_target(&state->loudness, enabled && !dsp->config.channels[channel].loudness_bypass &&
        dsp->config.channels[channel].source_deck >= 0 ? pow(10, state->loudness_db / 20) : 1, FALSE);
}
void radios_audio_dsp_set_channel_auto_gain(RadioSAudioDsp *dsp, guint channel,
    gdouble manual_gain_db, gdouble correction_db, gboolean enabled)
{
    g_return_if_fail(dsp && channel < RADIOS_AUDIO_MIXER_CHANNELS && isfinite(manual_gain_db) && isfinite(correction_db));
    ChannelState *state = &dsp->channels[channel];
    gboolean active = enabled && !dsp->config.channels[channel].loudness_bypass && dsp->config.channels[channel].source_deck >= 0;
    gdouble correction = CLAMP(correction_db, -24, 12);
    gdouble fader = active ? CLAMP(correction, -60, 6) : CLAMP(manual_gain_db, -60, 6);
    state->auto_fader = active; state->loudness_enabled = active;
    state->loudness_db = active ? correction - fader : 0;
    dsp->config.channels[channel].gain_db = fader;
    /* Only the unrepresented correction remains at the source ingress. The
     * normal channel fader carries the rest, so LUFS is never applied twice.
     * No configuration traversal, allocation, EQ reset or FX work happens. */
    ramp_target(&state->gain, pow(10, fader / 20), FALSE);
    ramp_target(&state->loudness, pow(10, state->loudness_db / 20), FALSE);
}
void radios_audio_dsp_capture_channels(RadioSAudioDsp *dsp, gboolean enabled)
{ g_return_if_fail(dsp); dsp->capture_channels = enabled; memset(dsp->channel_output, 0, sizeof dsp->channel_output); }
void radios_audio_dsp_channel_frame(const RadioSAudioDsp *dsp, guint channel, gfloat output[2])
{
    g_return_if_fail(dsp && output);
    output[0] = channel < 32 ? dsp->channel_output[channel][0] : 0; output[1] = channel < 32 ? dsp->channel_output[channel][1] : 0;
}
void radios_audio_dsp_process_frame(RadioSAudioDsp *dsp, const gfloat *sources, const gboolean *routed,
    const gfloat microphone[2], gboolean microphone_gate, gboolean cue_mix, gfloat main_output[2], gfloat cue_output[2])
{
    radios_audio_dsp_process_frame_with_external(dsp, sources, routed, microphone,
        microphone_gate, NULL, cue_mix, main_output, cue_output);
}
void radios_audio_dsp_set_source_activity(RadioSAudioDsp *dsp, const gboolean *active)
{
    g_return_if_fail(dsp);
    dsp->source_activity_known = active != NULL;
    if (active) memcpy(dsp->source_activity, active, sizeof dsp->source_activity);
}
guint32 radios_audio_dsp_contributing_solo_mask(const RadioSAudioDsp *dsp)
{ return dsp ? dsp->contributing_solo_mask : 0; }
static gboolean source_contributes(const RadioSAudioDsp *dsp, const gfloat *sources, const gboolean *routed, guint deck)
{
    return routed[deck] && (dsp->source_activity_known ? dsp->source_activity[deck] :
        sources[deck * 2] != 0 || sources[deck * 2 + 1] != 0);
}
void radios_audio_dsp_process_frame_with_external(RadioSAudioDsp *dsp, const gfloat *sources,
    const gboolean *routed, const gfloat microphone[2], gboolean microphone_gate,
    const gfloat external[2], gboolean cue_mix, gfloat main_output[2], gfloat cue_output[2])
{
    /* L/R must not touch the programme (PDF p.8, p.15). A gate change used to
     * clear every MASTER EQ instance, the MASTER inline and aux effects (also
     * the MASTER sampler's aux path), strip EQs of voice/returns/CUE and
     * microphone strips' music histories: clicks and cut echo/reverb tails in
     * the playing song. Only the microphone path follows the gate now. */
    if (dsp->microphone_gate != microphone_gate) {
        dsp->microphone_gate = microphone_gate;
        if (!dsp->microphone_ramp) voice_effects_reset(dsp);
        else if (!microphone_gate) dsp->voice_dip = TRUE;
        else if (dsp->voice_dip) dsp->voice_dip = FALSE; /* reopened while closing: one continuous voice */
        else if (dsp->microphone_gain == 0 && !dsp->voice_fed) voice_effects_reset(dsp); /* silent: restart rolls/LFOs */
    }
    dsp->microphone_gain = gate_ramp_next(dsp->microphone_gain, microphone_gate, dsp->microphone_ramp);
    if (dsp->voice_dip) {
        /* The voice effects' wet output fades with the closing ingress
         * (voice_wet >= microphone_gain), so their histories are cleared
         * only once silent and with no input left to cut. */
        dsp->voice_wet = gate_ramp_next(dsp->voice_wet, FALSE, dsp->microphone_ramp);
        if (dsp->voice_wet == 0) { voice_effects_reset(dsp); dsp->voice_dip = FALSE; }
    } else if (dsp->voice_wet < 1) dsp->voice_wet = gate_ramp_next(dsp->voice_wet, TRUE, dsp->microphone_ramp);
    /* On-air microphone routing stays live until its ramp and voice dip end. */
    const gboolean microphone_live = microphone_gate || dsp->microphone_gain > 0 || dsp->voice_dip;
    const gdouble voice_wet = dsp->voice_wet;
    if (dsp->capture_channels) memset(dsp->channel_output, 0, sizeof dsp->channel_output);
    gdouble music[2] = {0}, speech[2] = {0}, pfl[2] = {0}, fx_send[5][2] = {{0}}, fx_return[5][2] = {{0}};
    gdouble incoming_music[32][2] = {{0}}, incoming_speech[32][2] = {{0}}, route_send[32], route_return[32];
    gdouble bus_music[5][2] = {{0}}, bus_speech[5][2] = {{0}}, strip_gain[32], strip_pan[32][2], strip_crossfade[32];
    gdouble channel_cue[32][2] = {{0}}; gboolean strip_allowed[32];
    for (guint index = 0; index < 32; ++index) {
        route_send[index] = ramp_next(&dsp->channels[index].channel_send);
        route_return[index] = ramp_next(&dsp->channels[index].channel_return);
    }
    gdouble slot_send[5], slot_return[5];
    for (guint i = 0; i < 5; ++i) { slot_send[i] = ramp_next(&dsp->fx_send[i]); slot_return[i] = ramp_next(&dsp->fx_return[i]); }
    gdouble master_strip_send = ramp_next(&dsp->master_send), master_strip_return = ramp_next(&dsp->master_return);
    gdouble master_send = master_strip_send * ramp_next(&dsp->master_fx_send);
    gdouble master_return = master_strip_return * ramp_next(&dsp->master_fx_return);
    gdouble master_route_send = ramp_next(&dsp->master_channel_send), master_route_return = ramp_next(&dsp->master_channel_return);
    gboolean solo = FALSE, mic_assigned = FALSE, microphone_solo = FALSE, contributing[32] = {FALSE}, incoming[32] = {FALSE};
    /* A dormant solo button cannot mute every live player. Transport activity
     * keeps an intentional live solo stable across waveform zero crossings. */
    for (guint position = 0; position < 32; ++position) {
        guint i = dsp->channel_order[position]; const RadioSAudioChannelDsp *channel = &dsp->config.channels[i];
        gboolean source = channel->source_deck == -2 ? dsp->config.speech_to_master && microphone_live : channel->source_deck >= 0 &&
            source_contributes(dsp, sources, routed, (guint)channel->source_deck);
        source |= source_contributes(dsp, sources, routed, RADIOS_AUDIO_SAMPLER_BASE + i);
        source |= !dsp->config.speech_to_master && dsp->config.speech_channel == (gint)i && microphone_live;
        contributing[i] = !channel->mute && channel->gain_db > -60 && dsp->crossfade[channel->crossfade_group] > 0.000000001 && (source || incoming[i]);
        if (contributing[i] && channel->send_db > -60) for (guint route = dsp->route_offsets[i]; route < dsp->route_offsets[i + 1]; ++route) {
            guint destination = dsp->route_targets[route];
            incoming[destination] |= dsp->config.channels[destination].return_db > -60;
        }
    }
    /* An explicit wet RETURN has no direct deck, but its SOLO is deliberate
     * while an enabled effect can receive live input. Determine feed potential
     * before SOLO gates the senders, and keep it stable at zero crossings. */
    gboolean effect_fed[5] = {FALSE}, main_fed = source_contributes(dsp, sources, routed, RADIOS_AUDIO_MASTER_SAMPLER) ||
        (external && (external[0] != 0 || external[1] != 0));
    main_fed |= microphone_live && dsp->config.speech_to_master;
    for (guint i = 0; i < 32; ++i) {
        const RadioSAudioChannelDsp *channel = &dsp->config.channels[i];
        main_fed |= contributing[i];
        if (contributing[i] && channel->fx_slot >= 0 && channel->fx_send_db > -60) effect_fed[channel->fx_slot] = TRUE;
    }
    if (main_fed && dsp->config.master.fx_slot >= 0 && dsp->config.master.fx_send_db > -60)
        effect_fed[dsp->config.master.fx_slot] = TRUE;
    if (microphone_live && dsp->config.speech_fx_slot >= 0) effect_fed[dsp->config.speech_fx_slot] = TRUE;
    for (guint i = 0; i < 32; ++i) {
        const RadioSAudioChannelDsp *channel = &dsp->config.channels[i]; gint slot = channel->return_effect;
        if (slot >= 0 && effect_fed[slot] && dsp->config.effects[slot].enabled && dsp->config.effects[slot].send_db > -60 &&
            dsp->config.effects[slot].return_db > -60 && channel->return_db > -60 && !channel->mute && channel->gain_db > -60 &&
            dsp->crossfade[channel->crossfade_group] > 0.000000001) contributing[i] = TRUE;
    }
    dsp->contributing_solo_mask = 0;
    for (guint i = 0; i < 32; ++i) {
        const RadioSAudioChannelDsp *channel = &dsp->config.channels[i];
        solo |= channel->solo && contributing[i]; mic_assigned |= channel->source_deck == -2;
        if (channel->solo && contributing[i]) dsp->contributing_solo_mask |= ((guint32)1) << i;
        microphone_solo |= (dsp->config.speech_to_master ? channel->source_deck == -2 :
            dsp->config.speech_channel == (gint)i) && channel->solo && contributing[i];
    }
    /* mic_input is the device signal (meter, ducking detector, strip PFL);
     * mic is the on-air ingress after the gate ramp. */
    gdouble mic_input[2] = {clean(microphone[0]), clean(microphone[1])};
    gdouble mic[2] = {mic_input[0] * dsp->microphone_gain, mic_input[1] * dsp->microphone_gain};
    gboolean detected = microphone_gate && MAX(fabs(mic_input[0]), fabs(mic_input[1])) >= .005;
    if (detected) dsp->speech_hold = 4800; else if (dsp->speech_hold) --dsp->speech_hold;
    /* Designed ducking envelope: one-pole attack (~5 ms) and release (~50 ms). */
    gdouble target = dsp->config.speech_ducking && microphone_gate && dsp->speech_hold ? pow(10, dsp->config.speech_bed_db / 20) : 1;
    dsp->duck_gain += (target - dsp->duck_gain) * (target < dsp->duck_gain ? .004 : .0004);
    for (guint c = 0; c < 2; ++c) dsp->meters.microphone_peak[c] = MAX(dsp->meters.microphone_peak[c], (gfloat)fabs(mic_input[c]));
    if (dsp->config.speech_to_master) {
        if (!mic_assigned && microphone_live && !solo) { speech[0] += mic[0]; speech[1] += mic[1]; }
    } else if (microphone_live && dsp->config.speech_channel >= 0) {
        guint destination = (guint)dsp->config.speech_channel;
        incoming_speech[destination][0] += mic[0]; incoming_speech[destination][1] += mic[1];
    }
    /* This SEND belongs to the microphone, independent of channel assignments.
     * It contributes exactly once to the shared wet bus and never feeds itself. */
    gint speech_slot = dsp->config.speech_fx_slot;
    gboolean speech_send_active = microphone_live && !dsp->config.master.mute && (!solo || microphone_solo) &&
        speech_slot >= 0 && dsp->config.effects[speech_slot].enabled;
    if (dsp->speech_send_active != speech_send_active) {
        /* Only the SEND's own history restarts. Its returns pass strip and
         * MASTER EQ instances shared with other returned speech (MASTER
         * sampler); those filters are left running (no click, no gate reset). */
        memset(&dsp->speech_aux, 0, sizeof dsp->speech_aux);
        dsp->speech_send_active = speech_send_active;
    }
    if (speech_send_active) {
        guint slot = (guint)speech_slot; gdouble input[2], wet[2];
        for (guint c = 0; c < 2; ++c) {
            input[c] = mic[c] * slot_send[slot]; fx_send[slot][c] += input[c];
            dsp->meters.speech_fx_send_peak[c] = MAX(dsp->meters.speech_fx_send_peak[c], (gfloat)fabs(input[c]));
        }
        effect(&dsp->speech_aux, &dsp->config.effects[slot], input, wet);
        for (guint c = 0; c < 2; ++c) {
            gdouble returned = wet[c] * voice_wet * slot_return[slot]; bus_speech[slot][c] += returned;
            if (!dsp->explicit_returns) fx_return[slot][c] += returned;
        }
    }
    for (guint component = 0; component < 2; ++component) {
        if (external) music[component] += clean(external[component]);
        if (routed[RADIOS_AUDIO_MASTER_SAMPLER]) speech[component] += clean(sources[RADIOS_AUDIO_MASTER_SAMPLER * 2 + component]);
    }
    gboolean master_sent = FALSE;
    for (guint position = 0; position < 32; ++position) {
        guint i = dsp->channel_order[position];
        if (!master_sent && dsp->master_receivers[i]) {
            guint destination = (guint)dsp->config.master.send_channel;
            gdouble level = dsp->config.master.mute ? 0 : master_route_send * route_return[destination];
            for (guint component = 0; component < 2; ++component) {
                incoming_music[destination][component] += music[component] * level;
                incoming_speech[destination][component] += speech[component] * level;
            }
            master_sent = TRUE;
        }
        const RadioSAudioChannelDsp *channel = &dsp->config.channels[i]; ChannelState *state = &dsp->channels[i];
        gdouble gain = ramp_next(&state->gain), loudness = ramp_next(&state->loudness);
        gdouble pan[] = {ramp_next(&state->pan[0]), ramp_next(&state->pan[1])};
        gdouble crossfade = ramp_next(&state->crossfade), send_gain = ramp_next(&state->send), return_gain = ramp_next(&state->return_gain);
        strip_gain[i] = gain; strip_pan[i][0] = pan[0]; strip_pan[i][1] = pan[1]; strip_crossfade[i] = crossfade;
        strip_allowed[i] = !channel->mute && (!solo || channel->solo);
        guint sampler = RADIOS_AUDIO_SAMPLER_BASE + i; gdouble value[2] = {0}, voice[2] = {0}, cue[2] = {0};
        if (state->bypass_eq && channel->fx_slot < 0 && sources[sampler * 2] == 0 && sources[sampler * 2 + 1] == 0 &&
            incoming_music[i][0] == 0 && incoming_music[i][1] == 0 && incoming_speech[i][0] == 0 && incoming_speech[i][1] == 0 &&
            (channel->source_deck == -1 || (channel->source_deck == -2 ? mic_input[0] == 0 && mic_input[1] == 0 :
                sources[channel->source_deck * 2] == 0 && sources[channel->source_deck * 2 + 1] == 0))) continue;
        for (guint c = 0; c < 2; ++c) {
            /* A microphone strip's PFL hears the device signal; MAIN gets the gated ingress. */
            gdouble input = channel->source_deck == -2 ? (dsp->config.speech_to_master ? mic_input[c] : 0) :
                channel->source_deck < 0 ? 0 : clean(sources[channel->source_deck * 2 + (gint)c]) * loudness;
            cue[c] = input + clean(sources[sampler * 2 + c]) + incoming_music[i][c] + incoming_speech[i][c];
            gboolean source_main = channel->source_deck == -2 ? microphone_live : channel->source_deck >= 0 && routed[channel->source_deck];
            value[c] = (source_main && channel->source_deck != -2 ? input : 0) +
                (routed[sampler] ? clean(sources[sampler * 2 + c]) : 0) + incoming_music[i][c];
            voice[c] = (source_main && channel->source_deck == -2 ? input * dsp->microphone_gain : 0) + incoming_speech[i][c];
            for (guint band = 0; !state->bypass_eq && band < 3; ++band) {
                value[c] = filter(value[c], &state->eq[band], &state->states[0][band][c]);
                voice[c] = filter(voice[c], &state->eq[band], &state->states[2][band][c]);
                cue[c] = filter(cue[c], &state->eq[band], &state->states[1][band][c]);
            }
            value[c] *= gain * pan[c]; voice[c] *= gain * pan[c]; cue[c] *= gain * pan[c];
            channel_cue[i][c] = cue[c];
            if (!dsp->explicit_returns)
                dsp->meters.channel_peak[i][c] = MAX(dsp->meters.channel_peak[i][c], (gfloat)fabs(clean(cue[c])));
            if (channel->pfl) pfl[c] += cue[c];
        }
        gboolean allowed = !channel->mute && (!solo || channel->solo);
        gdouble wet[2] = {0}, wet_voice[2] = {0};
        if (channel->fx_slot >= 0) {
            guint slot = (guint)channel->fx_slot; gdouble send[2], speech_send[2];
            for (guint c = 0; c < 2; ++c) {
                send[c] = allowed ? value[c] * send_gain * slot_send[slot] * crossfade : 0;
                speech_send[c] = allowed ? voice[c] * send_gain * slot_send[slot] * crossfade : 0;
                dsp->voice_fed |= speech_send[c] != 0;
                fx_send[slot][c] += send[c] + speech_send[c];
                dsp->meters.channel_fx_send_peak[i][c] = MAX(dsp->meters.channel_fx_send_peak[i][c], (gfloat)fabs(clean(send[c] + speech_send[c])));
            }
            effect(&state->effect, &dsp->config.effects[slot], send, wet);
            effect(&state->speech_effect, &dsp->config.effects[slot], speech_send, wet_voice);
            for (guint c = 0; c < 2; ++c) {
                wet_voice[c] *= voice_wet;
                bus_music[slot][c] += allowed ? wet[c] * slot_return[slot] : 0;
                bus_speech[slot][c] += allowed ? wet_voice[c] * slot_return[slot] : 0;
                wet[c] *= return_gain * slot_return[slot]; wet_voice[c] *= return_gain * slot_return[slot];
                if (!dsp->explicit_returns) fx_return[slot][c] += allowed ? wet[c] + wet_voice[c] : 0;
                if (channel->return_effect != -2) wet[c] = wet_voice[c] = 0;
            }
        }
        if (allowed) for (guint c = 0; c < 2; ++c) {
            gdouble contribution = value[c] * crossfade + wet[c];
            gdouble spoken = voice[c] * crossfade + wet_voice[c];
            gdouble master_level = dsp->config.master.return_channel == (gint)i ? route_send[i] * master_route_return : 1;
            music[c] += contribution * master_level; speech[c] += spoken * master_level;
            if (dsp->capture_channels) dsp->channel_output[i][c] = (gfloat)clean(contribution + spoken);
            for (guint route = dsp->route_offsets[i]; route < dsp->route_offsets[i + 1]; ++route) {
                guint destination = dsp->route_targets[route];
                gdouble level = route_send[i] * route_return[destination];
                incoming_music[destination][c] += contribution * level;
                incoming_speech[destination][c] += spoken * level;
            }
        }
    }
    for (guint c = 0; c < 2; ++c) {
        gdouble main = music[c] * dsp->duck_gain + speech[c];
        for (guint band = 0; band < 3; ++band) main = filter(main, &dsp->master_eq[band], &dsp->master_states[band][c]);
        main_output[c] = (gfloat)clean(main);
    }
    if (!dsp->explicit_returns && dsp->config.master.fx_slot >= 0) {
        guint slot = (guint)dsp->config.master.fx_slot; gdouble input[2], wet[2];
        for (guint c = 0; c < 2; ++c) {
            input[c] = main_output[c] * master_strip_send * slot_send[slot];
            fx_send[slot][c] += input[c];
            dsp->meters.master_fx_send_peak[c] = MAX(dsp->meters.master_fx_send_peak[c], (gfloat)fabs(clean(input[c])));
        }
        effect(&dsp->master_aux, &dsp->config.effects[slot], input, wet);
        for (guint c = 0; c < 2; ++c) {
            wet[c] *= master_strip_return * slot_return[slot];
            fx_return[slot][c] += wet[c]; main_output[c] = (gfloat)clean(main_output[c] + wet[c]);
        }
    }
    if (dsp->explicit_returns) {
        /* Complete every wet bus before consuming any explicit RETURN. Returned
         * audio is excluded from SEND inputs, so this path cannot feed itself. */
        if (dsp->config.master.fx_slot >= 0) {
            guint slot = (guint)dsp->config.master.fx_slot;
            gdouble input[2], voice_input[2], wet[2], wet_voice[2];
            for (guint c = 0; c < 2; ++c) {
                input[c] = music[c]; voice_input[c] = speech[c];
                for (guint band = 0; band < 3; ++band) {
                    input[c] = filter(input[c], &dsp->master_eq[band], &dsp->master_aux_states[0][band][c]);
                    voice_input[c] = filter(voice_input[c], &dsp->master_eq[band], &dsp->master_aux_states[1][band][c]);
                }
                input[c] *= master_strip_send * slot_send[slot];
                voice_input[c] *= master_strip_send * slot_send[slot];
                fx_send[slot][c] += input[c] + voice_input[c];
                dsp->meters.master_fx_send_peak[c] = MAX(dsp->meters.master_fx_send_peak[c], (gfloat)fabs(clean(input[c] + voice_input[c])));
            }
            effect(&dsp->master_aux, &dsp->config.effects[slot], input, wet);
            effect(&dsp->master_aux_speech, &dsp->config.effects[slot], voice_input, wet_voice);
            for (guint c = 0; c < 2; ++c) {
                wet[c] *= slot_return[slot]; wet_voice[c] *= slot_return[slot];
                bus_music[slot][c] += wet[c]; bus_speech[slot][c] += wet_voice[c];
                if (dsp->config.master.return_effect == -2)
                    main_output[c] = (gfloat)clean(main_output[c] + (wet[c] * dsp->duck_gain + wet_voice[c]) * master_strip_return);
            }
        }
        gdouble returned_music[2] = {0}, returned_speech[2] = {0};
        for (guint i = 0; i < 32; ++i) {
            const RadioSAudioChannelDsp *channel = &dsp->config.channels[i]; ChannelState *state = &dsp->channels[i];
            if (channel->return_effect >= 0) for (guint c = 0; c < 2; ++c) {
                guint slot = (guint)channel->return_effect;
                gdouble value = bus_music[slot][c] * route_return[i], voice = bus_speech[slot][c] * route_return[i];
                gdouble cue = value + voice;
                for (guint band = 0; !state->bypass_eq && band < 3; ++band) {
                    value = filter(value, &state->eq[band], &state->states[3][band][c]);
                    voice = filter(voice, &state->eq[band], &state->states[4][band][c]);
                    cue = filter(cue, &state->eq[band], &state->states[5][band][c]);
                }
                gdouble level = strip_gain[i] * strip_pan[i][c];
                channel_cue[i][c] += cue * level;
                if (channel->pfl) pfl[c] += cue * level;
                if (strip_allowed[i]) {
                    returned_music[c] += value * level * strip_crossfade[i];
                    returned_speech[c] += voice * level * strip_crossfade[i];
                }
            }
            for (guint c = 0; c < 2; ++c)
                dsp->meters.channel_peak[i][c] = MAX(dsp->meters.channel_peak[i][c], (gfloat)fabs(clean(channel_cue[i][c])));
        }
        for (guint c = 0; c < 2; ++c) {
            if (dsp->config.master.return_effect >= 0) {
                guint slot = (guint)dsp->config.master.return_effect;
                returned_music[c] += bus_music[slot][c] * master_route_return;
                returned_speech[c] += bus_speech[slot][c] * master_route_return;
            }
            for (guint band = 0; band < 3; ++band) {
                returned_music[c] = filter(returned_music[c], &dsp->master_eq[band], &dsp->master_return_states[0][band][c]);
                returned_speech[c] = filter(returned_speech[c], &dsp->master_eq[band], &dsp->master_return_states[1][band][c]);
            }
            main_output[c] = (gfloat)clean(main_output[c] + returned_music[c] * dsp->duck_gain + returned_speech[c]);
            for (guint slot = 0; slot < 5; ++slot) fx_return[slot][c] = bus_music[slot][c] + bus_speech[slot][c];
        }
    }
    gdouble master_input[2] = {main_output[0] * master_send, main_output[1] * master_send}, master_wet[2];
    effect(&dsp->master_effect, &dsp->config.master_fx, master_input, master_wet);
    for (guint c = 0; c < 2; ++c) {
        master_wet[c] *= master_return;
        main_output[c] = dsp->config.master.mute ? 0 : (gfloat)clean(main_output[c] + master_wet[c]);
        dsp->meters.master_peak[c] = MAX(dsp->meters.master_peak[c], fabsf(main_output[c]));
        cue_output[c] = cue_mix ? (gfloat)clean(pfl[c]) : 0;
        for (guint slot = 0; slot < 5; ++slot) {
            dsp->meters.fx_send_peak[slot][c] = MAX(dsp->meters.fx_send_peak[slot][c], (gfloat)fabs(clean(fx_send[slot][c])));
            dsp->meters.fx_return_peak[slot][c] = MAX(dsp->meters.fx_return_peak[slot][c], (gfloat)fabs(clean(fx_return[slot][c])));
            if (cue_mix && dsp->config.effects[slot].cue) {
                gint path = dsp->config.effects[slot].monitor_path;
                cue_output[c] += (gfloat)clean((path != 1 ? fx_send[slot][c] : 0) + (path != 0 ? fx_return[slot][c] : 0));
            }
        }
        if (cue_mix && dsp->config.master_fx.cue) {
            gint path = dsp->config.master_fx.monitor_path;
            cue_output[c] += (gfloat)clean((path != 1 ? master_input[c] : 0) + (path != 0 ? master_wet[c] : 0));
        }
    }
    dsp->meters.speech_gain = dsp->duck_gain;
}
gboolean radios_audio_transition_validate(const RadioSAudioTransition *transition, GError **error)
{
    if (!transition || !transition->a_frames || transition->a_frames > G_GUINT64_CONSTANT(4147200000) ||
        transition->overlap_frames > transition->a_frames || transition->overlap_frames > 1440000 ||
        transition->fade_in_frames > 1440000 || !range(transition->duck_db, -30, 0))
        return invalid(error, "Invalid sample-based transition envelope");
    return TRUE;
}
void radios_audio_transition_gains(const RadioSAudioTransition *transition, guint64 frame, gdouble *a, gdouble *b)
{
    guint64 start = transition->a_frames - transition->overlap_frames;
    *a = frame < transition->a_frames ? 1 : 0; *b = 0;
    if (frame >= start) {
        guint64 elapsed = frame - start;
        if (transition->overlap_frames && frame < transition->a_frames) {
            *a = cos(G_PI / 2 * (gdouble)elapsed / transition->overlap_frames);
            gdouble ramp = MIN(1.0, (gdouble)elapsed / MIN(240u, transition->overlap_frames));
            *a *= pow(10, transition->duck_db * ramp / 20);
        }
        *b = transition->fade_in_frames ? sin(G_PI / 2 * MIN(1.0, (gdouble)elapsed / transition->fade_in_frames)) : 1;
    }
}

/* Curve evaluation and shape validation are pure and live in radios_core
 * (src/core/transition_preset.c) so planner-only programs link without the
 * audio library; the voice below is the render-side processor. */
struct RadioSAudioShapeVoice {
    EffectState effect;
    FilterState bass[2];
    Coefficients bass_coefficients;
    gdouble bass_db, tone[2];
    gboolean bass_cached;
};
RadioSAudioShapeVoice *radios_audio_shape_voice_new(void)
{ return g_new0(RadioSAudioShapeVoice, 1); }
void radios_audio_shape_voice_free(RadioSAudioShapeVoice *voice)
{ g_free(voice); }
void radios_audio_shape_voice_reset(RadioSAudioShapeVoice *voice)
{ if (voice) memset(voice, 0, sizeof *voice); }
void radios_audio_shape_voice_process(RadioSAudioShapeVoice *voice, const RadioSAudioTransitionShape *shape,
    guint side, gdouble progress, gfloat stereo[2])
{
    if (!voice || !shape || !shape->enabled || side > 1 || !stereo) return;
    gdouble p = isfinite(progress) ? CLAMP(progress, 0, 1) : 0;
    gdouble bass = radios_audio_curve_value(&shape->bass[side], p);
    /* 0.1 dB steps keep the shelf recomputation rare and inaudible. */
    gdouble db = floor(-30 * (1 - bass) * 10 + .5) / 10;
    if (db < 0) {
        if (!voice->bass_cached || voice->bass_db != db) { voice->bass_coefficients = coefficients(0, db); voice->bass_db = db; voice->bass_cached = TRUE; }
        for (guint c = 0; c < 2; ++c) stereo[c] = (gfloat)filter(stereo[c], &voice->bass_coefficients, &voice->bass[c]);
    } else voice->bass[0] = voice->bass[1] = (FilterState){0, 0};
    gdouble tone = radios_audio_curve_value(&shape->tone[side], p);
    if (tone > 0 && shape->tone_kind[side] != RADIOS_AUDIO_TONE_NONE) {
        gboolean low = shape->tone_kind[side] == RADIOS_AUDIO_TONE_LOWPASS;
        gdouble cutoff = low ? 18000 * pow(300.0 / 18000, tone) : 30 * pow(8000.0 / 30, tone);
        gdouble alpha = 1 - exp(-2 * G_PI * cutoff / RATE);
        for (guint c = 0; c < 2; ++c) {
            voice->tone[c] += alpha * (stereo[c] - voice->tone[c]);
            gdouble wet = low ? voice->tone[c] : stereo[c] - voice->tone[c];
            stereo[c] = (gfloat)(stereo[c] * (1 - tone) + wet * tone);
        }
    } else voice->tone[0] = voice->tone[1] = 0;
    if (shape->effect >= 0 && shape->effect < (gint)RADIOS_FX_COUNT && side == shape->effect_side) {
        RadioSAudioFx settings = { .type = (RadioSAudioFxType)shape->effect, .enabled = TRUE, .depth = shape->effect_depth, .tempo_bpm = shape->effect_bpm };
        gdouble wet = radios_audio_curve_value(&shape->effect_mix, p), input[2] = {stereo[0], stereo[1]}, output[2];
        effect(&voice->effect, &settings, input, output);
        gboolean tail = shape->effect <= RADIOS_FX_REVERB; /* delay, echo, spiral and reverb add to the dry signal */
        for (guint c = 0; c < 2; ++c) stereo[c] = (gfloat)(tail ? input[c] + output[c] * wet : input[c] * (1 - wet) + output[c] * wet);
    }
}
