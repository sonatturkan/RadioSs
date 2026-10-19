#define _POSIX_C_SOURCE 200809L
#include <glib.h>

/* Genuine private HTTP/store, native GTK and MAIN PCM fixture. The existing
 * cases remain available for diagnostics; this runner selects only the new
 * /stream-metadata namespace, with a fresh fixture per independent case. */
static int stream_metadata_run(void);
#define main radios_stream_metadata_fixture_main
#define g_test_run stream_metadata_run
#include "test_playout_ui.c"
#undef g_test_run
#undef main

#include "radios/stream_metadata.h"
#include "radios/broadcast_header.h"
#include "radios/broadcast_header.h"
#include "radios/audio_patchbay.h"
#include "radios/routing_graph_ui.h"
#include "radios/broadcast_output_ui.h"
#include <unistd.h>

typedef struct {
    Window *window;
    RadioSRoutingMetadataSource source;
    guint deck;
    const gchar *format, *title, *artist, *text;
    RadioSStreamMetadataSnapshot observed;
} StreamMetadataExpected;

static gboolean stream_metadata_matches(gpointer value)
{
    StreamMetadataExpected *expected = value;
    gboolean available = radios_stream_metadata_resolve(expected->window->ctx,
        expected->source, expected->deck, expected->format, &expected->observed);
    if (!expected->title) return !available && !expected->observed.title[0] &&
        !expected->observed.artist[0] && !expected->observed.text[0];
    return available && !g_strcmp0(expected->observed.title, expected->title) &&
        !g_strcmp0(expected->observed.artist, expected->artist) &&
        !g_strcmp0(expected->observed.text, expected->text) &&
        g_utf8_validate(expected->observed.text, -1, NULL);
}

static RadioSStreamMetadataSnapshot stream_metadata_wait(Window *window,
    RadioSRoutingMetadataSource source, guint deck, const gchar *format,
    const gchar *title, const gchar *artist, const gchar *text)
{
    StreamMetadataExpected expected = {window, source, deck, format, title, artist, text, {{0}, {0}, {0}, 0, 0}};
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!stream_metadata_matches(&expected) && g_get_monotonic_time() < deadline) pump();
    if (!stream_metadata_matches(&expected))
        g_error("Stream metadata source %d deck %u expected '%s' / '%s' / '%s'; actual '%s' / '%s' / '%s' generation %" G_GUINT64_FORMAT,
            source, deck, title ? title : "<none>", artist ? artist : "", text ? text : "",
            expected.observed.title, expected.observed.artist, expected.observed.text, expected.observed.generation);
    return expected.observed;
}


typedef struct { Window *window; guint deck; } MetadataDeckOutput;
static gboolean stream_metadata_output_ready(gpointer value)
{
    MetadataDeckOutput *expected = value; RadioSAudioUiSnapshot actual;
    return window_ready(expected->window) &&
        radios_audio_ui_get_snapshot(expected->window->ctx, expected->deck, &actual) && actual.output_ready;
}
static void stream_metadata_wait_output(Window *window, guint deck)
{
    MetadataDeckOutput expected = {window, deck};
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!stream_metadata_output_ready(&expected) && g_get_monotonic_time() < deadline) pump();
    if (!stream_metadata_output_ready(&expected)) {
        RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window->ctx); RadioSAudioSnapshot raw = {0};
        if (engine) { radios_audio_engine_snapshot(engine, &raw); radios_audio_engine_unref(engine); }
        gint channel = -1;
        for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i)
            if (raw.mixer.channels[i].source_deck == (gint)deck) channel = (gint)i;
        g_error("Metadata deck %u output wait: connected%d, bus%d, MAIN-routed%d, channel%d, MAIN/CUE/MON ready%d/%d/%d, document/window-ready%d, engine error '%s'",
            deck, raw.connected, raw.decks[deck].output_bus, raw.decks[deck].main_routed, channel,
            raw.buses[0].ready, raw.buses[1].ready, raw.buses[2].ready, window_ready(window), raw.error);
    }
}
typedef struct { Window *window; RadioSAudioUiBus bus; } MetadataBusOutput;
static gboolean stream_metadata_bus_ready(gpointer value)
{
    MetadataBusOutput *expected = value;
    return window_ready(expected->window) && radios_audio_ui_ready(expected->window->ctx, expected->bus);
}
static void stream_metadata_wait_bus(Window *window, RadioSAudioUiBus bus)
{
    MetadataBusOutput expected = {window, bus};
    wait_until(stream_metadata_bus_ready, &expected, "actual private bus readiness after routing/PFL output flush");
}
static void stream_metadata_assign_outputs(Window *window)
{
    for (guint deck = 0; deck < 2; ++deck) {
        gchar *name = g_strdup_printf("deck-%u-output-channel", deck), *id = g_strdup_printf("%u", deck);
        GtkWidget *selector = find(window->root, name); g_assert_true(GTK_IS_COMBO_BOX(selector));
        wait_until(widget_enabled, selector, "explicit player channel assignment after manual fixture baseline");
        combo(window->root, name, "-1"); combo(window->root, name, id); g_free(name); g_free(id);
        stream_metadata_wait_output(window, deck);
    }
}

static void stream_metadata_format_contract(void)
{
    gchar text[1025];
    g_assert_true(radios_stream_metadata_format("{artist} - {title}", "Sanatçı 🎧", "Şarkı & eser", text));
    g_assert_cmpstr(text, ==, "Sanatçı 🎧 - Şarkı & eser");
    g_assert_true(radios_stream_metadata_format("{artist} - {title}", "", "Yalnız başlık", text));
    g_assert_cmpstr(text, ==, "Yalnız başlık");
    g_assert_true(radios_stream_metadata_format("{title} | {artist}", "Sanatçı", "Başlık", text));
    g_assert_cmpstr(text, ==, "Başlık | Sanatçı");
    /* Substitutions do not recursively expand braces originating in metadata. */
    g_assert_true(radios_stream_metadata_format("{artist}/{title}/{station}", "{title}", "{artist}", text));
    g_assert_cmpstr(text, ==, "{title}/{artist}/{station}");
    g_strlcpy(text, "stale previous song", sizeof text);
    g_assert_false(radios_stream_metadata_format("{artist} - {title}", "Artist without title", "", text));
    g_assert_cmpstr(text, ==, "");
    g_assert_false(radios_stream_metadata_format("{title}", "", "\xff", text));
    g_assert_cmpstr(text, ==, "");
    gchar format[257]; memset(format, 'x', 256); format[256] = 0;
    g_assert_false(radios_stream_metadata_format(format, "", "valid title", text));
    g_assert_cmpstr(text, ==, "");
    GString *title = g_string_new(NULL);
    for (guint i = 0; i < 300; ++i) g_string_append(title, "🎧");
    g_assert_true(radios_stream_metadata_format("{title}", "", title->str, text));
    g_assert_cmpuint(strlen(text), <=, 1024); g_assert_true(g_utf8_validate(text, -1, NULL));
    g_assert_cmpuint(g_utf8_strlen(text, -1), ==, 256); g_string_free(title, TRUE);
    RadioSStreamMetadataSnapshot snapshot; memset(&snapshot, 0x41, sizeof snapshot);
    g_assert_false(radios_stream_metadata_resolve(NULL, RG_METADATA_PLAYER, 0, "{title}", &snapshot));
    g_assert_cmpstr(snapshot.text, ==, ""); g_assert_cmpstr(snapshot.title, ==, "");
}

static void stream_metadata_bound_player_pcm_source_switch(void)
{
    Window window = window_open(); navigate(&window, "broadcast");
    JsonNode *first = media_import(&window, "Doğrulanmış player A", 700, .2, "music");
    JsonNode *second = media_import(&window, "Doğrulanmış player B", 1700, .2, "music");
    first = media_metadata_save(first, "Gerçek sanatçı A", "Albüm A");
    second = media_metadata_save(second, "Gerçek sanatçı B", "Albüm B");
    GError *error = NULL; RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    stream_metadata_wait(&window, RG_METADATA_PLAYER, 0, "{artist} - {title}", NULL, NULL, NULL);
    stream_metadata_wait_output(&window, 0);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(first), 0, FALSE, &error), &error);
    stream_metadata_wait_output(&window, 1);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(second), 1, FALSE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0); deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0);
    stream_metadata_wait(&window, RG_METADATA_PLAYER, 0, "{artist} - {title}", NULL, NULL, NULL);
    RadioSAudioMixerConfig cue_mixer; g_assert_true(radios_audio_ui_get_mixer(window.ctx, &cue_mixer, NULL, NULL));
    cue_mixer.channels[0].pfl = TRUE;
    require_call(radios_audio_ui_set_mixer(window.ctx, &cue_mixer, &error), &error);
    stream_metadata_wait_bus(&window, RADIOS_AUDIO_UI_CUE);
    require_call(radios_audio_ui_cue_mix(window.ctx, TRUE, &error), &error);
    require_call(radios_audio_ui_set_deck_main(window.ctx, 0, FALSE, &error), &error);
    stream_metadata_wait_output(&window, 0);
    require_call(radios_audio_ui_play(window.ctx, 0, &error), &error); deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    stream_metadata_wait(&window, RG_METADATA_PLAYER, 0, "{artist} - {title}", NULL, NULL, NULL);
    goffset cue_offset = capture_offset("CUE"); run_for(180);
    g_assert_cmpfloat(tone_energy("CUE", cue_offset, 700), >, .03);
    require_call(radios_audio_ui_set_deck_main(window.ctx, 0, TRUE, &error), &error);
    stream_metadata_wait_output(&window, 0);
    require_call(radios_audio_ui_cue_mix(window.ctx, FALSE, &error), &error);
    stream_metadata_wait_bus(&window, RADIOS_AUDIO_UI_MAIN);
    RadioSStreamMetadataSnapshot a = stream_metadata_wait(&window, RG_METADATA_PLAYER, 0,
        "{artist} - {title}", "Doğrulanmış player A", "Gerçek sanatçı A", "Gerçek sanatçı A - Doğrulanmış player A");
    RadioSAudioSnapshot raw; radios_audio_engine_snapshot(engine, &raw);
    g_assert_cmpuint(a.deck, ==, 0); g_assert_cmpuint(a.generation, ==, raw.decks[0].generation);
    goffset offset = capture_offset("MAIN"); run_for(180); g_assert_cmpfloat(tone_energy("MAIN", offset, 700), >, .03);
    /* Another selected deck must not silently fall back to the playing one. */
    stream_metadata_wait(&window, RG_METADATA_PLAYER, 1, "{artist} - {title}", NULL, NULL, NULL);
    stream_metadata_wait_output(&window, 1);
    require_call(radios_audio_ui_play(window.ctx, 1, &error), &error); deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 100);
    stream_metadata_wait(&window, RG_METADATA_PLAYER, 1, "{title} | {artist}",
        "Doğrulanmış player B", "Gerçek sanatçı B", "Doğrulanmış player B | Gerçek sanatçı B");
    require_call(radios_audio_ui_pause(window.ctx, 0, &error), &error); deck_wait(&window, 0, RADIOS_AUDIO_UI_PAUSED, 0);
    stream_metadata_wait(&window, RG_METADATA_PLAYER, 0, "{artist} - {title}", NULL, NULL, NULL);
    stream_metadata_wait(&window, RG_METADATA_AUTOMATION, 0, "{title}", NULL, NULL, NULL);
    require_call(radios_audio_ui_stop(window.ctx, 1, &error), &error); deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0);
    stream_metadata_wait(&window, RG_METADATA_PLAYER, 1, "{artist} - {title}", NULL, NULL, NULL);
    radios_audio_engine_unref(engine); json_node_free(first); json_node_free(second); window_close(&window);
}

static JsonNode *stream_metadata_save_private(guint player, JsonNode *first, JsonNode *second)
{
    JsonNode *configuration = programme_config(first, second); JsonObject *settings = json_node_get_object(configuration);
    json_object_set_int_member(settings, "queue_player", player); json_object_set_int_member(settings, "queue_transition", 0);
    json_object_set_double_member(settings, "queue_overlap", 0);
    gchar *name = g_strdup_printf("Oynatıcı %u yayın sırası", player);
    JsonNode *saved = document_save("programme", name, settings); json_node_free(saved); g_free(name); return configuration;
}

static void stream_metadata_private_accepted_plan_pause_stop(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL);
    stream_metadata_assign_outputs(&window);
    JsonNode *first = media_import(&window, "Kabul edilmiş özel A", 600, .2, "music");
    JsonNode *second = media_import(&window, "Kabul edilmiş özel B", 1600, .2, "music");
    first = media_metadata_save(first, "Plan sanatçısı A", "A"); second = media_metadata_save(second, "Plan sanatçısı B", "B");
    JsonNode *configuration = stream_metadata_save_private(1, first, second); radios_data_refresh(window.ctx);
    navigate(&window, "broadcast"); RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    player_queue_select(&window, "1", "Kabul edilmiş özel A");
    stream_metadata_wait_output(&window, 0); click(window.root, "programme-play");
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    stream_metadata_wait(&window, RG_METADATA_PLAYER, 0, "{artist} - {title}",
        "Kabul edilmiş özel A", "Plan sanatçısı A", "Plan sanatçısı A - Kabul edilmiş özel A");
    stream_metadata_wait(&window, RG_METADATA_AUTOMATION, 0, "{artist} - {title}", NULL, NULL, NULL);
    JsonNode *draft = deep_copy(configuration); JsonArray *events = json_object_get_array_member(json_node_get_object(draft), "events");
    json_object_set_string_member(json_array_get_object_element(events, 0), "title", "Sonradan değiştirilmiş taslak");
    JsonNode *saved = document_save("programme", "Oynatıcı 1 yayın sırası", json_node_get_object(draft)); json_node_free(saved);
    radios_data_refresh(window.ctx); run_for(220);
    stream_metadata_wait(&window, RG_METADATA_PLAYER, 0, "{artist} - {title}",
        "Kabul edilmiş özel A", "Plan sanatçısı A", "Plan sanatçısı A - Kabul edilmiş özel A");
    click(window.root, "programme-pause"); player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PAUSED, 0, 0);
    /* Runner pause leaves the decoder PLAYING; metadata must follow owned PCM. */
    stream_metadata_wait(&window, RG_METADATA_PLAYER, 0, "{artist} - {title}", NULL, NULL, NULL);
    click(window.root, "programme-stop"); player_run_wait(engine, 0, RADIOS_AUDIO_RUN_IDLE, 0, 0);
    stream_metadata_wait(&window, RG_METADATA_PLAYER, 0, "{artist} - {title}", NULL, NULL, NULL);
    radios_audio_engine_unref(engine); json_node_free(configuration); json_node_free(draft);
    json_node_free(first); json_node_free(second); window_close(&window);
}

static void stream_metadata_automation_current_overlap_pause_stop(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL);
    stream_metadata_assign_outputs(&window);
    JsonNode *first = media_import(&window, "Otomasyon geçiş A", 700, .2, "music");
    JsonNode *second = media_import(&window, "Otomasyon geçiş B", 1700, .2, "music");
    first = media_metadata_save(first, "Otomasyon sanatçısı A", "A");
    second = media_metadata_save(second, "Otomasyon sanatçısı B", "B");
    JsonNode *left = playlist_create("Metadata otomasyon A", first, NULL);
    JsonNode *right = playlist_create("Metadata otomasyon B", second, NULL);
    JsonNode *configuration = automation_config(left, right, left); JsonObject *settings = json_node_get_object(configuration);
    json_object_set_boolean_member(settings, "jingleEnabled", FALSE); json_object_set_int_member(settings, "sourceMode", 0);
    json_object_set_int_member(settings, "trackMode", 1); json_object_set_int_member(settings, "previewCount", 2);
    json_object_set_int_member(settings, "transition", 1); json_object_set_double_member(settings, "overlap", 1);
    stream_metadata_wait(&window, RG_METADATA_AUTOMATION, 0, "{artist} - {title}", NULL, NULL, NULL);
    GError *error = NULL;
    stream_metadata_wait_output(&window, 0); stream_metadata_wait_output(&window, 1);
    require_call(radios_playout_ui_start_automation(window.ctx, window.root, settings, &error), &error);
    RadioSPlayoutSnapshot playing = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100); g_assert_true(playing.automation);
    RadioSStreamMetadataSnapshot a = stream_metadata_wait(&window, RG_METADATA_AUTOMATION, 0, "{artist} - {title}",
        "Otomasyon geçiş A", "Otomasyon sanatçısı A", "Otomasyon sanatçısı A - Otomasyon geçiş A");
    g_assert_cmpuint(a.deck, ==, (guint)playing.deck);
    RadioSPlayoutSnapshot overlap = run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 0);
    g_assert_cmpint(overlap.next_deck, !=, overlap.deck);
    stream_metadata_wait(&window, RG_METADATA_AUTOMATION, 0, "{artist} - {title}",
        "Otomasyon geçiş A", "Otomasyon sanatçısı A", "Otomasyon sanatçısı A - Otomasyon geçiş A");
    /* The preloaded next deck is already playing in overlap, but automation
     * metadata changes only with the actual runner's current entry. */
    RadioSPlayoutSnapshot next = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 1200);
    RadioSStreamMetadataSnapshot b = stream_metadata_wait(&window, RG_METADATA_AUTOMATION, 0, "{title} | {artist}",
        "Otomasyon geçiş B", "Otomasyon sanatçısı B", "Otomasyon geçiş B | Otomasyon sanatçısı B");
    g_assert_cmpuint(b.deck, ==, (guint)next.deck); g_assert_cmpuint(b.deck, !=, a.deck);
    goffset captured = capture_offset("MAIN"); run_for(180); g_assert_cmpfloat(tone_energy("MAIN", captured, 1700), >, .03);
    require_call(radios_playout_ui_action(window.ctx, RADIOS_PLAYOUT_PAUSE, &error), &error);
    run_wait(&window, RADIOS_PLAYOUT_PAUSED, 1, 0);
    stream_metadata_wait(&window, RG_METADATA_AUTOMATION, 0, "{artist} - {title}", NULL, NULL, NULL);
    stop_run(&window); stream_metadata_wait(&window, RG_METADATA_AUTOMATION, 0, "{artist} - {title}", NULL, NULL, NULL);
    json_node_free(configuration); json_node_free(left); json_node_free(right);
    json_node_free(first); json_node_free(second); window_close(&window);
}


/* Loopback HTTP source/metadata receiver. Each connection has its own worker,
 * so the continuous MP3 upload cannot block independent metadata requests. */
typedef struct {
    GSocketListener *listener;
    GCancellable *cancel;
    GThread *thread;
    GPtrArray *connections;
    GMutex mutex;
    gint metadata_count, metadata_status, hold_metadata, source_count, drop_source;
    gint64 first_metadata_us, second_metadata_us;
    gchar *last_metadata;
} MetadataServer;
typedef struct { MetadataServer *server; GSocket *socket; } MetadataConnection;

static gpointer stream_metadata_http_connection(gpointer value)
{
    MetadataConnection *connection = value; MetadataServer *server = connection->server;
    GSocket *socket = connection->socket; GString *head = g_string_new(NULL); gchar byte;
    while (head->len < 16384 && !strstr(head->str, "\r\n\r\n") &&
            g_socket_receive(socket, &byte, 1, server->cancel, NULL) == 1) g_string_append_c(head, byte);
    if (g_str_has_prefix(head->str, "GET /admin/metadata?")) {
        g_mutex_lock(&server->mutex); g_free(server->last_metadata); server->last_metadata = g_strdup(head->str);
        gint count = g_atomic_int_get(&server->metadata_count);
        if (!count) server->first_metadata_us = g_get_monotonic_time();
        else if (count == 1) server->second_metadata_us = g_get_monotonic_time();
        g_atomic_int_inc(&server->metadata_count); g_mutex_unlock(&server->mutex);
        while (g_atomic_int_get(&server->hold_metadata) && !g_cancellable_is_cancelled(server->cancel)) g_usleep(1000);
        const gchar *reply = g_atomic_int_get(&server->metadata_status) == 200 ?
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" :
            "HTTP/1.1 500 Fixture Failure\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        g_socket_send(socket, reply, strlen(reply), server->cancel, NULL);
    } else if (g_str_has_prefix(head->str, "PUT /metadata-fixture")) {
        g_atomic_int_inc(&server->source_count);
        const gchar *reply = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
        g_socket_send(socket, reply, strlen(reply), server->cancel, NULL);
        gchar bytes[8192];
        while (g_socket_receive(socket, bytes, sizeof bytes, server->cancel, NULL) > 0)
            if (g_atomic_int_compare_and_exchange(&server->drop_source, 1, 0)) break;
    }
    g_string_free(head, TRUE); g_socket_close(socket, NULL); g_object_unref(socket); g_free(connection); return NULL;
}
static gboolean metadata_source_counted(gpointer value)
{ return g_atomic_int_get(&((MetadataServer *)value)->source_count) >= 1; }
static gpointer stream_metadata_http_accept(gpointer value)
{
    MetadataServer *server = value;
    while (!g_cancellable_is_cancelled(server->cancel)) {
        GSocket *socket = g_socket_listener_accept_socket(server->listener, NULL, server->cancel, NULL);
        if (!socket) break;
        MetadataConnection *connection = g_new(MetadataConnection, 1); *connection = (MetadataConnection){server, socket};
        g_ptr_array_add(server->connections, g_thread_new("metadata-loopback-connection", stream_metadata_http_connection, connection));
    }
    return NULL;
}
static guint16 stream_metadata_http_start(MetadataServer *server)
{
    server->listener = g_socket_listener_new(); server->cancel = g_cancellable_new(); server->connections = g_ptr_array_new();
    g_mutex_init(&server->mutex); g_atomic_int_set(&server->metadata_status, 500);
    GInetAddress *loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *address = g_inet_socket_address_new(loopback, 0), *bound = NULL; GError *error = NULL;
    g_assert_true(g_socket_listener_add_address(server->listener, address, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_TCP, NULL, &bound, &error)); g_assert_no_error(error);
    guint16 port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
    g_object_unref(bound); g_object_unref(address); g_object_unref(loopback);
    server->thread = g_thread_new("metadata-loopback-listener", stream_metadata_http_accept, server); return port;
}
static void stream_metadata_http_stop(MetadataServer *server)
{
    g_cancellable_cancel(server->cancel); g_thread_join(server->thread);
    for (guint i = 0; i < server->connections->len; ++i) g_thread_join(g_ptr_array_index(server->connections, i));
    g_ptr_array_unref(server->connections); g_object_unref(server->listener); g_object_unref(server->cancel);
    g_free(server->last_metadata); g_mutex_clear(&server->mutex);
}
typedef struct {
    RadioSAudioPatchbay *patchbay;
    guint node;
    RadioSAudioMetadataState state;
    gboolean active;
} MetadataHttpExpected;
static gboolean stream_metadata_http_state(gpointer value)
{
    MetadataHttpExpected *expected = value; RadioSAudioOutputSnapshot actual;
    return radios_audio_patchbay_stream_metadata_status(expected->patchbay, expected->node, &actual) &&
        actual.metadata_state == expected->state && (!expected->active || actual.state == RADIOS_AUDIO_OUTPUT_ACTIVE);
}
static void stream_metadata_http_wait(RadioSAudioPatchbay *patchbay, guint node, RadioSAudioMetadataState state)
{
    MetadataHttpExpected expected = {patchbay, node, state, TRUE};
    wait_until(stream_metadata_http_state, &expected, "real stream metadata HTTP acknowledgment state, source still ACTIVE");
}
static guint stream_metadata_graph_kind(const RadioSRoutingGraph *graph, RadioSRoutingGraphKind kind)
{
    for (guint i = 0; i < graph->node_count; ++i) if (graph->nodes[i].kind == kind) return graph->nodes[i].id;
    return 0;
}
static void stream_metadata_routing_entry(Window *window, const gchar *name, const gchar *value)
{
    GtkWidget *input = find(window->root, name); g_assert_true(GTK_IS_ENTRY(input)); gtk_entry_set_text(GTK_ENTRY(input), value);
}
static void stream_metadata_server_song(MetadataServer *server, const gchar *song)
{
    g_mutex_lock(&server->mutex); g_assert_nonnull(server->last_metadata);
    g_assert_nonnull(strstr(server->last_metadata, song)); g_mutex_unlock(&server->mutex);
}
typedef struct { MetadataServer *server; const gchar *song; gint count; } MetadataRequestExpected;
static gboolean stream_metadata_server_received(gpointer value)
{
    MetadataRequestExpected *expected = value;
    g_mutex_lock(&expected->server->mutex);
    gboolean received = g_atomic_int_get(&expected->server->metadata_count) >= expected->count &&
        expected->server->last_metadata && strstr(expected->server->last_metadata, expected->song);
    g_mutex_unlock(&expected->server->mutex); return received;
}
static void stream_metadata_server_wait(MetadataServer *server, const gchar *song, gint count)
{
    MetadataRequestExpected expected = {server, song, count};
    wait_until(stream_metadata_server_received, &expected, "loopback server received exact metadata song, beyond local PENDING enqueue");
}
static void stream_metadata_http_dedupe_retry_late_close(void)
{
    MetadataServer server = {0}; guint16 port = stream_metadata_http_start(&server);
    Window window = window_open();
    JsonNode *a = media_import(&window, "Server Song A", 700, .2, "music");
    JsonNode *b = media_import(&window, "Server Song B", 1700, .2, "music");
    a = media_metadata_save(a, "Server Artist", "A"); b = media_metadata_save(b, "Server Artist", "B");
    GError *error = NULL;
    stream_metadata_wait_output(&window, 0);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(a), 0, TRUE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    navigate(&window, "routing"); GtkWidget *page = find(window.root, "routing-workspace"); g_assert_nonnull(page);
    RadioSAudioPatchbay *patchbay = g_object_get_data(G_OBJECT(page), "routing-patchbay"); g_assert_nonnull(patchbay);
    RadioSRoutingGraphUI *editor = radios_routing_graph_ui_from_widget(find(page, "routing-graph-editor")); g_assert_nonnull(editor);
    guint stream = radios_routing_graph_ui_add(editor, RG_STREAM); g_assert_cmpuint(stream, >, 0);
    radios_routing_graph_ui_select(editor, stream); run_for(150);
    RadioSRoutingGraph *graph = (RadioSRoutingGraph *)radios_routing_graph_ui_graph(editor);
    guint main = stream_metadata_graph_kind(graph, RG_MAIN); g_assert_cmpuint(main, >, 0);
    g_assert_true(radios_routing_graph_connect(graph, main, stream, &error)); g_assert_no_error(error);
    stream_metadata_routing_entry(&window, "routing-stream-host", "127.0.0.1");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(page, "routing-stream-port")), port);
    stream_metadata_routing_entry(&window, "routing-stream-mount", "/metadata-fixture");
    stream_metadata_routing_entry(&window, "routing-stream-password", "private-fixture-only");
    combo(window.root, "routing-stream-metadata-source", "player"); combo(window.root, "routing-stream-metadata-player", "0");
    click(page, "routing-stream-start");
    stream_metadata_http_wait(patchbay, stream, RADIOS_AUDIO_METADATA_FAILED);
    RadioSAudioOutputSnapshot failed; g_assert_true(radios_audio_patchbay_stream_metadata_status(patchbay, stream, &failed));
    g_assert_cmpint(failed.metadata_http_status, ==, 500); g_assert_cmpint(failed.state, ==, RADIOS_AUDIO_OUTPUT_ACTIVE);
    /* The fixture parses the source request head byte by byte on its own
     * thread; the metadata reply can come first. Wait for the count, then
     * still require exactly one source connection. */
    wait_until(metadata_source_counted, &server, "fixture counted the stream's source connection");
    g_assert_cmpint(g_atomic_int_get(&server.source_count), ==, 1);
    stream_metadata_server_song(&server, "song=Server%20Artist%20-%20Server%20Song%20A");
    /* Observe at least one 500ms automatic poll with no eligible source.
     * Pausing/resuming the same song cannot erase its failed-request budget. */
    require_call(radios_audio_ui_pause(window.ctx, 0, &error), &error); deck_wait(&window, 0, RADIOS_AUDIO_UI_PAUSED, 0);
    run_for(650); stream_metadata_wait_output(&window, 0);
    require_call(radios_audio_ui_play(window.ctx, 0, &error), &error); deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    run_for(600); g_assert_cmpint(g_atomic_int_get(&server.metadata_count), ==, 1);
    /* A failed submission cannot be treated as a successful dedupe cache. */
    g_atomic_int_set(&server.metadata_status, 200);
    stream_metadata_http_wait(patchbay, stream, RADIOS_AUDIO_METADATA_SUCCESS);
    gint successful_count = g_atomic_int_get(&server.metadata_count); g_assert_cmpint(successful_count, >=, 2);
    g_mutex_lock(&server.mutex);
    g_assert_cmpint(server.second_metadata_us - server.first_metadata_us, >=, 4900000);
    g_mutex_unlock(&server.mutex);
    run_for(600); g_assert_cmpint(g_atomic_int_get(&server.metadata_count), ==, successful_count);
    g_assert_cmpint(g_atomic_int_get(&server.source_count), ==, 1);
    GPtrArray *routing_documents = g_object_get_data(G_OBJECT(page), "radios-data-documents");
    g_assert_nonnull(routing_documents); g_assert_cmpuint(routing_documents->len, >, 0);
    RadioSDocument *routing_document = g_ptr_array_index(routing_documents, 0);
    gint64 routing_revision = radios_data_document_revision(routing_document);
    /* Hiding the panel does not disable the selected node's automation. */
    radios_routing_graph_ui_select(editor, main); navigate(&window, "broadcast");
    g_atomic_int_set(&server.hold_metadata, 1);
    stream_metadata_wait_output(&window, 0);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(b), 0, TRUE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    stream_metadata_http_wait(patchbay, stream, RADIOS_AUDIO_METADATA_PENDING);
    stream_metadata_server_wait(&server, "song=Server%20Artist%20-%20Server%20Song%20B", successful_count + 1);
    RadioSAudioOutputSnapshot pending_before, pending_after;
    g_assert_true(radios_audio_patchbay_stream_metadata_status(patchbay, stream, &pending_before));
    run_for(650); g_assert_true(radios_audio_patchbay_stream_metadata_status(patchbay, stream, &pending_after));
    g_assert_cmpint(pending_after.metadata_state, ==, RADIOS_AUDIO_METADATA_PENDING);
    g_assert_cmpuint(pending_after.metadata_sequence, ==, pending_before.metadata_sequence);
    g_assert_cmpint(g_atomic_int_get(&server.metadata_count), ==, successful_count + 1);
    g_assert_cmpint(radios_data_document_revision(routing_document), ==, routing_revision);
    navigate(&window, "routing"); radios_routing_graph_ui_select(editor, stream); run_for(150);
    combo(window.root, "routing-stream-metadata-source", "manual");
    stream_metadata_routing_entry(&window, "routing-stream-metadata", "Manual draft survives late automatic acknowledgment");
    g_atomic_int_set(&server.hold_metadata, 0); run_for(400);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(page, "routing-stream-metadata"))), ==,
        "Manual draft survives late automatic acknowledgment");
    g_assert_cmpint(g_atomic_int_get(&server.source_count), ==, 1);
    gint manual_count = g_atomic_int_get(&server.metadata_count); run_for(500);
    g_assert_cmpint(g_atomic_int_get(&server.metadata_count), ==, manual_count);
    /* Closing while a real metadata response is pending exercises the timer,
     * patchbay output reference and HTTP worker teardown together. */
    g_atomic_int_set(&server.hold_metadata, 1);
    stream_metadata_wait_output(&window, 0);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(a), 0, TRUE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    combo(window.root, "routing-stream-metadata-source", "player");
    stream_metadata_http_wait(patchbay, stream, RADIOS_AUDIO_METADATA_PENDING);
    stream_metadata_server_wait(&server, "song=Server%20Artist%20-%20Server%20Song%20A", manual_count + 1);
    window_close(&window); g_atomic_int_set(&server.hold_metadata, 0); run_for(180);
    stream_metadata_http_stop(&server); json_node_free(a); json_node_free(b);
}


static guint stream_metadata_add_output(Window *window, guint16 port, const gchar *mount)
{
    GtkWidget *page = find(window->root, "routing-workspace"); g_assert_nonnull(page);
    RadioSRoutingGraphUI *editor = radios_routing_graph_ui_from_widget(find(page, "routing-graph-editor")); g_assert_nonnull(editor);
    guint stream = radios_routing_graph_ui_add(editor, RG_STREAM); g_assert_cmpuint(stream, >, 0);
    radios_routing_graph_ui_select(editor, stream); run_for(100);
    RadioSRoutingGraph *graph = (RadioSRoutingGraph *)radios_routing_graph_ui_graph(editor);
    guint main = stream_metadata_graph_kind(graph, RG_MAIN); g_assert_cmpuint(main, >, 0); GError *error = NULL;
    g_assert_true(radios_routing_graph_connect(graph, main, stream, &error)); g_assert_no_error(error);
    stream_metadata_routing_entry(window, "routing-stream-host", "127.0.0.1");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(page, "routing-stream-port")), port);
    stream_metadata_routing_entry(window, "routing-stream-mount", mount);
    stream_metadata_routing_entry(window, "routing-stream-password", "private-fixture-only");
    combo(window->root, "routing-stream-metadata-source", "player"); combo(window->root, "routing-stream-metadata-player", "0");
    click(page, "routing-stream-start"); return stream;
}
typedef struct { MetadataServer *server; gint source_count, metadata_count; } MetadataReconnectExpected;
static gboolean stream_metadata_reconnected(gpointer value)
{
    MetadataReconnectExpected *expected = value;
    return g_atomic_int_get(&expected->server->source_count) >= expected->source_count &&
        g_atomic_int_get(&expected->server->metadata_count) >= expected->metadata_count;
}
static JsonNode *stream_metadata_long_media(Window *window)
{
    const guint seconds = 45, frames = 48000 * seconds + 31, bytes = frames * 4;
    guchar *wav = g_malloc(44 + bytes);
    memcpy(wav, "RIFF", 4); write_u32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8); write_u32(wav + 16, 16);
    write_u16(wav + 20, 1); write_u16(wav + 22, 2); write_u32(wav + 24, 48000); write_u32(wav + 28, 48000 * 4);
    write_u16(wav + 32, 4); write_u16(wav + 34, 16); memcpy(wav + 36, "data", 4); write_u32(wav + 40, bytes);
    for (guint i = 0; i < frames; ++i) {
        gint16 sample = (gint16)lrint(sin(2 * G_PI * 700 * i / 48000) * .2 * 32767);
        write_u16(wav + 44 + i * 4, (guint16)sample); write_u16(wav + 46 + i * 4, (guint16)sample);
    }
    gchar *uuid = g_uuid_string_random(), *name = g_strconcat(uuid, ".wav", NULL);
    gchar *path = g_build_filename(config.project_root, name, NULL); g_free(name); g_free(uuid);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL)); g_free(wav);
    Import imported = {0};
    GCancellable *handle = radios_media_import_async(window->ctx, window->root, path, "music", "All Stream Song",
        import_complete, &imported, import_destroy);
    g_assert_nonnull(handle); wait_until(import_done, &imported, "genuine 45-second PCM SHA/import analysis");
    g_assert_no_error(imported.error); g_assert_nonnull(imported.result); g_assert_cmpuint(imported.callbacks, ==, 1);
    JsonObject *media = json_object_get_object_member(json_node_get_object(imported.result), "media");
    JsonNode *query = body_new(); json_object_set_string_member(json_node_get_object(query), "media_id", radios_json_string(media, "_id", ""));
    JsonNode *detail = request("/v1/workflows/desktop-media/get", query); json_node_free(query); json_node_free(imported.result);
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    g_assert_true(json_object_get_boolean_member(version, "verified")); g_assert_true(json_object_get_boolean_member(version, "object_available"));
    g_assert_cmpint(radios_json_int(version, "duration_ms", 0), ==, 45001);
    g_assert_true(g_file_test(radios_json_string(version, "managed_path", ""), G_FILE_TEST_IS_REGULAR));
    g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); g_object_unref(handle); return detail;
}
static void stream_metadata_all_outputs_reconnect_no_empty(void)
{
    MetadataServer first = {0}, second = {0};
    guint16 first_port = stream_metadata_http_start(&first), second_port = stream_metadata_http_start(&second);
    g_atomic_int_set(&first.metadata_status, 200); g_atomic_int_set(&second.metadata_status, 200);
    Window window = window_open();
    JsonNode *media = stream_metadata_long_media(&window);
    media = media_metadata_save(media, "All Stream Artist", "A"); GError *error = NULL;
    stream_metadata_wait_output(&window, 0);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(media), 0, TRUE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100); navigate(&window, "routing");
    GtkWidget *page = find(window.root, "routing-workspace"); RadioSAudioPatchbay *patchbay = g_object_get_data(G_OBJECT(page), "routing-patchbay");
    guint one = stream_metadata_add_output(&window, first_port, "/metadata-fixture-one");
    /* The real editor blocks structural edits while start authorization is
     * pending. The first actual ACK proves that phase ended before adding two. */
    stream_metadata_http_wait(patchbay, one, RADIOS_AUDIO_METADATA_SUCCESS);
    guint two = stream_metadata_add_output(&window, second_port, "/metadata-fixture-two");
    stream_metadata_http_wait(patchbay, two, RADIOS_AUDIO_METADATA_SUCCESS);
    stream_metadata_server_wait(&first, "song=All%20Stream%20Artist%20-%20All%20Stream%20Song", 1);
    stream_metadata_server_wait(&second, "song=All%20Stream%20Artist%20-%20All%20Stream%20Song", 1);
    g_assert_cmpint(g_atomic_int_get(&first.metadata_count), ==, 1); g_assert_cmpint(g_atomic_int_get(&second.metadata_count), ==, 1);
    /* Both nodes must continue to be observed with no selected stream panel. */
    navigate(&window, "broadcast");
    require_call(radios_audio_ui_pause(window.ctx, 0, &error), &error); deck_wait(&window, 0, RADIOS_AUDIO_UI_PAUSED, 0);
    run_for(400);
    g_assert_cmpint(g_atomic_int_get(&first.metadata_count), ==, 1); g_assert_cmpint(g_atomic_int_get(&second.metadata_count), ==, 1);
    stream_metadata_wait_output(&window, 0);
    require_call(radios_audio_ui_play(window.ctx, 0, &error), &error); deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    run_for(400);
    g_assert_cmpint(g_atomic_int_get(&first.metadata_count), ==, 1); g_assert_cmpint(g_atomic_int_get(&second.metadata_count), ==, 1);
    /* Close one actual upload connection. Its automatic patchbay reconnect
     * must resubmit the current title while the untouched stream stays deduped. */
    g_atomic_int_set(&first.drop_source, 1);
    MetadataReconnectExpected expected = {&first, 2, 2};
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!stream_metadata_reconnected(&expected) && g_get_monotonic_time() < deadline) pump();
    if (!stream_metadata_reconnected(&expected)) {
        RadioSAudioOutputSnapshot output = {0}; RadioSAudioUiSnapshot deck = {0};
        radios_audio_patchbay_stream_metadata_status(patchbay, one, &output); radios_audio_ui_get_snapshot(window.ctx, 0, &deck);
        g_error("Stream reconnect expected first source/metadata counts >=2/2; actual first%d/%d, second%d/%d; output state%d metadata%d HTTP%ld; deck state%d cursor%" G_GUINT64_FORMAT
            "/duration%" G_GUINT64_FORMAT " MAIN%d ready%d error '%s'",
            g_atomic_int_get(&first.source_count), g_atomic_int_get(&first.metadata_count),
            g_atomic_int_get(&second.source_count), g_atomic_int_get(&second.metadata_count),
            output.state, output.metadata_state, output.http_status, deck.state, deck.position_ms, deck.duration_ms,
            deck.main_enabled, deck.output_ready, output.error);
    }
    stream_metadata_http_wait(patchbay, one, RADIOS_AUDIO_METADATA_SUCCESS);
    g_assert_cmpint(g_atomic_int_get(&second.source_count), ==, 1); g_assert_cmpint(g_atomic_int_get(&second.metadata_count), ==, 1);
    stream_metadata_server_song(&first, "song=All%20Stream%20Artist%20-%20All%20Stream%20Song");
    require_call(radios_audio_ui_stop(window.ctx, 0, &error), &error); deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    gint first_count = g_atomic_int_get(&first.metadata_count), second_count = g_atomic_int_get(&second.metadata_count);
    run_for(400);
    g_assert_cmpint(g_atomic_int_get(&first.metadata_count), ==, first_count);
    g_assert_cmpint(g_atomic_int_get(&second.metadata_count), ==, second_count);
    window_close(&window); stream_metadata_http_stop(&first); stream_metadata_http_stop(&second); json_node_free(media);
}

/* A real HTTP listener proves that filtering means no request, rather than
 * publishing an empty string or merely hiding the automatic entry field. */
static void stream_metadata_player_jingle_advertisement_keep_last_http(void)
{
    MetadataServer server = {0}; guint16 port = stream_metadata_http_start(&server);
    g_atomic_int_set(&server.metadata_status, 200);
    Window window = window_open();
    const gchar *titles[] = {"Last Song A", "Private Station Jingle", "Private Advert", "Next Song B"};
    const gchar *kinds[] = {"music", "jingle", "advertisement", "music"};
    const gdouble tones[] = {700, 1200, 2200, 1700};
    JsonNode *media[G_N_ELEMENTS(titles)]; GError *error = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(media); ++i) {
        media[i] = media_import(&window, titles[i], tones[i], .2, kinds[i]);
        media[i] = media_metadata_save(media[i], "Actual Artist", "A");
    }
    stream_metadata_wait_output(&window, 0);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(media[0]), 0, TRUE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100); navigate(&window, "routing");
    GtkWidget *page = find(window.root, "routing-workspace");
    RadioSAudioPatchbay *patchbay = g_object_get_data(G_OBJECT(page), "routing-patchbay");
    guint stream = stream_metadata_add_output(&window, port, "/metadata-fixture-filter");
    stream_metadata_http_wait(patchbay, stream, RADIOS_AUDIO_METADATA_SUCCESS);
    stream_metadata_server_wait(&server, "song=Actual%20Artist%20-%20Last%20Song%20A", 1);
    gint previous_count = g_atomic_int_get(&server.metadata_count);
    RadioSAudioOutputSnapshot prior = {0};
    g_assert_true(radios_audio_patchbay_stream_metadata_status(patchbay, stream, &prior));
    navigate(&window, "broadcast");
    for (guint i = 1; i < 3; ++i) {
        stream_metadata_wait_output(&window, 0);
        require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(media[i]), 0, TRUE, &error), &error);
        RadioSAudioUiSnapshot actual = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
        g_assert_cmpstr(actual.title, ==, titles[i]); g_assert_cmpstr(actual.kind, ==, kinds[i]);
        stream_metadata_wait(&window, RG_METADATA_PLAYER, 0, "{artist} - {title}", NULL, NULL, NULL);
        goffset pcm = capture_offset("MAIN"); run_for(1150);
        g_assert_cmpfloat(tone_energy("MAIN", pcm, tones[i]), >, .03);
        g_assert_cmpint(g_atomic_int_get(&server.metadata_count), ==, previous_count);
        stream_metadata_server_song(&server, "song=Actual%20Artist%20-%20Last%20Song%20A");
        RadioSAudioOutputSnapshot kept = {0};
        g_assert_true(radios_audio_patchbay_stream_metadata_status(patchbay, stream, &kept));
        g_assert_cmpuint(kept.metadata_sequence, ==, prior.metadata_sequence);
        g_assert_cmpstr(kept.metadata_title, ==, prior.metadata_title);
    }
    stream_metadata_wait_output(&window, 0);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(media[3]), 0, TRUE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    stream_metadata_wait(&window, RG_METADATA_PLAYER, 0, "{artist} - {title}", "Next Song B", "Actual Artist", "Actual Artist - Next Song B");
    stream_metadata_server_wait(&server, "song=Actual%20Artist%20-%20Next%20Song%20B", previous_count + 1);
    stream_metadata_http_wait(patchbay, stream, RADIOS_AUDIO_METADATA_SUCCESS);
    g_assert_cmpint(g_atomic_int_get(&server.metadata_count), ==, previous_count + 1);
    window_close(&window); stream_metadata_http_stop(&server);
    for (guint i = 0; i < G_N_ELEMENTS(media); ++i) json_node_free(media[i]);
}

static JsonNode *stream_metadata_four_events(JsonNode *media[4])
{
    JsonNode *configuration = programme_config(media[0], media[1]);
    JsonNode *tail = programme_config(media[2], media[3]);
    JsonArray *events = json_object_get_array_member(json_node_get_object(configuration), "events");
    JsonArray *remaining = json_object_get_array_member(json_node_get_object(tail), "events");
    for (guint i = 0; i < json_array_get_length(remaining); ++i)
        json_array_add_element(events, json_node_copy(json_array_get_element(remaining, i)));
    json_node_free(tail); return configuration;
}

/* Both global automation and a view-independent app-owned player scope read
 * classification from their accepted plan, including after a draft changes. */
static void stream_metadata_accepted_plan_jingle_advertisement_pcm(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL);
    const gchar *titles[] = {"Accepted Song A", "Accepted Jingle", "Accepted Advert", "Accepted Song B"};
    const gchar *kinds[] = {"music", "jingle", "advertisement", "music"};
    const gdouble tones[] = {700, 1200, 2200, 1700}; JsonNode *media[4]; GError *error = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(media); ++i) {
        media[i] = media_import(&window, titles[i], tones[i], .2, kinds[i]);
        media[i] = media_metadata_save(media[i], "Accepted Artist", "A");
    }
    JsonNode *configuration = stream_metadata_four_events(media);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    RadioSPlayerWorkspace *workspace = window.ctx->players; g_assert_nonnull(workspace);
    ScopeReady ready = {workspace, 0};
    wait_until(scope_workspace_execution_ready, &ready, "app-owned metadata scope's private history baseline");
    for (guint scoped = 0; scoped < 2; ++scoped) {
        if (scoped) {
            RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
            RadioSAutomationBinding binding;
            require_call(radios_automation_scope_resolve(radios_player_workspace_topology(workspace), &scope, &binding, &error), &error);
            JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "kind", "programme");
            json_object_set_member(json_node_get_object(body), "config", deep_copy(configuration));
            JsonNode *prepared = request("/v1/workflows/desktop-playout/prepare", body); json_node_free(body);
            require_call(radios_player_workspace_start_prepared(workspace, &binding, json_node_get_object(prepared), RADIOS_AUTODJ_SEMI, &error), &error);
            json_node_free(prepared);
        } else require_call(radios_playout_ui_start_programme(window.ctx, window.root, json_node_get_object(configuration), 0, RADIOS_AUDIO_RUN_EOF, 0, &error), &error);
        for (guint index = 0; index < G_N_ELEMENTS(media); ++index) {
            guint deck = scoped ? (guint)player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, index, 4800).deck_current :
                (guint)run_wait(&window, RADIOS_PLAYOUT_PLAYING, index, 100).deck;
            RadioSPlayoutDeckMetadata accepted = {0};
            g_assert_true(radios_broadcast_player_deck_metadata(window.ctx, deck, &accepted));
            g_assert_cmpstr(accepted.title, ==, titles[index]); g_assert_cmpstr(accepted.kind, ==, kinds[index]);
            gboolean song = index == 0 || index == 3;
            gchar *text = song ? g_strdup_printf("Accepted Artist - %s", titles[index]) : NULL;
            stream_metadata_wait(&window, RG_METADATA_PLAYER, deck, "{artist} - {title}",
                song ? titles[index] : NULL, song ? "Accepted Artist" : NULL, text);
            if (!scoped) stream_metadata_wait(&window, RG_METADATA_AUTOMATION, 0, "{artist} - {title}",
                song ? titles[index] : NULL, song ? "Accepted Artist" : NULL, text);
            g_free(text);
            goffset pcm = capture_offset("MAIN"); run_for(170);
            g_assert_cmpfloat(tone_energy("MAIN", pcm, tones[index]), >, .03);
            if (index + 1 < G_N_ELEMENTS(media)) {
                if (scoped) require_call(radios_player_workspace_action(workspace, 0, RADIOS_PLAYOUT_NEXT, &error), &error);
                else require_call(radios_playout_ui_action(window.ctx, RADIOS_PLAYOUT_NEXT, &error), &error);
            }
        }
        if (scoped) require_call(radios_player_workspace_action(workspace, 0, RADIOS_PLAYOUT_STOP, &error), &error);
        else stop_run(&window);
        run_for(150);
    }
    radios_audio_engine_unref(engine); json_node_free(configuration);
    for (guint i = 0; i < G_N_ELEMENTS(media); ++i) json_node_free(media[i]);
    window_close(&window);
}

/* Insertion is authoritative even when a jingle playlist contains a row
 * catalogued as music. entry->jingle must override the underlying row kind. */
/* R10.1: the output panel's "Son çalıştırmalar" list shows the station's
 * broadcast_local_run receipts (desktop-broadcast/overview): a fresh station
 * lists none; a started MAIN recording appears with kind, name, state and the
 * time of its last observation, without any operator refresh. */
typedef struct { GtkWidget *scope; const gchar *kind; const gchar *name; } RunsRow;
static gboolean runs_list_has(gpointer data)
{
    RunsRow *expected = data; GtkWidget *view = find(expected->scope, "broadcast-runs-list");
    if (!GTK_IS_TREE_VIEW(view)) return FALSE;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view)); GtkTreeIter iter; gboolean found = FALSE;
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid && !found; valid = gtk_tree_model_iter_next(model, &iter)) {
        gchar *kind = NULL, *name = NULL, *state = NULL, *when = NULL;
        gtk_tree_model_get(model, &iter, 0, &kind, 1, &name, 2, &state, 3, &when, -1);
        found = !g_strcmp0(kind, expected->kind) && !g_strcmp0(name, expected->name) && state && *state &&
            (!g_strcmp0(state, "Hazırlandı") || !g_strcmp0(state, "Çalışıyor") || !g_strcmp0(state, "Tamamlandı")) &&
            when && g_strcmp0(when, "—") && g_utf8_validate(when, -1, NULL);
        g_free(kind); g_free(name); g_free(state); g_free(when);
    }
    return found;
}
static gboolean profile_acknowledged(gpointer data)
{
    GPtrArray *documents = g_object_get_data(G_OBJECT(data), "radios-data-documents");
    return documents && documents->len && radios_data_document_revision(g_ptr_array_index(documents, 0)) > 0;
}
static void stream_metadata_broadcast_runs_list(void)
{
    Window window = window_open();
    /* The output panel is mounted the way test_broadcast_ui mounts it: the
     * broadcast page itself no longer includes it. Closed, it reads nothing. */
    GtkWidget *layout = gtk_bin_get_child(GTK_BIN(window.root)); g_assert_true(GTK_IS_BOX(layout));
    GtkWidget *panel = radios_broadcast_output_panel_new(window.ctx);
    gtk_box_pack_start(GTK_BOX(layout), panel, FALSE, FALSE, 0); gtk_widget_show_all(panel);
    GtkWidget *view = find(panel, "broadcast-runs-list"); g_assert_true(GTK_IS_TREE_VIEW(view));
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    run_for(400); g_assert_cmpint(gtk_tree_model_iter_n_children(model, NULL), ==, 0);
    gtk_expander_set_expanded(GTK_EXPANDER(panel), TRUE);
    run_for(800); g_assert_cmpint(gtk_tree_model_iter_n_children(model, NULL), ==, 0);
    set(window.root, "broadcast-record-filename", "son-calistirmalar-kaniti.wav"); click(window.root, "broadcast-record-save");
    wait_until(profile_acknowledged, find(window.root, "broadcast-profile-record"), "record profile acknowledged by the private store");
    click(window.root, "broadcast-record-start");
    RunsRow expected = {panel, "Kayıt", "MAIN yayın kaydı"};
    wait_until(runs_list_has, &expected, "Son çalıştırmalar lists the new record run with kind, name, state and time");
    click(window.root, "broadcast-record-stop"); run_for(300);
    g_assert_cmpint(gtk_tree_model_iter_n_children(model, NULL), ==, 1);
    window_close(&window);
}
/* R10.1 in the application: Teknik yönetim › Sunucular shows the same list.
 * While the page is hidden it asks nothing (a run created meanwhile does not
 * appear there); shown, it lists the run at once from the private store. */
static void stream_metadata_technical_servers_runs_list(void)
{
    Window window = window_open();
    GtkWidget *servers = find(window.root, "technical-servers-pane"); g_assert_true(GTK_IS_WIDGET(servers));
    GtkWidget *technical_view = find(servers, "broadcast-runs-list"); g_assert_true(GTK_IS_TREE_VIEW(technical_view));
    GtkTreeModel *technical = gtk_tree_view_get_model(GTK_TREE_VIEW(technical_view));
    g_assert_false(gtk_widget_get_mapped(technical_view));
    /* A run is created through the test-mounted output panel meanwhile. */
    GtkWidget *layout = gtk_bin_get_child(GTK_BIN(window.root)); g_assert_true(GTK_IS_BOX(layout));
    GtkWidget *panel = radios_broadcast_output_panel_new(window.ctx);
    gtk_box_pack_start(GTK_BOX(layout), panel, FALSE, FALSE, 0); gtk_widget_show_all(panel);
    gtk_expander_set_expanded(GTK_EXPANDER(panel), TRUE);
    set(window.root, "broadcast-record-filename", "sunucular-son-calistirmalar.wav"); click(window.root, "broadcast-record-save");
    wait_until(profile_acknowledged, find(window.root, "broadcast-profile-record"), "record profile acknowledged by the private store");
    click(window.root, "broadcast-record-start");
    RunsRow in_panel = {panel, "Kayıt", "MAIN yayın kaydı"};
    wait_until(runs_list_has, &in_panel, "the visible panel list shows the new run");
    run_for(1500);
    g_assert_false(gtk_widget_get_mapped(technical_view));
    g_assert_cmpint(gtk_tree_model_iter_n_children(technical, NULL), ==, 0);
    navigate(&window, "technical");
    gtk_stack_set_visible_child_name(GTK_STACK(gtk_widget_get_parent(servers)), "servers");
    RunsRow in_servers = {servers, "Kayıt", "MAIN yayın kaydı"};
    wait_until(runs_list_has, &in_servers, "Teknik yönetim › Sunucular lists the run as soon as it is shown");
    g_assert_true(gtk_widget_get_mapped(technical_view));
    click(window.root, "broadcast-record-stop"); run_for(300);
    window_close(&window);
}
/* R8.4: the full native window stays responsive while audio devices are
 * plugged into and removed from the (private) sound server. Only sinks this
 * case creates are loaded/unloaded; the default device is never changed.
 * MAIN stays ready and the MAIN output selector follows every device by
 * itself, without "Yenile". */
typedef struct { gint done; guint loaded, cycles; gchar prefix[64]; } WindowPlugger;
static gchar *window_pactl(const gchar *a, const gchar *b, const gchar *c, const gchar *d)
{
    const gchar *argv[] = {"pactl", a, b, c, d, NULL};
    gchar *out = NULL; gint status = 0;
    if (!g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
            &out, NULL, &status, NULL) || !g_spawn_check_wait_status(status, NULL)) { g_free(out); return NULL; }
    return g_strstrip(out);
}
static gpointer window_plug_cycles(gpointer data)
{
    WindowPlugger *plugger = data;
    for (guint i = 0; i < plugger->cycles; ++i) {
        gchar *argument = g_strdup_printf("sink_name=%s%u", plugger->prefix, i);
        gchar *module = window_pactl("load-module", "module-null-sink", argument, "sink_properties=device.description=Takılan_aygıt");
        if (module && g_ascii_string_to_unsigned(module, 10, 0, G_MAXUINT32, NULL, NULL)) ++plugger->loaded;
        g_usleep(1200000);
        if (module) g_free(window_pactl("unload-module", module, NULL, NULL));
        g_free(module); g_free(argument); g_usleep(400000);
    }
    g_atomic_int_set(&plugger->done, 1);
    return NULL;
}
typedef struct {
    Window *window; WindowPlugger *plugger; GtkWidget *combo; gint64 last, max_gap; gboolean main_lost; GHashTable *seen;
    gint64 lost_at, lost_total; guint lost_count; gchar lost_detail[512];
} WindowHotplug;
static gboolean window_hotplug_tick(gpointer data)
{
    WindowHotplug *hotplug = data; gint64 now = g_get_monotonic_time();
    if (hotplug->last) hotplug->max_gap = MAX(hotplug->max_gap, now - hotplug->last);
    hotplug->last = now;
    GtkWidget *combo = hotplug->combo;
    GtkTreeModel *model = GTK_IS_COMBO_BOX(combo) ? gtk_combo_box_get_model(GTK_COMBO_BOX(combo)) : NULL;
    gint column = GTK_IS_COMBO_BOX(combo) ? gtk_combo_box_get_id_column(GTK_COMBO_BOX(combo)) : -1;
    GtkTreeIter iter;
    for (gboolean valid = model && column >= 0 && gtk_tree_model_get_iter_first(model, &iter); valid; valid = gtk_tree_model_iter_next(model, &iter)) {
        gchar *id = NULL; gtk_tree_model_get(model, &iter, column, &id, -1);
        if (id && g_str_has_prefix(id, hotplug->plugger->prefix)) g_hash_table_add(hotplug->seen, id); else g_free(id);
    }
    if (!radios_audio_ui_ready(hotplug->window->ctx, RADIOS_AUDIO_UI_MAIN)) {
        if (!hotplug->lost_at) {
            hotplug->lost_at = now; ++hotplug->lost_count;
            RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(hotplug->window->ctx);
            RadioSAudioSnapshot *s = g_new0(RadioSAudioSnapshot, 1);
            if (engine) { radios_audio_engine_snapshot(engine, s); radios_audio_engine_unref(engine); }
            if (!hotplug->main_lost)
                g_snprintf(hotplug->lost_detail, sizeof hotplug->lost_detail, "connected=%d ready=%d device=%s bus_error=%s error=%s",
                    s->connected, s->buses[RADIOS_AUDIO_BUS_MAIN].ready, s->buses[RADIOS_AUDIO_BUS_MAIN].device,
                    s->buses[RADIOS_AUDIO_BUS_MAIN].error, s->error);
            g_free(s);
        }
        hotplug->main_lost = TRUE;
    } else if (hotplug->lost_at) { hotplug->lost_total += now - hotplug->lost_at; hotplug->lost_at = 0; }
    return g_atomic_int_get(&hotplug->plugger->done) ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}
static gboolean main_output_ready(gpointer data)
{ return radios_audio_ui_ready(((Window *)data)->ctx, RADIOS_AUDIO_UI_MAIN); }
static void stream_metadata_device_hotplug_window_responsive(void)
{
    Window window = window_open();
    /* Measure only the hotplug period: window setup re-routes every deck,
     * which reopens the outputs; start once MAIN has settled again. */
    wait_until(main_output_ready, &window, "MAIN output settled after window setup");
    run_for(1500); wait_until(main_output_ready, &window, "MAIN output still ready before devices are plugged");
    WindowPlugger plugger = {0, 0, 5, ""};
    g_snprintf(plugger.prefix, sizeof plugger.prefix, "radios_window_hotplug_%d_", (gint)getpid());
    WindowHotplug hotplug = {&window, &plugger, find(window.root, "task-main-output"), 0, 0, FALSE,
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), 0, 0, 0, ""};
    g_assert_true(GTK_IS_COMBO_BOX(hotplug.combo));
    g_timeout_add(20, window_hotplug_tick, &hotplug);
    GThread *thread = g_thread_new("window-hotplug", window_plug_cycles, &plugger);
    while (!g_atomic_int_get(&plugger.done)) g_main_context_iteration(NULL, TRUE);
    g_thread_join(thread); run_for(100);
    g_test_message("window hotplug: %u devices plugged/unplugged, main loop max gap %" G_GINT64_FORMAT
        " ms, MAIN lost: %s (%u times, %" G_GINT64_FORMAT " ms%s%s), devices listed without Yenile: %u", plugger.loaded,
        hotplug.max_gap / 1000, hotplug.main_lost ? "yes" : "no", hotplug.lost_count, hotplug.lost_total / 1000,
        hotplug.lost_detail[0] ? "; " : "", hotplug.lost_detail, g_hash_table_size(hotplug.seen));
    g_assert_cmpuint(plugger.loaded, ==, plugger.cycles);
    g_assert_cmpint(hotplug.max_gap, <, 500000); /* a freeze, not scheduler jitter on a loaded host */
    g_assert_false(hotplug.main_lost);
    g_assert_cmpuint(g_hash_table_size(hotplug.seen), ==, plugger.cycles);
    g_hash_table_unref(hotplug.seen);
    window_close(&window);
}
static void stream_metadata_automation_inserted_jingle_marker_pcm(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL);
    JsonNode *a = media_import(&window, "Marker Music A", 700, .2, "music");
    JsonNode *b = media_import(&window, "Marker Music B", 1700, .2, "music");
    JsonNode *j = media_import(&window, "Inserted Station ID", 2500, .2, "music");
    JsonNode *left = playlist_create("Marker source A", a, NULL);
    JsonNode *right = playlist_create("Marker source B", b, NULL);
    JsonNode *ids = playlist_create("Station IDs with legacy music kind", j, NULL);
    JsonNode *configuration = automation_config(left, right, ids); JsonObject *settings = json_node_get_object(configuration);
    json_object_set_boolean_member(settings, "jingleEnabled", TRUE); json_object_set_int_member(settings, "jingleEvery", 1);
    json_object_set_int_member(settings, "sourceMode", 0); json_object_set_int_member(settings, "trackMode", 1);
    json_object_set_int_member(settings, "previewCount", 2); json_object_set_int_member(settings, "transition", 0);
    json_object_set_double_member(settings, "overlap", 0); GError *error = NULL;
    require_call(radios_playout_ui_start_automation(window.ctx, window.root, settings, &error), &error);
    RadioSPlayoutSnapshot current = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100);
    stream_metadata_wait(&window, RG_METADATA_AUTOMATION, 0, "{title}", "Marker Music A", "", "Marker Music A");
    require_call(radios_playout_ui_action(window.ctx, RADIOS_PLAYOUT_NEXT, &error), &error);
    current = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 100);
    RadioSPlayoutDeckMetadata accepted = {0};
    g_assert_true(radios_playout_ui_deck_metadata(window.ctx, (guint)current.deck, &accepted));
    g_assert_cmpstr(accepted.title, ==, "Inserted Station ID"); g_assert_cmpstr(accepted.kind, ==, "jingle");
    stream_metadata_wait(&window, RG_METADATA_AUTOMATION, 0, "{title}", NULL, NULL, NULL);
    stream_metadata_wait(&window, RG_METADATA_PLAYER, (guint)current.deck, "{title}", NULL, NULL, NULL);
    goffset pcm = capture_offset("MAIN"); run_for(200); g_assert_cmpfloat(tone_energy("MAIN", pcm, 2500), >, .03);
    require_call(radios_playout_ui_action(window.ctx, RADIOS_PLAYOUT_NEXT, &error), &error);
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 2, 100);
    stream_metadata_wait(&window, RG_METADATA_AUTOMATION, 0, "{title}", "Marker Music B", "", "Marker Music B");
    stop_run(&window); json_node_free(configuration); json_node_free(left); json_node_free(right); json_node_free(ids);
    json_node_free(a); json_node_free(b); json_node_free(j); window_close(&window);
}

static int stream_metadata_run(void)
{
    g_test_add_func("/stream-metadata/format-UTF8-bounds-placeholder-contract", stream_metadata_format_contract);
    g_test_add_func("/stream-metadata/player-jingle-advertisement-keep-last-HTTP", stream_metadata_player_jingle_advertisement_keep_last_http);
    g_test_add_func("/stream-metadata/accepted-plan-jingle-advertisement-PCM", stream_metadata_accepted_plan_jingle_advertisement_pcm);
    g_test_add_func("/stream-metadata/automation-inserted-jingle-marker-PCM", stream_metadata_automation_inserted_jingle_marker_pcm);
    g_test_add_func("/stream-metadata/bound-player-source-switch-PCM", stream_metadata_bound_player_pcm_source_switch);
    g_test_add_func("/stream-metadata/private-accepted-plan-pause-stop", stream_metadata_private_accepted_plan_pause_stop);
    g_test_add_func("/stream-metadata/automation-current-overlap-pause-stop", stream_metadata_automation_current_overlap_pause_stop);
    g_test_add_func("/stream-metadata/HTTP-dedupe-failure-retry-late-close", stream_metadata_http_dedupe_retry_late_close);
    g_test_add_func("/stream-metadata/all-output-reconnect-no-empty", stream_metadata_all_outputs_reconnect_no_empty);
    g_test_add_func("/stream-metadata/broadcast-runs-list-fills", stream_metadata_broadcast_runs_list);
    g_test_add_func("/stream-metadata/technical-servers-runs-list", stream_metadata_technical_servers_runs_list);
    g_test_add_func("/stream-metadata/device-hotplug-window-responsive", stream_metadata_device_hotplug_window_responsive);
    return g_test_run();
}

int main(int argc, char **argv)
{ return radios_stream_metadata_fixture_main(argc, argv); }
