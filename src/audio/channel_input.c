#define _POSIX_C_SOURCE 200809L
#include "radios/audio_channel_input.h"
#include <math.h>
#include <string.h>
#include <pulse/pulseaudio.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

/* The worker keeps at most 100 ms queued ahead of the renderer, so a bursty
 * network source never grows into delay and is never trimmed by the line. */
#define PACE_FRAMES 4800u
#define CONNECT_TIMEOUT_US (15 * G_USEC_PER_SEC)
#define BLOCK_FRAMES 4096u
#define FRAME_BYTES 8u
/* https rides on tls and tcp; rtsp negotiates over tcp and carries rtp/udp.
 * Local files, pipes, concat and other protocols stay refused. */
#define PROTOCOL_WHITELIST "http,https,tls,tcp,udp,rtp,rtsp,srt"
/* free() waits this long for the worker; see radios_audio_channel_input_free. */
#define CLOSE_WAIT_US (250 * G_TIME_SPAN_MILLISECOND)
/* A connected network source that has sent no audio for this long is shown
 * as silent (ERROR with this text, peak 0) while its connection is kept: a
 * UDP/RTP sender that comes back is heard again on the same socket, and an
 * HTTP stall still ends in the 10 s read timeout and the usual reconnect. */
#define NO_DATA_US (5 * G_USEC_PER_SEC)
#define NO_DATA_MESSAGE RADIOS_CHANNEL_INPUT_NO_DATA_MESSAGE
/* A level is that of the last delivered block, and only this long. */
#define PEAK_HOLD_US (500 * G_TIME_SPAN_MILLISECOND)

struct RadioSAudioChannelInput {
    RadioSAudioEngine *engine;
    RadioSAudioLine *line;
    guint channel;
    RadioSAudioChannelInputSpec spec;
    gchar *server;
    GThread *thread;
    GMutex mutex;
    GCond condition;
    gboolean closing;
    gboolean exited; /* the worker left its loop (under mutex) */
    gint refs; /* the owner and the worker; the last one releases */
    gint64 deadline; /* connect deadline for the FFmpeg interrupt callback */
    gint64 waiting_since; /* worker: the read loop has waited for audio since (0 = not reading) */
    gboolean starved; /* worker: NO_DATA_MESSAGE is shown */
    gint64 delivered; /* last delivery to the line (under mutex) */
    RadioSAudioChannelInputStatus status;
};

static gboolean fail(GError **error, const gchar *message)
{ g_set_error_literal(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID, message); return FALSE; }

const gchar *radios_audio_channel_input_kind_name(RadioSAudioChannelInputKind kind)
{
    switch (kind) {
    case RADIOS_CHANNEL_INPUT_CAPTURE: return "capture";
    case RADIOS_CHANNEL_INPUT_NETWORK: return "network";
    default: return "none";
    }
}
gboolean radios_audio_channel_input_kind_parse(const gchar *name, RadioSAudioChannelInputKind *kind)
{
    if (!name || !kind) return FALSE;
    if (g_str_equal(name, "capture")) { *kind = RADIOS_CHANNEL_INPUT_CAPTURE; return TRUE; }
    if (g_str_equal(name, "network")) { *kind = RADIOS_CHANNEL_INPUT_NETWORK; return TRUE; }
    return FALSE;
}
static gboolean text_valid(const gchar *text, gsize size)
{
    gsize length = strnlen(text, size);
    return length > 0 && length < size && g_utf8_validate(text, (gssize)length, NULL);
}
/* The network schemes and what each needs from this FFmpeg build: rtsp is a
 * demuxer over tcp/udp/rtp, rtp needs its demuxer and protocol, the others
 * are input protocols. Builds differ: the Freedesktop SDK's has no srt. */
typedef struct { const gchar *prefix, *label, *protocol, *demuxer; } Scheme;
static const Scheme schemes[] = {
    {"http://", "HTTP", "http", NULL}, {"https://", "HTTPS", "https", NULL}, {"rtp://", "RTP", "rtp", "rtp"},
    {"udp://", "UDP", "udp", NULL}, {"rtsp://", "RTSP", "tcp", "rtsp"}, {"srt://", "SRT", "srt", NULL},
};
static gboolean input_protocol(const gchar *name)
{
    void *opaque = NULL;
    for (const gchar *protocol; (protocol = avio_enum_protocols(&opaque, 0));) if (g_str_equal(protocol, name)) return TRUE;
    return FALSE;
}
static gboolean scheme_available(guint index)
{
    static gsize once; static gboolean present[G_N_ELEMENTS(schemes)];
    if (g_once_init_enter(&once)) {
        for (guint i = 0; i < G_N_ELEMENTS(schemes); ++i)
            present[i] = input_protocol(schemes[i].protocol) && (!schemes[i].demuxer || av_find_input_format(schemes[i].demuxer));
        g_once_init_leave(&once, 1);
    }
    return present[index];
}
/* The index of the URL's scheme in `schemes`, or -1 when it has none of them,
 * nothing after it, or white space or control characters anywhere. */
static gint url_scheme(const gchar *url)
{
    for (guint i = 0; i < G_N_ELEMENTS(schemes); ++i) {
        gsize length = strlen(schemes[i].prefix);
        if (g_ascii_strncasecmp(url, schemes[i].prefix, length) != 0 || url[length] == '\0') continue;
        for (const gchar *c = url; *c; ++c) if (g_ascii_isspace(*c) || g_ascii_iscntrl(*c)) return -1;
        return (gint)i;
    }
    return -1;
}
/* "http, https, rtp, udp veya rtsp": the schemes this build can open. */
static gboolean scheme_fail(GError **error)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < G_N_ELEMENTS(schemes); ++i)
        if (scheme_available(i)) g_ptr_array_add(names, g_strndup(schemes[i].prefix, strlen(schemes[i].prefix) - 3));
    gboolean any = names->len > 0;
    if (any) {
        GString *list = g_string_new(NULL);
        for (guint i = 0; i < names->len; ++i) {
            if (i) g_string_append(list, i + 1 == names->len ? " veya " : ", ");
            g_string_append(list, g_ptr_array_index(names, i));
        }
        g_set_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID, "Ağ kaynağı adresi %s ile başlamalı; boşluk içeremez.", list->str);
        g_string_free(list, TRUE);
    }
    g_ptr_array_unref(names);
    return any ? FALSE : fail(error, "Bu sistemin FFmpeg'inde ağ kaynağı desteği yok.");
}
gboolean radios_audio_channel_input_spec_validate(const RadioSAudioChannelInputSpec *spec, GError **error)
{
    if (!spec || (spec->kind != RADIOS_CHANNEL_INPUT_CAPTURE && spec->kind != RADIOS_CHANNEL_INPUT_NETWORK))
        return fail(error, "Kanal girişi ses kartı girişi ya da ağ kaynağı olmalı.");
    if (!text_valid(spec->device, sizeof spec->device))
        return fail(error, spec->kind == RADIOS_CHANNEL_INPUT_CAPTURE ? "Ses kartı giriş adı 1..512 karakter olmalı." : "Ağ kaynağı adresi 1..512 karakter olmalı.");
    if (!text_valid(spec->name, sizeof spec->name)) return fail(error, "Kaynak adı 1..128 karakter olmalı.");
    if (spec->kind != RADIOS_CHANNEL_INPUT_NETWORK) return TRUE;
    gint scheme = url_scheme(spec->device);
    if (scheme < 0) return scheme_fail(error);
    /* Registering or starting a source this FFmpeg cannot open would only
     * retry "Protocol not found" every five seconds. */
    if (!scheme_available((guint)scheme)) {
        g_set_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID, "Bu sistemin FFmpeg'inde %s desteği yok; %s ağ kaynağı kullanılamaz.",
                    schemes[scheme].label, schemes[scheme].prefix);
        return FALSE;
    }
    return TRUE;
}
gboolean radios_audio_channel_input_spec_equal(const RadioSAudioChannelInputSpec *a, const RadioSAudioChannelInputSpec *b)
{
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return FALSE;
    if (a->kind == RADIOS_CHANNEL_INPUT_NONE) return TRUE;
    return g_str_equal(a->device, b->device) && g_str_equal(a->name, b->name);
}

static gboolean closing(RadioSAudioChannelInput *input)
{ return g_atomic_int_get(&input->closing); }
static void status_set(RadioSAudioChannelInput *input, RadioSAudioChannelInputState state, const gchar *error)
{
    g_mutex_lock(&input->mutex);
    input->status.state = state;
    if (error) g_strlcpy(input->status.error, error, sizeof input->status.error);
    if (state != RADIOS_CHANNEL_INPUT_READY) input->status.peak[0] = input->status.peak[1] = 0;
    g_mutex_unlock(&input->mutex);
}
/* Delivers stereo float frames to the line, pacing a producer that runs
 * ahead of the renderer. Returns FALSE once the input is closing. */
static gboolean deliver(RadioSAudioChannelInput *input, const gfloat *stereo, guint frames)
{
    gfloat peak[2] = {0, 0};
    for (guint i = 0; i < frames; ++i) for (guint c = 0; c < 2; ++c) {
        gfloat value = stereo[i * 2 + c];
        if (!isfinite(value)) value = 0;
        peak[c] = MAX(peak[c], fabsf(value));
    }
    guint done = 0;
    while (done < frames) {
        if (closing(input)) return FALSE;
        if (radios_audio_line_queued(input->line) > PACE_FRAMES) { g_usleep(5000); continue; }
        guint written = radios_audio_line_write(input->line, stereo + done * 2, frames - done);
        if (!written) { g_usleep(2000); continue; }
        done += written;
    }
    g_mutex_lock(&input->mutex);
    input->status.frames += frames; input->status.peak[0] = peak[0]; input->status.peak[1] = peak[1];
    input->delivered = g_get_monotonic_time();
    g_mutex_unlock(&input->mutex);
    return TRUE;
}

/* Capture: one record stream on a private Pulse connection. */
typedef struct {
    RadioSAudioChannelInput *input;
    pa_context *context;
    pa_stream *stream;
    gboolean failed, stream_ready;
    gchar error[256];
} Capture;
static void capture_fail(Capture *capture, const gchar *message)
{
    if (!capture->failed) g_strlcpy(capture->error, message, sizeof capture->error);
    capture->failed = TRUE;
}
static void capture_read(pa_stream *stream, size_t bytes, void *data)
{
    (void)bytes; Capture *capture = data; const void *samples = NULL; size_t size = 0;
    if (pa_stream_peek(stream, &samples, &size) < 0) { capture_fail(capture, "Ses kartı girişi okunamadı."); return; }
    if (size && size % FRAME_BYTES == 0) {
        guint frames = (guint)(size / FRAME_BYTES);
        if (samples) (void)deliver(capture->input, samples, frames);
        else {
            gfloat silence[BLOCK_FRAMES * 2] = {0};
            for (guint done = 0; done < frames && !closing(capture->input);) {
                guint count = MIN(frames - done, BLOCK_FRAMES); (void)deliver(capture->input, silence, count); done += count;
            }
        }
    }
    if (size) (void)pa_stream_drop(stream);
}
static void capture_stream_state(pa_stream *stream, void *data)
{
    Capture *capture = data;
    switch (pa_stream_get_state(stream)) {
    case PA_STREAM_READY: capture->stream_ready = TRUE; break;
    case PA_STREAM_FAILED: {
        gchar message[256]; g_snprintf(message, sizeof message, "Ses kartı girişi açılamadı: %s", pa_strerror(pa_context_errno(capture->context)));
        capture_fail(capture, message); break;
    }
    case PA_STREAM_TERMINATED: capture_fail(capture, "Ses kartı girişi kapandı."); break;
    default: break;
    }
}
static void capture_context_state(pa_context *context, void *data)
{
    Capture *capture = data;
    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY: {
        pa_sample_spec spec = {PA_SAMPLE_FLOAT32NE, 48000, 2};
        pa_proplist *properties = pa_proplist_new();
        pa_proplist_sets(properties, PA_PROP_MEDIA_NAME, "RadioS kanal girişi");
        pa_proplist_sets(properties, PA_PROP_MEDIA_ROLE, "production");
        capture->stream = pa_stream_new_with_proplist(context, "RadioS kanal girişi", &spec, NULL, properties);
        pa_proplist_free(properties);
        if (!capture->stream) { capture_fail(capture, "Ses kartı giriş akışı oluşturulamadı."); break; }
        pa_stream_set_state_callback(capture->stream, capture_stream_state, capture);
        pa_stream_set_read_callback(capture->stream, capture_read, capture);
        pa_buffer_attr attr = {(uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, 480 * FRAME_BYTES};
        const gchar *device = capture->input->spec.device;
        if (!*device || g_str_equal(device, "@default")) device = NULL;
        if (pa_stream_connect_record(capture->stream, device, &attr, PA_STREAM_ADJUST_LATENCY) < 0) {
            gchar message[256]; g_snprintf(message, sizeof message, "Ses kartı girişi bağlanamadı: %s", pa_strerror(pa_context_errno(context)));
            capture_fail(capture, message);
        }
        break;
    }
    case PA_CONTEXT_FAILED: {
        gchar message[256]; g_snprintf(message, sizeof message, "Ses sunucusuna bağlanılamadı: %s", pa_strerror(pa_context_errno(context)));
        capture_fail(capture, message); break;
    }
    case PA_CONTEXT_TERMINATED: capture_fail(capture, "Ses sunucusu bağlantısı kapandı."); break;
    default: break;
    }
}
static gboolean capture_run(RadioSAudioChannelInput *input, gchar error[256])
{
    Capture capture = {input, NULL, NULL, FALSE, FALSE, {0}};
    pa_mainloop *loop = pa_mainloop_new();
    if (!loop) { g_strlcpy(error, "Ses döngüsü oluşturulamadı.", 256); return FALSE; }
    pa_proplist *properties = pa_proplist_new();
    pa_proplist_sets(properties, PA_PROP_APPLICATION_NAME, "RadioS");
    pa_proplist_sets(properties, PA_PROP_APPLICATION_ID, "org.radios.channel-input");
    capture.context = pa_context_new_with_proplist(pa_mainloop_get_api(loop), "RadioS", properties);
    pa_proplist_free(properties);
    if (!capture.context) { pa_mainloop_free(loop); g_strlcpy(error, "Ses bağlamı oluşturulamadı.", 256); return FALSE; }
    pa_context_set_state_callback(capture.context, capture_context_state, &capture);
    gboolean connected = pa_context_connect(capture.context, input->server, PA_CONTEXT_NOAUTOSPAWN, NULL) >= 0;
    if (!connected) capture_fail(&capture, "Ses sunucusuna bağlanılamadı.");
    gint64 deadline = g_get_monotonic_time() + CONNECT_TIMEOUT_US;
    gboolean reported_ready = FALSE;
    while (!capture.failed && !closing(input)) {
        if (pa_mainloop_prepare(loop, 50000) < 0 || pa_mainloop_poll(loop) < 0 || pa_mainloop_dispatch(loop) < 0) {
            capture_fail(&capture, "Ses döngüsü başarısız oldu."); break;
        }
        if (capture.stream_ready && !reported_ready) { reported_ready = TRUE; status_set(input, RADIOS_CHANNEL_INPUT_READY, NULL); }
        if (!capture.stream_ready && g_get_monotonic_time() > deadline) { capture_fail(&capture, "Ses kartı girişi zamanında açılmadı."); break; }
    }
    if (capture.stream) {
        pa_stream_set_state_callback(capture.stream, NULL, NULL); pa_stream_set_read_callback(capture.stream, NULL, NULL);
        if (pa_stream_get_state(capture.stream) == PA_STREAM_READY) (void)pa_stream_disconnect(capture.stream);
        pa_stream_unref(capture.stream);
    }
    pa_context_set_state_callback(capture.context, NULL, NULL);
    if (connected) pa_context_disconnect(capture.context);
    pa_context_unref(capture.context); pa_mainloop_free(loop);
    if (capture.failed) g_strlcpy(error, capture.error, 256);
    return !capture.failed;
}

/* Network: FFmpeg demux/decode of one whitelisted URL, resampled to 48 kHz. */
static int network_interrupt(void *opaque)
{
    RadioSAudioChannelInput *input = opaque;
    if (closing(input)) return 1;
    /* Read on the worker thread only: the FFmpeg calls run there. FFmpeg
     * calls this at least every 100 ms while it waits for the network; UDP
     * and RTP reads never time out on their own, so a stopped sender would
     * otherwise stay "Hazır" with its last level. */
    gint64 now = g_get_monotonic_time();
    if (input->waiting_since && !input->starved && now - input->waiting_since > NO_DATA_US) {
        input->starved = TRUE; status_set(input, RADIOS_CHANNEL_INPUT_ERROR, NO_DATA_MESSAGE);
    }
    return input->deadline && now > input->deadline;
}
/* Audio is arriving again on the kept connection. */
static void data_resumed(RadioSAudioChannelInput *input)
{
    input->starved = FALSE;
    g_mutex_lock(&input->mutex);
    input->status.state = RADIOS_CHANNEL_INPUT_READY;
    if (g_str_equal(input->status.error, NO_DATA_MESSAGE)) input->status.error[0] = '\0';
    g_mutex_unlock(&input->mutex);
}
/* FFmpeg before 5.0 read "?tcp" in an rtsp:// URL as "carry RTP inside the
 * RTSP connection"; FFmpeg 7 sends it to the server as part of the path and
 * stays on UDP. The flag becomes rtsp_transport=tcp and leaves the URL. */
static gchar *rtsp_url_options(const gchar *url, AVDictionary **options)
{
    const gchar *query = g_ascii_strncasecmp(url, "rtsp://", 7) == 0 ? strchr(url, '?') : NULL;
    if (!query) return g_strdup(url);
    gchar **parts = g_strsplit(query + 1, "&", -1);
    GString *kept = g_string_new_len(url, query - url); gboolean tcp = FALSE; guint count = 0;
    for (guint i = 0; parts[i]; ++i) {
        if (!g_ascii_strcasecmp(parts[i], "tcp")) { tcp = TRUE; continue; }
        g_string_append_c(kept, count++ ? '&' : '?'); g_string_append(kept, parts[i]);
    }
    g_strfreev(parts);
    if (!tcp) { g_string_free(kept, TRUE); return g_strdup(url); }
    av_dict_set(options, "rtsp_transport", "tcp", 0);
    return g_string_free(kept, FALSE);
}
static void av_message(gchar error[256], const gchar *prefix, gint code)
{
    gchar text[AV_ERROR_MAX_STRING_SIZE]; av_strerror(code, text, sizeof text);
    g_snprintf(error, 256, "%s: %s", prefix, text);
}
typedef struct {
    AVFormatContext *format;
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packet;
    SwrContext *resampler;
    AVChannelLayout layout;
    gint stream_index, rate, sample_format;
    gfloat *buffer;
    guint capacity;
} Network;
static void network_close(Network *net)
{
    swr_free(&net->resampler); av_channel_layout_uninit(&net->layout);
    av_frame_free(&net->frame); av_packet_free(&net->packet); avcodec_free_context(&net->codec);
    if (net->format) avformat_close_input(&net->format);
    g_free(net->buffer); net->buffer = NULL; net->capacity = 0;
}
static gboolean network_convert(RadioSAudioChannelInput *input, Network *net, gchar error[256])
{
    AVFrame *frame = net->frame; gint rate = frame->sample_rate, channels = frame->ch_layout.nb_channels;
    if (rate < 8000 || rate > 384000 || channels < 1 || channels > 32 || frame->nb_samples <= 0 || frame->nb_samples > 65536) {
        g_strlcpy(error, "Ağ kaynağının ses biçimi desteklenen sınırların dışında.", 256); return FALSE;
    }
    if (!net->resampler || net->rate != rate || net->sample_format != frame->format || net->layout.nb_channels != channels) {
        swr_free(&net->resampler); av_channel_layout_uninit(&net->layout);
        gint code = av_channel_layout_copy(&net->layout, &frame->ch_layout);
        if (code >= 0 && net->layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_uninit(&net->layout); av_channel_layout_default(&net->layout, channels);
        }
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (code >= 0) code = swr_alloc_set_opts2(&net->resampler, &stereo, AV_SAMPLE_FMT_FLT, 48000,
            &net->layout, (enum AVSampleFormat)frame->format, rate, 0, NULL);
        if (code >= 0 && channels == 1) { const gdouble matrix[] = {1.0, 1.0}; code = swr_set_matrix(net->resampler, matrix, 1); }
        if (code >= 0) code = swr_init(net->resampler);
        if (code < 0) { av_message(error, "Ağ kaynağı yeniden örneklenemedi", code); return FALSE; }
        net->rate = rate; net->sample_format = frame->format;
    }
    gint64 requested = av_rescale_rnd(swr_get_delay(net->resampler, rate) + frame->nb_samples, 48000, rate, AV_ROUND_UP);
    if (requested <= 0 || requested > 48000 * 10) { g_strlcpy(error, "Ağ kaynağı ses tamponu sınırı aşıldı.", 256); return FALSE; }
    if ((guint)requested > net->capacity) {
        gfloat *buffer = g_try_realloc_n(net->buffer, (gsize)requested * 2, sizeof(gfloat));
        if (!buffer) { g_strlcpy(error, "Ağ kaynağı PCM belleği ayrılamadı.", 256); return FALSE; }
        net->buffer = buffer; net->capacity = (guint)requested;
    }
    uint8_t *output[] = {(uint8_t *)net->buffer};
    gint frames = swr_convert(net->resampler, output, (gint)requested, (const uint8_t **)frame->extended_data, frame->nb_samples);
    if (frames < 0) { av_message(error, "Ağ kaynağı çözülemedi", frames); return FALSE; }
    for (guint done = 0; done < (guint)frames;) {
        guint count = MIN((guint)frames - done, BLOCK_FRAMES);
        if (!deliver(input, net->buffer + done * 2, count)) return FALSE;
        done += count;
    }
    return TRUE;
}
static gboolean network_run(RadioSAudioChannelInput *input, gchar error[256])
{
    Network net = {0}; net.stream_index = -1;
    net.format = avformat_alloc_context();
    if (!net.format) { g_strlcpy(error, "Ağ kaynağı bağlamı ayrılamadı.", 256); return FALSE; }
    net.format->interrupt_callback.callback = network_interrupt; net.format->interrupt_callback.opaque = input;
    net.format->probesize = 512 * 1024; net.format->max_analyze_duration = 2 * AV_TIME_BASE;
    net.format->max_streams = 16;
    AVDictionary *options = NULL;
    av_dict_set(&options, "protocol_whitelist", PROTOCOL_WHITELIST, 0);
    av_dict_set(&options, "rw_timeout", "10000000", 0);
    av_dict_set(&options, "user_agent", "RadioS", 0);
    av_dict_set(&options, "icy", "0", 0);
    gchar *url = rtsp_url_options(input->spec.device, &options);
    input->waiting_since = 0; input->starved = FALSE;
    input->deadline = g_get_monotonic_time() + CONNECT_TIMEOUT_US;
    gint code = avformat_open_input(&net.format, url, NULL, &options); av_dict_free(&options); g_free(url);
    if (code < 0) { input->deadline = 0; av_message(error, closing(input) ? "Ağ kaynağı kapatıldı" : "Ağ kaynağı açılamadı", code); return FALSE; }
    code = avformat_find_stream_info(net.format, NULL);
    if (code < 0) { input->deadline = 0; av_message(error, "Ağ kaynağının akış bilgisi okunamadı", code); network_close(&net); return FALSE; }
    const AVCodec *codec = NULL;
    net.stream_index = av_find_best_stream(net.format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (net.stream_index < 0 || !codec) { input->deadline = 0; g_strlcpy(error, "Ağ kaynağında çözülebilir ses akışı yok.", 256); network_close(&net); return FALSE; }
    AVStream *stream = net.format->streams[net.stream_index];
    net.codec = avcodec_alloc_context3(codec);
    if (!net.codec || avcodec_parameters_to_context(net.codec, stream->codecpar) < 0) {
        input->deadline = 0; g_strlcpy(error, "Ağ kaynağı çözücüsü hazırlanamadı.", 256); network_close(&net); return FALSE;
    }
    net.codec->pkt_timebase = stream->time_base; net.codec->thread_count = 1;
    if ((code = avcodec_open2(net.codec, codec, NULL)) < 0) { input->deadline = 0; av_message(error, "Ağ kaynağı çözücüsü açılamadı", code); network_close(&net); return FALSE; }
    net.frame = av_frame_alloc(); net.packet = av_packet_alloc();
    if (!net.frame || !net.packet) { input->deadline = 0; g_strlcpy(error, "Ağ kaynağı kare belleği ayrılamadı.", 256); network_close(&net); return FALSE; }
    input->deadline = 0;
    status_set(input, RADIOS_CHANNEL_INPUT_READY, NULL);
    gboolean ok = TRUE;
    /* The wait for audio starts now and again after every delivered packet,
     * so time spent pacing behind the renderer never counts as silence. */
    input->waiting_since = g_get_monotonic_time();
    while (ok && !closing(input)) {
        code = av_read_frame(net.format, net.packet);
        if (code < 0) {
            if (closing(input)) break;
            av_message(error, code == AVERROR_EOF ? "Ağ kaynağı akışı sona erdi" : "Ağ kaynağı okunamadı", code); ok = FALSE; break;
        }
        if (net.packet->stream_index != net.stream_index) { av_packet_unref(net.packet); continue; }
        if (input->starved) data_resumed(input);
        code = avcodec_send_packet(net.codec, net.packet); av_packet_unref(net.packet);
        if (code < 0 && code != AVERROR(EAGAIN)) { av_message(error, "Ağ kaynağı paketi çözülemedi", code); ok = FALSE; break; }
        for (;;) {
            code = avcodec_receive_frame(net.codec, net.frame);
            if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) break;
            if (code < 0) { av_message(error, "Ağ kaynağı karesi çözülemedi", code); ok = FALSE; break; }
            gboolean delivered = network_convert(input, &net, error); av_frame_unref(net.frame);
            if (!delivered) { ok = closing(input); if (!ok) break; }
            if (closing(input)) break;
        }
        input->waiting_since = g_get_monotonic_time();
    }
    input->waiting_since = 0;
    network_close(&net);
    return ok || closing(input);
}

static void input_release(RadioSAudioChannelInput *input)
{
    if (!g_atomic_int_dec_and_test(&input->refs)) return;
    radios_audio_line_close(input->line);
    radios_audio_engine_unref(input->engine);
    g_mutex_clear(&input->mutex); g_cond_clear(&input->condition);
    g_free(input->server); g_free(input);
}
static gpointer worker(gpointer value)
{
    RadioSAudioChannelInput *input = value;
    while (!closing(input)) {
        g_mutex_lock(&input->mutex);
        ++input->status.attempts; input->status.state = RADIOS_CHANNEL_INPUT_CONNECTING;
        input->status.peak[0] = input->status.peak[1] = 0;
        g_mutex_unlock(&input->mutex);
        gchar error[256] = {0};
        gboolean ok = input->spec.kind == RADIOS_CHANNEL_INPUT_CAPTURE ? capture_run(input, error) : network_run(input, error);
        if (closing(input)) break;
        status_set(input, RADIOS_CHANNEL_INPUT_ERROR, ok ? "Kaynak bağlantısı kesildi." : error);
        g_mutex_lock(&input->mutex);
        gint64 until = g_get_monotonic_time() + RADIOS_CHANNEL_INPUT_RETRY_SECONDS * G_USEC_PER_SEC;
        while (!input->closing && g_cond_wait_until(&input->condition, &input->mutex, until)) {}
        g_mutex_unlock(&input->mutex);
    }
    status_set(input, RADIOS_CHANNEL_INPUT_IDLE, NULL);
    g_mutex_lock(&input->mutex); input->exited = TRUE; g_cond_broadcast(&input->condition); g_mutex_unlock(&input->mutex);
    input_release(input);
    return NULL;
}

RadioSAudioChannelInput *radios_audio_channel_input_new(RadioSAudioEngine *engine, guint channel,
    const RadioSAudioChannelInputSpec *spec, const gchar *pulse_server, GError **error)
{
    if (!engine || channel >= RADIOS_AUDIO_MIXER_CHANNELS) { fail(error, "Kanal girişi için motor ve 1..32 arası kanal gerekli."); return NULL; }
    if (!radios_audio_channel_input_spec_validate(spec, error)) return NULL;
    RadioSAudioLine *line = radios_audio_engine_line_open(engine, (gint)channel, error);
    if (!line) return NULL;
    RadioSAudioChannelInput *input = g_new0(RadioSAudioChannelInput, 1);
    input->engine = radios_audio_engine_ref(engine); input->line = line; input->channel = channel; input->spec = *spec;
    input->server = pulse_server && *pulse_server ? g_strdup(pulse_server) : NULL;
    g_mutex_init(&input->mutex); g_cond_init(&input->condition);
    input->status.state = RADIOS_CHANNEL_INPUT_CONNECTING; input->refs = 2;
    input->thread = g_thread_new("radios-channel-input", worker, input);
    return input;
}
guint radios_audio_channel_input_channel(const RadioSAudioChannelInput *input)
{ return input ? input->channel : 0; }
void radios_audio_channel_input_spec(const RadioSAudioChannelInput *input, RadioSAudioChannelInputSpec *spec)
{ if (spec) { if (input) *spec = input->spec; else memset(spec, 0, sizeof *spec); } }
void radios_audio_channel_input_status(RadioSAudioChannelInput *input, RadioSAudioChannelInputStatus *status)
{
    if (!status) return;
    if (!input) { memset(status, 0, sizeof *status); return; }
    g_mutex_lock(&input->mutex);
    *status = input->status;
    gboolean stale = g_get_monotonic_time() - input->delivered > PEAK_HOLD_US;
    g_mutex_unlock(&input->mutex);
    /* A source that stopped delivering has no level, not its last block's. */
    if (stale) status->peak[0] = status->peak[1] = 0;
}
void radios_audio_channel_input_free(RadioSAudioChannelInput *input)
{
    if (!input) return;
    /* The caller is normally the GTK thread. A worker in a Pulse poll, a
     * pacing wait or an interruptible FFmpeg call leaves within ~100 ms. A host
     * name lookup inside FFmpeg cannot be interrupted; rather than freezing
     * the interface until the resolver gives up, the detached worker then
     * finishes the teardown (line close, engine release) itself. */
    gint64 until = g_get_monotonic_time() + CLOSE_WAIT_US;
    g_mutex_lock(&input->mutex); g_atomic_int_set(&input->closing, TRUE); g_cond_broadcast(&input->condition);
    while (!input->exited && g_cond_wait_until(&input->condition, &input->mutex, until)) {}
    gboolean exited = input->exited;
    g_mutex_unlock(&input->mutex);
    if (exited) g_thread_join(input->thread); else g_thread_unref(input->thread);
    input_release(input);
}
