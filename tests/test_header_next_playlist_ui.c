#define _POSIX_C_SOURCE 200809L
#include <glib.h>

/* U3 (C10, 18.10.2026): the header "Sonraki playlist ▾" playlist picker.
 * Genuine private SDBASE/PCM/window fixture of test_playout_ui.c: real
 * prepared plans, the real header widgets and the owned MAIN capture. */
static int next_playlist_run(void);
#define main radios_next_playlist_fixture_main
#define g_test_run next_playlist_run
#include "test_playout_ui.c"
#undef g_test_run
#undef main

#include "radios/broadcast_header.h"
#include "radios/content_scale.h"
#include "radios/header_navigation.h"

typedef struct { void (*forward)(gpointer, const gchar *); gpointer forward_data; GPtrArray *messages; } U3Notices;
static void u3_notice(gpointer value, const gchar *message)
{
    U3Notices *notices = value; g_test_message("Header notification: %s", message);
    g_ptr_array_add(notices->messages, g_strdup(message));
    if (notices->forward) notices->forward(notices->forward_data, message);
}
static void u3_notices_attach(Window *window, U3Notices *notices)
{
    notices->forward = window->ctx->notify; notices->forward_data = window->ctx->notify_data;
    notices->messages = g_ptr_array_new_with_free_func(g_free);
    window->ctx->notify = u3_notice; window->ctx->notify_data = notices;
}
static void u3_notices_detach(Window *window, U3Notices *notices)
{
    window->ctx->notify = notices->forward; window->ctx->notify_data = notices->forward_data;
    g_ptr_array_unref(notices->messages);
}
typedef struct { U3Notices *notices; const gchar *text; } U3Notice;
static gboolean u3_noticed(gpointer value)
{
    U3Notice *expected = value;
    for (guint i = 0; i < expected->notices->messages->len; ++i)
        if (strstr(g_ptr_array_index(expected->notices->messages, i), expected->text)) return TRUE;
    return FALSE;
}
static void u3_notice_wait(U3Notices *notices, const gchar *text)
{ U3Notice expected = {notices, text}; wait_until(u3_noticed, &expected, text); }

static GtkWidget *u3_choose(Window *window)
{ GtkWidget *choose = find(window->root, "header-next-playlist-choose"); g_assert_true(GTK_IS_BUTTON(choose)); return choose; }
static gboolean u3_disabled(gpointer widget) { return !gtk_widget_is_sensitive(widget); }
static gboolean u3_mapped(gpointer widget) { return gtk_widget_get_mapped(widget); }
/* Screen origin through the nearest native window. */
static void u3_screen(GtkWidget *widget, gint *x, gint *y)
{
    GtkWidget *native = widget;
    while (!gtk_widget_get_has_window(native)) native = gtk_widget_get_parent(native);
    gint nx = 0, ny = 0, ox = 0, oy = 0;
    g_assert_true(gtk_widget_translate_coordinates(widget, native, 0, 0, &nx, &ny));
    gdk_window_get_origin(gtk_widget_get_window(native), &ox, &oy); *x = ox + nx; *y = oy + ny;
}
static gboolean u3_closed(gpointer choose) { return !gtk_toggle_button_get_active(choose); }
/* ▾ is a menu button: its popover points at the middle of ▾, just below
 * it. The header is outside the zoomed workspace at any workspace scale. */
static GtkWidget *u3_menu_open_at(Window *window, gint *x, gint *y)
{
    GtkWidget *choose = u3_choose(window); g_assert_true(GTK_IS_MENU_BUTTON(choose));
    wait_until(u3_closed, choose, "the previous playlist picker closed");
    wait_until(widget_enabled, choose, "▾ follows an available Sonraki playlist");
    gtk_button_clicked(GTK_BUTTON(choose));
    GtkWidget *menu = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(choose)));
    g_assert_true(GTK_IS_POPOVER(menu)); g_assert_cmpstr(gtk_widget_get_name(menu), ==, "header-next-playlist-menu");
    g_assert_true(gtk_popover_get_relative_to(GTK_POPOVER(menu)) == choose);
    wait_until(u3_mapped, menu, "playlist picker opens under ▾");
    /* The popover slides in; measure where it settles. */
    gint ax = 0, ay = 0, mx = 0, my = 0, px = G_MININT, py = G_MININT; guint still = 0;
    for (gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND; still < 4 && g_get_monotonic_time() < deadline;) {
        run_for(40); u3_screen(menu, &mx, &my);
        still = mx == px && my == py ? still + 1 : 0; px = mx; py = my;
    }
    g_assert_cmpuint(still, >=, 4); u3_screen(choose, &ax, &ay);
    gint width = gtk_widget_get_allocated_width(choose), height = gtk_widget_get_allocated_height(choose);
    gint menu_width = gtk_widget_get_allocated_width(menu);
    g_test_message("▾ at (%d,%d) %dx%d; picker at (%d,%d) width %d", ax, ay, width, height, mx, my, menu_width);
    g_assert_cmpint(ABS(mx + menu_width / 2 - (ax + width / 2)), <=, 2); g_assert_cmpint(ABS(my - (ay + height)), <=, 2);
    if (x) {
        *x = mx;
    }
    if (y) {
        *y = my;
    }
    return menu;
}
static GtkWidget *u3_menu_open(Window *window) { return u3_menu_open_at(window, NULL, NULL); }
static GtkWidget *u3_item(GtkWidget *menu, guint index)
{
    GList *items = gtk_container_get_children(GTK_CONTAINER(gtk_bin_get_child(GTK_BIN(menu))));
    GtkWidget *item = g_list_nth_data(items, index); g_list_free(items);
    g_assert_true(GTK_IS_MODEL_BUTTON(item)); return item;
}
/* Titles in flow order; the first is the playing playlist: marked, no target. */
static void u3_menu_expect(GtkWidget *menu, const gchar *const *titles, guint count)
{
    GList *items = gtk_container_get_children(GTK_CONTAINER(gtk_bin_get_child(GTK_BIN(menu))));
    g_assert_cmpuint(g_list_length(items), ==, count); g_list_free(items);
    for (guint i = 0; i < count; ++i) {
        GtkWidget *item = u3_item(menu, i); gchar *name = g_strdup_printf("header-next-playlist-choice-%u", i);
        g_assert_cmpstr(gtk_widget_get_name(item), ==, name); g_free(name);
        gchar *text = NULL; GtkButtonRole role = GTK_BUTTON_ROLE_NORMAL; gboolean active = FALSE;
        g_object_get(item, "text", &text, "role", &role, "active", &active, NULL);
        g_assert_cmpstr(text, ==, titles[i]); g_free(text);
        g_assert_cmpint(role, ==, GTK_BUTTON_ROLE_RADIO);
        g_assert_cmpint(!!active, ==, i == 0);
        g_assert_cmpint(!!gtk_widget_get_sensitive(item), ==, i != 0);
        g_assert_true(gtk_widget_get_mapped(item));
    }
}
static void u3_pick(GtkWidget *menu, guint index)
{ gtk_button_clicked(GTK_BUTTON(u3_item(menu, index))); }
/* Optional private-display capture of the header with the picker open:
 * RADIOS_U3_CAPTURE_DIR=<dir> writes <dir>/u3-<name>.png. */
static void u3_capture(Window *window, const gchar *name)
{
    const gchar *directory = g_getenv("RADIOS_U3_CAPTURE_DIR"); if (!directory || !*directory) return;
    run_for(150); g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    GdkWindow *root = gtk_widget_get_window(window->root);
    GdkPixbuf *picture = gdk_pixbuf_get_from_window(root, 0, 0, gdk_window_get_width(root), MIN(gdk_window_get_height(root), 520));
    g_assert_nonnull(picture);
    gchar *file = g_strdup_printf("u3-%s.png", name), *path = g_build_filename(directory, file, NULL); GError *error = NULL;
    g_assert_true(gdk_pixbuf_save(picture, path, "png", &error, NULL)); g_assert_no_error(error);
    g_test_message("U3 capture: %s", path); g_object_unref(picture); g_free(path); g_free(file);
}
static void u3_tone(gdouble wanted, gdouble absent)
{
    goffset at = capture_offset("MAIN"); run_for(300);
    gdouble present = tone_energy("MAIN", at, wanted), other = tone_energy("MAIN", at, absent);
    g_test_message("MAIN after the jump: %.0f Hz %.4f, %.0f Hz %.4f", wanted, present, absent, other);
    g_assert_cmpfloat(present, >, .04); g_assert_cmpfloat(other, <, .01);
}

typedef struct { guint index; gchar *title, *first, *entry; } U3Target;
/* Independent expectation from the accepted plan snapshot: the playing
 * block, then every later block start, as repeated "Sonraki playlist". */
static GArray *u3_expected(RadioSViewContext *ctx)
{
    JsonNode *plan = radios_playout_ui_plan_snapshot(ctx); g_assert_nonnull(plan);
    JsonObject *object = json_node_get_object(plan); JsonArray *rows = json_object_get_array_member(object, "entries");
    guint current = (guint)radios_json_int(object, "index", 0); const gchar *previous = NULL;
    GArray *targets = g_array_new(FALSE, TRUE, sizeof(U3Target));
    for (guint i = current; i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i); const gchar *block = radios_json_string(row, "block_id", "");
        if (i > current && (!*block || !g_strcmp0(block, previous))) continue;
        U3Target target = {i, g_strdup(radios_json_string(row, "block_title", "")), g_strdup(radios_json_string(row, "title", "")),
            g_strdup(radios_json_string(row, "entry_id", ""))};
        g_test_message("Accepted plan playlist %u: row %u '%s' first song '%s'", targets->len, i, target.title, target.first);
        g_array_append_val(targets, target); previous = block;
    }
    json_node_free(plan); return targets;
}
static void u3_targets_free(GArray *targets)
{
    for (guint i = 0; i < targets->len; ++i) {
        U3Target *target = &g_array_index(targets, U3Target, i); g_free(target->title); g_free(target->first); g_free(target->entry);
    }
    g_array_unref(targets);
}
static void u3_menu_expect_targets(GtkWidget *menu, GArray *targets)
{
    const gchar *titles[16]; g_assert_cmpuint(targets->len, <=, G_N_ELEMENTS(titles));
    for (guint i = 0; i < targets->len; ++i) titles[i] = g_array_index(targets, U3Target, i).title;
    u3_menu_expect(menu, titles, targets->len);
}
static void u3_choices_expect(JsonArray *choices, GArray *targets, guint64 epoch)
{
    g_assert_nonnull(choices); g_assert_cmpuint(json_array_get_length(choices), ==, targets->len);
    for (guint i = 0; i < targets->len; ++i) {
        JsonObject *choice = json_array_get_object_element(choices, i); U3Target *target = &g_array_index(targets, U3Target, i);
        g_assert_cmpint(radios_json_int(choice, "index", -1), ==, target->index);
        g_assert_cmpstr(radios_json_string(choice, "title", ""), ==, target->title);
        g_assert_cmpstr(radios_json_string(choice, "first_title", ""), ==, target->first);
        g_assert_cmpstr(radios_json_string(choice, "entry_id", ""), ==, target->entry);
        g_assert_cmpint(radios_json_int(choice, "epoch", 0), ==, (gint64)epoch);
        g_assert_cmpint(!!json_object_get_boolean_member(choice, "current"), ==, i == 0);
    }
}

typedef struct { RadioSAudioEngine *engine; gint deck; guint index; } U3Runner;
static void u3_runner_snapshot(RadioSAudioEngine *engine, gint deck, RadioSAudioRunSnapshot *run)
{
    if (deck < 0) radios_audio_engine_runner_snapshot(engine, run);
    else radios_audio_engine_player_runner_snapshot(engine, (guint)deck, run);
}
static gboolean u3_runner_at(gpointer value)
{
    U3Runner *expected = value; RadioSAudioRunSnapshot run = {0}; u3_runner_snapshot(expected->engine, expected->deck, &run);
    return run.state == RADIOS_AUDIO_RUN_PLAYING && run.index == expected->index;
}
static RadioSAudioRunSnapshot u3_runner_wait(RadioSAudioEngine *engine, gint deck, guint index)
{
    U3Runner expected = {engine, deck, index}; wait_until(u3_runner_at, &expected, "runner plays the expected row");
    RadioSAudioRunSnapshot run = {0}; u3_runner_snapshot(engine, deck, &run); return run;
}
/* The engine runs on its own threads. Without a GTK iteration nothing else
 * (here: the general driver's 250 ms tick) observes the new run. */
static RadioSAudioRunSnapshot u3_engine_playing_unpumped(RadioSAudioEngine *engine)
{
    RadioSAudioRunSnapshot run = {0}; gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    do { g_usleep(2000); radios_audio_engine_runner_snapshot(engine, &run); }
    while ((run.state != RADIOS_AUDIO_RUN_PLAYING || run.index) && g_get_monotonic_time() < deadline);
    if (run.state != RADIOS_AUDIO_RUN_PLAYING || run.index)
        g_error("Foreign run did not reach PLAYING without the main loop: state%d index%u", run.state, run.index);
    return run;
}

/* Three explicit playlists of two verified songs each and an empty one
 * between A and B; event rows 0-1 A, 2-3 B, 4-5 C. */
static const gchar *const u3_song_titles[] = {"U3 A1", "U3 A2", "U3 B1", "U3 B2", "U3 C1", "U3 C2"};
static const gdouble u3_song_tones[] = {600, 700, 1300, 1400, 2100, 2300};
static const gchar *const u3_playlist_titles[] = {"U3 Playlist A", "U3 Playlist B", "U3 Playlist C"};
static void u3_songs_new(Window *window, JsonNode **songs)
{
    for (guint i = 0; i < G_N_ELEMENTS(u3_song_titles); ++i)
        songs[i] = media_import_rhythm(window, u3_song_titles[i], u3_song_tones[i], .24, "music", 120);
}
static JsonNode *u3_programme(JsonNode *const *songs)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(node, object);
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d"); g_date_time_unref(now);
    json_object_set_string_member(object, "title", "U3 üç playlist"); json_object_set_string_member(object, "date", date); g_free(date);
    json_object_set_string_member(object, "note", "");
    JsonArray *blocks = json_array_new(), *events = json_array_new(); gchar *ids[4];
    const gchar *titles[] = {"U3 Playlist A", "U3 Boş playlist", "U3 Playlist B", "U3 Playlist C"};
    for (guint i = 0; i < 4; ++i) {
        ids[i] = g_uuid_string_random(); JsonObject *block = json_object_new();
        json_object_set_string_member(block, "id", ids[i]); json_object_set_string_member(block, "kind", "music");
        json_object_set_string_member(block, "title", titles[i]); json_object_set_string_member(block, "start_time", "");
        json_object_set_string_member(block, "note", ""); json_array_add_object_element(blocks, block);
    }
    const guint owner[] = {0, 0, 2, 2, 3, 3};
    for (guint i = 0; i < G_N_ELEMENTS(owner); ++i) {
        JsonObject *detail = json_node_get_object(songs[i]), *media = json_object_get_object_member(detail, "media");
        JsonObject *version = json_object_get_object_member(detail, "version"), *event = json_object_new(); gchar *id = g_uuid_string_random();
        json_object_set_string_member(event, "id", id); g_free(id); json_object_set_string_member(event, "block_id", ids[owner[i]]);
        const gchar *keys[] = {"title", "artist", "album", "kind"};
        for (guint j = 0; j < G_N_ELEMENTS(keys); ++j) json_object_set_string_member(event, keys[j], radios_json_string(media, keys[j], ""));
        json_object_set_int_member(event, "duration_ms", radios_json_int(version, "duration_ms", 0));
        const gchar *empty[] = {"start_time", "version_label", "primary_file", "backup_file", "note"};
        for (guint j = 0; j < G_N_ELEMENTS(empty); ++j) json_object_set_string_member(event, empty[j], "");
        bind(event, detail); json_array_add_object_element(events, event);
    }
    for (guint i = 0; i < 4; ++i) g_free(ids[i]);
    json_object_set_array_member(object, "events", events); json_object_set_array_member(object, "blocks", blocks);
    return node;
}
static void u3_seed(Window *window, JsonNode *configuration)
{
    GtkWidget *continuous = find(window->root, "programme-general-continuous");
    if (continuous) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(continuous), FALSE);
    JsonNode *saved = document_save("programme", "Mevcut program", json_node_get_object(configuration));
    JsonObject *record = json_node_get_object(saved); click_label(window->root, "Verileri yenile");
    SeedCondition condition = {{window, 6, u3_song_titles[0]}, radios_json_string(record, "_id", ""), radios_json_int(record, "_rev", 0)};
    wait_until(seeded_programme_ready, &condition, "U3 three-playlist programme acknowledged in the native queue");
    json_node_free(saved);
}

/* (1)-(3) in the broadcast queue: the split button, the ordered list with
 * the playing playlist marked, picks to a playlist's first song with real
 * MAIN PCM, an unchanged "Sonraki playlist", stale picks and no flow. */
static void test_u3_local_queue(void)
{
    Window window = window_open(); U3Notices notices; u3_notices_attach(&window, &notices);
    JsonNode *songs[G_N_ELEMENTS(u3_song_titles)]; u3_songs_new(&window, songs);
    JsonNode *configuration = u3_programme(songs); u3_seed(&window, configuration);
    GtkWidget *choose = u3_choose(&window), *next = find(window.root, "header-next-playlist"), *split = gtk_widget_get_parent(choose);
    g_assert_true(GTK_IS_BUTTON(next)); g_assert_true(gtk_widget_get_parent(next) == split);
    g_assert_cmpstr(gtk_widget_get_name(split), ==, "header-next-playlist-split");
    g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(split), "linked"));
    GList *children = gtk_container_get_children(GTK_CONTAINER(split));
    g_assert_cmpuint(g_list_length(children), ==, 2);
    g_assert_true(children->data == next); g_assert_true(children->next->data == choose); g_list_free(children);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(next)), ==, "Sonraki playlist");
    gchar *tip = gtk_widget_get_tooltip_text(next); g_assert_cmpstr(tip, ==, "Sonraki playlistin ilk şarkısına geç"); g_free(tip);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(choose)), ==, "▾");
    tip = gtk_widget_get_tooltip_text(choose); g_assert_cmpstr(tip, ==, "Playlist seç"); g_free(tip);
    /* (3) No flow: ▾ is inactive together with "Sonraki playlist". */
    run_for(300); g_assert_false(gtk_widget_is_sensitive(next)); g_assert_false(gtk_widget_is_sensitive(choose));
    g_assert_null(radios_broadcast_navigation_playlists(window.ctx));
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(window.root, "programme-transition-mode")), 0);
    click(window.root, "header-play"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 0);
    RadioSAudioRunSnapshot first = {0}; g_assert_true(radios_playout_ui_navigation_snapshot(window.ctx, &first));
    JsonNode *plan = radios_playout_ui_plan_snapshot(window.ctx); g_assert_nonnull(plan);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(plan), "entries");
    g_assert_cmpuint(json_array_get_length(rows), ==, 6);
    /* (1) The flow's playlists in order (the empty one has no song). The
     * workspace zoom does not move the header picker. */
    gint x100 = 0, y100 = 0, x73 = 0, y73 = 0;
    GtkWidget *menu = u3_menu_open_at(&window, &x100, &y100); u3_menu_expect(menu, u3_playlist_titles, 3); u3_capture(&window, "picker-100");
    gtk_popover_popdown(GTK_POPOVER(menu));
    radios_content_scale_set_percent(find(window.root, "main-content-scale"), 73); run_for(200);
    menu = u3_menu_open_at(&window, &x73, &y73); u3_menu_expect(menu, u3_playlist_titles, 3);
    g_assert_cmpint(x73, ==, x100); g_assert_cmpint(y73, ==, y100);
    JsonArray *choices = radios_broadcast_navigation_playlists(window.ctx); g_assert_nonnull(choices);
    const guint starts[] = {0, 2, 4};
    for (guint i = 0; i < G_N_ELEMENTS(starts); ++i)
        g_assert_cmpint(radios_json_int(json_array_get_object_element(choices, i), "index", -1), ==, starts[i]);
    json_array_unref(choices);
    /* (2) A pick plays the chosen playlist's first song: B is passed over. */
    u3_pick(menu, 2); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 4, 0);
    RadioSAudioRunSnapshot jumped = {0}; g_assert_true(radios_playout_ui_navigation_snapshot(window.ctx, &jumped));
    g_assert_cmpuint(jumped.epoch, ==, first.epoch);
    g_assert_cmpstr(jumped.entry_id, ==, radios_json_string(json_array_get_object_element(rows, 4), "entry_id", ""));
    u3_tone(u3_song_tones[4], u3_song_tones[0]);
    wait_until(u3_disabled, choose, "the last playlist has no successor; ▾ follows Sonraki playlist");
    /* "Sonraki playlist" keeps its own behaviour next to ▾. */
    click(window.root, "header-previous"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 3, 0);
    click(window.root, "header-next-playlist"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 4, 0);
    u3_tone(u3_song_tones[4], u3_song_tones[3]);
    for (guint index = 3; index >= 1; --index) { click(window.root, "header-previous"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, index, 0); }
    menu = u3_menu_open(&window); u3_menu_expect(menu, u3_playlist_titles, 3);
    u3_pick(menu, 1); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 2, 0); u3_tone(u3_song_tones[2], u3_song_tones[1]);
    /* A stale pick (B already plays) and a pick from an older run change nothing. */
    gtk_button_clicked(GTK_BUTTON(u3_item(menu, 1))); u3_notice_wait(&notices, "Seçilen playlist artık sırada değil.");
    run_for(200); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 2, 0);
    click(window.root, "header-stop"); run_wait(&window, RADIOS_PLAYOUT_IDLE, 0, 0);
    wait_until(u3_disabled, choose, "stopped flow: ▾ inactive");
    click(window.root, "header-play"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 0);
    RadioSAudioRunSnapshot again = {0}; g_assert_true(radios_playout_ui_navigation_snapshot(window.ctx, &again));
    g_assert_cmpuint(again.epoch, !=, first.epoch);
    gtk_button_clicked(GTK_BUTTON(u3_item(menu, 2))); u3_notice_wait(&notices, "Yayın sırası değişti");
    run_for(200); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 0);
    click(window.root, "header-stop"); run_wait(&window, RADIOS_PLAYOUT_IDLE, 0, 0);
    json_node_free(plan); json_node_free(configuration);
    for (guint i = 0; i < G_N_ELEMENTS(songs); ++i) json_node_free(songs[i]);
    u3_notices_detach(&window, &notices); window_close(&window);
}

/* One rotation of three playlists (one verified song each, played once):
 * the calendar plan gives every source turn its own playlist block. */
static const gchar *const u3_source_titles[] = {"U3 kaynak A", "U3 kaynak B", "U3 kaynak C"};
static const gdouble u3_source_tones[] = {600, 1300, 2100};
typedef struct { JsonNode *media[3], *playlists[3]; } U3Sources;
static void u3_sources_new(Window *window, U3Sources *sources)
{
    for (guint i = 0; i < 3; ++i) {
        sources->media[i] = media_import_rhythm(window, u3_source_titles[i], u3_source_tones[i], .24, "music", 120);
        sources->playlists[i] = playlist_create(u3_playlist_titles[i], sources->media[i], NULL);
    }
}
static void u3_sources_free(U3Sources *sources)
{ for (guint i = 0; i < 3; ++i) { json_node_free(sources->media[i]); json_node_free(sources->playlists[i]); } }
static gdouble u3_source_tone(const gchar *title)
{
    for (guint i = 0; i < 3; ++i) if (!g_strcmp0(title, u3_source_titles[i])) return u3_source_tones[i];
    g_error("U3 plan row has an unknown song: %s", title); return 0;
}
static void u3_source_once(JsonObject *source, JsonNode *playlist)
{
    json_object_set_int_member(source, "interval_minutes", 0);
    JsonArray *tracks = json_object_get_array_member(json_node_get_object(playlist), "tracks"), *rules = json_array_new();
    JsonObject *rule = json_object_new();
    json_object_set_string_member(rule, "entry_id", radios_json_string(json_array_get_object_element(tracks, 0), "_id", ""));
    json_object_set_int_member(rule, "weight", 100); json_object_set_int_member(rule, "order", 0);
    json_object_set_int_member(rule, "frequency", 1); json_object_set_int_member(rule, "max_plays", 1);
    json_object_set_int_member(rule, "start_minute", 0); json_object_set_int_member(rule, "end_minute", 1440);
    json_object_set_int_member(rule, "interval_minutes", 0); json_array_add_object_element(rules, rule);
    json_object_set_array_member(source, "track_rules", rules);
}
static JsonNode *u3_weekly(JsonNode *const *playlists, gint64 start, guint minutes, const gchar *title)
{
    JsonNode *programme = weekly_pcm_configuration(playlists[0], NULL, start, minutes, title);
    JsonObject *calendar = json_object_get_object_member(json_node_get_object(programme), "calendar");
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(calendar, "rotation_library"), 0);
    JsonArray *sources = json_object_get_array_member(definition, "sources");
    for (guint i = 1; i < 3; ++i) {
        JsonNode *copy = deep_copy(json_array_get_element(sources, 0)); JsonObject *source = json_node_get_object(copy);
        gchar *id = g_uuid_string_random(); json_object_set_string_member(source, "id", id); g_free(id);
        json_object_set_string_member(source, "playlist_id", radios_json_string(json_node_get_object(playlists[i]), "_id", ""));
        json_array_add_element(sources, copy);
    }
    for (guint i = 0; i < 3; ++i) u3_source_once(json_array_get_object_element(sources, i), playlists[i]);
    return programme;
}
typedef struct { Window *window; RadioSPlayerWeekly *driver; } U3General;
static gboolean u3_general_owned(gpointer value)
{
    U3General *general = value; RadioSPlayerWeeklySnapshot owner = {0}; RadioSPlayoutSnapshot ui = {0}; RadioSAudioRunSnapshot run = {0};
    return radios_player_weekly_header_snapshot(general->driver, &owner) &&
        radios_playout_ui_get_snapshot(general->window->ctx, &ui) && ui.state == RADIOS_PLAYOUT_PLAYING &&
        radios_playout_ui_navigation_snapshot(general->window->ctx, &run) && run.state == RADIOS_AUDIO_RUN_PLAYING;
}
static gboolean u3_driver_armed(gpointer driver) { return weekly_snapshot(driver).armed; }
static gboolean u3_driver_released(gpointer driver) { return !weekly_snapshot(driver).armed; }

/* Continuous general queue while its driver owns the run: the picker lists
 * and jumps within that run, which the driver keeps owning. */
static void test_u3_general_owner(void)
{
    Window window = window_open(); navigate(&window, "broadcast"); RadioSPlayerWeekly *driver = general_driver(&window);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(window.root, "programme-general-continuous"))));
    U3Sources sources; u3_sources_new(&window, &sources);
    gint64 start = g_get_real_time() / G_TIME_SPAN_DAY * G_TIME_SPAN_DAY;
    JsonNode *programme = u3_weekly(sources.playlists, start, 1440, "U3 genel akış");
    JsonObject *calendar = json_object_get_object_member(json_node_get_object(programme), "calendar");
    JsonNode *saved = document_save("automation_calendar", "main", calendar); radios_player_weekly_general_refresh(driver);
    GeneralReady ready = {&window, driver, "U3 genel akış", start};
    wait_until(general_current_mixed_ready, &ready, "current U3 general slot projected before header Play");
    click(window.root, "header-play");
    U3General general = {&window, driver}; wait_until(u3_general_owned, &general, "general driver owns its playing run");
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    RadioSAudioRunSnapshot run = {0}; radios_audio_engine_runner_snapshot(engine, &run);
    GArray *targets = u3_expected(window.ctx); g_assert_cmpuint(targets->len, ==, 3);
    JsonArray *choices = radios_broadcast_navigation_playlists(window.ctx); u3_choices_expect(choices, targets, run.epoch); json_array_unref(choices);
    GtkWidget *menu = u3_menu_open(&window); u3_menu_expect_targets(menu, targets);
    U3Target *playing = &g_array_index(targets, U3Target, 0), *last = &g_array_index(targets, U3Target, targets->len - 1);
    u3_pick(menu, targets->len - 1);
    RadioSAudioRunSnapshot jumped = u3_runner_wait(engine, -1, last->index);
    g_assert_cmpuint(jumped.epoch, ==, run.epoch); g_assert_cmpstr(jumped.entry_id, ==, last->entry);
    u3_tone(u3_source_tone(last->first), u3_source_tone(playing->first));
    RadioSPlayerWeeklySnapshot owner = {0};
    g_assert_true(radios_player_weekly_header_snapshot(driver, &owner)); g_assert_true(owner.armed);
    wait_until(u3_disabled, u3_choose(&window), "last general playlist: ▾ inactive");
    click(window.root, "header-stop"); wait_until(u3_driver_released, driver, "header Stop disarms the general driver");
    radios_audio_engine_unref(engine); u3_targets_free(targets);
    json_node_free(saved); json_node_free(programme); u3_sources_free(&sources); window_close(&window);
}

/* Kılavuz koşulu (radios-9a): under the general driver's controls a run the
 * driver does not own is neither listed nor jumped. The driver observes a
 * foreign start on its next tick; until then "Sonraki playlist" would still
 * see the plan, but the picker refuses. Released controls follow the flow. */
static void test_u3_general_not_owner(void)
{
    Window window = window_open(); navigate(&window, "broadcast"); RadioSPlayerWeekly *driver = general_driver(&window);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(window.root, "programme-general-continuous"))));
    JsonNode *songs[G_N_ELEMENTS(u3_song_titles)]; u3_songs_new(&window, songs);
    JsonNode *configuration = u3_programme(songs), *body = body_new();
    json_object_set_string_member(json_node_get_object(body), "kind", "programme");
    json_object_set_member(json_node_get_object(body), "config", deep_copy(configuration));
    JsonNode *prepared = request("/v1/workflows/desktop-playout/prepare", body); json_node_free(body);
    click(window.root, "header-play"); wait_until(u3_driver_armed, driver, "continuous general automation armed on its empty calendar");
    RadioSPlayoutSnapshot idle = {0}; g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &idle));
    g_assert_cmpint(idle.state, ==, RADIOS_PLAYOUT_IDLE);
    GError *error = NULL;
    require_call(radios_playout_ui_start_prepared(window.ctx, window.root, json_node_get_object(prepared), 0, &error), &error);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    RadioSAudioRunSnapshot run = u3_engine_playing_unpumped(engine);
    RadioSPlayerWeeklySnapshot owner = {0};
    g_assert_true(weekly_snapshot(driver).armed); g_assert_false(radios_player_weekly_header_snapshot(driver, &owner));
    g_assert_true(radios_broadcast_navigation_available(window.ctx, 5));
    JsonArray *plain = radios_playout_ui_playlist_choices(window.ctx); g_assert_nonnull(plain);
    g_assert_cmpuint(json_array_get_length(plain), ==, 3);
    JsonObject *target = json_array_get_object_element(plain, 2);
    guint index = (guint)radios_json_int(target, "index", 0); gchar *entry = g_strdup(radios_json_string(target, "entry_id", ""));
    g_assert_cmpuint(index, ==, 4); json_array_unref(plain);
    g_assert_null(radios_broadcast_navigation_playlists(window.ctx));
    g_assert_false(radios_broadcast_navigation_jump_playlist(window.ctx, run.epoch, index, entry, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(error->message, ==, "Genel program bu yayının sahibi değil."); g_clear_error(&error);
    g_usleep(200 * 1000); RadioSAudioRunSnapshot kept = {0}; radios_audio_engine_runner_snapshot(engine, &kept);
    g_assert_cmpuint(kept.epoch, ==, run.epoch); g_assert_cmpuint(kept.index, ==, 0);
    g_assert_cmpint(kept.state, ==, RADIOS_AUDIO_RUN_PLAYING);
    /* The driver's tick hands the controls back; ▾ then follows the flow
     * exactly like "Sonraki playlist" does. */
    wait_until(u3_driver_released, driver, "general driver releases its controls after the foreign start");
    GtkWidget *menu = u3_menu_open(&window); u3_menu_expect(menu, u3_playlist_titles, 3);
    u3_pick(menu, 2); RadioSAudioRunSnapshot jumped = u3_runner_wait(engine, -1, 4);
    g_assert_cmpuint(jumped.epoch, ==, run.epoch); g_assert_cmpstr(jumped.entry_id, ==, entry);
    u3_tone(u3_song_tones[4], u3_song_tones[0]);
    click(window.root, "header-stop"); run_wait(&window, RADIOS_PLAYOUT_IDLE, 0, 0);
    g_free(entry); radios_audio_engine_unref(engine); json_node_free(prepared); json_node_free(configuration);
    for (guint i = 0; i < G_N_ELEMENTS(songs); ++i) json_node_free(songs[i]);
    window_close(&window);
}

/* The header's own "Program akışı" scope flow (a weekly schema assigned to a
 * player cluster): the picker lists and jumps within the scope runner. */
static void test_u3_scope_flow(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "U3 scope workspace ready");
    seed_programme(&window, NULL, NULL);
    U3Sources sources; u3_sources_new(&window, &sources);
    gint64 minute = G_TIME_SPAN_MINUTE, day = g_get_real_time() / G_TIME_SPAN_DAY * G_TIME_SPAN_DAY;
    gint64 start = MAX(day, g_get_real_time() / minute * minute - minute);
    guint length = (guint)MIN((gint64)3, (day + G_TIME_SPAN_DAY - start) / minute);
    JsonNode *programme = u3_weekly(sources.playlists, start, length, "U3 kapsam akışı");
    JsonNode *record = document_save("weekly_programme", "U3 kapsam programı", json_node_get_object(programme));
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players); gchar cluster[37]; GError *error = NULL;
    require_call(radios_player_topology_add_cluster(&topology, "U3 kapsam kümesi", cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 0, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster_automation(&topology, cluster,
        radios_json_string(json_node_get_object(record), "_id", ""), &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    U3Notices notices; u3_notices_attach(&window, &notices);
    g_assert_false(radios_broadcast_navigation_queue_playable(window.ctx));
    click(window.root, "header-play"); u3_notice_wait(&notices, "Program akışı yayında: U3 kapsam akışı");
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    RadioSAudioRunSnapshot run = u3_runner_wait(engine, 0, 0);
    RadioSViewContext *source = radios_player_workspace_source_context(window.ctx->players, 0); g_assert_nonnull(source);
    GArray *targets = u3_expected(source); g_assert_cmpuint(targets->len, ==, 3);
    JsonArray *choices = radios_playout_ui_playlist_choices(source); u3_choices_expect(choices, targets, run.epoch); json_array_unref(choices);
    g_assert_null(radios_broadcast_navigation_playlists(window.ctx));
    GtkWidget *menu = u3_menu_open(&window); u3_menu_expect_targets(menu, targets);
    U3Target *playing = &g_array_index(targets, U3Target, 0), *last = &g_array_index(targets, U3Target, targets->len - 1);
    u3_pick(menu, targets->len - 1);
    RadioSAudioRunSnapshot jumped = u3_runner_wait(engine, 0, last->index);
    g_assert_cmpuint(jumped.epoch, ==, run.epoch); g_assert_cmpstr(jumped.entry_id, ==, last->entry);
    u3_tone(u3_source_tone(last->first), u3_source_tone(playing->first));
    wait_until(u3_disabled, u3_choose(&window), "last scope playlist: ▾ inactive");
    click(window.root, "header-stop"); u3_notice_wait(&notices, "Program akışı durduruldu.");
    radios_audio_engine_unref(engine); u3_targets_free(targets);
    json_node_free(record); json_node_free(programme); u3_sources_free(&sources);
    u3_notices_detach(&window, &notices); window_close(&window);
}

static int next_playlist_run(void)
{
    g_test_add_func("/native-playout/header-next-playlist/local-queue", test_u3_local_queue);
    g_test_add_func("/native-playout/header-next-playlist/general-owner", test_u3_general_owner);
    g_test_add_func("/native-playout/header-next-playlist/general-not-owner", test_u3_general_not_owner);
    g_test_add_func("/native-playout/header-next-playlist/scope-flow", test_u3_scope_flow);
    return g_test_run();
}
int main(int argc, char **argv) { return radios_next_playlist_fixture_main(argc, argv); }
