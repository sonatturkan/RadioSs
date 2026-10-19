/* Kılavuz 18.10.2026 §06/§08 (A-12, A-18): the broadcast list of a long
 * programme stays responsive. Only the rows of the display window are built,
 * finished events leave it and the next ones fill it, and every per-tick
 * update stays bounded by the window instead of the programme size. */
#include "radios/programme_queue.h"
#include <string.h>

enum { EVENTS = 100, CONTENTS = 20, EVENT_SHOW = 8, CONTENT_SHOW = 20 };

static void pump(void)
{
    for (guint iteration = 0; iteration < 5; ++iteration) while (g_main_context_iteration(NULL, FALSE)) {}
}
static void noop_selected(gpointer data, const gchar *id) { (void)data; (void)id; }
static void noop_action(gpointer data, const gchar *id, RadioSProgrammeRowAction action) { (void)data; (void)id; (void)action; }
static gboolean noop_drop(gpointer data, RadioSDragInfo info, const gchar *payload, RadioSProgrammePlacement placement)
{ (void)data; (void)info; (void)payload; (void)placement; return FALSE; }

static gchar *uuid_for(guint kind, guint index)
{ return g_strdup_printf("%08x-0000-4000-8000-%012x", kind, index); }

/* EVENTS music events of CONTENTS contents; every fifth event is followed by
 * a one-content sweeper event that attaches below it as its 1.1 row. */
static JsonObject *programme_new(GPtrArray *leaf_ids, GPtrArray *event_ids)
{
    JsonObject *config = json_object_new(); JsonArray *blocks = json_array_new(), *events = json_array_new();
    json_object_set_string_member(config, "title", "Uzun program"); json_object_set_string_member(config, "date", "2026-10-18");
    json_object_set_string_member(config, "note", "");
    guint leaf = 0, sweepers = 0;
    for (guint e = 0; e < EVENTS; ++e) {
        for (guint pass = 0; pass < (e % 5 == 4 ? 2u : 1u); ++pass) {
            gboolean sweeper = pass == 1;
            gchar *block_id = uuid_for(sweeper ? 2 : 1, sweeper ? sweepers : e);
            JsonObject *block = json_object_new();
            json_object_set_string_member(block, "id", block_id); json_object_set_string_member(block, "kind", sweeper ? "sweeper" : "music");
            gchar *title = sweeper ? g_strdup_printf("Sweeper %u", sweepers) : g_strdup_printf("Müzik eventi %u", e);
            json_object_set_string_member(block, "title", title); g_free(title);
            json_object_set_string_member(block, "start_time", ""); json_object_set_string_member(block, "note", "");
            json_array_add_object_element(blocks, block); g_ptr_array_add(event_ids, g_strdup(block_id));
            for (guint c = 0; c < (sweeper ? 1u : (guint)CONTENTS); ++c) {
                gchar *id = uuid_for(3, leaf++); JsonObject *item = json_object_new();
                json_object_set_string_member(item, "id", id); json_object_set_string_member(item, "block_id", block_id);
                gchar *name = g_strdup_printf("Parça %u", leaf); json_object_set_string_member(item, "title", name); g_free(name);
                json_object_set_string_member(item, "artist", "Sanatçı"); json_object_set_string_member(item, "album", "");
                json_object_set_string_member(item, "kind", sweeper ? "sweeper" : "music");
                json_object_set_string_member(item, "start_time", ""); json_object_set_int_member(item, "duration_ms", sweeper ? 8000 : 180000);
                json_object_set_string_member(item, "version_label", ""); json_object_set_string_member(item, "primary_file", "");
                json_object_set_string_member(item, "backup_file", ""); json_object_set_string_member(item, "note", "");
                json_object_set_string_member(item, "media_id", "media");
                json_array_add_object_element(events, item); g_ptr_array_add(leaf_ids, id);
            }
            if (sweeper) ++sweepers;
            g_free(block_id);
        }
    }
    json_object_set_array_member(config, "blocks", blocks); json_object_set_array_member(config, "events", events);
    return config;
}

static guint rows_of(GtkWidget *queue, RadioSProgrammeRowLevel level)
{
    GtkListBox *list = g_object_get_data(G_OBJECT(queue), "list-box"); guint count = 0;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *item = rows; item; item = item->next) count += radios_programme_queue_row_level(item->data) == level;
    g_list_free(rows); return count;
}
static gchar *first_row_id(GtkWidget *queue)
{
    GtkListBox *list = g_object_get_data(G_OBJECT(queue), "list-box");
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, 0);
    return row ? g_strdup(radios_programme_queue_row_id(row)) : NULL;
}

static void test_long_programme_window_stays_bounded(void)
{
    GPtrArray *leaves = g_ptr_array_new_with_free_func(g_free), *events = g_ptr_array_new_with_free_func(g_free);
    JsonObject *config = programme_new(leaves, events);
    g_assert_cmpuint(leaves->len, ==, EVENTS * CONTENTS + EVENTS / 5);
    RadioSProgrammeQueueHooks hooks = {.data = NULL, .selected = noop_selected, .action = noop_action, .drop = noop_drop};
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL), *queue = radios_programme_queue_new(&hooks);
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 600); gtk_container_add(GTK_CONTAINER(window), queue);
    gtk_widget_show_all(window); pump();

    /* Unlimited list: the old behaviour, every event row is built. */
    gint64 started = g_get_monotonic_time();
    radios_programme_queue_render(queue, config, NULL); pump();
    gint64 unlimited = g_get_monotonic_time() - started;
    g_assert_cmpuint(rows_of(queue, RADIOS_PROGRAMME_ROW_MAIN), ==, EVENTS);

    /* The display window keeps the first EVENT_SHOW main events (with their
     * attached 1.1 rows) and builds nothing else. */
    started = g_get_monotonic_time();
    radios_programme_queue_set_window(queue, EVENT_SHOW, CONTENT_SHOW); pump();
    gint64 windowed = g_get_monotonic_time() - started;
    g_assert_cmpuint(rows_of(queue, RADIOS_PROGRAMME_ROW_MAIN), ==, EVENT_SHOW);
    g_assert_cmpuint(rows_of(queue, RADIOS_PROGRAMME_ROW_SUB), <=, EVENT_SHOW);
    started = g_get_monotonic_time();
    radios_programme_queue_render(queue, config, NULL); pump();
    gint64 rerender = g_get_monotonic_time() - started;
    g_test_message("render: unlimited %" G_GINT64_FORMAT " ms, window %" G_GINT64_FORMAT " ms, windowed re-render %" G_GINT64_FORMAT " ms",
        unlimited / 1000, windowed / 1000, rerender / 1000);
    g_assert_cmpint(rerender, <, 400 * 1000);

    /* Play through the first three events: each content completes, the next
     * one plays; finished events leave and new ones fill the window. Every
     * single update is bounded by the window, not by 2020 contents. */
    gint64 worst = 0, total = 0; guint ticks = 0;
    for (guint i = 0; i + 1 < leaves->len && i < 3 * CONTENTS + 1; ++i) {
        const gchar *playing = g_ptr_array_index(leaves, i), *next = g_ptr_array_index(leaves, i + 1);
        for (guint step = 0; step < 5; ++step) {
            started = g_get_monotonic_time();
            radios_programme_queue_set_progress(queue, playing, next, (guint64)step * 30000, 180000, FALSE);
            gint64 spent = g_get_monotonic_time() - started; worst = MAX(worst, spent); total += spent; ++ticks;
        }
        started = g_get_monotonic_time();
        radios_programme_queue_complete(queue, playing); pump();
        gint64 spent = g_get_monotonic_time() - started; worst = MAX(worst, spent); total += spent; ++ticks;
        g_assert_cmpuint(rows_of(queue, RADIOS_PROGRAMME_ROW_MAIN), <=, EVENT_SHOW);
    }
    g_test_message("updates: %u, worst %" G_GINT64_FORMAT " ms, average %.2f ms", ticks, worst / 1000, (gdouble)total / ticks / 1000.0);
    g_assert_cmpint(worst, <, 250 * 1000);
    /* The first three music events finished: the window now starts at the
     * fourth one and is full again (the next events filled it). */
    gchar *first = first_row_id(queue);
    g_assert_cmpstr(first, ==, g_ptr_array_index(events, 3));
    g_free(first);
    g_assert_cmpuint(rows_of(queue, RADIOS_PROGRAMME_ROW_MAIN), ==, EVENT_SHOW);

    /* The programme itself is unchanged: the list only shows a range of it. */
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(config, "events")), ==, leaves->len);
    gtk_widget_destroy(window); pump();
    json_object_unref(config); g_ptr_array_unref(leaves); g_ptr_array_unref(events);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check(&argc, &argv)) return 77;
    g_test_add_func("/programme-queue-scale/long-programme-window-stays-bounded", test_long_programme_window_stays_bounded);
    return g_test_run();
}
