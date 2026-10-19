#include "radios/audio_transition.h"
#include "radios/platform.h"
#include <math.h>
#include <string.h>

#define SECOND 48000u
#define BLOCK 2048u
static gboolean invalid(GError **error, const gchar *text)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, text); return FALSE; }
gboolean radios_audio_transition_plan(const RadioSAudioTransitionMedia *s, RadioSAudioTransitionPlan *p, GError **error)
{
    if (!s || !p || !s->duration_a || !s->duration_b || s->duration_a > 86400ULL * SECOND || s->duration_b > 86400ULL * SECOND ||
        s->cue_a >= s->out_a || s->out_a > s->duration_a || s->cue_b >= s->duration_b ||
        s->overlap > 30u * SECOND || s->overlap > s->out_a - s->cue_a || s->overlap > s->duration_b - s->cue_b ||
        s->fade_in > 30u * SECOND || s->fade_in > s->duration_b - s->cue_b ||
        !isfinite(s->duck_db) || s->duck_db < -30 || s->duck_db > 0 ||
        !isfinite(s->level_db) || s->level_db < -36 || s->level_db > 0)
        return invalid(error, "Geçiş cue/fade/bindirme değerleri doğrulanmış A ve B süreleri içinde olmalı.");
    if (s->shape && !radios_audio_transition_shape_validate(s->shape, NULL))
        return invalid(error, "Tasarlanmış geçiş eğrileri veya efekti geçersiz.");
    memset(p, 0, sizeof *p);
    guint64 a_length = MIN(s->out_a - s->cue_a, s->overlap + 10u * SECOND);
    p->start_a = s->out_a - a_length; p->end_a = a_length; p->start_b = a_length - s->overlap;
    p->frames = MAX(a_length, p->start_b + MIN(s->duration_b - s->cue_b, s->overlap + 10u * SECOND));
    p->overlap = s->overlap; p->fade_in = s->fade_in;
    p->duck_gain = pow(10, s->duck_db / 20); p->level_gain = pow(10, s->level_db / 20);
    p->duck_db = s->duck_db;
    return p->frames <= 90u * SECOND || invalid(error, "Geçiş önizlemesi en fazla 90 saniye olabilir.");
}
void radios_audio_transition_envelope(const RadioSAudioTransitionPlan *p, guint64 frame, gdouble *a, gdouble *b)
{
    RadioSAudioTransition envelope = { .a_frames = p->end_a, .overlap_frames = p->overlap,
        .fade_in_frames = p->fade_in, .duck_db = p->duck_db };
    radios_audio_transition_gains(&envelope, frame, a, b);
    if (frame >= p->frames) *a = *b = 0;
    /* Click-free audition start/end. This does not change the saved cues. */
    gdouble edge = MIN(1.0, (gdouble)frame / 240.0) * MIN(1.0, (gdouble)(p->frames - MIN(frame, p->frames)) / 240.0);
    *a *= p->level_gain * edge; *b *= p->level_gain * edge;
}
static gboolean write_all(gint fd, const void *data, gsize size, GChecksum *hash, GError **error)
{
    const guint8 *bytes = data; gsize done = 0;
    while (done < size) {
        int count = _write(fd, bytes + done, (unsigned int)MIN(size - done, (gsize)0x40000000));
        if (count <= 0) {
            gint code = count < 0 && errno ? errno : EIO;
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(code), "Geçiş dosyası yazılamadı: %s", g_strerror(code)); return FALSE;
        }
        done += (gsize)count;
    }
    g_checksum_update(hash, bytes, size); return TRUE;
}
/* Private "transition-XXXXXX" file: created exclusively (an existing name is
 * never reused) with the owner-only descriptor that replaces mode 0600, as a
 * binary, non-inheritable descriptor. Names are lower case because Windows
 * file names are case-insensitive. */
static gint create_temporary(const gchar *directory, gchar **path)
{
    static const gchar letters[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (guint attempt = 0; attempt < 100; ++attempt) {
        gchar name[] = "transition-XXXXXX";
        for (guint i = 0; i < 6; ++i) name[11 + i] = letters[g_random_int_range(0, (gint)sizeof letters - 1)];
        gchar *candidate = g_build_filename(directory, name, NULL);
        gint fd = radios_fs_open(candidate, RADIOS_O_RDWR | RADIOS_O_CREAT | RADIOS_O_EXCL | RADIOS_O_BINARY | RADIOS_O_CLOEXEC, 0600);
        if (fd >= 0) { *path = candidate; return fd; }
        gint code = errno; g_free(candidate);
        if (code != EEXIST) { errno = code; return -1; }
    }
    errno = EEXIST; return -1;
}
static void le16(guint8 *p, guint16 n) { n = GUINT16_TO_LE(n); memcpy(p, &n, 2); }
static void le32(guint8 *p, guint32 n) { n = GUINT32_TO_LE(n); memcpy(p, &n, 4); }
gboolean radios_audio_transition_prepare(const RadioSAudioTransitionMedia *s, const gchar *directory,
    GCancellable *cancel, gchar **path, gchar **sha, guint64 *frames, GError **error)
{
    RadioSAudioTransitionPlan plan;
    if (!path || !sha || !frames || !radios_audio_transition_plan(s, &plan, error)) return FALSE;
    *path = NULL; *sha = NULL; *frames = 0;
    RadioSAudioDecoder *a = radios_audio_decoder_open(s->path_a, s->sha_a, cancel, error);
    if (!a) return FALSE;
    RadioSAudioDecoder *b = radios_audio_decoder_open(s->path_b, s->sha_b, cancel, error);
    if (!b) { radios_audio_decoder_close(a); return FALSE; }
    gboolean ok = radios_audio_decoder_seek(a, plan.start_a, cancel, error) && radios_audio_decoder_seek(b, s->cue_b, cancel, error);
    gchar *temporary = NULL;
    gint fd = -1;
    if (ok && (!directory || radios_fs_mkdir_with_parents(directory, 0700) != 0)) {
        gint code = directory ? errno : EINVAL;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(code), "Geçiş önizleme dizini açılamadı: %s", g_strerror(code)); ok = FALSE;
    }
    if (ok) {
        fd = create_temporary(directory, &temporary);
        if (fd < 0) { gint code = errno; g_set_error(error, G_IO_ERROR, g_io_error_from_errno(code), "Geçiş önizlemesi oluşturulamadı: %s", g_strerror(code)); ok = FALSE; }
    }
    GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256);
    guint8 header[44] = {0}; memcpy(header, "RIFF", 4); le32(header + 4, (guint32)(36 + plan.frames * 8));
    memcpy(header + 8, "WAVEfmt ", 8); le32(header + 16, 16); le16(header + 20, 3); le16(header + 22, 2);
    le32(header + 24, SECOND); le32(header + 28, SECOND * 8); le16(header + 32, 8); le16(header + 34, 32);
    memcpy(header + 36, "data", 4); le32(header + 40, (guint32)(plan.frames * 8));
    if (ok) ok = write_all(fd, header, sizeof header, hash, error);
    gfloat pcm_a[BLOCK * 2], pcm_b[BLOCK * 2], mix[BLOCK * 2];
    /* A designed shape replaces the overlap envelope and tone/effect processing;
     * the private voices live only for this preparation. */
    const RadioSAudioTransitionShape *shape = s->shape && s->shape->enabled ? s->shape : NULL;
    RadioSAudioShapeVoice *voices[2] = {NULL, NULL};
    if (shape) for (guint side = 0; side < 2; ++side) voices[side] = radios_audio_shape_voice_new();
    for (guint64 at = 0; ok && at < plan.frames;) {
        if (cancel && g_cancellable_set_error_if_cancelled(cancel, error)) { ok = FALSE; break; }
        guint count = (guint)MIN((guint64)BLOCK, plan.frames - at);
        if (at < plan.end_a) count = (guint)MIN((guint64)count, plan.end_a - at);
        if (at < plan.start_b) count = (guint)MIN((guint64)count, plan.start_b - at);
        memset(pcm_a, 0, count * 2 * sizeof(gfloat)); memset(pcm_b, 0, count * 2 * sizeof(gfloat));
        if (at < plan.end_a) { gssize n = radios_audio_decoder_read(a, pcm_a, count, cancel, error); if (n != (gssize)count) { if (n >= 0) invalid(error, "A dosyasının gerçek PCM sonu kaydedilmiş süreyle uyuşmuyor."); ok = FALSE; break; } }
        if (at >= plan.start_b) { gssize n = radios_audio_decoder_read(b, pcm_b, count, cancel, error); if (n != (gssize)count) { if (n >= 0) invalid(error, "B dosyasının gerçek PCM sonu kaydedilmiş süreyle uyuşmuyor."); ok = FALSE; break; } }
        for (guint i = 0; i < count; ++i) {
            gdouble ga, gb; guint64 frame = at + i; radios_audio_transition_envelope(&plan, frame, &ga, &gb);
            if (shape && plan.overlap && frame >= plan.start_b && frame < plan.end_a) {
                gdouble progress = (gdouble)(frame - plan.start_b) / plan.overlap, sa, sb;
                gdouble edge = MIN(1.0, (gdouble)frame / 240.0) * MIN(1.0, (gdouble)(plan.frames - frame) / 240.0);
                radios_audio_transition_shape_gains(shape, progress, &sa, &sb);
                ga = sa * plan.level_gain * edge; gb = sb * plan.level_gain * edge;
                radios_audio_shape_voice_process(voices[0], shape, 0, progress, &pcm_a[i * 2]);
                radios_audio_shape_voice_process(voices[1], shape, 1, progress, &pcm_b[i * 2]);
            }
            for (guint c = 0; c < 2; ++c) { gfloat value = (gfloat)(pcm_a[i * 2 + c] * ga + pcm_b[i * 2 + c] * gb); guint32 bits; memcpy(&bits, &value, 4); bits = GUINT32_TO_LE(bits); memcpy(&mix[i * 2 + c], &bits, 4); }
        }
        ok = write_all(fd, mix, count * 2 * sizeof(gfloat), hash, error); at += count;
    }
    for (guint side = 0; side < 2; ++side) radios_audio_shape_voice_free(voices[side]);
    if (fd >= 0 && _close(fd) != 0 && ok) { gint code = errno; g_set_error(error, G_IO_ERROR, g_io_error_from_errno(code), "Geçiş önizlemesi kapatılamadı: %s", g_strerror(code)); ok = FALSE; }
    radios_audio_decoder_close(a); radios_audio_decoder_close(b);
    if (ok) { *path = temporary; *sha = g_strdup(g_checksum_get_string(hash)); *frames = plan.frames; }
    else { if (temporary) radios_fs_unlink(temporary); g_free(temporary); }
    g_checksum_free(hash); return ok;
}

void radios_audio_transition_lowpass(RadioSAudioTransitionFilter *state,
    guint64 elapsed, guint64 total, gfloat stereo[2])
{
    if (!state || !stereo || !total) return;
    gdouble wet = MIN((gdouble)elapsed / total, 1.0);
    gdouble cutoff = 18000 * pow(300.0 / 18000, wet);
    gdouble alpha = 1 - exp(-2 * G_PI * cutoff / 48000);
    for (guint c = 0; c < 2; ++c) {
        state->stereo[c] += alpha * (stereo[c] - state->stereo[c]);
        stereo[c] = (gfloat)(stereo[c] * (1 - wet) + state->stereo[c] * wet);
    }
}
