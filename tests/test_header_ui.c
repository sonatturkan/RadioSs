#include "radios/header_status.h"
#include "radios/audio_ui.h"
#include "radios/broadcast_header.h"
#include "radios/broadcast_ui.h"
#include "radios/playout_ui.h"
#include "radios/routing_ui.h"
#include "radios/ui_preferences.h"

#include <glib/gstdio.h>
#include <string.h>

/* This component test controls read-only observations, while exercising real
 * GTK widgets and the real atomic preference store in isolated directories. */
static RadioSAudioUiSnapshot decks[RADIOS_AUDIO_NORMAL_DECKS];
static RadioSPlayoutSnapshot playout;
static RadioSPlayoutHeaderSnapshot programme;
static RadioSPlayoutDeckMetadata metadata;
static guint metadata_deck;
static gboolean have_playout, have_programme, have_metadata;
static RadioSBroadcastHeaderSnapshot broadcast;
static gboolean have_broadcast;
/* Internet stream observations behind the header light: routing page stream
 * blocks and the Yayın page stream profile (worker snapshots only). */
static RadioSRoutingOutputSnapshot routing_outputs;
static gboolean have_routing_outputs;
static RadioSAudioOutputSnapshot profile_stream;
static gboolean have_profile_stream;

gboolean
radios_ui_routing_output_snapshot(RadioSViewContext *ctx, RadioSRoutingOutputSnapshot *snapshot)
{
    (void)ctx;
    memset(snapshot, 0, sizeof *snapshot);
    if (have_routing_outputs) *snapshot = routing_outputs;
    return have_routing_outputs;
}

gboolean
radios_broadcast_ui_output_snapshot(RadioSViewContext *ctx, const gchar *kind, RadioSAudioOutputSnapshot *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (!have_profile_stream || g_strcmp0(kind, "stream")) return FALSE;
    *out = profile_stream; return TRUE;
}

gboolean
radios_broadcast_header_snapshot(RadioSViewContext *ctx, RadioSBroadcastHeaderSnapshot *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (have_broadcast) *out = broadcast;
    return have_broadcast;
}

gboolean
radios_audio_ui_get_snapshot(RadioSViewContext *ctx, guint deck, RadioSAudioUiSnapshot *out)
{
    (void)ctx;
    if (deck >= G_N_ELEMENTS(decks)) return FALSE;
    *out = decks[deck]; return TRUE;
}

gboolean
radios_playout_ui_get_snapshot(RadioSViewContext *ctx, RadioSPlayoutSnapshot *out)
{
    (void)ctx;
    *out = playout; return have_playout;
}

gboolean
radios_playout_ui_header_snapshot(RadioSViewContext *ctx, RadioSPlayoutHeaderSnapshot *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (have_programme) *out = programme;
    return have_programme;
}

gboolean
radios_playout_ui_deck_metadata(RadioSViewContext *ctx, guint deck, RadioSPlayoutDeckMetadata *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (have_metadata && deck == metadata_deck) *out = metadata;
    return have_metadata && deck == metadata_deck;
}


/* Deterministic observation clock and station timezone. GTK's monotonic
 * timers remain real; this executable never opens a data or audio service. */
static gint64 header_observation_time;
static const gchar *header_station_timezone = "UTC";
static guint header_station_token;
gint64 __wrap_g_get_real_time(void) { return header_observation_time; }
RadioSDataClient *radios_data_client_acquire(RadioSViewContext *ctx)
{ (void)ctx; return (RadioSDataClient *)(gpointer)&header_station_token; }
const gchar *radios_data_client_station_timezone(const RadioSDataClient *client)
{ g_assert_true(client == (RadioSDataClient *)(gpointer)&header_station_token); return header_station_timezone; }
void radios_data_client_unref(RadioSDataClient *client)
{ g_assert_true(client == (RadioSDataClient *)(gpointer)&header_station_token); }
static gint64 header_instant(const gchar *iso)
{
    GDateTime *time = g_date_time_new_from_iso8601(iso, NULL); g_assert_nonnull(time);
    gint64 result = g_date_time_to_unix(time) * G_TIME_SPAN_SECOND; g_date_time_unref(time); return result;
}

typedef struct {
    gchar *directory, *config_directory, *notice;
    RadioSConfig config;
    RadioSViewContext ctx;
    GtkWidget *window, *header, *next, *popover, *menu;
} Fixture;

static void
notice(gpointer data, const gchar *message)
{
    Fixture *fixture = data;
    g_free(fixture->notice); fixture->notice = g_strdup(message);
}

static GtkWidget *
find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (GTK_IS_MENU_BUTTON(root)) {
        GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(root)));
        GtkWidget *found = popover ? find(popover, name) : NULL;
        if (found) return found;
    }
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find(item->data, name);
    g_list_free(children); return found;
}

static const gchar *
value(Fixture *fixture, const gchar *name)
{
    GtkWidget *label = find(fixture->header, name);
    g_assert_true(GTK_IS_LABEL(label));
    return gtk_label_get_text(GTK_LABEL(label));
}

static void
setup(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GError *error = NULL;
    fixture->directory = g_dir_make_tmp("radios-header-test-XXXXXX", &error);
    g_assert_no_error(error); g_assert_nonnull(fixture->directory);
    fixture->config_directory = g_build_filename(fixture->directory, "config", NULL);
    g_assert_cmpint(g_mkdir(fixture->config_directory, 0700), ==, 0);
    fixture->config.project_root = fixture->directory;
    fixture->ctx.config = &fixture->config;
    fixture->ctx.notify = notice; fixture->ctx.notify_data = fixture;
    fixture->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(fixture->window);
    fixture->ctx.parent = GTK_WINDOW(fixture->window);
    fixture->header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    fixture->next = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_style_context_add_class(gtk_widget_get_style_context(fixture->next), "header-next");
    const gchar *const legacy_texts[] = {"GERÇEK SIRADAKİ", "GEÇİŞE KALAN", "—", "Reklama kalan  —:—", "—:—"};
    const gchar *const legacy_names[] = {"legacy-next-caption", "legacy-transition-caption", "legacy-next-title", "legacy-next-advert", "legacy-countdown"};
    const gchar *const legacy_styles[] = {"broadcast-label", "broadcast-label", "header-next-title", "header-next-advert", "header-countdown"};
    for (guint i = 0; i < G_N_ELEMENTS(legacy_texts); ++i) {
        GtkWidget *label = gtk_label_new(legacy_texts[i]);
        gtk_widget_set_name(label, legacy_names[i]);
        gtk_style_context_add_class(gtk_widget_get_style_context(label), legacy_styles[i]);
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_box_pack_start(GTK_BOX(fixture->next), label, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(fixture->header), fixture->next, TRUE, TRUE, 0);
    GtkWidget *legacy_programme = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_name(legacy_programme, "header-legacy-programme");
    gtk_style_context_add_class(gtk_widget_get_style_context(legacy_programme), "header-programme");
    gtk_box_pack_start(GTK_BOX(legacy_programme), gtk_label_new("MEVCUT PROGRAM —"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fixture->header), legacy_programme, FALSE, FALSE, 0);
    GtkWidget *gear = gtk_menu_button_new();
    fixture->popover = gtk_popover_new(gear); gtk_widget_set_name(fixture->popover, "header-settings-menu");
    fixture->menu = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_container_add(GTK_CONTAINER(fixture->popover), fixture->menu);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(gear), fixture->popover);
    gtk_box_pack_start(GTK_BOX(fixture->header), gear, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(fixture->window), fixture->header);
    memset(decks, 0, sizeof decks); memset(&playout, 0, sizeof playout);
    memset(&programme, 0, sizeof programme); memset(&metadata, 0, sizeof metadata);
    have_playout = have_programme = have_metadata = FALSE;
    have_broadcast = FALSE; memset(&broadcast, 0, sizeof broadcast);
    have_routing_outputs = have_profile_stream = FALSE;
    memset(&routing_outputs, 0, sizeof routing_outputs); memset(&profile_stream, 0, sizeof profile_stream);
    metadata_deck = 0;
    header_observation_time = header_instant("2026-10-13T20:25:30Z");
    header_station_timezone = "UTC";
}

static void
teardown(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gtk_widget_destroy(fixture->window); g_object_unref(fixture->window);
    while (g_main_context_iteration(NULL, FALSE)) {}
    gchar *path = g_build_filename(fixture->config_directory, "ui-preferences.ini", NULL);
    g_remove(path); g_free(path);
    g_rmdir(fixture->config_directory); g_rmdir(fixture->directory);
    g_free(fixture->config_directory); g_free(fixture->directory); g_free(fixture->notice);
}

static void
observe_deck(guint deck, RadioSAudioUiState state, gboolean main)
{
    decks[deck].state = state; decks[deck].main_enabled = main; decks[deck].output_ready = TRUE;
    decks[deck].duration_ms = 180000; decks[deck].position_ms = 30000; decks[deck].tempo_ratio = 1;
    g_strlcpy(decks[deck].title, "Çalan parça", sizeof decks[deck].title);
    g_strlcpy(decks[deck].artist, "Sanatçı", sizeof decks[deck].artist);
}

static void
unknown_and_main(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_header_status_bind(fixture->header, &fixture->ctx);
    gtk_widget_show_all(fixture->header);
    g_assert_cmpint(gtk_box_get_spacing(GTK_BOX(fixture->next)), ==, 8);
    g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(fixture->next), "header-status-bound"));
    const gchar *const legacy_names[] = {"legacy-next-caption", "legacy-transition-caption", "legacy-next-title", "legacy-next-advert", "legacy-countdown"};
    for (guint i = 0; i < G_N_ELEMENTS(legacy_names); ++i) {
        GtkLabel *label = GTK_LABEL(find(fixture->header, legacy_names[i]));
        g_assert_false(gtk_label_get_line_wrap(label));
        g_assert_true(gtk_label_get_single_line_mode(label));
        if (i == 2 || i == 3) g_assert_cmpint(gtk_label_get_ellipsize(label), ==, PANGO_ELLIPSIZE_END);
    }
    const gchar *const new_fields[] = {"header-status-programme", "header-status-music", "header-status-remaining"};
    for (guint i = 0; i < G_N_ELEMENTS(new_fields); ++i) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(find(fixture->header, new_fields[i])));
        GtkLabel *caption = GTK_LABEL(children->data);
        g_assert_true(gtk_label_get_line_wrap(caption));
        g_assert_cmpint(gtk_label_get_line_wrap_mode(caption), ==, PANGO_WRAP_WORD);
        g_list_free(children);
    }
    g_assert_false(gtk_widget_get_visible(find(fixture->header, "header-legacy-programme")));
    g_assert_cmpstr(value(fixture, "header-status-programme-value"), ==, "—");
    g_assert_cmpstr(value(fixture, "header-status-music-value"), ==, "—");
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "—");
    observe_deck(0, RADIOS_AUDIO_UI_PLAYING, FALSE); /* CUE never appears. */
    observe_deck(1, RADIOS_AUDIO_UI_READY, TRUE); /* Prepared is not playing. */
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-music-value"), ==, "—");
    observe_deck(2, RADIOS_AUDIO_UI_PLAYING, TRUE);
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-music-value"), ==, "Sanatçı — Çalan parça");
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "2 dk 30 sn");
    decks[2].state = RADIOS_AUDIO_UI_PAUSED;
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-music-value"), ==, "Sanatçı — Çalan parça · Duraklatıldı");
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "2 dk 30 sn");
    decks[2].state = RADIOS_AUDIO_UI_EOF;
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-music-value"), ==, "—");
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "—");
}

static void
accepted_programme_and_cue(Fixture *fixture, gconstpointer data)
{
    (void)data;
    programme.programme_active = TRUE; g_strlcpy(programme.programme_title, "Gece yayını", sizeof programme.programme_title);
    have_programme = TRUE; have_playout = TRUE; have_metadata = TRUE;
    metadata_deck = 2;
    playout.deck = 2; playout.state = RADIOS_PLAYOUT_PLAYING; playout.duration_ms = 90000; playout.position_ms = 30000;
    g_strlcpy(metadata.title, "Planın gerçek parçası", sizeof metadata.title);
    g_strlcpy(metadata.artist, "Yayın sanatçısı", sizeof metadata.artist);
    observe_deck(0, RADIOS_AUDIO_UI_PLAYING, TRUE);
    observe_deck(2, RADIOS_AUDIO_UI_PLAYING, TRUE); decks[2].tempo_ratio = 2;
    radios_header_status_bind(fixture->header, &fixture->ctx);
    g_assert_cmpstr(value(fixture, "header-status-programme-value"), ==, "Gece yayını");
    g_assert_cmpstr(value(fixture, "header-status-music-value"), ==, "Yayın sanatçısı — Planın gerçek parçası · Çoklu MAIN");
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "0 dk 30 sn");
    playout.position_ms = 91000;
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "0 dk 00 sn");
    programme.programme_active = FALSE;
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-programme-value"), ==, "—");
    have_playout = FALSE; decks[2].duration_ms = 0; decks[0].state = RADIOS_AUDIO_UI_EMPTY;
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "—");
}

static void
playing_precedes_paused_and_stale_plan(Fixture *fixture, gconstpointer data)
{
    (void)data;
    have_playout = TRUE;
    playout.deck = 0; playout.state = RADIOS_PLAYOUT_PAUSED; playout.duration_ms = 10000; playout.position_ms = 9000;
    observe_deck(0, RADIOS_AUDIO_UI_PAUSED, TRUE);
    observe_deck(1, RADIOS_AUDIO_UI_PLAYING, TRUE);
    g_strlcpy(decks[1].title, "Diğer yayındaki parça", sizeof decks[1].title);
    radios_header_status_bind(fixture->header, &fixture->ctx);
    g_assert_cmpstr(value(fixture, "header-status-music-value"), ==, "Sanatçı — Diğer yayındaki parça");
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "2 dk 30 sn");
    /* A cached plan references this deck, but its owned media no longer match.
     * Actual new media duration must be used until coherent metadata arrive. */
    playout.deck = 1; playout.state = RADIOS_PLAYOUT_PLAYING;
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "2 dk 30 sn");
    decks[1].cue_out_ms = 60000; decks[1].tempo_ratio = 2;
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "0 dk 15 sn");
}

static void
authoritative_online_and_no_main(Fixture *fixture, gconstpointer data)
{
    (void)data;
    /* The direct cached snapshot could still look routed while the genuine
     * engine driver is CUE. Authoritative no-source must suppress it. */
    observe_deck(0, RADIOS_AUDIO_UI_PLAYING, TRUE);
    have_broadcast = TRUE;
    radios_header_status_bind(fixture->header, &fixture->ctx);
    g_assert_cmpstr(value(fixture, "header-status-music-value"), ==, "—");
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "—");
    broadcast.programme_active = TRUE; broadcast.current = TRUE; broadcast.active_sources = 1;
    g_strlcpy(broadcast.programme_title, "Kabul edilmiş çevrimiçi program", sizeof broadcast.programme_title);
    g_strlcpy(broadcast.title, "Çevrimiçi gerçek parça", sizeof broadcast.title);
    g_strlcpy(broadcast.artist, "Çevrimiçi sanatçı", sizeof broadcast.artist);
    broadcast.duration_ms = 125500; broadcast.position_ms = 65000; broadcast.tempo_ratio = 1;
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-programme-value"), ==, "Kabul edilmiş çevrimiçi program");
    g_assert_cmpstr(value(fixture, "header-status-music-value"), ==, "Çevrimiçi sanatçı — Çevrimiçi gerçek parça");
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "1 dk 01 sn");
    broadcast.paused = TRUE;
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-music-value"), ==, "Çevrimiçi sanatçı — Çevrimiçi gerçek parça · Duraklatıldı");
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "1 dk 01 sn");
    broadcast.duration_ms = 0; broadcast.active_sources = 2;
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "—");
    g_assert_cmpstr(value(fixture, "header-status-music-value"), ==, "Çevrimiçi sanatçı — Çevrimiçi gerçek parça · Duraklatıldı · Çoklu MAIN");
    broadcast.current = FALSE; broadcast.programme_active = FALSE;
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-programme-value"), ==, "—");
    g_assert_cmpstr(value(fixture, "header-status-music-value"), ==, "—");
}

static void
preferences_preserved(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_header_status_bind(fixture->header, &fixture->ctx);
    RadioSUIPreferences latest = { .text_size = 2, .default_profile = 8, .last_profile = 7, .header_hidden_fields = 2,
        .content_scale_percent = 125, .mixer_scale_percent = 100, .effector_scale_percent = 100,
        .mixer_height_percent = 110, .effector_height_percent = 90 };
    GError *error = NULL;
    g_assert_true(radios_ui_preferences_save(fixture->directory, &latest, &error)); g_assert_no_error(error);
    GtkWidget *toggle = find(fixture->header, "header-show-programme"); g_assert_true(GTK_IS_CHECK_BUTTON(toggle));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), FALSE);
    RadioSUIPreferences saved;
    g_assert_true(radios_ui_preferences_load(fixture->directory, &saved, &error)); g_assert_no_error(error);
    g_assert_cmpuint(saved.text_size, ==, 2); g_assert_cmpuint(saved.default_profile, ==, 8); g_assert_cmpuint(saved.last_profile, ==, 7);
    g_assert_cmpuint(saved.header_hidden_fields, ==, 3);
    g_assert_cmpuint(saved.content_scale_percent, ==, 125);
    g_assert_cmpuint(saved.mixer_scale_percent, ==, 100); g_assert_cmpuint(saved.effector_scale_percent, ==, 100);
    g_assert_cmpuint(saved.mixer_height_percent, ==, 110); g_assert_cmpuint(saved.effector_height_percent, ==, 90);
    g_assert_false(gtk_widget_get_visible(find(fixture->header, "header-status-programme")));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(fixture->header, "header-show-music"))));
    GtkWidget *other = radios_header_status_settings_new(fixture->header); g_object_ref_sink(other);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(other, "header-show-remaining")), FALSE);
    g_assert_true(radios_ui_preferences_load(fixture->directory, &saved, &error)); g_assert_no_error(error);
    g_assert_cmpuint(saved.header_hidden_fields, ==, 7);
    g_assert_cmpuint(saved.mixer_scale_percent, ==, 100); g_assert_cmpuint(saved.effector_scale_percent, ==, 100);
    g_assert_cmpuint(saved.mixer_height_percent, ==, 110); g_assert_cmpuint(saved.effector_height_percent, ==, 90);
    gtk_widget_show_all(fixture->window);
    g_assert_false(gtk_widget_get_visible(find(fixture->header, "header-status-panel")));
    gtk_widget_destroy(other); g_object_unref(other);
    gtk_widget_destroy(fixture->header);
    fixture->header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(fixture->header), "header-next");
    gtk_container_add(GTK_CONTAINER(fixture->window), fixture->header);
    radios_header_status_bind(fixture->header, &fixture->ctx);
    gtk_widget_show_all(fixture->window);
    g_assert_false(gtk_widget_get_visible(find(fixture->header, "header-status-panel")));
    GtkWidget *reopened = radios_header_status_preferences_new(&fixture->ctx); g_object_ref_sink(reopened);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(reopened, "header-show-programme"))));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(reopened, "header-show-music"))));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(reopened, "header-show-remaining"))));
    gtk_widget_destroy(reopened); g_object_unref(reopened);
}

static void
save_failure_rollback(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_header_status_bind(fixture->header, &fixture->ctx);
    gchar *path = g_build_filename(fixture->config_directory, "ui-preferences.ini", NULL);
    g_assert_true(g_file_set_contents(path, "invalid preference file", -1, NULL));
    GtkWidget *button = find(fixture->header, "header-show-programme");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)));
    g_assert_true(gtk_widget_get_visible(find(fixture->header, "header-status-programme")));
    g_assert_nonnull(fixture->notice);
    gchar *contents = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "invalid preference file"); g_free(contents); g_free(path);
}

static void
lifetime_and_rebind(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_header_status_bind(fixture->header, &fixture->ctx);
    GtkWidget *panel = find(fixture->header, "header-status-panel");
    radios_header_status_bind(fixture->header, &fixture->ctx);
    g_assert_true(panel == find(fixture->header, "header-status-panel"));
    g_assert_true(g_object_get_data(G_OBJECT(fixture->window), "radios-header-status-root") == fixture->header);
    GtkWidget *settings = radios_header_status_settings_new(fixture->header); g_object_ref_sink(settings);
    gtk_widget_destroy(fixture->header);
    g_assert_null(g_object_get_data(G_OBJECT(fixture->window), "radios-header-status-root"));
    g_assert_false(gtk_widget_get_sensitive(find(settings, "header-show-programme")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(settings, "header-show-programme")), FALSE);
    while (g_main_context_iteration(NULL, FALSE)) {}
    gtk_widget_destroy(settings); g_object_unref(settings);
}

/* R8.2: the stream light shows the worker's real state. Its colour changes
 * only with that state; connecting and error blink on the monotonic clock,
 * connected and off stay steady however often the strip refreshes. */
typedef struct { gboolean dim, steady; } LightSample;
static LightSample
light_watch(Fixture *fixture, const gchar *expected_class, const gchar *expected_text, guint milliseconds)
{
    static const gchar *const classes[] = {"header-status-light-off", "header-status-light-connecting",
        "header-status-light-on", "header-status-light-error"};
    GtkWidget *light = find(fixture->header, "header-status-stream-light");
    GtkStyleContext *style = gtk_widget_get_style_context(light);
    LightSample sample = {FALSE, FALSE};
    gint64 until = g_get_monotonic_time() + (gint64)milliseconds * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < until) {
        radios_header_status_refresh(fixture->header);
        for (guint i = 0; i < G_N_ELEMENTS(classes); ++i)
            g_assert_cmpint(gtk_style_context_has_class(style, classes[i]), ==, g_str_equal(classes[i], expected_class));
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(light)), ==, expected_text);
        if (gtk_style_context_has_class(style, "header-status-light-dim")) sample.dim = TRUE; else sample.steady = TRUE;
        g_usleep(40000);
    }
    return sample;
}

static void
stream_light_real_state(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_header_status_bind(fixture->header, &fixture->ctx);
    gtk_widget_show_all(fixture->window);
    GtkWidget *light = find(fixture->header, "header-status-stream-light");
    g_assert_true(GTK_IS_LABEL(light));
    g_assert_true(gtk_widget_get_parent(light) == gtk_widget_get_parent(find(fixture->header, "header-status-remaining-value")));
    LightSample sample = light_watch(fixture, "header-status-light-off", "● Yayın kapalı", 300);
    g_assert_false(sample.dim);
    gchar *tooltip = gtk_widget_get_tooltip_text(light);
    g_assert_cmpstr(tooltip, ==, "İnternet yayını hazır değil"); g_free(tooltip);
    /* A started encoder still waiting for the server answer: yellow, blinking. */
    have_routing_outputs = TRUE; routing_outputs.available = routing_outputs.running = TRUE;
    routing_outputs.streams = 1; routing_outputs.pending = 1;
    sample = light_watch(fixture, "header-status-light-connecting", "● Bağlanıyor…", 1200);
    g_assert_true(sample.dim); g_assert_true(sample.steady);
    /* The server accepted the source: steady green. */
    routing_outputs.pending = 0; routing_outputs.active = 1;
    sample = light_watch(fixture, "header-status-light-on", "● Yayın bağlı", 1200);
    g_assert_false(sample.dim);
    tooltip = gtk_widget_get_tooltip_text(light);
    g_assert_true(g_str_has_prefix(tooltip, "İnternet yayını: 1 bağlı · sunucu kabul etti")); g_free(tooltip);
    /* A dropped stream waiting for its next attempt: red, blinking, with its reconnect count. */
    routing_outputs.active = 0; routing_outputs.failed = 1; routing_outputs.reconnects = 2;
    g_strlcpy(routing_outputs.error, "Sunucu bağlantıyı kapattı", sizeof routing_outputs.error);
    sample = light_watch(fixture, "header-status-light-error", "● Yayın hatası · 2", 1200);
    g_assert_true(sample.dim); g_assert_true(sample.steady);
    tooltip = gtk_widget_get_tooltip_text(light);
    g_assert_nonnull(strstr(tooltip, "Sunucu bağlantıyı kapattı")); g_free(tooltip);
    /* The Yayın page profile counts as connected only after the server's answer. */
    have_routing_outputs = FALSE; have_profile_stream = TRUE;
    profile_stream.state = RADIOS_AUDIO_OUTPUT_ACTIVE; profile_stream.response_confirmed = FALSE;
    light_watch(fixture, "header-status-light-connecting", "● Bağlanıyor…", 200);
    profile_stream.response_confirmed = TRUE;
    sample = light_watch(fixture, "header-status-light-on", "● Yayın bağlı", 700);
    g_assert_false(sample.dim);
    profile_stream.state = RADIOS_AUDIO_OUTPUT_STOPPED; profile_stream.response_confirmed = FALSE;
    sample = light_watch(fixture, "header-status-light-off", "● Yayın kapalı", 300);
    g_assert_false(sample.dim);
    tooltip = gtk_widget_get_tooltip_text(light);
    g_assert_cmpstr(tooltip, ==, "İnternet yayını başlatılmadı"); g_free(tooltip);
}

static void
stylesheet(void)
{
    GtkCssProvider *provider = gtk_css_provider_new(); GError *error = NULL;
    g_assert_true(gtk_css_provider_load_from_path(provider, "assets/themes/header-status.css", &error));
    g_assert_no_error(error); g_object_unref(provider);
}


static void programme_playlist_end_times_station_timezone(Fixture *fixture, gconstpointer data)
{
    (void)data; have_broadcast = TRUE; broadcast.programme_active = broadcast.current = TRUE;
    broadcast.active_sources = 1; broadcast.duration_ms = 180000; broadcast.position_ms = 30000; broadcast.tempo_ratio = 1;
    g_strlcpy(broadcast.programme_title, "Kabul edilmiş gece programı", sizeof broadcast.programme_title);
    g_strlcpy(broadcast.playlist_title, "Kaydedilmiş gece playlisti", sizeof broadcast.playlist_title);
    g_strlcpy(broadcast.title, "Gece parçası", sizeof broadcast.title);
    broadcast.programme_end_utc_us = header_instant("2026-10-13T20:30:00Z");
    broadcast.playlist_end_utc_us = header_instant("2026-10-13T20:45:00Z");
    broadcast.track_remaining_us = 90 * G_TIME_SPAN_SECOND;
    header_station_timezone = "Europe/Istanbul";
    radios_header_status_bind(fixture->header, &fixture->ctx);
    g_assert_cmpstr(value(fixture, "header-status-programme-value"), ==, "Kabul edilmiş gece programı");
    g_assert_cmpstr(value(fixture, "header-status-playlist-value"), ==, "Playlist · Kaydedilmiş gece playlisti");
    g_assert_cmpstr(value(fixture, "header-status-programme-end-value"), ==, "Program bitişi · 23:30:00");
    g_assert_cmpstr(value(fixture, "header-status-playlist-end-value"), ==, "Playlist bitişi · 23:45:00");
    g_assert_cmpstr(value(fixture, "header-status-track-end-value"), ==, "Parça bitişi · 23:27:00");
    g_assert_cmpstr(value(fixture, "header-status-music-value"), ==, "Gece parçası");
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "1 dk 30 sn");
    GtkWidget *program_end = find(fixture->header, "header-status-programme-end-value");
    gchar *tooltip = gtk_widget_get_tooltip_text(program_end); g_assert_nonnull(tooltip);
    g_assert_nonnull(strstr(tooltip, "13.10.2026 23:30:00")); g_assert_nonnull(strstr(tooltip, "+03")); g_free(tooltip);
    /* A midnight boundary must keep its date in the tooltip, while the visible
     * compact clock uses the verified station zone instead of workstation TZ. */
    broadcast.programme_end_utc_us = header_instant("2026-10-13T23:30:00Z");
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-programme-end-value"), ==, "Program bitişi · 02:30:00");
    tooltip = gtk_widget_get_tooltip_text(program_end); g_assert_nonnull(tooltip);
    g_assert_nonnull(strstr(tooltip, "14.10.2026 02:30:00")); g_free(tooltip);
    header_station_timezone = "UTC"; radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-programme-end-value"), ==, "Program bitişi · 23:30:00");
    g_assert_cmpstr(value(fixture, "header-status-playlist-end-value"), ==, "Playlist bitişi · 20:45:00");
    g_assert_cmpstr(value(fixture, "header-status-track-end-value"), ==, "Parça bitişi · 20:27:00");
}

static void end_times_unknown_paused_wait_and_clear(Fixture *fixture, gconstpointer data)
{
    (void)data; have_broadcast = TRUE; broadcast.programme_active = broadcast.current = TRUE;
    g_strlcpy(broadcast.programme_title, "Elle geçiş bekleyen program", sizeof broadcast.programme_title);
    g_strlcpy(broadcast.playlist_title, "Bekleyen playlist", sizeof broadcast.playlist_title);
    g_strlcpy(broadcast.title, "Bekleme sırasında tutulan parça", sizeof broadcast.title);
    broadcast.duration_ms = 180000; broadcast.position_ms = 30000; broadcast.tempo_ratio = 1;
    broadcast.programme_remaining_us = 120 * G_TIME_SPAN_SECOND;
    broadcast.playlist_remaining_us = 90 * G_TIME_SPAN_SECOND;
    radios_header_status_bind(fixture->header, &fixture->ctx);
    g_assert_cmpstr(value(fixture, "header-status-programme-end-value"), ==, "Program bitişi · —");
    g_assert_cmpstr(value(fixture, "header-status-playlist-end-value"), ==, "Playlist bitişi · —");
    g_assert_cmpstr(value(fixture, "header-status-track-end-value"), ==, "Parça bitişi · 20:28:00");
    broadcast.waiting = TRUE; radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-music-value"), !=, "—");
    g_assert_cmpstr(value(fixture, "header-status-remaining-value"), ==, "2 dk 30 sn");
    g_assert_cmpstr(value(fixture, "header-status-track-end-value"), ==, "Parça bitişi · —");
    broadcast.waiting = FALSE; radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-track-end-value"), ==, "Parça bitişi · 20:28:00");
    broadcast.paused = TRUE; radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-programme-end-value"), ==, "Program bitişi · —");
    g_assert_cmpstr(value(fixture, "header-status-playlist-end-value"), ==, "Playlist bitişi · —");
    g_assert_cmpstr(value(fixture, "header-status-track-end-value"), ==, "Parça bitişi · Duraklatıldı");
    /* A genuine schedule deadline stays authoritative while paused; it is
     * distinct from a moving end estimate that must remain unknown. */
    broadcast.programme_end_utc_us = header_instant("2026-10-13T20:30:00Z");
    radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-programme-end-value"), ==, "Program bitişi · 20:30:00");
    broadcast.current = FALSE; broadcast.programme_end_utc_us = 0; radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-programme-value"), ==, "Elle geçiş bekleyen program");
    g_assert_cmpstr(value(fixture, "header-status-playlist-value"), ==, "Playlist · Bekleyen playlist");
    g_assert_cmpstr(value(fixture, "header-status-track-end-value"), ==, "Parça bitişi · —");
    memset(&broadcast, 0, sizeof broadcast); radios_header_status_refresh(fixture->header);
    g_assert_cmpstr(value(fixture, "header-status-programme-value"), ==, "—");
    g_assert_cmpstr(value(fixture, "header-status-playlist-value"), ==, "Playlist · —");
    g_assert_cmpstr(value(fixture, "header-status-programme-end-value"), ==, "Program bitişi · —");
    g_assert_cmpstr(value(fixture, "header-status-playlist-end-value"), ==, "Playlist bitişi · —");
    g_assert_cmpstr(value(fixture, "header-status-track-end-value"), ==, "Parça bitişi · —");
    g_assert_null(gtk_widget_get_tooltip_text(find(fixture->header, "header-status-programme-end-value")));
    g_assert_null(gtk_widget_get_tooltip_text(find(fixture->header, "header-status-playlist-end-value")));
    g_assert_null(gtk_widget_get_tooltip_text(find(fixture->header, "header-status-track-end-value")));
}

static void playlist_without_programme_and_field_preferences(Fixture *fixture, gconstpointer data)
{
    (void)data; have_programme = TRUE;
    g_strlcpy(programme.playlist_title, "Normal kart playlisti", sizeof programme.playlist_title);
    programme.playlist_end_utc_us = header_instant("2026-10-13T20:45:00Z");
    observe_deck(0, RADIOS_AUDIO_UI_PLAYING, TRUE);
    radios_header_status_bind(fixture->header, &fixture->ctx); gtk_widget_show_all(fixture->window);
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_assert_cmpstr(value(fixture, "header-status-programme-value"), ==, "—");
    g_assert_cmpstr(value(fixture, "header-status-playlist-value"), ==, "Playlist · Normal kart playlisti");
    g_assert_cmpstr(value(fixture, "header-status-programme-end-value"), ==, "Program bitişi · —");
    g_assert_cmpstr(value(fixture, "header-status-playlist-end-value"), ==, "Playlist bitişi · 20:45:00");
    GtkWidget *playlist = find(fixture->header, "header-status-playlist-value");
    GtkWidget *playlist_end = find(fixture->header, "header-status-playlist-end-value");
    GtkWidget *track_end = find(fixture->header, "header-status-track-end-value");
    g_assert_true(gtk_widget_get_mapped(playlist)); g_assert_true(gtk_widget_get_mapped(track_end));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(fixture->header, "header-show-programme")), FALSE);
    g_assert_false(gtk_widget_get_mapped(playlist)); g_assert_false(gtk_widget_get_mapped(playlist_end));
    g_assert_true(gtk_widget_get_mapped(track_end));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(fixture->header, "header-show-programme")), TRUE);
    g_assert_true(gtk_widget_get_mapped(playlist)); g_assert_true(gtk_widget_get_mapped(playlist_end));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(fixture->header, "header-show-music")), FALSE);
    g_assert_false(gtk_widget_get_mapped(track_end)); g_assert_true(gtk_widget_get_mapped(playlist));
}


int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check(&argc, &argv)) return 77;
    g_test_add("/header/unknown-main-pause-stop", Fixture, NULL, setup, unknown_and_main, teardown);
    g_test_add("/header/accepted-programme-cue-tempo", Fixture, NULL, setup, accepted_programme_and_cue, teardown);
    g_test_add("/header/playing-precedes-paused-stale-plan", Fixture, NULL, setup, playing_precedes_paused_and_stale_plan, teardown);
    g_test_add("/header/authoritative-online-no-main", Fixture, NULL, setup, authoritative_online_and_no_main, teardown);
    g_test_add("/header/preferences-preservation", Fixture, NULL, setup, preferences_preserved, teardown);
    g_test_add("/header/save-failure-rollback", Fixture, NULL, setup, save_failure_rollback, teardown);
    g_test_add("/header/lifetime-idempotent-bind", Fixture, NULL, setup, lifetime_and_rebind, teardown);
    g_test_add("/header/programme-playlist-track-end-station-timezone", Fixture, NULL, setup, programme_playlist_end_times_station_timezone, teardown);
    g_test_add("/header/end-unknown-pause-wait-clear", Fixture, NULL, setup, end_times_unknown_paused_wait_and_clear, teardown);
    g_test_add("/header/playlist-only-field-preferences", Fixture, NULL, setup, playlist_without_programme_and_field_preferences, teardown);
    g_test_add("/header/stream-light-real-state", Fixture, NULL, setup, stream_light_real_state, teardown);
    g_test_add_func("/header/stylesheet", stylesheet);
    return g_test_run();
}
