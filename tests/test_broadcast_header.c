#include "radios/broadcast_header.h"
#include "radios/audio_ui.h"
#include "radios/audio.h"
#include "radios/playout_ui.h"

#include <math.h>
#include <string.h>

/* Exercise the production selector. Only observations from the engine and
 * playout controllers are wrapped; the selector and its filtering are real.
 * No GTK window, output device, credentials or database is required. */
static RadioSViewContext context;
static RadioSAudioSnapshot raw;
static RadioSAudioUiSnapshot audio[RADIOS_AUDIO_NORMAL_DECKS];
static RadioSPlayoutDeckMetadata metadata[RADIOS_AUDIO_NORMAL_DECKS];
static gboolean audio_available[RADIOS_AUDIO_NORMAL_DECKS];
static gboolean metadata_available[RADIOS_AUDIO_NORMAL_DECKS];
static RadioSPlayoutSnapshot plan;
static gboolean engine_available, plan_available;
static RadioSPlayoutHeaderSnapshot programmes[3];
static guint programme_count, programme_reads, engine_token, acquisitions, releases;

static RadioSAudioEngine *
fake_engine(void)
{
    return (RadioSAudioEngine *)(gpointer)&engine_token;
}

RadioSAudioEngine *
__wrap_radios_audio_ui_engine_acquire(RadioSViewContext *ctx)
{
    g_assert_true(ctx == &context);
    ++acquisitions;
    return engine_available ? fake_engine() : NULL;
}

void
__wrap_radios_audio_engine_snapshot(RadioSAudioEngine *engine, RadioSAudioSnapshot *out)
{
    g_assert_true(engine == fake_engine());
    *out = raw;
}

void
__wrap_radios_audio_engine_unref(RadioSAudioEngine *engine)
{
    g_assert_true(engine == fake_engine());
    ++releases;
}

gboolean
__wrap_radios_audio_ui_get_snapshot(RadioSViewContext *ctx, guint deck, RadioSAudioUiSnapshot *out)
{
    g_assert_true(ctx == &context);
    g_assert_cmpuint(deck, <, G_N_ELEMENTS(audio));
    *out = audio[deck];
    return audio_available[deck];
}

gboolean
__wrap_radios_playout_ui_header_snapshot(RadioSViewContext *ctx, RadioSPlayoutHeaderSnapshot *out)
{
    g_assert_true(ctx == &context);
    *out = programmes[MIN(programme_reads, programme_count - 1)];
    ++programme_reads;
    return TRUE;
}

gboolean
__wrap_radios_playout_ui_get_snapshot(RadioSViewContext *ctx, RadioSPlayoutSnapshot *out)
{
    g_assert_true(ctx == &context);
    *out = plan;
    return plan_available;
}

gboolean
__wrap_radios_playout_ui_deck_metadata(RadioSViewContext *ctx, guint deck, RadioSPlayoutDeckMetadata *out)
{
    g_assert_true(ctx == &context);
    g_assert_cmpuint(deck, <, G_N_ELEMENTS(metadata));
    memset(out, 0, sizeof *out);
    if (metadata_available[deck]) *out = metadata[deck];
    return metadata_available[deck];
}

static void
reset(void)
{
    memset(&context, 0, sizeof context);
    memset(&raw, 0, sizeof raw);
    memset(audio, 0, sizeof audio);
    memset(metadata, 0, sizeof metadata);
    memset(audio_available, 0, sizeof audio_available);
    memset(metadata_available, 0, sizeof metadata_available);
    memset(&plan, 0, sizeof plan);
    memset(programmes, 0, sizeof programmes);
    for (guint channel = 0; channel < G_N_ELEMENTS(raw.mixer.channels); ++channel)
        raw.mixer.channels[channel].source_deck = -1;
    raw.buses[RADIOS_AUDIO_BUS_MAIN].ready = TRUE;
    raw.buses[RADIOS_AUDIO_BUS_MAIN].gate = TRUE;
    engine_available = TRUE;
    plan_available = FALSE;
    programme_count = 1;
    programme_reads = acquisitions = releases = 0;
    plan.deck = -1;
}

static void
observe(guint deck, RadioSAudioState state, const gchar *title)
{
    raw.mixer.channels[deck].source_deck = (gint)deck;
    raw.decks[deck].state = state;
    raw.decks[deck].main_routed = TRUE;
    raw.decks[deck].output_bus = RADIOS_AUDIO_BUS_MAIN;
    audio_available[deck] = TRUE;
    audio[deck].state = (RadioSAudioUiState)state;
    audio[deck].main_enabled = TRUE;
    audio[deck].output_ready = TRUE;
    audio[deck].duration_ms = 180000;
    audio[deck].position_ms = 30000;
    audio[deck].tempo_ratio = 1.0;
    g_strlcpy(audio[deck].title, title, sizeof audio[deck].title);
    g_strlcpy(audio[deck].artist, "Gerçek sanatçı", sizeof audio[deck].artist);
}

static void
assert_empty(const RadioSBroadcastHeaderSnapshot *value)
{
    g_assert_false(value->current);
    g_assert_false(value->paused);
    g_assert_cmpuint(value->active_sources, ==, 0);
    g_assert_cmpstr(value->title, ==, "");
    g_assert_cmpstr(value->artist, ==, "");
    g_assert_cmpuint(value->position_ms, ==, 0);
    g_assert_cmpuint(value->duration_ms, ==, 0);
    g_assert_cmpfloat(value->tempo_ratio, ==, 1.0);
    g_assert_null(value->card);
}

typedef enum {
    FILTER_CUE_DRIVER, FILTER_MAIN_GATE, FILTER_MAIN_DEVICE, FILTER_UNMAPPED,
    FILTER_NOT_MAIN_ROUTED, FILTER_MISSING_UI_OBSERVATION, FILTER_READY,
    FILTER_LOADING, FILTER_FINISHED, FILTER_ENGINE_ABSENT,
    FILTER_GENERATION_CHANGED, FILTER_STATE_CHANGED
} Filter;

static void
rejects_non_main(gconstpointer data)
{
    reset();
    observe(0, RADIOS_AUDIO_PLAYING, "Yayın diye gösterilmemeli");
    switch (GPOINTER_TO_UINT(data)) {
    case FILTER_CUE_DRIVER: raw.decks[0].output_bus = RADIOS_AUDIO_BUS_CUE; break;
    case FILTER_MAIN_GATE: raw.buses[RADIOS_AUDIO_BUS_MAIN].gate = FALSE; break;
    case FILTER_MAIN_DEVICE: raw.buses[RADIOS_AUDIO_BUS_MAIN].ready = FALSE; break;
    case FILTER_UNMAPPED: raw.mixer.channels[0].source_deck = -1; break;
    case FILTER_NOT_MAIN_ROUTED: raw.decks[0].main_routed = FALSE; break;
    case FILTER_MISSING_UI_OBSERVATION: audio_available[0] = FALSE; break;
    case FILTER_READY: raw.decks[0].state = RADIOS_AUDIO_READY; break;
    case FILTER_LOADING: raw.decks[0].state = RADIOS_AUDIO_LOADING; break;
    case FILTER_FINISHED: raw.decks[0].state = RADIOS_AUDIO_EOF; break;
    case FILTER_ENGINE_ABSENT: engine_available = FALSE; break;
    case FILTER_GENERATION_CHANGED:
        raw.decks[0].generation = 7;
        audio[0].generation = 8;
        break;
    case FILTER_STATE_CHANGED: audio[0].state = RADIOS_AUDIO_UI_EOF; break;
    default: g_assert_not_reached();
    }
    RadioSBroadcastHeaderSnapshot value;
    memset(&value, 0xa5, sizeof value);
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    assert_empty(&value);
    g_assert_cmpuint(acquisitions, ==, 1);
    g_assert_cmpuint(releases, ==, engine_available ? 1 : 0);
}

static void
main_playing(void)
{
    reset();
    observe(3, RADIOS_AUDIO_PLAYING, "Asıl MAIN parçası");
    RadioSBroadcastHeaderSnapshot value;
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    g_assert_true(value.current);
    g_assert_false(value.paused);
    g_assert_cmpuint(value.active_sources, ==, 1);
    g_assert_cmpstr(value.title, ==, "Asıl MAIN parçası");
    g_assert_cmpstr(value.artist, ==, "Gerçek sanatçı");
    g_assert_cmpuint(value.duration_ms, ==, 180000);
    g_assert_cmpuint(value.position_ms, ==, 30000);
    g_assert_cmpuint(releases, ==, acquisitions);
}

static void
manual_source_uses_actual_cue_out(void)
{
    reset();
    observe(3, RADIOS_AUDIO_PLAYING, "Elle başlatılan işaretli parça");
    raw.decks[3].cue_in_frame = 10000 * 48;
    raw.decks[3].cue_out_frame = 90000 * 48;
    audio[3].cue_in_ms = 10000;
    audio[3].cue_out_ms = 90000;
    RadioSBroadcastHeaderSnapshot value;
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    g_assert_true(value.current);
    g_assert_cmpstr(value.title, ==, "Elle başlatılan işaretli parça");
    g_assert_cmpuint(value.position_ms, ==, 30000);
    g_assert_cmpuint(value.duration_ms, ==, 90000);
    g_assert_cmpuint(value.duration_ms - value.position_ms, ==, 60000);
}

static void
retiring_owned_source_uses_actual_cue_out(void)
{
    reset();
    observe(0, RADIOS_AUDIO_PLAYING, "Eski deck başlığı");
    raw.decks[0].cue_in_frame = 10000 * 48;
    raw.decks[0].cue_out_frame = 90000 * 48;
    audio[0].cue_in_ms = 10000;
    audio[0].cue_out_ms = 90000;
    metadata_available[0] = TRUE;
    metadata[0].duration_ms = 180000;
    g_strlcpy(metadata[0].title, "Doğrulanmış çıkan program parçası", sizeof metadata[0].title);
    plan_available = TRUE;
    plan.deck = 1; /* Retiring source remains owned, but is not the current plan deck. */
    plan.state = RADIOS_PLAYOUT_OVERLAP;
    plan.duration_ms = 120000;
    plan.position_ms = 5000;
    RadioSBroadcastHeaderSnapshot value;
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    g_assert_true(value.current);
    g_assert_cmpstr(value.title, ==, "Doğrulanmış çıkan program parçası");
    g_assert_cmpuint(value.position_ms, ==, 30000);
    g_assert_cmpuint(value.duration_ms, ==, 90000);
    g_assert_cmpuint(value.duration_ms - value.position_ms, ==, 60000);
}

static void
playing_precedes_paused_programme(void)
{
    reset();
    observe(0, RADIOS_AUDIO_PAUSED, "Duraklamış program parçası");
    observe(1, RADIOS_AUDIO_PLAYING, "Şimdi gerçekten çalan parça");
    metadata_available[0] = TRUE;
    g_strlcpy(metadata[0].title, "Planın duraklamış parçası", sizeof metadata[0].title);
    plan_available = TRUE;
    plan.deck = 0;
    plan.state = RADIOS_PLAYOUT_PAUSED;
    plan.duration_ms = 90000;
    plan.position_ms = 45000;
    RadioSBroadcastHeaderSnapshot value;
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    g_assert_true(value.current);
    g_assert_false(value.paused);
    g_assert_cmpstr(value.title, ==, "Şimdi gerçekten çalan parça");
    g_assert_cmpuint(value.active_sources, ==, 1);
    g_assert_cmpuint(value.duration_ms, ==, 180000);
}

static void
all_paused_has_no_audible_multiple(void)
{
    reset();
    observe(0, RADIOS_AUDIO_PAUSED, "Birinci duraklamış parça");
    observe(1, RADIOS_AUDIO_PAUSED, "İkinci duraklamış parça");
    RadioSBroadcastHeaderSnapshot value;
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    g_assert_true(value.current);
    g_assert_true(value.paused);
    g_assert_cmpstr(value.title, ==, "Birinci duraklamış parça");
    g_assert_cmpuint(value.active_sources, ==, 0);
}

static void
owned_runner_pause_overrides_playing_decoder(void)
{
    reset();
    observe(0, RADIOS_AUDIO_PLAYING, "Decoder hâlâ PLAYING");
    metadata_available[0] = TRUE;
    g_strlcpy(metadata[0].title, "Doğrulanmış duraklayan parça", sizeof metadata[0].title);
    plan_available = TRUE;
    plan.deck = 0;
    plan.state = RADIOS_PLAYOUT_PAUSED;
    plan.duration_ms = 90000;
    plan.position_ms = 30000;
    RadioSBroadcastHeaderSnapshot value;
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    g_assert_true(value.current);
    g_assert_true(value.paused);
    g_assert_cmpuint(value.active_sources, ==, 0);
    g_assert_cmpstr(value.title, ==, "Doğrulanmış duraklayan parça");
    g_assert_cmpuint(value.duration_ms, ==, 90000);
    g_assert_cmpuint(value.position_ms, ==, 30000);
    observe(1, RADIOS_AUDIO_PLAYING, "Gerçek çalan başka kaynak");
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    g_assert_false(value.paused);
    g_assert_cmpuint(value.active_sources, ==, 1);
    g_assert_cmpstr(value.title, ==, "Gerçek çalan başka kaynak");
    g_assert_cmpuint(value.duration_ms, ==, 180000);
}

static void
verified_cue_out_tempo_and_plan_preference(void)
{
    reset();
    observe(0, RADIOS_AUDIO_PLAYING, "Elle çalan diğer parça");
    observe(2, RADIOS_AUDIO_PLAYING, "Eski deck etiketi");
    audio[2].tempo_ratio = 1.5;
    metadata_available[2] = TRUE;
    g_strlcpy(metadata[2].title, "Doğrulanmış program içeriği", sizeof metadata[2].title);
    g_strlcpy(metadata[2].artist, "Doğrulanmış sanatçı", sizeof metadata[2].artist);
    metadata[2].duration_ms = 180000;
    plan_available = TRUE;
    plan.deck = 2;
    plan.state = RADIOS_PLAYOUT_PLAYING;
    plan.cue_in_ms = 10000;
    plan.position_ms = 30000;
    plan.duration_ms = 90000;
    programmes[0].programme_active = TRUE;
    g_strlcpy(programmes[0].programme_title, "Kabul edilmiş program", sizeof programmes[0].programme_title);
    RadioSBroadcastHeaderSnapshot value;
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    g_assert_cmpstr(value.title, ==, "Doğrulanmış program içeriği");
    g_assert_cmpstr(value.artist, ==, "Doğrulanmış sanatçı");
    g_assert_true(value.programme_active);
    g_assert_cmpstr(value.programme_title, ==, "Kabul edilmiş program");
    g_assert_cmpuint(value.active_sources, ==, 2);
    g_assert_cmpuint(value.duration_ms, ==, 90000);
    g_assert_cmpuint(value.position_ms, ==, 30000);
    g_assert_cmpfloat(value.tempo_ratio, ==, 1.5);
    g_assert_cmpfloat((value.duration_ms - value.position_ms) / value.tempo_ratio, ==, 40000.0);
}

static void
unowned_cached_plan_is_ignored(void)
{
    reset();
    observe(0, RADIOS_AUDIO_PLAYING, "Sonradan elle yüklenen parça");
    plan_available = TRUE;
    plan.deck = 0;
    plan.state = RADIOS_PLAYOUT_PLAYING;
    plan.duration_ms = 90000;
    plan.position_ms = 80000;
    RadioSBroadcastHeaderSnapshot value;
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    g_assert_cmpstr(value.title, ==, "Sonradan elle yüklenen parça");
    g_assert_cmpuint(value.duration_ms, ==, 180000);
    g_assert_cmpuint(value.position_ms, ==, 30000);
}

static void
clears_stale_programme_on_selected_source(void)
{
    reset();
    observe(0, RADIOS_AUDIO_PAUSED, "Duraklamış önceki kaynak");
    observe(1, RADIOS_AUDIO_PLAYING, "Yeni gerçek kaynak");
    programme_count = 3;
    programmes[0].programme_active = programmes[1].programme_active = TRUE;
    g_strlcpy(programmes[0].programme_title, "Önceki program", sizeof programmes[0].programme_title);
    g_strlcpy(programmes[1].programme_title, "Önceki program", sizeof programmes[1].programme_title);
    RadioSBroadcastHeaderSnapshot value;
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    g_assert_cmpstr(value.title, ==, "Yeni gerçek kaynak");
    g_assert_false(value.programme_active);
    g_assert_cmpstr(value.programme_title, ==, "");
}

static void
unknown_and_null_clear_previous_result(void)
{
    reset();
    RadioSBroadcastHeaderSnapshot value;
    memset(&value, 0xa5, sizeof value);
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    assert_empty(&value);
    g_assert_false(value.programme_active);
    g_assert_cmpstr(value.programme_title, ==, "");
    memset(&value, 0xa5, sizeof value);
    g_assert_false(radios_broadcast_header_snapshot(NULL, &value));
    assert_empty(&value);
    g_assert_false(value.programme_active);
    g_assert_cmpstr(value.programme_title, ==, "");
    g_assert_false(radios_broadcast_header_snapshot(&context, NULL));
}

static void
tempo_fallback_and_bounded_utf8(void)
{
    reset();
    observe(0, RADIOS_AUDIO_PLAYING, "Geçerli parça");
    audio[0].tempo_ratio = NAN;
    memset(audio[0].title, 'x', sizeof audio[0].title);
    audio[0].title[799] = (gchar)0xc3;
    audio[0].title[800] = '\0';
    RadioSBroadcastHeaderSnapshot value;
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    g_assert_cmpfloat(value.tempo_ratio, ==, 1.0);
    g_assert_true(g_utf8_validate(value.title, -1, NULL));
    g_assert_cmpuint(strlen(value.title), ==, 799);
    audio[0].tempo_ratio = -2;
    g_assert_true(radios_broadcast_header_snapshot(&context, &value));
    g_assert_cmpfloat(value.tempo_ratio, ==, 1.0);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    static const gchar *const filters[] = {
        "cue-driver-main-routing-flag", "closed-main-gate", "main-device-offline", "unmapped-deck",
        "not-main-routed", "missing-ui-observation", "ready-not-playing", "loading-not-playing",
        "finished-not-playing", "engine-absent", "generation-changed-between-observations",
        "state-changed-to-EOF-between-observations"
    };
    for (guint filter = 0; filter < G_N_ELEMENTS(filters); ++filter) {
        gchar *name = g_strdup_printf("/broadcast-header/reject/%s", filters[filter]);
        g_test_add_data_func(name, GUINT_TO_POINTER(filter), rejects_non_main);
        g_free(name);
    }
    g_test_add_func("/broadcast-header/genuine-main-playing", main_playing);
    g_test_add_func("/broadcast-header/manual-source-actual-cue-out", manual_source_uses_actual_cue_out);
    g_test_add_func("/broadcast-header/retiring-owned-source-actual-cue-out", retiring_owned_source_uses_actual_cue_out);
    g_test_add_func("/broadcast-header/playing-precedes-paused-programme", playing_precedes_paused_programme);
    g_test_add_func("/broadcast-header/all-paused-no-audible-multiple", all_paused_has_no_audible_multiple);
    g_test_add_func("/broadcast-header/owned-runner-pause-playing-decoder", owned_runner_pause_overrides_playing_decoder);
    g_test_add_func("/broadcast-header/verified-cue-out-tempo-plan-preference", verified_cue_out_tempo_and_plan_preference);
    g_test_add_func("/broadcast-header/unowned-cached-plan-ignored", unowned_cached_plan_is_ignored);
    g_test_add_func("/broadcast-header/selected-source-clears-stale-programme", clears_stale_programme_on_selected_source);
    g_test_add_func("/broadcast-header/unknown-null-clears-previous", unknown_and_null_clear_previous_result);
    g_test_add_func("/broadcast-header/tempo-fallback-bounded-utf8", tempo_fallback_and_bounded_utf8);
    return g_test_run();
}
