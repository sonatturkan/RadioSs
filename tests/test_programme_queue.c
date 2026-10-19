#include "radios/programme_queue.h"
#include "radios/playout_ui.h"
#include <string.h>

#define BLOCK_A "11111111-1111-4111-8111-111111111111"
#define BLOCK_B "22222222-2222-4222-8222-222222222222"
#define BLOCK_EMPTY "33333333-3333-4333-8333-333333333333"
#define TRACK_A "44444444-4444-4444-8444-444444444444"
#define TRACK_B "55555555-5555-4555-8555-555555555555"
#define TRACK_C "66666666-6666-4666-8666-666666666666"
#define TRACK_D "77777777-7777-4777-8777-777777777777"

typedef struct {
    GtkWidget *window, *queue;
    GtkListBox *list;
    JsonObject *config;
    gchar *selected, *payload;
    RadioSProgrammePlacement placement;
    RadioSDragInfo info;
    guint drops;
    gboolean accepted;
    GArray *actions;       /* RadioSProgrammeRowAction received by the row hooks, in order */
    GPtrArray *action_ids; /* the id each recorded action carried */
    guint cleared;         /* selection hooks that carried NULL (selection cleared) */
} Fixture;

void __wrap_gtk_drag_finish(GdkDragContext *context, gboolean success, gboolean delete_source, guint time)
{
    (void)context; (void)success; (void)delete_source; (void)time;
}

static void pump(void)
{
    for (guint iteration = 0; iteration < 30; ++iteration) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
}

static void run_for(guint milliseconds)
{
    gint64 until = g_get_monotonic_time() + (gint64)milliseconds * 1000;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
}

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}

static GtkWidget *with_class(GtkWidget *root, const gchar *name)
{
    if (gtk_style_context_has_class(gtk_widget_get_style_context(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = with_class(item->data, name);
    g_list_free(children); return result;
}

static void selected(gpointer data, const gchar *id)
{
    Fixture *fixture = data;
    g_free(fixture->selected); fixture->selected = g_strdup(id);
    if (!id) ++fixture->cleared;
}

static void acted(gpointer data, const gchar *id, RadioSProgrammeRowAction action)
{
    Fixture *fixture = data;
    g_array_append_val(fixture->actions, action);
    g_ptr_array_add(fixture->action_ids, g_strdup(id));
}

static gboolean dropped(gpointer data, RadioSDragInfo info, const gchar *payload, RadioSProgrammePlacement placement)
{
    Fixture *fixture = data;
    fixture->info = info; fixture->placement = placement; ++fixture->drops;
    g_free(fixture->payload); fixture->payload = g_strdup(payload);
    fixture->accepted = TRUE;
    if (info != RADIOS_DRAG_INFO_ROW) return TRUE;
    RadioSProgrammeTree *tree = radios_programme_tree_new(fixture->config);
    fixture->accepted = radios_programme_tree_move(tree, payload, (guint)MAX(placement.block, 0), placement.position);
    if (fixture->accepted) {
        JsonNode *updated = radios_programme_tree_config(tree, fixture->config);
        json_object_unref(fixture->config); fixture->config = json_object_ref(json_node_get_object(updated));
        json_node_free(updated);
        radios_programme_queue_render(fixture->queue, fixture->config, payload);
    }
    radios_programme_tree_free(tree);
    return fixture->accepted;
}

static void setup(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *source = "{\"title\":\"Program\",\"blocks\":["
        "{\"id\":\"" BLOCK_A "\",\"title\":\"Sabah müzikleri\",\"kind\":\"music\"},"
        "{\"id\":\"" BLOCK_B "\",\"title\":\"Akşam müzikleri\",\"kind\":\"music\"},"
        "{\"id\":\"" BLOCK_EMPTY "\",\"title\":\"Boş event\",\"kind\":\"music\"}],\"events\":["
        "{\"id\":\"" TRACK_A "\",\"block_id\":\"" BLOCK_A "\",\"title\":\"Birinci parça\",\"artist\":\"Sanatçı A\",\"kind\":\"music\",\"duration_ms\":120000,\"media_id\":\"media-a\"},"
        "{\"id\":\"" TRACK_B "\",\"block_id\":\"" BLOCK_A "\",\"title\":\"İkinci parça\",\"kind\":\"music\",\"duration_ms\":180000,\"media_id\":\"media-b\"},"
        "{\"id\":\"" TRACK_C "\",\"block_id\":\"" BLOCK_A "\",\"title\":\"Üçüncü parça\",\"kind\":\"music\",\"duration_ms\":150000,\"media_id\":\"media-c\"},"
        "{\"id\":\"" TRACK_D "\",\"block_id\":\"" BLOCK_B "\",\"title\":\"Akşam parçası\",\"kind\":\"music\",\"duration_ms\":90000,\"media_id\":\"media-d\"}]}";
    JsonParser *parser = json_parser_new(); GError *error = NULL;
    g_assert_true(json_parser_load_from_data(parser, source, -1, &error)); g_assert_no_error(error);
    fixture->config = json_object_ref(json_node_get_object(json_parser_get_root(parser))); g_object_unref(parser);
    fixture->actions = g_array_new(FALSE, FALSE, sizeof(RadioSProgrammeRowAction));
    fixture->action_ids = g_ptr_array_new_with_free_func(g_free);
    RadioSProgrammeQueueHooks hooks = {.data = fixture, .selected = selected, .action = acted, .drop = dropped};
    fixture->queue = radios_programme_queue_new(&hooks);
    fixture->list = g_object_get_data(G_OBJECT(fixture->queue), "list-box");
    fixture->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_style_context_add_class(gtk_widget_get_style_context(fixture->window), "radios");
    gtk_window_set_default_size(GTK_WINDOW(fixture->window), 820, 480);
    GtkWidget *workspace = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(workspace, "live-task-view");
    gtk_container_add(GTK_CONTAINER(fixture->window), workspace);
    gtk_box_pack_start(GTK_BOX(workspace), fixture->queue, TRUE, TRUE, 0);
    radios_programme_queue_render(fixture->queue, fixture->config, NULL);
    gtk_widget_show_all(fixture->window); pump();
}

static void teardown(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gtk_widget_destroy(fixture->window); pump();
    json_object_unref(fixture->config); g_free(fixture->selected); g_free(fixture->payload);
    g_array_unref(fixture->actions); g_ptr_array_unref(fixture->action_ids);
}

static GtkWidget *row(Fixture *fixture, guint index)
{
    GtkWidget *result = GTK_WIDGET(gtk_list_box_get_row_at_index(fixture->list, (gint)index));
    g_assert_nonnull(result); return result;
}

static void assert_rows(Fixture *fixture, const gchar *const *ids, guint count)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(fixture->list));
    g_assert_cmpuint(g_list_length(rows), ==, count); g_list_free(rows);
    for (guint index = 0; index < count; ++index)
        g_assert_cmpstr(radios_programme_queue_row_id(GTK_LIST_BOX_ROW(row(fixture, index))), ==, ids[index]);
}

static void open_first(Fixture *fixture)
{
    GtkWidget *button = with_class(row(fixture, 0), "programme-block-number");
    g_assert_true(GTK_IS_BUTTON(button)); gtk_button_clicked(GTK_BUTTON(button)); pump();
}

static void back(Fixture *fixture)
{
    GtkWidget *button = find(fixture->queue, "programme-queue-back");
    g_assert_true(gtk_widget_is_visible(button)); gtk_button_clicked(GTK_BUTTON(button)); pump();
}

static void clipboard_get(GtkClipboard *clipboard, GtkSelectionData *selection, guint info, gpointer data)
{
    (void)clipboard; (void)info;
    radios_drag_set_payload(selection, data);
}

static void click_body(Fixture *fixture, guint index)
{
    GtkWidget *handle = gtk_bin_get_child(GTK_BIN(row(fixture, index)));
    GdkEvent *event = gdk_event_new(GDK_BUTTON_RELEASE); event->button.button = 1;
    g_signal_emit_by_name(handle, "event-after", event);
    gdk_event_free(event); pump();
}

static void drop(Fixture *fixture, RadioSDragInfo info, const gchar *payload, gint y)
{
    guint count = 0; const GtkTargetEntry *targets = radios_drag_targets(&count);
    g_assert_cmpuint(info, <=, count);
    /* Independent GUI tests may share a display; isolate this synthetic drag
     * selection so another process cannot steal its clipboard ownership. */
    gchar *selection_name = g_uuid_string_random();
    GtkClipboard *clipboard = gtk_clipboard_get(gdk_atom_intern(selection_name, FALSE));
    g_free(selection_name);
    g_assert_true(gtk_clipboard_set_with_data(clipboard, &targets[info - 1], 1, clipboard_get, NULL, (gpointer)payload));
    GtkSelectionData *selection = gtk_clipboard_wait_for_contents(clipboard, gdk_atom_intern_static_string(targets[info - 1].target));
    g_assert_nonnull(selection);
    g_signal_emit_by_name(fixture->list, "drag-data-received", NULL, 0, y, selection, info, GDK_CURRENT_TIME);
    gtk_selection_data_free(selection); gtk_clipboard_clear(clipboard); pump();
}

static void move(Fixture *fixture, guint source, guint destination, gboolean after)
{
    GtkWidget *handle = gtk_bin_get_child(GTK_BIN(row(fixture, source)));
    g_object_ref(handle);
    GtkAllocation allocation; gtk_widget_get_allocation(row(fixture, destination), &allocation);
    gchar *id = g_strdup(radios_programme_queue_row_id(GTK_LIST_BOX_ROW(row(fixture, source))));
    g_signal_emit_by_name(handle, "drag-begin", NULL);
    drop(fixture, RADIOS_DRAG_INFO_ROW, id, allocation.y + (after ? allocation.height - 1 : 1));
    g_signal_emit_by_name(handle, "drag-end", NULL);
    g_object_unref(handle); g_free(id);
}

static void test_flat_navigation(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *blocks[] = {BLOCK_A, BLOCK_B, BLOCK_EMPTY};
    const gchar *tracks[] = {TRACK_A, TRACK_B, TRACK_C};
    assert_rows(fixture, blocks, G_N_ELEMENTS(blocks));
    g_assert_false(gtk_widget_is_visible(with_class(fixture->queue, "programme-queue-navigation")));
    gtk_list_box_select_row(fixture->list, GTK_LIST_BOX_ROW(row(fixture, 0))); pump();
    assert_rows(fixture, blocks, G_N_ELEMENTS(blocks));
    open_first(fixture); assert_rows(fixture, tracks, G_N_ELEMENTS(tracks));
    g_assert_cmpstr(fixture->selected, ==, BLOCK_A);
    for (guint index = 0; index < G_N_ELEMENTS(tracks); ++index) {
        gchar *expected = g_strdup_printf("%u", index + 1);
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(with_class(row(fixture, index), "broadcast-queue-index"))), ==, expected);
        g_free(expected);
    }
    back(fixture); assert_rows(fixture, blocks, G_N_ELEMENTS(blocks));
    g_assert_cmpstr(fixture->selected, ==, BLOCK_A);
    g_signal_emit_by_name(fixture->list, "row-activated", row(fixture, 1)); pump();
    const gchar *evening[] = {TRACK_D}; assert_rows(fixture, evening, G_N_ELEMENTS(evening));
    back(fixture); assert_rows(fixture, blocks, G_N_ELEMENTS(blocks));
}

static void test_track_reorder(Fixture *fixture, gconstpointer data)
{
    (void)data; open_first(fixture);
    move(fixture, 0, 2, TRUE); g_assert_true(fixture->accepted);
    const gchar *forward[] = {TRACK_B, TRACK_C, TRACK_A}; assert_rows(fixture, forward, G_N_ELEMENTS(forward));
    g_assert_cmpint(fixture->placement.block, ==, 0); g_assert_cmpuint(fixture->placement.position, ==, 3);
    move(fixture, 2, 0, FALSE); g_assert_true(fixture->accepted);
    const gchar *original[] = {TRACK_A, TRACK_B, TRACK_C}; assert_rows(fixture, original, G_N_ELEMENTS(original));
    move(fixture, 0, 0, FALSE); g_assert_false(fixture->accepted);
    assert_rows(fixture, original, G_N_ELEMENTS(original));
    radios_programme_queue_render(fixture->queue, fixture->config, TRACK_C);
    assert_rows(fixture, original, G_N_ELEMENTS(original));
    radios_programme_queue_set_playing(fixture->queue, TRACK_C, FALSE);
    g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(row(fixture, 2)), "is-playing"));
}

static void test_event_reorder(Fixture *fixture, gconstpointer data)
{
    (void)data;
    move(fixture, 0, 2, TRUE); g_assert_true(fixture->accepted);
    const gchar *forward[] = {BLOCK_B, BLOCK_EMPTY, BLOCK_A}; assert_rows(fixture, forward, G_N_ELEMENTS(forward));
    g_assert_cmpint(fixture->placement.block, ==, -1); g_assert_cmpuint(fixture->placement.position, ==, 3);
    move(fixture, 2, 0, FALSE); g_assert_true(fixture->accepted);
    const gchar *original[] = {BLOCK_A, BLOCK_B, BLOCK_EMPTY}; assert_rows(fixture, original, G_N_ELEMENTS(original));
}

static void test_drop_targets(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GtkAllocation allocation; gtk_widget_get_allocation(row(fixture, 1), &allocation);
    drop(fixture, RADIOS_DRAG_INFO_MEDIA, "media", allocation.y + 1);
    g_assert_cmpint(fixture->placement.block, ==, 1); g_assert_cmpuint(fixture->placement.position, ==, 1);
    drop(fixture, RADIOS_DRAG_INFO_PLAYLIST, "playlist", allocation.y + 1);
    g_assert_cmpint(fixture->placement.block, ==, -1); g_assert_cmpuint(fixture->placement.position, ==, 2);
    drop(fixture, RADIOS_DRAG_INFO_PROGRAMME, "programme", allocation.y + 1);
    g_assert_cmpint(fixture->placement.block, ==, -1); g_assert_cmpuint(fixture->placement.position, ==, 2);
    open_first(fixture);
    gtk_widget_get_allocation(row(fixture, 1), &allocation);
    drop(fixture, RADIOS_DRAG_INFO_MEDIA, "media", allocation.y + 1);
    g_assert_cmpint(fixture->placement.block, ==, 0); g_assert_cmpuint(fixture->placement.position, ==, 1);
    drop(fixture, RADIOS_DRAG_INFO_PLAYLIST, "playlist", allocation.y + 1);
    g_assert_cmpint(fixture->placement.block, ==, 0); g_assert_cmpuint(fixture->placement.position, ==, 1);
    drop(fixture, RADIOS_DRAG_INFO_PLAYLIST, "playlist", allocation.y + allocation.height - 1);
    g_assert_cmpint(fixture->placement.block, ==, 0); g_assert_cmpuint(fixture->placement.position, ==, 2);
    drop(fixture, RADIOS_DRAG_INFO_PROGRAMME, "programme", 10000);
    g_assert_cmpint(fixture->placement.block, ==, 0); g_assert_cmpuint(fixture->placement.position, ==, 3);
    drop(fixture, RADIOS_DRAG_INFO_PLAYLIST, "playlist", 10000);
    g_assert_cmpint(fixture->placement.block, ==, 0); g_assert_cmpuint(fixture->placement.position, ==, 3);
    back(fixture);
    radios_programme_queue_set_expanded(fixture->queue, BLOCK_EMPTY, TRUE); pump();
    g_assert_null(radios_programme_queue_row_id(GTK_LIST_BOX_ROW(row(fixture, 0))));
    drop(fixture, RADIOS_DRAG_INFO_MEDIA, "media", 1);
    g_assert_cmpint(fixture->placement.block, ==, 2); g_assert_cmpuint(fixture->placement.position, ==, 0);
    drop(fixture, RADIOS_DRAG_INFO_MEDIA, "media", 10000);
    g_assert_cmpint(fixture->placement.block, ==, 2); g_assert_cmpuint(fixture->placement.position, ==, 0);
    drop(fixture, RADIOS_DRAG_INFO_PLAYLIST, "playlist", 10000);
    g_assert_cmpint(fixture->placement.block, ==, 2); g_assert_cmpuint(fixture->placement.position, ==, 0);
}

static void test_removed_event(Fixture *fixture, gconstpointer data)
{
    (void)data; open_first(fixture);
    RadioSProgrammeTree *tree = radios_programme_tree_new(fixture->config);
    g_assert_true(radios_programme_tree_remove(tree, BLOCK_A));
    JsonNode *updated = radios_programme_tree_config(tree, fixture->config); radios_programme_tree_free(tree);
    radios_programme_queue_render(fixture->queue, json_node_get_object(updated), NULL);
    const gchar *remaining[] = {BLOCK_B, BLOCK_EMPTY}; assert_rows(fixture, remaining, G_N_ELEMENTS(remaining));
    g_assert_false(gtk_widget_is_visible(with_class(fixture->queue, "programme-queue-navigation")));
    json_node_free(updated);
}

static void test_click_after_drop(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GtkWidget *handle = gtk_bin_get_child(GTK_BIN(row(fixture, 0)));
    GtkAllocation allocation; gtk_widget_get_allocation(row(fixture, 2), &allocation);
    g_signal_emit_by_name(handle, "drag-begin", NULL);
    drop(fixture, RADIOS_DRAG_INFO_ROW, BLOCK_A, allocation.y + allocation.height - 1);
    const gchar *blocks[] = {BLOCK_B, BLOCK_EMPTY, BLOCK_A};
    assert_rows(fixture, blocks, G_N_ELEMENTS(blocks));
    click_body(fixture, 2);
    const gchar *tracks[] = {TRACK_A, TRACK_B, TRACK_C};
    assert_rows(fixture, tracks, G_N_ELEMENTS(tracks));
    click_body(fixture, 2); g_assert_cmpstr(fixture->selected, ==, TRACK_C);
    back(fixture); assert_rows(fixture, blocks, G_N_ELEMENTS(blocks));
}

static void test_pending_navigation_destroy(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GtkWidget *button = with_class(row(fixture, 0), "programme-block-number");
    gtk_button_clicked(GTK_BUTTON(button));
    gtk_widget_destroy(fixture->queue); pump();
}

/* Check computed GTK colors after loading the application's actual CSS.  A
 * class assertion alone cannot prove that selected-row CSS has not hidden the
 * requested black/white background or made the content unreadable. */
static GdkRGBA background(GtkWidget *widget)
{
    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    GdkRGBA *value = NULL;
    gtk_style_context_get(context, gtk_style_context_get_state(context), "background-color", &value, NULL);
    g_assert_nonnull(value);
    GdkRGBA result = *value; gdk_rgba_free(value); return result;
}

static gdouble assert_grey(GtkWidget *widget, gdouble expected)
{
    GdkRGBA color = background(widget);
    g_assert_cmpfloat_with_epsilon(color.red, expected, 0.015);
    g_assert_cmpfloat_with_epsilon(color.green, color.red, 0.005);
    g_assert_cmpfloat_with_epsilon(color.blue, color.red, 0.005);
    g_assert_cmpfloat_with_epsilon(color.alpha, 1, 0.005);
    return color.red;
}

static gboolean has_class(GtkWidget *widget, const gchar *name)
{
    return gtk_style_context_has_class(gtk_widget_get_style_context(widget), name);
}

static void screenshot(Fixture *fixture, const gchar *name)
{
    const gchar *directory = g_getenv("RADIOS_PROMPT3_SCREENSHOTS");
    if (!directory || !*directory) return;
    g_assert_cmpint(g_mkdir_with_parents(directory, 0755), ==, 0);
    pump();
    GdkPixbuf *pixels = gdk_pixbuf_get_from_window(gtk_widget_get_window(fixture->window), 0, 0,
        gtk_widget_get_allocated_width(fixture->window), gtk_widget_get_allocated_height(fixture->window));
    g_assert_nonnull(pixels);
    gchar *path = g_build_filename(directory, name, NULL); GError *error = NULL;
    g_assert_true(gdk_pixbuf_save(pixels, path, "png", &error, NULL)); g_assert_no_error(error);
    g_free(path); g_object_unref(pixels);
}

static void test_upcoming_progress(Fixture *fixture, gconstpointer data)
{
    (void)data; open_first(fixture);
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 0, 120000, FALSE); pump();
    g_assert_true(has_class(row(fixture, 1), "is-upcoming"));
    assert_grey(row(fixture, 1), 0); screenshot(fixture, "next-black.png");
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 30000, 120000, FALSE); pump();
    gdouble quarter = assert_grey(row(fixture, 1), 0.0625);
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 60000, 120000, FALSE); pump();
    gdouble middle = assert_grey(row(fixture, 1), 0.25); screenshot(fixture, "next-grey.png");
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 90000, 120000, FALSE); pump();
    gdouble later = assert_grey(row(fixture, 1), 0.5625);
    g_assert_cmpfloat(later - middle, >, middle - quarter);
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 118800, 120000, FALSE); pump();
    assert_grey(row(fixture, 1), 0.9801);
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 120000, 120000, FALSE); pump();
    assert_grey(row(fixture, 1), 1);
    /* The number/time cell must reveal the row's actual background, while
     * its clickable number button keeps its contrasting dark surface. */
    GdkRGBA number_cell = background(with_class(row(fixture, 1), "broadcast-queue-number"));
    g_assert_cmpfloat(number_cell.alpha, <, 0.01);
    gtk_list_box_select_row(fixture->list, GTK_LIST_BOX_ROW(row(fixture, 1))); pump();
    assert_grey(row(fixture, 1), 1);
    GtkWidget *title = with_class(row(fixture, 1), "ld-item-title"); GdkRGBA foreground;
    gtk_style_context_get_color(gtk_widget_get_style_context(title), gtk_widget_get_state_flags(title), &foreground);
    g_assert_cmpfloat(foreground.red, <, 0.25);
    screenshot(fixture, "next-white-selected.png");
    radios_programme_queue_set_progress(fixture->queue, TRACK_B, TRACK_C, 0, 180000, FALSE); pump();
    g_assert_false(has_class(row(fixture, 1), "is-upcoming"));
    g_assert_true(has_class(row(fixture, 1), "is-playing"));
    g_assert_true(has_class(row(fixture, 2), "is-upcoming")); assert_grey(row(fixture, 2), 0);
    radios_programme_queue_set_progress(fixture->queue, TRACK_C, NULL, 50000, 150000, FALSE); pump();
    g_assert_null(with_class(fixture->queue, "is-upcoming"));
}

static void test_upcoming_pause_unknown_reset(Fixture *fixture, gconstpointer data)
{
    (void)data; open_first(fixture);
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 60000, 120000, FALSE); pump();
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 60000, 120000, TRUE);
    run_for(600); assert_grey(row(fixture, 1), 0.25);
    g_assert_true(has_class(row(fixture, 0), "is-playing"));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(with_class(row(fixture, 0), "ld-state"))), ==, "Ⅱ Duraklatıldı");
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 60000, 0, FALSE); pump();
    assert_grey(row(fixture, 1), 0);
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, "unknown-leaf", 60000, 120000, FALSE); pump();
    g_assert_null(with_class(fixture->queue, "is-upcoming"));
    radios_programme_queue_set_progress(fixture->queue, "unknown-playing", TRACK_B, 60000, 120000, FALSE); pump();
    g_assert_null(with_class(fixture->queue, "is-upcoming"));
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, BLOCK_A, 60000, 120000, FALSE); pump();
    g_assert_null(with_class(fixture->queue, "is-upcoming"));
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_A, 60000, 120000, FALSE); pump();
    g_assert_null(with_class(fixture->queue, "is-upcoming"));
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, G_MAXUINT64, 120000, FALSE); pump();
    assert_grey(row(fixture, 1), 1);
    radios_programme_queue_reset_progress(fixture->queue); pump();
    g_assert_null(with_class(fixture->queue, "is-upcoming"));
    g_assert_null(with_class(fixture->queue, "is-playing"));
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
}

static void test_upcoming_navigation_reorder(Fixture *fixture, gconstpointer data)
{
    (void)data; open_first(fixture);
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 60000, 120000, FALSE); pump();
    back(fixture); g_assert_null(with_class(fixture->queue, "is-upcoming"));
    open_first(fixture); assert_grey(row(fixture, 1), 0.25);
    radios_programme_queue_render(fixture->queue, fixture->config, TRACK_B); pump();
    g_assert_true(has_class(row(fixture, 1), "is-upcoming")); assert_grey(row(fixture, 1), 0.25);
    move(fixture, 1, 2, TRUE);
    g_assert_cmpstr(radios_programme_queue_row_id(GTK_LIST_BOX_ROW(row(fixture, 2))), ==, TRACK_B);
    g_assert_true(has_class(row(fixture, 2), "is-upcoming")); assert_grey(row(fixture, 2), 0.25);
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_C, 60000, 120000, FALSE); pump();
    g_assert_true(has_class(row(fixture, 1), "is-upcoming"));
    g_assert_false(has_class(row(fixture, 2), "is-upcoming"));
}

static void assert_inactive(GtkWidget *widget)
{
    g_assert_false(has_class(widget, "is-playing"));
    g_assert_false(has_class(widget, "is-alert-white"));
    g_assert_null(with_class(widget, "is-pressed"));
}

static void test_event_completion_stable(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_programme_queue_complete(fixture->queue, TRACK_A);
    radios_programme_queue_complete(fixture->queue, "unknown-leaf");
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
    radios_programme_queue_set_progress(fixture->queue, TRACK_C, TRACK_D, 149000, 150000, FALSE);
    g_assert_true(has_class(row(fixture, 0), "is-playing"));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(with_class(row(fixture, 0), "ld-state"))), ==, "▶ Çalıyor");
    gtk_list_box_select_row(fixture->list, GTK_LIST_BOX_ROW(row(fixture, 0)));
    radios_programme_queue_complete(fixture->queue, TRACK_C); pump();
    GtkWidget *finished = row(fixture, 0);
    g_assert_true(has_class(finished, "is-event-completed"));
    g_assert_true(has_class(finished, "is-selected"));
    assert_inactive(finished);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(with_class(finished, "ld-state"))), ==, "✓ Tamamlandı");
    /* Let GTK's ordinary selection fade settle before comparing alert phases. */
    run_for(250);
    GdkRGBA initial = background(finished);
    /* The former 500 ms timer alternated the whole completed row forever.
     * Check real computed GTK colors on both phases, including selection. */
    for (guint i = 0; i < 2; ++i) {
        run_for(530);
        GdkRGBA current = background(finished);
        g_assert_cmpfloat_with_epsilon(current.red, initial.red, 0.005);
        g_assert_cmpfloat_with_epsilon(current.green, initial.green, 0.005);
        g_assert_cmpfloat_with_epsilon(current.blue, initial.blue, 0.005);
        g_assert_cmpfloat_with_epsilon(current.alpha, initial.alpha, 0.005);
        assert_inactive(finished);
    }
    screenshot(fixture, "event-completed-stable.png");
    radios_programme_queue_set_progress(fixture->queue, TRACK_D, NULL, 0, 90000, FALSE); pump();
    g_assert_true(has_class(finished, "is-event-completed"));
    assert_inactive(finished);
    g_assert_true(has_class(row(fixture, 1), "is-playing"));
    /* A duplicate old completion cannot clear the new event's active state. */
    radios_programme_queue_complete(fixture->queue, TRACK_C); pump();
    g_assert_true(has_class(row(fixture, 1), "is-playing"));
    radios_programme_queue_render(fixture->queue, fixture->config, BLOCK_A); pump();
    g_assert_true(has_class(row(fixture, 0), "is-event-completed"));
    open_first(fixture); back(fixture);
    g_assert_true(has_class(row(fixture, 0), "is-event-completed"));
    move(fixture, 0, 1, TRUE);
    g_assert_cmpstr(radios_programme_queue_row_id(GTK_LIST_BOX_ROW(row(fixture, 1))), ==, BLOCK_A);
    g_assert_true(has_class(row(fixture, 1), "is-event-completed"));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(with_class(row(fixture, 1), "ld-state"))), ==, "✓ Tamamlandı");
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 0, 120000, FALSE); pump();
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
    /* Switching playing ids alone never fabricates a natural completion. */
    radios_programme_queue_set_progress(fixture->queue, TRACK_D, NULL, 0, 90000, FALSE); pump();
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
    radios_programme_queue_complete(fixture->queue, TRACK_D); pump();
    g_assert_nonnull(with_class(fixture->queue, "is-event-completed"));
    g_assert_null(with_class(fixture->queue, "is-playing"));
    radios_programme_queue_reset_progress(fixture->queue); pump();
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
    g_assert_null(with_class(fixture->queue, "is-alert-white"));
}

static void test_event_stop_pause_restart(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_programme_queue_set_run_config(fixture->queue, fixture->config);
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 30000, 120000, TRUE); pump();
    g_assert_true(has_class(row(fixture, 0), "is-playing"));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(with_class(row(fixture, 0), "ld-state"))), ==, "Ⅱ Duraklatıldı");
    g_assert_null(with_class(row(fixture, 0), "is-pressed"));
    /* STOP/ERROR/idle supplies no sounding leaf and no COMPLETE receipt. */
    radios_programme_queue_set_progress(fixture->queue, NULL, NULL, 0, 0, FALSE); pump();
    assert_inactive(row(fixture, 0));
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(with_class(row(fixture, 0), "ld-state"))), ==, "✓ Hazır");
    radios_programme_queue_set_progress(fixture->queue, TRACK_C, NULL, 0, 150000, FALSE);
    radios_programme_queue_complete(fixture->queue, TRACK_C); pump();
    g_assert_true(has_class(row(fixture, 0), "is-event-completed"));
    radios_programme_queue_reset_progress(fixture->queue);
    radios_programme_queue_set_run_config(fixture->queue, fixture->config);
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 0, 120000, FALSE); pump();
    g_assert_true(has_class(row(fixture, 0), "is-playing"));
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
    g_assert_null(with_class(fixture->queue, "is-alert-white"));
}

/* The mixed flow skips silent/unbound contents. Finishing the event means
 * its final genuinely played content ended, even with a silent tail. */
static void test_event_completion_silent_tail(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *silent_id = "88888888-8888-4888-8888-888888888888";
    const gchar *last_id = "99999999-9999-4999-8999-999999999999";
    JsonArray *events = json_object_get_array_member(fixture->config, "events");
    JsonObject *silent = json_object_new();
    json_object_set_string_member(silent, "id", silent_id);
    json_object_set_string_member(silent, "block_id", BLOCK_A);
    json_object_set_string_member(silent, "title", "Sessiz içerik");
    json_object_set_string_member(silent, "kind", "music");
    json_array_add_object_element(events, silent);
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); pump();
    radios_programme_queue_complete(fixture->queue, silent_id);
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
    radios_programme_queue_complete(fixture->queue, TRACK_C); pump();
    g_assert_true(has_class(row(fixture, 0), "is-event-completed"));
    radios_programme_queue_reset_progress(fixture->queue);

    JsonObject *last = json_object_new();
    json_object_set_string_member(last, "id", last_id);
    json_object_set_string_member(last, "block_id", BLOCK_A);
    json_object_set_string_member(last, "title", "Son sesli parça");
    json_object_set_string_member(last, "kind", "music");
    json_object_set_string_member(last, "media_id", "media-e");
    json_object_set_int_member(last, "duration_ms", 90000);
    json_array_add_object_element(events, last);
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); pump();
    radios_programme_queue_complete(fixture->queue, TRACK_C);
    radios_programme_queue_complete(fixture->queue, silent_id);
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
    radios_programme_queue_complete(fixture->queue, last_id); pump();
    g_assert_true(has_class(row(fixture, 0), "is-event-completed"));
    radios_programme_queue_reset_progress(fixture->queue);

    /* Existing trailing tracks can also become silent after an edit. */
    json_object_remove_member(last, "media_id");
    for (guint i = 0; i < json_array_get_length(events); ++i) {
        JsonObject *event = json_array_get_object_element(events, i);
        if (g_str_equal(json_object_get_string_member(event, "id"), TRACK_C))
            json_object_remove_member(event, "media_id");
    }
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); pump();
    radios_programme_queue_complete(fixture->queue, TRACK_A);
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
    radios_programme_queue_complete(fixture->queue, TRACK_B); pump();
    g_assert_true(has_class(row(fixture, 0), "is-event-completed"));
    radios_programme_queue_reset_progress(fixture->queue);
    for (guint i = 0; i < json_array_get_length(events); ++i) {
        JsonObject *event = json_array_get_object_element(events, i);
        if (g_str_equal(json_object_get_string_member(event, "block_id"), BLOCK_A))
            json_object_remove_member(event, "media_id");
    }
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); pump();
    radios_programme_queue_complete(fixture->queue, TRACK_A);
    radios_programme_queue_complete(fixture->queue, TRACK_B);
    radios_programme_queue_complete(fixture->queue, TRACK_C);
    radios_programme_queue_complete(fixture->queue, silent_id);
    radios_programme_queue_complete(fixture->queue, last_id);
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
}

/* Accepted playback owns its original event even while the editable draft
 * is moved, appended to, or removed. Mutating the caller's input after handing
 * it to the queue also checks that the accepted model was deeply copied. */
static void test_event_completion_frozen_owner(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_programme_queue_set_run_config(fixture->queue, fixture->config);
    JsonArray *events = json_object_get_array_member(fixture->config, "events");
    for (guint i = 0; i < json_array_get_length(events); ++i) {
        JsonObject *event = json_array_get_object_element(events, i);
        if (g_str_equal(json_object_get_string_member(event, "id"), TRACK_C))
            json_object_set_string_member(event, "block_id", BLOCK_B);
    }
    JsonObject *draft_tail = json_object_new();
    json_object_set_string_member(draft_tail, "id", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    json_object_set_string_member(draft_tail, "block_id", BLOCK_A);
    json_object_set_string_member(draft_tail, "title", "Sonradan eklenen taslak parça");
    json_object_set_string_member(draft_tail, "kind", "music");
    json_object_set_string_member(draft_tail, "media_id", "draft-tail-media");
    json_array_add_object_element(events, draft_tail);
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); pump();
    radios_programme_queue_complete(fixture->queue, TRACK_B);
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
    radios_programme_queue_complete(fixture->queue, TRACK_C); pump();
    g_assert_true(has_class(row(fixture, 0), "is-event-completed"));
    g_assert_false(has_class(row(fixture, 1), "is-event-completed"));
    /* Replaying C still clears the original accepted event's completion rather
     * than the edited destination event. */
    radios_programme_queue_set_progress(fixture->queue, TRACK_C, TRACK_D, 0, 150000, FALSE); pump();
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
    radios_programme_queue_complete(fixture->queue, TRACK_C); pump();
    g_assert_true(has_class(row(fixture, 0), "is-event-completed"));

    RadioSProgrammeTree *tree = radios_programme_tree_new(fixture->config);
    g_assert_true(radios_programme_tree_remove(tree, BLOCK_A));
    JsonNode *updated = radios_programme_tree_config(tree, fixture->config);
    radios_programme_tree_free(tree);
    radios_programme_queue_render(fixture->queue, json_node_get_object(updated), NULL); pump();
    radios_programme_queue_complete(fixture->queue, TRACK_C); pump();
    g_assert_null(with_class(fixture->queue, "is-event-completed"));
    g_assert_cmpstr(radios_programme_queue_row_id(GTK_LIST_BOX_ROW(row(fixture, 0))), ==, BLOCK_B);
    json_node_free(updated);
    radios_programme_queue_reset_progress(fixture->queue);
    radios_programme_queue_set_run_config(fixture->queue, NULL);
}

static void test_independent_event_runtime_is_separate_from_primary(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 1000, 120000, FALSE);
    GtkWidget *primary = row(fixture, 0), *independent = row(fixture, 1);
    GtkWidget *badge = find(independent, "programme-row-runtime"); g_assert_nonnull(badge);
    g_assert_false(gtk_widget_get_visible(badge));
    JsonArray *runs = json_array_new(); JsonObject *run = json_object_new();
    json_object_set_string_member(run, "origin_block_id", BLOCK_B); json_object_set_int_member(run, "source", 2);
    json_object_set_int_member(run, "state", RADIOS_PLAYOUT_PLAYING);
    json_object_set_int_member(run, "cue_in_ms", 1000); json_object_set_int_member(run, "position_ms", 2200);
    json_object_set_int_member(run, "duration_ms", 91000); json_object_set_boolean_member(run, "completed", FALSE);
    json_array_add_object_element(runs, run);
    radios_programme_queue_set_event_runs(fixture->queue, runs); pump();
    g_assert_true(gtk_widget_get_visible(badge));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(badge)), ==, "Yarı AutoDJ · Oynatıcı 3 · Çalıyor · 0:01 / 1:30");
    g_assert_true(has_class(primary, "is-playing")); g_assert_false(has_class(independent, "is-playing"));
    g_assert_true(has_class(independent, "is-event-running"));
    json_object_set_int_member(run, "state", RADIOS_PLAYOUT_PAUSED);
    radios_programme_queue_set_event_runs(fixture->queue, runs);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(badge)), "Duraklatıldı"));
    json_object_set_string_member(run, "source_name", "Sonradan değişen çağıran kopyası");
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); pump();
    primary = row(fixture, 0); independent = row(fixture, 1); badge = find(independent, "programme-row-runtime");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(badge)), "Oynatıcı 3"));
    json_object_set_int_member(run, "state", RADIOS_PLAYOUT_FINISHED); json_object_set_boolean_member(run, "completed", TRUE);
    json_object_set_boolean_member(run, "continuing", TRUE);
    json_object_remove_member(run, "source_name");
    radios_programme_queue_set_event_runs(fixture->queue, runs);
    radios_programme_queue_complete_event_run(fixture->queue, BLOCK_B, 2); pump();
    g_assert_true(has_class(independent, "is-event-run-completed"));
    g_assert_false(has_class(independent, "is-event-completed")); g_assert_false(has_class(independent, "is-event-running"));
    g_assert_true(has_class(primary, "is-playing"));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(badge)), "Tamamlandı"));
    radios_programme_queue_set_event_runs(fixture->queue, NULL); pump();
    g_assert_true(has_class(independent, "is-event-run-completed"));
    radios_programme_queue_set_progress(fixture->queue, NULL, NULL, 0, 0, FALSE);
    g_assert_true(gtk_widget_get_visible(badge));
    radios_programme_queue_reset_progress(fixture->queue); pump();
    g_assert_false(gtk_widget_get_visible(badge)); g_assert_false(has_class(independent, "is-event-run-completed"));
    json_object_set_int_member(run, "state", RADIOS_PLAYOUT_PREPARING); json_object_set_boolean_member(run, "completed", FALSE);
    radios_programme_queue_set_event_runs(fixture->queue, runs);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(badge)), "Hazırlanıyor"));
    radios_programme_queue_set_event_runs(fixture->queue, NULL);
    g_assert_false(gtk_widget_get_visible(badge)); g_assert_false(has_class(independent, "is-event-run-completed"));
    radios_programme_queue_complete_event_run(fixture->queue, BLOCK_B, 2);
    json_object_set_int_member(run, "state", RADIOS_PLAYOUT_PLAYING);
    radios_programme_queue_set_event_runs(fixture->queue, runs);
    g_assert_true(has_class(independent, "is-event-running")); g_assert_false(has_class(independent, "is-event-run-completed"));
    RadioSProgrammeTree *tree = radios_programme_tree_new(fixture->config);
    g_assert_true(radios_programme_tree_remove(tree, BLOCK_B));
    JsonNode *removed = radios_programme_tree_config(tree, fixture->config); radios_programme_tree_free(tree);
    radios_programme_queue_render(fixture->queue, json_node_get_object(removed), NULL);
    radios_programme_queue_set_event_runs(fixture->queue, runs);
    g_assert_null(with_class(fixture->queue, "is-event-running")); g_assert_null(with_class(fixture->queue, "is-event-run-completed"));
    json_node_free(removed); json_array_unref(runs);
}

static const gchar *player_text(GtkWidget *row)
{
    GtkWidget *label = find(row, "programme-row-player"); g_assert_nonnull(label);
    return gtk_label_get_text(GTK_LABEL(label));
}

/* Every event and content row names its player at the right: "otomatik" until
 * known, then the queue's own player, the accepted plan's player and finally
 * the player the content really sounded on. Event rows list all their players. */
static void test_row_players(Fixture *fixture, gconstpointer data)
{
    (void)data;
    g_assert_cmpstr(player_text(row(fixture, 0)), ==, "Oynatıcı · otomatik");
    g_assert_cmpstr(player_text(row(fixture, 2)), ==, "Oynatıcı · otomatik");
    const gchar *names[32] = {NULL}; names[0] = "Ana"; names[2] = "Stüdyo";
    radios_programme_queue_set_player_context(fixture->queue, 3, names); pump();
    for (guint index = 0; index < 3; ++index) g_assert_cmpstr(player_text(row(fixture, index)), ==, "Stüdyo");
    /* Invalid players are ignored. */
    radios_programme_queue_set_player(fixture->queue, TRACK_A, 0); radios_programme_queue_set_player(fixture->queue, TRACK_A, 33);
    radios_programme_queue_set_player(fixture->queue, NULL, 1); radios_programme_queue_set_player(fixture->queue, "", 1); pump();
    g_assert_cmpstr(player_text(row(fixture, 0)), ==, "Stüdyo");
    radios_programme_queue_set_player(fixture->queue, TRACK_A, 1); pump();
    g_assert_cmpstr(player_text(row(fixture, 0)), ==, "Ana, Stüdyo");
    g_assert_cmpstr(player_text(row(fixture, 1)), ==, "Stüdyo");
    radios_programme_queue_set_expanded(fixture->queue, BLOCK_A, TRUE); pump();
    g_assert_cmpstr(player_text(row(fixture, 0)), ==, "Ana");
    g_assert_cmpstr(player_text(row(fixture, 1)), ==, "Stüdyo");
    g_assert_cmpstr(player_text(row(fixture, 2)), ==, "Stüdyo");
    screenshot(fixture, "row-players-contents.png");
    /* The accepted plan's player wins over the queue's own player. */
    JsonObject *accepted = radios_programme_clone_object(fixture->config);
    JsonArray *events = json_object_get_array_member(accepted, "events");
    for (guint i = 0; i < json_array_get_length(events); ++i) {
        JsonObject *event = json_array_get_object_element(events, i);
        if (g_str_equal(json_object_get_string_member(event, "id"), TRACK_B)) json_object_set_int_member(event, "player", 2);
    }
    radios_programme_queue_set_run_config(fixture->queue, accepted); json_object_unref(accepted); pump();
    g_assert_cmpstr(player_text(row(fixture, 0)), ==, "Ana");
    g_assert_cmpstr(player_text(row(fixture, 1)), ==, "Oynatıcı 2");
    g_assert_cmpstr(player_text(row(fixture, 2)), ==, "Stüdyo");
    radios_programme_queue_set_player(fixture->queue, TRACK_B, 3); pump();
    g_assert_cmpstr(player_text(row(fixture, 1)), ==, "Stüdyo");
    /* An independent event run sounds its whole event on its own player. */
    radios_programme_queue_set_expanded(fixture->queue, BLOCK_A, FALSE); pump();
    JsonArray *runs = json_array_new(); JsonObject *run = json_object_new();
    json_object_set_string_member(run, "origin_block_id", BLOCK_B); json_object_set_int_member(run, "source", 3);
    json_object_set_int_member(run, "state", RADIOS_PLAYOUT_PLAYING); json_object_set_boolean_member(run, "completed", FALSE);
    json_array_add_object_element(runs, run);
    radios_programme_queue_set_event_runs(fixture->queue, runs); json_array_unref(runs); pump();
    g_assert_cmpstr(player_text(row(fixture, 1)), ==, "Oynatıcı 4");
    screenshot(fixture, "row-players-events.png");
    /* Resetting the run forgets what sounded where; the context stays. */
    radios_programme_queue_reset_progress(fixture->queue); pump();
    g_assert_cmpstr(player_text(row(fixture, 0)), ==, "Stüdyo");
    g_assert_cmpstr(player_text(row(fixture, 1)), ==, "Stüdyo");
    radios_programme_queue_set_player_context(fixture->queue, 0, NULL); pump();
    g_assert_cmpstr(player_text(row(fixture, 0)), ==, "Oynatıcı · otomatik");
}

static void test_completion_destroy(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_programme_queue_complete(fixture->queue, TRACK_C);
    gtk_widget_destroy(fixture->queue);
    /* Completed rows own no repeating alert timer after destruction. */
    run_for(60);
}

/* ---- Kılavuz 2026-10-18 · yayın sırası (PDF s.8–16, akım A) -------------
 * Every case drives the real GTK rows: their signals, GdkEvents delivered with
 * gtk_widget_event, and the actions the row hooks record. */
#define K_MAIN RADIOS_PROGRAMME_ROW_MAIN
#define K_SUB RADIOS_PROGRAMME_ROW_SUB
#define K_BLOCK(id, title, kind) "{\"id\":\"" id "\",\"title\":\"" title "\",\"kind\":\"" kind "\"}"
#define K_ITEM(id, block, title, kind, ms) "{\"id\":\"" id "\",\"block_id\":\"" block "\",\"title\":\"" title \
    "\",\"kind\":\"" kind "\",\"duration_ms\":" ms ",\"media_id\":\"media-" id "\"}"
/* Numbering: music, its sweeper and jingle, music, advertisement with two contents. */
#define KN_MUSIC_1 "c1000001-0000-4000-8000-000000000001"
#define KN_SWEEPER "c1000001-0000-4000-8000-000000000002"
#define KN_JINGLE "c1000001-0000-4000-8000-000000000003"
#define KN_MUSIC_2 "c1000001-0000-4000-8000-000000000004"
#define KN_ADVERT "c1000001-0000-4000-8000-000000000005"
#define KN_SONG_1 "c1000001-0000-4000-8000-000000000011"
#define KN_SWEEP "c1000001-0000-4000-8000-000000000012"
#define KN_JING "c1000001-0000-4000-8000-000000000013"
#define KN_SONG_2 "c1000001-0000-4000-8000-000000000014"
#define KN_AD_1 "c1000001-0000-4000-8000-000000000015"
#define KN_AD_2 "c1000001-0000-4000-8000-000000000016"
/* Display window: four events; the second carries an attached sweeper. */
#define KW_EVENT_1 "c2000002-0000-4000-8000-000000000001"
#define KW_EVENT_2 "c2000002-0000-4000-8000-000000000002"
#define KW_SWEEPER "c2000002-0000-4000-8000-000000000003"
#define KW_EVENT_3 "c2000002-0000-4000-8000-000000000004"
#define KW_EVENT_4 "c2000002-0000-4000-8000-000000000005"
#define KW_SONG_1 "c2000002-0000-4000-8000-000000000011"
#define KW_SONG_2 "c2000002-0000-4000-8000-000000000012"
#define KW_SWEEP "c2000002-0000-4000-8000-000000000013"
#define KW_SONG_3 "c2000002-0000-4000-8000-000000000014"
#define KW_SONG_4 "c2000002-0000-4000-8000-000000000015"
/* Live: a talk step between two music events. */
#define KL_MUSIC_1 "c3000003-0000-4000-8000-000000000001"
#define KL_LIVE "c3000003-0000-4000-8000-000000000002"
#define KL_MUSIC_2 "c3000003-0000-4000-8000-000000000003"
#define KL_SONG_1 "c3000003-0000-4000-8000-000000000011"
#define KL_SONG_2 "c3000003-0000-4000-8000-000000000012"

static const gchar kilavuz_numbering[] = "{\"title\":\"Program\",\"blocks\":["
    K_BLOCK(KN_MUSIC_1, "Müzik eventi", "music") "," K_BLOCK(KN_SWEEPER, "Sweeper / Enerji", "sweeper") ","
    K_BLOCK(KN_JINGLE, "İstasyon jingle", "jingle") "," K_BLOCK(KN_MUSIC_2, "Müzik / House & Club A", "music") ","
    K_BLOCK(KN_ADVERT, "Reklam eventi", "advertisement") "],\"events\":["
    K_ITEM(KN_SONG_1, KN_MUSIC_1, "Şarkı", "music", "180000") ","
    K_ITEM(KN_SWEEP, KN_SWEEPER, "Enerji", "sweeper", "10000") ","
    K_ITEM(KN_JING, KN_JINGLE, "Radyo S", "jingle", "5000") ","
    K_ITEM(KN_SONG_2, KN_MUSIC_2, "House", "music", "392000") ","
    K_ITEM(KN_AD_1, KN_ADVERT, "Reklam 1", "advertisement", "30000") ","
    K_ITEM(KN_AD_2, KN_ADVERT, "Reklam 2", "advertisement", "20000") "]}";
static const gchar kilavuz_window[] = "{\"title\":\"Program\",\"blocks\":["
    K_BLOCK(KW_EVENT_1, "Event 1", "music") "," K_BLOCK(KW_EVENT_2, "Event 2", "music") ","
    K_BLOCK(KW_SWEEPER, "Sweeper", "sweeper") "," K_BLOCK(KW_EVENT_3, "Event 3", "music") ","
    K_BLOCK(KW_EVENT_4, "Event 4", "music") "],\"events\":["
    K_ITEM(KW_SONG_1, KW_EVENT_1, "Parça 1", "music", "120000") ","
    K_ITEM(KW_SONG_2, KW_EVENT_2, "Parça 2", "music", "120000") ","
    K_ITEM(KW_SWEEP, KW_SWEEPER, "Sweeper", "sweeper", "10000") ","
    K_ITEM(KW_SONG_3, KW_EVENT_3, "Parça 3", "music", "120000") ","
    K_ITEM(KW_SONG_4, KW_EVENT_4, "Parça 4", "music", "120000") "]}";
static const gchar kilavuz_live[] = "{\"title\":\"Program\",\"blocks\":["
    K_BLOCK(KL_MUSIC_1, "Sabah müzikleri", "music") ","
    "{\"id\":\"" KL_LIVE "\",\"title\":\"Konuşma\",\"kind\":\"live\",\"live\":{\"label\":\"talk\",\"channels\":[3],"
    "\"automation_id\":\"\",\"end_rule\":\"manual\",\"duration_ms\":0}},"
    K_BLOCK(KL_MUSIC_2, "Akşam müzikleri", "music") "],\"events\":["
    K_ITEM(KL_SONG_1, KL_MUSIC_1, "Birinci parça", "music", "120000") ","
    K_ITEM(KL_SONG_2, KL_MUSIC_2, "İkinci parça", "music", "90000") "]}";

static guint actions_of(Fixture *fixture, RadioSProgrammeRowAction action, const gchar *id)
{
    guint count = 0;
    for (guint i = 0; i < fixture->actions->len; ++i)
        if (g_array_index(fixture->actions, RadioSProgrammeRowAction, i) == action &&
            (!id || !g_strcmp0(g_ptr_array_index(fixture->action_ids, i), id))) ++count;
    return count;
}
static void actions_clear(Fixture *fixture)
{ g_array_set_size(fixture->actions, 0); g_ptr_array_set_size(fixture->action_ids, 0); }

static void config_use(Fixture *fixture, const gchar *source)
{
    JsonParser *parser = json_parser_new(); GError *error = NULL;
    g_assert_true(json_parser_load_from_data(parser, source, -1, &error)); g_assert_no_error(error);
    json_object_unref(fixture->config);
    fixture->config = json_object_ref(json_node_get_object(json_parser_get_root(parser))); g_object_unref(parser);
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); pump();
}
static JsonObject *config_event(Fixture *fixture, guint index)
{ return json_array_get_object_element(json_object_get_array_member(fixture->config, "events"), index); }
static void set_json_member(JsonObject *object, const gchar *member, const gchar *json)
{
    GError *error = NULL; JsonNode *node = json_from_string(json, &error);
    g_assert_no_error(error); g_assert_nonnull(node); json_object_set_member(object, member, node);
}

static guint rows_count(Fixture *fixture)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(fixture->list));
    guint count = g_list_length(rows); g_list_free(rows); return count;
}
static GtkWidget *row_find(Fixture *fixture, const gchar *id)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(fixture->list)); GtkWidget *result = NULL;
    for (GList *item = rows; item && !result; item = item->next)
        if (!g_strcmp0(radios_programme_queue_row_id(GTK_LIST_BOX_ROW(item->data)), id)) result = item->data;
    g_list_free(rows); return result;
}
static GtkWidget *row_of(Fixture *fixture, const gchar *id)
{ GtkWidget *result = row_find(fixture, id); g_assert_nonnull(result); return result; }
static const gchar *text_of(GtkWidget *root, const gchar *class_name)
{
    GtkWidget *label = with_class(root, class_name); g_assert_true(GTK_IS_LABEL(label));
    return gtk_label_get_text(GTK_LABEL(label));
}
static GtkWidget *with_text(GtkWidget *root, const gchar *value)
{
    if (GTK_IS_LABEL(root) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(root)), value)) return root;
    if (GTK_IS_BUTTON(root) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), value)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = with_text(item->data, value);
    g_list_free(children); return result;
}
static GtkWidget *control(GtkWidget *item, const gchar *name)
{
    gchar *full = g_strconcat("programme-row-", name, NULL);
    GtkWidget *button = find(item, full); g_free(full);
    g_assert_true(GTK_IS_BUTTON(button)); return button;
}

typedef struct { const gchar *id, *number; RadioSProgrammeRowLevel level; } RowExpect;
/* The list shows exactly these rows (id, displayed number, level); only a
 * sub-row is indented beside a branch line to its main row. */
static void assert_layout(Fixture *fixture, const RowExpect *expected, guint count)
{
    g_assert_cmpuint(rows_count(fixture), ==, count);
    for (guint index = 0; index < count; ++index) {
        GtkWidget *item = row(fixture, index); gboolean sub = expected[index].level == K_SUB;
        g_assert_cmpstr(radios_programme_queue_row_id(GTK_LIST_BOX_ROW(item)), ==, expected[index].id);
        g_assert_cmpint(radios_programme_queue_row_level(GTK_LIST_BOX_ROW(item)), ==, expected[index].level);
        g_assert_cmpstr(radios_programme_queue_row_number(GTK_LIST_BOX_ROW(item)), ==, expected[index].number);
        g_assert_cmpstr(text_of(item, "broadcast-queue-index"), ==, expected[index].number);
        g_assert_cmpint(find(item, "programme-row-branch") != NULL, ==, sub);
        g_assert_cmpint(has_class(item, "programme-sub-row"), ==, sub);
    }
}
static gchar *controls_text(GtkWidget *item)
{
    GtkWidget *box = with_class(item, "broadcast-row-actions"); g_assert_true(GTK_IS_BOX(box));
    GString *value = g_string_new(NULL); GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *child = children; child; child = child->next) {
        const gchar *label = GTK_IS_BUTTON(child->data) ? gtk_button_get_label(GTK_BUTTON(child->data)) : NULL;
        g_string_append_printf(value, "%s%s", value->len ? " " : "", label ? label : "?");
    }
    g_list_free(children); return g_string_free(value, FALSE);
}
/* S · V · L · R · ×: the former Ö has no button (Özellikler is in the menu). */
static void assert_controls(GtkWidget *item)
{
    gchar *controls = controls_text(item); g_assert_cmpstr(controls, ==, "S V R L ×"); g_free(controls);
    g_assert_null(with_text(item, "Ö")); g_assert_null(find(item, "programme-row-properties"));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(control(item, "live"))), ==, "L");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(control(item, "talk"))), ==, "R");
    g_assert_true(gtk_widget_get_sensitive(control(item, "live")));
}
static void assert_play(GtkWidget *item, gboolean transition)
{
    GtkWidget *play = control(item, "play");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(play)), ==, transition ? "G" : "S");
    g_assert_cmpint(has_class(play, "is-transition"), ==, transition);
}

static GdkEvent *synthetic(GtkWidget *widget, GdkEventType type, gboolean keyboard)
{
    GdkWindow *window = gtk_widget_get_window(widget);
    g_assert_nonnull(window); g_assert_true(gdk_window_is_viewable(window));
    GdkEvent *event = gdk_event_new(type);
    event->any.window = g_object_ref(window); event->any.send_event = TRUE;
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(widget));
    gdk_event_set_device(event, keyboard ? gdk_seat_get_keyboard(seat) : gdk_seat_get_pointer(seat));
    return event;
}
/* A GdkEventButton through gtk_widget_event, at the widget's centre. */
static void pointer_button(GtkWidget *widget, GdkEventType type, guint button)
{
    GdkEvent *event = synthetic(widget, type, FALSE);
    GtkAllocation allocation; gtk_widget_get_allocation(widget, &allocation);
    gint x = 0, y = 0; gdk_window_get_origin(event->any.window, &x, &y);
    event->button.button = button; event->button.time = GDK_CURRENT_TIME;
    event->button.x = allocation.x + allocation.width / 2.0; event->button.y = allocation.y + allocation.height / 2.0;
    event->button.x_root = x + event->button.x; event->button.y_root = y + event->button.y;
    gtk_widget_event(widget, event); gdk_event_free(event);
}
static void pointer_leave(GtkWidget *widget)
{
    GdkEvent *event = synthetic(widget, GDK_LEAVE_NOTIFY, FALSE);
    event->crossing.mode = GDK_CROSSING_NORMAL; event->crossing.detail = GDK_NOTIFY_NONLINEAR;
    event->crossing.time = GDK_CURRENT_TIME;
    gtk_widget_event(widget, event); gdk_event_free(event);
}
static void key_event(GtkWidget *widget, GdkEventType type, guint keyval)
{
    GdkEvent *event = synthetic(widget, type, TRUE);
    event->key.keyval = keyval; event->key.time = GDK_CURRENT_TIME;
    gtk_widget_event(widget, event); gdk_event_free(event);
}

static gboolean menu_shown(GtkWidget *menu)
{
    GdkWindow *window = gtk_widget_get_window(menu);
    return gtk_widget_get_mapped(menu) && window && gdk_window_is_viewable(window);
}
/* A third-button press on the row body opens the row's own context menu. */
static GtkWidget *context_menu(GtkWidget *item)
{
    g_assert_null(gtk_menu_get_for_attach_widget(item));
    pointer_button(gtk_bin_get_child(GTK_BIN(item)), GDK_BUTTON_PRESS, 3);
    GList *menus = gtk_menu_get_for_attach_widget(item);
    g_assert_nonnull(menus); g_assert_null(menus->next);
    GtkWidget *menu = menus->data;
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!menu_shown(menu) && g_get_monotonic_time() < deadline) pump();
    g_assert_true(menu_shown(menu)); return menu;
}

static GdkRGBA color_of(GtkWidget *widget)
{
    GdkRGBA color;
    gtk_style_context_get_color(gtk_widget_get_style_context(widget), gtk_widget_get_state_flags(widget), &color);
    return color;
}
static void assert_rgb(GdkRGBA color, guint32 rgb)
{
    g_assert_cmpfloat_with_epsilon(color.red, ((rgb >> 16) & 0xff) / 255.0, 0.01);
    g_assert_cmpfloat_with_epsilon(color.green, ((rgb >> 8) & 0xff) / 255.0, 0.01);
    g_assert_cmpfloat_with_epsilon(color.blue, (rgb & 0xff) / 255.0, 0.01);
}
static void assert_same_color(GdkRGBA color, GdkRGBA expected)
{
    g_assert_cmpfloat_with_epsilon(color.red, expected.red, 0.005);
    g_assert_cmpfloat_with_epsilon(color.green, expected.green, 0.005);
    g_assert_cmpfloat_with_epsilon(color.blue, expected.blue, 0.005);
    g_assert_cmpfloat_with_epsilon(color.alpha, expected.alpha, 0.005);
}
/* The default theme animates a button's colour change (about 200 ms); the
 * colour is checked once it has settled. */
static void assert_rgb_settled(GtkWidget *widget, guint32 rgb)
{
    gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    for (;;) {
        GdkRGBA color = color_of(widget);
        gboolean near = ABS(color.red - ((rgb >> 16) & 0xff) / 255.0) < 0.01 &&
            ABS(color.green - ((rgb >> 8) & 0xff) / 255.0) < 0.01 && ABS(color.blue - (rgb & 0xff) / 255.0) < 0.01;
        if (near || g_get_monotonic_time() >= deadline) { assert_rgb(color, rgb); return; }
        run_for(20);
    }
}

typedef struct { gboolean on, off, elsewhere; } Blink;
/* Samples is-ending for milliseconds: whether target showed both phases and
 * whether any other row (or extra) ever showed it. */
static Blink blink_watch(Fixture *fixture, GtkWidget *target, GtkWidget *extra, guint milliseconds)
{
    Blink blink = {FALSE, FALSE, FALSE};
    gint64 until = g_get_monotonic_time() + (gint64)milliseconds * 1000;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        gboolean ending = has_class(target, "is-ending");
        blink.on |= ending; blink.off |= !ending;
        if (extra) blink.elsewhere |= has_class(extra, "is-ending");
        GList *rows = gtk_container_get_children(GTK_CONTAINER(fixture->list));
        for (GList *item = rows; item; item = item->next)
            if (item->data != target) blink.elsewhere |= has_class(item->data, "is-ending");
        g_list_free(rows); g_usleep(2000);
    } while (g_get_monotonic_time() < until);
    return blink;
}

/* (a) PDF s.8, s.15 (A-1, A-3): S · V · L · R · × without Ö; R is a real
 * press/release push-to-talk on the on-air event only and never a transport
 * command; L acts on the row's event, also from a content row. */
static void test_kilavuz_row_controls(Fixture *fixture, gconstpointer data)
{
    (void)data;
    for (guint index = 0; index < 3; ++index) {
        assert_controls(row(fixture, index));
        g_assert_false(gtk_widget_get_sensitive(control(row(fixture, index), "talk")));
    }
    /* A passive R starts nothing. */
    GtkWidget *talk = control(row(fixture, 0), "talk");
    pointer_button(talk, GDK_BUTTON_PRESS, 1); pointer_button(talk, GDK_BUTTON_RELEASE, 1); pump();
    g_assert_cmpuint(fixture->actions->len, ==, 0);
    radios_programme_queue_set_talk_available(fixture->queue, BLOCK_A); pump();
    g_assert_true(gtk_widget_get_sensitive(talk));
    g_assert_false(gtk_widget_get_sensitive(control(row(fixture, 1), "talk")));
    g_assert_false(gtk_widget_get_sensitive(control(row(fixture, 2), "talk")));
    pointer_button(talk, GDK_BUTTON_PRESS, 1); pump();
    g_assert_cmpuint(fixture->actions->len, ==, 1);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_TALK_PRESS, BLOCK_A), ==, 1);
    g_assert_true(has_class(talk, "is-held"));
    pointer_button(talk, GDK_BUTTON_RELEASE, 1); pump();
    g_assert_cmpuint(fixture->actions->len, ==, 2);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_TALK_RELEASE, BLOCK_A), ==, 1);
    g_assert_false(has_class(talk, "is-held"));
    /* The keyboard holds R the same way. */
    key_event(talk, GDK_KEY_PRESS, GDK_KEY_space);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_TALK_PRESS, BLOCK_A), ==, 2);
    key_event(talk, GDK_KEY_RELEASE, GDK_KEY_space);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_TALK_RELEASE, BLOCK_A), ==, 2);
    /* A held R also ends when the pointer leaves it (no second release)... */
    pointer_button(talk, GDK_BUTTON_PRESS, 1); pointer_leave(talk);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_TALK_RELEASE, BLOCK_A), ==, 3);
    pointer_button(talk, GDK_BUTTON_RELEASE, 1);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_TALK_RELEASE, BLOCK_A), ==, 3);
    /* ...and when its event stops being the on-air event. */
    pointer_button(talk, GDK_BUTTON_PRESS, 1);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_TALK_PRESS, BLOCK_A), ==, 4);
    radios_programme_queue_set_talk_available(fixture->queue, BLOCK_B); pump();
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_TALK_RELEASE, BLOCK_A), ==, 4);
    g_assert_false(gtk_widget_get_sensitive(talk)); g_assert_false(has_class(talk, "is-held"));
    g_assert_true(gtk_widget_get_sensitive(control(row(fixture, 1), "talk")));
    pointer_button(talk, GDK_BUTTON_RELEASE, 1); pump();
    g_assert_cmpuint(fixture->actions->len, ==, 8);
    /* Pressing or releasing R never starts, advances or edits the flow. */
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_PLAY, NULL), ==, 0);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_REMOVE, NULL), ==, 0);
    actions_clear(fixture);
    gtk_button_clicked(GTK_BUTTON(control(row(fixture, 1), "live"))); pump();
    g_assert_cmpuint(fixture->actions->len, ==, 1);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_LIVE, BLOCK_B), ==, 1);
    /* Content rows carry the same controls; L and R act on their event. */
    open_first(fixture);
    for (guint index = 0; index < 3; ++index) {
        assert_controls(row(fixture, index));
        g_assert_false(gtk_widget_get_sensitive(control(row(fixture, index), "talk")));
    }
    radios_programme_queue_set_talk_available(fixture->queue, BLOCK_A); pump();
    actions_clear(fixture);
    gtk_button_clicked(GTK_BUTTON(control(row(fixture, 1), "live")));
    talk = control(row(fixture, 1), "talk"); g_assert_true(gtk_widget_get_sensitive(talk));
    pointer_button(talk, GDK_BUTTON_PRESS, 1); pointer_button(talk, GDK_BUTTON_RELEASE, 1); pump();
    g_assert_cmpuint(fixture->actions->len, ==, 3);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_LIVE, BLOCK_A), ==, 1);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_TALK_PRESS, BLOCK_A), ==, 1);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_TALK_RELEASE, BLOCK_A), ==, 1);
}

/* (b) PDF s.15 (A-1): Özellikler lives in the row's context menu. */
static void test_kilavuz_context_menu(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GtkWidget *item = row(fixture, 1), *menu = context_menu(item);
    g_assert_cmpstr(fixture->selected, ==, BLOCK_B); g_assert_true(has_class(item, "is-selected"));
    GtkWidget *properties = find(menu, "programme-menu-properties");
    g_assert_true(GTK_IS_MENU_ITEM(properties));
    g_assert_cmpstr(gtk_menu_item_get_label(GTK_MENU_ITEM(properties)), ==, "Özellikler");
    g_assert_true(GTK_IS_MENU_ITEM(find(menu, "programme-menu-contents")));
    g_assert_cmpuint(fixture->actions->len, ==, 0);
    gtk_menu_shell_activate_item(GTK_MENU_SHELL(menu), properties, TRUE); pump();
    g_assert_false(menu_shown(menu));
    g_assert_cmpuint(fixture->actions->len, ==, 1);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_PROPERTIES, BLOCK_B), ==, 1);
    /* Özellikler neither opens the event nor rebuilds the list. */
    g_assert_false(gtk_widget_is_visible(with_class(fixture->queue, "programme-queue-navigation")));
    g_assert_true(row(fixture, 1) == item);
    /* A content row's menu offers Özellikler for that content only. */
    open_first(fixture);
    item = row(fixture, 2); menu = context_menu(item);
    g_assert_cmpstr(fixture->selected, ==, TRACK_C);
    g_assert_null(find(menu, "programme-menu-contents"));
    properties = find(menu, "programme-menu-properties"); g_assert_true(GTK_IS_MENU_ITEM(properties));
    gtk_menu_shell_activate_item(GTK_MENU_SHELL(menu), properties, TRUE); pump();
    g_assert_cmpuint(fixture->actions->len, ==, 2);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_PROPERTIES, TRACK_C), ==, 1);
}

/* (c) PDF s.13–14 (A-14, A-15): attached jingle/sweeper events are 1.1, 1.2
 * of the event above; the next independent event continues as 2; an
 * advertisement's contents are N.1, N.2. Kind accents never recolour S/G. */
static void test_kilavuz_sub_row_numbering(Fixture *fixture, gconstpointer data)
{
    (void)data;
    config_use(fixture, kilavuz_numbering);
    const RowExpect events[] = {{KN_MUSIC_1, "1", K_MAIN}, {KN_SWEEPER, "1.1", K_SUB}, {KN_JINGLE, "1.2", K_SUB},
        {KN_MUSIC_2, "2", K_MAIN}, {KN_ADVERT, "3", K_MAIN}, {KN_AD_1, "3.1", K_SUB}, {KN_AD_2, "3.2", K_SUB}};
    assert_layout(fixture, events, G_N_ELEMENTS(events));
    g_assert_true(GTK_IS_DRAWING_AREA(find(row_of(fixture, KN_SWEEPER), "programme-row-branch")));
    g_assert_true(GTK_IS_DRAWING_AREA(find(row_of(fixture, KN_AD_2), "programme-row-branch")));
    g_assert_cmpstr(text_of(row_of(fixture, KN_SWEEPER), "broadcast-queue-artist"), ==, "Event · Sweeper");
    g_assert_cmpstr(text_of(row_of(fixture, KN_JINGLE), "broadcast-queue-artist"), ==, "Event · Jingle");
    g_assert_cmpstr(text_of(row_of(fixture, KN_ADVERT), "broadcast-queue-artist"), ==, "Event · Reklam");
    g_assert_cmpstr(text_of(row_of(fixture, KN_AD_1), "ld-item-title"), ==, "Reklam 1");
    const gchar *const blue[] = {KN_SWEEPER, KN_JINGLE};
    const gchar *const purple[] = {KN_ADVERT, KN_AD_1, KN_AD_2};
    const gchar *const plain[] = {KN_MUSIC_1, KN_MUSIC_2};
    for (guint i = 0; i < G_N_ELEMENTS(blue); ++i) {
        GtkWidget *item = row_of(fixture, blue[i]);
        g_assert_true(has_class(item, "programme-kind-jingle")); g_assert_false(has_class(item, "programme-kind-advertisement"));
        assert_rgb(color_of(with_class(item, "broadcast-queue-artist")), 0x5fb8ff);
    }
    for (guint i = 0; i < G_N_ELEMENTS(purple); ++i) {
        GtkWidget *item = row_of(fixture, purple[i]);
        g_assert_true(has_class(item, "programme-kind-advertisement")); g_assert_false(has_class(item, "programme-kind-jingle"));
        assert_rgb(color_of(with_class(item, "broadcast-queue-artist")), 0xb48cff);
    }
    GdkRGBA blue_eyebrow = color_of(with_class(row_of(fixture, KN_SWEEPER), "broadcast-queue-artist"));
    for (guint i = 0; i < G_N_ELEMENTS(plain); ++i) {
        GtkWidget *item = row_of(fixture, plain[i]);
        g_assert_false(has_class(item, "programme-kind-jingle")); g_assert_false(has_class(item, "programme-kind-advertisement"));
        GdkRGBA eyebrow = color_of(with_class(item, "broadcast-queue-artist"));
        g_assert_false(gdk_rgba_equal(&eyebrow, &blue_eyebrow));
    }
    /* S keeps the music row's colour on accented rows; G stays green on both
     * transition rows, the attached sweeper included. */
    GdkRGBA music_s = color_of(control(row_of(fixture, KN_MUSIC_1), "play"));
    assert_same_color(color_of(control(row_of(fixture, KN_SWEEPER), "play")), music_s);
    assert_same_color(color_of(control(row_of(fixture, KN_ADVERT), "play")), music_s);
    radios_programme_queue_set_transition(fixture->queue, KN_SONG_1, KN_SWEEP); pump();
    assert_play(row_of(fixture, KN_MUSIC_1), TRUE); assert_play(row_of(fixture, KN_SWEEPER), TRUE);
    assert_play(row_of(fixture, KN_JINGLE), FALSE); assert_play(row_of(fixture, KN_MUSIC_2), FALSE);
    assert_rgb_settled(control(row_of(fixture, KN_MUSIC_1), "play"), 0x8ee6a0);
    assert_rgb_settled(control(row_of(fixture, KN_SWEEPER), "play"), 0x8ee6a0);
}

/* (d) PDF s.13–14 (A-14, A-16): a planned jingle/sweeper of a song is its
 * 1.1 sub-row in the opened event; both views follow the same voice; the song
 * keeps S while the slogan sounds; a finished voice leaves both lists. */
static void test_kilavuz_overlay_sub_row(Fixture *fixture, gconstpointer data)
{
    (void)data;
    JsonObject *song = config_event(fixture, 0);
    set_json_member(song, "planned_jingles", "[{\"title\":\"Radyo S Kısa\",\"placement\":\"overlay\",\"kind\":\"jingle\","
        "\"duration_ms\":4000},{\"title\":\"Enerji\",\"placement\":\"after\",\"kind\":\"sweeper\"}]");
    JsonArray *hints = json_object_get_array_member(song, "planned_jingles");
    gchar *voice = radios_programme_hint_id(song, json_array_get_object_element(hints, 0), 0);
    gchar *sweeper = radios_programme_hint_id(song, json_array_get_object_element(hints, 1), 1);
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); pump();
    open_first(fixture);
    const RowExpect contents[] = {{TRACK_A, "1", K_MAIN}, {voice, "1.1", K_SUB}, {sweeper, "1.2", K_SUB},
        {TRACK_B, "2", K_MAIN}, {TRACK_C, "3", K_MAIN}};
    assert_layout(fixture, contents, G_N_ELEMENTS(contents));
    GtkWidget *overlay = row(fixture, 1), *after = row(fixture, 2);
    g_assert_cmpstr(text_of(overlay, "ld-item-title"), ==, "Radyo S Kısa");
    g_assert_cmpstr(text_of(overlay, "broadcast-queue-artist"), ==, "Jingle");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(overlay, "programme-row-meta"))), ==, "Üzerine jingle");
    g_assert_cmpstr(text_of(overlay, "broadcast-queue-duration"), ==, "0:04");
    g_assert_cmpstr(text_of(after, "ld-item-title"), ==, "Enerji");
    g_assert_cmpstr(text_of(after, "broadcast-queue-artist"), ==, "Sweeper");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(after, "programme-row-meta"))), ==, "Araya sweeper");
    g_assert_true(has_class(overlay, "programme-kind-jingle")); g_assert_true(has_class(after, "programme-kind-jingle"));
    g_assert_cmpstr(text_of(overlay, "ld-state"), ==, "○ Planlandı");
    g_assert_cmpstr(text_of(after, "ld-state"), ==, "○ Planlandı");
    /* The slogan enters over the sounding song: the song keeps its S. */
    radios_programme_queue_set_playing(fixture->queue, TRACK_A, FALSE);
    radios_programme_queue_set_overlay(fixture->queue, TRACK_A, "Radyo S Kısa", RADIOS_PLAYOUT_CONTENT_PLAYING); pump();
    g_assert_true(row(fixture, 1) == overlay);
    g_assert_cmpstr(text_of(overlay, "ld-state"), ==, "▶ Çalıyor"); g_assert_true(has_class(overlay, "is-playing"));
    g_assert_cmpstr(text_of(after, "ld-state"), ==, "○ Planlandı"); g_assert_false(has_class(after, "is-playing"));
    g_assert_cmpstr(text_of(row(fixture, 0), "ld-state"), ==, "▶ Çalıyor");
    assert_play(row(fixture, 0), FALSE); g_assert_true(has_class(control(row(fixture, 0), "play"), "is-pressed"));
    /* The event view shows the same voice as 1.1 of event 1, in the same state. */
    back(fixture);
    const RowExpect events[] = {{BLOCK_A, "1", K_MAIN}, {voice, "1.1", K_SUB}, {BLOCK_B, "2", K_MAIN}, {BLOCK_EMPTY, "3", K_MAIN}};
    assert_layout(fixture, events, G_N_ELEMENTS(events));
    g_assert_cmpstr(text_of(row(fixture, 1), "ld-state"), ==, "▶ Çalıyor");
    /* NONE after playing finishes the voice: its sub-row leaves both lists. */
    radios_programme_queue_set_overlay(fixture->queue, TRACK_A, "Radyo S Kısa", RADIOS_PLAYOUT_CONTENT_NONE); pump();
    const RowExpect events_done[] = {{BLOCK_A, "1", K_MAIN}, {BLOCK_B, "2", K_MAIN}, {BLOCK_EMPTY, "3", K_MAIN}};
    assert_layout(fixture, events_done, G_N_ELEMENTS(events_done));
    open_first(fixture);
    const RowExpect contents_done[] = {{TRACK_A, "1", K_MAIN}, {sweeper, "1.1", K_SUB}, {TRACK_B, "2", K_MAIN}, {TRACK_C, "3", K_MAIN}};
    assert_layout(fixture, contents_done, G_N_ELEMENTS(contents_done));
    g_assert_cmpstr(text_of(row(fixture, 1), "ld-state"), ==, "○ Planlandı");
    /* The playout's identity of a voice addresses the same sub-row. */
    radios_programme_queue_set_overlay_identity(fixture->queue, TRACK_A, sweeper, "Enerji", RADIOS_PLAYOUT_CONTENT_PLAYING); pump();
    g_assert_cmpstr(text_of(row(fixture, 1), "ld-state"), ==, "▶ Çalıyor"); g_assert_true(has_class(row(fixture, 1), "is-playing"));
    radios_programme_queue_set_overlay_identity(fixture->queue, TRACK_A, sweeper, NULL, RADIOS_PLAYOUT_CONTENT_NONE); pump();
    const RowExpect plain[] = {{TRACK_A, "1", K_MAIN}, {TRACK_B, "2", K_MAIN}, {TRACK_C, "3", K_MAIN}};
    assert_layout(fixture, plain, G_N_ELEMENTS(plain));
    g_free(voice); g_free(sweeper);
}

/* (e) PDF s.10–11, s.13 (A-7, A-10, A-12, A-16): the event show count limits
 * main events from the current position; attached sub-rows take no slot;
 * finished rows leave the active list and the next event fills in; the
 * programme itself is unchanged. */
static void test_kilavuz_display_window(Fixture *fixture, gconstpointer data)
{
    (void)data;
    config_use(fixture, kilavuz_window);
    const RowExpect all[] = {{KW_EVENT_1, "1", K_MAIN}, {KW_EVENT_2, "2", K_MAIN}, {KW_SWEEPER, "2.1", K_SUB},
        {KW_EVENT_3, "3", K_MAIN}, {KW_EVENT_4, "4", K_MAIN}};
    assert_layout(fixture, all, G_N_ELEMENTS(all));
    radios_programme_queue_set_window(fixture->queue, 2, 0); pump();
    const RowExpect first[] = {{KW_EVENT_1, "1", K_MAIN}, {KW_EVENT_2, "2", K_MAIN}, {KW_SWEEPER, "2.1", K_SUB}};
    assert_layout(fixture, first, G_N_ELEMENTS(first));
    radios_programme_queue_complete(fixture->queue, KW_SONG_1); pump();
    const RowExpect second[] = {{KW_EVENT_2, "1", K_MAIN}, {KW_SWEEPER, "1.1", K_SUB}, {KW_EVENT_3, "2", K_MAIN}};
    assert_layout(fixture, second, G_N_ELEMENTS(second));
    radios_programme_queue_complete(fixture->queue, KW_SWEEP); pump();
    const RowExpect third[] = {{KW_EVENT_2, "1", K_MAIN}, {KW_EVENT_3, "2", K_MAIN}};
    assert_layout(fixture, third, G_N_ELEMENTS(third));
    radios_programme_queue_set_window(fixture->queue, 0, 0); pump();
    assert_layout(fixture, all, G_N_ELEMENTS(all));
    g_assert_true(has_class(row_of(fixture, KW_EVENT_1), "is-event-completed"));
    g_assert_false(has_class(row_of(fixture, KW_EVENT_2), "is-event-completed"));
}

/* (f) PDF s.11 (A-8, A-10): S turns into the green G on the transition's
 * source and target rows; a limited view keeps both rows; on completion the
 * old row leaves and the target returns to S. */
static void test_kilavuz_transition(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_programme_queue_set_transition(fixture->queue, TRACK_C, TRACK_D); pump();
    assert_play(row(fixture, 0), TRUE); assert_play(row(fixture, 1), TRUE); assert_play(row(fixture, 2), FALSE);
    open_first(fixture);
    assert_play(row(fixture, 0), FALSE); assert_play(row(fixture, 1), FALSE); assert_play(row(fixture, 2), TRUE);
    back(fixture);
    radios_programme_queue_set_transition(fixture->queue, NULL, NULL); pump();
    for (guint index = 0; index < 3; ++index) assert_play(row(fixture, index), FALSE);
    radios_programme_queue_set_window(fixture->queue, 1, 0); pump();
    const RowExpect limited[] = {{BLOCK_A, "1", K_MAIN}};
    assert_layout(fixture, limited, G_N_ELEMENTS(limited));
    radios_programme_queue_set_progress(fixture->queue, TRACK_C, TRACK_D, 140000, 150000, FALSE);
    radios_programme_queue_set_transition(fixture->queue, TRACK_C, TRACK_D); pump();
    const RowExpect both[] = {{BLOCK_A, "1", K_MAIN}, {BLOCK_B, "2", K_MAIN}};
    assert_layout(fixture, both, G_N_ELEMENTS(both));
    assert_play(row(fixture, 0), TRUE); assert_play(row(fixture, 1), TRUE);
    radios_programme_queue_complete(fixture->queue, TRACK_C);
    radios_programme_queue_set_progress(fixture->queue, TRACK_D, NULL, 0, 90000, FALSE); pump();
    const RowExpect target[] = {{BLOCK_B, "1", K_MAIN}};
    assert_layout(fixture, target, G_N_ELEMENTS(target));
    assert_play(row(fixture, 0), FALSE); g_assert_true(has_class(row(fixture, 0), "is-playing"));
}

/* (g) PDF s.11 (A-9): only the ending event's own area blinks; FALSE or the
 * event's completion clears the warning and stops it. */
static void test_kilavuz_ending_blink(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GtkWidget *navigation = with_class(fixture->queue, "programme-queue-navigation");
    radios_programme_queue_set_ending(fixture->queue, BLOCK_A, TRUE);
    Blink blink = blink_watch(fixture, row(fixture, 0), navigation, 1200);
    g_assert_true(blink.on); g_assert_true(blink.off); g_assert_false(blink.elsewhere);
    /* In the opened event its title bar blinks; its contents stay steady. */
    open_first(fixture);
    Blink opened = blink_watch(fixture, navigation, NULL, 1100);
    g_assert_true(opened.on); g_assert_true(opened.off); g_assert_false(opened.elsewhere);
    back(fixture);
    radios_programme_queue_set_ending(fixture->queue, BLOCK_A, FALSE);
    Blink cleared = blink_watch(fixture, row(fixture, 0), navigation, 650);
    g_assert_false(cleared.on); g_assert_false(cleared.elsewhere);
    radios_programme_queue_set_ending(fixture->queue, BLOCK_A, TRUE);
    g_assert_true(has_class(row(fixture, 0), "is-ending"));
    radios_programme_queue_complete(fixture->queue, TRACK_C);
    Blink completed = blink_watch(fixture, row(fixture, 0), navigation, 600);
    g_assert_false(completed.on); g_assert_false(completed.elsewhere);
    g_assert_true(has_class(row(fixture, 0), "is-event-completed"));
}

/* (h) PDF s.8, s.15 (A-2, A-5): a live step is an event row "Event · Canlı"
 * driven by L; S and V are passive; it never shows "Boş event". */
static void test_kilavuz_live_row(Fixture *fixture, gconstpointer data)
{
    (void)data;
    config_use(fixture, kilavuz_live);
    const RowExpect events[] = {{KL_MUSIC_1, "1", K_MAIN}, {KL_LIVE, "2", K_MAIN}, {KL_MUSIC_2, "3", K_MAIN}};
    assert_layout(fixture, events, G_N_ELEMENTS(events));
    GtkWidget *live = row_of(fixture, KL_LIVE);
    assert_controls(live);
    g_assert_cmpstr(text_of(live, "broadcast-queue-artist"), ==, "Event · Canlı");
    g_assert_cmpstr(text_of(live, "ld-item-title"), ==, "Konuşma");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(live, "programme-row-meta"))), ==, "Canlı · Konuşma");
    g_assert_cmpstr(text_of(live, "broadcast-queue-duration"), ==, "—:—");
    g_assert_cmpstr(text_of(live, "ld-state"), ==, "✓ Hazır");
    g_assert_false(gtk_widget_get_sensitive(control(live, "play")));
    g_assert_false(gtk_widget_get_sensitive(control(live, "waveform")));
    g_assert_false(gtk_widget_get_sensitive(control(live, "talk")));
    g_assert_true(gtk_widget_get_sensitive(control(row_of(fixture, KL_MUSIC_1), "play")));
    g_assert_null(with_text(GTK_WIDGET(fixture->list), "Boş event"));
    gtk_button_clicked(GTK_BUTTON(control(live, "live"))); pump();
    g_assert_cmpuint(fixture->actions->len, ==, 1);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_LIVE, KL_LIVE), ==, 1);
    radios_programme_queue_set_talk_available(fixture->queue, KL_LIVE);
    radios_programme_queue_set_live(fixture->queue, KL_LIVE, TRUE, FALSE); pump();
    g_assert_cmpstr(text_of(live, "ld-state"), ==, "● Yayında");
    g_assert_true(has_class(control(live, "live"), "is-pressed"));
    g_assert_false(has_class(control(live, "talk"), "is-pressed"));
    g_assert_true(gtk_widget_get_sensitive(control(live, "talk")));
    g_assert_false(gtk_widget_get_sensitive(control(row_of(fixture, KL_MUSIC_2), "talk")));
    radios_programme_queue_set_live(fixture->queue, KL_LIVE, TRUE, TRUE); pump();
    g_assert_true(has_class(control(live, "talk"), "is-pressed"));
    radios_programme_queue_set_live(fixture->queue, KL_LIVE, FALSE, FALSE); pump();
    g_assert_cmpstr(text_of(live, "ld-state"), ==, "✓ Hazır");
    g_assert_false(has_class(control(live, "live"), "is-pressed"));
    g_assert_false(has_class(control(live, "talk"), "is-pressed"));
    /* Opened, the live step is its own row instead of an empty-event note. */
    radios_programme_queue_set_expanded(fixture->queue, KL_LIVE, TRUE); pump();
    g_assert_true(gtk_widget_is_visible(with_class(fixture->queue, "programme-queue-navigation")));
    const RowExpect step[] = {{KL_LIVE, "1", K_MAIN}};
    assert_layout(fixture, step, G_N_ELEMENTS(step));
    live = row(fixture, 0);
    g_assert_cmpstr(text_of(live, "broadcast-queue-artist"), ==, "Event · Canlı");
    g_assert_false(gtk_widget_get_sensitive(control(live, "play")));
    g_assert_false(gtk_widget_get_sensitive(control(live, "waveform")));
    g_assert_true(gtk_widget_get_sensitive(control(live, "live")));
    g_assert_null(with_text(GTK_WIDGET(fixture->list), "Boş event"));
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_PLAY, NULL), ==, 0);
}

/* (i) PDF s.15 (A-3): while R is held a list change waits; the held row is
 * not rebuilt and the talk is not cut. The release ends it, then the finished
 * content leaves the limited list. */
static void test_kilavuz_talk_defers_rebuild(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_programme_queue_set_window(fixture->queue, 0, 1);
    open_first(fixture);
    const RowExpect first[] = {{TRACK_A, "1", K_MAIN}};
    assert_layout(fixture, first, G_N_ELEMENTS(first));
    radios_programme_queue_set_progress(fixture->queue, TRACK_A, TRACK_B, 110000, 120000, FALSE);
    radios_programme_queue_set_talk_available(fixture->queue, BLOCK_A); pump();
    GtkWidget *held = g_object_ref(row(fixture, 0)), *talk = g_object_ref(control(held, "talk"));
    g_assert_true(gtk_widget_get_sensitive(talk));
    pointer_button(talk, GDK_BUTTON_PRESS, 1); pump();
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_TALK_PRESS, BLOCK_A), ==, 1);
    g_assert_true(has_class(talk, "is-held"));
    /* The content finishes, the next one starts and the programme refreshes. */
    radios_programme_queue_complete(fixture->queue, TRACK_A);
    radios_programme_queue_set_progress(fixture->queue, TRACK_B, TRACK_C, 0, 180000, FALSE);
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); pump();
    g_assert_true(row(fixture, 0) == held); g_assert_cmpuint(rows_count(fixture), ==, 1);
    g_assert_true(gtk_widget_get_parent(held) == GTK_WIDGET(fixture->list));
    g_assert_true(has_class(talk, "is-held"));
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_TALK_RELEASE, NULL), ==, 0);
    g_assert_cmpstr(text_of(held, "ld-state"), ==, "✓ Tamamlandı");
    pointer_button(talk, GDK_BUTTON_RELEASE, 1);
    g_assert_cmpuint(actions_of(fixture, RADIOS_PROGRAMME_ROW_TALK_RELEASE, BLOCK_A), ==, 1);
    g_assert_false(has_class(talk, "is-held"));
    pump();
    g_assert_null(gtk_widget_get_parent(held));
    const RowExpect next[] = {{TRACK_B, "1", K_MAIN}};
    assert_layout(fixture, next, G_N_ELEMENTS(next));
    g_assert_true(has_class(row(fixture, 0), "is-playing"));
    g_assert_true(gtk_widget_get_sensitive(control(row(fixture, 0), "talk")));
    g_assert_cmpuint(fixture->actions->len, ==, 2);
    g_object_unref(talk); g_object_unref(held);
}

/* ---- Kılavuz 18.10 görsel denetim düzeltmeleri (F1-F3) ve canlı adım -------- */
/* A widget drawn by GTK itself into an image (gtk_widget_draw). */
typedef struct { cairo_surface_t *surface; gint width, height; } Pixels;
static Pixels pixels_of(GtkWidget *widget)
{
    Pixels p = {NULL, gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget)};
    g_assert_cmpint(p.width, >, 0); g_assert_cmpint(p.height, >, 0);
    p.surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, p.width, p.height);
    cairo_t *cr = cairo_create(p.surface); gtk_widget_draw(widget, cr); cairo_destroy(cr);
    cairo_surface_flush(p.surface); return p;
}
static guint32 pixel_at(const Pixels *p, gint x, gint y)
{
    g_assert_cmpint(x, >=, 0); g_assert_cmpint(x, <, p->width); g_assert_cmpint(y, >=, 0); g_assert_cmpint(y, <, p->height);
    const guchar *data = cairo_image_surface_get_data(p->surface) + y * cairo_image_surface_get_stride(p->surface) + x * 4;
    guint32 value; memcpy(&value, data, sizeof value); return value & 0xffffff;
}
/* Where a row's branch runs, in row coordinates: the trunk column (the middle
 * of the indent left of a sub-row's number; on a main row the same distance
 * from the start of its number) and the number's extent. */
typedef struct { gint trunk, number_x, number_top, number_bottom; } Branch;
static Branch branch_of(GtkWidget *item, gint indent)
{
    GtkWidget *area = find(item, "programme-row-branch"), *number = with_class(item, "programme-block-number");
    if (!number) number = with_class(item, "programme-leaf-number");
    g_assert_true(GTK_IS_BUTTON(number));
    gint x = 0, y = 0, ax = 0, ay = 0;
    g_assert_true(gtk_widget_translate_coordinates(number, item, 0, 0, &x, &y));
    g_assert_true(gtk_widget_translate_coordinates(area ? area : number, item, 0, 0, &ax, &ay));
    Branch branch = {ax + indent / 2, x, y, y + gtk_widget_get_allocated_height(number)};
    return branch;
}

/* (j) PDF s.13 (A-14): a sub-row's branch runs its row's full height (├ down
 * to the bottom edge, └ down to the elbow into its number) and a main row
 * with sub-rows continues it below its number, all in one column, so the
 * line from the main row to its last sub-row is unbroken. It takes the kind
 * accent: blue jingle/sweeper, purple advertisement. */
static void test_kilavuz_branch_line(Fixture *fixture, gconstpointer data)
{
    (void)data;
    config_use(fixture, kilavuz_numbering);
    enum { B_NONE, B_MAIN, B_MIDDLE, B_LAST };
    const guint32 blue = 0x2f6f9f, purple = 0x6d4fa8;
    const struct { const gchar *id; gint kind; guint32 rgb; } rows[] = {{KN_MUSIC_1, B_MAIN, blue}, {KN_SWEEPER, B_MIDDLE, blue},
        {KN_JINGLE, B_LAST, blue}, {KN_MUSIC_2, B_NONE, 0}, {KN_ADVERT, B_MAIN, purple}, {KN_AD_1, B_MIDDLE, purple}, {KN_AD_2, B_LAST, purple}};
    gint indent = gtk_widget_get_allocated_width(find(row_of(fixture, KN_SWEEPER), "programme-row-branch")), trunk = -1;
    g_assert_cmpint(indent, >=, 10);
    for (guint i = 0; i < G_N_ELEMENTS(rows); ++i) {
        GtkWidget *item = row_of(fixture, rows[i].id); Branch branch = branch_of(item, indent);
        if (trunk < 0) trunk = branch.trunk;
        g_assert_cmpint(branch.trunk, ==, trunk);
        /* A sub-row's trunk runs left of its indented number; a main row's
         * stub leaves from under its own number. */
        if (rows[i].kind == B_MIDDLE || rows[i].kind == B_LAST) g_assert_cmpint(branch.trunk, <, branch.number_x);
        else g_assert_cmpint(branch.trunk, >, branch.number_x);
        Pixels p = pixels_of(item); gint elbow = (branch.number_top + branch.number_bottom) / 2;
        for (gint y = 0; y < p.height; ++y) {
            /* A main row's own number box crosses the column. */
            if ((rows[i].kind == B_MAIN || rows[i].kind == B_NONE) && y >= branch.number_top && y < branch.number_bottom) continue;
            gboolean line = rows[i].kind == B_MIDDLE || (rows[i].kind == B_LAST && y <= elbow) ||
                (rows[i].kind == B_MAIN && y >= branch.number_bottom);
            guint32 color = pixel_at(&p, trunk, y);
            if (line) g_assert_cmphex(color, ==, rows[i].rgb);
            else { g_assert_cmphex(color, !=, blue); g_assert_cmphex(color, !=, purple); }
        }
        if (rows[i].kind == B_MIDDLE || rows[i].kind == B_LAST)
            for (gint x = trunk; x < branch.number_x; ++x) g_assert_cmphex(pixel_at(&p, x, elbow), ==, rows[i].rgb);
        cairo_surface_destroy(p.surface);
    }
    /* The rows touch, so the drawn column has no gap between them. */
    for (guint i = 0; i + 1 < G_N_ELEMENTS(rows); ++i) {
        GtkAllocation upper, lower;
        gtk_widget_get_allocation(row_of(fixture, rows[i].id), &upper); gtk_widget_get_allocation(row_of(fixture, rows[i + 1].id), &lower);
        g_assert_cmpint(upper.y + upper.height, ==, lower.y);
    }
}

/* (k) PDF s.13 (A-15): the kind accent stays on the next row in its black
 * and white phases, on a selected and on a playing row; S/G keep their own
 * colours there. */
static void test_kilavuz_kind_accent_states(Fixture *fixture, gconstpointer data)
{
    (void)data;
    config_use(fixture, kilavuz_numbering);
    radios_programme_queue_set_progress(fixture->queue, KN_AD_1, KN_AD_2, 1000, 30000, FALSE); pump();
    GtkWidget *next = row_of(fixture, KN_AD_2), *playing = row_of(fixture, KN_AD_1);
    GtkWidget *eyebrow = with_class(next, "broadcast-queue-artist");
    g_assert_true(has_class(next, "is-upcoming-dark"));
    assert_rgb(color_of(eyebrow), 0xb48cff);
    assert_rgb(color_of(with_class(next, "ld-item-title")), 0xffffff);
    assert_rgb_settled(control(next, "play"), 0xededed);
    g_assert_true(has_class(playing, "is-playing"));
    assert_rgb(color_of(with_class(playing, "broadcast-queue-artist")), 0xb48cff);
    radios_programme_queue_set_progress(fixture->queue, KN_AD_1, KN_AD_2, 29000, 30000, FALSE); pump();
    g_assert_true(has_class(next, "is-upcoming-light"));
    assert_rgb(color_of(eyebrow), 0x4c2a8f);
    assert_rgb(color_of(with_class(next, "ld-item-title")), 0x111111);
    gtk_list_box_select_row(fixture->list, GTK_LIST_BOX_ROW(next)); pump();
    g_assert_true(has_class(next, "is-selected"));
    assert_rgb(color_of(eyebrow), 0x4c2a8f);
    radios_programme_queue_set_transition(fixture->queue, KN_AD_1, KN_AD_2); pump();
    assert_play(next, TRUE); assert_rgb_settled(control(next, "play"), 0x8ee6a0);
    assert_rgb(color_of(eyebrow), 0x4c2a8f);
    radios_programme_queue_set_transition(fixture->queue, NULL, NULL);
    /* Blue: a sweeper content next in its opened event, then sounding. */
    radios_programme_queue_set_expanded(fixture->queue, KN_SWEEPER, TRUE); pump();
    radios_programme_queue_set_progress(fixture->queue, KN_SONG_1, KN_SWEEP, 1000, 180000, FALSE); pump();
    GtkWidget *sweep = row_of(fixture, KN_SWEEP); eyebrow = with_class(sweep, "broadcast-queue-artist");
    g_assert_true(has_class(sweep, "is-upcoming-dark"));
    assert_rgb(color_of(eyebrow), 0x5fb8ff);
    radios_programme_queue_set_progress(fixture->queue, KN_SONG_1, KN_SWEEP, 179000, 180000, FALSE); pump();
    g_assert_true(has_class(sweep, "is-upcoming-light"));
    assert_rgb(color_of(eyebrow), 0x164b80);
    radios_programme_queue_set_progress(fixture->queue, KN_SWEEP, NULL, 1000, 10000, FALSE); pump();
    g_assert_true(has_class(sweep, "is-playing")); g_assert_false(has_class(sweep, "is-upcoming"));
    assert_rgb(color_of(eyebrow), 0x5fb8ff);
}

/* The GdkWindow that receives a widget's pointer events: its own input window
 * (an event box or a button) or the window it is drawn in. */
static GdkWindow *pointer_window(GtkWidget *widget)
{
    GdkWindow *window = gtk_widget_get_window(widget), *result = window;
    GList *children = gdk_window_get_children(window);
    for (GList *item = children; item; item = item->next) {
        gpointer owner = NULL; gdk_window_get_user_data(item->data, &owner);
        if (owner == widget) { result = item->data; break; }
    }
    g_list_free(children); return result;
}
/* A left-button event GTK dispatches itself (gtk_main_do_event): the row's
 * handle, the list box's own gesture (selection, activation, focus) and the
 * queue see it in their real order. x and y are in widget coordinates. */
static void dispatch_button(GtkWidget *widget, GdkEventType type, gdouble x, gdouble y)
{
    GdkWindow *window = pointer_window(widget);
    GdkEvent *event = gdk_event_new(type);
    event->any.window = g_object_ref(window); event->any.send_event = TRUE;
    GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(widget)));
    gdk_event_set_device(event, pointer); gdk_event_set_source_device(event, pointer);
    gint ox = 0, oy = 0; gdk_window_get_origin(window, &ox, &oy);
    event->button.button = 1; event->button.time = GDK_CURRENT_TIME;
    event->button.x = x; event->button.y = y; event->button.x_root = ox + x; event->button.y_root = oy + y;
    if (type == GDK_BUTTON_RELEASE) event->button.state = GDK_BUTTON1_MASK;
    gtk_main_do_event(event); gdk_event_free(event);
}
static void real_click(GtkWidget *widget, gdouble x, gdouble y)
{
    dispatch_button(widget, GDK_BUTTON_PRESS, x, y); dispatch_button(widget, GDK_BUTTON_RELEASE, x, y);
    run_for(150); /* past the test's double-click time: the next click is a new one */
}
static void click_row(GtkWidget *item)
{
    GtkWidget *handle = gtk_bin_get_child(GTK_BIN(item));
    real_click(handle, gtk_widget_get_allocated_width(handle) * 0.4, gtk_widget_get_allocated_height(handle) / 2.0);
}

/* (l) PDF s.8 (A-4): the counter shows the selected item's duration. Esc in
 * the queue, a click on the list's empty area and a second click on the
 * selected content row each clear the selection and the hook receives NULL,
 * so the page shows the sounding content's remaining time again. A click on
 * an event still opens it; a double click never clears. */
static void test_kilavuz_deselect(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GtkSettings *settings = gtk_settings_get_default(); gint double_click = 0;
    g_object_get(settings, "gtk-double-click-time", &double_click, NULL);
    g_object_set(settings, "gtk-double-click-time", 60, NULL);
    const gchar *tracks[] = {TRACK_A, TRACK_B, TRACK_C};
    /* A click on an event opens it; the event stays selected and the
     * keyboard focus follows the rebuilt rows. */
    click_row(row(fixture, 0)); pump();
    assert_rows(fixture, tracks, G_N_ELEMENTS(tracks));
    g_assert_cmpstr(fixture->selected, ==, BLOCK_A); g_assert_cmpuint(fixture->cleared, ==, 0);
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(fixture->window));
    g_assert_nonnull(focus); g_assert_true(gtk_widget_is_ancestor(focus, fixture->queue));
    /* 1. Esc; with nothing selected Esc passes on. */
    key_event(fixture->window, GDK_KEY_PRESS, GDK_KEY_Escape); pump();
    g_assert_null(fixture->selected); g_assert_cmpuint(fixture->cleared, ==, 1);
    assert_rows(fixture, tracks, G_N_ELEMENTS(tracks));
    key_event(fixture->window, GDK_KEY_PRESS, GDK_KEY_Escape); pump();
    g_assert_cmpuint(fixture->cleared, ==, 1);
    /* 2. A second click on the selected content row; the list box selecting
     * the same row on that release does not bring the selection back. */
    GtkWidget *second = row(fixture, 1);
    click_row(second);
    g_assert_cmpstr(fixture->selected, ==, TRACK_B); g_assert_true(has_class(second, "is-selected"));
    g_assert_true(gtk_list_box_get_selected_row(fixture->list) == GTK_LIST_BOX_ROW(second));
    click_row(second); pump();
    g_assert_null(fixture->selected); g_assert_cmpuint(fixture->cleared, ==, 2);
    g_assert_null(gtk_list_box_get_selected_row(fixture->list));
    g_assert_null(with_class(GTK_WIDGET(fixture->list), "is-selected"));
    /* A double click selects and keeps the selection. */
    GtkWidget *third = row(fixture, 2), *handle = gtk_bin_get_child(GTK_BIN(third));
    gdouble x = gtk_widget_get_allocated_width(handle) * 0.4, y = gtk_widget_get_allocated_height(handle) / 2.0;
    dispatch_button(handle, GDK_BUTTON_PRESS, x, y); dispatch_button(handle, GDK_BUTTON_RELEASE, x, y);
    dispatch_button(handle, GDK_BUTTON_PRESS, x, y); dispatch_button(handle, GDK_2BUTTON_PRESS, x, y);
    dispatch_button(handle, GDK_BUTTON_RELEASE, x, y); run_for(150);
    g_assert_cmpstr(fixture->selected, ==, TRACK_C); g_assert_cmpuint(fixture->cleared, ==, 2);
    g_assert_true(gtk_list_box_get_selected_row(fixture->list) == GTK_LIST_BOX_ROW(third));
    /* 3. A click on the list's empty area below the rows. */
    GtkAllocation last; gtk_widget_get_allocation(third, &last);
    g_assert_cmpint(gtk_widget_get_allocated_height(GTK_WIDGET(fixture->list)), >, last.y + last.height + 20);
    real_click(GTK_WIDGET(fixture->list), 200, last.y + last.height + 10);
    g_assert_null(fixture->selected); g_assert_cmpuint(fixture->cleared, ==, 3);
    g_assert_null(gtk_list_box_get_selected_row(fixture->list)); g_assert_false(has_class(third, "is-selected"));
    real_click(GTK_WIDGET(fixture->list), 200, last.y + last.height + 10);
    g_assert_cmpuint(fixture->cleared, ==, 3);
    /* The selected row's number counts as the row; the list box's own
     * unselect (Ctrl+Space) clears the selection the same way. */
    click_row(row(fixture, 0)); g_assert_cmpstr(fixture->selected, ==, TRACK_A);
    gtk_button_clicked(GTK_BUTTON(with_class(row(fixture, 0), "programme-leaf-number"))); pump();
    g_assert_null(fixture->selected); g_assert_cmpuint(fixture->cleared, ==, 4);
    gtk_button_clicked(GTK_BUTTON(with_class(row(fixture, 1), "programme-leaf-number"))); pump();
    g_assert_cmpstr(fixture->selected, ==, TRACK_B);
    gtk_list_box_unselect_all(fixture->list); pump();
    g_assert_null(fixture->selected); g_assert_cmpuint(fixture->cleared, ==, 5);
    /* Back in the event view the opened event is selected again; the focus
     * that was on the back button moves to its row, so Esc clears it. */
    GtkWidget *back = find(fixture->queue, "programme-queue-back");
    gtk_widget_grab_focus(back); gtk_button_clicked(GTK_BUTTON(back)); pump();
    g_assert_cmpstr(fixture->selected, ==, BLOCK_A); g_assert_true(has_class(row(fixture, 0), "is-selected"));
    g_assert_true(gtk_window_get_focus(GTK_WINDOW(fixture->window)) == row(fixture, 0));
    key_event(fixture->window, GDK_KEY_PRESS, GDK_KEY_Escape); pump();
    g_assert_null(fixture->selected); g_assert_cmpuint(fixture->cleared, ==, 6);
    g_assert_false(has_class(row(fixture, 0), "is-selected")); g_assert_null(gtk_list_box_get_selected_row(fixture->list));
    g_object_set(settings, "gtk-double-click-time", double_click, NULL);
}

/* (m) PDF s.8, s.11 (A-5, A-19): the live step the flow waits at reads as the
 * sounding row (S lit, "● Yayında"), with or without L; when the flow
 * finishes it the row is a finished event that a limited view drops; a live
 * row names no player. */
static void test_kilavuz_live_step(Fixture *fixture, gconstpointer data)
{
    (void)data;
    config_use(fixture, kilavuz_live);
    const gchar *names[32] = {NULL}; names[2] = "Stüdyo";
    radios_programme_queue_set_player_context(fixture->queue, 3, names); pump();
    GtkWidget *live = row_of(fixture, KL_LIVE);
    g_assert_cmpstr(player_text(live), ==, "");
    g_assert_cmpstr(player_text(row_of(fixture, KL_MUSIC_1)), ==, "Stüdyo");
    g_assert_false(has_class(live, "is-playing")); g_assert_cmpstr(text_of(live, "ld-state"), ==, "✓ Hazır");
    radios_programme_queue_set_live_step(fixture->queue, KL_LIVE); pump();
    for (guint pass = 0; pass < 2; ++pass) {
        g_assert_true(row_of(fixture, KL_LIVE) == live);
        g_assert_true(has_class(live, "is-playing")); g_assert_true(has_class(control(live, "play"), "is-pressed"));
        g_assert_cmpstr(text_of(live, "ld-state"), ==, "● Yayında");
        assert_rgb_settled(control(live, "play"), 0xffce69);
        g_assert_false(has_class(row_of(fixture, KL_MUSIC_1), "is-playing"));
        g_assert_false(has_class(row_of(fixture, KL_MUSIC_2), "is-playing"));
        g_assert_cmpstr(player_text(live), ==, "");
        /* L latched on top keeps the same reading. */
        radios_programme_queue_set_live(fixture->queue, KL_LIVE, TRUE, FALSE); pump();
    }
    radios_programme_queue_set_live_step(fixture->queue, NULL); pump();
    g_assert_false(has_class(live, "is-playing")); g_assert_cmpstr(text_of(live, "ld-state"), ==, "● Yayında");
    radios_programme_queue_set_live(fixture->queue, KL_LIVE, FALSE, FALSE); pump();
    g_assert_cmpstr(text_of(live, "ld-state"), ==, "✓ Hazır");
    /* A limited view: the finished step leaves it and the next event fills in. */
    radios_programme_queue_set_window(fixture->queue, 2, 0); pump();
    const RowExpect start[] = {{KL_MUSIC_1, "1", K_MAIN}, {KL_LIVE, "2", K_MAIN}};
    assert_layout(fixture, start, G_N_ELEMENTS(start));
    radios_programme_queue_complete(fixture->queue, KL_SONG_1); pump();
    const RowExpect waiting[] = {{KL_LIVE, "1", K_MAIN}, {KL_MUSIC_2, "2", K_MAIN}};
    assert_layout(fixture, waiting, G_N_ELEMENTS(waiting));
    radios_programme_queue_set_live_step(fixture->queue, KL_LIVE); pump();
    assert_layout(fixture, waiting, G_N_ELEMENTS(waiting));
    g_assert_true(has_class(row(fixture, 0), "is-playing"));
    radios_programme_queue_complete_live_step(fixture->queue, KL_LIVE); pump();
    const RowExpect after[] = {{KL_MUSIC_2, "1", K_MAIN}};
    assert_layout(fixture, after, G_N_ELEMENTS(after));
    g_assert_null(with_class(GTK_WIDGET(fixture->list), "is-playing"));
    radios_programme_queue_set_window(fixture->queue, 0, 0); pump();
    live = row_of(fixture, KL_LIVE);
    g_assert_true(has_class(live, "is-event-completed")); g_assert_false(has_class(live, "is-playing"));
    g_assert_cmpstr(text_of(live, "ld-state"), ==, "✓ Tamamlandı");
    /* Reaching the step again revives it; reset clears finished steps; a
     * content id or an unknown id is not a live step. */
    radios_programme_queue_set_live_step(fixture->queue, KL_LIVE); pump();
    g_assert_false(has_class(live, "is-event-completed")); g_assert_cmpstr(text_of(live, "ld-state"), ==, "● Yayında");
    radios_programme_queue_complete_live_step(fixture->queue, KL_LIVE); radios_programme_queue_complete_live_step(fixture->queue, KL_LIVE); pump();
    g_assert_true(has_class(live, "is-event-completed"));
    radios_programme_queue_reset_progress(fixture->queue); pump();
    live = row_of(fixture, KL_LIVE);
    g_assert_false(has_class(live, "is-event-completed")); g_assert_cmpstr(text_of(live, "ld-state"), ==, "✓ Hazır");
    radios_programme_queue_complete_live_step(fixture->queue, KL_SONG_2);
    radios_programme_queue_complete_live_step(fixture->queue, "unknown-event"); pump();
    g_assert_null(with_class(GTK_WIDGET(fixture->list), "is-event-completed"));
}

static void count_notify(GObject *object, GParamSpec *pspec, gpointer data)
{ (void)object; (void)pspec; ++*(guint *)data; }
/* (n) PDF s.4 step 5 (A-2): a live row's subtitle follows the run-time label
 * of its channels and returns to the event's own label with NULL; the text
 * is set only when it changes and survives a rebuild. */
static void test_kilavuz_live_label(Fixture *fixture, gconstpointer data)
{
    (void)data;
    config_use(fixture, kilavuz_live);
    GtkWidget *live = row_of(fixture, KL_LIVE), *meta = find(live, "programme-row-meta");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(meta)), ==, "Canlı · Konuşma");
    guint changes = 0; gulong watch = g_signal_connect(meta, "notify::label", G_CALLBACK(count_notify), &changes);
    radios_programme_queue_set_live_label(fixture->queue, KL_LIVE, "dj"); pump();
    g_assert_true(row_of(fixture, KL_LIVE) == live);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(meta)), ==, "Canlı · DJ"); g_assert_cmpuint(changes, ==, 1);
    g_assert_cmpstr(gtk_widget_get_tooltip_text(meta), ==, "Canlı · DJ");
    radios_programme_queue_set_live_label(fixture->queue, KL_LIVE, "dj"); pump();
    radios_programme_queue_set_live_label(fixture->queue, KL_MUSIC_1, "music"); pump();
    g_assert_cmpuint(changes, ==, 1);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(row_of(fixture, KL_MUSIC_1), "programme-row-meta"))), ==, "Müzik · 1 parça / içerik");
    radios_programme_queue_set_live_label(fixture->queue, KL_LIVE, "music"); pump();
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(meta)), ==, "Canlı · Müzik"); g_assert_cmpuint(changes, ==, 2);
    g_signal_handler_disconnect(meta, watch);
    /* A rebuilt list and the opened step keep the run-time label. */
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); pump();
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(row_of(fixture, KL_LIVE), "programme-row-meta"))), ==, "Canlı · Müzik");
    radios_programme_queue_set_expanded(fixture->queue, KL_LIVE, TRUE); pump();
    meta = find(row(fixture, 0), "programme-row-meta");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(meta)), ==, "Canlı · Müzik");
    radios_programme_queue_set_live_label(fixture->queue, KL_LIVE, NULL); pump();
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(meta)), ==, "Canlı · Konuşma");
    radios_programme_queue_set_live_label(fixture->queue, KL_LIVE, "talk"); radios_programme_queue_set_live_label(fixture->queue, KL_LIVE, ""); pump();
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(meta)), ==, "Canlı · Konuşma");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check(&argc, &argv)) return 77;
    GtkCssProvider *provider = gtk_css_provider_new(); GError *error = NULL;
    gchar *tests = g_path_get_dirname(__FILE__);
    gchar *css = g_build_filename(tests, "..", "assets", "themes", "radios.css", NULL);
    g_assert_true(gtk_css_provider_load_from_path(provider, css, &error)); g_assert_no_error(error);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider); g_free(css); g_free(tests);
    g_test_add("/programme-queue/navigation", Fixture, NULL, setup, test_flat_navigation, teardown);
    g_test_add("/programme-queue/track-reorder", Fixture, NULL, setup, test_track_reorder, teardown);
    g_test_add("/programme-queue/event-reorder", Fixture, NULL, setup, test_event_reorder, teardown);
    g_test_add("/programme-queue/drop-targets", Fixture, NULL, setup, test_drop_targets, teardown);
    g_test_add("/programme-queue/removed-event", Fixture, NULL, setup, test_removed_event, teardown);
    g_test_add("/programme-queue/click-after-drop", Fixture, NULL, setup, test_click_after_drop, teardown);
    g_test_add("/programme-queue/pending-navigation-destroy", Fixture, NULL, setup, test_pending_navigation_destroy, teardown);
    g_test_add("/programme-queue/upcoming-progress", Fixture, NULL, setup, test_upcoming_progress, teardown);
    g_test_add("/programme-queue/upcoming-pause-unknown-reset", Fixture, NULL, setup, test_upcoming_pause_unknown_reset, teardown);
    g_test_add("/programme-queue/upcoming-navigation-reorder", Fixture, NULL, setup, test_upcoming_navigation_reorder, teardown);
    g_test_add("/programme-queue/event-completion-stable", Fixture, NULL, setup, test_event_completion_stable, teardown);
    g_test_add("/programme-queue/event-stop-pause-restart", Fixture, NULL, setup, test_event_stop_pause_restart, teardown);
    g_test_add("/programme-queue/event-completion-silent-tail", Fixture, NULL, setup, test_event_completion_silent_tail, teardown);
    g_test_add("/programme-queue/event-completion-frozen-owner", Fixture, NULL, setup, test_event_completion_frozen_owner, teardown);
    g_test_add("/programme-queue/independent-event-runtime-separate-primary", Fixture, NULL, setup, test_independent_event_runtime_is_separate_from_primary, teardown);
    g_test_add("/programme-queue/row-players", Fixture, NULL, setup, test_row_players, teardown);
    g_test_add("/programme-queue/completion-destroy", Fixture, NULL, setup, test_completion_destroy, teardown);
    g_test_add("/programme-queue/kilavuz-row-controls-talk", Fixture, NULL, setup, test_kilavuz_row_controls, teardown);
    g_test_add("/programme-queue/kilavuz-context-menu-properties", Fixture, NULL, setup, test_kilavuz_context_menu, teardown);
    g_test_add("/programme-queue/kilavuz-sub-row-numbering", Fixture, NULL, setup, test_kilavuz_sub_row_numbering, teardown);
    g_test_add("/programme-queue/kilavuz-overlay-sub-row-state", Fixture, NULL, setup, test_kilavuz_overlay_sub_row, teardown);
    g_test_add("/programme-queue/kilavuz-display-window", Fixture, NULL, setup, test_kilavuz_display_window, teardown);
    g_test_add("/programme-queue/kilavuz-transition-g", Fixture, NULL, setup, test_kilavuz_transition, teardown);
    g_test_add("/programme-queue/kilavuz-ending-blink", Fixture, NULL, setup, test_kilavuz_ending_blink, teardown);
    g_test_add("/programme-queue/kilavuz-live-row", Fixture, NULL, setup, test_kilavuz_live_row, teardown);
    g_test_add("/programme-queue/kilavuz-talk-defers-rebuild", Fixture, NULL, setup, test_kilavuz_talk_defers_rebuild, teardown);
    g_test_add("/programme-queue/kilavuz-branch-line-full-height", Fixture, NULL, setup, test_kilavuz_branch_line, teardown);
    g_test_add("/programme-queue/kilavuz-kind-accent-next-selected-playing", Fixture, NULL, setup, test_kilavuz_kind_accent_states, teardown);
    g_test_add("/programme-queue/kilavuz-deselect-esc-empty-reclick", Fixture, NULL, setup, test_kilavuz_deselect, teardown);
    g_test_add("/programme-queue/kilavuz-live-step-on-air-complete", Fixture, NULL, setup, test_kilavuz_live_step, teardown);
    g_test_add("/programme-queue/kilavuz-live-label-runtime", Fixture, NULL, setup, test_kilavuz_live_label, teardown);
    return g_test_run();
}
