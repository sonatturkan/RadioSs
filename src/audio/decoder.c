#define _POSIX_C_SOURCE 200809L
#include "radios/audio_decoder.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>

#define MAX_FILE_BYTES (G_GUINT64_CONSTANT(2) * 1024 * 1024 * 1024)
#define MAX_INPUT_SAMPLES 65536
#define MAX_OUTPUT_BUFFER_FRAMES (48000 * 10)
#define MAX_PACKET_BYTES (4 * 1024 * 1024)
#define MAX_TIMELINE_FRAMES (G_GUINT64_CONSTANT(48000) * 86400)

struct RadioSAudioDecoder {
    gchar *path;
    gint fd, stream_index;
    struct stat identity;
    gint64 io_position;
    GCancellable *active_cancel;
    AVIOContext *io;
    AVFormatContext *format;
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packet;
    SwrContext *resampler;
    AVChannelLayout source_layout;
    gint source_rate, source_format;
    gfloat *buffer;
    guint buffer_capacity, buffered, buffer_offset;
    guint64 position, converted_frames, source_frames;
    gboolean input_eof, sent_flush, codec_eof, drained, failed;
};

GQuark radios_audio_decoder_error_quark(void)
{ return g_quark_from_static_string("radios-audio-decoder-error"); }

static gboolean fail(GError **error, RadioSAudioDecoderError code, const gchar *message)
{ g_set_error_literal(error, RADIOS_AUDIO_DECODER_ERROR, code, message); return FALSE; }
static gboolean av_fail(GError **error, gint code)
{
    gchar message[AV_ERROR_MAX_STRING_SIZE]; av_strerror(code, message, sizeof message);
    g_set_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_DECODE,
        "Ses çözülemedi: %s", message); return FALSE;
}
static gboolean check_cancel(RadioSAudioDecoder *decoder, GError **error)
{
    if (!decoder->active_cancel || !g_cancellable_is_cancelled(decoder->active_cancel)) return TRUE;
    return fail(error, RADIOS_AUDIO_DECODER_ERROR_CANCELLED, "Ses çözme işlemi iptal edildi.");
}
static gboolean same_file(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
static gboolean unchanged(RadioSAudioDecoder *decoder, GError **error)
{
    struct stat fd_stat, name_stat;
    if (fstat(decoder->fd, &fd_stat) != 0 || lstat(decoder->path, &name_stat) != 0 ||
        !S_ISREG(fd_stat.st_mode) || !S_ISREG(name_stat.st_mode) ||
        !same_file(&decoder->identity, &fd_stat) || !same_file(&decoder->identity, &name_stat))
        return fail(error, RADIOS_AUDIO_DECODER_ERROR_CHANGED, "Doğrulanmış ses dosyası değişti veya değiştirildi.");
    return TRUE;
}
static gint open_local(const gchar *path)
{
    gchar *directory = g_path_get_dirname(path), *name = g_path_get_basename(path);
    gchar **parts = g_strsplit(directory, "/", -1);
    gint parent = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC), fd = -1;
    for (guint i = 0; parent >= 0 && parts[i]; ++i) {
        if (!*parts[i]) continue;
        gint next = openat(parent, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        gint saved = errno; close(parent); parent = next; errno = saved;
    }
    if (parent >= 0) {
        fd = openat(parent, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        gint saved = errno; close(parent); errno = saved;
    }
    g_strfreev(parts); g_free(directory); g_free(name); return fd;
}
static gboolean hash_valid(const gchar *sha)
{
    if (!sha || strlen(sha) != 64) return FALSE;
    for (guint i = 0; i < 64; ++i) if (!g_ascii_isxdigit(sha[i])) return FALSE;
    return TRUE;
}
static gboolean verify_hash(RadioSAudioDecoder *decoder, const gchar *sha, GError **error)
{
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256); guchar block[65536]; gint64 offset = 0;
    while (offset < decoder->identity.st_size) {
        if (!check_cancel(decoder, error)) { g_checksum_free(sum); return FALSE; }
        ssize_t count = pread(decoder->fd, block, (size_t)MIN((gint64)sizeof block,
            (gint64)decoder->identity.st_size - offset), offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { g_checksum_free(sum); return fail(error, RADIOS_AUDIO_DECODER_ERROR_IO, "Ses dosyasının tamamı okunamadı."); }
        g_checksum_update(sum, block, (gsize)count); offset += count;
    }
    gboolean matches = !g_ascii_strcasecmp(g_checksum_get_string(sum), sha); g_checksum_free(sum);
    if (!matches) return fail(error, RADIOS_AUDIO_DECODER_ERROR_HASH, "Ses dosyasının SHA-256 özeti kayıtla uyuşmuyor.");
    return unchanged(decoder, error);
}
static int local_read(void *opaque, uint8_t *buffer, int requested)
{
    RadioSAudioDecoder *decoder = opaque;
    if (decoder->active_cancel && g_cancellable_is_cancelled(decoder->active_cancel)) return AVERROR_EXIT;
    if (decoder->io_position >= decoder->identity.st_size) return AVERROR_EOF;
    requested = (int)MIN((gint64)requested, (gint64)decoder->identity.st_size - decoder->io_position);
    ssize_t count;
    do count = pread(decoder->fd, buffer, (size_t)requested, decoder->io_position); while (count < 0 && errno == EINTR);
    if (count < 0) return AVERROR(errno);
    if (!count) return AVERROR_INVALIDDATA;
    decoder->io_position += count; return (int)count;
}
static int64_t local_seek(void *opaque, int64_t offset, int whence)
{
    RadioSAudioDecoder *decoder = opaque; whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return decoder->identity.st_size;
    gint64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? decoder->io_position :
        whence == SEEK_END ? decoder->identity.st_size : -1;
    if (base < 0 || offset < -base || offset > decoder->identity.st_size - base) return AVERROR(EINVAL);
    decoder->io_position = base + offset; return decoder->io_position;
}
static int interrupted(void *opaque)
{
    RadioSAudioDecoder *decoder = opaque;
    return decoder->active_cancel && g_cancellable_is_cancelled(decoder->active_cancel);
}
static int deny_external(AVFormatContext *format, AVIOContext **io, const char *url, int flags, AVDictionary **options)
{ (void)format; (void)io; (void)url; (void)flags; (void)options; return AVERROR(EACCES); }

/* Explicit container boundaries reject silent demuxer EOF on truncated input.
 * Packet-aligned raw MP3/ADTS truncation has no independently declared end. */
static gboolean container_complete(RadioSAudioDecoder *decoder, GError **error)
{
    const gchar *name = decoder->format->iformat->name; gint64 length = decoder->identity.st_size;
    guchar header[27];
    if (!strcmp(name, "wav") && pread(decoder->fd, header, 12, 0) == 12 && !memcmp(header, "RIFF", 4)) {
        guint32 length_declared = (guint32)header[4] | (guint32)header[5] << 8 | (guint32)header[6] << 16 | (guint32)header[7] << 24;
        if ((guint64)length_declared + 8 > (guint64)length) return av_fail(error, AVERROR_INVALIDDATA);
    }
    if (!strcmp(name, "ogg")) {
        gint64 offset = 0; gboolean eos = FALSE;
        while (offset < length) {
            if (!check_cancel(decoder, error)) return FALSE;
            if (length - offset < 27 || pread(decoder->fd, header, 27, offset) != 27 || memcmp(header, "OggS", 4) || header[4])
                return av_fail(error, AVERROR_INVALIDDATA);
            guchar lengths[255]; guint count = header[26], bytes = 27 + count;
            if (pread(decoder->fd, lengths, count, offset + 27) != (ssize_t)count) return av_fail(error, AVERROR_INVALIDDATA);
            for (guint i = 0; i < count; ++i) bytes += lengths[i];
            if ((gint64)bytes > length - offset) return av_fail(error, AVERROR_INVALIDDATA);
            eos = (header[5] & 4) != 0; offset += bytes;
        }
        if (!eos) return av_fail(error, AVERROR_INVALIDDATA);
    }
    if (g_str_has_prefix(name, "mov,")) {
        gint64 offset = 0;
        while (offset < length) {
            if (!check_cancel(decoder, error)) return FALSE;
            if (length - offset < 8 || pread(decoder->fd, header, 8, offset) != 8) return av_fail(error, AVERROR_INVALIDDATA);
            guint64 bytes = (guint64)header[0] << 24 | (guint64)header[1] << 16 | (guint64)header[2] << 8 | header[3]; guint minimum = 8;
            if (bytes == 1) {
                if (length - offset < 16 || pread(decoder->fd, header + 8, 8, offset + 8) != 8) return av_fail(error, AVERROR_INVALIDDATA);
                bytes = 0; for (guint i = 8; i < 16; ++i) bytes = bytes << 8 | header[i]; minimum = 16;
            } else if (!bytes) bytes = (guint64)(length - offset);
            if (bytes < minimum || bytes > (guint64)(length - offset)) return av_fail(error, AVERROR_INVALIDDATA);
            offset += (gint64)bytes;
        }
    }
    return TRUE;
}
static void close_stream(RadioSAudioDecoder *decoder)
{
    swr_free(&decoder->resampler); av_channel_layout_uninit(&decoder->source_layout);
    av_frame_free(&decoder->frame); av_packet_free(&decoder->packet); avcodec_free_context(&decoder->codec);
    if (decoder->format) avformat_close_input(&decoder->format);
    if (decoder->io) { av_freep(&decoder->io->buffer); avio_context_free(&decoder->io); }
    decoder->source_rate = 0; decoder->buffered = decoder->buffer_offset = 0;
    decoder->input_eof = decoder->sent_flush = decoder->codec_eof = decoder->drained = FALSE;
    decoder->position = decoder->converted_frames = decoder->source_frames = 0;
    decoder->io_position = 0;
}
static gboolean open_stream(RadioSAudioDecoder *decoder, GError **error)
{
    guchar *buffer = av_malloc(32768);
    if (!buffer) return fail(error, RADIOS_AUDIO_DECODER_ERROR_LIMIT, "Ses okuma belleği ayrılamadı.");
    decoder->io = avio_alloc_context(buffer, 32768, 0, decoder, local_read, NULL, local_seek);
    if (!decoder->io) { av_free(buffer); return fail(error, RADIOS_AUDIO_DECODER_ERROR_LIMIT, "Ses okuma bağlamı ayrılamadı."); }
    decoder->io->seekable = AVIO_SEEKABLE_NORMAL; decoder->format = avformat_alloc_context();
    if (!decoder->format) return fail(error, RADIOS_AUDIO_DECODER_ERROR_LIMIT, "Ses biçimi bağlamı ayrılamadı.");
    decoder->format->pb = decoder->io; decoder->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    decoder->format->interrupt_callback.callback = interrupted; decoder->format->interrupt_callback.opaque = decoder;
    decoder->format->io_open = deny_external;
    decoder->format->probesize = 1048576; decoder->format->max_analyze_duration = 5 * AV_TIME_BASE;
    decoder->format->max_streams = 16; decoder->format->max_probe_packets = 2048; decoder->format->error_recognition = AV_EF_EXPLODE;
    AVDictionary *options = NULL; av_dict_set(&options, "protocol_whitelist", "", 0);
    av_dict_set(&options, "format_whitelist", "wav,mp3,flac,ogg,mov,mp4,m4a,3gp,3g2,mj2,aac,matroska,webm", 0);
    av_dict_set(&options, "enable_drefs", "0", 0);
    gint code = avformat_open_input(&decoder->format, decoder->path, NULL, &options); av_dict_free(&options);
    if (!check_cancel(decoder, error)) return FALSE;
    if (code < 0) return av_fail(error, code);
    if (!container_complete(decoder, error)) return FALSE;
    code = avformat_find_stream_info(decoder->format, NULL);
    if (!check_cancel(decoder, error)) return FALSE;
    if (code < 0) return av_fail(error, code);
    const AVCodec *codec = NULL;
    decoder->stream_index = av_find_best_stream(decoder->format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (decoder->stream_index < 0 || !codec) return av_fail(error, decoder->stream_index < 0 ? decoder->stream_index : AVERROR_DECODER_NOT_FOUND);
    decoder->codec = avcodec_alloc_context3(codec);
    if (!decoder->codec) return fail(error, RADIOS_AUDIO_DECODER_ERROR_LIMIT, "Ses çözücü belleği ayrılamadı.");
    AVStream *stream = decoder->format->streams[decoder->stream_index];
    if (stream->codecpar->ch_layout.nb_channels > 32 || stream->codecpar->sample_rate > 384000)
        return fail(error, RADIOS_AUDIO_DECODER_ERROR_LIMIT, "Dosyanın ses kanalı/hızı çözme sınırını aşıyor.");
    code = avcodec_parameters_to_context(decoder->codec, stream->codecpar);
    decoder->codec->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
    decoder->codec->thread_count = 1; decoder->codec->pkt_timebase = stream->time_base;
    decoder->codec->max_samples = MAX_INPUT_SAMPLES;
    if (code >= 0) code = avcodec_open2(decoder->codec, codec, NULL);
    if (code < 0) return av_fail(error, code);
    decoder->frame = av_frame_alloc(); decoder->packet = av_packet_alloc();
    if (!decoder->frame || !decoder->packet) return fail(error, RADIOS_AUDIO_DECODER_ERROR_LIMIT, "Ses kare belleği ayrılamadı.");
    decoder->failed = FALSE; return TRUE;
}
static gboolean reserve_buffer(RadioSAudioDecoder *decoder, gint frames, GError **error)
{
    if (frames <= 0 || frames > MAX_OUTPUT_BUFFER_FRAMES) return fail(error, RADIOS_AUDIO_DECODER_ERROR_LIMIT, "Çözülmüş ses tamponu 10 saniye sınırını aşıyor.");
    if ((guint)frames > decoder->buffer_capacity) {
        gfloat *buffer = g_try_realloc_n(decoder->buffer, (gsize)frames * 2, sizeof(gfloat));
        if (!buffer) return fail(error, RADIOS_AUDIO_DECODER_ERROR_LIMIT, "PCM tampon belleği ayrılamadı.");
        decoder->buffer = buffer; decoder->buffer_capacity = (guint)frames;
    }
    return TRUE;
}
static gboolean converted_valid(RadioSAudioDecoder *decoder, gint frames, GError **error)
{
    if (frames < 0) return av_fail(error, frames);
    if (decoder->converted_frames + (guint)frames > MAX_TIMELINE_FRAMES)
        return fail(error, RADIOS_AUDIO_DECODER_ERROR_LIMIT, "Çözülmüş ses 24 saat sınırını aşıyor.");
    for (guint i = 0; i < (guint)frames * 2; ++i) {
        if (i % 4096 == 0 && !check_cancel(decoder, error)) return FALSE;
        if (!isfinite(decoder->buffer[i]) || fabs((gdouble)decoder->buffer[i]) > 1000000)
            return fail(error, RADIOS_AUDIO_DECODER_ERROR_DECODE, "PCM örneği sonlu genlik sınırını aşıyor.");
    }
    decoder->converted_frames += (guint)frames; decoder->buffered = (guint)frames; decoder->buffer_offset = 0; return TRUE;
}
static gboolean convert_frame(RadioSAudioDecoder *decoder, GError **error)
{
    AVFrame *frame = decoder->frame; gint rate = frame->sample_rate, channels = frame->ch_layout.nb_channels;
    if (frame->flags & AV_FRAME_FLAG_CORRUPT) return av_fail(error, AVERROR_INVALIDDATA);
    if (rate < 8000 || rate > 384000 || channels < 1 || channels > 32 || frame->nb_samples <= 0 || frame->nb_samples > MAX_INPUT_SAMPLES)
        return fail(error, RADIOS_AUDIO_DECODER_ERROR_LIMIT, "Ses hızı/kanalı/kare belleği desteklenen sınırların dışında.");
    if (!decoder->source_rate) {
        decoder->source_rate = rate; decoder->source_format = frame->format;
        gint code = av_channel_layout_copy(&decoder->source_layout, &frame->ch_layout);
        /* Unknown layouts get the conventional layout for their channel count.
         * This affects only routing, never the actual decoded frame count. */
        if (code >= 0 && decoder->source_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_uninit(&decoder->source_layout); av_channel_layout_default(&decoder->source_layout, channels);
        }
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (code >= 0) code = swr_alloc_set_opts2(&decoder->resampler, &stereo, AV_SAMPLE_FMT_FLT, 48000,
            &decoder->source_layout, (enum AVSampleFormat)frame->format, rate, 0, NULL);
        if (code >= 0 && channels == 1) {
            const gdouble matrix[] = {1.0, 1.0};
            code = swr_set_matrix(decoder->resampler, matrix, 1);
        }
        if (code >= 0) code = swr_init(decoder->resampler);
        if (code < 0) return av_fail(error, code);
    } else if (decoder->source_rate != rate || decoder->source_format != frame->format ||
        decoder->source_layout.nb_channels != channels ||
        (frame->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC && av_channel_layout_compare(&decoder->source_layout, &frame->ch_layout)))
        return fail(error, RADIOS_AUDIO_DECODER_ERROR_DECODE, "Dosyanın ortasında değişen ses biçimi desteklenmiyor.");
    gint64 requested = av_rescale_rnd(swr_get_delay(decoder->resampler, rate) + frame->nb_samples, 48000, rate, AV_ROUND_UP);
    if (requested > MAX_OUTPUT_BUFFER_FRAMES)
        return fail(error, RADIOS_AUDIO_DECODER_ERROR_LIMIT, "Yeniden örneklenmiş ses tamponu 10 saniye sınırını aşıyor.");
    if (!reserve_buffer(decoder, (gint)requested, error)) return FALSE;
    uint8_t *output[] = {(uint8_t *)decoder->buffer};
    gint frames = swr_convert(decoder->resampler, output, (gint)requested, (const uint8_t **)frame->extended_data, frame->nb_samples);
    decoder->source_frames += (guint)frame->nb_samples; return converted_valid(decoder, frames, error);
}
static gboolean fill_buffer(RadioSAudioDecoder *decoder, GError **error)
{
    decoder->buffered = decoder->buffer_offset = 0;
    for (;;) {
        if (!check_cancel(decoder, error)) return FALSE;
        if (decoder->codec_eof) {
            if (decoder->drained) return TRUE;
            AVStream *stream = decoder->format->streams[decoder->stream_index];
            if (!strcmp(decoder->format->iformat->name, "flac") && stream->duration > 0 && decoder->source_rate > 0 &&
                av_rescale_q(stream->duration, stream->time_base, (AVRational){1, decoder->source_rate}) != (gint64)decoder->source_frames)
                return av_fail(error, AVERROR_INVALIDDATA);
            if (!decoder->resampler) return fail(error, RADIOS_AUDIO_DECODER_ERROR_DECODE, "Dosyada çözülebilir ses örneği yok.");
            if (!reserve_buffer(decoder, 4096, error)) return FALSE;
            uint8_t *output[] = {(uint8_t *)decoder->buffer};
            gint frames = swr_convert(decoder->resampler, output, 4096, NULL, 0);
            if (!converted_valid(decoder, frames, error)) return FALSE;
            if (!frames) decoder->drained = TRUE;
            return TRUE;
        }
        gint code = avcodec_receive_frame(decoder->codec, decoder->frame);
        if (code == 0) {
            gboolean result = convert_frame(decoder, error); av_frame_unref(decoder->frame);
            if (!result) return FALSE;
            if (decoder->buffered) return TRUE;
            continue;
        }
        if (code == AVERROR_EOF) { decoder->codec_eof = TRUE; continue; }
        if (code != AVERROR(EAGAIN)) return av_fail(error, code);
        if (decoder->input_eof) {
            if (decoder->sent_flush) return av_fail(error, AVERROR_INVALIDDATA);
            code = avcodec_send_packet(decoder->codec, NULL);
            if (code < 0 && code != AVERROR_EOF) return av_fail(error, code);
            decoder->sent_flush = TRUE; if (code == AVERROR_EOF) decoder->codec_eof = TRUE; continue;
        }
        do {
            av_packet_unref(decoder->packet);
            code = av_read_frame(decoder->format, decoder->packet);
            if (!check_cancel(decoder, error)) return FALSE;
            if (code == AVERROR_EOF) { decoder->input_eof = TRUE; break; }
            if (code < 0) return av_fail(error, code);
            if (decoder->packet->size > MAX_PACKET_BYTES) return fail(error, RADIOS_AUDIO_DECODER_ERROR_LIMIT, "Ses paketi 4 MiB sınırını aşıyor.");
        } while (decoder->packet->stream_index != decoder->stream_index);
        if (decoder->input_eof) { av_packet_unref(decoder->packet); continue; }
        if (decoder->packet->flags & AV_PKT_FLAG_CORRUPT) return av_fail(error, AVERROR_INVALIDDATA);
        code = avcodec_send_packet(decoder->codec, decoder->packet); av_packet_unref(decoder->packet);
        if (code < 0) return av_fail(error, code);
    }
}

RadioSAudioDecoder *radios_audio_decoder_open(const gchar *path, const gchar *expected_sha256,
    GCancellable *cancellable, GError **error)
{
    g_return_val_if_fail(!error || !*error, NULL);
    if (!path || !*path || strstr(path, "://") || !hash_valid(expected_sha256)) {
        fail(error, RADIOS_AUDIO_DECODER_ERROR_INVALID, "Yerel dosya yolu ve 64 haneli SHA-256 özeti gerekli."); return NULL;
    }
    RadioSAudioDecoder *decoder = g_new0(RadioSAudioDecoder, 1); decoder->fd = -1;
    decoder->path = g_canonicalize_filename(path, NULL); decoder->active_cancel = cancellable;
    if (!check_cancel(decoder, error)) goto failed;
    struct stat before;
    if (lstat(decoder->path, &before) != 0 || !S_ISREG(before.st_mode)) {
        fail(error, RADIOS_AUDIO_DECODER_ERROR_INVALID, "Ses kaynağı normal yerel dosya olmalı; sembolik bağlantı/aygıt/kuyruk kabul edilmez."); goto failed;
    }
    if (before.st_size <= 0 || (guint64)before.st_size > MAX_FILE_BYTES) {
        fail(error, RADIOS_AUDIO_DECODER_ERROR_LIMIT, "Ses dosyası boş veya 2 GiB sınırını aşıyor."); goto failed;
    }
    decoder->fd = open_local(decoder->path);
    if (decoder->fd < 0) { fail(error, RADIOS_AUDIO_DECODER_ERROR_IO, "Ses dosyası açılamadı; tüm yol bileşenleri normal dizin olmalı."); goto failed; }
    if (fstat(decoder->fd, &decoder->identity) != 0 || !same_file(&before, &decoder->identity)) {
        fail(error, RADIOS_AUDIO_DECODER_ERROR_CHANGED, "Ses dosyası açılırken değişti."); goto failed;
    }
    if (!verify_hash(decoder, expected_sha256, error) || !open_stream(decoder, error) || !fill_buffer(decoder, error) ||
        !unchanged(decoder, error) || !check_cancel(decoder, error)) goto failed;
    decoder->active_cancel = NULL; return decoder;
failed:
    radios_audio_decoder_close(decoder); return NULL;
}
gssize radios_audio_decoder_read(RadioSAudioDecoder *decoder, gfloat *stereo, guint max_frames,
    GCancellable *cancellable, GError **error)
{
    g_return_val_if_fail(!error || !*error, -1);
    if (!decoder || !stereo || !max_frames || max_frames > RADIOS_AUDIO_MAX_READ_FRAMES) {
        fail(error, RADIOS_AUDIO_DECODER_ERROR_INVALID, "PCM okuma isteği 1..4096 kare olmalı."); return -1;
    }
    decoder->active_cancel = cancellable; guint copied = 0;
    if (!check_cancel(decoder, error) || !unchanged(decoder, error)) goto failed;
    if (decoder->failed) { fail(error, RADIOS_AUDIO_DECODER_ERROR_DECODE, "Çözücü hata durumunda; yeniden yükleyin veya konumu sıfırlayın."); goto failed; }
    while (copied < max_frames) {
        if (!check_cancel(decoder, error)) goto failed;
        if (decoder->buffer_offset == decoder->buffered && !fill_buffer(decoder, error)) goto failed;
        guint count = MIN(max_frames - copied, decoder->buffered - decoder->buffer_offset);
        if (!count) break;
        memcpy(stereo + copied * 2, decoder->buffer + decoder->buffer_offset * 2, (gsize)count * 2 * sizeof(gfloat));
        decoder->buffer_offset += count; copied += count;
    }
    if (!unchanged(decoder, error) || !check_cancel(decoder, error)) goto failed;
    decoder->position += copied; decoder->active_cancel = NULL; return copied;
failed:
    decoder->failed = TRUE; decoder->active_cancel = NULL; return -1;
}
gboolean radios_audio_decoder_seek(RadioSAudioDecoder *decoder, guint64 output_frame,
    GCancellable *cancellable, GError **error)
{
    g_return_val_if_fail(!error || !*error, FALSE);
    if (!decoder || output_frame > MAX_TIMELINE_FRAMES) return fail(error, RADIOS_AUDIO_DECODER_ERROR_SEEK, "Ses konumu 24 saat sınırının dışında.");
    decoder->active_cancel = cancellable;
    if (!check_cancel(decoder, error) || !unchanged(decoder, error)) { decoder->active_cancel = NULL; return FALSE; }
    /* A forward target continues from the current position: only the frames
     * in between are decoded and discarded, so a seek ahead costs O(delta)
     * rather than a reopen plus a decode from the origin (R8.3). The output
     * is identical, because the frame count since the origin is the only
     * position this decoder reports. Any other target reopens the same
     * verified descriptor, which resets timestamp origin, codec
     * priming/skip-samples metadata, demuxer state and resampler phase
     * together. A generic timestamp seek cannot promise sample-exact
     * compressed audio. */
    if (decoder->failed || !decoder->format || output_frame < decoder->position) {
        close_stream(decoder);
        if (!open_stream(decoder, error)) goto failed;
    }
    gfloat discard[RADIOS_AUDIO_MAX_READ_FRAMES * 2];
    while (decoder->position < output_frame) {
        guint requested = (guint)MIN((guint64)RADIOS_AUDIO_MAX_READ_FRAMES, output_frame - decoder->position);
        gssize frames = radios_audio_decoder_read(decoder, discard, requested, cancellable, error);
        decoder->active_cancel = cancellable;
        if (frames < 0) goto failed;
        if (!frames) {
            fail(error, RADIOS_AUDIO_DECODER_ERROR_SEEK, "İstenen konum gerçek çözülmüş sesin sonunda veya dışında.");
            decoder->active_cancel = NULL; return FALSE;
        }
    }
    if (!check_cancel(decoder, error) || !unchanged(decoder, error)) goto failed;
    decoder->active_cancel = NULL; return TRUE;
failed:
    decoder->failed = TRUE; decoder->active_cancel = NULL; return FALSE;
}
guint64 radios_audio_decoder_position_frames(const RadioSAudioDecoder *decoder)
{ return decoder ? decoder->position : 0; }
void radios_audio_decoder_close(RadioSAudioDecoder *decoder)
{
    if (!decoder) return;
    close_stream(decoder); if (decoder->fd >= 0) close(decoder->fd);
    g_free(decoder->buffer); g_free(decoder->path); g_free(decoder);
}
