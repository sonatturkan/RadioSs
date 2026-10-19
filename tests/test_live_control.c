#include "radios/live_control.h"
#include <string.h>

/* Kılavuz §06–§07 L/R control logic without Pulse: the engine binding is a
 * fake, the window is a plain GObject (only its data table is used). */
typedef struct {
    gboolean latch, ptt, on_air[33];
    const gchar *label[33];
    gboolean fail_latch, fail_ptt, fail_channel;
    guint latch_calls, ptt_calls, channel_calls;
} Fake;
static Fake fake;
static gboolean fake_latch(RadioSViewContext *ctx, gboolean on, GError **error)
{
    (void)ctx; ++fake.latch_calls;
    if (fake.fail_latch) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "latch failed"); return FALSE; }
    fake.latch = on; return TRUE;
}
static gboolean fake_ptt(RadioSViewContext *ctx, gboolean held, GError **error)
{
    (void)ctx; ++fake.ptt_calls;
    if (held && fake.fail_ptt) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ptt failed"); return FALSE; }
    fake.ptt = held; return TRUE;
}
static gboolean fake_channel(RadioSViewContext *ctx, guint channel, gboolean on_air, GError **error)
{
    (void)ctx; ++fake.channel_calls; g_assert_cmpuint(channel, >=, 1); g_assert_cmpuint(channel, <=, 32);
    if (fake.fail_channel) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "channel failed"); return FALSE; }
    fake.on_air[channel] = on_air; return TRUE;
}
static gboolean fake_channel_on_air(RadioSViewContext *ctx, guint channel) { (void)ctx; return fake.on_air[channel]; }
static gboolean fake_latch_active(RadioSViewContext *ctx) { (void)ctx; return fake.latch; }
static const gchar *fake_channel_label(RadioSViewContext *ctx, guint channel) { (void)ctx; return channel <= 32 ? fake.label[channel] : NULL; }
static const RadioSLiveControlBackend fake_backend = {fake_latch, fake_ptt, fake_channel, fake_channel_on_air, fake_latch_active, fake_channel_label};

static JsonObject *block_new(const gchar *id, const gchar *label, const gint *channels, guint count)
{
    JsonObject *block = json_object_new();
    if (id) json_object_set_string_member(block, "id", id);
    json_object_set_string_member(block, "kind", "music");
    if (label) {
        JsonObject *live = json_object_new(); json_object_set_string_member(live, "label", label);
        json_object_set_string_member(live, "automation_id", ""); json_object_set_string_member(live, "end_rule", "manual");
        if (channels) {
            JsonArray *list = json_array_new();
            for (guint i = 0; i < count; ++i) json_array_add_int_element(list, channels[i]);
            json_object_set_array_member(live, "channels", list);
        }
        json_object_set_object_member(block, "live", live);
    }
    return block;
}
static JsonObject *block_text(const gchar *text)
{
    JsonParser *parser = json_parser_new(); GError *error = NULL;
    g_assert_true(json_parser_load_from_data(parser, text, -1, &error)); g_assert_no_error(error);
    JsonObject *block = json_object_ref(json_node_get_object(json_parser_get_root(parser)));
    g_object_unref(parser); return block;
}
typedef struct { RadioSViewContext ctx; GObject *parent; gint engine; } Harness;
static void harness_init(Harness *h, gboolean audio)
{
    memset(&fake, 0, sizeof fake); memset(h, 0, sizeof *h);
    h->parent = g_object_new(G_TYPE_OBJECT, NULL);
    h->ctx.parent = (GtkWindow *)h->parent; /* only its GObject data is used */
    if (audio) h->ctx.audio = (RadioSAudioController *)&h->engine; /* opaque; the fake never dereferences it */
    radios_live_control_set_backend(&fake_backend);
}
static void harness_clear(Harness *h) { radios_live_control_set_backend(NULL); g_object_unref(h->parent); }
static GHashTable *table(Harness *h) { return g_object_get_data(h->parent, "radios-live-control-states"); }
static void assert_state(RadioSViewContext *ctx, JsonObject *block, gboolean active, gboolean held)
{
    gboolean a = !active, t = !held;
    g_assert_true(radios_live_control_state(ctx, block, &a, &t));
    g_assert_true(a == active); g_assert_true(t == held);
}
static void assert_error(gboolean result, GError **error, gint code)
{ g_assert_false(result); g_assert_error(*error, G_IO_ERROR, code); g_clear_error(error); }

static void test_label_and_available(void)
{
    const gint one[] = {1};
    JsonObject *none = block_new("b", NULL, NULL, 0), *dj = block_new("b", "dj", one, 1), *talk = block_new("b", "talk", NULL, 0);
    JsonObject *music = block_new("b", "music", NULL, 0), *other = block_new("b", "voice", NULL, 0);
    JsonObject *odd = block_text("{\"id\":\"b\",\"live\":{\"label\":7}}"), *flat = block_text("{\"id\":\"b\",\"live\":\"dj\"}");
    g_assert_cmpint(radios_live_control_label(NULL), ==, RADIOS_LIVE_LABEL_NONE);
    g_assert_cmpint(radios_live_control_label(none), ==, RADIOS_LIVE_LABEL_NONE); g_assert_false(radios_live_control_available(none));
    g_assert_cmpint(radios_live_control_label(dj), ==, RADIOS_LIVE_LABEL_DJ); g_assert_true(radios_live_control_available(dj));
    g_assert_cmpint(radios_live_control_label(talk), ==, RADIOS_LIVE_LABEL_TALK); g_assert_true(radios_live_control_available(talk));
    g_assert_cmpint(radios_live_control_label(music), ==, RADIOS_LIVE_LABEL_MUSIC); g_assert_true(radios_live_control_available(music));
    g_assert_cmpint(radios_live_control_label(other), ==, RADIOS_LIVE_LABEL_NONE); g_assert_false(radios_live_control_available(other));
    g_assert_cmpint(radios_live_control_label(odd), ==, RADIOS_LIVE_LABEL_NONE);
    g_assert_cmpint(radios_live_control_label(flat), ==, RADIOS_LIVE_LABEL_NONE); g_assert_false(radios_live_control_available(flat));
    g_assert_false(radios_live_control_state(NULL, none, NULL, NULL));
    g_assert_true(radios_live_control_state(NULL, dj, NULL, NULL));
    json_object_unref(none); json_object_unref(dj); json_object_unref(talk); json_object_unref(music); json_object_unref(other);
    json_object_unref(odd); json_object_unref(flat);
}
/* Kılavuz s.4 adım 5 / s.7: the bound strips' label decides what L does. */
static void test_effective_label_follows_strips(void)
{
    Harness h; harness_init(&h, TRUE);
    const gint pair[] = {3, 4}, mixed[] = {3, 6}, unlabelled[] = {1, 2};
    JsonObject *talk = block_new("t", "talk", pair, 2), *music = block_new("m", "music", pair, 2);
    JsonObject *split = block_new("s", "talk", mixed, 2), *plain = block_new("p", "dj", unlabelled, 2), *none = block_new("n", NULL, pair, 2);
    fake.label[3] = fake.label[4] = "dj"; fake.label[6] = "music";
    g_assert_cmpint(radios_live_control_effective_label(&h.ctx, talk), ==, RADIOS_LIVE_LABEL_DJ);
    g_assert_cmpint(radios_live_control_effective_label(&h.ctx, music), ==, RADIOS_LIVE_LABEL_DJ);
    g_assert_cmpint(radios_live_control_effective_label(&h.ctx, split), ==, RADIOS_LIVE_LABEL_TALK); /* mixed: own label */
    g_assert_cmpint(radios_live_control_effective_label(&h.ctx, plain), ==, RADIOS_LIVE_LABEL_DJ);   /* unlabelled: own label */
    g_assert_cmpint(radios_live_control_effective_label(&h.ctx, none), ==, RADIOS_LIVE_LABEL_NONE);  /* no binding */
    /* A Müzik label on the strips makes L the schema control: R/L refuse the
     * microphone path for that block. */
    fake.label[3] = fake.label[4] = "music"; GError *error = NULL;
    g_assert_cmpint(radios_live_control_effective_label(&h.ctx, talk), ==, RADIOS_LIVE_LABEL_MUSIC);
    g_assert_false(radios_live_control_toggle(&h.ctx, NULL, talk, TRUE, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED); g_clear_error(&error);
    g_assert_false(fake.latch); g_assert_cmpuint(fake.latch_calls, ==, 0);
    /* A strip relabelled DJ again: the same block opens the microphone. */
    fake.label[3] = fake.label[4] = "dj";
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, talk, TRUE, &error)); g_assert_no_error(error); g_assert_true(fake.latch);
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, talk, FALSE, &error)); g_assert_no_error(error);
    json_object_unref(talk); json_object_unref(music); json_object_unref(split); json_object_unref(plain); json_object_unref(none);
    harness_clear(&h);
}
static void test_channels(void)
{
    const gint ordered[] = {3, 1, 32}, twice[] = {1, 1}, zero[] = {0}, high[] = {33};
    guint channels[32]; gint many[33]; for (gint i = 0; i < 33; ++i) many[i] = i + 1;
    JsonObject *block = block_new("b", "dj", NULL, 0);
    g_assert_cmpuint(radios_live_control_channels(block, channels), ==, 0); g_assert_true(radios_live_control_available(block)); json_object_unref(block);
    block = block_new("b", "dj", ordered, 3);
    g_assert_cmpuint(radios_live_control_channels(block, channels), ==, 3);
    g_assert_cmpuint(channels[0], ==, 3); g_assert_cmpuint(channels[1], ==, 1); g_assert_cmpuint(channels[2], ==, 32);
    g_assert_cmpuint(radios_live_control_channels(block, NULL), ==, 3); g_assert_true(radios_live_control_available(block)); json_object_unref(block);
    block = block_new("b", "dj", twice, 2);
    g_assert_cmpuint(radios_live_control_channels(block, channels), ==, 0); g_assert_false(radios_live_control_available(block)); json_object_unref(block);
    block = block_new("b", "dj", zero, 1); g_assert_false(radios_live_control_available(block)); json_object_unref(block);
    block = block_new("b", "dj", high, 1); g_assert_false(radios_live_control_available(block)); json_object_unref(block);
    block = block_new("b", "dj", many, 33); g_assert_false(radios_live_control_available(block)); g_assert_cmpuint(radios_live_control_channels(block, channels), ==, 0); json_object_unref(block);
    block = block_text("{\"id\":\"b\",\"live\":{\"label\":\"talk\",\"channels\":[\"3\"]}}"); g_assert_false(radios_live_control_available(block)); json_object_unref(block);
    block = block_text("{\"id\":\"b\",\"live\":{\"label\":\"talk\",\"channels\":[2.5]}}"); g_assert_false(radios_live_control_available(block)); json_object_unref(block);
    block = block_text("{\"id\":\"b\",\"live\":{\"label\":\"talk\",\"channels\":7}}"); g_assert_false(radios_live_control_available(block)); json_object_unref(block);
    block = block_text("{\"id\":\"b\",\"live\":{\"label\":\"talk\",\"channels\":null}}");
    g_assert_true(radios_live_control_available(block)); g_assert_cmpuint(radios_live_control_channels(block, channels), ==, 0); json_object_unref(block);
    block = block_text("{\"id\":\"b\",\"live\":{\"label\":\"talk\",\"channels\":[8,16]}}");
    g_assert_cmpuint(radios_live_control_channels(block, channels), ==, 2); g_assert_cmpuint(channels[1], ==, 16); json_object_unref(block);
}
static void test_without_engine(void)
{
    Harness h; harness_init(&h, FALSE); GError *error = NULL; const gint two[] = {2};
    JsonObject *dj = block_new("b1", "dj", two, 1), *talk = block_new("b2", "talk", NULL, 0);
    JsonObject *music = block_new("b3", "music", NULL, 0), *none = block_new("b4", NULL, NULL, 0);
    assert_error(radios_live_control_toggle(&h.ctx, NULL, dj, TRUE, &error), &error, G_IO_ERROR_NOT_CONNECTED);
    assert_error(radios_live_control_toggle(&h.ctx, NULL, talk, TRUE, &error), &error, G_IO_ERROR_NOT_CONNECTED);
    assert_error(radios_live_control_talk(&h.ctx, dj, TRUE, &error), &error, G_IO_ERROR_NOT_CONNECTED);
    assert_error(radios_live_control_talk(&h.ctx, talk, FALSE, &error), &error, G_IO_ERROR_NOT_CONNECTED);
    assert_state(&h.ctx, dj, FALSE, FALSE); assert_state(&h.ctx, talk, FALSE, FALSE);
    g_assert_null(table(&h)); g_assert_cmpuint(fake.latch_calls + fake.ptt_calls + fake.channel_calls, ==, 0);
    /* The Müzik label is the caller's automation, with or without an engine. */
    assert_error(radios_live_control_toggle(&h.ctx, NULL, music, TRUE, &error), &error, G_IO_ERROR_NOT_SUPPORTED);
    assert_error(radios_live_control_talk(&h.ctx, music, TRUE, &error), &error, G_IO_ERROR_NOT_SUPPORTED);
    assert_error(radios_live_control_toggle(&h.ctx, NULL, none, TRUE, &error), &error, G_IO_ERROR_NOT_FOUND);
    assert_error(radios_live_control_talk(&h.ctx, none, TRUE, &error), &error, G_IO_ERROR_NOT_FOUND);
    g_assert_false(radios_live_control_state(&h.ctx, none, NULL, NULL));
    assert_error(radios_live_control_toggle(NULL, NULL, dj, TRUE, &error), &error, G_IO_ERROR_NOT_CONNECTED);
    RadioSViewContext bare = {0};
    assert_error(radios_live_control_talk(&bare, dj, TRUE, &error), &error, G_IO_ERROR_NOT_CONNECTED);
    harness_clear(&h);
    harness_init(&h, TRUE);
    JsonObject *anonymous = block_new(NULL, "dj", two, 1);
    assert_error(radios_live_control_toggle(&h.ctx, NULL, anonymous, TRUE, &error), &error, G_IO_ERROR_INVALID_DATA);
    g_assert_null(table(&h)); g_assert_cmpuint(fake.channel_calls, ==, 0);
    /* An engine without a window has nowhere to keep the state. */
    h.ctx.parent = NULL;
    assert_error(radios_live_control_toggle(&h.ctx, NULL, dj, TRUE, &error), &error, G_IO_ERROR_NOT_CONNECTED);
    h.ctx.parent = (GtkWindow *)h.parent;
    json_object_unref(anonymous); json_object_unref(dj); json_object_unref(talk); json_object_unref(music); json_object_unref(none);
    harness_clear(&h);
}
static void test_latch_opens_and_restores(void)
{
    Harness h; harness_init(&h, TRUE); GError *error = NULL; const gint pair[] = {2, 3};
    JsonObject *dj = block_new("b1", "dj", pair, 2);
    fake.on_air[3] = TRUE; /* another source already opened strip 3 */
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, dj, TRUE, &error)); g_assert_no_error(error);
    g_assert_true(fake.on_air[2]); g_assert_true(fake.on_air[3]); g_assert_true(fake.latch);
    g_assert_cmpuint(fake.channel_calls, ==, 1); g_assert_cmpuint(fake.latch_calls, ==, 1);
    assert_state(&h.ctx, dj, TRUE, FALSE); g_assert_nonnull(table(&h)); g_assert_cmpuint(g_hash_table_size(table(&h)), ==, 1);
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, dj, TRUE, &error)); g_assert_cmpuint(fake.latch_calls, ==, 1);
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, dj, FALSE, &error)); g_assert_no_error(error);
    g_assert_false(fake.on_air[2]); g_assert_true(fake.on_air[3]); g_assert_false(fake.latch);
    assert_state(&h.ctx, dj, FALSE, FALSE); g_assert_cmpuint(g_hash_table_size(table(&h)), ==, 0);
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, dj, FALSE, &error)); g_assert_cmpuint(fake.channel_calls, ==, 2);
    /* A latch the operator already holds is not taken over. */
    fake.latch = TRUE; fake.latch_calls = 0;
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, dj, TRUE, &error)); g_assert_cmpuint(fake.latch_calls, ==, 0);
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, dj, FALSE, &error)); g_assert_true(fake.latch); g_assert_cmpuint(fake.latch_calls, ==, 0);
    json_object_unref(dj); harness_clear(&h);
}
static void test_talk_press_release(void)
{
    Harness h; harness_init(&h, TRUE); GError *error = NULL; const gint four[] = {4};
    JsonObject *talk = block_new("b1", "talk", four, 1);
    g_assert_true(radios_live_control_talk(&h.ctx, talk, TRUE, &error)); g_assert_no_error(error);
    g_assert_true(fake.ptt); g_assert_true(fake.on_air[4]); g_assert_false(fake.latch); assert_state(&h.ctx, talk, FALSE, TRUE);
    g_assert_true(radios_live_control_talk(&h.ctx, talk, TRUE, &error)); g_assert_cmpuint(fake.ptt_calls, ==, 1);
    g_assert_true(radios_live_control_talk(&h.ctx, talk, FALSE, &error)); g_assert_no_error(error);
    g_assert_false(fake.ptt); g_assert_false(fake.on_air[4]); assert_state(&h.ctx, talk, FALSE, FALSE);
    g_assert_cmpuint(fake.ptt_calls, ==, 2); g_assert_cmpuint(g_hash_table_size(table(&h)), ==, 0);
    g_assert_true(radios_live_control_talk(&h.ctx, talk, FALSE, &error)); g_assert_cmpuint(fake.ptt_calls, ==, 2);
    /* R over an open L: release ends only the momentary talk. */
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, talk, TRUE, &error)); guint calls = fake.channel_calls;
    g_assert_true(radios_live_control_talk(&h.ctx, talk, TRUE, &error)); g_assert_cmpuint(fake.channel_calls, ==, calls);
    assert_state(&h.ctx, talk, TRUE, TRUE);
    g_assert_true(radios_live_control_talk(&h.ctx, talk, FALSE, &error));
    g_assert_false(fake.ptt); g_assert_true(fake.latch); g_assert_true(fake.on_air[4]); assert_state(&h.ctx, talk, TRUE, FALSE);
    /* L off while R is held: the strip stays with R until release. */
    g_assert_true(radios_live_control_talk(&h.ctx, talk, TRUE, &error));
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, talk, FALSE, &error));
    g_assert_false(fake.latch); g_assert_true(fake.ptt); g_assert_true(fake.on_air[4]); assert_state(&h.ctx, talk, FALSE, TRUE);
    g_assert_true(radios_live_control_talk(&h.ctx, talk, FALSE, &error));
    g_assert_false(fake.ptt); g_assert_false(fake.on_air[4]); assert_state(&h.ctx, talk, FALSE, FALSE);
    g_assert_cmpuint(g_hash_table_size(table(&h)), ==, 0);
    json_object_unref(talk); harness_clear(&h);
}
static void test_failure_rollback(void)
{
    Harness h; harness_init(&h, TRUE); GError *error = NULL; const gint five[] = {5};
    JsonObject *dj = block_new("b1", "dj", five, 1);
    fake.fail_latch = TRUE;
    assert_error(radios_live_control_toggle(&h.ctx, NULL, dj, TRUE, &error), &error, G_IO_ERROR_FAILED);
    g_assert_false(fake.on_air[5]); g_assert_false(fake.latch); assert_state(&h.ctx, dj, FALSE, FALSE); g_assert_cmpuint(g_hash_table_size(table(&h)), ==, 0);
    fake.fail_latch = FALSE; fake.fail_ptt = TRUE;
    assert_error(radios_live_control_talk(&h.ctx, dj, TRUE, &error), &error, G_IO_ERROR_FAILED);
    g_assert_false(fake.on_air[5]); g_assert_false(fake.ptt); assert_state(&h.ctx, dj, FALSE, FALSE);
    fake.fail_ptt = FALSE;
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, dj, TRUE, &error)); g_assert_true(fake.on_air[5]);
    fake.fail_channel = TRUE;
    /* A strip that cannot be muted keeps the block latched for a retry; the
     * latch itself (its own step) is already off. */
    assert_error(radios_live_control_toggle(&h.ctx, NULL, dj, FALSE, &error), &error, G_IO_ERROR_PENDING);
    assert_state(&h.ctx, dj, TRUE, FALSE); g_assert_true(fake.on_air[5]); g_assert_false(fake.latch);
    fake.fail_channel = FALSE;
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, dj, FALSE, &error)); g_assert_no_error(error);
    g_assert_false(fake.on_air[5]); g_assert_false(fake.latch); assert_state(&h.ctx, dj, FALSE, FALSE);
    json_object_unref(dj); harness_clear(&h);
}
static void test_two_blocks_share_strip(void)
{
    Harness h; harness_init(&h, TRUE); GError *error = NULL; const gint six[] = {6}, six_seven[] = {6, 7};
    JsonObject *a = block_new("a", "dj", six, 1), *b = block_new("b", "talk", six_seven, 2);
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, a, TRUE, &error));
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, b, TRUE, &error));
    g_assert_true(fake.on_air[6]); g_assert_true(fake.on_air[7]); g_assert_true(fake.latch); g_assert_cmpuint(fake.latch_calls, ==, 1);
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, a, FALSE, &error));
    g_assert_true(fake.on_air[6]); g_assert_true(fake.latch); assert_state(&h.ctx, a, FALSE, FALSE); assert_state(&h.ctx, b, TRUE, FALSE);
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, b, FALSE, &error));
    g_assert_false(fake.on_air[6]); g_assert_false(fake.on_air[7]); g_assert_false(fake.latch);
    g_assert_cmpuint(g_hash_table_size(table(&h)), ==, 0);
    /* The table belongs to the window and goes with it. */
    g_assert_true(radios_live_control_toggle(&h.ctx, NULL, a, TRUE, &error)); g_assert_nonnull(table(&h));
    json_object_unref(a); json_object_unref(b); harness_clear(&h);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/live-control/label-available", test_label_and_available);
    g_test_add_func("/live-control/channels-validation", test_channels);
    g_test_add_func("/live-control/effective-label-follows-strips", test_effective_label_follows_strips);
    g_test_add_func("/live-control/without-engine-not-connected", test_without_engine);
    g_test_add_func("/live-control/latch-opens-restores-own", test_latch_opens_and_restores);
    g_test_add_func("/live-control/talk-press-release-no-transport", test_talk_press_release);
    g_test_add_func("/live-control/failure-rollback", test_failure_rollback);
    g_test_add_func("/live-control/two-blocks-share-strip-window-table", test_two_blocks_share_strip);
    return g_test_run();
}
