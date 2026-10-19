#include "radios/data.h"
#include "radios/media_ui.h"
#include "radios/editors.h"
#include "radios/media_routing.h"
#include "radios/playlist_browser.h"
#include <glib/gstdio.h>
#include <fontconfig/fontconfig.h>
#include <string.h>

static RadioSConfig config;
static RadioSDataClient *reader;
typedef struct { RadioSViewContext ctx; GtkWidget *window; } Harness;
typedef gboolean (*Condition)(gpointer);
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void wait_for(Condition condition, gpointer value)
{
    gint64 end = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < end) pump();
    g_assert_true(condition(value));
}
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *dialog(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), name)) result = item->data;
    g_list_free(windows); return result;
}
static const gchar *status(void)
{
    GtkWidget *library = dialog("media-library-dialog"); g_assert_nonnull(library);
    GtkWidget *label = find(library, "media-import-status"); g_assert_true(GTK_IS_LABEL(label));
    return gtk_label_get_text(GTK_LABEL(label));
}
static gboolean connected(gpointer value) { return radios_data_connected(&((Harness *)value)->ctx); }
static Harness *open_harness(gboolean connect)
{
    Harness *h = g_new0(Harness, 1); h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(h->window);
    h->ctx = (RadioSViewContext){ .config = &config, .runtime = radios_runtime_new(32, 64), .parent = GTK_WINDOW(h->window) };
    radios_ui_bind_media_drop(h->window, &h->ctx); gtk_widget_show_all(h->window);
    if (connect) { h->ctx.data = radios_data_new(&h->ctx); radios_data_connect(&h->ctx); wait_for(connected, h); }
    return h;
}
static void close_harness(Harness *h)
{
    gtk_widget_destroy(h->window); radios_data_close(h->ctx.data); radios_runtime_free(h->ctx.runtime);
    while (g_main_context_iteration(NULL, FALSE)) {}
    radios_data_unref(h->ctx.data); g_object_unref(h->window); g_free(h);
}
typedef struct { const gchar *path; JsonNode *body, *result; GError *error; gint done; } Request;
static gpointer request_worker(gpointer value)
{
    Request *r = value; r->result = radios_data_client_request_sync(reader, "POST", r->path, r->body, NULL, &r->error);
    g_atomic_int_set(&r->done, 1); return NULL;
}
static gboolean request_done(gpointer value) { return g_atomic_int_get(&((Request *)value)->done); }
static JsonNode *request(const gchar *path, JsonNode *body)
{
    Request work = {.path = path, .body = body}; GThread *thread = g_thread_new("media-drop-read", request_worker, &work);
    wait_for(request_done, &work); g_thread_join(thread); g_assert_no_error(work.error); g_assert_nonnull(work.result); return work.result;
}
static JsonNode *body(void)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
    json_object_set_string_member(json_node_get_object(node), "station_id", radios_data_client_station_id(reader)); return node;
}
static guint media_count(void)
{
    JsonNode *input = body(), *result = request("/v1/workflows/desktop-media/list", input);
    guint count = (guint)radios_json_int(json_node_get_object(result), "total", -1);
    json_node_free(input); json_node_free(result); return count;
}
static guint staged_files(void)
{
    GDir *directory = g_dir_open(config.import_root, 0, NULL); g_assert_nonnull(directory); guint count = 0;
    while (g_dir_read_name(directory)) ++count;
    g_dir_close(directory); return count;
}
static void write16(guchar *p, guint16 n) { p[0] = n & 255; p[1] = n >> 8; }
static void write32(guchar *p, guint32 n) { for (guint i = 0; i < 4; ++i) p[i] = (n >> (8 * i)) & 255; }
static gchar *audio(const gchar *name, guint amplitude)
{
    const guint bytes = 32000; guchar *wav = g_malloc0(44 + bytes);
    memcpy(wav, "RIFF", 4); write32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8); write32(wav + 16, 16);
    write16(wav + 20, 1); write16(wav + 22, 1); write32(wav + 24, 8000); write32(wav + 28, 16000);
    write16(wav + 32, 2); write16(wav + 34, 16); memcpy(wav + 36, "data", 4); write32(wav + 40, bytes);
    for (guint i = 0; i < bytes / 2; ++i) write16(wav + 44 + 2 * i, (guint16)(i % 8 < 4 ? (gint)amplitude : -(gint)amplitude));
    gchar *path = g_build_filename(config.project_root, name, NULL);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL)); g_free(wav); return path;
}
static gchar *sha(const gchar *path)
{
    gchar *bytes = NULL; gsize size = 0; g_assert_true(g_file_get_contents(path, &bytes, &size, NULL));
    gchar *result = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)bytes, size); g_free(bytes); return result;
}
/* A bulk drop (several files or a folder) takes the songs in first: verified
 * bytes, no analysis yet. The library pass analyses them afterwards, as the
 * background upkeep does once the library has been quiet after an import. */
static JsonNode *media_detail(const gchar *title)
{
    JsonNode *input = body(); json_object_set_string_member(json_node_get_object(input), "query", title);
    JsonNode *rows = request("/v1/workflows/desktop-media/list", input); json_node_free(input);
    JsonArray *records = json_object_get_array_member(json_node_get_object(rows), "records");
    g_assert_cmpuint(json_array_get_length(records), ==, 1);
    JsonObject *media = json_object_get_object_member(json_array_get_object_element(records, 0), "media");
    input = body(); json_object_set_string_member(json_node_get_object(input), "media_id", radios_json_string(media, "_id", ""));
    JsonNode *detail = request("/v1/workflows/desktop-media/get", input); json_node_free(input); json_node_free(rows);
    return detail;
}
static void assert_staged(const gchar *title, const gchar *digest)
{
    JsonNode *detail = media_detail(title); JsonObject *object = json_node_get_object(detail);
    JsonObject *version = json_object_get_object_member(object, "version");
    g_assert_cmpstr(radios_json_string(version, "sha256", ""), ==, digest);
    g_assert_true(json_object_get_boolean_member(version, "verified"));
    JsonNode *analysis = json_object_get_member(object, "analysis");
    g_assert_true(!analysis || JSON_NODE_HOLDS_NULL(analysis));
    json_node_free(detail);
}
typedef struct { gboolean done; GError *error; JsonNode *summary; } LibraryPass;
static void library_pass_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; LibraryPass *pass = value;
    if (error) pass->error = g_error_copy(error);
    if (node) pass->summary = json_node_copy(node);
    pass->done = TRUE;
}
static gboolean library_pass_finished(gpointer value) { return ((LibraryPass *)value)->done; }
static gboolean import_batch_idle(gpointer value)
{ return g_object_get_data(G_OBJECT(((Harness *)value)->window), "radios-media-import-batch") == NULL; }
static void analyse_library(Harness *h)
{
    /* Like the background upkeep, never overlap an import batch still running. */
    wait_for(import_batch_idle, h);
    LibraryPass pass = {0};
    GCancellable *cancel = radios_media_rhythm_library_scan_async(&h->ctx, h->window, library_pass_done, &pass, NULL);
    g_assert_nonnull(cancel);
    wait_for(library_pass_finished, &pass); g_assert_no_error(pass.error);
    JsonObject *summary = json_node_get_object(pass.summary);
    if (radios_json_int(summary, "failed", -1) != 0)
        g_test_message("library pass: analysed %" G_GINT64_FORMAT ", failed %" G_GINT64_FORMAT ": %s", radios_json_int(summary, "analyzed", -1),
                       radios_json_int(summary, "failed", -1), radios_json_string(summary, "first_error", ""));
    g_assert_cmpint(radios_json_int(summary, "failed", -1), ==, 0);
    json_node_free(pass.summary); g_object_unref(cancel);
}
static void assert_imported(const gchar *title, const gchar *source, const gchar *digest)
{
    JsonNode *input = body(); json_object_set_string_member(json_node_get_object(input), "query", title);
    JsonNode *rows = request("/v1/workflows/desktop-media/list", input); json_node_free(input);
    JsonArray *records = json_object_get_array_member(json_node_get_object(rows), "records");
    g_assert_cmpuint(json_array_get_length(records), ==, 1);
    JsonObject *media = json_object_get_object_member(json_array_get_object_element(records, 0), "media");
    g_assert_cmpstr(radios_json_string(media, "title", ""), ==, title);
    input = body(); json_object_set_string_member(json_node_get_object(input), "media_id", radios_json_string(media, "_id", ""));
    JsonNode *detail = request("/v1/workflows/desktop-media/get", input); json_node_free(input);
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    JsonObject *analysis = json_object_get_object_member(json_node_get_object(detail), "analysis");
    g_assert_cmpstr(radios_json_string(version, "sha256", ""), ==, digest);
    g_assert_cmpstr(radios_json_string(analysis, "sha256", ""), ==, digest);
    g_assert_cmpint(radios_json_int(analysis, "duration_ms", 0), ==, 2000);
    g_assert_cmpstr(radios_json_string(version, "status", ""), ==, "draft");
    g_assert_true(json_object_get_boolean_member(version, "verified"));
    const gchar *managed = radios_json_string(version, "managed_path", "");
    g_assert_true(g_file_test(managed, G_FILE_TEST_IS_REGULAR)); g_assert_cmpstr(managed, !=, source);
    gchar *managed_sha = sha(managed), *source_sha = sha(source);
    g_assert_cmpstr(managed_sha, ==, digest); g_assert_cmpstr(source_sha, ==, digest);
    g_free(managed_sha); g_free(source_sha); json_node_free(rows); json_node_free(detail);
}
typedef struct { const guchar *data; gint length; } ClipboardPayload;
static void clipboard_get(GtkClipboard *clipboard, GtkSelectionData *selection, guint info, gpointer value)
{
    (void)clipboard; (void)info; ClipboardPayload *payload = value;
    gtk_selection_data_set(selection, gdk_atom_intern_static_string("text/uri-list"), 8, payload->data, payload->length);
}
static void drop_bytes(GtkWidget *target, const guchar *bytes, gint length)
{
    static const GtkTargetEntry targets[] = {{"text/uri-list", 0, 0}};
    ClipboardPayload payload = {bytes, length}; GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    g_assert_true(gtk_clipboard_set_with_data(clipboard, targets, G_N_ELEMENTS(targets), clipboard_get, NULL, &payload));
    GtkSelectionData *selection = gtk_clipboard_wait_for_contents(clipboard, gdk_atom_intern_static_string("text/uri-list"));
    g_assert_nonnull(selection);
    g_signal_emit_by_name(target, "drag-data-received", NULL, 0, 0, selection, 0, GDK_CURRENT_TIME);
    gtk_selection_data_free(selection); gtk_clipboard_clear(clipboard);
}
static void drop(GtkWidget *target, const gchar *const *uris)
{
    GString *text = g_string_new(NULL);
    for (guint i = 0; uris[i]; ++i) g_string_append_printf(text, "%s\r\n", uris[i]);
    drop_bytes(target, (const guchar *)text->str, (gint)text->len); g_string_free(text, TRUE);
}
static gboolean import_finished(gpointer value)
{
    GtkWidget *button = find(value, "media-import");
    return button && gtk_widget_is_sensitive(button) && strstr(status(), "İçe aktarma tamamlandı");
}
/* A window drop never opens a page: the library is built unseen. */
static GtkWidget *hidden_library(void)
{
    GtkWidget *library = dialog("media-library-dialog"); g_assert_nonnull(library);
    g_assert_false(gtk_widget_get_visible(library)); g_assert_null(dialog("media-playlist-add-dialog")); return library;
}
static gchar *media_id(const gchar *title)
{
    JsonNode *input = body(); json_object_set_string_member(json_node_get_object(input), "query", title);
    JsonNode *rows = request("/v1/workflows/desktop-media/list", input); json_node_free(input);
    JsonArray *records = json_object_get_array_member(json_node_get_object(rows), "records");
    if (json_array_get_length(records) != 1) {
        /* Diagnose a missing title: list what the library holds instead. */
        JsonNode *all = request("/v1/workflows/desktop-media/list", body());
        JsonArray *every = json_object_get_array_member(json_node_get_object(all), "records");
        for (guint i = 0; i < json_array_get_length(every); ++i)
            g_test_message("library: %s", radios_json_string(json_object_get_object_member(json_array_get_object_element(every, i), "media"), "title", ""));
        json_node_free(all);
    }
    g_assert_cmpuint(json_array_get_length(records), ==, 1);
    JsonObject *media = json_object_get_object_member(json_array_get_object_element(records, 0), "media");
    gchar *identifier = g_strdup(radios_json_string(media, "_id", "")); json_node_free(rows); return identifier;
}
static gboolean inline_ready(gpointer value)
{
    GtkWidget *panel = find(value, "media-marker-panel");
    return panel && radios_marker_editor_detail(panel) != NULL && find(panel, "qwf-timeline") != NULL && gtk_widget_is_sensitive(find(value, "media-tags-save"));
}
static void assert_inline_design(GtkWidget *library)
{
    GtkWidget *panel = find(library, "media-marker-panel"); g_assert_nonnull(panel);
    const gchar *fields[] = {"qwf-duration", "qwf-cue-in", "qwf-intro", "qwf-out", "qwf-fade-start", "qwf-fade-end", "qwf-auto-next", "qwf-overlap"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) {
        g_assert_true(GTK_IS_ENTRY(find(panel, fields[i]))); g_assert_false(gtk_widget_get_visible(find(panel, fields[i])));
    }
    g_assert_false(gtk_editable_get_editable(GTK_EDITABLE(find(panel, "qwf-duration"))));
    g_assert_cmpfloat(g_ascii_strtod(gtk_entry_get_text(GTK_ENTRY(find(panel, "qwf-duration"))), NULL), ==, 2.0);
    g_assert_true(GTK_IS_TOGGLE_BUTTON(find(panel, "qwf-automatic")));
    const guint marker_indices[] = {2, 4, 5, 6, 8, 9};
    for (guint i = 0; i < G_N_ELEMENTS(marker_indices); ++i) {
        gchar *name = g_strdup_printf("qwf-set-%u", marker_indices[i]); g_assert_true(GTK_IS_BUTTON(find(panel, name))); g_free(name);
    }
    const gchar *controls[] = {"qwf-preview-play", "qwf-preview-pause", "qwf-preview-stop", "qwf-preview-cue", "qwf-markers-save", "qwf-selection-in", "qwf-selection-out", "qwf-crop-selection"};
    for (guint i = 0; i < G_N_ELEMENTS(controls); ++i) g_assert_true(GTK_IS_BUTTON(find(panel, controls[i])));
    g_assert_null(find(panel, "qwf-media-info"));
    g_assert_true(GTK_IS_COMBO_BOX(find(panel, "qwf-preview-output")));
    g_assert_nonnull(g_object_get_data(G_OBJECT(find(panel, "qwf-preview-gain")), "adjustment"));
    g_assert_true(GTK_IS_SPIN_BUTTON(find(panel, "qwf-overlap-seconds")));
    g_assert_null(find(library, "media-audio-deck")); g_assert_null(find(library, "media-audio-load"));
    g_assert_null(find(library, "media-audio-preview")); g_assert_null(find(library, "media-audio-preview-stop"));
    g_assert_null(dialog("queue-waveform-dialog"));
    GtkWidget *timeline = find(panel, "qwf-timeline");
    JsonNode *analysis = g_object_get_data(G_OBJECT(timeline), "radios-waveform-analysis"); g_assert_nonnull(analysis);
    JsonObject *measured = json_node_get_object(analysis);
    g_assert_cmpint(radios_json_int(measured, "duration_ms", 0), ==, 2000);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(measured, "waveform")), >, 0);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(find(library, "media-table")));
    g_assert_cmpint(gtk_tree_selection_get_mode(selection), ==, GTK_SELECTION_MULTIPLE);
}
static void capture_library(GtkWidget *library, const gchar *filename)
{
    const gchar *directory = g_getenv("RADIOS_P11_SCREENSHOT_ROOT"); if (!directory || !*directory) return;
    for (guint i = 0; i < 40; ++i) pump();
    GdkWindow *window = gtk_widget_get_window(library); g_assert_nonnull(window);
    GdkPixbuf *pixels = gdk_pixbuf_get_from_window(window, 0, 0, gdk_window_get_width(window), gdk_window_get_height(window));
    g_assert_nonnull(pixels); gchar *path = g_build_filename(directory, filename, NULL); GError *error = NULL;
    g_assert_true(gdk_pixbuf_save(pixels, path, "png", &error, NULL)); g_assert_no_error(error); g_free(path); g_object_unref(pixels);
}
static void capture_library_section(GtkWidget *library, const gchar *section, const gchar *filename)
{
    if (!g_getenv("RADIOS_P11_SCREENSHOT_ROOT")) return;
    GtkWidget *scroll = find(library, "media-library-scroll");
    GtkAdjustment *vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    gtk_adjustment_set_value(vertical, gtk_adjustment_get_lower(vertical));
    for (guint i = 0; i < 40; ++i) pump();
    gint x = 0, y = 0; g_assert_true(gtk_widget_translate_coordinates(find(library, section), scroll, 0, 0, &x, &y));
    gtk_adjustment_set_value(vertical, MAX(0, y - 20)); capture_library(library, filename);
}
static void inline_screenshot(GtkWidget *library)
{
    const gchar *directory = g_getenv("RADIOS_P11_SCREENSHOT_ROOT"); if (!directory || !*directory) return;
    GtkWidget *scroll = find(library, "media-library-scroll"); g_assert_true(GTK_IS_SCROLLED_WINDOW(scroll));
    GtkAdjustment *vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    gtk_adjustment_set_value(vertical, gtk_adjustment_get_lower(vertical)); capture_library(library, "library-inline-top.png");
    capture_library_section(library, "media-marker-panel", "library-inline-markers.png");
    capture_library_section(library, "media-playlist-routing", "library-inline-routing.png");
    gtk_adjustment_set_value(vertical, gtk_adjustment_get_lower(vertical));
}
static void test_window_multi_copy(void)
{
    Harness *h = open_harness(TRUE); guint before = media_count(), staged = staged_files();
    gchar *first = audio("sürükle yüzde % bir.wav", 12000), *second = audio("sürükle çoklu iki.wav", 14000);
    gchar *first_sha = sha(first), *second_sha = sha(second);
    gchar *first_uri = g_filename_to_uri(first, NULL, NULL), *second_uri = g_filename_to_uri(second, NULL, NULL);
    const gchar *uris[] = { first_uri, second_uri, NULL }; drop(h->window, uris);
    GtkWidget *library = hidden_library();
    wait_for(import_finished, library); g_assert_false(gtk_widget_get_visible(library));
    g_assert_cmpuint(media_count(), ==, before + 2); g_assert_cmpuint(staged_files(), ==, staged);
    g_assert_nonnull(strstr(status(), "2 kayıt doğrulandı")); g_assert_nonnull(strstr(status(), "0 sonuç doğrulanamadı"));
    assert_staged("sürükle yüzde % bir", first_sha); assert_staged("sürükle çoklu iki", second_sha);
    analyse_library(h);
    assert_imported("sürükle yüzde % bir", first, first_sha); assert_imported("sürükle çoklu iki", second, second_sha);
    gchar *identifier = media_id("sürükle yüzde % bir"); radios_media_library_open_selected(&h->ctx, identifier); g_free(identifier);
    wait_for(inline_ready, library); assert_inline_design(library); inline_screenshot(library);
    close_harness(h); g_unlink(first); g_unlink(second);
    g_free(first); g_free(second); g_free(first_sha); g_free(second_sha); g_free(first_uri); g_free(second_uri);
}
static gboolean inline_saved(gpointer value)
{
    GtkWidget *panel = find(value, "media-marker-panel"), *label = panel ? find(panel, "editor-status") : NULL;
    return label && strstr(gtk_label_get_text(GTK_LABEL(label)), "kaydedildi") != NULL &&
        !radios_marker_editor_dirty(panel);
}
static void test_inline_marker_persistence(void)
{
    Harness *h = open_harness(TRUE); gchar *path = audio("inline işaret kayıt.wav", 11500), *uri = g_filename_to_uri(path, NULL, NULL);
    gchar *other = audio("inline başka kayıt.wav", 16500), *other_uri = g_filename_to_uri(other, NULL, NULL);
    const gchar *uris[] = {uri, other_uri, NULL}; radios_media_library_open(&h->ctx); GtkWidget *library = dialog("media-library-dialog"); drop(library, uris); wait_for(import_finished, library);
    analyse_library(h); /* the two dropped songs are a bulk import: measured by the library pass */
    gchar *identifier = media_id("inline işaret kayıt"), *other_id = media_id("inline başka kayıt");
    wait_for(inline_ready, library); radios_media_library_open_selected(&h->ctx, identifier); wait_for(inline_ready, library);
    assert_inline_design(library); GtkWidget *panel = find(library, "media-marker-panel");
    const gchar *fields[] = {"qwf-cue-in", "qwf-intro", "qwf-out", "qwf-fade-start", "qwf-fade-end", "qwf-auto-next", "qwf-overlap"};
    const gchar *values[] = {"0.1", "0.3", "1.9", "1.6", "1.9", "1.8", "0.1"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) gtk_entry_set_text(GTK_ENTRY(find(panel, fields[i])), values[i]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(panel, "qwf-automatic")), TRUE); g_assert_true(radios_marker_editor_dirty(panel));
    radios_media_library_open_selected(&h->ctx, other_id);
    g_assert_true(find(library, "media-marker-panel") == panel);
    g_assert_cmpstr(radios_json_string(json_object_get_object_member(radios_marker_editor_detail(panel), "media"), "_id", ""), ==, identifier);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(panel, "qwf-cue-in"))), ==, "0.1");
    gtk_button_clicked(GTK_BUTTON(find(panel, "qwf-markers-save"))); wait_for(inline_saved, library);
    JsonNode *input = body(); json_object_set_string_member(json_node_get_object(input), "media_id", identifier);
    JsonNode *detail = request("/v1/workflows/desktop-media/get", input); json_node_free(input);
    JsonObject *markers = json_object_get_object_member(json_node_get_object(detail), "markers"); g_assert_nonnull(markers);
    const gchar *keys[] = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms"};
    const gint64 expected[] = {100, 300, 1900, 1600, 1900, 1800, 100};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) g_assert_cmpint(radios_json_int(markers, keys[i], -1), ==, expected[i]);
    g_assert_true(json_object_get_boolean_member(markers, "automatic"));
    gint64 marker_revision = radios_json_int(json_object_get_object_member(json_node_get_object(detail), "media"), "_rev", -1); json_node_free(detail);
    gtk_entry_set_text(GTK_ENTRY(find(library, "media-title")), "İşaret sonrası güncel CAS etiket");
    gtk_button_clicked(GTK_BUTTON(find(library, "media-tags-save")));
    wait_for(inline_ready, library); g_assert_null(dialog("media-reload-dialog"));
    input = body(); json_object_set_string_member(json_node_get_object(input), "media_id", identifier);
    detail = request("/v1/workflows/desktop-media/get", input); json_node_free(input);
    JsonObject *saved_media = json_object_get_object_member(json_node_get_object(detail), "media");
    g_assert_cmpstr(radios_json_string(saved_media, "title", ""), ==, "İşaret sonrası güncel CAS etiket");
    g_assert_cmpint(radios_json_int(saved_media, "_rev", -2), >, marker_revision);
    markers = json_object_get_object_member(json_node_get_object(detail), "markers");
    g_assert_cmpint(radios_json_int(markers, "cue_in_ms", -1), ==, 100); json_node_free(detail);
    gtk_dialog_response(GTK_DIALOG(library), GTK_RESPONSE_CLOSE); radios_media_library_open_selected(&h->ctx, identifier);
    library = dialog("media-library-dialog"); wait_for(inline_ready, library);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(library, "media-title"))), ==, "İşaret sonrası güncel CAS etiket");
    g_assert_cmpfloat(g_ascii_strtod(gtk_entry_get_text(GTK_ENTRY(find(library, "qwf-cue-in"))), NULL), ==, 0.1);
    close_harness(h); g_unlink(path); g_unlink(other); g_free(identifier); g_free(other_id); g_free(path); g_free(other); g_free(uri); g_free(other_uri);
}
static gchar *create_playlist(const gchar *name)
{
    JsonNode *input = body(); JsonObject *object = json_node_get_object(input); gchar *key = g_uuid_string_random();
    json_object_set_string_member(object, "name", name); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonNode *saved = request("/v1/workflows/desktop-playlists/create", input); json_node_free(input);
    gchar *identifier = g_strdup(radios_json_string(json_node_get_object(saved), "_id", "")); json_node_free(saved); return identifier;
}
static JsonNode *playlist_snapshot(const gchar *identifier)
{
    JsonNode *input = body(), *listed = request("/v1/workflows/desktop-playlists/list", input), *result = NULL; json_node_free(input);
    JsonArray *playlists = json_object_get_array_member(json_node_get_object(listed), "playlists");
    for (guint i = 0; i < json_array_get_length(playlists); ++i) {
        JsonObject *row = json_array_get_object_element(playlists, i);
        if (!g_strcmp0(radios_json_string(row, "_id", ""), identifier)) {
            result = json_node_new(JSON_NODE_OBJECT); json_node_set_object(result, row); break;
        }
    }
    g_assert_nonnull(result); json_node_free(listed); return result;
}
static void assert_playlist(const gchar *identifier, const gchar *const *expected, guint count)
{
    JsonNode *snapshot = playlist_snapshot(identifier); JsonArray *tracks = json_object_get_array_member(json_node_get_object(snapshot), "tracks");
    g_assert_cmpuint(json_array_get_length(tracks), ==, count);
    for (guint i = 0; i < count; ++i) {
        JsonObject *track = NULL;
        for (guint j = 0; j < count; ++j) {
            JsonObject *candidate = json_array_get_object_element(tracks, j);
            if (!g_strcmp0(radios_json_string(candidate, "media_id", ""), expected[i])) { g_assert_null(track); track = candidate; }
        }
        g_assert_nonnull(track);
        JsonNode *input = body(); json_object_set_string_member(json_node_get_object(input), "media_id", expected[i]);
        JsonNode *detail = request("/v1/workflows/desktop-media/get", input); json_node_free(input);
        JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
        g_assert_cmpstr(radios_json_string(track, "version_id", ""), ==, radios_json_string(version, "_id", ""));
        g_assert_cmpstr(radios_json_string(track, "media_file_id", ""), ==, radios_json_string(version, "media_file_id", ""));
        g_assert_cmpstr(radios_json_string(track, "media_sha256", ""), ==, radios_json_string(version, "sha256", ""));
        g_assert_cmpint(radios_json_int(track, "duration_ms", 0), ==, 2000); json_node_free(detail);
    }
    json_node_free(snapshot);
}
static void select_media(GtkWidget *library, const gchar *first, const gchar *second)
{
    GtkTreeView *tree = GTK_TREE_VIEW(find(library, "media-table")); GtkTreeSelection *selection = gtk_tree_view_get_selection(tree);
    GtkTreeModel *model = gtk_tree_view_get_model(tree); GtkTreeIter iter; guint selected = 0;
    gtk_tree_selection_unselect_all(selection); gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        gchar *identifier = NULL; gtk_tree_model_get(model, &iter, 4, &identifier, -1);
        if (!g_strcmp0(identifier, first) || (second && !g_strcmp0(identifier, second))) { gtk_tree_selection_select_iter(selection, &iter); ++selected; }
        g_free(identifier); valid = gtk_tree_model_iter_next(model, &iter);
    }
    g_assert_cmpuint(selected, ==, second ? 2 : 1); g_assert_cmpint(gtk_tree_selection_count_selected_rows(selection), ==, (gint)selected);
}
static GtkWidget *target_for(GtkWidget *library, const gchar *identifier)
{
    for (guint i = 0; i < 2048; ++i) {
        gchar *name = g_strdup_printf("media-playlist-choice-%u", i); GtkWidget *check = find(library, name); g_free(name);
        if (!check) break;
        if (!g_strcmp0(g_object_get_data(G_OBJECT(check), "playlist-id"), identifier)) return check;
    }
    return NULL;
}
static GtkWidget *membership_for(GtkWidget *library, const gchar *identifier)
{
    for (guint i = 0; i < 256; ++i) {
        gchar *name = g_strdup_printf("media-playlist-membership-%u", i); GtkWidget *label = find(library, name); g_free(name);
        if (!label) break;
        if (!g_strcmp0(g_object_get_data(G_OBJECT(label), "media-id"), identifier)) return label;
    }
    return NULL;
}
static gboolean routes_ready(gpointer value)
{
    GtkWidget *first = find(value, "media-playlist-choice-0"), *second = find(value, "media-playlist-choice-1");
    return first && second && gtk_widget_is_sensitive(first) && gtk_widget_is_sensitive(second);
}
static gboolean routes_finished(gpointer value)
{
    GtkWidget *panel = find(value, "media-playlist-routing"), *apply = find(value, "media-playlist-route-apply");
    /* Refreshes may replace the informational status after a completed save.
     * The controls/operation state are stable; actual DB membership is asserted below. */
    return panel && apply && !radios_media_routing_busy(panel) && gtk_widget_is_sensitive(apply);
}
static gboolean routes_uncertain(gpointer value)
{
    GtkWidget *panel = find(value, "media-playlist-routing"), *apply = find(value, "media-playlist-route-apply");
    const gchar *text = gtk_label_get_text(GTK_LABEL(find(value, "media-playlist-route-status")));
    return panel && apply && radios_media_routing_busy(panel) && gtk_widget_is_sensitive(apply) && strstr(text, "yeniden denemek");
}
static void assert_inside_scroller(GtkWidget *widget, GtkWidget *scroll)
{
    gint x = 0, y = 0; g_assert_true(gtk_widget_translate_coordinates(widget, scroll, 0, 0, &x, &y));
    g_assert_cmpint(gtk_widget_get_allocated_height(widget), >, 1); g_assert_cmpint(y, >=, 0);
    g_assert_cmpint(y + gtk_widget_get_allocated_height(widget), <=, gtk_widget_get_allocated_height(scroll));
}
static void test_multiple_playlist_routing(void)
{
    Harness *h = open_harness(TRUE); gchar *one = create_playlist("Prompt11 rota bir"), *two = create_playlist("Prompt11 rota iki"), *both = create_playlist("Prompt11 rota ortak");
    gchar *first = audio("route alpha.wav", 12500), *second = audio("route beta.wav", 15500);
    gchar *first_uri = g_filename_to_uri(first, NULL, NULL), *second_uri = g_filename_to_uri(second, NULL, NULL);
    const gchar *uris[] = {first_uri, second_uri, NULL}; radios_media_library_open(&h->ctx); GtkWidget *library = dialog("media-library-dialog"); drop(library, uris); wait_for(import_finished, library);
    analyse_library(h); /* bulk drop: the library pass measures the songs */
    gchar *first_id = media_id("route alpha"), *second_id = media_id("route beta");
    radios_media_library_open_selected(&h->ctx, first_id); wait_for(inline_ready, library); assert_inline_design(library);
    select_media(library, first_id, second_id); wait_for(routes_ready, library);
    GtkWidget *apply = find(library, "media-playlist-route-apply"); g_assert_true(GTK_IS_BUTTON(apply)); g_assert_false(gtk_widget_is_sensitive(apply));
    g_assert_null(find(library, "media-playlist-target-all")); g_assert_null(find(library, "media-playlists-refresh"));
    assert_playlist(one, NULL, 0); assert_playlist(two, NULL, 0); assert_playlist(both, NULL, 0);
    GtkWidget *first_membership = membership_for(library, first_id), *second_membership = membership_for(library, second_id);
    GtkWidget *first_target = target_for(library, one), *second_target = target_for(library, two);
    g_assert_true(GTK_IS_CHECK_BUTTON(first_target)); g_assert_true(GTK_IS_CHECK_BUTTON(second_target));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(first_membership)), "Hiçbir playlistte değil"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(first_target), TRUE); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(second_target), TRUE);
    g_assert_true(gtk_widget_is_sensitive(apply));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(library, "media-playlist-route-summary"))), "2 parça · 2 playlist"));
    for (guint i = 0; i < 40; ++i) pump();
    assert_inside_scroller(find(library, "media-playlist-choice-0"), find(library, "media-playlist-choices-scroll"));
    assert_inside_scroller(first_membership, find(library, "media-playlist-membership-scroll"));
    assert_inside_scroller(second_membership, find(library, "media-playlist-membership-scroll"));
    capture_library_section(library, "media-playlist-routing", "library-multiple-routing.png");
    /* Lose the third response after two confirmed writes. Retrying must reuse
     * the committed third receipt and then finish the fourth assignment. */
    gchar *directory = g_path_get_dirname(config.database_file), *gate = g_build_filename(directory, ".acceptance-playlist-drop", NULL);
    g_assert_true(g_file_set_contents(gate, "2", -1, NULL));
    gtk_button_clicked(GTK_BUTTON(apply)); wait_for(routes_uncertain, library);
    g_assert_false(g_file_test(gate, G_FILE_TEST_EXISTS));
    JsonNode *one_before_retry = playlist_snapshot(one), *two_before_retry = playlist_snapshot(two);
    guint confirmed_lists = 0, committed_lists = 0;
    JsonNode *partial[] = {one_before_retry, two_before_retry};
    for (guint i = 0; i < G_N_ELEMENTS(partial); ++i) {
        guint count = json_array_get_length(json_object_get_array_member(json_node_get_object(partial[i]), "tracks"));
        if (count == 2) ++confirmed_lists;
        if (count == 1) ++committed_lists;
    }
    g_assert_cmpuint(confirmed_lists, ==, 1); g_assert_cmpuint(committed_lists, ==, 1);
    g_assert_false(gtk_widget_is_sensitive(target_for(library, one))); g_assert_false(gtk_widget_is_sensitive(target_for(library, two)));
    gtk_button_clicked(GTK_BUTTON(apply)); wait_for(routes_finished, library);
    const gchar *retry_targets[] = {one, two};
    for (guint i = 0; i < G_N_ELEMENTS(partial); ++i) {
        JsonNode *after_retry = playlist_snapshot(retry_targets[i]);
        guint count = json_array_get_length(json_object_get_array_member(json_node_get_object(partial[i]), "tracks"));
        g_assert_cmpint(radios_json_int(json_node_get_object(after_retry), "_rev", -1), ==,
            radios_json_int(json_node_get_object(partial[i]), "_rev", -2) + (count == 1 ? 1 : 0));
        if (count == 2) g_assert_true(json_node_equal(partial[i], after_retry));
        json_node_free(after_retry); json_node_free(partial[i]);
    }
    g_free(gate); g_free(directory);
    const gchar *both_expected[] = {first_id, second_id};
    assert_playlist(one, both_expected, 2); assert_playlist(two, both_expected, 2); assert_playlist(both, NULL, 0);
    first_membership = membership_for(library, first_id); second_membership = membership_for(library, second_id);
    g_assert_nonnull(first_membership); g_assert_nonnull(second_membership);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(first_membership)), "Prompt11 rota bir"));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(second_membership)), "Prompt11 rota iki"));
    /* Successful writes trigger reloads, keeping the multiple target choices. */
    wait_for(routes_ready, library);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(target_for(library, one))));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(target_for(library, two))));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(target_for(library, both)), TRUE);
    gtk_button_clicked(GTK_BUTTON(apply)); wait_for(routes_finished, library); assert_playlist(both, both_expected, 2);
    JsonNode *before[] = {playlist_snapshot(one), playlist_snapshot(two), playlist_snapshot(both)};
    wait_for(routes_ready, library); gtk_button_clicked(GTK_BUTTON(apply)); wait_for(routes_finished, library);
    const gchar *targets[] = {one, two, both};
    for (guint i = 0; i < G_N_ELEMENTS(targets); ++i) {
        assert_playlist(targets[i], both_expected, 2); JsonNode *after = playlist_snapshot(targets[i]);
        g_assert_true(json_node_equal(before[i], after)); json_node_free(after); json_node_free(before[i]);
    }
    /* Media selection changes retain the checked playlists and refresh membership. */
    select_media(library, first_id, NULL); wait_for(routes_ready, library);
    for (guint i = 0; i < G_N_ELEMENTS(targets); ++i) g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(target_for(library, targets[i]))));
    select_media(library, first_id, second_id); wait_for(routes_ready, library);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(membership_for(library, first_id))), "Prompt11 rota ortak"));
    close_harness(h); g_unlink(first); g_unlink(second); g_free(first); g_free(second); g_free(first_uri); g_free(second_uri);
    g_free(first_id); g_free(second_id); g_free(one); g_free(two); g_free(both);
}
typedef struct { GtkWidget *tree; const gchar *title; GtkTreePath *path; } SourcePick;
static gboolean source_row_ready(gpointer value)
{
    SourcePick *pick = value; GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(pick->tree)); GtkTreeIter iter;
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid; valid = gtk_tree_model_iter_next(model, &iter)) {
        gchar *title = NULL; gtk_tree_model_get(model, &iter, 1, &title, -1); gboolean same = !g_strcmp0(title, pick->title); g_free(title);
        if (same) { g_clear_pointer(&pick->path, gtk_tree_path_free); pick->path = gtk_tree_model_get_path(model, &iter); return TRUE; }
    }
    return FALSE;
}
static void source_button(GtkWidget *tree, GtkTreePath *path, GdkEventType type, GdkModifierType modifiers)
{
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(tree), path, NULL, FALSE, 0, 0); for (guint i = 0; i < 20; ++i) pump();
    GdkRectangle rect; gtk_tree_view_get_background_area(GTK_TREE_VIEW(tree), path, NULL, &rect);
    GdkEvent *event = gdk_event_new(type); event->button.window = g_object_ref(gtk_tree_view_get_bin_window(GTK_TREE_VIEW(tree)));
    event->button.send_event = TRUE; event->button.button = 1; event->button.state = modifiers; event->button.time = GDK_CURRENT_TIME;
    event->button.x = rect.x + 20; event->button.y = rect.y + rect.height / 2;
    gdk_event_set_device(event, gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(tree))));
    gboolean handled = FALSE; g_signal_emit_by_name(tree, type == GDK_BUTTON_PRESS ? "button-press-event" : "button-release-event", event, &handled);
    g_assert_true(handled); gdk_event_free(event); for (guint i = 0; i < 20; ++i) pump();
}
static void source_click(GtkWidget *tree, GtkTreePath *path, GdkModifierType modifiers)
{
    source_button(tree, path, GDK_BUTTON_PRESS, modifiers); g_assert_null(dialog("media-library-dialog"));
    source_button(tree, path, GDK_BUTTON_RELEASE, modifiers);
}
typedef struct { GdkDragContext *context; gboolean finished; } SourceDrag;
static void source_drag_started(GtkWidget *tree, GdkDragContext *context, gpointer value)
{ (void)tree; SourceDrag *captured = value; g_assert_null(captured->context); captured->context = g_object_ref(context); }
static void source_drag_finished(GtkWidget *tree, GdkDragContext *context, gpointer value)
{ (void)tree; (void)context; ((SourceDrag *)value)->finished = TRUE; }
static gboolean source_drag_done(gpointer value) { return ((SourceDrag *)value)->finished; }
static void source_drag(GtkWidget *tree, GtkTreePath *path)
{
    SourceDrag captured = {0}; gulong started = g_signal_connect(tree, "drag-begin", G_CALLBACK(source_drag_started), &captured);
    gulong finished = g_signal_connect(tree, "drag-end", G_CALLBACK(source_drag_finished), &captured);
    source_button(tree, path, GDK_BUTTON_PRESS, 0); g_assert_null(dialog("media-library-dialog"));
    GdkRectangle rect; gtk_tree_view_get_background_area(GTK_TREE_VIEW(tree), path, NULL, &rect);
    GdkEvent *event = gdk_event_new(GDK_MOTION_NOTIFY); event->motion.window = g_object_ref(gtk_tree_view_get_bin_window(GTK_TREE_VIEW(tree)));
    event->motion.send_event = TRUE; event->motion.time = GDK_CURRENT_TIME; event->motion.state = GDK_BUTTON1_MASK;
    event->motion.x = rect.x + 100; event->motion.y = rect.y + rect.height / 2;
    gdk_event_set_device(event, gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(tree))));
    gboolean handled = FALSE; g_signal_emit_by_name(tree, "motion-notify-event", event, &handled);
    g_assert_true(handled); g_assert_nonnull(captured.context); gdk_event_free(event);
    source_button(tree, path, GDK_BUTTON_RELEASE, 0); g_assert_null(dialog("media-library-dialog"));
    gtk_drag_cancel(captured.context); wait_for(source_drag_done, &captured);
    g_signal_handler_disconnect(tree, started); g_signal_handler_disconnect(tree, finished); g_object_unref(captured.context);
}
typedef struct { const gchar *id; GtkWidget *library; } SourceEditor;
static gboolean source_editor_ready(gpointer value)
{
    SourceEditor *expected = value; GtkWidget *library = dialog("media-library-dialog");
    if (!library || !gtk_widget_get_visible(library) || !inline_ready(library)) return FALSE;
    JsonObject *detail = radios_marker_editor_detail(find(library, "media-marker-panel"));
    if (g_strcmp0(radios_json_string(json_object_get_object_member(detail, "media"), "_id", ""), expected->id)) return FALSE;
    expected->library = library; return TRUE;
}
static void test_playlist_source_editor_click(void)
{
    Harness *h = open_harness(TRUE);
    gchar *paths[] = {audio("source editor alpha.wav", 6200), audio("source editor beta.wav", 7100), audio("source editor gamma.wav", 8300)};
    gchar *uris[] = {g_filename_to_uri(paths[0], NULL, NULL), g_filename_to_uri(paths[1], NULL, NULL), g_filename_to_uri(paths[2], NULL, NULL), NULL};
    drop(h->window, (const gchar *const *)uris); GtkWidget *import = hidden_library(); wait_for(import_finished, import);
    analyse_library(h); /* bulk drop: the library pass measures the songs */
    gchar *ids[] = {media_id("source editor alpha"), media_id("source editor beta"), media_id("source editor gamma")};
    gtk_dialog_response(GTK_DIALOG(import), GTK_RESPONSE_CLOSE); g_assert_null(dialog("media-library-dialog"));
    GtkWidget *page = radios_playlist_new(&h->ctx); gtk_container_add(GTK_CONTAINER(h->window), page);
    gtk_window_set_default_size(GTK_WINDOW(h->window), 1200, 900); gtk_widget_show_all(h->window);
    GtkWidget *tree = find(page, "pw-source-table"), *browser = find(page, "pw-source-browser"), *query = find(page, "pw-source-search");
    g_assert_true(GTK_IS_TREE_VIEW(tree)); gtk_entry_set_text(GTK_ENTRY(query), "source editor"); g_signal_emit_by_name(query, "activate");
    SourcePick picks[] = {{tree, "source editor alpha", NULL}, {tree, "source editor beta", NULL}, {tree, "source editor gamma", NULL}};
    for (guint i = 0; i < G_N_ELEMENTS(picks); ++i) wait_for(source_row_ready, &picks[i]);
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(tree)), NULL), ==, 3);
    source_click(tree, picks[0].path, GDK_CONTROL_MASK); g_assert_null(dialog("media-library-dialog"));
    source_click(tree, picks[1].path, GDK_CONTROL_MASK); g_assert_null(dialog("media-library-dialog"));
    JsonArray *selected = radios_playlist_browser_selected(browser); g_assert_cmpuint(json_array_get_length(selected), ==, 2); json_array_unref(selected);
    source_click(tree, picks[2].path, GDK_SHIFT_MASK); g_assert_null(dialog("media-library-dialog"));
    selected = radios_playlist_browser_selected(browser); g_assert_cmpuint(json_array_get_length(selected), ==, 2); json_array_unref(selected);
    source_click(tree, picks[0].path, GDK_CONTROL_MASK);
    selected = radios_playlist_browser_selected(browser); g_assert_cmpuint(json_array_get_length(selected), ==, 3); json_array_unref(selected);
    source_drag(tree, picks[1].path);
    selected = radios_playlist_browser_selected(browser); g_assert_cmpuint(json_array_get_length(selected), ==, 3); json_array_unref(selected);
    /* Opening is deferred until release; plain click collapses the selected
     * set and supplies the exact clicked media binding to the real editor. */
    source_click(tree, picks[1].path, 0); SourceEditor expected = {ids[1], NULL}; wait_for(source_editor_ready, &expected);
    assert_inline_design(expected.library); g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(expected.library, "media-title"))), ==, "source editor beta");
    selected = radios_playlist_browser_selected(browser); g_assert_cmpuint(json_array_get_length(selected), ==, 1);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(selected, 0), "media_id", ""), ==, ids[1]); json_array_unref(selected);
    gtk_dialog_response(GTK_DIALOG(expected.library), GTK_RESPONSE_CLOSE);
    source_click(tree, picks[0].path, 0); expected = (SourceEditor){ids[0], NULL}; wait_for(source_editor_ready, &expected);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(expected.library, "media-title"))), ==, "source editor alpha");
    close_harness(h);
    for (guint i = 0; i < G_N_ELEMENTS(paths); ++i) { gtk_tree_path_free(picks[i].path); g_unlink(paths[i]); g_free(paths[i]); g_free(uris[i]); g_free(ids[i]); }
}
static void test_library_mixed_rejection(void)
{
    Harness *h = open_harness(TRUE); radios_media_library_open(&h->ctx); GtkWidget *library = dialog("media-library-dialog");
    guint before = media_count(), staged = staged_files(); gchar *valid = audio("karışık yerel.wav", 13000), *digest = sha(valid);
    gchar *broken = g_build_filename(config.project_root, "bozuk ses.wav", NULL); g_assert_true(g_file_set_contents(broken, "not audio", -1, NULL));
    gchar *valid_uri = g_filename_to_uri(valid, NULL, NULL), *broken_uri = g_filename_to_uri(broken, NULL, NULL);
    gchar *directory_uri = g_filename_to_uri(config.project_root, NULL, NULL), *missing_uri = g_strconcat(directory_uri, "/yok.wav", NULL);
    const gchar *uris[] = {valid_uri, broken_uri, directory_uri, missing_uri, "https://example.invalid/music.mp3", "file://remote.invalid/music.wav", "%%%bad", valid_uri, NULL};
    drop(library, uris); wait_for(import_finished, library);
    g_assert_cmpuint(media_count(), ==, before + 1); g_assert_cmpuint(staged_files(), ==, staged);
    g_assert_nonnull(strstr(status(), "1 kayıt doğrulandı")); g_assert_nonnull(strstr(status(), "7 sonuç doğrulanamadı"));
    assert_staged("karışık yerel", digest); analyse_library(h);
    assert_imported("karışık yerel", valid, digest); g_assert_true(g_file_test(broken, G_FILE_TEST_IS_REGULAR));
    close_harness(h); g_unlink(valid); g_unlink(broken);
    g_free(valid); g_free(digest); g_free(broken); g_free(valid_uri); g_free(broken_uri); g_free(directory_uri); g_free(missing_uri);
}
static void test_disconnected_drop(void)
{
    guint before = media_count(), staged = staged_files(); Harness *h = open_harness(FALSE);
    gchar *path = audio("bağlantısız.wav", 9000), *digest = sha(path), *uri = g_filename_to_uri(path, NULL, NULL);
    const gchar *uris[] = {uri, NULL}; drop(h->window, uris);
    g_assert_nonnull(strstr(status(), "Veri servisi bağlı değil"));
    g_assert_cmpuint(media_count(), ==, before); g_assert_cmpuint(staged_files(), ==, staged);
    gchar *after = sha(path); g_assert_cmpstr(after, ==, digest); g_free(after);
    close_harness(h); g_unlink(path); g_free(path); g_free(uri); g_free(digest);
}
typedef struct { gint started, release; } Block;
static gchar *block_worker(gpointer value, const RadioSCancelToken *cancel, GError **error)
{
    (void)error; Block *block = value; g_atomic_int_set(&block->started, 1);
    while (!g_atomic_int_get(&block->release) && !radios_cancelled(cancel)) g_usleep(1000);
    return g_strdup("released");
}
static gboolean block_started(gpointer value) { return g_atomic_int_get(&((Block *)value)->started); }
static void test_busy_chooser_and_cancel(void)
{
    Harness *h = open_harness(TRUE); guint before = media_count(), staged = staged_files();
    gchar *first = audio("ilk kuyruk.wav", 10000), *second = audio("reddedilen kuyruk.wav", 11000);
    gchar *first_uri = g_filename_to_uri(first, NULL, NULL), *second_uri = g_filename_to_uri(second, NULL, NULL);
    const gchar *first_uris[] = {first_uri, NULL}, *second_uris[] = {second_uri, NULL};
    radios_media_library_open(&h->ctx); GtkWidget *library = dialog("media-library-dialog");
    gtk_button_clicked(GTK_BUTTON(find(library, "media-import"))); GtkWidget *chooser = dialog("media-import-chooser"); g_assert_nonnull(chooser);
    drop(h->window, first_uris); g_assert_nonnull(strstr(status(), "Dosya seçim penceresi açık"));
    gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_CANCEL); g_assert_cmpuint(media_count(), ==, before);
    Block block = {0}; GError *error = NULL;
    g_assert_cmpuint(radios_runtime_submit(h->ctx.runtime, block_worker, &block, NULL, &error), >, 0); g_assert_no_error(error); wait_for(block_started, &block);
    drop(h->window, first_uris); g_assert_false(gtk_widget_is_sensitive(find(library, "media-import")));
    drop(library, second_uris); g_assert_nonnull(strstr(status(), "İçe aktarma sürüyor"));
    gtk_button_clicked(GTK_BUTTON(find(library, "media-import-cancel"))); g_atomic_int_set(&block.release, 1);
    wait_for(import_finished, library); g_assert_cmpuint(media_count(), ==, before); g_assert_cmpuint(staged_files(), ==, staged);
    drop(library, second_uris); wait_for(import_finished, library); g_assert_cmpuint(media_count(), ==, before + 1);
    close_harness(h); g_unlink(first); g_unlink(second); g_free(first); g_free(second); g_free(first_uri); g_free(second_uri);
}
static void test_bad_payload_and_close(void)
{
    Harness *h = open_harness(TRUE); guint before = media_count(), staged = staged_files();
    drop_bytes(h->window, (const guchar *)"# empty URI list\r\n", 18); g_assert_nonnull(strstr(status(), "dosya listesi boş"));
    const gchar *bad[] = {"file:///bad%XX.wav", "ftp://remote.invalid/a.wav", NULL}; drop(h->window, bad);
    g_assert_nonnull(strstr(status(), "2 dosya kabul edilmedi"));
    const guchar nul_list[] = "file:///first.wav\0file:///second.wav\r\n";
    drop_bytes(h->window, nul_list, sizeof(nul_list) - 1); g_assert_nonnull(strstr(status(), "dosya listesi bozuk"));
    GString *large = g_string_new(NULL);
    for (guint i = 0; i < 257; ++i) g_string_append(large, "file:///not-found.wav\r\n");
    drop_bytes(h->window, (const guchar *)large->str, (gint)large->len); g_assert_nonnull(strstr(status(), "en fazla 256")); g_string_free(large, TRUE);
    guchar *oversize = g_malloc0(1024 * 1024 + 1); memset(oversize, 'a', 1024 * 1024 + 1);
    drop_bytes(h->window, oversize, 1024 * 1024 + 1); g_assert_nonnull(strstr(status(), "çok büyük")); g_free(oversize);
    gchar *path = audio("kapanış kuyruk.wav", 8000), *uri = g_filename_to_uri(path, NULL, NULL); const gchar *uris[] = {uri, NULL};
    Block block = {0}; GError *error = NULL;
    g_assert_cmpuint(radios_runtime_submit(h->ctx.runtime, block_worker, &block, NULL, &error), >, 0); g_assert_no_error(error); wait_for(block_started, &block);
    drop(h->window, uris); close_harness(h);
    g_assert_cmpuint(media_count(), ==, before); g_assert_cmpuint(staged_files(), ==, staged);
    g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR)); g_unlink(path); g_free(path); g_free(uri);
}
static gint folder_playlist_tracks(const gchar *name)
{
    JsonNode *input = body(), *result = request("/v1/workflows/desktop-playlists/list", input); json_node_free(input);
    JsonArray *playlists = json_object_get_array_member(json_node_get_object(result), "playlists"); gint tracks = -1;
    for (guint i = 0; playlists && i < json_array_get_length(playlists); ++i) {
        JsonObject *playlist = json_array_get_object_element(playlists, i);
        if (g_str_equal(radios_json_string(playlist, "name", ""), name)) tracks = (gint)json_array_get_length(json_object_get_array_member(playlist, "tracks"));
    }
    json_node_free(result); return tracks;
}
/* A dropped folder becomes a playlist named after it, holding its audio files. */
static void test_window_folder_playlist(void)
{
    Harness *h = open_harness(TRUE); guint before = media_count();
    gchar *folder = g_build_filename(config.project_root, "Klasör playlisti", NULL); g_assert_cmpint(g_mkdir_with_parents(folder, 0700), ==, 0);
    gchar *first = audio("Klasör playlisti/01 açılış.wav", 9000), *second = audio("Klasör playlisti/02 kapanış.wav", 9500);
    gchar *note = g_build_filename(folder, "notlar.txt", NULL); g_assert_true(g_file_set_contents(note, "ses değil", -1, NULL));
    gchar *uri = g_filename_to_uri(folder, NULL, NULL); const gchar *uris[] = {uri, NULL};
    drop(h->window, uris); GtkWidget *library = hidden_library();
    wait_for(import_finished, library); g_assert_false(gtk_widget_get_visible(library));
    g_assert_cmpuint(media_count(), ==, before + 2); g_assert_cmpint(folder_playlist_tracks("Klasör playlisti"), ==, 2);
    close_harness(h); g_unlink(first); g_unlink(second); g_unlink(note); g_rmdir(folder);
    g_free(first); g_free(second); g_free(note); g_free(uri); g_free(folder);
}
static gboolean chooser_has_folder(gpointer value)
{
    gchar *chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(g_object_get_data(G_OBJECT(value), "chooser")));
    gboolean match = g_strcmp0(chosen, g_object_get_data(G_OBJECT(value), "folder")) == 0; g_free(chosen); return match;
}
/* Playlist "+" folder chooser takes the same path as a dropped folder. */
static void test_folder_chooser_playlist(void)
{
    Harness *h = open_harness(TRUE); guint before = media_count();
    gchar *folder = g_build_filename(config.project_root, "Seçilen klasör", NULL), *sub = g_build_filename(folder, "alt", NULL);
    g_assert_cmpint(g_mkdir_with_parents(sub, 0700), ==, 0);
    gchar *first = audio("Seçilen klasör/01 sabah.wav", 9000), *second = audio("Seçilen klasör/alt/02 akşam.wav", 9500);
    radios_media_folder_import_open(&h->ctx); GtkWidget *chooser = dialog("media-folder-chooser"); g_assert_nonnull(chooser);
    radios_media_folder_import_open(&h->ctx); g_assert_true(dialog("media-folder-chooser") == chooser);
    g_assert_true(gtk_file_chooser_select_filename(GTK_FILE_CHOOSER(chooser), folder));
    g_object_set_data(G_OBJECT(h->window), "chooser", chooser); g_object_set_data(G_OBJECT(h->window), "folder", folder);
    wait_for(chooser_has_folder, h->window);
    gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_ACCEPT); g_assert_null(dialog("media-folder-chooser"));
    GtkWidget *library = hidden_library(); wait_for(import_finished, library); g_assert_false(gtk_widget_get_visible(library));
    g_assert_cmpuint(media_count(), ==, before + 2); g_assert_cmpint(folder_playlist_tracks("Seçilen klasör"), ==, 2);
    close_harness(h); g_unlink(first); g_unlink(second); g_rmdir(sub); g_rmdir(folder);
    g_free(sub); g_free(first); g_free(second); g_free(folder);
}
static void test_late_drop_after_destroy(void)
{
    Harness *h = open_harness(TRUE); guint before = media_count(), staged = staged_files();
    gchar *path = audio("gecikmiş bırakma.wav", 7000), *uri = g_filename_to_uri(path, NULL, NULL);
    const gchar *uris[] = {uri, NULL};
    radios_media_library_open(&h->ctx);
    GtkWidget *library = dialog("media-library-dialog"), *window = h->window;
    g_object_ref(library); g_object_ref(window);
    close_harness(h); /* Frees the context while both destroyed widgets remain alive. */
    drop(window, uris); drop(library, uris);
    gboolean handled = TRUE;
    g_signal_emit_by_name(window, "drag-drop", NULL, 0, 0, GDK_CURRENT_TIME, &handled); g_assert_false(handled);
    g_signal_emit_by_name(library, "drag-drop", NULL, 0, 0, GDK_CURRENT_TIME, &handled); g_assert_false(handled);
    g_assert_null(g_object_get_data(G_OBJECT(window), "radios-media-library-dialog"));
    g_assert_cmpuint(media_count(), ==, before); g_assert_cmpuint(staged_files(), ==, staged);
    g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));
    g_object_unref(library); g_object_unref(window); g_unlink(path); g_free(path); g_free(uri);
}
static void screenshot_style(void)
{
    if (!g_getenv("RADIOS_P11_SCREENSHOT_ROOT")) return;
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    extern const gchar *const radios_reference_css_parts[];
    GString *stylesheet = g_string_new(NULL);
    for (guint i = 0; radios_reference_css_parts[i]; ++i) g_string_append(stylesheet, radios_reference_css_parts[i]);
    gchar *path = g_build_filename(config.assets_root, "themes", "radios.css", NULL), *adaptation = NULL; GError *error = NULL;
    g_assert_true(g_file_get_contents(path, &adaptation, NULL, &error)); g_assert_no_error(error); g_free(path);
    g_string_append(stylesheet, adaptation); g_free(adaptation); GtkCssProvider *provider = gtk_css_provider_new();
    g_assert_true(gtk_css_provider_load_from_data(provider, stylesheet->str, stylesheet->len, &error)); g_assert_no_error(error); g_string_free(stylesheet, TRUE);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION); g_object_unref(provider);
    path = g_build_filename(config.assets_root, "fonts", "InterVariable.woff2", NULL);
    g_assert_true(FcInit()); g_assert_true(FcConfigAppFontAddFile(NULL, (const FcChar8 *)path)); g_free(path);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *project_root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc; g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    GError *error = NULL; g_assert_true(radios_config_load(project_root, NULL, &config, &error)); g_assert_no_error(error); g_free(project_root);
    screenshot_style();
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    /* Start the isolated genuine backend before the disconnected case reads it. */
    Harness *starter = open_harness(TRUE); close_harness(starter);
    g_test_add_func("/media-drop/window-multi-copy-sha", test_window_multi_copy);
    g_test_add_func("/media-drop/window-folder-playlist", test_window_folder_playlist);
    g_test_add_func("/media-drop/folder-chooser-playlist", test_folder_chooser_playlist);
    g_test_add_func("/media-drop/inline-markers-persist", test_inline_marker_persistence);
    g_test_add_func("/media-drop/multiple-playlist-routing", test_multiple_playlist_routing);
    g_test_add_func("/media-drop/playlist-source-editor-click", test_playlist_source_editor_click);
    g_test_add_func("/media-drop/library-mixed-invalid-source", test_library_mixed_rejection);
    g_test_add_func("/media-drop/disconnected-preserves-source", test_disconnected_drop);
    g_test_add_func("/media-drop/busy-chooser-cancel", test_busy_chooser_and_cancel);
    g_test_add_func("/media-drop/malformed-limit-close", test_bad_payload_and_close);
    g_test_add_func("/media-drop/retained-widget-late-drop", test_late_drop_after_destroy);
    gint result = g_test_run(); radios_data_client_unref(reader); radios_config_clear(&config); return result;
}
