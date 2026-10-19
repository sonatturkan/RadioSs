#define _POSIX_C_SOURCE 200809L
#include <glib.h>

/* Keep the genuine native broadcast fixture, private database and loopback
 * authenticated Icecast endpoint. The wrapper selects only this new case. */
static int broadcast_metadata_run(void);
#define main radios_broadcast_metadata_fixture_main
#define g_test_run broadcast_metadata_run
#include "test_broadcast_ui.c"
#undef g_test_run
#undef main

typedef struct { Window *window; const gchar *title; } AutomaticMetadataWait;
static gboolean automatic_metadata_matches(gpointer data)
{
    AutomaticMetadataWait *wanted = data; RadioSAudioOutputSnapshot actual = {0};
    return radios_broadcast_ui_output_snapshot(wanted->window->ctx, "stream", &actual) &&
        actual.metadata_state == RADIOS_AUDIO_METADATA_SUCCESS &&
        !g_strcmp0(actual.metadata_title, wanted->title);
}
static RadioSAudioOutputSnapshot automatic_metadata_wait(Window *window, const gchar *title)
{
    AutomaticMetadataWait wanted = {window, title};
    wait_until(automatic_metadata_matches, &wanted, "automatic song metadata HTTP acknowledgement");
    RadioSAudioOutputSnapshot actual = {0};
    g_assert_true(radios_broadcast_ui_output_snapshot(window->ctx, "stream", &actual));
    return actual;
}
static void assert_captured_metadata(const gchar *title)
{
    gchar *capture = NULL;
    g_assert_true(g_file_get_contents(g_getenv("RADIOS_TEST_METADATA_CAPTURE"), &capture, NULL, NULL));
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, capture, -1, NULL));
    JsonObject *metadata = json_node_get_object(json_parser_get_root(parser));
    g_assert_cmpstr(radios_json_string(metadata, "title", ""), ==, title);
    g_assert_true(json_object_get_boolean_member(metadata, "authorization_seen"));
    g_object_unref(parser); g_free(capture);
}
static void test_automatic_metadata_excludes_jingle_advertisement(void)
{
    Window window = window_open();
    const gchar *titles[] = {"Legacy Last Song", "Legacy Private Jingle", "Legacy Private Advert", "Legacy Next Song"};
    const gchar *kinds[] = {"music", "jingle", "advertisement", "music"};
    JsonNode *media[G_N_ELEMENTS(titles)];
    for (guint i = 0; i < G_N_ELEMENTS(media); ++i)
        media[i] = media_import(&window, titles[i], 48000 * 12, kinds[i]);
    const gchar *url = g_getenv("RADIOS_TEST_STREAM_URL");
    const gchar *password = g_getenv("RADIOS_TEST_STREAM_PASSWORD");
    g_assert_nonnull(url); g_assert_nonnull(password);
    set(window.root, "broadcast-stream-url", url);
    set(window.root, "broadcast-stream-username", "source"); save_profile(&window, "stream");
    set(window.root, "broadcast-stream-password", password); click(window.root, "broadcast-stream-start");
    output_wait(&window, "stream", RADIOS_AUDIO_OUTPUT_ACTIVE, 0);
    radios_broadcast_ui_metadata_automatic(window.ctx, TRUE);
    programme_start(&window, media[0]);
    RadioSAudioOutputSnapshot previous = automatic_metadata_wait(&window, titles[0]);
    assert_captured_metadata(titles[0]);
    for (guint i = 1; i < 3; ++i) {
        programme_stop(&window); programme_start(&window, media[i]);
        /* Several automatic polls consume actual MAIN PCM. Neither a jingle,
         * an advertisement nor a blank replacement may enqueue metadata. */
        run_wait(&window, titles[i], 900); run_for(350);
        RadioSAudioOutputSnapshot kept = {0};
        g_assert_true(radios_broadcast_ui_output_snapshot(window.ctx, "stream", &kept));
        g_assert_cmpuint(kept.metadata_sequence, ==, previous.metadata_sequence);
        g_assert_cmpstr(kept.metadata_title, ==, titles[0]);
        g_assert_cmpuint(kept.frames_processed, >, previous.frames_processed);
        assert_captured_metadata(titles[0]); previous = kept;
    }
    programme_stop(&window); programme_start(&window, media[3]);
    RadioSAudioOutputSnapshot final = automatic_metadata_wait(&window, titles[3]);
    g_assert_cmpuint(final.metadata_sequence, ==, previous.metadata_sequence + 1);
    assert_captured_metadata(titles[3]);
    click(window.root, "broadcast-stream-stop");
    output_wait(&window, "stream", RADIOS_AUDIO_OUTPUT_STOPPED, 48000);
    programme_stop(&window);
    for (guint i = 0; i < G_N_ELEMENTS(media); ++i) json_node_free(media[i]);
    window_close(&window);
}
static int broadcast_metadata_run(void)
{
    g_test_add_func("/native-broadcast/automatic-metadata-excludes-jingle-advertisement",
        test_automatic_metadata_excludes_jingle_advertisement);
    return g_test_run();
}
int main(int argc, char **argv)
{ return radios_broadcast_metadata_fixture_main(argc, argv); }
