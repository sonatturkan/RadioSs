#include "radios/views.h"
#include "radios/runtime.h"
#include "radios/broadcast_header.h"
#include "radios/programme_queue.h"
#include "radios/playout_ui.h"
#include <string.h>

/* Kılavuz 2026-10-18 §06/§08: the broadcast flow's "⚙ Ayarlar" button sits at
 * the far right of the ⇈ ↑ ↓ ⇊ order row, opens the five-field view dialog,
 * rejects fetch counts below the shown counts and applies accepted counts to
 * the queue window and the playout preparation window at once. The real
 * broadcast page is built without audio, playout or a data connection, so the
 * record save is refused and only the local application is observed here. */

typedef struct { RadioSViewContext ctx; GtkWidget *window, *page; } Harness;

static guint notices, window_calls, playout_window_calls;
static guint last_event_show, last_content_show, last_event_fetch, last_content_fetch;

void __real_radios_programme_queue_set_window(GtkWidget *queue, guint event_show, guint content_show);
void __wrap_radios_programme_queue_set_window(GtkWidget *queue, guint event_show, guint content_show)
{
    ++window_calls; last_event_show = event_show; last_content_show = content_show;
    __real_radios_programme_queue_set_window(queue, event_show, content_show);
}
void __real_radios_playout_ui_set_programme_window(RadioSViewContext *ctx, guint event_fetch, guint content_fetch);
void __wrap_radios_playout_ui_set_programme_window(RadioSViewContext *ctx, guint event_fetch, guint content_fetch)
{
    ++playout_window_calls; last_event_fetch = event_fetch; last_content_fetch = content_fetch;
    __real_radios_playout_ui_set_programme_window(ctx, event_fetch, content_fetch);
}

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *find_button(GtkWidget *root, const gchar *label)
{
    if (GTK_IS_BUTTON(root) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), label)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find_button(item->data, label);
    g_list_free(children); return result;
}
static void run_for(guint milliseconds)
{
    gint64 end = g_get_monotonic_time() + (gint64)milliseconds * 1000;
    do { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); } while (g_get_monotonic_time() < end);
}
static void notify(gpointer data, const gchar *message)
{ (void)data; g_assert_nonnull(message); ++notices; }
static Harness *harness_new(void)
{
    notices = window_calls = playout_window_calls = 0;
    Harness *h = g_new0(Harness, 1); h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(h->window);
    gtk_window_set_default_size(GTK_WINDOW(h->window), 1000, 700);
    h->ctx.parent = GTK_WINDOW(h->window); h->ctx.runtime = radios_runtime_new(8, 32); h->ctx.notify = notify;
    h->page = radios_broadcast_new(&h->ctx); gtk_container_add(GTK_CONTAINER(h->window), h->page);
    gtk_widget_show_all(h->window); run_for(150); return h;
}
static void harness_free(Harness *h)
{
    gtk_widget_destroy(h->window); radios_runtime_free(h->ctx.runtime); g_object_unref(h->window); g_free(h);
    while (g_main_context_iteration(NULL, FALSE)) {}
}
static GtkWidget *dialog_find(void)
{
    GList *toplevels = gtk_window_list_toplevels(); GtkWidget *dialog = NULL;
    for (GList *item = toplevels; item && !dialog; item = item->next)
        if (!g_strcmp0(gtk_widget_get_name(item->data), "programme-view-settings-dialog")) dialog = item->data;
    g_list_free(toplevels); return dialog;
}
static GtkWidget *dialog_open(Harness *h)
{
    GtkWidget *button = find(h->page, "programme-view-settings"); g_assert_true(GTK_IS_BUTTON(button));
    gtk_button_clicked(GTK_BUTTON(button)); run_for(60);
    GtkWidget *dialog = dialog_find(); g_assert_nonnull(dialog);
    /* Five non-wrapping rows and the buttons: no half-empty dialog. */
    gint height = 0; gtk_window_get_size(GTK_WINDOW(dialog), NULL, &height);
    g_assert_cmpint(height, <, 480);
    return dialog;
}
static void spin_set(GtkWidget *dialog, const gchar *name, gdouble value)
{
    GtkWidget *spin = find(dialog, name); g_assert_true(GTK_IS_SPIN_BUTTON(spin));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
}
static gint spin_get(GtkWidget *dialog, const gchar *name)
{
    GtkWidget *spin = find(dialog, name); g_assert_true(GTK_IS_SPIN_BUTTON(spin));
    return gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
}
static void spin_range(GtkWidget *dialog, const gchar *name, gdouble low, gdouble high)
{
    GtkWidget *spin = find(dialog, name); gdouble minimum = 0, maximum = 0; g_assert_true(GTK_IS_SPIN_BUTTON(spin));
    gtk_spin_button_get_range(GTK_SPIN_BUTTON(spin), &minimum, &maximum);
    g_assert_cmpfloat(minimum, ==, low); g_assert_cmpfloat(maximum, ==, high);
}

static void test_settings_button_far_right_of_order_row(void)
{
    Harness *h = harness_new();
    GtkWidget *settings = find(h->page, "programme-view-settings"), *last = find(h->page, "programme-last");
    g_assert_true(GTK_IS_BUTTON(settings)); g_assert_true(GTK_IS_BUTTON(last));
    GtkWidget *row = gtk_widget_get_parent(settings);
    g_assert_true(row == gtk_widget_get_parent(last)); g_assert_true(GTK_IS_BOX(row));
    /* The order controls stay packed from the left; only Ayarlar is packed at the end. */
    const gchar *order[] = {"programme-first", "programme-up", "programme-down", "programme-last"};
    for (guint i = 0; i < G_N_ELEMENTS(order); ++i) {
        GtkPackType packing = GTK_PACK_START; gboolean expand = TRUE, fill = TRUE; guint padding = 0;
        GtkWidget *control = find(h->page, order[i]); g_assert_true(gtk_widget_get_parent(control) == row);
        gtk_box_query_child_packing(GTK_BOX(row), control, &expand, &fill, &padding, &packing);
        g_assert_cmpint(packing, ==, GTK_PACK_START);
    }
    GtkPackType packing = GTK_PACK_START; gboolean expand = TRUE, fill = TRUE; guint padding = 0;
    gtk_box_query_child_packing(GTK_BOX(row), settings, &expand, &fill, &padding, &packing);
    g_assert_cmpint(packing, ==, GTK_PACK_END);
    g_assert_true(gtk_widget_get_visible(settings)); g_assert_true(gtk_widget_get_mapped(settings));
    /* Rightmost child of the row: every other control ends before it starts. */
    GtkAllocation gear = {0}, right = {0}, box = {0};
    gtk_widget_get_allocation(settings, &gear); gtk_widget_get_allocation(last, &right); gtk_widget_get_allocation(row, &box);
    g_assert_cmpint(gear.width, >, 0); g_assert_cmpint(gear.x, >=, right.x + right.width);
    g_assert_cmpint(gear.x + gear.width, <=, box.x + box.width);
    GList *children = gtk_container_get_children(GTK_CONTAINER(row));
    for (GList *item = children; item; item = item->next) {
        GtkAllocation other = {0}; if (item->data == settings) continue;
        gtk_widget_get_allocation(item->data, &other); g_assert_cmpint(other.x + other.width, <=, gear.x);
    }
    g_list_free(children);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(settings)), ==, "⚙ Ayarlar");
    g_assert_null(dialog_find());
    harness_free(h);
}

static void test_dialog_fields_defaults_and_cancel(void)
{
    Harness *h = harness_new(); guint before = window_calls;
    GtkWidget *dialog = dialog_open(h);
    g_assert_true(GTK_IS_DIALOG(dialog)); g_assert_true(gtk_window_get_modal(GTK_WINDOW(dialog)));
    g_assert_cmpint(spin_get(dialog, "programme-view-event-fetch"), ==, 12);
    g_assert_cmpint(spin_get(dialog, "programme-view-event-show"), ==, 8);
    g_assert_cmpint(spin_get(dialog, "programme-view-content-fetch"), ==, 50);
    g_assert_cmpint(spin_get(dialog, "programme-view-content-show"), ==, 20);
    g_assert_cmpint(spin_get(dialog, "programme-view-refresh"), ==, 5);
    spin_range(dialog, "programme-view-event-fetch", 1, 500); spin_range(dialog, "programme-view-event-show", 1, 200);
    spin_range(dialog, "programme-view-content-fetch", 1, 500); spin_range(dialog, "programme-view-content-show", 1, 500);
    spin_range(dialog, "programme-view-refresh", 1, 60);
    g_assert_nonnull(find_button(dialog, "Vazgeç")); g_assert_nonnull(find_button(dialog, "Kaydet"));
    GtkWidget *status = find(dialog, "programme-view-status");
    g_assert_true(GTK_IS_LABEL(status)); g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(status)), ==, "");
    gpointer alive = dialog; g_object_add_weak_pointer(G_OBJECT(dialog), &alive);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); run_for(60);
    g_assert_null(alive); g_assert_null(dialog_find());
    g_assert_cmpuint(window_calls, ==, before);
    harness_free(h);
}

static void test_fetch_below_show_is_rejected_in_status_line(void)
{
    Harness *h = harness_new(); guint before = window_calls, playout_before = playout_window_calls;
    GtkWidget *dialog = dialog_open(h); gpointer alive = dialog; g_object_add_weak_pointer(G_OBJECT(dialog), &alive);
    spin_set(dialog, "programme-view-event-fetch", 4);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); run_for(60);
    g_assert_nonnull(alive);
    GtkWidget *status = find(dialog, "programme-view-status");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(status)), "Event çekme adedi"));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(status)), "küçük olamaz"));
    g_assert_cmpuint(window_calls, ==, before); g_assert_cmpuint(playout_window_calls, ==, playout_before);
    spin_set(dialog, "programme-view-event-fetch", 12); spin_set(dialog, "programme-view-content-fetch", 10);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); run_for(60);
    g_assert_nonnull(alive);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(status)), "İçerik çekme adedi"));
    g_assert_cmpuint(window_calls, ==, before);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); run_for(60); g_assert_null(alive);
    harness_free(h);
}

static void test_saved_counts_apply_to_queue_and_playout_window(void)
{
    Harness *h = harness_new();
    RadioSBroadcastHeaderSnapshot header = {0};
    g_assert_true(radios_broadcast_header_snapshot(&h->ctx, &header));
    g_assert_false(header.selected); g_assert_cmpint(header.selected_duration_ms, ==, -1);
    GtkWidget *dialog = dialog_open(h); gpointer alive = dialog; g_object_add_weak_pointer(G_OBJECT(dialog), &alive);
    guint before = window_calls, playout_before = playout_window_calls, notices_before = notices;
    spin_set(dialog, "programme-view-event-show", 4); spin_set(dialog, "programme-view-content-fetch", 30);
    spin_set(dialog, "programme-view-content-show", 10); spin_set(dialog, "programme-view-refresh", 7);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); run_for(60);
    g_assert_null(alive); g_assert_null(dialog_find());
    g_assert_cmpuint(window_calls, >, before); g_assert_cmpuint(playout_window_calls, >, playout_before);
    g_assert_cmpuint(last_event_show, ==, 4); g_assert_cmpuint(last_content_show, ==, 10);
    g_assert_cmpuint(last_event_fetch, ==, 12); g_assert_cmpuint(last_content_fetch, ==, 30);
    /* Without a programme record the save is refused; the operator is told. */
    g_assert_cmpuint(notices, >, notices_before);
    dialog = dialog_open(h);
    g_assert_cmpint(spin_get(dialog, "programme-view-event-fetch"), ==, 12);
    g_assert_cmpint(spin_get(dialog, "programme-view-event-show"), ==, 4);
    g_assert_cmpint(spin_get(dialog, "programme-view-content-fetch"), ==, 30);
    g_assert_cmpint(spin_get(dialog, "programme-view-content-show"), ==, 10);
    g_assert_cmpint(spin_get(dialog, "programme-view-refresh"), ==, 7);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); run_for(60);
    harness_free(h);
}

static void test_refresh_timer_preserves_unchanged_rows(void)
{
    Harness *h = harness_new();
    GtkWidget *dialog = dialog_open(h);
    spin_set(dialog, "programme-view-refresh", 1);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); run_for(60);
    GtkWidget *queue = find(h->page, "programme-queue-table"); g_assert_nonnull(queue);
    GtkListBox *list = g_object_get_data(G_OBJECT(queue), "list-box"); g_assert_true(GTK_IS_LIST_BOX(list));
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, 0); g_assert_nonnull(row);
    gpointer alive = row; g_object_add_weak_pointer(G_OBJECT(row), &alive);
    guint before = window_calls, playout_before = playout_window_calls;
    /* A seconds timer may coalesce its first firing by nearly one second.
     * An unchanged/offline queue must keep its row controls and user gestures. */
    run_for(2300);
    g_assert_nonnull(alive);
    g_assert_true(gtk_list_box_get_row_at_index(list, 0) == row);
    g_assert_cmpuint(window_calls, ==, before);
    g_assert_cmpuint(playout_window_calls, ==, playout_before);
    g_object_remove_weak_pointer(G_OBJECT(row), &alive);
    harness_free(h);
}

int main(int argc, char **argv)
{
    if (!gtk_init_check(&argc, &argv)) { g_printerr("Private GTK test skipped: no display.\n"); return 77; }
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/programme-view/settings-button-far-right-of-order-row", test_settings_button_far_right_of_order_row);
    g_test_add_func("/programme-view/dialog-fields-defaults-cancel", test_dialog_fields_defaults_and_cancel);
    g_test_add_func("/programme-view/fetch-below-show-rejected", test_fetch_below_show_is_rejected_in_status_line);
    g_test_add_func("/programme-view/saved-counts-apply-window", test_saved_counts_apply_to_queue_and_playout_window);
    g_test_add_func("/programme-view/refresh-timer-preserves-unchanged-rows", test_refresh_timer_preserves_unchanged_rows);
    return g_test_run();
}
