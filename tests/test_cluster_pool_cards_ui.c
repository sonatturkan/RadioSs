/* Kullanıcı isteği U1 (18.10.2026): a scope of four players shows four loaded
 * cards — the playing item and the next three, each on its own player — and
 * after a transition the freed card takes the following item. Real
 * authenticated media, accepted runner leases and isolated MAIN PCM. */
#define main radios_cluster_pool_cards_legacy_fixture_main
#include "test_playout_ui.c"
#undef main

static GtkWidget *card_label(Window *window, guint deck, const gchar *suffix)
{
    gchar *name = g_strdup_printf("deck-%u-%s", deck, suffix);
    GtkWidget *label = find(window->root, name); g_free(name);
    g_assert_true(GTK_IS_LABEL(label)); return label;
}
typedef struct { GtkWidget *status, *title; const gchar *status_text, *title_text; } CardCondition;
static gboolean card_matches(gpointer value)
{
    CardCondition *card = value;
    return g_str_equal(gtk_label_get_text(GTK_LABEL(card->status)), card->status_text) &&
        g_str_equal(gtk_label_get_text(GTK_LABEL(card->title)), card->title_text);
}
static void card_wait(Window *window, guint deck, const gchar *status, const gchar *title)
{
    CardCondition card = {card_label(window, deck, "content-status"), card_label(window, deck, "title"), status, title};
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!card_matches(&card) && g_get_monotonic_time() < deadline) pump();
    if (!card_matches(&card))
        g_error("Player %u card expected '%s' / '%s', actual '%s' / '%s'", deck + 1, status, title,
            gtk_label_get_text(GTK_LABEL(card.status)), gtk_label_get_text(GTK_LABEL(card.title)));
}
static JsonNode *pool_programme(JsonNode **details, guint count)
{
    JsonNode *result = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(result, object);
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d"); g_date_time_unref(now);
    json_object_set_string_member(object, "title", "Dört oynatıcılı küme"); json_object_set_string_member(object, "date", date); g_free(date);
    json_object_set_string_member(object, "note", "");
    JsonArray *events = json_array_new();
    for (guint i = 0; i < count; ++i) {
        JsonObject *detail = json_node_get_object(details[i]), *media = json_object_get_object_member(detail, "media");
        JsonObject *version = json_object_get_object_member(detail, "version"), *event = json_object_new(); gchar *id = g_uuid_string_random();
        json_object_set_string_member(event, "id", id); g_free(id);
        const gchar *keys[] = {"title", "artist", "album", "kind"};
        for (guint j = 0; j < G_N_ELEMENTS(keys); ++j) json_object_set_string_member(event, keys[j], radios_json_string(media, keys[j], ""));
        json_object_set_string_member(event, "start_time", ""); json_object_set_int_member(event, "duration_ms", radios_json_int(version, "duration_ms", 0));
        json_object_set_string_member(event, "version_label", "Yerel taslak"); json_object_set_string_member(event, "primary_file", "");
        json_object_set_string_member(event, "backup_file", ""); json_object_set_string_member(event, "note", ""); bind(event, detail);
        json_array_add_object_element(events, event);
    }
    json_object_set_array_member(object, "events", events); return result;
}
static void test_four_player_pool_cards_pcm(void)
{
    enum { COUNT = 6, POOL = 4 };
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "pool scope definitions");
    ScopeReady ready = {window.ctx->players, 0}; wait_until(scope_workspace_execution_ready, &ready, "pool scope history");
    RadioSViewContext *scope = radios_player_workspace_source_context(window.ctx->players, 0); g_assert_nonnull(scope);
    JsonNode *details[COUNT]; gchar *titles[COUNT];
    for (guint i = 0; i < COUNT; ++i) {
        titles[i] = g_strdup_printf("Küme parçası %u", i + 1);
        details[i] = media_import(&window, titles[i], 500 + 200 * i, .2, "music");
    }
    JsonNode *programme = pool_programme(details, COUNT), *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "programme"); json_object_set_member(object, "config", deep_copy(programme));
    JsonNode *prepared = request("/v1/workflows/desktop-playout/prepare", body); json_node_free(body);
    const guint decks[POOL] = {0, 1, 2, 3}; GError *error = NULL;
    require_call(radios_playout_ui_start_prepared_scope(scope, window.root, json_node_get_object(prepared), decks,
        POOL, RADIOS_AUTODJ_TRANSITIONS, &error), &error);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    /* Every selected player is loaded: the playing song and the next three. */
    card_wait(&window, 0, "Çalıyor · Müzik", titles[0]);
    for (guint d = 1; d < POOL; ++d) card_wait(&window, d, "Hazır · Müzik", titles[d]);
    for (guint d = 0; d < POOL; ++d) {
        RadioSPlayoutDeckMetadata metadata;
        g_assert_true(radios_playout_ui_deck_metadata(scope, d, &metadata)); g_assert_cmpstr(metadata.title, ==, titles[d]);
        g_assert_cmpstr(metadata.next_title, ==, titles[d + 1]);
    }
    goffset at = capture_offset("MAIN"); run_for(150);
    g_assert_cmpfloat(tone_energy("MAIN", at, 500), >, .06);
    /* After the first transition player 1 holds song 5, player 2 plays. */
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 1, 4800);
    card_wait(&window, 1, "Çalıyor · Müzik", titles[1]);
    card_wait(&window, 0, "Hazır · Müzik", titles[4]);
    for (guint d = 2; d < POOL; ++d) card_wait(&window, d, "Hazır · Müzik", titles[d]);
    at = capture_offset("MAIN"); run_for(150);
    g_assert_cmpfloat(tone_energy("MAIN", at, 700), >, .06);
    RadioSAudioRunSnapshot run; radios_audio_engine_player_runner_snapshot(engine, 0, &run);
    for (guint d = 0; d < POOL; ++d) g_assert_true(run.owned[d]);
    require_call(radios_playout_ui_action(scope, RADIOS_PLAYOUT_STOP, &error), &error); run_for(250);
    for (guint d = 0; d < POOL; ++d) {
        GtkWidget *status = card_label(&window, d, "content-status"); gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        while (*gtk_label_get_text(GTK_LABEL(status)) && g_get_monotonic_time() < deadline) pump();
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(status)), ==, "");
    }
    radios_audio_engine_unref(engine); json_node_free(prepared); json_node_free(programme);
    for (guint i = 0; i < COUNT; ++i) { json_node_free(details[i]); g_free(titles[i]); }
    window_close(&window);
    g_test_message("Four-player scope: playing card plus three ready cards in pool order, freed card reloads with the fifth song after the transition; real MAIN tones verified.");
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"); cue_sink = g_getenv("RADIOS_TEST_CUE_SINK"); monitor_sink = g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink); g_assert_nonnull(cue_sink); g_assert_nonnull(monitor_sink);
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.NativeClusterPoolCards", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-playout/cluster-pool-cards-four-players-PCM", test_four_player_pool_cards_pcm);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
