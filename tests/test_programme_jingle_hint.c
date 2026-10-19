#define main radios_programme_queue_fixture_main
#include "test_programme_queue.c"
#undef main

static JsonObject *event_at(Fixture *fixture, guint index)
{
    return json_array_get_object_element(json_object_get_array_member(fixture->config, "events"), index);
}
static void planned_set(JsonObject *event, const gchar *member, const gchar *json)
{
    GError *error = NULL;
    JsonNode *node = json_from_string(json, &error);
    g_assert_no_error(error); g_assert_nonnull(node);
    json_object_set_member(event, member, node);
}
static GtkWidget *meta_label(Fixture *fixture, guint index)
{
    GtkWidget *label = find(row(fixture, index), "programme-row-meta");
    g_assert_true(GTK_IS_LABEL(label));
    return label;
}
static void assert_meta(Fixture *fixture, guint index, const gchar *expected)
{
    GtkWidget *label = meta_label(fixture, index);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, expected);
    gchar *tooltip = gtk_widget_get_tooltip_text(label);
    g_assert_cmpstr(tooltip, ==, expected); g_free(tooltip);
}
/* PDF s.13–14: a planned voice is its own 1.1 sub-row right below its song;
 * its title is the voice's name and its subtitle the relation. The song's
 * subtitle no longer repeats the voices. */
static void assert_hint_row(Fixture *fixture, guint index, const gchar *id, const gchar *number,
    const gchar *title, const gchar *relation)
{
    GtkListBoxRow *item = GTK_LIST_BOX_ROW(row(fixture, index));
    g_assert_cmpint(radios_programme_queue_row_level(item), ==, RADIOS_PROGRAMME_ROW_SUB);
    g_assert_cmpstr(radios_programme_queue_row_number(item), ==, number);
    g_assert_cmpstr(radios_programme_queue_row_id(item), ==, id);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(with_class(GTK_WIDGET(item), "ld-item-title"))), ==, title);
    assert_meta(fixture, index, relation);
}
static gchar *hint_id(JsonObject *event, const gchar *member, guint index)
{
    JsonNode *node = json_object_get_member(event, member); g_assert_nonnull(node);
    JsonObject *hint = JSON_NODE_HOLDS_ARRAY(node) ? json_array_get_object_element(json_node_get_array(node), index) :
        json_node_get_object(node);
    return radios_programme_hint_id(event, hint, index);
}
/* Sub-rows are separate list rows; the contents themselves stay the main rows,
 * numbered 1, 2, 3 in order. */
static void assert_main_rows(Fixture *fixture, const gchar *const *ids, guint count)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(fixture->list)); guint mains = 0;
    for (GList *item = rows; item; item = item->next) {
        if (radios_programme_queue_row_level(item->data) != RADIOS_PROGRAMME_ROW_MAIN) continue;
        g_assert_cmpuint(mains, <, count);
        g_assert_cmpstr(radios_programme_queue_row_id(item->data), ==, ids[mains]);
        gchar *number = g_strdup_printf("%u", mains + 1);
        g_assert_cmpstr(radios_programme_queue_row_number(item->data), ==, number); g_free(number);
        ++mains;
    }
    g_list_free(rows); g_assert_cmpuint(mains, ==, count);
}

static void test_array_sub_rows_and_clear(Fixture *fixture, gconstpointer data)
{
    (void)data;
    JsonObject *event = event_at(fixture, 0);
    json_object_set_string_member(event, "album", "İlk albüm");
    json_object_set_string_member(event, "version_label", "Original");
    planned_set(event, "planned_jingles", "["
        "{\"title\":\"Radyo S Kısa\",\"placement\":\"overlay\",\"player\":3},"
        "{\"title\":\"İstasyon kimliği\",\"placement\":\"after\",\"player\":2}]");
    gchar *overlay = hint_id(event, "planned_jingles", 0), *after = hint_id(event, "planned_jingles", 1);
    radios_programme_queue_render(fixture->queue, fixture->config, NULL);
    open_first(fixture);
    assert_meta(fixture, 0, "İlk albüm · Müzik · Original");
    assert_hint_row(fixture, 1, overlay, "1.1", "Radyo S Kısa", "Üzerine jingle");
    assert_hint_row(fixture, 2, after, "1.2", "İstasyon kimliği", "Araya jingle");
    assert_meta(fixture, 3, "Müzik");
    /* The voices are rows of their song, not programme items: the contents
     * stay 1, 2, 3 and nothing else is inserted. */
    const gchar *ids[] = {TRACK_A, TRACK_B, TRACK_C}; assert_main_rows(fixture, ids, G_N_ELEMENTS(ids));
    g_assert_cmpuint(rows_count(fixture), ==, 5);
    /* The event view shows the voice that sounds over the song as 1.1 of its event. */
    back(fixture);
    const gchar *events[] = {BLOCK_A, BLOCK_B, BLOCK_EMPTY}; assert_main_rows(fixture, events, G_N_ELEMENTS(events));
    assert_hint_row(fixture, 1, overlay, "1.1", "Radyo S Kısa", "Üzerine jingle");
    g_assert_cmpuint(rows_count(fixture), ==, 4);
    json_object_remove_member(event, "planned_jingles");
    radios_programme_queue_render(fixture->queue, fixture->config, TRACK_A); pump();
    assert_meta(fixture, 0, "İlk albüm · Müzik · Original");
    assert_main_rows(fixture, ids, G_N_ELEMENTS(ids));
    g_assert_cmpuint(rows_count(fixture), ==, 3);
    g_free(overlay); g_free(after);
}

static void test_malformed_and_singleton(Fixture *fixture, gconstpointer data)
{
    (void)data;
    planned_set(event_at(fixture, 0), "planned_jingles", "[null,3,\"not a hint\",{},"
        "{\"title\":\"  \",\"placement\":\"after\"},"
        "{\"title\":42,\"placement\":\"overlay\"}]");
    planned_set(event_at(fixture, 1), "planned_jingle", "{\"title\":\"Ön anons\",\"placement\":\"before\"}");
    json_object_set_string_member(event_at(fixture, 2), "kind", "jingle");
    planned_set(event_at(fixture, 2), "planned_jingles", "[{\"title\":\"İstasyon kimliği\",\"placement\":\"after\"}]");
    gchar *singleton = hint_id(event_at(fixture, 1), "planned_jingle", 0);
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); open_first(fixture);
    /* Non-objects and blank or non-text titles make no sub-row. */
    const gchar *ids[] = {TRACK_A, TRACK_B, TRACK_C}; assert_main_rows(fixture, ids, G_N_ELEMENTS(ids));
    g_assert_cmpstr(radios_programme_queue_row_id(GTK_LIST_BOX_ROW(row(fixture, 1))), ==, TRACK_B);
    assert_meta(fixture, 0, "Müzik");
    /* The legacy singleton is one sub-row of its song, shown once. */
    assert_meta(fixture, 1, "Müzik");
    assert_hint_row(fixture, 2, singleton, "2.1", "Ön anons", "Önce jingle");
    /* A jingle content carries no voice of its own. */
    g_assert_cmpstr(radios_programme_queue_row_id(GTK_LIST_BOX_ROW(row(fixture, 3))), ==, TRACK_C);
    assert_meta(fixture, 3, "Jingle");
    g_assert_true(has_class(row(fixture, 3), "programme-kind-jingle"));
    g_assert_cmpuint(rows_count(fixture), ==, 4);
    g_free(singleton);
}

/* A hint outside the accepted placements (overlay · before · after) is not a
 * planned voice: the former subtitle ignored it, so its 1.1 row must not
 * appear either; a valid voice beside it still does. */
static void test_unknown_placement_ignored(Fixture *fixture, gconstpointer data)
{
    (void)data;
    planned_set(event_at(fixture, 0), "planned_jingles", "["
        "{\"title\":\"Unplanned\",\"placement\":\"unknown\"},"
        "{\"title\":\"Radyo S Kısa\",\"placement\":\"overlay\"}]");
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); open_first(fixture);
    const gchar *ids[] = {TRACK_A, TRACK_B, TRACK_C}; assert_main_rows(fixture, ids, G_N_ELEMENTS(ids));
    g_assert_null(with_text(GTK_WIDGET(fixture->list), "Unplanned"));
    g_assert_cmpuint(rows_count(fixture), ==, 4);
    g_assert_cmpint(radios_programme_queue_row_level(GTK_LIST_BOX_ROW(row(fixture, 1))), ==, RADIOS_PROGRAMME_ROW_SUB);
    g_assert_cmpstr(radios_programme_queue_row_number(GTK_LIST_BOX_ROW(row(fixture, 1))), ==, "1.1");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(with_class(row(fixture, 1), "ld-item-title"))), ==, "Radyo S Kısa");
    assert_meta(fixture, 1, "Üzerine jingle");
}

static void test_accepted_override_refresh_and_stop(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *ids[] = {TRACK_A, TRACK_B, TRACK_C};
    json_object_set_string_member(event_at(fixture, 0), "version_label", "Original");
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); open_first(fixture);
    g_assert_cmpuint(rows_count(fixture), ==, 3);
    JsonObject *accepted = radios_programme_clone_object(fixture->config);
    JsonObject *accepted_event = json_array_get_object_element(json_object_get_array_member(accepted, "events"), 0);
    planned_set(accepted_event, "planned_jingles", "[{\"title\":\"Gerçek jingle\",\"placement\":\"overlay\"}]");
    gchar *real = hint_id(accepted_event, "planned_jingles", 0);
    radios_programme_queue_set_run_config(fixture->queue, accepted); pump();
    assert_meta(fixture, 0, "Müzik · Original");
    assert_hint_row(fixture, 1, real, "1.1", "Gerçek jingle", "Üzerine jingle");
    assert_main_rows(fixture, ids, G_N_ELEMENTS(ids)); g_assert_cmpuint(rows_count(fixture), ==, 4);
    /* An unchanged accepted plan refreshes in place, without rebuilding rows. */
    GtkWidget *same_row = row(fixture, 0), *same_hint = row(fixture, 1);
    radios_programme_queue_set_run_config(fixture->queue, accepted); pump();
    g_assert_true(same_row == row(fixture, 0)); g_assert_true(same_hint == row(fixture, 1));
    /* The accepted copy remains frozen even when a planning object changes,
     * and it wins over the draft's own rule. */
    planned_set(accepted_event, "planned_jingles", "[{\"title\":\"Sonraki gerçek jingle\",\"placement\":\"after\"}]");
    gchar *next = hint_id(accepted_event, "planned_jingles", 0);
    planned_set(event_at(fixture, 0), "planned_jingles", "[{\"title\":\"Eski taslak\",\"placement\":\"after\"}]");
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); pump();
    assert_hint_row(fixture, 1, real, "1.1", "Gerçek jingle", "Üzerine jingle");
    assert_meta(fixture, 0, "Müzik · Original"); g_assert_cmpuint(rows_count(fixture), ==, 4);
    radios_programme_queue_set_run_config(fixture->queue, accepted); pump();
    assert_hint_row(fixture, 1, next, "1.1", "Sonraki gerçek jingle", "Araya jingle");
    assert_meta(fixture, 0, "Müzik · Original"); g_assert_cmpuint(rows_count(fixture), ==, 4);
    /* An accepted entry without a jingle suppresses an obsolete draft rule. */
    json_object_remove_member(accepted_event, "planned_jingles");
    radios_programme_queue_set_run_config(fixture->queue, accepted); pump();
    assert_main_rows(fixture, ids, G_N_ELEMENTS(ids)); g_assert_cmpuint(rows_count(fixture), ==, 3);
    assert_meta(fixture, 0, "Müzik · Original");
    planned_set(accepted_event, "planned_jingles", "[{\"title\":\"Gerçek jingle\",\"placement\":\"overlay\"}]");
    radios_programme_queue_set_run_config(fixture->queue, accepted); pump();
    assert_hint_row(fixture, 1, real, "1.1", "Gerçek jingle", "Üzerine jingle");
    /* Stopping the run removes the accepted voices; the draft's stays hidden. */
    radios_programme_queue_reset_progress(fixture->queue); pump();
    assert_main_rows(fixture, ids, G_N_ELEMENTS(ids)); g_assert_cmpuint(rows_count(fixture), ==, 3);
    assert_meta(fixture, 0, "Müzik · Original");
    /* A newly supplied preview may show its own planned jingle again. */
    planned_set(event_at(fixture, 0), "planned_jingles", "[{\"title\":\"Yeni önizleme\",\"placement\":\"after\"}]");
    gchar *preview = hint_id(event_at(fixture, 0), "planned_jingles", 0);
    radios_programme_queue_render(fixture->queue, fixture->config, NULL); pump();
    assert_hint_row(fixture, 1, preview, "1.1", "Yeni önizleme", "Araya jingle");
    assert_meta(fixture, 0, "Müzik · Original"); g_assert_cmpuint(rows_count(fixture), ==, 4);
    json_object_unref(accepted); g_free(real); g_free(next); g_free(preview);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check(&argc, &argv)) return 77;
    g_test_add("/programme-jingle-hint/array-sub-rows-and-clear", Fixture, NULL, setup, test_array_sub_rows_and_clear, teardown);
    g_test_add("/programme-jingle-hint/malformed-singleton-no-duplicate", Fixture, NULL, setup, test_malformed_and_singleton, teardown);
    g_test_add("/programme-jingle-hint/accepted-override-refresh-stop", Fixture, NULL, setup, test_accepted_override_refresh_and_stop, teardown);
    g_test_add("/programme-jingle-hint/unknown-placement-ignored", Fixture, NULL, setup, test_unknown_placement_ignored, teardown);
    return g_test_run();
}
