#include "radios/audio_tempo.h"
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <math.h>
#include <string.h>

struct RadioSAudioTempo {
    AVFilterGraph *graph;
    AVFilterContext *source, *sink, *filter;
    AVFrame *output;
    guint offset;
    guint64 remaining;
    guint64 expected_output, output_count;
    gboolean bounded, drained;
    RadioSAudioTempoProfile profile;
    gdouble output_offset, source_offset, commanded_ratio;
    guint64 input_count;
};
static gboolean tempo_error(GError **error, gint code)
{
    gchar message[AV_ERROR_MAX_STRING_SIZE]; av_strerror(code, message, sizeof message);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Tempo DSP: %s", message); return FALSE;
}
RadioSAudioTempo *radios_audio_tempo_new_profile(const RadioSAudioTempoProfile *profile, gdouble output_offset, guint64 source_frames, GError **error)
{
    if (!profile || !isfinite(profile->ratio) || profile->ratio < 0.5 || profile->ratio > 2 || !isfinite(output_offset) || output_offset < 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Tempo oranı veya dönüş profili geçersiz."); return NULL;
    }
    gdouble ratio = radios_audio_tempo_profile_ratio(profile, output_offset);
    RadioSAudioTempo *tempo = g_new0(RadioSAudioTempo, 1); tempo->graph = avfilter_graph_alloc(); tempo->output = av_frame_alloc();
    tempo->remaining = source_frames; tempo->bounded = source_frames != 0;
    tempo->profile = *profile; tempo->output_offset = output_offset;
    tempo->source_offset = radios_audio_tempo_profile_source(profile, output_offset); tempo->commanded_ratio = ratio;
    tempo->expected_output = (guint64)ceil(radios_audio_tempo_profile_output(profile, tempo->source_offset + source_frames) - output_offset);
    if (!tempo->graph || !tempo->output) { tempo_error(error, AVERROR(ENOMEM)); radios_audio_tempo_free(tempo); return NULL; }
    tempo->graph->nb_threads = 1;
    gint result = avfilter_graph_create_filter(&tempo->source, avfilter_get_by_name("abuffer"), "input",
        "time_base=1/48000:sample_rate=48000:sample_fmt=flt:channel_layout=stereo", NULL, tempo->graph);
    AVFilterContext *filter = NULL;
    if (result >= 0) {
        filter = avfilter_graph_alloc_filter(tempo->graph, avfilter_get_by_name("atempo"), "tempo");
        if (!filter) result = AVERROR(ENOMEM);
    }
    /* A typed option avoids locale-dependent parsing of a fractional ratio. */
    if (result >= 0) result = av_opt_set_double(filter->priv, "tempo", ratio, 0);
    if (result >= 0) result = avfilter_init_str(filter, NULL);
    if (result >= 0) result = avfilter_graph_create_filter(&tempo->sink, avfilter_get_by_name("abuffersink"), "output", NULL, NULL, tempo->graph);
    if (result >= 0) result = avfilter_link(tempo->source, 0, filter, 0);
    if (result >= 0) result = avfilter_link(filter, 0, tempo->sink, 0);
    if (result >= 0) result = avfilter_graph_config(tempo->graph, NULL);
    if (result < 0) { tempo_error(error, result); radios_audio_tempo_free(tempo); return NULL; }
    tempo->filter = filter; return tempo;
}
RadioSAudioTempo *radios_audio_tempo_new(gdouble ratio, guint64 source_frames, GError **error)
{
    RadioSAudioTempoProfile profile = {ratio, 0, 0};
    return radios_audio_tempo_new_profile(&profile, 0, source_frames, error);
}
gssize radios_audio_tempo_read(RadioSAudioTempo *tempo, RadioSAudioDecoder *decoder, gfloat *stereo, guint frames, GCancellable *cancel, GError **error)
{
    if (!tempo || !decoder || !stereo || !frames || frames > 4096) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Tempo PCM okuma aralığı geçersiz."); return -1;
    }
    if (tempo->bounded) frames = (guint)MIN((guint64)frames, tempo->expected_output - tempo->output_count);
    guint written = 0;
    while (written < frames) {
        if (cancel && g_cancellable_set_error_if_cancelled(cancel, error)) return -1;
        if (tempo->output->nb_samples > (gint)tempo->offset) {
            guint count = MIN(frames - written, (guint)tempo->output->nb_samples - tempo->offset);
            memcpy(stereo + written * 2, ((gfloat *)tempo->output->data[0]) + tempo->offset * 2, count * 2 * sizeof(gfloat));
            written += count; tempo->offset += count; continue;
        }
        av_frame_unref(tempo->output); tempo->offset = 0;
        gint result = av_buffersink_get_frame(tempo->sink, tempo->output);
        if (result >= 0) continue;
        if (result == AVERROR_EOF) {
            if (tempo->bounded) { memset(stereo + written * 2, 0, (frames - written) * 2 * sizeof(gfloat)); written = frames; }
            tempo->output_count += written; return written;
        }
        if (result != AVERROR(EAGAIN)) { tempo_error(error, result); return -1; }
        if (tempo->drained) { tempo_error(error, AVERROR_INVALIDDATA); return -1; }
        gfloat input[4096 * 2];
        guint wanted = tempo->bounded ? (guint)MIN(G_GUINT64_CONSTANT(4096), tempo->remaining) : 4096;
        if (tempo->profile.return_frames && wanted) {
            /* Keep atempo's original WSOLA drift anchor. Runtime "tempo"
             * commands re-anchor each fragment and smear transient timing.
             * The typed option instead gives that anchor the curve's integral
             * mean ratio: desired source position / elapsed output position.
             * Its drift correction follows the gradual curve while preserving
             * pitch and overlap/add state. One half-window of lookahead at
             * 48 kHz bounds filter latency; all updates run on this worker. */
            wanted = MIN(wanted, 1024u);
            gdouble elapsed = tempo->output_count + written + 1024;
            gdouble source = radios_audio_tempo_profile_source(&tempo->profile, tempo->output_offset + elapsed) - tempo->source_offset;
            gdouble next = source / elapsed;
            if (fabs(next - tempo->commanded_ratio) > 1e-12) {
                result = av_opt_set_double(tempo->filter->priv, "tempo", next, 0);
                if (result < 0) { tempo_error(error, result); return -1; }
                tempo->commanded_ratio = next;
            }
        }
        gssize got = wanted ? radios_audio_decoder_read(decoder, input, wanted, cancel, error) : 0;
        if (got < 0) return -1;
        if (!got) {
            if (tempo->bounded && tempo->remaining) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Tempo kaynağı doğrulanmış cue süresinden önce bitti."); return -1;
            }
            result = av_buffersrc_add_frame_flags(tempo->source, NULL, 0); tempo->drained = TRUE;
        } else {
            AVFrame *frame = av_frame_alloc();
            if (!frame) { tempo_error(error, AVERROR(ENOMEM)); return -1; }
            frame->format = AV_SAMPLE_FMT_FLT; frame->sample_rate = 48000; frame->nb_samples = (gint)got;
            frame->pts = (gint64)tempo->input_count; tempo->input_count += (guint64)got;
            av_channel_layout_default(&frame->ch_layout, 2);
            result = av_frame_get_buffer(frame, 0);
            if (result >= 0) {
                memcpy(frame->data[0], input, (gsize)got * 2 * sizeof(gfloat));
                result = av_buffersrc_add_frame_flags(tempo->source, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
            }
            av_frame_free(&frame);
            if (tempo->bounded) tempo->remaining -= (guint64)got;
        }
        if (result < 0) { tempo_error(error, result); return -1; }
    }
    tempo->output_count += written; return written;
}
void radios_audio_tempo_free(RadioSAudioTempo *tempo)
{
    if (!tempo) return;
    av_frame_free(&tempo->output); avfilter_graph_free(&tempo->graph); g_free(tempo);
}
