/* Kılavuz §08 R8.7 / kullanıcı isteği U1 (18.10.2026): "4 player seçtim ama
 * 2'li 2'li yüklüyor". A pool of N players holds the playing item and the
 * next N-1 items, each on its planned player; transitions go through the
 * pool in order. Two-player pairs (AutoDJ continuation) and single-player
 * (Yarı AutoDJ) pools keep their former current/next behaviour. */
#include "radios/audio_runner.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#ifndef RADIOS_TEST_PROJECT_ROOT
#define RADIOS_TEST_PROJECT_ROOT "."
#endif
#define FILES 4
#define ENTRIES 16
#define FILE_FRAMES 192000u
static const gfloat levels[FILES] = {.0625f, .125f, .25f, .5f};
typedef struct {
    gchar *directory, *path[FILES], *sha[FILES], *media[FILES], *version[FILES], *entry[ENTRIES];
} Fixture;
static void append16(GByteArray *bytes, guint16 n)
{ guint8 b[] = {(guint8)n, (guint8)(n >> 8)}; g_byte_array_append(bytes, b, 2); }
static void append32(GByteArray *bytes, guint32 n)
{ guint8 b[] = {(guint8)n, (guint8)(n >> 8), (guint8)(n >> 16), (guint8)(n >> 24)}; g_byte_array_append(bytes, b, 4); }
static void setup(Fixture *f, gconstpointer unused)
{
    (void)unused; gchar *root = g_canonicalize_filename(RADIOS_TEST_PROJECT_ROOT, NULL), *build = g_build_filename(root, "build", NULL);
    g_assert_cmpint(g_mkdir_with_parents(build, 0700), ==, 0);
    f->directory = g_build_filename(build, "runner-pool-XXXXXX", NULL); g_assert_nonnull(g_mkdtemp(f->directory));
    for (guint file = 0; file < FILES; ++file) {
        gchar *name = g_strdup_printf("pool-%u.wav", file); f->path[file] = g_build_filename(f->directory, name, NULL); g_free(name);
        GByteArray *b = g_byte_array_new(); g_byte_array_append(b, (guint8 *)"RIFF", 4); append32(b, FILE_FRAMES * 4 + 36);
        g_byte_array_append(b, (guint8 *)"WAVEfmt ", 8); append32(b, 16); append16(b, 1); append16(b, 2); append32(b, 48000);
        append32(b, 192000); append16(b, 4); append16(b, 16); g_byte_array_append(b, (guint8 *)"data", 4); append32(b, FILE_FRAMES * 4);
        guint16 value = (guint16)(levels[file] * 32768);
        for (guint i = 0; i < FILE_FRAMES; ++i) { append16(b, value); append16(b, (guint16)-value); }
        g_assert_true(g_file_set_contents(f->path[file], (gchar *)b->data, b->len, NULL));
        f->sha[file] = g_compute_checksum_for_data(G_CHECKSUM_SHA256, b->data, b->len); g_byte_array_unref(b);
        f->media[file] = g_uuid_string_random(); f->version[file] = g_uuid_string_random();
    }
    for (guint i = 0; i < ENTRIES; ++i) f->entry[i] = g_uuid_string_random();
    g_free(root); g_free(build);
}
static void teardown(Fixture *f, gconstpointer unused)
{
    (void)unused;
    for (guint i = 0; i < FILES; ++i) {
        g_assert_cmpint(g_unlink(f->path[i]), ==, 0); g_free(f->path[i]); g_free(f->sha[i]); g_free(f->media[i]); g_free(f->version[i]);
    }
    for (guint i = 0; i < ENTRIES; ++i) g_free(f->entry[i]);
    g_assert_cmpint(g_rmdir(f->directory), ==, 0); g_free(f->directory);
}
/* Item i plays file i % FILES; its level identifies it in MAIN. */
static RadioSAudioRunItem item(Fixture *f, guint i, guint64 out, guint64 overlap, gint deck)
{
    RadioSAudioRunItem r = {0}; guint file = i % FILES;
    r.entry_id = f->entry[i]; r.media = (RadioSAudioLoadSpec){f->path[file], f->sha[file], f->media[file], f->version[file], 0, out};
    r.duration_frames = FILE_FRAMES; r.auto_next_frame = r.fade_start_frame = r.fade_end_frame = out; r.preferred_deck = deck;
    if (overlap) { r.mode = RADIOS_AUDIO_RUN_CROSSFADE; r.overlap_frames = overlap; }
    return r;
}
/* calendar_route_plan / start_prepared_scope place every item round-robin. */
static void queue(Fixture *f, RadioSAudioRunItem *q, guint count, guint pool, guint64 out, guint64 overlap)
{
    for (guint i = 0; i < count; ++i) {
        q[i] = item(f, i, out, i + 1 < count ? overlap : 0, (gint)(i % pool));
        if (i) q[i].fade_in_frames = overlap;
    }
}
static void options_pool(RadioSAudioRunOptions *o, guint pool)
{
    radios_audio_run_options_default(o); o->pool_count = pool;
    for (guint d = 0; d < pool; ++d) o->deck_pool[d] = d;
}
static void start(RadioSAudioEngine *engine, RadioSAudioRunItem *items, guint count, RadioSAudioRunOptions *options)
{
    GError *error = NULL; g_assert_true(radios_audio_engine_runner_configure(engine, items, count, options, &error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_runner_start(engine, &error)); g_assert_no_error(error);
}
static void render(RadioSAudioEngine *engine, guint frames, gfloat *main)
{
    static gfloat cue[8192 * 2], monitor[8192 * 2]; GError *error = NULL;
    g_assert_cmpuint(frames, <=, 8192);
    g_assert_true(radios_audio_engine_render(engine, frames, main, cue, monitor, &error)); g_assert_no_error(error);
}
/* Plan item held by the deck (its bound entry), -1 when empty. */
static gint held(Fixture *f, const RadioSAudioDeckSnapshot *deck)
{
    if (deck->state == RADIOS_AUDIO_EMPTY) return -1;
    for (guint i = 0; i < ENTRIES; ++i) if (g_str_equal(deck->binding.entry_id, f->entry[i])) return (gint)i;
    return -2;
}
static gchar *describe(Fixture *f, RadioSAudioEngine *engine, guint pool)
{
    RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1); radios_audio_engine_snapshot(engine, s);
    RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(engine, &r);
    GString *text = g_string_new(NULL);
    g_string_append_printf(text, "index %u current %d next %d state %d |", r.index, r.deck_current, r.deck_next, r.state);
    for (guint d = 0; d < pool; ++d) g_string_append_printf(text, " deck%u=%d/%d", d, held(f, &s->decks[d]), s->decks[d].state);
    g_free(s); return g_string_free(text, FALSE);
}
/* Wait for the control worker and the decoders: no pool deck loading, each
 * loaded deck buffered ahead, decks holding the expected items
 * (expect[d] = item, -1 empty, -3 any). */
static gboolean settled(Fixture *f, RadioSAudioEngine *engine, guint pool, const gint *expect)
{
    gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    do {
        RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1); radios_audio_engine_snapshot(engine, s);
        gboolean ok = TRUE;
        for (guint d = 0; d < pool; ++d) {
            const RadioSAudioDeckSnapshot *deck = &s->decks[d];
            guint64 remaining = deck->cue_out_frame > deck->cursor_frames ? deck->cue_out_frame - deck->cursor_frames : 0;
            ok &= deck->state != RADIOS_AUDIO_LOADING && deck->state != RADIOS_AUDIO_ERROR;
            ok &= deck->state == RADIOS_AUDIO_EMPTY || deck->buffered_frames >= MIN(remaining, G_GUINT64_CONSTANT(24000));
            if (expect && expect[d] != -3) ok &= held(f, deck) == expect[d];
        }
        g_free(s); if (ok) return TRUE; g_usleep(2000);
    } while (g_get_monotonic_time() < until);
    return FALSE;
}
/* Offline blocks render far faster than real time. Before the next block,
 * wait for the 10 ms control turn: retired players cleared, nothing loading,
 * every held item still upcoming (order[index..count-1]). A player holding
 * anything else after that is a stale preload. */
static void catch_up(Fixture *f, RadioSAudioEngine *engine, guint pool, const guint *order, guint count)
{
    gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; RadioSAudioRunSnapshot r;
    do {
        radios_audio_engine_runner_snapshot(engine, &r);
        RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1); radios_audio_engine_snapshot(engine, s);
        gboolean ok = r.state != RADIOS_AUDIO_RUN_ERROR;
        for (guint d = 0; d < pool; ++d) {
            gint item = held(f, &s->decks[d]); gboolean upcoming = item < 0;
            for (guint j = r.index; !upcoming && j < count; ++j) upcoming = (gint)order[j] == item;
            ok &= upcoming && s->decks[d].state != RADIOS_AUDIO_LOADING;
        }
        g_free(s);
        if (ok && settled(f, engine, pool, NULL)) return;
        g_usleep(2000);
    } while (g_get_monotonic_time() < until);
    gchar *state = describe(f, engine, pool); g_error("A player keeps an item that is no longer upcoming: %s", state);
}
static void expect_holding(Fixture *f, RadioSAudioEngine *engine, guint pool, const gint *expect)
{
    if (settled(f, engine, pool, expect)) return;
    gchar *state = describe(f, engine, pool); GString *want = g_string_new(NULL);
    for (guint d = 0; d < pool; ++d) g_string_append_printf(want, " deck%u=%d", d, expect[d]);
    g_error("Pool decks did not settle: %s (expected%s)", state, want->str);
}
/* Expected holdings while item `index` plays alone: index..index+pool-1. */
static void steady(Fixture *f, RadioSAudioEngine *engine, guint pool, guint index, guint count)
{
    gint expect[32];
    for (guint d = 0; d < pool; ++d) expect[d] = -1;
    for (guint j = 0; j < pool && index + j < count; ++j) expect[(index + j) % pool] = (gint)(index + j);
    expect_holding(f, engine, pool, expect);
}
static void near(gdouble actual, gdouble expected, guint frame)
{
    if (fabs(actual - expected) >= .00001) g_error("MAIN frame %u: %.9f, expected %.9f", frame, actual, expected);
}
/* Renders a whole queue in blocks, checks each steady point and collects the
 * order of current players and the MAIN programme. */
static guint run_queue(Fixture *f, RadioSAudioEngine *engine, guint pool, guint count, guint depth,
    gfloat *main, guint capacity, GString *order)
{
    guint rendered = 0; gint last = -1; RadioSAudioRunSnapshot r;
    for (;;) {
        radios_audio_engine_runner_snapshot(engine, &r);
        if (r.state == RADIOS_AUDIO_RUN_FINISHED || r.state == RADIOS_AUDIO_RUN_ERROR || rendered >= capacity) break;
        if (r.deck_current >= 0 && (gint)r.index != last) {
            last = (gint)r.index; g_string_append_printf(order, "%s%d", order->len ? "-" : "", r.deck_current + 1);
        }
        if (r.state == RADIOS_AUDIO_RUN_PLAYING) {
            gint expect[32];
            for (guint d = 0; d < pool; ++d) expect[d] = -1;
            for (guint j = 0; j < depth && r.index + j < count; ++j) expect[(r.index + j) % pool] = (gint)(r.index + j);
            /* A two-player pair never preloads beyond next; nothing else is held. */
            expect_holding(f, engine, pool, expect);
        } else g_assert_true(settled(f, engine, pool, NULL));
        render(engine, 500, main + (gsize)rendered * 2); rendered += 500;
    }
    g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_FINISHED);
    return rendered;
}
static void four_player_cluster_pcm(Fixture *f, gconstpointer unused)
{
    (void)unused; enum { COUNT = 10, POOL = 4, OUT = 6000, OVERLAP = 1000 };
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; options_pool(&o, POOL);
    RadioSAudioRunItem q[COUNT]; queue(f, q, COUNT, POOL, OUT, OVERLAP); start(engine, q, COUNT, &o);
    /* Before the first sample: every selected player holds its own item. */
    steady(f, engine, POOL, 0, COUNT);
    gsize capacity = 70000; gfloat *main = g_new0(gfloat, capacity * 2); GString *order = g_string_new(NULL);
    guint rendered = run_queue(f, engine, POOL, COUNT, POOL, main, (guint)capacity - 500, order);
    g_assert_cmpstr(order->str, ==, "1-2-3-4-1-2-3-4-1-2");
    /* Continuous programme: item i starts at i * (OUT - OVERLAP); pure
     * stretches carry its own level, overlaps the accepted envelope. */
    const guint step = OUT - OVERLAP, total = COUNT * OUT - (COUNT - 1) * OVERLAP;
    RadioSAudioTransition envelope = {OVERLAP, OVERLAP, OVERLAP, 0};
    for (guint frame = 0; frame < total; ++frame) {
        guint i = MIN(frame / step, COUNT - 1u), within = frame - i * step;
        gdouble expected = levels[i % FILES];
        if (within < OVERLAP && i > 0) {
            gdouble a, b; radios_audio_transition_gains(&envelope, within, &a, &b);
            expected = levels[(i - 1) % FILES] * a + levels[i % FILES] * b;
        }
        near(main[frame * 2], expected, frame);
    }
    g_assert_cmpuint(rendered, >=, total);
    for (guint frame = total; frame < rendered; ++frame) near(main[frame * 2], 0, frame);
    RadioSAudioRunReceipt receipts[COUNT + 2];
    g_assert_cmpuint(radios_audio_engine_runner_receipts(engine, receipts, COUNT + 2), ==, COUNT);
    for (guint i = 0; i < COUNT; ++i) { g_assert_cmpuint(receipts[i].index, ==, i); g_assert_cmpuint(receipts[i].position_frames, ==, OUT); }
    g_string_free(order, TRUE); g_free(main); radios_audio_engine_unref(engine);
}
/* AutoDJ continuation pair (source + target) and Yarı AutoDJ keep loading
 * the playing item and one successor; players outside the pool stay empty. */
static void pair_and_single_unchanged(Fixture *f, gconstpointer unused)
{
    (void)unused;
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; options_pool(&o, 2);
    RadioSAudioRunItem q[6]; queue(f, q, 6, 2, 4000, 800); start(engine, q, 6, &o);
    steady(f, engine, 2, 0, 6);
    gfloat *main = g_new0(gfloat, 40000 * 2); GString *order = g_string_new(NULL);
    run_queue(f, engine, 2, 6, 2, main, 39000, order);
    g_assert_cmpstr(order->str, ==, "1-2-1-2-1-2");
    RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1); radios_audio_engine_snapshot(engine, s);
    for (guint d = 2; d < 4; ++d) g_assert_cmpint(s->decks[d].state, ==, RADIOS_AUDIO_EMPTY);
    radios_audio_engine_unref(engine); g_string_truncate(order, 0);
    /* Yarı AutoDJ: one player, plain ends, reloaded for each item. */
    engine = radios_audio_engine_new(FALSE, NULL); options_pool(&o, 1);
    RadioSAudioRunItem single[3]; queue(f, single, 3, 1, 3000, 0); start(engine, single, 3, &o);
    gint expect[1] = {0}; expect_holding(f, engine, 1, expect);
    run_queue(f, engine, 1, 3, 1, main, 39000, order);
    g_assert_cmpstr(order->str, ==, "1-1-1");
    radios_audio_engine_snapshot(engine, s); g_assert_cmpint(s->decks[1].state, ==, RADIOS_AUDIO_EMPTY);
    radios_audio_engine_unref(engine);
    /* Unplaced items (no preferred player) keep current/next only. */
    engine = radios_audio_engine_new(FALSE, NULL); options_pool(&o, 3);
    RadioSAudioRunItem unplaced[4]; queue(f, unplaced, 4, 3, 4000, 0);
    for (guint i = 0; i < 4; ++i) unplaced[i].preferred_deck = -1;
    start(engine, unplaced, 4, &o);
    gint first[3] = {0, 1, -1}; expect_holding(f, engine, 3, first);
    g_usleep(100000); expect_holding(f, engine, 3, first);
    g_free(s); g_string_free(order, TRUE); g_free(main); radios_audio_engine_unref(engine);
}
/* A preloaded player is silent and unsynced until it becomes next; then it
 * follows the playing player's beat like a player loaded at that moment. */
static void sync_master_on_promotion(Fixture *f, gconstpointer unused)
{
    (void)unused; enum { COUNT = 6, POOL = 4 };
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; options_pool(&o, POOL);
    RadioSAudioRunItem q[COUNT]; queue(f, q, COUNT, POOL, 60000, 0);
    for (guint i = 0; i < COUNT; ++i) {
        q[i].beat_sync = TRUE;
        q[i].rhythm = (RadioSRhythmAnalysis){.bpm = 120, .first_beat_ms = 0, .confidence = .9, .analyzed_ms = 4000};
    }
    start(engine, q, COUNT, &o); steady(f, engine, POOL, 0, COUNT);
    RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1); radios_audio_engine_snapshot(engine, s);
    g_assert_cmpint(s->decks[0].sync_master, ==, -1); g_assert_cmpint(s->decks[1].sync_master, ==, 0);
    g_assert_cmpint(s->decks[2].sync_master, ==, -1); g_assert_cmpint(s->decks[3].sync_master, ==, -1);
    gfloat *main = g_new0(gfloat, 8192 * 2);
    for (guint rendered = 0; rendered < 61000; rendered += 4000) { g_assert_true(settled(f, engine, POOL, NULL)); render(engine, 4000, main); }
    RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(engine, &r);
    g_assert_cmpuint(r.index, ==, 1); g_assert_cmpint(r.deck_current, ==, 1);
    steady(f, engine, POOL, 1, COUNT); radios_audio_engine_snapshot(engine, s);
    /* deck2 was preloaded; promoted to next it syncs to the playing deck1. */
    g_assert_cmpint(s->decks[2].sync_master, ==, 1); g_assert_cmpint(s->decks[3].sync_master, ==, -1);
    g_assert_cmpint(s->decks[0].sync_master, ==, -1); g_assert_cmpint(held(f, &s->decks[0]), ==, 4);
    g_free(main); g_free(s); radios_audio_engine_unref(engine);
}
/* An item whose copyright window closes while it is preloaded is released at
 * once and skipped; the pool keeps its order and the queue never stalls. */
static void copyright_releases_preload(Fixture *f, gconstpointer unused)
{
    (void)unused; enum { COUNT = 8, POOL = 4 };
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; options_pool(&o, POOL);
    RadioSAudioRunItem q[COUNT]; queue(f, q, COUNT, POOL, 3000, 500);
    gint64 now = g_get_real_time();
    q[2].copyright = (RadioSAudioCopyrightWindow){TRUE, FALSE, now - G_USEC_PER_SEC, now + 60 * G_USEC_PER_SEC};
    start(engine, q, COUNT, &o); steady(f, engine, POOL, 0, COUNT);
    RadioSAudioCopyrightWindow windows[COUNT];
    for (guint i = 0; i < COUNT; ++i) windows[i] = q[i].copyright;
    windows[2].ends_at_utc_us = g_get_real_time() - 1;
    g_assert_true(radios_audio_engine_runner_copyright(engine, -1, windows, COUNT, NULL));
    gint released[POOL] = {0, 1, -1, 3}; expect_holding(f, engine, POOL, released);
    gfloat *main = g_new0(gfloat, 40000 * 2); RadioSAudioRunSnapshot r; GString *order = g_string_new(NULL); gint last = -1;
    for (guint rendered = 0; rendered < 39000; rendered += 500) {
        radios_audio_engine_runner_snapshot(engine, &r);
        if (r.state == RADIOS_AUDIO_RUN_FINISHED) break;
        if ((gint)r.index != last && r.deck_current >= 0) { last = (gint)r.index; g_string_append_printf(order, "%s%u", order->len ? "-" : "", r.index); }
        g_assert_true(settled(f, engine, POOL, NULL)); render(engine, 500, main + (gsize)rendered * 2);
    }
    radios_audio_engine_runner_snapshot(engine, &r); g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_FINISHED);
    g_assert_cmpstr(order->str, ==, "0-1-3-4-5-6-7");
    RadioSAudioRunReceipt receipts[COUNT];
    guint count = radios_audio_engine_runner_receipts(engine, receipts, COUNT); g_assert_cmpuint(count, ==, COUNT - 1);
    for (guint i = 0; i < count; ++i) g_assert_cmpuint(receipts[i].index, !=, 2);
    g_string_free(order, TRUE); g_free(main); radios_audio_engine_unref(engine);
}
/* Replacing the future releases preloads of the old items; the new items take
 * their planned players in order. */
static void replace_future_releases_preload(Fixture *f, gconstpointer unused)
{
    (void)unused; enum { COUNT = 6, POOL = 4 };
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; options_pool(&o, POOL);
    RadioSAudioRunItem q[COUNT]; queue(f, q, COUNT, POOL, 30000, 0); start(engine, q, COUNT, &o);
    steady(f, engine, POOL, 0, COUNT);
    gfloat *main = g_new0(gfloat, 8192 * 2); render(engine, 1000, main);
    RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(engine, &r);
    RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1); radios_audio_engine_snapshot(engine, s);
    RadioSAudioRunItem future[3];
    for (guint i = 0; i < 3; ++i) future[i] = item(f, 10 + i, 30000, 0, (gint)((3 - i) % POOL));
    GError *error = NULL;
    g_assert_true(radios_audio_engine_runner_replace_future(engine, -1, r.epoch, s->decks[0].generation, f->entry[0], future, 3, &error));
    g_assert_no_error(error);
    /* future[0] → deck3 (next), future[1] → deck2, future[2] → deck1. */
    gint expect[POOL] = {0, 12, 11, 10}; expect_holding(f, engine, POOL, expect);
    /* The snapshot follows rendered marks: render once to observe the new next. */
    render(engine, 10, main); near(main[0], levels[0], 0);
    radios_audio_engine_runner_snapshot(engine, &r); g_assert_cmpint(r.deck_next, ==, 3); g_assert_cmpuint(r.count, ==, 4);
    g_free(s); g_free(main); radios_audio_engine_unref(engine);
}
/* U7 × U1: a future item removed from the queue (replace_future with the
 * remaining entries, as radios_playout_ui_remove_future_entries does) while
 * four players are preloaded. Old preloads are released, the remaining items
 * reload on their own players, no player keeps the removed item and MAIN
 * continues in the new order without a gap. */
static void remove_future_item_keeps_pool(Fixture *f, gconstpointer api)
{
    /* api: NULL = replace_future, otherwise remove_future (U7 ×, keeps the
     * sounding item's policy and the loaded successor). */
    enum { COUNT = 8, POOL = 4, OUT = 6000, OVERLAP = 1000, REMOVED = 2 };
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; options_pool(&o, POOL);
    RadioSAudioRunItem q[COUNT]; queue(f, q, COUNT, POOL, OUT, OVERLAP); start(engine, q, COUNT, &o);
    steady(f, engine, POOL, 0, COUNT);
    gsize capacity = 60000; gfloat *main = g_new0(gfloat, capacity * 2); guint rendered = 0;
    render(engine, 1000, main); rendered += 1000;
    RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(engine, &r);
    RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1); radios_audio_engine_snapshot(engine, s);
    guint order[COUNT], kept = 0; RadioSAudioRunItem future[COUNT];
    order[kept++] = 0;
    for (guint i = 1; i < COUNT; ++i) if (i != REMOVED) { future[kept - 1] = q[i]; order[kept++] = i; }
    GError *error = NULL; guint64 successor = s->decks[1].generation;
    gboolean accepted = api ? radios_audio_engine_runner_remove_future(engine, -1, r.epoch, s->decks[0].generation, f->entry[0], future, kept - 1, &error) :
        radios_audio_engine_runner_replace_future(engine, -1, r.epoch, s->decks[0].generation, f->entry[0], future, kept - 1, &error);
    g_assert_no_error(error); g_assert_true(accepted);
    /* remove_future keeps the loaded successor (item 1) without reloading it. */
    if (api) { radios_audio_engine_snapshot(engine, s); g_assert_cmpuint(s->decks[1].generation, ==, successor); }
    /* Item 3 keeps its player 4, item 1 its player 2; nothing holds item 2. */
    gint after[POOL] = {0, 1, -1, 3}; expect_holding(f, engine, POOL, after);
    GString *played = g_string_new(NULL); gint last = -1;
    for (;;) {
        radios_audio_engine_runner_snapshot(engine, &r);
        if (r.state == RADIOS_AUDIO_RUN_FINISHED || r.state == RADIOS_AUDIO_RUN_ERROR || rendered + 500 > capacity) break;
        catch_up(f, engine, POOL, order, kept);
        if (r.deck_current >= 0 && (gint)r.index != last) { last = (gint)r.index; g_string_append_printf(played, "%s%u", played->len ? "-" : "", order[r.index]); }
        radios_audio_engine_snapshot(engine, s);
        for (guint d = 0; d < POOL; ++d) g_assert_cmpint(held(f, &s->decks[d]), !=, REMOVED);
        render(engine, 500, main + (gsize)rendered * 2); rendered += 500;
    }
    g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_FINISHED);
    g_assert_cmpstr(played->str, ==, "0-1-3-4-5-6-7");
    const guint step = OUT - OVERLAP, total = kept * OUT - (kept - 1) * OVERLAP;
    RadioSAudioTransition envelope = {OVERLAP, OVERLAP, OVERLAP, 0};
    for (guint frame = 0; frame < total; ++frame) {
        guint i = MIN(frame / step, kept - 1), within = frame - i * step;
        gdouble expected = levels[order[i] % FILES];
        if (within < OVERLAP && i > 0) {
            gdouble a, b; radios_audio_transition_gains(&envelope, within, &a, &b);
            expected = levels[order[i - 1] % FILES] * a + levels[order[i] % FILES] * b;
        }
        near(main[frame * 2], expected, frame);
    }
    for (guint frame = total; frame < rendered; ++frame) near(main[frame * 2], 0, frame);
    RadioSAudioRunReceipt receipts[COUNT];
    guint count = radios_audio_engine_runner_receipts(engine, receipts, COUNT); g_assert_cmpuint(count, ==, kept);
    for (guint i = 0; i < count; ++i) { g_assert_cmpuint(receipts[i].index, ==, i); g_assert_cmpuint(receipts[i].position_frames, ==, OUT); }
    g_string_free(played, TRUE); g_free(s); g_free(main); radios_audio_engine_unref(engine);
}
/* Next/previous cuts reload the pool in order; a seek of the playing player,
 * pause and resume keep the preloads; stop releases every player. */
static void navigation_seek_pause_stop(Fixture *f, gconstpointer unused)
{
    (void)unused; enum { COUNT = 8, POOL = 4 };
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; options_pool(&o, POOL);
    RadioSAudioRunItem q[COUNT]; queue(f, q, COUNT, POOL, 20000, 0); start(engine, q, COUNT, &o);
    steady(f, engine, POOL, 0, COUNT);
    gfloat *main = g_new0(gfloat, 8192 * 2); render(engine, 1000, main);
    RadioSAudioSnapshot *before = g_new0(RadioSAudioSnapshot, 1), *after = g_new0(RadioSAudioSnapshot, 1);
    radios_audio_engine_snapshot(engine, before);
    GError *error = NULL;
    g_assert_true(radios_audio_engine_seek(engine, 0, 5000, &error)); g_assert_no_error(error);
    /* Seeking a preloaded player is refused like seeking next; nothing is cancelled. */
    g_assert_false(radios_audio_engine_seek(engine, 2, 100, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    steady(f, engine, POOL, 0, COUNT); radios_audio_engine_snapshot(engine, after);
    for (guint d = 1; d < POOL; ++d) g_assert_cmpuint(after->decks[d].generation, ==, before->decks[d].generation);
    render(engine, 100, main); near(main[0], levels[0], 0);
    g_assert_true(radios_audio_engine_runner_pause(engine, NULL)); g_usleep(50000);
    steady(f, engine, POOL, 0, COUNT);
    g_assert_true(radios_audio_engine_runner_resume(engine, NULL)); render(engine, 100, main); near(main[0], levels[0], 0);
    radios_audio_engine_snapshot(engine, after);
    for (guint d = 1; d < POOL; ++d) g_assert_cmpuint(after->decks[d].generation, ==, before->decks[d].generation);
    g_assert_true(radios_audio_engine_runner_next(engine, NULL)); render(engine, 241, main);
    steady(f, engine, POOL, 1, COUNT); render(engine, 10, main); near(main[0], levels[1], 0);
    g_assert_true(radios_audio_engine_runner_next(engine, NULL)); render(engine, 241, main);
    steady(f, engine, POOL, 2, COUNT); render(engine, 10, main); near(main[0], levels[2], 0);
    g_assert_true(radios_audio_engine_runner_previous(engine, NULL)); render(engine, 241, main);
    steady(f, engine, POOL, 1, COUNT); render(engine, 10, main); near(main[0], levels[1], 0);
    RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(engine, &r); g_assert_cmpint(r.deck_current, ==, 1);
    g_assert_true(radios_audio_engine_runner_stop(engine, NULL));
    radios_audio_engine_runner_snapshot(engine, &r); radios_audio_engine_snapshot(engine, after);
    for (guint d = 0; d < POOL; ++d) { g_assert_false(r.owned[d]); g_assert_cmpint(after->decks[d].state, ==, RADIOS_AUDIO_EMPTY); }
    g_free(before); g_free(after); g_free(main); radios_audio_engine_unref(engine);
}
/* Music on players 1-2, a sweeper item on player 3 and a midtrack jingle
 * layer on player 4 (outside the pool): the sweeper is preloaded, the jingle
 * still sounds over its song and the pool order is kept. */
static void jingle_and_sweeper_layers(Fixture *f, gconstpointer unused)
{
    (void)unused; enum { COUNT = 5, POOL = 3, OUT = 4000 };
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; options_pool(&o, POOL);
    RadioSAudioRunItem q[COUNT]; static const gint placed[COUNT] = {0, 1, 2, 0, 1};
    for (guint i = 0; i < COUNT; ++i) q[i] = item(f, i, OUT, 0, placed[i]);
    q[1].adjunct = (RadioSAudioRunAdjunct){.enabled = TRUE, .player = 3,
        .media = {f->path[3], f->sha[3], f->media[3], f->version[3], 0, 1000},
        .entry_id = f->entry[15], .title = "Doğrulanmış jingle", .artist = "Anons",
        .start_output_frame = 1000, .fade_frames = 0, .gain_db = 0, .duck_db = 0};
    start(engine, q, COUNT, &o);
    gint first[POOL] = {0, 1, 2}; expect_holding(f, engine, POOL, first);
    gfloat *main = g_new0(gfloat, 30000 * 2); guint rendered = 0; static const guint order[COUNT] = {0, 1, 2, 3, 4};
    while (rendered < 2 * OUT + 500) {
        gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1);
        do { radios_audio_engine_snapshot(engine, s); if (s->decks[3].state != RADIOS_AUDIO_LOADING) break; g_usleep(2000); } while (g_get_monotonic_time() < until);
        g_free(s); catch_up(f, engine, POOL, order, COUNT); render(engine, 500, main + (gsize)rendered * 2); rendered += 500;
    }
    /* Song 1 alone, then song 2 with the jingle from its output frame 1000. */
    near(main[500 * 2], levels[0], 500);
    near(main[(OUT + 500) * 2], levels[1], OUT + 500);
    near(main[(OUT + 1500) * 2], levels[1] + levels[3], OUT + 1500);
    near(main[(OUT + 2500) * 2], levels[1], OUT + 2500);
    RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(engine, &r); g_assert_cmpuint(r.index, ==, 2); g_assert_cmpint(r.deck_current, ==, 2);
    gint later[POOL] = {3, 4, 2}; expect_holding(f, engine, POOL, later);
    near(main[(2 * OUT - 1) * 2], levels[1], 2 * OUT - 1);
    near(main[(2 * OUT) * 2], levels[2], 2 * OUT);
    g_free(main); radios_audio_engine_unref(engine);
}
/* Real output: four players on owned virtual MAIN/CUE/monitor sinks. Every
 * observation sees all four players holding an item until the tail; MAIN
 * plays 1-2-3-4-1-… without a gap. */
static goffset capture_size(const gchar *path)
{ GStatBuf info; return path && g_stat(path, &info) == 0 ? (goffset)info.st_size : 0; }
/* removal != NULL: while song 2 plays, × removes songs 5-8 (one whole pool
 * rotation) through remove_future; the loaded successor stays, MAIN is not
 * flushed and the programme goes on 1-2-3-4-1 without a gap. */
static void four_player_cluster_realtime(Fixture *f, gconstpointer removal)
{
    enum { COUNT = 9, POOL = 4, OUT = 48000, OVERLAP = 12000 };
    const gchar *main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"), *capture = g_getenv("RADIOS_TEST_MAIN_CAPTURE");
    if (!main_sink || !capture) { g_test_skip("Owned virtual Pulse sinks and captures require tests/run_audio_ui.py --engine-only"); return; }
    RadioSAudioEngine *engine = radios_audio_engine_new(TRUE, NULL); RadioSAudioPreferences prefs;
    radios_audio_preferences_default(&prefs); prefs.loudness_normalize = FALSE;
    g_strlcpy(prefs.main_sink, main_sink, sizeof prefs.main_sink);
    g_strlcpy(prefs.cue_sink, g_getenv("RADIOS_TEST_CUE_SINK"), sizeof prefs.cue_sink);
    g_strlcpy(prefs.monitor_sink, g_getenv("RADIOS_TEST_MONITOR_SINK"), sizeof prefs.monitor_sink);
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1); gint64 until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    do { radios_audio_engine_snapshot(engine, s); if (s->buses[0].ready && s->buses[1].ready && s->buses[2].ready) break; g_usleep(10000); } while (g_get_monotonic_time() < until);
    g_assert_true(s->buses[0].ready);
    RadioSAudioRunOptions o; options_pool(&o, POOL); RadioSAudioRunItem q[COUNT]; queue(f, q, COUNT, POOL, OUT, OVERLAP);
    goffset offset = capture_size(capture); start(engine, q, COUNT, &o);
    GString *order = g_string_new(NULL); gint last = -1; guint observations = 0, full = 0, count = COUNT; RadioSAudioRunSnapshot r;
    gboolean removed = FALSE; gint64 reload_until = 0;
    until = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
    do {
        radios_audio_engine_runner_snapshot(engine, &r); radios_audio_engine_snapshot(engine, s);
        if (r.state == RADIOS_AUDIO_RUN_ERROR) g_error("Runner: %s", r.error);
        if (r.state == RADIOS_AUDIO_RUN_FINISHED) break;
        if (r.deck_current >= 0 && (gint)r.index != last) { last = (gint)r.index; g_string_append_printf(order, "%s%d", order->len ? "-" : "", r.deck_current + 1); }
        if (removal && !removed && r.index == 1 && r.state == RADIOS_AUDIO_RUN_PLAYING) {
            RadioSAudioRunItem future[] = {q[2], q[3], q[8]}; GError *error = NULL;
            guint64 flushes = radios_audio_engine_output_flushes(engine), successor = s->decks[2].generation;
            if (radios_audio_engine_runner_remove_future(engine, -1, r.epoch, s->decks[1].generation, f->entry[1], future, 3, &error)) {
                removed = TRUE; count = 5; reload_until = g_get_monotonic_time() + 200000;
                g_assert_cmpuint(radios_audio_engine_output_flushes(engine), ==, flushes);
                radios_audio_engine_snapshot(engine, s); g_assert_cmpuint(s->decks[2].generation, ==, successor);
            } else { g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error(&error); }
        }
        if (r.state == RADIOS_AUDIO_RUN_PLAYING || r.state == RADIOS_AUDIO_RUN_OVERLAP) {
            guint holding = 0;
            for (guint d = 0; d < POOL; ++d) holding += s->decks[d].state != RADIOS_AUDIO_EMPTY;
            ++observations;
            /* Until the queue tail every selected player holds an item (a
             * removal releases the moved preloads for one or two turns). */
            if (r.index + POOL <= count && g_get_monotonic_time() >= reload_until) {
                if (holding != POOL) { gchar *state = describe(f, engine, POOL); g_error("Only %u/%u players hold an item: %s", holding, POOL, state); }
                ++full;
            }
        }
        g_usleep(5000);
    } while (g_get_monotonic_time() < until);
    g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_FINISHED);
    g_test_message("Realtime pool: order %s, %u observations, %u with 4/4 players loaded", order->str, observations, full);
    g_assert_cmpstr(order->str, ==, removal ? "1-2-3-4-1" : "1-2-3-4-1-2-3-4-1");
    g_assert_true(!removal || removed);
    g_assert_cmpuint(full, >, 100);
    g_usleep(400000);
    gchar *bytes = NULL; gsize length = 0; g_assert_true(g_file_get_contents(capture, &bytes, &length, NULL));
    offset -= offset % 8; const gfloat *pcm = (const gfloat *)(bytes + offset); guint frames = (guint)((length - (gsize)offset) / 8);
    guint first = 0; while (first < frames && fabsf(pcm[first * 2]) < .01f) ++first;
    guint end = frames; while (end > first && fabsf(pcm[(end - 1) * 2]) < .01f) --end;
    guint silence = 0, run = 0; GArray *plateaus = g_array_new(FALSE, FALSE, sizeof(gint));
    gint level = -1; guint stable = 0;
    for (guint i = first; i < end; ++i) {
        gfloat value = fabsf(pcm[i * 2]);
        if (value < .01f) silence = MAX(silence, ++run); else run = 0;
        gint match = -1;
        for (guint l = 0; l < FILES; ++l) if (fabsf(value - levels[l]) < .01f) match = (gint)l;
        if (match >= 0 && match == level) {
            if (++stable == 4800) g_array_append_val(plateaus, match);
        } else { level = match; stable = 1; }
    }
    GString *sequence = g_string_new(NULL);
    for (guint i = 0; i < plateaus->len; ++i) g_string_append_printf(sequence, "%s%d", i ? "-" : "", g_array_index(plateaus, gint, i) + 1);
    guint expected = count * OUT - (count - 1) * OVERLAP;
    g_test_message("MAIN capture: %u programme frames (expected %u), longest silence %u frames, levels %s",
        end - first, expected, silence, sequence->str);
    g_assert_cmpuint(silence, <=, 480);
    g_assert_cmpstr(sequence->str, ==, removal ? "1-2-3-4-1" : "1-2-3-4-1-2-3-4-1");
    g_assert_cmpuint(end - first, >=, expected - 4800); g_assert_cmpuint(end - first, <=, expected + 4800);
    g_array_unref(plateaus); g_string_free(sequence, TRUE); g_free(bytes); g_string_free(order, TRUE); g_free(s);
    radios_audio_engine_unref(engine);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/runner-pool-preload/four-player-cluster-all-loaded-PCM", Fixture, NULL, setup, four_player_cluster_pcm, teardown);
    g_test_add("/runner-pool-preload/continuation-pair-and-single-player-unchanged", Fixture, NULL, setup, pair_and_single_unchanged, teardown);
    g_test_add("/runner-pool-preload/sync-master-on-promotion", Fixture, NULL, setup, sync_master_on_promotion, teardown);
    g_test_add("/runner-pool-preload/copyright-releases-preload", Fixture, NULL, setup, copyright_releases_preload, teardown);
    g_test_add("/runner-pool-preload/replace-future-releases-preload", Fixture, NULL, setup, replace_future_releases_preload, teardown);
    g_test_add("/runner-pool-preload/remove-future-item-keeps-pool-gapless-PCM", Fixture, NULL, setup, remove_future_item_keeps_pool, teardown);
    g_test_add("/runner-pool-preload/remove-future-middle-entry-keeps-successor-gapless-PCM", Fixture, GUINT_TO_POINTER(1), setup, remove_future_item_keeps_pool, teardown);
    g_test_add("/runner-pool-preload/next-previous-seek-pause-stop", Fixture, NULL, setup, navigation_seek_pause_stop, teardown);
    g_test_add("/runner-pool-preload/jingle-and-sweeper-layers", Fixture, NULL, setup, jingle_and_sweeper_layers, teardown);
    g_test_add("/runner-pool-preload/four-player-cluster-realtime-PCM", Fixture, NULL, setup, four_player_cluster_realtime, teardown);
    g_test_add("/runner-pool-preload/remove-future-realtime-no-flush-PCM", Fixture, GUINT_TO_POINTER(1), setup, four_player_cluster_realtime, teardown);
    return g_test_run();
}
