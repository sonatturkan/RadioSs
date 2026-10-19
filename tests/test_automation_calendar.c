#include "radios/data.h"
#include "radios/widgets.h"
#include "radios/automation_calendar.h"
#include "radios/rotation_editor.h"
#include "radios/media_jobs.h"
#include "radios/media_ui.h"
#include "radios/platform.h"

#include <string.h>
#include <stdio.h>
#include <fontconfig/fontconfig.h>
#include <glib/gstdio.h>
#include <math.h>

/* Playlist builder native acceptance: exercise real controls and verify acknowledged
 * documents through a second authenticated client, then reopen the widget.
 * This deliberately uses a focused GTK/data harness, so unrelated profiles
 * and audio devices cannot make calendar acceptance pass or fail. */
static RadioSConfig config;
static RadioSDataClient *reader;
static GtkCssProvider *theme;
static RadioSRuntime *active_runtime;
extern const gchar *const radios_reference_css_parts[];
typedef gboolean (*Condition)(gpointer);
typedef struct {
    RadioSViewContext ctx;
    GtkWidget *window, *page;
    gchar *last_notice;
} Harness;

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    /* GTK owns attached menus outside the widget child hierarchy. Discover
     * the actual menu, then activate its real item through GtkMenuShell. */
    GList *menus = gtk_menu_get_for_attach_widget(root);
    for (GList *item = menus; item; item = item->next) {
        GtkWidget *found = find(item->data, name);
        if (found) return found;
    }
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next)
        found = find(item->data, name);
    g_list_free(children); return found;
}

static GtkWidget *dialog_find(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *found = NULL;
    for (GList *item = windows; item && !found; item = item->next)
        if (!g_strcmp0(gtk_widget_get_name(item->data), name)) found = item->data;
    g_list_free(windows); return found;
}

static GtkWidget *find_paned(GtkWidget *widget)
{
    if (GTK_IS_PANED(widget)) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find_paned(item->data);
    g_list_free(children); return found;
}

static void capture_to(Harness *harness, const gchar *path);

static void pump(void)
{
    /* The real window drains runtime status events on its UI timer. This
     * focused harness must also consume them: the bounded runtime correctly
     * pauses its worker when an undrained event queue fills. */
    RadioSEvent *event;
    while (active_runtime && (event = radios_runtime_pop_event(active_runtime)) != NULL)
        radios_event_free(event);
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
}

static void wait_until(Condition condition, gpointer value, const gchar *reason)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < deadline) pump();
    if (!condition(value)) g_error("Native calendar acceptance timed out: %s", reason);
}

static gboolean documents_ready(GtkWidget *widget)
{
    GPtrArray *documents = g_object_get_data(G_OBJECT(widget), "radios-data-documents");
    for (guint i = 0; documents && i < documents->len; ++i)
        if (!radios_data_document_ready(g_ptr_array_index(documents, i))) return FALSE;
    if (!GTK_IS_CONTAINER(widget)) return TRUE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget)); gboolean ready = TRUE;
    for (GList *item = children; item && ready; item = item->next)
        ready = documents_ready(item->data);
    g_list_free(children); return ready;
}

static gboolean harness_ready(gpointer value)
{
    Harness *harness = value;
    GtkWidget *main = find(harness->page, "automation-calendar-main"); g_assert_nonnull(main);
    GtkWidget *legacy = g_object_get_data(G_OBJECT(main), "legacy-rules");
    return radios_data_connected(&harness->ctx) && documents_ready(harness->page) &&
        (!legacy || documents_ready(legacy));
}

static void harness_notice(gpointer value, const gchar *message)
{
    Harness *harness = value; g_free(harness->last_notice); harness->last_notice = g_strdup(message);
    g_test_message("Native data notice: %s", message);
}

static Harness *harness_open_with_rules(gboolean production_rules)
{
    Harness *harness = g_new0(Harness, 1);
    harness->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(harness->window);
    gboolean wide_capture = g_getenv("RADIOS_AUTOMATION_WIDE_CAPTURE") != NULL;
    gint window_width = wide_capture ? 1920 : 1500, window_height = wide_capture ? 1042 : 980;
    const gchar *window_size = g_getenv("RADIOS_AUTOMATION_WINDOW_SIZE");
    if (window_size && *window_size) {
        gchar *end = NULL;
        gint64 width = g_ascii_strtoll(window_size, &end, 10);
        g_assert_nonnull(end); g_assert_cmpint(*end, ==, 'x');
        gint64 height = g_ascii_strtoll(end + 1, &end, 10);
        g_assert_cmpint(*end, ==, 0);
        g_assert_cmpint(width, >=, 1000); g_assert_cmpint(width, <=, 3840);
        g_assert_cmpint(height, >=, 584); g_assert_cmpint(height, <=, 2160);
        window_width = (gint)width; window_height = (gint)height;
    }
    gtk_window_set_default_size(GTK_WINDOW(harness->window), window_width, window_height);
    gtk_style_context_add_class(gtk_widget_get_style_context(harness->window), "radios");
    harness->ctx.config = &config; harness->ctx.parent = GTK_WINDOW(harness->window);
    harness->ctx.notify = harness_notice; harness->ctx.notify_data = harness;
    harness->ctx.runtime = radios_runtime_new(16, 32);
    active_runtime = harness->ctx.runtime;
    harness->ctx.data = radios_data_new(&harness->ctx);
    if (production_rules) harness->page = radios_automation_new(&harness->ctx);
    else {
        GtkWidget *legacy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), *canary = gtk_entry_new();
        gtk_widget_set_name(canary, "automation-legacy-acceptance"); gtk_entry_set_text(GTK_ENTRY(canary), "Korunan gelişmiş kural");
        gtk_box_pack_start(GTK_BOX(legacy), canary, FALSE, FALSE, 0);
        harness->page = radios_automation_calendar_new(&harness->ctx, legacy);
    }
    g_assert_nonnull(harness->page); gtk_container_add(GTK_CONTAINER(harness->window), harness->page);
    gtk_widget_show_all(harness->window);
    /* Real keyboard activation cannot bypass readiness through a disabled
     * create button and replace a saved document with an unloaded draft. */
    g_assert_false(documents_ready(harness->page));
    JsonNode *unloaded = radios_automation_calendar_snapshot(harness->page); g_assert_nonnull(unloaded);
    radios_automation_calendar_select(harness->page, 0, 600, 660, FALSE);
    GtkWidget *create = find(harness->page, "automation-calendar-save-selection");
    g_assert_nonnull(create); g_assert_false(gtk_widget_is_sensitive(create));
    GtkWidget *grid = find(harness->page, "automation-calendar-grid"); g_assert_nonnull(grid);
    GdkEvent *enter = gdk_event_new(GDK_KEY_PRESS);
    enter->key.window = g_object_ref(gtk_widget_get_window(grid)); enter->key.send_event = TRUE;
    enter->key.keyval = GDK_KEY_Return;
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(grid));
    gdk_event_set_device(enter, gdk_seat_get_keyboard(seat));
    g_assert_true(gtk_widget_event(grid, enter)); gdk_event_free(enter);
    g_assert_null(dialog_find("automation-calendar-event-dialog"));
    JsonNode *after_enter = radios_automation_calendar_snapshot(harness->page);
    g_assert_true(json_node_equal(unloaded, after_enter)); json_node_free(unloaded); json_node_free(after_enter);
    GtkWidget *date = find(harness->page, "automation-calendar-date"); g_assert_nonnull(date);
    g_signal_emit_by_name(date, "activate");
    radios_data_connect(&harness->ctx);
    wait_until(harness_ready, harness, "genuine DB context and saved timetable loaded");
    return harness;
}

static Harness *harness_open(void)
{ return harness_open_with_rules(FALSE); }

static void harness_close(Harness *harness)
{
    /* Mirror the production pre-data-close hook, including the subsequent
     * widget destroy callback. The checkpoint must tolerate both calls. */
    g_test_message("Native window close: pre-data-close checkpoint then widget destroy");
    radios_automation_calendar_prepare_close(harness->page);
    const gchar *menu_items[] = {"automation-calendar-copy", "automation-calendar-row-edit"};
    for (guint i = 0; i < G_N_ELEMENTS(menu_items); ++i) {
        GtkWidget *item = find(harness->page, menu_items[i]); g_assert_true(GTK_IS_MENU_ITEM(item));
        g_assert_false(gtk_widget_get_mapped(gtk_widget_get_parent(item)));
    }
    gtk_widget_destroy(harness->window); radios_data_close(harness->ctx.data);
    active_runtime = NULL;
    radios_runtime_free(harness->ctx.runtime);
    while (g_main_context_iteration(NULL, FALSE)) {}
    radios_data_unref(harness->ctx.data); g_object_unref(harness->window); g_free(harness->last_notice); g_free(harness);
}

static GtkWidget *control(GtkWidget *root, const gchar *name)
{
    GtkWidget *widget = find(root, name);
    if (!widget && !g_strcmp0(name, "weekly-programme-calendar")) widget = g_object_get_data(G_OBJECT(root), "weekly-programme-calendar");
    g_assert_nonnull(widget); return widget;
}

static void click(GtkWidget *root, const gchar *name)
{
    GtkWidget *button = control(root, name); g_assert_true(GTK_IS_BUTTON(button));
    g_assert_true(gtk_widget_is_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button)); pump();
}

/* Check the user's layout at actual allocated coordinates. A mapped child
 * alone does not prove that its actions fit beside the timetable. */
static void settle_layout(void)
{
    gint64 deadline = g_get_monotonic_time() + 120 * 1000;
    while (g_get_monotonic_time() < deadline) pump();
}

static GdkRectangle window_rect(Harness *harness, GtkWidget *widget)
{
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    GdkRectangle rectangle = {0, 0, allocation.width, allocation.height};
    g_assert_true(gtk_widget_translate_coordinates(widget, harness->window, 0, 0,
                                                  &rectangle.x, &rectangle.y));
    return rectangle;
}

static void assert_top_calendar_controls(Harness *harness)
{
    settle_layout();
    GtkWidget *paned = find_paned(harness->page);
    g_assert_true(GTK_IS_PANED(paned));
    GtkWidget *left = gtk_paned_get_child1(GTK_PANED(paned));
    GtkWidget *header = control(harness->page, "automation-calendar-days");
    GdkRectangle day_header = window_rect(harness, header);
    g_assert_null(find(harness->page, "automation-calendar-save"));
    const gchar *top_controls[] = {"automation-calendar-date", "automation-calendar-week-range",
        "automation-calendar-selection", "automation-calendar-save-selection"};
    for (guint i = 0; i < G_N_ELEMENTS(top_controls); ++i) {
        GtkWidget *widget = control(harness->page, top_controls[i]);
        g_assert_true(gtk_widget_is_ancestor(widget, left));
        g_assert_true(gtk_widget_get_mapped(widget));
        GdkRectangle rectangle = window_rect(harness, widget);
        g_assert_cmpint(rectangle.y + rectangle.height, <=, day_header.y);
    }
}

static void assert_programme_panel(Harness *harness)
{
    settle_layout();
    GtkWidget *paned = find_paned(harness->page);
    g_assert_true(GTK_IS_PANED(paned));
    GtkWidget *right = gtk_paned_get_child2(GTK_PANED(paned));
    GtkWidget *editor = control(harness->page, "automation-calendar-programme-editor");
    GtkWidget *rows = control(harness->page, "automation-calendar-rotations");
    GtkWidget *define = control(harness->page, "automation-calendar-define-rotation");
    g_assert_null(find(harness->page, "automation-calendar-tabs"));
    GtkWidget *main = harness->page;
    g_assert_cmpstr(gtk_widget_get_name(main), ==, "automation-calendar-main");
    GtkWidget *program = control(main, "weekly-programme-workspace");
    g_assert_true(gtk_widget_is_ancestor(program, right));
    g_assert_true(gtk_widget_get_mapped(control(program, "weekly-programme-choice")));
    g_assert_true(g_object_get_data(G_OBJECT(program), "weekly-programme-calendar") == main);
    GtkWidget *available = control(main, "automation-calendar-available-rotations");
    g_assert_cmpint(gtk_tree_selection_get_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(available))), ==, GTK_SELECTION_MULTIPLE);
    /* Name and content only; the rotation table keeps its weight column hidden. */
    g_assert_cmpuint(gtk_tree_view_get_n_columns(GTK_TREE_VIEW(available)), ==, 2);
    g_assert_cmpuint(gtk_tree_view_get_n_columns(GTK_TREE_VIEW(rows)), ==, 3);
    for (gint i = 0; i < 3; ++i) {
        if (i < 2) g_assert_true(gtk_tree_view_column_get_visible(gtk_tree_view_get_column(GTK_TREE_VIEW(available), i)));
        g_assert_cmpint(gtk_tree_view_column_get_visible(gtk_tree_view_get_column(GTK_TREE_VIEW(rows), i)), ==, i < 2);
    }
    g_assert_false(gtk_expander_get_expanded(GTK_EXPANDER(control(main, "automation-calendar-details"))));
    g_assert_true(gtk_widget_is_ancestor(control(main, "automation-calendar-programme-name"), control(main, "automation-calendar-programme-section")));
    g_assert_true(gtk_widget_is_ancestor(rows, control(main, "automation-calendar-rotation-section")));
    g_assert_true(gtk_widget_is_ancestor(control(main, "automation-calendar-programme-probability"), control(main, "automation-calendar-rules-section")));
    g_assert_false(gtk_widget_get_visible(control(main, "automation-calendar-existing-rotation")));
    g_assert_false(gtk_widget_get_visible(control(main, "automation-calendar-manage-rotation")));
    GtkWidget *save = control(main, "automation-calendar-save-programme");
    g_assert_true(GTK_IS_BUTTON(save)); g_assert_true(gtk_widget_get_mapped(save));
    const gchar *removed[] = {"automation-calendar-rules-tab",
        "automation-calendar-detail-stack", "automation-calendar-autodj", "automation-calendar-edit-programme"};
    for (guint i = 0; i < G_N_ELEMENTS(removed); ++i) g_assert_null(find(harness->page, removed[i]));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(control(harness->page, "automation-calendar-detail-title"))), ==, "Özellikler");
    const gchar *fields[] = {"automation-calendar-programme-date", "automation-calendar-programme-start",
        "automation-calendar-programme-end"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) {
        GtkWidget *field = control(harness->page, fields[i]);
        g_assert_true(GTK_IS_ENTRY(field)); g_assert_true(gtk_widget_get_mapped(field));
        g_assert_true(gtk_widget_is_ancestor(field, right));
    }
    g_assert_true(gtk_widget_is_ancestor(editor, right));
    g_assert_true(gtk_widget_is_ancestor(rows, editor));
    g_assert_true(gtk_widget_get_mapped(editor));
    g_assert_true(gtk_widget_get_mapped(define));
    GtkWidget *row_scroll = gtk_widget_get_ancestor(rows, GTK_TYPE_SCROLLED_WINDOW);
    g_assert_nonnull(row_scroll);
    GdkRectangle panel = window_rect(harness, right);
    g_assert_cmpint(panel.width, >=, 480);
    g_assert_cmpint(panel.width, <=, 600);
    GdkRectangle body = window_rect(harness, editor);
    GdkRectangle table = window_rect(harness, row_scroll);
    GdkRectangle action = window_rect(harness, define);
    g_assert_cmpint(table.height, >=, 90);
    g_assert_cmpint(action.x, >=, panel.x);
    g_assert_cmpint(action.x + action.width, <=, panel.x + panel.width);
    g_assert_cmpint(action.y, >=, table.y + table.height);
    g_assert_cmpint(action.y + action.height, <=, panel.y + panel.height);
    g_assert_cmpint(body.x + body.width - action.x - action.width, >=, 0);
    g_assert_cmpint(body.x + body.width - action.x - action.width, <=, 32);
    g_test_message("PDF13 allocated layout: right=%dx%d at %d,%d; rotations=%dx%d; define=%d,%d %dx%d",
        panel.width, panel.height, panel.x, panel.y, table.width, table.height,
        action.x, action.y, action.width, action.height);
}

static void set_date(Harness *harness, const gchar *date)
{
    GtkWidget *entry = control(harness->page, "automation-calendar-date");
    g_assert_true(GTK_IS_ENTRY(entry)); gtk_entry_set_text(GTK_ENTRY(entry), date);
    g_signal_emit_by_name(entry, "activate"); pump();
}

static void pointer_button(GtkWidget *widget, GdkEventType type, gdouble x, gdouble y,
                           GdkModifierType state, guint button)
{
    GdkEvent *event = gdk_event_new(type);
    GdkWindow *window = GTK_IS_TREE_VIEW(widget) ? gtk_tree_view_get_bin_window(GTK_TREE_VIEW(widget)) : gtk_widget_get_window(widget);
    event->any.window = g_object_ref(window); event->any.send_event = TRUE;
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(widget));
    gdk_event_set_device(event, gdk_seat_get_pointer(seat));
    gint root_x, root_y; gdk_window_get_origin(window, &root_x, &root_y);
    if (type == GDK_MOTION_NOTIFY) {
        event->motion.x = x; event->motion.y = y; event->motion.state = state;
        event->motion.x_root = root_x + x; event->motion.y_root = root_y + y;
    } else {
        event->button.x = x; event->button.y = y; event->button.state = state; event->button.button = button;
        event->button.x_root = root_x + x; event->button.y_root = root_y + y;
    }
    g_assert_true(gtk_widget_event(widget, event)); gdk_event_free(event); pump();
}

static void pointer_event(GtkWidget *widget, GdkEventType type, gdouble x, gdouble y,
                          GdkModifierType state)
{ pointer_button(widget, type, x, y, state, 1); }

static void grid_point(Harness *harness, guint day, guint minute, gdouble *x, gdouble *y)
{
    GtkWidget *grid = control(harness->page, "automation-calendar-grid");
    GtkAllocation allocation; gtk_widget_get_allocation(grid, &allocation);
    gint height; gtk_widget_get_size_request(grid, NULL, &height);
    *x = 60.0 + (day + 0.5) * (allocation.width - 60.0) / 7.0;
    *y = minute * height / 1440.0 + 1;
    GtkWidget *scroll = gtk_widget_get_ancestor(grid, GTK_TYPE_SCROLLED_WINDOW); g_assert_nonnull(scroll);
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)), MAX(0.0, *y - 90.0)); pump();
}

static gboolean menu_item_viewable(gpointer value)
{
    GtkWidget *item = value, *menu = gtk_widget_get_parent(item);
    GdkWindow *window = gtk_widget_get_window(menu);
    return gtk_widget_get_mapped(item) && gtk_widget_get_mapped(menu) && window && gdk_window_is_viewable(window);
}

static void activate_menu(Harness *harness, const gchar *name)
{
    GtkWidget *item = control(harness->page, name);
    g_assert_true(GTK_IS_MENU_ITEM(item)); g_assert_true(gtk_widget_is_sensitive(item));
    wait_until(menu_item_viewable, item, "native context item and menu mapped/viewable before activation");
    g_assert_true(gtk_widget_get_mapped(item));
    GtkWidget *menu = gtk_widget_get_parent(item); g_assert_true(GTK_IS_MENU(menu));
    gtk_menu_shell_activate_item(GTK_MENU_SHELL(menu), item, TRUE); pump();
}

static void block_action(Harness *harness, guint day, guint minute, const gchar *name)
{
    gdouble x, y; grid_point(harness, day, minute, &x, &y);
    pointer_button(control(harness->page, "automation-calendar-grid"), GDK_BUTTON_PRESS, x, y, 0, 3);
    activate_menu(harness, name);
}

static void row_pointer(Harness *harness, guint index, GdkEventType type, guint button)
{
    GtkWidget *tree = control(harness->page, "automation-calendar-rotations");
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)index, -1);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(tree), path, NULL, FALSE, 0, 0); settle_layout();
    GdkRectangle rect; gtk_tree_view_get_cell_area(GTK_TREE_VIEW(tree), path, NULL, &rect);
    gtk_tree_path_free(path);
    pointer_button(tree, type, MAX(12, rect.x + 4), rect.y + rect.height / 2.0, 0, button);
}

static void row_action(Harness *harness, guint index, const gchar *name)
{
    row_pointer(harness, index, GDK_BUTTON_PRESS, 3); activate_menu(harness, name);
}

static void pointer_select(Harness *harness, guint day, guint start, guint end, gboolean drag)
{
    GtkWidget *grid = control(harness->page, "automation-calendar-grid");
    GtkAllocation allocation; gtk_widget_get_allocation(grid, &allocation);
    gint requested_height; gtk_widget_get_size_request(grid, NULL, &requested_height);
    gdouble scale = requested_height / 1440.0;
    gdouble x = 60.0 + (day + 0.5) * (allocation.width - 60.0) / 7.0;
    gdouble y = start * scale + 1;
    GtkWidget *scroll = gtk_widget_get_ancestor(grid, GTK_TYPE_SCROLLED_WINDOW); g_assert_nonnull(scroll);
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    gtk_adjustment_set_value(adjustment, MAX(0.0, y - 90.0)); pump();
    pointer_event(grid, GDK_BUTTON_PRESS, x, y, 0);
    if (drag) {
        y = (end - 30) * scale + 1;
        pointer_event(grid, GDK_MOTION_NOTIFY, x, y, GDK_BUTTON1_MASK);
    }
    pointer_event(grid, GDK_BUTTON_RELEASE, x, y, 0);
}

typedef struct {
    const gchar *path;
    JsonNode *body, *result;
    GError *error;
    gint done;
} Request;

static gpointer request_worker(gpointer value)
{
    Request *request = value;
    request->result = radios_data_client_request_sync(reader, "POST", request->path,
                                                     request->body, NULL, &request->error);
    g_atomic_int_set(&request->done, 1); return NULL;
}
static gboolean request_done(gpointer value) { return g_atomic_int_get(&((Request *)value)->done) != 0; }
static JsonNode *request(const gchar *path, JsonNode *body)
{
    Request task = {path, body, NULL, NULL, 0};
    GThread *thread = g_thread_new("calendar-acceptance-read", request_worker, &task);
    wait_until(request_done, &task, "authenticated independent DB operation"); g_thread_join(thread);
    g_assert_no_error(task.error); g_assert_nonnull(task.result); return task.result;
}

static JsonNode *body_new(void)
{
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(body, json_object_new());
    json_object_set_string_member(json_node_get_object(body), "station_id", radios_data_client_station_id(reader));
    return body;
}

static gchar *create_playlist(const gchar *name)
{
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "name", name); gchar *key = g_uuid_string_random();
    json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonNode *result = request("/v1/workflows/desktop-playlists/create", body);
    gchar *id = g_strdup(radios_json_string(json_node_get_object(result), "_id", ""));
    g_assert_cmpstr(id, !=, ""); json_node_free(result); json_node_free(body); return id;
}

static JsonNode *calendar_record(void)
{
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "kind", "automation_calendar");
    JsonNode *result = request("/v1/workflows/desktop/list", body); json_node_free(body);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(result), "records"); JsonNode *record = NULL;
    if (rows && json_array_get_length(rows)) {
        g_assert_cmpuint(json_array_get_length(rows), ==, 1);
        record = json_node_copy(json_array_get_element(rows, 0));
    }
    json_node_free(result); return record;
}

static JsonNode *snapshot_of(GtkWidget *page);

static gboolean calendar_acknowledged(gpointer value)
{
    Harness *harness = value;
    GtkWidget *main = find(harness->page, "automation-calendar-main");
    GPtrArray *documents = g_object_get_data(G_OBJECT(main ? main : harness->page), "radios-data-documents");
    return documents && documents->len == 1 &&
        radios_data_document_revision(g_ptr_array_index(documents, 0)) > 0;
}

static void wait_saved(Harness *harness)
{
    JsonNode *expected = snapshot_of(harness->page); g_assert_nonnull(expected);
    g_test_message("Save checkpoint: %s; status=%s",
        gtk_label_get_text(GTK_LABEL(control(harness->page, "automation-calendar-selection"))),
        gtk_label_get_text(GTK_LABEL(control(harness->page, "automation-calendar-status"))));
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    gboolean saved = FALSE;
    while (!saved && g_get_monotonic_time() < deadline) {
        pump();
        if (!calendar_acknowledged(harness)) continue;
        JsonNode *record = calendar_record();
        if (record) {
            JsonObject *object = json_node_get_object(record);
            saved = radios_json_int(object, "_rev", 0) > 0 &&
                json_node_equal(json_object_get_member(object, "config"), expected);
            json_node_free(record);
        }
        if (!saved) {
            gint64 next_read = MIN(deadline, g_get_monotonic_time() + 25 * 1000);
            while (g_get_monotonic_time() < next_read) pump();
        }
    }
    if (!saved) {
        gchar *wanted = json_to_string(expected, FALSE); JsonNode *last = calendar_record();
        gchar *actual = last ? json_to_string(json_object_get_member(json_node_get_object(last), "config"), FALSE) : g_strdup("no record");
        g_test_message("Expected fixture config: %s", wanted); g_test_message("Saved fixture config: %s", actual);
        g_test_message("Native status: %s", gtk_label_get_text(GTK_LABEL(control(harness->page, "automation-calendar-status"))));
        g_test_message("Last native data notice: %s", harness->last_notice ? harness->last_notice : "none");
        g_free(wanted); g_free(actual); if (last) json_node_free(last);
    }
    json_node_free(expected);
    if (!saved) g_error("Native calendar changes did not reach the genuine saved document");
}

static JsonNode *snapshot_of(GtkWidget *page)
{
    JsonNode *current = radios_automation_calendar_snapshot(page); g_assert_nonnull(current);
    gchar *text = json_to_string(current, FALSE); JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    JsonNode *frozen = json_node_copy(json_parser_get_root(parser));
    g_object_unref(parser); g_free(text); json_node_free(current); return frozen;
}

static JsonArray *events(JsonNode *snapshot)
{
    g_assert_true(JSON_NODE_HOLDS_OBJECT(snapshot));
    JsonObject *object = json_node_get_object(snapshot);
    g_assert_cmpint(radios_json_int(object, "schema", 0), ==, 1);
    JsonArray *array = json_object_get_array_member(object, "events"); g_assert_nonnull(array); return array;
}

static JsonObject *event_at(JsonNode *snapshot, const gchar *date, gint start)
{
    JsonArray *array = events(snapshot);
    for (guint i = 0; i < json_array_get_length(array); ++i) {
        JsonObject *event = json_array_get_object_element(array, i);
        if (!g_strcmp0(radios_json_string(event, "date", ""), date) &&
            radios_json_int(event, "start_minute", -1) == start) return event;
    }
    g_error("Expected calendar event at %s minute %d", date, start); return NULL;
}

static void new_event(Harness *harness, guint day, guint start, guint end, const gchar *title, gboolean green)
{
    if (end - start >= 30) pointer_select(harness, day, start, end, TRUE);
    else radios_automation_calendar_select(harness->page, day, start, end, FALSE);
    GtkWidget *selection = control(harness->page, "automation-calendar-selection");
    g_assert_true(GTK_IS_LABEL(selection));
    gchar *start_text = g_strdup_printf("%02u:%02u", start / 60, start % 60);
    if (!strstr(gtk_label_get_text(GTK_LABEL(selection)), start_text)) g_test_message("selection label: '%s' expected %s", gtk_label_get_text(GTK_LABEL(selection)), start_text);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(selection)), start_text)); g_free(start_text);
    click(harness->page, "automation-calendar-save-selection");
    GtkWidget *dialog = dialog_find("automation-calendar-event-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "automation-calendar-event-name")), title);
    gtk_combo_box_set_active(GTK_COMBO_BOX(control(dialog, "automation-calendar-event-color")), green ? 1 : 0);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); pump(); wait_saved(harness);
    g_assert_true(gtk_widget_get_mapped(control(harness->page, "automation-calendar-programme-name")));
    g_assert_true(gtk_widget_get_mapped(control(harness->page, "automation-calendar-rotations")));
}

typedef struct {GtkWidget *combo; const gchar *id;} PlaylistChoice;
static gboolean playlist_available(gpointer value)
{
    PlaylistChoice *choice = value;
    return gtk_combo_box_set_active_id(GTK_COMBO_BOX(choice->combo), choice->id);
}

typedef struct {GtkWidget *tree; const gchar *id;} RotationLibraryChoice;
static gboolean rotation_library_selected(gpointer value)
{
    RotationLibraryChoice *choice = value; GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(choice->tree));
    GtkTreeIter row; gboolean valid = gtk_tree_model_get_iter_first(model, &row);
    while (valid) {
        gchar *id = NULL; gtk_tree_model_get(model, &row, 0, &id, -1);
        gboolean matches = !g_strcmp0(id, choice->id); g_free(id);
        if (matches) {
            GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(choice->tree));
            gtk_tree_selection_unselect_all(selection); gtk_tree_selection_select_iter(selection, &row);
            GtkTreePath *path = gtk_tree_model_get_path(model, &row);
            gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(choice->tree), path, NULL, FALSE, 0, 0);
            gtk_tree_path_free(path); return TRUE;
        }
        valid = gtk_tree_model_iter_next(model, &row);
    }
    return FALSE;
}
static void select_library_rotation(GtkWidget *page, const gchar *id)
{
    GtkWidget *picker = control(page, "automation-calendar-library-picker");
    gtk_expander_set_expanded(GTK_EXPANDER(picker), TRUE); settle_layout();
    RotationLibraryChoice choice = {control(page, "automation-calendar-available-rotations"), id};
    wait_until(rotation_library_selected, &choice, "saved rotation selected in the actual assignment table");
    pump();
    g_assert_cmpint(gtk_tree_selection_count_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(choice.tree))), ==, 1);
    g_assert_true(gtk_widget_get_mapped(choice.tree));
}

typedef struct {Harness *harness; JsonNode *expected;} LoadedCalendar;
static gboolean fixture_calendar_loaded(gpointer value)
{
    LoadedCalendar *loaded = value;
    JsonNode *current = snapshot_of(loaded->harness->page);
    gboolean matches = json_node_equal(current, loaded->expected);
    json_node_free(current); return matches;
}

/* Legacy rotation creation is now fixture preparation through the genuine
 * authenticated CAS document workflow. The reusable editor acceptance below
 * creates new definitions through actual GTK controls. Existing legacy edit,
 * copy/move/cancel and verified preview behavior remain native regressions. */
static void seed_legacy_rotation(Harness *harness, const gchar *playlist, guint frequency,
                                 gboolean random, gboolean track, gboolean advert, gboolean sweeper)
{
    wait_saved(harness);
    const gchar *date = gtk_entry_get_text(GTK_ENTRY(control(harness->page, "automation-calendar-programme-date")));
    const gchar *time = gtk_entry_get_text(GTK_ENTRY(control(harness->page, "automation-calendar-programme-start")));
    const gchar *colon = strchr(time, ':'); g_assert_nonnull(colon);
    gint minute = (gint)g_ascii_strtoll(time, NULL, 10) * 60 + (gint)g_ascii_strtoll(colon + 1, NULL, 10);
    JsonNode *expected = snapshot_of(harness->page);
    JsonArray *rows = json_object_get_array_member(event_at(expected, date, minute), "rotations");
    guint previous_rows = json_array_get_length(rows);
    JsonObject *row = json_object_new(); gchar *id = g_uuid_string_random();
    json_object_set_string_member(row, "id", id); g_free(id);
    json_object_set_string_member(row, "playlist_id", playlist);
    json_object_set_int_member(row, "frequency", frequency);
    json_object_set_boolean_member(row, "random", random);
    json_object_set_boolean_member(row, "track_random", track);
    json_object_set_boolean_member(row, "advert_random", advert);
    json_object_set_boolean_member(row, "sweeper_random", sweeper);
    json_array_add_object_element(rows, row);
    JsonNode *record = calendar_record(); g_assert_nonnull(record);
    JsonObject *saved = json_node_get_object(record);
    JsonNode *body = body_new(); JsonObject *put = json_node_get_object(body);
    json_object_set_string_member(put, "kind", "automation_calendar");
    json_object_set_string_member(put, "name", radios_json_string(saved, "name", "Automation calendar"));
    json_object_set_string_member(put, "document_id", radios_json_string(saved, "_id", ""));
    json_object_set_int_member(put, "expected_revision", radios_json_int(saved, "_rev", 0));
    json_object_set_member(put, "config", json_node_copy(expected));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(put, "idempotency_key", key); g_free(key);
    JsonNode *reply = request("/v1/workflows/desktop/put", body);
    g_assert_true(json_node_equal(expected, json_object_get_member(json_node_get_object(reply), "config")));
    json_node_free(reply); json_node_free(body); json_node_free(record);
    radios_data_refresh(&harness->ctx);
    LoadedCalendar loaded = {harness, expected};
    wait_until(fixture_calendar_loaded, &loaded, "genuine legacy document fixture loaded by production refresh");
    GtkWidget *tree = control(harness->page, "automation-calendar-rotations");
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(tree)), NULL), ==,
                    (gint)previous_rows + 1);
    wait_saved(harness); json_node_free(expected);
}

static void cancel_rotation(Harness *harness, const gchar *playlist)
{
    JsonNode *original = snapshot_of(harness->page);
    GtkWidget *tree = control(harness->page, "automation-calendar-rotations");
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
    guint index = (guint)gtk_tree_model_iter_n_children(model, NULL);
    seed_legacy_rotation(harness, playlist, 1, FALSE, FALSE, FALSE, FALSE);
    JsonNode *before = snapshot_of(harness->page);
    row_action(harness, index, "automation-calendar-row-edit");
    GtkWidget *dialog = dialog_find("automation-calendar-rotation-dialog");
    g_assert_true(GTK_IS_DIALOG(dialog));
    PlaylistChoice choice = {control(dialog, "automation-calendar-rotation-playlist"), playlist};
    wait_until(playlist_available, &choice, "actual saved legacy playlist before cancelling edit");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(dialog, "automation-calendar-rotation-frequency")), 9);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(dialog, "automation-calendar-rotation-random")), TRUE);
    const gchar *rotation_capture = g_getenv("RADIOS_AUTOMATION_ROTATION_CAPTURE");
    if (rotation_capture && *rotation_capture) {
        Harness dialog_harness = {0}; dialog_harness.window = dialog;
        capture_to(&dialog_harness, rotation_capture);
    }
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); pump();
    g_assert_cmpint(gtk_tree_model_iter_n_children(model, NULL), ==, (gint)index + 1);
    JsonNode *after = snapshot_of(harness->page);
    g_assert_true(json_node_equal(before, after)); json_node_free(before); json_node_free(after);
    wait_saved(harness);
    /* Remove the temporary fixture through the actual row context action;
     * the original counters and complete document must be restored. */
    row_action(harness, index, "automation-calendar-row-remove"); wait_saved(harness);
    after = snapshot_of(harness->page);
    g_assert_true(json_node_equal(original, after)); json_node_free(original); json_node_free(after);
}

static void select_rotation(Harness *harness, guint index)
{
    GtkWidget *tree = control(harness->page, "automation-calendar-rotations"); g_assert_true(GTK_IS_TREE_VIEW(tree));
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    gtk_tree_selection_unselect_all(selection);
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)index, -1);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(tree), path, NULL, FALSE); gtk_tree_selection_select_path(selection, path);
    gtk_tree_path_free(path);
    g_assert_cmpint(gtk_tree_selection_count_selected_rows(selection), ==, 1); pump();
    g_assert_cmpint(gtk_tree_selection_count_selected_rows(selection), ==, 1);
}

static void assert_rotation(JsonObject *rotation, const gchar *playlist, guint frequency,
                            gboolean random, gboolean track, gboolean advert, gboolean sweeper)
{
    g_assert_cmpstr(radios_json_string(rotation, "playlist_id", ""), ==, playlist);
    g_assert_cmpint(radios_json_int(rotation, "frequency", 0), ==, frequency);
    g_assert_cmpint(json_object_get_boolean_member(rotation, "random"), ==, random);
    g_assert_cmpint(json_object_get_boolean_member(rotation, "track_random"), ==, track);
    g_assert_cmpint(json_object_get_boolean_member(rotation, "advert_random"), ==, advert);
    g_assert_cmpint(json_object_get_boolean_member(rotation, "sweeper_random"), ==, sweeper);
}

static gboolean capture_viewable(gpointer value)
{
    GtkWidget *widget = value; GdkWindow *window = gtk_widget_get_window(widget);
    return gtk_widget_get_mapped(widget) && window && gdk_window_is_viewable(window);
}

static void capture_to(Harness *harness, const gchar *path)
{
    if (!path || !*path) return;
    g_test_message("Capture target %s (%s): mapped=%d visible=%d",
        gtk_widget_get_name(harness->window), G_OBJECT_TYPE_NAME(harness->window),
        gtk_widget_get_mapped(harness->window), gtk_widget_get_visible(harness->window));
    wait_until(capture_viewable, harness->window, "actual GTK capture window mapped and viewable");
    /* Let the frame clock paint the acknowledged state before capturing. */
    gint64 frame_deadline = g_get_monotonic_time() + 75 * 1000;
    while (g_get_monotonic_time() < frame_deadline) pump();
    GdkWindow *window = gtk_widget_get_window(harness->window); g_assert_nonnull(window); g_assert_true(gdk_window_is_viewable(window));
    GtkAllocation allocation; gtk_widget_get_allocation(harness->window, &allocation);
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_window(window, 0, 0, allocation.width, allocation.height);
    g_assert_nonnull(pixbuf); GError *error = NULL;
    g_assert_true(gdk_pixbuf_save(pixbuf, path, "png", &error, NULL)); g_assert_no_error(error); g_object_unref(pixbuf);
}

static void capture(Harness *harness)
{ capture_to(harness, g_getenv("RADIOS_AUTOMATION_CAPTURE")); }

static void maximum_date_bounds(Harness *harness)
{
    JsonNode *before = snapshot_of(harness->page);
    set_date(harness, "9999-12-31");
    GtkWidget *date = control(harness->page, "automation-calendar-date");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(date)), ==, "9999-12-27");
    /* This last supported week ends on Friday. Saturday and Sunday cannot
     * produce an out-of-range date or an empty-date saved selection. */
    radios_automation_calendar_select(harness->page, 5, 0, 30, FALSE);
    radios_automation_calendar_select(harness->page, 6, 0, 30, FALSE);
    g_assert_false(gtk_widget_is_sensitive(control(harness->page, "automation-calendar-save-selection")));
    click(harness->page, "automation-calendar-next");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(date)), ==, "9999-12-27");
    radios_automation_calendar_select(harness->page, 4, 0, 30, FALSE);
    g_assert_true(gtk_widget_is_sensitive(control(harness->page, "automation-calendar-save-selection")));
    const gchar *selection = gtk_label_get_text(GTK_LABEL(control(harness->page, "automation-calendar-selection")));
    g_assert_nonnull(strstr(selection, "9999-12-31"));
    /* Paint the final partial week too, exercising all seven header/grid
     * date calculations under the fatal-warning acceptance environment. */
    gint64 frame_deadline = g_get_monotonic_time() + 75 * 1000;
    while (g_get_monotonic_time() < frame_deadline) pump();
    JsonNode *after = snapshot_of(harness->page);
    g_assert_true(json_node_equal(before, after)); json_node_free(before); json_node_free(after);
    set_date(harness, "2026-10-05");
}

static void zoom_preserves_selection(Harness *harness)
{
    JsonNode *before = snapshot_of(harness->page);
    GtkWidget *grid = control(harness->page, "automation-calendar-grid");
    GtkWidget *selection = control(harness->page, "automation-calendar-selection");
    gchar *coordinates = g_strdup(gtk_label_get_text(GTK_LABEL(selection)));
    gint initial_height, enlarged_height, restored_height;
    gtk_widget_get_size_request(grid, NULL, &initial_height);
    click(harness->page, "automation-calendar-zoom-in");
    gtk_widget_get_size_request(grid, NULL, &enlarged_height);
    g_assert_cmpint(enlarged_height, >, initial_height);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(selection)), ==, coordinates);
    click(harness->page, "automation-calendar-zoom-out");
    gtk_widget_get_size_request(grid, NULL, &restored_height);
    g_assert_cmpint(restored_height, ==, initial_height);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(selection)), ==, coordinates);
    JsonNode *after = snapshot_of(harness->page);
    g_assert_true(json_node_equal(before, after));
    json_node_free(before); json_node_free(after); g_free(coordinates);
}

static void test_calendar_selection_rotation_copy_persistence(void)
{
    gchar *first_playlist = create_playlist("PDF13 Müzik"), *second_playlist = create_playlist("PDF13 Reklam ve sweeper");
    Harness *harness = harness_open(); set_date(harness, "2026-10-05");
    assert_top_calendar_controls(harness);
    g_assert_true(GTK_IS_DRAWING_AREA(control(harness->page, "automation-calendar-grid")));
    g_assert_true(GTK_IS_DRAWING_AREA(control(harness->page, "automation-calendar-days")));
    GtkWidget *paned = find_paned(harness->page); g_assert_true(GTK_IS_PANED(paned));
    g_assert_nonnull(gtk_paned_get_child1(GTK_PANED(paned))); g_assert_nonnull(gtk_paned_get_child2(GTK_PANED(paned)));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(control(harness->page, "automation-calendar-save-selection"))), ==, "Program oluştur");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(control(harness->page, "automation-calendar-define-rotation"))), ==, "Yeni rotasyon");
    maximum_date_bounds(harness);
    radios_automation_calendar_select(harness->page, 7, 0, 60, FALSE);
    radios_automation_calendar_select(harness->page, 0, 60, 60, FALSE);
    radios_automation_calendar_select(harness->page, 0, 90, 60, FALSE);
    radios_automation_calendar_select(harness->page, 0, 1430, 1441, FALSE);
    g_assert_false(gtk_widget_is_sensitive(control(harness->page, "automation-calendar-save-selection")));
    JsonNode *unchanged = snapshot_of(harness->page);
    radios_automation_calendar_select(harness->page, 6, 600, 630, FALSE);
    click(harness->page, "automation-calendar-save-selection");
    GtkWidget *cancelled = dialog_find("automation-calendar-event-dialog"); g_assert_true(GTK_IS_DIALOG(cancelled));
    gtk_entry_set_text(GTK_ENTRY(control(cancelled, "automation-calendar-event-name")), "İptal edilen program");
    gtk_dialog_response(GTK_DIALOG(cancelled), GTK_RESPONSE_CANCEL); pump();
    JsonNode *after_cancel = snapshot_of(harness->page);
    g_assert_true(json_node_equal(unchanged, after_cancel)); json_node_free(unchanged); json_node_free(after_cancel);
    new_event(harness, 0, 1080, 1140, "Akşam programı", FALSE);
    assert_programme_panel(harness);
    zoom_preserves_selection(harness);
    JsonNode *snapshot = snapshot_of(harness->page);
    JsonObject *first = event_at(snapshot, "2026-10-05", 1080);
    g_assert_cmpint(radios_json_int(first, "end_minute", 0), ==, 1140);
    g_assert_cmpstr(radios_json_string(first, "title", ""), ==, "Akşam programı");
    g_assert_cmpstr(radios_json_string(first, "color", ""), ==, "#abd6e4");
    json_node_free(snapshot);

    /* A coloured block opens the right properties inspector directly. */
    pointer_select(harness, 0, 1080, 1140, FALSE);
    assert_programme_panel(harness);
    g_assert_false(gtk_widget_is_sensitive(control(harness->page, "automation-calendar-save-selection")));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(harness->page, "automation-calendar-programme-name"))), ==, "Akşam programı");
    cancel_rotation(harness, first_playlist);
    seed_legacy_rotation(harness, first_playlist, 3, FALSE, TRUE, FALSE, TRUE);
    GtkWidget *tree = control(harness->page, "automation-calendar-rotations");
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(tree)), NULL), ==, 1);
    seed_legacy_rotation(harness, second_playlist, 7, TRUE, FALSE, TRUE, FALSE);
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(tree)), NULL), ==, 2);
    select_rotation(harness, 1); row_action(harness, 1, "automation-calendar-row-up"); wait_saved(harness);
    snapshot = snapshot_of(harness->page); first = event_at(snapshot, "2026-10-05", 1080);
    JsonArray *rotations = json_object_get_array_member(first, "rotations"); g_assert_cmpuint(json_array_get_length(rotations), ==, 2);
    assert_rotation(json_array_get_object_element(rotations, 0), second_playlist, 7, TRUE, FALSE, TRUE, FALSE);
    assert_rotation(json_array_get_object_element(rotations, 1), first_playlist, 3, FALSE, TRUE, FALSE, TRUE);
    json_node_free(snapshot);

    new_event(harness, 0, 1170, 1320, "Uzun yayın", TRUE);
    new_event(harness, 0, 1380, 1440, "Gece yarısı sınırı", FALSE);
    radios_automation_calendar_select(harness->page, 0, 1080, 1140, FALSE);
    radios_automation_calendar_select(harness->page, 0, 1170, 1320, TRUE);
    block_action(harness, 0, 1080, "automation-calendar-copy");
    radios_automation_calendar_select(harness->page, 3, 1080, 1140, FALSE);
    block_action(harness, 3, 1080, "automation-calendar-paste"); wait_saved(harness);
    snapshot = snapshot_of(harness->page);
    g_assert_cmpuint(json_array_get_length(events(snapshot)), ==, 5);
    first = event_at(snapshot, "2026-10-05", 1080);
    JsonObject *copied = event_at(snapshot, "2026-10-08", 1080);
    g_assert_cmpstr(radios_json_string(first, "id", ""), !=, radios_json_string(copied, "id", ""));
    g_assert_cmpstr(radios_json_string(first, "title", ""), ==, radios_json_string(copied, "title", ""));
    JsonArray *original_rotations = json_object_get_array_member(first, "rotations"),
              *copied_rotations = json_object_get_array_member(copied, "rotations");
    g_assert_cmpuint(json_array_get_length(copied_rotations), ==, 2);
    for (guint i = 0; i < 2; ++i)
        g_assert_cmpstr(radios_json_string(json_array_get_object_element(original_rotations, i), "id", ""), !=,
                        radios_json_string(json_array_get_object_element(copied_rotations, i), "id", ""));
    assert_rotation(json_array_get_object_element(copied_rotations, 0), second_playlist, 7, TRUE, FALSE, TRUE, FALSE);
    assert_rotation(json_array_get_object_element(copied_rotations, 1), first_playlist, 3, FALSE, TRUE, FALSE, TRUE);
    g_assert_cmpint(radios_json_int(event_at(snapshot, "2026-10-08", 1170), "end_minute", 0), ==, 1320);
    g_assert_cmpstr(radios_json_string(event_at(snapshot, "2026-10-05", 1170), "color", ""), ==, "#abfe33");
    g_assert_cmpstr(radios_json_string(event_at(snapshot, "2026-10-08", 1170), "color", ""), ==, "#abfe33");
    g_assert_cmpint(radios_json_int(event_at(snapshot, "2026-10-05", 1380), "end_minute", 0), ==, 1440);
    json_node_free(snapshot);

    /* The first of two copied blocks conflicts, while the second is free.
     * A rejected paste must leave both days intact, without a partial copy. */
    new_event(harness, 1, 1110, 1170, "Kopyalamayla çakışan hedef", FALSE);
    radios_automation_calendar_select(harness->page, 0, 1080, 1140, FALSE);
    radios_automation_calendar_select(harness->page, 0, 1170, 1320, TRUE);
    block_action(harness, 0, 1080, "automation-calendar-copy");
    radios_automation_calendar_select(harness->page, 1, 1080, 1140, FALSE);
    unchanged = snapshot_of(harness->page);
    block_action(harness, 1, 1080, "automation-calendar-paste");
    JsonNode *after_conflict = snapshot_of(harness->page);
    g_assert_true(json_node_equal(unchanged, after_conflict));
    g_assert_cmpuint(json_array_get_length(events(after_conflict)), ==, 6);
    json_node_free(unchanged); json_node_free(after_conflict); wait_saved(harness);

    /* Editing the copied program must leave its source and copied row order
     * independent; this catches shallow copies hidden by persistence alone. */
    radios_automation_calendar_select(harness->page, 3, 1080, 1140, FALSE);
    select_rotation(harness, 0); row_action(harness, 0, "automation-calendar-row-down"); wait_saved(harness);
    select_rotation(harness, 0); row_action(harness, 0, "automation-calendar-row-remove"); wait_saved(harness);
    snapshot = snapshot_of(harness->page); copied = event_at(snapshot, "2026-10-08", 1080);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(copied, "rotations")), ==, 1); json_node_free(snapshot);
    gtk_entry_set_text(GTK_ENTRY(control(harness->page, "automation-calendar-programme-name")), "Kopya program bilgisi");
    wait_saved(harness);
    snapshot = snapshot_of(harness->page); copied = event_at(snapshot, "2026-10-08", 1080);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(copied, "rotations")), ==, 1); json_node_free(snapshot);
    gtk_entry_set_text(GTK_ENTRY(control(harness->page, "automation-calendar-programme-name")), "Kopya program düzenlendi");
    wait_saved(harness);
    snapshot = snapshot_of(harness->page);
    first = event_at(snapshot, "2026-10-05", 1080); copied = event_at(snapshot, "2026-10-08", 1080);
    g_assert_cmpstr(radios_json_string(first, "title", ""), ==, "Akşam programı");
    g_assert_cmpstr(radios_json_string(copied, "title", ""), ==, "Kopya program düzenlendi");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(first, "rotations")), ==, 2);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(copied, "rotations")), ==, 1);
    assert_rotation(json_array_get_object_element(json_object_get_array_member(copied, "rotations"), 0),
                    second_playlist, 7, TRUE, FALSE, TRUE, FALSE);
    capture(harness); harness_close(harness);

    harness = harness_open();
    JsonNode *restored = snapshot_of(harness->page);
    g_assert_true(json_node_equal(snapshot, restored));
    json_node_free(restored); json_node_free(snapshot); harness_close(harness);
    g_free(first_playlist); g_free(second_playlist);
}

typedef struct {JsonNode *result; GError *error; gboolean done;} MediaImport;
static void media_imported(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result,
                           const GError *error, gpointer value)
{
    (void)ctx; (void)owner; MediaImport *import = value;
    import->result = result ? json_node_copy(result) : NULL;
    import->error = error ? g_error_copy(error) : NULL; import->done = TRUE;
}
static gboolean media_done(gpointer value) { return ((MediaImport *)value)->done; }
static void wav_u16(guchar *bytes, guint16 value)
{ value = GUINT16_TO_LE(value); memcpy(bytes, &value, sizeof value); }
static void wav_u32(guchar *bytes, guint32 value)
{ value = GUINT32_TO_LE(value); memcpy(bytes, &value, sizeof value); }

static JsonNode *verified_media(Harness *harness)
{
    guint frames = 48000 * 2, size = frames * 4; guchar *wav = g_malloc(44 + size);
    memcpy(wav, "RIFF", 4); wav_u32(wav + 4, 36 + size); memcpy(wav + 8, "WAVEfmt ", 8);
    wav_u32(wav + 16, 16); wav_u16(wav + 20, 1); wav_u16(wav + 22, 2);
    wav_u32(wav + 24, 48000); wav_u32(wav + 28, 48000 * 4); wav_u16(wav + 32, 4); wav_u16(wav + 34, 16);
    memcpy(wav + 36, "data", 4); wav_u32(wav + 40, size);
    for (guint frame = 0; frame < frames; ++frame) {
        gint16 sample = (gint16)lrint(sin(2 * G_PI * 440 * frame / 48000) * 0.2 * 32767);
        wav_u16(wav + 44 + frame * 4, (guint16)sample); wav_u16(wav + 46 + frame * 4, (guint16)sample);
    }
    gchar *path = g_build_filename(config.project_root, "pdf13-verified-preparation.wav", NULL);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + size, NULL)); g_free(wav);
    MediaImport import = {0};
    GCancellable *handle = radios_media_import_async(&harness->ctx, harness->page, path,
        "music", "PDF13 Doğrulanmış PCM", media_imported, &import, NULL);
    g_assert_nonnull(handle); wait_until(media_done, &import, "real PCM staging, analysis, SHA and import");
    g_assert_no_error(import.error); g_assert_nonnull(import.result);
    JsonObject *media = json_object_get_object_member(json_node_get_object(import.result), "media");
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "media_id", radios_json_string(media, "_id", ""));
    JsonNode *detail = request("/v1/workflows/desktop-media/get", body); json_node_free(body);
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    g_assert_true(json_object_get_boolean_member(version, "verified"));
    g_assert_true(json_object_get_boolean_member(version, "object_available"));
    g_assert_cmpint(radios_json_int(version, "duration_ms", 0), ==, 2000);
    g_assert_true(g_file_test(radios_json_string(version, "managed_path", ""), G_FILE_TEST_IS_REGULAR));
    json_node_free(import.result); g_object_unref(handle); g_assert_cmpint(g_unlink(path), ==, 0); g_free(path);
    return detail;
}

static void bind_playlist_with_title(const gchar *playlist, JsonNode *detail, const gchar *title)
{
    JsonNode *body = body_new(); JsonNode *result = request("/v1/workflows/desktop-playlists/list", body); json_node_free(body);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(result), "playlists"); gint64 revision = 0;
    for (guint i = 0; i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (!g_strcmp0(radios_json_string(row, "_id", ""), playlist)) revision = radios_json_int(row, "_rev", 0);
    }
    g_assert_cmpint(revision, >, 0); json_node_free(result);
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media"),
               *version = json_object_get_object_member(json_node_get_object(detail), "version");
    body = body_new(); JsonObject *object = json_node_get_object(body), *metadata = json_object_new();
    json_object_set_string_member(object, "playlist_id", playlist); json_object_set_int_member(object, "expected_revision", revision);
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    const gchar *fields[] = {"title", "artist", "album", "genre"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i)
        json_object_set_string_member(metadata, fields[i], radios_json_string(media, fields[i], ""));
    if (title) json_object_set_string_member(metadata, "title", title);
    json_object_set_int_member(metadata, "duration_ms", radios_json_int(version, "duration_ms", 0));
    json_object_set_double_member(metadata, "bpm", 120); json_object_set_string_member(metadata, "key", "");
    json_object_set_string_member(metadata, "location", "/not-a-trusted-preview-path.wav");
    json_object_set_string_member(metadata, "comment", "Genuine PDF13 fixture binding");
    json_object_set_string_member(metadata, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_string_member(metadata, "version_id", radios_json_string(version, "_id", ""));
    json_object_set_string_member(metadata, "media_file_id", radios_json_string(version, "media_file_id", ""));
    json_object_set_string_member(metadata, "media_sha256", radios_json_string(version, "sha256", ""));
    json_object_set_object_member(object, "metadata", metadata);
    result = request("/v1/workflows/desktop-playlists/entry-save", body); json_node_free(result); json_node_free(body);
}

static void bind_playlist(const gchar *playlist, JsonNode *detail)
{ bind_playlist_with_title(playlist, detail, NULL); }

static GtkWidget *find_tree(GtkWidget *widget);
static gboolean preview_open(gpointer value);

static void assert_visible_song_rows(GtkWidget *dialog)
{
    GtkWidget *songs = control(dialog, "rotation-editor-songs");
    g_assert_true(GTK_IS_TREE_VIEW(songs)); g_assert_true(gtk_widget_get_mapped(songs));
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(songs));
    gint count = gtk_tree_model_iter_n_children(model, NULL); g_assert_cmpint(count, >, 0);
    settle_layout();
    GdkWindow *bin = gtk_tree_view_get_bin_window(GTK_TREE_VIEW(songs));
    g_assert_nonnull(bin); g_assert_true(gdk_window_is_viewable(bin));
    gint bin_x, bin_y, dialog_x, dialog_y;
    gdk_window_get_origin(bin, &bin_x, &bin_y);
    gdk_window_get_origin(gtk_widget_get_window(dialog), &dialog_x, &dialog_y);
    Harness modal = {0}; modal.window = dialog;
    GtkAllocation allocated; gtk_widget_get_allocation(dialog, &allocated);
    for (gint index = 0; index < MIN(count, 2); ++index) {
        GtkTreePath *path = gtk_tree_path_new_from_indices(index, -1);
        GtkTreeViewColumn *title_column = gtk_tree_view_get_column(GTK_TREE_VIEW(songs), 1);
        g_assert_nonnull(title_column); g_assert_cmpstr(gtk_tree_view_column_get_title(title_column), ==, "Şarkı");
        GdkRectangle row; gtk_tree_view_get_cell_area(GTK_TREE_VIEW(songs), path, title_column, &row);
        gtk_tree_path_free(path); row.x += bin_x - dialog_x; row.y += bin_y - dialog_y;
        g_assert_cmpint(row.height, >, 0); g_assert_cmpint(row.y, >=, 0);
        g_assert_cmpint(row.y + row.height, <=, allocated.height);
        for (GtkWidget *parent = gtk_widget_get_parent(songs); parent && parent != dialog; parent = gtk_widget_get_parent(parent)) {
            if (!GTK_IS_SCROLLED_WINDOW(parent) && !GTK_IS_VIEWPORT(parent)) continue;
            GdkRectangle viewport = window_rect(&modal, parent), intersection;
            g_assert_true(gdk_rectangle_intersect(&row, &viewport, &intersection));
            g_assert_cmpint(intersection.height, ==, row.height);
            g_assert_cmpint(intersection.width, >, 0);
        }
        g_test_message("Actual visible reusable song row %d: y=%d height=%d in dialog %dx%d", index, row.y, row.height, allocated.width, allocated.height);
    }
}

static JsonObject *named_definition(JsonNode *calendar, const gchar *id)
{
    JsonArray *library = json_object_get_array_member(json_node_get_object(calendar), "rotation_library");
    g_assert_nonnull(library);
    for (guint i = 0; i < json_array_get_length(library); ++i) {
        JsonObject *definition = json_array_get_object_element(library, i);
        if (!g_strcmp0(radios_json_string(definition, "id", ""), id)) return definition;
    }
    g_error("Expected reusable rotation %s", id); return NULL;
}

static GtkWidget *reusable_dialog(void)
{
    GtkWidget *dialog = dialog_find("automation-calendar-library-dialog");
    g_assert_true(GTK_IS_DIALOG(dialog));
    g_assert_true(gtk_widget_get_mapped(dialog));
    g_assert_true(gtk_widget_get_mapped(control(dialog, "rotation-editor")));
    return dialog;
}

/* Advanced controls live behind sections and notebook pages. Open the
 * containing section and page before dispatching edits. */
static void reusable_reveal(GtkWidget *widget)
{
    for (GtkWidget *parent = widget; parent; parent = gtk_widget_get_parent(parent)) {
        if (strcmp(gtk_widget_get_name(parent), "rotation-editor-source-properties")) continue;
        GtkWidget *dialog = gtk_widget_get_toplevel(widget), *sources = control(dialog, "rotation-editor-sources");
        GList *paths = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(sources)), NULL);
        g_assert_nonnull(paths); g_signal_emit_by_name(sources, "row-activated", paths->data, NULL);
        g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free); settle_layout(); break;
    }

    for (GtkWidget *child = widget, *parent = gtk_widget_get_parent(child); parent;
         child = parent, parent = gtk_widget_get_parent(parent)) {
        if (GTK_IS_EXPANDER(parent)) { gtk_expander_set_expanded(GTK_EXPANDER(parent), TRUE); settle_layout(); }
        if (GTK_IS_STACK(parent)) { gtk_stack_set_visible_child(GTK_STACK(parent), child); settle_layout(); }
        if (!GTK_IS_NOTEBOOK(parent)) continue;
        gint page = gtk_notebook_page_num(GTK_NOTEBOOK(parent), child);
        g_assert_cmpint(page, >=, 0);
        gtk_notebook_set_current_page(GTK_NOTEBOOK(parent), page); settle_layout();
    }
}

static void reusable_assert_accessible(GtkWidget *widget)
{
    if (gtk_widget_get_mapped(widget)) return;
    gboolean legacy = !strcmp(gtk_widget_get_name(widget), "rotation-editor-selection-mode");
    for (GtkWidget *parent = gtk_widget_get_parent(widget); parent; parent = gtk_widget_get_parent(parent))
        legacy |= !strcmp(gtk_widget_get_name(parent), "rotation-editor-settings");
    /* These preserved backing fields exercise old saved rules. New PDF user
     * surface visibility has separate genuine probability-editor acceptance. */
    g_test_message("Backing field visibility check: %s", gtk_widget_get_name(widget));
    g_assert_true(legacy); g_test_message("Legacy backing field preserved outside PDF main surface: %s", gtk_widget_get_name(widget));
}
static void reusable_spin(GtkWidget *dialog, const gchar *name, gint value)
{
    GtkWidget *widget = control(dialog, name);
    g_assert_true(GTK_IS_SPIN_BUTTON(widget));
    reusable_reveal(widget);
    reusable_assert_accessible(widget); g_assert_true(gtk_widget_is_sensitive(widget));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), value); pump();
}

static void reusable_combo(GtkWidget *dialog, const gchar *name, const gchar *id)
{
    GtkWidget *widget = control(dialog, name);
    g_assert_true(GTK_IS_COMBO_BOX(widget));
    reusable_reveal(widget);
    reusable_assert_accessible(widget); g_assert_true(gtk_widget_is_sensitive(widget));
    if (!strcmp(name, "rotation-editor-selection-mode")) {
        g_assert_true(!strcmp(id, "linear") || !strcmp(id, "weighted"));
        gint index = !strcmp(id, "weighted") ? 1 : 0;
        gtk_combo_box_set_active(GTK_COMBO_BOX(widget), index);
        g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(widget)), ==, index);
    } else if (!strcmp(name, "rotation-editor-source-track-order")) {
        g_assert_true(!strcmp(id, "linear") || !strcmp(id, "random") || !strcmp(id, "weighted"));
        gint index = !strcmp(id, "weighted") ? 2 : !strcmp(id, "random") ? 1 : 0;
        gtk_combo_box_set_active(GTK_COMBO_BOX(widget), index);
        g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(widget)), ==, index);
    } else g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), id));
    pump();
}

static void reusable_check(GtkWidget *dialog, const gchar *name, gboolean active)
{
    GtkWidget *widget = control(dialog, name);
    g_assert_true(GTK_IS_TOGGLE_BUTTON(widget));
    reusable_reveal(widget);
    reusable_assert_accessible(widget); g_assert_true(gtk_widget_is_sensitive(widget));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), active); pump();
}

static void reusable_select_source(GtkWidget *dialog, guint index)
{
    GtkWidget *tree = control(dialog, "rotation-editor-sources");
    if (!gtk_widget_get_mapped(tree)) { click(dialog, "rotation-editor-back"); settle_layout(); }
    g_assert_true(GTK_IS_TREE_VIEW(tree)); g_assert_true(gtk_widget_get_mapped(tree));
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)index, -1);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(tree), path, NULL, FALSE);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), path);
    gtk_tree_path_free(path); pump();
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(control(dialog, "rotation-editor-pages"))), ==, "library");
}

static void assert_named_settings(JsonObject *settings, const gchar *selection,
                                  gint track, gint artist, gint album,
                                  gint eof, gint crossfade, gint cue, gint cross_overlap, gint cue_overlap)
{
    if (selection) g_assert_cmpstr(radios_json_string(settings, "selection_mode", ""), ==, selection);
    g_assert_cmpint(radios_json_int(settings, "track_gap_minutes", -1), ==, track);
    g_assert_cmpint(radios_json_int(settings, "artist_gap_minutes", -1), ==, artist);
    g_assert_cmpint(radios_json_int(settings, "album_gap_minutes", -1), ==, album);
    /* eof < 0: a playlist override carries repeat limits only. */
    if (eof < 0) { g_assert_false(json_object_has_member(settings, "transitions")); return; }
    JsonArray *transitions = json_object_get_array_member(settings, "transitions");
    g_assert_nonnull(transitions); g_assert_cmpuint(json_array_get_length(transitions), ==, 3);
    const gint weights[] = {eof, crossfade, cue}, overlaps[] = {0, cross_overlap, cue_overlap};
    for (guint i = 0; i < 3; ++i) {
        JsonObject *transition = json_array_get_object_element(transitions, i);
        g_assert_cmpint(radios_json_int(transition, "mode", -1), ==, i);
        g_assert_cmpint(radios_json_int(transition, "weight", -1), ==, weights[i]);
        g_assert_cmpint(radios_json_int(transition, "overlap_ms", -1), ==, overlaps[i]);
    }
}

static void assert_reusable_source(JsonObject *source, const gchar *playlist)
{
    assert_rotation(source, playlist, 3, FALSE, TRUE, FALSE, TRUE);
    g_assert_cmpstr(radios_json_string(source, "media_kind", ""), ==, "music");
    g_assert_cmpint(radios_json_int(source, "count", -1), ==, 2);
    g_assert_cmpint(radios_json_int(source, "weight", -1), ==, 70);
    g_assert_cmpint(radios_json_int(source, "start_minute", -1), ==, 495);
    g_assert_cmpint(radios_json_int(source, "end_minute", -1), ==, 1290);
    g_assert_cmpint(radios_json_int(source, "interval_minutes", -1), ==, 45);
    assert_named_settings(json_object_get_object_member(source, "settings"), NULL, 7, 11, 13, -1, 0, 0, 0, 0);
}

static void reusable_source_song_click(GtkWidget *dialog, guint index)
{
    GtkWidget *tree = control(dialog, "rotation-editor-sources");
    if (!gtk_widget_get_mapped(tree)) reusable_select_source(dialog, index);
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)index, -1);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(tree), path, NULL, FALSE, 0, 0); settle_layout();
    GdkRectangle rectangle; gtk_tree_view_get_cell_area(GTK_TREE_VIEW(tree), path, NULL, &rectangle);
    gtk_tree_path_free(path);
    gdouble x = MAX(12, rectangle.x + 4), y = rectangle.y + rectangle.height / 2.0;
    pointer_button(tree, GDK_BUTTON_PRESS, x, y, 0, 1);
    /* The native release signal propagates FALSE after opening songs; its
     * visible page change is the acceptance condition, not event consumption. */
    GdkWindow *window = gtk_tree_view_get_bin_window(GTK_TREE_VIEW(tree));
    GdkEvent *release = gdk_event_new(GDK_BUTTON_RELEASE);
    release->button.window = g_object_ref(window); release->button.send_event = TRUE;
    release->button.button = 1; release->button.x = x; release->button.y = y;
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(tree));
    gdk_event_set_device(release, gdk_seat_get_pointer(seat));
    gint root_x, root_y; gdk_window_get_origin(window, &root_x, &root_y);
    release->button.x_root = root_x + x; release->button.y_root = root_y + y;
    gtk_widget_event(tree, release); gdk_event_free(release); pump();
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(control(dialog, "rotation-editor-pages"))), ==, "songs");
    assert_visible_song_rows(dialog);
}

typedef struct {
    RadioSDataClient *previous;
    gboolean observed;
} ReconnectProbe;

/* Observe the real public refresh boundary after re-authentication. This
 * neither replaces requests nor supplies cached results to the editor. */
static void reusable_reconnect_refreshed(RadioSViewContext *ctx, GtkWidget *dialog)
{
    ReconnectProbe *probe = g_object_get_data(G_OBJECT(dialog), "native-reconnect-probe");
    RadioSDataClient *current = radios_data_client_acquire(ctx);
    if (!current || current == probe->previous) {
        if (current) radios_data_client_unref(current);
        return;
    }
    radios_data_client_unref(current);
    if (probe->observed) return;
    GtkWidget *combo = control(dialog, "rotation-editor-playlist-combo");
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(combo)), NULL), ==, 0);
    GtkWidget *sources = control(dialog, "rotation-editor-sources"), *start = control(dialog, "rotation-editor-source-start");
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(sources)), NULL), ==, 2);
    GtkTreeModel *model = NULL; GtkTreeIter selected;
    g_assert_true(gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(sources)), &model, &selected));
    GtkTreePath *path = gtk_tree_model_get_path(model, &selected);
    g_assert_cmpint(gtk_tree_path_get_indices(path)[0], ==, 0); gtk_tree_path_free(path);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(start)), ==, "0");
    g_assert_cmpint(gtk_editable_get_position(GTK_EDITABLE(start)), ==, 1);
    g_assert_false(radios_rotation_editor_ready(control(dialog, "rotation-editor")));
    probe->observed = TRUE;
    g_test_message("Re-authenticated actual client boundary: old catalog unavailable; source selection and invalid draft preserved");
}

static gboolean reusable_reconnect_observed(gpointer value)
{ return ((ReconnectProbe *)value)->observed; }

static void test_native_reusable_rotation_library_songs_settings(void)
{
    const gchar *rotation_name = "Bağımsız gece rotasyonu";
    gchar *playlist = create_playlist("Yeniden kullanılan iki şarkı"), *other = create_playlist("İkinci playlist ağırlığı");
    Harness *harness = harness_open(); set_date(harness, "2026-12-07");
    JsonNode *initial = snapshot_of(harness->page);
    g_assert_cmpuint(json_array_get_length(events(initial)), ==, 0);
    JsonNode *detail = verified_media(harness);
    bind_playlist_with_title(playlist, detail, "Reusable song A");
    bind_playlist_with_title(playlist, detail, "Reusable song B");
    bind_playlist_with_title(other, detail, "Reusable song C");
    radios_data_refresh(&harness->ctx);
    click(harness->page, "automation-calendar-independent-rotation");
    GtkWidget *dialog = reusable_dialog();
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-name")), rotation_name); pump();
    JsonNode *snapshot = snapshot_of(harness->page);
    JsonArray *library = json_object_get_array_member(json_node_get_object(snapshot), "rotation_library");
    g_assert_cmpuint(json_array_get_length(library), ==, 1);
    JsonObject *definition = json_array_get_object_element(library, 0);
    gchar *rotation_id = g_strdup(radios_json_string(definition, "id", ""));
    g_assert_true(g_uuid_string_is_valid(rotation_id));
    g_assert_cmpstr(radios_json_string(definition, "name", ""), ==, rotation_name);
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(initial), "events"),
                                 json_object_get_member(json_node_get_object(snapshot), "events")));
    json_node_free(snapshot);
    GtkWidget *settings = control(dialog, "rotation-editor-settings"); g_assert_true(GTK_IS_EXPANDER(settings));
    gtk_expander_set_expanded(GTK_EXPANDER(settings), TRUE); settle_layout();
    reusable_combo(dialog, "rotation-editor-selection-mode", "weighted");
    /* Advanced rules hold repeat limits; stored transition choices keep their defaults. */
    const gchar *global_names[] = {"track-gap", "artist-gap", "album-gap"};
    const gint global_values[] = {5, 9, 12};
    for (guint i = 0; i < G_N_ELEMENTS(global_names); ++i) {
        gchar *name = g_strconcat("rotation-editor-", global_names[i], NULL);
        reusable_spin(dialog, name, global_values[i]); g_free(name);
    }
    PlaylistChoice choice = {control(dialog, "rotation-editor-playlist-combo"), playlist};
    wait_until(playlist_available, &choice, "authenticated cached first playlist in reusable editor");
    click(dialog, "rotation-editor-add");
    choice.id = other; wait_until(playlist_available, &choice, "authenticated cached second reusable playlist");
    click(dialog, "rotation-editor-add");
    click(dialog, "rotation-editor-back");
    GtkWidget *sources = control(dialog, "rotation-editor-sources");
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(sources)), NULL), ==, 2);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(control(dialog, "rotation-editor-total"))), ==, "2 yayın adımı · 3 kaynak şarkı");
    reusable_select_source(dialog, 1); reusable_source_song_click(dialog, 1);
    reusable_spin(dialog, "rotation-editor-source-weight", 30);
    click(dialog, "rotation-editor-back");
    reusable_select_source(dialog, 0); reusable_source_song_click(dialog, 0);
    reusable_spin(dialog, "rotation-editor-source-frequency", 3);
    reusable_spin(dialog, "rotation-editor-source-count", 2);
    reusable_spin(dialog, "rotation-editor-source-weight", 70);
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-source-start")), "08:15");
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-source-end")), "21:30");
    reusable_spin(dialog, "rotation-editor-source-interval", 45);
    reusable_combo(dialog, "rotation-editor-source-track-order", "weighted");
    reusable_combo(dialog, "rotation-editor-source-media-kind", "music");
    reusable_check(dialog, "rotation-editor-source-advert-random", FALSE);
    reusable_check(dialog, "rotation-editor-source-sweeper-random", TRUE);
    GtkWidget *source_settings = control(dialog, "rotation-editor-source-settings");
    g_assert_true(GTK_IS_EXPANDER(source_settings));
    gtk_expander_set_expanded(GTK_EXPANDER(source_settings), TRUE); settle_layout();
    reusable_check(dialog, "rotation-editor-source-override", TRUE);
    const gint source_values[] = {7, 11, 13};
    for (guint i = 0; i < G_N_ELEMENTS(global_names); ++i) {
        gchar *name = g_strconcat("rotation-editor-source-", global_names[i], NULL);
        reusable_spin(dialog, name, source_values[i]); g_free(name);
    }
    /* A pending valid save ACK/playlist refresh must not replace this invalid
     * focused draft. The parent persists its last complete valid definition. */
    GtkWidget *start = control(dialog, "rotation-editor-source-start");
    gtk_widget_grab_focus(start); gtk_entry_set_text(GTK_ENTRY(start), "0"); gtk_editable_set_position(GTK_EDITABLE(start), 1);
    pump(); g_assert_false(radios_rotation_editor_ready(control(dialog, "rotation-editor")));
    g_assert_null(radios_rotation_editor_snapshot(control(dialog, "rotation-editor")));
    wait_saved(harness);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(start)), ==, "0");
    g_assert_cmpint(gtk_editable_get_position(GTK_EDITABLE(start)), ==, 1);
    gtk_entry_set_text(GTK_ENTRY(start), "08:15"); pump(); wait_saved(harness);
    Harness modal = {0}; modal.window = dialog;
    capture_to(&modal, g_getenv("RADIOS_ROTATION_SETTINGS_CAPTURE"));
    GtkWidget *child_scroll = control(dialog, "rotation-editor-scroll");
    GtkWidget *child_viewport = gtk_bin_get_child(GTK_BIN(child_scroll));
    g_assert_true(GTK_IS_VIEWPORT(child_viewport));
    GtkAdjustment *child_adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(child_scroll));
    for (guint i = 0; i < G_N_ELEMENTS(global_names); ++i) {
        gchar *name = g_strconcat("rotation-editor-source-", global_names[i], NULL);
        GtkWidget *widget = control(dialog, name);
        reusable_reveal(widget); gtk_widget_grab_focus(widget); settle_layout();
        gint x, y;
        g_assert_true(gtk_widget_translate_coordinates(widget, child_viewport, 0, 0, &x, &y));
        gdouble top = y + gtk_adjustment_get_value(child_adjustment);
        gtk_adjustment_clamp_page(child_adjustment, MAX(0, top - 6), top + gtk_widget_get_allocated_height(widget) + 6);
        settle_layout();
        g_assert_true(gtk_widget_get_mapped(widget));
        GdkRectangle field = window_rect(&modal, widget), viewport = window_rect(&modal, child_viewport);
        g_assert_cmpint(field.x, >=, viewport.x); g_assert_cmpint(field.y, >=, viewport.y);
        g_assert_cmpint(field.x + field.width, <=, viewport.x + viewport.width);
        g_assert_cmpint(field.y + field.height, <=, viewport.y + viewport.height);
        g_test_message("Advanced field %s is reachable in its tab and scroll viewport: %d,%d %dx%d", name, field.x, field.y, field.width, field.height);
        g_free(name);
    }
    capture_to(&modal, g_getenv("RADIOS_ROTATION_CHILD_SETTINGS_CAPTURE"));
    click(dialog, "rotation-editor-back");
    reusable_source_song_click(dialog, 0);
    GtkWidget *songs = control(dialog, "rotation-editor-songs"); g_assert_true(gtk_widget_get_mapped(songs));
    GtkTreeModel *song_model = gtk_tree_view_get_model(GTK_TREE_VIEW(songs));
    g_assert_cmpint(gtk_tree_model_iter_n_children(song_model, NULL), ==, 2);
    const gchar *titles[] = {"Reusable song A", "Reusable song B"};
    GtkTreeIter song; g_assert_true(gtk_tree_model_get_iter_first(song_model, &song));
    for (guint i = 0; i < G_N_ELEMENTS(titles); ++i) {
        gchar *entry_id = NULL, *title = NULL;
        gtk_tree_model_get(song_model, &song, 0, &entry_id, 2, &title, -1);
        g_assert_true(g_uuid_string_is_valid(entry_id)); g_assert_cmpstr(title, ==, titles[i]);
        g_free(entry_id); g_free(title);
        if (i + 1 < G_N_ELEMENTS(titles)) g_assert_true(gtk_tree_model_iter_next(song_model, &song));
    }
    capture_to(&modal, g_getenv("RADIOS_ROTATION_SONGS_CAPTURE"));
    click(dialog, "rotation-editor-back");
    reusable_source_song_click(dialog, 0); /* same selected source opens again */
    click(dialog, "rotation-editor-back");
    capture_to(&modal, g_getenv("RADIOS_ROTATION_CAPTURE"));
    snapshot = snapshot_of(harness->page); definition = named_definition(snapshot, rotation_id);
    g_assert_cmpstr(radios_json_string(definition, "name", ""), ==, rotation_name);
    assert_named_settings(json_object_get_object_member(definition, "settings"), "weighted", 5, 9, 12, 100, 0, 0, 3000, 3000);
    JsonArray *source_rows = json_object_get_array_member(definition, "sources");
    g_assert_cmpuint(json_array_get_length(source_rows), ==, 2);
    assert_reusable_source(json_array_get_object_element(source_rows, 0), playlist);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(source_rows, 1), "playlist_id", ""), ==, other);
    g_assert_cmpint(radios_json_int(json_array_get_object_element(source_rows, 1), "weight", -1), ==, 30);
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(initial), "events"),
                                 json_object_get_member(json_node_get_object(snapshot), "events")));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump(); wait_saved(harness);
    harness_close(harness); harness = harness_open();
    JsonNode *restored = snapshot_of(harness->page); g_assert_true(json_node_equal(snapshot, restored)); json_node_free(restored);
    PlaylistChoice saved_choice = {control(harness->page, "automation-calendar-existing-rotation"), rotation_id};
    wait_until(playlist_available, &saved_choice, "saved reusable definition visible without selected calendar block");
    g_assert_false(gtk_widget_is_sensitive(control(harness->page, "automation-calendar-assign-rotation")));
    click(harness->page, "automation-calendar-manage-rotation"); dialog = reusable_dialog();
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(dialog, "rotation-editor-name"))), ==, rotation_name);
    gtk_expander_set_expanded(GTK_EXPANDER(control(dialog, "rotation-editor-settings")), TRUE); settle_layout();
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(control(dialog, "rotation-editor-selection-mode"))), ==, 1);
    reusable_select_source(dialog, 0); reusable_source_song_click(dialog, 0);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(dialog, "rotation-editor-source-start"))), ==, "08:15");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(dialog, "rotation-editor-source-end"))), ==, "21:30");
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control(dialog, "rotation-editor-source-interval"))), ==, 45);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control(dialog, "rotation-editor-source-override"))));
    JsonNode *editor_snapshot = radios_rotation_editor_snapshot(control(dialog, "rotation-editor")); g_assert_nonnull(editor_snapshot);
    g_assert_true(json_node_equal(editor_snapshot, json_array_get_element(json_object_get_array_member(json_node_get_object(snapshot), "rotation_library"), 0)));
    json_node_free(editor_snapshot);
    JsonNode *reconnect_before = snapshot_of(harness->page), *reconnect_record = calendar_record();
    gint64 reconnect_revision = radios_json_int(json_node_get_object(reconnect_record), "_rev", 0); json_node_free(reconnect_record);
    ReconnectProbe reconnect = {radios_data_client_acquire(&harness->ctx), FALSE}; g_assert_nonnull(reconnect.previous);
    GtkWidget *reconnect_start = control(dialog, "rotation-editor-source-start");
    gtk_widget_grab_focus(reconnect_start); gtk_entry_set_text(GTK_ENTRY(reconnect_start), "0");
    gtk_editable_set_position(GTK_EDITABLE(reconnect_start), 1); pump();
    g_object_set_data(G_OBJECT(dialog), "native-reconnect-probe", &reconnect);
    radios_data_register_refresh(&harness->ctx, dialog, reusable_reconnect_refreshed);
    radios_data_refresh(&harness->ctx); radios_data_connect(&harness->ctx);
    g_assert_false(radios_data_connected(&harness->ctx));
    g_assert_null(radios_data_client_acquire(&harness->ctx));
    g_assert_false(radios_rotation_editor_ready(control(dialog, "rotation-editor")));
    wait_until(reusable_reconnect_observed, &reconnect, "real reconnect clears stale catalog at the new client refresh boundary");
    wait_until(harness_ready, harness, "re-authenticated genuine Calendar document ready");
    RadioSDataClient *replacement = radios_data_client_acquire(&harness->ctx);
    g_assert_nonnull(replacement); g_assert_true(replacement != reconnect.previous);
    g_assert_true(radios_data_client_same_identity(replacement, reconnect.previous)); radios_data_client_unref(replacement);
    PlaylistChoice reconnect_choice = {control(dialog, "rotation-editor-playlist-combo"), playlist};
    wait_until(playlist_available, &reconnect_choice, "new active client authenticated catalog reaches actual editor");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(reconnect_start)), ==, "0");
    g_assert_cmpint(gtk_editable_get_position(GTK_EDITABLE(reconnect_start)), ==, 1);
    /* An unfinished draft is never saved by reconnect. A new client for the
     * same authenticated store/user/station may continue this editor. */
    reconnect_record = calendar_record();
    g_assert_cmpint(radios_json_int(json_node_get_object(reconnect_record), "_rev", 0), ==, reconnect_revision);
    g_assert_true(json_node_equal(reconnect_before, json_object_get_member(json_node_get_object(reconnect_record), "config")));
    json_node_free(reconnect_record);
    gtk_entry_set_text(GTK_ENTRY(reconnect_start), "08:15"); pump();
    g_assert_true(radios_rotation_editor_ready(control(dialog, "rotation-editor")));
    editor_snapshot = radios_rotation_editor_snapshot(control(dialog, "rotation-editor"));
    g_assert_nonnull(editor_snapshot); json_node_free(editor_snapshot); wait_saved(harness);
    restored = snapshot_of(harness->page); g_assert_true(json_node_equal(reconnect_before, restored)); json_node_free(restored);
    reconnect_record = calendar_record();
    g_assert_cmpint(radios_json_int(json_node_get_object(reconnect_record), "_rev", 0), >=, reconnect_revision);
    g_assert_true(json_node_equal(reconnect_before, json_object_get_member(json_node_get_object(reconnect_record), "config")));
    json_node_free(reconnect_record); json_node_free(reconnect_before); radios_data_client_unref(reconnect.previous);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump();
    click(harness->page, "automation-calendar-manage-rotation"); dialog = reusable_dialog();
    reconnect_choice.combo = control(dialog, "rotation-editor-playlist-combo");
    wait_until(playlist_available, &reconnect_choice, "fresh editor uses only the replacement client catalog");
    reusable_select_source(dialog, 0); reusable_source_song_click(dialog, 0);
    g_assert_true(radios_rotation_editor_ready(control(dialog, "rotation-editor")));
    editor_snapshot = radios_rotation_editor_snapshot(control(dialog, "rotation-editor")); g_assert_nonnull(editor_snapshot);
    g_assert_true(json_node_equal(editor_snapshot, json_array_get_element(json_object_get_array_member(json_node_get_object(snapshot), "rotation_library"), 0)));
    json_node_free(editor_snapshot);
    gtk_expander_set_expanded(GTK_EXPANDER(control(dialog, "rotation-editor-settings")), TRUE); settle_layout();
    reusable_combo(dialog, "rotation-editor-selection-mode", "linear"); wait_saved(harness);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump();
    set_date(harness, "2026-12-07");
    new_event(harness, 0, 600, 660, "İlk tekrar kullanılabilir blok", TRUE);
    select_library_rotation(harness->page, rotation_id);
    click(harness->page, "automation-calendar-assign-rotation"); wait_saved(harness);
    new_event(harness, 1, 660, 720, "İkinci tekrar kullanılabilir blok", FALSE);
    select_library_rotation(harness->page, rotation_id);
    click(harness->page, "automation-calendar-assign-rotation"); wait_saved(harness);
    restored = snapshot_of(harness->page);
    JsonArray *first_rows = json_object_get_array_member(event_at(restored, "2026-12-07", 600), "rotations");
    JsonArray *second_rows = json_object_get_array_member(event_at(restored, "2026-12-08", 660), "rotations");
    g_assert_cmpuint(json_array_get_length(first_rows), ==, 1); g_assert_cmpuint(json_array_get_length(second_rows), ==, 1);
    JsonObject *first = json_array_get_object_element(first_rows, 0), *second = json_array_get_object_element(second_rows, 0);
    g_assert_cmpstr(radios_json_string(first, "rotation_id", ""), ==, rotation_id);
    g_assert_cmpstr(radios_json_string(second, "rotation_id", ""), ==, rotation_id);
    g_assert_cmpstr(radios_json_string(first, "id", ""), !=, radios_json_string(second, "id", ""));
    g_assert_cmpstr(radios_json_string(first, "id", ""), !=, rotation_id);
    assert_named_settings(json_object_get_object_member(named_definition(restored, rotation_id), "settings"), "linear", 5, 9, 12, 100, 0, 0, 3000, 3000);
    json_node_free(snapshot); snapshot = restored;
    harness_close(harness); harness = harness_open();
    restored = snapshot_of(harness->page); g_assert_true(json_node_equal(snapshot, restored)); json_node_free(restored);
    set_date(harness, "2026-12-07");
    pointer_select(harness, 0, 600, 660, FALSE);
    JsonNode *preview_before = snapshot_of(harness->page), *saved_record = calendar_record();
    gint64 preview_revision = radios_json_int(json_node_get_object(saved_record), "_rev", 0); json_node_free(saved_record);
    row_action(harness, 0, "automation-calendar-preview");
    wait_until(preview_open, NULL, "named assignment authenticates both phases and produces actual native preview");
    GtkWidget *preview = dialog_find("automation-calendar-preview-dialog"), *preview_tree = find_tree(preview);
    g_assert_true(GTK_IS_TREE_VIEW(preview_tree)); g_assert_true(gtk_widget_get_mapped(preview_tree));
    GtkTreeModel *preview_model = gtk_tree_view_get_model(GTK_TREE_VIEW(preview_tree));
    gint preview_count = gtk_tree_model_iter_n_children(preview_model, NULL);
    /* All three aliases bind the same verified media; persisted repeat gaps
     * permit exactly one entry in this genuine native preparation. */
    g_assert_cmpint(preview_count, ==, 1);
    GtkTreeIter prepared; g_assert_true(gtk_tree_model_get_iter_first(preview_model, &prepared));
    const gchar *preview_timezone = radios_json_string(json_node_get_object(preview_before), "timezone", "UTC");
    GTimeZone *preview_zone = radios_time_zone_new(preview_timezone); g_assert_nonnull(preview_zone);
    GDateTime *bound_start = g_date_time_new(preview_zone, 2026, 12, 7, 10, 0, 0);
    GDateTime *bound_end = g_date_time_new(preview_zone, 2026, 12, 7, 11, 0, 0);
    g_assert_nonnull(bound_start); g_assert_nonnull(bound_end);
    gint64 minimum = g_date_time_to_unix(bound_start), maximum = g_date_time_to_unix(bound_end), previous = minimum - 1;
    g_date_time_unref(bound_start); g_date_time_unref(bound_end);
    JsonObject *source_media = json_object_get_object_member(json_node_get_object(detail), "media");
    for (gint index = 0; index < preview_count; ++index) {
        gchar *order = NULL, *title = NULL, *artist = NULL, *kind = NULL, *duration = NULL;
        gtk_tree_model_get(preview_model, &prepared, 0, &order, 1, &title, 2, &artist, 3, &kind, 4, &duration, -1);
        g_assert_cmpint(g_ascii_strtoll(order, NULL, 10), ==, index + 1);
        const gchar *separator = strstr(order, " · "); g_assert_nonnull(separator);
        /* The preview shows a local Turkish date for the user; its backing
         * plan still carries UTC. Interpret the visible value in that same
         * programme timezone before checking authenticated time bounds. */
        const gchar *visible_stamp = separator + strlen(" · ");
        gint day, month, year, hour, minute, second; gchar trailing;
        g_assert_cmpint(sscanf(visible_stamp, "%2d.%2d.%4d %2d:%2d:%2d%c",
            &day, &month, &year, &hour, &minute, &second, &trailing), ==, 6);
        GDateTime *stamp = g_date_time_new(preview_zone, year, month, day, hour, minute, second); g_assert_nonnull(stamp);
        gchar *formatted = g_date_time_format(stamp, "%d.%m.%Y %H:%M:%S");
        g_assert_cmpstr(visible_stamp, ==, formatted); g_free(formatted);
        gint64 instant = g_date_time_to_unix(stamp); g_date_time_unref(stamp);
        g_assert_cmpint(instant, >=, minimum); g_assert_cmpint(instant + 2, <=, maximum); g_assert_cmpint(instant, >, previous); previous = instant;
        g_assert_cmpstr(title, ==, radios_json_string(source_media, "title", ""));
        g_test_message("Actual named preview %s: authoritative title=%s, artist=%s", order, title, artist);
        g_assert_cmpstr(artist, ==, radios_json_string(source_media, "artist", ""));
        g_assert_cmpstr(kind, ==, "music"); g_assert_cmpstr(duration, ==, "00:02");
        g_free(order); g_free(title); g_free(artist); g_free(kind); g_free(duration);
        if (index + 1 < preview_count) g_assert_true(gtk_tree_model_iter_next(preview_model, &prepared));
    }
    g_time_zone_unref(preview_zone);
    Harness preview_capture = {0}; preview_capture.window = preview;
    capture_to(&preview_capture, g_getenv("RADIOS_ROTATION_PREVIEW_CAPTURE"));
    restored = snapshot_of(harness->page); g_assert_true(json_node_equal(preview_before, restored)); json_node_free(restored);
    saved_record = calendar_record(); g_assert_cmpint(radios_json_int(json_node_get_object(saved_record), "_rev", 0), ==, preview_revision);
    g_assert_true(json_node_equal(preview_before, json_object_get_member(json_node_get_object(saved_record), "config"))); json_node_free(saved_record);
    gtk_dialog_response(GTK_DIALOG(preview), GTK_RESPONSE_CLOSE); pump();
    g_assert_true(gtk_widget_is_sensitive(control(harness->page, "automation-calendar-append")));
    click(harness->page, "automation-calendar-manage-rotation"); dialog = reusable_dialog();
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-name")), "Bağımsız gece rotasyonu · güncel"); pump();
    g_assert_false(gtk_widget_is_sensitive(control(harness->page, "automation-calendar-append")));
    g_assert_false(gtk_widget_is_sensitive(control(harness->page, "automation-calendar-schedule")));
    wait_saved(harness);
    restored = snapshot_of(harness->page);
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(preview_before), "events"),
                                 json_object_get_member(json_node_get_object(restored), "events")));
    json_node_free(restored); json_node_free(preview_before);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump();
    /* Leave older absolute calendar-counter cases their original empty-event
     * baseline, using only actual native block removal. Keep the library. */
    block_action(harness, 0, 600, "automation-calendar-remove-programme"); wait_saved(harness);
    block_action(harness, 1, 660, "automation-calendar-remove-programme"); wait_saved(harness);
    restored = snapshot_of(harness->page);
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(initial), "events"),
                                 json_object_get_member(json_node_get_object(restored), "events")));
    assert_reusable_source(json_array_get_object_element(json_object_get_array_member(named_definition(restored, rotation_id), "sources"), 0), playlist);
    json_node_free(restored); json_node_free(snapshot); json_node_free(initial); json_node_free(detail);
    g_free(rotation_id); g_free(playlist); g_free(other); harness_close(harness);
}

static GtkWidget *find_tree(GtkWidget *widget)
{
    if (GTK_IS_TREE_VIEW(widget)) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find_tree(item->data);
    g_list_free(children); return found;
}
static gboolean preview_open(gpointer value)
{ (void)value; return dialog_find("automation-calendar-preview-dialog") != NULL; }

static void test_native_verified_rotation_preview(void)
{
    gchar *playlist = create_playlist("PDF13 Gerçek önizleme kaynağı"); Harness *harness = harness_open();
    JsonNode *detail = verified_media(harness); bind_playlist(playlist, detail);
    radios_data_refresh(&harness->ctx); set_date(harness, "2026-10-12");
    new_event(harness, 0, 600, 601, "Doğrulanmış rotasyon hazırlığı", TRUE);
    seed_legacy_rotation(harness, playlist, 2, TRUE, TRUE, FALSE, TRUE);
    JsonNode *before = snapshot_of(harness->page), *record = calendar_record();
    gint64 revision = radios_json_int(json_node_get_object(record), "_rev", 0); json_node_free(record);
    row_action(harness, 0, "automation-calendar-preview");
    wait_until(preview_open, NULL, "both genuine prepare responses produce the native readonly preview");
    GtkWidget *dialog = dialog_find("automation-calendar-preview-dialog"), *tree = find_tree(dialog);
    g_assert_true(GTK_IS_TREE_VIEW(tree)); GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
    gint count = gtk_tree_model_iter_n_children(model, NULL); g_assert_cmpint(count, ==, 30);
    GtkWidget *notes = control(dialog, "automation-calendar-preview-notes");
    g_assert_true(GTK_IS_LABEL(notes));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(notes)), "blok sonunu"));
    GtkTreeIter iter; g_assert_true(gtk_tree_model_get_iter_first(model, &iter)); gchar *title = NULL;
    gtk_tree_model_get(model, &iter, 1, &title, -1); g_assert_cmpstr(title, ==, "PDF13 Doğrulanmış PCM"); g_free(title);
    g_assert_true(gtk_widget_is_sensitive(control(harness->page, "automation-calendar-append")));
    g_assert_false(gtk_widget_is_sensitive(control(harness->page, "automation-calendar-schedule")));
    JsonNode *after = snapshot_of(harness->page); g_assert_true(json_node_equal(before, after)); json_node_free(after);
    record = calendar_record(); g_assert_cmpint(radios_json_int(json_node_get_object(record), "_rev", 0), ==, revision); json_node_free(record);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump();
    gtk_entry_set_text(GTK_ENTRY(control(harness->page, "automation-calendar-programme-name")), "Hazırlıktan sonra değişen program");
    g_assert_false(gtk_widget_is_sensitive(control(harness->page, "automation-calendar-append")));
    g_assert_false(gtk_widget_is_sensitive(control(harness->page, "automation-calendar-schedule")));
    wait_saved(harness);
    json_node_free(before); json_node_free(detail); harness_close(harness); g_free(playlist);
}

static JsonObject *event_id(JsonNode *snapshot, const gchar *id)
{
    JsonArray *array = events(snapshot);
    for (guint i = 0; i < json_array_get_length(array); ++i) {
        JsonObject *event = json_array_get_object_element(array, i);
        if (!g_strcmp0(radios_json_string(event, "id", ""), id)) return event;
    }
    g_error("Expected calendar UUID %s", id); return NULL;
}

static gint64 saved_revision(void)
{
    JsonNode *record = calendar_record(); g_assert_nonnull(record);
    gint64 revision = radios_json_int(json_node_get_object(record), "_rev", 0);
    json_node_free(record); g_assert_cmpint(revision, >, 0); return revision;
}

static void entry_set(Harness *harness, const gchar *name, const gchar *value)
{ gtk_entry_set_text(GTK_ENTRY(control(harness->page, name)), value); }

static void select_group(Harness *harness, guint day, guint first, guint second)
{
    gdouble x, y; GtkWidget *grid = control(harness->page, "automation-calendar-grid");
    grid_point(harness, day, first, &x, &y);
    pointer_event(grid, GDK_BUTTON_PRESS, x, y, 0); pointer_event(grid, GDK_BUTTON_RELEASE, x, y, 0);
    grid_point(harness, day, second, &x, &y);
    pointer_event(grid, GDK_BUTTON_PRESS, x, y, GDK_CONTROL_MASK);
    pointer_event(grid, GDK_BUTTON_RELEASE, x, y, GDK_CONTROL_MASK);
}

static void drag_blocks(Harness *harness, guint day, guint minute, guint target_day,
                        guint target_minute, gboolean outside)
{
    GtkWidget *grid = control(harness->page, "automation-calendar-grid");
    gdouble x, y, target_x, target_y;
    grid_point(harness, day, minute, &x, &y);
    pointer_event(grid, GDK_BUTTON_PRESS, x, y, 0);
    grid_point(harness, target_day, target_minute, &target_x, &target_y);
    JsonNode *before = snapshot_of(harness->page); gint64 revision = saved_revision();
    for (guint i = 1; i <= 8; ++i)
        pointer_event(grid, GDK_MOTION_NOTIFY, x + (target_x - x) * i / 8.0,
                      y + (target_y - y) * i / 8.0, GDK_BUTTON1_MASK);
    JsonNode *ghost = snapshot_of(harness->page);
    g_assert_true(json_node_equal(before, ghost));
    g_assert_cmpint(saved_revision(), ==, revision);
    json_node_free(before); json_node_free(ghost);
    pointer_event(grid, GDK_BUTTON_RELEASE, outside ? -4 : target_x, target_y, 0);
}

static void assert_database_unchanged(JsonNode *before, Harness *harness)
{
    JsonNode *after = snapshot_of(harness->page); g_assert_true(json_node_equal(before, after));
    JsonNode *record = calendar_record();
    g_assert_true(json_node_equal(before, json_object_get_member(json_node_get_object(record), "config")));
    json_node_free(after); json_node_free(record);
}

static void test_native_context_drag_properties_autosave(void)
{
    Harness *harness = harness_open_with_rules(TRUE); set_date(harness, "2026-11-02");
    /* The former AutoDJ form still supplies real acknowledged defaults but
     * has no mapped hierarchy, tab or secondary properties surface. */
    GtkWidget *main = find(harness->page, "automation-calendar-main"); g_assert_nonnull(main);
    GtkWidget *legacy = g_object_get_data(G_OBJECT(main), "legacy-rules");
    g_assert_nonnull(legacy); g_assert_false(gtk_widget_get_mapped(legacy));
    g_assert_false(gtk_widget_is_ancestor(legacy, harness->page));
    JsonNode *defaults = radios_automation_rules_configuration(legacy);
    g_assert_nonnull(defaults); g_assert_true(JSON_NODE_HOLDS_OBJECT(defaults)); json_node_free(defaults);
    assert_top_calendar_controls(harness);
    click(harness->page, "automation-calendar-next");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(harness->page, "automation-calendar-date"))), ==, "2026-11-09");
    click(harness->page, "automation-calendar-prev");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(harness->page, "automation-calendar-date"))), ==, "2026-11-02");
    new_event(harness, 0, 360, 420, "Birlikte taşınan bir", FALSE);
    new_event(harness, 0, 480, 540, "Birlikte taşınan iki", TRUE);
    JsonNode *snapshot = snapshot_of(harness->page);
    gchar *first_id = g_strdup(radios_json_string(event_at(snapshot, "2026-11-02", 360), "id", ""));
    gchar *second_id = g_strdup(radios_json_string(event_at(snapshot, "2026-11-02", 480), "id", ""));
    json_node_free(snapshot);
    select_group(harness, 0, 360, 480); drag_blocks(harness, 0, 360, 3, 540, FALSE); wait_saved(harness);
    snapshot = snapshot_of(harness->page);
    JsonObject *first = event_id(snapshot, first_id), *second = event_id(snapshot, second_id);
    g_assert_cmpstr(radios_json_string(first, "date", ""), ==, "2026-11-05");
    g_assert_cmpstr(radios_json_string(second, "date", ""), ==, "2026-11-05");
    g_assert_cmpint(radios_json_int(first, "start_minute", 0), ==, 540);
    g_assert_cmpint(radios_json_int(first, "end_minute", 0), ==, 600);
    g_assert_cmpint(radios_json_int(second, "start_minute", 0), ==, 660);
    g_assert_cmpint(radios_json_int(second, "end_minute", 0), ==, 720); json_node_free(snapshot);
    new_event(harness, 4, 540, 600, "Çakışan tek hedef", FALSE);
    select_group(harness, 3, 540, 660); snapshot = snapshot_of(harness->page);
    drag_blocks(harness, 3, 540, 4, 540, FALSE);
    assert_database_unchanged(snapshot, harness);
    drag_blocks(harness, 3, 540, 5, 540, TRUE);
    assert_database_unchanged(snapshot, harness); json_node_free(snapshot);
    /* A single real context activation removes the whole selected group. */
    block_action(harness, 3, 540, "automation-calendar-remove-programme"); wait_saved(harness);
    g_assert_null(dialog_find("automation-calendar-remove-dialog"));
    snapshot = snapshot_of(harness->page);
    JsonArray *array = events(snapshot);
    for (guint i = 0; i < json_array_get_length(array); ++i) {
        const gchar *id = radios_json_string(json_array_get_object_element(array, i), "id", "");
        g_assert_cmpstr(id, !=, first_id); g_assert_cmpstr(id, !=, second_id);
    }
    json_node_free(snapshot); g_free(first_id); g_free(second_id);
    pointer_select(harness, 4, 540, 600, FALSE); assert_programme_panel(harness);
    snapshot = snapshot_of(harness->page);
    gchar *remaining_id = g_strdup(radios_json_string(event_at(snapshot, "2026-11-06", 540), "id", ""));
    entry_set(harness, "automation-calendar-programme-start", "25:00"); settle_layout();
    assert_database_unchanged(snapshot, harness);
    entry_set(harness, "automation-calendar-programme-start", "08:05");
    entry_set(harness, "automation-calendar-programme-end", "08:10");
    entry_set(harness, "automation-calendar-programme-date", "2026-11-07");
    entry_set(harness, "automation-calendar-programme-name", "Beş dakikalık jingle bloğu"); wait_saved(harness);
    json_node_free(snapshot); snapshot = snapshot_of(harness->page);
    first = event_id(snapshot, remaining_id);
    g_assert_cmpstr(radios_json_string(first, "date", ""), ==, "2026-11-07");
    g_assert_cmpstr(radios_json_string(first, "title", ""), ==, "Beş dakikalık jingle bloğu");
    g_assert_cmpint(radios_json_int(first, "start_minute", 0), ==, 485);
    g_assert_cmpint(radios_json_int(first, "end_minute", 0), ==, 490); json_node_free(snapshot);
    /* Hit testing and dragging must support properties-created intervals
     * that are smaller than, and unaligned with, the current grid step. */
    pointer_select(harness, 5, 486, 490, FALSE);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(harness->page, "automation-calendar-programme-name"))), ==, "Beş dakikalık jingle bloğu");
    drag_blocks(harness, 5, 486, 6, 516, FALSE); wait_saved(harness);
    snapshot = snapshot_of(harness->page); first = event_id(snapshot, remaining_id);
    g_assert_cmpstr(radios_json_string(first, "date", ""), ==, "2026-11-08");
    g_assert_cmpint(radios_json_int(first, "start_minute", 0), ==, 515);
    g_assert_cmpint(radios_json_int(first, "end_minute", 0), ==, 520);
    capture_to(harness, g_getenv("RADIOS_AUTOMATION_INLINE_CAPTURE"));
    gdouble x, y; grid_point(harness, 6, 516, &x, &y);
    pointer_button(control(harness->page, "automation-calendar-grid"), GDK_BUTTON_PRESS, x, y, 0, 3);
    Harness menu_harness = {0}; menu_harness.window = gtk_widget_get_toplevel(control(harness->page, "automation-calendar-copy"));
    capture_to(&menu_harness, g_getenv("RADIOS_AUTOMATION_BLOCK_MENU_CAPTURE"));
    gtk_menu_popdown(GTK_MENU(gtk_widget_get_parent(control(harness->page, "automation-calendar-copy")))); pump();
    harness_close(harness); harness = harness_open_with_rules(TRUE);
    JsonNode *restored = snapshot_of(harness->page); g_assert_true(json_node_equal(snapshot, restored));
    json_node_free(restored); json_node_free(snapshot); g_free(remaining_id); harness_close(harness);
}

static void rotation_values(GtkWidget *dialog, const gchar *playlist, const gchar *kind, guint frequency,
                            gboolean random, gboolean track, gboolean advert, gboolean sweeper)
{
    PlaylistChoice choice = {control(dialog, "automation-calendar-rotation-playlist"), playlist};
    wait_until(playlist_available, &choice, "existing rotation playlist available");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(dialog, "automation-calendar-rotation-media-kind")), kind));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(dialog, "automation-calendar-rotation-frequency")), frequency);
    const gchar *names[] = {"automation-calendar-rotation-random", "automation-calendar-rotation-track-random",
        "automation-calendar-rotation-advert-random", "automation-calendar-rotation-sweeper-random"};
    const gboolean states[] = {random, track, advert, sweeper};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(dialog, names[i])), states[i]);
}

static void test_native_rotation_edit_in_place(void)
{
    gchar *playlist = create_playlist("Düzenlenen jingle playlisti");
    gchar *other = create_playlist("Korunan ikinci satır");
    Harness *harness = harness_open(); set_date(harness, "2026-11-09");
    new_event(harness, 0, 600, 660, "Düzenlenebilir iki satır", TRUE);
    seed_legacy_rotation(harness, playlist, 2, FALSE, TRUE, FALSE, TRUE);
    seed_legacy_rotation(harness, other, 7, TRUE, FALSE, TRUE, FALSE);
    JsonNode *before = snapshot_of(harness->page);
    JsonArray *rows = json_object_get_array_member(event_at(before, "2026-11-09", 600), "rotations");
    gchar *row_id = g_strdup(radios_json_string(json_array_get_object_element(rows, 0), "id", ""));
    JsonNode *other_row = json_node_copy(json_array_get_element(rows, 1));
    row_pointer(harness, 0, GDK_BUTTON_PRESS, 3);
    Harness menu_harness = {0}; menu_harness.window = gtk_widget_get_toplevel(control(harness->page, "automation-calendar-row-edit"));
    capture_to(&menu_harness, g_getenv("RADIOS_AUTOMATION_ROW_MENU_CAPTURE"));
    gtk_menu_popdown(GTK_MENU(gtk_widget_get_parent(control(harness->page, "automation-calendar-row-edit")))); pump();
    row_pointer(harness, 0, GDK_2BUTTON_PRESS, 1);
    GtkWidget *dialog = dialog_find("automation-calendar-rotation-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(control(dialog, "automation-calendar-rotation-media-kind"))), ==, "all");
    rotation_values(dialog, other, "jingle", 4, TRUE, FALSE, TRUE, FALSE);
    Harness dialog_harness = {0}; dialog_harness.window = dialog;
    capture_to(&dialog_harness, g_getenv("RADIOS_AUTOMATION_EDIT_CAPTURE"));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); pump(); wait_saved(harness);
    JsonNode *after = snapshot_of(harness->page);
    rows = json_object_get_array_member(event_at(after, "2026-11-09", 600), "rotations");
    g_assert_cmpuint(json_array_get_length(rows), ==, 2);
    JsonObject *edited = json_array_get_object_element(rows, 0);
    g_assert_cmpstr(radios_json_string(edited, "id", ""), ==, row_id);
    g_assert_cmpstr(radios_json_string(edited, "media_kind", ""), ==, "jingle");
    assert_rotation(edited, other, 4, TRUE, FALSE, TRUE, FALSE);
    g_assert_true(json_node_equal(other_row, json_array_get_element(rows, 1)));
    /* Existing historical flat rows appear in the genuine reuse selector.
     * Assigning one imports a distinct reusable definition without changing
     * either original row or its playlist, flags and jingle constraint. */
    gchar *legacy_choice = g_strconcat("legacy/", row_id, NULL);
    select_library_rotation(harness->page, legacy_choice);
    click(harness->page, "automation-calendar-assign-rotation"); wait_saved(harness);
    JsonNode *converted = snapshot_of(harness->page);
    JsonArray *converted_rows = json_object_get_array_member(event_at(converted, "2026-11-09", 600), "rotations");
    g_assert_cmpuint(json_array_get_length(converted_rows), ==, 3);
    g_assert_true(json_node_equal(json_array_get_element(rows, 0), json_array_get_element(converted_rows, 0)));
    g_assert_true(json_node_equal(json_array_get_element(rows, 1), json_array_get_element(converted_rows, 1)));
    JsonObject *assignment = json_array_get_object_element(converted_rows, 2);
    const gchar *converted_id = radios_json_string(assignment, "rotation_id", "");
    g_assert_true(g_uuid_string_is_valid(converted_id));
    g_assert_cmpstr(converted_id, !=, row_id);
    g_assert_cmpstr(radios_json_string(assignment, "id", ""), !=, row_id);
    g_assert_cmpstr(radios_json_string(assignment, "id", ""), !=, converted_id);
    JsonObject *imported = named_definition(converted, converted_id);
    JsonArray *imported_sources = json_object_get_array_member(imported, "sources");
    g_assert_cmpuint(json_array_get_length(imported_sources), ==, 1);
    JsonObject *imported_source = json_array_get_object_element(imported_sources, 0);
    g_assert_cmpstr(radios_json_string(imported_source, "id", ""), !=, row_id);
    g_assert_cmpstr(radios_json_string(imported_source, "id", ""), !=, converted_id);
    assert_rotation(imported_source, other, 1, TRUE, FALSE, TRUE, FALSE);
    g_assert_cmpstr(radios_json_string(imported_source, "media_kind", ""), ==, "jingle");
    g_assert_cmpint(radios_json_int(imported_source, "count", -1), ==, 4);
    g_assert_cmpint(radios_json_int(imported_source, "weight", -1), ==, 1);
    g_assert_cmpint(radios_json_int(imported_source, "start_minute", -1), ==, 0);
    g_assert_cmpint(radios_json_int(imported_source, "end_minute", -1), ==, 1440);
    g_assert_cmpint(radios_json_int(imported_source, "interval_minutes", -1), ==, 0);
    assert_named_settings(json_object_get_object_member(imported, "settings"), "linear", 0, 0, 0, 100, 0, 0, 3000, 3000);
    row_action(harness, 2, "automation-calendar-row-remove"); wait_saved(harness);
    JsonNode *cleaned = snapshot_of(harness->page);
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(after), "events"),
                                 json_object_get_member(json_node_get_object(cleaned), "events")));
    json_node_free(converted); json_node_free(after); after = cleaned; g_free(legacy_choice);
    row_action(harness, 0, "automation-calendar-row-edit");
    dialog = dialog_find("automation-calendar-rotation-dialog");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(control(dialog, "automation-calendar-rotation-media-kind"))), ==, "jingle");
    rotation_values(dialog, playlist, "advertisement", 9, FALSE, TRUE, FALSE, TRUE);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); pump(); assert_database_unchanged(after, harness);
    harness_close(harness); harness = harness_open();
    JsonNode *restored = snapshot_of(harness->page); g_assert_true(json_node_equal(after, restored));
    set_date(harness, "2026-11-09"); pointer_select(harness, 0, 600, 660, FALSE);
    row_action(harness, 0, "automation-calendar-row-edit"); dialog = dialog_find("automation-calendar-rotation-dialog");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(control(dialog, "automation-calendar-rotation-media-kind"))), ==, "jingle");
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control(dialog, "automation-calendar-rotation-frequency"))), ==, 4);
    PlaylistChoice choice = {control(dialog, "automation-calendar-rotation-playlist"), other};
    wait_until(playlist_available, &choice, "saved edit playlist on reopening");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); pump();
    json_node_free(before); json_node_free(after); json_node_free(restored); json_node_free(other_row);
    g_free(row_id); g_free(playlist); g_free(other); harness_close(harness);
}

/* A genuine bounded Runtime job holds its worker; UI saves still prepare
 * durable receipts, queue normally and use the production data callbacks. */
typedef struct {gint started, release;} RuntimeGate;
static gchar *gate_worker(gpointer value, const RadioSCancelToken *cancel, GError **error)
{
    (void)error; RuntimeGate *gate = value; g_atomic_int_set(&gate->started, 1);
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (!g_atomic_int_get(&gate->release) && !radios_cancelled(cancel) && g_get_monotonic_time() < deadline) g_usleep(1000);
    return g_strdup("Native calendar receipt gate released");
}
static gboolean gate_started(gpointer value) { return g_atomic_int_get(&((RuntimeGate *)value)->started) != 0; }
static void gate_start(Harness *harness, RuntimeGate *gate)
{
    GError *error = NULL;
    g_assert_cmpuint(radios_runtime_submit(harness->ctx.runtime, gate_worker, gate, NULL, &error), >, 0);
    g_assert_no_error(error); wait_until(gate_started, gate, "genuine worker gate started");
}
static guint pending_count(Harness *harness)
{
    JsonNode *status = radios_data_recovery_status(&harness->ctx); g_assert_nonnull(status);
    guint pending = (guint)radios_json_int(json_node_get_object(status), "pending", 0);
    json_node_free(status); return pending;
}
static gboolean recovery_empty(gpointer value)
{
    Harness *harness = value;
    JsonNode *status = radios_data_recovery_status(&harness->ctx); g_assert_nonnull(status);
    JsonObject *object = json_node_get_object(status); g_assert_false(json_object_has_member(object, "error"));
    gboolean empty = radios_json_int(object, "pending", -1) == 0 && radios_json_int(object, "blocked", -1) == 0;
    json_node_free(status); return empty;
}
typedef struct {Harness *harness; guint count;} Pending;
static gboolean receipt_pending(gpointer value)
{ Pending *pending = value; return pending_count(pending->harness) >= pending->count; }
static gboolean reload_open(gpointer value)
{ (void)value; return dialog_find("database-reload-dialog") != NULL; }

static void test_native_autosave_burst_cas_conflict(void)
{
    Harness *harness = harness_open(); set_date(harness, "2026-11-16");
    new_event(harness, 0, 600, 660, "Autosave başlangıcı", FALSE);
    gint64 base_revision = saved_revision();
    for (guint i = 0; i < 50; ++i) {
        gchar *title = g_strdup_printf("Birleştirilen değişiklik %u", i);
        entry_set(harness, "automation-calendar-programme-name", title); g_free(title);
    }
    wait_saved(harness); gint64 final_revision = saved_revision();
    g_assert_cmpint(final_revision - base_revision, >=, 1);
    g_assert_cmpint(final_revision - base_revision, <=, 2);
    JsonNode *record = calendar_record();
    JsonNode *configuration = json_object_get_member(json_node_get_object(record), "config");
    g_assert_cmpstr(radios_json_string(event_at(configuration, "2026-11-16", 600), "title", ""), ==, "Birleştirilen değişiklik 49");
    json_node_free(record);
    /* A valid name save may finish after a user starts an incomplete time.
     * The ACK and asynchronous playlist refresh must preserve that buffer. */
    RuntimeGate gate = {0}; gate_start(harness, &gate);
    guint pending_before = pending_count(harness);
    entry_set(harness, "automation-calendar-programme-name", "ACK sırasında korunan zaman girdisi");
    Pending pending = {harness, pending_before + 1}; wait_until(receipt_pending, &pending, "real autosave receipt prepared behind worker gate");
    GtkWidget *start = control(harness->page, "automation-calendar-programme-start");
    gtk_widget_grab_focus(start); gtk_entry_set_text(GTK_ENTRY(start), "0"); gtk_editable_set_position(GTK_EDITABLE(start), 1);
    g_atomic_int_set(&gate.release, 1); wait_until(calendar_acknowledged, harness, "real delayed ACK completes");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(start)), ==, "0");
    g_assert_cmpint(gtk_editable_get_position(GTK_EDITABLE(start)), ==, 1);
    radios_data_refresh(&harness->ctx); settle_layout();
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(start)), ==, "0");
    entry_set(harness, "automation-calendar-programme-start", "10:00"); wait_saved(harness);
    /* An external genuine CAS write wins. Autosave must preserve the local
     * draft and offer explicit reload without overwriting that record. */
    record = calendar_record(); JsonObject *object = json_node_get_object(record);
    JsonNode *external = json_node_copy(json_object_get_member(object, "config"));
    json_object_set_string_member(event_at(external, "2026-11-16", 600), "title", "Başka istemcinin kaydı");
    JsonNode *body = body_new(); JsonObject *put = json_node_get_object(body);
    json_object_set_string_member(put, "kind", "automation_calendar");
    json_object_set_string_member(put, "name", radios_json_string(object, "name", "Automation calendar"));
    json_object_set_string_member(put, "document_id", radios_json_string(object, "_id", ""));
    json_object_set_int_member(put, "expected_revision", radios_json_int(object, "_rev", 0));
    json_object_set_member(put, "config", json_node_copy(external));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(put, "idempotency_key", key); g_free(key);
    JsonNode *reply = request("/v1/workflows/desktop/put", body);
    json_node_free(reply); json_node_free(body); json_node_free(record);
    entry_set(harness, "automation-calendar-programme-name", "Çatışmada korunan yerel taslak");
    wait_until(reload_open, NULL, "actual autosave stale CAS rejected with explicit reload dialog");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(harness->page, "automation-calendar-programme-name"))), ==, "Çatışmada korunan yerel taslak");
    record = calendar_record();
    g_assert_true(json_node_equal(external, json_object_get_member(json_node_get_object(record), "config"))); json_node_free(record);
    GtkWidget *dialog = dialog_find("database-reload-dialog");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); pump(); settle_layout();
    g_assert_null(dialog_find("database-reload-dialog"));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(harness->page, "automation-calendar-programme-name"))), ==, "Çatışmada korunan yerel taslak");
    record = calendar_record(); g_assert_true(json_node_equal(external, json_object_get_member(json_node_get_object(record), "config")));
    json_node_free(record); harness_close(harness);
    harness = harness_open(); JsonNode *restored = snapshot_of(harness->page);
    g_assert_true(json_node_equal(external, restored)); json_node_free(restored); json_node_free(external); harness_close(harness);
}

typedef struct {guint recovered; GError *error; gint done;} Recovery;
static gpointer recovery_worker(gpointer value)
{
    Recovery *recovery = value;
    recovery->recovered = radios_data_client_recover_sync(reader, NULL, 8, &recovery->error);
    g_atomic_int_set(&recovery->done, 1); return NULL;
}
static gboolean recovery_done(gpointer value) { return g_atomic_int_get(&((Recovery *)value)->done) != 0; }

static void test_native_pending_close_recovery(void)
{
    Harness *harness = harness_open(); set_date(harness, "2026-11-23");
    new_event(harness, 0, 600, 660, "Kapanıştan önceki onaylı kayıt", FALSE);
    gint64 revision = saved_revision(); guint before = pending_count(harness);
    RuntimeGate gate = {0}; gate_start(harness, &gate);
    entry_set(harness, "automation-calendar-programme-name", "Kuyruktaki ilk autosave");
    Pending pending = {harness, before + 1}; wait_until(receipt_pending, &pending, "first autosave durably queued before HTTP");
    entry_set(harness, "automation-calendar-programme-name", "Kapanışta korunan son autosave");
    gint64 deadline = g_get_monotonic_time() + 320 * 1000;
    while (g_get_monotonic_time() < deadline) pump();
    JsonNode *expected = snapshot_of(harness->page);
    gdouble x, y; grid_point(harness, 0, 600, &x, &y);
    pointer_button(control(harness->page, "automation-calendar-grid"), GDK_BUTTON_PRESS, x, y, 0, 3);
    wait_until(menu_item_viewable, control(harness->page, "automation-calendar-copy"),
               "real block popup left open for pre-parent-close cleanup");
    /* Runtime shutdown cancels the gate and prevents either queued HTTP
     * callback from rescuing the close: only exact durable recovery can. */
    harness_close(harness);
    Recovery recovery = {0}; GThread *thread = g_thread_new("calendar-acceptance-recover", recovery_worker, &recovery);
    wait_until(recovery_done, &recovery, "actual durable close receipts recovered"); g_thread_join(thread);
    g_assert_no_error(recovery.error); g_assert_cmpuint(recovery.recovered, ==, 2);
    JsonNode *record = calendar_record();
    g_assert_cmpint(radios_json_int(json_node_get_object(record), "_rev", 0), ==, revision + 2);
    g_assert_true(json_node_equal(expected, json_object_get_member(json_node_get_object(record), "config")));
    json_node_free(record); harness = harness_open();
    JsonNode *restored = snapshot_of(harness->page); g_assert_true(json_node_equal(expected, restored));
    json_node_free(expected); json_node_free(restored); harness_close(harness);
}

static void test_native_repeat_rules_tabs_persistence_override(void)
{
    Harness *harness = harness_open();
    gchar *playlist = create_playlist("Native repeat rules playlist");
    JsonNode *media = verified_media(harness); bind_playlist(playlist, media);
    radios_data_refresh(&harness->ctx);
    click(harness->page, "automation-calendar-independent-rotation");
    GtkWidget *dialog = reusable_dialog();
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-name")), "Native repeat rules"); pump();
    JsonNode *snapshot = snapshot_of(harness->page);
    JsonArray *library = json_object_get_array_member(json_node_get_object(snapshot), "rotation_library");
    JsonObject *definition = json_array_get_object_element(library, json_array_get_length(library) - 1);
    gchar *rotation_id = g_strdup(radios_json_string(definition, "id", "")); json_node_free(snapshot);
    gtk_expander_set_expanded(GTK_EXPANDER(control(dialog, "rotation-editor-settings")), TRUE); settle_layout();
    /* Transitions have one home each (probability window, AutoDJ page). The
     * advanced rotation rules keep only repeat limits; the AutoDJ page opens
     * from the library footer button beside "Olasılık ve geçişler…". */
    g_assert_null(find(dialog, "rotation-editor-settings-tabs"));
    GtkWidget *repeat_page = control(dialog, "rotation-editor-settings");
    GtkWidget *autodj_page = control(dialog, "rotation-editor-autodj-policy-page");
    GtkWidget *pages = control(dialog, "rotation-editor-pages"); g_assert_true(GTK_IS_STACK(pages));
    g_assert_true(gtk_widget_is_ancestor(control(dialog, "rotation-editor-track-gap"), repeat_page));
    g_assert_true(gtk_widget_is_ancestor(control(dialog, "rotation-editor-soft-end-seconds"), repeat_page));
    g_assert_true(gtk_widget_is_ancestor(control(dialog, "rotation-editor-autodj-policy-enabled"), autodj_page));
    g_assert_false(gtk_widget_is_ancestor(autodj_page, repeat_page));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(pages)), ==, "library");
    click(dialog, "rotation-editor-autodj-policy-open"); settle_layout();
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(pages)), ==, "autodj-automation");
    g_assert_true(gtk_widget_get_mapped(control(dialog, "rotation-editor-autodj-policy-enabled")));
    click(dialog, "rotation-editor-autodj-automation-back"); settle_layout();
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(pages)), ==, "library");
    g_assert_null(find(dialog, "rotation-editor-transition-mode"));
    g_assert_null(find(dialog, "rotation-editor-transition-crossfade-effect"));
    g_assert_null(find(dialog, "rotation-editor-source-routing"));
    reusable_spin(dialog, "rotation-editor-artist-gap", 45); wait_saved(harness);
    PlaylistChoice choice = {control(dialog, "rotation-editor-playlist-combo"), playlist};
    wait_until(playlist_available, &choice, "genuine playlist available in repeat rules editor"); click(dialog, "rotation-editor-add");
    GtkWidget *source_settings = control(dialog, "rotation-editor-source-settings");
    g_assert_cmpstr(gtk_expander_get_label(GTK_EXPANDER(source_settings)), ==, "Bu playlistin tekrar kuralları");
    gtk_expander_set_expanded(GTK_EXPANDER(source_settings), TRUE); settle_layout();
    g_assert_null(find(dialog, "rotation-editor-source-settings-tabs"));
    g_assert_null(find(dialog, "rotation-editor-source-transition-mode"));
    reusable_check(dialog, "rotation-editor-source-override", TRUE);
    reusable_spin(dialog, "rotation-editor-source-artist-gap", 15); wait_saved(harness);
    snapshot = snapshot_of(harness->page); definition = named_definition(snapshot, rotation_id);
    JsonObject *settings = json_object_get_object_member(definition, "settings");
    assert_named_settings(settings, "linear", 0, 45, 0, 100, 0, 0, 3000, 3000);
    g_assert_false(json_object_has_member(settings, "transition_selection_mode"));
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(definition, "sources"), 0);
    assert_named_settings(json_object_get_object_member(source, "settings"), NULL, 0, 15, 0, -1, 0, 0, 0, 0);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump(); harness_close(harness);
    harness = harness_open(); JsonNode *restored = snapshot_of(harness->page);
    g_assert_true(json_node_equal(snapshot, restored)); json_node_free(restored); json_node_free(snapshot);
    choice.combo = control(harness->page, "automation-calendar-existing-rotation"); choice.id = rotation_id;
    wait_until(playlist_available, &choice, "saved repeat rules rotation available after genuine reopen");
    click(harness->page, "automation-calendar-manage-rotation"); dialog = reusable_dialog();
    reusable_select_source(dialog, 0); reusable_source_song_click(dialog, 0);
    gtk_expander_set_expanded(GTK_EXPANDER(control(dialog, "rotation-editor-source-settings")), TRUE); settle_layout();
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control(dialog, "rotation-editor-source-override"))));
    reusable_reveal(control(dialog, "rotation-editor-source-artist-gap"));
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control(dialog, "rotation-editor-source-artist-gap"))), ==, 15);
    reusable_check(dialog, "rotation-editor-source-override", FALSE); wait_saved(harness);
    snapshot = snapshot_of(harness->page); definition = named_definition(snapshot, rotation_id);
    source = json_array_get_object_element(json_object_get_array_member(definition, "sources"), 0);
    g_assert_false(json_object_has_member(source, "settings"));
    assert_named_settings(json_object_get_object_member(definition, "settings"), "linear", 0, 45, 0, 100, 0, 0, 3000, 3000);
    JsonNode *record = calendar_record();
    g_assert_true(json_node_equal(snapshot, json_object_get_member(json_node_get_object(record), "config")));
    json_node_free(record); json_node_free(snapshot);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump(); harness_close(harness);
    json_node_free(media); g_free(playlist); g_free(rotation_id);
    g_test_message("Repeat-only advanced rules beside the AutoDJ page, playlist override persistence, reopen and override removal passed.");
}


static gchar *reusable_select_atomic_song(GtkWidget *dialog, guint index)
{
    GtkWidget *songs = control(dialog, "rotation-editor-songs");
    reusable_reveal(songs);
    g_assert_true(GTK_IS_TREE_VIEW(songs)); g_assert_true(gtk_widget_get_mapped(songs));
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)index, -1); GtkTreeIter iter;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(songs));
    g_assert_true(gtk_tree_model_get_iter(model, &iter, path));
    gchar *entry_id = NULL; gtk_tree_model_get(model, &iter, 0, &entry_id, -1);
    g_assert_true(g_uuid_string_is_valid(entry_id));
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(songs), path, NULL, FALSE);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(songs)), path);
    gtk_tree_path_free(path); pump(); settle_layout();
    reusable_reveal(control(dialog, "rotation-editor-song-properties"));
    g_assert_true(gtk_widget_get_mapped(control(dialog, "rotation-editor-song-properties")));
    GtkWidget *summary = control(dialog, "rotation-editor-song-summary"); g_assert_true(GTK_IS_LABEL(summary));
    g_assert_true(*gtk_label_get_text(GTK_LABEL(summary)) != '\0');
    return entry_id;
}

static gchar *reusable_atomic_song_id(GtkWidget *dialog, guint index)
{
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(control(dialog, "rotation-editor-songs")));
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)index, -1); GtkTreeIter iter;
    g_assert_true(gtk_tree_model_get_iter(model, &iter, path)); gtk_tree_path_free(path);
    gchar *id = NULL; gtk_tree_model_get(model, &iter, 0, &id, -1); g_assert_true(g_uuid_string_is_valid(id)); return id;
}
static void reusable_select_atomic_song_id(GtkWidget *dialog, const gchar *entry_id)
{
    GtkTreeView *songs = GTK_TREE_VIEW(control(dialog, "rotation-editor-songs"));
    reusable_reveal(GTK_WIDGET(songs)); GtkTreeModel *model = gtk_tree_view_get_model(songs);
    GtkTreeIter iter; gboolean found = FALSE;
    for (gboolean more = gtk_tree_model_get_iter_first(model, &iter); more && !found; more = gtk_tree_model_iter_next(model, &iter)) {
        gchar *id = NULL; gtk_tree_model_get(model, &iter, 0, &id, -1); found = !g_strcmp0(id, entry_id); g_free(id);
        if (found) {
            GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
            gtk_tree_view_set_cursor(songs, path, NULL, FALSE); gtk_tree_selection_select_path(gtk_tree_view_get_selection(songs), path);
            gtk_tree_path_free(path); pump(); settle_layout();
        }
    }
    g_assert_true(found); reusable_reveal(control(dialog, "rotation-editor-song-properties"));
    g_assert_true(gtk_widget_get_mapped(control(dialog, "rotation-editor-song-properties")));
    GtkTreeModel *selected_model = NULL; GtkTreeIter selected;
    g_assert_true(gtk_tree_selection_get_selected(gtk_tree_view_get_selection(songs), &selected_model, &selected));
    gchar *actual = NULL; gtk_tree_model_get(selected_model, &selected, 0, &actual, -1); g_assert_cmpstr(actual, ==, entry_id); g_free(actual);
}

static JsonObject *atomic_rule(JsonObject *source, const gchar *entry_id)
{
    JsonArray *rules = json_object_get_array_member(source, "track_rules"); g_assert_nonnull(rules);
    for (guint i = 0; i < json_array_get_length(rules); ++i) {
        JsonObject *rule = json_array_get_object_element(rules, i);
        if (!g_strcmp0(radios_json_string(rule, "entry_id", ""), entry_id)) return rule;
    }
    g_error("Saved atomic rule is missing for selected real playlist entry %s", entry_id); return NULL;
}
static void assert_atomic_rule(JsonObject *source, const gchar *entry_id, gint weight, gint order,
                               gint frequency, gint max_plays, gint start, gint end, gint interval)
{
    JsonObject *rule = atomic_rule(source, entry_id); g_assert_cmpuint(json_object_get_size(rule), ==, 8);
    const gchar *names[] = {"weight", "order", "frequency", "max_plays", "start_minute", "end_minute", "interval_minutes"};
    const gint values[] = {weight, order, frequency, max_plays, start, end, interval};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) g_assert_cmpint(radios_json_int(rule, names[i], -1), ==, values[i]);
}
static JsonObject *atomic_source(JsonNode *calendar, const gchar *rotation_id, guint index)
{
    JsonArray *sources = json_object_get_array_member(named_definition(calendar, rotation_id), "sources");
    g_assert_cmpuint(json_array_get_length(sources), >, index); return json_array_get_object_element(sources, index);
}
static void assert_atomic_rule_controls(GtkWidget *dialog, gint weight, gint order, gint frequency,
                                       gint max_plays, const gchar *start, const gchar *end, gint interval)
{
    const gchar *names[] = {"song-weight", "song-order", "song-frequency", "song-max-plays", "song-interval"};
    const gint values[] = {weight, order, frequency, max_plays, interval};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        gchar *name = g_strconcat("rotation-editor-", names[i], NULL); GtkWidget *widget = control(dialog, name); g_free(name);
        reusable_reveal(widget);
        g_assert_true(GTK_IS_SPIN_BUTTON(widget)); g_assert_true(gtk_widget_get_mapped(widget));
        g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget)), ==, values[i]);
    }
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(dialog, "rotation-editor-song-start"))), ==, start);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(dialog, "rotation-editor-song-end"))), ==, end);
}

static void atomic_scroll_controls_into_view(GtkWidget *dialog, const gchar *const *names, guint count)
{
    for (guint i = 0; i < count; ++i) reusable_reveal(control(dialog, names[i]));
    GtkWidget *scroll = control(dialog, "rotation-editor-scroll"); g_assert_true(GTK_IS_SCROLLED_WINDOW(scroll));
    g_assert_false(gtk_scrolled_window_get_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll)));
    GtkPolicyType horizontal, vertical; gtk_scrolled_window_get_policy(GTK_SCROLLED_WINDOW(scroll), &horizontal, &vertical);
    g_assert_cmpint(horizontal, ==, GTK_POLICY_AUTOMATIC); g_assert_cmpint(vertical, ==, GTK_POLICY_ALWAYS);
    GtkWidget *viewport = gtk_bin_get_child(GTK_BIN(scroll)); g_assert_true(GTK_IS_VIEWPORT(viewport));
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)); settle_layout();
    gdouble page = gtk_adjustment_get_page_size(adjustment), value = gtk_adjustment_get_value(adjustment);
    g_assert_cmpfloat(gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_lower(adjustment), >, page);
    gdouble top = G_MAXDOUBLE, bottom = -G_MAXDOUBLE;
    for (guint i = 0; i < count; ++i) {
        GtkWidget *widget = control(dialog, names[i]); gint x = 0, y = 0;
        g_assert_true(gtk_widget_translate_coordinates(widget, viewport, 0, 0, &x, &y));
        top = MIN(top, y + value); bottom = MAX(bottom, y + value + gtk_widget_get_allocated_height(widget));
    }
    g_assert_cmpfloat(bottom - top + 12, <=, page);
    gtk_adjustment_clamp_page(adjustment, MAX(0, top - 6), bottom + 6); settle_layout();
    GtkWidget *bar = gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(scroll));
    g_assert_true(gtk_widget_get_visible(bar)); g_assert_true(gtk_widget_get_mapped(bar));
    g_assert_cmpint(gtk_widget_get_allocated_width(bar), >, 0); g_assert_cmpint(gtk_widget_get_allocated_height(bar), >, 0);
    for (guint i = 0; i < count; ++i) {
        GtkWidget *widget = control(dialog, names[i]); gint x = 0, y = 0;
        g_assert_true(gtk_widget_translate_coordinates(widget, viewport, 0, 0, &x, &y));
        g_assert_cmpint(x, >=, 0); g_assert_cmpint(y, >=, 0);
        g_assert_cmpint(x + gtk_widget_get_allocated_width(widget), <=, gtk_widget_get_allocated_width(viewport));
        g_assert_cmpint(y + gtk_widget_get_allocated_height(widget), <=, gtk_widget_get_allocated_height(viewport));
        g_test_message("Actual scrolled viewport control %s: %dx%d at %d,%d in %dx%d with visible nonoverlay bar",
            names[i], gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget), x, y,
            gtk_widget_get_allocated_width(viewport), gtk_widget_get_allocated_height(viewport));
    }
}
static void atomic_scroll_top(GtkWidget *dialog)
{
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(control(dialog, "rotation-editor-scroll")));
    gtk_adjustment_set_value(adjustment, gtk_adjustment_get_lower(adjustment)); settle_layout();
}

static void test_native_playlist_atomic_probability_rules_reopen_invalid(void)
{
    gchar *playlist = create_playlist("Atomic rule three-song playlist"), *other = create_playlist("Independent weighted source");
    Harness *harness = harness_open(); JsonNode *initial = snapshot_of(harness->page), *detail = verified_media(harness);
    bind_playlist_with_title(playlist, detail, "Atomic song A");
    bind_playlist_with_title(playlist, detail, "Atomic song B");
    bind_playlist_with_title(playlist, detail, "Atomic unrestricted C");
    bind_playlist_with_title(other, detail, "Atomic second source D"); radios_data_refresh(&harness->ctx);
    click(harness->page, "automation-calendar-independent-rotation"); GtkWidget *dialog = reusable_dialog();
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-name")), "Playlist ve atomik olasılık kuralları"); pump();
    JsonNode *snapshot = snapshot_of(harness->page); JsonArray *library = json_object_get_array_member(json_node_get_object(snapshot), "rotation_library");
    g_assert_cmpuint(json_array_get_length(library), ==, 1);
    gchar *rotation_id = g_strdup(radios_json_string(json_array_get_object_element(library, 0), "id", "")); json_node_free(snapshot);
    GtkWidget *settings = control(dialog, "rotation-editor-settings");
    gtk_expander_set_expanded(GTK_EXPANDER(settings), FALSE); settle_layout();
    g_assert_false(gtk_widget_get_visible(control(dialog, "rotation-editor-selection-mode")));
    reusable_combo(dialog, "rotation-editor-selection-mode", "weighted");
    PlaylistChoice choice = {control(dialog, "rotation-editor-playlist-combo"), playlist};
    wait_until(playlist_available, &choice, "first genuinely saved playlist available in atomic editor"); click(dialog, "rotation-editor-add");
    snapshot = snapshot_of(harness->page); JsonObject *source = atomic_source(snapshot, rotation_id, 0);
    g_assert_false(json_object_has_member(source, "track_rules")); json_node_free(snapshot);
    reusable_spin(dialog, "rotation-editor-source-frequency", 3);
    reusable_spin(dialog, "rotation-editor-source-count", 4);
    reusable_spin(dialog, "rotation-editor-source-weight", 75);
    reusable_spin(dialog, "rotation-editor-source-interval", 45);
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-source-start")), "08:15");
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-source-end")), "21:30");
    reusable_combo(dialog, "rotation-editor-source-track-order", "weighted");
    gchar *song_a = reusable_atomic_song_id(dialog, 0), *song_b = reusable_atomic_song_id(dialog, 1), *song_c = reusable_atomic_song_id(dialog, 2);
    reusable_select_atomic_song_id(dialog, song_a);
    reusable_spin(dialog, "rotation-editor-song-weight", 0);
    reusable_spin(dialog, "rotation-editor-song-order", 6);
    reusable_spin(dialog, "rotation-editor-song-frequency", 3);
    reusable_spin(dialog, "rotation-editor-song-max-plays", 2);
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-song-start")), "08:45");
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-song-end")), "18:00");
    reusable_spin(dialog, "rotation-editor-song-interval", 11);
    reusable_select_atomic_song_id(dialog, song_b);
    reusable_spin(dialog, "rotation-editor-song-weight", 90);
    reusable_spin(dialog, "rotation-editor-song-order", 2);
    reusable_spin(dialog, "rotation-editor-song-frequency", 2);
    reusable_spin(dialog, "rotation-editor-song-max-plays", 1);
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-song-start")), "09:30");
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-song-end")), "19:45");
    reusable_spin(dialog, "rotation-editor-song-interval", 17); wait_saved(harness);
    snapshot = snapshot_of(harness->page); source = atomic_source(snapshot, rotation_id, 0);
    g_assert_cmpstr(radios_json_string(source, "track_selection_mode", ""), ==, "weighted");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(source, "track_rules")), ==, 2);
    assert_atomic_rule(source, song_a, 0, 5, 3, 2, 525, 1080, 11);
    assert_atomic_rule(source, song_b, 90, 1, 2, 1, 570, 1185, 17);
    json_node_free(snapshot);
    const gchar *song_fields[] = {"rotation-editor-song-weight", "rotation-editor-song-order", "rotation-editor-song-frequency",
        "rotation-editor-song-max-plays", "rotation-editor-song-start", "rotation-editor-song-end", "rotation-editor-song-interval"};
    atomic_scroll_controls_into_view(dialog, song_fields, G_N_ELEMENTS(song_fields));
    Harness modal = {0}; modal.window = dialog; capture_to(&modal, g_getenv("RADIOS_ROTATION_ATOMIC_CAPTURE")); atomic_scroll_top(dialog);

    /* The simple playlist overview hides detailed controls. Reopening the
     * selected playlist and its advanced section exposes the same rules. */
    click(dialog, "rotation-editor-back");
    g_assert_false(gtk_widget_get_mapped(control(dialog, "rotation-editor-source-properties")));
    reusable_reveal(control(dialog, "rotation-editor-source-count"));
    g_assert_true(gtk_widget_get_mapped(control(dialog, "rotation-editor-source-properties")));
    g_assert_true(gtk_widget_get_mapped(control(dialog, "rotation-editor-source-count")));
    GtkWidget *summary = control(dialog, "rotation-editor-selected-source-summary"); g_assert_true(GTK_IS_LABEL(summary));
    g_assert_true(*gtk_label_get_text(GTK_LABEL(summary)) != '\0');
    reusable_spin(dialog, "rotation-editor-source-count", 3); wait_saved(harness);
    const gchar *source_fields[] = {"rotation-editor-source-frequency", "rotation-editor-source-count", "rotation-editor-source-weight",
        "rotation-editor-source-interval", "rotation-editor-source-start", "rotation-editor-source-end", "rotation-editor-source-track-order", "rotation-editor-source-media-kind"};
    atomic_scroll_controls_into_view(dialog, source_fields, G_N_ELEMENTS(source_fields));
    capture_to(&modal, g_getenv("RADIOS_ROTATION_SOURCE_CAPTURE")); atomic_scroll_top(dialog);
    choice.id = other; wait_until(playlist_available, &choice, "second genuinely saved source independently available"); click(dialog, "rotation-editor-add");
    reusable_spin(dialog, "rotation-editor-source-weight", 0); wait_saved(harness);
    snapshot = snapshot_of(harness->page); g_assert_cmpint(radios_json_int(atomic_source(snapshot, rotation_id, 1), "weight", -1), ==, 0); json_node_free(snapshot);
    reusable_spin(dialog, "rotation-editor-source-weight", 25);
    gchar *song_d = reusable_select_atomic_song(dialog, 0); reusable_spin(dialog, "rotation-editor-song-weight", 0); wait_saved(harness);
    snapshot = snapshot_of(harness->page); source = atomic_source(snapshot, rotation_id, 1);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(source, "track_rules")), ==, 1);
    assert_atomic_rule(source, song_d, 0, 0, 1, 0, 0, 1440, 0); json_node_free(snapshot);
    click(dialog, "rotation-editor-back"); reusable_select_source(dialog, 0); reusable_source_song_click(dialog, 0);
    reusable_select_atomic_song_id(dialog, song_b);
    assert_atomic_rule_controls(dialog, 90, 2, 2, 1, "09:30", "19:45", 17);

    /* Invalid focused atomic clocks have no persisted revision and cannot
     * be replaced by a late complete-definition acknowledgement or refresh. */
    wait_saved(harness); JsonNode *valid_record = calendar_record(); g_assert_nonnull(valid_record);
    GtkWidget *start = control(dialog, "rotation-editor-song-start"); gtk_widget_grab_focus(start);
    gtk_entry_set_text(GTK_ENTRY(start), "25:00"); gtk_editable_set_position(GTK_EDITABLE(start), 5); pump();
    g_assert_false(radios_rotation_editor_ready(control(dialog, "rotation-editor")));
    g_assert_null(radios_rotation_editor_snapshot(control(dialog, "rotation-editor"))); wait_saved(harness);
    JsonNode *invalid_record = calendar_record(); g_assert_true(json_node_equal(valid_record, invalid_record));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(start)), ==, "25:00");
    g_assert_cmpint(gtk_editable_get_position(GTK_EDITABLE(start)), ==, 5);
    json_node_free(valid_record); json_node_free(invalid_record);
    gtk_entry_set_text(GTK_ENTRY(start), "09:30"); pump(); wait_saved(harness);

    reusable_combo(dialog, "rotation-editor-source-track-order", "linear"); wait_saved(harness);
    snapshot = snapshot_of(harness->page); source = atomic_source(snapshot, rotation_id, 0);
    g_assert_cmpstr(radios_json_string(source, "track_selection_mode", ""), ==, "linear");
    assert_atomic_rule(source, song_a, 0, 5, 3, 2, 525, 1080, 11); assert_atomic_rule(source, song_b, 90, 1, 2, 1, 570, 1185, 17); json_node_free(snapshot);
    reusable_combo(dialog, "rotation-editor-source-track-order", "random"); wait_saved(harness);
    snapshot = snapshot_of(harness->page); source = atomic_source(snapshot, rotation_id, 0);
    g_assert_false(json_object_has_member(source, "track_selection_mode"));
    g_assert_true(json_object_get_boolean_member(source, "track_random"));
    assert_atomic_rule(source, song_a, 0, 5, 3, 2, 525, 1080, 11); assert_atomic_rule(source, song_b, 90, 1, 2, 1, 570, 1185, 17); json_node_free(snapshot);
    reusable_combo(dialog, "rotation-editor-source-track-order", "weighted");
    reusable_combo(dialog, "rotation-editor-selection-mode", "linear"); pump();
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control(dialog, "rotation-editor-source-weight"))), ==, 75);
    reusable_combo(dialog, "rotation-editor-selection-mode", "weighted");
    gtk_expander_set_expanded(GTK_EXPANDER(settings), TRUE); settle_layout();
    reusable_spin(dialog, "rotation-editor-track-gap", 4); reusable_spin(dialog, "rotation-editor-artist-gap", 6); reusable_spin(dialog, "rotation-editor-album-gap", 8); wait_saved(harness);
    snapshot = snapshot_of(harness->page); JsonObject *definition = named_definition(snapshot, rotation_id);
    assert_named_settings(json_object_get_object_member(definition, "settings"), "weighted", 4, 6, 8, 100, 0, 0, 3000, 3000);
    source = atomic_source(snapshot, rotation_id, 0);
    g_assert_cmpint(radios_json_int(source, "frequency", -1), ==, 3); g_assert_cmpint(radios_json_int(source, "count", -1), ==, 3);
    g_assert_cmpint(radios_json_int(source, "weight", -1), ==, 75); g_assert_cmpint(radios_json_int(source, "interval_minutes", -1), ==, 45);
    g_assert_cmpint(radios_json_int(source, "start_minute", -1), ==, 495); g_assert_cmpint(radios_json_int(source, "end_minute", -1), ==, 1290);
    assert_atomic_rule(source, song_a, 0, 5, 3, 2, 525, 1080, 11); assert_atomic_rule(source, song_b, 90, 1, 2, 1, 570, 1185, 17);
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(initial), "events"), json_object_get_member(json_node_get_object(snapshot), "events")));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump(); wait_saved(harness); harness_close(harness);
    harness = harness_open(); JsonNode *restored = snapshot_of(harness->page); g_assert_true(json_node_equal(snapshot, restored)); json_node_free(restored);
    PlaylistChoice saved_choice = {control(harness->page, "automation-calendar-existing-rotation"), rotation_id};
    wait_until(playlist_available, &saved_choice, "atomic definition survives real owner close and reauthentication");
    click(harness->page, "automation-calendar-manage-rotation"); dialog = reusable_dialog();
    reusable_select_source(dialog, 0); reusable_source_song_click(dialog, 0);
    reusable_select_atomic_song_id(dialog, song_a);
    assert_atomic_rule_controls(dialog, 0, 6, 3, 2, "08:45", "18:00", 11);
    reusable_select_atomic_song_id(dialog, song_b);
    assert_atomic_rule_controls(dialog, 90, 2, 2, 1, "09:30", "19:45", 17);
    reusable_select_atomic_song_id(dialog, song_c);
    assert_atomic_rule_controls(dialog, 100, 3, 1, 0, "00:00", "24:00", 0);
    JsonNode *editor = radios_rotation_editor_snapshot(control(dialog, "rotation-editor")); g_assert_nonnull(editor);
    g_assert_true(json_node_equal(editor, json_array_get_element(json_object_get_array_member(json_node_get_object(snapshot), "rotation_library"), 0))); json_node_free(editor);
    /* The shared reset removes only the selected source's sparse song rules.
     * Whole-calendar equality proves ordering, repeats, routing, other source
     * rules and calendar assignments survive the acknowledged operation. */
    JsonNode *expected_reset = snapshot_of(harness->page);
    json_object_remove_member(atomic_source(expected_reset, rotation_id, 0), "track_rules");
    GtkWidget *reset = control(dialog, "rotation-editor-source-reset-song-rules"); g_assert_true(GTK_IS_BUTTON(reset));
    reusable_reveal(reset);
    g_assert_true(gtk_widget_get_mapped(reset)); click(dialog, "rotation-editor-source-reset-song-rules"); wait_saved(harness);
    restored = snapshot_of(harness->page); g_assert_true(json_node_equal(restored, expected_reset));
    g_assert_false(json_object_has_member(atomic_source(restored, rotation_id, 0), "track_rules"));
    assert_atomic_rule(atomic_source(restored, rotation_id, 1), song_d, 0, 0, 1, 0, 0, 1440, 0); json_node_free(restored);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump(); harness_close(harness);
    harness = harness_open(); restored = snapshot_of(harness->page); g_assert_true(json_node_equal(restored, expected_reset)); json_node_free(restored);
    saved_choice.combo = control(harness->page, "automation-calendar-existing-rotation");
    wait_until(playlist_available, &saved_choice, "explicit song-rule reset survives actual owner reopen");
    click(harness->page, "automation-calendar-manage-rotation"); dialog = reusable_dialog();
    reusable_select_source(dialog, 0); reusable_source_song_click(dialog, 0);
    reusable_reveal(control(dialog, "rotation-editor-source-reset-song-rules"));
    g_assert_true(gtk_widget_get_mapped(control(dialog, "rotation-editor-source-reset-song-rules")));
    reusable_select_atomic_song_id(dialog, song_a);
    assert_atomic_rule_controls(dialog, 100, 1, 1, 0, "00:00", "24:00", 0);
    editor = radios_rotation_editor_snapshot(control(dialog, "rotation-editor")); g_assert_nonnull(editor);
    g_assert_true(json_node_equal(editor, json_array_get_element(json_object_get_array_member(json_node_get_object(expected_reset), "rotation_library"), 0))); json_node_free(editor);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump(); harness_close(harness); json_node_free(expected_reset);
    json_node_free(snapshot); json_node_free(initial); json_node_free(detail); g_free(playlist); g_free(other); g_free(rotation_id); g_free(song_a); g_free(song_b); g_free(song_c); g_free(song_d);
    g_test_message("Real native weighted playlist and sparse atomic song probabilities/order/frequency/caps/UTC windows/intervals, zero-weight intent, three-mode retention, invalid focused clock no-write, unchanged calendar events, scoped song-rule reset and owner reopen passed.");
}

/* This fixture shares the user's compositor. Physical typing into a briefly
 * focused test window must not edit its assertions; generated GDK events and
 * every non-key GTK event retain the normal native dispatch path. */
/* Real saved-programme UI acceptance. Every DB operation below uses the
 * existing isolated authenticated reader; the detached editor is production. */
static JsonNode *weekly_record(const gchar *id, const gchar *name)
{
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "kind", "weekly_programme");
    JsonNode *reply = request("/v1/workflows/desktop/list", body); json_node_free(body);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(reply), "records"); JsonNode *found = NULL;
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *record = json_array_get_object_element(rows, i);
        if ((id && !g_strcmp0(radios_json_string(record, "_id", ""), id)) ||
            (!id && name && !g_strcmp0(radios_json_string(record, "name", ""), name))) {
            g_assert_null(found); found = json_node_copy(json_array_get_element(rows, i));
        }
    }
    json_node_free(reply); return found;
}

static JsonNode *weekly_put(JsonNode *record, const gchar *name, JsonNode *configuration, const gchar *new_id)
{
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "weekly_programme");
    json_object_set_string_member(object, "name", name);
    if (record) {
        JsonObject *saved = json_node_get_object(record);
        json_object_set_string_member(object, "document_id", radios_json_string(saved, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(saved, "_rev", 0));
    } else if (new_id) {
        json_object_set_string_member(object, "document_id", new_id);
        json_object_set_int_member(object, "expected_revision", 0);
    }
    json_object_set_member(object, "config", json_node_copy(configuration));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonNode *reply = request("/v1/workflows/desktop/put", body); json_node_free(body); return reply;
}

static void weekly_private_diff(JsonNode *expected, JsonNode *actual, const gchar *path, guint *remaining);

static void weekly_main_unchanged(Harness *harness, JsonNode *record)
{
    JsonNode *current = calendar_record(); g_assert_nonnull(current);
    if (!json_node_equal(record, current)) {
        guint remaining = 16;
        weekly_private_diff(record, current, "main-record", &remaining);
    }
    g_assert_true(json_node_equal(record, current));
    JsonNode *snapshot = radios_automation_calendar_main_snapshot(control(harness->page, "automation-calendar-main"));
    g_assert_true(json_node_equal(snapshot, json_object_get_member(json_node_get_object(record), "config")));
    json_node_free(snapshot); json_node_free(current);
}

static void weekly_assignment_weight(GtkWidget *page, guint index, const gchar *value)
{
    GtkWidget *tree = control(page, "automation-calendar-rotations");
    GtkCellRenderer *renderer = g_object_get_data(G_OBJECT(tree), "automation-calendar-rotation-weight");
    g_assert_true(GTK_IS_CELL_RENDERER_TEXT(renderer));
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
    g_assert_cmpint(gtk_tree_model_get_column_type(model, 7), ==, G_TYPE_INT);
    gchar *path = g_strdup_printf("%u", index);
    g_signal_emit_by_name(renderer, "edited", path, value); g_free(path); pump();
}

static void weekly_assign(Harness *harness, const gchar *rotation)
{
    select_library_rotation(harness->page, rotation);
    click(harness->page, "automation-calendar-assign-rotation"); wait_saved(harness);
}

static gchar *weekly_definition(Harness *harness, const gchar *name, const gchar *playlist, gboolean atomic)
{
    click(harness->page, "automation-calendar-independent-rotation"); GtkWidget *dialog = reusable_dialog();
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-name")), name); pump();
    PlaylistChoice choice = {control(dialog, "rotation-editor-playlist-combo"), playlist};
    wait_until(playlist_available, &choice, "verified playlist for complete weekly snapshot");
    click(dialog, "rotation-editor-add");
    reusable_combo(dialog, "rotation-editor-selection-mode", "weighted");
    reusable_spin(dialog, "rotation-editor-source-weight", 75);
    reusable_spin(dialog, "rotation-editor-source-count", 2);
    reusable_spin(dialog, "rotation-editor-source-frequency", 3);
    reusable_spin(dialog, "rotation-editor-source-interval", 45);
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-source-start")), "08:15");
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-source-end")), "21:30");
    reusable_combo(dialog, "rotation-editor-source-track-order", "weighted");
    if (atomic) {
        gchar *entry = reusable_select_atomic_song(dialog, 0);
        reusable_spin(dialog, "rotation-editor-song-weight", 0);
        reusable_spin(dialog, "rotation-editor-song-order", 6);
        reusable_spin(dialog, "rotation-editor-song-frequency", 3);
        reusable_spin(dialog, "rotation-editor-song-max-plays", 2);
        gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-song-start")), "09:15");
        gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-song-end")), "18:30");
        reusable_spin(dialog, "rotation-editor-song-interval", 11); g_free(entry);
    }
    wait_saved(harness);
    JsonNode *snapshot = snapshot_of(harness->page);
    JsonArray *library = json_object_get_array_member(json_node_get_object(snapshot), "rotation_library");
    JsonObject *definition = json_array_get_object_element(library, json_array_get_length(library) - 1);
    gchar *id = g_strdup(radios_json_string(definition, "id", "")); g_assert_true(g_uuid_string_is_valid(id));
    json_node_free(snapshot); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump(); wait_saved(harness); return id;
}

static void weekly_two_week_main(Harness *harness)
{
    gchar *playlist = create_playlist("Haftalık program gerçek kaynakları");
    JsonNode *media = verified_media(harness);
    bind_playlist_with_title(playlist, media, "Weekly source A");
    bind_playlist_with_title(playlist, media, "Weekly source B"); radios_data_refresh(&harness->ctx);
    set_date(harness, "2026-11-02");
    gchar *first = weekly_definition(harness, "Haftalık atomik kurallar", playlist, TRUE);
    gchar *second = weekly_definition(harness, "Haftalık ikinci rotasyon", playlist, FALSE);
    new_event(harness, 0, 600, 660, "Birinci hafta", FALSE);
    weekly_assign(harness, first); weekly_assign(harness, second);
    gtk_combo_box_set_active(GTK_COMBO_BOX(control(harness->page, "automation-calendar-rotation-mode")), 1); pump();
    weekly_assignment_weight(harness->page, 0, "0"); weekly_assignment_weight(harness->page, 1, "100"); wait_saved(harness);
    set_date(harness, "2026-11-09"); new_event(harness, 2, 690, 720, "İkinci hafta", TRUE);
    weekly_assign(harness, second); weekly_assignment_weight(harness->page, 0, "55"); wait_saved(harness);
    /* Save from the first visible week must retain the second week as well. */
    set_date(harness, "2026-11-02"); wait_saved(harness);
    json_node_free(media); g_free(playlist); g_free(first); g_free(second);
}


static gboolean weekly_save_dialog_closed(gpointer unused)
{ (void)unused; return dialog_find("automation-programme-save-dialog") == NULL; }
static gboolean weekly_workspace_acknowledged(gpointer value)
{
    GtkWidget *workspace = value, *holder = find(workspace, "weekly-programme-editor-holder");
    GPtrArray *documents = holder ? g_object_get_data(G_OBJECT(holder), "radios-data-documents") : NULL;
    return holder && gtk_widget_is_sensitive(holder) && documents && documents->len == 1 &&
        radios_data_document_revision(g_ptr_array_index(documents, 0)) > 0 &&
        strstr(gtk_label_get_text(GTK_LABEL(control(workspace, "weekly-programme-status"))), "veritabanına kaydedildi") != NULL;
}

static JsonNode *weekly_save_as(Harness *harness, const gchar *name)
{
    click(harness->page, "automation-calendar-save-programme");
    GtkWidget *dialog = dialog_find("automation-programme-save-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "automation-programme-save-name")), name);
    click(dialog, "automation-programme-save-apply");
    JsonNode *record = NULL; gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!record && g_get_monotonic_time() < deadline) { pump(); record = weekly_record(NULL, name); }
    g_assert_nonnull(record); g_assert_cmpint(radios_json_int(json_node_get_object(record), "_rev", 0), ==, 1);
    wait_until(weekly_save_dialog_closed, NULL, "genuine save acknowledgement closes save-as dialog"); return record;
}

static void test_native_save_full_weekly_programme_source_unchanged(void)
{
    Harness *harness = harness_open(); weekly_two_week_main(harness);
    JsonNode *before = calendar_record(); JsonNode *snapshot = snapshot_of(harness->page);
    JsonNode *record = weekly_save_as(harness, "İki haftalık tam program");
    JsonObject *programme = json_object_get_object_member(json_node_get_object(record), "config");
    g_assert_cmpint(radios_json_int(programme, "schema", 0), ==, 1);
    g_assert_cmpstr(radios_json_string(programme, "week_anchor", ""), ==, "2026-11-02");
    g_assert_cmpint(radios_json_int(programme, "week_count", 0), ==, 2);
    g_assert_false(json_object_get_boolean_member(programme, "repeat"));
    g_assert_true(json_node_equal(snapshot, json_object_get_member(programme, "calendar")));
    g_assert_cmpuint(json_array_get_length(events(json_object_get_member(programme, "calendar"))), ==, 2);
    JsonObject *event = event_at(json_object_get_member(programme, "calendar"), "2026-11-02", 600);
    g_assert_cmpstr(radios_json_string(event, "rotation_selection_mode", ""), ==, "weighted");
    JsonArray *assignments = json_object_get_array_member(event, "rotations");
    g_assert_cmpint(radios_json_int(json_array_get_object_element(assignments, 0), "weight", -1), ==, 0);
    g_assert_cmpint(radios_json_int(json_array_get_object_element(assignments, 1), "weight", -1), ==, 100);
    radios_automation_calendar_select(harness->page, 0, 600, 660, FALSE); pump();
    JsonNode *invalid_before = calendar_record();
    weekly_assignment_weight(harness->page, 0, "101"); settle_layout();
    weekly_main_unchanged(harness, invalid_before); json_node_free(invalid_before);
    set_date(harness, "2026-11-09"); set_date(harness, "2026-11-02"); weekly_main_unchanged(harness, before);
    /* A later main edit must not mutate the independent acknowledged snapshot. */
    radios_automation_calendar_select(harness->page, 0, 600, 660, FALSE);
    entry_set(harness, "automation-calendar-programme-name", "Ana takvim sonradan değişti"); wait_saved(harness);
    JsonNode *saved = weekly_record(radios_json_string(json_node_get_object(record), "_id", ""), NULL);
    g_assert_true(json_node_equal(record, saved)); json_node_free(saved);
    capture_to(harness, g_getenv("RADIOS_WEEKLY_MAIN_CAPTURE"));
    harness_close(harness); harness = harness_open();
    saved = weekly_record(radios_json_string(json_node_get_object(record), "_id", ""), NULL);
    g_assert_true(json_node_equal(record, saved)); json_node_free(saved); harness_close(harness);
    json_node_free(before); json_node_free(snapshot); json_node_free(record);
}

static gboolean weekly_documents_ready(gpointer value)
{
    GtkWidget *workspace = value, *holder = find(workspace, "weekly-programme-editor-holder");
    GPtrArray *documents = holder ? g_object_get_data(G_OBJECT(holder), "radios-data-documents") : NULL;
    return holder && gtk_widget_is_sensitive(holder) && documents && documents->len == 1 &&
        radios_data_document_ready(g_ptr_array_index(documents, 0)) &&
        radios_data_document_revision(g_ptr_array_index(documents, 0)) > 0;
}

static GtkWidget *weekly_workspace(Harness *harness, const gchar *id)
{
    g_assert_null(find(harness->page, "automation-calendar-tabs")); settle_layout();
    GtkWidget *workspace = control(harness->page, "weekly-programme-workspace");
    PlaylistChoice choice = {control(workspace, "weekly-programme-choice"), id};
    wait_until(playlist_available, &choice, "actual saved programme selected by immutable UUID");
    wait_until(weekly_documents_ready, workspace, "UUID-pinned programme document loaded"); settle_layout(); return workspace;
}

static JsonNode *weekly_workspace_snapshot(GtkWidget *workspace)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(node, object);
    json_object_set_int_member(object, "schema", 1);
    json_object_set_string_member(object, "week_anchor", gtk_entry_get_text(GTK_ENTRY(control(workspace, "weekly-programme-anchor"))));
    json_object_set_int_member(object, "week_count", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control(workspace, "weekly-programme-weeks"))));
    json_object_set_boolean_member(object, "repeat", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control(workspace, "weekly-programme-repeat"))));
    json_object_set_member(object, "calendar", snapshot_of(control(workspace, "weekly-programme-calendar"))); return node;
}

static void weekly_private_diff(JsonNode *expected, JsonNode *actual, const gchar *path, guint *remaining)
{
    if (!*remaining || (expected && actual && json_node_equal(expected, actual))) return;
    if (expected && actual && JSON_NODE_HOLDS_OBJECT(expected) && JSON_NODE_HOLDS_OBJECT(actual)) {
        JsonObject *left = json_node_get_object(expected), *right = json_node_get_object(actual);
        GList *members = json_object_get_members(left);
        for (GList *item = members; item && *remaining; item = item->next) {
            gchar *child = g_strdup_printf("%s.%s", path, (const gchar *)item->data);
            weekly_private_diff(json_object_get_member(left, item->data), json_object_get_member(right, item->data), child, remaining); g_free(child);
        }
        g_list_free(members); members = json_object_get_members(right);
        for (GList *item = members; item && *remaining; item = item->next) if (!json_object_has_member(left, item->data)) {
            gchar *child = g_strdup_printf("%s.%s", path, (const gchar *)item->data);
            weekly_private_diff(NULL, json_object_get_member(right, item->data), child, remaining); g_free(child);
        }
        g_list_free(members); return;
    }
    if (expected && actual && JSON_NODE_HOLDS_ARRAY(expected) && JSON_NODE_HOLDS_ARRAY(actual)) {
        JsonArray *left = json_node_get_array(expected), *right = json_node_get_array(actual);
        guint a = json_array_get_length(left), b = json_array_get_length(right);
        if (a != b) { g_test_message("Private weekly mismatch %s.length: expected%u actual%u", path, a, b); --*remaining; }
        for (guint i = 0; i < MIN(a, b) && *remaining; ++i) {
            gchar *child = g_strdup_printf("%s[%u]", path, i);
            weekly_private_diff(json_array_get_element(left, i), json_array_get_element(right, i), child, remaining); g_free(child);
        }
        return;
    }
    gchar *left = expected ? json_to_string(expected, FALSE) : g_strdup("<missing>");
    gchar *right = actual ? json_to_string(actual, FALSE) : g_strdup("<missing>");
    g_test_message("Private weekly mismatch %s: expected%.240s actual%.240s", path, left, right);
    --*remaining; g_free(left); g_free(right);
}

static JsonNode *weekly_wait_saved(GtkWidget *workspace, const gchar *id)
{
    JsonNode *expected = weekly_workspace_snapshot(workspace), *record = NULL;
    GtkWidget *page = control(workspace, "weekly-programme-calendar");
    g_test_message("Private weekly save checkpoint: id=%s name=%s anchor=%s weeks=%d event=%s date=%s start=%s end=%s status=%s", id,
        gtk_entry_get_text(GTK_ENTRY(control(workspace, "weekly-programme-name"))),
        gtk_entry_get_text(GTK_ENTRY(control(workspace, "weekly-programme-anchor"))),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control(workspace, "weekly-programme-weeks"))),
        gtk_entry_get_text(GTK_ENTRY(control(page, "automation-calendar-programme-name"))),
        gtk_entry_get_text(GTK_ENTRY(control(page, "automation-calendar-programme-date"))),
        gtk_entry_get_text(GTK_ENTRY(control(page, "automation-calendar-programme-start"))),
        gtk_entry_get_text(GTK_ENTRY(control(page, "automation-calendar-programme-end"))),
        gtk_label_get_text(GTK_LABEL(control(workspace, "weekly-programme-status"))));
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline) {
        pump(); if (record) json_node_free(record); record = weekly_record(id, NULL);
        if (record && json_node_equal(expected, json_object_get_member(json_node_get_object(record), "config"))) break;
        gint64 next = MIN(deadline, g_get_monotonic_time() + 100 * 1000);
        while (g_get_monotonic_time() < next) pump();
    }
    g_assert_nonnull(record);
    if (!json_node_equal(expected, json_object_get_member(json_node_get_object(record), "config"))) {
        guint remaining = 32;
        weekly_private_diff(expected, json_object_get_member(json_node_get_object(record), "config"), "config", &remaining);
        g_test_message("Private weekly failure status: %s", gtk_label_get_text(GTK_LABEL(control(workspace, "weekly-programme-status"))));
    }
    g_assert_true(json_node_equal(expected, json_object_get_member(json_node_get_object(record), "config")));
    wait_until(weekly_workspace_acknowledged, workspace, "complete clean acknowledged pinned programme CAS revision");
    json_node_free(record); record = weekly_record(id, NULL);
    g_assert_true(json_node_equal(expected, json_object_get_member(json_node_get_object(record), "config")));
    json_node_free(expected); return record;
}


static void weekly_create_draft_two_weeks(Harness *harness)
{
    g_assert_null(find(harness->page, "automation-calendar-tabs")); settle_layout();
    GtkWidget *workspace = control(harness->page, "weekly-programme-workspace");
    click(workspace, "weekly-programme-new");
    GtkWidget *name = control(workspace, "weekly-programme-name");
    g_assert_true(gtk_editable_get_editable(GTK_EDITABLE(name)));
    gtk_entry_set_text(GTK_ENTRY(name), "Program sekmesinden ayrı iki haftalık taslak");
    gtk_entry_set_text(GTK_ENTRY(control(workspace, "weekly-programme-anchor")), "2026-11-02");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(workspace, "weekly-programme-weeks")), 2); pump();
    GtkWidget *page = control(workspace, "weekly-programme-calendar"); Harness embedded = *harness; embedded.page = page;
    JsonNode *calendar = snapshot_of(page); g_assert_cmpuint(json_array_get_length(events(calendar)), ==, 0);
    JsonArray *library = json_object_get_array_member(json_node_get_object(calendar), "rotation_library");
    g_assert_cmpuint(json_array_get_length(library), ==, 2);
    gchar *rotation = g_strdup(radios_json_string(json_array_get_object_element(library, 1), "id", "")); json_node_free(calendar);
    const gchar *dates[] = {"2026-11-02", "2026-11-09"};
    for (guint week = 0; week < 2; ++week) {
        set_date(&embedded, dates[week]); radios_automation_calendar_select(page, week, 900, 930, FALSE);
        click(page, "automation-calendar-save-selection");
        GtkWidget *dialog = dialog_find("automation-calendar-event-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
        gtk_entry_set_text(GTK_ENTRY(control(dialog, "automation-calendar-event-name")), week ? "Yeni program ikinci hafta" : "Yeni program ilk hafta");
        gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); pump();
        select_library_rotation(page, rotation);
        click(page, "automation-calendar-assign-rotation");
        gtk_combo_box_set_active(GTK_COMBO_BOX(control(page, "automation-calendar-rotation-mode")), 1); pump();
        weekly_assignment_weight(page, 0, week ? "64" : "36");
    }
    JsonNode *expected = weekly_workspace_snapshot(workspace);
    click(workspace, "weekly-programme-save");
    JsonNode *record = NULL; gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!record && g_get_monotonic_time() < deadline) { pump(); record = weekly_record(NULL, "Program sekmesinden ayrı iki haftalık taslak"); }
    g_assert_nonnull(record); g_assert_true(json_node_equal(expected, json_object_get_member(json_node_get_object(record), "config")));
    wait_until(weekly_workspace_acknowledged, workspace, "new pinned programme first save acknowledged before choosing another record");
    g_assert_false(gtk_editable_get_editable(GTK_EDITABLE(name)));
    g_assert_cmpuint(json_array_get_length(events(json_object_get_member(json_node_get_object(expected), "calendar"))), ==, 2);
    json_node_free(expected); json_node_free(record); g_free(rotation);
}

static void test_native_programme_isolated_weighted_two_week_reopen_CAS(void)
{
    const gchar *name = "Aynı isimli bağımsız haftalık program";
    Harness *harness = harness_open(); weekly_two_week_main(harness);
    JsonNode *main_record = calendar_record(), *record = weekly_save_as(harness, name);
    gchar *id = g_strdup(radios_json_string(json_node_get_object(record), "_id", "")), *decoy_id = g_uuid_string_random();
    JsonNode *decoy = weekly_put(NULL, name, json_object_get_member(json_node_get_object(record), "config"), decoy_id);
    radios_data_refresh(&harness->ctx);
    g_test_message("Private weekly stage: create separate two-week draft");
    weekly_create_draft_two_weeks(harness); weekly_main_unchanged(harness, main_record);
    g_test_message("Private weekly stage: select acknowledged saved original UUID");
    GtkWidget *workspace = weekly_workspace(harness, id);
    GtkWidget *name_field = control(workspace, "weekly-programme-name"); g_assert_true(GTK_IS_ENTRY(name_field));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(name_field)), ==, name); g_assert_false(gtk_editable_get_editable(GTK_EDITABLE(name_field)));
    GtkWidget *page = control(workspace, "weekly-programme-calendar"); Harness embedded = *harness; embedded.page = page;
    set_date(&embedded, "2026-11-02"); radios_automation_calendar_select(page, 0, 600, 660, FALSE); pump();
    gtk_combo_box_set_active(GTK_COMBO_BOX(control(page, "automation-calendar-rotation-mode")), 0); pump();
    weekly_assignment_weight(page, 0, "25"); weekly_assignment_weight(page, 1, "75");
    entry_set(&embedded, "automation-calendar-programme-name", "Program ilk hafta düzenlendi");
    JsonNode *saved = weekly_wait_saved(workspace, id); json_node_free(saved);
    set_date(&embedded, "2026-11-09"); radios_automation_calendar_select(page, 2, 690, 720, FALSE); pump();
    gtk_combo_box_set_active(GTK_COMBO_BOX(control(page, "automation-calendar-rotation-mode")), 1); pump();
    weekly_assignment_weight(page, 0, "37"); entry_set(&embedded, "automation-calendar-programme-name", "Program ikinci hafta düzenlendi");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(workspace, "weekly-programme-repeat")), TRUE);
    saved = weekly_wait_saved(workspace, id);
    JsonObject *configuration = json_object_get_object_member(json_node_get_object(saved), "config");
    JsonNode *calendar = json_object_get_member(configuration, "calendar");
    JsonObject *first = event_at(calendar, "2026-11-02", 600), *second = event_at(calendar, "2026-11-11", 690);
    g_assert_cmpstr(radios_json_string(first, "rotation_selection_mode", ""), ==, "linear");
    g_assert_cmpstr(radios_json_string(second, "rotation_selection_mode", ""), ==, "weighted");
    g_assert_cmpint(radios_json_int(json_array_get_object_element(json_object_get_array_member(second, "rotations"), 0), "weight", -1), ==, 37);
    g_assert_true(json_object_get_boolean_member(configuration, "repeat"));
    GtkWidget *anchor = control(workspace, "weekly-programme-anchor");
    gtk_widget_grab_focus(anchor); gtk_entry_set_text(GTK_ENTRY(anchor), "2026-11-03");
    gtk_editable_set_position(GTK_EDITABLE(anchor), 10);
    for (guint i = 0; i < 5; ++i) settle_layout();
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(anchor)), ==, "2026-11-03");
    g_assert_cmpint(gtk_editable_get_position(GTK_EDITABLE(anchor)), ==, 10);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(control(workspace, "weekly-programme-status"))), "Pazartesi"));
    JsonNode *invalid_saved = weekly_record(id, NULL); g_assert_true(json_node_equal(saved, invalid_saved)); json_node_free(invalid_saved);
    gtk_entry_set_text(GTK_ENTRY(anchor), "2026-11-02");
    JsonNode *repaired = weekly_wait_saved(workspace, id); json_node_free(saved); saved = repaired;
    wait_until(weekly_workspace_acknowledged, workspace, "valid Monday repair acknowledged without losing focused draft");
    const gchar *invalid_fields[] = {"automation-calendar-programme-name", "automation-calendar-programme-date",
        "automation-calendar-programme-start", "automation-calendar-programme-end"};
    const gchar *invalid_values[] = {"", "2026-11-40", "25:00", "09:00"};
    const gchar *repaired_values[] = {"Program ikinci hafta düzenlendi", "2026-11-11", "11:30", "12:00"};
    for (guint field = 0; field < G_N_ELEMENTS(invalid_fields); ++field) {
        GtkWidget *input = control(page, invalid_fields[field]); gtk_widget_grab_focus(input);
        gtk_entry_set_text(GTK_ENTRY(input), invalid_values[field]); gtk_editable_set_position(GTK_EDITABLE(input), -1); pump();
        g_assert_false(radios_automation_calendar_snapshot_ready(page));
        click(workspace, "weekly-programme-save");
        for (guint i = 0; i < 5; ++i) settle_layout();
        g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(input)), ==, invalid_values[field]);
        g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(control(workspace, "weekly-programme-status"))), "geçersiz"));
        JsonNode *unchanged = weekly_record(id, NULL); g_assert_true(json_node_equal(saved, unchanged)); json_node_free(unchanged);
        gtk_entry_set_text(GTK_ENTRY(input), repaired_values[field]); pump(); g_assert_true(radios_automation_calendar_snapshot_ready(page));
        repaired = weekly_wait_saved(workspace, id); json_node_free(saved); saved = repaired;
    }
    weekly_main_unchanged(harness, main_record);
    JsonNode *other = weekly_record(decoy_id, NULL); g_assert_true(json_node_equal(decoy, other)); json_node_free(other);
    capture_to(harness, g_getenv("RADIOS_WEEKLY_PROGRAM_CAPTURE")); harness_close(harness); harness = harness_open();
    workspace = weekly_workspace(harness, id); JsonNode *restored = weekly_workspace_snapshot(workspace);
    g_assert_true(json_node_equal(restored, json_object_get_member(json_node_get_object(saved), "config"))); json_node_free(restored);
    weekly_main_unchanged(harness, main_record);
    /* Another real client wins CAS; preserve the losing focused local draft. */
    page = control(workspace, "weekly-programme-calendar"); embedded = *harness; embedded.page = page;
    set_date(&embedded, "2026-11-09"); radios_automation_calendar_select(page, 2, 690, 720, FALSE); pump();
    JsonNode *external = weekly_workspace_snapshot(workspace);
    JsonObject *external_config = json_node_get_object(external);
    json_object_set_string_member(event_at(json_object_get_member(external_config, "calendar"), "2026-11-11", 690), "title", "Başka istemci haftalık programı kazandı");
    JsonNode *reopened_record = weekly_record(id, NULL);
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(saved), "config"),
        json_object_get_member(json_node_get_object(reopened_record), "config")));
    JsonNode *won = weekly_put(reopened_record, name, external, NULL); json_node_free(reopened_record);
    entry_set(&embedded, "automation-calendar-programme-name", "Çatışmada haftalık yerel taslak");
    wait_until(reload_open, NULL, "actual weekly autosave CAS conflict opens explicit reload");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(control(page, "automation-calendar-programme-name"))), ==, "Çatışmada haftalık yerel taslak");
    other = weekly_record(id, NULL); g_assert_true(json_node_equal(other, won)); json_node_free(other);
    gtk_dialog_response(GTK_DIALOG(dialog_find("database-reload-dialog")), GTK_RESPONSE_CANCEL); pump(); settle_layout();
    weekly_main_unchanged(harness, main_record);
    other = weekly_record(decoy_id, NULL); g_assert_true(json_node_equal(decoy, other)); json_node_free(other);
    harness_close(harness); harness = harness_open(); workspace = weekly_workspace(harness, id);
    restored = weekly_workspace_snapshot(workspace); g_assert_true(json_node_equal(external, restored)); json_node_free(restored);
    weekly_main_unchanged(harness, main_record); harness_close(harness);
    json_node_free(main_record); json_node_free(record); json_node_free(saved); json_node_free(decoy); json_node_free(external); json_node_free(won);
    g_free(id); g_free(decoy_id);
}

static void test_native_weekly_first_save_immediate_close_recovery(void)
{
    const gchar *name = "Kapanışta korunması gereken ilk haftalık kayıt";
    Harness *harness = harness_open(); weekly_two_week_main(harness);
    /* Earlier acknowledged main-calendar intents may still need their exact
     * durable reconciliation. Drain them through the real recovery API before
     * measuring the one new weekly intent, and prove the main record stays
     * byte-for-byte equivalent through that idempotent recovery. */
    JsonNode *baseline_record = calendar_record(); g_assert_nonnull(baseline_record);
    radios_data_recovery_retry(&harness->ctx);
    wait_until(recovery_empty, harness, "genuine preparation recovery reaches pending0 and blocked0");
    wait_saved(harness);
    JsonNode *main_record = calendar_record(), *expected = json_node_new(JSON_NODE_OBJECT);
    g_assert_true(json_node_equal(baseline_record, main_record)); json_node_free(baseline_record);
    JsonObject *configuration = json_object_new(); json_node_take_object(expected, configuration);
    json_object_set_int_member(configuration, "schema", 1);
    json_object_set_string_member(configuration, "week_anchor", "2026-11-02");
    json_object_set_int_member(configuration, "week_count", 2);
    json_object_set_boolean_member(configuration, "repeat", FALSE);
    json_object_set_member(configuration, "calendar", snapshot_of(harness->page));
    guint pending_before = pending_count(harness); g_assert_cmpuint(pending_before, ==, 0);
    RuntimeGate gate = {0}; gate_start(harness, &gate);
    click(harness->page, "automation-calendar-save-programme");
    GtkWidget *dialog = dialog_find("automation-programme-save-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "automation-programme-save-name")), name);
    click(dialog, "automation-programme-save-apply");
    GPtrArray *documents = g_object_get_data(G_OBJECT(dialog), "radios-data-documents");
    g_assert_nonnull(documents); g_assert_cmpuint(documents->len, ==, 1);
    RadioSDocument *document = g_ptr_array_index(documents, 0);
    g_assert_true(radios_data_document_ready(document));
    gchar *id = g_strdup(radios_data_document_id(document)); g_assert_true(g_uuid_string_is_valid(id));
    g_assert_cmpuint(pending_count(harness), ==, pending_before + 1);
    JsonNode *uncommitted = weekly_record(id, NULL); g_assert_null(uncommitted);
    /* The real worker is still blocked before HTTP. Immediate owner close
     * cancels it; no initial baseline read/ACK may rescue this first intent. */
    harness_close(harness);
    Recovery recovery = {0}; GThread *thread = g_thread_new("weekly-first-save-durable-recover", recovery_worker, &recovery);
    wait_until(recovery_done, &recovery, "real persisted first weekly save recovered after cancellation"); g_thread_join(thread);
    g_assert_no_error(recovery.error); g_assert_cmpuint(recovery.recovered, ==, 1);
    JsonNode *record = weekly_record(id, NULL); g_assert_nonnull(record);
    g_assert_cmpstr(radios_json_string(json_node_get_object(record), "name", ""), ==, name);
    g_assert_cmpint(radios_json_int(json_node_get_object(record), "_rev", 0), ==, 1);
    g_assert_true(json_node_equal(expected, json_object_get_member(json_node_get_object(record), "config")));
    JsonNode *main_after = calendar_record(); g_assert_true(json_node_equal(main_record, main_after)); json_node_free(main_after);
    Recovery replay = {0}; thread = g_thread_new("weekly-first-save-no-duplicate-recover", recovery_worker, &replay);
    wait_until(recovery_done, &replay, "second actual recovery proves first intent is not duplicated"); g_thread_join(thread);
    g_assert_no_error(replay.error); g_assert_cmpuint(replay.recovered, ==, 0);
    JsonNode *again = weekly_record(id, NULL); g_assert_true(json_node_equal(record, again)); json_node_free(again);
    harness = harness_open(); GtkWidget *workspace = weekly_workspace(harness, id);
    JsonNode *restored = weekly_workspace_snapshot(workspace); g_assert_true(json_node_equal(expected, restored)); json_node_free(restored);
    weekly_main_unchanged(harness, main_record); harness_close(harness);
    json_node_free(main_record); json_node_free(expected); json_node_free(record); g_free(id);
}

static void test_native_pdf_single_grid_multiselect_schema_reopen(void)
{
    Harness *harness = harness_open(); set_date(harness, "2026-11-02");
    new_event(harness, 0, 600, 660, "PDF tek ekran programı", FALSE);
    g_assert_null(find(harness->page, "automation-calendar-tabs"));
    GtkWidget *workspace = control(harness->page, "weekly-programme-workspace");
    g_assert_true(g_object_get_data(G_OBJECT(workspace), "weekly-programme-calendar") == harness->page);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(control(workspace, "weekly-programme-choice"))), ==, "main");
    const gchar *create[] = {"automation-calendar-independent-rotation", "automation-calendar-create-rotation-schema"};
    for (guint i = 0; i < G_N_ELEMENTS(create); ++i) {
        click(harness->page, create[i]);
        if (i) {
            g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(control(harness->page, create[i]))), ==, "Şema Ayarları");
            GtkWidget *chooser = dialog_find("automation-calendar-schema-settings-dialog"); g_assert_true(GTK_IS_DIALOG(chooser));
            gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_APPLY); pump();
        }
        GtkWidget *dialog = reusable_dialog();
        gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-name")), i ? "PDF otomasyon şeması" : "PDF birinci rotasyon");
        pump(); wait_saved(harness); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump();
    }
    JsonNode *snapshot = snapshot_of(harness->page); JsonArray *library = json_object_get_array_member(json_node_get_object(snapshot), "rotation_library");
    g_assert_cmpuint(json_array_get_length(library), ==, 2);
    JsonObject *settings = json_object_get_object_member(json_array_get_object_element(library, 1), "settings");
    g_assert_true(json_object_get_boolean_member(settings, "automation_schema")); json_node_free(snapshot);
    GtkWidget *picker = control(harness->page, "automation-calendar-library-picker");
    g_assert_false(gtk_expander_get_expanded(GTK_EXPANDER(picker)));
    gtk_expander_set_expanded(GTK_EXPANDER(picker), TRUE); settle_layout();
    GtkWidget *available = control(harness->page, "automation-calendar-available-rotations"); GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(available));
    gtk_tree_selection_unselect_all(selection); pump();
    g_assert_false(gtk_widget_is_sensitive(control(harness->page, "automation-calendar-assign-rotation")));
    for (guint i = 0; i < 2; ++i) { GtkTreePath *path = gtk_tree_path_new_from_indices((gint)i, -1); gtk_tree_selection_select_path(selection, path); gtk_tree_path_free(path); }
    g_assert_cmpint(gtk_tree_selection_count_selected_rows(selection), ==, 2);
    click(harness->page, "automation-calendar-assign-rotation"); wait_saved(harness);
    snapshot = snapshot_of(harness->page); JsonObject *event = event_at(snapshot, "2026-11-02", 600);
    JsonArray *rows = json_object_get_array_member(event, "rotations"); g_assert_cmpuint(json_array_get_length(rows), ==, 2);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(rows, 0), "rotation_id", ""), !=, radios_json_string(json_array_get_object_element(rows, 1), "rotation_id", ""));
    JsonNode *before = snapshot_of(harness->page);
    /* A joint assign that would add2 onto49 fails as a whole. */
    JsonArray *filled = json_object_get_array_member(event, "rotations");
    const gchar *ref = radios_json_string(json_array_get_object_element(filled, 0), "rotation_id", ""); gchar *saved_ref = g_strdup(ref);
    while (json_array_get_length(filled) < 49) {
        JsonObject *row = json_object_new(); gchar *id = g_uuid_string_random(); json_object_set_string_member(row, "id", id); g_free(id);
        json_object_set_string_member(row, "rotation_id", saved_ref); json_array_add_object_element(filled, row);
    }
    g_free(saved_ref); radios_automation_calendar_set_snapshot(harness->page, json_node_get_object(snapshot));
    radios_automation_calendar_select(harness->page, 0, 600, 660, FALSE); pump();
    for (guint i = 0; i < 2; ++i) { GtkTreePath *path = gtk_tree_path_new_from_indices((gint)i, -1); gtk_tree_selection_select_path(selection, path); gtk_tree_path_free(path); }
    click(harness->page, "automation-calendar-assign-rotation");
    JsonNode *rejected = snapshot_of(harness->page); g_assert_true(json_node_equal(snapshot, rejected)); json_node_free(rejected);
    radios_automation_calendar_set_snapshot(harness->page, json_node_get_object(before)); radios_automation_calendar_select(harness->page, 0, 600, 660, FALSE); pump();
    gtk_expander_set_expanded(GTK_EXPANDER(picker), FALSE); settle_layout();
    assert_programme_panel(harness); capture_to(harness, g_getenv("RADIOS_PDF_UNIFIED_CAPTURE"));
    click(harness->page, "automation-calendar-programme-probability");
    GtkWidget *settings_pages = control(harness->page, "automation-calendar-pages");
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(settings_pages)), ==, "probability");
    GtkWidget *probability_page = control(harness->page, "automation-calendar-probability-page");
    g_assert_true(gtk_widget_get_toplevel(probability_page) == harness->window);
    g_assert_false(GTK_IS_WINDOW(probability_page));
    capture_to(harness, g_getenv("RADIOS_AUTOMATION_PROBABILITY_CAPTURE"));
    click(probability_page, "automation-calendar-probability-back");
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(settings_pages)), ==, "calendar");
    JsonNode *after_probability = snapshot_of(harness->page);
    g_assert_true(json_node_equal(before, after_probability)); json_node_free(after_probability);
    json_node_free(snapshot); harness_close(harness); harness = harness_open();
    JsonNode *reopened = snapshot_of(harness->page); g_assert_true(json_node_equal(before, reopened));
    json_node_free(before); json_node_free(reopened); harness_close(harness);
}

/* Kılavuz §04 adım 1-2 (akım 4c): a "Live event" programme persists its
 * live settings in the genuine saved document (contract §1b), lists rotation
 * schemas only and reopens the same panel from "Program özellikleri". */
/* Kullanıcı isteği U4: the live strips are chosen in a folding tree; its
 * rows read "[group] Kanal n · name · source;" for the assertions. */
static gchar *channel_tree_rows(GtkWidget *picker)
{
    /* The tree lives in the picker's popover, outside the dialog's widget tree. */
    GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(picker)));
    gchar *name = g_strdup_printf("%s-tree", gtk_widget_get_name(picker)); GtkWidget *tree = control(popover, name); g_free(name);
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); GString *text = g_string_new(NULL); GtkTreeIter group, child;
    for (gboolean more = gtk_tree_model_get_iter_first(model, &group); more; more = gtk_tree_model_iter_next(model, &group)) {
        gchar *title = NULL; gtk_tree_model_get(model, &group, 2, &title, -1); g_string_append_printf(text, "[%s]", title); g_free(title);
        for (gboolean kids = gtk_tree_model_iter_children(model, &child, &group); kids; kids = gtk_tree_model_iter_next(model, &child)) {
            gchar *row = NULL; gtk_tree_model_get(model, &child, 2, &row, -1); g_string_append_printf(text, " %s;", row); g_free(row);
        }
    }
    return g_string_free(text, FALSE);
}
static GtkWidget *live_channels_of(GtkWidget *dialog)
{ GtkWidget *picker = control(dialog, "automation-calendar-event-live-channels"); g_assert_true(GTK_IS_MENU_BUTTON(picker)); return picker; }
static gboolean live_check_active(GtkWidget *dialog, guint number)
{
    JsonArray *chosen = radios_ui_channel_picker_selected(live_channels_of(dialog)); gboolean found = FALSE;
    for (guint i = 0; i < json_array_get_length(chosen); ++i) found |= json_array_get_int_element(chosen, i) == number;
    json_array_unref(chosen); return found;
}
/* Kılavuz s.4 adım 5 / s.7: the DJ/Konuşma/Müzik label of the chosen mixer
 * strips sets the live event's label, i.e. what its L does. A genuine
 * dj_mixer record labels strips 3 and 4 DJ and strip 6 Müzik. */
static JsonObject *mixer_effect_slot(void)
{
    JsonObject *effect = json_object_new();
    json_object_set_int_member(effect, "type", 0); json_object_set_boolean_member(effect, "enabled", FALSE);
    json_object_set_double_member(effect, "depth", .5); json_object_set_double_member(effect, "send_db", 0);
    json_object_set_double_member(effect, "return_db", 0); json_object_set_int_member(effect, "monitor_path", 0);
    json_object_set_double_member(effect, "tempo_bpm", 120); return effect;
}
static void mixer_labels_save(void)
{
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *config = json_object_new();
    JsonArray *channels = json_array_new();
    for (guint i = 0; i < 32; ++i) {
        JsonObject *strip = json_object_new();
        const gchar *levels[] = {"eq_low_db", "eq_mid_db", "eq_high_db", "pan", "gain_db", "fx_send_db", "fx_return_db"};
        json_object_set_int_member(strip, "source_deck", -1);
        for (guint k = 0; k < G_N_ELEMENTS(levels); ++k) json_object_set_double_member(strip, levels[k], 0);
        json_object_set_boolean_member(strip, "mute", FALSE); json_object_set_boolean_member(strip, "solo", FALSE);
        json_object_set_int_member(strip, "crossfade_group", 0); json_object_set_int_member(strip, "fx_slot", -1);
        if (i == 2 || i == 3) json_object_set_string_member(strip, "label", "dj");
        if (i == 5) json_object_set_string_member(strip, "label", "music");
        json_array_add_object_element(channels, strip);
    }
    json_object_set_array_member(config, "channels", channels);
    JsonObject *master = json_object_new();
    const gchar *master_levels[] = {"eq_low_db", "eq_mid_db", "eq_high_db", "fx_send_db", "fx_return_db"};
    for (guint k = 0; k < G_N_ELEMENTS(master_levels); ++k) json_object_set_double_member(master, master_levels[k], 0);
    json_object_set_boolean_member(master, "mute", FALSE); json_object_set_int_member(master, "fx_slot", -1);
    json_object_set_object_member(config, "master", master);
    JsonArray *effects = json_array_new(); for (guint i = 0; i < 5; ++i) json_array_add_object_element(effects, mixer_effect_slot());
    json_object_set_array_member(config, "effects", effects); json_object_set_object_member(config, "master_fx", mixer_effect_slot());
    json_object_set_double_member(config, "crossfader", 0);
    JsonObject *speech = json_object_new(); json_object_set_boolean_member(speech, "ducking", FALSE); json_object_set_double_member(speech, "bed_db", -12);
    json_object_set_object_member(config, "speech", speech);
    json_object_set_string_member(object, "kind", "dj_mixer"); json_object_set_string_member(object, "name", "DJ mixer");
    json_object_set_object_member(object, "config", config);
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonNode *saved = request("/v1/workflows/desktop/put", body); json_node_free(saved); json_node_free(body);
}
static const gchar *live_label_of(GtkWidget *dialog)
{ return gtk_combo_box_get_active_id(GTK_COMBO_BOX(control(dialog, "automation-calendar-event-live-label"))); }
static void live_channel_set(GtkWidget *dialog, guint number, gboolean active)
{
    if (live_check_active(dialog, number) != active) radios_ui_channel_picker_toggle(live_channels_of(dialog), number);
    pump();
}
/* The calendar reads the mixer asynchronously (on connect and at each dialog
 * opening): reopen the new-programme dialog until strip 3 shows its label. */
static GtkWidget *live_dialog_with_labels(Harness *harness)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    for (;;) {
        pointer_select(harness, 2, 840, 900, TRUE); click(harness->page, "automation-calendar-save-selection");
        GtkWidget *dialog = dialog_find("automation-calendar-event-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
        gchar *rows = channel_tree_rows(live_channels_of(dialog)); gboolean labelled = strstr(rows, "[DJ] Kanal 3;") != NULL;
        if (labelled) { g_free(rows); return dialog; }
        if (g_get_monotonic_time() > deadline) g_error("Mixer strip labels never reached the live panel (tree: '%s')", rows);
        g_free(rows); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); pump();
        for (gint64 until = g_get_monotonic_time() + 200 * G_TIME_SPAN_MILLISECOND; g_get_monotonic_time() < until;) pump();
    }
}
static void test_native_live_event_label_follows_mixer_channels(void)
{
    mixer_labels_save();
    Harness *harness = harness_open(); set_date(harness, "2026-12-07");
    GtkWidget *dialog = live_dialog_with_labels(harness);
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "automation-calendar-event-name")), "Etiketli canlı");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(dialog, "automation-calendar-event-live")), TRUE); settle_layout();
    g_assert_cmpstr(live_label_of(dialog), ==, "talk");
    /* A DJ strip makes the event a DJ event; a second DJ strip agrees. */
    live_channel_set(dialog, 3, TRUE); g_assert_cmpstr(live_label_of(dialog), ==, "dj");
    live_channel_set(dialog, 4, TRUE); g_assert_cmpstr(live_label_of(dialog), ==, "dj");
    /* A Müzik strip beside them is a mixed choice: the label stays. */
    live_channel_set(dialog, 6, TRUE); g_assert_cmpstr(live_label_of(dialog), ==, "dj");
    /* Only the Müzik strip left: the event becomes a music event. */
    live_channel_set(dialog, 3, FALSE); live_channel_set(dialog, 4, FALSE); g_assert_cmpstr(live_label_of(dialog), ==, "music");
    /* An unlabelled strip does not count. */
    live_channel_set(dialog, 1, TRUE); g_assert_cmpstr(live_label_of(dialog), ==, "music");
    /* The operator may still choose another label; it is saved as chosen. */
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(dialog, "automation-calendar-event-live-label")), "talk"));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); pump(); wait_saved(harness);
    JsonNode *snapshot = snapshot_of(harness->page); JsonObject *event = event_at(snapshot, "2026-12-09", 840); g_assert_nonnull(event);
    JsonObject *saved = json_object_get_object_member(event, "live"); g_assert_nonnull(saved);
    g_assert_cmpstr(radios_json_string(saved, "label", ""), ==, "talk");
    JsonArray *channels = json_object_get_array_member(saved, "channels"); g_assert_cmpuint(json_array_get_length(channels), ==, 2);
    g_assert_cmpint(json_array_get_int_element(channels, 0), ==, 1); g_assert_cmpint(json_array_get_int_element(channels, 1), ==, 6);
    json_node_free(snapshot);
    /* Reopening shows the saved label: loading the strips never moves it. */
    radios_automation_calendar_select(harness->page, 2, 840, 900, FALSE); pump();
    block_action(harness, 2, 840, "automation-calendar-programme-properties");
    dialog = dialog_find("automation-calendar-event-dialog"); g_assert_true(GTK_IS_DIALOG(dialog)); settle_layout();
    g_assert_cmpstr(live_label_of(dialog), ==, "talk");
    g_assert_true(live_check_active(dialog, 1)); g_assert_true(live_check_active(dialog, 6)); g_assert_false(live_check_active(dialog, 3));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); pump();
    harness_close(harness);
}
/* Kullanıcı isteği U4: the schema editor's channel tree lists the strips
 * the station defined — here the labelled strips 3, 4 and 6 of a genuine
 * dj_mixer record and the DJ page's eight default strips — grouped by label
 * instead of 32 fixed rows. */
static void test_native_schema_channel_tree_lists_defined_strips(void)
{
    mixer_labels_save();
    Harness *harness = harness_open(); set_date(harness, "2026-12-14");
    click(harness->page, "automation-calendar-create-rotation-schema");
    GtkWidget *chooser = dialog_find("automation-calendar-schema-settings-dialog"); g_assert_true(GTK_IS_DIALOG(chooser));
    gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_APPLY); pump();
    GtkWidget *editor = reusable_dialog(), *picker = control(editor, "schema-settings-step-channels");
    g_assert_true(GTK_IS_MENU_BUTTON(picker));
    /* The calendar reads the mixer asynchronously; the open editor follows. */
    const gchar *expected = "[DJ] Kanal 3; Kanal 4;[Müzik] Kanal 6;[Etiketsiz] Kanal 1; Kanal 2; Kanal 5; Kanal 7; Kanal 8;";
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND; gchar *rows = channel_tree_rows(picker);
    while (g_strcmp0(rows, expected) && g_get_monotonic_time() < deadline) {
        g_free(rows); for (gint64 until = g_get_monotonic_time() + 200 * G_TIME_SPAN_MILLISECOND; g_get_monotonic_time() < until;) pump();
        rows = channel_tree_rows(picker);
    }
    g_assert_cmpstr(rows, ==, expected); g_free(rows);
    gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_CLOSE); pump();
    harness_close(harness);
}
static void test_native_live_event_programme_schema_list_reopen(void)
{
    Harness *harness = harness_open(); set_date(harness, "2026-11-09");
    const gchar *create[] = {"automation-calendar-independent-rotation", "automation-calendar-create-rotation-schema"};
    for (guint i = 0; i < G_N_ELEMENTS(create); ++i) {
        click(harness->page, create[i]);
        if (i) {
            GtkWidget *chooser = dialog_find("automation-calendar-schema-settings-dialog"); g_assert_true(GTK_IS_DIALOG(chooser));
            gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_APPLY); pump();
        }
        GtkWidget *editor = reusable_dialog();
        gtk_entry_set_text(GTK_ENTRY(control(editor, "rotation-editor-name")), i ? "Canlı sohbet şeması" : "Sabah müziği");
        pump(); wait_saved(harness); gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_CLOSE); pump();
    }
    JsonNode *snapshot = snapshot_of(harness->page); JsonArray *library = json_object_get_array_member(json_node_get_object(snapshot), "rotation_library");
    g_assert_cmpuint(json_array_get_length(library), ==, 2);
    gchar *rotation_id = g_strdup(radios_json_string(json_array_get_object_element(library, 0), "id", ""));
    gchar *schema_id = g_strdup(radios_json_string(json_array_get_object_element(library, 1), "id", "")); json_node_free(snapshot);
    pointer_select(harness, 1, 600, 660, TRUE); click(harness->page, "automation-calendar-save-selection");
    GtkWidget *dialog = dialog_find("automation-calendar-event-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
    GtkWidget *name = control(dialog, "automation-calendar-event-name"); gtk_entry_set_text(GTK_ENTRY(name), "Sabah sohbeti");
    GtkWidget *live = control(dialog, "automation-calendar-event-live"), *panel = control(dialog, "automation-calendar-event-live-panel");
    g_assert_false(gtk_widget_get_visible(panel));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(live), TRUE); settle_layout();
    g_assert_true(gtk_widget_get_mapped(panel));
    GtkAllocation name_box; gint name_x = 0, panel_x = 0; gtk_widget_get_allocation(name, &name_box);
    g_assert_true(gtk_widget_translate_coordinates(name, dialog, 0, 0, &name_x, NULL));
    g_assert_true(gtk_widget_translate_coordinates(panel, dialog, 0, 0, &panel_x, NULL));
    g_assert_cmpint(panel_x, >=, name_x + name_box.width);
    GtkWidget *schema = control(dialog, "automation-calendar-event-live-schema");
    g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(schema), rotation_id));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(schema), schema_id));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(dialog, "automation-calendar-event-live-label")), "dj"));
    live_channel_set(dialog, 2, TRUE); live_channel_set(dialog, 5, TRUE);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(dialog, "automation-calendar-event-live-end")), "duration"));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(dialog, "automation-calendar-event-live-duration")), 45);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); pump(); wait_saved(harness);
    snapshot = snapshot_of(harness->page); JsonObject *event = event_at(snapshot, "2026-11-10", 600);
    JsonObject *saved = json_object_get_object_member(event, "live"); g_assert_nonnull(saved);
    g_assert_cmpstr(radios_json_string(saved, "label", ""), ==, "dj");
    JsonArray *channels = json_object_get_array_member(saved, "channels"); g_assert_cmpuint(json_array_get_length(channels), ==, 2);
    g_assert_cmpint(json_array_get_int_element(channels, 0), ==, 2); g_assert_cmpint(json_array_get_int_element(channels, 1), ==, 5);
    g_assert_cmpstr(radios_json_string(saved, "automation_id", ""), ==, schema_id);
    g_assert_cmpstr(radios_json_string(saved, "end_rule", ""), ==, "duration"); g_assert_cmpint(radios_json_int(saved, "duration_ms", 0), ==, 45 * 60000);
    JsonArray *rows = json_object_get_array_member(event, "rotations"); g_assert_cmpuint(json_array_get_length(rows), ==, 1);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(rows, 0), "rotation_id", ""), ==, schema_id);
    json_node_free(snapshot);
    /* Kılavuz §04 adım 2: the live programme shows rotation schemas only. */
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(control(harness->page, "automation-calendar-rotation-title"))), ==, "2  Rotasyon şemaları");
    GtkWidget *picker = control(harness->page, "automation-calendar-library-picker"); gtk_expander_set_expanded(GTK_EXPANDER(picker), TRUE); settle_layout();
    GtkTreeModel *available = gtk_tree_view_get_model(GTK_TREE_VIEW(control(harness->page, "automation-calendar-available-rotations")));
    g_assert_cmpint(gtk_tree_model_iter_n_children(available, NULL), ==, 1);
    GtkTreeIter first; gchar *listed = NULL; g_assert_true(gtk_tree_model_get_iter_first(available, &first)); gtk_tree_model_get(available, &first, 0, &listed, -1);
    g_assert_cmpstr(listed, ==, schema_id); g_free(listed);
    gtk_expander_set_expanded(GTK_EXPANDER(picker), FALSE); settle_layout();
    JsonNode *before = snapshot_of(harness->page); harness_close(harness); harness = harness_open(); set_date(harness, "2026-11-09");
    JsonNode *reopened = snapshot_of(harness->page); g_assert_true(json_node_equal(before, reopened)); json_node_free(reopened);
    radios_automation_calendar_select(harness->page, 1, 600, 660, FALSE); pump();
    block_action(harness, 1, 600, "automation-calendar-programme-properties");
    dialog = dialog_find("automation-calendar-event-dialog"); g_assert_true(GTK_IS_DIALOG(dialog)); settle_layout();
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control(dialog, "automation-calendar-event-live"))));
    g_assert_true(gtk_widget_get_mapped(control(dialog, "automation-calendar-event-live-panel")));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(control(dialog, "automation-calendar-event-live-label"))), ==, "dj");
    g_assert_true(live_check_active(dialog, 2)); g_assert_true(live_check_active(dialog, 5)); g_assert_false(live_check_active(dialog, 1));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(control(dialog, "automation-calendar-event-live-schema"))), ==, schema_id);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(control(dialog, "automation-calendar-event-live-end"))), ==, "duration");
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control(dialog, "automation-calendar-event-live-duration"))), ==, 45);
    /* Unchecking the box removes the live settings in the saved document. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(dialog, "automation-calendar-event-live")), FALSE);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); pump(); wait_saved(harness);
    snapshot = snapshot_of(harness->page); event = event_at(snapshot, "2026-11-10", 600);
    g_assert_false(json_object_has_member(event, "live")); json_node_free(snapshot);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(control(harness->page, "automation-calendar-rotation-title"))), ==, "2  Rotasyonlar");
    json_node_free(before); g_free(rotation_id); g_free(schema_id); harness_close(harness);
}

static void fixture_event(GdkEvent *event, gpointer value)
{
    (void)value;
    if (!event->any.send_event && (event->type == GDK_KEY_PRESS || event->type == GDK_KEY_RELEASE)) return;
    gtk_main_do_event(event);
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *project_root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc; g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    gdk_event_handler_set(fixture_event, NULL, NULL);
    GError *error = NULL;
    g_assert_true(radios_config_load(project_root, NULL, &config, &error)); g_assert_no_error(error); g_free(project_root);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    theme = gtk_css_provider_new(); GString *css = g_string_new(NULL);
    for (guint i = 0; radios_reference_css_parts[i]; ++i) g_string_append(css, radios_reference_css_parts[i]);
    gchar *theme_path = g_build_filename(config.assets_root, "themes", "radios.css", NULL), *adaptation = NULL;
    g_assert_true(g_file_get_contents(theme_path, &adaptation, NULL, &error)); g_assert_no_error(error);
    g_string_append(css, adaptation); g_free(theme_path); g_free(adaptation);
    g_assert_true(gtk_css_provider_load_from_data(theme, css->str, (gssize)css->len, &error)); g_assert_no_error(error); g_string_free(css, TRUE);
    gchar *font_path = g_build_filename(config.assets_root, "fonts", "InterVariable.woff2", NULL);
    g_assert_true(FcInit()); g_assert_true(FcConfigAppFontAddFile(NULL, (const FcChar8 *)font_path)); g_free(font_path);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(theme), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_test_add_func("/native-automation-calendar/live-event-programme-schema-list-reopen", test_native_live_event_programme_schema_list_reopen);
    g_test_add_func("/native-automation-calendar/pdf-single-grid-multiselect-schema-reopen", test_native_pdf_single_grid_multiselect_schema_reopen);
    g_test_add_func("/native-automation-calendar/weekly-first-save-immediate-close-durable-recovery", test_native_weekly_first_save_immediate_close_recovery);
    g_test_add_func("/native-automation-calendar/save-full-weekly-programme-snapshot-source-unchanged", test_native_save_full_weekly_programme_source_unchanged);
    g_test_add_func("/native-automation-calendar/programme-isolated-two-week-weighted-edit-reopen-CAS", test_native_programme_isolated_weighted_two_week_reopen_CAS);
    g_test_add_func("/native-automation-calendar/reusable-rotation-library-songs-settings", test_native_reusable_rotation_library_songs_settings);
    g_test_add_func("/native-automation-calendar/playlist-atomic-probability-rules-invalid-reopen", test_native_playlist_atomic_probability_rules_reopen_invalid);
    g_test_add_func("/native-automation-calendar/selection-rotation-copy-persistence", test_calendar_selection_rotation_copy_persistence);
    g_test_add_func("/native-automation-calendar/verified-rotation-preview", test_native_verified_rotation_preview);
    g_test_add_func("/native-automation-calendar/context-drag-properties-autosave", test_native_context_drag_properties_autosave);
    g_test_add_func("/native-automation-calendar/rotation-edit-in-place", test_native_rotation_edit_in_place);
    g_test_add_func("/native-automation-calendar/repeat-rules-tabs-persistence-override", test_native_repeat_rules_tabs_persistence_override);
    g_test_add_func("/native-automation-calendar/live-event-label-follows-mixer-channels", test_native_live_event_label_follows_mixer_channels);
    g_test_add_func("/native-automation-calendar/schema-channel-tree-lists-defined-strips", test_native_schema_channel_tree_lists_defined_strips);
    /* Deliberate unresolved CAS leaves a durable blocked ancestor on this
     * shared calendar resource. Verify independent close recovery first. */
    g_test_add_func("/native-automation-calendar/pending-close-recovery", test_native_pending_close_recovery);
    g_test_add_func("/native-automation-calendar/autosave-burst-cas-conflict", test_native_autosave_burst_cas_conflict);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(theme); radios_config_clear(&config); return result;
}
