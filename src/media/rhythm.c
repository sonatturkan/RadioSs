#include "radios/rhythm.h"
#include <math.h>
#include <string.h>

#define RHYTHM_BINS 72000u /* six minutes at 200 bins/s */
#define RHYTHM_WINDOW 24000u /* two minutes */
#define PHASE_BINS 64u
struct RadioSRhythmAnalyzer {
    guint sample_rate, hop, frames, count;
    gboolean banded;
    gdouble sum, low_sum, energy[RHYTHM_BINS], low[RHYTHM_BINS];
    /* 2nd-order Butterworth low-pass at 150 Hz isolates kick attacks. */
    gdouble b0, b1, b2, a1, a2, x1, x2, y1, y2;
};
RadioSRhythmAnalyzer *radios_rhythm_analyzer_new(guint sample_rate)
{
    if (!sample_rate) return NULL;
    RadioSRhythmAnalyzer *analyzer = g_new0(RadioSRhythmAnalyzer, 1);
    analyzer->sample_rate = sample_rate; analyzer->hop = MAX(1u, sample_rate / 200);
    gdouble w0 = 2 * G_PI * MIN(150.0, sample_rate * 0.45) / sample_rate, alpha = sin(w0) / G_SQRT2 /* Q = 1/sqrt(2) */, cosine = cos(w0), a0 = 1 + alpha;
    analyzer->b0 = (1 - cosine) / 2 / a0; analyzer->b1 = (1 - cosine) / a0; analyzer->b2 = analyzer->b0;
    analyzer->a1 = -2 * cosine / a0; analyzer->a2 = (1 - alpha) / a0;
    return analyzer;
}
static void push(RadioSRhythmAnalyzer *analyzer, gdouble energy, gdouble low)
{
    analyzer->sum += energy; analyzer->low_sum += low;
    if (++analyzer->frames == analyzer->hop) {
        analyzer->energy[analyzer->count] = analyzer->sum / analyzer->hop;
        analyzer->low[analyzer->count++] = analyzer->low_sum / analyzer->hop;
        analyzer->sum = analyzer->low_sum = 0; analyzer->frames = 0;
    }
}
void radios_rhythm_analyzer_push(RadioSRhythmAnalyzer *analyzer, gdouble energy)
{
    if (!analyzer || analyzer->count == RHYTHM_BINS) return;
    push(analyzer, energy, energy);
}
void radios_rhythm_analyzer_push_frame(RadioSRhythmAnalyzer *analyzer, const gfloat *samples, guint channels)
{
    if (!analyzer || !samples || !channels || analyzer->count == RHYTHM_BINS) return;
    gdouble energy = 0, mono = 0;
    for (guint channel = 0; channel < channels; ++channel) { energy += (gdouble)samples[channel] * samples[channel]; mono += samples[channel]; }
    mono /= channels;
    gdouble low = analyzer->b0 * mono + analyzer->b1 * analyzer->x1 + analyzer->b2 * analyzer->x2 - analyzer->a1 * analyzer->y1 - analyzer->a2 * analyzer->y2;
    analyzer->x2 = analyzer->x1; analyzer->x1 = mono; analyzer->y2 = analyzer->y1; analyzer->y1 = low;
    analyzer->banded = TRUE; push(analyzer, energy / channels, low * low);
}
/* Positive log-energy attacks with slow swells removed, unit variance. */
static gdouble *attack_envelope(const gdouble *energy, guint n, gdouble rate)
{
    gdouble *level = g_new0(gdouble, n), *onset = g_new0(gdouble, n), *flux = g_new0(gdouble, n), loudest = -300;
    for (guint i = 0; i < n; ++i) { level[i] = 10 * log10(energy[i] + 1e-12); loudest = MAX(loudest, level[i]); }
    for (guint i = 3; i < n; ++i)
        if (level[i] >= loudest - 60) onset[i] = MIN(30, MAX(0, level[i] - (level[i - 1] + level[i - 2] + level[i - 3]) / 3));
    guint half = (guint)lround(rate * 0.25); gdouble running = 0, mean = 0, square = 0;
    for (guint i = 0; i < MIN(n, half); ++i) running += onset[i];
    for (guint i = 0; i < n; ++i) {
        if (i + half < n) running += onset[i + half];
        if (i > half) running -= onset[i - half - 1];
        guint lo = i > half ? i - half : 0, hi = MIN(n - 1, i + half);
        flux[i] = MAX(0, onset[i] - running / (hi - lo + 1)); mean += flux[i];
    }
    mean /= n;
    for (guint i = 0; i < n; ++i) square += (flux[i] - mean) * (flux[i] - mean);
    gdouble deviation = sqrt(square / n);
    for (guint i = 0; i < n; ++i) flux[i] = deviation > 1e-9 ? flux[i] / deviation : 0;
    g_free(level); g_free(onset); return flux;
}
static gdouble autocorrelation(const gdouble *x, guint n, guint lag)
{
    long double product = 0, first = 0, second = 0;
    for (guint i = lag; i < n; ++i) { product += x[i] * x[i - lag]; first += x[i] * x[i]; second += x[i - lag] * x[i - lag]; }
    return first > 0 && second > 0 ? (gdouble)(product / sqrtl(first * second)) : 0;
}
/* Folds the envelope onto one beat period. A constant tempo keeps every attack
 * in one phase bin; the peak-to-mean ratio measures that salience. */
static gdouble fold(const gdouble *x, guint n, gdouble period, gdouble *phase)
{
    gdouble histogram[PHASE_BINS] = {0}, peak = 0, mean = 0, advance = PHASE_BINS / period, position = 0; guint best = 0;
    for (guint i = 0; i < n; ++i, position += advance) {
        if (position >= PHASE_BINS) position -= PHASE_BINS;
        guint lower = MIN((guint)position, PHASE_BINS - 1); gdouble share = position - lower;
        histogram[lower] += x[i] * (1 - share); histogram[(lower + 1) % PHASE_BINS] += x[i] * share;
    }
    for (guint b = 0; b < PHASE_BINS; ++b) { mean += histogram[b]; if (histogram[b] > peak) { peak = histogram[b]; best = b; } }
    mean /= PHASE_BINS;
    gdouble l = histogram[(best + PHASE_BINS - 1) % PHASE_BINS], r = histogram[(best + 1) % PHASE_BINS], d = l - 2 * peak + r;
    gdouble shift = d < 0 ? CLAMP(0.5 * (l - r) / d, -0.5, 0.5) : 0;
    *phase = fmod((best + shift + PHASE_BINS) * period / PHASE_BINS, period);
    return mean > 0 ? peak / mean : 0;
}
static gdouble tempo_prior(gdouble bpm) { gdouble x = log2(bpm / 120.0); return exp(-0.5 * x * x / (0.9 * 0.9)); }
/* The bar starts where section energy changes and attacks are strongest. */
static guint bar_phase(const RadioSRhythmAnalyzer *analyzer, const gdouble *kick, guint n, gdouble first, gdouble period)
{
    gdouble novelty[4] = {0}, attack[4] = {0}, novelty_max = 1e-9, attack_max = 1e-9, best = -1; guint bar = 0;
    for (guint k = 0; k < 4; ++k) {
        gdouble previous = 0; guint bars = 0;
        for (gdouble t = first + k * period; t + 4 * period < n; t += 4 * period, ++bars) {
            gdouble energy = 0; guint count = 0;
            for (glong i = lround(t); i < lround(t + 4 * period) && i < (glong)n; ++i, ++count) energy += analyzer->energy[i];
            gdouble level = 10 * log10(energy / MAX(1u, count) + 1e-12);
            if (bars) novelty[k] += fabs(level - previous);
            previous = level;
            glong at = lround(t); if (at >= 0 && at < (glong)n) attack[k] += kick[at];
        }
        novelty_max = MAX(novelty_max, novelty[k]); attack_max = MAX(attack_max, attack[k]);
    }
    for (guint k = 0; k < 4; ++k) {
        gdouble score = novelty[k] / novelty_max + 0.5 * attack[k] / attack_max;
        if (score > best) { best = score; bar = k; }
    }
    return bar;
}
typedef struct { gdouble bpm, period, first, confidence; } TempoEstimate;
/* Tempo and beat phase of envelope bins [offset, offset + count). The phase
 * is returned relative to bin zero of the whole analysis. */
static TempoEstimate estimate(const gdouble *mix_all, const gdouble *kick_all, guint offset, guint count, gdouble rate)
{
    TempoEstimate result = {0};
    const gdouble *mix = mix_all + offset, *kick = kick_all + offset;
    guint n = count;
    /* Coarse tempo: onset autocorrelation with a mild 120 BPM preference. */
    guint min_lag = (guint)floor(rate * 60 / 200), max_lag = (guint)ceil(rate * 60 / 60), top = MIN(n - 1, max_lag * 2 + 2);
    gdouble *ac = g_new0(gdouble, top + 1), best_score = -1; guint best_lag = 0;
    for (guint lag = 1; lag <= top; ++lag) ac[lag] = autocorrelation(mix, n, lag);
    for (guint lag = min_lag; lag <= max_lag; ++lag) {
        gdouble score = (ac[lag] + (2 * lag <= top ? 0.5 * ac[2 * lag] : 0)) * tempo_prior(rate * 60 / lag);
        if (score > best_score) { best_score = score; best_lag = lag; }
    }
    /* Fine tempo and phase: the period whose folded attack envelope is
     * sharpest. Kicks mark the beat in dance music; sparse hip-hop kicks are
     * completed by the backbeat in the full-band attacks. */
    gdouble period = best_lag, first = 0, salience = 0;
    for (guint pass = 0; best_lag && pass < 2; ++pass) {
        gdouble centre = period, span = pass ? 0.04 : centre * 0.035, step = pass ? 0.002 : 0.02;
        for (gdouble candidate = centre - span; candidate <= centre + span; candidate += step) {
            gdouble phase = 0, value = fold(kick, n, candidate, &phase), mixed_phase = 0, mixed = fold(mix, n, candidate, &mixed_phase);
            if (mixed > value) { value = mixed; phase = mixed_phase; }
            if (value > salience) { salience = value; period = candidate; first = phase; }
        }
    }
    gdouble correlation = best_lag ? ac[(guint)CLAMP(lround(period), 1, (glong)top)] : 0;
    gdouble tempo_part = CLAMP((correlation - 0.06) / 0.30, 0, 1), phase_part = CLAMP((salience - 2.0) / 2.0, 0, 1);
    result.confidence = sqrt(tempo_part * phase_part); result.bpm = period > 0 ? rate * 60 / period : 0;
    /* Refinement may step just outside 60-200 BPM; fold such tempos by octaves. */
    while (result.bpm > 0 && result.bpm < 60) { result.bpm *= 2; period /= 2; first = fmod(first, period); }
    while (result.bpm > 200) { result.bpm /= 2; period *= 2; }
    result.period = period; result.first = period > 0 ? fmod(first + offset, period) : 0;
    g_free(ac); return result;
}
RadioSRhythmAnalysis radios_rhythm_analyzer_finish(RadioSRhythmAnalyzer *analyzer)
{
    RadioSRhythmAnalysis result = {0};
    if (!analyzer) return result;
    guint n = analyzer->count;
    /* The stored grid schema caps the analysed span at two minutes. */
    result.analyzed_ms = (guint)MIN((guint64)n * analyzer->hop * 1000 / analyzer->sample_rate, 120000);
    if (n < 1000) return result;
    gdouble rate = (gdouble)analyzer->sample_rate / analyzer->hop;
    gdouble *full = attack_envelope(analyzer->energy, n, rate), *low = analyzer->banded ? attack_envelope(analyzer->low, n, rate) : NULL;
    gdouble *mix = g_new0(gdouble, n), *kick = g_new0(gdouble, n);
    for (guint i = 0; i < n; ++i) { mix[i] = full[i] + (low ? low[i] : full[i]); kick[i] = (low ? low[i] : full[i]) + 0.35 * full[i]; }
    /* Every song gets several looks: its opening two minutes (live drums
     * drift less there), the middle two minutes (past a beatless intro) and
     * up to six minutes at once (sparse beats repeat more often). The most
     * dependable estimate wins. */
    guint window = MIN(n, RHYTHM_WINDOW), offsets[3] = {0, (n - window) / 2, 0}, counts[3] = {window, window, n};
    TempoEstimate best = {0};
    for (guint i = 0; i < G_N_ELEMENTS(offsets); ++i) {
        if (i && counts[i] == counts[0] && offsets[i] == offsets[0]) continue;
        TempoEstimate candidate = estimate(mix, kick, offsets[i], counts[i], rate);
        if (candidate.bpm >= 60 && candidate.bpm <= 200 && candidate.confidence > best.confidence + 1e-9) best = candidate;
    }
    if (best.bpm >= 60 && best.bpm <= 200 && best.confidence >= 0.45) {
        guint bar = bar_phase(analyzer, kick, n, best.first, best.period);
        result.bpm = best.bpm; result.confidence = MIN(1, best.confidence);
        result.first_beat_ms = MAX(0, best.first * 1000 / rate);
        result.downbeat_ms = MAX(0, fmod(best.first + bar * best.period, 4 * best.period) * 1000 / rate); result.has_downbeat = TRUE;
    }
    g_free(full); g_free(low); g_free(mix); g_free(kick);
    return result;
}
void radios_rhythm_analyzer_free(RadioSRhythmAnalyzer *analyzer) { g_free(analyzer); }
JsonObject *radios_rhythm_analysis_json(const RadioSRhythmAnalysis *analysis)
{
    JsonObject *object = json_object_new();
    json_object_set_int_member(object, "schema", 1);
    json_object_set_string_member(object, "algorithm", analysis->manual ? RADIOS_RHYTHM_ALGORITHM_MANUAL : RADIOS_RHYTHM_ALGORITHM);
    json_object_set_double_member(object, "bpm", analysis->bpm); json_object_set_double_member(object, "first_beat_ms", analysis->first_beat_ms);
    json_object_set_double_member(object, "confidence", analysis->confidence); json_object_set_int_member(object, "analyzed_ms", analysis->analyzed_ms);
    if (analysis->has_downbeat && analysis->bpm > 0) json_object_set_double_member(object, "downbeat_ms", analysis->downbeat_ms);
    return object;
}
static gboolean number_member(JsonObject *object, const gchar *name, gdouble *value)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || (json_node_get_value_type(node) != G_TYPE_DOUBLE && json_node_get_value_type(node) != G_TYPE_INT64)) return FALSE;
    *value = json_node_get_double(node); return isfinite(*value);
}
static const gchar *algorithm_of(JsonObject *object)
{
    if (!object || !json_object_has_member(object, "schema") || !json_object_has_member(object, "algorithm")) return NULL;
    JsonNode *schema = json_object_get_member(object, "schema"), *algorithm = json_object_get_member(object, "algorithm");
    if (!JSON_NODE_HOLDS_VALUE(schema) || json_node_get_value_type(schema) != G_TYPE_INT64 || json_node_get_int(schema) != 1 ||
        !JSON_NODE_HOLDS_VALUE(algorithm) || json_node_get_value_type(algorithm) != G_TYPE_STRING) return NULL;
    const gchar *name = json_node_get_string(algorithm);
    return !g_strcmp0(name, RADIOS_RHYTHM_ALGORITHM_LEGACY) || !g_strcmp0(name, RADIOS_RHYTHM_ALGORITHM_V2) ||
        !g_strcmp0(name, RADIOS_RHYTHM_ALGORITHM) || !g_strcmp0(name, RADIOS_RHYTHM_ALGORITHM_MANUAL) ? name : NULL;
}
gboolean radios_rhythm_analysis_current(JsonObject *object)
{
    const gchar *name = algorithm_of(object);
    return name && (!g_strcmp0(name, RADIOS_RHYTHM_ALGORITHM) || !g_strcmp0(name, RADIOS_RHYTHM_ALGORITHM_MANUAL)) &&
        json_object_has_member(object, "analyzed_ms");
}
gboolean radios_rhythm_analysis_read(JsonObject *object, RadioSRhythmAnalysis *analysis)
{
    if (!analysis) return FALSE;
    memset(analysis, 0, sizeof(*analysis));
    const gchar *algorithm = algorithm_of(object);
    if (!algorithm) return FALSE;
    const gchar *names[] = {"bpm", "first_beat_ms", "confidence", "analyzed_ms"};
    gdouble values[4];
    for (guint index = 0; index < G_N_ELEMENTS(names); ++index)
        if (!number_member(object, names[index], &values[index])) return FALSE;
    if (values[0] < 60 || values[0] > 200 || values[1] < 0 || values[2] < 0.45 || values[2] > 1 || values[3] < 4000 || values[3] > 120000 || values[1] >= values[3] ||
        json_node_get_value_type(json_object_get_member(object, "analyzed_ms")) != G_TYPE_INT64) return FALSE;
    gdouble downbeat = 0; gboolean has_downbeat = json_object_has_member(object, "downbeat_ms");
    if (has_downbeat && (!g_strcmp0(algorithm, RADIOS_RHYTHM_ALGORITHM_LEGACY) || !number_member(object, "downbeat_ms", &downbeat) ||
            downbeat < 0 || downbeat >= values[3])) return FALSE;
    *analysis = (RadioSRhythmAnalysis){.bpm = values[0], .first_beat_ms = values[1], .confidence = values[2],
        .analyzed_ms = (guint)values[3], .downbeat_ms = downbeat, .has_downbeat = has_downbeat,
        .manual = !g_strcmp0(algorithm, RADIOS_RHYTHM_ALGORITHM_MANUAL)};
    return TRUE;
}
gboolean radios_rhythm_tempo_ratio(gdouble source_bpm, gdouble target_bpm, gdouble *ratio)
{
    if (!ratio || !isfinite(source_bpm) || !isfinite(target_bpm) || source_bpm < 30 || source_bpm > 300 || target_bpm < 30 || target_bpm > 600) return FALSE;
    gdouble best = target_bpm / source_bpm;
    if (fabs(best * 2 - 1) < fabs(best - 1)) best *= 2;
    if (fabs(best / 2 - 1) < fabs(best - 1)) best /= 2;
    if (best < 0.84 || best > 1.16) return FALSE;
    *ratio = best; return TRUE;
}
guint64 radios_rhythm_transition_frame(const RadioSRhythmAnalysis *rhythm, guint64 trigger, guint64 cue_in, guint64 cue_out)
{
    if (!rhythm || !(rhythm->bpm > 0) || trigger >= cue_out) return trigger;
    gdouble first = rhythm->first_beat_ms * 48, period = 48000 * 60 / rhythm->bpm;
    if (trigger < first) return trigger;
    guint64 beat = MAX(cue_in, (guint64)llround(first + floor((trigger - first) / period) * period));
    if (!rhythm->has_downbeat) return beat;
    gdouble bar = 4 * period, origin = rhythm->downbeat_ms * 48;
    gdouble before = origin + floor((trigger - origin) / bar) * bar, after = before + bar;
    gboolean before_fits = before >= cue_in && before < cue_out, after_fits = after >= cue_in && after < cue_out;
    if (after_fits && (!before_fits || after - trigger < trigger - before)) return (guint64)llround(after);
    return before_fits ? (guint64)llround(before) : beat;
}
gdouble radios_rhythm_phase_offset(const RadioSRhythmAnalysis *deck, gdouble deck_ratio, gdouble deck_frame,
    const RadioSRhythmAnalysis *master, gdouble master_ratio, gdouble master_frame, guint unit)
{
    if (!deck || !master || !(deck->bpm > 0) || !(master->bpm > 0) || !(deck_ratio > 0) || !(master_ratio > 0)) return 0;
    gboolean bars = unit == 4 && deck->has_downbeat && master->has_downbeat;
    gdouble span = bars ? 4 : 1;
    gdouble deck_beats = (deck_frame - (bars ? deck->downbeat_ms : deck->first_beat_ms) * 48) * deck->bpm / 2880000;
    gdouble master_beats = (master_frame - (bars ? master->downbeat_ms : master->first_beat_ms) * 48) * master->bpm / 2880000;
    gdouble desired = master_beats * deck->bpm * deck_ratio / (master->bpm * master_ratio), offset = fmod(deck_beats - desired, span);
    return offset < 0 ? offset + span : offset >= span ? 0 : offset;
}
gboolean radios_rhythm_entry_frame(const RadioSRhythmAnalysis *rhythm, guint64 cue_in, guint64 cue_out, guint64 *frame)
{
    if (!rhythm || !frame || !(rhythm->bpm > 0)) return FALSE;
    gdouble period = 48000 * 60 / rhythm->bpm;
    if (rhythm->has_downbeat) {
        gdouble bar = 4 * period, origin = rhythm->downbeat_ms * 48, early = MIN(960.0, (gdouble)cue_in);
        gdouble at = origin + MAX(0, ceil((cue_in - early - origin) / bar)) * bar;
        if (at < cue_out) { *frame = (guint64)llround(at); return TRUE; }
    }
    gdouble origin = rhythm->first_beat_ms * 48, at = origin + MAX(0, ceil((cue_in - origin) / period)) * period;
    if (at >= cue_out) return FALSE;
    *frame = (guint64)llround(at); return TRUE;
}
