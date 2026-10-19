/* White-box GTK checks keep fixture records out of the production browser API.
 * This translation unit owns the browser symbols; radios_ui's static browser
 * object is consequently not extracted by the linker. */
#include "../src/ui/playlist_browser.c"

typedef struct {
    RadioSViewContext ctx;
    GtkWidget *window, *widget;
    PlaylistBrowser *browser;
} BrowserHarness;

static JsonObject *fixture_object(const gchar *text)
{
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    JsonObject *object = object_copy(json_node_get_object(json_parser_get_root(parser)));
    g_object_unref(parser); return object;
}

static JsonObject *local_fixture(void)
{
    return fixture_object("{\"media\":{\"_id\":\"11111111-1111-4111-8111-111111111111\",\"title\":\"Yerel parça\",\"artist\":\"Sanatçı\",\"album\":\"Albüm\"},"
        "\"version\":{\"_id\":\"22222222-2222-4222-8222-222222222222\",\"media_file_id\":\"33333333-3333-4333-8333-333333333333\","
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"duration_ms\":183000,\"verified\":true},"
        "\"analysis\":{\"rhythm\":{\"bpm\":128.5}}}");
}

/* A verified local media row with its own media/version identity. */
static BrowserRow *song_fixture(guint index, const gchar *title)
{
    JsonObject *record = local_fixture();
    JsonObject *media = object_member(record, "media"), *version = object_member(record, "version");
    gchar *media_id = g_strdup_printf("11111111-1111-4111-8111-1111111111%02u", index);
    gchar *version_id = g_strdup_printf("22222222-2222-4222-8222-2222222222%02u", index);
    json_object_set_string_member(media, "_id", media_id);
    json_object_set_string_member(version, "_id", version_id);
    json_object_set_string_member(media, "title", title);
    BrowserRow *row = local_row(record); g_assert_nonnull(row);
    json_object_unref(record); g_free(media_id); g_free(version_id); return row;
}

static void pump(void)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
}

static BrowserHarness *harness_new(void)
{
    BrowserHarness *h = g_new0(BrowserHarness, 1);
    h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(h->window);
    h->ctx.parent = GTK_WINDOW(h->window);
    gtk_window_set_default_size(GTK_WINDOW(h->window), 1250, 600);
    h->widget = radios_playlist_browser_new(&h->ctx); h->browser = browser_state(h->widget);
    gtk_container_add(GTK_CONTAINER(h->window), h->widget); gtk_widget_show_all(h->window); pump();
    return h;
}

static void harness_free(BrowserHarness *h)
{
    gtk_widget_destroy(h->window); g_object_unref(h->window); pump(); g_free(h);
}

static void fill_rows(BrowserHarness *h)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(row_free);
    g_ptr_array_add(rows, song_fixture(0, "Birinci"));
    g_ptr_array_add(rows, song_fixture(1, "İkinci"));
    g_ptr_array_add(rows, song_fixture(2, "Üçüncü"));
    g_ptr_array_add(rows, song_fixture(3, "Dördüncü"));
    render_rows(h->browser, rows); g_ptr_array_unref(rows); pump();
}

static void press_row(BrowserHarness *h, gint row, GdkModifierType modifiers)
{
    GtkTreePath *path = gtk_tree_path_new_from_indices(row, -1);
    GdkRectangle rect;
    gtk_tree_view_get_background_area(GTK_TREE_VIEW(h->browser->tree), path, NULL, &rect);
    GdkEventButton event = {0};
    event.type = GDK_BUTTON_PRESS; event.button = 1; event.state = modifiers;
    event.x = rect.x + 20; event.y = rect.y + rect.height / 2;
    g_assert_true(row_pressed(h->browser->tree, &event, h->browser));
    gtk_tree_path_free(path);
}

static void release_row(BrowserHarness *h)
{
    GdkEventButton event = {0}; event.type = GDK_BUTTON_RELEASE; event.button = 1;
    g_assert_true(row_released(h->browser->tree, &event, h->browser));
}

static guint selection_count(BrowserHarness *h)
{
    JsonArray *selected = selected_rows(h->browser);
    guint count = json_array_get_length(selected); json_array_unref(selected); return count;
}

static void test_local_binding(void)
{
    JsonObject *record = local_fixture();
    BrowserRow *row = local_row(record); g_assert_nonnull(row);
    g_assert_cmpstr(radios_json_string(row->metadata, "media_id", ""), ==, "11111111-1111-4111-8111-111111111111");
    g_assert_cmpstr(radios_json_string(row->metadata, "version_id", ""), ==, "22222222-2222-4222-8222-222222222222");
    g_assert_cmpstr(radios_json_string(row->metadata, "media_file_id", ""), ==, "33333333-3333-4333-8333-333333333333");
    g_assert_cmpint(radios_json_int(row->metadata, "duration_ms", 0), ==, 183000);
    g_assert_false(json_object_has_member(row->metadata, "provider"));
    g_assert_cmpfloat(json_object_get_double_member(row->metadata, "bpm"), ==, 128.5);
    row_free(row);
    JsonObject *version = object_member(record, "version");
    json_object_set_boolean_member(version, "verified", FALSE);
    g_assert_null(local_row(record));
    json_object_set_boolean_member(version, "verified", TRUE);
    json_object_set_string_member(version, "media_file_id", "absent");
    g_assert_null(local_row(record));
    json_object_unref(record);
    record = local_fixture(); version = object_member(record, "version");
    json_object_set_string_member(version, "sha256", "path-is-not-a-verified-binding");
    g_assert_null(local_row(record)); json_object_unref(record);
}

static void test_alt_shift_selection(void)
{
    BrowserHarness *h = harness_new(); fill_rows(h);
    press_row(h, 0, 0); release_row(h); g_assert_cmpuint(selection_count(h), ==, 1);
    press_row(h, 1, GDK_MOD1_MASK); release_row(h); g_assert_cmpuint(selection_count(h), ==, 2);
    press_row(h, 0, GDK_MOD1_MASK); release_row(h); g_assert_cmpuint(selection_count(h), ==, 1);
    press_row(h, 2, GDK_SHIFT_MASK); release_row(h); g_assert_cmpuint(selection_count(h), ==, 3);
    JsonArray *selected = selected_rows(h->browser);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(selected, 0), "title", ""), ==, "Birinci");
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(selected, 1), "title", ""), ==, "İkinci");
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(selected, 2), "title", ""), ==, "Üçüncü");
    json_array_unref(selected);
    /* A selected row retains the entire multi-selection on press, giving a
     * subsequent drag the whole set. A click released without drag singles it. */
    press_row(h, 0, 0); g_assert_cmpuint(selection_count(h), ==, 3);
    h->browser->dragging = TRUE; release_row(h); g_assert_cmpuint(selection_count(h), ==, 3);
    drag_finished(h->browser->tree, NULL, h->browser);
    press_row(h, 0, 0); release_row(h); g_assert_cmpuint(selection_count(h), ==, 1);
    selected = selected_rows(h->browser);
    json_object_set_string_member(json_array_get_object_element(selected, 0), "title", "Edited copy");
    json_array_unref(selected); selected = selected_rows(h->browser);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(selected, 0), "title", ""), ==, "Birinci");
    json_array_unref(selected);
    /* No selected fixture may be appended without the live supplying identity. */
    selected = radios_playlist_browser_selected(h->widget);
    g_assert_cmpuint(json_array_get_length(selected), ==, 0); json_array_unref(selected);
    harness_free(h);
}

/* A queued search must not fire after the browser is destroyed. */
static void test_cancelled_debounce(void)
{
    BrowserHarness *h = harness_new();
    gtk_entry_set_text(GTK_ENTRY(h->browser->query), "queued search");
    g_assert_cmpuint(h->browser->search_timer, !=, 0);
    harness_free(h);
    gint64 end = g_get_monotonic_time() + 500 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < end) { pump(); g_usleep(1000); }
}

int main(int argc, char **argv)
{
    gtk_test_init(&argc, &argv, NULL);
    g_test_add_func("/playlist-browser/local-verified-binding", test_local_binding);
    g_test_add_func("/playlist-browser/alt-shift-and-drag-selection", test_alt_shift_selection);
    g_test_add_func("/playlist-browser/cancelled-search-debounce", test_cancelled_debounce);
    return g_test_run();
}
