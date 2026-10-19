#include "radios/views.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define ENTRIES_MIME "application/x-radios-playlist-entries"

typedef struct {
    GtkWidget *window, *page;
    RadioSViewContext ctx;
    RadioSConfig config;
    gchar *notice;
} Fixture;

static guint import_calls;
static RadioSViewContext *import_context;

/* Keep the action test offline while verifying dispatch to the application's
 * existing import entry point. No media files or production data are opened. */
void __wrap_radios_media_import_open(RadioSViewContext *ctx)
{
    ++import_calls;
    import_context = ctx;
}

static void pump(void)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
}

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (!root) return NULL;
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next)
        result = find(item->data, name);
    g_list_free(children);
    return result;
}

static GtkWidget *dialog(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels();
    GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (!g_strcmp0(gtk_widget_get_name(item->data), name)) result = item->data;
    g_list_free(windows);
    return result;
}

static gboolean contains_text(GtkWidget *root, const gchar *text)
{
    if (GTK_IS_LABEL(root) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(root)), text)) return TRUE;
    if (GTK_IS_MENU_ITEM(root) && !g_strcmp0(gtk_menu_item_get_label(GTK_MENU_ITEM(root)), text)) return TRUE;
    if (!GTK_IS_CONTAINER(root)) return FALSE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    gboolean found = FALSE;
    for (GList *item = children; item && !found; item = item->next)
        found = contains_text(item->data, text);
    g_list_free(children);
    return found;
}

static void click(GtkWidget *root, const gchar *name)
{
    GtkWidget *button = find(root, name);
    g_assert_true(GTK_IS_BUTTON(button));
    g_assert_true(gtk_widget_is_sensitive(button));
    gtk_button_clicked(GTK_BUTTON(button));
    pump();
}

static void notice(gpointer data, const gchar *message)
{
    Fixture *fixture = data;
    g_free(fixture->notice);
    fixture->notice = g_strdup(message);
}

static void setup(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GError *error = NULL;
    fixture->config.project_root = g_dir_make_tmp("radios-playlist-redesign-XXXXXX", &error);
    g_assert_no_error(error);
    fixture->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_ref_sink(fixture->window);
    gtk_style_context_add_class(gtk_widget_get_style_context(fixture->window), "radios");
    gtk_window_set_default_size(GTK_WINDOW(fixture->window), 1200, 1000);
    fixture->ctx = (RadioSViewContext){ .config = &fixture->config,
        .parent = GTK_WINDOW(fixture->window), .notify = notice, .notify_data = fixture };
    fixture->page = radios_playlist_new(&fixture->ctx);
    gtk_container_add(GTK_CONTAINER(fixture->window), fixture->page);
    gtk_widget_show_all(fixture->window);
    pump();
    import_calls = 0;
    import_context = NULL;
}

static void teardown(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gtk_widget_destroy(fixture->window);
    g_object_unref(fixture->window);
    pump();
    g_assert_cmpint(g_rmdir(fixture->config.project_root), ==, 0);
    radios_config_clear(&fixture->config);
    g_free(fixture->notice);
}

static GtkComboBox *target(Fixture *fixture)
{
    GtkWidget *widget = find(fixture->page, "pw-target-playlist");
    g_assert_true(GTK_IS_COMBO_BOX_TEXT(widget));
    return GTK_COMBO_BOX(widget);
}

static GtkTreeView *table(Fixture *fixture, const gchar *name)
{
    GtkWidget *widget = find(fixture->page, name);
    g_assert_true(GTK_IS_TREE_VIEW(widget));
    return GTK_TREE_VIEW(widget);
}

static guint row_count(Fixture *fixture)
{
    return (guint)gtk_tree_model_iter_n_children(
        gtk_tree_view_get_model(table(fixture, "pw-builder-table")), NULL);
}

static void choose_tab(Fixture *fixture, const gchar *name)
{
    GtkWidget *tabs = find(fixture->page, "pw-playlists");
    g_assert_true(GTK_IS_CONTAINER(tabs));
    GList *children = gtk_container_get_children(GTK_CONTAINER(tabs));
    GtkWidget *match = NULL;
    for (GList *item = children; item && !match; item = item->next)
        if (GTK_IS_BUTTON(item->data) && contains_text(item->data, name)) match = item->data;
    g_list_free(children);
    g_assert_nonnull(match);
    g_object_ref(match);
    gtk_button_clicked(GTK_BUTTON(match));
    g_object_unref(match);
    pump();
}

static void create(Fixture *fixture, const gchar *name, const gchar *purpose)
{
    click(fixture->page, "pw-new-playlist");
    GtkWidget *form = dialog("pw-create-dialog");
    g_assert_true(GTK_IS_DIALOG(form));
    GtkWidget *entry = find(form, "pw-new-name"), *combo = find(form, "pw-new-purpose");
    g_assert_true(GTK_IS_ENTRY(entry));
    g_assert_true(GTK_IS_COMBO_BOX(combo));
    gtk_entry_set_text(GTK_ENTRY(entry), name);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), purpose));
    gtk_dialog_response(GTK_DIALOG(form), GTK_RESPONSE_ACCEPT);
    pump();
    g_assert_null(dialog("pw-create-dialog"));
    gchar *active = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(target(fixture)));
    g_assert_cmpstr(active, ==, name);
    g_free(active);
}

static void test_screen_layout(Fixture *fixture, gconstpointer data)
{
    (void)data;
    g_assert_null(find(fixture->page, "pw-management"));
    g_assert_false(contains_text(fixture->page, "New Rotation Playlist"));
    g_assert_false(contains_text(fixture->page, "Playlist Builder"));
    GtkWidget *browser = find(fixture->page, "pw-source-browser");
    GtkWidget *destination = find(fixture->page, "pw-target-panel");
    g_assert_true(GTK_IS_WIDGET(browser));
    g_assert_true(GTK_IS_WIDGET(destination));
    GtkWidget *split = gtk_widget_get_parent(browser);
    g_assert_true(GTK_IS_PANED(split));
    g_assert_true(gtk_paned_get_child1(GTK_PANED(split)) == browser);
    g_assert_true(gtk_paned_get_child2(GTK_PANED(split)) == destination);
    g_assert_cmpint(gtk_orientable_get_orientation(GTK_ORIENTABLE(split)), ==, GTK_ORIENTATION_VERTICAL);
    g_assert_cmpint(gtk_tree_selection_get_mode(gtk_tree_view_get_selection(
        table(fixture, "pw-source-table"))), ==, GTK_SELECTION_MULTIPLE);
    GtkTargetList *targets = gtk_drag_dest_get_target_list(GTK_WIDGET(table(fixture, "pw-builder-table")));
    g_assert_nonnull(targets);
    g_assert_true(gtk_target_list_find(targets, gdk_atom_intern_static_string(ENTRIES_MIME), NULL));
}

static void test_create_and_switch(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GtkComboBox *combo = target(fixture);
    gint initial = gtk_tree_model_iter_n_children(gtk_combo_box_get_model(combo), NULL);
    click(fixture->page, "pw-new-playlist");
    GtkWidget *form = dialog("pw-create-dialog");
    g_assert_true(GTK_IS_DIALOG(form));
    gtk_dialog_response(GTK_DIALOG(form), GTK_RESPONSE_ACCEPT);
    pump();
    g_assert_true(form == dialog("pw-create-dialog"));
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_combo_box_get_model(combo), NULL), ==, initial);
    gtk_dialog_response(GTK_DIALOG(form), GTK_RESPONSE_CANCEL);
    create(fixture, "Kısa jingles", "jingle");
    gint first = gtk_combo_box_get_active(combo);
    g_assert_cmpuint(row_count(fixture), ==, 0);
    create(fixture, "Ana yayın", "music");
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_combo_box_get_model(combo), NULL), ==, initial + 2);
    choose_tab(fixture, "Kısa jingles");
    g_assert_cmpint(gtk_combo_box_get_active(combo), ==, first);
    gchar *selected = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    g_assert_cmpstr(selected, ==, "Kısa jingles");
    g_free(selected);
    g_assert_null(fixture->ctx.data);
}

static void test_add_track_uses_import(Fixture *fixture, gconstpointer data)
{
    (void)data;
    create(fixture, "İçe aktarılacaklar", "music");
    click(fixture->page, "pw-track-new");
    g_assert_cmpuint(import_calls, ==, 1);
    g_assert_true(import_context == &fixture->ctx);
    g_assert_null(dialog("pw-track-dialog"));
    g_assert_cmpuint(row_count(fixture), ==, 0);
}

static void clipboard_get(GtkClipboard *clipboard, GtkSelectionData *selection, guint info, gpointer data)
{
    (void)clipboard;
    (void)info;
    const gchar *payload = data;
    gtk_selection_data_set(selection, gdk_atom_intern_static_string(ENTRIES_MIME),
        8, (const guchar *)payload, (gint)strlen(payload));
}

static void drop(Fixture *fixture, const gchar *payload)
{
    gchar *selection_name = g_uuid_string_random();
    GtkClipboard *clipboard = gtk_clipboard_get(gdk_atom_intern(selection_name, FALSE));
    g_free(selection_name);
    GtkTargetEntry entry = { (gchar *)ENTRIES_MIME, 0, 0 };
    g_assert_true(gtk_clipboard_set_with_data(clipboard, &entry, 1, clipboard_get, NULL, (gpointer)payload));
    GtkSelectionData *selection = gtk_clipboard_wait_for_contents(clipboard,
        gdk_atom_intern_static_string(ENTRIES_MIME));
    g_assert_nonnull(selection);
    g_signal_emit_by_name(table(fixture, "pw-builder-table"), "drag-data-received",
        NULL, 0, 0, selection, 0, GDK_CURRENT_TIME);
    gtk_selection_data_free(selection);
    gtk_clipboard_clear(clipboard);
    pump();
}

static void screenshot(Fixture *fixture)
{
    const gchar *path = g_getenv("RADIOS_PLAYLIST_SCREENSHOT");
    if (!path || !*path) return;
    gint64 deadline = g_get_monotonic_time() + 150 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < deadline) { pump(); g_usleep(1000); }
    gdk_display_sync(gtk_widget_get_display(fixture->window));
    GdkWindow *window = gtk_widget_get_window(fixture->window);
    g_assert_nonnull(window);
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_window(window, 0, 0,
        gdk_window_get_width(window), gdk_window_get_height(window));
    g_assert_nonnull(pixbuf);
    GError *error = NULL;
    g_assert_true(gdk_pixbuf_save(pixbuf, path, "png", &error, NULL));
    g_assert_no_error(error);
    g_object_unref(pixbuf);
}

static void test_drop_preserves_target(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *payload = "[{\"title\":\"Birinci şarkı\",\"artist\":\"A\",\"album\":\"\",\"genre\":\"\",\"key\":\"\",\"location\":\"\",\"comment\":\"\",\"bpm\":120,\"duration_ms\":180000},"
        "{\"title\":\"İkinci şarkı\",\"artist\":\"B\",\"album\":\"\",\"genre\":\"\",\"key\":\"\",\"location\":\"\",\"comment\":\"\",\"bpm\":0,\"duration_ms\":95000}]";
    create(fixture, "Bırakılan parçalar", "music");
    gint first = gtk_combo_box_get_active(target(fixture));
    drop(fixture, payload);
    g_assert_cmpuint(row_count(fixture), ==, 2);
    create(fixture, "Diğer liste", "advert");
    g_assert_cmpuint(row_count(fixture), ==, 0);
    gtk_combo_box_set_active(target(fixture), first);
    pump();
    g_assert_cmpuint(row_count(fixture), ==, 2);
    screenshot(fixture);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(table(fixture, "pw-builder-table"));
    GtkTreePath *first_path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_selection_unselect_all(selection);
    gtk_tree_selection_select_path(selection, first_path);
    gtk_tree_path_free(first_path);
    click(fixture->page, "pw-track-edit");
    GtkWidget *editor = dialog("pw-track-dialog");
    g_assert_true(GTK_IS_DIALOG(editor));
    GtkWidget *title = find(editor, "pw-edit-title"), *artist = find(editor, "pw-edit-artist");
    g_assert_true(GTK_IS_ENTRY(title));
    g_assert_true(GTK_IS_ENTRY(artist));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(title)), ==, "Birinci şarkı");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(artist)), ==, "A");
    gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_CANCEL);
    gtk_tree_selection_select_all(selection);
    g_assert_cmpint(gtk_tree_selection_count_selected_rows(selection), ==, 2);
    drop(fixture, "[{\"artist\":\"Başlık yok\"}]");
    g_assert_cmpuint(row_count(fixture), ==, 2);
    g_assert_null(fixture->ctx.data);
}

static void test_large_playlist_search_and_selection(Fixture *fixture, gconstpointer data)
{
    (void)data;
    create(fixture, "Uzun playlist", "music");
    GString *payload = g_string_new("[");
    for (guint i = 1; i <= 105; ++i) {
        if (i > 1) g_string_append_c(payload, ',');
        g_string_append_printf(payload,
            "{\"title\":\"Parça %03u\",\"artist\":\"Arşiv\",\"duration_ms\":180000}", i);
    }
    g_string_append_c(payload, ']');
    drop(fixture, payload->str);
    g_string_free(payload, TRUE);
    g_assert_cmpuint(row_count(fixture), ==, 105);

    GtkTreeView *builder = table(fixture, "pw-builder-table");
    GtkTreeSelection *selection = gtk_tree_view_get_selection(builder);
    GtkTreePath *last = gtk_tree_path_new_from_indices(104, -1);
    gtk_tree_selection_unselect_all(selection);
    gtk_tree_selection_select_path(selection, last);
    g_assert_true(gtk_tree_selection_path_is_selected(selection, last));
    gtk_tree_path_free(last);

    GtkWidget *search = find(fixture->page, "pw-track-search");
    g_assert_true(GTK_IS_ENTRY(search));
    gtk_entry_set_text(GTK_ENTRY(search), "Parça 105");
    pump();
    g_assert_cmpuint(row_count(fixture), ==, 1);
    gtk_tree_selection_select_all(selection);
    click(fixture->page, "pw-track-edit");
    GtkWidget *editor = dialog("pw-track-dialog");
    g_assert_true(GTK_IS_DIALOG(editor));
    GtkWidget *title = find(editor, "pw-edit-title");
    g_assert_true(GTK_IS_ENTRY(title));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(title)), ==, "Parça 105");
    gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_CANCEL);

    gtk_entry_set_text(GTK_ENTRY(search), "");
    pump();
    g_assert_cmpuint(row_count(fixture), ==, 105);
    gtk_tree_selection_select_all(selection);
    g_assert_cmpint(gtk_tree_selection_count_selected_rows(selection), ==, 105);
    gtk_tree_selection_unselect_all(selection);
    g_assert_cmpint(gtk_tree_selection_count_selected_rows(selection), ==, 0);
}

int main(int argc, char **argv)
{
    g_setenv("GSETTINGS_BACKEND", "memory", FALSE);
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check(&argc, &argv)) return 77;
    if (g_getenv("RADIOS_PLAYLIST_SCREENSHOT")) {
        extern const gchar *const radios_reference_css_parts[];
        GString *stylesheet = g_string_new(NULL);
        for (guint i = 0; radios_reference_css_parts[i]; ++i)
            g_string_append(stylesheet, radios_reference_css_parts[i]);
        const gchar *theme_path = g_getenv("RADIOS_PLAYLIST_THEME");
        if (theme_path && *theme_path) {
            gchar *theme = NULL;
            GError *error = NULL;
            g_assert_true(g_file_get_contents(theme_path, &theme, NULL, &error));
            g_assert_no_error(error);
            g_string_append(stylesheet, theme);
            g_free(theme);
        }
        GtkCssProvider *provider = gtk_css_provider_new();
        GError *error = NULL;
        g_assert_true(gtk_css_provider_load_from_data(provider, stylesheet->str, stylesheet->len, &error));
        g_assert_no_error(error);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider);
        g_string_free(stylesheet, TRUE);
        g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    }
    g_test_add("/playlist-redesign/layout", Fixture, NULL, setup, test_screen_layout, teardown);
    g_test_add("/playlist-redesign/create-switch", Fixture, NULL, setup, test_create_and_switch, teardown);
    g_test_add("/playlist-redesign/add-track-import", Fixture, NULL, setup, test_add_track_uses_import, teardown);
    g_test_add("/playlist-redesign/drop-target", Fixture, NULL, setup, test_drop_preserves_target, teardown);
    g_test_add("/playlist-redesign/large-playlist-search-selection", Fixture, NULL, setup, test_large_playlist_search_and_selection, teardown);
    return g_test_run();
}
