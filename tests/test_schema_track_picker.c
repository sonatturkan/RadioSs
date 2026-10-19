/* Kullanıcı isteği U2: rows of the "Şemadan yükle…" window follow the
 * calendar selection's own handover geometry. */
#include "radios/schema_track_picker.h"
#include <string.h>

static JsonObject *track(const gchar *title, const gchar *artist, const gchar *kind, gint64 duration_ms,
    gint64 autodj_in_ms, gint64 autodj_out_ms, gboolean markers)
{
    JsonObject *row = json_object_new(), *detail = json_object_new(), *media = json_object_new(), *version = json_object_new(), *metadata = json_object_new();
    gchar *media_id = g_uuid_string_random(), *version_id = g_uuid_string_random(), *entry_id = g_uuid_string_random();
    json_object_set_string_member(media, "_id", media_id); json_object_set_string_member(media, "title", title);
    json_object_set_string_member(media, "artist", artist); json_object_set_string_member(media, "kind", kind);
    json_object_set_string_member(version, "_id", version_id); json_object_set_int_member(version, "duration_ms", duration_ms);
    json_object_set_string_member(version, "sha256", "ab");
    json_object_set_object_member(detail, "media", media); json_object_set_object_member(detail, "version", version);
    if (markers) {
        JsonObject *marks = json_object_new();
        json_object_set_int_member(marks, "cue_in_ms", 0); json_object_set_int_member(marks, "out_ms", duration_ms);
        json_object_set_int_member(marks, "autodj_in_ms", autodj_in_ms); json_object_set_int_member(marks, "autodj_out_ms", autodj_out_ms);
        json_object_set_object_member(detail, "markers", marks);
    }
    json_object_set_string_member(metadata, "title", title); json_object_set_string_member(metadata, "artist", artist);
    json_object_set_string_member(metadata, "kind", kind);
    json_object_set_object_member(row, "detail", detail); json_object_set_object_member(row, "metadata", metadata);
    json_object_set_string_member(row, "entry_id", entry_id);
    g_free(media_id); g_free(version_id); g_free(entry_id); return row;
}
static void rows_follow_selection_geometry(void)
{
    const gint64 start = 1792324800000000;
    JsonObject *selection = json_object_new(); JsonArray *tracks = json_array_new();
    json_array_add_object_element(tracks, track("Birinci", "Sanatçı A", "music", 10000, 1000, 8000, TRUE));
    json_array_add_object_element(tracks, track("İkinci", "", "jingle", 4000, 0, 0, FALSE));
    JsonObject *waited = track("Üçüncü", "Sanatçı C", "music", 6000, 0, 5000, TRUE), *constraints = json_object_new();
    json_object_set_int_member(constraints, "earliest_utc_us", start + 60 * G_USEC_PER_SEC);
    json_object_set_object_member(waited, "calendar_constraints", constraints);
    json_object_set_boolean_member(waited, "calendar_window_wait", TRUE);
    json_array_add_object_element(tracks, waited);
    json_array_add_object_element(tracks, json_object_new()); /* malformed rows are skipped */
    json_object_set_array_member(selection, "tracks", tracks);

    JsonArray *rows = radios_schema_track_picker_rows(selection, start, FALSE);
    g_assert_cmpuint(json_array_get_length(rows), ==, 3);
    JsonObject *a = json_array_get_object_element(rows, 0), *b = json_array_get_object_element(rows, 1), *c = json_array_get_object_element(rows, 2);
    g_assert_cmpint(json_object_get_int_member(a, "number"), ==, 1); g_assert_cmpint(json_object_get_int_member(c, "number"), ==, 3);
    g_assert_cmpstr(json_object_get_string_member(a, "title"), ==, "Birinci"); g_assert_cmpstr(json_object_get_string_member(a, "artist"), ==, "Sanatçı A");
    g_assert_cmpstr(json_object_get_string_member(b, "kind"), ==, "jingle");
    /* AutoDJ IN 1 s → OUT 8 s hands over after 7 s; the span lasts to its OUT. */
    g_assert_cmpint(json_object_get_int_member(a, "start_utc_us"), ==, start);
    g_assert_cmpint(json_object_get_int_member(a, "duration_ms"), ==, 9000);
    g_assert_cmpint(json_object_get_int_member(b, "start_utc_us"), ==, start + 7 * G_USEC_PER_SEC);
    g_assert_cmpint(json_object_get_int_member(b, "duration_ms"), ==, 4000);
    /* A row waiting for its window starts there, never at the previous end. */
    g_assert_cmpint(json_object_get_int_member(c, "start_utc_us"), ==, start + 60 * G_USEC_PER_SEC);
    g_assert_true(g_uuid_string_is_valid(json_object_get_string_member(c, "media_id")));
    g_assert_true(g_uuid_string_is_valid(json_object_get_string_member(c, "version_id")));
    json_array_unref(rows);

    /* A single player plays cue IN → OUT: no AutoDJ IN/OUT handover. */
    rows = radios_schema_track_picker_rows(selection, start, TRUE);
    a = json_array_get_object_element(rows, 0); b = json_array_get_object_element(rows, 1);
    g_assert_cmpint(json_object_get_int_member(a, "duration_ms"), ==, 10000);
    g_assert_cmpint(json_object_get_int_member(b, "start_utc_us"), ==, start + 10 * G_USEC_PER_SEC);
    json_array_unref(rows);

    JsonObject *empty = json_object_new(); rows = radios_schema_track_picker_rows(empty, start, FALSE);
    g_assert_cmpuint(json_array_get_length(rows), ==, 0); json_array_unref(rows); json_object_unref(empty);
    rows = radios_schema_track_picker_rows(NULL, start, FALSE); g_assert_cmpuint(json_array_get_length(rows), ==, 0); json_array_unref(rows);
    json_object_unref(selection);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/schema-track-picker/rows-follow-selection-geometry", rows_follow_selection_geometry);
    return g_test_run();
}
