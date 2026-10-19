#include "radios/loudness.h"
#include <math.h>

#define LOUDNESS_MAX_CHANNELS 32u
typedef struct { gdouble b0, b1, b2, a1, a2, x1, x2, y1, y2; } Biquad;
struct RadioSLoudnessMeter {
    guint channels, hop, filled;
    gdouble weights[LOUDNESS_MAX_CHANNELS], sum;
    Biquad shelf[LOUDNESS_MAX_CHANNELS], highpass[LOUDNESS_MAX_CHANNELS];
    GArray *quarters; /* weighted mean square of each 100 ms quarter block */
};
static gdouble biquad(Biquad *filter, gdouble x)
{
    gdouble y = filter->b0 * x + filter->b1 * filter->x1 + filter->b2 * filter->x2 - filter->a1 * filter->y1 - filter->a2 * filter->y2;
    filter->x2 = filter->x1; filter->x1 = x; filter->y2 = filter->y1; filter->y1 = y; return y;
}
RadioSLoudnessMeter *radios_loudness_meter_new(guint sample_rate, guint channels)
{
    if (sample_rate < 8000 || !channels || channels > LOUDNESS_MAX_CHANNELS) return NULL;
    RadioSLoudnessMeter *meter = g_new0(RadioSLoudnessMeter, 1);
    meter->channels = channels; meter->hop = sample_rate / 10; meter->quarters = g_array_new(FALSE, FALSE, sizeof(gdouble));
    /* K-weighting at any rate: the BS.1770 high-shelf pre-filter and RLB
     * high-pass, derived from their analogue prototypes. */
    gdouble k = tan(G_PI * 1681.974450955533 / sample_rate), q = 0.7071752369554196;
    gdouble vh = pow(10, 3.999843853973347 / 20), vb = pow(vh, 0.4996667741545416), a0 = 1 + k / q + k * k;
    Biquad shelf = {(vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
        2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0, 0, 0, 0, 0};
    k = tan(G_PI * 38.13547087602444 / sample_rate); q = 0.5003270373238773; a0 = 1 + k / q + k * k;
    Biquad highpass = {1, -2, 1, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0, 0, 0, 0, 0};
    for (guint channel = 0; channel < channels; ++channel) {
        meter->shelf[channel] = shelf; meter->highpass[channel] = highpass;
        /* Surround channels of 5.1 weigh 1.41 and LFE 0. A mono file plays on
         * both speakers, so it counts as dual mono like its stereo copy. */
        meter->weights[channel] = channels == 1 ? 2 : channels == 6 ? (channel == 3 ? 0 : channel >= 4 ? 1.41 : 1) : 1;
    }
    return meter;
}
void radios_loudness_meter_push_frame(RadioSLoudnessMeter *meter, const gfloat *samples)
{
    if (!meter || !samples) return;
    gdouble energy = 0;
    for (guint channel = 0; channel < meter->channels; ++channel) {
        gdouble value = biquad(&meter->highpass[channel], biquad(&meter->shelf[channel], samples[channel]));
        energy += meter->weights[channel] * value * value;
    }
    meter->sum += energy;
    if (++meter->filled == meter->hop) {
        gdouble mean = meter->sum / meter->hop; g_array_append_val(meter->quarters, mean);
        meter->sum = 0; meter->filled = 0;
    }
}
static gdouble block_loudness(gdouble mean) { return -0.691 + 10 * log10(mean); }
gboolean radios_loudness_meter_finish(RadioSLoudnessMeter *meter, gdouble *lufs)
{
    if (!meter || !lufs || meter->quarters->len < 4) return FALSE;
    guint blocks = meter->quarters->len - 3; gdouble *z = g_new(gdouble, blocks), total = 0; guint kept = 0;
    for (guint index = 0; index < blocks; ++index) {
        gdouble *quarter = &g_array_index(meter->quarters, gdouble, index);
        z[index] = (quarter[0] + quarter[1] + quarter[2] + quarter[3]) / 4;
        if (z[index] > 0 && block_loudness(z[index]) > -70) { total += z[index]; ++kept; }
    }
    gboolean measured = kept > 0;
    if (measured) {
        gdouble relative = block_loudness(total / kept) - 10, gated = 0; guint count = 0;
        for (guint index = 0; index < blocks; ++index)
            if (z[index] > 0 && block_loudness(z[index]) > -70 && block_loudness(z[index]) > relative) { gated += z[index]; ++count; }
        *lufs = block_loudness(gated / MAX(1u, count));
        measured = count > 0 && isfinite(*lufs);
    }
    g_free(z); return measured;
}
void radios_loudness_meter_free(RadioSLoudnessMeter *meter)
{
    if (!meter) return;
    g_array_unref(meter->quarters); g_free(meter);
}
JsonObject *radios_loudness_json(const RadioSLoudness *loudness)
{
    JsonObject *object = json_object_new();
    json_object_set_int_member(object, "schema", 1);
    json_object_set_string_member(object, "algorithm", RADIOS_LOUDNESS_ALGORITHM);
    if (loudness->measured) json_object_set_double_member(object, "integrated_lufs", loudness->integrated_lufs);
    else json_object_set_null_member(object, "integrated_lufs");
    return object;
}
gboolean radios_loudness_read(JsonObject *analysis, RadioSLoudness *loudness)
{
    if (!loudness) return FALSE;
    *loudness = (RadioSLoudness){0};
    JsonNode *node = analysis ? json_object_get_member(analysis, "loudness") : NULL;
    JsonObject *object = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    JsonNode *schema = object ? json_object_get_member(object, "schema") : NULL, *algorithm = object ? json_object_get_member(object, "algorithm") : NULL;
    if (!schema || !JSON_NODE_HOLDS_VALUE(schema) || json_node_get_value_type(schema) != G_TYPE_INT64 || json_node_get_int(schema) != 1 ||
        !algorithm || !JSON_NODE_HOLDS_VALUE(algorithm) || json_node_get_value_type(algorithm) != G_TYPE_STRING ||
        g_strcmp0(json_node_get_string(algorithm), RADIOS_LOUDNESS_ALGORITHM)) return FALSE;
    JsonNode *value = json_object_get_member(object, "integrated_lufs"), *peak = json_object_get_member(analysis, "peak");
    if (peak && JSON_NODE_HOLDS_VALUE(peak) && (json_node_get_value_type(peak) == G_TYPE_DOUBLE || json_node_get_value_type(peak) == G_TYPE_INT64))
        loudness->peak = json_node_get_double(peak);
    if (!isfinite(loudness->peak) || loudness->peak < 0) loudness->peak = 0;
    if (value && JSON_NODE_HOLDS_VALUE(value) && (json_node_get_value_type(value) == G_TYPE_DOUBLE || json_node_get_value_type(value) == G_TYPE_INT64)) {
        loudness->integrated_lufs = json_node_get_double(value);
        loudness->measured = isfinite(loudness->integrated_lufs) && loudness->integrated_lufs >= -70 && loudness->integrated_lufs <= 10;
        if (!loudness->measured) return FALSE;
    } else if (!value || !JSON_NODE_HOLDS_NULL(value)) return FALSE;
    return TRUE;
}
gdouble radios_loudness_gain_db(const RadioSLoudness *loudness, gdouble target_lufs)
{
    if (!loudness || !loudness->measured || !isfinite(target_lufs)) return 0;
    gdouble gain = CLAMP(target_lufs - loudness->integrated_lufs, -24, 12);
    if (gain > 0 && loudness->peak > 0) gain = MIN(gain, MAX(0, -1 - 20 * log10(loudness->peak)));
    return gain;
}
