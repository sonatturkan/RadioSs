#include "radios/data.h"
#include "radios/media_jobs.h"
#include "radios/media_ui.h"
#include "radios/preferences.h"
#include "radios/programme_prerequisites.h"
#include "radios/playout_ui.h"
#include <glib/gstdio.h>
#include <string.h>

/* Same private connected-media harness and generated PCM import contract as
 * test_data_ui.c; only the genuine Settings/rights widgets drive grant writes. */
static RadioSConfig config;
static RadioSDataClient *reader;
typedef struct { RadioSViewContext ctx; GtkWidget *parent; } Harness;
typedef gboolean (*Condition)(gpointer);
static GtkWidget *find(GtkWidget *widget, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(widget), name)) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget)); GtkWidget *result = NULL;
    for (GList *it = children; it && !result; it = it->next) result = find(it->data, name);
    g_list_free(children); return result;
}
static GtkWidget *control(GtkWidget *widget, const gchar *name)
{ GtkWidget *result = find(widget, name); g_assert_nonnull(result); return result; }
static GtkWidget *dialog_find(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *it = windows; it && !result; it = it->next)
        if (!g_strcmp0(gtk_widget_get_name(it->data), name)) result = it->data;
    g_list_free(windows); return result;
}
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void run_for(guint milliseconds)
{ gint64 until = g_get_monotonic_time() + milliseconds * G_TIME_SPAN_MILLISECOND; while (g_get_monotonic_time() < until) pump(); }
static void wait_until(Condition condition, gpointer value, const gchar *description)
{
    gint64 until = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < until) pump();
    if (!condition(value)) {
        GtkWidget *rights = dialog_find("media-rights-dialog");
        if (rights) g_test_message("Actual rights status: %s", gtk_label_get_text(GTK_LABEL(control(rights, "media-rights-status"))));
        g_error("Native rights acceptance timed out: %s", description);
    }
}
static void click(GtkWidget *root, const gchar *name)
{ GtkWidget *button = control(root, name); g_assert_true(GTK_IS_BUTTON(button)); g_assert_true(gtk_widget_is_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button)); }
static void entry(GtkWidget *root, const gchar *name, const gchar *text)
{ GtkWidget *widget = control(root, name); g_assert_true(GTK_IS_ENTRY(widget)); gtk_entry_set_text(GTK_ENTRY(widget), text); }
static void combo(GtkWidget *root, const gchar *name, const gchar *id)
{ GtkWidget *widget = control(root, name); g_assert_true(GTK_IS_COMBO_BOX(widget)); g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), id)); }
static gboolean connected(gpointer value) { return radios_data_connected(&((Harness *)value)->ctx); }
static Harness *harness_open(void)
{
    Harness *h = g_new0(Harness, 1); h->parent = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(h->parent);
    h->ctx.config = &config; h->ctx.parent = GTK_WINDOW(h->parent); h->ctx.runtime = radios_runtime_new(16, 32);
    h->ctx.data = radios_data_new(&h->ctx); radios_data_connect(&h->ctx);
    wait_until(connected, h, "authenticated private media station");
    GtkWidget *settings = radios_preferences_button_new(&h->ctx); gtk_container_add(GTK_CONTAINER(h->parent), settings);
    gtk_widget_show_all(h->parent); return h;
}
static void harness_close(Harness *h)
{
    gtk_widget_destroy(h->parent); radios_data_close(h->ctx.data); radios_runtime_free(h->ctx.runtime);
    while (g_main_context_iteration(NULL, FALSE)) {}
    radios_data_unref(h->ctx.data); g_object_unref(h->parent); g_free(h);
}
typedef struct { const gchar *path; JsonNode *body, *result; GError *error; gint done; } Read;
static gpointer read_work(gpointer value)
{ Read *r = value; r->result = radios_data_client_request_sync(reader, "POST", r->path, r->body, NULL, &r->error); g_atomic_int_set(&r->done, 1); return NULL; }
static gboolean read_done(gpointer value) { return g_atomic_int_get(&((Read *)value)->done) != 0; }
static JsonNode *request(const gchar *path, JsonNode *body)
{
    Read r = {.path = path, .body = body}; GThread *thread = g_thread_new("private-rights-readback", read_work, &r);
    wait_until(read_done, &r, "genuine private API readback"); g_thread_join(thread); g_assert_no_error(r.error); g_assert_nonnull(r.result); return r.result;
}
static JsonNode *body_new(void)
{ JsonNode *n = json_node_new(JSON_NODE_OBJECT); json_node_take_object(n, json_object_new()); json_object_set_string_member(json_node_get_object(n), "station_id", radios_data_client_station_id(reader)); return n; }
static void write_u16(guchar *bytes, guint16 value) { value = GUINT16_TO_LE(value); memcpy(bytes, &value, sizeof value); }
static void write_u32(guchar *bytes, guint32 value) { value = GUINT32_TO_LE(value); memcpy(bytes, &value, sizeof value); }
static gchar *fixture_audio(const gchar *name)
{
    const guint frames = 16000, bytes = frames * 2; guchar *wav = g_malloc0(44 + bytes);
    memcpy(wav, "RIFF", 4); write_u32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8); write_u32(wav + 16, 16);
    write_u16(wav + 20, 1); write_u16(wav + 22, 1); write_u32(wav + 24, 8000); write_u32(wav + 28, 16000);
    write_u16(wav + 32, 2); write_u16(wav + 34, 16); memcpy(wav + 36, "data", 4); write_u32(wav + 40, bytes);
    for (guint i = 0; i < frames; ++i) write_u16(wav + 44 + i * 2, (guint16)(i % 8 < 4 ? 16384 : -16384));
    gchar *path = g_build_filename(config.project_root, name, NULL); g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL)); g_free(wav); return path;
}
typedef struct { JsonNode *result; GError *error; gboolean done; guint destroyed; } Imported;
static void imported(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{ (void)ctx; (void)owner; Imported *i = value; i->result = result ? json_node_copy(result) : NULL; i->error = error ? g_error_copy(error) : NULL; i->done = TRUE; }
static void import_destroyed(gpointer value) { ++((Imported *)value)->destroyed; }
static gboolean import_done(gpointer value) { return ((Imported *)value)->done; }
static JsonNode *import_media(Harness *h, const gchar *name, const gchar *title)
{
    gchar *path = fixture_audio(name); Imported i = {0};
    GCancellable *job = radios_media_import_async(&h->ctx, h->parent, path, "music", title, imported, &i, import_destroyed);
    g_assert_nonnull(job); wait_until(import_done, &i, "actual WAV import/hash/analysis and verified stored media");
    g_assert_no_error(i.error); g_assert_nonnull(i.result); g_assert_cmpuint(i.destroyed, ==, 1);
    JsonObject *version = json_object_get_object_member(json_node_get_object(i.result), "version");
    g_assert_true(json_object_get_boolean_member(version, "verified")); g_assert_cmpstr(radios_json_string(version, "status", ""), ==, "draft");
    g_object_unref(job); g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); return i.result;
}
typedef struct { GtkWidget *dialog; const gchar *id; GtkTreeIter iter; } Pick;
static gboolean pick_row(gpointer value)
{
    Pick *pick = value; GtkWidget *tree = find(pick->dialog, "media-table"); if (!GTK_IS_TREE_VIEW(tree)) return FALSE;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); GtkTreeIter iter;
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid; valid = gtk_tree_model_iter_next(model, &iter)) {
        gchar *id = NULL; gtk_tree_model_get(model, &iter, 4, &id, -1); gboolean same = !g_strcmp0(id, pick->id); g_free(id);
        if (same) { pick->iter = iter; return TRUE; }
    }
    return FALSE;
}
static gboolean widget_enabled(gpointer value) { return gtk_widget_is_sensitive(GTK_WIDGET(value)); }
static gboolean rights_ready(gpointer value)
{ GtkWidget *rights = value; return gtk_widget_is_sensitive(control(rights, "media-rights-refresh")) && gtk_combo_box_get_active_id(GTK_COMBO_BOX(control(rights, "media-rights-grant"))) != NULL; }
static GtkWidget *rights_open(Harness *h, JsonNode *media)
{
    click(h->parent, "header-settings"); GtkWidget *settings = dialog_find("preferences-window"); g_assert_nonnull(settings);
    GtkWidget *categories = control(settings, "preferences-categories"), *row = control(settings, "preferences-category-copyright");
    gtk_list_box_select_row(GTK_LIST_BOX(categories), GTK_LIST_BOX_ROW(row)); pump();
    GtkWidget *license = control(settings, "preferences-copyright-license");
    g_assert_true(GTK_IS_COMBO_BOX(license)); gtk_combo_box_set_active(GTK_COMBO_BOX(license), 0);
    GtkWidget *text = control(settings, "preferences-copyright-text");
    g_assert_true(GTK_IS_TEXT_VIEW(text)); g_assert_false(gtk_text_view_get_editable(GTK_TEXT_VIEW(text)));
    g_assert_false(gtk_text_view_get_cursor_visible(GTK_TEXT_VIEW(text)));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(control(settings, "preferences-copyright-source"))), ==, "assets/fonts/LICENSE.txt");
    gchar *path = g_build_filename(config.project_root, "assets", "fonts", "LICENSE.txt", NULL), *expected = NULL;
    gsize length = 0; GError *error = NULL; g_assert_true(g_file_get_contents(path, &expected, &length, &error));
    g_assert_no_error(error); g_assert_cmpuint(length, >, 0); g_assert_true(g_utf8_validate(expected, (gssize)length, NULL));
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)); GtkTextIter begin, end;
    gtk_text_buffer_get_bounds(buffer, &begin, &end); gchar *shown = gtk_text_buffer_get_text(buffer, &begin, &end, TRUE);
    g_assert_cmpstr(shown, ==, expected); g_free(shown); g_free(expected); g_free(path);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(control(settings, "preferences-copyright-status"))), "tamamı"));
    g_assert_null(find(settings, "preferences-copyright-media-rights"));
    g_assert_nonnull(find(settings, "preferences-category-copyright-policy"));
    /* The former version-grant editor remains covered through its API; its
     * launch action has been removed from the software-license page. */
    radios_media_rights_open(&h->ctx, settings); GtkWidget *rights = dialog_find("media-rights-dialog"); g_assert_nonnull(rights);
    g_assert_false(gtk_widget_is_sensitive(control(rights, "media-rights-save")));
    click(rights, "media-rights-pick"); GtkWidget *library = dialog_find("media-library-dialog"); g_assert_nonnull(library);
    Pick pick = {.dialog = library, .id = radios_json_string(json_object_get_object_member(json_node_get_object(media), "media"), "_id", "")};
    wait_until(pick_row, &pick, "real verified media appears in genuine station picker");
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(control(library, "media-table"))), &pick.iter);
    wait_until(widget_enabled, control(library, "media-pick-accept"), "verified selected version is assignable"); click(library, "media-pick-accept");
    wait_until(rights_ready, rights, "fresh exact station/media/version rights metadata"); return rights;
}
static JsonNode *rights_read(JsonNode *media)
{
    JsonObject *detail = json_node_get_object(media); JsonNode *body = body_new(); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "media_id", radios_json_string(json_object_get_object_member(detail, "media"), "_id", ""));
    json_object_set_string_member(o, "version_id", radios_json_string(json_object_get_object_member(detail, "version"), "_id", ""));
    JsonNode *result = request("/v1/workflows/desktop-media/rights", body); json_node_free(body); return result;
}
static JsonArray *grants(JsonNode *record)
{ return json_object_get_array_member(json_array_get_object_element(json_object_get_array_member(json_node_get_object(record), "records"), 0), "rights"); }
static JsonNode *register_output(const gchar *code)
{
    JsonNode *body = body_new(); JsonObject *o = json_node_get_object(body); json_object_set_string_member(o, "endpoint_id", radios_data_client_endpoint_id(reader));
    json_object_set_string_member(o, "output_id", code); JsonArray *geos = json_array_new(); json_array_add_string_element(geos, "LOCAL"); json_object_set_array_member(o, "geo_codes", geos);
    JsonNode *output = request("/v1/workflows/outputs/create", body); json_node_free(body); return output;
}
typedef struct { GtkWidget *dialog; const gchar *output_id; } Output;
static gboolean output_ready(gpointer value)
{ Output *o = value; return gtk_widget_is_sensitive(control(o->dialog, "media-rights-refresh")) && !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(control(o->dialog, "media-rights-output"))), o->output_id); }
static void set_terms(GtkWidget *rights, JsonNode *output, const gchar *decision, const gchar *source)
{
    combo(rights, "media-rights-output", radios_json_string(json_node_get_object(output), "_id", ""));
    entry(rights, "media-rights-territory", "LOCAL"); entry(rights, "media-rights-starts", "2026-10-11T12:00:00+03:00");
    entry(rights, "media-rights-ends", "2026-10-12T12:00:00+03:00"); entry(rights, "media-rights-source", source); combo(rights, "media-rights-decision", decision);
}
static gboolean save_readback(gpointer value)
{
    GtkWidget *rights = value; const gchar *message = gtk_label_get_text(GTK_LABEL(control(rights, "media-rights-status")));
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(control(rights, "media-rights-grant")));
    return strstr(message, "gerçek kayıttan yeniden okundu") && id && g_uuid_string_is_valid(id) && gtk_widget_is_sensitive(control(rights, "media-rights-refresh"));
}
static void assert_grant(JsonObject *grant, JsonNode *media, const gchar *output, const gchar *decision, const gchar *source)
{
    g_assert_true(g_uuid_string_is_valid(radios_json_string(grant, "_id", "")));
    g_assert_cmpstr(radios_json_string(grant, "station_id", ""), ==, radios_data_client_station_id(reader));
    g_assert_cmpstr(radios_json_string(grant, "version_id", ""), ==, radios_json_string(json_object_get_object_member(json_node_get_object(media), "version"), "_id", ""));
    g_assert_cmpstr(radios_json_string(grant, "approved_by", ""), ==, radios_data_client_user_id(reader));
    g_assert_cmpstr(radios_json_string(grant, "territory", ""), ==, "LOCAL"); g_assert_cmpstr(radios_json_string(grant, "output_id", ""), ==, output);
    g_assert_cmpstr(radios_json_string(grant, "decision", ""), ==, decision); g_assert_cmpstr(radios_json_string(grant, "source_ref", ""), ==, source);
    const gchar *fields[] = {"starts_at", "ends_at"}, *expected[] = {"2026-10-11T09:00:00Z", "2026-10-12T09:00:00Z"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) {
        GDateTime *a = g_date_time_new_from_iso8601(radios_json_string(grant, fields[i], ""), NULL), *b = g_date_time_new_from_iso8601(expected[i], NULL);
        g_assert_nonnull(a); g_assert_nonnull(b); g_assert_cmpint(g_date_time_compare(a, b), ==, 0); g_date_time_unref(a); g_date_time_unref(b);
    }
}
static void capture(GtkWidget *rights)
{
    const gchar *path = g_getenv("RADIOS_TEST_RIGHTS_CAPTURE"); if (!path || !*path) return; run_for(100);
    GdkPixbuf *pixels = gdk_pixbuf_get_from_window(gtk_widget_get_window(rights), 0, 0, gtk_widget_get_allocated_width(rights), gtk_widget_get_allocated_height(rights));
    g_assert_nonnull(pixels); GError *error = NULL; g_assert_true(gdk_pixbuf_save(pixels, path, "png", &error, NULL)); g_assert_no_error(error); g_object_unref(pixels);
}
static void test_settings_rights_roundtrip_append_and_guards(void)
{
    Harness *h = harness_open(); JsonNode *media = import_media(h, "rights-roundtrip.wav", "Gerçek Telif Medyası"); GtkWidget *rights = rights_open(h, media);
    JsonNode *before = rights_read(media); g_assert_cmpuint(json_array_get_length(grants(before)), ==, 0);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(before), "outputs")), ==, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(rights, "media-rights-confirm")), TRUE);
    g_assert_false(gtk_widget_is_sensitive(control(rights, "media-rights-save"))); json_node_free(before);
    JsonNode *output = register_output("RIGHTS_ROUNDTRIP"); click(rights, "media-rights-refresh");
    Output wait = {rights, radios_json_string(json_node_get_object(output), "_id", "")}; wait_until(output_ready, &wait, "explicitly registered station output appears with exact record UUID");
    set_terms(rights, output, "allow", "Türkçe sözleşme 1 / gerçek özel test");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(rights, "media-rights-confirm")), FALSE); g_assert_false(gtk_widget_is_sensitive(control(rights, "media-rights-save")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(rights, "media-rights-confirm")), TRUE);
    entry(rights, "media-rights-starts", "2026-10-11T09:00:00"); click(rights, "media-rights-save");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(control(rights, "media-rights-status"))), "UTC ofseti"));
    before = rights_read(media); g_assert_cmpuint(json_array_get_length(grants(before)), ==, 0); json_node_free(before);
    entry(rights, "media-rights-starts", "2026-10-11T12:00:00+03:00"); click(rights, "media-rights-save"); wait_until(save_readback, rights, "write receipt re-read from genuine persisted per-version rights");
    JsonNode *first = rights_read(media); g_assert_cmpuint(json_array_get_length(grants(first)), ==, 1); JsonObject *grant = json_array_get_object_element(grants(first), 0);
    assert_grant(grant, media, "RIGHTS_ROUNDTRIP", "allow", "Türkçe sözleşme 1 / gerçek özel test");
    gchar *first_id = g_strdup(radios_json_string(grant, "_id", "")); combo(rights, "media-rights-grant", first_id);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(rights, "media-rights-source"))), ==, "Türkçe sözleşme 1 / gerçek özel test");
    set_terms(rights, output, "deny", "Daha sonra eklenen yasak kararı"); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(rights, "media-rights-confirm")), TRUE);
    click(rights, "media-rights-save"); wait_until(save_readback, rights, "existing grant fields save as separate decision without overwriting first grant");
    JsonNode *second = rights_read(media); g_assert_cmpuint(json_array_get_length(grants(second)), ==, 2); gboolean original = FALSE, denied = FALSE;
    for (guint i = 0; i < json_array_get_length(grants(second)); ++i) {
        JsonObject *row = json_array_get_object_element(grants(second), i);
        if (!g_strcmp0(radios_json_string(row, "_id", ""), first_id)) { JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, row); g_assert_true(json_node_equal(node, json_array_get_element(grants(first), 0))); json_node_free(node); original = TRUE; }
        else { assert_grant(row, media, "RIGHTS_ROUNDTRIP", "deny", "Daha sonra eklenen yasak kararı"); denied = TRUE; }
    }
    g_assert_true(original); g_assert_true(denied); capture(rights); harness_close(h);
    h = harness_open(); rights = rights_open(h, media); combo(rights, "media-rights-grant", first_id);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(rights, "media-rights-source"))), ==, "Türkçe sözleşme 1 / gerçek özel test");
    JsonNode *reopened = rights_read(media); g_assert_true(json_node_equal(second, reopened));
    JsonObject *record = json_array_get_object_element(json_object_get_array_member(json_node_get_object(reopened), "records"), 0);
    g_assert_cmpstr(radios_json_string(json_object_get_object_member(record, "version"), "status", ""), ==, "draft");
    harness_close(h); json_node_free(media); json_node_free(output); json_node_free(first); json_node_free(second); json_node_free(reopened); g_free(first_id);
}
static gboolean uncertain_reply(gpointer value)
{
    GtkWidget *rights = value; return gtk_widget_is_sensitive(control(rights, "media-rights-refresh")) &&
        !gtk_widget_is_sensitive(control(rights, "media-rights-save")) && !save_readback(value);
}
static void test_lost_reply_reconciles_without_duplicate_write(void)
{
    Harness *h = harness_open(); JsonNode *media = import_media(h, "rights-lost-reply.wav", "Kayıp Yanıt Telif Medyası");
    JsonNode *output = register_output("RIGHTS_LOST_REPLY"); GtkWidget *rights = rights_open(h, media);
    set_terms(rights, output, "allow", "Kaybolan yanıtta korunacak gerçek karar");
    gchar *directory = g_path_get_dirname(config.database_file), *gate = g_build_filename(directory, ".acceptance-rights-drop", NULL);
    g_assert_true(g_file_set_contents(gate, "drop next real committed rights reply", -1, NULL));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(rights, "media-rights-confirm")), TRUE); click(rights, "media-rights-save");
    wait_until(uncertain_reply, rights, "private server drops actual committed rights reply; UI enters uncertain state");
    g_assert_false(g_file_test(gate, G_FILE_TEST_EXISTS));
    JsonNode *committed = rights_read(media); g_assert_cmpuint(json_array_get_length(grants(committed)), ==, 1);
    assert_grant(json_array_get_object_element(grants(committed), 0), media, "RIGHTS_LOST_REPLY", "allow", "Kaybolan yanıtta korunacak gerçek karar");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(rights, "media-rights-confirm")), TRUE);
    g_assert_false(gtk_widget_is_sensitive(control(rights, "media-rights-save")));
    gtk_button_clicked(GTK_BUTTON(control(rights, "media-rights-save"))); run_for(200);
    JsonNode *blocked = rights_read(media); g_assert_true(json_node_equal(committed, blocked)); json_node_free(blocked);
    click(rights, "media-rights-refresh"); wait_until(save_readback, rights, "uncertain intent reconciles exact genuinely committed same-user grant without resending");
    JsonNode *reconciled = rights_read(media); g_assert_true(json_node_equal(committed, reconciled)); json_node_free(reconciled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(rights, "media-rights-confirm")), TRUE); click(rights, "media-rights-save");
    wait_until(save_readback, rights, "fresh preflight recognizes existing identical decision and suppresses duplicate write");
    JsonNode *repeat = rights_read(media); g_assert_true(json_node_equal(committed, repeat));
    capture(rights); harness_close(h); json_node_free(repeat); json_node_free(committed); json_node_free(output); json_node_free(media); g_free(gate); g_free(directory);
}
static gboolean copyright_ready(gpointer value)
{ return gtk_widget_is_sensitive(control(value, "copyright-policy-save")); }
static gboolean copyright_saved(gpointer value)
{ return !g_strcmp0(gtk_label_get_text(GTK_LABEL(control(value, "copyright-policy-status"))), "Kaydedildi"); }
static GtkWidget *copyright_open(Harness *h)
{
    GtkWidget *settings = radios_preferences_open(&h->ctx);
    gtk_list_box_select_row(GTK_LIST_BOX(control(settings, "preferences-categories")),
        GTK_LIST_BOX_ROW(control(settings, "preferences-category-copyright-policy")));
    wait_until(copyright_ready, settings, "copyright policy loads genuine station CAS baseline");
    g_assert_null(find(settings, "preferences-copyright-media-rights")); return settings;
}
static JsonNode *copyright_read(void)
{
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "kind", "copyright_policy");
    JsonNode *result = request("/v1/workflows/desktop/list", body); json_node_free(body);
    JsonArray *records = json_object_get_array_member(json_node_get_object(result), "records");
    for (guint i = 0; i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i);
        if (!g_strcmp0(radios_json_string(record, "name", ""), "main")) {
            JsonNode *copy = json_node_copy(json_array_get_element(records, i)); json_node_free(result); return copy;
        }
    }
    json_node_free(result); return NULL;
}
static JsonObject *copyright_config(JsonNode *record)
{ g_assert_nonnull(record); return json_object_get_object_member(json_node_get_object(record), "config"); }
static void copyright_select(GtkWidget *settings, const gchar *id)
{
    GtkWidget *tree = control(settings, "copyright-policy-rules"); GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); GtkTreeIter iter; gboolean found = FALSE;
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid && !found; valid = gtk_tree_model_iter_next(model, &iter)) {
        gchar *row = NULL; gtk_tree_model_get(model, &iter, 4, &row, -1); found = !g_strcmp0(row, id); g_free(row);
        if (found) gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), &iter);
    }
    g_assert_true(found);
}
static void copyright_capture_png(GtkWidget *settings, const gchar *path)
{
    run_for(100);
    GdkPixbuf *pixels = gdk_pixbuf_get_from_window(gtk_widget_get_window(settings), 0, 0, gtk_widget_get_allocated_width(settings), gtk_widget_get_allocated_height(settings));
    g_assert_nonnull(pixels); GError *error = NULL; g_assert_true(gdk_pixbuf_save(pixels, path, "png", &error, NULL)); g_assert_no_error(error); g_object_unref(pixels);
}
static void copyright_capture(GtkWidget *settings)
{
    const gchar *path = g_getenv("RADIOS_TEST_COPYRIGHT_CAPTURE"); if (!path || !*path) return;
    GtkAdjustment *scroll = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(control(settings, "preferences-copyright-policy")));
    gdouble position = gtk_adjustment_get_value(scroll); gint width = 0, height = 0; gtk_window_get_size(GTK_WINDOW(settings), &width, &height);
    gchar *base = g_str_has_suffix(path, ".png") ? g_strndup(path, strlen(path) - 4) : g_strdup(path);
    gchar *details = g_strconcat(base, "-details.png", NULL), *small = g_strconcat(base, "-small.png", NULL), *small_details = g_strconcat(base, "-small-details.png", NULL);
    gtk_adjustment_set_value(scroll, 0); copyright_capture_png(settings, path);
    gtk_adjustment_set_value(scroll, gtk_adjustment_get_upper(scroll) - gtk_adjustment_get_page_size(scroll)); copyright_capture_png(settings, details);
    gtk_window_resize(GTK_WINDOW(settings), 660, 540); run_for(150); gtk_adjustment_set_value(scroll, 0); copyright_capture_png(settings, small);
    gtk_adjustment_set_value(scroll, gtk_adjustment_get_upper(scroll) - gtk_adjustment_get_page_size(scroll)); copyright_capture_png(settings, small_details);
    gtk_window_resize(GTK_WINDOW(settings), width, height); run_for(150); gtk_adjustment_set_value(scroll, position);
    g_free(base); g_free(details); g_free(small); g_free(small_details);
}
static void test_manual_copyright_policy_persistence(void)
{
    Harness *h = harness_open(); GtkWidget *settings = copyright_open(h);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control(settings, "copyright-policy-unrestricted"))));
    JsonNode *record = copyright_read(); g_assert_null(record);
    /* Manual metadata requires no media import or version selection. Invalid
     * selectors/dates cannot make a request or leave a policy document. */
    click(settings, "copyright-policy-save");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(control(settings, "copyright-policy-status"))), "En az bir"));
    entry(settings, "copyright-policy-artist", "Şebnem Ferah"); entry(settings, "copyright-policy-album", "Kadın");
    entry(settings, "copyright-policy-title", "Vazgeçtim"); entry(settings, "copyright-policy-note", "Elle girilen izin / Türkçe kayıt");
    entry(settings, "copyright-policy-starts", "2026-10-11T12:00:00+03:00"); entry(settings, "copyright-policy-ends", "2026-10-10T12:00:00+03:00");
    click(settings, "copyright-policy-save"); g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(control(settings, "copyright-policy-status"))), "bitiş"));
    record = copyright_read(); g_assert_null(record);
    entry(settings, "copyright-policy-ends", "2027-10-11T12:00:00+03:00"); click(settings, "copyright-policy-save");
    wait_until(copyright_saved, settings, "manual metadata copyright rule genuinely persists with acknowledged revision");
    record = copyright_read(); JsonObject *config_value = copyright_config(record);
    g_assert_cmpint(radios_json_int(config_value, "schema", 0), ==, 1); g_assert_false(json_object_get_boolean_member(config_value, "restrict"));
    g_assert_cmpstr(radios_json_string(config_value, "unregistered", ""), ==, "allow");
    JsonArray *list = json_object_get_array_member(config_value, "rules"); g_assert_cmpuint(json_array_get_length(list), ==, 1);
    JsonObject *first = json_array_get_object_element(list, 0); gchar *first_id = g_strdup(radios_json_string(first, "id", ""));
    g_assert_true(g_uuid_string_is_valid(first_id)); g_assert_cmpstr(radios_json_string(first, "artist", ""), ==, "Şebnem Ferah");
    g_assert_cmpstr(radios_json_string(first, "album", ""), ==, "Kadın"); g_assert_cmpstr(radios_json_string(first, "title", ""), ==, "Vazgeçtim");
    g_assert_cmpstr(radios_json_string(first, "note", ""), ==, "Elle girilen izin / Türkçe kayıt");
    g_assert_cmpstr(radios_json_string(first, "starts_at", ""), ==, "2026-10-11T09:00:00Z");
    g_assert_cmpstr(radios_json_string(first, "ends_at", ""), ==, "2027-10-11T09:00:00Z"); g_assert_true(json_object_get_boolean_member(first, "enabled"));
    gint64 revision = radios_json_int(json_node_get_object(record), "_rev", 0); g_assert_cmpint(revision, >, 0);
    JsonNode *older_policy = json_node_copy(record); json_node_free(record);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(settings, "copyright-policy-restrict")), TRUE);
    wait_until(copyright_saved, settings, "global restrict toggle saves to station database");
    record = copyright_read(); config_value = copyright_config(record); g_assert_true(json_object_get_boolean_member(config_value, "restrict"));
    g_assert_cmpint(radios_json_int(json_node_get_object(record), "_rev", 0), ==, revision + 1);
    /* The saved toggle must reach runtime only with its acknowledged DB
     * revision. A late playlist snapshot cannot restore the older policy. */
    g_assert_true(json_object_get_boolean_member(radios_playout_ui_copyright_policy_get(&h->ctx), "restrict"));
    g_assert_false(radios_playout_ui_copyright_policy_record(&h->ctx, copyright_config(older_policy),
        radios_json_string(json_node_get_object(older_policy), "_id", ""), revision));
    g_assert_false(radios_playout_ui_copyright_policy_record(&h->ctx, copyright_config(older_policy), NULL, 0));
    g_assert_true(json_object_get_boolean_member(radios_playout_ui_copyright_policy_get(&h->ctx), "restrict"));
    json_node_free(older_policy); json_node_free(record);
    click(settings, "copyright-policy-new"); entry(settings, "copyright-policy-artist", "Şebnem Ferah");
    entry(settings, "copyright-policy-starts", "2026-10-10 09:00"); entry(settings, "copyright-policy-ends", "2026-10-12 09:00");
    click(settings, "copyright-policy-save"); wait_until(copyright_saved, settings, "artist-only expired rule persists alongside specific rule");
    record = copyright_read(); config_value = copyright_config(record); list = json_object_get_array_member(config_value, "rules"); g_assert_cmpuint(json_array_get_length(list), ==, 2);
    gchar *second_id = g_strdup(radios_json_string(json_array_get_object_element(list, 1), "id", "")); json_node_free(record);
    copyright_select(settings, first_id); entry(settings, "copyright-policy-note", "Düzenlenmiş izin kaydı");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(settings, "copyright-policy-enabled")), FALSE); click(settings, "copyright-policy-save");
    wait_until(copyright_saved, settings, "rule edit keeps UUID and updates exact database row");
    record = copyright_read(); first = json_array_get_object_element(json_object_get_array_member(copyright_config(record), "rules"), 0);
    g_assert_cmpstr(radios_json_string(first, "id", ""), ==, first_id); g_assert_cmpstr(radios_json_string(first, "note", ""), ==, "Düzenlenmiş izin kaydı");
    g_assert_false(json_object_get_boolean_member(first, "enabled")); json_node_free(record); copyright_capture(settings);
    /* Reopening a new connected frontend proves persistence is in SDBASE,
     * rather than widget state or a preference file on this workstation. */
    harness_close(h); h = harness_open(); settings = copyright_open(h);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control(settings, "copyright-policy-restrict"))));
    copyright_select(settings, first_id);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(settings, "copyright-policy-note"))), ==, "Düzenlenmiş izin kaydı");
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control(settings, "copyright-policy-enabled"))));
    copyright_select(settings, second_id); click(settings, "copyright-policy-remove"); wait_until(copyright_saved, settings, "selected copyright rule removal persists without deleting other rules");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(settings, "copyright-policy-unrestricted")), TRUE);
    wait_until(copyright_saved, settings, "unrestricted toggle persists after reopen");
    record = copyright_read(); config_value = copyright_config(record); g_assert_false(json_object_get_boolean_member(config_value, "restrict"));
    list = json_object_get_array_member(config_value, "rules"); g_assert_cmpuint(json_array_get_length(list), ==, 1);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(list, 0), "id", ""), ==, first_id);
    json_node_free(record); harness_close(h); g_free(first_id); g_free(second_id);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]); for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1]; --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv); GError *error = NULL;
    g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-media-rights/settings-verified-picker-grant-append-reopen-guards", test_settings_rights_roundtrip_append_and_guards);
    g_test_add_func("/native-media-rights/lost-committed-reply-reconcile-no-duplicate", test_lost_reply_reconciles_without_duplicate_write);
    g_test_add_func("/native-copyright-policy/manual-metadata-dates-toggle-edit-remove-reopen", test_manual_copyright_policy_persistence);
    gint result = g_test_run(); radios_data_client_unref(reader); radios_config_clear(&config); return result;
}
