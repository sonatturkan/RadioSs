/* Linked only into the process-level shutdown acceptance executable.
 * Drive the real widgets; production startup, authorization and teardown run
 * unchanged. There are no test hooks in the deployed application. */
#include "radios/window.h"
#include "radios/data.h"
#include "radios/audio_patchbay.h"
#include "radios/routing_graph_ui.h"
#include <string.h>

void __real_radios_window_present(RadioSWindow *window);
void __wrap_radios_window_present(RadioSWindow *window);

typedef struct {
    RadioSViewContext *context;
    gint64 deadline;
    gboolean requested;
    guint recorder;
} Driver;

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next)
        result = find(item->data, name);
    g_list_free(children);
    return result;
}

static void ready_marker(guint64 frames, gboolean active, const gchar *path)
{
    const gchar *marker = g_getenv("RADIOS_SHUTDOWN_READY");
    g_assert_nonnull(marker);
    JsonObject *object = json_object_new();
    json_object_set_int_member(object, "frames", (gint64)frames);
    json_object_set_boolean_member(object, "active", active);
    json_object_set_string_member(object, "path", path ? path : "");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, object);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, node);
    GError *error = NULL;
    g_assert_true(json_generator_to_file(generator, marker, &error));
    g_assert_no_error(error);
    g_object_unref(generator);
    json_node_free(node);
}

static gboolean drive(gpointer value)
{
    Driver *driver = value;
    g_assert_cmpint(g_get_monotonic_time(), <, driver->deadline);
    GtkWidget *page = find(GTK_WIDGET(driver->context->parent), "routing-workspace");
    g_assert_nonnull(page);
    RadioSAudioPatchbay *patchbay = g_object_get_data(G_OBJECT(page), "routing-patchbay");
    g_assert_nonnull(patchbay);
    RadioSAudioPatchbaySnapshot *snapshot = g_new0(RadioSAudioPatchbaySnapshot, 1);
    radios_audio_patchbay_snapshot(patchbay, snapshot);
    GPtrArray *documents = g_object_get_data(G_OBJECT(page), "radios-data-documents");
    if (!radios_data_connected(driver->context) || !snapshot->connected || !documents ||
        !documents->len || !radios_data_document_ready(g_ptr_array_index(documents, 0))) {
        g_free(snapshot); return G_SOURCE_CONTINUE;
    }
    const gchar *leaf = g_getenv("RADIOS_SHUTDOWN_RECORD");
    if (!leaf || !*leaf) {
        g_assert_false(snapshot->running);
        for (guint i = 0; i < snapshot->node_count; ++i) g_assert_false(snapshot->nodes[i].recording);
        ready_marker(0, FALSE, NULL); g_free(snapshot);
        return G_SOURCE_REMOVE;
    }
    RadioSRoutingGraphUI *editor = radios_routing_graph_ui_from_widget(find(page, "routing-graph-editor"));
    g_assert_nonnull(editor);
    if (!driver->requested) {
        if (!snapshot->input_count) { g_free(snapshot); return G_SOURCE_CONTINUE; }
        guint input = radios_routing_graph_ui_add(editor, RG_INPUT);
        g_assert_cmpuint(input, !=, 0);
        GtkWidget *device = find(page, "routing-block-device");
        g_assert_true(GTK_IS_COMBO_BOX(device));
        g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(device), "routing_microphone.monitor"));
        driver->recorder = radios_routing_graph_ui_add(editor, RG_RECORDER);
        g_assert_cmpuint(driver->recorder, !=, 0);
        GtkWidget *prefix = find(page, "routing-record-prefix");
        g_assert_true(GTK_IS_ENTRY(prefix));
        gchar *stem = g_strndup(leaf, g_str_has_suffix(leaf, ".wav") ? strlen(leaf) - 4 : strlen(leaf));
        gtk_entry_set_text(GTK_ENTRY(prefix), stem); g_free(stem);
        g_assert_true(radios_routing_graph_ui_connect(editor, input, driver->recorder));
        GtkWidget *start = find(page, "routing-record-start");
        g_assert_true(GTK_IS_BUTTON(start));
        driver->requested = TRUE;
        /* The actual start callback requests fresh operate authorization from
         * the private HTTP service before opening a recorder. */
        gtk_button_clicked(GTK_BUTTON(start));
        g_free(snapshot); return G_SOURCE_CONTINUE;
    }
    const RadioSPatchNodeSnapshot *recorder = NULL;
    for (guint i = 0; i < snapshot->node_count; ++i) if (snapshot->nodes[i].id == driver->recorder) recorder = &snapshot->nodes[i];
    if (recorder && recorder->state == RADIOS_PATCH_FAILED) g_error("Shutdown acceptance recorder failed: %s", recorder->error);
    if (!recorder || !recorder->recording || recorder->frames < 96000) { g_free(snapshot); return G_SOURCE_CONTINUE; }
    g_assert_cmpfloat(recorder->peak[0], >, .05);
    ready_marker(recorder->frames, TRUE, recorder->path); g_free(snapshot);
    return G_SOURCE_REMOVE;
}

void __wrap_radios_window_present(RadioSWindow *window)
{
    __real_radios_window_present(window);
    Driver *driver = g_new0(Driver, 1);
    driver->context = radios_window_view_context(window);
    g_assert_nonnull(driver->context);
    driver->deadline = g_get_monotonic_time() + 25 * G_TIME_SPAN_SECOND;
    g_timeout_add_full(G_PRIORITY_DEFAULT, 30, drive, driver, g_free);
}
