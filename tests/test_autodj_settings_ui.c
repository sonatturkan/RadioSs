/* Native Preferences + genuine private HTTP/media/PCM Auto DJ acceptance. */
#define main radios_autodj_settings_fixture_main
#include "test_playout_ui.c"
#undef main
#include "radios/preferences.h"
#include "radios/playout.h"

static JsonNode *settings_padded_media(Window *window, const gchar *title, gdouble frequency)
{
    const guint frames = 6 * 48000 + 31, bytes = frames * 4;
    guchar *wav = g_malloc0(44 + bytes);
    memcpy(wav, "RIFF", 4); write_u32(wav + 4, 36 + bytes);
    memcpy(wav + 8, "WAVEfmt ", 8); write_u32(wav + 16, 16);
    write_u16(wav + 20, 1); write_u16(wav + 22, 2);
    write_u32(wav + 24, 48000); write_u32(wav + 28, 192000);
    write_u16(wav + 32, 4); write_u16(wav + 34, 16);
    memcpy(wav + 36, "data", 4); write_u32(wav + 40, bytes);
    for (guint frame = 48000; frame < 5 * 48000; ++frame) {
        gint16 sample = (gint16)lrint(.25 * 32767 * sin(2 * G_PI * frequency * frame / 48000));
        write_u16(wav + 44 + frame * 4, (guint16)sample);
        write_u16(wav + 46 + frame * 4, (guint16)sample);
    }
    gchar *id = g_uuid_string_random(), *name = g_strconcat(id, ".wav", NULL);
    gchar *path = g_build_filename(config.project_root, name, NULL);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL));
    g_free(wav); g_free(id); g_free(name);
    Import output = {0};
    GCancellable *handle = radios_media_import_async(window->ctx, window->root, path,
        "music", title, import_complete, &output, import_destroy);
    g_assert_nonnull(handle); wait_until(import_done, &output, "private padded WAV is measured and imported");
    g_assert_no_error(output.error); g_assert_nonnull(output.result);
    JsonNode *detail = media_read(output.result); json_node_free(output.result);
    g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); g_object_unref(handle);
    return detail;
}

static GtkWidget *settings_open(Window *window)
{
    GtkWidget *preferences = radios_preferences_open(window->ctx);
    GtkWidget *category = find(preferences, "preferences-category-autodj");
    GtkWidget *categories = find(preferences, "preferences-categories");
    g_assert_true(GTK_IS_LIST_BOX_ROW(category)); g_assert_true(GTK_IS_LIST_BOX(categories));
    gtk_list_box_select_row(GTK_LIST_BOX(categories), GTK_LIST_BOX_ROW(category));
    run_for(100);
    g_assert_true(gtk_widget_get_mapped(find(preferences, "preferences-autodj-page")));
    g_assert_true(GTK_IS_BUTTON(find(preferences, "autodj-marker-start")));
    return preferences;
}

typedef struct { GtkWidget *preferences; const gchar *playlist_id; } SettingsPlaylist;
static gboolean settings_playlist_ready(gpointer value)
{
    SettingsPlaylist *expected = value;
    GtkWidget *selector = find(expected->preferences, "autodj-marker-playlist");
    if (!GTK_IS_COMBO_BOX(selector)) return FALSE;
    return gtk_combo_box_set_active_id(GTK_COMBO_BOX(selector), expected->playlist_id) &&
        gtk_widget_is_sensitive(find(expected->preferences, "autodj-marker-start"));
}

static gboolean settings_policy_saved(gpointer unused)
{
    (void)unused;
    JsonNode *record = document_read("playout_policy", "main");
    JsonObject *policy = record ? json_object_get_object_member(json_node_get_object(record), "config") : NULL;
    gboolean saved = policy && radios_json_int(policy, "mode", -1) == 1 &&
        radios_json_int(policy, "overlap_ms", -1) == 1000 &&
        !json_object_get_boolean_member(policy, "beat_sync");
    g_clear_pointer(&record, json_node_free); return saved;
}

static void settings_assign_with_lead(GtkWidget *preferences, const gchar *scope, gboolean replace, gdouble out_lead)
{
    combo(preferences, "autodj-marker-scope", scope);
    toggle(preferences, "autodj-marker-replace", replace);
    spin(preferences, "autodj-marker-threshold", -45);
    spin(preferences, "autodj-marker-out-lead", out_lead);
    click(preferences, "autodj-marker-start");
    wait_until(widget_enabled, find(preferences, "autodj-marker-start"), "automatic IN/OUT assignment finishes its genuine serial HTTP saves");
    GtkWidget *progress = find(preferences, "autodj-marker-progress");
    g_assert_true(GTK_IS_PROGRESS_BAR(progress));
    g_assert_cmpfloat(gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(progress)), ==, 1);
    const gchar *status = gtk_label_get_text(GTK_LABEL(find(preferences, "autodj-marker-status")));
    g_assert_nonnull(strstr(status, "0 başarısız"));
    g_test_message("Assignment status: %s", status);
}

static void settings_assign(GtkWidget *preferences, const gchar *scope, gboolean replace)
{
    settings_assign_with_lead(preferences, scope, replace, 1);
}

static void settings_assert_counts(GtkWidget *preferences, guint updated, guint unchanged, guint skipped)
{
    const gchar *status = gtk_label_get_text(GTK_LABEL(find(preferences, "autodj-marker-status")));
    gchar *expected = g_strdup_printf("%u hesaplandı · %u güncellendi, %u zaten güncel, %u atlandı, 0 başarısız",
        updated + unchanged, updated, unchanged, skipped);
    g_assert_nonnull(strstr(status, expected));
    g_free(expected);
}

static void settings_assert_media_markers_unchanged(JsonNode *before, JsonNode *after)
{
    JsonObject *old_detail = json_node_get_object(before), *new_detail = json_node_get_object(after);
    /* No-op calculations preserve both revisioned records, not just their points. */
    g_assert_true(json_node_equal(json_object_get_member(old_detail, "media"),
        json_object_get_member(new_detail, "media")));
    g_assert_true(json_node_equal(json_object_get_member(old_detail, "markers"),
        json_object_get_member(new_detail, "markers")));
}

static JsonObject *settings_marks(JsonNode *detail)
{
    JsonNode *node = json_object_get_member(json_node_get_object(detail), "markers");
    g_assert_nonnull(node); g_assert_true(JSON_NODE_HOLDS_OBJECT(node));
    return json_node_get_object(node);
}

static void settings_assert_points(JsonNode *detail, gint64 expected_in, gint64 expected_out)
{
    JsonObject *markers = settings_marks(detail);
    /* Waveform bins are measured from real PCM and conservatively bound silence. */
    g_assert_cmpint(llabs(radios_json_int(markers, "autodj_in_ms", -100000) - expected_in), <=, 15);
    g_assert_cmpint(llabs(radios_json_int(markers, "autodj_out_ms", -100000) - expected_out), <=, 15);
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    g_assert_cmpstr(radios_json_string(markers, "version_id", ""), ==, radios_json_string(version, "_id", ""));
    g_assert_cmpstr(radios_json_string(markers, "media_sha256", ""), ==, radios_json_string(version, "sha256", ""));
    g_assert_cmpstr(radios_json_string(markers, "item_id", ""), ==, radios_json_string(media, "item_id", ""));
    g_assert_cmpint(radios_json_int(markers, "out_ms", 0), ==, 6001);
}

static void settings_capture(GtkWidget *preferences)
{
    const gchar *path = g_getenv("RADIOS_AUTODJ_SETTINGS_CAPTURE");
    if (!path || !*path) return;
    run_for(150);
    GdkPixbuf *pixels = gdk_pixbuf_get_from_window(gtk_widget_get_window(preferences), 0, 0,
        gtk_widget_get_allocated_width(preferences), gtk_widget_get_allocated_height(preferences));
    g_assert_nonnull(pixels); GError *error = NULL;
    g_assert_true(gdk_pixbuf_save(pixels, path, "png", &error, NULL)); g_assert_no_error(error);
    g_object_unref(pixels);
}

static void settings_capture_wait(const gchar *bus, goffset first)
{
    gint64 deadline = g_get_monotonic_time() + 1500000;
    while (capture_offset(bus) - first <= 4800 * 8 && g_get_monotonic_time() < deadline) pump();
    g_assert_cmpint(capture_offset(bus) - first, >, 4800 * 8);
}

static gboolean settings_empty_catalog(gpointer preferences)
{
    GtkWidget *status = find(preferences, "autodj-marker-status");
    return GTK_IS_LABEL(status) && strstr(gtk_label_get_text(GTK_LABEL(status)), "Kayıtlı playlist yok");
}

static void test_settings_bulk_markers_scopes_persistence_PCM(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL);
    GtkWidget *empty = settings_open(&window);
    wait_until(settings_empty_catalog, empty, "empty authenticated playlist catalog is acknowledged without a NULL UUID warning");
    g_assert_false(gtk_widget_is_sensitive(find(empty, "autodj-marker-start")));
    combo(empty, "autodj-marker-scope", "all");
    g_assert_true(gtk_widget_is_sensitive(find(empty, "autodj-marker-start")));
    click(empty, "preferences-close"); run_for(100);
    JsonNode *a = settings_padded_media(&window, "Auto DJ settings selected padded A", 700);
    JsonNode *b = media_import(&window, "Auto DJ settings existing manual B", 1700, .25, "music");
    JsonNode *c = settings_padded_media(&window, "Auto DJ settings outside padded C", 2700);
    JsonNode *jingle = media_import(&window, "Auto DJ settings not a music item", 3700, .2, "jingle");
    JsonNode *marked = autodj_marker_detail(b, 1500, 3500); json_node_free(b); b = marked;
    JsonNode *original_b_markers = json_node_copy(json_object_get_member(json_node_get_object(b), "markers"));
    JsonNode *playlist = playlist_create("Auto DJ settings selected playlist", a, b);
    const gchar *playlist_id = radios_json_string(json_node_get_object(playlist), "_id", "");
    GtkWidget *preferences = settings_open(&window);
    SettingsPlaylist selected = {preferences, playlist_id};
    wait_until(settings_playlist_ready, &selected, "actual authenticated playlist catalog contains the selected playlist");
    wait_until(widget_enabled, find(preferences, "autodj-transition-mode"), "acknowledged station transition policy is ready");
    combo_index(preferences, "autodj-transition-mode", 1); spin(preferences, "autodj-transition-overlap", 1);
    click(preferences, "autodj-transition-save");
    wait_until(settings_policy_saved, NULL, "Preferences transition default is genuinely persisted in the existing station policy document");
    settings_assign(preferences, "playlist", FALSE);
    settings_assert_counts(preferences, 1, 0, 1);
    JsonNode *saved_a = media_read(a), *saved_b = media_read(b), *outside = media_read(c);
    settings_assert_points(saved_a, 1000, 4000);
    g_assert_true(json_node_equal(original_b_markers, json_object_get_member(json_node_get_object(saved_b), "markers")));
    JsonNode *outside_markers = json_object_get_member(json_node_get_object(outside), "markers");
    g_assert_true(!outside_markers || JSON_NODE_HOLDS_NULL(outside_markers));
    json_node_free(outside); json_node_free(saved_b);
    JsonNode *first_a_markers = json_node_copy(json_object_get_member(json_node_get_object(saved_a), "markers"));
    settings_assign(preferences, "all", FALSE);
    settings_assert_counts(preferences, 1, 0, 2);
    JsonNode *after_a = media_read(a), *saved_c = media_read(c); saved_b = media_read(b);
    settings_assert_points(saved_c, 1000, 4000);
    g_assert_true(json_node_equal(first_a_markers, json_object_get_member(json_node_get_object(after_a), "markers")));
    g_assert_true(json_node_equal(original_b_markers, json_object_get_member(json_node_get_object(saved_b), "markers")));
    JsonNode *unchanged_jingle = media_read(jingle);
    JsonNode *jingle_markers = json_object_get_member(json_node_get_object(unchanged_jingle), "markers");
    g_assert_true(!jingle_markers || JSON_NODE_HOLDS_NULL(jingle_markers));
    json_node_free(unchanged_jingle); json_node_free(after_a); json_node_free(saved_b);
    settings_assign(preferences, "playlist", TRUE);
    settings_assert_counts(preferences, 1, 1, 0);
    saved_b = media_read(b); settings_assert_points(saved_b, 0, 5001);
    const gchar *preserved[] = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms", "automatic"};
    for (guint i = 0; i < G_N_ELEMENTS(preserved); ++i)
        g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(original_b_markers), preserved[i]),
            json_object_get_member(settings_marks(saved_b), preserved[i])));
    JsonNode *recalculated_a = media_read(a), *recalculated_b = media_read(b);
    settings_assign(preferences, "playlist", TRUE);
    settings_assert_counts(preferences, 0, 2, 0);
    JsonNode *same_a = media_read(a), *same_b = media_read(b);
    settings_assert_media_markers_unchanged(recalculated_a, same_a);
    settings_assert_media_markers_unchanged(recalculated_b, same_b);
    settings_assign(preferences, "playlist", FALSE);
    settings_assert_counts(preferences, 0, 0, 2);
    JsonNode *protected_a = media_read(a), *protected_b = media_read(b);
    settings_assert_media_markers_unchanged(same_a, protected_a);
    settings_assert_media_markers_unchanged(same_b, protected_b);
    /* A changed detector setting still performs genuine revisioned HTTP writes. */
    settings_assign_with_lead(preferences, "playlist", TRUE, 2);
    settings_assert_counts(preferences, 2, 0, 0);
    JsonNode *changed_a = media_read(a), *changed_b = media_read(b);
    settings_assert_points(changed_a, 1000, 3000);
    settings_assert_points(changed_b, 0, 4001);
    g_assert_cmpint(radios_json_int(settings_marks(changed_a), "_rev", 0), ==,
        radios_json_int(settings_marks(protected_a), "_rev", 0) + 1);
    g_assert_cmpint(radios_json_int(settings_marks(changed_b), "_rev", 0), ==,
        radios_json_int(settings_marks(protected_b), "_rev", 0) + 1);
    settings_assign(preferences, "playlist", TRUE);
    settings_assert_counts(preferences, 2, 0, 0);
    JsonNode *restored_a = media_read(a), *restored_b = media_read(b);
    settings_assert_points(restored_a, 1000, 4000);
    settings_assert_points(restored_b, 0, 5001);
    json_node_free(recalculated_a); json_node_free(recalculated_b);
    json_node_free(same_a); json_node_free(same_b); json_node_free(protected_a); json_node_free(protected_b);
    json_node_free(changed_a); json_node_free(changed_b); json_node_free(restored_a); json_node_free(restored_b);
    assert_saved_playlist_unchanged(playlist); settings_capture(preferences);
    click(preferences, "preferences-close"); run_for(100);
    preferences = settings_open(&window); selected.preferences = preferences;
    wait_until(settings_playlist_ready, &selected, "Auto DJ settings remains usable after reopen");
    wait_until(widget_enabled, find(preferences, "autodj-transition-mode"), "saved transition policy reloads after closing settings");
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(find(preferences, "autodj-transition-mode"))), ==, 1);
    g_assert_cmpfloat(fabs(gtk_spin_button_get_value(GTK_SPIN_BUTTON(find(preferences, "autodj-transition-overlap"))) - 1), <, .001);
    click(preferences, "preferences-close");
    window_close(&window); window = window_open();
    JsonNode *reopened_a = media_read(a), *reopened_c = media_read(c);
    settings_assert_points(reopened_a, 1000, 4000); settings_assert_points(reopened_c, 1000, 4000);
    JsonNode *programme = programme_config(a, c); GError *error = NULL;
    require_call(radios_playout_ui_start_programme(window.ctx, window.root, json_node_get_object(programme), 0, 1, 1, &error), &error);
    RadioSPlayoutSnapshot run = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 1150);
    g_test_message("Reopened actual runtime transition=%u overlap=%" G_GUINT64_FORMAT "ms cue=%" G_GUINT64_FORMAT "ms", run.transition, run.overlap_ms, run.cue_in_ms);
    g_assert_cmpuint(run.transition, ==, RADIOS_AUDIO_RUN_CROSSFADE);
    g_assert_cmpuint(run.overlap_ms, ==, 1000);
    g_assert_cmpuint(run.cue_in_ms, >=, 985); g_assert_cmpuint(run.cue_in_ms, <=, 1015);
    goffset pcm = capture_offset("MAIN"); run_for(200); settings_capture_wait("MAIN", pcm);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .1);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 2700), <, .01);
    run = run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 3900);
    g_assert_cmpuint(run.position_ms, >=, 3980); g_assert_cmpuint(run.position_ms, <, 4300);
    pcm = capture_offset("MAIN"); run_for(150); settings_capture_wait("MAIN", pcm);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .015);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 2700), >, .015);
    run = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 1000);
    g_assert_cmpuint(run.cue_in_ms, >=, 985); g_assert_cmpuint(run.cue_in_ms, <=, 1015);
    pcm = capture_offset("MAIN"); run_for(200); settings_capture_wait("MAIN", pcm); g_assert_cmpfloat(tone_energy("MAIN", pcm, 2700), >, .1);
    stop_run(&window); g_assert_cmpfloat(measure("CUE").peak[0], <, .0001);
    JsonNode *final_a = media_read(a), *final_c = media_read(c);
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(reopened_a), "markers"), json_object_get_member(json_node_get_object(final_a), "markers")));
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(reopened_c), "markers"), json_object_get_member(json_node_get_object(final_c), "markers")));
    assert_saved_playlist_unchanged(playlist);
    json_node_free(final_a); json_node_free(final_c); json_node_free(reopened_a); json_node_free(reopened_c); json_node_free(programme);
    json_node_free(first_a_markers); json_node_free(original_b_markers); json_node_free(saved_a); json_node_free(saved_b);
    json_node_free(saved_c); json_node_free(playlist); json_node_free(a); json_node_free(b); json_node_free(c); json_node_free(jingle);
    window_close(&window);
    g_test_message("Native Auto DJ settings distinguishes actual updates, successful unchanged recalculations and protected skips without revision churn, saves changed detector settings, assigns only selected playlist or all music, preserves manual cues, survives app reopen and drives measured MAIN IN/OUT crossfades; CUE remains silent.");
}


static JsonNode *settings_dense_outro_media(Window *window)
{
    const guint frames = 130 * 48000, bytes = frames * 4;
    guchar *wav = g_malloc0(44 + bytes);
    memcpy(wav, "RIFF", 4); write_u32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8);
    write_u32(wav + 16, 16); write_u16(wav + 20, 1); write_u16(wav + 22, 2);
    write_u32(wav + 24, 48000); write_u32(wav + 28, 192000); write_u16(wav + 32, 4); write_u16(wav + 34, 16);
    memcpy(wav + 36, "data", 4); write_u32(wav + 40, bytes);
    for (guint frame = 0; frame < 129 * 48000; ++frame) {
        gdouble seconds = frame / 48000.0, phase = fmod(seconds, .5);
        gdouble floor = seconds < 76 ? .35 : seconds < 100 ? .12 : .65;
        gdouble pulse = seconds < 76 ? .3 : seconds < 100 ? .22 : .65;
        /* Earlier music breathes inside each bar. The final section changes
         * its rhythmic shape as well as its level; a gain step alone must not
         * be mistaken for a DJ mix point. */
        gdouble envelope = seconds < 100 ? .35 + .65 * fabs(sin(2 * G_PI * seconds / .8)) : 1;
        gdouble value = envelope * (floor + pulse * exp(-phase * 35)) * sin(2 * G_PI * 1700 * seconds);
        gint16 sample = (gint16)lrint(CLAMP(value, -1, 1) * 32767);
        write_u16(wav + 44 + frame * 4, (guint16)sample); write_u16(wav + 46 + frame * 4, (guint16)sample);
    }
    gchar *id = g_uuid_string_random(), *name = g_strconcat(id, ".wav", NULL);
    gchar *path = g_build_filename(config.project_root, name, NULL); g_free(name); g_free(id);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL)); g_free(wav);
    Import imported = {0}; GCancellable *handle = radios_media_import_async(window->ctx, window->root, path,
        "music", "130 saniye gerçek yoğun son bölüm", import_complete, &imported, import_destroy);
    g_assert_nonnull(handle); wait_until(import_done, &imported, "full 130-second PCM is genuinely measured and imported");
    g_assert_no_error(imported.error); JsonNode *detail = media_read(imported.result);
    json_node_free(imported.result); g_object_unref(handle); g_assert_cmpint(g_unlink(path), ==, 0); g_free(path);
    RadioSRhythmAnalysis grid = {.bpm = 120, .confidence = 1, .first_beat_ms = 0,
        .downbeat_ms = 0, .has_downbeat = TRUE, .analyzed_ms = 120000, .manual = TRUE};
    Import saved = {0}; handle = radios_media_rhythm_save_async(window->ctx, window->root, json_node_get_object(detail),
        &grid, import_complete, &saved, import_destroy);
    g_assert_nonnull(handle); wait_until(import_done, &saved, "manual beat grid is saved through genuine analysis CAS");
    g_assert_no_error(saved.error); json_node_free(detail); detail = media_read(saved.result);
    json_node_free(saved.result); g_object_unref(handle); return detail;
}
static void test_settings_dense_final_section_moves_OUT_before_physical_tail(void)
{
    Window window = window_open(); JsonNode *detail = settings_dense_outro_media(&window);
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    JsonNode *body = body_new(); JsonObject *input = json_node_get_object(body), *markers = json_object_new();
    json_object_set_string_member(input, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_int_member(input, "expected_revision", radios_json_int(media, "_rev", 0));
    json_object_set_null_member(input, "expected_marker_revision"); gchar *key = g_uuid_string_random();
    json_object_set_string_member(input, "idempotency_key", key); g_free(key);
    const gchar *names[] = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms", "autodj_in_ms", "autodj_out_ms"};
    const gint64 values[] = {0, 2500, 130000, 130000, 130000, 130000, 0, 0, 129000};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) json_object_set_int_member(markers, names[i], values[i]);
    json_object_set_boolean_member(markers, "automatic", FALSE); json_object_set_object_member(input, "markers", markers);
    JsonNode *stale = request("/v1/workflows/desktop-media/markers-save", body); json_node_free(body); json_node_free(detail);
    JsonNode *playlist = playlist_create("Gerçek son bölüm OUT kabul testi", stale, NULL);
    GtkWidget *preferences = settings_open(&window);
    SettingsPlaylist selected = {preferences, radios_json_string(json_node_get_object(playlist), "_id", "")};
    wait_until(settings_playlist_ready, &selected, "outro fixture playlist is selectable in actual AutoDJ settings");
    settings_assign_with_lead(preferences, "playlist", TRUE, 0); settings_assert_counts(preferences, 1, 0, 0);
    JsonNode *updated = media_read(stale); JsonObject *updated_marks = settings_marks(updated);
    gint64 out = radios_json_int(updated_marks, "autodj_out_ms", 0);
    g_test_message("Full measured 130s song: stale OUT129000ms, new OUT%" G_GINT64_FORMAT "ms; final section starts100000ms", out);
    g_assert_cmpint(out, ==, 104000);
    const gchar *preserved[] = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms", "automatic"};
    for (guint i = 0; i < G_N_ELEMENTS(preserved); ++i)
        g_assert_true(json_node_equal(json_object_get_member(settings_marks(stale), preserved[i]), json_object_get_member(updated_marks, preserved[i])));
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(stale), "analysis"),
        json_object_get_member(json_node_get_object(updated), "analysis")));
    settings_assign_with_lead(preferences, "playlist", TRUE, 0); settings_assert_counts(preferences, 0, 1, 0);
    JsonNode *same = media_read(updated); settings_assert_media_markers_unchanged(updated, same);
    click(preferences, "preferences-close");
    radios_marker_editor_open_media(window.ctx, json_node_get_object(updated));
    GtkWidget *editor = dialog_find("queue-waveform-dialog"); g_assert_nonnull(editor);
    GtkWidget *out_field = find(editor, "qwf-autodj-out"); g_assert_true(GTK_IS_ENTRY(out_field));
    g_assert_cmpfloat(g_ascii_strtod(gtk_entry_get_text(GTK_ENTRY(out_field)), NULL), ==, 104);
    gtk_widget_destroy(editor);
    JsonNode *prepared = scope_authenticated_programme(updated, NULL);
    RadioSPlayoutRequest plan_request = {.station_id = radios_data_station_id(window.ctx), .database_file = config.database_file,
        .mode = RADIOS_AUDIO_RUN_CROSSFADE, .overlap_seconds = 3, .now_utc_us = g_get_real_time()};
    GError *error = NULL; RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(prepared), &plan_request, &error);
    g_assert_no_error(error); g_assert_nonnull(plan); RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, 0);
    g_assert_cmpuint(entry->run.autodj_out_frame, ==, 104000 * 48);
    g_assert_cmpuint(entry->run.media.cue_out_frame / 48, >, 128500); radios_playout_plan_free(plan);
    plan_request.single_player = TRUE; plan = radios_playout_plan_build(json_node_get_object(prepared), &plan_request, &error);
    g_assert_no_error(error); g_assert_nonnull(plan); entry = g_ptr_array_index(plan->entries, 0);
    g_assert_cmpuint(entry->run.autodj_out_frame, ==, 0);
    g_assert_cmpuint(entry->run.media.cue_out_frame / 48, >, 128500); radios_playout_plan_free(plan);
    assert_saved_playlist_unchanged(playlist);
    json_node_free(prepared); json_node_free(same); json_node_free(updated); json_node_free(stale); json_node_free(playlist); window_close(&window);
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc; g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"); cue_sink = g_getenv("RADIOS_TEST_CUE_SINK"); monitor_sink = g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink); g_assert_nonnull(cue_sink); g_assert_nonnull(monitor_sink);
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.AutoDJSettingsAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-playout/AutoDJ-settings-bulk-markers-playlist-all-persistence-MAIN-PCM", test_settings_bulk_markers_scopes_persistence_PCM);
    g_test_add_func("/native-playout/AutoDJ-settings-dense-final-section-OUT-not-end", test_settings_dense_final_section_moves_OUT_before_physical_tail);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
