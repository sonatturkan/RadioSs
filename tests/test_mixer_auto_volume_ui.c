#define _POSIX_C_SOURCE 200809L
#include <glib.h>
#include "radios/loudness.h"

static int mixer_auto_volume_run(void);
#define main mixer_auto_volume_fixture_main
#define g_test_run mixer_auto_volume_run
#include "test_audio_ui.c"
#undef g_test_run
#undef main

typedef struct { Window *window; gdouble gain; gboolean active, enabled; } AutoVolumeExpected;
static gboolean auto_volume_matches(gpointer value)
{
    AutoVolumeExpected *expected = value; RadioSAudioMixerConfig mixer; RadioSAudioDspMeters meters;
    if (!radios_audio_ui_get_mixer(expected->window->ctx, &mixer, &meters, NULL)) return FALSE;
    GtkWidget *fader = find(expected->window->root, "dj-channel-1-gain"), *toggle = find(expected->window->root, "dj-channel-1-normalize");
    GtkWidget *label = find(expected->window->root, "dj-channel-1-normalization");
    if (!GTK_IS_TOGGLE_BUTTON(toggle) || !GTK_IS_LABEL(label)) return FALSE;
    GtkStyleContext *style = gtk_widget_get_style_context(toggle);
    gboolean appearance = expected->active ? gtk_style_context_has_class(style, "is-applying") :
        expected->enabled ? gtk_style_context_has_class(style, "is-suspended") &&
            !gtk_style_context_has_class(style, "is-applying") &&
            !g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), "Genel kapalı") :
        !g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), "Kapalı");
    return appearance && GTK_IS_RANGE(fader) && GTK_IS_TOGGLE_BUTTON(toggle) &&
        fabs(mixer.channels[0].gain_db - expected->gain) < .001 &&
        fabs(gtk_range_get_value(GTK_RANGE(fader)) - expected->gain) < .001 &&
        meters.channel_loudness_active[0] == expected->active &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle)) == expected->enabled;
}
static void auto_volume_wait(Window *window, gdouble gain, gboolean active, gboolean enabled)
{
    AutoVolumeExpected expected = {window, gain, active, enabled};
    wait_until(auto_volume_matches, &expected, "actual auto gain, mixer fader and real OTO SES state agree");
}
typedef struct { gdouble gain; gboolean bypass; } AutoVolumeSaved;
static gboolean auto_volume_saved(gpointer value)
{
    AutoVolumeSaved *expected = value; JsonNode *body = body_new();
    json_object_set_string_member(json_node_get_object(body), "kind", "dj_mixer");
    JsonNode *response = request("/v1/workflows/desktop/list", body); json_node_free(body);
    JsonArray *records = json_object_get_array_member(json_node_get_object(response), "records"); gboolean matched = FALSE;
    for (guint i = 0; i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i);
        if (g_strcmp0(radios_json_string(record, "name", ""), "DJ mixer")) continue;
        JsonObject *cfg = json_object_get_object_member(record, "config");
        JsonObject *channel = json_array_get_object_element(json_object_get_array_member(cfg, "channels"), 0);
        matched = fabs(json_object_get_double_member(channel, "gain_db") - expected->gain) < .001 &&
            json_object_get_boolean_member(channel, "loudness_bypass") == expected->bypass;
    }
    json_node_free(response); return matched;
}
static gdouble auto_volume_capture(void)
{
    goffset offset = capture_offset("MAIN"); run_for(180); gdouble peak;
    gdouble rms = capture_rms("MAIN", offset, &peak); g_assert_cmpfloat(peak, <, .99); return rms;
}
static void auto_volume_screenshot(Window *window, const gchar *name)
{
    const gchar *directory = g_getenv("RADIOS_SHOT_DIR"); if (!directory || !*directory) return;
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0); run_for(75);
    GtkWidget *target = find(window->root, "dj-channel-1-normalize"); g_assert_true(GTK_IS_WIDGET(target));
    for (GtkWidget *parent = gtk_widget_get_parent(target); parent; parent = gtk_widget_get_parent(parent)) {
        if (!GTK_IS_SCROLLED_WINDOW(parent)) continue;
        GtkWidget *content = gtk_bin_get_child(GTK_BIN(parent));
        if (GTK_IS_VIEWPORT(content)) content = gtk_bin_get_child(GTK_BIN(content));
        gint x = 0, y = 0;
        if (!content || !gtk_widget_translate_coordinates(target, content, 0, 0, &x, &y)) continue;
        GtkAdjustment *vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent));
        GtkAdjustment *horizontal = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(parent));
        gtk_adjustment_set_value(vertical, CLAMP((gdouble)y - 60, gtk_adjustment_get_lower(vertical),
            MAX(gtk_adjustment_get_lower(vertical), gtk_adjustment_get_upper(vertical) - gtk_adjustment_get_page_size(vertical))));
        gtk_adjustment_set_value(horizontal, CLAMP((gdouble)x - 20, gtk_adjustment_get_lower(horizontal),
            MAX(gtk_adjustment_get_lower(horizontal), gtk_adjustment_get_upper(horizontal) - gtk_adjustment_get_page_size(horizontal))));
        run_for(25);
    }
    run_for(75);
    GdkWindow *surface = gtk_widget_get_window(window->root); g_assert_nonnull(surface);
    gint target_x = 0, target_y = 0;
    g_assert_true(gtk_widget_translate_coordinates(target, window->root, 0, 0, &target_x, &target_y));
    g_assert_cmpint(target_x, >=, 0); g_assert_cmpint(target_y, >=, 0);
    g_assert_cmpint(target_x + gtk_widget_get_allocated_width(target), <=, gdk_window_get_width(surface));
    g_assert_cmpint(target_y + gtk_widget_get_allocated_height(target), <=, gdk_window_get_height(surface));
    GdkPixbuf *shot = gdk_pixbuf_get_from_window(surface, 0, 0, gdk_window_get_width(surface), gdk_window_get_height(surface));
    g_assert_nonnull(shot); gchar *path = g_build_filename(directory, name, NULL); GError *error = NULL;
    g_assert_true(gdk_pixbuf_save(shot, path, "png", &error, NULL)); g_assert_no_error(error); g_free(path); g_object_unref(shot);
}
static void auto_volume_load(Window *window, JsonNode *detail)
{
    GError *error = NULL; g_assert_true(radios_window_select_profile(window->window, "dj"));
    require_call(radios_audio_ui_set_deck_main(window->ctx, 0, TRUE, &error), &error);
    require_call(radios_audio_ui_load_media(window->ctx, window->root, json_node_get_object(detail), 0, TRUE, &error), &error);
    deck_wait(window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
}
static void real_toggle_manual_takeover_off_baseline_pcm(void)
{
    Window window = window_open(); GError *error = NULL; JsonNode *detail = media_import(&window);
    RadioSLoudness measured;
    g_assert_true(radios_loudness_read(json_object_get_object_member(json_node_get_object(detail), "analysis"), &measured));
    g_assert_true(measured.measured); gdouble target = -24, correction = radios_loudness_gain_db(&measured, target);
    g_assert_cmpfloat(correction, <, -1); g_assert_cmpfloat(correction, >, -24);
    require_call(radios_audio_ui_set_loudness(window.ctx, FALSE, target, &error), &error);
    RadioSAudioLoudnessPolicy policy; radios_audio_loudness_policy_default(&policy);
    require_call(radios_audio_ui_set_loudness_policy(window.ctx, &policy, &error), &error);
    RadioSAudioMixerConfig mixer; g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i) if (mixer.channels[i].source_deck == 0) mixer.channels[i].source_deck = -1;
    mixer.channels[0].source_deck = 0; mixer.channels[0].gain_db = 0; mixer.channels[0].loudness_bypass = TRUE;
    require_call(radios_audio_ui_set_mixer(window.ctx, &mixer, &error), &error);
    auto_volume_load(&window, detail); auto_volume_wait(&window, 0, FALSE, FALSE); run_for(150);
    gdouble baseline = auto_volume_capture(); g_assert_cmpfloat(baseline, >, .1);
    GtkWidget *fader = find(window.root, "dj-channel-1-gain"); g_assert_true(GTK_IS_RANGE(fader));
    gtk_range_set_value(GTK_RANGE(fader), -18); auto_volume_wait(&window, -18, FALSE, FALSE);
    toggle(window.root, "dj-channel-1-normalize", TRUE);
    auto_volume_wait(&window, -18, FALSE, TRUE);
    GtkWidget *status = find(window.root, "dj-channel-1-normalization"); g_assert_true(GTK_IS_LABEL(status));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(status)), ==, "Genel kapalı");
    GtkWidget *normalizer = find(window.root, "dj-channel-1-normalize");
    g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(normalizer), "is-suspended"));
    auto_volume_screenshot(&window, "auto-global-off.png");
    require_call(radios_audio_ui_set_loudness(window.ctx, TRUE, target, &error), &error);
    auto_volume_wait(&window, correction, TRUE, TRUE); run_for(100);
    gdouble normalized = auto_volume_capture(), factor = pow(10, correction / 20);
    g_assert_cmpfloat(fabs(normalized / baseline - factor), <, .015);
    g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(normalizer), "is-applying"));
    g_assert_false(gtk_style_context_has_class(gtk_widget_get_style_context(normalizer), "is-suspended"));
    auto_volume_screenshot(&window, "mixer-auto-volume.png");
    gtk_range_set_value(GTK_RANGE(fader), 6); auto_volume_wait(&window, correction, TRUE, TRUE); run_for(100);
    gdouble after_manual = auto_volume_capture(); g_assert_cmpfloat(fabs(after_manual / normalized - 1), <, .04);
    gtk_range_set_value(GTK_RANGE(fader), -12); auto_volume_wait(&window, correction, TRUE, TRUE);
    AutoVolumeSaved saved = {-12, FALSE}; wait_until(auto_volume_saved, &saved, "manual -12 baseline persists while effective fader remains automatic");
    window_close(&window); window = window_open();
    auto_volume_load(&window, detail); auto_volume_wait(&window, correction, TRUE, TRUE);
    toggle(window.root, "dj-channel-1-normalize", FALSE); auto_volume_wait(&window, -12, FALSE, FALSE);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(window.root, "dj-channel-1-normalization"))), ==, "Kapalı");
    run_for(150); gdouble restored = auto_volume_capture();
    g_assert_cmpfloat(fabs(restored / baseline - pow(10, -12.0 / 20)), <, .015);
    auto_volume_screenshot(&window, "auto-off-restored-manual.png");
    saved.bypass = TRUE; wait_until(auto_volume_saved, &saved, "OTO SES off and recovered manual level are acknowledged together");
    json_node_free(detail); window_close(&window);
    g_test_message("Real OTO SES takes over prior/live manual fader, applies LUFS once to MAIN PCM, persists manual intent and restores it after close/reopen and off.");
}
static int mixer_auto_volume_run(void)
{
    g_test_add_func("/native-mixer-auto-volume/real-toggle-manual-takeover-off-baseline-MAIN-PCM", real_toggle_manual_takeover_off_baseline_pcm);
    return g_test_run();
}
int main(int argc, char **argv) { return mixer_auto_volume_fixture_main(argc, argv); }
