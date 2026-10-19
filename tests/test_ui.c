#include "radios/editors.h"
#include "radios/widgets.h"
#include "radios/window.h"
#include "radios/header_navigation.h"
#include "radios/preferences.h"
#include "radios/ui_preferences.h"
#include "radios/content_scale.h"
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    GtkWidget *parent;
    RadioSViewContext ctx;
    gchar *notice;
} Fixture;

static RadioSConfig config;

static void
notice(gpointer data, const gchar *message)
{
    Fixture *fixture = data;
    g_free(fixture->notice);
    fixture->notice = g_strdup(message);
}

static void
setup(Fixture *fixture, gconstpointer data)
{
    (void)data;
    fixture->parent = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_ref_sink(fixture->parent);
    fixture->ctx.config = &config;
    fixture->ctx.parent = GTK_WINDOW(fixture->parent);
    fixture->ctx.notify = notice;
    fixture->ctx.notify_data = fixture;
}

static void
teardown(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gtk_widget_destroy(fixture->parent);
    g_object_unref(fixture->parent);
    g_free(fixture->notice);
    while (g_main_context_iteration(NULL, FALSE)) {}
}

static GtkWidget *
find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name))
        return root;
    if (GTK_IS_MENU_BUTTON(root)) {
        GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(root)));
        GtkWidget *result = popover ? find(popover, name) : NULL;
        if (result) return result;
    }
    if (!GTK_IS_CONTAINER(root))
        return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *result = NULL;
    for (GList *child = children; child != NULL && result == NULL; child = child->next)
        result = find(child->data, name);
    g_list_free(children);
    return result;
}

static GtkWidget *
dialog_find(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels();
    GtkWidget *result = NULL;
    for (GList *item = windows; item != NULL; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), name))
            result = item->data;
    g_list_free(windows);
    g_assert_nonnull(result);
    return result;
}

static void
set(GtkWidget *dialog, const gchar *name, const gchar *text)
{
    GtkWidget *entry = find(dialog, name);
    g_assert_true(GTK_IS_ENTRY(entry));
    gtk_entry_set_text(GTK_ENTRY(entry), text);
}

static const gchar *
get(GtkWidget *dialog, const gchar *name)
{
    GtkWidget *entry = find(dialog, name);
    g_assert_true(GTK_IS_ENTRY(entry));
    return gtk_entry_get_text(GTK_ENTRY(entry));
}

static void
click(GtkWidget *dialog, const gchar *name)
{
    GtkWidget *button = find(dialog, name);
    g_assert_true(GTK_IS_BUTTON(button));
    g_assert_true(gtk_widget_is_sensitive(button));
    gtk_button_clicked(GTK_BUTTON(button));
}

static const gchar *
status(GtkWidget *dialog)
{
    return gtk_label_get_text(GTK_LABEL(find(dialog, "editor-status")));
}

static GtkWidget *
find_data(GtkWidget *root, const gchar *key)
{
    if (g_object_get_data(G_OBJECT(root), key) != NULL)
        return root;
    if (!GTK_IS_CONTAINER(root))
        return NULL;
    GtkWidget *result = NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    for (GList *item = children; item != NULL && result == NULL; item = item->next)
        result = find_data(item->data, key);
    g_list_free(children);
    return result;
}

static guint
child_count(GtkWidget *container)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(container));
    guint result = g_list_length(children);
    g_list_free(children);
    return result;
}

static void
playlist_session(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GtkWidget *page = radios_playlist_new(&fixture->ctx);
    gtk_container_add(GTK_CONTAINER(fixture->parent), page);
    click(page, "pw-new-playlist");
    GtkWidget *dialog = dialog_find("pw-create-dialog");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_true(gtk_widget_get_visible(dialog));
    set(dialog, "pw-new-name", "Deneme listesi");
    GtkWidget *purpose = find(dialog, "pw-new-purpose");
    g_assert_true(GTK_IS_COMBO_BOX(purpose));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(purpose), "jingle"));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(fixture->notice);
    GtkWidget *selector = find(page, "pw-target-playlist");
    g_assert_true(GTK_IS_COMBO_BOX_TEXT(selector));
    gchar *name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(selector));
    g_assert_cmpstr(name, ==, "Deneme listesi");
    g_free(name);
    gint first = gtk_combo_box_get_active(GTK_COMBO_BOX(selector));
    GtkWidget *table = find(page, "pw-builder-table");
    g_assert_true(GTK_IS_TREE_VIEW(table));
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(table)), NULL), ==, 0);
    click(page, "pw-new-playlist");
    dialog = dialog_find("pw-create-dialog");
    set(dialog, "pw-new-name", "Ana yayın listesi");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(dialog, "pw-new-purpose")), "music"));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(selector));
    g_assert_cmpstr(name, ==, "Ana yayın listesi");
    g_free(name);
    gtk_combo_box_set_active(GTK_COMBO_BOX(selector), first);
    name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(selector));
    g_assert_cmpstr(name, ==, "Deneme listesi");
    g_free(name);
    g_assert_true(gtk_widget_is_sensitive(find(page, "pw-track-new")));
}

static void
dj_session(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GtkWidget *page = radios_dj_new(&fixture->ctx);
    gtk_container_add(GTK_CONTAINER(fixture->parent), page);
    GtkWidget *channels = g_object_get_data(G_OBJECT(page), "dj-channel-box");
    GtkWidget *add = g_object_get_data(G_OBJECT(page), "dj-add-button");
    guint baseline = child_count(channels);
    gtk_button_clicked(GTK_BUTTON(add));
    g_assert_cmpuint(child_count(channels), ==, baseline + 1);
    GtkWidget *pad = find_data(page, "dj-pad-edit");
    g_assert_nonnull(pad);
    GtkWidget *edit = g_object_get_data(G_OBJECT(pad), "dj-pad-edit");
    gtk_button_clicked(GTK_BUTTON(edit));
    GtkWidget *dialog = dialog_find("dj-sampler-dialog");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_true(gtk_widget_get_visible(dialog));
    set(dialog, "dj-sampler-title", "Deneme anonsu");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_button_clicked(GTK_BUTTON(edit));
    dialog = dialog_find("dj-sampler-dialog");
    g_assert_cmpstr(get(dialog, "dj-sampler-title"), ==, "Deneme anonsu");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_REJECT);
    gtk_button_clicked(GTK_BUTTON(edit));
    dialog = dialog_find("dj-sampler-dialog");
    g_assert_cmpstr(get(dialog, "dj-sampler-title"), ==, "");
    /* Leave a channel-owned editor open to exercise parent destruction. */
}

static const gchar *const dj_scaled_controls[] = {
    "dj-mixer", "dj-mixer-scale", "dj-effector-scale", "dj-effects-pedals",
    "dj-channel-1", "dj-channel-1-send", "dj-speech-bed",
    "dj-fx-1-depth", "dj-fx-2-depth", "dj-fx-3-depth", "dj-fx-4-depth"
};

static void
assert_dj_scaled_identity(GtkWidget *root, GtkWidget *const *original, guint mixer_width, guint mixer_height,
    guint effector_width, guint effector_height)
{
    for (guint i = 0; i < G_N_ELEMENTS(dj_scaled_controls); ++i)
        g_assert_true(find(root, dj_scaled_controls[i]) == original[i]);
    GtkWidget *mixer = original[0], *mixer_scale = original[1], *effector_scale = original[2];
    g_assert_true(RADIOS_IS_CONTENT_SCALE(mixer_scale)); g_assert_true(RADIOS_IS_CONTENT_SCALE(effector_scale));
    g_assert_true(gtk_widget_get_parent(mixer_scale) == mixer); g_assert_true(gtk_widget_get_parent(effector_scale) == mixer);
    g_assert_cmpuint(radios_content_scale_get_percent(mixer_scale), ==, mixer_width);
    g_assert_cmpuint(radios_content_scale_get_vertical_percent(mixer_scale), ==, mixer_height);
    g_assert_cmpuint(radios_content_scale_get_percent(effector_scale), ==, effector_width);
    g_assert_cmpuint(radios_content_scale_get_vertical_percent(effector_scale), ==, effector_height);
    g_assert_true(gtk_widget_is_ancestor(original[3], effector_scale));
    g_assert_false(gtk_widget_is_ancestor(original[3], mixer_scale));
    g_assert_true(gtk_widget_is_ancestor(original[4], mixer_scale));
    g_assert_true(gtk_widget_is_ancestor(original[6], mixer_scale));
    g_assert_false(gtk_widget_is_ancestor(original[4], effector_scale));
    g_assert_cmpfloat(gtk_adjustment_get_value(g_object_get_data(G_OBJECT(original[5]), "adjustment")), ==, -12.5);
    g_assert_cmpfloat(gtk_adjustment_get_value(g_object_get_data(G_OBJECT(original[7]), "adjustment")), ==, 72);
    GList *windows = gtk_window_list_toplevels();
    for (GList *item = windows; item; item = item->next)
        g_assert_cmpstr(gtk_widget_get_name(item->data), !=, "dj-effector-window");
    g_list_free(windows);
}

static void
dj_independent_scales_inline_and_detach(Fixture *fixture, gconstpointer data)
{
    (void)data; g_assert_null(fixture->ctx.audio); g_assert_null(fixture->ctx.data);
    GtkWidget *page = radios_dj_new(&fixture->ctx); gtk_container_add(GTK_CONTAINER(fixture->parent), page);
    GtkWidget *original[G_N_ELEMENTS(dj_scaled_controls)];
    for (guint i = 0; i < G_N_ELEMENTS(dj_scaled_controls); ++i) {
        original[i] = find(page, dj_scaled_controls[i]); g_assert_nonnull(original[i]); g_object_ref(original[i]);
    }
    gtk_adjustment_set_value(g_object_get_data(G_OBJECT(original[5]), "adjustment"), -12.5);
    gtk_adjustment_set_value(g_object_get_data(G_OBJECT(original[7]), "adjustment"), 72);
    GtkWidget *dock = gtk_widget_get_parent(original[0]);
    const guint scales[][4] = {{75, 125, 150, 50}, {150, 50, 60, 140}, {100, 100, 100, 100}};
    for (guint i = 0; i < G_N_ELEMENTS(scales); ++i) {
        radios_dj_set_scale(page, scales[i][0], scales[i][1], scales[i][2], scales[i][3]);
        assert_dj_scaled_identity(page, original, scales[i][0], scales[i][1], scales[i][2], scales[i][3]);
        g_assert_true(gtk_widget_get_toplevel(original[2]) == fixture->parent);
    }
    radios_dj_set_scale(page, 75, 125, 125, 75); click(page, "dj-mixer-window-toggle");
    GtkWidget *floating = dialog_find("dj-mixer-window"); g_assert_true(GTK_IS_WINDOW(floating));
    g_assert_true(gtk_window_get_transient_for(GTK_WINDOW(floating)) == GTK_WINDOW(fixture->parent));
    g_assert_true(gtk_widget_get_toplevel(original[1]) == floating); g_assert_true(gtk_widget_get_toplevel(original[2]) == floating);
    g_assert_null(find(page, "dj-mixer")); assert_dj_scaled_identity(floating, original, 75, 125, 125, 75);
    gtk_window_close(GTK_WINDOW(floating));
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while (gtk_widget_get_parent(original[0]) != dock && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000);
    }
    g_assert_true(gtk_widget_get_parent(original[0]) == dock);
    g_assert_true(gtk_widget_get_toplevel(original[2]) == fixture->parent);
    assert_dj_scaled_identity(page, original, 75, 125, 125, 75);
    for (guint i = 0; i < G_N_ELEMENTS(dj_scaled_controls); ++i) g_object_unref(original[i]);
}

static void
emergency_session(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GtkWidget *page = radios_emergency_new(&fixture->ctx);
    gtk_container_add(GTK_CONTAINER(fixture->parent), page);
    GtkWidget *list = g_object_get_data(G_OBJECT(page), "em-list");
    g_assert_nonnull(list);
    guint baseline = child_count(list);
    click(page, "em-list-new");
    set(page, "em-editor-title", "Deneme öğesi");
    set(page, "em-editor-duration", "03:70");
    click(page, "em-editor-save");
    g_assert_cmpuint(child_count(list), ==, baseline);
    set(page, "em-editor-duration", "03:30");
    click(page, "em-editor-save");
    g_assert_cmpuint(child_count(list), ==, baseline + 1);
    click(page, "em-list-new");
    set(page, "em-editor-title", "İkinci öğe");
    set(page, "em-editor-duration", "02:00");
    click(page, "em-editor-save");
    g_assert_cmpuint(child_count(list), ==, baseline + 2);
    GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
    g_assert_cmpint(gtk_list_box_row_get_index(selected), ==, 1);
    click(page, "em-list-up");
    g_assert_cmpint(gtk_list_box_row_get_index(selected), ==, 0);
    click(page, "em-list-remove");
    g_assert_cmpuint(child_count(list), ==, baseline + 1);
}

static void
preferences_window(Fixture *fixture, gconstpointer data)
{
    (void)fixture;
    (void)data;
    gchar *root = g_build_filename(config.project_root, "build", "ui-test-XXXXXX", NULL);
    g_assert_nonnull(g_mkdtemp(root));
    gchar *directory = g_build_filename(root, "config", NULL);
    gchar *filename = g_build_filename(directory, "ui-preferences.ini", NULL);
    g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
    /* Only the preference destination is isolated. Font/assets and read-only
     * resource diagnostics retain their actual project paths. */
    RadioSConfig isolated = config;
    isolated.project_root = root;
    GError *error = NULL;
    GtkApplication *application = gtk_application_new("org.radios.GuiAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error));
    g_assert_no_error(error);
    RadioSRuntime *runtime = radios_runtime_new(16, 32);
    RadioSWindow *window = radios_window_new(application, &isolated, runtime, FALSE, NULL, &error);
    g_assert_nonnull(window);
    g_assert_no_error(error);
    radios_window_present(window);
    GtkWidget *native = gtk_application_get_windows(application)->data;
    GtkWidget *broadcast = find(native, "profile-broadcast-tab");
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(broadcast)));
    GdkEventKey event = {0};
    event.type = GDK_KEY_PRESS;
    event.keyval = GDK_KEY_Right;
    gboolean handled = FALSE;
    g_signal_emit_by_name(broadcast, "key-press-event", &event, &handled);
    g_assert_true(handled);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(native, "profile-playlist-tab"))));
    GtkWidget *settings = find(native, "header-settings");
    g_assert_true(GTK_IS_BUTTON(settings));
    g_assert_false(GTK_IS_MENU_BUTTON(settings));
    g_assert_true(GTK_IS_IMAGE(gtk_button_get_image(GTK_BUTTON(settings))));
    g_assert_cmpstr(atk_object_get_name(gtk_widget_get_accessible(settings)), ==, "Ayarlar");
    GtkWidget *footer_actions = gtk_widget_get_parent(settings);
    GtkWidget *footer = gtk_widget_get_parent(footer_actions);
    g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(footer), "status-footer"));
    g_assert_null(find(native, "programme-history-table"));
    g_assert_null(find(find(native, "live-task-view"), "broadcast-output-panel"));
    GtkWidget *history_button = find(native, "header-playback-history");
    g_assert_true(GTK_IS_BUTTON(history_button));
    g_assert_true(gtk_widget_get_parent(history_button) == footer_actions);
    GList *footer_children = gtk_container_get_children(GTK_CONTAINER(footer_actions));
    g_assert_true(g_list_find(footer_children, settings)->next->data == history_button);
    g_list_free(footer_children);
    click(native, "header-playback-history");
    GtkWidget *history_window = dialog_find("programme-history-dialog");
    g_assert_true(GTK_IS_WINDOW(history_window));
    g_assert_true(gtk_window_get_transient_for(GTK_WINDOW(history_window)) == GTK_WINDOW(native));
    g_assert_nonnull(find(history_window, "programme-history-table"));
    click(native, "header-playback-history");
    g_assert_true(dialog_find("programme-history-dialog") == history_window);
    gtk_dialog_response(GTK_DIALOG(history_window), GTK_RESPONSE_CLOSE);
    click(native, "header-playback-history");
    history_window = dialog_find("programme-history-dialog");
    g_assert_nonnull(find(history_window, "programme-history-table"));
    g_object_add_weak_pointer(G_OBJECT(history_window), (gpointer *)&history_window);
    click(native, "header-settings");
    GtkWidget *settings_window = dialog_find("preferences-window");
    g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(settings_window)), ==, "Ayarlar");
    GtkWidget *category = find(settings_window, "preferences-category-view");
    gtk_list_box_select_row(GTK_LIST_BOX(find(settings_window, "preferences-categories")), GTK_LIST_BOX_ROW(category));
    g_assert_true(gtk_widget_get_mapped(find(settings_window, "preferences-appearance")));
    GtkWidget *appearance_size = find(settings_window, "preferences-text-size");
    GtkWidget *appearance_startup = find(settings_window, "preferences-default-profile");
    g_assert_true(GTK_IS_COMBO_BOX(appearance_size)); g_assert_true(GTK_IS_COMBO_BOX(appearance_startup));
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(appearance_size)), ==, 1);
    PangoFontDescription *before = NULL, *after = NULL;
    gtk_style_context_get(gtk_widget_get_style_context(native), GTK_STATE_FLAG_NORMAL, "font", &before, NULL);
    GtkWidget *show_programme = find(settings_window, "header-show-programme");
    g_assert_true(GTK_IS_TOGGLE_BUTTON(show_programme));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(show_programme), FALSE);
    gtk_combo_box_set_active(GTK_COMBO_BOX(appearance_size), 2);
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(find(native, "task-text-size"))), ==, 2);
    while (g_main_context_iteration(NULL, FALSE)) {}
    gtk_style_context_get(gtk_widget_get_style_context(native), GTK_STATE_FLAG_NORMAL, "font", &after, NULL);
    g_assert_cmpint(pango_font_description_get_size(after), >, pango_font_description_get_size(before));
    pango_font_description_free(before); pango_font_description_free(after);
    gtk_combo_box_set_active(GTK_COMBO_BOX(appearance_startup), 3);
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(find(native, "task-default-profile"))), ==, 3);
    click(settings_window, "preferences-appearance-save");
    RadioSUIPreferences appearance_saved = {0};
    g_assert_true(radios_ui_preferences_load(root, &appearance_saved, &error)); g_assert_no_error(error);
    g_assert_cmpuint(appearance_saved.text_size, ==, 2);
    g_assert_cmpuint(appearance_saved.default_profile, ==, 3);
    g_assert_cmpuint(appearance_saved.header_hidden_fields, ==, RADIOS_HEADER_HIDE_PROGRAMME);
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(native, "task-text-size")), 0);
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(appearance_size)), ==, 0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(appearance_size), 2);
    gtk_combo_box_set_active(GTK_COMBO_BOX(appearance_startup), 0);
    category = find(settings_window, "preferences-category-advanced");
    gtk_list_box_select_row(GTK_LIST_BOX(find(settings_window, "preferences-categories")), GTK_LIST_BOX_ROW(category));
    g_assert_false(gtk_widget_get_mapped(show_programme));
    click(settings_window, "preferences-open-advanced");
    gtk_widget_destroy(settings_window);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(native, "profile-technical-tab"))));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(find(native, "technical-settings-stack"))), ==, "devices");
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(native, "task-text-size")), 2);
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(native, "task-default-profile")), 0);
    click(native, "task-preferences-save");
    RadioSUIPreferences saved = {0};
    g_assert_true(radios_ui_preferences_load(root, &saved, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(saved.text_size, ==, 2);
    g_assert_cmpuint(saved.default_profile, ==, 0);
    g_assert_cmpuint(saved.last_profile, ==, 6);
    g_assert_cmpuint(saved.header_hidden_fields, ==, RADIOS_HEADER_HIDE_PROGRAMME);
    click(native, "profile-playlist-tab");
    radios_window_free(window);
    g_assert_null(history_window);
    window = radios_window_new(application, &isolated, runtime, FALSE, NULL, &error);
    g_assert_nonnull(window);
    g_assert_no_error(error);
    radios_window_present(window);
    native = gtk_application_get_windows(application)->data;
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(native, "profile-playlist-tab"))));
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(find(native, "task-text-size"))), ==, 2);
    click(native, "profile-technical-tab");
    gtk_stack_set_visible_child_name(GTK_STACK(find(native, "technical-settings-stack")), "servers");
    click(native, "header-settings");
    settings_window = dialog_find("preferences-window");
    category = find(settings_window, "preferences-category-view");
    gtk_list_box_select_row(GTK_LIST_BOX(find(settings_window, "preferences-categories")), GTK_LIST_BOX_ROW(category));
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(find(settings_window, "preferences-text-size"))), ==, 2);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(settings_window, "header-show-programme"))));
    category = find(settings_window, "preferences-category-advanced");
    gtk_list_box_select_row(GTK_LIST_BOX(find(settings_window, "preferences-categories")), GTK_LIST_BOX_ROW(category));
    click(settings_window, "preferences-open-advanced");
    gtk_widget_destroy(settings_window);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(find(native, "technical-settings-stack"))), ==, "devices");
    radios_window_free(window);
    radios_runtime_shutdown(runtime);
    radios_runtime_free(runtime);
    g_object_unref(application);
    g_assert_cmpint(g_remove(filename), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_assert_cmpint(g_rmdir(root), ==, 0);
    g_free(filename);
    g_free(directory);
    g_free(root);
}

static void
marker_validation(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_marker_editor_open(&fixture->ctx);
    GtkWidget *dialog = dialog_find("queue-waveform-dialog");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(strstr(status(dialog), "Parça süresi"));
    g_assert_null(fixture->notice);
    set(dialog, "qwf-duration", "180");
    set(dialog, "qwf-cue-in", "5");
    set(dialog, "qwf-out", "200");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(strstr(status(dialog), "toplam süre"));
    g_assert_cmpstr(get(dialog, "qwf-out"), ==, "200");
    set(dialog, "qwf-out", "175");
    set(dialog, "qwf-fade-start", "150");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(strstr(status(dialog), "birlikte"));
    set(dialog, "qwf-fade-end", "140");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(strstr(status(dialog), "doğru sırada"));
    set(dialog, "qwf-fade-end", "170");
    GtkWidget *automatic = find(dialog, "qwf-automatic");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(automatic), TRUE);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(strstr(status(dialog), "geçiş noktası gerekli"));
    set(dialog, "qwf-auto-next", "170");
    set(dialog, "qwf-overlap", "171");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(strstr(status(dialog), "Bindirme"));
    set(dialog, "qwf-overlap", "3");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(strstr(fixture->notice, "Veritabanına kaydedilmedi"));
    g_assert_true(gtk_widget_get_visible(dialog));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
}

static void
marker_history(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_marker_editor_open(&fixture->ctx);
    GtkWidget *dialog = dialog_find("queue-waveform-dialog");
    set(dialog, "qwf-duration", "180");
    set(dialog, "qwf-out", "180");
    set(dialog, "qwf-cue-in", "5");
    click(dialog, "qwf-undo");
    g_assert_cmpstr(get(dialog, "qwf-cue-in"), ==, "0");
    click(dialog, "qwf-redo");
    g_assert_cmpstr(get(dialog, "qwf-cue-in"), ==, "5");
    click(dialog, "qwf-reset");
    g_assert_cmpstr(get(dialog, "qwf-cue-in"), ==, "0");
    click(dialog, "qwf-undo");
    g_assert_cmpstr(get(dialog, "qwf-cue-in"), ==, "5");
    GtkWidget *timeline = find(dialog, "qwf-timeline");
    GdkEventKey event = {0};
    event.type = GDK_KEY_PRESS;
    event.keyval = GDK_KEY_End;
    gboolean handled = FALSE;
    g_signal_emit_by_name(timeline, "key-press-event", &event, &handled);
    g_assert_true(handled);
    click(dialog, "qwf-set-3");
    g_assert_cmpfloat(g_ascii_strtod(get(dialog, "qwf-out"), NULL), ==, 180);
    click(dialog, "qwf-zoom-in");
    /* Changing view cannot alter marker coordinates. */
    g_assert_cmpstr(get(dialog, "qwf-cue-in"), ==, "5");
    g_assert_cmpfloat(g_ascii_strtod(get(dialog, "qwf-out"), NULL), ==, 180);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
}

static void
marker_cancel(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_marker_editor_open(&fixture->ctx);
    GtkWidget *dialog = dialog_find("queue-waveform-dialog");
    set(dialog, "qwf-duration", "180");
    set(dialog, "qwf-out", "175");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    radios_marker_editor_open(&fixture->ctx);
    dialog = dialog_find("queue-waveform-dialog");
    g_assert_cmpstr(get(dialog, "qwf-out"), ==, "175");
    set(dialog, "qwf-out", "100");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    radios_marker_editor_open(&fixture->ctx);
    dialog = dialog_find("queue-waveform-dialog");
    g_assert_cmpstr(get(dialog, "qwf-out"), ==, "175");
    /* Opening twice presents one dialog, rather than losing edits. */
    radios_marker_editor_open(&fixture->ctx);
    g_assert_true(dialog == dialog_find("queue-waveform-dialog"));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
}

static void marker_pointer(GtkWidget *area, gdouble seconds, gdouble duration, gboolean release)
{
    GdkEventButton event = {0}; event.type = release ? GDK_BUTTON_RELEASE : GDK_BUTTON_PRESS;
    event.button = 1; event.x = 16 + (gtk_widget_get_allocated_width(area) - 32) * seconds / duration;
    event.y = 190; gboolean handled = FALSE;
    g_signal_emit_by_name(area, release ? "button-release-event" : "button-press-event", &event, &handled);
    g_assert_true(handled);
}

static void marker_embedded_selection_crop(Fixture *fixture, gconstpointer data)
{
    (void)data;
    JsonObject *detail = json_object_new(), *media = json_object_new(), *version = json_object_new(), *analysis = json_object_new();
    json_object_set_string_member(media, "_id", "00000000-0000-0000-0000-000000000001");
    json_object_set_string_member(media, "title", "Seçilen parça");
    json_object_set_int_member(version, "duration_ms", 10000);
    json_object_set_int_member(analysis, "sample_rate", 48000); json_object_set_int_member(analysis, "waveform_frames_per_bin", 30000);
    JsonArray *waveform = json_array_new();
    for (guint i = 0; i < 16; ++i) { json_array_add_double_element(waveform, -.5); json_array_add_double_element(waveform, .5); }
    json_object_set_array_member(analysis, "waveform", waveform);
    json_object_set_object_member(detail, "media", media); json_object_set_object_member(detail, "version", version);
    json_object_set_object_member(detail, "analysis", analysis); json_object_set_null_member(detail, "markers");
    GtkWidget *panel = radios_marker_editor_embed(&fixture->ctx, fixture->parent, detail);
    gtk_container_add(GTK_CONTAINER(fixture->parent), panel); gtk_window_set_default_size(GTK_WINDOW(fixture->parent), 1000, 700);
    gtk_widget_show_all(fixture->parent);
    while (g_main_context_iteration(NULL, FALSE)) {}
    GtkWidget *area = find(panel, "qwf-timeline"); g_assert_true(gtk_widget_get_mapped(area));
    g_assert_null(find(panel, "qwf-media-info"));
    g_assert_false(gtk_widget_is_visible(find(panel, "qwf-duration")));
    g_assert_false(gtk_widget_is_visible(find(panel, "qwf-automatic")));
    g_assert_nonnull(find(panel, "qwf-preview-output")); g_assert_nonnull(find(panel, "qwf-preview-gain"));
    g_assert_false(radios_marker_editor_dirty(panel));
    marker_pointer(area, 2, 10, FALSE);
    GdkEventMotion motion = {0}; motion.type = GDK_MOTION_NOTIFY; motion.state = GDK_BUTTON1_MASK;
    motion.x = 16 + (gtk_widget_get_allocated_width(area) - 32) * .7; motion.y = 190;
    gboolean handled = FALSE; g_signal_emit_by_name(area, "motion-notify-event", &motion, &handled); g_assert_true(handled);
    marker_pointer(area, 7, 10, TRUE);
    g_assert_false(radios_marker_editor_dirty(panel)); g_assert_true(gtk_widget_is_sensitive(find(panel, "qwf-crop-selection")));
    click(panel, "qwf-crop-selection");
    g_assert_cmpstr(get(panel, "qwf-cue-in"), ==, "2.000"); g_assert_cmpstr(get(panel, "qwf-out"), ==, "7.000");
    g_assert_cmpstr(get(panel, "qwf-fade-start"), ==, "7.000"); g_assert_cmpstr(get(panel, "qwf-auto-next"), ==, "7.000");
    g_assert_cmpstr(get(panel, "qwf-duration"), ==, "10.000"); g_assert_true(radios_marker_editor_dirty(panel));
    click(panel, "qwf-undo"); g_assert_cmpstr(get(panel, "qwf-cue-in"), ==, "0.000"); g_assert_cmpstr(get(panel, "qwf-out"), ==, "10.000");
    click(panel, "qwf-redo"); g_assert_cmpstr(get(panel, "qwf-cue-in"), ==, "2.000");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(panel, "qwf-overlap-seconds")), .5);
    g_assert_cmpstr(get(panel, "qwf-overlap"), ==, "0.500"); g_assert_cmpstr(get(panel, "qwf-auto-next"), ==, "6.500");
    marker_pointer(area, 5, 10, FALSE); marker_pointer(area, 5, 10, TRUE); click(panel, "qwf-set-6");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(panel, "qwf-overlap-seconds")), 1);
    g_assert_cmpstr(get(panel, "qwf-auto-next"), ==, "5.000");
    click(panel, "qwf-undo"); g_assert_cmpstr(get(panel, "qwf-overlap"), ==, "0.500");
    click(panel, "qwf-undo"); g_assert_cmpstr(get(panel, "qwf-auto-next"), ==, "6.500");
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(panel, "qwf-automatic"))));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(panel, "qwf-overlap-seconds")), 1);
    g_assert_cmpstr(get(panel, "qwf-overlap"), ==, "1.000"); g_assert_cmpstr(get(panel, "qwf-auto-next"), ==, "6.000");
    click(panel, "qwf-undo");
    g_assert_cmpstr(get(panel, "qwf-overlap"), ==, "0.500"); g_assert_cmpstr(get(panel, "qwf-auto-next"), ==, "6.500");
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(panel, "qwf-automatic"))));
    click(panel, "qwf-redo");
    g_assert_cmpstr(get(panel, "qwf-overlap"), ==, "1.000"); g_assert_cmpstr(get(panel, "qwf-auto-next"), ==, "6.000");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(panel, "qwf-overlap-seconds")), .5);
    g_assert_cmpstr(get(panel, "qwf-overlap"), ==, "0.500"); g_assert_cmpstr(get(panel, "qwf-auto-next"), ==, "6.500");
    marker_pointer(area, 3, 10, FALSE); marker_pointer(area, 3, 10, TRUE); click(panel, "qwf-set-8");
    marker_pointer(area, 6, 10, FALSE); marker_pointer(area, 6, 10, TRUE); click(panel, "qwf-set-9");
    g_assert_cmpstr(get(panel, "qwf-autodj-in"), ==, "3.000"); g_assert_cmpstr(get(panel, "qwf-autodj-out"), ==, "6.000");
    click(panel, "qwf-undo"); g_assert_cmpstr(get(panel, "qwf-autodj-out"), ==, "");
    click(panel, "qwf-redo"); g_assert_cmpstr(get(panel, "qwf-autodj-out"), ==, "6.000");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(panel, "qwf-overlap-seconds")), .75);
    g_assert_cmpstr(get(panel, "qwf-auto-next"), ==, "6.500"); g_assert_cmpstr(get(panel, "qwf-autodj-out"), ==, "6.000");
    click(panel, "qwf-undo"); g_assert_cmpstr(get(panel, "qwf-overlap"), ==, "0.500");
    GdkEventButton drag = {0}; drag.type = GDK_BUTTON_PRESS; drag.button = 1; drag.y = 17 + 6 * 21;
    drag.x = 16 + (gtk_widget_get_allocated_width(area) - 32) * .3;
    g_signal_emit_by_name(area, "button-press-event", &drag, &handled); g_assert_true(handled);
    motion.x = 16 + (gtk_widget_get_allocated_width(area) - 32) * .4; motion.y = drag.y;
    g_signal_emit_by_name(area, "motion-notify-event", &motion, &handled);
    drag.type = GDK_BUTTON_RELEASE; drag.x = motion.x;
    g_signal_emit_by_name(area, "button-release-event", &drag, &handled);
    g_assert_cmpstr(get(panel, "qwf-autodj-in"), ==, "4.000");
    click(panel, "qwf-undo"); g_assert_cmpstr(get(panel, "qwf-autodj-in"), ==, "3.000");
    /* Editing the view or marker draft never rewrites the original media. */
    g_assert_cmpint(json_object_get_int_member(version, "duration_ms"), ==, 10000);
    g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(detail, "markers")));
    json_object_unref(detail);
}

static void
transition_validation(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_transition_editor_open(&fixture->ctx);
    GtkWidget *dialog = dialog_find("transition-editor");
    set(dialog, "transition-duration", "nan");
    set(dialog, "transition-out", "175");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(strstr(status(dialog), "A toplam süre"));
    set(dialog, "transition-duration", "180");
    set(dialog, "transition-ducking", "1");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(strstr(status(dialog), "A kısma"));
    set(dialog, "transition-ducking", "-12");
    set(dialog, "transition-overlap", "31");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(strstr(status(dialog), "Bindirme"));
    set(dialog, "transition-overlap", "3");
    gchar *filename = g_build_filename(config.project_root, "build", "transition-source-XXXXXX", NULL);
    gint descriptor = g_mkstemp(filename);
    g_assert_cmpint(descriptor, >=, 0);
    g_assert_true(g_close(descriptor, NULL));
    GtkFileChooser *source = GTK_FILE_CHOOSER(find(dialog, "transition-file-a"));
    g_assert_true(gtk_file_chooser_set_filename(source, filename));
    g_signal_emit_by_name(source, "file-set");
    gtk_range_set_value(GTK_RANGE(find(dialog, "transition-preview-level")), -18);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(strstr(fixture->notice, "ses veya yayın komutu gönderilmedi"));
    g_assert_true(GTK_IS_FILE_CHOOSER(find(dialog, "transition-file-a")));
    g_assert_true(GTK_IS_FILE_CHOOSER(find(dialog, "transition-file-b")));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    radios_transition_editor_open(&fixture->ctx);
    dialog = dialog_find("transition-editor");
    g_assert_cmpstr(get(dialog, "transition-ducking"), ==, "-12");
    gchar *restored = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(find(dialog, "transition-file-a")));
    g_assert_cmpstr(restored, ==, filename);
    g_free(restored);
    g_assert_cmpfloat(gtk_range_get_value(GTK_RANGE(find(dialog, "transition-preview-level"))), ==, -18);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    g_assert_cmpint(g_remove(filename), ==, 0);
    g_free(filename);
}

static void
form_preserves_input(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *const fields[] = { "Başlık", NULL };
    radios_ui_form_open(&fixture->ctx, "Giriş koruma", fields);
    GList *windows = gtk_window_list_toplevels();
    GtkWidget *dialog = NULL;
    for (GList *item = windows; item != NULL; item = item->next)
        if (g_strcmp0(gtk_window_get_title(GTK_WINDOW(item->data)), "Giriş koruma") == 0)
            dialog = item->data;
    g_list_free(windows);
    g_assert_nonnull(dialog);
    set(dialog, "form-field-0", "Kaydedilmemiş giriş");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_true(gtk_widget_get_visible(dialog));
    g_assert_nonnull(strstr(fixture->notice, "Girişler korunuyor"));
    g_assert_cmpstr(get(dialog, "form-field-0"), ==, "Kaydedilmemiş giriş");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
}

static void
all_views_lifecycle(Fixture *fixture, gconstpointer data)
{
    (void)data;
    typedef GtkWidget *(*Builder)(RadioSViewContext *);
    const Builder builders[] = { radios_header_new, radios_broadcast_new,
        radios_playlist_new, radios_automation_new, radios_dj_new,
        radios_emergency_new, radios_advert_new, radios_technical_new, radios_routing_new };
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(fixture->parent), box);
    for (guint i = 0; i < G_N_ELEMENTS(builders); ++i) {
        GtkWidget *view = builders[i](&fixture->ctx);
        g_assert_true(GTK_IS_WIDGET(view));
        gtk_box_pack_start(GTK_BOX(box), view, FALSE, FALSE, 0);
    }
    /* Destroying the parent while both secondary editors are open must
     * destroy their windows, timers and draft stores without callbacks into
     * freed profile data. GLib/GTK warnings are fatal in this test process. */
    radios_marker_editor_open(&fixture->ctx);
    radios_transition_editor_open(&fixture->ctx);
}

static void
footer_refresh_for(guint milliseconds)
{
    gint64 deadline = g_get_monotonic_time() + milliseconds * 1000;
    while (g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
}

static void
footer_backup_stable_refresh(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GtkWidget *health = radios_header_health_new(&fixture->ctx);
    gtk_container_add(GTK_CONTAINER(fixture->parent), health);
    gtk_widget_show_all(fixture->parent);
    GtkWidget *backup = find(health, "header-backup-details");
    g_assert_true(GTK_IS_BUTTON(backup));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(backup)), ==, "Yedek  Okunmadı");
    /* Keep the original child alive so allocator reuse cannot hide a rebuild. */
    GtkWidget *child = g_object_ref(gtk_bin_get_child(GTK_BIN(backup)));
    footer_refresh_for(650);
    g_assert_true(gtk_bin_get_child(GTK_BIN(backup)) == child);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(backup)), ==, "Yedek  Okunmadı");
    g_object_unref(child);

    g_object_set_data(G_OBJECT(fixture->parent), "radios-backup-summary", "Doğrulandı");
    g_object_set_data(G_OBJECT(fixture->parent), "radios-backup-summary-detail", "Özel test yedeği doğrulandı.");
    footer_refresh_for(350);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(backup)), ==, "Yedek  Doğrulandı");
    gchar *tooltip = gtk_widget_get_tooltip_text(backup);
    g_assert_cmpstr(tooltip, ==, "Özel test yedeği doğrulandı."); g_free(tooltip);
    child = g_object_ref(gtk_bin_get_child(GTK_BIN(backup)));
    footer_refresh_for(650);
    g_assert_true(gtk_bin_get_child(GTK_BIN(backup)) == child);

    g_object_set_data(G_OBJECT(fixture->parent), "radios-backup-summary-detail", "Yalnız ayrıntı güncellendi.");
    footer_refresh_for(350);
    g_assert_true(gtk_bin_get_child(GTK_BIN(backup)) == child);
    tooltip = gtk_widget_get_tooltip_text(backup);
    g_assert_cmpstr(tooltip, ==, "Yalnız ayrıntı güncellendi."); g_free(tooltip);
    g_object_unref(child);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    gtk_init(&argc, &argv);
    GError *error = NULL;
    g_assert_true(radios_config_load(RADIOS_TEST_PROJECT_ROOT, NULL, &config, &error));
    g_assert_no_error(error);
    g_test_add("/ui/markers/validation", Fixture, NULL, setup, marker_validation, teardown);
    g_test_add("/ui/markers/history", Fixture, NULL, setup, marker_history, teardown);
    g_test_add("/ui/markers/cancel", Fixture, NULL, setup, marker_cancel, teardown);
    g_test_add("/ui/markers/embedded-selection-crop-autodj-overlap", Fixture, NULL, setup, marker_embedded_selection_crop, teardown);
    g_test_add("/ui/transition/validation", Fixture, NULL, setup, transition_validation, teardown);
    g_test_add("/ui/forms/preserve-input", Fixture, NULL, setup, form_preserves_input, teardown);
    g_test_add("/ui/profiles/lifecycle", Fixture, NULL, setup, all_views_lifecycle, teardown);
    g_test_add("/ui/playlist/session", Fixture, NULL, setup, playlist_session, teardown);
    g_test_add("/ui/dj/session", Fixture, NULL, setup, dj_session, teardown);
    g_test_add("/ui/dj/independent-scales-inline-and-detach", Fixture, NULL, setup, dj_independent_scales_inline_and_detach, teardown);
    g_test_add("/ui/emergency/session", Fixture, NULL, setup, emergency_session, teardown);
    g_test_add("/ui/preferences/window", Fixture, NULL, setup, preferences_window, teardown);
    g_test_add("/ui/footer/backup-stable-refresh", Fixture, NULL, setup, footer_backup_stable_refresh, teardown);
    int result = g_test_run();
    radios_config_clear(&config);
    return result;
}
