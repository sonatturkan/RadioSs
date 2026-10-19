#include "audio_internal.h"
#include "wasapi_device.h"
#include "radios/audio_runner.h"
#include "radios/audio_tempo.h"
#include "radios/audio_transition.h"
#include "radios/platform.h"
#include <math.h>
#include <string.h>

/* Public declarations also live in header_navigation.h; the engine remains
 * independent of GTK include paths. */
gboolean radios_audio_engine_runner_jump_to(RadioSAudioEngine *engine,
    guint64 expected_epoch, guint index, GError **error);
gboolean radios_audio_engine_player_runner_jump_to(RadioSAudioEngine *engine,
    guint deck, guint64 expected_epoch, guint index, GError **error);

#define PCM_CAPACITY 131072u
#define PCM_AHEAD 48000u
#define OUTPUT_CAPACITY 8192u
#define RENDER_FRAMES 480u
#define HISTORY_COUNT 512u
/* A burst of flush requests (routing set up for a whole window, several
 * transport or gate commands) settles into one output flush: the control
 * worker waits until no new request arrived for FLUSH_SETTLE_US, at most
 * FLUSH_SETTLE_MAX_US after the first. Rendering already waits for it. */
#define FLUSH_SETTLE_US 8000
#define FLUSH_SETTLE_MAX_US 40000
#define ENGINE_TAPS 24u
#define ENGINE_LINES 16u
#define TAP_CAPACITY 48000u
/* A line starts after 30 ms of audio and is trimmed back to 50 ms when its
 * producer runs ahead, so clock drift never grows into delay. */
#define LINE_PRIME 1440u
#define LINE_LIMIT 9600u
#define LINE_KEEP 2400u
typedef struct { gdouble value, target, step; guint remaining; } GainRamp;
static void gain_target(GainRamp *ramp, gdouble target)
{
    if (ramp->target == target) return;
    ramp->target = target; ramp->remaining = 240; ramp->step = (target - ramp->value) / 240;
}
static gdouble gain_next(GainRamp *ramp)
{
    if (ramp->remaining) { ramp->value += ramp->step; if (!--ramp->remaining) ramp->value = ramp->target; }
    return ramp->value;
}

struct RadioSAudioTap {
    struct RadioSAudioEngine *engine;
    gint source;
    RadioSAudioRing ring;
    gfloat block[RADIOS_AUDIO_MAX_READ_FRAMES * 2];
};
struct RadioSAudioLine {
    struct RadioSAudioEngine *engine;
    gint target;
    gboolean primed, muted;
    RadioSAudioRing ring;
};
typedef struct {
    struct RadioSAudioEngine *engine;
    guint index;
    GThread *thread;
    GCancellable *cancel;
    gchar *path, *sha;
    gchar media_id[37], version_id[37], error[256];
    RadioSAudioRing pcm;
    guint64 generation, request, base, target, cue_in, cue_out, reported;
    gboolean requested_play, pause_after_load, source_eof, main_routed, reopen;
    RadioSAudioState state;
    gdouble gain_db;
    gdouble tempo_ratio;
    guint64 tempo_return_start_frames, tempo_return_frames, tempo_source_origin;
    gdouble tempo_offset;
    gint sync_master;
    gboolean sync_manual; /* operator SYNC; automatic transition grids are separate */
    guint phase_mode;
    RadioSRhythmAnalysis rhythm;
    RadioSLoudness loudness;
    gpointer loudness_owner;
    guint64 loudness_owner_epoch, loudness_revision;
    gboolean loudness_origin_allowed, loudness_enabled;
    GainRamp gain, direct_loudness;
    RadioSAudioTransitionFilter transition_filter;
    gfloat peak[2];
    gboolean external_bound;
    RadioSAudioPlayerBinding binding;
    guint64 external_received, external_consumed;
} AudioDeck;
/* A deck synced to a master starts on the master's grid. WAIT holds a started
 * deck silent for at most one beat, until its beat meets the master's; SKIP
 * drops the audio a reload or a dry buffer made it miss and never waits. */
enum { PHASE_NONE, PHASE_WAIT, PHASE_SKIP };
typedef struct {
    RadioSAudioRunItem spec;
    gchar path[4097], sha[65], media_id[37], version_id[37], entry_id[37];
    gchar adjunct_path[4097], adjunct_sha[65], adjunct_media[37], adjunct_version[37], adjunct_entry[37];
    gchar adjunct_title[801], adjunct_artist[801];
} RunEntry;
typedef struct {
    guint offset, index, next_index;
    gint current, next;
    RadioSAudioRunState state;
    guint64 transition, transition_total;
    /* overlap is recorded on its own: a PAUSED (or WAITING_MEDIA) mark taken
     * during a crossfade must rewind into that crossfade, not out of it. */
    guint8 current_started, next_started, pending_stop, overlap;
    guint cut_remaining, cut_total;
    gint pending_jump;
} RunMark;
typedef struct {
    RunEntry *items;
    gchar playlist_id[37];
    guint slot;
    guint count, index;
    RadioSAudioRunOptions options;
    guint64 epoch, transition, transition_total, final_tag, final_frame, finish_frames;
    gint current, next, loaded_index[32];
    gint loudness_pair_source, loudness_pair_target; /* last successful physical handover */
    guint64 retire_tag[32];
    guint64 first_tag[32];
    RadioSAudioRunTerminal retire_terminal[32];
    guint64 receipt_occurrence[32], receipt_write;
    RadioSAudioRunReceipt receipts[RADIOS_AUDIO_RUN_MAX_RECEIPTS];
    gboolean owned[32], started[32], active, paused, overlap, ending;
    gboolean adjunct_owned[32], adjunct_completed[RADIOS_AUDIO_RUN_MAX_ITEMS];
    struct { gint item; guint64 generation, retire_tag; gboolean started; } adjunct[32];
    RadioSAudioRunState state;
    gchar error[256];
    guint cut_remaining, cut_total;
    gint pending_jump;
    gboolean pending_stop;
    gboolean handover_pending, handover_active, handover_ending_before, handover_cut, handover_completed;
    gint64 handover_utc_us;
    RadioSAudioRunStartPolicy handover_policy;
    guint64 handover_max_wait_frames;
    guint handover_fade_frames;
    GThread *thread;
    RadioSAudioShapeVoice *shape_voice[2]; /* designed overlap: outgoing, incoming */
} AudioRunner;
typedef struct {
    guint mark_count;
    guint64 epoch;
    RunMark marks[8];
} RunHistory;
typedef struct {
    guint64 first_tag;
    guint frames;
    guint64 generation[RADIOS_AUDIO_DECKS], start[RADIOS_AUDIO_DECKS];
    guint consumed[RADIOS_AUDIO_DECKS];
    RadioSAudioTempoProfile tempo_profile[RADIOS_AUDIO_DECKS];
    gdouble tempo_output_start[RADIOS_AUDIO_DECKS];
    guint start_offset[RADIOS_AUDIO_DECKS];
    RunHistory runs[33];
    RadioSAudioDspMeters dsp_meters;
} RenderHistory;
struct RadioSAudioEngine {
    gatomicrefcount refs;
    GMutex mutex;
    GCond condition;
    gboolean realtime, closing, preferences_pending, refresh_pending, flush_pending;
    gboolean flush_requested; /* deferred deck transport flush, settling while rendering goes on */
    gboolean gates[3], microphone_gate; /* on-air gate = microphone_latch || microphone_ptt */
    gboolean microphone_latch, microphone_ptt; /* operator latch (L) and momentary push-to-talk (R) */
    gboolean preview_output_enabled;
    RadioSAudioBus preview_output_bus;
    guint preview_flush_mask;
    guint64 preview_flush_serial;
    GThread *render_thread, *control_thread;
    guint output_sample_rate, output_buffer_frames;
    gboolean output_configuration_pending;
    RadioSAudioPreferences preferences;
    RadioSAudioBackend *backend;
    RadioSAudioBackendSnapshot physical;
    RadioSAudioRing outputs[3], capture, external;
    guint64 external_serial, external_token, external_received;
    guint external_deck;
    gfloat external_peak[2];
    gchar external_error[256];
    RadioSAudioOutput *deliveries[RADIOS_AUDIO_OUTPUT_COUNT];
    struct RadioSAudioTap *taps[ENGINE_TAPS];
    struct RadioSAudioLine *lines[ENGINE_LINES];
    gint gate_mirror;
    RadioSAudioDsp *dsp;
    RadioSAudioMixerConfig mixer; /* actual effective channel faders */
    gdouble manual_channel_gain[RADIOS_AUDIO_MIXER_CHANNELS];
    gboolean channel_auto_gain[RADIOS_AUDIO_MIXER_CHANNELS];
    GainRamp gains[3], microphone_gain;
    GTimeZone *loudness_zone;
    guint64 loudness_revision;
    gint64 loudness_clock_minute;
    gboolean loudness_time_matches;
    AudioRunner runner;
    AudioRunner *player_runners[32], *processing_runner;
    AudioRunner *deck_runner[32];
    AudioDeck decks[RADIOS_AUDIO_DECKS];
    RenderHistory history[HISTORY_COUNT];
    guint64 history_write, next_tag, offline_consumed;
    guint64 flush_serial, flushes_done; /* requested / performed full output flushes */
    guint64 loss_epoch;
    gfloat peak[3][2], rms[3][2];
};
static GPrivate runner_scope = G_PRIVATE_INIT(NULL);
static AudioRunner *existing_runner(RadioSAudioEngine *engine, guint slot)
{ return slot ? engine->player_runners[slot - 1] : &engine->runner; }
static AudioRunner *active_runner(RadioSAudioEngine *engine)
{
    if (engine->processing_runner) return engine->processing_runner;
    return existing_runner(engine,GPOINTER_TO_UINT(g_private_get(&runner_scope)));
}
static void runner_each_locked(RadioSAudioEngine *engine, void (*operation)(RadioSAudioEngine *))
{
    AudioRunner *previous = engine->processing_runner;
    for (guint slot = 0; slot < 33; ++slot) {
        AudioRunner *run = existing_runner(engine,slot); if (!run) continue;
        engine->processing_runner = run; operation(engine);
    }
    engine->processing_runner = previous;
}
static gboolean runner_other_owns(RadioSAudioEngine *engine, guint deck, const AudioRunner *owner)
{
    for (guint slot = 0; slot < 33; ++slot) {
        AudioRunner *run = existing_runner(engine,slot);
        if (run && run != owner && (run->owned[deck] || run->adjunct_owned[deck])) return TRUE;
    }
    return FALSE;
}
gboolean radios_audio_engine_deck_runner_owner(RadioSAudioEngine *engine, guint deck, gint *source)
{
    if (!engine || deck >= RADIOS_AUDIO_NORMAL_DECKS) return FALSE;
    gboolean found = FALSE;
    g_mutex_lock(&engine->mutex);
    for (guint slot = 0; slot < 33; ++slot) {
        AudioRunner *run = existing_runner(engine, slot);
        if (run && (run->owned[deck] || run->adjunct_owned[deck])) {
            if (source) *source = slot ? (gint)slot - 1 : -1;
            found = TRUE; break;
        }
    }
    g_mutex_unlock(&engine->mutex); return found;
}
/* Caller holds the engine mutex. PFL and foreign leases are unavailable even
 * when a decoder has not loaded any media into the auxiliary player yet. */
static gboolean runner_adjunct_available_locked(RadioSAudioEngine *engine, guint deck, const AudioRunner *run)
{
    gboolean mapped = FALSE, pfl = FALSE;
    for (guint c = 0; c < 32; ++c) if (engine->mixer.channels[c].source_deck == (gint)deck) {
        mapped = TRUE; pfl |= engine->mixer.channels[c].pfl;
    }
    return engine->decks[deck].state == RADIOS_AUDIO_EMPTY &&
        !runner_other_owns(engine, deck, run) && engine->decks[deck].main_routed && mapped && !pfl;
}
static void runner_manual_locked(RadioSAudioEngine *engine, guint deck);
static void runner_adjunct_clear_locked(RadioSAudioEngine *engine, gboolean release);
static void runner_adjunct_clear_player_locked(RadioSAudioEngine *engine, guint player);
static guint64 runner_out(const AudioDeck *deck, const RadioSAudioRunItem *item);
static void runner_rewind_locked(RadioSAudioEngine *engine);
static void runner_control_locked(RadioSAudioEngine *engine);
static void runner_receipts_locked(RadioSAudioEngine *engine);
static void runner_frame_locked(RadioSAudioEngine *engine, RenderHistory *history, guint offset,
    gint64 wall_us, gboolean *active, gdouble *gains);

GQuark radios_audio_error_quark(void) { return g_quark_from_static_string("radios-audio-error"); }
/* Operators read these in notices and player cards. Each says in Turkish what
 * happened and what to do, so nobody needs a details view to act on it. */
static const gchar *const ENGINE_MESSAGES[][2] = {
    {"Deck output device is not ready", "Oynatıcının ses çıkışı hazır değil. Ayarlar › Ses bölümünde çıkış aygıtını seçin."},
    {"Deck has no ready audio", "Oynatıcıda çalmaya hazır parça yok. Önce bir parça ya da playlist yükleyin."},
    {"Deck has no loaded audio", "Oynatıcıda yüklü parça yok."},
    {"Seek is outside loaded cue span", "Seçilen konum parçanın çalınabilir aralığının dışında."},
    {"Microphone and MAIN device must be ready", "Mikrofon ve MAIN çıkışı hazır olmalı."},
    {"Too many live audio taps", "Çok fazla canlı dinleme noktası açık; birini kapatın."},
    {"Too many live audio lines", "Çok fazla canlı ses hattı açık; birini kapatın."},
    {"Audio engine is closed", "Ses motoru kapalı. Uygulamayı yeniden başlatın."},
    {"Deck gain must be -60..6 dB", "Oynatıcı ses düzeyi -60 ile 6 dB arasında olmalı."},
    {"Preview requires CUE/monitor and -60..6 dB gain", "Ön dinleme için CUE/monitör çıkışı ve -60 ile 6 dB arası düzey gerekli."},
    {"Preview deck cannot be routed to MAIN", "Ön dinlemedeki oynatıcı MAIN'e verilemez."},
    {"Automation deck pool must contain distinct normal decks", "Otomasyon için seçilen oynatıcılar birbirinden farklı olmalı."},
    {"Overlap requires at least two automation decks", "Bindirmeli geçiş için en az iki boş oynatıcı gerekli."},
    {"Automation overlap exceeds the following verified cue span", "Bindirme, sonraki parçanın çalınabilir süresinden uzun. Geçiş süresini kısaltın."},
    {"Overlapping automation items require distinct physical decks", "Bindirmeli parçalar farklı oynatıcılarda çalmalı; ikinci bir boş oynatıcı ekleyin."},
    {"Overlapping jingles require distinct explicit players", "Üst üste gelen jingle'lar farklı oynatıcılarda çalmalı."},
    {"Stop or finish the current automation run before replacing its queue", "Sırayı değiştirmeden önce çalan otomasyonu durdurun ya da bitmesini bekleyin."},
    {"Automation scope overlaps another running queue", "Bu oynatıcılar çalan başka bir otomasyona ait. Önce onu durdurun."},
    {"Append requires a live queue with capacity before its rendered end", "Eklemek için çalan ve yeri olan bir otomasyon sırası gerekli."},
    {"Append cannot steal an occupied or CUE jingle player", "Şarkı içi jingle oynatıcısı dolu ya da CUE'da; bu parçalarda jingle atlanır."},
    {"Automation has no idle configured queue", "Başlatılacak hazır bir otomasyon sırası yok. Rotasyonu ya da programı yeniden yükleyin."},
    {"Automation requires empty normal decks mapped to MAIN; a manual deck is never stolen",
     "Otomasyon için boş ve MAIN'e bağlı oynatıcı gerekli. Elle çalan oynatıcı alınmaz: onu durdurun ya da başka oynatıcı seçin."},
    {"Jingle requires its explicit empty MAIN player; occupied or CUE players are never stolen",
     "Şarkı içi jingle için seçilen oynatıcı boş değil, CUE'da ya da MAIN'e bağlı değil. Olasılık ve geçişler › Şarkı içinde jingle bölümünde boş bir oynatıcı seçin."},
    {"Automation is not running", "Otomasyon çalışmıyor."},
    {"Automation is not paused", "Otomasyon duraklatılmış değil."},
    {"Automation is not playing", "Otomasyon çalmıyor."},
    {"Handover requires the same live owned runner without a pending cut", "Devir için aynı çalan otomasyon gerekli."},
    {"Queue jump requires the same playing queue and a valid target", "Sırada atlamak için çalan sıra ve geçerli bir hedef gerekli."},
    {"An independent single-player queue cannot crossfade between two physical decks",
     "Tek oynatıcılı sıra iki oynatıcı arasında bindirme yapamaz. Geçişi \"Parça sonunda\" yapın ya da ikinci oynatıcı ekleyin."},
    {"Automation scope pool must contain its source player", "Otomasyon kümesi kaynak oynatıcıyı içermeli."},
    {"Automation scope is not configured", "Bu küme için otomasyon hazırlanmamış. Yeniden yükleyin."},
    {"Player queue is not configured", "Oynatıcı sırası hazırlanmamış. Yeniden yükleyin."},
    {"Copyright queue changed before update", "Sıra telif güncellemesinden önce değişti; yeniden deneyin."},
    {"Jingle queue changed before copyright update", "Jingle sırası telif güncellemesinden önce değişti; yeniden deneyin."},
};
static gboolean fail(GError **error, RadioSAudioError code, const gchar *message)
{
    for (guint i = 0; i < G_N_ELEMENTS(ENGINE_MESSAGES); ++i)
        if (!strcmp(message, ENGINE_MESSAGES[i][0])) { g_set_error_literal(error, RADIOS_AUDIO_ERROR_DOMAIN, code, ENGINE_MESSAGES[i][1]); return FALSE; }
    /* Malformed requests are program faults: say so in Turkish, keep the key. */
    if (g_str_has_prefix(message, "Invalid ")) {
        g_set_error(error, RADIOS_AUDIO_ERROR_DOMAIN, code, "Ses motoru isteği geçersiz buldu; yeniden deneyin. Sürerse destek için kod: %s", message); return FALSE;
    }
    g_set_error_literal(error, RADIOS_AUDIO_ERROR_DOMAIN, code, message); return FALSE;
}
static void ring_init(RadioSAudioRing *ring, guint capacity, gboolean tags)
{
    ring->capacity = capacity; ring->samples = g_new0(gfloat, capacity * 2);
    if (tags) ring->tags = g_new0(guint64, capacity);
    atomic_init(&ring->read, 0); atomic_init(&ring->write, 0);
}
static void ring_clear(RadioSAudioRing *ring)
{ g_free(ring->samples); g_free(ring->tags); }
guint radios_audio_ring_read(RadioSAudioRing *ring, gfloat *samples, guint frames, guint64 *last_tag)
{
    guint64 read = atomic_load_explicit(&ring->read, memory_order_relaxed);
    guint64 write = atomic_load_explicit(&ring->write, memory_order_acquire);
    guint count = (guint)MIN((guint64)frames, write - read);
    for (guint i = 0; i < count; ++i) {
        guint slot = (guint)((read + i) % ring->capacity);
        samples[i * 2] = ring->samples[slot * 2]; samples[i * 2 + 1] = ring->samples[slot * 2 + 1];
        if (last_tag && ring->tags) *last_tag = ring->tags[slot];
    }
    atomic_store_explicit(&ring->read, read + count, memory_order_release); return count;
}
guint radios_audio_ring_write(RadioSAudioRing *ring, const gfloat *samples, guint frames, guint64 first_tag)
{
    guint64 write = atomic_load_explicit(&ring->write, memory_order_relaxed);
    guint64 read = atomic_load_explicit(&ring->read, memory_order_acquire);
    guint count = (guint)MIN((guint64)frames, ring->capacity - (write - read));
    for (guint i = 0; i < count; ++i) {
        guint slot = (guint)((write + i) % ring->capacity);
        ring->samples[slot * 2] = samples[i * 2]; ring->samples[slot * 2 + 1] = samples[i * 2 + 1];
        if (ring->tags) ring->tags[slot] = first_tag ? first_tag + i : 0;
    }
    atomic_store_explicit(&ring->write, write + count, memory_order_release); return count;
}
void radios_audio_preferences_default(RadioSAudioPreferences *prefs)
{
    g_return_if_fail(prefs); memset(prefs, 0, sizeof *prefs);
    g_strlcpy(prefs->main_sink, "@default", sizeof prefs->main_sink);
    prefs->cue_source = RADIOS_AUDIO_PREVIEW_DECK; prefs->monitor_source = -1;
    prefs->loudness_normalize = TRUE; prefs->loudness_target_lufs = RADIOS_LOUDNESS_DEFAULT_TARGET_LUFS;
    radios_audio_loudness_policy_default(&prefs->loudness_policy);
}
static gboolean deck_loudness_allowed_locked(RadioSAudioEngine *engine, AudioDeck *deck)
{
    const RadioSAudioLoudnessPolicy *p = &engine->preferences.loudness_policy;
    AudioRunner *owner = deck->index < 32 ? engine->deck_runner[deck->index] : NULL;
    if (owner && !owner->owned[deck->index] && !owner->adjunct_owned[deck->index]) owner = NULL;
    guint64 epoch = owner ? owner->epoch : 0;
    if (deck->loudness_revision != engine->loudness_revision || deck->loudness_owner != owner || deck->loudness_owner_epoch != epoch) {
        RadioSAudioLoudnessExecution execution = {.deck = deck->index,
            .context = owner && owner->options.loudness_context ? owner->options.loudness_context : RADIOS_AUDIO_LOUDNESS_MANUAL,
            .assigned = owner && owner->options.loudness_assigned,
            .automation_id = owner ? owner->options.loudness_automation_id : "", .source = -1, .target = -1};
        RadioSAudioLoudnessPolicy base = *p; base.phase = RADIOS_AUDIO_LOUDNESS_PHASE_ALWAYS;
        base.pairs_mode = RADIOS_AUDIO_LOUDNESS_PAIRS_ALL; base.schedule_enabled = FALSE;
        deck->loudness_origin_allowed = deck->index >= 32 ? radios_audio_loudness_policy_unrestricted(p) :
            radios_audio_loudness_policy_matches(&base, &execution, TRUE);
        deck->loudness_owner = owner; deck->loudness_owner_epoch = epoch; deck->loudness_revision = engine->loudness_revision;
    }
    if (!engine->preferences.loudness_normalize || !engine->loudness_time_matches || !deck->loudness_origin_allowed) return FALSE;
    gboolean participant = owner && (owner->current == (gint)deck->index || owner->next == (gint)deck->index);
    gboolean transition = participant && (owner->overlap || owner->cut_remaining);
    if ((p->phase == RADIOS_AUDIO_LOUDNESS_PHASE_TRANSITION && !transition) ||
        (p->phase == RADIOS_AUDIO_LOUDNESS_PHASE_STEADY && transition)) return FALSE;
    if (p->pairs_mode == RADIOS_AUDIO_LOUDNESS_PAIRS_SELECTED) {
        gint source = -1, target = -1;
        if (owner && owner->current >= 0 && owner->next >= 0 && owner->current != owner->next &&
            owner->owned[owner->current] && owner->owned[owner->next] && owner->loaded_index[owner->next] >= 0) {
            source = owner->current; target = owner->next;
        } else if (owner && owner->index + 1 >= owner->count && owner->current == owner->loudness_pair_target) {
            source = owner->loudness_pair_source; target = owner->loudness_pair_target;
        }
        if (source < 0 || target < 0 || source == target ||
            (deck->index != (guint)source && deck->index != (guint)target) ||
            !(p->pairs[source] & ((guint32)1 << target))) return FALSE;
    }
    return TRUE;
}
static gdouble deck_loudness_db(RadioSAudioEngine *engine, AudioDeck *deck)
{ return deck_loudness_allowed_locked(engine, deck) ? radios_loudness_gain_db(&deck->loudness, engine->preferences.loudness_target_lufs) : 0; }
static void channel_loudness_locked(RadioSAudioEngine *engine, guint channel)
{
    gint source = engine->mixer.channels[channel].source_deck;
    AudioDeck *deck = source >= 0 ? &engine->decks[source] : NULL;
    gboolean enabled = deck && deck->loudness.measured && !engine->mixer.channels[channel].loudness_bypass &&
        deck_loudness_allowed_locked(engine, deck);
    gdouble correction = enabled ? radios_loudness_gain_db(&deck->loudness, engine->preferences.loudness_target_lufs) : 0;
    engine->channel_auto_gain[channel] = enabled;
    engine->mixer.channels[channel].gain_db = enabled ? CLAMP(correction, -60, 6) : engine->manual_channel_gain[channel];
    radios_audio_dsp_set_channel_auto_gain(engine->dsp, channel, engine->manual_channel_gain[channel], correction, enabled);
}
/* Normal players receive LUFS correction through the actual mixer fader,
 * with only its unrepresented positive range left at the source ingress.
 * Player gain, runner envelopes and dedicated preview/direct audition trims
 * retain their separate controls. The saved manual mixer level is restored
 * whenever this source is unmeasured, bypassed or outside the LUFS policy. */
static void deck_gain_locked(RadioSAudioEngine *engine, AudioDeck *deck)
{
    deck->loudness_enabled = deck_loudness_allowed_locked(engine, deck);
    gdouble loudness = deck_loudness_db(engine, deck);
    gain_target(&deck->gain, pow(10, (deck->gain_db + (deck->index < RADIOS_AUDIO_NORMAL_DECKS ? 0 : loudness)) / 20));
    gain_target(&deck->direct_loudness, pow(10, loudness / 20));
    if (deck->index < RADIOS_AUDIO_NORMAL_DECKS)
        for (guint c = 0; c < RADIOS_AUDIO_MIXER_CHANNELS; ++c)
            if (engine->mixer.channels[c].source_deck == (gint)deck->index) channel_loudness_locked(engine, c);
}
/* Control/caller thread only. The renderer consumes a cached boolean and does
 * not construct dates, resolve zones, read files or depend on a GTK timer. */
static void loudness_clock_locked(RadioSAudioEngine *engine, gint64 now)
{
    gint64 minute = now / (60 * G_USEC_PER_SEC);
    if (minute == engine->loudness_clock_minute) return;
    engine->loudness_clock_minute = minute;
    const RadioSAudioLoudnessPolicy *p = &engine->preferences.loudness_policy;
    if (!p->schedule_enabled) { engine->loudness_time_matches = TRUE; return; }
    GDateTime *utc = g_date_time_new_from_unix_utc(now / G_USEC_PER_SEC);
    GDateTime *local = utc && engine->loudness_zone ? g_date_time_to_timezone(utc, engine->loudness_zone) : NULL;
    engine->loudness_time_matches = local && radios_audio_loudness_policy_time_matches(p,
        g_date_time_get_day_of_week(local), g_date_time_get_hour(local) * 60 + g_date_time_get_minute(local));
    if (local) g_date_time_unref(local);
    if (utc) g_date_time_unref(utc);
}
static gint driver_locked(RadioSAudioEngine *engine, AudioDeck *deck)
{
    if (deck->index == RADIOS_AUDIO_PREVIEW_DECK) {
        if (engine->preview_output_enabled) return (gint)engine->preview_output_bus;
        if (engine->preferences.cue_source == RADIOS_AUDIO_PREVIEW_DECK) return RADIOS_AUDIO_BUS_CUE;
        if (engine->preferences.monitor_source == RADIOS_AUDIO_PREVIEW_DECK) return RADIOS_AUDIO_BUS_MONITOR;
        return -1;
    }
    gboolean mapped = deck->index == RADIOS_AUDIO_MASTER_SAMPLER, cue = FALSE;
    for (guint i = 0; i < 32; ++i) {
        const RadioSAudioChannelDsp *channel = &engine->mixer.channels[i];
        if (channel->source_deck != (gint)deck->index && deck->index != RADIOS_AUDIO_SAMPLER_BASE + i) continue;
        mapped = TRUE;
        cue |= channel->pfl || (channel->fx_slot >= 0 && engine->mixer.effects[channel->fx_slot].cue);
    }
    gboolean main = mapped && deck->main_routed;
    gboolean master_cue = engine->mixer.master_fx.cue || (engine->mixer.master.fx_slot >= 0 &&
        engine->mixer.effects[engine->mixer.master.fx_slot].cue);
    if (main && (!engine->realtime || engine->physical.ready[0])) return 0;
    if (engine->preferences.cue_source == (gint)deck->index ||
        (engine->preferences.cue_source == -1 && engine->gates[1] && (cue || (mapped && master_cue)))) return 1;
    if (engine->preferences.monitor_source == (gint)deck->index) return 2;
    if (main) return 0;
    return -1;
}
static gboolean driver_ready_locked(RadioSAudioEngine *engine, AudioDeck *deck)
{
    gint bus = driver_locked(engine, deck);
    return bus >= 0 && !(engine->preview_flush_mask & (1u << bus)) &&
        (!engine->realtime || engine->physical.ready[bus]);
}
static RadioSAudioTempoProfile deck_tempo_profile(const AudioDeck *deck)
{
    return (RadioSAudioTempoProfile){deck->tempo_ratio, deck->tempo_return_start_frames, deck->tempo_return_frames};
}
static gdouble deck_output_position(const AudioDeck *deck, guint64 source)
{
    RadioSAudioTempoProfile profile = deck_tempo_profile(deck);
    gdouble origin = radios_audio_tempo_profile_source(&profile, deck->tempo_offset);
    return radios_audio_tempo_profile_output(&profile, origin + (source >= deck->base ? source - deck->base : 0));
}
static gdouble deck_current_ratio(const AudioDeck *deck)
{
    RadioSAudioTempoProfile profile = deck_tempo_profile(deck);
    return radios_audio_tempo_profile_ratio(&profile, deck->tempo_offset + atomic_load(&deck->pcm.read));
}
static guint64 source_frame(const AudioDeck *deck, guint64 output_frames)
{
    RadioSAudioTempoProfile profile = deck_tempo_profile(deck);
    gdouble origin = radios_audio_tempo_profile_source(&profile, deck->tempo_offset);
    guint64 frame = deck->base + (guint64)floor(MAX(0.0, radios_audio_tempo_profile_source(&profile, deck->tempo_offset + output_frames) - origin));
    return deck->cue_out ? MIN(frame, deck->cue_out) : frame;
}
static guint64 actual_cursor_locked(RadioSAudioEngine *engine, AudioDeck *deck)
{
    if (deck->state == RADIOS_AUDIO_LOADING || deck->state == RADIOS_AUDIO_PAUSED || deck->state == RADIOS_AUDIO_EMPTY)
        return deck->reported;
    if (engine->realtime && (engine->flush_pending ||
        (driver_locked(engine, deck) >= 0 && (engine->preview_flush_mask & (1u << driver_locked(engine, deck)))))) return deck->reported;
    if (!engine->realtime) {
        deck->reported = deck->external_bound ? deck->base + deck->external_consumed :
            source_frame(deck, atomic_load_explicit(&deck->pcm.read, memory_order_acquire));
        return deck->reported;
    }
    gint bus = driver_locked(engine, deck);
    if (bus < 0) return deck->reported;
    guint64 tag = engine->physical.played_tag[bus], result = deck->reported;
    guint64 count = MIN(engine->history_write, HISTORY_COUNT);
    for (guint64 offset = 0; offset < count; ++offset) {
        RenderHistory *record = &engine->history[(engine->history_write - count + offset) % HISTORY_COUNT];
        if (record->first_tag > tag || record->generation[deck->index] != deck->generation) continue;
        guint64 played = MIN((guint64)record->frames, tag - record->first_tag + 1);
        guint64 consumed = played > record->start_offset[deck->index] ? played - record->start_offset[deck->index] : 0;
        /* A silent history record is not evidence that this source advanced. */
        if (!consumed || !record->consumed[deck->index]) continue;
        const RadioSAudioTempoProfile *profile = &record->tempo_profile[deck->index];
        gdouble output_start = record->tempo_output_start[deck->index];
        guint64 source_consumed = (guint64)floor(MAX(0.0,
            radios_audio_tempo_profile_source(profile, output_start + MIN(consumed, record->consumed[deck->index])) -
            radios_audio_tempo_profile_source(profile, output_start)));
        guint64 position = record->start[deck->index] + source_consumed;
        result = MAX(result, deck->cue_out ? MIN(position, deck->cue_out) : position);
    }
    deck->reported = result; return result;
}
static void rewind_locked(RadioSAudioEngine *engine)
{
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) {
        AudioDeck *deck = &engine->decks[i];
        if (!deck->pcm.samples || deck->state == RADIOS_AUDIO_LOADING) continue;
        guint64 cursor = actual_cursor_locked(engine, deck);
        guint64 write = atomic_load_explicit(&deck->pcm.write, memory_order_acquire);
        guint64 output_cursor = (guint64)floor(MAX(0.0, deck_output_position(deck, cursor) - deck->tempo_offset));
        if (cursor >= deck->base && output_cursor <= write && write - output_cursor < PCM_CAPACITY)
            atomic_store_explicit(&deck->pcm.read, output_cursor, memory_order_release);
    }
}
/* Device loss closes the on-air microphone completely: the latch and the
 * push-to-talk both drop, and the operator opens them again explicitly. */
static void microphone_close_locked(RadioSAudioEngine *engine)
{
    engine->microphone_gate = engine->microphone_latch = engine->microphone_ptt = FALSE;
    g_atomic_int_set(&engine->gate_mirror, FALSE);
}
/* Immediate flush: every source is rewound to the PCM actually heard and
 * rendering stops until the control worker has flushed. Runner commands rely
 * on this order (rewind first, then change the runner), so they keep it. */
static void schedule_flush_locked(RadioSAudioEngine *engine)
{
    if (!engine->realtime) return;
    if (!engine->flush_pending) { runner_each_locked(engine,runner_rewind_locked); rewind_locked(engine); }
    engine->flush_pending = TRUE; ++engine->flush_serial;
}
/* Deferred flush for a deck transport change (seek, stop, pause, load or
 * unload of a deck being heard): rendering of every other source goes on
 * while requests settle; the control worker rewinds all sources to the PCM
 * heard at that moment just before it flushes (flush_begin_locked). Rewinding
 * with fresh playback evidence instead of the last control turn's also keeps
 * the other decks from repeating audio. */
static void request_flush_locked(RadioSAudioEngine *engine)
{
    if (!engine->realtime) return;
    engine->flush_requested = TRUE; ++engine->flush_serial;
}
static void flush_begin_locked(RadioSAudioEngine *engine)
{
    if (!engine->flush_pending) { runner_each_locked(engine,runner_rewind_locked); rewind_locked(engine); }
    engine->flush_pending = TRUE;
}
/* Audition transport only discards the non-MAIN output queue. Sources whose
 * clock is MAIN keep rendering, and their physical stream and DSP are intact. */
static void schedule_preview_flush_locked(RadioSAudioEngine *engine, guint bus_mask)
{
    if (!engine->realtime) return;
    bus_mask &= (1u << RADIOS_AUDIO_BUS_CUE) | (1u << RADIOS_AUDIO_BUS_MONITOR);
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) {
        AudioDeck *deck = &engine->decks[i]; gint bus = driver_locked(engine, deck);
        if (bus < 0 || !(bus_mask & (1u << bus)) || !deck->pcm.samples || deck->state == RADIOS_AUDIO_LOADING) continue;
        guint64 cursor = actual_cursor_locked(engine, deck);
        guint64 write = atomic_load_explicit(&deck->pcm.write, memory_order_acquire);
        guint64 output_cursor = (guint64)floor(MAX(0.0, deck_output_position(deck, cursor) - deck->tempo_offset));
        if (cursor >= deck->base && output_cursor <= write && write - output_cursor < PCM_CAPACITY)
            atomic_store_explicit(&deck->pcm.read, output_cursor, memory_order_release);
    }
    engine->preview_flush_mask |= bus_mask; ++engine->preview_flush_serial;
    g_cond_broadcast(&engine->condition);
}
static void schedule_deck_flush_locked(RadioSAudioEngine *engine, AudioDeck *deck)
{
    if (deck->index != RADIOS_AUDIO_PREVIEW_DECK) { request_flush_locked(engine); return; }
    gint bus = driver_locked(engine, deck);
    if (bus >= 0) schedule_preview_flush_locked(engine, 1u << bus);
}
static void external_clear_locked(RadioSAudioEngine *engine, const gchar *message)
{
    gboolean had_session = engine->external_token != 0;
    if (had_session && engine->external_deck < RADIOS_AUDIO_NORMAL_DECKS) {
        AudioDeck *deck = &engine->decks[engine->external_deck];
        deck->reported = actual_cursor_locked(engine, deck); deck->requested_play = FALSE;
        if (deck->external_bound)
            deck->state = deck->cue_out && deck->reported >= deck->cue_out ? RADIOS_AUDIO_EOF : RADIOS_AUDIO_READY;
    }
    engine->external_token = 0;
    atomic_store(&engine->external.read, atomic_load(&engine->external.write));
    memset(engine->external_peak, 0, sizeof engine->external_peak);
    if (had_session && !engine->realtime) radios_audio_dsp_reset(engine->dsp);
    if (message) g_strlcpy(engine->external_error, message, sizeof engine->external_error);
}
static void external_unbind_locked(RadioSAudioEngine *engine, AudioDeck *deck)
{
    if (engine->external_token && engine->external_deck == deck->index) external_clear_locked(engine, NULL);
    deck->external_bound = FALSE; memset(&deck->binding, 0, sizeof deck->binding);
    deck->external_received = deck->external_consumed = 0;
}
static gboolean binding_text(const gchar *value, gsize capacity, gboolean required)
{
    const gchar *end = memchr(value, 0, capacity);
    return end && (!required || end != value) && g_utf8_validate(value, end - value, NULL);
}
static gboolean binding_valid(const RadioSAudioPlayerBinding *binding)
{
    return binding && binding->deck < RADIOS_AUDIO_NORMAL_DECKS &&
        binding_text(binding->playlist_id, sizeof binding->playlist_id, TRUE) && g_uuid_string_is_valid(binding->playlist_id) &&
        binding_text(binding->entry_id, sizeof binding->entry_id, TRUE) && g_uuid_string_is_valid(binding->entry_id) &&
        binding_text(binding->provider, sizeof binding->provider, TRUE) && binding_text(binding->source_id, sizeof binding->source_id, TRUE) &&
        binding_text(binding->title, sizeof binding->title, TRUE) && binding_text(binding->artist, sizeof binding->artist, FALSE) &&
        binding->duration_frames <= G_GUINT64_CONSTANT(4147200000);
}
static gboolean binding_matches_locked(RadioSAudioEngine *engine, const RadioSAudioPlayerBinding *binding)
{
    if (!binding_valid(binding) || !binding->generation) return FALSE;
    AudioDeck *deck = &engine->decks[binding->deck];
    return deck->external_bound && deck->generation == binding->generation &&
        g_str_equal(deck->binding.playlist_id, binding->playlist_id) && g_str_equal(deck->binding.entry_id, binding->entry_id) &&
        g_str_equal(deck->binding.provider, binding->provider) && g_str_equal(deck->binding.source_id, binding->source_id);
}
static gboolean local_metadata_valid(guint index, const RadioSAudioPlayerBinding *metadata)
{
    return metadata && metadata->deck == index &&
        binding_text(metadata->playlist_id, sizeof metadata->playlist_id, TRUE) && g_uuid_string_is_valid(metadata->playlist_id) &&
        binding_text(metadata->entry_id, sizeof metadata->entry_id, TRUE) && g_uuid_string_is_valid(metadata->entry_id) &&
        binding_text(metadata->provider, sizeof metadata->provider, TRUE) && binding_text(metadata->source_id, sizeof metadata->source_id, TRUE) &&
        binding_text(metadata->title, sizeof metadata->title, TRUE) && binding_text(metadata->artist, sizeof metadata->artist, FALSE) &&
        metadata->duration_frames <= G_GUINT64_CONSTANT(4147200000);
}
static void local_binding_locked(AudioDeck *deck, const RadioSAudioLoadSpec *spec,
    const RadioSAudioPlayerBinding *metadata, const gchar *playlist_id, const gchar *entry_id)
{
    if (metadata) deck->binding = *metadata;
    else {
        memset(&deck->binding, 0, sizeof deck->binding);
        gchar *playlist = playlist_id ? NULL : g_uuid_string_random();
        gchar *entry = entry_id ? NULL : g_uuid_string_random();
        g_strlcpy(deck->binding.playlist_id, playlist_id ? playlist_id : playlist, sizeof deck->binding.playlist_id);
        g_strlcpy(deck->binding.entry_id, entry_id ? entry_id : entry, sizeof deck->binding.entry_id);
        g_free(playlist); g_free(entry);
        g_strlcpy(deck->binding.provider, "local", sizeof deck->binding.provider);
        g_strlcpy(deck->binding.source_id, spec->media_id && *spec->media_id ? spec->media_id : spec->sha256, sizeof deck->binding.source_id);
        gchar *title = g_path_get_basename(spec->path);
        g_strlcpy(deck->binding.title, title, sizeof deck->binding.title); g_free(title);
        const gchar *invalid = NULL;
        if (!g_utf8_validate(deck->binding.title, -1, &invalid)) deck->binding.title[invalid - deck->binding.title] = 0;
        deck->binding.duration_frames = spec->cue_out_frame;
    }
    deck->binding.deck = deck->index; deck->binding.generation = deck->generation;
}
static void manual_sync_idle_locked(AudioDeck *deck)
{
    if (!deck->sync_manual) return;
    deck->sync_master = -1; deck->phase_mode = PHASE_NONE;
}
static gpointer deck_worker(gpointer value)
{
    AudioDeck *deck = value; RadioSAudioEngine *engine = deck->engine;
    RadioSAudioDecoder *decoder = NULL; gchar *opened_path = NULL, *opened_sha = NULL;
    RadioSAudioTempo *tempo = NULL;
    gfloat *samples = g_new(gfloat, RADIOS_AUDIO_MAX_READ_FRAMES * 2);
    guint64 handled = 0;
    for (;;) {
        g_mutex_lock(&engine->mutex);
        while (!engine->closing && handled == deck->request)
            g_cond_wait(&engine->condition, &engine->mutex);
        if (engine->closing) { g_mutex_unlock(&engine->mutex); break; }
        handled = deck->request; gchar *path = g_strdup(deck->path), *sha = g_strdup(deck->sha);
        guint64 target = deck->target, out = deck->cue_out;
        RadioSAudioTempoProfile profile = deck_tempo_profile(deck); gdouble tempo_offset = deck->tempo_offset;
        gboolean reopen = deck->reopen;
        GCancellable *cancel = g_object_ref(deck->cancel);
        g_mutex_unlock(&engine->mutex);
        GError *error = NULL;
        radios_audio_tempo_free(tempo); tempo = NULL;
        if (!path) {
            radios_audio_decoder_close(decoder); decoder = NULL;
            g_clear_pointer(&opened_path, g_free); g_clear_pointer(&opened_sha, g_free);
            g_free(sha); g_object_unref(cancel); continue;
        }
        if (reopen || !decoder || g_strcmp0(path, opened_path) || g_strcmp0(sha, opened_sha)) {
            radios_audio_decoder_close(decoder); decoder = radios_audio_decoder_open(path, sha, cancel, &error);
            g_free(opened_path); g_free(opened_sha); opened_path = g_strdup(path); opened_sha = g_strdup(sha);
        }
        gboolean valid = decoder && !error && radios_audio_decoder_seek(decoder, target, cancel, &error);
        if (valid && radios_audio_tempo_profile_ratio(&profile, tempo_offset) != 1) { tempo = radios_audio_tempo_new_profile(&profile, tempo_offset, out > target ? out - target : 0, &error); valid = tempo != NULL; }
        g_free(path); g_free(sha);
        g_mutex_lock(&engine->mutex);
        if (engine->closing || handled != deck->request) {
            g_mutex_unlock(&engine->mutex); g_clear_error(&error); g_object_unref(cancel); continue;
        }
        if (!valid) {
            deck->state = RADIOS_AUDIO_ERROR;
            g_strlcpy(deck->error, error ? error->message : "Audio decoder could not open source", sizeof deck->error);
            g_mutex_unlock(&engine->mutex); g_clear_error(&error); g_object_unref(cancel); continue;
        }
        deck->base = target; atomic_store(&deck->pcm.read, 0); atomic_store(&deck->pcm.write, 0);
        deck->source_eof = FALSE;
        g_mutex_unlock(&engine->mutex);
        for (;;) {
            g_mutex_lock(&engine->mutex);
            if (engine->closing || handled != deck->request) { g_mutex_unlock(&engine->mutex); break; }
            guint64 write = atomic_load(&deck->pcm.write), read = atomic_load(&deck->pcm.read);
            if (write - read >= PCM_AHEAD) {
                g_cond_wait_until(&engine->condition, &engine->mutex, g_get_monotonic_time() + 10000);
                g_mutex_unlock(&engine->mutex); continue;
            }
            guint frames = (guint)MIN((guint64)RADIOS_AUDIO_MAX_READ_FRAMES, PCM_AHEAD - (write - read));
            if (out) {
                gdouble source_offset = radios_audio_tempo_profile_source(&profile, tempo_offset);
                guint64 total = (guint64)ceil(radios_audio_tempo_profile_output(&profile, source_offset + out - target) - tempo_offset);
                frames = write < total ? (guint)MIN((guint64)frames, total - write) : 0;
            }
            g_mutex_unlock(&engine->mutex);
            gssize got = frames ? (tempo ? radios_audio_tempo_read(tempo, decoder, samples, frames, cancel, &error) :
                radios_audio_decoder_read(decoder, samples, frames, cancel, &error)) : 0;
            g_mutex_lock(&engine->mutex);
            if (engine->closing || handled != deck->request) { g_mutex_unlock(&engine->mutex); g_clear_error(&error); break; }
            if (got < 0) {
                deck->state = RADIOS_AUDIO_ERROR; g_strlcpy(deck->error, error ? error->message : "Audio decoding failed", sizeof deck->error);
                g_mutex_unlock(&engine->mutex); g_clear_error(&error); break;
            }
            if (got == 0) {
                deck->source_eof = TRUE;
                if (!deck->binding.duration_frames) deck->binding.duration_frames = source_frame(deck, atomic_load(&deck->pcm.write));
                if (atomic_load(&deck->pcm.write) == 0) { deck->state = RADIOS_AUDIO_EOF; manual_sync_idle_locked(deck); }
                g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); break;
            }
            (void)radios_audio_ring_write(&deck->pcm, samples, (guint)got, 0);
            if (deck->state == RADIOS_AUDIO_LOADING)
                deck->state = deck->requested_play && driver_ready_locked(engine, deck) ? RADIOS_AUDIO_PLAYING :
                    deck->pause_after_load ? RADIOS_AUDIO_PAUSED : RADIOS_AUDIO_READY;
            gboolean at_out = out && source_frame(deck, atomic_load(&deck->pcm.write)) == out;
            if (at_out) deck->source_eof = TRUE;
            g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex);
            if (at_out) break;
        }
        g_object_unref(cancel);
    }
    radios_audio_tempo_free(tempo); radios_audio_decoder_close(decoder); g_free(opened_path); g_free(opened_sha); g_free(samples); return NULL;
}
static void meter(const gfloat *samples, guint frames, gfloat peak[2], gfloat rms[2])
{
    gdouble squares[2] = {0, 0}; peak[0] = peak[1] = 0;
    for (guint i = 0; i < frames; ++i) for (guint c = 0; c < 2; ++c) {
        gfloat sample = samples[i * 2 + c]; peak[c] = MAX(peak[c], fabsf(sample)); squares[c] += (gdouble)sample * sample;
    }
    for (guint c = 0; c < 2; ++c) rms[c] = (gfloat)sqrt(squares[c] / frames);
}
/* TRUE when deck `index` may consume its next frame; see PHASE_WAIT/SKIP. */
static gboolean phase_gate_locked(RadioSAudioEngine *engine, RenderHistory *history, guint index)
{
    AudioDeck *deck = &engine->decks[index];
    AudioDeck *master = deck->sync_master >= 0 && deck->sync_master < (gint)RADIOS_AUDIO_NORMAL_DECKS ? &engine->decks[deck->sync_master] : NULL;
    if (!master || master == deck || deck->external_bound || master->external_bound || master->state != RADIOS_AUDIO_PLAYING ||
        !(deck->rhythm.bpm > 0) || !(master->rhythm.bpm > 0)) { deck->phase_mode = PHASE_NONE; return TRUE; }
    gdouble deck_frame = source_frame(deck, atomic_load(&deck->pcm.read));
    gdouble master_frame = source_frame(master, atomic_load(&master->pcm.read));
    gdouble ratio = deck_current_ratio(deck), master_ratio = deck_current_ratio(master);
    gdouble offset = radios_rhythm_phase_offset(&deck->rhythm, ratio, deck_frame, &master->rhythm, master_ratio, master_frame, 1);
    gdouble step = deck->rhythm.bpm * ratio / 2880000, late = 1 - offset;
    /* Up to 2 ms behind is inaudible; start rather than wait a whole beat. A
     * deck that is ahead after a reload simply plays on. */
    if (offset < step || late <= MAX(step, deck->rhythm.bpm / 30000) || (deck->phase_mode == PHASE_SKIP && late > 0.5)) {
        deck->phase_mode = PHASE_NONE; return TRUE;
    }
    if (deck->phase_mode == PHASE_WAIT) return FALSE;
    guint64 drop = (guint64)floor(late / step), dropped = 0; gfloat scratch[512];
    while (dropped < drop) {
        guint read = radios_audio_ring_read(&deck->pcm, scratch, (guint)MIN(drop - dropped, G_N_ELEMENTS(scratch) / 2), NULL);
        if (!read) break;
        dropped += read;
    }
    if (!history->consumed[index]) {
        history->start[index] = source_frame(deck, atomic_load(&deck->pcm.read));
        history->tempo_output_start[index] = deck->tempo_offset + atomic_load(&deck->pcm.read);
    }
    if (dropped < drop) return FALSE;
    deck->phase_mode = PHASE_NONE; return TRUE;
}
static void render_locked(RadioSAudioEngine *engine, guint frames, gfloat *outputs[3])
{
    RenderHistory *history = &engine->history[engine->history_write % HISTORY_COUNT];
    memset(history, 0, sizeof *history); history->first_tag = engine->next_tag; history->frames = frames;
    gdouble bus_gains[3];
    gboolean active[RADIOS_AUDIO_DECKS];
    gboolean routed[RADIOS_AUDIO_DECKS];
    gboolean selected_cue = engine->mixer.master_fx.cue;
    for (guint i = 0; i < 32; ++i) selected_cue |= engine->mixer.channels[i].pfl;
    for (guint i = 0; i < 5; ++i) selected_cue |= engine->mixer.effects[i].cue;
    radios_audio_dsp_begin_block(engine->dsp);
    engine->external_peak[0] = engine->external_peak[1] = 0;
    g_atomic_int_set(&engine->gate_mirror, engine->microphone_gate);
    for (guint l = 0; l < ENGINE_LINES; ++l) {
        RadioSAudioLine *line = engine->lines[l]; if (!line) continue;
        guint64 read = atomic_load(&line->ring.read), queued = atomic_load(&line->ring.write) - read;
        if (queued > LINE_LIMIT) { atomic_store(&line->ring.read, read + queued - LINE_KEEP); queued = LINE_KEEP; }
        if (!line->primed && queued >= LINE_PRIME) line->primed = TRUE;
    }
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) {
        AudioDeck *deck = &engine->decks[i]; history->generation[i] = deck->generation;
        history->start[i] = deck->external_bound ? deck->base + deck->external_consumed : source_frame(deck, atomic_load(&deck->pcm.read));
        history->tempo_profile[i] = deck_tempo_profile(deck);
        history->tempo_output_start[i] = deck->tempo_offset + atomic_load(&deck->pcm.read);
        active[i] = deck->state == RADIOS_AUDIO_PLAYING && driver_ready_locked(engine, deck);
        history->start_offset[i] = frames;
        routed[i] = i != RADIOS_AUDIO_PREVIEW_DECK && deck->main_routed;
        deck->peak[0] = deck->peak[1] = 0;
    }
    /* A line plays through its channel's sampler slot (or the MAIN sampler);
     * an unrouted sampler deck on that slot is silenced instead of leaking. */
    for (guint l = 0; l < ENGINE_LINES; ++l) {
        RadioSAudioLine *line = engine->lines[l]; if (!line) continue;
        guint slot = line->target < 0 ? RADIOS_AUDIO_MASTER_SAMPLER : RADIOS_AUDIO_SAMPLER_BASE + (guint)line->target;
        line->muted = !routed[slot]; routed[slot] = TRUE;
    }
    gint64 block_wall = g_get_real_time();
    /* Tags may contain stream gaps and are consumption evidence, not a wall
     * latency estimate. Never start a scheduled source early by projecting
     * their difference into UTC; actual output may follow the deadline by the
     * hardware buffer latency. Envelopes remain exact sample boundaries. */
    for (guint frame = 0; frame < frames; ++frame) {
        gfloat values[RADIOS_AUDIO_DECKS][2] = {{0}};
        gboolean source_activity[RADIOS_AUDIO_DECKS] = {FALSE};
        gfloat main[2] = {0, 0}, mic[2] = {0, 0}, pfl[2] = {0, 0}, external[2] = {0, 0};
        gdouble runner_gains[RADIOS_AUDIO_DECKS];
        gdouble direct_loudness[RADIOS_AUDIO_NORMAL_DECKS];
        for (guint i = 0; i < RADIOS_AUDIO_NORMAL_DECKS; ++i) direct_loudness[i] = 1;
        for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) runner_gains[i] = 1;
        for (guint slot = 0; slot < 33; ++slot) {
            AudioRunner *run = existing_runner(engine,slot); if (!run || (!run->active && !run->pending_stop)) continue;
            engine->processing_runner = run;
            runner_frame_locked(engine, history, frame, block_wall + (gint64)frame * 1000000 / 48000, active, runner_gains);
        }
        engine->processing_runner = NULL;
        for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) {
            AudioDeck *deck = &engine->decks[i];
            if (active[i] && deck->phase_mode && !phase_gate_locked(engine, history, i)) continue;
            /* A synced deck that ran dry (reload, slow source) has fallen
             * behind its master; it drops what it missed once audio returns. */
            if (active[i] && !deck->external_bound && deck->sync_master >= 0 && !deck->source_eof &&
                !(i < RADIOS_AUDIO_NORMAL_DECKS && engine->deck_runner[i] && engine->deck_runner[i]->owned[i]) &&
                atomic_load(&deck->pcm.write) == atomic_load(&deck->pcm.read)) { deck->phase_mode = PHASE_SKIP; continue; }
            if (active[i] && deck_loudness_allowed_locked(engine, deck) != deck->loudness_enabled)
                deck_gain_locked(engine, deck);
            if (active[i] && !deck->external_bound && radios_audio_ring_read(&deck->pcm, values[i], 1, NULL)) {
                source_activity[i] = TRUE;
                AudioRunner *owner = i < 32 ? engine->deck_runner[i] : NULL;
                if (owner && owner->owned[i] && !owner->first_tag[i]) owner->first_tag[i] = history->first_tag + frame;
                if (owner && owner->owned[i] && owner->overlap && (owner->current == (gint)i || owner->next == (gint)i)) {
                    const RadioSAudioRunItem *outgoing = &owner->items[owner->index].spec;
                    guint64 elapsed = owner->transition ? owner->transition - 1 : 0;
                    if (outgoing->shape.enabled) {
                        guint side = owner->current == (gint)i ? 0 : 1;
                        radios_audio_shape_voice_process(owner->shape_voice[side], &outgoing->shape, side,
                            owner->transition_total ? (gdouble)elapsed / owner->transition_total : 1, values[i]);
                    } else if (owner->current == (gint)i && outgoing->effect == RADIOS_AUDIO_RUN_EFFECT_LOWPASS)
                        radios_audio_transition_lowpass(&deck->transition_filter, elapsed, owner->transition_total, values[i]);
                }
                gdouble deck_gain = gain_next(&deck->gain);
                if (i < RADIOS_AUDIO_NORMAL_DECKS) direct_loudness[i] = gain_next(&deck->direct_loudness);
                if (!history->consumed[i]) history->start_offset[i] = frame;
                ++history->consumed[i];
                for (guint c = 0; c < 2; ++c) {
                    values[i][c] *= (gfloat)(deck_gain * runner_gains[i]); deck->peak[c] = MAX(deck->peak[c], fabsf(values[i][c]));
                }
            }
        }
        if (engine->realtime) (void)radios_audio_ring_read(&engine->capture, mic, 1, NULL);
        gdouble mic_gain = gain_next(&engine->microphone_gain);
        for (guint c = 0; c < 2; ++c) mic[c] *= (gfloat)mic_gain;
        if (engine->external_token && engine->external_deck < RADIOS_AUDIO_NORMAL_DECKS) {
            guint index = engine->external_deck; AudioDeck *deck = &engine->decks[index];
            if (deck->external_bound && active[index] && radios_audio_ring_read(&engine->external, external, 1, NULL)) {
                source_activity[index] = TRUE;
                gdouble level = gain_next(&deck->gain);
                direct_loudness[index] = gain_next(&deck->direct_loudness);
                if (!history->consumed[index]) history->start_offset[index] = frame;
                ++history->consumed[index]; ++deck->external_consumed;
                for (guint c = 0; c < 2; ++c) {
                    values[index][c] = external[c] * (gfloat)level;
                    deck->peak[c] = MAX(deck->peak[c], fabsf(values[index][c]));
                    engine->external_peak[c] = MAX(engine->external_peak[c], fabsf(values[index][c]));
                }
            }
        }
        for (guint l = 0; l < ENGINE_LINES; ++l) {
            RadioSAudioLine *line = engine->lines[l];
            if (line && line->muted) {
                guint slot = line->target < 0 ? RADIOS_AUDIO_MASTER_SAMPLER : RADIOS_AUDIO_SAMPLER_BASE + (guint)line->target;
                values[slot][0] = values[slot][1] = 0;
            }
        }
        for (guint l = 0; l < ENGINE_LINES; ++l) {
            RadioSAudioLine *line = engine->lines[l]; gfloat input[2];
            if (!line || !line->primed) continue;
            if (!radios_audio_ring_read(&line->ring, input, 1, NULL)) { line->primed = FALSE; continue; }
            guint slot = line->target < 0 ? RADIOS_AUDIO_MASTER_SAMPLER : RADIOS_AUDIO_SAMPLER_BASE + (guint)line->target;
            values[slot][0] += input[0]; values[slot][1] += input[1];
            source_activity[slot] = TRUE;
        }
        radios_audio_dsp_set_source_activity(engine->dsp, source_activity);
        radios_audio_dsp_process_frame_with_external(engine->dsp, &values[0][0], routed, mic,
            engine->microphone_gate, NULL, engine->preferences.cue_source == -1, main, pfl);
        for (guint t = 0; t < ENGINE_TAPS; ++t) {
            RadioSAudioTap *tap = engine->taps[t];
            if (!tap || tap->source == RADIOS_AUDIO_TAP_MAIN) continue;
            gfloat *out = &tap->block[frame * 2];
            if (tap->source == RADIOS_AUDIO_TAP_MICROPHONE) { out[0] = mic[0]; out[1] = mic[1]; }
            else radios_audio_dsp_channel_frame(engine->dsp, (guint)tap->source, out);
        }
        for (guint bus = 0; bus < 3; ++bus) {
            bus_gains[bus] = gain_next(&engine->gains[bus]);
            gint source = bus == 1 ? engine->preferences.cue_source : bus == 2 ? engine->preferences.monitor_source : -1;
            gboolean preview = engine->preview_output_enabled && bus == (guint)engine->preview_output_bus;
            if (preview) source = RADIOS_AUDIO_PREVIEW_DECK;
            gboolean suppress_preview = engine->preview_output_enabled && !preview && source == RADIOS_AUDIO_PREVIEW_DECK;
            for (guint c = 0; c < 2; ++c) {
                gdouble value = source < 0 ? main[c] : values[source][c] *
                    (source < (gint)RADIOS_AUDIO_NORMAL_DECKS ? direct_loudness[source] : 1);
                if (bus == 1 && source == -1 && selected_cue) value = pfl[c];
                outputs[bus][frame * 2 + c] = (engine->gates[bus] || preview) && !suppress_preview ?
                    (gfloat)CLAMP(value * bus_gains[bus], -1, 1) : 0;
            }
        }
    }
    for (guint bus = 0; bus < 3; ++bus) meter(outputs[bus], frames, engine->peak[bus], engine->rms[bus]);
    for (guint kind = 0; kind < RADIOS_AUDIO_OUTPUT_COUNT; ++kind)
        radios_audio_output_push_internal(engine->deliveries[kind], outputs[0], frames);
    for (guint t = 0; t < ENGINE_TAPS; ++t) if (engine->taps[t])
        (void)radios_audio_ring_write(&engine->taps[t]->ring,
            engine->taps[t]->source == RADIOS_AUDIO_TAP_MAIN ? outputs[0] : engine->taps[t]->block, frames, 0);
    radios_audio_dsp_get_meters(engine->dsp, &history->dsp_meters);
    ++engine->history_write; engine->next_tag += frames;
    if (!engine->realtime) engine->offline_consumed += frames;
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) {
        AudioDeck *deck = &engine->decks[i];
        guint64 cursor = actual_cursor_locked(engine, deck);
        if (deck->state == RADIOS_AUDIO_PLAYING && deck->source_eof &&
            cursor >= source_frame(deck, atomic_load(&deck->pcm.write))) {
            deck->state = RADIOS_AUDIO_EOF; manual_sync_idle_locked(deck);
        }
    }
    g_cond_broadcast(&engine->condition);
}
static gpointer render_worker(gpointer value)
{
    RadioSAudioEngine *engine = value; gfloat samples[3][RENDER_FRAMES * 2];
    gfloat *outputs[] = {samples[0], samples[1], samples[2]};
    /* MMCSS "Pro Audio": a busy or minimised desktop must not delay the
     * 10 ms blocks the device threads are waiting for. */
    gpointer priority = radios_wasapi_audio_priority_begin();
    for (;;) {
        g_mutex_lock(&engine->mutex);
        if (engine->closing) { g_mutex_unlock(&engine->mutex); break; }
        gboolean available = !engine->flush_pending, any = FALSE;
        for (guint bus = 0; bus < 3; ++bus) if (engine->physical.ready[bus] && !(engine->preview_flush_mask & (1u << bus))) {
            any = TRUE;
            guint64 queued = atomic_load(&engine->outputs[bus].write) - atomic_load(&engine->outputs[bus].read);
            if (queued >= RENDER_FRAMES * 4) available = FALSE;
        }
        if (any && available) {
            guint64 tag = engine->next_tag; render_locked(engine, RENDER_FRAMES, outputs);
            for (guint bus = 0; bus < 3; ++bus) if (engine->physical.ready[bus] && !(engine->preview_flush_mask & (1u << bus)))
                (void)radios_audio_ring_write(&engine->outputs[bus], outputs[bus], RENDER_FRAMES, tag);
        } else g_cond_wait_until(&engine->condition, &engine->mutex, g_get_monotonic_time() + 3000);
        g_mutex_unlock(&engine->mutex);
    }
    radios_wasapi_audio_priority_end(priority);
    return NULL;
}
static void flush_settle_locked(RadioSAudioEngine *engine)
{
    /* Only deferred requests settle, and rendering continues meanwhile. An
     * immediate flush (rendering already stopped) is carried out at once. */
    if (!engine->flush_requested || engine->flush_pending) return;
    gint64 now = g_get_monotonic_time(), deadline = now + FLUSH_SETTLE_MAX_US, quiet = now + FLUSH_SETTLE_US;
    guint64 serial = engine->flush_serial;
    while (!engine->closing && engine->flush_requested && !engine->flush_pending) {
        now = g_get_monotonic_time();
        if (engine->flush_serial != serial) { serial = engine->flush_serial; quiet = now + FLUSH_SETTLE_US; }
        if (now >= quiet || now >= deadline) break;
        g_cond_wait_until(&engine->condition, &engine->mutex, MIN(quiet, deadline));
    }
}
static gpointer control_worker(gpointer value)
{
    RadioSAudioEngine *engine = value;
    /* Two device snapshots (~133 KB each with 64+64 devices) live on the
     * heap, not on this thread's stack. */
    RadioSAudioBackendSnapshot *physical = g_new0(RadioSAudioBackendSnapshot, 1);
    RadioSAudioBackendSnapshot *settled = g_new0(RadioSAudioBackendSnapshot, 1);
    for (;;) {
        g_mutex_lock(&engine->mutex); flush_settle_locked(engine); g_mutex_unlock(&engine->mutex);
        radios_audio_backend_snapshot(engine->backend, physical);
        g_mutex_lock(&engine->mutex);
        if (engine->closing) { g_mutex_unlock(&engine->mutex); break; }
        loudness_clock_locked(engine, g_get_real_time());
        gboolean lost = engine->physical.connected && !physical->connected;
        for (guint bus = 0; bus < 3; ++bus)
            lost |= engine->physical.ready[bus] && !physical->ready[bus] &&
                (physical->bus_error[bus][0] || physical->error[0]);
        if (lost) {
            external_clear_locked(engine, "MAIN ses bağlantısı kesildi; dış kaynak yeniden başlatılmalı.");
            ++engine->loss_epoch;
            /* Freeze only the last confirmed server consumption before the
             * disconnected snapshot loses its timing evidence. Drop queued
             * PCM and require an explicit operator action after reconnect. */
            runner_each_locked(engine,runner_receipts_locked);
            schedule_flush_locked(engine);
            for (guint slot = 0; slot < 33; ++slot) {
                AudioRunner *run = existing_runner(engine,slot);
                if (run && run->active && run->state != RADIOS_AUDIO_RUN_ERROR && run->state != RADIOS_AUDIO_RUN_FINISHED) {
                    run->paused = TRUE; run->state = RADIOS_AUDIO_RUN_PAUSED;
                    g_strlcpy(run->error,"Ses bağlantısı kesildi; aygıt geri geldiğinde operatör devam ettirmeli",sizeof run->error);
                }
            }
            for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) {
                AudioDeck *deck = &engine->decks[i];
                deck->requested_play = FALSE;
                if (deck->state == RADIOS_AUDIO_PLAYING || deck->state == RADIOS_AUDIO_READY || deck->state == RADIOS_AUDIO_EOF) {
                    deck->reported = actual_cursor_locked(engine, deck); deck->state = RADIOS_AUDIO_PAUSED; manual_sync_idle_locked(deck);
                    g_strlcpy(deck->error, "Ses bağlantısı kesildi; devam etmek için oynatın", sizeof deck->error);
                }
            }
            microphone_close_locked(engine);
            atomic_store(&engine->capture.read, atomic_load(&engine->capture.write));
            engine->preferences_pending = TRUE;
        }
        engine->physical = *physical;
        if (!physical->microphone_ready && (engine->microphone_gate || engine->microphone_latch || engine->microphone_ptt)) {
            microphone_close_locked(engine);
            atomic_store(&engine->capture.read, atomic_load(&engine->capture.write));
        }
        gboolean apply = engine->preferences_pending && physical->connected;
        gboolean refresh = engine->refresh_pending && physical->connected;
        gboolean flush = engine->flush_pending || engine->flush_requested; guint64 flush_serial = engine->flush_serial;
        guint preview_flush_mask = engine->preview_flush_mask;
        guint64 preview_flush_serial = engine->preview_flush_serial;
        RadioSAudioPreferences prefs = engine->preferences;
        gboolean configure_output = engine->output_configuration_pending && physical->connected;
        guint output_rate = engine->output_sample_rate, output_frames = engine->output_buffer_frames;
        if (configure_output) engine->output_configuration_pending = FALSE;
        if (apply) engine->preferences_pending = FALSE;
        if (refresh) engine->refresh_pending = FALSE;
        if (flush) { flush_begin_locked(engine); rewind_locked(engine); }
        for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) {
            AudioDeck *deck = &engine->decks[i]; gint bus = driver_locked(engine, deck);
            if (deck->state == RADIOS_AUDIO_READY && deck->requested_play && driver_ready_locked(engine, deck))
                deck->state = RADIOS_AUDIO_PLAYING;
            if (deck->state == RADIOS_AUDIO_PLAYING && bus < 0) {
                deck->state = RADIOS_AUDIO_PAUSED; manual_sync_idle_locked(deck);
                g_strlcpy(deck->error, "Deck has no output route", sizeof deck->error);
            }
            if (deck->state == RADIOS_AUDIO_PLAYING && bus >= 0 && !physical->ready[bus] &&
                (physical->bus_error[bus][0] || physical->error[0])) {
                deck->reported = actual_cursor_locked(engine, deck); deck->state = RADIOS_AUDIO_PAUSED; manual_sync_idle_locked(deck);
                g_strlcpy(deck->error, physical->bus_error[bus][0] ? physical->bus_error[bus] : physical->error, sizeof deck->error);
            }
        }
        g_mutex_unlock(&engine->mutex);
        if (configure_output) radios_audio_backend_configure_output(engine->backend, output_rate, output_frames);
        if (flush) radios_audio_backend_flush(engine->backend);
        else if (preview_flush_mask) radios_audio_backend_flush_buses(engine->backend, preview_flush_mask);
        if (configure_output && !flush) radios_audio_backend_apply(engine->backend, &prefs);
        if (apply) radios_audio_backend_apply(engine->backend, &prefs);
        if (refresh) radios_audio_backend_refresh(engine->backend);
        radios_audio_backend_reconnect(engine->backend);
        /* The backend flushes a playing stream in place and keeps it ready, so
         * rendering resumes at once from the backend's actual state instead of
         * waiting for the next control turn. A stream it had to re-create, or a
         * device/format change, still reports not ready. */
        memset(settled, 0, sizeof *settled);
        if (flush || preview_flush_mask) radios_audio_backend_snapshot(engine->backend, settled);
        g_mutex_lock(&engine->mutex);
        if (flush) {
            /* A request made while flushing gets its own settled flush. */
            engine->flush_requested = engine->flush_serial != flush_serial;
            engine->flush_pending = FALSE;
            ++engine->flushes_done; engine->history_write = 0;
            radios_audio_dsp_reset(engine->dsp);
            memcpy(engine->physical.ready, settled->ready, sizeof engine->physical.ready);
            memset(engine->physical.played_tag, 0, sizeof engine->physical.played_tag);
        }
        if (preview_flush_mask) {
            if (engine->preview_flush_serial == preview_flush_serial) engine->preview_flush_mask = 0;
            for (guint bus = 1; bus < 3; ++bus) if (preview_flush_mask & (1u << bus)) {
                engine->physical.ready[bus] = settled->ready[bus];
                engine->physical.played_tag[bus] = 0;
            }
        }
        g_cond_broadcast(&engine->condition);
        if (!engine->closing) g_cond_wait_until(&engine->condition, &engine->mutex, g_get_monotonic_time() + 20000);
        g_mutex_unlock(&engine->mutex);
    }
    g_free(physical); g_free(settled);
    return NULL;
}
RadioSAudioEngine *radios_audio_engine_new(gboolean realtime, const gchar *server)
{
    RadioSAudioEngine *engine = g_new0(RadioSAudioEngine, 1);
    g_atomic_ref_count_init(&engine->refs); g_mutex_init(&engine->mutex); g_cond_init(&engine->condition);
    engine->realtime = realtime; engine->next_tag = 1;
    engine->output_sample_rate = 48000; engine->output_buffer_frames = 2400;
    active_runner(engine)->current = active_runner(engine)->next = active_runner(engine)->pending_jump = -1;
    for (guint i = 0; i < 32; ++i) active_runner(engine)->loaded_index[i] = -1;
    radios_audio_run_options_default(&active_runner(engine)->options);
    for (guint side = 0; side < 2; ++side) active_runner(engine)->shape_voice[side] = radios_audio_shape_voice_new();
    engine->dsp = radios_audio_dsp_new();
    radios_audio_dsp_set_microphone_ramp(engine->dsp, 240);
    radios_audio_dsp_get_config(engine->dsp, &engine->mixer);
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i) engine->manual_channel_gain[i] = engine->mixer.channels[i].gain_db;
    radios_audio_preferences_default(&engine->preferences);
    engine->loudness_zone = radios_time_zone_new("UTC");
    if (!engine->loudness_zone) engine->loudness_zone = g_time_zone_new_utc();
    engine->loudness_revision = 1;
    engine->loudness_clock_minute = G_MININT64; engine->loudness_time_matches = TRUE;
    for (guint bus = 0; bus < 3; ++bus) {
        ring_init(&engine->outputs[bus], OUTPUT_CAPACITY, TRUE); engine->gates[bus] = TRUE;
        engine->gains[bus].value = engine->gains[bus].target = 1;
    }
    engine->microphone_gain.value = engine->microphone_gain.target = 1;
    for (guint kind = 0; kind < RADIOS_AUDIO_OUTPUT_COUNT; ++kind)
        engine->deliveries[kind] = radios_audio_output_new_internal((RadioSAudioOutputKind)kind);
    ring_init(&engine->capture, OUTPUT_CAPACITY, FALSE);
    ring_init(&engine->external, OUTPUT_CAPACITY, FALSE);
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) {
        engine->decks[i].engine = engine; engine->decks[i].index = i;
        engine->decks[i].main_routed = i != RADIOS_AUDIO_PREVIEW_DECK;
        engine->decks[i].gain.value = engine->decks[i].gain.target = 1;
        engine->decks[i].direct_loudness.value = engine->decks[i].direct_loudness.target = 1;
        engine->decks[i].tempo_ratio = 1; engine->decks[i].sync_master = -1;
        atomic_init(&engine->decks[i].pcm.read, 0); atomic_init(&engine->decks[i].pcm.write, 0);
    }
    if (realtime) {
        /* 1 ms timers, no power throttling/EcoQoS when minimised, no idle
         * sleep while a realtime engine exists (released in unref). */
        radios_wasapi_realtime_acquire();
        RadioSAudioRing *outputs[] = {&engine->outputs[0], &engine->outputs[1], &engine->outputs[2]};
        engine->backend = radios_audio_backend_new(outputs, &engine->capture, server);
        engine->preferences_pending = TRUE;
        engine->control_thread = g_thread_new("radios-devices", control_worker, engine);
        engine->render_thread = g_thread_new("radios-render", render_worker, engine);
    }
    return engine;
}
RadioSAudioEngine *radios_audio_engine_ref(RadioSAudioEngine *engine)
{ g_return_val_if_fail(engine, NULL); g_atomic_ref_count_inc(&engine->refs); return engine; }
void radios_audio_engine_unref(RadioSAudioEngine *engine)
{
    if (!engine || !g_atomic_ref_count_dec(&engine->refs)) return;
    g_mutex_lock(&engine->mutex); engine->closing = TRUE;
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) if (engine->decks[i].cancel) g_cancellable_cancel(engine->decks[i].cancel);
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex);
    if (engine->render_thread) g_thread_join(engine->render_thread);
    if (engine->control_thread) g_thread_join(engine->control_thread);
    for (guint kind = 0; kind < RADIOS_AUDIO_OUTPUT_COUNT; ++kind) {
        radios_audio_output_shutdown_internal(engine->deliveries[kind]);
        radios_audio_output_unref(engine->deliveries[kind]);
    }
    if (engine->runner.thread) g_thread_join(engine->runner.thread);
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) {
        AudioDeck *deck = &engine->decks[i]; if (deck->thread) g_thread_join(deck->thread);
        g_clear_object(&deck->cancel); g_free(deck->path); g_free(deck->sha); ring_clear(&deck->pcm);
    }
    radios_audio_backend_free(engine->backend);
    if (engine->realtime) radios_wasapi_realtime_release();
    radios_audio_dsp_free(engine->dsp);
    g_free(active_runner(engine)->items);
    for (guint side = 0; side < 2; ++side) radios_audio_shape_voice_free(active_runner(engine)->shape_voice[side]);
    for (guint deck = 0; deck < 32; ++deck) {
        AudioRunner *run = engine->player_runners[deck]; if (!run) continue;
        for (guint side = 0; side < 2; ++side) radios_audio_shape_voice_free(run->shape_voice[side]);
        g_free(run->items); g_free(run);
    }
    for (guint bus = 0; bus < 3; ++bus) ring_clear(&engine->outputs[bus]);
    ring_clear(&engine->capture); ring_clear(&engine->external);
    if (engine->loudness_zone) g_time_zone_unref(engine->loudness_zone);
    g_cond_clear(&engine->condition); g_mutex_clear(&engine->mutex); g_free(engine);
}
RadioSAudioOutput *radios_audio_engine_output_acquire(RadioSAudioEngine *engine, RadioSAudioOutputKind kind)
{
    if (!engine || kind < 0 || kind >= RADIOS_AUDIO_OUTPUT_COUNT) return NULL;
    return radios_audio_output_ref(engine->deliveries[kind]);
}
static gboolean valid_device(const gchar *name)
{ return memchr(name, '\0', RADIOS_AUDIO_DEVICE_NAME_SIZE) && g_utf8_validate(name, -1, NULL); }
gboolean radios_audio_engine_apply_preferences(RadioSAudioEngine *engine, const RadioSAudioPreferences *prefs, GError **error)
{
    if (!engine || !prefs || !valid_device(prefs->main_sink) || !valid_device(prefs->cue_sink) ||
        !valid_device(prefs->monitor_sink) || !valid_device(prefs->microphone_source))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Device name must be valid UTF-8 of at most 512 bytes");
    const gdouble gains[] = {prefs->main_gain_db, prefs->cue_gain_db, prefs->monitor_gain_db, prefs->microphone_gain_db};
    for (guint i = 0; i < 4; ++i) if (!isfinite(gains[i]) || gains[i] < -60 || gains[i] > (i == 3 ? 30 : 6))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Audio gain is outside supported range");
    if (!isfinite(prefs->loudness_target_lufs) || prefs->loudness_target_lufs < -30 || prefs->loudness_target_lufs > -5)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Loudness target must be -30..-5 LUFS");
    if (prefs->cue_source < -1 || prefs->cue_source > 32 || prefs->monitor_source < -1 || prefs->monitor_source > 32)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "CUE/monitor source must be MAIN or an existing deck");
    if (!radios_audio_loudness_policy_validate(&prefs->loudness_policy, NULL))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid loudness normalization policy");
    /* IANA ids ("Europe/Istanbul"): Windows GLib only knows registry zone
     * names, the platform layer maps them (historical DST included). */
    GTimeZone *zone = radios_time_zone_new(prefs->loudness_policy.timezone);
    if (!zone) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Loudness policy time zone is unavailable");
    g_mutex_lock(&engine->mutex);
    gboolean devices = g_strcmp0(prefs->main_sink, engine->preferences.main_sink) ||
        g_strcmp0(prefs->cue_sink, engine->preferences.cue_sink) || g_strcmp0(prefs->monitor_sink, engine->preferences.monitor_sink) ||
        g_strcmp0(prefs->microphone_source, engine->preferences.microphone_source);
    gboolean routing = prefs->cue_source != engine->preferences.cue_source || prefs->monitor_source != engine->preferences.monitor_source;
    if (engine->realtime && engine->physical.connected) {
        const gchar *names[] = {prefs->main_sink, prefs->cue_sink, prefs->monitor_sink};
        for (guint i = 0; i < 3; ++i) if (*names[i] && !engine->physical.ready[i] && engine->physical.bus_error[i][0]) devices = TRUE;
        if (*prefs->microphone_source && !engine->physical.microphone_ready && engine->physical.microphone_error[0]) devices = TRUE;
    }
    if (devices || routing) schedule_flush_locked(engine);
    engine->preferences = *prefs;
    if (engine->loudness_zone) g_time_zone_unref(engine->loudness_zone);
    engine->loudness_zone = zone;
    ++engine->loudness_revision; engine->loudness_clock_minute = G_MININT64;
    loudness_clock_locked(engine, g_get_real_time());
    const gdouble targets[] = {prefs->main_gain_db, prefs->cue_gain_db, prefs->monitor_gain_db};
    for (guint bus = 0; bus < 3; ++bus) gain_target(&engine->gains[bus], pow(10, targets[bus] / 20));
    gain_target(&engine->microphone_gain, pow(10, prefs->microphone_gain_db / 20));
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) deck_gain_locked(engine, &engine->decks[i]);
    if (devices && engine->realtime) { engine->preferences_pending = TRUE; microphone_close_locked(engine); }
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_configure_output(RadioSAudioEngine *engine,
    guint rate, guint frames, GError **error)
{
    if (!engine || (rate != 44100 && rate != 48000) || frames < 64 || frames > 4096 || (frames & (frames - 1)))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Playback format requires 44100/48000 Hz and power-of-two 64..4096 frames");
    g_mutex_lock(&engine->mutex);
    if (engine->closing) {
        g_mutex_unlock(&engine->mutex);
        return fail(error, RADIOS_AUDIO_ERROR_CLOSED, "Audio engine is closed");
    }
    if (engine->output_sample_rate != rate || engine->output_buffer_frames != frames) {
        if (engine->realtime) {
            runner_each_locked(engine, runner_receipts_locked);
            schedule_flush_locked(engine);
            ++engine->loss_epoch;
            external_clear_locked(engine, "Çıkış biçimi değişti; dış kaynak yeniden başlatılmalı.");
            for (guint slot = 0; slot < 33; ++slot) {
                AudioRunner *run = existing_runner(engine, slot);
                if (run && run->active && run->state != RADIOS_AUDIO_RUN_ERROR && run->state != RADIOS_AUDIO_RUN_FINISHED) {
                    run->paused = TRUE; run->state = RADIOS_AUDIO_RUN_PAUSED;
                    g_strlcpy(run->error, "Çıkış biçimi değişti; devam etmek için oynatın.", sizeof run->error);
                }
            }
            for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) {
                AudioDeck *deck = &engine->decks[i];
                deck->requested_play = FALSE;
                if (deck->state == RADIOS_AUDIO_PLAYING) {
                    deck->reported = actual_cursor_locked(engine, deck);
                    deck->state = RADIOS_AUDIO_PAUSED; manual_sync_idle_locked(deck);
                    g_strlcpy(deck->error, "Çıkış biçimi değişti; devam etmek için oynatın.", sizeof deck->error);
                }
            }
            microphone_close_locked(engine);
            atomic_store(&engine->capture.read, atomic_load(&engine->capture.write));
            engine->output_configuration_pending = TRUE;
        }
        engine->output_sample_rate = rate; engine->output_buffer_frames = frames;
        g_cond_broadcast(&engine->condition);
    }
    g_mutex_unlock(&engine->mutex); return TRUE;
}
void radios_audio_engine_output_configuration(RadioSAudioEngine *engine, guint *rate, guint *frames)
{
    if (!engine) return;
    g_mutex_lock(&engine->mutex);
    if (rate) *rate = engine->output_sample_rate;
    if (frames) *frames = engine->output_buffer_frames;
    g_mutex_unlock(&engine->mutex);
}
void radios_audio_engine_refresh_devices(RadioSAudioEngine *engine)
{ if (!engine) return; g_mutex_lock(&engine->mutex); engine->refresh_pending = TRUE; g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); }
static gboolean main_path_locked(RadioSAudioEngine *engine, AudioDeck *deck, gchar reason[256])
{
    reason[0] = 0;
    const gchar *blocked = deck->index == RADIOS_AUDIO_PREVIEW_DECK ? "Önizleme yalnız CUE çıkışını kullanır." :
        !deck->main_routed ? "Bu oynatıcı MAIN çıkışına bağlı değil." :
        !engine->gates[0] ? "MAIN çıkışı kapalı." :
        engine->realtime && (engine->flush_pending || !engine->physical.ready[0]) ? "MAIN ses çıkışı hazır değil." :
        engine->mixer.master.mute ? "MASTER sessize alınmış." :
        engine->preferences.main_gain_db <= -60 ? "MASTER ses düzeyi en altta." :
        deck->gain_db <= -60 ? "Oynatıcı ses düzeyi en altta." : NULL;
    if (blocked) { g_strlcpy(reason, blocked, 256); return FALSE; }
    guint32 solo = radios_audio_dsp_contributing_solo_mask(engine->dsp);
    gboolean mapped = FALSE; guint blocked_strip = 0;
    for (guint i = 0; i < 32; ++i) {
        const RadioSAudioChannelDsp *strip = &engine->mixer.channels[i];
        if (strip->source_deck != (gint)deck->index) continue;
        mapped = TRUE; blocked_strip = i;
        gboolean crossfaded = (strip->crossfade_group == 1 && engine->mixer.crossfader >= .999999999) ||
            (strip->crossfade_group == 2 && engine->mixer.crossfader <= -.999999999);
        if (!strip->mute && strip->gain_db > -60 && !crossfaded && (!solo || (solo & (((guint32)1) << i)))) return TRUE;
    }
    if (!mapped) g_strlcpy(reason, "Oynatıcı bir mikser kanalına atanmamış.", 256);
    else if (solo && !engine->mixer.channels[blocked_strip].solo)
        g_strlcpy(reason, "Başka bir canlı mikser kanalında SOLO açık; bu oynatıcı MAIN'de duyulmaz.", 256);
    else g_snprintf(reason, 256, "Mikser kanalı %u sessiz veya crossfader bu kanalı kapatıyor.", blocked_strip + 1);
    return FALSE;
}
void radios_audio_engine_snapshot(RadioSAudioEngine *engine, RadioSAudioSnapshot *snapshot)
{
    g_return_if_fail(engine && snapshot); memset(snapshot, 0, sizeof *snapshot);
    g_mutex_lock(&engine->mutex); snapshot->realtime = engine->realtime;
    snapshot->connected = !engine->realtime || engine->physical.connected;
    snapshot->loss_epoch = engine->loss_epoch;
    snapshot->external_main_active = engine->external_token != 0;
    snapshot->external_main_queued_frames = (guint)(atomic_load(&engine->external.write) - atomic_load(&engine->external.read));
    snapshot->external_main_received_frames = engine->external_received;
    memcpy(snapshot->external_main_peak, engine->external_peak, sizeof snapshot->external_main_peak);
    g_strlcpy(snapshot->external_main_error, engine->external_error, sizeof snapshot->external_main_error);
    snapshot->microphone_gate = engine->microphone_gate; snapshot->microphone_ready = engine->physical.microphone_ready;
    snapshot->microphone_latch = engine->microphone_latch; snapshot->microphone_ptt = engine->microphone_ptt;
    g_strlcpy(snapshot->error, engine->physical.error, sizeof snapshot->error);
    g_strlcpy(snapshot->microphone_error, engine->physical.microphone_error, sizeof snapshot->microphone_error);
    memcpy(snapshot->microphone_peak, engine->physical.microphone_peak, sizeof snapshot->microphone_peak);
    snapshot->output_count = engine->physical.output_count; snapshot->input_count = engine->physical.input_count;
    memcpy(snapshot->outputs, engine->physical.outputs, sizeof snapshot->outputs); memcpy(snapshot->inputs, engine->physical.inputs, sizeof snapshot->inputs);
    radios_audio_dsp_get_config(engine->dsp, &snapshot->mixer);
    if (!engine->realtime) radios_audio_dsp_get_meters(engine->dsp, &snapshot->dsp_meters);
    else {
        guint64 tag = engine->physical.played_tag[0], count = MIN(engine->history_write, HISTORY_COUNT);
        for (guint64 offset = 0; offset < count; ++offset) {
            RenderHistory *record = &engine->history[(engine->history_write - count + offset) % HISTORY_COUNT];
            if (record->first_tag + record->frames - 1 <= tag) snapshot->dsp_meters = record->dsp_meters;
        }
        memcpy(snapshot->dsp_meters.microphone_peak, engine->physical.microphone_peak, sizeof snapshot->dsp_meters.microphone_peak);
        memcpy(snapshot->dsp_meters.master_peak, engine->physical.peak[0], sizeof snapshot->dsp_meters.master_peak);
    }
    const gchar *devices[] = {engine->preferences.main_sink, engine->preferences.cue_sink, engine->preferences.monitor_sink};
    for (guint bus = 0; bus < 3; ++bus) {
        RadioSAudioBusSnapshot *target = &snapshot->buses[bus];
        target->gate = engine->gates[bus] || (engine->preview_output_enabled && bus == (guint)engine->preview_output_bus);
        target->ready = !engine->realtime || (!engine->flush_pending &&
            !(engine->preview_flush_mask & (1u << bus)) && engine->physical.ready[bus]);
        target->consumed_frames = engine->realtime ? engine->physical.consumed_frames[bus] : engine->offline_consumed;
        if (engine->realtime && target->ready) {
            target->output_sample_rate = engine->physical.output_sample_rate[bus];
            target->output_buffer_frames = engine->physical.output_buffer_frames[bus];
        }
        memcpy(target->peak, engine->realtime ? engine->physical.peak[bus] : engine->peak[bus], sizeof target->peak);
        memcpy(target->rms, engine->realtime ? engine->physical.rms[bus] : engine->rms[bus], sizeof target->rms);
        g_strlcpy(target->device, devices[bus], sizeof target->device); g_strlcpy(target->error, engine->physical.bus_error[bus], sizeof target->error);
    }
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) {
        AudioDeck *deck = &engine->decks[i]; RadioSAudioDeckSnapshot *target = &snapshot->decks[i];
        target->cursor_frames = actual_cursor_locked(engine, deck);
        if (!deck->external_bound && deck->state == RADIOS_AUDIO_PLAYING && deck->source_eof && target->cursor_frames >= source_frame(deck, atomic_load(&deck->pcm.write))) {
            deck->state = RADIOS_AUDIO_EOF; manual_sync_idle_locked(deck);
        }
        target->state = deck->state; target->generation = deck->generation;
        target->cue_in_frame = deck->cue_in; target->cue_out_frame = deck->cue_out;
        target->buffered_frames = deck->external_bound && engine->external_token && engine->external_deck == i ?
            atomic_load(&engine->external.write) - atomic_load(&engine->external.read) : atomic_load(&deck->pcm.write) - atomic_load(&deck->pcm.read);
        target->external_bound = deck->external_bound; target->binding = deck->binding;
        target->external_received_frames = deck->external_received;
        RadioSAudioTempoProfile tempo_profile = deck_tempo_profile(deck);
        target->bpm = deck->rhythm.bpm;
        target->tempo_ratio = radios_audio_tempo_profile_ratio(&tempo_profile, deck_output_position(deck, target->cursor_frames));
        target->loudness_measured = deck->loudness.measured; target->loudness_lufs = deck->loudness.integrated_lufs;
        target->loudness_gain_db = deck_loudness_db(engine, deck);
        target->effective_bpm = deck->rhythm.bpm * target->tempo_ratio; target->sync_master = deck->sync_master;
        target->gain_db = deck->gain_db; target->main_routed = deck->main_routed;
        target->main_path_ready = main_path_locked(engine, deck, target->main_block_reason);
        target->runner_paused = i < RADIOS_AUDIO_NORMAL_DECKS && engine->deck_runner[i] && engine->deck_runner[i]->paused;
        target->output_bus = driver_locked(engine, deck);
        g_strlcpy(target->media_id, deck->media_id, sizeof target->media_id); g_strlcpy(target->version_id, deck->version_id, sizeof target->version_id);
        g_strlcpy(target->error, deck->error, sizeof target->error); memcpy(target->peak, deck->peak, sizeof target->peak);
    }
    g_mutex_unlock(&engine->mutex);
}
static gboolean check_deck(RadioSAudioEngine *engine, guint deck, GError **error)
{ return engine && deck < RADIOS_AUDIO_DECKS ? TRUE : fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid audio engine/deck"); }
/* Only a deck being heard (PLAYING, or an external stream) has PCM queued in
 * the outputs. Loading, cueing, stopping or seeking an idle deck must not
 * flush the outputs, which reopens them under the decks on air. */
static gboolean deck_audible_locked(const AudioDeck *deck)
{ return deck->state == RADIOS_AUDIO_PLAYING || deck->external_bound; }
static void request_locked(RadioSAudioEngine *engine, AudioDeck *deck, guint64 target, gboolean play)
{
    gboolean audible = deck_audible_locked(deck);
    if (deck->external_bound) external_unbind_locked(engine, deck);
    if (audible) schedule_deck_flush_locked(engine, deck);
    if (!engine->realtime && deck->index != RADIOS_AUDIO_PREVIEW_DECK) radios_audio_dsp_reset(engine->dsp);
    if (deck->cancel) g_cancellable_cancel(deck->cancel);
    g_clear_object(&deck->cancel); deck->cancel = g_cancellable_new();
    ++deck->generation; ++deck->request; deck->binding.generation = deck->generation;
    RadioSAudioTempoProfile tempo_profile = deck_tempo_profile(deck);
    deck->tempo_offset = radios_audio_tempo_profile_output(&tempo_profile, target >= deck->tempo_source_origin ? target - deck->tempo_source_origin : 0);
    deck->target = target; deck->reported = target; deck->requested_play = play; deck->pause_after_load = FALSE;
    deck->state = RADIOS_AUDIO_LOADING; deck->source_eof = FALSE; deck->error[0] = '\0';
    deck->reopen = FALSE;
    if (!deck->thread) { ring_init(&deck->pcm, PCM_CAPACITY, FALSE); deck->thread = g_thread_new("radios-decode", deck_worker, deck); }
    g_cond_broadcast(&engine->condition);
}
/* Same-source seek (R8.3): the transport keeps its state instead of showing a
 * reload. A target inside the decoded window only moves the read cursor and
 * needs no new decode. Any other target restarts the decoder at the target
 * while the ring is cleared at once and base/reported already name the target:
 * the cursor reports the chosen point immediately, the renderer never replays
 * the old position, and PLAYING/PAUSED/READY survive instead of LOADING. */
static void seek_locked(RadioSAudioEngine *engine, AudioDeck *deck, guint64 target, gboolean play)
{
    RadioSAudioState before = deck->state;
    gboolean loaded = deck->pcm.samples && !deck->external_bound && before != RADIOS_AUDIO_LOADING &&
        before != RADIOS_AUDIO_ERROR && before != RADIOS_AUDIO_EMPTY;
    RadioSAudioState after = play ? RADIOS_AUDIO_PLAYING : before == RADIOS_AUDIO_PAUSED ? RADIOS_AUDIO_PAUSED : RADIOS_AUDIO_READY;
    if (loaded) {
        guint64 read = atomic_load(&deck->pcm.read), write = atomic_load(&deck->pcm.write);
        guint64 cursor = (guint64)floor(MAX(0.0, deck_output_position(deck, target) - deck->tempo_offset));
        if (target >= deck->base && cursor >= read && cursor < write && source_frame(deck, cursor) == target) {
            ++deck->generation; deck->binding.generation = deck->generation;
            atomic_store_explicit(&deck->pcm.read, cursor, memory_order_release);
            deck->reported = target; deck->requested_play = play; deck->pause_after_load = FALSE;
            deck->state = after; deck->error[0] = '\0';
            if (before == RADIOS_AUDIO_PLAYING) schedule_deck_flush_locked(engine, deck);
            if (!engine->realtime && deck->index != RADIOS_AUDIO_PREVIEW_DECK) radios_audio_dsp_reset(engine->dsp);
            g_cond_broadcast(&engine->condition); return;
        }
    }
    request_locked(engine, deck, target, play);
    if (!loaded) return;
    deck->base = target;
    atomic_store(&deck->pcm.read, 0); atomic_store(&deck->pcm.write, 0);
    deck->state = after;
}
static void manual_sync_reset_locked(AudioDeck *deck)
{
    deck->sync_manual = FALSE; deck->sync_master = -1; deck->phase_mode = PHASE_NONE;
    deck->tempo_ratio = 1; deck->tempo_return_start_frames = deck->tempo_return_frames = 0;
    deck->tempo_offset = 0; deck->tempo_source_origin = deck->cue_in;
}
static void runner_load_locked(RadioSAudioEngine *engine, guint index, guint item)
{
    AudioDeck *deck = &engine->decks[index]; RunEntry *entry = &active_runner(engine)->items[item];
    external_unbind_locked(engine, deck);
    g_free(deck->path); g_free(deck->sha); deck->path = g_strdup(entry->path); deck->sha = g_strdup(entry->sha);
    g_strlcpy(deck->media_id, entry->media_id, sizeof deck->media_id); g_strlcpy(deck->version_id, entry->version_id, sizeof deck->version_id);
    deck->cue_in = entry->spec.media.cue_in_frame; deck->cue_out = entry->spec.media.cue_out_frame;
    deck->rhythm = entry->spec.rhythm; deck->tempo_ratio = entry->spec.tempo_ratio ? entry->spec.tempo_ratio : 1;
    deck->tempo_return_start_frames = entry->spec.tempo_return_start_frames;
    deck->tempo_return_frames = entry->spec.tempo_return_frames; deck->tempo_offset = 0; deck->tempo_source_origin = deck->cue_in;
    deck->loudness = entry->spec.loudness; deck_gain_locked(engine, deck);
    deck->sync_master = entry->spec.beat_sync ? active_runner(engine)->current : -1; deck->sync_manual = FALSE;
    deck->transition_filter = (RadioSAudioTransitionFilter){0};
    if (deck->cancel) g_cancellable_cancel(deck->cancel);
    g_clear_object(&deck->cancel); deck->cancel = g_cancellable_new();
    ++deck->generation; ++deck->request; deck->target = deck->reported = deck->cue_in;
    local_binding_locked(deck, &entry->spec.media, NULL, active_runner(engine)->playlist_id, entry->entry_id);
    deck->requested_play = FALSE; deck->pause_after_load = FALSE;
    deck->state = RADIOS_AUDIO_LOADING; deck->source_eof = FALSE; deck->reopen = TRUE; deck->error[0] = '\0';
    atomic_store(&deck->pcm.read, 0); atomic_store(&deck->pcm.write, 0);
    active_runner(engine)->loaded_index[index] = (gint)item;
    active_runner(engine)->adjunct_completed[item] = FALSE;
    active_runner(engine)->started[index] = FALSE;
    active_runner(engine)->first_tag[index] = 0;
    active_runner(engine)->receipt_occurrence[index] = 0;
    active_runner(engine)->retire_terminal[index] = RADIOS_AUDIO_RUN_TERMINAL_NONE;
    g_cond_broadcast(&engine->condition);
}
/* Worker/control thread only. Retiring a silent/drained runner source never
 * flushes another source's MAIN PCM. No render-thread allocation or file I/O. */
static void runner_clear_deck_locked(RadioSAudioEngine *engine, guint index)
{
    AudioDeck *deck = &engine->decks[index];
    external_unbind_locked(engine, deck);
    if (deck->cancel) g_cancellable_cancel(deck->cancel);
    else deck->cancel = g_cancellable_new();
    ++deck->generation; ++deck->request; deck->requested_play = FALSE; deck->state = RADIOS_AUDIO_EMPTY;
    g_clear_pointer(&deck->path, g_free); g_clear_pointer(&deck->sha, g_free);
    deck->media_id[0] = deck->version_id[0] = deck->error[0] = '\0';
    deck->base = deck->target = deck->reported = deck->cue_in = deck->cue_out = 0; deck->source_eof = FALSE;
    deck->rhythm = (RadioSRhythmAnalysis){0}; deck->tempo_ratio = 1; deck->tempo_return_start_frames = deck->tempo_return_frames = 0; deck->tempo_offset = 0; deck->tempo_source_origin = deck->cue_in; deck->sync_master = -1; deck->sync_manual = FALSE;
    deck->loudness = (RadioSLoudness){0}; deck_gain_locked(engine, deck);
    atomic_store(&deck->pcm.read, 0); atomic_store(&deck->pcm.write, 0);
    active_runner(engine)->loaded_index[index] = -1; active_runner(engine)->retire_tag[index] = 0;
    active_runner(engine)->first_tag[index] = 0;
    active_runner(engine)->receipt_occurrence[index] = 0;
    active_runner(engine)->retire_terminal[index] = RADIOS_AUDIO_RUN_TERMINAL_NONE;
    g_cond_broadcast(&engine->condition);
}
/* The auxiliary voice has its own lease. It is never a sequential music
 * item and never advances the primary run index or receipts. Control/API
 * thread only: generation cancellation prevents stale decoder work returning. */
static void runner_adjunct_clear_player_locked(RadioSAudioEngine *engine, guint d)
{
    AudioRunner *run = active_runner(engine);
    if (run->adjunct[d].generation && run->adjunct_owned[d] && engine->decks[d].generation == run->adjunct[d].generation)
        runner_clear_deck_locked(engine,d);
    run->adjunct[d].generation=run->adjunct[d].retire_tag=0; run->adjunct[d].item=-1; run->adjunct[d].started=FALSE;
}
static void runner_adjunct_clear_locked(RadioSAudioEngine *engine, gboolean release)
{
    AudioRunner *run=active_runner(engine);
    for (guint d=0;d<32;++d) {
        runner_adjunct_clear_player_locked(engine,d);
        if (release && run->adjunct_owned[d]) { run->adjunct_owned[d]=FALSE; if (engine->deck_runner[d]==run) engine->deck_runner[d]=NULL; }
    }
}
static void runner_adjunct_load_locked(RadioSAudioEngine *engine, guint i)
{
    AudioRunner *run = active_runner(engine);
    const RadioSAudioRunAdjunct *voice = &run->items[i].spec.adjunct; guint d = voice->player;
    AudioDeck *deck = &engine->decks[d];
    if (!voice->enabled || !run->adjunct_owned[d] || run->adjunct[d].generation || run->adjunct_completed[i] || deck->state != RADIOS_AUDIO_EMPTY) return;
    if (!deck->thread) { ring_init(&deck->pcm, PCM_CAPACITY, FALSE); deck->thread = g_thread_new("radios-decode", deck_worker, deck); }
    external_unbind_locked(engine, deck);
    g_free(deck->path); g_free(deck->sha); deck->path = g_strdup(voice->media.path); deck->sha = g_strdup(voice->media.sha256);
    g_strlcpy(deck->media_id, voice->media.media_id, sizeof deck->media_id); g_strlcpy(deck->version_id, voice->media.version_id, sizeof deck->version_id);
    deck->cue_in = voice->media.cue_in_frame; deck->cue_out = voice->media.cue_out_frame;
    manual_sync_reset_locked(deck); deck->rhythm = (RadioSRhythmAnalysis){0}; deck->loudness = voice->loudness; deck_gain_locked(engine, deck);
    deck->transition_filter = (RadioSAudioTransitionFilter){0};
    if (deck->cancel) g_cancellable_cancel(deck->cancel);
    g_clear_object(&deck->cancel); deck->cancel = g_cancellable_new(); ++deck->generation; ++deck->request;
    deck->target = deck->reported = deck->cue_in;
    RadioSAudioPlayerBinding metadata = {0}; metadata.deck = d;
    g_strlcpy(metadata.playlist_id, run->playlist_id, sizeof metadata.playlist_id);
    g_strlcpy(metadata.entry_id, voice->entry_id, sizeof metadata.entry_id);
    g_strlcpy(metadata.provider, "local", sizeof metadata.provider); g_strlcpy(metadata.source_id, voice->media.media_id, sizeof metadata.source_id);
    g_strlcpy(metadata.title, voice->title ? voice->title : "", sizeof metadata.title); g_strlcpy(metadata.artist, voice->artist ? voice->artist : "", sizeof metadata.artist);
    local_binding_locked(deck, &voice->media, &metadata, run->playlist_id, voice->entry_id);
    deck->requested_play = deck->pause_after_load = FALSE; deck->state = RADIOS_AUDIO_LOADING;
    deck->source_eof = FALSE; deck->reopen = TRUE; deck->error[0] = '\0';
    atomic_store(&deck->pcm.read, 0); atomic_store(&deck->pcm.write, 0);
    run->adjunct[d].item=(gint)i; run->adjunct[d].generation=deck->generation;
    run->adjunct[d].retire_tag=0; run->adjunct[d].started=FALSE; g_cond_broadcast(&engine->condition);
}
gboolean radios_audio_engine_load(RadioSAudioEngine *engine, guint index, const RadioSAudioLoadSpec *spec, GError **error)
{
    return radios_audio_engine_load_player(engine, index, spec, NULL, error);
}
gboolean radios_audio_engine_load_player(RadioSAudioEngine *engine, guint index, const RadioSAudioLoadSpec *spec,
    const RadioSAudioPlayerBinding *metadata, GError **error)
{
    if (!check_deck(engine, index, error)) return FALSE;
    if (metadata && !local_metadata_valid(index, metadata))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid local playlist/player entry metadata");
    gboolean transient = spec && index == RADIOS_AUDIO_PREVIEW_DECK && (!spec->media_id || !*spec->media_id) && (!spec->version_id || !*spec->version_id);
    if (!spec || !spec->path || !*spec->path || strlen(spec->path) > 4096 || !g_utf8_validate(spec->path, -1, NULL) ||
        !spec->sha256 || strlen(spec->sha256) != 64 || (!transient && (!spec->media_id || !g_uuid_string_is_valid(spec->media_id) ||
        !spec->version_id || !g_uuid_string_is_valid(spec->version_id))) ||
        (spec->cue_out_frame && spec->cue_out_frame <= spec->cue_in_frame) || spec->cue_in_frame > G_GUINT64_CONSTANT(4147200000) ||
        spec->cue_out_frame > G_GUINT64_CONSTANT(4147200000))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid verified media identity or cue span");
    for (guint i = 0; i < 64; ++i) if (!g_ascii_isxdigit(spec->sha256[i]))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid SHA-256");
    if (metadata && (g_strcmp0(metadata->provider, "local") ||
        g_strcmp0(metadata->source_id, transient ? spec->sha256 : spec->media_id)))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Local playlist source must match the verified media identity");
    g_mutex_lock(&engine->mutex); AudioDeck *deck = &engine->decks[index];
    runner_manual_locked(engine, index); external_unbind_locked(engine, deck);
    local_binding_locked(deck, spec, metadata, NULL, NULL);
    g_free(deck->path); g_free(deck->sha); deck->path = g_strdup(spec->path); deck->sha = g_ascii_strdown(spec->sha256, -1);
    g_strlcpy(deck->media_id, spec->media_id ? spec->media_id : "", sizeof deck->media_id); g_strlcpy(deck->version_id, spec->version_id ? spec->version_id : "", sizeof deck->version_id);
    deck->cue_in = spec->cue_in_frame; deck->cue_out = spec->cue_out_frame;
    memset(&deck->rhythm, 0, sizeof deck->rhythm); deck->tempo_ratio = 1; deck->tempo_return_start_frames = deck->tempo_return_frames = 0; deck->tempo_offset = 0; deck->tempo_source_origin = deck->cue_in; deck->sync_master = -1; deck->sync_manual = FALSE;
    deck->loudness = (RadioSLoudness){0}; deck_gain_locked(engine, deck);
    request_locked(engine, deck, deck->cue_in, FALSE); deck->reopen = TRUE; g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_play(RadioSAudioEngine *engine, guint index, GError **error)
{
    if (!check_deck(engine, index, error)) return FALSE;
    g_mutex_lock(&engine->mutex); AudioDeck *deck = &engine->decks[index];
    runner_manual_locked(engine, index);
    if (deck->external_bound && (!engine->external_token || engine->external_deck != index)) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Bağlı sağlayıcı kendi playlist sürücüsünden başlatılmalı.");
    }
    if (!driver_ready_locked(engine, deck)) { g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_UNAVAILABLE, "Deck output device is not ready"); }
    if (deck->state == RADIOS_AUDIO_EOF) {
        if (deck->sync_manual) manual_sync_reset_locked(deck);
        request_locked(engine, deck, deck->cue_in, TRUE); g_mutex_unlock(&engine->mutex); return TRUE;
    }
    if (deck->state != RADIOS_AUDIO_READY && deck->state != RADIOS_AUDIO_PAUSED && deck->state != RADIOS_AUDIO_LOADING && deck->state != RADIOS_AUDIO_PLAYING) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Deck has no ready audio");
    }
    if (deck->sync_manual && deck->sync_master < 0) {
        guint64 cursor = actual_cursor_locked(engine, deck);
        manual_sync_reset_locked(deck); request_locked(engine, deck, cursor, TRUE);
        g_mutex_unlock(&engine->mutex); return TRUE;
    }
    if (deck->state != RADIOS_AUDIO_PLAYING) deck->phase_mode = deck->sync_master >= 0 ? PHASE_WAIT : PHASE_NONE;
    deck->requested_play = TRUE; deck->pause_after_load = FALSE; deck->error[0] = '\0';
    if (deck->state != RADIOS_AUDIO_LOADING) deck->state = RADIOS_AUDIO_PLAYING;
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_pause(RadioSAudioEngine *engine, guint index, GError **error)
{
    if (!check_deck(engine, index, error)) return FALSE;
    g_mutex_lock(&engine->mutex); AudioDeck *deck = &engine->decks[index];
    runner_manual_locked(engine, index);
    if (deck->state == RADIOS_AUDIO_EMPTY || deck->state == RADIOS_AUDIO_ERROR) { g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Deck has no loaded audio"); }
    deck->requested_play = FALSE;
    deck->reported = actual_cursor_locked(engine, deck);
    if (deck_audible_locked(deck)) schedule_deck_flush_locked(engine, deck);
    if (deck->sync_manual) {
        guint64 cursor = deck->reported;
        manual_sync_reset_locked(deck); request_locked(engine, deck, cursor, FALSE);
    } else if (deck->state != RADIOS_AUDIO_LOADING) deck->state = RADIOS_AUDIO_PAUSED;
    if (deck->state == RADIOS_AUDIO_LOADING) deck->pause_after_load = TRUE;
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
static gboolean seek_command(RadioSAudioEngine *engine, guint index, guint64 frame, gboolean stop,
    guint64 expected_generation, gboolean preserve_play, GError **error)
{
    if (!check_deck(engine, index, error)) return FALSE;
    g_mutex_lock(&engine->mutex); AudioDeck *deck = &engine->decks[index];
    if (expected_generation && deck->generation != expected_generation) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Şarkı değişti; eski konum komutu uygulanmadı.");
    }
    if (deck->external_bound) {
        if (!stop) { g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Sağlayıcı konumu bağlı oynatıcı üzerinden değiştirilmelidir."); }
        if (engine->external_token && engine->external_deck == index) external_clear_locked(engine, NULL);
        schedule_flush_locked(engine); ++deck->generation; deck->binding.generation = deck->generation;
        deck->base = deck->reported = deck->external_consumed = deck->external_received = 0;
        deck->state = RADIOS_AUDIO_READY; deck->requested_play = FALSE;
        g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
    }
    if (!deck->path || (!stop && (frame > G_GUINT64_CONSTANT(4147200000) || frame < deck->cue_in || (deck->cue_out && frame > deck->cue_out)))) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Seek is outside loaded cue span");
    }
    gboolean play = !stop && (deck->requested_play || deck->state == RADIOS_AUDIO_PLAYING);
    /* A paused runner retains its physical PLAYING deck for resume. */
    AudioRunner *owner = index < RADIOS_AUDIO_NORMAL_DECKS ? engine->deck_runner[index] : NULL;
    if (!stop && owner && owner->owned[index] && owner->active) {
        /* Rewind any rendered lookahead before deciding which source is
         * current. Seeking the incoming deck or an overlap is ambiguous. */
        schedule_flush_locked(engine);
        if (owner->current != (gint)index || owner->overlap || owner->cut_remaining || owner->handover_pending || owner->ending ||
            (deck->cue_out && frame >= deck->cue_out)) {
            g_mutex_unlock(&engine->mutex);
            return fail(error, RADIOS_AUDIO_ERROR_STATE, "AutoDJ geçişinde veya sıradaki parçada konum değiştirilemez.");
        }
        AudioRunner *previous = engine->processing_runner; engine->processing_runner = owner;
        runner_adjunct_clear_locked(engine, FALSE); owner->adjunct_completed[owner->index]=TRUE;
        engine->processing_runner = previous;
        seek_locked(engine, deck, frame, FALSE);
        owner->started[index] = FALSE;
        owner->state = owner->paused ? RADIOS_AUDIO_RUN_PAUSED : RADIOS_AUDIO_RUN_PREROLL;
        g_mutex_unlock(&engine->mutex); return TRUE;
    }
    if (preserve_play && owner && owner->paused) play = FALSE;
    runner_manual_locked(engine, index);
    if (!preserve_play) play = !stop && (deck->requested_play || deck->state == RADIOS_AUDIO_PLAYING);
    if (stop && deck->sync_manual) manual_sync_reset_locked(deck);
    if (stop) request_locked(engine, deck, deck->cue_in, FALSE);
    else seek_locked(engine, deck, frame, play);
    g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_stop(RadioSAudioEngine *engine, guint deck, GError **error)
{ return seek_command(engine, deck, 0, TRUE, 0, FALSE, error); }
gboolean radios_audio_engine_unload(RadioSAudioEngine *engine, guint index, GError **error)
{
    if (!check_deck(engine, index, error)) return FALSE;
    g_mutex_lock(&engine->mutex); AudioDeck *deck = &engine->decks[index]; gboolean audible = deck_audible_locked(deck);
    external_unbind_locked(engine, deck); if (audible) schedule_deck_flush_locked(engine, deck);
    runner_manual_locked(engine, index);
    if (deck->cancel) g_cancellable_cancel(deck->cancel);
    g_clear_object(&deck->cancel); deck->cancel = g_cancellable_new();
    ++deck->generation; ++deck->request; deck->requested_play = FALSE; deck->state = RADIOS_AUDIO_EMPTY;
    g_clear_pointer(&deck->path, g_free); g_clear_pointer(&deck->sha, g_free);
    deck->media_id[0] = deck->version_id[0] = deck->error[0] = '\0';
    deck->base = deck->target = deck->reported = deck->cue_in = deck->cue_out = 0; deck->source_eof = FALSE;
    atomic_store(&deck->pcm.read, 0); atomic_store(&deck->pcm.write, 0);
    if (!engine->realtime && index != RADIOS_AUDIO_PREVIEW_DECK) radios_audio_dsp_reset(engine->dsp);
    deck->rhythm = (RadioSRhythmAnalysis){0}; deck->tempo_ratio = 1; deck->tempo_return_start_frames = deck->tempo_return_frames = 0; deck->tempo_offset = 0; deck->tempo_source_origin = deck->cue_in; deck->sync_master = -1; deck->sync_manual = FALSE;
    deck->loudness = (RadioSLoudness){0}; deck_gain_locked(engine, deck);
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_seek(RadioSAudioEngine *engine, guint deck, guint64 frame, GError **error)
{ return seek_command(engine, deck, frame, FALSE, 0, FALSE, error); }
gboolean radios_audio_engine_seek_if_generation(RadioSAudioEngine *engine, guint deck,
    guint64 expected_generation, guint64 frame, GError **error)
{
    if (!expected_generation) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Şarkı yükleme kimliği gerekli.");
    return seek_command(engine, deck, frame, FALSE, expected_generation, TRUE, error);
}
gboolean radios_audio_engine_set_deck_gain(RadioSAudioEngine *engine, guint index, gdouble db, GError **error)
{
    if (!check_deck(engine, index, error)) return FALSE;
    if (!isfinite(db) || db < -60 || db > 6) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Deck gain must be -60..6 dB");
    g_mutex_lock(&engine->mutex); engine->decks[index].gain_db = db;
    deck_gain_locked(engine, &engine->decks[index]); g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_set_preview_output(RadioSAudioEngine *engine,
    RadioSAudioBus bus, gdouble gain_db, gboolean enabled, GError **error)
{
    if (!engine || (bus != RADIOS_AUDIO_BUS_CUE && bus != RADIOS_AUDIO_BUS_MONITOR) ||
        !isfinite(gain_db) || gain_db < -60 || gain_db > 6)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Preview requires CUE/monitor and -60..6 dB gain");
    g_mutex_lock(&engine->mutex);
    if (engine->preview_output_enabled != enabled || (enabled && engine->preview_output_bus != bus)) {
        guint mask = enabled ? (1u << bus) : 0;
        if (engine->preview_output_enabled) mask |= 1u << engine->preview_output_bus;
        schedule_preview_flush_locked(engine, mask);
    }
    engine->preview_output_enabled = enabled; engine->preview_output_bus = bus;
    engine->decks[RADIOS_AUDIO_PREVIEW_DECK].gain_db = gain_db;
    deck_gain_locked(engine, &engine->decks[RADIOS_AUDIO_PREVIEW_DECK]);
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_set_deck_loudness(RadioSAudioEngine *engine, guint index, guint64 generation, const RadioSLoudness *loudness, GError **error)
{
    if (!check_deck(engine, index, error) || !loudness) return FALSE;
    if (loudness->measured && (!isfinite(loudness->integrated_lufs) || loudness->integrated_lufs < -70 || loudness->integrated_lufs > 10 ||
            !isfinite(loudness->peak) || loudness->peak < 0))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Geçerli ses yüksekliği ölçümü gerekli.");
    g_mutex_lock(&engine->mutex); AudioDeck *deck = &engine->decks[index];
    if (generation != deck->generation || !deck->path) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Ölçüm başka bir yükleme nesline ait.");
    }
    deck->loudness = *loudness; deck_gain_locked(engine, deck); g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_set_deck_main(RadioSAudioEngine *engine, guint index, gboolean routed, GError **error)
{
    if (!check_deck(engine, index, error)) return FALSE;
    if (index == RADIOS_AUDIO_PREVIEW_DECK && routed) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Preview deck cannot be routed to MAIN");
    g_mutex_lock(&engine->mutex);
    if (engine->decks[index].main_routed != routed) runner_manual_locked(engine, index);
    /* Routing applies from the next render block without an output flush; a
     * window applying every deck's route used to cause a flush storm. */
    engine->decks[index].main_routed = routed;
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_set_deck_rhythm(RadioSAudioEngine *engine, guint index, guint64 generation, const RadioSRhythmAnalysis *rhythm, GError **error)
{
    if (!check_deck(engine, index, error) || !rhythm) return FALSE;
    if (!isfinite(rhythm->bpm) || rhythm->bpm < 60 || rhythm->bpm > 200 || !isfinite(rhythm->first_beat_ms) || rhythm->first_beat_ms < 0 ||
        !isfinite(rhythm->confidence) || rhythm->confidence < 0.45 || rhythm->confidence > 1 || rhythm->analyzed_ms < 4000 || rhythm->analyzed_ms > 120000 || rhythm->first_beat_ms >= rhythm->analyzed_ms ||
        (rhythm->has_downbeat && (!isfinite(rhythm->downbeat_ms) || rhythm->downbeat_ms < 0 || rhythm->downbeat_ms >= rhythm->analyzed_ms)))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Güvenilir BPM / beatgrid analizi gerekli.");
    g_mutex_lock(&engine->mutex); AudioDeck *deck = &engine->decks[index];
    if (generation != deck->generation || !deck->path || (deck->cue_out && rhythm->first_beat_ms * 48 >= deck->cue_out)) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Analiz başka bir yükleme nesline ait.");
    }
    if (index < RADIOS_AUDIO_NORMAL_DECKS && engine->deck_runner[index] && engine->deck_runner[index]->owned[index]) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Çalan AutoDJ / yayın sırasının donmuş beatgrid'i değiştirilemez.");
    }
    deck->rhythm = *rhythm; g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_sync(RadioSAudioEngine *engine, guint index, gint master, gboolean enabled, GError **error)
{
    if (!check_deck(engine, index, error) || index >= RADIOS_AUDIO_NORMAL_DECKS) return FALSE;
    if (enabled && (master < 0 || master >= (gint)RADIOS_AUDIO_NORMAL_DECKS || master == (gint)index))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "SYNC için farklı bir ana oynatıcı seçin.");
    g_mutex_lock(&engine->mutex); AudioDeck *deck = &engine->decks[index];
    AudioRunner *owner = engine->deck_runner[index];
    if (enabled && (deck->state != RADIOS_AUDIO_PLAYING || (owner && owner->paused))) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "SYNC yalnızca Play durumundaki oynatıcıda etkinleştirilebilir.");
    }
    if (owner && (owner->overlap || owner->paused)) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Geçiş veya duraklatılmış AutoDJ sırasında SYNC yeniden kurulamaz.");
    }
    if (!deck->path || (deck->state != RADIOS_AUDIO_READY && deck->state != RADIOS_AUDIO_PLAYING && deck->state != RADIOS_AUDIO_PAUSED)) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Önce doğrulanmış parçayı oynatıcıya yükleyin.");
    }
    gdouble ratio = 1; guint64 target = actual_cursor_locked(engine, deck);
    if (enabled) {
        AudioDeck *reference = &engine->decks[master];
        gint current = master;
        for (guint step = 0; current >= 0 && step < RADIOS_AUDIO_NORMAL_DECKS; ++step) {
            if (current == (gint)index) { g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_INVALID, "SYNC bağlantısı döngü oluşturamaz."); }
            current = engine->decks[current].sync_master;
        }
        if (deck->rhythm.confidence < 0.45 || reference->rhythm.confidence < 0.45 ||
            (reference->state != RADIOS_AUDIO_READY && reference->state != RADIOS_AUDIO_PLAYING && reference->state != RADIOS_AUDIO_PAUSED) ||
            !radios_rhythm_tempo_ratio(deck->rhythm.bpm, reference->rhythm.bpm * deck_current_ratio(reference), &ratio)) {
            g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "İki deckte güvenilir BPM analizi ve ±%16 içinde tempo eşleşmesi gerekli.");
        }
        gdouble period = 48000 * 60 / deck->rhythm.bpm;
        gdouble master_beats = (actual_cursor_locked(engine, reference) - reference->rhythm.first_beat_ms * 48) * reference->rhythm.bpm / (48000 * 60);
        gdouble desired_phase = master_beats * (deck->rhythm.bpm * ratio / (reference->rhythm.bpm * deck_current_ratio(reference)));
        gdouble source_beats = ((gdouble)target - deck->rhythm.first_beat_ms * 48) / period;
        gdouble difference = remainder(desired_phase - source_beats, 1);
        gdouble aligned = target + difference * period;
        while (aligned < deck->cue_in) aligned += period;
        if (deck->cue_out && aligned >= deck->cue_out) aligned -= period;
        if (aligned < deck->cue_in || (deck->cue_out && aligned >= deck->cue_out)) {
            g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Cue aralığı beat hizalaması için çok kısa.");
        }
        target = (guint64)llround(aligned);
    }
    gboolean play = deck->state == RADIOS_AUDIO_PLAYING;
    schedule_flush_locked(engine); deck->tempo_ratio = ratio;
    deck->tempo_return_start_frames = deck->tempo_return_frames = 0; deck->tempo_offset = 0; deck->tempo_source_origin = deck->cue_in;
    deck->sync_master = enabled ? master : -1; deck->sync_manual = enabled;
    request_locked(engine, deck, target, play);
    /* Reloading a playing deck takes time while the master moves on; the
     * render gate drops that much audio when the deck resumes. */
    deck->phase_mode = enabled && play ? PHASE_SKIP : PHASE_NONE;
    if (owner && owner->active && owner->owned[index]) {
        owner->started[index] = FALSE; owner->state = RADIOS_AUDIO_RUN_PREROLL;
    }
    g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_set_bus_gate(RadioSAudioEngine *engine, RadioSAudioBus bus, gboolean gate, GError **error)
{
    if (!engine || bus < 0 || bus >= RADIOS_AUDIO_BUS_COUNT) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid output bus");
    g_mutex_lock(&engine->mutex);
    if (engine->gates[bus] != gate) schedule_flush_locked(engine);
    engine->gates[bus] = gate;
    if (bus == RADIOS_AUDIO_BUS_MAIN && !gate)
        external_clear_locked(engine, "MAIN kapatıldı; dış kaynak bağlantısı durduruldu.");
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_bind_external_player(RadioSAudioEngine *engine, RadioSAudioPlayerBinding *binding, GError **error)
{
    if (!engine || !binding_valid(binding)) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Geçerli playlist, girdi ve bağlı oynatıcı gerekli.");
    g_mutex_lock(&engine->mutex); AudioDeck *deck = &engine->decks[binding->deck];
    if (engine->closing || (binding->generation && binding->generation != deck->generation) ||
        (engine->deck_runner[binding->deck] && (engine->deck_runner[binding->deck]->owned[binding->deck] ||
            engine->deck_runner[binding->deck]->adjunct_owned[binding->deck])) ||
        deck->state == RADIOS_AUDIO_PLAYING || deck->state == RADIOS_AUDIO_LOADING || deck->requested_play) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Oynatıcı etkin veya yükleme kimliği değişti.");
    }
    gboolean replacing = deck->external_bound || deck->state != RADIOS_AUDIO_EMPTY;
    external_unbind_locked(engine, deck); if (replacing) schedule_flush_locked(engine);
    if (deck->cancel) g_cancellable_cancel(deck->cancel);
    g_clear_object(&deck->cancel); deck->cancel = g_cancellable_new();
    ++deck->generation; ++deck->request; binding->generation = deck->generation;
    g_clear_pointer(&deck->path, g_free); g_clear_pointer(&deck->sha, g_free);
    deck->media_id[0] = deck->version_id[0] = deck->error[0] = 0;
    deck->binding = *binding; deck->external_bound = TRUE;
    deck->base = deck->target = deck->reported = deck->cue_in = 0; deck->cue_out = binding->duration_frames;
    deck->external_received = deck->external_consumed = 0; deck->source_eof = FALSE; deck->requested_play = FALSE;
    deck->tempo_ratio = 1; deck->tempo_return_start_frames = deck->tempo_return_frames = 0; deck->tempo_offset = 0; deck->tempo_source_origin = deck->cue_in; deck->sync_master = -1; deck->sync_manual = FALSE; deck->rhythm = (RadioSRhythmAnalysis){0}; deck->state = RADIOS_AUDIO_READY;
    deck->loudness = (RadioSLoudness){0}; deck_gain_locked(engine, deck);
    atomic_store(&deck->pcm.read, 0); atomic_store(&deck->pcm.write, 0);
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
guint64 radios_audio_engine_external_begin(RadioSAudioEngine *engine, GError **error)
{
    (void)engine; fail(error, RADIOS_AUDIO_ERROR_INVALID, "Dış kaynak başlamadan önce playlist ve oynatıcı bağı gerekli."); return 0;
}
guint64 radios_audio_engine_external_begin_player(RadioSAudioEngine *engine, const RadioSAudioPlayerBinding *binding, GError **error)
{
    return radios_audio_engine_external_begin_player_at(engine, binding, 0, error);
}
guint64 radios_audio_engine_external_begin_player_at(RadioSAudioEngine *engine, const RadioSAudioPlayerBinding *binding,
    guint64 start_frame, GError **error)
{
    if (!engine || !binding_valid(binding) || start_frame > G_GUINT64_CONSTANT(4147200000) ||
        (binding->duration_frames && start_frame > binding->duration_frames)) { fail(error, RADIOS_AUDIO_ERROR_INVALID, "Geçerli playlist ve oynatıcı bağı gerekli."); return 0; }
    g_mutex_lock(&engine->mutex);
    if (!binding_matches_locked(engine, binding)) { g_mutex_unlock(&engine->mutex); fail(error, RADIOS_AUDIO_ERROR_STATE, "Playlist girdisi veya oynatıcı değişti."); return 0; }
    if (engine->closing || !engine->gates[0] || (engine->realtime &&
            (!engine->physical.connected || !engine->physical.ready[0] || engine->flush_pending))) {
        g_mutex_unlock(&engine->mutex); fail(error, RADIOS_AUDIO_ERROR_UNAVAILABLE, "Bağlı oynatıcı için MAIN ses çıkışı hazır olmalı."); return 0;
    }
    if (engine->external_token) { g_mutex_unlock(&engine->mutex); fail(error, RADIOS_AUDIO_ERROR_STATE, "Bağlı dış kaynak bağlantısı zaten açık."); return 0; }
    atomic_store(&engine->external.read, atomic_load(&engine->external.write));
    engine->external_received = 0; engine->external_error[0] = 0; engine->external_deck = binding->deck;
    if (!++engine->external_serial) ++engine->external_serial;
    guint64 token = engine->external_token = engine->external_serial;
    AudioDeck *deck = &engine->decks[binding->deck];
    deck->base = deck->target = deck->reported = start_frame;
    deck->external_received = deck->external_consumed = 0;
    for (guint i = 0; i < HISTORY_COUNT; ++i) engine->history[i].generation[binding->deck] = 0;
    if (deck->state != RADIOS_AUDIO_PAUSED) deck->state = RADIOS_AUDIO_PLAYING;
    deck->requested_play = deck->state == RADIOS_AUDIO_PLAYING;
    g_mutex_unlock(&engine->mutex); return token;
}
gboolean radios_audio_engine_external_push(RadioSAudioEngine *engine, guint64 token,
    const gfloat *stereo, guint frames, GError **error)
{
    if (!engine || !token || !stereo || !frames || frames > 4096)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Dış kaynak PCM bloğu geçersiz.");
    for (guint i = 0; i < frames * 2; ++i) if (!isfinite(stereo[i]) || fabsf(stereo[i]) > 8)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Dış kaynak PCM örneği geçersiz.");
    g_mutex_lock(&engine->mutex);
    if (engine->closing || engine->external_token != token || engine->external_deck >= RADIOS_AUDIO_NORMAL_DECKS ||
        !engine->decks[engine->external_deck].external_bound ||
        engine->decks[engine->external_deck].binding.generation != engine->decks[engine->external_deck].generation ||
        engine->decks[engine->external_deck].state != RADIOS_AUDIO_PLAYING) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Dış kaynak oturumu kapandı veya değişti.");
    }
    if (!engine->gates[0] || (engine->realtime && (!engine->physical.connected || !engine->physical.ready[0]))) {
        external_clear_locked(engine, "MAIN ses çıkışı kullanılamıyor; dış kaynak durduruldu.");
        schedule_flush_locked(engine); g_cond_broadcast(&engine->condition);
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_UNAVAILABLE, "MAIN ses çıkışı kullanılamıyor.");
    }
    guint64 queued = atomic_load(&engine->external.write) - atomic_load(&engine->external.read);
    if (frames > engine->external.capacity - queued) {
        external_clear_locked(engine, "Dış kaynak kuyruğu doldu; ses kaybı yerine bağlantı durduruldu.");
        schedule_flush_locked(engine); g_cond_broadcast(&engine->condition);
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_UNAVAILABLE, "Dış kaynak ses kuyruğu doldu.");
    }
    (void)radios_audio_ring_write(&engine->external, stereo, frames, 0);
    engine->external_received += frames; engine->decks[engine->external_deck].external_received += frames;
    g_cond_broadcast(&engine->condition);
    g_mutex_unlock(&engine->mutex); return TRUE;
}
void radios_audio_engine_external_end(RadioSAudioEngine *engine, guint64 token)
{
    if (!engine || !token) return;
    g_mutex_lock(&engine->mutex);
    if (engine->external_token == token) {
        external_clear_locked(engine, NULL); schedule_flush_locked(engine);
        g_cond_broadcast(&engine->condition);
    }
    g_mutex_unlock(&engine->mutex);
}
/* The on-air gate is latch || ptt. Opening needs the microphone and the MAIN
 * device; closing always succeeds. Neither request touches any deck's
 * transport, so releasing the talk key never skips or stops a track. An
 * offline engine has no devices and no capture PCM; it accepts the gate so
 * the latch/push-to-talk truth table is testable without a sound server. */
static gboolean microphone_request_locked(RadioSAudioEngine *engine, gboolean latch, gboolean ptt, GError **error)
{
    gboolean gate = latch || ptt;
    if (gate && !engine->microphone_gate && engine->realtime && (!engine->physical.microphone_ready || !engine->physical.ready[0]))
        return fail(error, RADIOS_AUDIO_ERROR_UNAVAILABLE, "Microphone and MAIN device must be ready");
    /* The new gate takes effect from the next render block. Flushing the
     * outputs here punched 36-190 ms holes into the song on MAIN at every
     * L/R press and release (PDF p.8, p.15); the queue latency is far shorter. */
    engine->microphone_latch = latch; engine->microphone_ptt = ptt;
    engine->microphone_gate = gate; g_atomic_int_set(&engine->gate_mirror, gate);
    if (gate) atomic_store(&engine->capture.read, atomic_load(&engine->capture.write));
    g_cond_broadcast(&engine->condition); return TRUE;
}
gboolean radios_audio_engine_set_microphone_gate(RadioSAudioEngine *engine, gboolean gate, GError **error)
{
    if (!engine) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid audio engine");
    g_mutex_lock(&engine->mutex);
    gboolean accepted = microphone_request_locked(engine, gate, engine->microphone_ptt, error);
    g_mutex_unlock(&engine->mutex); return accepted;
}
gboolean radios_audio_engine_set_microphone_ptt(RadioSAudioEngine *engine, gboolean held, GError **error)
{
    if (!engine) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid audio engine");
    g_mutex_lock(&engine->mutex);
    gboolean accepted = microphone_request_locked(engine, engine->microphone_latch, held, error);
    g_mutex_unlock(&engine->mutex); return accepted;
}
static void taps_changed_locked(RadioSAudioEngine *engine)
{
    gboolean channels = FALSE;
    for (guint t = 0; t < ENGINE_TAPS; ++t) channels |= engine->taps[t] && engine->taps[t]->source >= 0;
    radios_audio_dsp_capture_channels(engine->dsp, channels);
}
RadioSAudioTap *radios_audio_engine_tap_open(RadioSAudioEngine *engine, gint source, GError **error)
{
    if (!engine || source < RADIOS_AUDIO_TAP_MICROPHONE || source >= (gint)RADIOS_AUDIO_MIXER_CHANNELS) {
        fail(error, RADIOS_AUDIO_ERROR_INVALID, "Tap source must be MAIN, the microphone or mixer channel 1..32"); return NULL;
    }
    RadioSAudioTap *tap = g_new0(RadioSAudioTap, 1); tap->source = source; ring_init(&tap->ring, TAP_CAPACITY, FALSE);
    g_mutex_lock(&engine->mutex);
    for (guint t = 0; t < ENGINE_TAPS; ++t) if (!engine->taps[t] && !engine->closing) {
        engine->taps[t] = tap; tap->engine = radios_audio_engine_ref(engine); taps_changed_locked(engine);
        g_mutex_unlock(&engine->mutex); return tap;
    }
    g_mutex_unlock(&engine->mutex); ring_clear(&tap->ring); g_free(tap);
    fail(error, RADIOS_AUDIO_ERROR_UNAVAILABLE, "Too many live audio taps"); return NULL;
}
guint radios_audio_tap_read(RadioSAudioTap *tap, gfloat *stereo, guint frames)
{ return tap && stereo ? radios_audio_ring_read(&tap->ring, stereo, frames, NULL) : 0; }
guint radios_audio_tap_available(RadioSAudioTap *tap)
{ return tap ? (guint)MIN(atomic_load(&tap->ring.write) - atomic_load(&tap->ring.read), (guint64)G_MAXUINT) : 0; }
void radios_audio_tap_skip(RadioSAudioTap *tap, guint frames)
{
    if (!tap) return;
    guint64 read = atomic_load(&tap->ring.read), queued = atomic_load(&tap->ring.write) - read;
    atomic_store(&tap->ring.read, read + MIN((guint64)frames, queued));
}
void radios_audio_tap_close(RadioSAudioTap *tap)
{
    if (!tap) return;
    RadioSAudioEngine *engine = tap->engine;
    g_mutex_lock(&engine->mutex);
    for (guint t = 0; t < ENGINE_TAPS; ++t) if (engine->taps[t] == tap) engine->taps[t] = NULL;
    taps_changed_locked(engine); g_mutex_unlock(&engine->mutex);
    ring_clear(&tap->ring); g_free(tap); radios_audio_engine_unref(engine);
}
RadioSAudioLine *radios_audio_engine_line_open(RadioSAudioEngine *engine, gint target, GError **error)
{
    if (!engine || target < -1 || target >= (gint)RADIOS_AUDIO_MIXER_CHANNELS) {
        fail(error, RADIOS_AUDIO_ERROR_INVALID, "Line target must be MAIN or mixer channel 1..32"); return NULL;
    }
    RadioSAudioLine *line = g_new0(RadioSAudioLine, 1); line->target = target; ring_init(&line->ring, TAP_CAPACITY, FALSE);
    g_mutex_lock(&engine->mutex);
    for (guint l = 0; l < ENGINE_LINES; ++l) if (!engine->lines[l] && !engine->closing) {
        engine->lines[l] = line; line->engine = radios_audio_engine_ref(engine);
        g_mutex_unlock(&engine->mutex); return line;
    }
    g_mutex_unlock(&engine->mutex); ring_clear(&line->ring); g_free(line);
    fail(error, RADIOS_AUDIO_ERROR_UNAVAILABLE, "Too many live audio lines"); return NULL;
}
guint radios_audio_line_write(RadioSAudioLine *line, const gfloat *stereo, guint frames)
{ return line && stereo ? radios_audio_ring_write(&line->ring, stereo, frames, 0) : 0; }
guint radios_audio_line_queued(RadioSAudioLine *line)
{ return line ? (guint)MIN(atomic_load(&line->ring.write) - atomic_load(&line->ring.read), (guint64)G_MAXUINT) : 0; }
void radios_audio_line_close(RadioSAudioLine *line)
{
    if (!line) return;
    RadioSAudioEngine *engine = line->engine;
    g_mutex_lock(&engine->mutex);
    for (guint l = 0; l < ENGINE_LINES; ++l) if (engine->lines[l] == line) engine->lines[l] = NULL;
    g_mutex_unlock(&engine->mutex);
    ring_clear(&line->ring); g_free(line); radios_audio_engine_unref(engine);
}
guint64 radios_audio_engine_output_flushes(RadioSAudioEngine *engine)
{
    if (!engine) return 0;
    g_mutex_lock(&engine->mutex); guint64 count = engine->flushes_done; g_mutex_unlock(&engine->mutex); return count;
}
gboolean radios_audio_engine_microphone_open(RadioSAudioEngine *engine)
{ return engine && g_atomic_int_get(&engine->gate_mirror); }
gboolean radios_audio_engine_render(RadioSAudioEngine *engine, guint frames, gfloat *main, gfloat *cue, gfloat *monitor, GError **error)
{
    if (!engine || engine->realtime || !frames || frames > RADIOS_AUDIO_MAX_READ_FRAMES || !main || !cue || !monitor)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Manual render requires offline engine and 1..4096 frames");
    gfloat *outputs[] = {main, cue, monitor}; g_mutex_lock(&engine->mutex);
    loudness_clock_locked(engine, g_get_real_time());
    render_locked(engine, frames, outputs); g_mutex_unlock(&engine->mutex); return TRUE;
}
static gboolean mixer_locked(RadioSAudioEngine *engine, const RadioSAudioMixerConfig *config, GError **error)
{
    if (!radios_audio_mixer_config_validate(config, error)) return FALSE;
    RadioSAudioMixerConfig before; radios_audio_dsp_get_config(engine->dsp, &before);
    gdouble manual[RADIOS_AUDIO_MIXER_CHANNELS];
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i) {
        /* Most callers read/modify/write a whole effective snapshot. An
         * unchanged automatic fader is not a new manual baseline. */
        manual[i] = !engine->channel_auto_gain[i] || config->channels[i].gain_db != before.channels[i].gain_db ?
            config->channels[i].gain_db : engine->manual_channel_gain[i];
    }
    /* A changed channel source applies from the next render block; it never
     * flushes (and reopens) the outputs of everything else on air. */
    for (guint d = 0; d < 32; ++d) if (active_runner(engine)->owned[d]) {
        gboolean mapped = FALSE;
        for (guint c = 0; c < 32; ++c) mapped |= config->channels[c].source_deck == (gint)d;
        if (!mapped) { runner_manual_locked(engine, d); break; }
    }
    /* A changed physical mapping revokes only the affected jingle lease,
     * including a private scope. Silent auxiliary PCM must never duck music. */
    for (guint slot=0;slot<33;++slot) {
        AudioRunner *run=existing_runner(engine,slot);if (!run) continue;
        for (guint d=0;d<32;++d) if (run->adjunct_owned[d]) {
            gboolean mapped=FALSE;
            for (guint c=0;c<32;++c) mapped|=config->channels[c].source_deck==(gint)d;
            if (!mapped) runner_manual_locked(engine,d);
        }
    }
    gboolean accepted = radios_audio_dsp_configure(engine->dsp, config, error);
    if (accepted) {
        engine->mixer = *config;
        memcpy(engine->manual_channel_gain, manual, sizeof manual);
        for (guint c = 0; c < RADIOS_AUDIO_MIXER_CHANNELS; ++c) channel_loudness_locked(engine, c);
    }
    g_cond_broadcast(&engine->condition); return accepted;
}
gboolean radios_audio_engine_apply_mixer(RadioSAudioEngine *engine, const RadioSAudioMixerConfig *config, GError **error)
{
    if (!engine || !config) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid mixer configuration");
    g_mutex_lock(&engine->mutex); gboolean accepted = mixer_locked(engine, config, error); g_mutex_unlock(&engine->mutex); return accepted;
}
void radios_audio_engine_get_mixer(RadioSAudioEngine *engine, RadioSAudioMixerConfig *config)
{ g_return_if_fail(engine && config); g_mutex_lock(&engine->mutex); radios_audio_dsp_get_config(engine->dsp, config); g_mutex_unlock(&engine->mutex); }
void radios_audio_engine_get_manual_mixer(RadioSAudioEngine *engine, RadioSAudioMixerConfig *config)
{
    g_return_if_fail(engine && config);
    g_mutex_lock(&engine->mutex); *config = engine->mixer;
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i) config->channels[i].gain_db = engine->manual_channel_gain[i];
    g_mutex_unlock(&engine->mutex);
}
gboolean radios_audio_engine_set_channel_dsp(RadioSAudioEngine *engine, guint channel, const RadioSAudioChannelDsp *settings, GError **error)
{
    if (!engine || !settings || channel >= 32) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid mixer channel");
    g_mutex_lock(&engine->mutex); RadioSAudioMixerConfig config; radios_audio_dsp_get_config(engine->dsp, &config); config.channels[channel] = *settings;
    gboolean accepted = mixer_locked(engine, &config, error); g_mutex_unlock(&engine->mutex); return accepted;
}
gboolean radios_audio_engine_set_master_dsp(RadioSAudioEngine *engine, const RadioSAudioMasterDsp *settings, GError **error)
{
    if (!engine || !settings) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid master channel");
    g_mutex_lock(&engine->mutex); RadioSAudioMixerConfig config; radios_audio_dsp_get_config(engine->dsp, &config); config.master = *settings;
    gboolean accepted = mixer_locked(engine, &config, error); g_mutex_unlock(&engine->mutex); return accepted;
}
gboolean radios_audio_engine_set_fx(RadioSAudioEngine *engine, guint slot, const RadioSAudioFx *settings, GError **error)
{
    if (!engine || !settings || slot >= 5) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid effect slot");
    g_mutex_lock(&engine->mutex); RadioSAudioMixerConfig config; radios_audio_dsp_get_config(engine->dsp, &config); config.effects[slot] = *settings;
    gboolean accepted = mixer_locked(engine, &config, error); g_mutex_unlock(&engine->mutex); return accepted;
}
gboolean radios_audio_engine_set_master_fx(RadioSAudioEngine *engine, const RadioSAudioFx *settings, GError **error)
{
    if (!engine || !settings) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid master effect");
    g_mutex_lock(&engine->mutex); RadioSAudioMixerConfig config; radios_audio_dsp_get_config(engine->dsp, &config); config.master_fx = *settings;
    gboolean accepted = mixer_locked(engine, &config, error); g_mutex_unlock(&engine->mutex); return accepted;
}
gboolean radios_audio_engine_set_crossfader(RadioSAudioEngine *engine, gdouble position, GError **error)
{
    if (!engine) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid audio engine");
    g_mutex_lock(&engine->mutex); RadioSAudioMixerConfig config; radios_audio_dsp_get_config(engine->dsp, &config); config.crossfader = position;
    gboolean accepted = mixer_locked(engine, &config, error); g_mutex_unlock(&engine->mutex); return accepted;
}
gboolean radios_audio_engine_set_ducking(RadioSAudioEngine *engine, gboolean enabled, gdouble bed_db, GError **error)
{
    if (!engine) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid audio engine");
    g_mutex_lock(&engine->mutex); RadioSAudioMixerConfig config; radios_audio_dsp_get_config(engine->dsp, &config);
    config.speech_ducking = enabled; config.speech_bed_db = bed_db;
    gboolean accepted = mixer_locked(engine, &config, error); g_mutex_unlock(&engine->mutex); return accepted;
}

void radios_audio_run_options_default(RadioSAudioRunOptions *options)
{
    g_return_if_fail(options); memset(options, 0, sizeof *options);
    options->pool_count = 2; options->deck_pool[0] = 0; options->deck_pool[1] = 1;
    options->finish_grace_frames = 30u * 60u * 48000u;
    options->loudness_context = RADIOS_AUDIO_LOUDNESS_MANUAL;
}
static gboolean runner_has_leases(const AudioRunner *run)
{ for (guint i = 0; i < 32; ++i) if (run->owned[i] || run->adjunct_owned[i]) return TRUE; return FALSE; }
static void runner_receipts_locked(RadioSAudioEngine *engine)
{
    AudioRunner *run = active_runner(engine);
    guint64 played = engine->realtime ? engine->physical.played_tag[0] : engine->next_tag - 1;
    guint ready[32], count = 0; guint64 positions[32];
    for (guint deck = 0; deck < 32; ++deck) {
        gint index = run->loaded_index[deck];
        if (!run->owned[deck] || index < 0 || !run->first_tag[deck] || played < run->first_tag[deck]) continue;
        guint64 position = actual_cursor_locked(engine, &engine->decks[deck]);
        if (position <= run->items[index].spec.media.cue_in_frame) continue;
        positions[deck] = position; guint at = count++;
        while (at && run->first_tag[ready[at - 1]] > run->first_tag[deck]) {
            ready[at] = ready[at - 1]; --at;
        }
        ready[at] = deck;
    }
    /* Multiple short items can finish between observations. Source tag order,
     * rather than a physical deck number, defines their playback occurrences. */
    for (guint i = 0; i < count; ++i) {
        guint deck = ready[i]; gint index = run->loaded_index[deck];
        guint64 occurrence = run->receipt_occurrence[deck];
        if (!occurrence) {
            occurrence = ++run->receipt_write;
            run->receipt_occurrence[deck] = occurrence;
            RadioSAudioRunReceipt *created = &run->receipts[(occurrence - 1) % RADIOS_AUDIO_RUN_MAX_RECEIPTS];
            *created = (RadioSAudioRunReceipt){.epoch = run->epoch, .occurrence = occurrence,
                .index = (guint)index, .first_utc_us = g_get_real_time()};
        }
        RadioSAudioRunReceipt *receipt = &run->receipts[(occurrence - 1) % RADIOS_AUDIO_RUN_MAX_RECEIPTS];
        if (receipt->occurrence != occurrence || receipt->terminal != RADIOS_AUDIO_RUN_TERMINAL_NONE) continue;
        if (positions[deck] > receipt->position_frames) {
            receipt->position_frames = positions[deck]; receipt->last_observed_utc_us = g_get_real_time();
        }
        if (run->retire_tag[deck] && played >= run->retire_tag[deck]) {
            RadioSAudioRunTerminal terminal = run->retire_terminal[deck];
            if (terminal == RADIOS_AUDIO_RUN_TERMINAL_COMPLETE &&
                receipt->position_frames < runner_out(&engine->decks[deck], &run->items[index].spec)) continue;
            receipt->terminal = terminal; receipt->last_observed_utc_us = g_get_real_time();
        }
    }
}
/* Explicit control cancellation freezes the last confirmed source position
 * before output flush drops queued PCM. Unconsumed preloads create no event. */
static void runner_terminal_locked(RadioSAudioEngine *engine, RadioSAudioRunTerminal terminal)
{
    AudioRunner *run = active_runner(engine); runner_receipts_locked(engine);
    for (guint deck = 0; deck < 32; ++deck) {
        guint64 occurrence = run->receipt_occurrence[deck];
        if (!run->owned[deck] || !occurrence) continue;
        RadioSAudioRunReceipt *receipt = &run->receipts[(occurrence - 1) % RADIOS_AUDIO_RUN_MAX_RECEIPTS];
        if (receipt->occurrence != occurrence || receipt->terminal != RADIOS_AUDIO_RUN_TERMINAL_NONE) continue;
        receipt->terminal = terminal; receipt->last_observed_utc_us = g_get_real_time();
    }
}
static RunMark runner_mark(const AudioRunner *run, guint offset)
{
    RunMark mark = {offset, run->index, run->index + 1, run->current, run->next, run->state, run->transition, run->transition_total,
        run->current >= 0 && run->started[run->current], run->next >= 0 && run->started[run->next], run->pending_stop != FALSE,
        run->overlap != FALSE, run->cut_remaining, run->cut_total, run->pending_jump};
    if (run->next >= 0 && run->loaded_index[run->next] >= 0) mark.next_index = (guint)run->loaded_index[run->next];
    return mark;
}
static void runner_record_locked(AudioRunner *run, RenderHistory *history, guint offset)
{
    history->runs[run->slot].epoch = run->epoch; RunMark mark = runner_mark(run, offset);
    if (history->runs[run->slot].mark_count) {
        const RunMark *before = &history->runs[run->slot].marks[history->runs[run->slot].mark_count - 1];
        if (before->index == mark.index && before->next_index == mark.next_index && before->current == mark.current &&
            before->next == mark.next && before->state == mark.state && before->overlap == mark.overlap) return;
    }
    if (history->runs[run->slot].mark_count < G_N_ELEMENTS(history->runs[run->slot].marks)) history->runs[run->slot].marks[history->runs[run->slot].mark_count++] = mark;
}
static gboolean runner_audible_locked(RadioSAudioEngine *engine, RunMark *result)
{
    AudioRunner *run = active_runner(engine); gboolean found = FALSE;
    guint64 tag = engine->realtime ? engine->physical.played_tag[0] : engine->next_tag - 1;
    guint64 count = MIN(engine->history_write, HISTORY_COUNT);
    for (guint64 n = 0; n < count; ++n) {
        RenderHistory *history = &engine->history[(engine->history_write - count + n) % HISTORY_COUNT];
        if (history->runs[run->slot].epoch != run->epoch || history->first_tag > tag) continue;
        guint64 played = MIN((guint64)history->frames, tag - history->first_tag + 1);
        for (guint i = 0; i < history->runs[run->slot].mark_count; ++i) if (history->runs[run->slot].marks[i].offset < played) {
            RunMark *mark = &history->runs[run->slot].marks[i];
            /* A seek creates a new decoder generation. Old run marks must
             * never rewind the current queue to the pre-seek transport. */
            if (mark->current >= 0 && history->generation[mark->current] != engine->decks[mark->current].generation) continue;
            *result = *mark; found = TRUE;
            if (result->state == RADIOS_AUDIO_RUN_OVERLAP) result->transition += played - result->offset;
            /* A paused run freezes its cut just as it freezes its overlap. */
            if (result->cut_remaining && result->state != RADIOS_AUDIO_RUN_PAUSED)
                result->cut_remaining -= (guint)MIN((guint64)result->cut_remaining, played - result->offset);
        }
    }
    /* What was rendered while paused is still queued when the run resumes:
     * after the resume it is output latency, not a paused queue. The heard
     * point then reports the state the resume restored, so a resumed run
     * never reads PAUSED again in its epoch and a rewind keeps it running. */
    if (found && result->state == RADIOS_AUDIO_RUN_PAUSED && !run->paused)
        result->state = result->overlap ? RADIOS_AUDIO_RUN_OVERLAP : RADIOS_AUDIO_RUN_PLAYING;
    return found;
}
static void runner_rewind_locked(RadioSAudioEngine *engine)
{
    AudioRunner *run = active_runner(engine); RunMark point;
    if (!run->active || !runner_audible_locked(engine, &point)) return;
    run->index = point.index; run->current = point.current; run->next = point.next;
    run->transition = point.transition; run->transition_total = point.transition_total;
    run->overlap = point.overlap;
    run->cut_remaining = point.cut_remaining; run->cut_total = point.cut_total;
    run->pending_jump = point.pending_jump; run->pending_stop = point.pending_stop;
    if (point.state != RADIOS_AUDIO_RUN_FINISHED) run->final_tag = 0;
    if (!run->paused) run->state = point.state;
    for (guint d=0;d<32;++d) if (run->adjunct[d].generation && engine->decks[d].generation==run->adjunct[d].generation) {
        AudioDeck *voice=&engine->decks[d];
        if (actual_cursor_locked(engine,voice)<voice->cue_out &&
            (run->adjunct[d].item==(gint)run->index || (run->next>=0 && run->adjunct[d].item==run->loaded_index[run->next]))) {
            run->adjunct[d].retire_tag=0;run->adjunct_completed[run->adjunct[d].item]=FALSE;
        }
    }
    if (run->current >= 0) { run->retire_tag[run->current] = 0; run->retire_terminal[run->current] = RADIOS_AUDIO_RUN_TERMINAL_NONE; run->started[run->current] = point.current_started; }
    if (run->next >= 0) { run->retire_tag[run->next] = 0; run->retire_terminal[run->next] = RADIOS_AUDIO_RUN_TERMINAL_NONE; run->started[run->next] = point.next_started; }
}
static void runner_cancel_locked(RadioSAudioEngine *engine, gint preserve)
{
    AudioRunner *run = active_runner(engine);
    runner_adjunct_clear_locked(engine, TRUE);
    run->handover_pending = run->handover_active = run->handover_cut = FALSE;
    run->handover_completed = FALSE;
    run->loudness_pair_source = run->loudness_pair_target = -1;
    if (!runner_has_leases(run)) { run->active = FALSE; run->state = RADIOS_AUDIO_RUN_IDLE; return; }
    runner_terminal_locked(engine, RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED);
    schedule_flush_locked(engine); run->active = FALSE; run->paused = FALSE; ++run->epoch;
    for (guint i = 0; i < 32; ++i) if (run->owned[i]) {
        if ((gint)i == preserve) {
            AudioDeck *deck = &engine->decks[i]; deck->reported = actual_cursor_locked(engine, deck); deck->requested_play = FALSE;
            if (deck->state != RADIOS_AUDIO_LOADING && deck->state != RADIOS_AUDIO_ERROR && deck->state != RADIOS_AUDIO_EMPTY) deck->state = RADIOS_AUDIO_PAUSED;
            manual_sync_idle_locked(deck);
            run->loaded_index[i] = -1; run->retire_tag[i] = 0;
        } else runner_clear_deck_locked(engine, i);
        run->owned[i] = FALSE;
    }
    run->current = run->next = run->pending_jump = -1; run->overlap = run->ending = run->pending_stop = FALSE;
    run->cut_remaining = 0; run->state = RADIOS_AUDIO_RUN_IDLE; run->error[0] = '\0';
    g_cond_broadcast(&engine->condition);
}
static void runner_manual_locked(RadioSAudioEngine *engine, guint deck)
{
    if (deck >= 32) return;
    AudioRunner *previous = engine->processing_runner;
    for (guint slot = 0; slot < 33; ++slot) {
        AudioRunner *run = existing_runner(engine,slot);
        if (run && run->owned[deck]) { engine->processing_runner = run; runner_cancel_locked(engine,(gint)deck); }
        else if (run && run->adjunct_owned[deck]) {
            engine->processing_runner = run;
            if (run->adjunct[deck].generation) {
                run->adjunct_completed[run->adjunct[deck].item]=TRUE;
                /* Preserve the verified loaded source for the manual command
                 * that follows. STOP/PLAY/SEEK must never receive an emptied
                 * path merely because they took over an auxiliary lease. */
                AudioDeck *voice=&engine->decks[deck];schedule_flush_locked(engine);
                voice->reported=actual_cursor_locked(engine,voice);voice->requested_play=FALSE;
                if (voice->state!=RADIOS_AUDIO_LOADING && voice->state!=RADIOS_AUDIO_ERROR && voice->state!=RADIOS_AUDIO_EMPTY) voice->state=RADIOS_AUDIO_PAUSED;
                manual_sync_idle_locked(voice);
                run->adjunct[deck].generation=run->adjunct[deck].retire_tag=0;run->adjunct[deck].item=-1;run->adjunct[deck].started=FALSE;
            }
            run->adjunct_owned[deck] = FALSE; if (engine->deck_runner[deck] == run) engine->deck_runner[deck] = NULL;
        }
    }
    engine->processing_runner = previous;
}
static void runner_error_locked(RadioSAudioEngine *engine, const gchar *message)
{
    AudioRunner *run = active_runner(engine);
    run->handover_pending = run->handover_active = run->handover_cut = FALSE;
    run->handover_completed = FALSE;
    runner_terminal_locked(engine, RADIOS_AUDIO_RUN_TERMINAL_FAILED);
    schedule_flush_locked(engine); run->paused = TRUE; run->state = RADIOS_AUDIO_RUN_ERROR;
    g_strlcpy(run->error, message, sizeof run->error);
    for (guint i = 0; i < 32; ++i) if (run->owned[i]) {
        AudioDeck *deck = &engine->decks[i]; deck->requested_play = FALSE;
        if (deck->state == RADIOS_AUDIO_PLAYING) { deck->reported = actual_cursor_locked(engine, deck); deck->state = RADIOS_AUDIO_PAUSED; manual_sync_idle_locked(deck); }
    }
}
static gboolean runner_main_ready(RadioSAudioEngine *engine)
{ return engine->gates[0] && !engine->flush_pending && (!engine->realtime || engine->physical.ready[0]); }
static gboolean runner_ready(AudioDeck *deck)
{
    guint64 buffered = atomic_load(&deck->pcm.write) - atomic_load(&deck->pcm.read);
    guint64 cursor = source_frame(deck, atomic_load(&deck->pcm.read));
    guint64 span = deck->cue_out > cursor ? (guint64)ceil(MAX(0.0,
        deck_output_position(deck, deck->cue_out) - deck_output_position(deck, cursor))) : 0;
    return (deck->state == RADIOS_AUDIO_READY || deck->state == RADIOS_AUDIO_PLAYING || deck->state == RADIOS_AUDIO_PAUSED) &&
        buffered >= MIN(span, 4800u);
}
static gboolean runner_copyright_allowed(const AudioRunner *run, guint index, gint64 wall, gboolean started)
{
    const RadioSAudioRunItem *item = &run->items[index].spec;
    const RadioSAudioCopyrightWindow *window = &item->copyright;
    if (!started) wall = MAX(wall, MAX(run->options.start_utc_us, item->start_utc_us));
    return !window->restricted || (!window->denied &&
        (!window->starts_at_utc_us || wall >= window->starts_at_utc_us) &&
        (!window->ends_at_utc_us || wall < window->ends_at_utc_us));
}
static guint runner_copyright_next(const AudioRunner *run, guint from, gint64 wall)
{
    while (from < run->count && !runner_copyright_allowed(run, from, wall, FALSE)) ++from;
    return from;
}
static void runner_copyright_gate_locked(RadioSAudioEngine *, gint64, guint64);
static gint runner_find_empty(RadioSAudioEngine *engine, guint item)
{
    AudioRunner *run = active_runner(engine); gint preferred = run->items[item].spec.preferred_deck;
    for (guint n = 0; n < run->options.pool_count; ++n) {
        guint deck = run->options.deck_pool[n];
        guint following = runner_copyright_next(run, item + 1, g_get_real_time());
        if (preferred < 0 && following < run->count && run->items[item].spec.overlap_frames &&
            run->items[following].spec.preferred_deck == (gint)deck) continue;
        if ((preferred < 0 || preferred == (gint)deck) && run->loaded_index[deck] < 0 && !runner_other_owns(engine,deck,run) && engine->decks[deck].state == RADIOS_AUDIO_EMPTY) return (gint)deck;
    }
    return -1;
}
/* A pool player already holding `item` ahead of its turn (not retiring). */
static gint runner_find_loaded(RadioSAudioEngine *engine, guint item)
{
    AudioRunner *run = active_runner(engine);
    for (guint n = 0; n < run->options.pool_count; ++n) {
        guint deck = run->options.deck_pool[n];
        if ((gint)deck != run->current && (gint)deck != run->next && run->owned[deck] &&
            run->loaded_index[deck] == (gint)item && !run->retire_tag[deck]) return (gint)deck;
    }
    return -1;
}
/* The preloaded player holding `following` becomes next and follows the
 * playing player's beat. Engine mutex held; it changes fixed runner/deck
 * state only (no allocation, no I/O), so the render thread may call it. */
static gboolean runner_promote_locked(RadioSAudioEngine *engine, guint following)
{
    AudioRunner *run = active_runner(engine);
    gint deck = run->current >= 0 && run->next < 0 && following < run->count ? runner_find_loaded(engine, following) : -1;
    if (deck < 0) return FALSE;
    engine->decks[deck].sync_master = run->items[following].spec.beat_sync ? run->current : -1;
    run->next = deck; return TRUE;
}
/* Kılavuz §08 R8.7 (U1): a pool of N players holds the playing item, `next`
 * and the following N-2 items, each on its planned player, so every selected
 * player of a cluster is loaded in pool order. Only placed items (preferred
 * deck) are preloaded; scope plans place every item, two-player and
 * single-player pools keep current/next only. A preloaded player is silent
 * until it becomes `next`, which assigns its SYNC master. One load starts per
 * control turn once the playing item has audio. A player whose item is no longer
 * upcoming in order (copyright skip, replaced future) is released at once: it
 * never sounded, so nothing is retired or flushed. */
static void runner_preload_locked(RadioSAudioEngine *engine, gint64 wall)
{
    AudioRunner *run = active_runner(engine);
    gboolean claimed[32] = {FALSE}, keep[32] = {FALSE};
    gboolean load = run->current >= 0 && run->next >= 0 && !run->ending && run->options.pool_count > 2;
    gint target = -1; guint target_item = 0;
    if (load) {
        claimed[run->current] = claimed[run->next] = TRUE;
        load = engine->decks[run->current].state != RADIOS_AUDIO_LOADING;
        guint item = (guint)run->loaded_index[run->next];
        for (guint depth = 2; depth < run->options.pool_count; ++depth) {
            item = runner_copyright_next(run, item + 1, wall);
            if (item >= run->count) break;
            gint deck = run->items[item].spec.preferred_deck;
            /* Unplaced, or its player is still taken by an earlier item. */
            if (deck < 0 || claimed[deck]) break;
            claimed[deck] = TRUE;
            if (run->loaded_index[deck] == (gint)item && !run->retire_tag[deck]) keep[deck] = TRUE;
            else if (target < 0 && run->loaded_index[deck] < 0 && engine->decks[deck].state == RADIOS_AUDIO_EMPTY &&
                !runner_other_owns(engine, (guint)deck, run)) { target = deck; target_item = item; }
        }
    }
    for (guint n = 0; n < run->options.pool_count; ++n) {
        guint deck = run->options.deck_pool[n];
        if ((gint)deck == run->current || (gint)deck == run->next || keep[deck] || !run->owned[deck] ||
            run->loaded_index[deck] < 0 || run->retire_tag[deck] || run->started[deck] || run->first_tag[deck]) continue;
        runner_clear_deck_locked(engine, deck);
    }
    if (load && target >= 0) {
        runner_load_locked(engine, (guint)target, target_item);
        engine->decks[target].sync_master = -1;
    }
}
static void runner_silent_finish_locked(RadioSAudioEngine *engine)
{
    AudioRunner *run = active_runner(engine);
    runner_adjunct_clear_locked(engine, TRUE);
    run->final_frame = run->current >= 0 ? actual_cursor_locked(engine, &engine->decks[run->current]) : 0;
    runner_terminal_locked(engine, RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED);
    schedule_flush_locked(engine);
    for (guint i = 0; i < 32; ++i) if (run->owned[i]) { runner_clear_deck_locked(engine, i); run->owned[i] = FALSE; }
    run->handover_completed = run->handover_pending;
    run->active = run->paused = run->overlap = run->handover_pending = run->handover_active = run->handover_cut = FALSE;
    run->current = run->next = run->pending_jump = -1;
    run->cut_remaining = 0; run->pending_stop = FALSE; run->state = RADIOS_AUDIO_RUN_FINISHED;
}
static void runner_control_locked(RadioSAudioEngine *engine)
{
    AudioRunner *run = active_runner(engine);
    runner_receipts_locked(engine);
    guint64 played = engine->realtime ? engine->physical.played_tag[0] : engine->next_tag - 1;
    for (guint i = 0; i < 32; ++i) if (run->owned[i] && run->retire_tag[i] && (played >= run->retire_tag[i] || !run->first_tag[i])) runner_clear_deck_locked(engine, i);
    if (!run->active || run->state == RADIOS_AUDIO_RUN_ERROR) { runner_adjunct_clear_locked(engine, TRUE); return; }
    runner_copyright_gate_locked(engine, g_get_real_time(), MAX(G_GUINT64_CONSTANT(1), played));
    if (run->handover_pending && run->state != RADIOS_AUDIO_RUN_FINISHED &&
        (run->paused || !runner_main_ready(engine))) {
        gint64 now = g_get_real_time();
        gboolean due = now >= run->handover_utc_us;
        gboolean expired = run->handover_max_wait_frames && due && now - run->handover_utc_us >=
            (gint64)(run->handover_max_wait_frames * 1000000 / 48000);
        if (due && (run->handover_policy == RADIOS_AUDIO_RUN_START_CUT || expired)) {
            /* No live source can be faded while paused/disconnected. Freeze
             * only confirmed consumption and drop queued PCM before release. */
            runner_silent_finish_locked(engine);
            return;
        }
    }
    if (run->options.end_utc_us && run->state != RADIOS_AUDIO_RUN_FINISHED) {
        gint64 now = g_get_real_time(), grace = (gint64)(run->options.finish_grace_frames * 1000000 / 48000);
        gboolean silent_cut = run->options.end_policy == RADIOS_AUDIO_RUN_END_CUT && (run->paused || !runner_main_ready(engine));
        if (now >= run->options.end_utc_us && (silent_cut || now - run->options.end_utc_us >= grace)) {
            /* The old programme's own deadline is a natural handover, not a
             * new manual intent. Keep the epoch and accepted waiting gate. */
            if (run->handover_pending) { runner_silent_finish_locked(engine); return; }
            run->final_frame = run->current >= 0 ? actual_cursor_locked(engine, &engine->decks[run->current]) : 0;
            runner_cancel_locked(engine, -1); run->state = RADIOS_AUDIO_RUN_FINISHED; return;
        }
    }
    if (run->state == RADIOS_AUDIO_RUN_FINISHED) {
        if (played >= run->final_tag) {
            runner_adjunct_clear_locked(engine, TRUE);
            for (guint i = 0; i < 32; ++i) if (run->owned[i]) { runner_clear_deck_locked(engine, i); run->owned[i] = FALSE; }
            run->active = FALSE; run->current = run->next = -1;
            run->handover_completed = run->handover_pending;
            run->handover_pending = run->handover_active = run->handover_cut = FALSE;
        }
        return;
    }
    for (guint n = 0; n < run->options.pool_count; ++n) {
        guint i = run->options.deck_pool[n];
        /* A preloaded player that failed reports when it becomes next. */
        gboolean waiting = (gint)i != run->current && (gint)i != run->next && !run->retire_tag[i] && !run->started[i];
        if (run->loaded_index[i] >= 0 && engine->decks[i].state == RADIOS_AUDIO_ERROR && !waiting) { runner_error_locked(engine, engine->decks[i].error); return; }
    }
    if (run->current < 0 && run->index < run->count) {
        gint deck = runner_find_loaded(engine, run->index);
        if (deck >= 0) engine->decks[deck].sync_master = -1;
        else if ((deck = runner_find_empty(engine, run->index)) >= 0) runner_load_locked(engine, (guint)deck, run->index);
        if (deck >= 0) run->current = deck;
    }
    for (guint d=0;d<32;++d) if (run->adjunct[d].generation &&
        ((run->adjunct[d].retire_tag && played>=run->adjunct[d].retire_tag) ||
        engine->decks[d].generation!=run->adjunct[d].generation || engine->decks[d].state==RADIOS_AUDIO_ERROR)) {
        run->adjunct_completed[run->adjunct[d].item]=TRUE; runner_adjunct_clear_player_locked(engine,d);
    }
    if (run->current>=0 && run->items[run->index].spec.adjunct.enabled) runner_adjunct_load_locked(engine,run->index);
    gint64 wall = g_get_real_time();
    guint following = runner_copyright_next(run, run->index + 1, wall);
    if (run->current >= 0 && run->next < 0 && following < run->count && !run->ending && !runner_promote_locked(engine, following)) {
        gint deck = runner_find_empty(engine, following);
        if (deck >= 0) { runner_load_locked(engine, (guint)deck, following); run->next = deck; }
    }
    if (run->next>=0) {
        guint next=(guint)run->loaded_index[run->next];
        if (run->items[next].spec.adjunct.enabled) runner_adjunct_load_locked(engine,next);
    }
    runner_preload_locked(engine, wall);
}
static gpointer runner_worker(gpointer value)
{
    RadioSAudioEngine *engine = value;
    for (;;) {
        g_mutex_lock(&engine->mutex);
        if (engine->closing) { g_mutex_unlock(&engine->mutex); break; }
        runner_each_locked(engine,runner_control_locked);
        g_cond_wait_until(&engine->condition, &engine->mutex, g_get_monotonic_time() + 10000);
        g_mutex_unlock(&engine->mutex);
    }
    return NULL;
}
static guint64 runner_cursor(const AudioDeck *deck)
{ return source_frame(deck, atomic_load(&deck->pcm.read)); }
static guint64 runner_out(const AudioDeck *deck, const RadioSAudioRunItem *item)
{
    guint64 out = item->media.cue_out_frame;
    if (item->estimated_duration && deck->source_eof) {
        guint64 end = source_frame(deck, atomic_load(&deck->pcm.write));
        if (end < out && out - end <= 48) out = end;
    }
    return out;
}
static void runner_retire_locked(RadioSAudioEngine *engine, guint deck, guint64 tag)
{
    AudioRunner *run = active_runner(engine); gint index = run->loaded_index[deck];
    run->retire_tag[deck] = tag;
    run->retire_terminal[deck] = index >= 0 && runner_cursor(&engine->decks[deck]) >= runner_out(&engine->decks[deck], &run->items[index].spec) ?
        RADIOS_AUDIO_RUN_TERMINAL_COMPLETE : RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED;
}
/* Copyright expiry removes only the matching occurrence from this in-memory
 * run. Physical catalogue/playlists, queue indexes and unrelated decks remain
 * intact. Retirement is sample-tagged; decoding cleanup stays on the worker. */
static void runner_copyright_gate_locked(RadioSAudioEngine *engine, gint64 wall, guint64 tag)
{
    AudioRunner *run = active_runner(engine);
    if (!run->items || !run->active || run->state == RADIOS_AUDIO_RUN_FINISHED || run->state == RADIOS_AUDIO_RUN_ERROR) return;
    if (run->next >= 0) {
        guint next = (guint)run->loaded_index[run->next];
        guint expected = runner_copyright_next(run, run->index + 1, wall);
        if (!runner_copyright_allowed(run, next, wall, run->started[run->next]) ||
            (!run->started[run->next] && next != expected)) {
            runner_retire_locked(engine, (guint)run->next, tag);
            run->retire_terminal[run->next] = RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED;
            run->next = -1; run->overlap = FALSE; run->transition = run->transition_total = 0;
        }
    }
    gboolean started = run->current >= 0 && run->started[run->current];
    if (run->index < run->count && runner_copyright_allowed(run, run->index, wall, started)) return;
    if (run->current >= 0) {
        runner_retire_locked(engine, (guint)run->current, tag);
        run->retire_terminal[run->current] = RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED;
    }
    guint from = run->index < run->count ? run->index + 1 : run->count;
    run->index = runner_copyright_next(run, from, wall);
    run->current = run->next >= 0 && run->loaded_index[run->next] == (gint)run->index ? run->next : -1;
    run->next = -1; run->overlap = FALSE; run->transition = run->transition_total = 0;
    run->cut_remaining = 0; run->pending_jump = -1; run->pending_stop = FALSE;
    if (run->index >= run->count) {
        run->state = RADIOS_AUDIO_RUN_FINISHED; run->final_tag = tag; run->final_frame = 0;
    } else if (!run->paused) run->state = RADIOS_AUDIO_RUN_WAITING_MEDIA;
}
static void runner_finish_locked(RadioSAudioEngine *engine, RenderHistory *history, guint offset)
{
    AudioRunner *run = active_runner(engine);
    run->state = RADIOS_AUDIO_RUN_FINISHED; run->final_tag = history->first_tag + offset;
    if (run->current >= 0) run->final_frame = runner_cursor(&engine->decks[run->current]);
    if (run->current >= 0) runner_retire_locked(engine, (guint)run->current, run->final_tag);
    if (run->next >= 0) runner_retire_locked(engine, (guint)run->next, run->final_tag);
    run->overlap = FALSE;
}
/* Overlap envelope: a designed shape replaces the equal-power/duck curves. */
static void runner_overlap_gains(const AudioRunner *run, const RadioSAudioRunItem *item, gdouble *a, gdouble *b)
{
    if (item->shape.enabled && run->transition_total) {
        radios_audio_transition_shape_gains(&item->shape, (gdouble)run->transition / run->transition_total, a, b); return;
    }
    RadioSAudioTransition envelope = {run->transition_total, run->transition_total,
        MIN(run->items[run->loaded_index[run->next]].spec.fade_in_frames, run->transition_total), item->duck_db};
    radios_audio_transition_gains(&envelope, run->transition, a, b);
}
static void runner_shape_reset(AudioRunner *run)
{ for (guint side = 0; side < 2; ++side) radios_audio_shape_voice_reset(run->shape_voice[side]); }
static gdouble runner_fade(const RadioSAudioRunItem *item, guint64 cursor, gboolean include_in)
{
    gdouble gain = 1;
    guint64 fade_in = MIN(item->fade_in_frames, radios_audio_run_incoming_span_frames(item));
    RadioSAudioTempoProfile profile = radios_audio_run_tempo_profile(item);
    gdouble elapsed = radios_audio_tempo_profile_output(&profile, cursor - item->media.cue_in_frame);
    if (include_in && fade_in && elapsed < fade_in)
        gain *= sin(G_PI / 2 * elapsed / fade_in);
    if (item->fade_end_frame > item->fade_start_frame && cursor >= item->fade_start_frame)
        gain *= cursor >= item->fade_end_frame ? 0 : cos(G_PI / 2 * (gdouble)(cursor - item->fade_start_frame) /
            (item->fade_end_frame - item->fade_start_frame));
    return gain;
}
/* Render thread: only fixed state and already verified/prepared PCM. Trigger
 * and envelopes use output frames, so pause and tempo never turn wall time
 * into a fake played position. A forbidden/unavailable voice leaves music on. */
static void runner_adjunct_frame_locked(RadioSAudioEngine *engine, guint64 tag, gint64 wall,
    gboolean *active, gdouble *gains)
{
    AudioRunner *run=active_runner(engine);
    for (guint d=0;d<32;++d) {
        if (!run->adjunct[d].generation || run->adjunct[d].retire_tag) continue;
        gint item=run->adjunct[d].item, parent=-1;
        if (run->current>=0 && item==(gint)run->index) parent=run->current;
        else if (run->next>=0 && item==run->loaded_index[run->next]) parent=run->next;
        else { run->adjunct[d].retire_tag=tag; continue; }
        if (!active[parent]) continue;
        AudioDeck *voice=&engine->decks[d],*music=&engine->decks[parent];
        const RadioSAudioRunAdjunct *spec=&run->items[item].spec.adjunct;
        if (!run->adjunct_owned[d] || voice->generation!=run->adjunct[d].generation) continue;
        const RadioSAudioCopyrightWindow *rights=&spec->copyright;
        if (rights->restricted && (rights->denied || (rights->starts_at_utc_us && wall<rights->starts_at_utc_us) ||
            (rights->ends_at_utc_us && wall>=rights->ends_at_utc_us))) {
            if (!rights->starts_at_utc_us || wall>=rights->starts_at_utc_us) run->adjunct[d].retire_tag=tag;
            continue;
        }
        gdouble parent_elapsed=deck_output_position(music,runner_cursor(music))-deck_output_position(music,music->cue_in);
        if (parent_elapsed<spec->start_output_frame) continue;
        guint64 cursor=runner_cursor(voice);
        if (cursor>=voice->cue_out) { run->adjunct[d].retire_tag=tag; continue; }
        if (!run->adjunct[d].started) {
            if (!runner_ready(voice)) {
                if (parent_elapsed>spec->start_output_frame+48) run->adjunct[d].retire_tag=tag;
                continue;
            }
            voice->state=RADIOS_AUDIO_PLAYING;voice->requested_play=FALSE;run->adjunct[d].started=TRUE;
        }
        if (atomic_load(&voice->pcm.write)<=atomic_load(&voice->pcm.read)) continue;
        guint64 elapsed=cursor-voice->cue_in,remaining=voice->cue_out-cursor;
        gdouble envelope=1;
        if (spec->fade_frames) envelope=MIN(1.0,MIN((gdouble)elapsed/spec->fade_frames,(gdouble)remaining/spec->fade_frames));
        active[d]=TRUE;gains[d]=pow(10,spec->gain_db/20)*envelope;
        gains[parent]*=1+(pow(10,spec->duck_db/20)-1)*envelope;
    }
}
static gboolean runner_adjunct_item_ready(RadioSAudioEngine *engine,guint item)
{
    AudioRunner *run=active_runner(engine);const RadioSAudioRunAdjunct *voice=&run->items[item].spec.adjunct;
    if (!voice->enabled || voice->start_output_frame || run->adjunct_completed[item] || !run->adjunct_owned[voice->player] ||
        (voice->copyright.restricted && voice->copyright.denied)) return TRUE;
    guint d=voice->player;
    return run->adjunct[d].generation && run->adjunct[d].item==(gint)item &&
        engine->decks[d].generation==run->adjunct[d].generation && runner_ready(&engine->decks[d]);
}
static void runner_frame_locked(RadioSAudioEngine *engine, RenderHistory *history, guint offset,
    gint64 wall, gboolean *active, gdouble *gains)
{
    AudioRunner *run = active_runner(engine);
    for (guint i = 0; i < 32; ++i) if (run->owned[i] || run->adjunct_owned[i]) active[i] = FALSE;
    if (!run->items || !run->active || run->paused || run->state == RADIOS_AUDIO_RUN_ERROR || run->state == RADIOS_AUDIO_RUN_FINISHED) {
        if (run->items) runner_record_locked(run, history, offset);
        return;
    }
    runner_copyright_gate_locked(engine, wall, history->first_tag + offset);
    if (run->state == RADIOS_AUDIO_RUN_FINISHED) { runner_record_locked(run, history, offset); return; }
    if (!runner_main_ready(engine)) { run->state = RADIOS_AUDIO_RUN_WAITING_MEDIA; runner_record_locked(run, history, offset); return; }
    if (run->handover_pending && wall >= run->handover_utc_us) {
        if (!run->handover_active) {
            run->handover_active = TRUE; run->handover_ending_before = run->ending;
        }
        run->ending = TRUE;
        gboolean expired = run->handover_max_wait_frames && wall - run->handover_utc_us >=
            (gint64)(run->handover_max_wait_frames * 1000000 / 48000);
        if ((run->handover_policy == RADIOS_AUDIO_RUN_START_CUT || expired) && !run->handover_cut) {
            run->handover_cut = TRUE; run->pending_stop = TRUE;
            run->cut_total = run->cut_remaining = run->handover_fade_frames;
            if (!run->cut_remaining) {
                runner_finish_locked(engine, history, offset); runner_record_locked(run, history, offset); return;
            }
        }
    }
    if (run->options.end_utc_us && wall >= run->options.end_utc_us) {
        run->ending = TRUE;
        gboolean expired = run->options.finish_grace_frames == 0 || wall - run->options.end_utc_us >=
            (gint64)(run->options.finish_grace_frames * 1000000 / 48000);
        if ((run->options.end_policy == RADIOS_AUDIO_RUN_END_CUT || expired) && !run->cut_remaining) {
            run->pending_stop = TRUE; run->cut_total = run->cut_remaining = 240;
        }
    }
    if (run->current < 0 || run->index >= run->count) {
        if (run->ending) { runner_finish_locked(engine, history, offset); runner_record_locked(run, history, offset); return; }
        run->state = RADIOS_AUDIO_RUN_WAITING_MEDIA; runner_record_locked(run, history, offset); return;
    }
    AudioDeck *current = &engine->decks[run->current]; const RadioSAudioRunItem *item = &run->items[run->index].spec;
    if (current->state == RADIOS_AUDIO_ERROR) { runner_error_locked(engine, current->error); runner_record_locked(run, history, offset); return; }
    if (!run->started[run->current]) {
        /* A seek, a paused SYNC reload or SYNC re-prerolls an item that already
         * went on air (first_tag). It resumes at its new position: the start
         * window and the schedule end governed its first start, and applying
         * them again would stop the automation on an ordinary seek (R8.3). */
        gboolean resumed = run->first_tag[run->current] != 0;
        gint64 earliest = MAX(run->options.start_utc_us, item->start_utc_us);
        if (!resumed && earliest && wall < earliest) { run->state = RADIOS_AUDIO_RUN_SCHEDULED; runner_record_locked(run, history, offset); return; }
        if (!resumed && item->latest_start_utc_us && wall > item->latest_start_utc_us) {
            runner_error_locked(engine, "Automation source start window has expired"); runner_record_locked(run, history, offset); return;
        }
        if (!resumed && run->ending) { runner_finish_locked(engine, history, offset); runner_record_locked(run, history, offset); return; }
        if (!runner_ready(current) || !runner_adjunct_item_ready(engine,run->index)) { run->state = RADIOS_AUDIO_RUN_PREROLL; runner_record_locked(run, history, offset); return; }
        current->state = RADIOS_AUDIO_PLAYING; current->requested_play = FALSE;
        run->started[run->current] = TRUE;
    }
    guint64 cursor = runner_cursor(current), out = runner_out(current, item);
    if (run->overlap && cursor >= out && item->estimated_duration) run->transition = run->transition_total;
    if (run->cut_remaining) {
        runner_record_locked(run, history, offset);
        gdouble cut = (gdouble)run->cut_remaining / run->cut_total;
        if (cursor < out && atomic_load(&current->pcm.write) > atomic_load(&current->pcm.read)) { active[run->current] = TRUE; gains[run->current] = cut * runner_fade(item, cursor, TRUE); }
        if (run->overlap && run->next >= 0 && atomic_load(&engine->decks[run->next].pcm.write) > atomic_load(&engine->decks[run->next].pcm.read)) { active[run->next] = TRUE; gains[run->next] = cut; }
        if (run->overlap && run->next >= 0) {
            gdouble a, b; runner_overlap_gains(run, item, &a, &b);
            gains[run->current] *= a; gains[run->next] *= b;
            ++run->transition;
        }
        if (!--run->cut_remaining) {
            guint64 tag = history->first_tag + offset + 1;
            for (guint i = 0; i < 32; ++i) if (run->owned[i] && run->loaded_index[i] >= 0) runner_retire_locked(engine, i, tag);
            if (run->pending_stop) { run->state = RADIOS_AUDIO_RUN_FINISHED; run->final_tag = tag; run->final_frame = source_frame(current, atomic_load(&current->pcm.read) + (active[run->current] ? 1 : 0)); }
            else { run->index = (guint)run->pending_jump; run->current = run->next = -1; run->overlap = FALSE; run->state = RADIOS_AUDIO_RUN_WAITING_MEDIA; }
            run->pending_jump = -1; run->pending_stop = FALSE;
        }
        return;
    }
    if (run->overlap && run->transition >= run->transition_total) {
        if (run->handover_active) {
            runner_finish_locked(engine, history, offset); runner_record_locked(run, history, offset); return;
        }
        runner_retire_locked(engine, (guint)run->current, history->first_tag + offset);
        run->loudness_pair_source = run->current; run->loudness_pair_target = run->next;
        run->current = run->next; run->index = (guint)run->loaded_index[run->current]; run->next = -1;
        run->overlap = FALSE; run->transition = run->transition_total = 0;
        current = &engine->decks[run->current]; item = &run->items[run->index].spec;
        if (item->beat_sync || item->tempo_return_frames) current->sync_master = -1;
        cursor = runner_cursor(current); out = runner_out(current, item);
    }
    guint following_index = runner_copyright_next(run, run->index + 1, wall);
    const RadioSAudioRunItem *following = following_index < run->count ? &run->items[following_index].spec : NULL;
    RadioSAudioRunItem policy = *item;
    if (following) policy.overlap_frames = MIN(policy.overlap_frames, radios_audio_run_incoming_span_frames(following));
    /* Skipping a forbidden B in an A/B/A sequence can reveal two entries
     * pinned to one physical deck. Recycle it at the boundary; an impossible
     * same-deck overlap must never strand the otherwise legal successor. */
    if (following && following->preferred_deck == run->current) policy.overlap_frames = 0;
    guint64 trigger = radios_audio_run_trigger_frame(&policy, &current->rhythm, deck_current_ratio(current), out);
    gboolean boundary = cursor >= trigger && item->mode != RADIOS_AUDIO_RUN_MANUAL;
    if (!following || run->ending) boundary = FALSE;
    gboolean anchor_cut = FALSE;
    if (!run->overlap && !run->ending && following && following->start_utc_us) {
        if (wall < following->start_utc_us) boundary = FALSE;
        else if (following->start_policy == RADIOS_AUDIO_RUN_START_CUT) { boundary = TRUE; anchor_cut = TRUE; }
        else if (following->start_policy == RADIOS_AUDIO_RUN_START_FINISH) {
            gboolean waited = following->max_wait_frames && wall - following->start_utc_us >= (gint64)(following->max_wait_frames * 1000000 / 48000);
            boundary = cursor >= out || waited; anchor_cut = waited && cursor < out;
        }
    }
    if (!run->overlap && (boundary || cursor >= out)) {
        if (run->ending || !following) { runner_finish_locked(engine, history, offset); runner_record_locked(run, history, offset); return; }
        if (item->mode == RADIOS_AUDIO_RUN_MANUAL && !boundary) { run->state = RADIOS_AUDIO_RUN_MANUAL_WAIT; runner_record_locked(run, history, offset); return; }
        if (following->start_utc_us && wall < following->start_utc_us) { run->state = RADIOS_AUDIO_RUN_SCHEDULED; runner_record_locked(run, history, offset); return; }
        if (following->latest_start_utc_us && wall > following->latest_start_utc_us) {
            runner_error_locked(engine, "Automation source start window has expired"); runner_record_locked(run, history, offset); return;
        }
        if (run->next < 0 && (run->options.pool_count == 1 || (following->preferred_deck == run->current && !policy.overlap_frames))) {
            runner_retire_locked(engine, (guint)run->current, history->first_tag + offset);
            run->index = following_index; run->current = -1; run->state = RADIOS_AUDIO_RUN_WAITING_MEDIA;
            runner_record_locked(run, history, offset); return;
        }
        if (run->next < 0 || run->loaded_index[run->next] != (gint)following_index || !runner_ready(&engine->decks[run->next]) || !runner_adjunct_item_ready(engine,following_index)) {
            /* A preloaded successor (U1) is taken at once; it starts from the next frame. */
            if (run->next < 0) runner_promote_locked(engine, following_index);
            run->state = RADIOS_AUDIO_RUN_WAITING_MEDIA; runner_record_locked(run, history, offset); return;
        }
        guint64 remaining = (guint64)ceil(MAX(0.0,
            deck_output_position(current, out) - deck_output_position(current, cursor)));
        guint64 overlap = anchor_cut ? MIN(remaining, 240u) : radios_audio_run_overlap_frames(&policy, remaining);
        overlap = MIN(overlap, radios_audio_run_incoming_span_frames(following));
        if (overlap) {
            run->overlap = TRUE; run->transition = 0; run->transition_total = overlap;
            if (item->shape.enabled) runner_shape_reset(run);
            engine->decks[run->next].state = RADIOS_AUDIO_PLAYING;
            run->started[run->next] = TRUE;
        } else {
            runner_retire_locked(engine, (guint)run->current, history->first_tag + offset);
            run->loudness_pair_source = run->current; run->loudness_pair_target = run->next;
            run->current = run->next; run->index = (guint)run->loaded_index[run->current]; run->next = -1;
            current = &engine->decks[run->current]; current->state = RADIOS_AUDIO_PLAYING; item = &run->items[run->index].spec;
            run->started[run->current] = TRUE;
            cursor = runner_cursor(current); out = runner_out(current, item);
        }
    }
    if (cursor >= out) { run->state = RADIOS_AUDIO_RUN_WAITING_MEDIA; runner_record_locked(run, history, offset); return; }
    gboolean buffered = atomic_load(&current->pcm.write) > atomic_load(&current->pcm.read);
    if (run->overlap && run->next >= 0) buffered &= atomic_load(&engine->decks[run->next].pcm.write) > atomic_load(&engine->decks[run->next].pcm.read);
    if (!buffered && run->overlap && run->next >= 0) {
        AudioDeck *incoming = &engine->decks[run->next];
        const RadioSAudioRunItem *next_item = &run->items[run->loaded_index[run->next]].spec;
        guint64 end = runner_cursor(incoming), declared = next_item->media.cue_out_frame;
        if (next_item->estimated_duration && incoming->source_eof && end <= declared && declared - end <= 48) {
            /* The millisecond estimate extends past decoded EOF. End this
             * overlap and let the incoming item's normal boundary advance. */
            run->transition = run->transition_total;
            runner_record_locked(run, history, offset); return;
        }
    }
    if (!buffered) {
        if (current->source_eof || (run->overlap && engine->decks[run->next].source_eof)) runner_error_locked(engine, "Verified automation media ended before its declared cue span");
        else run->state = RADIOS_AUDIO_RUN_WAITING_MEDIA;
        runner_record_locked(run, history, offset); return;
    }
    active[run->current] = TRUE; gains[run->current] = runner_fade(item, cursor, TRUE);
    if (run->overlap) {
        gdouble a, b; runner_overlap_gains(run, item, &a, &b);
        gains[run->current] *= a; active[run->next] = TRUE;
        gains[run->next] = b * runner_fade(&run->items[run->loaded_index[run->next]].spec, runner_cursor(&engine->decks[run->next]), FALSE);
        run->state = RADIOS_AUDIO_RUN_OVERLAP; runner_record_locked(run, history, offset); ++run->transition;
    } else { run->state = RADIOS_AUDIO_RUN_PLAYING; runner_record_locked(run, history, offset); }
    runner_adjunct_frame_locked(engine, history->first_tag + offset, wall, active, gains);
}
static gboolean runner_validate(const RadioSAudioRunItem *items, guint count, const RadioSAudioRunOptions *options, GError **error)
{
    const guint64 maximum = G_GUINT64_CONSTANT(4147200000);
    if (!items || !count || count > RADIOS_AUDIO_RUN_MAX_ITEMS || !options || !options->pool_count || options->pool_count > 32 ||
        options->start_utc_us < 0 || options->end_utc_us < 0 || (options->end_utc_us && options->start_utc_us >= options->end_utc_us) ||
        options->end_policy < RADIOS_AUDIO_RUN_END_CUT || options->end_policy > RADIOS_AUDIO_RUN_END_FINISH || options->finish_grace_frames > maximum)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid bounded automation queue or schedule");
    if (options->loudness_context & ~RADIOS_AUDIO_LOUDNESS_ALL_CONTEXTS ||
        !memchr(options->loudness_automation_id, 0, sizeof options->loudness_automation_id) ||
        (*options->loudness_automation_id && !g_uuid_string_is_valid(options->loudness_automation_id)) ||
        (options->loudness_assigned && !*options->loudness_automation_id))
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid immutable loudness execution origin");
    gboolean pool[32] = {FALSE};
    for (guint i = 0; i < options->pool_count; ++i) {
        guint deck = options->deck_pool[i];
        if (deck >= 32 || pool[deck]) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Automation deck pool must contain distinct normal decks");
        pool[deck] = TRUE;
    }
    for (guint i = 0; i < count; ++i) {
        const RadioSAudioRunItem *item = &items[i]; const RadioSAudioLoadSpec *media = &item->media;
        if (item->effect < RADIOS_AUDIO_RUN_EFFECT_NONE || item->effect > RADIOS_AUDIO_RUN_EFFECT_LOWPASS ||
            (item->effect == RADIOS_AUDIO_RUN_EFFECT_LOWPASS &&
                (!item->overlap_frames || (item->mode != RADIOS_AUDIO_RUN_CROSSFADE && item->mode != RADIOS_AUDIO_RUN_CUE))))
            return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid outgoing transition filter");
        if (!radios_audio_transition_shape_validate(&item->shape, NULL) ||
            (item->shape.enabled && (!item->overlap_frames || (item->mode != RADIOS_AUDIO_RUN_CROSSFADE && item->mode != RADIOS_AUDIO_RUN_CUE))))
            return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid designed transition shape");
        if (!isfinite(item->tempo_ratio) || (item->tempo_ratio != 0 && (item->tempo_ratio < 0.84 || item->tempo_ratio > 1.16)) ||
            item->tempo_return_start_frames > 1440000 || item->tempo_return_frames > 192000 ||
            (item->beat_sync && (!isfinite(item->rhythm.bpm) || item->rhythm.bpm < 60 || item->rhythm.bpm > 200 ||
                !isfinite(item->rhythm.first_beat_ms) || item->rhythm.first_beat_ms < 0 || !isfinite(item->rhythm.confidence) || item->rhythm.confidence < 0.45 || item->rhythm.confidence > 1 ||
                item->rhythm.analyzed_ms < 4000 || item->rhythm.analyzed_ms > 120000 || item->rhythm.first_beat_ms >= item->rhythm.analyzed_ms ||
                (item->rhythm.has_downbeat && (!isfinite(item->rhythm.downbeat_ms) || item->rhythm.downbeat_ms < 0 ||
                    item->rhythm.downbeat_ms >= item->rhythm.analyzed_ms)) ||
                (guint64)item->rhythm.analyzed_ms * 48 > item->duration_frames + 47)))
            return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid verified beatgrid or bounded tempo ratio");
        guint64 out = media->cue_out_frame ? media->cue_out_frame : item->duration_frames;
        if (!item->entry_id || !g_uuid_string_is_valid(item->entry_id) || !media->path || !radios_path_is_absolute(media->path) ||
            strlen(media->path) > 4096 || !g_utf8_validate(media->path, -1, NULL) || !media->sha256 || strlen(media->sha256) != 64 ||
            !media->media_id || !g_uuid_string_is_valid(media->media_id) || !media->version_id || !g_uuid_string_is_valid(media->version_id) ||
            !item->duration_frames || item->duration_frames > maximum || media->cue_in_frame >= out || out > item->duration_frames ||
            item->auto_next_frame < media->cue_in_frame || item->auto_next_frame > out ||
            (item->autodj_out_frame && (item->autodj_out_frame <= media->cue_in_frame || item->autodj_out_frame > out)) ||
            ((item->fade_start_frame || item->fade_end_frame) && (item->fade_start_frame < media->cue_in_frame || item->fade_start_frame > item->fade_end_frame || item->fade_end_frame > out)) ||
            item->fade_in_frames > 1440000 || item->overlap_frames > 1440000 || item->overlap_frames > out - media->cue_in_frame ||
            !isfinite(item->duck_db) || item->duck_db < -30 || item->duck_db > 0 || item->mode < RADIOS_AUDIO_RUN_EOF || item->mode > RADIOS_AUDIO_RUN_MANUAL ||
            item->start_policy < RADIOS_AUDIO_RUN_START_SEQUENTIAL || item->start_policy > RADIOS_AUDIO_RUN_START_FINISH ||
            item->start_utc_us < 0 || item->latest_start_utc_us < 0 || (item->latest_start_utc_us && item->start_utc_us > item->latest_start_utc_us) ||
            item->copyright.starts_at_utc_us < 0 || item->copyright.ends_at_utc_us < 0 ||
            item->max_wait_frames > maximum || item->preferred_deck < -1 || item->preferred_deck >= 32 ||
            (item->preferred_deck >= 0 && !pool[item->preferred_deck]))
            return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid verified automation media, cue, window or transition");
        const RadioSAudioRunAdjunct *voice = &item->adjunct;
        if (voice->enabled) {
            const RadioSAudioLoadSpec *v = &voice->media;
            if (voice->player >= 32 || pool[voice->player] || !voice->entry_id || !g_uuid_string_is_valid(voice->entry_id) ||
                !v->path || !radios_path_is_absolute(v->path) || strlen(v->path) > 4096 || !g_utf8_validate(v->path, -1, NULL) ||
                !v->sha256 || strlen(v->sha256) != 64 || !v->media_id || !g_uuid_string_is_valid(v->media_id) ||
                !v->version_id || !g_uuid_string_is_valid(v->version_id) || v->cue_in_frame >= v->cue_out_frame || v->cue_out_frame > maximum ||
                voice->start_output_frame >= (guint64)ceil(radios_audio_tempo_profile_output(
                    &(RadioSAudioTempoProfile){item->tempo_ratio > 0 ? item->tempo_ratio : 1,
                        item->tempo_return_start_frames,item->tempo_return_frames},out-media->cue_in_frame)) ||
                voice->fade_frames > 240000 || !isfinite(voice->gain_db) || voice->gain_db < -60 || voice->gain_db > 12 ||
                !isfinite(voice->duck_db) || voice->duck_db < -60 || voice->duck_db > 0 ||
                voice->copyright.starts_at_utc_us < 0 || voice->copyright.ends_at_utc_us < 0 ||
                (voice->title && (strlen(voice->title) > 800 || !g_utf8_validate(voice->title,-1,NULL))) ||
                (voice->artist && (strlen(voice->artist) > 800 || !g_utf8_validate(voice->artist,-1,NULL))))
                return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid verified midtrack jingle or distinct auxiliary player");
            for (guint n = 0; n < 64; ++n) if (!g_ascii_isxdigit(v->sha256[n]))
                return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid midtrack jingle SHA-256");
        }
        for (guint n = 0; n < 64; ++n) if (!g_ascii_isxdigit(media->sha256[n])) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid automation media SHA-256");
        if (options->pool_count == 1 && (item->mode == RADIOS_AUDIO_RUN_CROSSFADE || item->overlap_frames))
            return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Overlap requires at least two automation decks");
        if (i + 1 < count) {
            guint64 next_out = items[i + 1].media.cue_out_frame ? items[i + 1].media.cue_out_frame : items[i + 1].duration_frames;
            if (next_out < items[i + 1].media.cue_in_frame || item->overlap_frames > next_out - items[i + 1].media.cue_in_frame)
                return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Automation overlap exceeds the following verified cue span");
            if (item->overlap_frames && item->preferred_deck >= 0 && item->preferred_deck == items[i + 1].preferred_deck)
                return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Overlapping automation items require distinct physical decks");
        }
    }
    gdouble positions[RADIOS_AUDIO_RUN_MAX_ITEMS]={0};
    for (guint i=0;i<count;++i) {
        if (i) {
            RadioSAudioRunItem previous=items[i-1];previous.overlap_frames=MIN(previous.overlap_frames,radios_audio_run_incoming_span_frames(&items[i]));
            guint64 out=previous.media.cue_out_frame?previous.media.cue_out_frame:previous.duration_frames;
            guint64 trigger=radios_audio_run_trigger_frame(&previous,&previous.rhythm,previous.tempo_ratio>0?previous.tempo_ratio:1,out);
            RadioSAudioTempoProfile profile=radios_audio_run_tempo_profile(&previous);
            positions[i]=positions[i-1]+radios_audio_tempo_profile_output(&profile,trigger-previous.media.cue_in_frame);
        }
        const RadioSAudioRunAdjunct *voice=&items[i].adjunct;if (!voice->enabled) continue;
        gdouble start=positions[i]+voice->start_output_frame,end=start+voice->media.cue_out_frame-voice->media.cue_in_frame;
        for (guint j=0;j<i;++j) {
            const RadioSAudioRunAdjunct *prior=&items[j].adjunct;if (!prior->enabled || prior->player!=voice->player) continue;
            gdouble prior_start=positions[j]+prior->start_output_frame,prior_end=prior_start+prior->media.cue_out_frame-prior->media.cue_in_frame;
            if (start<prior_end && end>prior_start) return fail(error,RADIOS_AUDIO_ERROR_INVALID,"Overlapping jingles require distinct explicit players");
        }
    }
    return TRUE;
}
static void runner_copy_entry(RunEntry *copy, const RadioSAudioRunItem *item)
{
    copy->spec = *item;
    g_strlcpy(copy->path, item->media.path, sizeof copy->path); g_strlcpy(copy->sha, item->media.sha256, sizeof copy->sha);
    for (guint n = 0; n < 64; ++n) copy->sha[n] = g_ascii_tolower(copy->sha[n]);
    g_strlcpy(copy->media_id, item->media.media_id, sizeof copy->media_id); g_strlcpy(copy->version_id, item->media.version_id, sizeof copy->version_id);
    g_strlcpy(copy->entry_id, item->entry_id, sizeof copy->entry_id);
    copy->spec.entry_id = copy->entry_id; copy->spec.media.path = copy->path; copy->spec.media.sha256 = copy->sha;
    copy->spec.media.media_id = copy->media_id; copy->spec.media.version_id = copy->version_id;
    if (item->adjunct.enabled) {
        g_strlcpy(copy->adjunct_path,item->adjunct.media.path,sizeof copy->adjunct_path);
        g_strlcpy(copy->adjunct_sha,item->adjunct.media.sha256,sizeof copy->adjunct_sha);
        for (guint n=0;n<64;++n) copy->adjunct_sha[n]=g_ascii_tolower(copy->adjunct_sha[n]);
        g_strlcpy(copy->adjunct_media,item->adjunct.media.media_id,sizeof copy->adjunct_media);
        g_strlcpy(copy->adjunct_version,item->adjunct.media.version_id,sizeof copy->adjunct_version);
        g_strlcpy(copy->adjunct_entry,item->adjunct.entry_id,sizeof copy->adjunct_entry);
        g_strlcpy(copy->adjunct_title,item->adjunct.title?item->adjunct.title:"",sizeof copy->adjunct_title);
        g_strlcpy(copy->adjunct_artist,item->adjunct.artist?item->adjunct.artist:"",sizeof copy->adjunct_artist);
        copy->spec.adjunct.media.path=copy->adjunct_path; copy->spec.adjunct.media.sha256=copy->adjunct_sha;
        copy->spec.adjunct.media.media_id=copy->adjunct_media; copy->spec.adjunct.media.version_id=copy->adjunct_version;
        copy->spec.adjunct.entry_id=copy->adjunct_entry; copy->spec.adjunct.title=copy->adjunct_title; copy->spec.adjunct.artist=copy->adjunct_artist;
    }
    if (!copy->spec.media.cue_out_frame) copy->spec.media.cue_out_frame = copy->spec.duration_frames;
}
static gboolean runner_configure_internal(RadioSAudioEngine *engine, const RadioSAudioRunItem *items,
    guint count, const RadioSAudioRunOptions *options, gboolean scoped_pool, GError **error)
{
    if (!engine) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid audio engine");
    if (!runner_validate(items, count, options, error)) return FALSE;
    RunEntry *copy = g_new0(RunEntry, RADIOS_AUDIO_RUN_MAX_ITEMS);
    for (guint i = 0; i < count; ++i) runner_copy_entry(&copy[i], &items[i]);
    g_mutex_lock(&engine->mutex); AudioRunner *run = active_runner(engine);
    if (engine->closing || run->active || runner_has_leases(run)) {
        g_mutex_unlock(&engine->mutex); g_free(copy); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Stop or finish the current automation run before replacing its queue");
    }
    for (guint i = 0; scoped_pool && i < options->pool_count; ++i) if (runner_other_owns(engine, options->deck_pool[i], run)) {
        g_mutex_unlock(&engine->mutex); g_free(copy);
        return fail(error, RADIOS_AUDIO_ERROR_STATE, "Automation scope overlaps another running queue");
    }
    g_free(run->items); run->items = copy; run->count = count; run->options = *options; run->index = 0;
    run->loudness_pair_source = run->loudness_pair_target = -1;
    gchar *playlist = g_uuid_string_random(); g_strlcpy(run->playlist_id, playlist, sizeof run->playlist_id); g_free(playlist);
    memset(run->receipts, 0, sizeof run->receipts);
    memset(run->receipt_occurrence, 0, sizeof run->receipt_occurrence); run->receipt_write = 0;
    memset(run->adjunct,0,sizeof run->adjunct); memset(run->adjunct_completed,0,sizeof run->adjunct_completed);
    run->state = RADIOS_AUDIO_RUN_IDLE; run->current = run->next = run->pending_jump = -1; run->error[0] = '\0'; ++run->epoch;
    run->handover_pending = run->handover_active = run->handover_cut = FALSE;
    run->handover_completed = FALSE;
    g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_runner_configure(RadioSAudioEngine *engine, const RadioSAudioRunItem *items,
    guint count, const RadioSAudioRunOptions *options, GError **error)
{ return runner_configure_internal(engine, items, count, options, FALSE, error); }
gboolean radios_audio_engine_runner_append(RadioSAudioEngine *engine, const RadioSAudioRunItem *items, guint count, GError **error)
{
    if (!engine || !items || !count || count > RADIOS_AUDIO_RUN_MAX_ITEMS)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid automation append batch");
    g_mutex_lock(&engine->mutex); AudioRunner *run = active_runner(engine);
    if (!run || engine->closing || !run->active || run->state == RADIOS_AUDIO_RUN_FINISHED || run->state == RADIOS_AUDIO_RUN_ERROR ||
        count > RADIOS_AUDIO_RUN_MAX_ITEMS - run->count) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Append requires a live queue with capacity before its rendered end");
    }
    /* Up to 500 items of ~1.5 KB: on the heap, never on the caller's stack. */
    RadioSAudioRunItem *combined = g_new(RadioSAudioRunItem, run->count + count);
    for (guint i = 0; i < run->count; ++i) combined[i] = run->items[i].spec;
    for (guint i = 0; i < count; ++i) combined[run->count + i] = items[i];
    gboolean valid = runner_validate(combined, run->count + count, &run->options, error);
    g_free(combined);
    if (!valid) { g_mutex_unlock(&engine->mutex); return FALSE; }
    gboolean new_auxiliary[32]={FALSE}, skipped_auxiliary[32]={FALSE};
    for (guint i=0;i<count;++i) if (items[i].adjunct.enabled && !run->adjunct_owned[items[i].adjunct.player]) new_auxiliary[items[i].adjunct.player]=TRUE;
    for (guint d=0;d<32;++d) if (new_auxiliary[d] && !runner_adjunct_available_locked(engine,d,run)) {
        if (!run->options.skip_unavailable_adjuncts) {
            g_mutex_unlock(&engine->mutex);return fail(error,RADIOS_AUDIO_ERROR_STATE,"Append cannot steal an occupied or CUE jingle player");
        }
        new_auxiliary[d]=FALSE; skipped_auxiliary[d]=TRUE;
    }
    for (guint d=0;d<32;++d) if (new_auxiliary[d]) { run->adjunct_owned[d]=TRUE;engine->deck_runner[d]=run; }
    for (guint i = 0; i < count; ++i) {
        runner_copy_entry(&run->items[run->count + i], &items[i]);
        if (items[i].adjunct.enabled && skipped_auxiliary[items[i].adjunct.player])
            run->items[run->count + i].spec.adjunct.enabled=FALSE;
    }
    run->count += count; g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_runner_append_source(RadioSAudioEngine *engine, gint source,
    const RadioSAudioRunItem *items, guint count, GError **error)
{
    if (source < -1 || source >= 32) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid runner source");
    gpointer previous = g_private_get(&runner_scope); g_private_set(&runner_scope, GUINT_TO_POINTER((guint)(source + 1)));
    gboolean result = radios_audio_engine_runner_append(engine, items, count, error);
    g_private_set(&runner_scope, previous); return result;
}
gboolean radios_audio_engine_runner_replace_future(RadioSAudioEngine *engine, gint source,
    guint64 epoch, guint64 generation, const gchar *entry_id,
    const RadioSAudioRunItem *items, guint count, GError **error)
{
    if (!engine || source < -1 || source >= 32 || !items || !count || count >= RADIOS_AUDIO_RUN_MAX_ITEMS)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid future queue");
    g_mutex_lock(&engine->mutex);
    AudioRunner *run = existing_runner(engine, (guint)(source + 1));
    if (!run || engine->closing || !run->active || run->epoch != epoch || run->current < 0 ||
        engine->decks[run->current].generation != generation || g_strcmp0(run->items[run->index].entry_id, entry_id)) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Current source changed while preparing the future queue");
    }
    if (run->overlap || run->ending || run->cut_remaining || run->handover_pending || run->pending_stop || run->pending_jump >= 0) {
        g_mutex_unlock(&engine->mutex); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Wait until the active transition has finished"); return FALSE;
    }
    guint prefix = run->index + 1;
    if (count > RADIOS_AUDIO_RUN_MAX_ITEMS - prefix) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Future queue exceeds the bounded runner capacity");
    }
    RadioSAudioRunItem *combined = g_new(RadioSAudioRunItem, prefix + count);
    for (guint i = 0; i < prefix; ++i) combined[i] = run->items[i].spec;
    for (guint i = 0; i < count; ++i) combined[prefix + i] = items[i];
    /* The new policy governs the next boundary; the sounding media, cue,
     * tempo and adjunct keep their accepted geometry and decoder state. */
    RadioSAudioRunItem *current = &combined[prefix - 1];
    current->mode = items[0].mode; current->overlap_frames = items[0].overlap_frames;
    current->effect = items[0].effect; current->shape = items[0].shape;
    if (!runner_validate(combined, prefix + count, &run->options, error)) {
        g_free(combined); g_mutex_unlock(&engine->mutex); return FALSE;
    }
    for (guint i = 0; i < count; ++i) if (items[i].adjunct.enabled &&
        !run->adjunct_owned[items[i].adjunct.player] && !runner_adjunct_available_locked(engine, items[i].adjunct.player, run)) {
        g_free(combined); g_mutex_unlock(&engine->mutex);
        return fail(error, RADIOS_AUDIO_ERROR_STATE, "The new future needs an occupied auxiliary player");
    }
    RunEntry *copy = g_new0(RunEntry, RADIOS_AUDIO_RUN_MAX_ITEMS);
    for (guint i = 0; i < prefix + count; ++i) runner_copy_entry(&copy[i], &combined[i]);
    g_free(combined);
    AudioRunner *previous = engine->processing_runner; engine->processing_runner = run;
    schedule_flush_locked(engine);
    for (guint d = 0; d < 32; ++d) if ((gint)d != run->current && run->loaded_index[d] >= (gint)prefix)
        runner_clear_deck_locked(engine, d);
    for (guint d = 0; d < 32; ++d) if (run->adjunct[d].generation && run->adjunct[d].item >= (gint)prefix) {
        runner_clear_deck_locked(engine, d); memset(&run->adjunct[d], 0, sizeof run->adjunct[d]);
    }
    g_free(run->items); run->items = copy; run->count = prefix + count; run->next = -1;
    memset(run->adjunct_completed + prefix, 0, (RADIOS_AUDIO_RUN_MAX_ITEMS - prefix) * sizeof run->adjunct_completed[0]);
    for (guint i = prefix; i < run->count; ++i) if (copy[i].spec.adjunct.enabled) {
        guint d = copy[i].spec.adjunct.player; run->adjunct_owned[d] = TRUE; engine->deck_runner[d] = run;
    }
    engine->processing_runner = previous;
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
/* The kept successor must be decoded exactly as the new entry would load. */
static gboolean runner_entry_same_load(const RunEntry *old, const RadioSAudioRunItem *item)
{
    const RadioSAudioRunItem *a = &old->spec;
    guint64 out = item->media.cue_out_frame ? item->media.cue_out_frame : item->duration_frames;
    if (g_strcmp0(old->entry_id, item->entry_id) || g_strcmp0(old->path, item->media.path) ||
        g_ascii_strcasecmp(old->sha, item->media.sha256 ? item->media.sha256 : "") ||
        g_strcmp0(old->media_id, item->media.media_id) || g_strcmp0(old->version_id, item->media.version_id) ||
        a->media.cue_in_frame != item->media.cue_in_frame || a->media.cue_out_frame != out ||
        a->duration_frames != item->duration_frames || a->preferred_deck != item->preferred_deck ||
        a->tempo_ratio != item->tempo_ratio || a->tempo_return_start_frames != item->tempo_return_start_frames ||
        a->tempo_return_frames != item->tempo_return_frames || a->beat_sync != item->beat_sync ||
        memcmp(&a->rhythm, &item->rhythm, sizeof a->rhythm) || memcmp(&a->loudness, &item->loudness, sizeof a->loudness) ||
        a->adjunct.enabled != item->adjunct.enabled) return FALSE;
    if (!a->adjunct.enabled) return TRUE;
    const RadioSAudioRunAdjunct *x = &a->adjunct, *y = &item->adjunct;
    return !g_strcmp0(old->adjunct_entry, y->entry_id) && !g_strcmp0(old->adjunct_media, y->media.media_id) &&
        !g_strcmp0(old->adjunct_version, y->media.version_id) && x->player == y->player &&
        x->media.cue_in_frame == y->media.cue_in_frame && x->media.cue_out_frame == y->media.cue_out_frame &&
        x->start_output_frame == y->start_output_frame && x->fade_frames == y->fade_frames &&
        x->gain_db == y->gain_db && x->duck_db == y->duck_db && !memcmp(&x->loudness, &y->loudness, sizeof x->loudness);
}
/* Kullanıcı isteği U7 (× in the running order): replace only the entries
 * that have not sounded. Unlike replace_future the sounding item keeps its
 * own accepted exit policy (mode, overlap, effect, shape — e.g. the MANUAL
 * wait before a live step), media, cue, tempo, adjunct and decoder state;
 * count 0 ends the queue after it. Only a policy the new successor makes
 * physically impossible is adapted, as the planner does: an overlap longer
 * than the successor's cue span is capped, an overlap onto the same planned
 * player becomes a plain cut. While the loaded successor stays the same
 * entry at the same place it stays loaded and nothing is flushed; otherwise
 * the old future is released like replace_future. Preloaded pool players
 * of removed or moved entries are released and refill in pool order (U1). */
gboolean radios_audio_engine_runner_remove_future(RadioSAudioEngine *engine, gint source,
    guint64 epoch, guint64 generation, const gchar *entry_id,
    const RadioSAudioRunItem *items, guint count, GError **error)
{
    if (!engine || source < -1 || source >= 32 || (count && !items) || count >= RADIOS_AUDIO_RUN_MAX_ITEMS)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid future queue");
    g_mutex_lock(&engine->mutex);
    AudioRunner *run = existing_runner(engine, (guint)(source + 1));
    if (!run || engine->closing || !run->active || run->epoch != epoch || run->current < 0 ||
        engine->decks[run->current].generation != generation || g_strcmp0(run->items[run->index].entry_id, entry_id)) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Current source changed while preparing the future queue");
    }
    if (run->overlap || run->ending || run->cut_remaining || run->handover_pending || run->pending_stop || run->pending_jump >= 0) {
        g_mutex_unlock(&engine->mutex); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Wait until the active transition has finished"); return FALSE;
    }
    guint prefix = run->index + 1;
    if (count > RADIOS_AUDIO_RUN_MAX_ITEMS - prefix) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Future queue exceeds the bounded runner capacity");
    }
    RadioSAudioRunItem *combined = g_new(RadioSAudioRunItem, prefix + count);
    for (guint i = 0; i < prefix; ++i) combined[i] = run->items[i].spec;
    for (guint i = 0; i < count; ++i) combined[prefix + i] = items[i];
    if (count) {
        RadioSAudioRunItem *current = &combined[prefix - 1];
        guint64 out = items[0].media.cue_out_frame ? items[0].media.cue_out_frame : items[0].duration_frames;
        guint64 span = out > items[0].media.cue_in_frame ? out - items[0].media.cue_in_frame : 0;
        current->overlap_frames = MIN(current->overlap_frames, span);
        if (current->overlap_frames && current->preferred_deck >= 0 && current->preferred_deck == items[0].preferred_deck)
            current->overlap_frames = 0;
        if (!current->overlap_frames) { current->effect = RADIOS_AUDIO_RUN_EFFECT_NONE; current->shape.enabled = FALSE; }
    }
    if (!runner_validate(combined, prefix + count, &run->options, error)) {
        g_free(combined); g_mutex_unlock(&engine->mutex); return FALSE;
    }
    for (guint i = 0; i < count; ++i) if (items[i].adjunct.enabled &&
        !run->adjunct_owned[items[i].adjunct.player] && !runner_adjunct_available_locked(engine, items[i].adjunct.player, run)) {
        g_free(combined); g_mutex_unlock(&engine->mutex);
        return fail(error, RADIOS_AUDIO_ERROR_STATE, "The new future needs an occupied auxiliary player");
    }
    /* Rendered marks name the loaded successor by player and index: keeping
     * it in place keeps them true, so the heard PCM needs no flush. */
    gint keep = run->next >= 0 && count && run->loaded_index[run->next] == (gint)prefix &&
        runner_entry_same_load(&run->items[prefix], &items[0]) ? run->next : -1;
    RunEntry *copy = g_new0(RunEntry, RADIOS_AUDIO_RUN_MAX_ITEMS);
    for (guint i = 0; i < prefix + count; ++i) runner_copy_entry(&copy[i], &combined[i]);
    g_free(combined);
    AudioRunner *previous = engine->processing_runner; engine->processing_runner = run;
    if (keep < 0) schedule_flush_locked(engine);
    for (guint d = 0; d < 32; ++d) if ((gint)d != run->current && (gint)d != keep && run->loaded_index[d] >= (gint)prefix)
        runner_clear_deck_locked(engine, d);
    for (guint d = 0; d < 32; ++d) if (run->adjunct[d].generation && run->adjunct[d].item >= (gint)prefix &&
        (keep < 0 || run->adjunct[d].item != (gint)prefix)) {
        runner_clear_deck_locked(engine, d); memset(&run->adjunct[d], 0, sizeof run->adjunct[d]);
    }
    gboolean kept_adjunct = keep >= 0 && run->adjunct_completed[prefix];
    g_free(run->items); run->items = copy; run->count = prefix + count; run->next = keep;
    memset(run->adjunct_completed + prefix, 0, (RADIOS_AUDIO_RUN_MAX_ITEMS - prefix) * sizeof run->adjunct_completed[0]);
    run->adjunct_completed[prefix] = kept_adjunct;
    for (guint i = prefix; i < run->count; ++i) if (copy[i].spec.adjunct.enabled) {
        guint d = copy[i].spec.adjunct.player; run->adjunct_owned[d] = TRUE; engine->deck_runner[d] = run;
    }
    engine->processing_runner = previous;
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
static gboolean runner_start_internal(RadioSAudioEngine *engine, gint adopt, guint64 generation, gboolean replace, GError **error)
{
    if (!engine) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid audio engine");
    g_mutex_lock(&engine->mutex); AudioRunner *run = active_runner(engine);
    if (engine->closing || !run->items || run->active || runner_has_leases(run)) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Automation has no idle configured queue");
    }
    if (adopt >= 0) {
        AudioDeck *source = &engine->decks[adopt]; RunEntry *first = &run->items[0];
        RadioSAudioTempoProfile profile = deck_tempo_profile(source);
        gdouble actual_ratio = radios_audio_tempo_profile_ratio(&profile,
            deck_output_position(source, actual_cursor_locked(engine, source)));
        if (first->spec.preferred_deck != adopt || source->generation != generation ||
            (source->state != RADIOS_AUDIO_READY && source->state != RADIOS_AUDIO_PLAYING && source->state != RADIOS_AUDIO_PAUSED) ||
            g_strcmp0(source->path, first->path) || g_strcmp0(source->sha, first->sha) ||
            g_strcmp0(source->media_id, first->media_id) || g_strcmp0(source->version_id, first->version_id) ||
            source->cue_in > first->spec.media.cue_in_frame || source->cue_out < first->spec.media.cue_out_frame ||
            fabs(actual_ratio - (first->spec.tempo_ratio ? first->spec.tempo_ratio : 1)) > 0.000001 ||
            actual_cursor_locked(engine, source) >= first->spec.media.cue_out_frame) {
            g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "AutoDJ kaynağı, medya sürümü veya temposu değişti; çalan ses korunuyor.");
        }
    }
    for (guint n = 0; n < run->options.pool_count; ++n) {
        guint index = run->options.deck_pool[n]; AudioDeck *deck = &engine->decks[index]; gboolean mapped = FALSE;
        for (guint c = 0; c < 32; ++c) mapped |= engine->mixer.channels[c].source_deck == (gint)index;
        if (((gint)index != adopt && deck->state != RADIOS_AUDIO_EMPTY && (!replace || deck->state == RADIOS_AUDIO_PLAYING)) ||
            runner_other_owns(engine,index,run) || !deck->main_routed || !mapped) {
            g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Automation requires empty normal decks mapped to MAIN; a manual deck is never stolen");
        }
    }
    gboolean auxiliary[32] = {FALSE}, skipped_auxiliary[32] = {FALSE};
    for (guint i=0;i<run->count;++i) if (run->items[i].spec.adjunct.enabled) auxiliary[run->items[i].spec.adjunct.player]=TRUE;
    for (guint d=0;d<32;++d) if (auxiliary[d] && !runner_adjunct_available_locked(engine,d,run)) {
        if (!run->options.skip_unavailable_adjuncts) {
            g_mutex_unlock(&engine->mutex); return fail(error,RADIOS_AUDIO_ERROR_STATE,"Jingle requires its explicit empty MAIN player; occupied or CUE players are never stolen");
        }
        auxiliary[d]=FALSE; skipped_auxiliary[d]=TRUE;
    }
    /* Only this accepted immutable run omits unavailable optional voices. The
     * caller's programme, explicit targets and other players are unchanged. */
    for (guint i=0;i<run->count;++i) if (run->items[i].spec.adjunct.enabled &&
        skipped_auxiliary[run->items[i].spec.adjunct.player]) run->items[i].spec.adjunct.enabled=FALSE;
    /* Validate the entire adoption and pool before clearing any player. A
     * failed start leaves the source and prepared target contents intact. */
    if (replace) for (guint n = 0; n < run->options.pool_count; ++n) {
        guint index = run->options.deck_pool[n];
        if ((gint)index != adopt && engine->decks[index].state != RADIOS_AUDIO_EMPTY) runner_clear_deck_locked(engine, index);
    }
    run->index = 0; run->current = run->next = run->pending_jump = -1; run->active = TRUE; run->paused = run->overlap = run->ending = run->pending_stop = FALSE;
    run->loudness_pair_source = run->loudness_pair_target = -1;
    run->handover_pending = run->handover_active = run->handover_cut = FALSE;
    run->handover_completed = FALSE;
    run->transition = run->transition_total = run->finish_frames = run->final_tag = 0; run->cut_remaining = 0;
    run->state = RADIOS_AUDIO_RUN_PREROLL; run->error[0] = '\0'; ++run->epoch;
    for (guint n = 0; n < run->options.pool_count; ++n) {
        guint index = run->options.deck_pool[n]; AudioDeck *deck = &engine->decks[index];
        run->owned[index] = TRUE; engine->deck_runner[index] = run; run->loaded_index[index] = -1; run->retire_tag[index] = 0;
        if (!deck->thread) { ring_init(&deck->pcm, PCM_CAPACITY, FALSE); deck->thread = g_thread_new("radios-decode", deck_worker, deck); }
    }
    for (guint d=0;d<32;++d) if (auxiliary[d]) { run->adjunct_owned[d]=TRUE; engine->deck_runner[d]=run; }
    memset(run->adjunct,0,sizeof run->adjunct); memset(run->adjunct_completed,0,sizeof run->adjunct_completed);
    if (adopt >= 0) {
        AudioDeck *source = &engine->decks[adopt]; gboolean was_playing = source->state == RADIOS_AUDIO_PLAYING;
        run->current = adopt; run->loaded_index[adopt] = 0;
        run->started[adopt] = FALSE; run->first_tag[adopt] = run->receipt_occurrence[adopt] = 0;
        run->retire_terminal[adopt] = RADIOS_AUDIO_RUN_TERMINAL_NONE;
        local_binding_locked(source, &run->items[0].spec.media, NULL, run->playlist_id, run->items[0].entry_id);
        source->cue_in = run->items[0].spec.media.cue_in_frame; source->cue_out = run->items[0].spec.media.cue_out_frame;
        source->rhythm = run->items[0].spec.rhythm; source->sync_master = -1; source->sync_manual = FALSE;
        source->loudness = run->items[0].spec.loudness; deck_gain_locked(engine, source);
        source->transition_filter = (RadioSAudioTransitionFilter){0};
        source->requested_play = FALSE;
        if ((!was_playing && actual_cursor_locked(engine, source) != source->cue_in) || actual_cursor_locked(engine, source) < source->cue_in)
            request_locked(engine, source, source->cue_in, FALSE);
    }
    if (!engine->runner.thread) engine->runner.thread = g_thread_new("radios-runner", runner_worker, engine);
    runner_control_locked(engine); g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_runner_start(RadioSAudioEngine *engine, GError **error)
{ return runner_start_internal(engine, -1, 0, FALSE, error); }
gboolean radios_audio_engine_runner_start_adopting(RadioSAudioEngine *engine, guint deck, guint64 generation, GError **error)
{
    if (deck >= RADIOS_AUDIO_NORMAL_DECKS || !generation) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "AutoDJ kaynak oynatıcısı geçersiz.");
    return runner_start_internal(engine, (gint)deck, generation, FALSE, error);
}
gboolean radios_audio_engine_runner_start_replacing(RadioSAudioEngine *engine, gint adopt, guint64 generation, GError **error)
{
    if (adopt < -1 || adopt >= (gint)RADIOS_AUDIO_NORMAL_DECKS) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid AutoDJ adoption deck");
    return runner_start_internal(engine, adopt, generation, TRUE, error);
}
gboolean radios_audio_engine_runner_pause(RadioSAudioEngine *engine, GError **error)
{
    if (!engine) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid audio engine");
    g_mutex_lock(&engine->mutex); AudioRunner *run = active_runner(engine);
    if (!run->active || run->state == RADIOS_AUDIO_RUN_ERROR) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Automation is not running");
    }
    schedule_flush_locked(engine); run->paused = TRUE; run->state = RADIOS_AUDIO_RUN_PAUSED;
    for (guint i = 0; i < 32; ++i) if (run->owned[i]) {
        AudioDeck *deck = &engine->decks[i]; deck->requested_play = FALSE;
        if (deck->sync_manual) {
            guint64 cursor = actual_cursor_locked(engine, deck);
            manual_sync_reset_locked(deck); request_locked(engine, deck, cursor, FALSE);
            deck->pause_after_load = TRUE; run->started[i] = FALSE;
        }
    }
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_runner_resume(RadioSAudioEngine *engine, GError **error)
{
    if (!engine) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid audio engine");
    g_mutex_lock(&engine->mutex); AudioRunner *run = active_runner(engine);
    if (!run->active || !run->paused || run->state == RADIOS_AUDIO_RUN_ERROR) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Automation is not paused");
    }
    /* A flush still settling (the pause's own or any other) is no reason to
     * refuse: the resume is kept and rendering continues from the heard point
     * once the flush is done. Only a closed or unavailable MAIN refuses. */
    if (!engine->gates[0] || (engine->realtime && !engine->physical.ready[0])) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_UNAVAILABLE, "MAIN output is not ready; automation remains paused");
    }
    for (guint i = 0; i < 32; ++i) if (run->owned[i] && run->started[i] && engine->decks[i].state == RADIOS_AUDIO_PAUSED) {
        engine->decks[i].state = RADIOS_AUDIO_PLAYING; engine->decks[i].error[0] = '\0';
    }
    run->paused = FALSE; run->state = run->overlap ? RADIOS_AUDIO_RUN_OVERLAP : RADIOS_AUDIO_RUN_PLAYING; run->error[0] = '\0';
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_runner_stop(RadioSAudioEngine *engine, GError **error)
{
    if (!engine) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid audio engine");
    g_mutex_lock(&engine->mutex); runner_cancel_locked(engine, -1); g_mutex_unlock(&engine->mutex); return TRUE;
}
static void runner_handover_cancel_locked(AudioRunner *run)
{
    if (run->handover_active && run->state != RADIOS_AUDIO_RUN_FINISHED)
        run->ending = run->handover_ending_before;
    if (run->handover_cut && run->state != RADIOS_AUDIO_RUN_FINISHED) {
        run->cut_remaining = 0; run->pending_stop = FALSE;
    }
    run->handover_pending = run->handover_active = run->handover_cut = FALSE;
    run->handover_completed = FALSE;
}
gboolean radios_audio_engine_runner_handover(RadioSAudioEngine *engine,
    guint64 expected_epoch, gint64 target_utc_us, RadioSAudioRunStartPolicy policy,
    guint64 max_wait_frames, guint fade_frames, GError **error)
{
    if (!engine || !expected_epoch || target_utc_us <= 0 ||
        (policy != RADIOS_AUDIO_RUN_START_CUT && policy != RADIOS_AUDIO_RUN_START_FINISH) ||
        max_wait_frames > G_GUINT64_CONSTANT(4147200000) || fade_frames > 48000)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid bounded runner handover");
    g_mutex_lock(&engine->mutex); AudioRunner *run = active_runner(engine);
    if (engine->closing || run->epoch != expected_epoch || !run->active ||
        run->state == RADIOS_AUDIO_RUN_ERROR || run->state == RADIOS_AUDIO_RUN_FINISHED ||
        run->cut_remaining || run->pending_stop || run->pending_jump >= 0 || run->handover_active) {
        g_mutex_unlock(&engine->mutex);
        return fail(error, RADIOS_AUDIO_ERROR_STATE, "Handover requires the same live owned runner without a pending cut");
    }
    run->handover_pending = TRUE; run->handover_completed = FALSE; run->handover_utc_us = target_utc_us;
    run->handover_policy = policy; run->handover_max_wait_frames = max_wait_frames;
    run->handover_fade_frames = fade_frames;
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
void radios_audio_engine_runner_handover_cancel(RadioSAudioEngine *engine, guint64 expected_epoch)
{
    if (!engine) return;
    g_mutex_lock(&engine->mutex);
    if (active_runner(engine)->epoch == expected_epoch) runner_handover_cancel_locked(active_runner(engine));
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex);
}
static gboolean runner_jump(RadioSAudioEngine *engine, gboolean previous, GError **error)
{
    if (!engine) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid audio engine");
    g_mutex_lock(&engine->mutex); AudioRunner *run = active_runner(engine);
    if (!run->active || run->paused || run->state == RADIOS_AUDIO_RUN_ERROR) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Automation is not playing");
    }
    runner_handover_cancel_locked(run);
    runner_terminal_locked(engine, RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED);
    schedule_flush_locked(engine);
    run->pending_jump = previous ? (gint)(run->index ? run->index - 1 : 0) : (gint)run->index + 1;
    run->pending_stop = run->pending_jump >= (gint)run->count;
    run->cut_total = run->cut_remaining = 240;
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
gboolean radios_audio_engine_runner_next(RadioSAudioEngine *engine, GError **error)
{ return runner_jump(engine, FALSE, error); }
gboolean radios_audio_engine_runner_previous(RadioSAudioEngine *engine, GError **error)
{ return runner_jump(engine, TRUE, error); }
gboolean radios_audio_engine_runner_jump_to(RadioSAudioEngine *engine,
    guint64 expected_epoch, guint index, GError **error)
{
    if (!engine || !expected_epoch) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid bounded queue jump");
    g_mutex_lock(&engine->mutex); AudioRunner *run = active_runner(engine);
    RunMark point = runner_mark(run, 0);
    if (run->active) (void)runner_audible_locked(engine, &point);
    if (engine->closing || run->epoch != expected_epoch || !run->active || run->paused ||
        run->state == RADIOS_AUDIO_RUN_ERROR || index >= run->count || run->cut_remaining ||
        run->pending_stop || run->pending_jump >= 0 ||
        (point.state != RADIOS_AUDIO_RUN_PLAYING && point.state != RADIOS_AUDIO_RUN_OVERLAP &&
         point.state != RADIOS_AUDIO_RUN_MANUAL_WAIT)) {
        g_mutex_unlock(&engine->mutex);
        return fail(error, RADIOS_AUDIO_ERROR_STATE, "Queue jump requires the same playing queue and a valid target");
    }
    runner_handover_cancel_locked(run);
    runner_terminal_locked(engine, RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED);
    schedule_flush_locked(engine);
    run->pending_jump = (gint)index; run->pending_stop = FALSE;
    run->cut_total = run->cut_remaining = 240;
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}
void radios_audio_engine_runner_snapshot(RadioSAudioEngine *engine, RadioSAudioRunSnapshot *snapshot)
{
    g_return_if_fail(engine && snapshot); memset(snapshot, 0, sizeof *snapshot);
    g_mutex_lock(&engine->mutex); AudioRunner *run = active_runner(engine); RunMark point = runner_mark(run, 0);
    if (run->active) (void)runner_audible_locked(engine, &point);
    snapshot->state = run->paused || run->state == RADIOS_AUDIO_RUN_ERROR ? run->state : point.state;
    snapshot->epoch = run->epoch; snapshot->count = run->count; snapshot->index = point.index; snapshot->next_index = point.next_index;
    snapshot->deck_current = point.current; snapshot->deck_next = point.next; snapshot->transition_frames = point.transition;
    snapshot->transition_total_frames = point.transition_total;
    if (point.current >= 0) snapshot->actual_current_frame = actual_cursor_locked(engine, &engine->decks[point.current]);
    if (point.next >= 0) snapshot->actual_next_frame = actual_cursor_locked(engine, &engine->decks[point.next]);
    if (run->state == RADIOS_AUDIO_RUN_FINISHED && !run->active) snapshot->actual_current_frame = run->final_frame;
    if (point.index < run->count) g_strlcpy(snapshot->entry_id, run->items[point.index].entry_id, sizeof snapshot->entry_id);
    if (point.next_index < run->count) g_strlcpy(snapshot->next_entry_id, run->items[point.next_index].entry_id, sizeof snapshot->next_entry_id);
    g_strlcpy(snapshot->error, run->error, sizeof snapshot->error); memcpy(snapshot->owned, run->owned, sizeof snapshot->owned);
    snapshot->handover_pending = run->handover_pending;
    snapshot->handover_completed = run->handover_completed;
    g_mutex_unlock(&engine->mutex);
}
guint radios_audio_engine_runner_receipts(RadioSAudioEngine *engine, RadioSAudioRunReceipt *out, guint capacity)
{
    g_return_val_if_fail(engine && (out || !capacity), 0); guint copied = 0;
    g_mutex_lock(&engine->mutex); runner_receipts_locked(engine);
    guint64 written = active_runner(engine)->receipt_write;
    guint count = (guint)MIN(written, MIN(capacity, RADIOS_AUDIO_RUN_MAX_RECEIPTS));
    for (guint64 i = written - count; i < written; ++i)
        out[copied++] = active_runner(engine)->receipts[i % RADIOS_AUDIO_RUN_MAX_RECEIPTS];
    g_mutex_unlock(&engine->mutex); return copied;
}
static gboolean player_runner_exists(RadioSAudioEngine *engine, guint deck)
{
    if (!engine || deck >= 32) return FALSE;
    g_mutex_lock(&engine->mutex); gboolean exists = engine->player_runners[deck] != NULL && !engine->closing;
    g_mutex_unlock(&engine->mutex); return exists;
}
gboolean radios_audio_engine_player_runner_configure(RadioSAudioEngine *engine, guint deck,
    const RadioSAudioRunItem *items, guint count, const RadioSAudioRunOptions *options, GError **error)
{
    if (!engine || deck >= 32 || !items || !count || count > RADIOS_AUDIO_RUN_MAX_ITEMS || !options)
        return fail(error,RADIOS_AUDIO_ERROR_INVALID,"Invalid player queue");
    for (guint index = 0; index < count; ++index)
        if (items[index].mode == RADIOS_AUDIO_RUN_CROSSFADE && items[index].overlap_frames)
            return fail(error,RADIOS_AUDIO_ERROR_INVALID,"An independent single-player queue cannot crossfade between two physical decks");
    RadioSAudioRunItem *copies = g_new(RadioSAudioRunItem, count);
    for (guint index = 0; index < count; ++index) { copies[index] = items[index]; copies[index].preferred_deck = (gint)deck; }
    RadioSAudioRunOptions scoped = *options; scoped.pool_count = 1; scoped.deck_pool[0] = deck;
    g_mutex_lock(&engine->mutex);
    if (engine->closing) { g_mutex_unlock(&engine->mutex); g_free(copies); return fail(error,RADIOS_AUDIO_ERROR_CLOSED,"Audio engine is closed"); }
    if (!engine->player_runners[deck]) {
        AudioRunner *run = g_new0(AudioRunner,1); run->slot = deck + 1; run->current = run->next = run->pending_jump = -1;
        for (guint index = 0; index < 32; ++index) run->loaded_index[index] = -1;
        for (guint side = 0; side < 2; ++side) run->shape_voice[side] = radios_audio_shape_voice_new();
        engine->player_runners[deck] = run;
    }
    g_mutex_unlock(&engine->mutex);
    gpointer previous = g_private_get(&runner_scope); g_private_set(&runner_scope,GUINT_TO_POINTER(deck + 1));
    gboolean configured = radios_audio_engine_runner_configure(engine,copies,count,&scoped,error);
    g_private_set(&runner_scope,previous); g_free(copies); return configured;
}
gboolean radios_audio_engine_scope_runner_configure(RadioSAudioEngine *engine, guint source,
    const RadioSAudioRunItem *items, guint count, const RadioSAudioRunOptions *options, GError **error)
{
    if (!engine || source >= RADIOS_AUDIO_NORMAL_DECKS)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid automation scope source");
    if (!runner_validate(items, count, options, error)) return FALSE;
    gboolean contains_source = FALSE;
    for (guint i = 0; i < options->pool_count; ++i) contains_source |= options->deck_pool[i] == source;
    if (!contains_source) return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Automation scope pool must contain its source player");
    g_mutex_lock(&engine->mutex);
    if (engine->closing) { g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_CLOSED, "Audio engine is closed"); }
    AudioRunner *run = engine->player_runners[source];
    for (guint i = 0; i < options->pool_count; ++i) if (runner_other_owns(engine, options->deck_pool[i], run)) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Automation scope overlaps another running queue");
    }
    if (!run) {
        run = g_new0(AudioRunner, 1); run->slot = source + 1;
        run->current = run->next = run->pending_jump = -1;
        for (guint i = 0; i < RADIOS_AUDIO_NORMAL_DECKS; ++i) run->loaded_index[i] = -1;
        for (guint side = 0; side < 2; ++side) run->shape_voice[side] = radios_audio_shape_voice_new();
        engine->player_runners[source] = run;
    }
    g_mutex_unlock(&engine->mutex);
    gpointer previous = g_private_get(&runner_scope); g_private_set(&runner_scope, GUINT_TO_POINTER(source + 1));
    gboolean result = runner_configure_internal(engine, items, count, options, TRUE, error);
    g_private_set(&runner_scope, previous); return result;
}
gboolean radios_audio_engine_scope_runner_start_replacing(RadioSAudioEngine *engine, guint source,
    gint adopt, guint64 generation, GError **error)
{
    if (!player_runner_exists(engine, source)) return fail(error, RADIOS_AUDIO_ERROR_STATE, "Automation scope is not configured");
    gpointer previous = g_private_get(&runner_scope); g_private_set(&runner_scope, GUINT_TO_POINTER(source + 1));
    gboolean result = radios_audio_engine_runner_start_replacing(engine, adopt, generation, error);
    g_private_set(&runner_scope, previous); return result;
}
gboolean radios_audio_engine_player_runner_action(RadioSAudioEngine *engine, guint deck, RadioSAudioPlayerRunCommand command, GError **error)
{
    if (!engine || deck >= 32) return fail(error,RADIOS_AUDIO_ERROR_INVALID,"Invalid player queue");
    if (!player_runner_exists(engine,deck)) return command == RADIOS_AUDIO_PLAYER_RUN_STOP ? TRUE : fail(error,RADIOS_AUDIO_ERROR_STATE,"Player queue is not configured");
    gpointer previous = g_private_get(&runner_scope); g_private_set(&runner_scope,GUINT_TO_POINTER(deck + 1)); gboolean result;
    switch (command) {
    case RADIOS_AUDIO_PLAYER_RUN_START: result = radios_audio_engine_runner_start(engine,error); break;
    case RADIOS_AUDIO_PLAYER_RUN_PAUSE: result = radios_audio_engine_runner_pause(engine,error); break;
    case RADIOS_AUDIO_PLAYER_RUN_RESUME: result = radios_audio_engine_runner_resume(engine,error); break;
    case RADIOS_AUDIO_PLAYER_RUN_STOP: result = radios_audio_engine_runner_stop(engine,error); break;
    case RADIOS_AUDIO_PLAYER_RUN_NEXT: result = radios_audio_engine_runner_next(engine,error); break;
    case RADIOS_AUDIO_PLAYER_RUN_PREVIOUS: result = radios_audio_engine_runner_previous(engine,error); break;
    default: result = fail(error,RADIOS_AUDIO_ERROR_INVALID,"Invalid player queue command"); break;
    }
    g_private_set(&runner_scope,previous); return result;
}
gboolean radios_audio_engine_player_runner_jump_to(RadioSAudioEngine *engine,
    guint deck, guint64 expected_epoch, guint index, GError **error)
{
    if (!engine || deck >= 32 || !player_runner_exists(engine, deck))
        return fail(error, RADIOS_AUDIO_ERROR_STATE, "Player queue is not configured");
    gpointer previous = g_private_get(&runner_scope);
    g_private_set(&runner_scope, GUINT_TO_POINTER(deck + 1));
    gboolean result = radios_audio_engine_runner_jump_to(engine, expected_epoch, index, error);
    g_private_set(&runner_scope, previous); return result;
}
void radios_audio_engine_player_runner_snapshot(RadioSAudioEngine *engine, guint deck, RadioSAudioRunSnapshot *snapshot)
{
    g_return_if_fail(snapshot != NULL);
    *snapshot = (RadioSAudioRunSnapshot){.state=RADIOS_AUDIO_RUN_IDLE,.deck_current=-1,.deck_next=-1};
    if (!player_runner_exists(engine,deck)) return;
    gpointer previous = g_private_get(&runner_scope); g_private_set(&runner_scope,GUINT_TO_POINTER(deck + 1));
    radios_audio_engine_runner_snapshot(engine,snapshot); g_private_set(&runner_scope,previous);
}
guint radios_audio_engine_player_runner_receipts(RadioSAudioEngine *engine, guint deck, RadioSAudioRunReceipt *out, guint capacity)
{
    if (!player_runner_exists(engine,deck)) return 0;
    gpointer previous = g_private_get(&runner_scope); g_private_set(&runner_scope,GUINT_TO_POINTER(deck + 1));
    guint count = radios_audio_engine_runner_receipts(engine,out,capacity); g_private_set(&runner_scope,previous); return count;
}

gboolean radios_audio_engine_runner_copyright(RadioSAudioEngine *engine, gint source,
    const RadioSAudioCopyrightWindow *windows, guint count, GError **error)
{
    if (!engine || source < -1 || source >= 32 || !windows || !count || count > RADIOS_AUDIO_RUN_MAX_ITEMS)
        return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid copyright queue update");
    g_mutex_lock(&engine->mutex);
    AudioRunner *run = source < 0 ? &engine->runner : engine->player_runners[source];
    if (engine->closing || !run || !run->items || run->count != count) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_STATE, "Copyright queue changed before update");
    }
    for (guint i = 0; i < count; ++i) if (windows[i].starts_at_utc_us < 0 || windows[i].ends_at_utc_us < 0) {
        g_mutex_unlock(&engine->mutex); return fail(error, RADIOS_AUDIO_ERROR_INVALID, "Invalid copyright UTC interval");
    }
    for (guint i = 0; i < count; ++i) run->items[i].spec.copyright = windows[i];
    g_cond_broadcast(&engine->condition); g_mutex_unlock(&engine->mutex); return TRUE;
}

gboolean radios_audio_engine_runner_adjunct_copyright(RadioSAudioEngine *engine,gint source,
    const RadioSAudioCopyrightWindow *windows,guint count,GError **error)
{
    if (!engine || source < -1 || source >= 32 || !windows || !count || count > RADIOS_AUDIO_RUN_MAX_ITEMS)
        return fail(error,RADIOS_AUDIO_ERROR_INVALID,"Invalid jingle copyright queue update");
    g_mutex_lock(&engine->mutex);AudioRunner *run=source<0?&engine->runner:engine->player_runners[source];
    if (engine->closing || !run || !run->items || run->count!=count) {
        g_mutex_unlock(&engine->mutex);return fail(error,RADIOS_AUDIO_ERROR_STATE,"Jingle queue changed before copyright update");
    }
    for (guint i=0;i<count;++i) if (windows[i].starts_at_utc_us<0 || windows[i].ends_at_utc_us<0) {
        g_mutex_unlock(&engine->mutex);return fail(error,RADIOS_AUDIO_ERROR_INVALID,"Invalid jingle copyright UTC interval");
    }
    for (guint i=0;i<count;++i) run->items[i].spec.adjunct.copyright=windows[i];
    g_cond_broadcast(&engine->condition);g_mutex_unlock(&engine->mutex);return TRUE;
}
