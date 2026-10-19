/* WASAPI layer and engine backend against the real Windows audio stack.
 *
 * Always runs: COM object reference counting and the notification watch.
 * With RADIOS_TEST_WASAPI=1 and an active output (label "hardware"): device
 * enumeration, a 1 kHz tone at -60 dBFS (practically inaudible) on the
 * default output for at most one second per case, verified through the
 * output's loopback; missing devices; silent reopen after an invalidated
 * stream; device loss and route retry; a simulated Windows Audio outage;
 * microphone open/close (nothing is stored); process loopback; sessions; and
 * the realtime engine on the same path. Every case ends with no open stream,
 * no live COM callback object and no IAudioClient left referenced. */
#include "../src/audio/audio_internal.h"
#include "../src/audio/wasapi_device.h"
#include "radios/platform.h"
#include <math.h>
#include <string.h>

#define TONE_HZ 1000.0
#define TONE_AMPLITUDE 0.001   /* -60 dBFS */
#define RATE 48000u
#define BLOCK 480u
#define RING_FRAMES 8192u
#define CAPTURE_SECONDS 4u

static gboolean hardware;

static void counters_clean(void)
{
    RadioSWasapiDebugCounters counters; radios_wasapi_debug_counters(&counters);
    g_assert_cmpint(counters.streams, ==, 0);
    g_assert_cmpint(counters.notify_clients, ==, 0);
    g_assert_cmpint(counters.activation_handlers, ==, 0);
    g_assert_cmpuint(counters.unreleased_clients, ==, 0);
}
static gboolean need_hardware(void)
{
    if (!hardware) g_test_skip("RADIOS_TEST_WASAPI=1 and an active Windows output are required");
    return hardware;
}
static void ring_setup(RadioSAudioRing *ring, gboolean tags)
{
    memset(ring, 0, sizeof *ring); ring->capacity = RING_FRAMES;
    ring->samples = g_new0(gfloat, RING_FRAMES * 2); if (tags) ring->tags = g_new0(guint64, RING_FRAMES);
    atomic_init(&ring->read, 0); atomic_init(&ring->write, 0);
}
static void ring_teardown(RadioSAudioRing *ring) { g_free(ring->samples); g_free(ring->tags); }
/* Coherent amplitude of one frequency (mono sum) over a span. */
static gdouble amplitude(const gfloat *stereo, guint frames, gdouble hz)
{
    gdouble i = 0, q = 0;
    for (guint n = 0; n < frames; ++n) {
        gdouble phase = 2 * G_PI * hz * n / RATE, s = (stereo[n * 2] + stereo[n * 2 + 1]) * 0.5;
        i += s * cos(phase); q += s * sin(phase);
    }
    return frames ? 2 * sqrt(i * i + q * q) / frames : 0;
}
/* The captured loopback must carry the tone: coherent over the tone's span
 * after its onset, well above the neighbouring bins (other programs may be
 * playing at the same time). */
static void assert_tone(const gfloat *captured, guint frames, guint tone_frames, const gchar *label)
{
    guint onset = 0;
    while (onset < frames && fabsf(captured[onset * 2]) < TONE_AMPLITUDE * 0.3f) ++onset;
    guint start = onset + RATE / 20, span = tone_frames > RATE / 5 ? tone_frames - RATE / 5 : 0;
    guint length = start < frames ? MIN(frames - start, MIN(span, RATE)) : 0;
    g_assert_cmpuint(length, >=, RATE / 4);
    gdouble tone = amplitude(captured + (gsize)start * 2, length, TONE_HZ);
    gdouble below = amplitude(captured + (gsize)start * 2, length, 830), above = amplitude(captured + (gsize)start * 2, length, 1170);
    g_test_message("%s: 1 kHz %.6f (expected %.6f), 830 Hz %.6f, 1170 Hz %.6f over %u frames",
        label, tone, TONE_AMPLITUDE, below, above, length);
    g_assert_cmpfloat(tone, >, TONE_AMPLITUDE * 0.4); g_assert_cmpfloat(tone, <, TONE_AMPLITUDE * 2.0);
    g_assert_cmpfloat(tone, >, 5 * MAX(below, above));
}

/* ------------------------------------------------------------------ layer */

static void test_com_objects(void)
{
    gchar error[RADIOS_WASAPI_ERROR_SIZE] = "";
    g_assert_true(radios_wasapi_debug_com_selftest(error));
    counters_clean();
}
typedef struct { gint calls, last_kind; gchar last_id[RADIOS_WASAPI_ID_SIZE]; gboolean relevant; } WatchProbe;
static void watch_probe(const RadioSWasapiChange *change, gpointer data)
{
    WatchProbe *probe = data;
    g_atomic_int_inc(&probe->calls); probe->last_kind = change->kind; probe->relevant = change->relevant;
    g_strlcpy(probe->last_id, change->endpoint_id ? change->endpoint_id : "", sizeof probe->last_id);
}
static void test_watch_lifecycle(void)
{
    gchar error[RADIOS_WASAPI_ERROR_SIZE] = "";
    g_assert_true(radios_wasapi_thread_enter(error));
    RadioSWasapiEnumerator *enumerator = radios_wasapi_enumerator_new(error);
    g_assert_nonnull(enumerator);
    RadioSWasapiWatch *watch = radios_wasapi_watch_new(enumerator, error);
    g_assert_nonnull(watch);
    RadioSWasapiDebugCounters counters; radios_wasapi_debug_counters(&counters);
    g_assert_cmpint(counters.notify_clients, ==, 1);
    WatchProbe first = {0}, second = {0};
    guint a = radios_wasapi_watch_add(watch, watch_probe, &first), b = radios_wasapi_watch_add(watch, watch_probe, &second);
    g_assert_cmpuint(a, >, 0); g_assert_cmpuint(b, >, a);
    radios_wasapi_debug_watch_emit(watch, RADIOS_WASAPI_CHANGE_ADDED, "{0.0.0.00000000}.{test-ğüş}");
    g_assert_cmpint(first.calls, ==, 1); g_assert_cmpint(second.calls, ==, 1);
    g_assert_cmpstr(first.last_id, ==, "{0.0.0.00000000}.{test-ğüş}"); g_assert_true(first.relevant);
    radios_wasapi_debug_watch_emit(watch, RADIOS_WASAPI_CHANGE_DEFAULT, NULL);
    g_assert_cmpint(first.last_kind, ==, RADIOS_WASAPI_CHANGE_DEFAULT); g_assert_cmpstr(first.last_id, ==, "");
    g_assert_true(first.relevant);   /* eConsole */
    radios_wasapi_watch_remove(watch, a);
    radios_wasapi_debug_watch_emit(watch, RADIOS_WASAPI_CHANGE_PROPERTY, "x");
    g_assert_cmpint(first.calls, ==, 2); g_assert_cmpint(second.calls, ==, 3); g_assert_true(second.relevant);
    radios_wasapi_watch_free(watch);
    radios_wasapi_enumerator_free(enumerator);
    radios_wasapi_thread_leave();
    counters_clean();
}
static void test_enumerate(void)
{
    if (!need_hardware()) return;
    gchar error[RADIOS_WASAPI_ERROR_SIZE] = "";
    g_assert_true(radios_wasapi_thread_enter(error));
    RadioSWasapiEnumerator *enumerator = radios_wasapi_enumerator_new(error); g_assert_nonnull(enumerator);
    RadioSWasapiEndpoint *list = g_new0(RadioSWasapiEndpoint, 64);
    for (guint flow = 0; flow < 2; ++flow) {
        guint count = 0, defaults = 0; gboolean stopped = TRUE;
        g_assert_cmpint(radios_wasapi_enumerator_list(enumerator, flow, list, 64, &count, NULL, NULL, &stopped, error), ==, RADIOS_WASAPI_OK);
        g_assert_false(stopped);
        if (flow == RADIOS_WASAPI_FLOW_RENDER) g_assert_cmpuint(count, >=, 1);
        gchar expected[RADIOS_WASAPI_ID_SIZE] = "";
        gboolean has_default = radios_wasapi_enumerator_default(enumerator, flow, expected);
        for (guint i = 0; i < count; ++i) {
            g_test_message("%s %s \"%s\" %u ch %u Hz%s", flow ? "capture" : "render", list[i].id, list[i].name,
                list[i].channels, list[i].rate, list[i].is_default ? " (default)" : "");
            g_assert_true(list[i].id[0] && strlen(list[i].id) <= 512 && g_utf8_validate(list[i].id, -1, NULL));
            g_assert_true(list[i].name[0] && g_utf8_validate(list[i].name, -1, NULL));
            g_assert_cmpuint(list[i].state, ==, 1); g_assert_cmpint(list[i].flow, ==, flow);
            defaults += list[i].is_default;
            if (list[i].is_default) g_assert_cmpstr(list[i].id, ==, expected);
            RadioSWasapiEndpoint again;
            g_assert_true(radios_wasapi_enumerator_lookup(enumerator, list[i].id, &again));
            g_assert_cmpstr(again.name, ==, list[i].name); g_assert_cmpint(again.flow, ==, flow);
        }
        g_assert_cmpuint(defaults, ==, has_default && count ? 1 : 0);
    }
    RadioSWasapiEndpoint missing;
    g_assert_false(radios_wasapi_enumerator_lookup(enumerator, "{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}", &missing));
    g_assert_false(radios_wasapi_enumerator_lookup(enumerator, "alsa_output.pci-0000_00_1f.3.analog-stereo", &missing));
    g_free(list); radios_wasapi_enumerator_free(enumerator); radios_wasapi_thread_leave();
    counters_clean();
}
/* Renders the tone on the default output with the layer alone while the
 * given capture stream records. */
static guint layer_tone(RadioSWasapiStream *capture, gdouble seconds, gfloat *captured, guint capacity)
{
    gchar error[RADIOS_WASAPI_ERROR_SIZE] = "";
    RadioSWasapiStreamSpec spec; memset(&spec, 0, sizeof spec);
    spec.kind = RADIOS_WASAPI_STREAM_RENDER; spec.endpoint_id = RADIOS_WASAPI_DEFAULT_ID; spec.rate = RATE;
    spec.buffer_frames = 2400; spec.session_name = "RadioS test";
    RadioSWasapiStream *render = NULL; RadioSWasapiStreamInfo info;
    g_assert_cmpint(radios_wasapi_stream_open(NULL, &spec, &render, &info, error), ==, RADIOS_WASAPI_OK);
    g_assert_true(info.event_driven); g_assert_cmpuint(info.rate, ==, RATE); g_assert_cmpuint(info.buffer_frames, >=, 2400);
    gfloat *block = g_new0(gfloat, 4096 * 2); guint64 phase = 0; guint frames = 0;
    g_assert_cmpint(radios_wasapi_stream_start(capture), ==, RADIOS_WASAPI_OK);
    g_assert_cmpint(radios_wasapi_stream_write(render, NULL, 960), ==, RADIOS_WASAPI_OK);
    g_assert_cmpint(radios_wasapi_stream_start(render), ==, RADIOS_WASAPI_OK);
    gint64 end = g_get_monotonic_time() + (gint64)(seconds * G_TIME_SPAN_SECOND);
    while (g_get_monotonic_time() < end) {
        (void)radios_wasapi_wait(render, NULL, 20);
        guint queued = 0; g_assert_cmpint(radios_wasapi_stream_padding(render, &queued), ==, RADIOS_WASAPI_OK);
        for (guint want = queued < 2400 ? 2400 - queued : 0; want;) {
            guint n = MIN(want, 4096u);
            for (guint i = 0; i < n; ++i)
                block[i * 2] = block[i * 2 + 1] = (gfloat)(TONE_AMPLITUDE * sin(2 * G_PI * TONE_HZ * (gdouble)(phase + i) / RATE));
            g_assert_cmpint(radios_wasapi_stream_write(render, block, n), ==, RADIOS_WASAPI_OK);
            phase += n; want -= n;
        }
        for (guint got = 1; got;) {
            guint flags = 0;
            g_assert_cmpint(radios_wasapi_stream_read(capture, block, 4096, &got, &flags), ==, RADIOS_WASAPI_OK);
            got = MIN(got, capacity - frames);
            memcpy(captured + (gsize)frames * 2, block, (gsize)got * 2 * sizeof(gfloat)); frames += got;
            if (frames == capacity) got = 0;
        }
    }
    guint64 position = 0; g_assert_cmpint(radios_wasapi_stream_position(render, &position), ==, RADIOS_WASAPI_OK);
    g_assert_cmpuint(position, >, 0); g_assert_cmpuint(position, <=, phase + 960);
    radios_wasapi_stream_close(render); g_free(block);
    return frames;
}
static void test_process_loopback(void)
{
    if (!need_hardware()) return;
    gchar error[RADIOS_WASAPI_ERROR_SIZE] = "";
    g_assert_true(radios_wasapi_thread_enter(error));
    if (!radios_wasapi_process_loopback_supported(error)) {
        g_test_skip(error); radios_wasapi_thread_leave(); return;
    }
    RadioSWasapiStreamSpec spec; memset(&spec, 0, sizeof spec);
    spec.kind = RADIOS_WASAPI_STREAM_PROCESS_LOOPBACK; spec.process_id = radios_process_id(); spec.rate = RATE;
    RadioSWasapiStream *capture = NULL; RadioSWasapiStreamInfo info;
    g_assert_cmpint(radios_wasapi_stream_open(NULL, &spec, &capture, &info, error), ==, RADIOS_WASAPI_OK);
    g_assert_cmpuint(info.buffer_frames, >, 0);
    gfloat *captured = g_new0(gfloat, (gsize)RATE * CAPTURE_SECONDS * 2);
    guint frames = layer_tone(capture, 1.0, captured, RATE * CAPTURE_SECONDS);
    assert_tone(captured, frames, RATE, "process loopback (this process only)");
    radios_wasapi_stream_close(capture); g_free(captured);
    /* A process that plays nothing yields silence, never the mixed output. */
    radios_wasapi_thread_leave();
    counters_clean();
}
static void test_sessions(void)
{
    if (!need_hardware()) return;
    gchar error[RADIOS_WASAPI_ERROR_SIZE] = "";
    g_assert_true(radios_wasapi_thread_enter(error));
    RadioSWasapiStreamSpec spec; memset(&spec, 0, sizeof spec);
    static const guint8 guid[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x10};
    spec.kind = RADIOS_WASAPI_STREAM_RENDER; spec.endpoint_id = RADIOS_WASAPI_DEFAULT_ID; spec.rate = RATE;
    spec.session_guid = guid; spec.session_name = "RadioS oturum sınaması";
    RadioSWasapiStream *render = NULL;
    g_assert_cmpint(radios_wasapi_stream_open(NULL, &spec, &render, NULL, error), ==, RADIOS_WASAPI_OK);
    RadioSWasapiEnumerator *enumerator = radios_wasapi_enumerator_new(error); g_assert_nonnull(enumerator);
    RadioSWasapiSession *sessions = g_new0(RadioSWasapiSession, 64); guint count = 0, mine = 0;
    g_assert_cmpint(radios_wasapi_sessions_list(enumerator, TRUE, sessions, 64, &count, error), ==, RADIOS_WASAPI_OK);
    guint64 started = 0; g_assert_true(radios_wasapi_process_start_time(radios_process_id(), &started));
    for (guint i = 0; i < count; ++i) {
        g_test_message("session pid %u %s \"%s\"%s", sessions[i].pid, sessions[i].exe, sessions[i].name, sessions[i].active ? " active" : "");
        g_assert_true(g_utf8_validate(sessions[i].name, -1, NULL) && sessions[i].name[0]);
        if (i) g_assert_cmpint(g_utf8_collate(sessions[i - 1].name, sessions[i].name), <=, 0);
        if (sessions[i].pid == radios_process_id()) {
            ++mine; g_assert_cmpuint(sessions[i].start_time, ==, started); g_assert_true(sessions[i].endpoint_id[0]);
            g_assert_true(g_str_has_suffix(sessions[i].exe, ".exe"));
        }
    }
    g_assert_cmpuint(mine, ==, 1);
    g_assert_cmpint(radios_wasapi_sessions_list(enumerator, FALSE, sessions, 64, &count, error), ==, RADIOS_WASAPI_OK);
    for (guint i = 0; i < count; ++i) g_assert_cmpuint(sessions[i].pid, !=, radios_process_id());
    g_free(sessions); radios_wasapi_enumerator_free(enumerator); radios_wasapi_stream_close(render);
    radios_wasapi_thread_leave();
    counters_clean();
}
/* Opens the default microphone, reads a third of a second and closes it.
 * Only a peak value is kept; no audio is stored or written anywhere. */
static void test_microphone(void)
{
    if (!need_hardware()) return;
    gchar error[RADIOS_WASAPI_ERROR_SIZE] = "";
    g_assert_true(radios_wasapi_thread_enter(error));
    RadioSWasapiStreamSpec spec; memset(&spec, 0, sizeof spec);
    spec.kind = RADIOS_WASAPI_STREAM_CAPTURE; spec.endpoint_id = RADIOS_WASAPI_DEFAULT_ID; spec.rate = RATE;
    RadioSWasapiStream *capture = NULL; RadioSWasapiStreamInfo info;
    RadioSWasapiStatus status = radios_wasapi_stream_open(NULL, &spec, &capture, &info, error);
    if (status != RADIOS_WASAPI_OK) {
        g_test_message("microphone: %s", error);
        /* No input device, or the Windows microphone privacy switch: both
         * are reported in Turkish and are not test failures. */
        g_assert_true(strstr(error, "Mikrofon izni kapalı") || strstr(error, "varsayılan giriş"));
        radios_wasapi_thread_leave(); g_test_skip("microphone unavailable"); return;
    }
    g_test_message("microphone %s (%s): buffer %u, mix %u Hz/%u ch, converted %d", info.endpoint_name, info.endpoint_id,
        info.buffer_frames, info.mix_rate, info.mix_channels, info.converted);
    g_assert_cmpint(radios_wasapi_stream_start(capture), ==, RADIOS_WASAPI_OK);
    gfloat *block = g_new0(gfloat, 4096 * 2); guint64 frames = 0; gfloat peak = 0;
    gint64 end = g_get_monotonic_time() + G_TIME_SPAN_SECOND / 3;
    while (g_get_monotonic_time() < end) {
        (void)radios_wasapi_wait(capture, NULL, 10);
        for (guint got = 1; got;) {
            guint flags = 0;
            g_assert_cmpint(radios_wasapi_stream_read(capture, block, 4096, &got, &flags), ==, RADIOS_WASAPI_OK);
            for (guint i = 0; i < got * 2; ++i) { g_assert_true(isfinite(block[i])); peak = MAX(peak, fabsf(block[i])); }
            frames += got;
        }
    }
    memset(block, 0, 4096 * 2 * sizeof(gfloat));
    g_test_message("microphone delivered %" G_GUINT64_FORMAT " frames, peak %.4f", frames, peak);
    g_assert_cmpuint(frames, >, RATE / 10);
    radios_wasapi_stream_close(capture); g_free(block);
    radios_wasapi_thread_leave();
    counters_clean();
}

/* ------------------------------------------------------------------ backend */

typedef struct {
    RadioSAudioRing rings[3], capture;
    RadioSAudioRing *outputs[3];
    RadioSAudioBackend *backend;
    RadioSAudioBackendSnapshot *snapshot;
    RadioSAudioPreferences preferences;
    _Atomic guint64 next_tag;
    guint64 phase;
    GThread *producer;
    gint producer_stop, producer_tone;
    gchar default_output[RADIOS_AUDIO_DEVICE_NAME_SIZE];
} Harness;
static void harness_open(Harness *h)
{
    memset(h, 0, sizeof *h); atomic_init(&h->next_tag, 1);
    for (guint i = 0; i < 3; ++i) { ring_setup(&h->rings[i], TRUE); h->outputs[i] = &h->rings[i]; }
    ring_setup(&h->capture, FALSE);
    h->snapshot = g_new0(RadioSAudioBackendSnapshot, 1);
    h->backend = radios_audio_backend_new(h->outputs, &h->capture, NULL);
    radios_audio_preferences_default(&h->preferences);
    h->preferences.main_sink[0] = '\0';
    g_assert_cmpstr(radios_audio_backend_label(h->backend), ==, "Windows ses (WASAPI)");
}
static void harness_close(Harness *h)
{
    radios_audio_backend_free(h->backend);
    for (guint i = 0; i < 3; ++i) ring_teardown(&h->rings[i]);
    ring_teardown(&h->capture); g_free(h->snapshot);
    counters_clean();
}
/* One engine control-thread turn, in the engine's order: the snapshot (loss
 * detection happens on it), then the bounded route retry/reapply. */
static void harness_step(Harness *h)
{
    gint64 started = g_get_monotonic_time();
    radios_audio_backend_snapshot(h->backend, h->snapshot);
    g_assert_cmpint(g_get_monotonic_time() - started, <, 20000);
    radios_audio_backend_reconnect(h->backend);
}
/* The engine renderer: 480-frame tagged blocks while MAIN holds < 4 blocks. */
static void harness_render(Harness *h, gboolean tone)
{
    RadioSAudioRing *ring = &h->rings[0]; gfloat block[BLOCK * 2];
    while (atomic_load(&ring->write) - atomic_load(&ring->read) < BLOCK * 4) {
        for (guint i = 0; i < BLOCK; ++i)
            block[i * 2] = block[i * 2 + 1] = tone ? (gfloat)(TONE_AMPLITUDE * sin(2 * G_PI * TONE_HZ * (gdouble)(h->phase + i) / RATE)) : 0;
        g_assert_cmpuint(radios_audio_ring_write(ring, block, BLOCK, atomic_load(&h->next_tag)), ==, BLOCK);
        atomic_fetch_add(&h->next_tag, BLOCK); h->phase += BLOCK;
    }
}
/* Like the engine's "radios-render" worker: its own MMCSS thread polling every
 * millisecond, so a busy desktop cannot starve MAIN during the measurement. */
static gpointer harness_producer(gpointer data)
{
    Harness *h = data; gpointer priority = radios_wasapi_audio_priority_begin();
    while (!g_atomic_int_get(&h->producer_stop)) { harness_render(h, g_atomic_int_get(&h->producer_tone)); g_usleep(1000); }
    radios_wasapi_audio_priority_end(priority); return NULL;
}
static void harness_producer_start(Harness *h)
{
    g_atomic_int_set(&h->producer_stop, FALSE); g_atomic_int_set(&h->producer_tone, FALSE);
    h->producer = g_thread_new("test-render", harness_producer, h);
}
static void harness_producer_stop(Harness *h)
{
    g_atomic_int_set(&h->producer_stop, TRUE); g_thread_join(h->producer); h->producer = NULL;
}
static gboolean harness_until(Harness *h, gboolean (*done)(Harness *), gint64 timeout_us)
{
    gint64 deadline = g_get_monotonic_time() + timeout_us;
    do {
        harness_step(h);
        if (done(h)) return TRUE;
        g_usleep(5000);
    } while (g_get_monotonic_time() < deadline);
    return FALSE;
}
static gboolean connected_with_devices(Harness *h) { return h->snapshot->connected && h->snapshot->output_count > 0; }
static gboolean main_and_mic_ready(Harness *h) { return h->snapshot->ready[0] && h->snapshot->microphone_ready; }
static gboolean main_ready(Harness *h) { return h->snapshot->ready[0]; }
static void harness_connect(Harness *h)
{
    g_assert_true(harness_until(h, connected_with_devices, 5 * G_TIME_SPAN_SECOND));
    g_assert_cmpstr(h->snapshot->error, ==, "");
    gchar error[RADIOS_WASAPI_ERROR_SIZE] = ""; g_assert_true(radios_wasapi_thread_enter(error));
    RadioSWasapiEnumerator *enumerator = radios_wasapi_enumerator_new(error); g_assert_nonnull(enumerator);
    g_assert_true(radios_wasapi_enumerator_default(enumerator, RADIOS_WASAPI_FLOW_RENDER, h->default_output));
    radios_wasapi_enumerator_free(enumerator); radios_wasapi_thread_leave();
    gboolean listed = FALSE, monitor = FALSE; gchar loopback[RADIOS_AUDIO_DEVICE_NAME_SIZE];
    g_snprintf(loopback, sizeof loopback, "%s.monitor", h->default_output);
    for (guint i = 0; i < h->snapshot->output_count; ++i) {
        listed |= !strcmp(h->snapshot->outputs[i].name, h->default_output);
        g_assert_false(h->snapshot->outputs[i].monitor);
        if (i) g_assert_cmpint(g_utf8_collate(h->snapshot->outputs[i - 1].description, h->snapshot->outputs[i].description), <=, 0);
    }
    for (guint i = 0; i < h->snapshot->input_count; ++i)
        if (!strcmp(h->snapshot->inputs[i].name, loopback)) { monitor = TRUE; g_assert_true(h->snapshot->inputs[i].monitor); }
    g_assert_true(listed); g_assert_true(monitor);
}
/* MAIN on Windows' default output, the microphone on its loopback: the tone
 * written into the MAIN ring must come back through the capture ring. */
static void backend_tone(Harness *h, guint rate, guint buffer_frames, gdouble seconds)
{
    radios_audio_backend_configure_output(h->backend, rate, buffer_frames);
    g_strlcpy(h->preferences.main_sink, RADIOS_WASAPI_DEFAULT_ID, sizeof h->preferences.main_sink);
    g_snprintf(h->preferences.microphone_source, sizeof h->preferences.microphone_source, "%s.monitor", h->default_output);
    radios_audio_backend_apply(h->backend, &h->preferences);
    g_assert_true(harness_until(h, main_and_mic_ready, 5 * G_TIME_SPAN_SECOND));
    g_assert_cmpuint(h->snapshot->output_sample_rate[0], ==, rate);
    g_assert_cmpuint(h->snapshot->output_buffer_frames[0], >, 0);
    g_assert_cmpstr(h->snapshot->bus_error[0], ==, ""); g_assert_cmpstr(h->snapshot->microphone_error, ==, "");
    /* 300 ms of silence first: whatever an earlier case left in the
     * loopback path drains before the tone starts. */
    harness_producer_start(h);
    for (gint64 quiet = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND / 10; g_get_monotonic_time() < quiet;) {
        harness_step(h); g_usleep(2000);
        atomic_store(&h->capture.read, atomic_load(&h->capture.write));
    }
    gfloat *captured = g_new0(gfloat, (gsize)RATE * CAPTURE_SECONDS * 2); guint frames = 0;
    guint64 first_tag = atomic_load(&h->next_tag), previous_played = 0, gaps_before = h->snapshot->gap_frames[0];
    g_atomic_int_set(&h->producer_tone, TRUE);
    gint64 started = g_get_monotonic_time(), end = started + (gint64)(seconds * G_TIME_SPAN_SECOND);
    gfloat peak = 0;
    while (g_get_monotonic_time() < end) {
        frames += radios_audio_ring_read(&h->capture, captured + (gsize)frames * 2, RATE * CAPTURE_SECONDS - frames, NULL);
        harness_step(h);
        g_assert_true(h->snapshot->ready[0]);
        guint64 played = h->snapshot->played_tag[0];
        g_assert_cmpuint(played, >=, previous_played); g_assert_cmpuint(played, <, atomic_load(&h->next_tag));
        previous_played = played; peak = MAX(peak, h->snapshot->microphone_peak[0]);
        g_usleep(2000);
    }
    gint64 elapsed = g_get_monotonic_time() - started;
    guint64 gaps = h->snapshot->gap_frames[0] - gaps_before;   /* dropouts during the tone */
    /* drain what the loopback still holds */
    g_atomic_int_set(&h->producer_tone, FALSE);
    for (guint i = 0; i < 20; ++i) {
        g_usleep(5000);
        frames += radios_audio_ring_read(&h->capture, captured + (gsize)frames * 2, RATE * CAPTURE_SECONDS - frames, NULL);
    }
    harness_producer_stop(h);
    harness_step(h);
    guint64 consumed = h->snapshot->consumed_frames[0];
    g_test_message("%u Hz/%u: played tag %" G_GUINT64_FORMAT " of %" G_GUINT64_FORMAT "..%" G_GUINT64_FORMAT
        ", consumed %" G_GUINT64_FORMAT " frames in %" G_GINT64_FORMAT " us, buffer %u, underruns %" G_GUINT64_FORMAT
        ", gap frames %" G_GUINT64_FORMAT ", mic peak %.5f",
        rate, buffer_frames, h->snapshot->played_tag[0], first_tag, (guint64)atomic_load(&h->next_tag), consumed, elapsed,
        h->snapshot->output_buffer_frames[0], h->snapshot->underruns[0], gaps, peak);
    g_assert_cmpuint(h->snapshot->played_tag[0], >, first_tag);
    g_assert_cmpuint(consumed, >, (guint64)(elapsed * 48 / 1000) / 2);
    g_assert_cmpfloat(peak, >, 0.0003f);
    gchar *label = g_strdup_printf("MAIN at %u Hz through the output loopback", rate);
    assert_tone(captured, frames, (guint)(seconds * RATE), label); g_free(label); g_free(captured);
    /* An in-place flush keeps MAIN ready; played evidence restarts. */
    radios_audio_backend_flush(h->backend);
    radios_audio_backend_snapshot(h->backend, h->snapshot);
    g_assert_true(h->snapshot->ready[0]);
    g_assert_cmpuint(h->snapshot->consumed_frames[0], <, 4800);
    g_assert_cmpuint(atomic_load(&h->rings[0].write) - atomic_load(&h->rings[0].read), ==, 0);
}
static void test_backend_tone(void)
{
    if (!need_hardware()) return;
    Harness h; harness_open(&h); harness_connect(&h);
    backend_tone(&h, 48000, 2400, 1.0);
    /* 44.1 kHz: the RadioS resampler path with per-frame playback tags. */
    backend_tone(&h, 44100, 1024, 0.8);
    harness_close(&h);
}
static gboolean main_error(Harness *h) { return !h->snapshot->ready[0] && h->snapshot->bus_error[0][0]; }
static gboolean all_missing(Harness *h)
{
    return !h->snapshot->ready[0] && strstr(h->snapshot->bus_error[0], "bulunamadı") &&
        !h->snapshot->ready[1] && strstr(h->snapshot->bus_error[1], "bulunamadı") &&
        !h->snapshot->microphone_ready && strstr(h->snapshot->microphone_error, "bulunamadı");
}
static void test_backend_missing_devices(void)
{
    if (!need_hardware()) return;
    Harness h; harness_open(&h); harness_connect(&h);
    /* An unknown endpoint and a PulseAudio name kept from Linux preferences:
     * not found, name kept, never another device. */
    g_strlcpy(h.preferences.main_sink, "{0.0.0.00000000}.{00000000-dead-beef-0000-000000000000}", sizeof h.preferences.main_sink);
    g_strlcpy(h.preferences.cue_sink, "alsa_output.pci-0000_00_1f.3.analog-stereo", sizeof h.preferences.cue_sink);
    g_strlcpy(h.preferences.microphone_source, "alsa_input.usb-Focusrite_Scarlett.monitor", sizeof h.preferences.microphone_source);
    radios_audio_backend_apply(h.backend, &h.preferences);
    g_assert_true(harness_until(&h, all_missing, 3 * G_TIME_SPAN_SECOND));
    g_test_message("MAIN: %s | mic: %s", h.snapshot->bus_error[0], h.snapshot->microphone_error);
    g_assert_true(h.snapshot->connected); g_assert_false(h.snapshot->ready[2]); g_assert_cmpstr(h.snapshot->bus_error[2], ==, "");
    g_usleep(300000); harness_step(&h);
    g_assert_true(all_missing(&h));
    /* Back to a real device: the next apply re-creates the failed buses. */
    g_strlcpy(h.preferences.main_sink, RADIOS_WASAPI_DEFAULT_ID, sizeof h.preferences.main_sink);
    h.preferences.cue_sink[0] = h.preferences.microphone_source[0] = '\0';
    radios_audio_backend_apply(h.backend, &h.preferences);
    g_assert_true(harness_until(&h, main_ready, 5 * G_TIME_SPAN_SECOND));
    g_assert_cmpstr(h.snapshot->bus_error[1], ==, ""); g_assert_cmpstr(h.snapshot->microphone_error, ==, "");
    harness_close(&h);
}
static gboolean disconnected(Harness *h)
{
    return !h->snapshot->connected && !h->snapshot->ready[0] && !h->snapshot->output_count && strstr(h->snapshot->error, "Windows ses hizmeti");
}
static gboolean reconnected(Harness *h) { return h->snapshot->connected && h->snapshot->output_count && h->snapshot->ready[0]; }
static void test_backend_recovery(void)
{
    if (!need_hardware()) return;
    Harness h; harness_open(&h); harness_connect(&h);
    g_strlcpy(h.preferences.main_sink, RADIOS_WASAPI_DEFAULT_ID, sizeof h.preferences.main_sink);
    radios_audio_backend_apply(h.backend, &h.preferences);
    g_assert_true(harness_until(&h, main_ready, 5 * G_TIME_SPAN_SECOND));
    /* Invalidated stream, endpoint still active (format change, resume):
     * silent reopen, never "not ready with an error" (an engine loss). */
    radios_wasapi_debug_inject_fault(RADIOS_WASAPI_FAULT_INVALIDATED);
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND; gboolean dropped = FALSE;
    while (g_get_monotonic_time() < deadline) {
        harness_render(&h, FALSE); harness_step(&h);
        g_assert_false(main_error(&h));
        dropped |= !h.snapshot->ready[0];
        if (dropped && h.snapshot->ready[0]) break;
        g_usleep(1000);
    }
    g_test_message("silent reopen: %s", dropped ? "MAIN reopened" : "reopen faster than the snapshot");
    g_assert_true(h.snapshot->ready[0]); g_assert_cmpstr(h.snapshot->bus_error[0], ==, "");
    /* The device went away: loss with the Turkish reason, then the 5 s
     * route retry brings MAIN back by itself. */
    radios_wasapi_debug_inject_fault(RADIOS_WASAPI_FAULT_DEVICE_GONE);
    g_assert_true(harness_until(&h, main_error, 3 * G_TIME_SPAN_SECOND));
    g_test_message("loss: %s", h.snapshot->bus_error[0]);
    g_assert_nonnull(strstr(h.snapshot->bus_error[0], "çıkarıldı"));
    gint64 lost = g_get_monotonic_time();
    g_assert_true(harness_until(&h, main_ready, 8 * G_TIME_SPAN_SECOND));
    g_test_message("route retry restored MAIN after %" G_GINT64_FORMAT " ms", (g_get_monotonic_time() - lost) / 1000);
    g_assert_cmpstr(h.snapshot->bus_error[0], ==, "");
    /* Windows Audio stops for 1.5 s: disconnected, lists cleared; then the
     * bounded 1 s/2 s reconnect and the re-apply restore everything. */
    radios_wasapi_debug_service_outage(1500);
    g_assert_true(harness_until(&h, disconnected, 3 * G_TIME_SPAN_SECOND));
    g_test_message("outage: %s", h.snapshot->error);
    g_assert_true(harness_until(&h, reconnected, 8 * G_TIME_SPAN_SECOND));
    g_assert_cmpstr(h.snapshot->error, ==, "");
    harness_close(&h);
}
static void test_backend_hotplug_rescan(void)
{
    if (!need_hardware()) return;
    Harness h; harness_open(&h); harness_connect(&h);
    /* Preferences reads these on the GTK thread: no device work there. */
    g_assert_cmpstr(radios_audio_backend_display_name(), ==, "Windows ses (WASAPI)");
    gint64 asked = g_get_monotonic_time(); guint highest = radios_audio_backend_max_sample_rate();
    g_test_message("highest output rate %u Hz (answered in %" G_GINT64_FORMAT " us)", highest, g_get_monotonic_time() - asked);
    g_assert_cmpuint(highest, >=, 44100); g_assert_cmpuint(highest, <=, 768000);
    guint64 scans = h.snapshot->device_scans; guint outputs = h.snapshot->output_count;
    /* "Aygıtları yenile": a full rescan; the published list never shrinks
     * meanwhile (atomic publish). */
    radios_audio_backend_refresh(h.backend);
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline && h.snapshot->device_scans == scans) {
        harness_step(&h); g_assert_cmpuint(h.snapshot->output_count, ==, outputs); g_usleep(1000);
    }
    g_assert_cmpuint(h.snapshot->device_scans, >, scans);
    for (guint i = 0; i < 20; ++i) { harness_step(&h); g_assert_cmpuint(h.snapshot->output_count, ==, outputs); g_usleep(5000); }
    harness_close(&h);
}

/* ------------------------------------------------------------------ backend core, fake provider */

/* A deterministic in-process "audio service" behind the provider seam: the
 * device lists, a per-device scan delay that honours cancellation, hotplug
 * notifications and the service state are driven by the test. No hardware. */
static struct {
    GMutex lock;
    guint sinks, sources, item_delay_us, scans;
    gchar sink_prefix[16], source_prefix[16];
    gboolean running;
    RadioSAudioHotplugSink sink;
} fake;
static RadioSAudioProvider *fake_create(const RadioSAudioHotplugSink *sink, gpointer config)
{ (void)config; g_mutex_lock(&fake.lock); fake.sink = *sink; g_mutex_unlock(&fake.lock); return (RadioSAudioProvider *)&fake; }
static void fake_destroy(RadioSAudioProvider *provider) { (void)provider; }
static gpointer fake_thread_enter(gboolean realtime) { (void)realtime; return NULL; }
static void fake_thread_leave(gpointer cookie) { (void)cookie; }
static RadioSAudioIoStatus fake_connect(RadioSAudioProvider *provider, gchar error[256])
{ (void)provider; (void)error; return RADIOS_AUDIO_IO_OK; }
static void fake_disconnect(RadioSAudioProvider *provider) { (void)provider; }
static gboolean fake_service_running(RadioSAudioProvider *provider)
{ (void)provider; g_mutex_lock(&fake.lock); gboolean running = fake.running; g_mutex_unlock(&fake.lock); return running; }
static RadioSAudioIoStatus fake_scan(RadioSAudioProvider *provider, RadioSAudioDevice outputs[64], guint *output_count,
    RadioSAudioDevice inputs[64], guint *input_count, gboolean (*cancelled)(gpointer data), gpointer data)
{
    (void)provider; *output_count = *input_count = 0;
    g_mutex_lock(&fake.lock);
    guint sinks = fake.sinks, sources = fake.sources, delay = fake.item_delay_us; ++fake.scans;
    gchar sink_prefix[16], source_prefix[16];
    g_strlcpy(sink_prefix, fake.sink_prefix, sizeof sink_prefix); g_strlcpy(source_prefix, fake.source_prefix, sizeof source_prefix);
    g_mutex_unlock(&fake.lock);
    for (guint i = 0; i < sinks + sources; ++i) {
        if (cancelled(data)) return RADIOS_AUDIO_IO_OK;
        if (delay) g_usleep(delay);
        RadioSAudioDevice *device = i < sinks ? &outputs[(*output_count)++] : &inputs[(*input_count)++];
        guint index = i < sinks ? i : i - sinks;
        g_snprintf(device->name, sizeof device->name, "%s-%u", i < sinks ? sink_prefix : source_prefix, index);
        g_snprintf(device->description, sizeof device->description, "Sahte aygıt %u", index);
        device->channels = 2; device->monitor = i >= sinks && index % 2;
    }
    return RADIOS_AUDIO_IO_OK;
}
static RadioSAudioIoStatus fake_open(RadioSAudioProvider *provider, gboolean capture, const gchar *device, guint rate,
    guint buffer_frames, guint slot, RadioSAudioProviderStream **stream, RadioSAudioStreamInfo *info, gchar error[256])
{
    (void)provider; (void)capture; (void)device; (void)rate; (void)buffer_frames; (void)slot; (void)info;
    *stream = NULL; g_strlcpy(error, "Sahte sunucuda akış yok.", 256); return RADIOS_AUDIO_IO_FAILED;
}
static gboolean fake_wait(RadioSAudioProviderStream *stream, gpointer wake, guint timeout_ms)
{ (void)stream; (void)radios_wasapi_wait(NULL, wake, timeout_ms); return FALSE; }
static RadioSAudioIoStatus fake_queued(RadioSAudioProviderStream *stream, guint *frames) { (void)stream; *frames = 0; return RADIOS_AUDIO_IO_FAILED; }
static RadioSAudioIoStatus fake_write(RadioSAudioProviderStream *stream, const gfloat *stereo, guint frames)
{ (void)stream; (void)stereo; (void)frames; return RADIOS_AUDIO_IO_FAILED; }
static RadioSAudioIoStatus fake_read(RadioSAudioProviderStream *stream, gfloat *stereo, guint capacity, guint *frames, gboolean *silent)
{ (void)stream; (void)stereo; (void)capacity; *frames = 0; *silent = FALSE; return RADIOS_AUDIO_IO_FAILED; }
static RadioSAudioIoStatus fake_position(RadioSAudioProviderStream *stream, guint64 *frames) { (void)stream; *frames = 0; return RADIOS_AUDIO_IO_FAILED; }
static RadioSAudioIoStatus fake_control(RadioSAudioProviderStream *stream) { (void)stream; return RADIOS_AUDIO_IO_FAILED; }
static const gchar *fake_error(RadioSAudioProviderStream *stream) { (void)stream; return "Sahte sunucuda akış yok."; }
static void fake_close(RadioSAudioProviderStream *stream) { (void)stream; }
static const RadioSAudioProviderOps fake_ops = {
    "Sahte ses sunucusu", fake_create, fake_destroy, fake_thread_enter, fake_thread_leave,
    fake_connect, fake_disconnect, fake_service_running, fake_scan, NULL,
    fake_open, fake_wait, fake_queued, fake_write, fake_read, fake_position,
    fake_control, fake_control, fake_control, NULL, fake_error, fake_close
};
static void fake_reset(guint sinks, guint sources, guint delay_us)
{
    g_mutex_lock(&fake.lock);
    fake.sinks = sinks; fake.sources = sources; fake.item_delay_us = delay_us; fake.scans = 0; fake.running = TRUE;
    g_strlcpy(fake.sink_prefix, "sink", sizeof fake.sink_prefix); g_strlcpy(fake.source_prefix, "source", sizeof fake.source_prefix);
    g_mutex_unlock(&fake.lock);
}
static void fake_set(guint *field, guint value) { g_mutex_lock(&fake.lock); *field = value; g_mutex_unlock(&fake.lock); }
static guint fake_scans(void) { g_mutex_lock(&fake.lock); guint scans = fake.scans; g_mutex_unlock(&fake.lock); return scans; }
static void fake_emit(RadioSAudioHotplugKind kind) { fake.sink.notify(fake.sink.core, kind); }

typedef struct {
    RadioSAudioRing rings[3], capture;
    RadioSAudioRing *outputs[3];
    RadioSAudioBackend *backend;
    RadioSAudioBackendSnapshot *snapshot;
    gboolean seen_outputs[65], seen_inputs[65];
    gint64 max_snapshot_us;
} Core;
static void core_open(Core *c)
{
    memset(c, 0, sizeof *c);
    for (guint i = 0; i < 3; ++i) { ring_setup(&c->rings[i], TRUE); c->outputs[i] = &c->rings[i]; }
    ring_setup(&c->capture, FALSE); c->snapshot = g_new0(RadioSAudioBackendSnapshot, 1);
    c->backend = radios_audio_backend_new_with_provider(c->outputs, &c->capture, &fake_ops, NULL);
    g_assert_cmpstr(radios_audio_backend_label(c->backend), ==, "Sahte ses sunucusu");
    RadioSAudioPreferences preferences; memset(&preferences, 0, sizeof preferences);   /* no buses here */
    radios_audio_backend_apply(c->backend, &preferences);
}
static void core_close(Core *c)
{
    radios_audio_backend_free(c->backend);
    for (guint i = 0; i < 3; ++i) ring_teardown(&c->rings[i]);
    ring_teardown(&c->capture); g_free(c->snapshot);
}
static void core_step(Core *c)
{
    gint64 started = g_get_monotonic_time();
    radios_audio_backend_snapshot(c->backend, c->snapshot);
    c->max_snapshot_us = MAX(c->max_snapshot_us, g_get_monotonic_time() - started);
    radios_audio_backend_reconnect(c->backend);
    c->seen_outputs[MIN(c->snapshot->output_count, 64u)] = TRUE; c->seen_inputs[MIN(c->snapshot->input_count, 64u)] = TRUE;
}
static gboolean core_until(Core *c, guint outputs, guint inputs, gint64 timeout_us)
{
    gint64 deadline = g_get_monotonic_time() + timeout_us;
    do {
        core_step(c);
        if (c->snapshot->connected && c->snapshot->output_count == outputs && c->snapshot->input_count == inputs) return TRUE;
        g_usleep(2000);
    } while (g_get_monotonic_time() < deadline);
    g_test_message("timeout: connected=%d outputs=%u inputs=%u", c->snapshot->connected, c->snapshot->output_count, c->snapshot->input_count);
    return FALSE;
}
static void core_settle(Core *c, gint64 timeout_us)
{
    gint64 until = g_get_monotonic_time() + timeout_us;
    while (g_get_monotonic_time() < until) { core_step(c); g_usleep(2000); }
}
static void assert_only_seen(const gboolean *seen, guint a, guint b)
{
    for (guint i = 0; i <= 64; ++i)
        if (i != a && i != b && seen[i]) g_error("a partial device list of %u entries was published (allowed: %u or %u)", i, a, b);
}
/* While the lists are re-read, snapshots show the previous complete list or
 * the new complete one, never a partial one; a rescan started during another
 * restarts it. */
static void test_core_lists_publish_atomically(void)
{
    fake_reset(6, 4, 15000);
    Core c; core_open(&c);
    g_assert_true(core_until(&c, 6, 4, 5 * G_TIME_SPAN_SECOND));
    assert_only_seen(c.seen_outputs, 0, 6); assert_only_seen(c.seen_inputs, 0, 4);
    g_assert_cmpstr(c.snapshot->outputs[5].name, ==, "sink-5"); g_assert_cmpstr(c.snapshot->inputs[3].name, ==, "source-3");
    g_assert_true(c.snapshot->inputs[1].monitor); g_assert_false(c.snapshot->inputs[0].monitor);
    memset(c.seen_outputs, 0, sizeof c.seen_outputs); memset(c.seen_inputs, 0, sizeof c.seen_inputs);
    fake_set(&fake.sinks, 8); fake_set(&fake.sources, 3);
    radios_audio_backend_refresh(c.backend);
    g_assert_true(core_until(&c, 8, 3, 5 * G_TIME_SPAN_SECOND));
    assert_only_seen(c.seen_outputs, 6, 8); assert_only_seen(c.seen_inputs, 4, 3);
    memset(c.seen_outputs, 0, sizeof c.seen_outputs);
    fake_set(&fake.sinks, 5); radios_audio_backend_refresh(c.backend); g_usleep(20000);
    fake_set(&fake.sinks, 7); radios_audio_backend_refresh(c.backend);
    g_assert_true(core_until(&c, 7, 3, 5 * G_TIME_SPAN_SECOND));
    for (guint i = 0; i <= 64; ++i) if (c.seen_outputs[i] && i != 8 && i != 7 && i != 5)
        g_error("a partial device list of %u entries was published during overlapping rescans", i);
    g_test_message("snapshot max %" G_GINT64_FORMAT " us", c.max_snapshot_us);
    g_assert_cmpint(c.max_snapshot_us, <, 20000);
    core_close(&c);
}
/* A plugged or removed device is followed by itself; a burst of events is one
 * scan 250 ms after the last; ignored events (volume, other roles) scan nothing. */
static void test_core_hotplug_followed_without_refresh(void)
{
    fake_reset(3, 2, 1000);
    Core c; core_open(&c);
    g_assert_true(core_until(&c, 3, 2, 5 * G_TIME_SPAN_SECOND)); core_settle(&c, 60000);
    memset(c.seen_outputs, 0, sizeof c.seen_outputs);
    guint scans = fake_scans();
    fake_set(&fake.sinks, 4); gint64 plugged = g_get_monotonic_time();
    fake_emit(RADIOS_AUDIO_HOTPLUG_DEVICES);
    g_assert_true(core_until(&c, 4, 2, 3 * G_TIME_SPAN_SECOND));
    gint64 latency = g_get_monotonic_time() - plugged;
    g_test_message("hotplug visible after %" G_GINT64_FORMAT " us", latency);
    g_assert_cmpint(latency, >=, 200000); g_assert_cmpint(latency, <, 2000000);
    g_assert_cmpuint(fake_scans(), ==, scans + 1);
    assert_only_seen(c.seen_outputs, 3, 4); memset(c.seen_outputs, 0, sizeof c.seen_outputs);
    scans = fake_scans(); fake_set(&fake.sinks, 10);
    for (guint i = 0; i < 50; ++i) fake_emit(i % 3 ? RADIOS_AUDIO_HOTPLUG_DEVICES : RADIOS_AUDIO_HOTPLUG_DEFAULT);
    g_assert_true(core_until(&c, 10, 2, 3 * G_TIME_SPAN_SECOND));
    g_assert_cmpuint(fake_scans(), ==, scans + 1);
    assert_only_seen(c.seen_outputs, 4, 10); memset(c.seen_outputs, 0, sizeof c.seen_outputs);
    fake_set(&fake.sinks, 9); fake_emit(RADIOS_AUDIO_HOTPLUG_DEVICES);
    g_assert_true(core_until(&c, 9, 2, 3 * G_TIME_SPAN_SECOND));
    assert_only_seen(c.seen_outputs, 10, 9);
    fake_set(&fake.sources, 3); fake_emit(RADIOS_AUDIO_HOTPLUG_DEVICES);
    g_assert_true(core_until(&c, 9, 3, 3 * G_TIME_SPAN_SECOND));
    scans = fake_scans();
    for (guint i = 0; i < 20; ++i) fake_emit(RADIOS_AUDIO_HOTPLUG_IGNORED);
    core_settle(&c, 600000);
    g_assert_cmpuint(fake_scans(), ==, scans);
    g_assert_cmpuint(c.snapshot->output_count, ==, 9); g_assert_cmpuint(c.snapshot->input_count, ==, 3);
    g_assert_cmpint(c.max_snapshot_us, <, 20000);
    core_close(&c);
}
/* Full inventories (64 + 64 devices) still snapshot quickly during rescans. */
static void test_core_snapshot_fast_with_full_inventory(void)
{
    fake_reset(64, 64, 0);
    Core c; core_open(&c);
    g_assert_true(core_until(&c, 64, 64, 5 * G_TIME_SPAN_SECOND));
    c.max_snapshot_us = 0; fake_set(&fake.item_delay_us, 5000);
    for (guint round = 0; round < 200; ++round) {
        if (round % 40 == 0) fake_emit(RADIOS_AUDIO_HOTPLUG_DEVICES);
        core_step(&c);
        g_assert_true(c.snapshot->output_count == 64 && c.snapshot->input_count == 64);
        g_usleep(1000);
    }
    g_test_message("snapshot max %" G_GINT64_FORMAT " us with 128 devices under rescans", c.max_snapshot_us);
    g_assert_cmpint(c.max_snapshot_us, <, 20000);
    core_close(&c);
}
/* The audio service stops: disconnected, lists cleared, Turkish reason; it
 * comes back: the bounded retry reconnects and rescans by itself. */
static void test_core_service_loss_and_reconnect(void)
{
    fake_reset(2, 2, 0);
    Core c; core_open(&c);
    g_assert_true(core_until(&c, 2, 2, 5 * G_TIME_SPAN_SECOND));
    g_mutex_lock(&fake.lock); fake.running = FALSE; g_mutex_unlock(&fake.lock);
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    do { core_step(&c); g_usleep(5000); } while (c.snapshot->connected && g_get_monotonic_time() < deadline);
    g_assert_false(c.snapshot->connected);
    g_assert_cmpuint(c.snapshot->output_count, ==, 0); g_assert_cmpuint(c.snapshot->input_count, ==, 0);
    g_assert_nonnull(strstr(c.snapshot->error, "Windows ses hizmeti"));
    g_mutex_lock(&fake.lock); fake.running = TRUE; fake.sinks = 3; g_mutex_unlock(&fake.lock);
    gint64 back = g_get_monotonic_time();
    g_assert_true(core_until(&c, 3, 2, 6 * G_TIME_SPAN_SECOND));
    g_test_message("reconnected after %" G_GINT64_FORMAT " ms", (g_get_monotonic_time() - back) / 1000);
    g_assert_cmpstr(c.snapshot->error, ==, "");
    core_close(&c);
}
/* An unknown locator never touches devices and says why. */
static void test_core_unsupported_locator(void)
{
    gchar reason[256] = "x";
    g_assert_true(radios_audio_server_resolve("wasapi", reason)); g_assert_cmpstr(reason, ==, "");
    g_assert_true(radios_audio_server_resolve("WASAPI:", NULL));
    g_assert_false(radios_audio_server_resolve("tcp:studio-pc", reason));
    g_assert_cmpstr(reason, ==, "Bu ses sunucusu adresi Windows'ta desteklenmiyor: tcp:studio-pc");
    gchar *saved = g_strdup(g_getenv("RADIOS_AUDIO_SERVER"));
    g_assert_true(g_setenv("RADIOS_AUDIO_SERVER", "unix:/tmp/pulse", TRUE));
    g_assert_false(radios_audio_server_resolve(NULL, reason)); g_assert_nonnull(strstr(reason, "unix:/tmp/pulse"));
    g_assert_true(radios_audio_server_resolve("wasapi", NULL));   /* an explicit locator wins */
    if (saved) g_setenv("RADIOS_AUDIO_SERVER", saved, TRUE); else g_unsetenv("RADIOS_AUDIO_SERVER");
    g_free(saved);
    RadioSAudioRing rings[3], capture; RadioSAudioRing *outputs[3];
    for (guint i = 0; i < 3; ++i) { ring_setup(&rings[i], TRUE); outputs[i] = &rings[i]; }
    ring_setup(&capture, FALSE);
    RadioSAudioBackend *backend = radios_audio_backend_new(outputs, &capture, "unix:/run/user/1000/pulse/native");
    RadioSAudioBackendSnapshot *snapshot = g_new0(RadioSAudioBackendSnapshot, 1);
    RadioSAudioPreferences preferences; radios_audio_preferences_default(&preferences);
    radios_audio_backend_apply(backend, &preferences); radios_audio_backend_reconnect(backend);
    radios_audio_backend_flush(backend); radios_audio_backend_refresh(backend);
    radios_audio_backend_snapshot(backend, snapshot);
    g_assert_false(snapshot->connected); g_assert_false(snapshot->ready[0]);
    g_assert_nonnull(strstr(snapshot->error, "Windows'ta desteklenmiyor: unix:/run/user/1000/pulse/native"));
    radios_audio_backend_free(backend); g_free(snapshot);
    for (guint i = 0; i < 3; ++i) ring_teardown(&rings[i]);
    ring_teardown(&capture);
}

/* ------------------------------------------------------------------ engine */

static gboolean engine_until(RadioSAudioEngine *engine, RadioSAudioSnapshot *snapshot,
    gboolean (*done)(const RadioSAudioSnapshot *), gint64 timeout_us)
{
    gint64 deadline = g_get_monotonic_time() + timeout_us;
    do {
        radios_audio_engine_snapshot(engine, snapshot);
        if (done(snapshot)) return TRUE;
        g_usleep(10000);
    } while (g_get_monotonic_time() < deadline);
    return FALSE;
}
static gboolean engine_ready(const RadioSAudioSnapshot *s) { return s->connected && s->buses[0].ready && s->microphone_ready; }
static gboolean engine_missing(const RadioSAudioSnapshot *s) { return !s->buses[0].ready && strstr(s->buses[0].error, "bulunamadı"); }
static gboolean engine_main(const RadioSAudioSnapshot *s) { return s->buses[0].ready; }
static void test_engine_realtime(void)
{
    if (!need_hardware()) return;
    RadioSAudioEngine *engine = radios_audio_engine_new(TRUE, NULL);
    RadioSAudioSnapshot *snapshot = g_new0(RadioSAudioSnapshot, 1);
    RadioSAudioPreferences preferences; radios_audio_preferences_default(&preferences);
    gchar error[RADIOS_WASAPI_ERROR_SIZE] = "", output[RADIOS_WASAPI_ID_SIZE] = "";
    g_assert_true(radios_wasapi_thread_enter(error));
    RadioSWasapiEnumerator *enumerator = radios_wasapi_enumerator_new(error); g_assert_nonnull(enumerator);
    g_assert_true(radios_wasapi_enumerator_default(enumerator, RADIOS_WASAPI_FLOW_RENDER, output));
    radios_wasapi_enumerator_free(enumerator); radios_wasapi_thread_leave();
    g_snprintf(preferences.microphone_source, sizeof preferences.microphone_source, "%s.monitor", output);
    GError *failure = NULL;
    g_assert_true(radios_audio_engine_apply_preferences(engine, &preferences, &failure)); g_assert_no_error(failure);
    g_assert_true(engine_until(engine, snapshot, engine_ready, 5 * G_TIME_SPAN_SECOND));
    g_assert_cmpuint(snapshot->buses[0].output_sample_rate, ==, 48000);
    g_assert_cmpuint(snapshot->buses[0].output_buffer_frames, >, 0);
    g_assert_cmpuint(snapshot->output_count, >, 0);
    /* The engine renders silence on MAIN; the device clock advances it. */
    guint64 before = snapshot->buses[0].consumed_frames; gint64 start = g_get_monotonic_time();
    g_usleep(400000); radios_audio_engine_snapshot(engine, snapshot);
    guint64 advanced = snapshot->buses[0].consumed_frames - before; gint64 elapsed = g_get_monotonic_time() - start;
    g_test_message("engine MAIN consumed %" G_GUINT64_FORMAT " frames in %" G_GINT64_FORMAT " us", advanced, elapsed);
    g_assert_cmpuint(advanced, >, (guint64)(elapsed * 48 / 1000) * 7 / 10);
    g_assert_cmpuint(advanced, <, (guint64)(elapsed * 48 / 1000) * 13 / 10 + 4800);
    guint64 losses = snapshot->loss_epoch;
    g_strlcpy(preferences.main_sink, "{0.0.0.00000000}.{00000000-dead-beef-0000-000000000000}", sizeof preferences.main_sink);
    g_assert_true(radios_audio_engine_apply_preferences(engine, &preferences, NULL));
    g_assert_true(engine_until(engine, snapshot, engine_missing, 3 * G_TIME_SPAN_SECOND));
    g_test_message("engine MAIN: %s (loss epoch %" G_GUINT64_FORMAT " -> %" G_GUINT64_FORMAT ")",
        snapshot->buses[0].error, losses, snapshot->loss_epoch);
    g_assert_false(snapshot->microphone_gate);
    g_strlcpy(preferences.main_sink, "@default", sizeof preferences.main_sink);
    g_assert_true(radios_audio_engine_apply_preferences(engine, &preferences, NULL));
    g_assert_true(engine_until(engine, snapshot, engine_main, 5 * G_TIME_SPAN_SECOND));
    g_assert_cmpstr(snapshot->buses[0].error, ==, "");
    radios_audio_engine_unref(engine); g_free(snapshot);
    counters_clean();
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    hardware = g_strcmp0(g_getenv("RADIOS_TEST_WASAPI"), "1") == 0 && radios_wasapi_service_running();
    if (hardware) {
        gchar error[RADIOS_WASAPI_ERROR_SIZE] = "", id[RADIOS_WASAPI_ID_SIZE] = "";
        if (radios_wasapi_thread_enter(error)) {
            RadioSWasapiEnumerator *enumerator = radios_wasapi_enumerator_new(error);
            hardware = enumerator && radios_wasapi_enumerator_default(enumerator, RADIOS_WASAPI_FLOW_RENDER, id);
            radios_wasapi_enumerator_free(enumerator); radios_wasapi_thread_leave();
        } else hardware = FALSE;
    }
    radios_wasapi_realtime_acquire();
    g_mutex_init(&fake.lock);
    g_test_add_func("/wasapi/com-objects", test_com_objects);
    g_test_add_func("/wasapi/watch-lifecycle", test_watch_lifecycle);
    g_test_add_func("/audio-backend/lists-publish-atomically", test_core_lists_publish_atomically);
    g_test_add_func("/audio-backend/hotplug-followed-without-refresh", test_core_hotplug_followed_without_refresh);
    g_test_add_func("/audio-backend/snapshot-fast-with-full-inventory", test_core_snapshot_fast_with_full_inventory);
    g_test_add_func("/audio-backend/service-loss-and-reconnect", test_core_service_loss_and_reconnect);
    g_test_add_func("/audio-backend/unsupported-locator", test_core_unsupported_locator);
    g_test_add_func("/wasapi/enumerate", test_enumerate);
    g_test_add_func("/wasapi/process-loopback", test_process_loopback);
    g_test_add_func("/wasapi/sessions", test_sessions);
    g_test_add_func("/wasapi/microphone-open-close", test_microphone);
    g_test_add_func("/wasapi-backend/tone-through-loopback", test_backend_tone);
    g_test_add_func("/wasapi-backend/missing-devices", test_backend_missing_devices);
    g_test_add_func("/wasapi-backend/reopen-loss-outage", test_backend_recovery);
    g_test_add_func("/wasapi-backend/rescan", test_backend_hotplug_rescan);
    g_test_add_func("/wasapi-engine/realtime", test_engine_realtime);
    gint result = g_test_run();
    radios_wasapi_realtime_release();
    return result;
}
