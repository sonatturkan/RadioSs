/* Real GTK menu -> existing chooser -> private file export, no production data. */
#define main radios_playlist_redesign_fixture_main
#include "test_playlist_redesign_ui.c"
#undef main

static GtkWidget *export_control(Fixture *fixture, const gchar *name)
{
    GtkWidget *manage = find(fixture->page, "pw-playlist-options");
    g_assert_true(GTK_IS_MENU_BUTTON(manage));
    GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(manage)));
    g_assert_true(GTK_IS_POPOVER(popover));
    return find(popover, name);
}
static void export_menu(Fixture *fixture)
{
    GtkWidget *manage = find(fixture->page, "pw-playlist-options");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(manage), TRUE);
    gint64 deadline = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < deadline) { pump(); g_usleep(1000); }
}
static void export_file(Fixture *fixture, const gchar *action, const gchar *name, const gchar *expected)
{
    export_menu(fixture);
    GtkWidget *button = export_control(fixture, action);
    g_assert_true(GTK_IS_BUTTON(button));
    g_assert_true(gtk_widget_get_mapped(button));
    g_assert_true(gtk_widget_is_sensitive(button));
    gtk_button_clicked(GTK_BUTTON(button)); pump();
    GtkWidget *chooser = dialog("pw-export-dialog");
    g_assert_true(GTK_IS_FILE_CHOOSER(chooser));
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), fixture->config.project_root);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), name);
    gchar *path = g_build_filename(fixture->config.project_root, name, NULL);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        gboolean ready = !g_strcmp0(filename, path); g_free(filename);
        if (ready) break;
        pump(); g_usleep(1000);
    }
    gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_ACCEPT);
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!g_file_test(path, G_FILE_TEST_IS_REGULAR) && g_get_monotonic_time() < deadline) { pump(); g_usleep(1000); }
    gchar *contents = NULL; GError *error = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error)); g_assert_no_error(error);
    g_assert_nonnull(strstr(contents, expected));
    g_assert_null(dialog("pw-export-dialog"));
    g_assert_cmpuint(row_count(fixture), ==, 1);
    g_assert_nonnull(strstr(fixture->notice, "dışa aktarıldı"));
    g_free(contents); g_assert_cmpint(g_unlink(path), ==, 0); g_free(path);
}
static void test_visible_export(Fixture *fixture, gconstpointer data)
{
    (void)data;
    create(fixture, "Türkçe yayın", "music");
    export_menu(fixture);
    g_assert_true(gtk_widget_get_mapped(export_control(fixture, "pw-export-csv")));
    g_assert_false(gtk_widget_is_sensitive(export_control(fixture, "pw-export-csv")));
    g_assert_false(gtk_widget_is_sensitive(export_control(fixture, "pw-export-m3u")));
    gtk_popover_popdown(GTK_POPOVER(gtk_menu_button_get_popover(GTK_MENU_BUTTON(find(fixture->page, "pw-playlist-options")))));
    drop(fixture, "[{\"title\": \"=Şarkı, \\\"Canlı\\\"\", \"artist\": \"Özgün\", \"duration_ms\": 180000, \"location\": \"/tmp/Özel şarkı.wav\"}]");
    g_assert_cmpuint(row_count(fixture), ==, 1);
    export_file(fixture, "pw-export-csv", "Türkçe çıktı.csv", "\"'=Şarkı, \"\"Canlı\"\"\"");
    export_file(fixture, "pw-export-m3u", "Türkçe çıktı.m3u8", "#EXTINF:180,Özgün - =Şarkı, \"Canlı\"\n/tmp/Özel şarkı.wav\n");
}
int main(int argc, char **argv)
{
    g_setenv("GSETTINGS_BACKEND", "memory", FALSE);
    g_test_init(&argc, &argv, NULL); if (!gtk_init_check(&argc, &argv)) return 77;
    g_test_add("/playlist-export/visible-menu-real-files-unicode-empty-disabled", Fixture, NULL, setup, test_visible_export, teardown);
    return g_test_run();
}
