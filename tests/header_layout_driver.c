#define _POSIX_C_SOURCE 200809L

#include "radios/window.h"
#include "radios/header_status.h"
#include "radios/broadcast_header.h"
#include "radios/ui_preferences.h"
#include "radios/preferences.h"

#include <glib/gstdio.h>
#include <locale.h>
#include <string.h>
#include <unistd.h>

#ifndef RADIOS_TEST_PROJECT_ROOT
#define RADIOS_TEST_PROJECT_ROOT "."
#endif

/* Link with --wrap=radios_broadcast_header_snapshot. The window, theme,
 * responsive grid, font scaling and widget lifetimes are production code;
 * header mode creates a fresh production header under the same active theme.
 * Only playback observations use explicit, invented non-secret test data. */
gboolean
__wrap_radios_broadcast_header_snapshot(RadioSViewContext *ctx, RadioSBroadcastHeaderSnapshot *out)
{
    (void)ctx;
    if (!out) return FALSE;
    memset(out, 0, sizeof *out);
    out->programme_active = out->current = TRUE;
    out->active_sources = 1; out->tempo_ratio = 1;
    out->duration_ms = 245000; out->position_ms = 34000;
    out->track_remaining_us = 211 * G_TIME_SPAN_SECOND;
    out->programme_end_utc_us = g_get_real_time() + 45 * G_TIME_SPAN_MINUTE;
    out->playlist_end_utc_us = g_get_real_time() + 15 * G_TIME_SPAN_MINUTE;
    g_strlcpy(out->playlist_title, "Türkçe gece playlisti · Uzun örnek yayın sırası", sizeof out->playlist_title);
    g_strlcpy(out->programme_title,
        "Şubat gecesi — Türkçe yayın · İstanbul / Çarşamba özel programı · Uzun başlık", sizeof out->programme_title);
    g_strlcpy(out->title,
        "Gecenin ışıkları — Çığlık, özgürlük ve müzik · Çok uzun örnek parça başlığı", sizeof out->title);
    g_strlcpy(out->artist, "Şebnem Örnek · Işıklı Şehir Orkestrası", sizeof out->artist);
    return TRUE;
}

static GtkWidget *
find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *result = NULL;
    for (GList *child = children; child && !result; child = child->next)
        result = find(child->data, name);
    g_list_free(children); return result;
}

static gboolean
number(const gchar *value, guint minimum, guint maximum, guint *out)
{
    gchar *end = NULL;
    guint64 parsed = g_ascii_strtoull(value, &end, 10);
    if (!*value || !end || *end || parsed < minimum || parsed > maximum) return FALSE;
    *out = (guint)parsed; return TRUE;
}

static void
settle(gint64 microseconds)
{
    gint64 end = g_get_monotonic_time() + microseconds;
    while (g_get_monotonic_time() < end) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(10000);
    }
}

static gboolean
inside(const gchar *root, const gchar *path)
{
    gsize length = strlen(root);
    return g_str_has_prefix(path, root) && path[length] == G_DIR_SEPARATOR;
}

static gboolean
label_geometry(GtkWidget *label, GtkWidget *window, gint width, gint height)
{
    if (!GTK_IS_LABEL(label) || !gtk_widget_get_mapped(label)) return FALSE;
    GtkAllocation allocation; gtk_widget_get_allocation(label, &allocation);
    gint x = 0, y = 0, text_width = 0, text_height = 0;
    gboolean translated = gtk_widget_translate_coordinates(label, window, 0, 0, &x, &y);
    PangoLayout *layout = gtk_label_get_layout(GTK_LABEL(label));
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    gboolean fits = translated && allocation.width > 0 && allocation.width >= text_width && allocation.height >= text_height &&
        x >= 0 && y >= 0 && x + allocation.width <= width && y + allocation.height <= height;
    g_print("Etiket %s: mapped=1 konum=%d,%d kutu=%d×%d metin=%d×%d ellipsis=%d görünürlük=%s\n",
        gtk_widget_get_name(label), x, y, allocation.width, allocation.height,
        text_width, text_height, pango_layout_is_ellipsized(layout), fits ? "PASS" : "FAIL");
    return fits;
}

static gboolean
legacy_geometry(GtkWidget *widget, GtkWidget *window, gint width, gint height)
{
    gboolean fits = TRUE;
    GtkStyleContext *style = gtk_widget_get_style_context(widget);
    const gchar *const classes[] = {"broadcast-label", "header-next-title", "header-next-advert", "header-countdown"};
    if (GTK_IS_LABEL(widget)) {
        for (guint i = 0; i < G_N_ELEMENTS(classes); ++i) {
            if (!gtk_style_context_has_class(style, classes[i])) continue;
            g_print("Mevcut NEXT %s: tek_satır=%d wrap=%d\n", classes[i],
                gtk_label_get_single_line_mode(GTK_LABEL(widget)), gtk_label_get_line_wrap(GTK_LABEL(widget)));
            if (!gtk_widget_get_mapped(widget)) {
                GtkWidget *header = find(window, "shared-header");
                fits = header && gtk_style_context_has_class(gtk_widget_get_style_context(header), "header-compact") && fits;
                continue;
            }
            fits = gtk_label_get_single_line_mode(GTK_LABEL(widget)) &&
                !gtk_label_get_line_wrap(GTK_LABEL(widget)) && label_geometry(widget, window, width, height) && fits;
        }
    }
    if (!GTK_IS_CONTAINER(widget)) return fits;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *child = children; child; child = child->next)
        fits = legacy_geometry(child->data, window, width, height) && fits;
    g_list_free(children); return fits;
}

static GtkWidget *
find_class(GtkWidget *root, const gchar *name)
{
    if (gtk_style_context_has_class(gtk_widget_get_style_context(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *result = NULL;
    for (GList *child = children; child && !result; child = child->next) result = find_class(child->data, name);
    g_list_free(children); return result;
}

/* A dialog type hint prevents a tiling window manager from maximizing this
 * test fixture. Requested sizes still obey the real GTK minimum requisition. */
static void
requested_size(GtkWindow *window, guint width, guint height)
{
    gtk_window_set_type_hint(window, GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_unmaximize(window);
    gtk_window_set_default_size(window, (gint)width, (gint)height);
    gtk_window_resize(window, (gint)width, (gint)height);
}

static gboolean
capture(GtkWidget *window, const gchar *path)
{
    GtkAllocation allocation; gtk_widget_get_allocation(window, &allocation);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
    cairo_t *drawing = cairo_create(surface);
    gtk_widget_draw(window, drawing); cairo_destroy(drawing);
    cairo_status_t result = cairo_surface_write_to_png(surface, path);
    cairo_surface_destroy(surface);
    if (result != CAIRO_STATUS_SUCCESS) g_printerr("Ekran görüntüsü yazılamadı: %s\n", cairo_status_to_string(result));
    return result == CAIRO_STATUS_SUCCESS;
}

static void
minimum_report(GtkWidget *native, GtkWidget *header)
{
    GtkRequisition full_minimum, full_natural, header_minimum, header_natural;
    gtk_widget_get_preferred_size(native, &full_minimum, &full_natural);
    gtk_widget_get_preferred_size(header, &header_minimum, &header_natural);
    GtkWidget *panel = find(header, "header-status-panel");
    gboolean shown = panel && gtk_widget_get_visible(panel);
    if (shown) gtk_widget_hide(panel);
    GtkRequisition hidden_minimum, hidden_natural, hidden_header, hidden_header_natural;
    gtk_widget_get_preferred_size(native, &hidden_minimum, &hidden_natural);
    gtk_widget_get_preferred_size(header, &hidden_header, &hidden_header_natural);
    if (shown) gtk_widget_show(panel);
    g_print("Aynı grid minimum karşılaştırması: panel açık pencere=%d×%d header=%d×%d; panel gizli pencere=%d×%d header=%d×%d; genişlik farkı=%d/%d\n",
        full_minimum.width, full_minimum.height, header_minimum.width, header_minimum.height,
        hidden_minimum.width, hidden_minimum.height, hidden_header.width, hidden_header.height,
        full_minimum.width - hidden_minimum.width, header_minimum.width - hidden_header.width);
    if (GTK_IS_GRID(header)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(header));
        for (GList *child = children; child; child = child->next) {
            GtkWidget *widget = child->data;
            GtkStyleContext *style = gtk_widget_get_style_context(widget);
            const gchar *const classes[] = {"header-brand-area", "header-onair", "header-behavior", "header-next", "header-time", "header-meter"};
            for (guint i = 0; i < G_N_ELEMENTS(classes); ++i) {
                if (!gtk_style_context_has_class(style, classes[i])) continue;
                gint left = 0, top = 0, columns = 0, rows = 0;
                gtk_container_child_get(GTK_CONTAINER(header), widget, "left-attach", &left, "top-attach", &top,
                    "width", &columns, "height", &rows, NULL);
                GtkRequisition minimum, natural; gtk_widget_get_preferred_size(widget, &minimum, &natural);
                g_print("Grid %s: konum=%d,%d span=%d×%d minimum=%d×%d doğal=%d×%d\n",
                    classes[i], left, top, columns, rows, minimum.width, minimum.height, natural.width, natural.height);
            }
        }
        g_list_free(children);
    }
}

int
main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    if (argc < 5 || argc > 7 || (argc == 7 && g_strcmp0(argv[6], "view") && g_strcmp0(argv[6], "header"))) {
        g_printerr("Kullanım: %s GENİŞLİK YÜKSEKLİK YAZI_BOYUTU PNG [PROJE_KÖKÜ [view|header]]\n", argv[0]);
        return 2;
    }
    gboolean capture_view = argc == 7 && g_str_equal(argv[6], "view");
    gboolean capture_header = argc == 7 && g_str_equal(argv[6], "header");
    guint requested_width = 0, requested_height = 0, text_size = 0;
    if (!number(argv[1], 64, 8192, &requested_width) || !number(argv[2], 64, 8192, &requested_height) ||
        !number(argv[3], 0, 2, &text_size)) { g_printerr("Geçersiz boyut; yazı boyutu 0–2 olmalı.\n"); return 2; }
    gchar *project = g_canonicalize_filename(argc >= 6 ? argv[5] : RADIOS_TEST_PROJECT_ROOT, NULL);
    gchar *screenshot = g_canonicalize_filename(argv[4], NULL);
    if (!inside(project, screenshot)) {
        g_printerr("Ekran görüntüsü RadioS projesi içinde olmalı.\n"); g_free(project); g_free(screenshot); return 2;
    }
    if (!gtk_init_check(NULL, NULL)) { g_free(project); g_free(screenshot); return 77; }
    GError *error = NULL;
    RadioSConfig config = {0}; RadioSRuntime *runtime = NULL; RadioSWindow *window = NULL;
    GtkApplication *application = NULL;
    gchar *temporary = NULL, *config_directory = NULL, *preferences_file = NULL, *assets_link = NULL;
    GtkWidget *preferences_window = NULL, *header_window = NULL;
    RadioSViewContext header_context = {0};
    gint result = 1;
    if (!radios_config_load(project, "config/radios.example.ini", &config, &error)) goto cleanup;
    /* Separate preferences and suppress private credential loading through
     * smoke mode. Existing data/database files are never opened by this tool. */
    temporary = g_build_filename(project, "build", "prompt-2-tests", "layout-XXXXXX", NULL);
    if (!g_mkdtemp(temporary)) { g_printerr("Geçici görünüm dizini oluşturulamadı.\n"); goto cleanup; }
    config_directory = g_build_filename(temporary, "config", NULL);
    if (g_mkdir(config_directory, 0700)) { g_printerr("Geçici config dizini oluşturulamadı.\n"); goto cleanup; }
    /* Some existing native theme modules resolve assets under project_root.
     * Keep those read-only resources available in the isolated test root. */
    assets_link = g_build_filename(temporary, "assets", NULL);
    if (symlink(config.assets_root, assets_link)) { g_printerr("Geçici salt okunur assets bağlantısı oluşturulamadı.\n"); goto cleanup; }
    preferences_file = g_build_filename(config_directory, "ui-preferences.ini", NULL);
    g_free(config.project_root); config.project_root = g_strdup(temporary);
    RadioSUIPreferences preferences = { .text_size = text_size, .default_profile = 1, .last_profile = 0, .header_hidden_fields = 0,
        .content_scale_percent = 100, .mixer_scale_percent = 100, .effector_scale_percent = 100,
        .mixer_height_percent = 100, .effector_height_percent = 100 };
    if (!radios_ui_preferences_save(config.project_root, &preferences, &error)) goto cleanup;
    application = gtk_application_new("org.radios.Prompt2Layout", G_APPLICATION_NON_UNIQUE);
    if (!g_application_register(G_APPLICATION(application), NULL, &error)) goto cleanup;
    runtime = radios_runtime_new(32, 64);
    window = radios_window_new(application, &config, runtime, TRUE, NULL, &error);
    if (!window) goto cleanup;
    RadioSViewContext *ctx = radios_window_view_context(window);
    GtkWidget *native = GTK_WIDGET(ctx->parent);
    requested_size(ctx->parent, requested_width, requested_height);
    GtkWidget *size = find(native, "task-text-size");
    if (!GTK_IS_COMBO_BOX(size)) { g_printerr("Gerçek yazı boyutu kontrolü bulunamadı.\n"); goto cleanup; }
    gtk_combo_box_set_active(GTK_COMBO_BOX(size), (gint)text_size);
    radios_window_present(window);
    /* Let the native smoke timer finish its profile tour and diagnostics.
     * We use a bounded context loop, so g_application_quit cannot free the
     * window before our own capture and explicit cleanup. */
    gint64 deadline = g_get_monotonic_time() + 11 * G_TIME_SPAN_SECOND;
    while (!radios_window_smoke_passed(window) && g_get_monotonic_time() < deadline) settle(50000);
    if (!radios_window_smoke_passed(window)) { g_printerr("Gerçek pencere kabulü zaman aşımına uğradı.\n"); goto cleanup; }
    radios_window_select_profile(window, "broadcast");
    requested_size(ctx->parent, requested_width, requested_height);
    GtkWidget *header = g_object_get_data(G_OBJECT(ctx->parent), "radios-header-status-root");
    if (!header) { g_printerr("Gerçek header hook bulunamadı.\n"); goto cleanup; }
    radios_header_status_refresh(header);
    settle(500000);
    minimum_report(native, header);
    settle(100000);
    if (capture_header) {
        /* Keep the complete production window and its font/theme provider
         * alive. Use a fresh header so the grid starts in its own production
         * initial layout and responds to genuine narrow GTK allocations. */
        gtk_widget_hide(native);
        header_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        g_object_ref_sink(header_window);
        gtk_window_set_application(GTK_WINDOW(header_window), application);
        gtk_window_set_title(GTK_WINDOW(header_window), "RadioS — üst bar görsel kabul fixture");
        gtk_style_context_add_class(gtk_widget_get_style_context(header_window), "radios");
        requested_size(GTK_WINDOW(header_window), requested_width, requested_height);
        header_context = *ctx; header_context.parent = GTK_WINDOW(header_window);
        header = radios_header_new(&header_context);
        GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);
        GtkWidget *space = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(root), space, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(header_window), root);
        gtk_widget_show_all(header_window);
        gtk_window_present(GTK_WINDOW(header_window));
        settle(500000);
        requested_size(GTK_WINDOW(header_window), requested_width, requested_height);
        settle(500000);
        native = header_window;
        radios_header_status_refresh(header);
        minimum_report(native, header);
        settle(100000);
        g_print("Kapsam: gerçek üretim üst barı ayrı GTK penceresinde; ana pencerenin tüm sayfa minimumları bu çekimde yoktur.\n");
    }
    gint actual_width = 0, actual_height = 0;
    gtk_window_get_size(GTK_WINDOW(native), &actual_width, &actual_height);
    GtkRequisition minimum, natural; gtk_widget_get_preferred_size(native, &minimum, &natural);
    g_print("Görsel kabul TEST FİXTURE: başlık ve süre verileri sentetiktir; gerçek yayın değildir.\n");
    g_print("Pencere: istenen=%u×%u gerçek=%d×%d minimum=%d×%d doğal=%d×%d yazı=%u boyut_farkı=%s\n",
        requested_width, requested_height, actual_width, actual_height, minimum.width, minimum.height,
        natural.width, natural.height, text_size,
        actual_width != (gint)requested_width || actual_height != (gint)requested_height ? "VAR" : "yok");
    gboolean fits = actual_width >= minimum.width && actual_height >= minimum.height;
    if (capture_header) {
        gboolean exact = actual_width == (gint)requested_width && actual_height == (gint)requested_height;
        g_print("Üst bar istenen gerçek boyut=%s\n", exact ? "PASS" : "FAIL");
        fits = fits && exact;
    }
    const gchar *const names[] = {"header-status-programme-value", "header-status-music-value", "header-status-remaining-value"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *label = find(header, names[i]);
        fits = label_geometry(label, native, actual_width, actual_height) && fits;
        if (!label) continue;
        GtkWidget *parent = gtk_widget_get_parent(label);
        GList *children = gtk_container_get_children(GTK_CONTAINER(parent));
        GtkWidget *caption = children ? children->data : NULL;
        fits = label_geometry(caption, native, actual_width, actual_height) && fits;
        g_list_free(children);
    }
    const gchar *const detail_names[] = {"header-status-playlist-value", "header-status-programme-end-value",
        "header-status-playlist-end-value", "header-status-track-end-value"};
    for (guint i = 0; i < G_N_ELEMENTS(detail_names); ++i)
        fits = label_geometry(find(header, detail_names[i]), native, actual_width, actual_height) && fits;
    GtkWidget *next = find_class(header, "header-next");
    fits = next && legacy_geometry(next, native, actual_width, actual_height) && fits;
    gchar *directory = g_path_get_dirname(screenshot);
    if (g_mkdir_with_parents(directory, 0700)) fits = FALSE;
    g_free(directory);
    GtkWidget *capture_widget = native;
    if (capture_view) {
        preferences_window = radios_preferences_open(ctx);
        GtkWidget *categories = preferences_window ? find(preferences_window, "preferences-categories") : NULL;
        GtkWidget *view = preferences_window ? find(preferences_window, "preferences-category-view") : NULL;
        if (!GTK_IS_LIST_BOX(categories) || !GTK_IS_LIST_BOX_ROW(view)) { g_printerr("Gerçek Preferences Görünüm kategorisi bulunamadı.\n"); goto cleanup; }
        gtk_list_box_select_row(GTK_LIST_BOX(categories), GTK_LIST_BOX_ROW(view));
        settle(400000);
        gint view_width = 0, view_height = 0;
        gtk_window_get_size(GTK_WINDOW(preferences_window), &view_width, &view_height);
        g_print("Preferences View: gerçek=%d×%d; üç gerçek canlı görünürlük seçeneği\n", view_width, view_height);
        const gchar *const buttons[] = {"header-show-programme", "header-show-music", "header-show-remaining"};
        for (guint i = 0; i < G_N_ELEMENTS(buttons); ++i) {
            GtkWidget *button = find(preferences_window, buttons[i]);
            gboolean ready = GTK_IS_CHECK_BUTTON(button) && gtk_widget_get_mapped(button) && gtk_widget_get_sensitive(button);
            g_print("Preferences %s: canlı=%s seçili=%d\n", buttons[i], ready ? "PASS" : "FAIL",
                GTK_IS_TOGGLE_BUTTON(button) ? gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)) : 0);
            fits = ready && fits;
            if (ready) fits = label_geometry(gtk_bin_get_child(GTK_BIN(button)), preferences_window, view_width, view_height) && fits;
        }
        capture_widget = preferences_window;
    }
    gboolean captured = capture(capture_widget, screenshot);
    g_print("Header geometri=%s ekran görüntüsü=%s\n", fits ? "PASS" : "FAIL", captured ? "PASS" : "FAIL");
    result = fits && captured ? 0 : 1;

cleanup:
    if (error) { g_printerr("Görsel kabul: %s\n", error->message); g_clear_error(&error); }
    if (preferences_window) gtk_widget_destroy(preferences_window);
    if (header_window) { gtk_widget_destroy(header_window); g_object_unref(header_window); }
    radios_window_free(window);
    if (runtime) { radios_runtime_shutdown(runtime); radios_runtime_free(runtime); }
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_clear_object(&application);
    radios_config_clear(&config);
    if (preferences_file) g_remove(preferences_file);
    if (assets_link) g_remove(assets_link);
    if (config_directory) g_rmdir(config_directory);
    if (temporary) g_rmdir(temporary);
    g_free(preferences_file); g_free(config_directory); g_free(temporary); g_free(assets_link);
    g_free(project); g_free(screenshot);
    return result;
}
