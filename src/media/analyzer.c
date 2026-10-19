#define _POSIX_C_SOURCE 200809L
#include "radios/media.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

typedef struct {
    gint fd;
    gint64 position, length;
    const RadioSCancelToken *cancel;
    GCancellable *cancellable;
} LocalInput;
typedef struct {
    RadioSMediaAnalysis *analysis;
    RadioSRhythmAnalyzer *rhythm;
    RadioSLoudnessMeter *loudness;
    SwrContext *convert;
    AVChannelLayout layout;
    enum AVSampleFormat input_format;
    long double squares;
    guint64 bin_count;
    gfloat bin_min, bin_max;
    const RadioSCancelToken *cancel;
    GCancellable *cancellable;
} Measurement;

GQuark radios_media_error_quark(void)
{ return g_quark_from_static_string("radios-media-error"); }

static gboolean cancelled(const RadioSCancelToken *token, GCancellable *cancellable)
{ return (token && radios_cancelled(token)) || (cancellable && g_cancellable_is_cancelled(cancellable)); }

static gboolean check_cancel(const RadioSCancelToken *token, GCancellable *cancellable, GError **error)
{
    if (!cancelled(token, cancellable)) return TRUE;
    g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CANCELLED, "Medya işlemi iptal edildi."); return FALSE;
}

static void io_error(GError **error, const gchar *action)
{ g_set_error(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_IO, "%s: %s", action, g_strerror(errno)); }

static void decode_error(GError **error, gint code)
{
    gchar message[AV_ERROR_MAX_STRING_SIZE]; av_strerror(code, message, sizeof message);
    g_set_error(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_DECODE, "Ses bütünüyle çözülemedi: %s", message);
}

static gboolean same_file(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static gint open_local_path(const gchar *path, gint flags)
{
    gchar *absolute = g_canonicalize_filename(path, NULL);
    gchar *directory = g_path_get_dirname(absolute), *name = g_path_get_basename(absolute);
    gchar **parts = g_strsplit(directory, "/", -1);
    gint parent = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC), fd = -1;
    for (guint i = 0; parent >= 0 && parts[i]; ++i) {
        if (!*parts[i]) continue;
        gint next = openat(parent, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        gint saved = errno; close(parent); parent = next; errno = saved;
    }
    if (parent >= 0) { fd = openat(parent, name, flags | O_NOFOLLOW | O_CLOEXEC); gint saved = errno; close(parent); errno = saved; }
    g_strfreev(parts); g_free(absolute); g_free(directory); g_free(name); return fd;
}

static gint open_source(const gchar *path, struct stat *identity, GError **error)
{
    struct stat before;
    if (!path || !*path || strstr(path, "://")) {
        g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_INVALID_FILE, "Yerel bir ses dosyası seçin."); return -1;
    }
    if (lstat(path, &before) != 0) { io_error(error, "Ses dosyası okunamadı"); return -1; }
    if (!S_ISREG(before.st_mode)) {
        g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_INVALID_FILE, "Ses kaynağı normal dosya olmalı; sembolik bağlantı/aygıt/kuyruk kabul edilmez."); return -1;
    }
    if (before.st_size <= 0 || (guint64)before.st_size > RADIOS_MEDIA_MAX_SIZE_BYTES) {
        g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_LIMIT, "Ses dosyası boş veya 2 GiB sınırını aşıyor."); return -1;
    }
    gint fd = open_local_path(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { io_error(error, "Ses dosyası açılamadı"); return -1; }
    if (fstat(fd, identity) != 0) { io_error(error, "Ses dosyası kimliği okunamadı"); close(fd); return -1; }
    if (!same_file(&before, identity) || !S_ISREG(identity->st_mode)) {
        g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CHANGED, "Ses dosyası açılırken değişti."); close(fd); return -1;
    }
    return fd;
}

static gboolean unchanged(gint fd, const gchar *path, const struct stat *identity, GError **error)
{
    struct stat actual, named;
    if (fstat(fd, &actual) != 0 || lstat(path, &named) != 0 || !same_file(identity, &actual) ||
        !same_file(identity, &named) || !S_ISREG(named.st_mode)) {
        g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CHANGED, "Ses dosyası işlem sırasında değişti veya değiştirildi."); return FALSE;
    }
    return TRUE;
}

static gchar *hash_fd(gint fd, gint64 length, const RadioSCancelToken *token,
                      GCancellable *cancellable, GError **error)
{
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256); guchar buffer[65536]; gint64 position = 0;
    while (position < length) {
        if (!check_cancel(token, cancellable, error)) { g_checksum_free(sum); return NULL; }
        ssize_t count = pread(fd, buffer, (size_t)MIN((gint64)sizeof buffer, length - position), position);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CHANGED, "Ses dosyası özeti alınırken eksildi."); g_checksum_free(sum); return NULL;
        }
        g_checksum_update(sum, buffer, (gsize)count); position += count;
    }
    gchar *result = g_strdup(g_checksum_get_string(sum)); g_checksum_free(sum); return result;
}

static int local_read(void *opaque, uint8_t *buffer, int requested)
{
    LocalInput *input = opaque;
    if (cancelled(input->cancel, input->cancellable)) return AVERROR_EXIT;
    if (input->position >= input->length) return AVERROR_EOF;
    requested = (int)MIN((gint64)requested, input->length - input->position);
    ssize_t count;
    do count = pread(input->fd, buffer, (size_t)requested, input->position); while (count < 0 && errno == EINTR);
    if (count < 0) return AVERROR(errno);
    if (!count) return AVERROR_INVALIDDATA;
    input->position += count; return (int)count;
}

static int64_t local_seek(void *opaque, int64_t offset, int whence)
{
    LocalInput *input = opaque; whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return input->length;
    gint64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? input->position : whence == SEEK_END ? input->length : -1;
    if (base < 0 || offset < -base || offset > input->length - base) return AVERROR(EINVAL);
    input->position = base + offset; return input->position;
}
static int interrupted(void *opaque)
{ LocalInput *input = opaque; return cancelled(input->cancel, input->cancellable); }

static gchar *tag(AVDictionary *stream, AVDictionary *container, const gchar *name)
{
    const AVDictionaryEntry *entry = av_dict_get(stream, name, NULL, 0);
    if (!entry) entry = av_dict_get(container, name, NULL, 0);
    return entry ? g_utf8_make_valid(entry->value, -1) : g_strdup("");
}

static void compact_waveform(Measurement *measurement)
{
    GArray *wave = measurement->analysis->waveform;
    for (guint i = 0; i < wave->len / 4; ++i) {
        g_array_index(wave, gfloat, i * 2) = MIN(g_array_index(wave, gfloat, i * 4), g_array_index(wave, gfloat, i * 4 + 2));
        g_array_index(wave, gfloat, i * 2 + 1) = MAX(g_array_index(wave, gfloat, i * 4 + 1), g_array_index(wave, gfloat, i * 4 + 3));
    }
    g_array_set_size(wave, wave->len / 2); measurement->analysis->waveform_frames_per_bin *= 2;
}

static void finish_bin(Measurement *measurement)
{
    gfloat values[] = {measurement->bin_min, measurement->bin_max};
    g_array_append_vals(measurement->analysis->waveform, values, 2); measurement->bin_count = 0;
}

static gboolean measure_frame(Measurement *measurement, AVFrame *frame, GError **error)
{
    RadioSMediaAnalysis *analysis = measurement->analysis;
    if (!check_cancel(measurement->cancel, measurement->cancellable, error)) return FALSE;
    gint channels = frame->ch_layout.nb_channels, rate = frame->sample_rate;
    if (rate <= 0 || rate > 384000 || channels <= 0 || channels > 32 || frame->nb_samples <= 0 || frame->nb_samples > 1048576) {
        g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_LIMIT, "Ses kareleri hız/kanal/bellek sınırını aşıyor."); return FALSE;
    }
    if (frame->flags & AV_FRAME_FLAG_CORRUPT) { decode_error(error, AVERROR_INVALIDDATA); return FALSE; }
    if (!analysis->sample_rate) {
        analysis->sample_rate = (guint)rate; analysis->channels = (guint)channels;
        measurement->rhythm = radios_rhythm_analyzer_new((guint)rate);
        measurement->loudness = radios_loudness_meter_new((guint)rate, (guint)channels);
        measurement->input_format = (enum AVSampleFormat)frame->format;
        gint code = av_channel_layout_copy(&measurement->layout, &frame->ch_layout);
        if (code >= 0) code = swr_alloc_set_opts2(&measurement->convert, &measurement->layout, AV_SAMPLE_FMT_FLT,
            rate, &measurement->layout, measurement->input_format, rate, 0, NULL);
        if (code >= 0) code = swr_init(measurement->convert);
        if (code < 0) { decode_error(error, code); return FALSE; }
    } else if (analysis->sample_rate != (guint)rate || analysis->channels != (guint)channels ||
               measurement->input_format != (enum AVSampleFormat)frame->format || av_channel_layout_compare(&measurement->layout, &frame->ch_layout)) {
        g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_UNSUPPORTED, "Tek dosyada değişen ses hızı/kanal biçimi desteklenmiyor."); return FALSE;
    }
    if (analysis->sample_frames + (guint64)frame->nb_samples > (guint64)rate * 86400) {
        g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_LIMIT, "Çözülen ses 24 saat sınırını aşıyor."); return FALSE;
    }
    gfloat *samples = g_new(gfloat, (gsize)frame->nb_samples * (guint)channels);
    uint8_t *output[] = {(uint8_t *)samples};
    gint converted = swr_convert(measurement->convert, output, frame->nb_samples,
                                 (const uint8_t **)frame->extended_data, frame->nb_samples);
    if (converted != frame->nb_samples) { g_free(samples); decode_error(error, converted < 0 ? converted : AVERROR_INVALIDDATA); return FALSE; }
    enum AVSampleFormat packed = av_get_packed_sample_fmt(measurement->input_format);
    gdouble positive_rail = packed == AV_SAMPLE_FMT_S16 ? 32767.0 / 32768.0 : packed == AV_SAMPLE_FMT_U8 ? 127.0 / 128.0 : 1.0;
    for (gint i = 0; i < converted; ++i) {
        if (i % 4096 == 0 && !check_cancel(measurement->cancel, measurement->cancellable, error)) { g_free(samples); return FALSE; }
        if (!measurement->bin_count && analysis->waveform->len == RADIOS_MEDIA_MAX_WAVEFORM_BINS * 2) compact_waveform(measurement);
        for (gint channel = 0; channel < channels; ++channel) {
            gfloat sample = samples[(gsize)i * (guint)channels + (guint)channel];
            if (!isfinite(sample) || fabs((gdouble)sample) > 1000000) {
                g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_DECODE, "Ses örneği sonlu genlik sınırının dışında."); g_free(samples); return FALSE;
            }
            if (measurement->bin_count == 0 && channel == 0) measurement->bin_min = measurement->bin_max = sample;
            else { measurement->bin_min = MIN(measurement->bin_min, sample); measurement->bin_max = MAX(measurement->bin_max, sample); }
            analysis->peak = MAX(analysis->peak, fabs((gdouble)sample));
            measurement->squares += (long double)sample * sample;
            if (sample >= positive_rail || sample <= -1.0) ++analysis->clipped_samples;
        }
        radios_rhythm_analyzer_push_frame(measurement->rhythm, samples + (gsize)i * (guint)channels, (guint)channels);
        radios_loudness_meter_push_frame(measurement->loudness, samples + (gsize)i * (guint)channels);
        ++measurement->bin_count; ++analysis->sample_frames;
        if (measurement->bin_count == analysis->waveform_frames_per_bin) finish_bin(measurement);
    }
    g_free(samples); return TRUE;
}

/* Containers with explicit completion/size must not silently accept a cut file.
 * Raw MP3/ADTS at an intact packet boundary cannot prove an absent final packet. */
static gboolean container_complete(gint fd, gint64 length, const gchar *format,
    const RadioSCancelToken *cancel, GCancellable *cancellable, GError **error)
{
    guchar header[27];
    if (g_str_equal(format, "wav") && pread(fd, header, 12, 0) == 12 && !memcmp(header, "RIFF", 4)) {
        guint32 size = (guint32)header[4] | (guint32)header[5] << 8 | (guint32)header[6] << 16 | (guint32)header[7] << 24;
        if ((guint64)size + 8 > (guint64)length) { decode_error(error, AVERROR_INVALIDDATA); return FALSE; }
    }
    if (g_str_equal(format, "ogg")) {
        gint64 offset = 0; gboolean eos = FALSE;
        while (offset < length) {
            if (!check_cancel(cancel, cancellable, error)) return FALSE;
            if (length - offset < 27 || pread(fd, header, 27, offset) != 27 || memcmp(header, "OggS", 4) || header[4]) {
                decode_error(error, AVERROR_INVALIDDATA); return FALSE;
            }
            guchar lengths[255]; guint count = header[26], bytes = 27 + count;
            if (pread(fd, lengths, count, offset + 27) != (ssize_t)count) { decode_error(error, AVERROR_INVALIDDATA); return FALSE; }
            for (guint i = 0; i < count; ++i) bytes += lengths[i];
            if ((gint64)bytes > length - offset) { decode_error(error, AVERROR_INVALIDDATA); return FALSE; }
            eos = (header[5] & 4) != 0; offset += bytes;
        }
        if (!eos) { decode_error(error, AVERROR_INVALIDDATA); return FALSE; }
    }
    if (g_str_has_prefix(format, "mov,")) {
        gint64 offset = 0;
        while (offset < length) {
            if (!check_cancel(cancel, cancellable, error)) return FALSE;
            if (length - offset < 8 || pread(fd, header, 8, offset) != 8) { decode_error(error, AVERROR_INVALIDDATA); return FALSE; }
            guint64 size = (guint64)header[0] << 24 | (guint64)header[1] << 16 | (guint64)header[2] << 8 | header[3];
            guint minimum = 8;
            if (size == 1) {
                if (length - offset < 16 || pread(fd, header + 8, 8, offset + 8) != 8) { decode_error(error, AVERROR_INVALIDDATA); return FALSE; }
                size = 0; for (guint i = 8; i < 16; ++i) size = (size << 8) | header[i]; minimum = 16;
            } else if (!size) size = (guint64)(length - offset);
            if (size < minimum || size > (guint64)(length - offset)) { decode_error(error, AVERROR_INVALIDDATA); return FALSE; }
            offset += (gint64)size;
        }
    }
    return TRUE;
}

static RadioSMediaAnalysis *analyze_file(const gchar *path, const RadioSCancelToken *cancel,
                                         GCancellable *cancellable, gboolean probe_only, GError **error)
{
    g_return_val_if_fail(!error || !*error, NULL);
    if (!check_cancel(cancel, cancellable, error)) return NULL;
    struct stat identity; gint fd = open_source(path, &identity, error); if (fd < 0) return NULL;
    RadioSMediaAnalysis *result = g_new0(RadioSMediaAnalysis, 1);
    result->path = g_canonicalize_filename(path, NULL); result->source_name = g_filename_display_basename(path);
    result->size_bytes = (guint64)identity.st_size; result->waveform_frames_per_bin = 1;
    result->waveform = g_array_sized_new(FALSE, FALSE, sizeof(gfloat), RADIOS_MEDIA_MAX_WAVEFORM_BINS * 2);
    result->sha256 = hash_fd(fd, identity.st_size, cancel, cancellable, error);
    LocalInput input = {fd, 0, identity.st_size, cancel, cancellable};
    AVIOContext *io = NULL; AVFormatContext *format = NULL; AVCodecContext *decoder = NULL;
    AVFrame *frame = NULL; AVPacket *packet = NULL; AVDictionary *options = NULL;
    Measurement measurement = {.analysis = result, .cancel = cancel, .cancellable = cancellable};
    gboolean success = FALSE; gchar *final_hash = NULL;
    if (!result->sha256) goto cleanup;
    guchar *buffer = av_malloc(32768);
    if (!buffer) { g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_LIMIT, "Ses okuma belleği ayrılamadı."); goto cleanup; }
    io = avio_alloc_context(buffer, 32768, 0, &input, local_read, NULL, local_seek);
    if (!io) { av_free(buffer); g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_LIMIT, "Ses okuma bağlamı ayrılamadı."); goto cleanup; }
    io->seekable = AVIO_SEEKABLE_NORMAL;
    format = avformat_alloc_context();
    if (!format) { g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_LIMIT, "Ses biçimi bağlamı ayrılamadı."); goto cleanup; }
    format->pb = io; format->flags |= AVFMT_FLAG_CUSTOM_IO;
    format->interrupt_callback.callback = interrupted; format->interrupt_callback.opaque = &input;
    format->probesize = 1048576; format->max_analyze_duration = 5 * AV_TIME_BASE;
    format->max_streams = 16; format->max_probe_packets = 2048; format->error_recognition = AV_EF_EXPLODE;
    av_dict_set(&options, "protocol_whitelist", "", 0);
    av_dict_set(&options, "format_whitelist", "wav,mp3,flac,ogg,mov,mp4,m4a,3gp,3g2,mj2,aac,matroska,webm", 0);
    av_dict_set(&options, "enable_drefs", "0", 0);
    gint code = avformat_open_input(&format, path, NULL, &options); av_dict_free(&options);
    if (code < 0) { if (!check_cancel(cancel, cancellable, error)) goto cleanup; decode_error(error, code); goto cleanup; }
    if (!container_complete(fd, identity.st_size, format->iformat->name, cancel, cancellable, error)) goto cleanup;
    code = avformat_find_stream_info(format, NULL);
    if (code < 0) { if (!check_cancel(cancel, cancellable, error)) goto cleanup; decode_error(error, code); goto cleanup; }
    const AVCodec *codec = NULL; gint stream_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream_index < 0 || !codec) { g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_UNSUPPORTED, "Dosyada desteklenen bir ses akışı bulunamadı."); goto cleanup; }
    AVStream *stream = format->streams[stream_index];
    decoder = avcodec_alloc_context3(codec);
    if (!decoder) { g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_LIMIT, "Ses çözücü belleği ayrılamadı."); goto cleanup; }
    code = avcodec_parameters_to_context(decoder, stream->codecpar);
    decoder->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
    decoder->thread_count = 1; decoder->pkt_timebase = stream->time_base;
    if (code >= 0) code = avcodec_open2(decoder, codec, NULL);
    if (code < 0) { decode_error(error, code); goto cleanup; }
    result->codec = g_strdup(avcodec_get_name(stream->codecpar->codec_id)); result->format = g_strdup(format->iformat->name);
    result->bitrate = stream->codecpar->bit_rate;
    result->title = tag(stream->metadata, format->metadata, "title"); result->artist = tag(stream->metadata, format->metadata, "artist");
    result->album = tag(stream->metadata, format->metadata, "album"); result->genre = tag(stream->metadata, format->metadata, "genre");
    if (probe_only) {
        result->sample_rate = stream->codecpar->sample_rate;
        result->channels = stream->codecpar->ch_layout.nb_channels;
        result->duration_ms = stream->duration > 0 ? av_rescale_q(stream->duration, stream->time_base, (AVRational){1, 1000}) :
            (format->duration > 0 ? format->duration / 1000 : 0);
        result->bitrate = MAX((gint64)0, result->bitrate);
        if (!result->sample_rate || !result->channels || result->duration_ms <= 0 || result->duration_ms > 86400000) {
            g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_UNSUPPORTED, "Ses başlığında geçerli süre veya kanal bilgisi yok."); goto cleanup;
        }
        success = unchanged(fd, path, &identity, error) && check_cancel(cancel, cancellable, error);
        goto cleanup;
    }
    frame = av_frame_alloc(); packet = av_packet_alloc();
    if (!frame || !packet) { g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_LIMIT, "Ses kare belleği ayrılamadı."); goto cleanup; }
    for (;;) {
        if (!check_cancel(cancel, cancellable, error)) goto cleanup;
        code = av_read_frame(format, packet);
        if (code == AVERROR_EOF) break;
        if (code < 0) { decode_error(error, code); goto cleanup; }
        if (packet->stream_index != stream_index) { av_packet_unref(packet); continue; }
        if (packet->flags & AV_PKT_FLAG_CORRUPT) { decode_error(error, AVERROR_INVALIDDATA); goto cleanup; }
        code = avcodec_send_packet(decoder, packet); av_packet_unref(packet);
        if (code < 0) { decode_error(error, code); goto cleanup; }
        for (;;) {
            code = avcodec_receive_frame(decoder, frame);
            if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) break;
            if (code < 0) { decode_error(error, code); goto cleanup; }
            if (!measure_frame(&measurement, frame, error)) goto cleanup;
            av_frame_unref(frame);
        }
    }
    code = avcodec_send_packet(decoder, NULL);
    if (code < 0 && code != AVERROR_EOF) { decode_error(error, code); goto cleanup; }
    for (;;) {
        code = avcodec_receive_frame(decoder, frame);
        if (code == AVERROR_EOF) break;
        if (code < 0) { decode_error(error, code); goto cleanup; }
        if (!measure_frame(&measurement, frame, error)) goto cleanup;
        av_frame_unref(frame);
    }
    if (!result->sample_frames) { decode_error(error, AVERROR_INVALIDDATA); goto cleanup; }
    if (measurement.bin_count) finish_bin(&measurement);
    result->duration_ms = MAX(G_GINT64_CONSTANT(1), (gint64)((result->sample_frames * 1000 + result->sample_rate / 2) / result->sample_rate));
    result->rms = sqrt((gdouble)(measurement.squares / ((long double)result->sample_frames * result->channels)));
    result->clipped = result->clipped_samples > 0;
    if (result->bitrate <= 0) result->bitrate = (gint64)((long double)result->size_bytes * 8 * result->sample_rate / result->sample_frames);
    if (g_str_equal(result->format, "flac") && stream->duration > 0 &&
        av_rescale_q(stream->duration, stream->time_base, (AVRational){1, (gint)result->sample_rate}) != (gint64)result->sample_frames) {
        decode_error(error, AVERROR_INVALIDDATA); goto cleanup;
    }
    if (!unchanged(fd, path, &identity, error)) goto cleanup;
    final_hash = hash_fd(fd, identity.st_size, cancel, cancellable, error);
    if (!final_hash) goto cleanup;
    if (!g_str_equal(final_hash, result->sha256)) {
        g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CHANGED, "Ses baytları analiz sırasında değişti."); goto cleanup;
    }
    if (!unchanged(fd, path, &identity, error) || !check_cancel(cancel, cancellable, error)) goto cleanup;
    result->rhythm = radios_rhythm_analyzer_finish(measurement.rhythm);
    result->loudness.peak = result->peak;
    result->loudness.measured = radios_loudness_meter_finish(measurement.loudness, &result->loudness.integrated_lufs);
    success = TRUE;
cleanup:
    radios_rhythm_analyzer_free(measurement.rhythm); radios_loudness_meter_free(measurement.loudness);
    g_free(final_hash); swr_free(&measurement.convert); av_channel_layout_uninit(&measurement.layout);
    av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&decoder); av_dict_free(&options);
    if (format) avformat_close_input(&format);
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    close(fd);
    if (!success) { radios_media_analysis_free(result); return NULL; }
    return result;
}

RadioSMediaAnalysis *radios_media_analyze(const gchar *path, const RadioSCancelToken *cancel,
    GCancellable *cancellable, GError **error)
{ return analyze_file(path, cancel, cancellable, FALSE, error); }

gboolean radios_media_analysis_complete(JsonObject *analysis)
{
    JsonNode *rhythm = analysis ? json_object_get_member(analysis, "rhythm") : NULL; RadioSLoudness loudness;
    return rhythm && JSON_NODE_HOLDS_OBJECT(rhythm) && radios_rhythm_analysis_current(json_node_get_object(rhythm)) &&
        radios_loudness_read(analysis, &loudness);
}
void radios_media_analysis_free(RadioSMediaAnalysis *analysis)
{
    if (!analysis) return;
    g_free(analysis->path); g_free(analysis->relative_path); g_free(analysis->source_name); g_free(analysis->sha256);
    g_free(analysis->codec); g_free(analysis->format); g_free(analysis->title); g_free(analysis->artist);
    g_free(analysis->album); g_free(analysis->genre); g_clear_pointer(&analysis->waveform, g_array_unref); g_free(analysis);
}

JsonNode *radios_media_analysis_to_json(const RadioSMediaAnalysis *analysis)
{
    g_return_val_if_fail(analysis, NULL);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(node, object);
    json_object_set_string_member(object, "sha256", analysis->sha256);
    json_object_set_int_member(object, "size_bytes", (gint64)analysis->size_bytes);
    json_object_set_int_member(object, "duration_ms", analysis->duration_ms);
    json_object_set_int_member(object, "sample_rate", analysis->sample_rate); json_object_set_int_member(object, "channels", analysis->channels);
    json_object_set_string_member(object, "codec", analysis->codec); json_object_set_string_member(object, "format", analysis->format);
    json_object_set_int_member(object, "bitrate", analysis->bitrate); json_object_set_double_member(object, "peak", analysis->peak);
    json_object_set_double_member(object, "rms", analysis->rms); json_object_set_boolean_member(object, "clipped", analysis->clipped);
    json_object_set_int_member(object, "clipped_samples", (gint64)analysis->clipped_samples);
    json_object_set_int_member(object, "sample_frames", (gint64)analysis->sample_frames);
    json_object_set_int_member(object, "waveform_frames_per_bin", (gint64)analysis->waveform_frames_per_bin);
    JsonArray *waveform = json_array_new();
    for (guint i = 0; i < analysis->waveform->len; ++i) json_array_add_double_element(waveform, g_array_index(analysis->waveform, gfloat, i));
    json_object_set_array_member(object, "waveform", waveform);
    json_object_set_object_member(object, "rhythm", radios_rhythm_analysis_json(&analysis->rhythm));
    json_object_set_object_member(object, "loudness", radios_loudness_json(&analysis->loudness)); return node;
}

/* Confinement is descriptor-based: every import directory component is opened
 * with O_NOFOLLOW, so replacing a component with a symlink cannot redirect us. */
static gint import_directory(const RadioSConfig *config, gchar **absolute, gchar **relative, GError **error)
{
    if (!config || !config->project_root || !config->import_root) {
        g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CONFINEMENT, "Proje ve import klasörü yapılandırılmalı."); return -1;
    }
    gchar *root = g_canonicalize_filename(config->project_root, NULL);
    *absolute = g_canonicalize_filename(config->import_root, root);
    gsize length = strlen(root);
    if (!g_str_has_prefix(*absolute, root) || (*absolute)[length] != '/' || !(*absolute)[length + 1]) {
        g_free(root); g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CONFINEMENT, "Import klasörü RadioS projesinin içinde olmalı."); return -1;
    }
    *relative = g_strdup(*absolute + length + 1);
    gint fd = open_local_path(root, O_RDONLY | O_DIRECTORY); g_free(root);
    if (fd < 0) { io_error(error, "Proje klasörü açılamadı"); return -1; }
    gchar **parts = g_strsplit(*relative, "/", -1);
    for (guint i = 0; parts[i]; ++i) {
        if (mkdirat(fd, parts[i], 0700) != 0 && errno != EEXIST) { io_error(error, "Import klasörü oluşturulamadı"); close(fd); g_strfreev(parts); return -1; }
        gint next = openat(fd, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CONFINEMENT, "Import klasöründe bağlantı veya geçersiz dizin bileşeni var.");
            close(fd); g_strfreev(parts); return -1;
        }
        close(fd); fd = next;
    }
    g_strfreev(parts); return fd;
}

static RadioSMediaAnalysis *stage_file(const RadioSConfig *config, const gchar *source,
    const RadioSCancelToken *cancel, GCancellable *cancellable, gboolean probe_only, GError **error)
{
    g_return_val_if_fail(!error || !*error, NULL);
    if (!check_cancel(cancel, cancellable, error)) return NULL;
    struct stat identity; gint source_fd = open_source(source, &identity, error); if (source_fd < 0) return NULL;
    gchar *directory = NULL, *relative = NULL;
    gint directory_fd = import_directory(config, &directory, &relative, error);
    gchar *uuid = NULL, *name = NULL, *path = NULL; gint destination_fd = -1; gboolean created = FALSE;
    GChecksum *copy_hash = NULL;
    RadioSMediaAnalysis *result = NULL;
    if (directory_fd < 0) goto cleanup;
    uuid = g_uuid_string_random();
    const gchar *extension = strrchr(source, '.'); gchar *suffix = extension ? g_ascii_strdown(extension, -1) : g_strdup(".audio");
    if (!g_str_equal(suffix, ".wav") && !g_str_equal(suffix, ".mp3") && !g_str_equal(suffix, ".flac") &&
        !g_str_equal(suffix, ".ogg") && !g_str_equal(suffix, ".oga") && !g_str_equal(suffix, ".m4a") &&
        !g_str_equal(suffix, ".mp4") && !g_str_equal(suffix, ".aac")) { g_free(suffix); suffix = g_strdup(".audio"); }
    name = g_strconcat(uuid, suffix, NULL); g_free(suffix); path = g_build_filename(directory, name, NULL);
    destination_fd = openat(directory_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (destination_fd < 0) { io_error(error, "Özel medya kopyası oluşturulamadı"); goto cleanup; }
    created = TRUE; copy_hash = g_checksum_new(G_CHECKSUM_SHA256); guchar buffer[65536]; gint64 total = 0;
    while (total < identity.st_size) {
        if (!check_cancel(cancel, cancellable, error)) goto cleanup;
        ssize_t count = read(source_fd, buffer, (size_t)MIN((gint64)sizeof buffer, identity.st_size - total));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CHANGED, "Kaynak kopyalama sırasında eksildi."); goto cleanup; }
        for (ssize_t written = 0; written < count;) {
            ssize_t amount = write(destination_fd, buffer + written, (size_t)(count - written));
            if (amount < 0 && errno == EINTR) continue;
            if (amount <= 0) { io_error(error, "Medya kopyası yazılamadı"); goto cleanup; }
            written += amount;
        }
        g_checksum_update(copy_hash, buffer, (gsize)count); total += count;
    }
    if (!unchanged(source_fd, source, &identity, error)) goto cleanup;
    if (fchmod(destination_fd, 0600) != 0 || fsync(destination_fd) != 0 || fsync(directory_fd) != 0) {
        io_error(error, "Medya kopyası kalıcılaştırılamadı"); goto cleanup;
    }
    struct stat opened, named;
    if (fstat(directory_fd, &opened) != 0 || lstat(directory, &named) != 0 || !S_ISDIR(named.st_mode) ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino) {
        g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CONFINEMENT, "Import klasörü kopyalama sırasında değiştirildi."); goto cleanup;
    }
    struct stat copied;
    if (fstat(destination_fd, &copied) != 0) { io_error(error, "Medya kopyası kimliği alınamadı"); goto cleanup; }
    result = analyze_file(path, cancel, cancellable, probe_only, error);
    if (!result) goto cleanup;
    if (result->size_bytes != (guint64)identity.st_size || !g_str_equal(result->sha256, g_checksum_get_string(copy_hash))) {
        g_set_error_literal(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CHANGED, "Özel medya kopyası kaynak baytlarıyla eşleşmiyor.");
        radios_media_analysis_free(result); result = NULL; goto cleanup;
    }
    if (!check_cancel(cancel, cancellable, error) || !unchanged(source_fd, source, &identity, error) ||
        !unchanged(destination_fd, path, &copied, error)) {
        radios_media_analysis_free(result); result = NULL; goto cleanup;
    }
    result->relative_path = g_build_filename(relative, name, NULL);
    g_free(result->source_name); result->source_name = g_filename_display_basename(source);
cleanup:
    if (!result && created) { unlinkat(directory_fd, name, 0); fsync(directory_fd); }
    if (copy_hash) g_checksum_free(copy_hash);
    if (destination_fd >= 0) close(destination_fd);
    if (directory_fd >= 0) close(directory_fd);
    close(source_fd); g_free(directory); g_free(relative); g_free(uuid); g_free(name); g_free(path); return result;
}

RadioSMediaAnalysis *radios_media_stage_analyze(const RadioSConfig *config, const gchar *source,
    const RadioSCancelToken *cancel, GCancellable *cancellable, GError **error)
{ return stage_file(config, source, cancel, cancellable, FALSE, error); }

RadioSMediaAnalysis *radios_media_stage_probe(const RadioSConfig *config, const gchar *source,
    const RadioSCancelToken *cancel, GCancellable *cancellable, GError **error)
{ return stage_file(config, source, cancel, cancellable, TRUE, error); }
