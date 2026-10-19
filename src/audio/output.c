#define _POSIX_C_SOURCE 200809L
#include "audio_internal.h"
#include <curl/curl.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#define QUEUE_FRAMES (48000u * 5u)
#define WAV_MAX_FRAMES ((G_MAXUINT32 - 36u) / 4u)

struct RadioSAudioOutput {
    gatomicrefcount refs;
    GMutex mutex;
    GCond condition;
    GThread *thread, *metadata_thread;
    gboolean closing, requested, metadata_requested;
    RadioSAudioOutputKind kind;
    RadioSAudioOutputSnapshot snapshot;
    gchar *target, *username, *password;
    guint bitrate;
    /* Patchbay server runs; plain URL streams keep protocol ICECAST and no station headers. */
    RadioSAudioStreamProtocol protocol;
    gboolean server, public_listing;
    gchar *host, *station, *genre, *description, *website;
    guint port, stream_id;
    gfloat *samples;
    _Atomic guint64 read, write, received, processed, bytes;
    _Atomic guint overrun, pushing;
    _Atomic gboolean accepting, stopping;
    _Atomic gint64 stop_time;
};

typedef struct {
    RadioSAudioOutput *output;
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packet;
    gint packet_offset;
    gint64 pts;
    gboolean flushing, failed, starved;
    gfloat pcm[2304 * 2];
} Encoder;

static gboolean fail(GError **error, RadioSAudioError code, const gchar *message)
{ g_set_error_literal(error, RADIOS_AUDIO_ERROR_DOMAIN, code, message); return FALSE; }
static gboolean active_state(RadioSAudioOutputState state)
{ return state == RADIOS_AUDIO_OUTPUT_STARTING || state == RADIOS_AUDIO_OUTPUT_ACTIVE || state == RADIOS_AUDIO_OUTPUT_STOPPING; }
static void activate(RadioSAudioOutput *output)
{
    g_mutex_lock(&output->mutex);
    output->snapshot.state = atomic_load(&output->stopping) ? RADIOS_AUDIO_OUTPUT_STOPPING : RADIOS_AUDIO_OUTPUT_ACTIVE;
    g_mutex_unlock(&output->mutex);
}
static guint take(RadioSAudioOutput *output, gfloat *samples, guint wanted, guint minimum, gboolean *starved)
{
    gint64 waiting_since = g_get_monotonic_time();
    for (;;) {
        guint64 read = atomic_load_explicit(&output->read, memory_order_relaxed);
        guint64 write = atomic_load_explicit(&output->write, memory_order_acquire);
        gboolean done = atomic_load(&output->stopping) && !atomic_load(&output->pushing);
        guint count = (guint)MIN((guint64)wanted, write - read);
        if (count >= minimum || done) {
            for (guint i = 0; i < count; ++i) {
                guint slot = (guint)((read + i) % QUEUE_FRAMES);
                samples[i * 2] = output->samples[slot * 2]; samples[i * 2 + 1] = output->samples[slot * 2 + 1];
            }
            atomic_store_explicit(&output->read, read + count, memory_order_release); return count;
        }
        if (starved && g_get_monotonic_time() - waiting_since >= 5000000) { *starved = TRUE; return 0; }
        g_mutex_lock(&output->mutex);
        g_cond_wait_until(&output->condition, &output->mutex, g_get_monotonic_time() + 10000);
        g_mutex_unlock(&output->mutex);
    }
}
void radios_audio_output_push_internal(RadioSAudioOutput *output, const gfloat *samples, guint frames)
{
    if (!output || !atomic_load(&output->accepting)) return;
    atomic_fetch_add(&output->pushing, 1);
    if (atomic_load(&output->accepting)) {
        guint64 write = atomic_load_explicit(&output->write, memory_order_relaxed);
        guint64 read = atomic_load_explicit(&output->read, memory_order_acquire);
        if ((guint64)frames > QUEUE_FRAMES - (write - read)) {
            atomic_fetch_add(&output->overrun, frames);
            atomic_store(&output->accepting, FALSE); atomic_store(&output->stopping, TRUE);
            atomic_store(&output->stop_time, g_get_monotonic_time());
        } else {
            for (guint i = 0; i < frames; ++i) {
                guint slot = (guint)((write + i) % QUEUE_FRAMES);
                output->samples[slot * 2] = samples[i * 2]; output->samples[slot * 2 + 1] = samples[i * 2 + 1];
            }
            atomic_fetch_add(&output->received, frames);
            atomic_store_explicit(&output->write, write + frames, memory_order_release);
        }
    }
    atomic_fetch_sub(&output->pushing, 1); g_cond_signal(&output->condition);
}
static void le16(guint8 *data, guint16 value)
{ data[0] = (guint8)value; data[1] = (guint8)(value >> 8); }
static void le32(guint8 *data, guint32 value)
{ for (guint i = 0; i < 4; ++i) data[i] = (guint8)(value >> (i * 8)); }
static void wav_header(guint8 header[44], guint64 frames)
{
    memset(header, 0, 44); memcpy(header, "RIFF", 4); le32(header + 4, (guint32)(frames * 4 + 36));
    memcpy(header + 8, "WAVEfmt ", 8); le32(header + 16, 16); le16(header + 20, 1); le16(header + 22, 2);
    le32(header + 24, 48000); le32(header + 28, 192000); le16(header + 32, 4); le16(header + 34, 16);
    memcpy(header + 36, "data", 4); le32(header + 40, (guint32)(frames * 4));
}
static gboolean write_all(gint fd, const guint8 *data, gsize bytes)
{
    while (bytes) {
        ssize_t written = write(fd, data, bytes);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return FALSE;
        data += written; bytes -= (gsize)written;
    }
    return TRUE;
}
static gboolean record_run(RadioSAudioOutput *output, const gchar *path, gchar error[256])
{
    gint fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) { g_snprintf(error, 256, "WAV could not be created: %s", g_strerror(errno)); return FALSE; }
    guint8 header[44], bytes[4096 * 4]; gfloat pcm[4096 * 2]; guint64 total = 0;
    wav_header(header, 0); gboolean okay = write_all(fd, header, sizeof header);
    if (!okay) g_strlcpy(error, "WAV header write failed", 256);
    if (okay) activate(output);
    while (okay) {
        guint count = take(output, pcm, 4096, 1, NULL); if (!count) break;
        if (total + count > WAV_MAX_FRAMES) { g_strlcpy(error, "WAV reached the 4 GiB RIFF size limit", 256); okay = FALSE; break; }
        for (guint i = 0; i < count * 2; ++i) {
            gfloat value = isfinite(pcm[i]) ? CLAMP(pcm[i], -1, 1) : 0;
            gint16 sample = (gint16)lrintf(value * (value < 0 ? 32768.0f : 32767.0f));
            le16(bytes + i * 2, (guint16)sample);
        }
        if (!write_all(fd, bytes, count * 4)) { g_strlcpy(error, "WAV PCM write failed", 256); okay = FALSE; break; }
        total += count; atomic_store(&output->processed, total); atomic_store(&output->bytes, total * 4);
    }
    wav_header(header, total);
    gboolean finalized = lseek(fd, 0, SEEK_SET) == 0 && write_all(fd, header, sizeof header) && fsync(fd) == 0;
    if (close(fd) != 0) finalized = FALSE;
    g_mutex_lock(&output->mutex); output->snapshot.recording_finalized = finalized; g_mutex_unlock(&output->mutex);
    if (!finalized) { g_strlcpy(error, "WAV finalization failed", 256); okay = FALSE; }
    return okay;
}
static gboolean encoder_open(Encoder *encoder, RadioSAudioOutput *output, guint bitrate)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("libmp3lame");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_MP3);
    if (!codec) return FALSE;
    encoder->output = output; encoder->codec = avcodec_alloc_context3(codec);
    if (!encoder->codec) return FALSE;
    encoder->codec->sample_rate = 48000; encoder->codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
    encoder->codec->bit_rate = (gint64)bitrate * 1000;
    av_channel_layout_default(&encoder->codec->ch_layout, 2);
    if (avcodec_open2(encoder->codec, codec, NULL) < 0 || encoder->codec->frame_size > 2304) return FALSE;
    encoder->frame = av_frame_alloc(); encoder->packet = av_packet_alloc();
    if (!encoder->frame || !encoder->packet) return FALSE;
    encoder->frame->nb_samples = encoder->codec->frame_size;
    encoder->frame->format = encoder->codec->sample_fmt; encoder->frame->sample_rate = 48000;
    if (av_channel_layout_copy(&encoder->frame->ch_layout, &encoder->codec->ch_layout) < 0) return FALSE;
    return av_frame_get_buffer(encoder->frame, 0) >= 0;
}
static void encoder_clear(Encoder *encoder)
{ av_packet_free(&encoder->packet); av_frame_free(&encoder->frame); avcodec_free_context(&encoder->codec); }
static gboolean next_packet(Encoder *encoder)
{
    for (;;) {
        av_packet_unref(encoder->packet); encoder->packet_offset = 0;
        gint result = avcodec_receive_packet(encoder->codec, encoder->packet);
        if (result == 0) return TRUE;
        if (result == AVERROR_EOF) return FALSE;
        if (result != AVERROR(EAGAIN)) { encoder->failed = TRUE; return FALSE; }
        if (encoder->flushing) { encoder->failed = TRUE; return FALSE; }
        guint wanted = (guint)encoder->codec->frame_size;
        guint count = take(encoder->output, encoder->pcm, wanted, wanted, &encoder->starved);
        if (encoder->starved) { encoder->failed = TRUE; return FALSE; }
        if (!count) {
            encoder->flushing = TRUE;
            if (avcodec_send_frame(encoder->codec, NULL) < 0) { encoder->failed = TRUE; return FALSE; }
            continue;
        }
        if (av_frame_make_writable(encoder->frame) < 0) { encoder->failed = TRUE; return FALSE; }
        encoder->frame->nb_samples = (gint)count; encoder->frame->pts = encoder->pts; encoder->pts += count;
        for (guint channel = 0; channel < 2; ++channel) {
            gfloat *plane = (gfloat *)encoder->frame->data[channel];
            for (guint i = 0; i < count; ++i) plane[i] = encoder->pcm[i * 2 + channel];
        }
        if (avcodec_send_frame(encoder->codec, encoder->frame) < 0) { encoder->failed = TRUE; return FALSE; }
        atomic_fetch_add(&encoder->output->processed, count);
    }
}
static size_t stream_read(char *destination, size_t size, size_t count, void *data)
{
    Encoder *encoder = data; gsize capacity = size * count;
    if (atomic_load(&encoder->output->overrun)) return CURL_READFUNC_ABORT;
    if (encoder->packet_offset == encoder->packet->size && !next_packet(encoder))
        return encoder->failed ? CURL_READFUNC_ABORT : 0;
    gsize copied = MIN(capacity, (gsize)(encoder->packet->size - encoder->packet_offset));
    memcpy(destination, encoder->packet->data + encoder->packet_offset, copied); encoder->packet_offset += (gint)copied;
    return copied;
}
static size_t discard_response(char *data, size_t size, size_t count, void *unused)
{ (void)data; (void)unused; return size * count; }
static size_t stream_header(char *data, size_t size, size_t count, void *userdata)
{
    RadioSAudioOutput *output = userdata; gsize length = size * count;
    if (length >= 12 && !memcmp(data, "HTTP/", 5)) {
        gchar line[64]; gsize copied = MIN(length, sizeof line - 1); memcpy(line, data, copied); line[copied] = 0;
        gchar *space = strchr(line, ' '); glong status = space ? (glong)g_ascii_strtoll(space + 1, NULL, 10) : 0;
        if (status >= 200 && status <= 599) {
            g_mutex_lock(&output->mutex); output->snapshot.http_status = status;
            output->snapshot.response_confirmed = status < 300; g_mutex_unlock(&output->mutex);
        }
    }
    return length;
}
static int stream_progress(void *data, curl_off_t down_total, curl_off_t down_now,
    curl_off_t up_total, curl_off_t up_now)
{
    (void)down_total; (void)down_now; (void)up_total; RadioSAudioOutput *output = data;
    if (up_now >= 0) atomic_store(&output->bytes, (guint64)up_now);
    gint64 stopped = atomic_load(&output->stop_time);
    return stopped && g_get_monotonic_time() - stopped > 3000000;
}
static void curl_common(CURL *curl, const gchar *url, const gchar *username, const gchar *password)
{
    curl_easy_setopt(curl, CURLOPT_URL, url); curl_easy_setopt(curl, CURLOPT_USERNAME, username);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password); curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https"); curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L); curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "RadioS native C output");
}
static gboolean stream_run(RadioSAudioOutput *output, const gchar *url,
    const gchar *username, const gchar *password, guint bitrate, gchar error[256])
{
    Encoder encoder = {0};
    if (!encoder_open(&encoder, output, bitrate)) { g_strlcpy(error, "Native MP3 encoder could not be opened", 256); encoder_clear(&encoder); return FALSE; }
    CURL *curl = curl_easy_init();
    if (!curl) { g_strlcpy(error, "Streaming network handle could not be opened", 256); encoder_clear(&encoder); return FALSE; }
    curl_common(curl, url, username, password);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: audio/mpeg"); headers = curl_slist_append(headers, "Expect:");
    /* Icecast 2.4 source PUT reads an indefinite raw body. Sending HTTP
     * chunk boundaries would contaminate the MP3 stream on those servers. */
    headers = curl_slist_append(headers, "Transfer-Encoding:");
    gchar *station = NULL;
    if (output->server) {
        const gchar *names[] = {"Ice-Name", "Ice-Genre", "Ice-Description", "Ice-URL"};
        const gchar *values[] = {output->station, output->genre, output->description, output->website};
        for (guint i = 0; i < G_N_ELEMENTS(names); ++i) if (values[i] && *values[i]) {
            gchar *line = g_strdup_printf("%s: %s", names[i], values[i]); headers = curl_slist_append(headers, line); g_free(line);
        }
        station = g_strdup_printf("Ice-Audio-Info: ice-samplerate=48000;ice-bitrate=%u;ice-channels=2", bitrate);
        headers = curl_slist_append(headers, station);
    }
    headers = curl_slist_append(headers, output->server && output->public_listing ? "Ice-Public: 1" : "Ice-Public: 0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers); curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, stream_header); curl_easy_setopt(curl, CURLOPT_HEADERDATA, output);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, stream_read); curl_easy_setopt(curl, CURLOPT_READDATA, &encoder);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)-1);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L); curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 5L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L); curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stream_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, output);
    activate(output); CURLcode result = curl_easy_perform(curl); long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_off_t uploaded = 0; curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD_T, &uploaded);
    atomic_store(&output->bytes, (guint64)MAX(uploaded, 0));
    g_mutex_lock(&output->mutex); output->snapshot.http_status = status;
    output->snapshot.response_confirmed = status >= 200 && status < 300;
    g_mutex_unlock(&output->mutex);
    gboolean stopped = atomic_load(&output->stopping);
    gboolean okay = !encoder.failed && ((result == CURLE_OK && status >= 200 && status < 300) ||
        (stopped && result == CURLE_ABORTED_BY_CALLBACK && !atomic_load(&output->overrun)));
    if (!okay) {
        if (encoder.starved) g_strlcpy(error, "MAIN PCM stopped producing frames for five seconds", 256);
        else if (encoder.failed) g_strlcpy(error, "Native MP3 encoding failed", 256);
        else if (result != CURLE_OK) g_snprintf(error, 256, "Stream transport failed: %s", curl_easy_strerror(result));
        else g_snprintf(error, 256, "Stream endpoint returned HTTP %ld", status);
    } else if (!stopped) { okay = FALSE; g_strlcpy(error, "Stream endpoint ended the live upload", 256); }
    curl_slist_free_all(headers); g_free(station); curl_easy_cleanup(curl); encoder_clear(&encoder); return okay;
}
static gboolean socket_write(GOutputStream *stream, const void *data, gsize size, RadioSAudioOutput *output)
{
    gsize written = 0;
    if (!g_output_stream_write_all(stream, data, size, &written, NULL, NULL)) return FALSE;
    atomic_fetch_add(&output->bytes, written); return TRUE;
}
/* Legacy SHOUTcast source: password line, "OK2" reply, icy headers, raw MP3. */
static gboolean shoutcast_run(RadioSAudioOutput *output, const gchar *password, guint bitrate, gchar error[256])
{
    Encoder encoder = {0};
    if (!encoder_open(&encoder, output, bitrate)) { g_strlcpy(error, "Native MP3 encoder could not be opened", 256); encoder_clear(&encoder); return FALSE; }
    GSocketClient *client = g_socket_client_new(); g_socket_client_set_timeout(client, 5);
    GError *failure = NULL;
    GSocketConnection *connection = g_socket_client_connect_to_host(client, output->host, (guint16)(output->port + 1), NULL, &failure);
    gboolean okay = FALSE;
    if (!connection) {
        g_snprintf(error, 256, "SHOUTcast server is unreachable on port %u: %s", output->port + 1, failure ? failure->message : "unknown");
        g_clear_error(&failure); g_object_unref(client); encoder_clear(&encoder); return FALSE;
    }
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    gchar *login = output->protocol == RADIOS_STREAM_SHOUTCAST2 ? g_strdup_printf("%s:#%u\r\n", password, output->stream_id) :
        g_strdup_printf("%s\r\n", password);
    gboolean sent = g_output_stream_write_all(out, login, strlen(login), NULL, NULL, NULL);
    memset(login, 0, strlen(login)); g_free(login);
    gchar reply[256] = ""; gsize length = 0;
    while (sent && length < sizeof reply - 1) {
        gchar c = 0; gssize got = g_input_stream_read(in, &c, 1, NULL, NULL);
        if (got <= 0) break;
        if (c == '\n') break;
        if (c != '\r') reply[length++] = c;
    }
    reply[length] = 0;
    if (!sent || !g_str_has_prefix(reply, "OK")) {
        g_snprintf(error, 256, reply[0] ? "SHOUTcast server refused the source login: %s" : "SHOUTcast server did not answer the source login", reply);
    } else {
        GString *headers = g_string_new(NULL);
        g_string_append_printf(headers, "icy-name:%s\r\nicy-genre:%s\r\nicy-url:%s\r\nicy-pub:%d\r\nicy-br:%u\r\n"
            "content-type:audio/mpeg\r\n\r\n", output->station ? output->station : "", output->genre ? output->genre : "",
            output->website ? output->website : "", output->public_listing ? 1 : 0, bitrate);
        okay = socket_write(out, headers->str, headers->len, output); g_string_free(headers, TRUE);
        g_mutex_lock(&output->mutex); output->snapshot.http_status = 200; output->snapshot.response_confirmed = TRUE; g_mutex_unlock(&output->mutex);
        if (okay) activate(output);
        while (okay && next_packet(&encoder)) {
            if (!socket_write(out, encoder.packet->data, (gsize)encoder.packet->size, output)) {
                g_strlcpy(error, "SHOUTcast connection was closed by the server", 256); okay = FALSE;
            }
        }
        if (okay && (encoder.failed || atomic_load(&output->overrun))) {
            okay = FALSE; g_strlcpy(error, encoder.starved ? "PCM stopped producing frames for five seconds" : "Native MP3 encoding failed", 256);
        }
        if (okay && !atomic_load(&output->stopping)) { okay = FALSE; g_strlcpy(error, "SHOUTcast upload ended", 256); }
    }
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL); g_object_unref(connection); g_object_unref(client);
    encoder_clear(&encoder); return okay;
}
static gpointer output_worker(gpointer data)
{
    RadioSAudioOutput *output = data;
    for (;;) {
        g_mutex_lock(&output->mutex);
        while (!output->requested && !output->closing) g_cond_wait(&output->condition, &output->mutex);
        if (output->closing && !output->requested) { g_mutex_unlock(&output->mutex); break; }
        output->requested = FALSE;
        gchar *target = g_strdup(output->target), *username = g_strdup(output->username), *password = g_strdup(output->password);
        guint bitrate = output->bitrate; RadioSAudioOutputKind kind = output->snapshot.kind;
        g_mutex_unlock(&output->mutex);
        gchar error[256] = "";
        gboolean shoutcast = output->server && output->protocol != RADIOS_STREAM_ICECAST;
        gboolean okay = kind == RADIOS_AUDIO_OUTPUT_RECORD ? record_run(output, target, error) :
            shoutcast ? shoutcast_run(output, password, bitrate, error) :
            stream_run(output, target, username, password, bitrate, error);
        atomic_store(&output->accepting, FALSE); atomic_store(&output->stopping, TRUE);
        while (atomic_load(&output->pushing)) g_thread_yield();
        if (atomic_load(&output->overrun)) { okay = FALSE; g_strlcpy(error, "MAIN PCM queue overrun; output stopped", sizeof error); }
        g_free(target); g_free(username);
        if (password) { memset(password, 0, strlen(password)); g_free(password); }
        g_mutex_lock(&output->mutex);
        output->snapshot.state = okay ? RADIOS_AUDIO_OUTPUT_STOPPED : RADIOS_AUDIO_OUTPUT_FAILED;
        g_strlcpy(output->snapshot.error, error, sizeof output->snapshot.error);
        g_cond_broadcast(&output->condition); g_mutex_unlock(&output->mutex);
    }
    return NULL;
}
static gpointer metadata_worker(gpointer data)
{
    RadioSAudioOutput *output = data;
    for (;;) {
        g_mutex_lock(&output->mutex);
        while (!output->metadata_requested && !output->closing) g_cond_wait(&output->condition, &output->mutex);
        if (output->closing) { g_mutex_unlock(&output->mutex); break; }
        output->metadata_requested = FALSE;
        gchar *target = g_strdup(output->target), *username = g_strdup(output->username), *password = g_strdup(output->password);
        gchar *title = g_strdup(output->snapshot.metadata_title), *host = g_strdup(output->host);
        guint64 generation = output->snapshot.generation, sequence = output->snapshot.metadata_sequence;
        gboolean shoutcast = output->server && output->protocol != RADIOS_STREAM_ICECAST;
        gboolean version2 = output->protocol == RADIOS_STREAM_SHOUTCAST2;
        guint port = output->port, stream_id = output->stream_id;
        g_mutex_unlock(&output->mutex);
        CURL *curl = curl_easy_init(); CURLcode result = CURLE_FAILED_INIT; long status = 0; GUri *uri = NULL;
        if (curl && shoutcast) {
            gchar *song = curl_easy_escape(curl, title, 0), *pass = curl_easy_escape(curl, password ? password : "", 0);
            gchar *sid = version2 ? g_strdup_printf("sid=%u&", stream_id) : g_strdup("");
            gchar *url = g_strdup_printf("http://%s:%u/admin.cgi?%smode=updinfo&pass=%s&song=%s", host, port, sid, pass, song);
            curl_easy_setopt(curl, CURLOPT_URL, url); curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http");
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response);
            /* SHOUTcast 1 only accepts admin requests from browser-like agents. */
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (RadioS)");
            result = curl_easy_perform(curl); curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            memset(url, 0, strlen(url)); g_free(url); g_free(sid); curl_free(song); curl_free(pass); curl_easy_cleanup(curl);
        } else if (curl && (uri = g_uri_parse(target, G_URI_FLAGS_NONE, NULL)) == NULL) {
            /* The mount URL was validated when the stream started; never dereference a failed parse. */
            result = CURLE_URL_MALFORMAT; curl_easy_cleanup(curl);
        } else if (curl) {
            gchar *mount = curl_easy_escape(curl, g_uri_get_path(uri), 0), *song = curl_easy_escape(curl, title, 0);
            gchar *query = g_strdup_printf("mode=updinfo&mount=%s&song=%s", mount, song);
            GUri *admin = g_uri_build(G_URI_FLAGS_ENCODED_QUERY, g_uri_get_scheme(uri), NULL, g_uri_get_host(uri),
                g_uri_get_port(uri), "/admin/metadata", query, NULL);
            gchar *url = g_uri_to_string(admin);
            curl_common(curl, url, username, password); curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
            result = curl_easy_perform(curl); curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            g_free(url); g_uri_unref(admin); g_free(query); curl_free(mount); curl_free(song); g_uri_unref(uri);
            curl_easy_cleanup(curl);
        }
        g_mutex_lock(&output->mutex);
        if (output->snapshot.generation == generation && output->snapshot.metadata_sequence == sequence) {
            gboolean okay = result == CURLE_OK && status >= 200 && status < 300;
            output->snapshot.metadata_state = okay ? RADIOS_AUDIO_METADATA_SUCCESS : RADIOS_AUDIO_METADATA_FAILED;
            output->snapshot.metadata_http_status = status;
            if (!okay) {
                if (result != CURLE_OK) g_snprintf(output->snapshot.metadata_error, sizeof output->snapshot.metadata_error,
                    "Metadata transport failed: %s", curl_easy_strerror(result));
                else g_snprintf(output->snapshot.metadata_error, sizeof output->snapshot.metadata_error,
                    "Metadata endpoint returned HTTP %ld", status);
            }
        }
        g_mutex_unlock(&output->mutex);
        g_free(target); g_free(username); g_free(title); g_free(host);
        if (password) { memset(password, 0, strlen(password)); g_free(password); }
    }
    return NULL;
}
RadioSAudioOutput *radios_audio_output_new_internal(RadioSAudioOutputKind kind)
{
    static gsize initialized = 0;
    if (g_once_init_enter(&initialized)) { curl_global_init(CURL_GLOBAL_DEFAULT); g_once_init_leave(&initialized, 1); }
    RadioSAudioOutput *output = g_new0(RadioSAudioOutput, 1); g_atomic_ref_count_init(&output->refs);
    g_mutex_init(&output->mutex); g_cond_init(&output->condition); output->kind = kind; output->snapshot.kind = kind;
    output->samples = g_new0(gfloat, QUEUE_FRAMES * 2);
    atomic_init(&output->read, 0); atomic_init(&output->write, 0); atomic_init(&output->received, 0);
    atomic_init(&output->processed, 0); atomic_init(&output->bytes, 0); atomic_init(&output->overrun, 0);
    atomic_init(&output->pushing, 0); atomic_init(&output->accepting, FALSE); atomic_init(&output->stopping, FALSE);
    atomic_init(&output->stop_time, 0);
    output->thread = g_thread_new("radios-output", output_worker, output);
    if (kind == RADIOS_AUDIO_OUTPUT_STREAM) output->metadata_thread = g_thread_new("radios-metadata", metadata_worker, output);
    return output;
}
RadioSAudioOutput *radios_audio_output_ref(RadioSAudioOutput *output)
{ g_return_val_if_fail(output, NULL); g_atomic_ref_count_inc(&output->refs); return output; }
void radios_audio_output_stop(RadioSAudioOutput *output)
{
    if (!output) return;
    g_mutex_lock(&output->mutex);
    atomic_store(&output->accepting, FALSE); atomic_store(&output->stopping, TRUE);
    gint64 expected = 0; atomic_compare_exchange_strong(&output->stop_time, &expected, g_get_monotonic_time());
    if (active_state(output->snapshot.state)) output->snapshot.state = RADIOS_AUDIO_OUTPUT_STOPPING;
    g_cond_broadcast(&output->condition); g_mutex_unlock(&output->mutex);
}
void radios_audio_output_shutdown_internal(RadioSAudioOutput *output)
{
    if (!output) return;
    radios_audio_output_stop(output);
    g_mutex_lock(&output->mutex); output->closing = TRUE; g_cond_broadcast(&output->condition);
    GThread *thread = output->thread, *metadata = output->metadata_thread;
    output->thread = output->metadata_thread = NULL; g_mutex_unlock(&output->mutex);
    if (thread) g_thread_join(thread);
    if (metadata) g_thread_join(metadata);
}
void radios_audio_output_unref(RadioSAudioOutput *output)
{
    if (!output || !g_atomic_ref_count_dec(&output->refs)) return;
    radios_audio_output_shutdown_internal(output); g_free(output->target); g_free(output->username);
    g_free(output->host); g_free(output->station); g_free(output->genre); g_free(output->description); g_free(output->website);
    if (output->password) { memset(output->password, 0, strlen(output->password)); g_free(output->password); }
    g_free(output->samples); g_cond_clear(&output->condition); g_mutex_clear(&output->mutex); g_free(output);
}
static gboolean start(RadioSAudioOutput *output, RadioSAudioOutputKind kind, const gchar *target,
    const gchar *username, const gchar *password, guint bitrate, const RadioSAudioStreamTarget *server, GError **error)
{
    if (!output || output->kind != kind) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Output kind does not match operation");
    g_mutex_lock(&output->mutex);
    if (output->closing || active_state(output->snapshot.state)) {
        gboolean closing = output->closing; g_mutex_unlock(&output->mutex);
        return fail(error, closing ? RADIOS_AUDIO_ERROR_CLOSED : RADIOS_AUDIO_ERROR_STATE,
            closing ? "Audio engine output has closed" : "Output is already active or stopping");
    }
    while (atomic_load(&output->pushing)) g_thread_yield();
    g_free(output->target); g_free(output->username);
    if (output->password) { memset(output->password, 0, strlen(output->password)); g_free(output->password); }
    output->target = g_strdup(target); output->username = g_strdup(username); output->password = g_strdup(password); output->bitrate = bitrate;
    output->server = server != NULL; output->protocol = server ? server->protocol : RADIOS_STREAM_ICECAST;
    output->public_listing = server && server->public_listing;
    output->port = server ? server->port : 0; output->stream_id = server ? server->stream_id : 0;
    g_free(output->host); g_free(output->station); g_free(output->genre); g_free(output->description); g_free(output->website);
    output->host = g_strdup(server ? server->host : ""); output->station = g_strdup(server && server->name ? server->name : "");
    output->genre = g_strdup(server && server->genre ? server->genre : "");
    output->description = g_strdup(server && server->description ? server->description : "");
    output->website = g_strdup(server && server->website ? server->website : "");
    guint64 generation = output->snapshot.generation + 1;
    output->snapshot = (RadioSAudioOutputSnapshot){.kind = kind, .state = RADIOS_AUDIO_OUTPUT_STARTING, .generation = generation};
    g_strlcpy(output->snapshot.target, target, sizeof output->snapshot.target); output->metadata_requested = FALSE;
    atomic_store(&output->read, 0); atomic_store(&output->write, 0); atomic_store(&output->received, 0);
    atomic_store(&output->processed, 0); atomic_store(&output->bytes, 0); atomic_store(&output->overrun, 0);
    atomic_store(&output->stop_time, 0); atomic_store(&output->stopping, FALSE); atomic_store(&output->accepting, TRUE);
    output->requested = TRUE; g_cond_broadcast(&output->condition); g_mutex_unlock(&output->mutex); return TRUE;
}
gboolean radios_audio_output_start_record(RadioSAudioOutput *output, const gchar *path, GError **error)
{
    if (!path || !g_path_is_absolute(path) || strlen(path) > 4096 || !g_utf8_validate(path, -1, NULL))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Recording requires an absolute UTF-8 path up to 4096 bytes");
    return start(output, RADIOS_AUDIO_OUTPUT_RECORD, path, NULL, NULL, 0, NULL, error);
}
gboolean radios_audio_output_start_stream(RadioSAudioOutput *output, const gchar *url,
    const gchar *username, const gchar *password, guint bitrate, GError **error)
{
    gboolean valid_bitrate = bitrate == 64 || bitrate == 96 || bitrate == 128 || bitrate == 160 ||
        bitrate == 192 || bitrate == 256 || bitrate == 320;
    if (!url || strlen(url) > 4096 || !g_utf8_validate(url, -1, NULL) || !username || !*username || strlen(username) > 256 ||
        !password || strlen(password) > 1024 || !valid_bitrate)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid stream URL, credentials or MP3 bitrate");
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    gboolean valid = uri && (!g_strcmp0(g_uri_get_scheme(uri), "http") || !g_strcmp0(g_uri_get_scheme(uri), "https")) &&
        g_uri_get_host(uri) && *g_uri_get_host(uri) && !g_uri_get_userinfo(uri) && !g_uri_get_query(uri) && !g_uri_get_fragment(uri) &&
        g_uri_get_path(uri) && g_uri_get_path(uri)[0] == '/' && g_uri_get_path(uri)[1];
    if (uri) g_uri_unref(uri);
    if (!valid) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Stream requires an HTTP(S) mount URL without embedded credentials or query");
    return start(output, RADIOS_AUDIO_OUTPUT_STREAM, url, username, password, bitrate, NULL, error);
}
static gboolean server_text(const gchar *value, gsize limit, gboolean required)
{
    if (!value) return !required;
    if ((required && !*value) || strlen(value) > limit || !g_utf8_validate(value, -1, NULL)) return FALSE;
    for (const guchar *c = (const guchar *)value; *c; ++c) if (*c < 32 || *c == 127) return FALSE;
    return TRUE;
}
gboolean radios_audio_output_start_server(RadioSAudioOutput *output, const RadioSAudioStreamTarget *target, GError **error)
{
    gboolean valid_bitrate = target && (target->bitrate_kbps == 64 || target->bitrate_kbps == 96 || target->bitrate_kbps == 128 ||
        target->bitrate_kbps == 160 || target->bitrate_kbps == 192 || target->bitrate_kbps == 256 || target->bitrate_kbps == 320);
    if (!target || !valid_bitrate || target->protocol > RADIOS_STREAM_SHOUTCAST2 || !server_text(target->host, 255, TRUE) ||
        strpbrk(target->host, " /\\?#@[]") || target->port < 1 || target->port > (target->protocol == RADIOS_STREAM_ICECAST ? 65535u : 65534u) ||
        !server_text(target->password, 1024, TRUE) || !server_text(target->name, 160, FALSE) || !server_text(target->genre, 160, FALSE) ||
        !server_text(target->description, 256, FALSE) || !server_text(target->website, 256, FALSE) ||
        (target->protocol == RADIOS_STREAM_SHOUTCAST2 && (target->stream_id < 1 || target->stream_id > 999)))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Yayın sunucusu adresi, portu, şifresi veya bit hızı geçersiz");
    if (target->protocol == RADIOS_STREAM_ICECAST && (!server_text(target->mount, 255, TRUE) || target->mount[0] != '/' || !target->mount[1] ||
        strpbrk(target->mount, " ?#\\") || !server_text(target->username, 128, TRUE) || strchr(target->username, ':')))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Icecast için '/' ile başlayan bağlama noktası ve kullanıcı adı gerekir");
    gboolean ipv6 = strchr(target->host, ':') != NULL;
    gchar *url = g_strdup_printf("http://%s%s%s:%u%s", ipv6 ? "[" : "", target->host, ipv6 ? "]" : "", target->port,
        target->protocol == RADIOS_STREAM_ICECAST ? target->mount : "/");
    gboolean started = start(output, RADIOS_AUDIO_OUTPUT_STREAM, url, target->protocol == RADIOS_STREAM_ICECAST ? target->username : "source",
        target->password, target->bitrate_kbps, target, error);
    g_free(url); return started;
}
void radios_audio_output_snapshot(RadioSAudioOutput *output, RadioSAudioOutputSnapshot *snapshot)
{
    g_return_if_fail(output && snapshot);
    g_mutex_lock(&output->mutex); *snapshot = output->snapshot;
    snapshot->frames_received = atomic_load(&output->received); snapshot->frames_processed = atomic_load(&output->processed);
    snapshot->bytes_written = atomic_load(&output->bytes); snapshot->overrun_frames = atomic_load(&output->overrun);
    guint64 write = atomic_load(&output->write), read = atomic_load(&output->read);
    snapshot->queued_frames = (guint)MIN(write >= read ? write - read : 0, QUEUE_FRAMES);
    g_mutex_unlock(&output->mutex);
}
gboolean radios_audio_output_metadata(RadioSAudioOutput *output, const gchar *title, GError **error)
{
    if (!output || output->kind != RADIOS_AUDIO_OUTPUT_STREAM || !title || !*title || strlen(title) > 1024 ||
        !g_utf8_validate(title, -1, NULL)) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Metadata requires a UTF-8 title up to 1024 bytes and a stream output");
    g_mutex_lock(&output->mutex);
    if (output->closing || (output->snapshot.state != RADIOS_AUDIO_OUTPUT_STARTING && output->snapshot.state != RADIOS_AUDIO_OUTPUT_ACTIVE)) {
        g_mutex_unlock(&output->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Stream must be active before sending metadata");
    }
    ++output->snapshot.metadata_sequence; output->snapshot.metadata_state = RADIOS_AUDIO_METADATA_PENDING;
    output->snapshot.metadata_http_status = 0; output->snapshot.metadata_error[0] = 0;
    g_strlcpy(output->snapshot.metadata_title, title, sizeof output->snapshot.metadata_title);
    output->metadata_requested = TRUE; g_cond_broadcast(&output->condition); g_mutex_unlock(&output->mutex); return TRUE;
}
