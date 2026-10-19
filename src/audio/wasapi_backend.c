/* Physical output backend of the realtime engine on Windows.
 *
 * Contract (audio_internal.h): three output buses MAIN/CUE/MONITOR consume
 * the engine's tagged 48 kHz rings, the talk microphone fills the capture
 * ring. Played evidence (played_tag, consumed_frames, meters) comes only
 * from the device clock (IAudioClock), never from the wall clock.
 *
 * Threads, all owned here and all in the COM multithreaded apartment:
 *   radios-audio-mgr   connection to Windows audio, hotplug, device scans,
 *                      Audiosrv watch and the 1/2/5 s reconnect backoff
 *   radios-audio-main/cue/mon   one event-driven shared-mode stream each,
 *                      MMCSS "Pro Audio"; device opening happens here, so no
 *                      engine thread ever waits for a device
 *   radios-audio-mic   the capture stream (microphone or output loopback)
 * The engine control thread calls the API below; every call only takes the
 * backend lock briefly, except flush/flush_buses (bounded 100 ms handshake). */
#include "audio_internal.h"
#include "wasapi_device.h"
#include "radios/output_resampler.h"
#include <math.h>
#include <stdarg.h>
#include <string.h>

#define FRAME_BYTES (2u * sizeof(gfloat))
#define BLOCK_FRAMES 4096u
#define PLAYBACK_TAG_CAPACITY 65536u
#define SPAN_COUNT 512u
#define REFILL_FRAMES 480u
#define REFILL_TIMEOUT_US (200 * G_TIME_SPAN_MILLISECOND)
#define HOTPLUG_DEBOUNCE_US (250 * G_TIME_SPAN_MILLISECOND)
#define ROUTE_RETRY_US (5 * G_TIME_SPAN_SECOND)
#define FLUSH_WAIT_US (100 * G_TIME_SPAN_MILLISECOND)
#define SERVICE_POLL_US G_TIME_SPAN_SECOND
#define STALL_US (2 * G_TIME_SPAN_SECOND)
#define STALL_WAIT_MS 250u
#define IDLE_WAIT_MS 1000u
#define REOPEN_LIMIT 3
#define REOPEN_WINDOW_US (2 * G_TIME_SPAN_SECOND)
#define MIC_RATE 48000u
#define MIC_BUFFER_FRAMES 9600u
#define MIC_POLL_MS 5u
#define MIC_PEAK_HOLD_US (100 * G_TIME_SPAN_MILLISECOND)
#define MIC_SLOT 3u

#define TEXT_START_FAILED "Ses arka ucu başlatılamadı. Uygulamayı yeniden başlatın."
#define TEXT_SERVICE_DOWN "Windows ses hizmeti çalışmıyor; hizmet açılınca ses kendiliğinden bağlanır."
#define TEXT_STALLED "Ses aygıtı yanıt vermiyor; yeniden açılıyor."
#define TEXT_RESAMPLER_OVERFLOW "Çıkış örnekleyici kuyruğu taştı; çıkış yeniden açılıyor."

typedef struct {
    guint64 start, count, first_tag, length;
    gfloat peak[2], rms[2];
} WriteSpan;
typedef enum { PHASE_IDLE, PHASE_OPENING, PHASE_READY, PHASE_FAILED } Phase;

/* Field ownership (every write under backend->lock):
 *   control thread: generation, device, rate, buffer_frames, flush_request
 *   bus thread:     everything from actual_rate on (read lock-free by it)
 *   both:           phase, ready, error */
typedef struct {
    struct RadioSAudioBackend *backend;
    guint index;
    GThread *thread;
    RadioSWasapiWaker *wake;
    guint64 generation;
    gchar device[RADIOS_AUDIO_DEVICE_NAME_SIZE];
    guint rate, buffer_frames;
    guint64 flush_request;
    Phase phase;
    gboolean ready;
    gchar error[256];
    guint actual_rate, fill_target;
    guint64 flush_ack;
    gboolean flush_kept;
    guint64 position, underruns, gap_frames;
    gboolean started, refilling;
    gint64 refill_since;
    /* Write accounting since the stream opened or was flushed: span starts
     * and `written` count stream-rate frames including silence padding. */
    guint64 written, last_tag, span_write;
    WriteSpan spans[SPAN_COUNT];
    guint64 playback_tags[PLAYBACK_TAG_CAPACITY];
} Bus;
typedef struct {
    GThread *thread;
    RadioSWasapiWaker *wake;
    guint64 generation;
    gchar source[RADIOS_AUDIO_DEVICE_NAME_SIZE];
    Phase phase;
    gboolean ready;
    gchar error[256];
    gfloat peak[2];
    gint64 last_packet;
} Mic;
struct RadioSAudioBackend {
    const RadioSAudioProviderOps *ops;
    RadioSAudioProvider *provider;
    RadioSAudioHotplugSink sink;
    GMutex lock;
    GCond cond;
    gboolean started, quit, connected, reapply, service_suspect, scan_requested;
    gchar error[256];
    RadioSAudioPreferences preferences;
    guint output_sample_rate, output_buffer_frames;
    gint64 next_retry, route_retry;
    guint retries;
    Bus buses[3];
    Mic mic;
    RadioSAudioRing *outputs[3], *capture;
    /* The published inventory has its own lock: copying 132 KB never holds
     * up the bus threads, which only take `lock`. Order: lock, then devices. */
    GMutex devices;
    guint output_count, input_count;
    guint64 device_scans;
    RadioSAudioDevice outputs_info[64], inputs_info[64];
    RadioSAudioDevice staged_outputs[64], staged_inputs[64];   /* manager thread only */
    _Atomic gint64 refresh_due;     /* hotplug debounce deadline; 0 = none */
    _Atomic guint scan_generation;  /* bumped to cancel an in-flight scan */
    GThread *manager;
    RadioSWasapiWaker *manager_wake;
};

static void text_set(gchar destination[256], const gchar *text)
{ radios_wasapi_copy_text(destination, 256, text); }
static gboolean phase_good(Phase phase) { return phase == PHASE_OPENING || phase == PHASE_READY; }
static void meter(const gfloat *samples, guint frames, gfloat peak[2], gfloat rms[2])
{
    gdouble squares[2] = {0, 0}; peak[0] = peak[1] = 0;
    for (guint i = 0; i < frames; ++i) for (guint c = 0; c < 2; ++c) {
        gfloat sample = samples[i * 2 + c]; peak[c] = MAX(peak[c], fabsf(sample)); squares[c] += (gdouble)sample * sample;
    }
    for (guint c = 0; c < 2; ++c) rms[c] = frames ? (gfloat)sqrt(squares[c] / frames) : 0;
}
/* Only the ring's consumer (the bus thread) or a quiescent consumer resets it. */
static void ring_reset(RadioSAudioRing *ring)
{
    guint64 write = atomic_load_explicit(&ring->write, memory_order_acquire);
    atomic_store_explicit(&ring->read, write, memory_order_release);
}
/* Unlike a last-tag-only read, this bounded copy preserves proof through
 * ring gaps, underruns and the resampler's filter queue. */
static guint read_tagged(RadioSAudioRing *ring, gfloat *samples, guint64 *tags, guint frames)
{
    guint64 read = atomic_load_explicit(&ring->read, memory_order_relaxed);
    guint64 write = atomic_load_explicit(&ring->write, memory_order_acquire);
    guint count = (guint)MIN((guint64)frames, write - read);
    for (guint i = 0; i < count; ++i) {
        guint slot = (guint)((read + i) % ring->capacity);
        samples[i * 2] = ring->samples[slot * 2]; samples[i * 2 + 1] = ring->samples[slot * 2 + 1];
        tags[i] = ring->tags ? ring->tags[slot] : 0;
    }
    atomic_store_explicit(&ring->read, read + count, memory_order_release);
    return count;
}
static void wake_all(RadioSAudioBackend *backend)
{
    for (guint i = 0; i < 3; ++i) radios_wasapi_waker_signal(backend->buses[i].wake);
    radios_wasapi_waker_signal(backend->mic.wake); radios_wasapi_waker_signal(backend->manager_wake);
}

/* ------------------------------------------------------------------ process-wide queries */

static GMutex max_rate_lock;
static GCond max_rate_cond;
static guint max_rate_value;
static gboolean max_rate_known, max_rate_probing;
static void max_rate_publish(guint rate)
{
    g_mutex_lock(&max_rate_lock); max_rate_value = rate; max_rate_known = TRUE;
    g_cond_broadcast(&max_rate_cond); g_mutex_unlock(&max_rate_lock);
}
static gpointer max_rate_probe(gpointer unused)
{
    (void)unused; gchar error[RADIOS_WASAPI_ERROR_SIZE] = ""; guint best = 0;
    if (radios_wasapi_thread_enter(error)) {
        RadioSWasapiEnumerator *enumerator = radios_wasapi_enumerator_new(error);
        RadioSWasapiEndpoint *endpoints = g_new0(RadioSWasapiEndpoint, 64); guint count = 0;
        if (enumerator && radios_wasapi_enumerator_list(enumerator, RADIOS_WASAPI_FLOW_RENDER, endpoints, 64, &count,
                NULL, NULL, NULL, error) == RADIOS_WASAPI_OK)
            for (guint i = 0; i < count; ++i) best = MAX(best, radios_wasapi_endpoint_max_rate(enumerator, endpoints[i].id, NULL));
        g_free(endpoints); radios_wasapi_enumerator_free(enumerator); radios_wasapi_thread_leave();
    }
    g_mutex_lock(&max_rate_lock);
    if (!max_rate_known) { max_rate_value = best; max_rate_known = TRUE; }
    max_rate_probing = FALSE; g_cond_broadcast(&max_rate_cond); g_mutex_unlock(&max_rate_lock);
    return NULL;
}
guint radios_audio_backend_max_sample_rate(void)
{
    g_mutex_lock(&max_rate_lock);
    if (!max_rate_known && !max_rate_probing) {
        GThread *thread = g_thread_try_new("radios-audio-probe", max_rate_probe, NULL, NULL);
        if (thread) { max_rate_probing = TRUE; g_thread_unref(thread); }
    }
    gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (!max_rate_known && max_rate_probing && g_cond_wait_until(&max_rate_cond, &max_rate_lock, deadline));
    guint rate = max_rate_value;
    g_mutex_unlock(&max_rate_lock); return rate;
}
const gchar *radios_audio_backend_display_name(void) { return "Windows ses (WASAPI)"; }

/* ------------------------------------------------------------------ WASAPI provider */

/* One volume-mixer session per bus, parity with the Pulse stream names. */
static const guint8 SESSION_GUIDS[4][16] = {
    {0x52, 0x1a, 0x8c, 0x6f, 0x7e, 0x3b, 0x61, 0x4d, 0x9a, 0x2f, 0x0c, 0x5e, 0x7b, 0x1d, 0x9a, 0x01},
    {0x52, 0x1a, 0x8c, 0x6f, 0x7e, 0x3b, 0x61, 0x4d, 0x9a, 0x2f, 0x0c, 0x5e, 0x7b, 0x1d, 0x9a, 0x02},
    {0x52, 0x1a, 0x8c, 0x6f, 0x7e, 0x3b, 0x61, 0x4d, 0x9a, 0x2f, 0x0c, 0x5e, 0x7b, 0x1d, 0x9a, 0x03},
    {0x52, 0x1a, 0x8c, 0x6f, 0x7e, 0x3b, 0x61, 0x4d, 0x9a, 0x2f, 0x0c, 0x5e, 0x7b, 0x1d, 0x9a, 0x04}
};
static const gchar *const SESSION_NAMES[4] = {"RadioS MAIN", "RadioS CUE", "RadioS MONITOR", "RadioS mikrofon"};

typedef struct {
    RadioSAudioHotplugSink sink;
    RadioSWasapiEnumerator *enumerator;
    RadioSWasapiWatch *watch;
    GHashTable *max_rates;          /* render id -> highest rate; manager thread */
    gchar render_ids[64][RADIOS_AUDIO_DEVICE_NAME_SIZE];
    guint render_count;
} WasapiProvider;
typedef struct { gboolean com; gpointer priority; } WasapiThread;

static RadioSAudioIoStatus wasapi_status(RadioSWasapiStatus status)
{
    switch (status) {
    case RADIOS_WASAPI_OK: return RADIOS_AUDIO_IO_OK;
    case RADIOS_WASAPI_LOST: return RADIOS_AUDIO_IO_LOST;
    case RADIOS_WASAPI_REOPEN: return RADIOS_AUDIO_IO_REOPEN;
    case RADIOS_WASAPI_SERVICE_DOWN: return RADIOS_AUDIO_IO_SERVICE_DOWN;
    default: return RADIOS_AUDIO_IO_FAILED;
    }
}
static RadioSAudioProvider *wasapi_create(const RadioSAudioHotplugSink *sink, gpointer config)
{
    (void)config; WasapiProvider *provider = g_new0(WasapiProvider, 1);
    provider->sink = *sink; provider->max_rates = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    return (RadioSAudioProvider *)provider;
}
static void wasapi_destroy(RadioSAudioProvider *value)
{
    WasapiProvider *provider = (WasapiProvider *)value;
    if (!provider) return;
    g_hash_table_unref(provider->max_rates); g_free(provider);
}
static gpointer wasapi_thread_enter(gboolean realtime)
{
    WasapiThread *thread = g_new0(WasapiThread, 1); gchar error[RADIOS_WASAPI_ERROR_SIZE] = "";
    thread->com = radios_wasapi_thread_enter(error);
    if (!thread->com) g_warning("%s", error);
    if (realtime) thread->priority = radios_wasapi_audio_priority_begin();
    return thread;
}
static void wasapi_thread_leave(gpointer cookie)
{
    WasapiThread *thread = cookie;
    if (!thread) return;
    if (thread->priority) radios_wasapi_audio_priority_end(thread->priority);
    if (thread->com) radios_wasapi_thread_leave();
    g_free(thread);
}
static void wasapi_changed(const RadioSWasapiChange *change, gpointer data)
{
    WasapiProvider *provider = data;
    RadioSAudioHotplugKind kind = !change->relevant ? RADIOS_AUDIO_HOTPLUG_IGNORED :
        change->kind == RADIOS_WASAPI_CHANGE_DEFAULT ? RADIOS_AUDIO_HOTPLUG_DEFAULT : RADIOS_AUDIO_HOTPLUG_DEVICES;
    provider->sink.notify(provider->sink.core, kind);
}
static void wasapi_disconnect(RadioSAudioProvider *value)
{
    WasapiProvider *provider = (WasapiProvider *)value;
    radios_wasapi_watch_free(provider->watch); provider->watch = NULL;
    radios_wasapi_enumerator_free(provider->enumerator); provider->enumerator = NULL;
}
static RadioSAudioIoStatus wasapi_connect(RadioSAudioProvider *value, gchar error[256])
{
    WasapiProvider *provider = (WasapiProvider *)value;
    wasapi_disconnect(value);
    /* A fresh enumerator after an Audiosrv restart: the analog of a new
     * PulseAudio context. */
    if (!(provider->enumerator = radios_wasapi_enumerator_new(error))) return RADIOS_AUDIO_IO_FAILED;
    if (!(provider->watch = radios_wasapi_watch_new(provider->enumerator, error))) { wasapi_disconnect(value); return RADIOS_AUDIO_IO_FAILED; }
    (void)radios_wasapi_watch_add(provider->watch, wasapi_changed, provider);
    return RADIOS_AUDIO_IO_OK;
}
static gboolean wasapi_service_running(RadioSAudioProvider *value) { (void)value; return radios_wasapi_service_running(); }
static gint endpoint_order(gconstpointer a, gconstpointer b)
{
    const RadioSWasapiEndpoint *x = a, *y = b;
    gint order = g_utf8_collate(x->name, y->name);
    return order ? order : strcmp(x->id, y->id);
}
/* Stable lists for the UI hash: sorted by friendly name then id; equal
 * names become "Hoparlör (USB)", "Hoparlör (USB) (2)" in id order. */
static void endpoint_label(const RadioSWasapiEndpoint *endpoints, guint index, gchar label[RADIOS_AUDIO_DEVICE_NAME_SIZE])
{
    guint same = 1;
    for (guint i = 0; i < index; ++i) if (!strcmp(endpoints[i].name, endpoints[index].name)) ++same;
    if (same == 1) { radios_wasapi_copy_text(label, RADIOS_AUDIO_DEVICE_NAME_SIZE, endpoints[index].name); return; }
    gchar suffix[16]; g_snprintf(suffix, sizeof suffix, " (%u)", same);
    gchar name[RADIOS_AUDIO_DEVICE_NAME_SIZE];
    radios_wasapi_copy_text(name, sizeof name - strlen(suffix), endpoints[index].name);
    g_snprintf(label, RADIOS_AUDIO_DEVICE_NAME_SIZE, "%s%s", name, suffix);
}
static RadioSAudioIoStatus wasapi_scan(RadioSAudioProvider *value, RadioSAudioDevice outputs[64], guint *output_count,
    RadioSAudioDevice inputs[64], guint *input_count, gboolean (*cancelled)(gpointer data), gpointer data)
{
    WasapiProvider *provider = (WasapiProvider *)value;
    *output_count = *input_count = 0;
    RadioSWasapiEndpoint *render = g_new0(RadioSWasapiEndpoint, 64), *capture = g_new0(RadioSWasapiEndpoint, 64);
    guint renders = 0, captures = 0; gboolean stopped = FALSE; gchar error[RADIOS_WASAPI_ERROR_SIZE] = "";
    RadioSWasapiStatus status = radios_wasapi_enumerator_list(provider->enumerator, RADIOS_WASAPI_FLOW_RENDER, render, 64, &renders,
        cancelled, data, &stopped, error);
    if (status == RADIOS_WASAPI_OK && !stopped)
        status = radios_wasapi_enumerator_list(provider->enumerator, RADIOS_WASAPI_FLOW_CAPTURE, capture, 64, &captures,
            cancelled, data, &stopped, error);
    if (status == RADIOS_WASAPI_OK && !stopped) {
        qsort(render, renders, sizeof *render, endpoint_order); qsort(capture, captures, sizeof *capture, endpoint_order);
        for (guint i = 0; i < renders; ++i) {
            RadioSAudioDevice *device = &outputs[(*output_count)++];
            g_strlcpy(device->name, render[i].id, sizeof device->name); endpoint_label(render, i, device->description);
            device->channels = render[i].channels ? render[i].channels : 2; device->monitor = FALSE;
        }
        for (guint i = 0; i < captures && *input_count < 64; ++i) {
            RadioSAudioDevice *device = &inputs[(*input_count)++];
            g_strlcpy(device->name, capture[i].id, sizeof device->name); endpoint_label(capture, i, device->description);
            device->channels = capture[i].channels ? capture[i].channels : 2; device->monitor = FALSE;
        }
        /* Every output can be listened back ("monitor" of the output). */
        for (guint i = 0; i < renders && *input_count < 64; ++i) {
            if (strlen(render[i].id) + strlen(RADIOS_WASAPI_MONITOR_SUFFIX) >= RADIOS_AUDIO_DEVICE_NAME_SIZE) continue;
            RadioSAudioDevice *device = &inputs[(*input_count)++];
            g_snprintf(device->name, sizeof device->name, "%s%s", render[i].id, RADIOS_WASAPI_MONITOR_SUFFIX);
            endpoint_label(render, i, device->description);
            device->channels = render[i].channels ? render[i].channels : 2; device->monitor = TRUE;
        }
        provider->render_count = renders;
        for (guint i = 0; i < renders; ++i) g_strlcpy(provider->render_ids[i], render[i].id, sizeof provider->render_ids[i]);
    }
    g_free(render); g_free(capture);
    if (status == RADIOS_WASAPI_OK && stopped) *output_count = *input_count = 0;
    return wasapi_status(status);
}
static guint wasapi_max_rate(RadioSAudioProvider *value)
{
    WasapiProvider *provider = (WasapiProvider *)value; guint best = 0;
    for (guint i = 0; i < provider->render_count; ++i) {
        gpointer cached = NULL;
        if (!g_hash_table_lookup_extended(provider->max_rates, provider->render_ids[i], NULL, &cached)) {
            gboolean in_use = FALSE;
            guint rate = radios_wasapi_endpoint_max_rate(provider->enumerator, provider->render_ids[i], &in_use);
            /* An endpoint held exclusively by another program is probed again later. */
            if (!in_use) g_hash_table_insert(provider->max_rates, g_strdup(provider->render_ids[i]), GUINT_TO_POINTER(rate));
            cached = GUINT_TO_POINTER(rate);
        }
        best = MAX(best, GPOINTER_TO_UINT(cached));
    }
    return best;
}
static RadioSAudioIoStatus wasapi_open(RadioSAudioProvider *value, gboolean capture, const gchar *device, guint rate,
    guint buffer_frames, guint slot, RadioSAudioProviderStream **stream, RadioSAudioStreamInfo *info, gchar error[256])
{
    (void)value;
    RadioSWasapiStreamSpec spec; memset(&spec, 0, sizeof spec);
    spec.kind = capture ? RADIOS_WASAPI_STREAM_CAPTURE : RADIOS_WASAPI_STREAM_RENDER;
    spec.endpoint_id = device; spec.rate = rate; spec.buffer_frames = buffer_frames;
    spec.session_guid = SESSION_GUIDS[MIN(slot, 3u)]; spec.session_name = SESSION_NAMES[MIN(slot, 3u)];
    RadioSWasapiStream *opened = NULL; RadioSWasapiStreamInfo opened_info;
    RadioSWasapiStatus status = radios_wasapi_stream_open(NULL, &spec, &opened, &opened_info, error);
    *stream = (RadioSAudioProviderStream *)opened;
    if (status == RADIOS_WASAPI_OK) {
        info->rate = opened_info.rate; info->buffer_frames = opened_info.buffer_frames; info->period_frames = opened_info.period_frames;
    }
    return wasapi_status(status);
}
static gboolean wasapi_wait(RadioSAudioProviderStream *stream, gpointer wake, guint timeout_ms)
{ return radios_wasapi_wait((RadioSWasapiStream *)stream, wake, timeout_ms) == RADIOS_WASAPI_WAIT_STREAM; }
static RadioSAudioIoStatus wasapi_padding(RadioSAudioProviderStream *stream, guint *queued)
{ return wasapi_status(radios_wasapi_stream_padding((RadioSWasapiStream *)stream, queued)); }
static RadioSAudioIoStatus wasapi_write(RadioSAudioProviderStream *stream, const gfloat *stereo, guint frames)
{ return wasapi_status(radios_wasapi_stream_write((RadioSWasapiStream *)stream, stereo, frames)); }
static RadioSAudioIoStatus wasapi_read(RadioSAudioProviderStream *stream, gfloat *stereo, guint capacity, guint *frames, gboolean *silent)
{
    guint flags = 0;
    RadioSWasapiStatus status = radios_wasapi_stream_read((RadioSWasapiStream *)stream, stereo, capacity, frames, &flags);
    *silent = (flags & RADIOS_WASAPI_PACKET_SILENT) != 0; return wasapi_status(status);
}
static RadioSAudioIoStatus wasapi_position(RadioSAudioProviderStream *stream, guint64 *frames)
{ return wasapi_status(radios_wasapi_stream_position((RadioSWasapiStream *)stream, frames)); }
static RadioSAudioIoStatus wasapi_start(RadioSAudioProviderStream *stream)
{ return wasapi_status(radios_wasapi_stream_start((RadioSWasapiStream *)stream)); }
static RadioSAudioIoStatus wasapi_stop(RadioSAudioProviderStream *stream)
{ return wasapi_status(radios_wasapi_stream_stop((RadioSWasapiStream *)stream)); }
static RadioSAudioIoStatus wasapi_reset(RadioSAudioProviderStream *stream)
{ return wasapi_status(radios_wasapi_stream_reset((RadioSWasapiStream *)stream)); }
static guint64 wasapi_underruns(RadioSAudioProviderStream *stream)
{ RadioSWasapiStreamStats stats; radios_wasapi_stream_stats((RadioSWasapiStream *)stream, &stats); return stats.underruns; }
static const gchar *wasapi_last_error(RadioSAudioProviderStream *stream)
{ return radios_wasapi_stream_error((RadioSWasapiStream *)stream); }
static void wasapi_close(RadioSAudioProviderStream *stream) { radios_wasapi_stream_close((RadioSWasapiStream *)stream); }

static const RadioSAudioProviderOps wasapi_ops = {
    "Windows ses (WASAPI)",
    wasapi_create, wasapi_destroy, wasapi_thread_enter, wasapi_thread_leave,
    wasapi_connect, wasapi_disconnect, wasapi_service_running, wasapi_scan, wasapi_max_rate,
    wasapi_open, wasapi_wait, wasapi_padding, wasapi_write, wasapi_read, wasapi_position,
    wasapi_start, wasapi_stop, wasapi_reset, wasapi_underruns, wasapi_last_error, wasapi_close
};

/* ------------------------------------------------------------------ bus threads */

typedef struct {
    guint64 generation;
    gchar device[RADIOS_AUDIO_DEVICE_NAME_SIZE];
    guint rate, buffer_frames;
    guint64 flush_request;
    gboolean connected, quit;
} BusCommand;
typedef struct {
    RadioSAudioProviderStream *stream;
    RadioSAudioStreamInfo info;
    RadioSOutputResampler *resampler;
    guint64 generation;
    guint low_water;
    gint64 last_event;
    gint reopen_attempts;
    gint64 reopen_window;
    gchar error[256];
    gfloat samples[BLOCK_FRAMES * 2], source_samples[BLOCK_FRAMES * 2];
    guint64 source_tags[BLOCK_FRAMES], output_tags[BLOCK_FRAMES], block_tags[BLOCK_FRAMES];
} BusLocal;

static void bus_command(Bus *bus, BusCommand *command)
{
    RadioSAudioBackend *backend = bus->backend;
    g_mutex_lock(&backend->lock);
    command->generation = bus->generation; g_strlcpy(command->device, bus->device, sizeof command->device);
    command->rate = bus->rate; command->buffer_frames = bus->buffer_frames; command->flush_request = bus->flush_request;
    command->connected = backend->connected; command->quit = backend->quit;
    g_mutex_unlock(&backend->lock);
}
/* Caller holds the lock. O(1): playback_tags entries are only read for
 * frames below `written`, which this generation writes before they count. */
static void bus_accounting_reset_locked(Bus *bus)
{
    bus->written = bus->last_tag = bus->span_write = 0; bus->position = 0;
}
static void bus_close_stream(Bus *bus, BusLocal *local)
{
    RadioSAudioBackend *backend = bus->backend;
    if (local->stream) backend->ops->close(local->stream);
    local->stream = NULL;
    radios_output_resampler_free(local->resampler); local->resampler = NULL;
    g_mutex_lock(&backend->lock); bus->started = bus->refilling = FALSE; g_mutex_unlock(&backend->lock);
}
static void bus_publish_failure(Bus *bus, BusLocal *local, RadioSAudioIoStatus status, const gchar *text)
{
    RadioSAudioBackend *backend = bus->backend;
    g_mutex_lock(&backend->lock);
    if (bus->generation == local->generation) {
        bus->phase = PHASE_FAILED; bus->ready = FALSE;
        text_set(bus->error, text && *text ? text : "Ses aygıtı açılamadı.");
    }
    if (status == RADIOS_AUDIO_IO_SERVICE_DOWN) backend->service_suspect = TRUE;
    g_mutex_unlock(&backend->lock);
    if (status == RADIOS_AUDIO_IO_SERVICE_DOWN) radios_wasapi_waker_signal(backend->manager_wake);
}
static void bus_flush_answer(Bus *bus, guint64 request, gboolean kept)
{
    RadioSAudioBackend *backend = bus->backend;
    g_mutex_lock(&backend->lock);
    bus->flush_kept = kept; bus->flush_ack = request; g_cond_broadcast(&backend->cond);
    g_mutex_unlock(&backend->lock);
}
/* Up to `frames` stream frames from the ring through the resampler. It never
 * invents source audio: when the ring runs dry it stops early (*filled
 * frames produced) and the low-water rule decides whether the device needs
 * silence. Zero-filling the source here would put a gap and a phase jump
 * into the middle of a song while the device still had audio queued. */
static gboolean resampled_audio(Bus *bus, BusLocal *local, guint frames, guint *filled)
{
    RadioSAudioRing *ring = bus->backend->outputs[bus->index]; guint rate = local->info.rate;
    *filled = 0;
    while (*filled < frames) {
        guint consumed = 0, produced = 0; GError *error = NULL;
        if (!radios_output_resampler_convert(local->resampler, NULL, NULL, 0, local->samples + *filled * 2,
                local->output_tags + *filled, frames - *filled, &consumed, &produced, &error)) {
            text_set(local->error, error->message); g_clear_error(&error); return FALSE;
        }
        *filled += produced;
        if (*filled == frames) break;
        guint needed = MIN(((frames - *filled) * 48000 + rate - 1) / rate + 32, BLOCK_FRAMES);
        guint read = read_tagged(ring, local->source_samples, local->source_tags, needed);
        if (!read) break;
        if (!radios_output_resampler_convert(local->resampler, local->source_samples, local->source_tags, read,
                local->samples + *filled * 2, local->output_tags + *filled, frames - *filled, &consumed, &produced, &error)) {
            text_set(local->error, error->message); g_clear_error(&error); return FALSE;
        }
        if (consumed != read) { text_set(local->error, TEXT_RESAMPLER_OVERFLOW); return FALSE; }
        *filled += produced;
    }
    return TRUE;
}
/* Appends `frames` written frames to the accounting: `real` of them came
 * from the ring (48 kHz path), the rest is silence padding. */
static void bus_account(Bus *bus, BusLocal *local, guint frames, guint real, guint64 last_tag, gboolean silence)
{
    RadioSAudioBackend *backend = bus->backend; gfloat peak[2] = {0, 0}, rms[2] = {0, 0};
    gboolean resampled = local->info.rate != 48000;
    guint64 running = bus->last_tag;
    if (resampled)
        for (guint i = 0; i < frames; ++i) {
            if (!silence && local->output_tags[i]) running = local->output_tags[i];
            local->block_tags[i] = running;
        }
    if (!silence) meter(local->samples, frames, peak, rms);
    g_mutex_lock(&backend->lock);
    if (resampled)
        for (guint i = 0; i < frames; ++i) bus->playback_tags[(bus->written + i) % PLAYBACK_TAG_CAPACITY] = local->block_tags[i];
    WriteSpan *span = &bus->spans[bus->span_write++ % SPAN_COUNT];
    span->start = bus->written; span->count = real; span->length = frames;
    span->first_tag = !resampled && last_tag ? last_tag - real + 1 : 0;
    memcpy(span->peak, peak, sizeof peak); memcpy(span->rms, rms, sizeof rms);
    if (resampled) bus->last_tag = running;
    else if (last_tag) bus->last_tag = last_tag;
    bus->written += frames;
    g_mutex_unlock(&backend->lock);
}
/* Writes up to `frames` frames of real ring audio; *written may be less when
 * the ring runs dry (never padded here). */
static RadioSAudioIoStatus bus_write_audio(Bus *bus, BusLocal *local, guint frames, guint *written)
{
    RadioSAudioBackend *backend = bus->backend; RadioSAudioRing *ring = backend->outputs[bus->index];
    guint64 last_tag = 0;
    *written = 0;
    if (local->info.rate == 48000) frames = radios_audio_ring_read(ring, local->samples, frames, &last_tag);
    else if (!resampled_audio(bus, local, frames, &frames)) return RADIOS_AUDIO_IO_FAILED;
    if (!frames) return RADIOS_AUDIO_IO_OK;
    RadioSAudioIoStatus status = backend->ops->write(local->stream, local->samples, frames);
    if (status != RADIOS_AUDIO_IO_OK) return status;
    bus_account(bus, local, frames, frames, last_tag, FALSE);
    *written = frames; return RADIOS_AUDIO_IO_OK;
}
static RadioSAudioIoStatus bus_write_silence(Bus *bus, BusLocal *local, guint frames)
{
    RadioSAudioIoStatus status = bus->backend->ops->write(local->stream, NULL, frames);
    if (status != RADIOS_AUDIO_IO_OK) return status;
    bus_account(bus, local, frames, 0, 0, TRUE);
    /* Silence queued while the stream plays is an audible gap (the engine
     * was late); the start-up fill before the first real audio is not. */
    if (bus->started) { g_mutex_lock(&bus->backend->lock); bus->gap_frames += frames; g_mutex_unlock(&bus->backend->lock); }
    return status;
}
static void bus_set_flag(Bus *bus, gboolean *flag, gboolean value)
{ g_mutex_lock(&bus->backend->lock); *flag = value; g_mutex_unlock(&bus->backend->lock); }
/* The write_audio() of the Pulse backend: keeps fill_target frames queued
 * from the ring. Silence is added only below the low-water mark (keeps the
 * device from starving and the accounting exact) and never while refilling
 * after a flush, so no silence is queued ahead of fresh audio. */
static RadioSAudioIoStatus bus_render(Bus *bus, BusLocal *local)
{
    RadioSAudioBackend *backend = bus->backend; RadioSAudioRing *ring = backend->outputs[bus->index];
    guint rate = local->info.rate, queued = 0;
    RadioSAudioIoStatus status = backend->ops->padding(local->stream, &queued);
    if (status != RADIOS_AUDIO_IO_OK) return status;
    if (bus->refilling && (bus->written >= REFILL_FRAMES || g_get_monotonic_time() - bus->refill_since > REFILL_TIMEOUT_US))
        bus_set_flag(bus, &bus->refilling, FALSE);
    guint space = local->info.buffer_frames > queued ? local->info.buffer_frames - queued : 0;
    guint want = MIN(bus->fill_target > queued ? bus->fill_target - queued : 0, space), wrote = 0;
    while (want) {
        guint64 input = atomic_load_explicit(&ring->write, memory_order_acquire) - atomic_load_explicit(&ring->read, memory_order_relaxed);
        guint64 fresh = rate == 48000 ? input : input > 32 ? (input - 32) * rate / 48000 : 0;
        if (!fresh) break;
        guint frames = (guint)MIN((guint64)MIN(want, BLOCK_FRAMES), fresh), done = 0;
        if ((status = bus_write_audio(bus, local, frames, &done)) != RADIOS_AUDIO_IO_OK) return status;
        wrote += done; want -= done;
        if (bus->refilling && bus->written >= REFILL_FRAMES) bus_set_flag(bus, &bus->refilling, FALSE);
        if (done < frames) break;   /* the ring ran dry */
    }
    if (!bus->refilling && queued + wrote < local->low_water) {
        guint pad = MIN(local->low_water - queued - wrote, space - wrote);
        if (pad && (status = bus_write_silence(bus, local, pad)) != RADIOS_AUDIO_IO_OK) return status;
    }
    if (!bus->started && !bus->refilling) {
        if ((status = backend->ops->start(local->stream)) != RADIOS_AUDIO_IO_OK) return status;
        bus_set_flag(bus, &bus->started, TRUE); local->last_event = g_get_monotonic_time();
    }
    guint64 position = 0;
    if (bus->started && (status = backend->ops->position(local->stream, &position)) != RADIOS_AUDIO_IO_OK) return status;
    guint64 underruns = backend->ops->underruns ? backend->ops->underruns(local->stream) : 0;
    g_mutex_lock(&backend->lock); bus->position = position; bus->underruns = underruns; g_mutex_unlock(&backend->lock);
    return RADIOS_AUDIO_IO_OK;
}
static void bus_failure(Bus *bus, BusLocal *local, RadioSAudioIoStatus status, const gchar *text);
/* Opens the stream of the current generation; publishes READY or FAILED. */
static void bus_open(Bus *bus, BusLocal *local, const BusCommand *command)
{
    RadioSAudioBackend *backend = bus->backend;
    gchar error[256] = ""; RadioSAudioProviderStream *stream = NULL; RadioSAudioStreamInfo info; memset(&info, 0, sizeof info);
    RadioSAudioIoStatus status = backend->ops->open(backend->provider, FALSE, command->device, command->rate,
        command->buffer_frames, bus->index, &stream, &info, error);
    g_mutex_lock(&backend->lock);
    gboolean superseded = bus->generation != local->generation || backend->quit;
    g_mutex_unlock(&backend->lock);
    if (superseded) { if (stream) backend->ops->close(stream); return; }
    if (status != RADIOS_AUDIO_IO_OK || !stream || !info.rate || !info.buffer_frames) {
        if (stream) backend->ops->close(stream);
        if (status == RADIOS_AUDIO_IO_OK) { status = RADIOS_AUDIO_IO_FAILED; text_set(error, "Çıkış akışı açılamadı. Aygıtı yeniden seçin."); }
        bus_publish_failure(bus, local, status, error); return;
    }
    RadioSOutputResampler *resampler = NULL;
    if (info.rate != 48000) {
        GError *failure = NULL;
        if (!(resampler = radios_output_resampler_new(info.rate, &failure))) {
            backend->ops->close(stream); bus_publish_failure(bus, local, RADIOS_AUDIO_IO_FAILED, failure->message);
            g_clear_error(&failure); return;
        }
    }
    local->stream = stream; local->info = info; local->resampler = resampler; local->error[0] = '\0';
    guint period = MAX(1u, info.period_frames);
    guint target = MIN(MAX(command->buffer_frames, 2 * period), info.buffer_frames);
    local->low_water = MIN(target, 2 * period);
    g_mutex_lock(&backend->lock);
    bus_accounting_reset_locked(bus);
    bus->actual_rate = info.rate; bus->fill_target = target; bus->refilling = bus->started = FALSE;
    bus->underruns = bus->gap_frames = 0;
    guint64 request = bus->flush_request;
    g_mutex_unlock(&backend->lock);
    /* A flush posted while the device was opening drops the stale ring. */
    if (request != bus->flush_ack) { ring_reset(backend->outputs[bus->index]); bus_flush_answer(bus, request, TRUE); }
    status = bus_render(bus, local);   /* initial fill, then start */
    if (status != RADIOS_AUDIO_IO_OK) { bus_failure(bus, local, status, NULL); return; }
    g_mutex_lock(&backend->lock);
    if (bus->generation == local->generation) { bus->phase = PHASE_READY; bus->ready = TRUE; bus->error[0] = '\0'; }
    g_mutex_unlock(&backend->lock);
}
static void bus_failure(Bus *bus, BusLocal *local, RadioSAudioIoStatus status, const gchar *text)
{
    RadioSAudioBackend *backend = bus->backend; gchar message[256] = "";
    if (text) text_set(message, text);
    else if (local->error[0]) text_set(message, local->error);
    else if (local->stream) text_set(message, backend->ops->last_error(local->stream));
    if (status == RADIOS_AUDIO_IO_REOPEN) {
        gint64 now = g_get_monotonic_time();
        if (now - local->reopen_window > REOPEN_WINDOW_US) { local->reopen_window = now; local->reopen_attempts = 0; }
        if (local->reopen_attempts++ < REOPEN_LIMIT) {
            /* Not an engine "loss": not ready without an error, the ring
             * (queued PCM) is kept and the stream comes back by itself. */
            g_mutex_lock(&backend->lock);
            gboolean current = bus->generation == local->generation;
            if (current) { bus->ready = FALSE; bus->phase = PHASE_OPENING; bus->error[0] = '\0'; }
            g_mutex_unlock(&backend->lock);
            bus_close_stream(bus, local);
            if (!current) return;
            BusCommand command; bus_command(bus, &command);
            if (command.generation != local->generation || command.quit) return;
            if (!command.connected) { bus_publish_failure(bus, local, RADIOS_AUDIO_IO_SERVICE_DOWN, TEXT_SERVICE_DOWN); return; }
            bus_open(bus, local, &command); return;
        }
        status = RADIOS_AUDIO_IO_LOST; text_set(message, TEXT_STALLED);
    }
    bus_close_stream(bus, local);
    bus_publish_failure(bus, local, status, message);
}
static void bus_flush(Bus *bus, BusLocal *local, guint64 request)
{
    RadioSAudioBackend *backend = bus->backend; gboolean kept = FALSE;
    g_mutex_lock(&backend->lock); gboolean ready = bus->phase == PHASE_READY && bus->ready; g_mutex_unlock(&backend->lock);
    if (local->stream && ready && backend->ops->stop(local->stream) == RADIOS_AUDIO_IO_OK &&
        backend->ops->reset(local->stream) == RADIOS_AUDIO_IO_OK) {
        /* Reset dropped the device queue and returned its clock to 0: the
         * new time base. A resampled bus starts a new tag generation. */
        kept = TRUE;
        if (local->info.rate != 48000) {
            GError *error = NULL; RadioSOutputResampler *fresh = radios_output_resampler_new(local->info.rate, &error);
            if (fresh) { radios_output_resampler_free(local->resampler); local->resampler = fresh; }
            else { kept = FALSE; g_clear_error(&error); }
        }
    }
    ring_reset(backend->outputs[bus->index]);
    if (kept) {
        g_mutex_lock(&backend->lock);
        bus_accounting_reset_locked(bus);
        bus->started = FALSE; bus->refilling = TRUE; bus->refill_since = g_get_monotonic_time();
        g_mutex_unlock(&backend->lock);
    }
    bus_flush_answer(bus, request, kept);
}
static gpointer bus_main(gpointer data)
{
    Bus *bus = data; RadioSAudioBackend *backend = bus->backend;
    gpointer thread = backend->ops->thread_enter(TRUE);
    BusLocal *local = g_new0(BusLocal, 1);
    for (;;) {
        guint timeout = !local->stream ? IDLE_WAIT_MS : (bus->refilling || !bus->started) ? 1 : STALL_WAIT_MS;
        if (backend->ops->wait(local->stream, bus->wake, timeout)) local->last_event = g_get_monotonic_time();
        BusCommand command; bus_command(bus, &command);
        if (command.quit) break;
        if (command.generation != local->generation) {
            bus_close_stream(bus, local);
            local->generation = command.generation; local->reopen_attempts = 0;
            ring_reset(backend->outputs[bus->index]);   /* stale PCM of the old generation */
            if (command.flush_request != bus->flush_ack) bus_flush_answer(bus, command.flush_request, FALSE);
            if (!command.device[0] || !command.connected) {
                g_mutex_lock(&backend->lock);
                if (bus->generation == command.generation) { bus->ready = FALSE; bus->phase = command.device[0] ? PHASE_FAILED : PHASE_IDLE; }
                g_mutex_unlock(&backend->lock);
                continue;
            }
            bus_open(bus, local, &command); continue;
        }
        if (command.flush_request != bus->flush_ack) { bus_flush(bus, local, command.flush_request); continue; }
        if (!local->stream) continue;
        RadioSAudioIoStatus status = bus->started && g_get_monotonic_time() - local->last_event > STALL_US ?
            RADIOS_AUDIO_IO_REOPEN : bus_render(bus, local);
        if (status != RADIOS_AUDIO_IO_OK) bus_failure(bus, local, status, NULL);
    }
    bus_close_stream(bus, local); g_free(local);
    backend->ops->thread_leave(thread);
    return NULL;
}

/* ------------------------------------------------------------------ microphone thread */

typedef struct {
    RadioSAudioProviderStream *stream;
    guint64 generation;
    gint reopen_attempts;
    gint64 reopen_window;
    gfloat samples[BLOCK_FRAMES * 2];
} MicLocal;

static void mic_publish(RadioSAudioBackend *backend, MicLocal *local, RadioSAudioIoStatus status, const gchar *text)
{
    Mic *mic = &backend->mic;
    g_mutex_lock(&backend->lock);
    if (mic->generation == local->generation) {
        if (status == RADIOS_AUDIO_IO_OK) { mic->phase = PHASE_READY; mic->ready = TRUE; mic->error[0] = '\0'; }
        else { mic->phase = PHASE_FAILED; mic->ready = FALSE; text_set(mic->error, text && *text ? text : "Giriş aygıtı açılamadı."); }
    }
    if (status == RADIOS_AUDIO_IO_SERVICE_DOWN) backend->service_suspect = TRUE;
    g_mutex_unlock(&backend->lock);
    if (status == RADIOS_AUDIO_IO_SERVICE_DOWN) radios_wasapi_waker_signal(backend->manager_wake);
}
static void mic_close(RadioSAudioBackend *backend, MicLocal *local)
{
    if (local->stream) backend->ops->close(local->stream);
    local->stream = NULL;
}
static void mic_open(RadioSAudioBackend *backend, MicLocal *local, const gchar *source)
{
    gchar error[256] = ""; RadioSAudioStreamInfo info; memset(&info, 0, sizeof info);
    RadioSAudioProviderStream *stream = NULL;
    RadioSAudioIoStatus status = backend->ops->open(backend->provider, TRUE, source, MIC_RATE, MIC_BUFFER_FRAMES, MIC_SLOT,
        &stream, &info, error);
    g_mutex_lock(&backend->lock);
    gboolean superseded = backend->mic.generation != local->generation || backend->quit;
    g_mutex_unlock(&backend->lock);
    if (superseded) { if (stream) backend->ops->close(stream); return; }
    if (status == RADIOS_AUDIO_IO_OK && stream && (status = backend->ops->start(stream)) != RADIOS_AUDIO_IO_OK)
        text_set(error, backend->ops->last_error(stream));
    if (status != RADIOS_AUDIO_IO_OK || !stream) {
        if (stream) backend->ops->close(stream);
        mic_publish(backend, local, status == RADIOS_AUDIO_IO_OK ? RADIOS_AUDIO_IO_FAILED : status, error); return;
    }
    local->stream = stream;
    g_mutex_lock(&backend->lock); backend->mic.last_packet = 0; memset(backend->mic.peak, 0, sizeof backend->mic.peak); g_mutex_unlock(&backend->lock);
    mic_publish(backend, local, RADIOS_AUDIO_IO_OK, NULL);
}
static gpointer mic_main(gpointer data)
{
    RadioSAudioBackend *backend = data; Mic *mic = &backend->mic;
    gpointer thread = backend->ops->thread_enter(TRUE);
    MicLocal *local = g_new0(MicLocal, 1);
    for (;;) {
        (void)backend->ops->wait(local->stream, mic->wake, local->stream ? MIC_POLL_MS : IDLE_WAIT_MS);
        g_mutex_lock(&backend->lock);
        gboolean quit = backend->quit, connected = backend->connected; guint64 generation = mic->generation;
        gchar source[RADIOS_AUDIO_DEVICE_NAME_SIZE]; g_strlcpy(source, mic->source, sizeof source);
        g_mutex_unlock(&backend->lock);
        if (quit) break;
        if (generation != local->generation) {
            mic_close(backend, local); local->generation = generation; local->reopen_attempts = 0;
            if (*source && connected) mic_open(backend, local, source);
            else {
                g_mutex_lock(&backend->lock);
                if (mic->generation == generation) { mic->ready = FALSE; mic->phase = *source ? PHASE_FAILED : PHASE_IDLE; }
                g_mutex_unlock(&backend->lock);
            }
            continue;
        }
        while (local->stream) {
            guint frames = 0; gboolean silent = FALSE;
            RadioSAudioIoStatus status = backend->ops->read(local->stream, local->samples, BLOCK_FRAMES, &frames, &silent);
            if (status == RADIOS_AUDIO_IO_REOPEN) {
                gint64 now = g_get_monotonic_time();
                if (now - local->reopen_window > REOPEN_WINDOW_US) { local->reopen_window = now; local->reopen_attempts = 0; }
                mic_close(backend, local);
                if (local->reopen_attempts++ < REOPEN_LIMIT && connected) {
                    g_mutex_lock(&backend->lock);
                    if (mic->generation == local->generation) { mic->ready = FALSE; mic->phase = PHASE_OPENING; }
                    g_mutex_unlock(&backend->lock);
                    mic_open(backend, local, source);
                } else mic_publish(backend, local, RADIOS_AUDIO_IO_LOST, TEXT_STALLED);
                break;
            }
            if (status != RADIOS_AUDIO_IO_OK) {
                gchar message[256]; text_set(message, backend->ops->last_error(local->stream));
                mic_close(backend, local); mic_publish(backend, local, status, message); break;
            }
            if (!frames) break;
            if (silent) memset(local->samples, 0, frames * FRAME_BYTES);
            gfloat peak[2] = {0, 0}; gboolean finite = TRUE;
            for (guint i = 0; i < frames * 2; ++i) {
                if (!isfinite(local->samples[i])) { finite = FALSE; break; }
                peak[i % 2] = MAX(peak[i % 2], fabsf(local->samples[i]));
            }
            /* A non-finite packet is dropped whole, never sent on air. */
            if (finite) (void)radios_audio_ring_write(backend->capture, local->samples, frames, 0);
            g_mutex_lock(&backend->lock);
            if (finite) memcpy(mic->peak, peak, sizeof peak);
            mic->last_packet = g_get_monotonic_time();
            g_mutex_unlock(&backend->lock);
        }
    }
    mic_close(backend, local); g_free(local);
    backend->ops->thread_leave(thread);
    return NULL;
}

/* ------------------------------------------------------------------ manager thread */

typedef struct { RadioSAudioBackend *backend; guint generation; } ScanTicket;
static gboolean scan_cancelled(gpointer data)
{
    ScanTicket *ticket = data;
    return atomic_load(&ticket->backend->scan_generation) != ticket->generation;
}
static gboolean device_listed(const RadioSAudioDevice *devices, guint count, const gchar *name)
{
    for (guint i = 0; i < count; ++i) if (!strcmp(devices[i].name, name)) return TRUE;
    return FALSE;
}
/* Caller holds `lock` and `devices`: a configured device that failed and now
 * appears in the inventory is retried at the next control turn, not in 5 s. */
static void route_retry_if_present_locked(RadioSAudioBackend *backend)
{
    const gchar *devices[] = {backend->preferences.main_sink, backend->preferences.cue_sink, backend->preferences.monitor_sink};
    for (guint i = 0; i < 3; ++i) {
        if (!*devices[i] || phase_good(backend->buses[i].phase)) continue;
        if (g_str_equal(devices[i], RADIOS_WASAPI_DEFAULT_ID) ? backend->output_count > 0 :
            device_listed(backend->outputs_info, backend->output_count, devices[i])) backend->route_retry = 0;
    }
    const gchar *source = backend->preferences.microphone_source;
    if (*source && !phase_good(backend->mic.phase) &&
        (g_str_equal(source, RADIOS_WASAPI_DEFAULT_ID) ? backend->input_count > 0 :
         device_listed(backend->inputs_info, backend->input_count, source))) backend->route_retry = 0;
}
/* Manager thread. It alone writes `connected` and the published lists, so
 * it reads them without a lock here. */
static void manager_scan(RadioSAudioBackend *backend)
{
    g_mutex_lock(&backend->lock);
    backend->scan_requested = FALSE; atomic_store(&backend->refresh_due, 0);
    ScanTicket ticket = {backend, atomic_fetch_add(&backend->scan_generation, 1) + 1};
    g_mutex_unlock(&backend->lock);
    g_mutex_lock(&backend->devices); ++backend->device_scans; g_mutex_unlock(&backend->devices);
    guint outputs = 0, inputs = 0;
    RadioSAudioIoStatus status = backend->ops->scan(backend->provider, backend->staged_outputs, &outputs,
        backend->staged_inputs, &inputs, scan_cancelled, &ticket);
    if (status == RADIOS_AUDIO_IO_OK && !scan_cancelled(&ticket) && backend->connected) {
        /* Published lists are always complete: a scan in progress, a
         * cancelled one or a failed one leaves the previous inventory. */
        g_mutex_lock(&backend->devices);
        memcpy(backend->outputs_info, backend->staged_outputs, outputs * sizeof *backend->outputs_info);
        memcpy(backend->inputs_info, backend->staged_inputs, inputs * sizeof *backend->inputs_info);
        backend->output_count = outputs; backend->input_count = inputs;
        g_mutex_unlock(&backend->devices);
        g_mutex_lock(&backend->lock); g_mutex_lock(&backend->devices);
        route_retry_if_present_locked(backend);
        g_mutex_unlock(&backend->devices); g_mutex_unlock(&backend->lock);
        if (backend->ops->max_rate) max_rate_publish(backend->ops->max_rate(backend->provider));
    } else if (status == RADIOS_AUDIO_IO_SERVICE_DOWN) {
        g_mutex_lock(&backend->lock); backend->service_suspect = TRUE; g_mutex_unlock(&backend->lock);
    }
}
/* Pulse context FAILED/TERMINATED: everything stops until the service is back. */
static void manager_connection_lost(RadioSAudioBackend *backend, const gchar *text)
{
    g_mutex_lock(&backend->lock);
    if (backend->connected) {
        backend->connected = FALSE; text_set(backend->error, text);
        for (guint i = 0; i < 3; ++i) {
            Bus *bus = &backend->buses[i];
            ++bus->generation; bus->ready = FALSE; bus->phase = bus->device[0] ? PHASE_FAILED : PHASE_IDLE;
        }
        ++backend->mic.generation; backend->mic.ready = FALSE;
        backend->mic.phase = backend->mic.source[0] ? PHASE_FAILED : PHASE_IDLE;
        g_mutex_lock(&backend->devices); backend->output_count = backend->input_count = 0; g_mutex_unlock(&backend->devices);
        atomic_store(&backend->refresh_due, 0); backend->scan_requested = FALSE;
        atomic_fetch_add(&backend->scan_generation, 1);
        if (!backend->next_retry) backend->next_retry = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    }
    g_mutex_unlock(&backend->lock);
    wake_all(backend);
}
static gpointer manager_main(gpointer data)
{
    RadioSAudioBackend *backend = data;
    gpointer thread = backend->ops->thread_enter(FALSE);
    gboolean attached = FALSE, initial = TRUE;
    gint64 next_service_check = 0;
    for (;;) {
        g_mutex_lock(&backend->lock);
        if (backend->quit) { g_mutex_unlock(&backend->lock); break; }
        gint64 now = g_get_monotonic_time();
        gboolean attempt = !backend->connected && now >= backend->next_retry;
        if (attempt && !initial) {
            /* The failure itself scheduled the first retry after 1 s; then 2 s, then 5 s. */
            guint delay = backend->retries == 0 ? 2 : 5;
            if (backend->retries < 3) ++backend->retries;
            backend->next_retry = now + delay * G_TIME_SPAN_SECOND;
        }
        initial = FALSE;
        gboolean suspect = backend->service_suspect; backend->service_suspect = FALSE;
        g_mutex_unlock(&backend->lock);
        if (attempt) {
            if (attached) { backend->ops->disconnect(backend->provider); attached = FALSE; }
            gchar error[256] = "";
            RadioSAudioIoStatus status = backend->ops->connect(backend->provider, error);
            attached = status == RADIOS_AUDIO_IO_OK;
            gboolean running = attached && backend->ops->service_running(backend->provider);
            g_mutex_lock(&backend->lock);
            if (running) {
                backend->connected = TRUE; backend->error[0] = '\0'; backend->retries = 0; backend->next_retry = 0;
                backend->reapply = TRUE; backend->scan_requested = TRUE; backend->route_retry = 0;
            } else {
                text_set(backend->error, attached || status == RADIOS_AUDIO_IO_SERVICE_DOWN ? TEXT_SERVICE_DOWN : error);
                if (!backend->next_retry) backend->next_retry = now + G_TIME_SPAN_SECOND;
            }
            g_mutex_unlock(&backend->lock);
            next_service_check = now + SERVICE_POLL_US;
        } else if (suspect || now >= next_service_check) {
            next_service_check = now + SERVICE_POLL_US;
            g_mutex_lock(&backend->lock); gboolean connected = backend->connected; g_mutex_unlock(&backend->lock);
            if (connected && !backend->ops->service_running(backend->provider)) manager_connection_lost(backend, TEXT_SERVICE_DOWN);
        }
        g_mutex_lock(&backend->lock);
        gboolean connected = backend->connected, requested = backend->scan_requested;
        gint64 retry = backend->next_retry;
        g_mutex_unlock(&backend->lock);
        gint64 due = atomic_load(&backend->refresh_due);
        now = g_get_monotonic_time();
        if (connected && (requested || (due && now >= due))) { manager_scan(backend); continue; }
        gint64 wait = next_service_check - now;
        if (connected && due) wait = MIN(wait, due - now);
        if (!connected) wait = MIN(wait, retry - now);
        wait = CLAMP(wait, 1000, (gint64)SERVICE_POLL_US);
        (void)backend->ops->wait(NULL, backend->manager_wake, (guint)((wait + 999) / 1000));
    }
    if (attached) backend->ops->disconnect(backend->provider);
    backend->ops->thread_leave(thread);
    return NULL;
}
static void hotplug_notify(gpointer core, RadioSAudioHotplugKind kind)
{
    RadioSAudioBackend *backend = core;
    if (kind == RADIOS_AUDIO_HOTPLUG_IGNORED) return;
    /* Trailing debounce: a burst of events becomes one scan 250 ms after the last. */
    atomic_store(&backend->refresh_due, g_get_monotonic_time() + HOTPLUG_DEBOUNCE_US);
    radios_wasapi_waker_signal(backend->manager_wake);
}

/* ------------------------------------------------------------------ API */

static RadioSAudioBackend *backend_new(RadioSAudioRing *outputs[3], RadioSAudioRing *capture,
    const RadioSAudioProviderOps *ops, gpointer config, const gchar *unavailable)
{
    RadioSAudioBackend *backend = g_new0(RadioSAudioBackend, 1);
    g_mutex_init(&backend->lock); g_mutex_init(&backend->devices); g_cond_init(&backend->cond);
    for (guint i = 0; i < 3; ++i) { backend->outputs[i] = outputs[i]; backend->buses[i].backend = backend; backend->buses[i].index = i; }
    backend->capture = capture; backend->output_sample_rate = 48000; backend->output_buffer_frames = 2400;
    atomic_init(&backend->refresh_due, 0); atomic_init(&backend->scan_generation, 0);
    if (!ops) { text_set(backend->error, unavailable); return backend; }
    backend->ops = ops; backend->sink.notify = hotplug_notify; backend->sink.core = backend;
    gboolean ready = (backend->provider = ops->create(&backend->sink, config)) != NULL &&
        (backend->manager_wake = radios_wasapi_waker_new()) && (backend->mic.wake = radios_wasapi_waker_new());
    for (guint i = 0; i < 3 && ready; ++i) ready = (backend->buses[i].wake = radios_wasapi_waker_new()) != NULL;
    static const gchar *const names[] = {"radios-audio-main", "radios-audio-cue", "radios-audio-mon"};
    if (ready) ready = (backend->manager = g_thread_try_new("radios-audio-mgr", manager_main, backend, NULL)) != NULL;
    for (guint i = 0; i < 3 && ready; ++i)
        ready = (backend->buses[i].thread = g_thread_try_new(names[i], bus_main, &backend->buses[i], NULL)) != NULL;
    if (ready) ready = (backend->mic.thread = g_thread_try_new("radios-audio-mic", mic_main, backend, NULL)) != NULL;
    if (!ready) {
        g_mutex_lock(&backend->lock); backend->quit = TRUE; text_set(backend->error, TEXT_START_FAILED); g_mutex_unlock(&backend->lock);
        wake_all(backend);
        for (guint i = 0; i < 3; ++i) if (backend->buses[i].thread) { g_thread_join(backend->buses[i].thread); backend->buses[i].thread = NULL; }
        if (backend->mic.thread) { g_thread_join(backend->mic.thread); backend->mic.thread = NULL; }
        if (backend->manager) { g_thread_join(backend->manager); backend->manager = NULL; }
        return backend;
    }
    backend->started = TRUE;
    return backend;
}
gboolean radios_audio_server_resolve(const gchar *server, gchar error[256])
{
    const gchar *locator = server && *server ? server : g_getenv("RADIOS_AUDIO_SERVER");
    if (error) error[0] = '\0';
    if (!locator || !*locator || !g_ascii_strcasecmp(locator, "wasapi") || !g_ascii_strcasecmp(locator, "wasapi:")) return TRUE;
    if (error) {
        gchar *text = g_strdup_printf("Bu ses sunucusu adresi Windows'ta desteklenmiyor: %s", locator);
        text_set(error, text); g_free(text);
    }
    return FALSE;
}
RadioSAudioBackend *radios_audio_backend_new(RadioSAudioRing *outputs[3], RadioSAudioRing *capture, const gchar *server)
{
    gchar error[256];
    return radios_audio_server_resolve(server, error) ? backend_new(outputs, capture, &wasapi_ops, NULL, NULL) :
        backend_new(outputs, capture, NULL, NULL, error);
}
RadioSAudioBackend *radios_audio_backend_new_with_provider(RadioSAudioRing *outputs[3], RadioSAudioRing *capture,
    const RadioSAudioProviderOps *ops, gpointer config)
{
    return backend_new(outputs, capture, ops, config, ops ? NULL : TEXT_START_FAILED);
}
const gchar *radios_audio_backend_label(RadioSAudioBackend *backend)
{ return backend && backend->ops ? backend->ops->label : "Desteklenmeyen ses sunucusu"; }
void radios_audio_backend_configure_output(RadioSAudioBackend *backend, guint rate, guint buffer_frames)
{
    if (!backend || !backend->started) return;
    g_mutex_lock(&backend->lock);
    backend->output_sample_rate = rate; backend->output_buffer_frames = buffer_frames;
    g_mutex_unlock(&backend->lock);
}
void radios_audio_backend_apply(RadioSAudioBackend *backend, const RadioSAudioPreferences *preferences)
{
    if (!backend || !backend->started || !preferences) return;
    g_mutex_lock(&backend->lock);
    backend->preferences = *preferences;
    if (!backend->connected) { g_mutex_unlock(&backend->lock); return; }
    const gchar *devices[] = {preferences->main_sink, preferences->cue_sink, preferences->monitor_sink};
    for (guint i = 0; i < 3; ++i) {
        Bus *bus = &backend->buses[i];
        if (!strcmp(bus->device, devices[i]) && bus->rate == backend->output_sample_rate &&
            bus->buffer_frames == backend->output_buffer_frames && (!*devices[i] || phase_good(bus->phase))) continue;
        /* "" = unassigned, "@default" = Windows' default output when it opens
         * (never followed later), any other id = exactly that endpoint. */
        ++bus->generation; bus->ready = FALSE; bus->phase = *devices[i] ? PHASE_OPENING : PHASE_IDLE; bus->error[0] = '\0';
        bus->rate = backend->output_sample_rate; bus->buffer_frames = backend->output_buffer_frames;
        g_strlcpy(bus->device, devices[i], sizeof bus->device);
        radios_wasapi_waker_signal(bus->wake);
    }
    Mic *mic = &backend->mic;
    if (strcmp(mic->source, preferences->microphone_source) || (*preferences->microphone_source && !phase_good(mic->phase))) {
        ++mic->generation; mic->ready = FALSE; mic->phase = *preferences->microphone_source ? PHASE_OPENING : PHASE_IDLE;
        mic->error[0] = '\0';
        atomic_store(&backend->capture->read, atomic_load(&backend->capture->write));
        g_strlcpy(mic->source, preferences->microphone_source, sizeof mic->source);
        radios_wasapi_waker_signal(mic->wake);
    }
    g_mutex_unlock(&backend->lock);
}
void radios_audio_backend_refresh(RadioSAudioBackend *backend)
{
    if (!backend || !backend->started) return;
    g_mutex_lock(&backend->lock);
    if (backend->connected) {
        /* A rescan requested while one runs cancels and restarts it. */
        atomic_fetch_add(&backend->scan_generation, 1);
        backend->scan_requested = TRUE; atomic_store(&backend->refresh_due, 0);
    }
    g_mutex_unlock(&backend->lock);
    radios_wasapi_waker_signal(backend->manager_wake);
}
void radios_audio_backend_reconnect(RadioSAudioBackend *backend)
{
    if (!backend || !backend->started) return;
    g_mutex_lock(&backend->lock);
    gboolean apply = backend->connected && backend->reapply;
    backend->reapply = FALSE;
    gint64 now = g_get_monotonic_time();
    if (backend->connected && now >= backend->route_retry) {
        const gchar *devices[] = {backend->preferences.main_sink, backend->preferences.cue_sink, backend->preferences.monitor_sink};
        gboolean retry = FALSE;
        for (guint i = 0; i < 3; ++i) if (*devices[i] && !phase_good(backend->buses[i].phase)) retry = TRUE;
        if (*backend->preferences.microphone_source && !phase_good(backend->mic.phase)) retry = TRUE;
        if (retry) { apply = TRUE; backend->route_retry = now + ROUTE_RETRY_US; }
    }
    RadioSAudioPreferences preferences = backend->preferences;
    g_mutex_unlock(&backend->lock);
    if (apply) radios_audio_backend_apply(backend, &preferences);
}
void radios_audio_backend_flush(RadioSAudioBackend *backend)
{ radios_audio_backend_flush_buses(backend, (1u << RADIOS_AUDIO_BUS_COUNT) - 1); }
void radios_audio_backend_flush_buses(RadioSAudioBackend *backend, guint bus_mask)
{
    if (!backend || !backend->started) return;
    g_mutex_lock(&backend->lock);
    guint waiting = 0; guint64 requests[3] = {0, 0, 0};
    for (guint i = 0; i < 3; ++i) {
        if (!(bus_mask & (1u << i))) continue;
        Bus *bus = &backend->buses[i];
        requests[i] = ++bus->flush_request;
        if (bus->phase == PHASE_READY && bus->ready) waiting |= 1u << i;
        radios_wasapi_waker_signal(bus->wake);
    }
    /* All masked buses flush in parallel; the worst case stays 100 ms. */
    gint64 deadline = g_get_monotonic_time() + FLUSH_WAIT_US;
    for (;;) {
        gboolean pending = FALSE;
        for (guint i = 0; i < 3; ++i) if ((waiting & (1u << i)) && backend->buses[i].flush_ack != requests[i]) pending = TRUE;
        if (!pending || !g_cond_wait_until(&backend->cond, &backend->lock, deadline)) break;
    }
    for (guint i = 0; i < 3; ++i) {
        if (!(bus_mask & (1u << i))) continue;
        Bus *bus = &backend->buses[i];
        /* A stream still opening has played nothing (its thread drops the
         * stale ring before the first write); a playing one was drained in
         * place. Anything else is re-created by the apply below. */
        gboolean kept = ((waiting & (1u << i)) && bus->flush_ack == requests[i] && bus->flush_kept) ||
            (bus->phase == PHASE_OPENING && !bus->written);
        if (!kept && phase_good(bus->phase)) {
            ++bus->generation; bus->ready = FALSE; bus->phase = PHASE_IDLE;
            radios_wasapi_waker_signal(bus->wake);
        }
    }
    RadioSAudioPreferences preferences = backend->preferences;
    g_mutex_unlock(&backend->lock);
    radios_audio_backend_apply(backend, &preferences);
}
void radios_audio_backend_snapshot(RadioSAudioBackend *backend, RadioSAudioBackendSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof *snapshot);
    if (!backend) return;
    g_mutex_lock(&backend->lock);
    snapshot->connected = backend->connected; g_strlcpy(snapshot->error, backend->error, sizeof snapshot->error);
    for (guint i = 0; i < 3; ++i) {
        Bus *bus = &backend->buses[i];
        snapshot->ready[i] = bus->ready; snapshot->underruns[i] = bus->underruns; snapshot->gap_frames[i] = bus->gap_frames;
        g_strlcpy(snapshot->bus_error[i], bus->error, sizeof snapshot->bus_error[i]);
        if (!bus->ready || !bus->actual_rate) continue;
        guint rate = bus->actual_rate;
        /* Played = device clock since open/flush, capped by what was written:
         * time spent starving never counts as played audio. */
        guint64 consumed = MIN(bus->position, bus->written);
        snapshot->consumed_frames[i] = consumed * 48000 / rate;
        if (rate != 48000 && consumed && bus->written - consumed < PLAYBACK_TAG_CAPACITY)
            snapshot->played_tag[i] = bus->playback_tags[(consumed - 1) % PLAYBACK_TAG_CAPACITY];
        guint64 count = MIN(bus->span_write, (guint64)SPAN_COUNT);
        for (guint64 j = 0; j < count; ++j) {
            const WriteSpan *span = &bus->spans[(bus->span_write - count + j) % SPAN_COUNT];
            if (rate == 48000 && span->first_tag && span->count && consumed > span->start)
                snapshot->played_tag[i] = MAX(snapshot->played_tag[i], span->first_tag + MIN(consumed - span->start, span->count) - 1);
            if (span->start + span->length <= consumed) {
                memcpy(snapshot->peak[i], span->peak, sizeof span->peak); memcpy(snapshot->rms[i], span->rms, sizeof span->rms);
            }
        }
        snapshot->output_sample_rate[i] = rate; snapshot->output_buffer_frames[i] = bus->fill_target;
    }
    Mic *mic = &backend->mic;
    snapshot->microphone_ready = mic->ready;
    g_strlcpy(snapshot->microphone_error, mic->error, sizeof snapshot->microphone_error);
    /* Output loopback delivers no packets while that output is idle. */
    if (mic->ready && mic->last_packet && g_get_monotonic_time() - mic->last_packet <= MIC_PEAK_HOLD_US)
        memcpy(snapshot->microphone_peak, mic->peak, sizeof snapshot->microphone_peak);
    g_mutex_unlock(&backend->lock);
    g_mutex_lock(&backend->devices);
    snapshot->output_count = backend->output_count; snapshot->input_count = backend->input_count;
    memcpy(snapshot->outputs, backend->outputs_info, backend->output_count * sizeof *snapshot->outputs);
    memcpy(snapshot->inputs, backend->inputs_info, backend->input_count * sizeof *snapshot->inputs);
    snapshot->device_scans = backend->device_scans;
    g_mutex_unlock(&backend->devices);
}
void radios_audio_backend_free(RadioSAudioBackend *backend)
{
    if (!backend) return;
    g_mutex_lock(&backend->lock); backend->quit = TRUE; g_mutex_unlock(&backend->lock);
    if (backend->started) {
        wake_all(backend);
        if (backend->mic.thread) g_thread_join(backend->mic.thread);
        for (guint i = 0; i < 3; ++i) if (backend->buses[i].thread) g_thread_join(backend->buses[i].thread);
        if (backend->manager) g_thread_join(backend->manager);
    }
    if (backend->ops && backend->provider) backend->ops->destroy(backend->provider);
    for (guint i = 0; i < 3; ++i) radios_wasapi_waker_free(backend->buses[i].wake);
    radios_wasapi_waker_free(backend->mic.wake); radios_wasapi_waker_free(backend->manager_wake);
    g_cond_clear(&backend->cond); g_mutex_clear(&backend->devices); g_mutex_clear(&backend->lock); g_free(backend);
}
