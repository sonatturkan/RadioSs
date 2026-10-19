#include "radios/preferences.h"
#include "radios/preferences_storage.h"
#include "radios/window.h"
#include <glib/gstdio.h>
#include <string.h>

static GtkWidget *named(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next)
        result = named(item->data, name);
    g_list_free(children);
    return result;
}

static GtkWidget *required(GtkWidget *root, const gchar *name)
{
    GtkWidget *result = named(root, name);
    g_assert_nonnull(result);
    return result;
}

static GtkWidget *toplevel(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels();
    GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), name)) result = item->data;
    g_list_free(windows);
    g_assert_nonnull(result);
    return result;
}

static void destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    *(gboolean *)data = TRUE;
}

static void settle(void)
{
    gint64 end = g_get_monotonic_time() + 250000;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    } while (g_get_monotonic_time() < end);
}

typedef struct { guint calls; gchar *category; } Navigation;
static void navigate(RadioSViewContext *ctx, const gchar *category, gpointer data)
{
    g_assert_nonnull(ctx);
    Navigation *navigation = data;
    ++navigation->calls;
    g_free(navigation->category);
    navigation->category = g_strdup(category);
}

static void panel(void)
{
    GError *error = NULL;
    gchar *root = g_dir_make_tmp("radios-prompt1-ui-XXXXXX", &error);
    g_assert_no_error(error);
    gchar *directory = g_build_filename(root, "config", NULL);
    g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
    RadioSConfig config = {.project_root = root};
    GtkWidget *parent = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_ref_sink(parent);
    RadioSViewContext ctx = {.config = &config, .parent = GTK_WINDOW(parent)};
    Navigation navigation = {0};
    radios_preferences_set_navigation(GTK_WINDOW(parent), navigate, &navigation);
    GtkWidget *button = radios_preferences_button_new(&ctx);
    gtk_container_add(GTK_CONTAINER(parent), button);
    gtk_widget_show_all(parent);
    g_assert_true(GTK_IS_BUTTON(button));
    g_assert_false(GTK_IS_MENU_BUTTON(button));
    g_assert_cmpstr(gtk_widget_get_name(button), ==, "header-settings");
    g_assert_cmpstr(atk_object_get_name(gtk_widget_get_accessible(button)), ==, "Ayarlar");
    gtk_button_clicked(GTK_BUTTON(button));
    GtkWidget *preferences = radios_preferences_open(&ctx);
    g_assert_true(GTK_IS_WINDOW(preferences));
    g_assert_true(gtk_window_get_transient_for(GTK_WINDOW(preferences)) == GTK_WINDOW(parent));
    g_assert_true(preferences == radios_preferences_open(&ctx));
    settle();
    g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(preferences)), ==, "Ayarlar");
    GtkWidget *categories = required(preferences, "preferences-categories");
    GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(categories));
    g_assert_true(GTK_WIDGET(selected) == required(preferences, "preferences-category-audio"));
    g_assert_cmpint(gtk_notebook_get_current_page(GTK_NOTEBOOK(required(preferences, "preferences-notebook"))), ==, 0);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(required(preferences, "preferences-sample-rate"))), ==, "44100");
    /* Buffer size, metronome and its level are no longer offered; the card's
     * highest rate, master device and loudness normalization are. */
    g_assert_null(named(preferences, "preferences-buffer-size")); g_assert_null(named(preferences, "preferences-click-sound-01"));
    g_assert_null(named(preferences, "preferences-volume-middle"));
    g_assert_true(GTK_IS_LABEL(required(preferences, "preferences-max-sample-rate")));
    g_assert_true(GTK_IS_TOGGLE_BUTTON(required(preferences, "preferences-loudness-normalize")));
    g_assert_true(GTK_IS_SPIN_BUTTON(required(preferences, "preferences-loudness-target")));
    const gchar *retired[] = {"play", "cloud", "analysis", "dj-system"};
    for (guint i = 0; i < G_N_ELEMENTS(retired); ++i) {
        gchar *name = g_strdup_printf("preferences-category-%s", retired[i]); g_assert_null(named(preferences, name)); g_free(name);
    }
    const gchar *present[] = {"audio", "autodj", "media-archive", "library-analysis", "export", "import", "view", "midi", "keyboard", "advanced", "reset", "copyright-policy", "copyright", "about"};
    for (guint i = 0; i < G_N_ELEMENTS(present); ++i) {
        gchar *name = g_strdup_printf("preferences-category-%s", present[i]); g_assert_true(GTK_IS_LIST_BOX_ROW(required(preferences, name))); g_free(name);
    }
    /* Kılavuz 18.10 R11.1/R11.2: the rows keep this order (Import right
     * after Export) and every row draws its own icon in the same 28 × 30
     * design box: not blank, inside the box, unlike every other icon. */
    GList *rows = gtk_container_get_children(GTK_CONTAINER(categories));
    g_assert_cmpuint(g_list_length(rows), ==, G_N_ELEMENTS(present));
    GBytes *drawn[G_N_ELEMENTS(present)] = {NULL};
    guint index = 0;
    for (GList *item = rows; item; item = item->next, ++index) {
        gchar *name = g_strdup_printf("preferences-category-%s", present[index]);
        g_assert_cmpstr(gtk_widget_get_name(item->data), ==, name); g_free(name);
        GList *parts = gtk_container_get_children(GTK_CONTAINER(gtk_bin_get_child(GTK_BIN(item->data))));
        GtkWidget *icon = parts->data; g_list_free(parts);
        g_assert_true(GTK_IS_DRAWING_AREA(icon));
        gint request_width = 0, request_height = 0; gtk_widget_get_size_request(icon, &request_width, &request_height);
        g_assert_cmpint(request_width, ==, 28); g_assert_cmpint(request_height, ==, 30);
        const gint width = gtk_widget_get_allocated_width(icon), height = gtk_widget_get_allocated_height(icon);
        g_assert_cmpint(width, >=, 28); g_assert_cmpint(height, >=, 28);
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        cairo_t *cr = cairo_create(surface); gtk_widget_draw(icon, cr); cairo_destroy(cr); cairo_surface_flush(surface);
        const guchar *pixels = cairo_image_surface_get_data(surface); const gint stride = cairo_image_surface_get_stride(surface);
        const gint left = (width - 28) / 2, top = (height - 28) / 2; guint ink = 0, outside = 0;
        for (gint y = 0; y < height; ++y)
            for (gint x = 0; x < width; ++x)
                if (((const guint32 *)(pixels + y * stride))[x] >> 24) {
                    ++ink; if (x < left || x >= left + 28 || y < top || y >= top + 28) ++outside;
                }
        g_assert_cmpuint(ink, >, 40); g_assert_cmpuint(outside, ==, 0);
        drawn[index] = g_bytes_new(pixels, (gsize)stride * (gsize)height);
        for (guint other = 0; other < index; ++other) g_assert_false(g_bytes_equal(drawn[index], drawn[other]));
        cairo_surface_destroy(surface);
    }
    for (guint i = 0; i < G_N_ELEMENTS(present); ++i) g_bytes_unref(drawn[i]);
    g_list_free(rows);
    /* Export and Import pages come from transfer_ui.c; neither has a workspace shortcut. */
    /* Görünüm starts with the saved views (name, list, Kaydet, Sil). */
    g_assert_true(GTK_IS_WIDGET(required(preferences, "preferences-view-presets")));
    g_assert_true(GTK_IS_COMBO_BOX(required(preferences, "preferences-view-presets-list")));
    g_assert_true(GTK_IS_BUTTON(required(preferences, "preferences-view-presets-save")));
    g_assert_true(GTK_IS_BOX(required(preferences, "preferences-export-page")));
    g_assert_true(GTK_IS_BOX(required(preferences, "preferences-import-page")));
    g_assert_null(named(preferences, "preferences-open-export"));
    g_assert_null(named(preferences, "preferences-open-import"));
    g_assert_true(GTK_IS_WIDGET(required(preferences, "preferences-autodj-page")));
    g_assert_false(gtk_widget_get_sensitive(required(preferences, "autodj-transition-save")));
    g_assert_false(gtk_widget_get_sensitive(required(preferences, "autodj-marker-start")));
    /* Medya arşivi: no folder and no data service yet; options persist to config/media-archive.ini. */
    g_assert_true(GTK_IS_WIDGET(required(preferences, "preferences-archive-page")));
    g_assert_false(gtk_widget_get_sensitive(required(preferences, "preferences-archive-scan")));
    g_assert_false(gtk_widget_get_sensitive(required(preferences, "preferences-archive-stop")));
    g_assert_true(gtk_widget_get_sensitive(required(preferences, "preferences-archive-add")));
    g_assert_false(gtk_widget_get_sensitive(required(preferences, "preferences-archive-remove")));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(required(preferences, "preferences-archive-kind"))), ==, "music");
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(required(preferences, "preferences-archive-minutes"))), ==, 30);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(required(preferences, "preferences-archive-watch"))));
    g_assert_false(gtk_widget_get_sensitive(required(preferences, "preferences-archive-minutes")));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(required(preferences, "preferences-archive-status"))), "veri servisi"));
    g_assert_true(GTK_IS_BUTTON(required(preferences, "preferences-storage-recordings-choose")));
    g_assert_false(gtk_widget_get_sensitive(required(preferences, "preferences-storage-restart")));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(required(preferences, "preferences-storage-database-path"))), ==, "—");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(required(preferences, "preferences-archive-watch")), TRUE);
    settle();
    g_assert_true(gtk_widget_get_sensitive(required(preferences, "preferences-archive-minutes")));
    gchar *archive_file = g_build_filename(directory, "media-archive.ini", NULL), *archive_text = NULL;
    g_assert_true(g_file_get_contents(archive_file, &archive_text, NULL, NULL));
    g_assert_nonnull(strstr(archive_text, "watch=true")); g_assert_nonnull(strstr(archive_text, "kind=music"));
    g_free(archive_text); g_free(archive_file);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(required(preferences, "preferences-archive-summary"))), "30 dakikada"));
    /* Sıfırlama offers six resets with a backup by default; none runs offline. */
    const gchar *resets[] = {"settings", "playlists", "media", "mixer", "automation", "factory"};
    for (guint i = 0; i < G_N_ELEMENTS(resets); ++i) {
        gchar *name = g_strdup_printf("preferences-reset-%s", resets[i]); GtkWidget *reset = required(preferences, name); g_free(name);
        g_assert_true(GTK_IS_BUTTON(reset)); g_assert_false(gtk_widget_get_sensitive(reset));
    }
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(required(preferences, "preferences-reset-backup"))));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(required(preferences, "preferences-reset-status"))), "veritabanı"));
    g_assert_true(GTK_IS_WIDGET(required(preferences, "preferences-midi")));
    g_assert_null(named(preferences, "preferences-copyright-media-rights"));
    g_assert_true(GTK_IS_TREE_VIEW(required(preferences, "copyright-policy-rules")));
    g_assert_false(gtk_widget_get_sensitive(required(preferences, "copyright-policy-save")));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(required(preferences, "copyright-policy-unrestricted"))));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(required(preferences, "copyright-policy-status"))), "veritabanı"));
    g_assert_true(GTK_IS_LABEL(required(preferences, "preferences-about-version")));
    g_assert_null(named(preferences, "preferences-camera"));
    g_assert_null(named(preferences, "preferences-google"));

    const gchar *capture = g_getenv("RADIOS_PREFERENCES_CAPTURE");
    if (capture && *capture) {
        /* RADIOS_PREFERENCES_CAPTURE_CATEGORY=<id> shows that page first. */
        const gchar *page = g_getenv("RADIOS_PREFERENCES_CAPTURE_CATEGORY");
        if (page && *page) {
            gchar *row_name = g_strdup_printf("preferences-category-%s", page);
            gtk_list_box_select_row(GTK_LIST_BOX(required(preferences, "preferences-categories")),
                                    GTK_LIST_BOX_ROW(required(preferences, row_name)));
            g_free(row_name);
            for (guint i = 0; i < 40; ++i) { while (gtk_events_pending()) gtk_main_iteration(); g_usleep(10000); }
        }
        GdkWindow *native = gtk_widget_get_window(preferences);
        GdkPixbuf *pixels = gdk_pixbuf_get_from_window(native, 0, 0,
            gtk_widget_get_allocated_width(preferences), gtk_widget_get_allocated_height(preferences));
        g_assert_nonnull(pixels);
        g_assert_true(gdk_pixbuf_save(pixels, capture, "png", &error, NULL));
        g_assert_no_error(error);
        g_object_unref(pixels);
    }

    gtk_combo_box_set_active_id(GTK_COMBO_BOX(required(preferences, "preferences-sample-rate")), "48000");
    settle();
    RadioSPreferencesAudio saved;
    g_assert_true(radios_preferences_audio_load(root, &saved, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(saved.sample_rate, ==, 48000);
    g_assert_cmpuint(saved.buffer_frames, ==, 256);
    g_assert_cmpuint(saved.click_sound, ==, 0);
    g_assert_cmpuint(saved.click_volume, ==, 1);
    gtk_button_clicked(GTK_BUTTON(required(preferences, "preferences-driver")));
    GtkWidget *driver = toplevel("preferences-driver-dialog");
    g_assert_true(gtk_window_get_transient_for(GTK_WINDOW(driver)) == GTK_WINDOW(preferences));
    gtk_button_clicked(GTK_BUTTON(required(driver, "preferences-driver-refresh")));
    settle();
    g_assert_true(GTK_IS_LABEL(required(driver, "preferences-driver-details")));
    gtk_widget_destroy(driver);
    settle();
    gtk_button_clicked(GTK_BUTTON(required(preferences, "preferences-driver")));
    driver = toplevel("preferences-driver-dialog");
    gboolean driver_destroyed = FALSE;
    g_signal_connect(driver, "destroy", G_CALLBACK(destroyed), &driver_destroyed);
    gtk_button_clicked(GTK_BUTTON(required(preferences, "preferences-close")));
    settle();
    g_assert_true(driver_destroyed);
    preferences = radios_preferences_open(&ctx);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(required(preferences, "preferences-sample-rate"))), ==, "48000");
    gtk_button_clicked(GTK_BUTTON(required(preferences, "preferences-reset")));
    settle();
    g_assert_true(radios_preferences_audio_load(root, &saved, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(saved.sample_rate, ==, 44100);
    g_assert_cmpuint(saved.buffer_frames, ==, 256);
    g_assert_cmpuint(saved.click_sound, ==, 0);
    g_assert_cmpuint(saved.click_volume, ==, 1);

    GtkWidget *search = required(preferences, "preferences-search");
    gtk_entry_set_text(GTK_ENTRY(search), "midi");
    settle();
    g_assert_true(gtk_widget_get_child_visible(required(preferences, "preferences-category-midi")));
    g_assert_false(gtk_widget_get_child_visible(required(preferences, "preferences-category-keyboard")));
    gtk_entry_set_text(GTK_ENTRY(search), "");
    settle();
    categories = required(preferences, "preferences-categories");
    gtk_list_box_select_row(GTK_LIST_BOX(categories), GTK_LIST_BOX_ROW(required(preferences, "preferences-category-keyboard")));
    gtk_button_clicked(GTK_BUTTON(required(preferences, "preferences-open-keyboard")));
    settle();
    g_assert_cmpuint(navigation.calls, ==, 1);
    g_assert_cmpstr(navigation.category, ==, "Keyboard");
    gtk_widget_destroy(parent);
    g_object_unref(parent);
    settle();
    g_free(navigation.category);
    gchar *filename = g_build_filename(directory, "audio-panel.ini", NULL);
    g_assert_cmpint(g_remove(filename), ==, 0);
    gchar *archive_settings = g_build_filename(directory, "media-archive.ini", NULL);
    g_assert_cmpint(g_remove(archive_settings), ==, 0); g_free(archive_settings);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_assert_cmpint(g_rmdir(root), ==, 0);
    g_free(filename); g_free(directory); g_free(root);
}

int main(int argc, char **argv)
{
    if (!gtk_init_check(&argc, &argv)) return 77;
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/preferences/native-panel", panel);
    return g_test_run();
}
