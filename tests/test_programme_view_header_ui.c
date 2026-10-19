#include "radios/views.h"
#include "radios/runtime.h"
#include "radios/broadcast_header.h"
#include <string.h>

/* Kılavuz 2026-10-18 A-4: the shared header's PARÇA BİTİŞİNE counter shows the
 * selected event's or content's duration while that selection is not the
 * sounding item, and returns to the remaining time of the playing track when
 * the sounding item is selected or the selection is cleared. The production
 * header is real; only the broadcast observation is wrapped. */

static RadioSBroadcastHeaderSnapshot observed;
static gboolean have_observation;

gboolean __wrap_radios_broadcast_header_snapshot(RadioSViewContext *ctx, RadioSBroadcastHeaderSnapshot *out)
{
    (void)ctx; if (!out) return FALSE;
    memset(out, 0, sizeof *out); out->tempo_ratio = 1; out->selected_duration_ms = -1;
    if (have_observation) *out = observed;
    return have_observation;
}

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static void run_for(guint milliseconds)
{
    gint64 end = g_get_monotonic_time() + (gint64)milliseconds * 1000;
    do { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); } while (g_get_monotonic_time() < end);
}
static void playing_track(void)
{
    memset(&observed, 0, sizeof observed); have_observation = TRUE;
    observed.programme_active = observed.current = TRUE; observed.active_sources = 1; observed.tempo_ratio = 1;
    observed.duration_ms = 245000; observed.position_ms = 34000; observed.track_remaining_us = 211 * G_TIME_SPAN_SECOND;
    observed.selected_duration_ms = -1;
    g_strlcpy(observed.title, "Çalan parça", sizeof observed.title); g_strlcpy(observed.artist, "Sanatçı", sizeof observed.artist);
}
static void expect(GtkWidget *header, const gchar *caption, const gchar *clock)
{
    run_for(320);
    GtkWidget *label = find(header, "header-track-countdown-caption"), *countdown = find(header, "header-track-countdown");
    g_assert_true(GTK_IS_LABEL(label)); g_assert_true(GTK_IS_LABEL(countdown));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, caption);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(countdown)), ==, clock);
}

static void test_selected_event_and_content_duration_in_counter(void)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(window);
    RadioSViewContext ctx = {0}; ctx.parent = GTK_WINDOW(window); ctx.runtime = radios_runtime_new(8, 32);
    GtkWidget *header = radios_header_new(&ctx); gtk_container_add(GTK_CONTAINER(window), header);
    gtk_widget_show_all(window);
    /* A sounding track without a selection: the remaining time, as before. */
    playing_track(); expect(header, "PARÇA BİTİŞİNE", "03:31");
    /* A selected event that is not sounding: its total duration (83:54 in the guide's screen). */
    observed.selected = TRUE; observed.selected_content = FALSE; observed.selected_playing = FALSE;
    observed.selected_duration_ms = (83 * 60 + 54) * 1000; g_strlcpy(observed.selected_title, "Müzik eventi", sizeof observed.selected_title);
    expect(header, "EVENT SÜRESİ", "83:54");
    /* A selected content that is not sounding: its own duration. */
    observed.selected_content = TRUE; observed.selected_duration_ms = 232000;
    expect(header, "PARÇA SÜRESİ", "03:52");
    /* The sounding item selected: back to the remaining time. */
    observed.selected_playing = TRUE; expect(header, "PARÇA BİTİŞİNE", "03:31");
    /* Selection cleared: the old behaviour, including the unknown marker without a track. */
    observed.selected = FALSE; observed.selected_playing = FALSE; observed.selected_duration_ms = -1;
    expect(header, "PARÇA BİTİŞİNE", "03:31");
    observed.current = FALSE; observed.duration_ms = 0; observed.track_remaining_us = 0;
    expect(header, "PARÇA BİTİŞİNE", "—:—");
    /* A zero-duration selection (an empty event) still reads as a duration. */
    observed.selected = TRUE; observed.selected_content = FALSE; observed.selected_duration_ms = 0;
    expect(header, "EVENT SÜRESİ", "00:00");
    gtk_widget_destroy(window); g_object_unref(window); radios_runtime_free(ctx.runtime);
    while (g_main_context_iteration(NULL, FALSE)) {}
}

int main(int argc, char **argv)
{
    if (!gtk_init_check(&argc, &argv)) { g_printerr("Private GTK test skipped: no display.\n"); return 77; }
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/programme-view/header-selected-duration-counter", test_selected_event_and_content_duration_in_counter);
    return g_test_run();
}
