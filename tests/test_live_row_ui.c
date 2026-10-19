/* Kılavuz 18.10.2026 s.6, s.8, s.15 (A-1, A-2, A-3, A-19): L and R of a live
 * event row, end to end with real PCM. The general queue is fed by the main
 * calendar; its schema plays A → live talk step (channels 3 and 4) → C. L
 * opens the step's mixer channels and the on-air microphone latch while A
 * keeps playing; R is push-to-talk only while the live step is on air;
 * releasing R never advances the flow; the step ends by its own rule
 * (manual: L switched off at the step, duration: the planned bound). On a
 * music step L starts the step's own schema (live.automation_id) as the
 * event's independent Yarı AutoDJ run beside the unchanged flow.
 * Runs only inside run_playout_ui.py's private DB and owned null outputs. */
#define _POSIX_C_SOURCE 200809L
#include <glib.h>
static int live_row_run(void);
#define main radios_live_row_fixture_main
#define g_test_run live_row_run
#include "test_playout_ui.c"
#undef g_test_run
#undef main
#include "radios/header_navigation.h"
#include "radios/event_playout.h"

#define LIVE_ROW_PROGRAMME "Canlı satır programı"
#define LIVE_ROW_STEP "Sabah sohbeti"
#define LIVE_ROW_A "Canlı satır A"
#define LIVE_ROW_C "Canlı satır C"
#define LIVE_ROW_A_HZ 700.0
#define LIVE_ROW_C_HZ 1700.0
#define LIVE_ROW_A2 "Canlı satır A2"
#define LIVE_ROW_A2_HZ 1100.0
#define LIVE_ROW_S "Canlı satır şema parçası"
#define LIVE_ROW_S_HZ 2600.0

/* ---- Calendar: test_general_queue_ui.c calendar() with the schema changes */
static void live_row_flags(JsonObject *source)
{
    const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
    for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(source, flags[i], FALSE);
}
static JsonObject *live_row_source_common(void)
{
    JsonObject *source = json_object_new(); gchar *id = g_uuid_string_random();
    json_object_set_string_member(source, "id", id); g_free(id);
    json_object_set_int_member(source, "frequency", 1); json_object_set_int_member(source, "count", 1); json_object_set_int_member(source, "weight", 100);
    json_object_set_int_member(source, "start_minute", 0); json_object_set_int_member(source, "end_minute", 1440);
    json_object_set_int_member(source, "interval_minutes", 0); live_row_flags(source); return source;
}
static JsonObject *live_row_playlist_source(JsonNode *playlist)
{
    JsonObject *source = live_row_source_common();
    json_object_set_string_member(source, "playlist_id", radios_json_string(json_node_get_object(playlist), "_id", ""));
    json_object_set_string_member(source, "media_kind", "music"); return source;
}
/* docs/kilavuz-2026-10-18-4c/sozlesme.md §2: no playlist_id, a live object. */
static JsonObject *live_row_step_source(const gchar *label, const gchar *end_rule, gint64 duration_ms, const gchar *automation_id)
{
    JsonObject *source = live_row_source_common(), *live = json_object_new();
    json_object_set_string_member(source, "source_type", "live");
    json_object_set_string_member(live, "title", LIVE_ROW_STEP); json_object_set_string_member(live, "label", label);
    JsonArray *channels = json_array_new(); json_array_add_int_element(channels, 3); json_array_add_int_element(channels, 4);
    json_object_set_array_member(live, "channels", channels); json_object_set_string_member(live, "automation_id", automation_id);
    json_object_set_string_member(live, "end_rule", end_rule); json_object_set_int_member(live, "duration_ms", duration_ms);
    json_object_set_object_member(source, "live", live); return source;
}
static JsonObject *live_row_definition(const gchar *name, JsonArray *sources)
{
    JsonObject *definition = json_object_new(), *settings = json_object_new(); gchar *id = g_uuid_string_random();
    json_object_set_string_member(definition, "id", id); g_free(id); json_object_set_string_member(definition, "name", name);
    json_object_set_string_member(settings, "selection_mode", "linear");
    const gchar *gaps[] = {"track_gap_minutes", "artist_gap_minutes", "album_gap_minutes"};
    for (guint i = 0; i < G_N_ELEMENTS(gaps); ++i) json_object_set_int_member(settings, gaps[i], 0);
    JsonArray *transitions = json_array_new();
    for (guint i = 0; i < 3; ++i) {
        JsonObject *transition = json_object_new(); json_object_set_int_member(transition, "mode", i);
        json_object_set_int_member(transition, "weight", i ? 0 : 100); json_object_set_int_member(transition, "overlap_ms", 0);
        json_array_add_object_element(transitions, transition);
    }
    json_object_set_array_member(settings, "transitions", transitions);
    json_object_set_boolean_member(settings, "automation_schema", TRUE);
    json_object_set_object_member(definition, "settings", settings);
    json_object_set_array_member(definition, "sources", sources); return definition;
}
/* schema: the playlist of a second automation_schema definition that the
 * live step names as its automation_id (*schema_id, owned by the caller). */
static JsonNode *live_row_calendar(JsonNode *a, JsonNode *c, JsonNode *schema, gint64 start_us,
    const gchar *label, const gchar *end_rule, gint64 duration_ms, gchar **schema_id)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *root = json_object_new(); json_node_take_object(node, root);
    json_object_set_int_member(root, "schema", 1); json_object_set_string_member(root, "timezone", "UTC");
    JsonArray *library = json_array_new(); JsonObject *music = NULL;
    if (schema) {
        JsonArray *own = json_array_new(); json_array_add_object_element(own, live_row_playlist_source(schema));
        music = live_row_definition("Canlı satır müzik şeması", own);
    }
    *schema_id = g_strdup(music ? radios_json_string(music, "id", "") : "");
    /* Outer order (linear): A (1) → live step (2) → C (3). */
    JsonArray *sources = json_array_new();
    json_array_add_object_element(sources, live_row_playlist_source(a));
    json_array_add_object_element(sources, live_row_step_source(label, end_rule, duration_ms, *schema_id));
    json_array_add_object_element(sources, live_row_playlist_source(c));
    JsonObject *definition = live_row_definition("Canlı satır şeması", sources);
    gchar *definition_id = g_strdup(radios_json_string(definition, "id", ""));
    json_array_add_object_element(library, definition);
    if (music) json_array_add_object_element(library, music);
    json_object_set_array_member(root, "rotation_library", library);
    GDateTime *start = g_date_time_new_from_unix_utc(start_us / G_USEC_PER_SEC); gchar *date = g_date_time_format(start, "%Y-%m-%d");
    gint minute = g_date_time_get_hour(start) * 60 + g_date_time_get_minute(start);
    JsonObject *event = json_object_new(), *assignment = json_object_new(); gchar *event_id = g_uuid_string_random(), *assignment_id = g_uuid_string_random();
    json_object_set_string_member(event, "id", event_id); json_object_set_string_member(event, "title", LIVE_ROW_PROGRAMME);
    json_object_set_string_member(event, "date", date); json_object_set_string_member(event, "color", "#abd6e4");
    json_object_set_int_member(event, "start_minute", minute); json_object_set_int_member(event, "end_minute", MIN(minute + 3, 1440));
    json_object_set_string_member(assignment, "id", assignment_id); json_object_set_string_member(assignment, "rotation_id", definition_id);
    json_object_set_int_member(assignment, "weight", 100);
    JsonArray *assignments = json_array_new(); json_array_add_object_element(assignments, assignment); json_object_set_array_member(event, "rotations", assignments);
    JsonArray *events = json_array_new(); json_array_add_object_element(events, event); json_object_set_array_member(root, "events", events);
    g_free(definition_id); g_free(event_id); g_free(assignment_id); g_free(date); g_date_time_unref(start); return node;
}

/* ---- Observation helpers ------------------------------------------------ */
typedef struct { gboolean ready, gate, latch, ptt; } LiveRowMic;
static LiveRowMic live_row_mic(Window *window)
{
    LiveRowMic state = {0}; RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window->ctx); g_assert_nonnull(engine);
    RadioSAudioSnapshot *snapshot = g_new0(RadioSAudioSnapshot, 1); radios_audio_engine_snapshot(engine, snapshot);
    state.ready = snapshot->microphone_ready; state.gate = snapshot->microphone_gate;
    state.latch = snapshot->microphone_latch; state.ptt = snapshot->microphone_ptt;
    g_free(snapshot); radios_audio_engine_unref(engine); return state;
}
static gboolean live_row_mic_ready(gpointer value) { return live_row_mic(value).ready; }
static RadioSPlayoutSnapshot live_row_flow(Window *window)
{ RadioSPlayoutSnapshot snapshot = {0}; g_assert_true(radios_playout_ui_get_snapshot(window->ctx, &snapshot)); return snapshot; }
static guint64 live_row_epoch(Window *window)
{ RadioSAudioRunSnapshot run = {0}; g_assert_true(radios_playout_ui_navigation_snapshot(window->ctx, &run)); return run.epoch; }
typedef struct { Window *window; RadioSPlayoutState state; const gchar *entry; guint64 position; } LiveRowFlowCondition;
static gboolean live_row_flow_matches(gpointer value)
{
    LiveRowFlowCondition *condition = value; RadioSPlayoutSnapshot snapshot = {0};
    return radios_playout_ui_get_snapshot(condition->window->ctx, &snapshot) && snapshot.state == condition->state &&
        (!condition->entry || g_str_equal(snapshot.entry_id, condition->entry)) && snapshot.position_ms >= condition->position;
}
static RadioSPlayoutSnapshot live_row_flow_wait(Window *window, RadioSPlayoutState state, const gchar *entry, guint64 position, guint seconds, const gchar *what)
{
    LiveRowFlowCondition condition = {window, state, entry, position}; gint64 end = g_get_monotonic_time() + (gint64)seconds * G_USEC_PER_SEC;
    while (!live_row_flow_matches(&condition) && g_get_monotonic_time() < end) pump();
    RadioSPlayoutSnapshot snapshot = live_row_flow(window);
    if (!live_row_flow_matches(&condition))
        g_error("Live row flow (%s): expected state%d entry %s cursor>=%" G_GUINT64_FORMAT "; actual state%d index%u/%u entry %s '%s' cursor%" G_GUINT64_FORMAT ": %s",
            what, state, entry ? entry : "*", position, snapshot.state, snapshot.index, snapshot.total, snapshot.entry_id, snapshot.title, snapshot.position_ms, snapshot.error);
    return snapshot;
}
static gboolean live_row_has_class(GtkWidget *widget, const gchar *name)
{ return gtk_style_context_has_class(gtk_widget_get_style_context(widget), name); }
static void live_row_labels(GtkWidget *root)
{
    if (GTK_IS_LABEL(root)) g_test_message("Queue label: %s", gtk_label_get_text(GTK_LABEL(root)));
    if (!GTK_IS_CONTAINER(root)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    for (GList *item = children; item; item = item->next) live_row_labels(item->data);
    g_list_free(children);
}

/* The accepted plan: A carries the live_step hint of the following talk step;
 * C is the next media row (its earliest start is the duration bound). */
typedef struct { gchar *a, *c, *block, *a_block, *c_block; gint64 bound_utc_us, a_planned_utc_us, c_planned_utc_us; } LiveRowPlan;
static JsonObject *live_row_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static void live_row_plan_read(Window *window, LiveRowPlan *plan, const gchar *label, const gchar *end_rule, gint64 duration_ms, const gchar *automation_id)
{
    JsonNode *node = radios_playout_ui_plan_snapshot(window->ctx); g_assert_nonnull(node);
    JsonNode *member = json_object_get_member(json_node_get_object(node), "entries");
    g_assert_true(member && JSON_NODE_HOLDS_ARRAY(member)); JsonArray *entries = json_node_get_array(member);
    g_assert_cmpuint(json_array_get_length(entries), >=, 2);
    /* A's event (one or two contents), the live step hint on its last row, C. */
    JsonObject *first = json_array_get_object_element(entries, 0), *second = NULL, *before = NULL;
    for (guint i = 1; i < json_array_get_length(entries) && !second; ++i) {
        JsonObject *row = json_array_get_object_element(entries, i);
        if (!g_strcmp0(radios_json_string(row, "title", ""), LIVE_ROW_C)) { second = row; before = json_array_get_object_element(entries, i - 1); }
    }
    g_assert_cmpstr(radios_json_string(first, "title", ""), ==, LIVE_ROW_A); g_assert_nonnull(second);
    JsonObject *step = live_row_member(before, "live_step"); g_assert_nonnull(step);
    g_assert_null(live_row_member(second, "live_step"));
    g_assert_cmpstr(radios_json_string(step, "label", ""), ==, label);
    g_assert_cmpstr(radios_json_string(step, "end_rule", ""), ==, end_rule);
    g_assert_cmpint(radios_json_int(step, "duration_ms", -1), ==, duration_ms);
    g_assert_cmpstr(radios_json_string(step, "automation_id", "-"), ==, automation_id);
    g_assert_cmpint(radios_json_int(step, "outer_order", 0), ==, 2); g_assert_cmpint(radios_json_int(step, "inner_order", -1), ==, 0);
    JsonNode *channels = json_object_get_member(step, "channels"); g_assert_true(channels && JSON_NODE_HOLDS_ARRAY(channels));
    g_assert_cmpuint(json_array_get_length(json_node_get_array(channels)), ==, 2);
    g_assert_cmpint(json_array_get_int_element(json_node_get_array(channels), 0), ==, 3);
    g_assert_cmpint(json_array_get_int_element(json_node_get_array(channels), 1), ==, 4);
    plan->a = g_strdup(radios_json_string(first, "entry_id", "")); plan->c = g_strdup(radios_json_string(second, "entry_id", ""));
    plan->block = g_strdup(radios_json_string(step, "block_id", ""));
    plan->a_block = g_strdup(radios_json_string(first, "block_id", "")); plan->c_block = g_strdup(radios_json_string(second, "block_id", ""));
    g_assert_true(g_uuid_string_is_valid(plan->a)); g_assert_true(g_uuid_string_is_valid(plan->c)); g_assert_true(g_uuid_string_is_valid(plan->block));
    /* broadcast.c live_step_record reads the step bound from this row. */
    plan->bound_utc_us = radios_json_int(live_row_member(second, "calendar_constraints"), "earliest_utc_us", 0);
    plan->a_planned_utc_us = radios_json_int(first, "planned_utc_us", 0); plan->c_planned_utc_us = radios_json_int(second, "planned_utc_us", 0);
    GString *members = g_string_new(NULL); GList *names = json_object_get_members(second);
    for (GList *name = names; name; name = name->next) g_string_append_printf(members, "%s%s", members->len ? "," : "", (const gchar *)name->data);
    g_list_free(names);
    g_test_message("Accepted plan: A %s planned %" G_GINT64_FORMAT " (%" G_GINT64_FORMAT " ms) · live step %s (%s, %" G_GINT64_FORMAT " ms) · C %s planned %" G_GINT64_FORMAT " (+%.3f s after A's planned end) · calendar_constraints.earliest_utc_us %" G_GINT64_FORMAT " · C row members: %s",
        plan->a, plan->a_planned_utc_us, radios_json_int(first, "duration_ms", 0), plan->block, end_rule, duration_ms, plan->c, plan->c_planned_utc_us,
        (plan->c_planned_utc_us - plan->a_planned_utc_us - radios_json_int(first, "duration_ms", 0) * 1000) / 1e6, plan->bound_utc_us, members->str);
    g_string_free(members, TRUE); json_node_free(node);
}

/* The live event row of the queue (eyebrow "Event · Canlı") and its L / R.
 * Rows are rebuilt by the queue, so every use looks them up again. */
typedef struct { GtkListBoxRow *row; GtkWidget *live, *talk; } LiveRowButtons;
static gboolean live_row_find(Window *window, const gchar *block, LiveRowButtons *out)
{
    memset(out, 0, sizeof *out);
    GtkWidget *queue = find(window->root, "programme-queue-table"); if (!queue) return FALSE;
    GtkListBox *list = g_object_get_data(G_OBJECT(queue), "list-box"); if (!list) return FALSE;
    for (gint index = 0;; ++index) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, index);
        if (!row) return FALSE;
        if (g_strcmp0(radios_programme_queue_row_id(row), block) || radios_programme_queue_row_level(row) != RADIOS_PROGRAMME_ROW_MAIN) continue;
        if (!label_present(GTK_WIDGET(row), "Event · Canlı") || !label_present(GTK_WIDGET(row), LIVE_ROW_STEP)) return FALSE;
        out->row = row; out->live = find(GTK_WIDGET(row), "programme-row-live"); out->talk = find(GTK_WIDGET(row), "programme-row-talk");
        return GTK_IS_BUTTON(out->live) && GTK_IS_BUTTON(out->talk);
    }
}
typedef struct { Window *window; const gchar *block; } LiveRowWanted;
static gboolean live_row_present(gpointer value)
{ LiveRowWanted *wanted = value; LiveRowButtons buttons; return live_row_find(wanted->window, wanted->block, &buttons); }
static LiveRowButtons live_row_get(Window *window, const gchar *block)
{
    LiveRowWanted wanted = {window, block}; gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (!live_row_present(&wanted) && g_get_monotonic_time() < end) pump();
    LiveRowButtons buttons;
    if (!live_row_find(window, block, &buttons)) {
        live_row_labels(find(window->root, "programme-queue-table"));
        g_error("The live event row %s (Event · Canlı / %s) with L and R is absent from the broadcast queue", block, LIVE_ROW_STEP);
    }
    return buttons;
}
static gboolean live_row_talk_enabled(gpointer value)
{ LiveRowWanted *wanted = value; LiveRowButtons buttons; return live_row_find(wanted->window, wanted->block, &buttons) && gtk_widget_get_sensitive(buttons.talk); }
/* R reacts to its own pointer press/release handlers (programme_queue.c
 * talk_button_event); the event goes to exactly those handlers. */
static void live_row_talk_event(GtkWidget *button, GdkEventType type)
{
    GdkEvent *event = gdk_event_new(type); GdkWindow *surface = gtk_widget_get_window(button);
    event->button.window = surface ? g_object_ref(surface) : NULL; event->button.send_event = TRUE;
    event->button.button = 1; event->button.time = GDK_CURRENT_TIME;
    GtkAllocation allocation; gtk_widget_get_allocation(button, &allocation);
    event->button.x = allocation.x + allocation.width / 2.0; event->button.y = allocation.y + allocation.height / 2.0;
    gdk_event_set_device(event, gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(button))));
    gboolean handled = FALSE;
    g_signal_emit_by_name(button, type == GDK_BUTTON_PRESS ? "button-press-event" : "button-release-event", event, &handled);
    gdk_event_free(event);
}
/* The queue keeps its private state as object data; its first member is the
 * public RadioSProgrammeQueueHooks the broadcast view installed (programme_
 * queue.c Queue), so this is the very hook path a row control calls. */
static RadioSProgrammeQueueHooks *live_row_hooks(Window *window)
{
    GtkWidget *queue = find(window->root, "programme-queue-table"); g_assert_nonnull(queue);
    RadioSProgrammeQueueHooks *hooks = g_object_get_data(G_OBJECT(queue), "radios-programme-queue");
    g_assert_nonnull(hooks); g_assert_nonnull(hooks->action); return hooks;
}

/* ---- Fixture: general queue playing A of the A → live → C schema -------- */
typedef struct {
    const gchar *label, *end_rule; gint64 duration_ms; guint a_ms, c_ms;
    gboolean schema; /* the step names a second schema of its own (automation_id) */
    gboolean assigned; /* a player cluster carries an assigned weekly programme, selected in the header */
    gboolean second_a; /* A's playlist carries a second track (A2) after A */
} LiveRowSetup;
typedef struct {
    Window window; RadioSPlayerWeekly *driver;
    JsonNode *a, *a2, *c, *s, *pa, *pc, *ps, *saved, *assigned; gchar *schema_id, cluster[37];
    LiveRowPlan plan; gboolean channel_was_on_air[2];
    gint64 a_started_utc_us; /* observed: wall time minus A's cursor */
} LiveRow;
static gboolean live_row_programme_ready(gpointer value)
{
    GeneralReady *ready = value; RadioSPlayerWeeklySnapshot snapshot = weekly_snapshot(ready->driver);
    return !snapshot.armed && snapshot.state == RADIOS_PLAYER_WEEKLY_LOADED && !snapshot.error[0] &&
        snapshot.start_utc_us == ready->start && g_str_equal(snapshot.current_event_name, ready->title) && queue_contents(ready->window) > 0;
}
typedef struct { GtkWidget *combo; const gchar *id; } LiveRowHeaderChoice;
static gboolean live_row_header_choice(gpointer value)
{ LiveRowHeaderChoice *choice = value; return gtk_combo_box_set_active_id(GTK_COMBO_BOX(choice->combo), choice->id); }
/* A plain tone of the given length (test_playout_ui.c media_import makes 6 s). */
static JsonNode *live_row_media_long(Window *window, const gchar *title, gdouble frequency, gdouble amplitude, const gchar *kind, guint seconds)
{
    guint frames = 48000 * seconds + 31, bytes = frames * 4; guchar *wav = g_malloc(44 + bytes);
    memcpy(wav, "RIFF", 4); write_u32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8); write_u32(wav + 16, 16);
    write_u16(wav + 20, 1); write_u16(wav + 22, 2); write_u32(wav + 24, 48000); write_u32(wav + 28, 48000 * 4);
    write_u16(wav + 32, 4); write_u16(wav + 34, 16); memcpy(wav + 36, "data", 4); write_u32(wav + 40, bytes);
    for (guint i = 0; i < frames; ++i) {
        gint16 sample = (gint16)lrint(sin(2 * G_PI * frequency * i / 48000) * amplitude * 32767);
        write_u16(wav + 44 + i * 4, (guint16)sample); write_u16(wav + 46 + i * 4, (guint16)sample);
    }
    gchar *uuid = g_uuid_string_random(), *name = g_strconcat(uuid, ".wav", NULL), *path = g_build_filename(config.project_root, name, NULL);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL)); g_free(wav); g_free(uuid); g_free(name);
    Import output = {0};
    GCancellable *handle = radios_media_import_async(window->ctx, window->root, path, kind, title, import_complete, &output, import_destroy);
    g_assert_nonnull(handle); wait_until(import_done, &output, "real SHA-bound PCM analysis/import of a long tone");
    g_assert_no_error(output.error); g_assert_nonnull(output.result);
    JsonObject *media = json_object_get_object_member(json_node_get_object(output.result), "media");
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "media_id", radios_json_string(media, "_id", ""));
    JsonNode *detail = request("/v1/workflows/desktop-media/get", body); json_node_free(body); json_node_free(output.result);
    g_assert_cmpint(radios_json_int(json_object_get_object_member(json_node_get_object(detail), "version"), "duration_ms", 0), ==, seconds * 1000 + 1);
    g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); g_clear_object(&handle); return detail;
}
static void live_row_open(LiveRow *f, const LiveRowSetup *setup)
{
    memset(f, 0, sizeof *f); f->window = window_open(); Window *w = &f->window;
    saved_player_notices_attach(w); assert_gates_off(w); navigate(w, "broadcast");
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(w->root, "programme-general-continuous"))));
    f->driver = general_driver(w);
    f->a = routing_short_markers(setup->a_ms > 6000 ? live_row_media_long(w, LIVE_ROW_A, LIVE_ROW_A_HZ, .22, "music", (setup->a_ms + 999) / 1000) :
        media_import(w, LIVE_ROW_A, LIVE_ROW_A_HZ, .22, "music"), setup->a_ms);
    f->c = routing_short_markers(media_import(w, LIVE_ROW_C, LIVE_ROW_C_HZ, .22, "music"), setup->c_ms);
    if (setup->second_a) f->a2 = routing_short_markers(media_import(w, LIVE_ROW_A2, LIVE_ROW_A2_HZ, .22, "music"), 3000);
    f->pa = playlist_create("Canlı satır A kaynağı", f->a, f->a2); f->pc = playlist_create("Canlı satır C kaynağı", f->c, NULL);
    if (setup->schema) { f->s = media_import(w, LIVE_ROW_S, LIVE_ROW_S_HZ, .22, "music"); f->ps = playlist_create("Canlı satır şema kaynağı", f->s, NULL); }
    GError *error = NULL;
    for (guint i = 0; i < 2; ++i) f->channel_was_on_air[i] = radios_audio_ui_channel_on_air(w->ctx, 3 + i);
    /* The talk strips start off air, so L has strips of its own to open and,
     * when it closes, to put back exactly as they were. A music step opens
     * no strip; its schema needs a free player whose strip is on air. */
    for (guint i = 0; i < 2 && g_strcmp0(setup->label, "music"); ++i) {
        require_call(radios_audio_ui_set_channel_on_air(w->ctx, 3 + i, FALSE, &error), &error);
        g_assert_false(radios_audio_ui_channel_on_air(w->ctx, 3 + i));
    }
    g_test_message("Mixer strips 3/4 on air before the fixture: %d/%d%s", f->channel_was_on_air[0], f->channel_was_on_air[1],
        g_strcmp0(setup->label, "music") ? " (set off air)" : "");
    wait_until(live_row_mic_ready, w, "private microphone source (MONITOR monitor) is ready for the live latch");
    LiveRowMic mic = live_row_mic(w); g_assert_false(mic.gate); g_assert_false(mic.latch); g_assert_false(mic.ptt);
    /* The slot covering the current UTC minute plays from its first outer
     * order at once (player_rotation.c general_tick: an active occurrence is
     * prepared with now as its planning time), so no minute boundary wait;
     * the runner gives one case 100 s. The slot never crosses midnight. */
    gint64 minute = 60 * G_USEC_PER_SEC, day = 24 * 60 * minute;
    if (day - g_get_real_time() % day < 40 * G_USEC_PER_SEC) weekly_wait_real_time((g_get_real_time() / day + 1) * day + G_USEC_PER_SEC);
    /* The step's schema is prepared for [current minute, + duration rounded
     * up to minutes) from the L press on (local_event_prepare.c schema_window):
     * L must come early enough in a minute for a whole schema track. */
    if (setup->schema && g_get_real_time() % minute > 30 * G_USEC_PER_SEC)
        weekly_wait_real_time((g_get_real_time() / minute + 1) * minute + G_USEC_PER_SEC);
    gint64 start = g_get_real_time() / minute * minute;
    JsonNode *calendar = live_row_calendar(f->pa, f->pc, f->ps, start, setup->label, setup->end_rule, setup->duration_ms, &f->schema_id);
    f->saved = document_save("automation_calendar", "main", json_node_get_object(calendar)); json_node_free(calendar);
    radios_player_weekly_general_refresh(f->driver);
    GeneralReady ready = {w, f->driver, LIVE_ROW_PROGRAMME, start};
    gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
    while (!live_row_programme_ready(&ready) && g_get_monotonic_time() < deadline) pump();
    if (!live_row_programme_ready(&ready)) {
        RadioSPlayerWeeklySnapshot snapshot = weekly_snapshot(f->driver);
        g_error("The live schema programme was not projected: state%d armed%d current '%s' next '%s' error '%s'",
            snapshot.state, snapshot.armed, snapshot.current_event_name, snapshot.next_event_name, snapshot.error);
    }
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    if (setup->assigned) {
        /* User report 18.10: with a cluster's assigned programme picked in the
         * header's "Program akışı", ▶ ran that programme as a separate cluster
         * flow and the broadcast event list never followed the transport. */
        wait_until(scope_workspace_ready, w->ctx->players, "player workspace ready for the cluster assignment");
        JsonNode *assigned = weekly_pcm_configuration(f->pa, NULL, start, 3, "Başlıkta atanmış program");
        f->assigned = document_save("weekly_programme", "Başlıkta atanmış program", json_node_get_object(assigned)); json_node_free(assigned);
        RadioSPlayerTopology topology = *radios_player_workspace_topology(w->ctx->players); GError *error = NULL;
        guint member = G_MAXUINT;
        for (guint deck = RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; deck-- > 0;) if (topology.players[deck].enabled) { member = deck; break; }
        g_assert_cmpuint(member, <, RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS);
        require_call(radios_player_topology_add_cluster(&topology, "Başlık kümesi", f->cluster, &error), &error);
        require_call(radios_player_topology_set_cluster(&topology, member, f->cluster, &error), &error);
        require_call(radios_player_topology_set_cluster_automation(&topology, f->cluster, radios_json_string(json_node_get_object(f->assigned), "_id", ""), &error), &error);
        require_call(radios_player_workspace_replace(w->ctx->players, &topology, &error), &error);
        LiveRowHeaderChoice choice = {find(w->root, "header-programme"), radios_json_string(json_node_get_object(f->assigned), "_id", "")};
        g_assert_true(GTK_IS_COMBO_BOX(choice.combo));
        wait_until(live_row_header_choice, &choice, "the header's Program akışı lists and selects the cluster's assigned programme");
    }
    click(w->root, "header-play");
    RadioSPlayoutSnapshot playing = general_pcm_wait(w, LIVE_ROW_A, 200, 20);
    f->a_started_utc_us = g_get_real_time() - (gint64)(playing.position_ms - MIN(playing.position_ms, playing.cue_in_ms)) * 1000;
    g_assert_true(weekly_snapshot(f->driver).armed);
    live_row_plan_read(w, &f->plan, setup->label, setup->end_rule, setup->duration_ms, f->schema_id);
    g_test_message("A observed on air %.3f s after its accepted planned start", (f->a_started_utc_us - f->plan.a_planned_utc_us) / 1e6);
}
static void live_row_close(LiveRow *f)
{
    Window *w = &f->window;
    click(w->root, "header-stop"); run_for(400);
    g_assert_false(weekly_snapshot(f->driver).armed); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    LiveRowMic mic = live_row_mic(w); g_assert_false(mic.gate); g_assert_false(mic.latch); g_assert_false(mic.ptt);
    calendar_schedule_unchanged(f->saved); assert_saved_playlist_unchanged(f->pa); assert_saved_playlist_unchanged(f->pc);
    if (f->ps) assert_saved_playlist_unchanged(f->ps);
    g_free(f->plan.a); g_free(f->plan.c); g_free(f->plan.block); g_free(f->plan.a_block); g_free(f->plan.c_block); g_free(f->schema_id);
    g_clear_pointer(&f->assigned, json_node_free);
    json_node_free(f->a); json_node_free(f->c); json_node_free(f->pa); json_node_free(f->pc); json_node_free(f->saved);
    g_clear_pointer(&f->a2, json_node_free);
    g_clear_pointer(&f->s, json_node_free); g_clear_pointer(&f->ps, json_node_free);
    window_close(w);
}

/* ---- 1. manual end rule: L during A, R in the wait, L off continues to C */
static void test_live_row_talk_manual_pcm(void)
{
    const LiveRowSetup setup = {"talk", "manual", 0, 4000, 3000, FALSE, FALSE, FALSE};
    LiveRow f; live_row_open(&f, &setup); Window *w = &f.window;
    /* (a) L while A plays: microphone latch and strips 3/4 on air, A uncut. */
    LiveRowButtons row = live_row_get(w, f.plan.block);
    g_test_message("While A plays, R of the live row is %s", gtk_widget_get_sensitive(row.talk) ? "pressable" : "passive");
    goffset around = capture_offset("MAIN"); run_for(300);
    gdouble level_before = tone_energy("MAIN", around, LIVE_ROW_A_HZ); guint64 quiet_before = 0;
    g_assert_cmpuint(capture_signal_span("MAIN", around, &quiet_before), >, 0); /* control: the 300 ms before L */
    RadioSPlayoutSnapshot before = live_row_flow(w); guint64 epoch = live_row_epoch(w);
    g_assert_cmpint(before.state, ==, RADIOS_PLAYOUT_PLAYING); g_assert_cmpstr(before.entry_id, ==, f.plan.a);
    g_assert_cmpuint(before.position_ms + 1200, <, before.duration_ms);
    gtk_button_clicked(GTK_BUTTON(row.live));
    LiveRowMic mic = live_row_mic(w);
    g_assert_true(radios_audio_ui_microphone_active(w->ctx)); g_assert_true(mic.latch); g_assert_false(mic.ptt);
    g_assert_true(radios_audio_ui_channel_on_air(w->ctx, 3)); g_assert_true(radios_audio_ui_channel_on_air(w->ctx, 4));
    /* A's level is measured once any transient of the press is over; the
     * longest MAIN silence around the press is checked at the end. */
    run_for(800);
    goffset pcm = capture_offset("MAIN"); run_for(300);
    RadioSPlayoutSnapshot after = live_row_flow(w);
    g_assert_cmpint(after.state, ==, RADIOS_PLAYOUT_PLAYING); g_assert_cmpstr(after.entry_id, ==, f.plan.a);
    g_assert_cmpuint(after.index, ==, before.index); g_assert_cmpuint(after.position_ms, >, before.position_ms);
    g_assert_cmpuint(live_row_epoch(w), ==, epoch);
    gdouble level_after = tone_energy("MAIN", pcm, LIVE_ROW_A_HZ); guint64 dropout = 0;
    g_assert_cmpuint(capture_signal_span("MAIN", around, &dropout), >, 0);
    g_test_message("A at MAIN: %.4f before L, %.4f after L; longest MAIN silence %.1f ms in the 300 ms before L, %.1f ms around the L press",
        level_before, level_after, quiet_before / 48.0, dropout / 48.0);
    g_assert_cmpfloat(level_before, >, .07);
    g_assert_cmpfloat(level_after, >, .07); g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_C_HZ), <, .01);
    row = live_row_get(w, f.plan.block);
    g_test_message("After L: L lit %d, R pressable %d", live_row_has_class(row.live, "is-pressed"), gtk_widget_get_sensitive(row.talk));
    /* (b) A ends: the executor holds at the manual step, C does not start. */
    live_row_flow_wait(w, RADIOS_PLAYOUT_MANUAL_WAIT, f.plan.a, 0, 10, "A finished, the manual live step holds the flow");
    run_for(300); /* A's last output buffers drain before the silent window */
    pcm = capture_offset("MAIN"); run_for(1200);
    RadioSPlayoutSnapshot held = live_row_flow(w);
    g_assert_cmpint(held.state, ==, RADIOS_PLAYOUT_MANUAL_WAIT); g_assert_cmpstr(held.entry_id, ==, f.plan.a); g_assert_cmpuint(held.index, ==, before.index);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_C_HZ), <, .01); g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_A_HZ), <, .01);
    g_assert_true(live_row_mic(w).latch);
    LiveRowWanted wanted = {w, f.plan.block};
    wait_until(live_row_talk_enabled, &wanted, "R of the on-air live step becomes pressable during the manual wait");
    row = live_row_get(w, f.plan.block);
    live_row_talk_event(row.talk, GDK_BUTTON_PRESS);
    mic = live_row_mic(w); g_assert_true(mic.ptt); g_assert_true(mic.gate); g_assert_true(mic.latch);
    g_assert_true(live_row_has_class(row.talk, "is-held"));
    run_for(400); g_assert_true(live_row_mic(w).ptt);
    g_assert_cmpint(live_row_flow(w).state, ==, RADIOS_PLAYOUT_MANUAL_WAIT);
    live_row_talk_event(row.talk, GDK_BUTTON_RELEASE);
    mic = live_row_mic(w); g_assert_false(mic.ptt); g_assert_true(mic.latch); g_assert_true(mic.gate);
    pcm = capture_offset("MAIN"); run_for(1000);
    held = live_row_flow(w);
    g_assert_cmpint(held.state, ==, RADIOS_PLAYOUT_MANUAL_WAIT); g_assert_cmpstr(held.entry_id, ==, f.plan.a); g_assert_cmpuint(held.index, ==, before.index);
    g_assert_cmpuint(live_row_epoch(w), ==, epoch); g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_C_HZ), <, .01);
    g_assert_true(radios_audio_ui_channel_on_air(w->ctx, 3)); g_assert_true(radios_audio_ui_channel_on_air(w->ctx, 4));
    /* (c) L off at the step completes it: latch and strips back, C plays. */
    row = live_row_get(w, f.plan.block); gtk_button_clicked(GTK_BUTTON(row.live));
    mic = live_row_mic(w); g_assert_false(mic.latch); g_assert_false(mic.gate); g_assert_false(radios_audio_ui_microphone_active(w->ctx));
    g_assert_false(radios_audio_ui_channel_on_air(w->ctx, 3)); g_assert_false(radios_audio_ui_channel_on_air(w->ctx, 4));
    RadioSPlayoutSnapshot following = live_row_flow_wait(w, RADIOS_PLAYOUT_PLAYING, f.plan.c, 200, 10, "L off completes the manual step and the outer order continues with C");
    g_assert_cmpstr(following.title, ==, LIVE_ROW_C); g_assert_cmpuint(following.index, ==, before.index + 1);
    pcm = capture_offset("MAIN"); run_for(300);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_C_HZ), >, .07); g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_A_HZ), <, .01);
    /* PDF s.8: L is used while the track plays and never cuts it. A sine has
     * no silence of its own; more than one PipeWire quantum is a MAIN hole. */
    if (dropout / 48.0 > 25)
        g_error("L pressed while A played left a %.1f ms silence in A at MAIN (%.1f ms in the 300 ms before L; A %.4f before, %.4f after); every other check of this case passed",
            dropout / 48.0, quiet_before / 48.0, level_before, level_after);
    live_row_close(&f);
}

/* ---- 2. duration end rule: C waits for the bound; L/R do not advance it - */
static gboolean live_row_c_started(Window *window, const gchar *c)
{ RadioSPlayoutSnapshot flow = live_row_flow(window); return flow.state == RADIOS_PLAYOUT_PLAYING && g_str_equal(flow.entry_id, c); }
static void test_live_row_talk_duration_pcm(void)
{
    const gint64 duration_ms = 2500;
    const LiveRowSetup setup = {"talk", "duration", duration_ms, 3000, 3000, FALSE, FALSE, FALSE};
    LiveRow f; live_row_open(&f, &setup); Window *w = &f.window;
    /* The accepted timeline places C after A plus the step's wait; the wait
     * itself runs from A's actual end (playout_ui.c live_wait_poll). */
    gint64 bound = f.plan.c_planned_utc_us;
    g_assert_cmpint(bound, >, f.plan.a_planned_utc_us + 3000 * 1000);
    goffset span = capture_offset("MAIN");
    /* Observe the actual end of A, then use L and R inside the wait. */
    RadioSPlayoutSnapshot flow = live_row_flow(w); gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (flow.state == RADIOS_PLAYOUT_PLAYING && g_str_equal(flow.entry_id, f.plan.a) && g_get_monotonic_time() < deadline) { pump(); flow = live_row_flow(w); }
    gint64 a_end = g_get_real_time();
    g_assert_false(flow.state == RADIOS_PLAYOUT_PLAYING && g_str_equal(flow.entry_id, f.plan.a));
    RadioSPlayoutState wait_state = flow.state;
    g_test_message("A ended %.3f s before C's accepted start; flow state %d index %u entry %s",
        (bound - a_end) / 1e6, flow.state, flow.index, flow.entry_id);
    /* L on, R press/release and L off, each only while C has not started. */
    LiveRowWanted wanted = {w, f.plan.block}; gboolean talk_available = FALSE, talk_on_air = FALSE; guint controls = 0;
    deadline = g_get_monotonic_time() + 400 * G_TIME_SPAN_MILLISECOND;
    while (!live_row_c_started(w, f.plan.c) && !(talk_available = live_row_talk_enabled(&wanted)) && g_get_monotonic_time() < deadline) pump();
    if (!live_row_c_started(w, f.plan.c)) {
        wait_state = live_row_flow(w).state;
        LiveRowButtons row = live_row_get(w, f.plan.block); gtk_button_clicked(GTK_BUTTON(row.live)); ++controls;
        LiveRowMic mic = live_row_mic(w); g_assert_true(mic.latch); g_assert_true(radios_audio_ui_microphone_active(w->ctx));
        g_assert_true(radios_audio_ui_channel_on_air(w->ctx, 3)); g_assert_true(radios_audio_ui_channel_on_air(w->ctx, 4));
    }
    if (controls == 1 && !live_row_c_started(w, f.plan.c)) {
        LiveRowButtons row = live_row_get(w, f.plan.block); live_row_talk_event(row.talk, GDK_BUTTON_PRESS);
        talk_on_air = live_row_mic(w).ptt; run_for(150);
        live_row_talk_event(row.talk, GDK_BUTTON_RELEASE); g_assert_false(live_row_mic(w).ptt); ++controls;
    }
    if (controls == 2 && !live_row_c_started(w, f.plan.c)) {
        LiveRowButtons row = live_row_get(w, f.plan.block); gtk_button_clicked(GTK_BUTTON(row.live)); ++controls;
        LiveRowMic mic = live_row_mic(w); g_assert_false(mic.latch); g_assert_false(mic.gate);
        g_assert_false(radios_audio_ui_channel_on_air(w->ctx, 3)); g_assert_false(radios_audio_ui_channel_on_air(w->ctx, 4));
    }
    gint64 controls_done = g_get_real_time();
    g_test_message("Wait state %d; R pressable %d, R press on air %d; %u of 3 controls (L on, R, L off) done %.3f s after A",
        wait_state, talk_available, talk_on_air, controls, (controls_done - a_end) / 1e6);
    /* The runtime bound the broadcast view reads: A's actual end + duration. */
    gint64 until = live_row_flow(w).live_wait_until_utc_us;
    if (until) {
        g_test_message("Runtime wait until %.3f s after A's observed end", (until - a_end) / 1e6);
        g_assert_cmpint(until, >=, a_end + duration_ms * 1000 - 400000); g_assert_cmpint(until, <=, a_end + duration_ms * 1000 + 400000);
        bound = until;
    }
    /* C starts at the step's bound, not when L closes or R is released. */
    RadioSPlayoutSnapshot following = live_row_flow_wait(w, RADIOS_PLAYOUT_PLAYING, f.plan.c, 0, 8, "C starts after the live step duration");
    gint64 c_start = g_get_real_time();
    g_assert_cmpstr(following.title, ==, LIVE_ROW_C);
    goffset pcm = capture_offset("MAIN"); run_for(400);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_C_HZ), >, .07); g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_A_HZ), <, .01);
    guint64 gap = 0; g_assert_cmpuint(capture_signal_span("MAIN", span, &gap), >, 0);
    gdouble silence = gap / 48000.0, lag = (f.a_started_utc_us - f.plan.a_planned_utc_us) / 1e6;
    g_test_message("MAIN silence between A and C %.3f s (rule %" G_GINT64_FORMAT " ms); C observed %.3f s after A, %.3f s after its accepted start",
        silence, duration_ms, (c_start - a_end) / 1e6, (c_start - bound) / 1e6);
    if (silence < duration_ms / 1000.0 - .5 || silence > duration_ms / 1000.0 + .5)
        g_error("The live step's duration wait between A and C was %.3f s, not %.3f ± 0.5 s; A went on air %.3f s after its accepted planned start",
            silence, duration_ms / 1000.0, lag);
    g_assert_cmpint(c_start, >=, bound - 150000);
    g_assert_cmpuint(controls, ==, 3);
    /* PDF s.15: R is pressable for the live step on air, i.e. in its wait. */
    if (!talk_available || !talk_on_air)
        g_error("R of the on-air live step during its duration wait: pressable %d, press opened push-to-talk %d (flow state %d)",
            talk_available, talk_on_air, wait_state);
    live_row_close(&f);
}

/* R of an event row of the broadcast queue (NULL when the row is absent). */
static GtkWidget *live_row_block_talk(Window *window, const gchar *block)
{
    GtkWidget *queue = find(window->root, "programme-queue-table"); if (!queue) return NULL;
    GtkListBox *list = g_object_get_data(G_OBJECT(queue), "list-box"); if (!list) return NULL;
    for (gint index = 0;; ++index) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, index); if (!row) return NULL;
        if (radios_programme_queue_row_level(row) == RADIOS_PROGRAMME_ROW_MAIN && !g_strcmp0(radios_programme_queue_row_id(row), block))
            return find(GTK_WIDGET(row), "programme-row-talk");
    }
}
/* ---- 3. R release never skips: off air R is passive; a hook release too - */
static void test_live_row_release_never_skips_pcm(void)
{
    const LiveRowSetup setup = {"talk", "manual", 0, 4000, 3000, FALSE, FALSE, FALSE};
    LiveRow f; live_row_open(&f, &setup); Window *w = &f.window;
    LiveRowButtons row = live_row_get(w, f.plan.block); run_for(300); row = live_row_get(w, f.plan.block);
    RadioSPlayoutSnapshot before = live_row_flow(w); guint64 epoch = live_row_epoch(w);
    g_assert_cmpint(before.state, ==, RADIOS_PLAYOUT_PLAYING); g_assert_cmpstr(before.entry_id, ==, f.plan.a);
    g_assert_cmpuint(before.position_ms + 1500, <, before.duration_ms);
    g_assert_false(gtk_widget_get_sensitive(row.talk));
    g_assert_true(gtk_widget_get_sensitive(row.live));
    /* A pointer press/release on the passive R reaches its handlers and does nothing. */
    live_row_talk_event(row.talk, GDK_BUTTON_PRESS);
    LiveRowMic mic = live_row_mic(w); g_assert_false(mic.ptt); g_assert_false(mic.gate);
    g_assert_false(live_row_has_class(row.talk, "is-held"));
    live_row_talk_event(row.talk, GDK_BUTTON_RELEASE);
    /* The row hooks directly: release, a refused press, release again. */
    RadioSProgrammeQueueHooks *hooks = live_row_hooks(w);
    hooks->action(hooks->data, f.plan.block, RADIOS_PROGRAMME_ROW_TALK_RELEASE);
    hooks->action(hooks->data, f.plan.block, RADIOS_PROGRAMME_ROW_TALK_PRESS);
    mic = live_row_mic(w); g_assert_false(mic.ptt); g_assert_false(mic.gate);
    hooks->action(hooks->data, f.plan.block, RADIOS_PROGRAMME_ROW_TALK_RELEASE);
    goffset pcm = capture_offset("MAIN"); run_for(300);
    RadioSPlayoutSnapshot after = live_row_flow(w);
    g_assert_cmpint(after.state, ==, RADIOS_PLAYOUT_PLAYING); g_assert_cmpstr(after.entry_id, ==, f.plan.a);
    g_assert_cmpuint(after.index, ==, before.index); g_assert_cmpuint(after.position_ms, >, before.position_ms);
    g_assert_cmpuint(live_row_epoch(w), ==, epoch);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_A_HZ), >, .07); g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_C_HZ), <, .01);
    row = live_row_get(w, f.plan.block); g_assert_false(gtk_widget_get_sensitive(row.talk));
    /* PDF s.15: the event on air is A's music event — its own R is pressable
     * and talks on the studio microphone; releasing it never skips A. */
    GtkWidget *a_talk = live_row_block_talk(w, f.plan.a_block); g_assert_true(GTK_IS_BUTTON(a_talk));
    g_assert_true(gtk_widget_get_sensitive(a_talk));
    live_row_talk_event(a_talk, GDK_BUTTON_PRESS); g_assert_true(live_row_mic(w).ptt);
    pcm = capture_offset("MAIN"); run_for(300);
    a_talk = live_row_block_talk(w, f.plan.a_block); live_row_talk_event(a_talk, GDK_BUTTON_RELEASE); g_assert_false(live_row_mic(w).ptt);
    after = live_row_flow(w);
    g_assert_cmpint(after.state, ==, RADIOS_PLAYOUT_PLAYING); g_assert_cmpstr(after.entry_id, ==, f.plan.a); g_assert_cmpuint(live_row_epoch(w), ==, epoch);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_A_HZ), >, .07);
    /* At the manual step a release without a press still never advances. */
    live_row_flow_wait(w, RADIOS_PLAYOUT_MANUAL_WAIT, f.plan.a, 0, 10, "A finished, the manual live step holds the flow");
    hooks = live_row_hooks(w);
    hooks->action(hooks->data, f.plan.block, RADIOS_PROGRAMME_ROW_TALK_RELEASE);
    run_for(300); /* A's last output buffers drain before the silent window */
    pcm = capture_offset("MAIN"); run_for(1200);
    RadioSPlayoutSnapshot held = live_row_flow(w);
    g_assert_cmpint(held.state, ==, RADIOS_PLAYOUT_MANUAL_WAIT); g_assert_cmpstr(held.entry_id, ==, f.plan.a); g_assert_cmpuint(held.index, ==, before.index);
    g_assert_cmpuint(live_row_epoch(w), ==, epoch);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_C_HZ), <, .01); g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_A_HZ), <, .01);
    mic = live_row_mic(w); g_assert_false(mic.ptt); g_assert_false(mic.gate);
    live_row_close(&f);
}

static gboolean live_row_run_over(gpointer value);
/* ---- 4. music label: L starts the step's own schema on a free player ---- */
typedef struct { gboolean found, completed; gint64 source, state, position_ms; gchar entry[37]; } LiveRowEventRun;
static LiveRowEventRun live_row_event_run(Window *window, const gchar *block)
{
    LiveRowEventRun result = {FALSE, FALSE, -1, -1, 0, ""};
    JsonNode *snapshot = radios_event_playout_display_snapshot(window->ctx);
    JsonNode *member = snapshot && JSON_NODE_HOLDS_OBJECT(snapshot) ? json_object_get_member(json_node_get_object(snapshot), "runs") : NULL;
    JsonArray *runs = member && JSON_NODE_HOLDS_ARRAY(member) ? json_node_get_array(member) : NULL;
    for (guint i = 0; runs && i < json_array_get_length(runs) && !result.found; ++i) {
        JsonObject *run = json_array_get_object_element(runs, i);
        if (g_strcmp0(radios_json_string(run, "origin_block_id", ""), block)) continue;
        JsonNode *completed = json_object_get_member(run, "completed");
        result.found = TRUE; result.source = radios_json_int(run, "source", -1); result.state = radios_json_int(run, "state", -1);
        result.position_ms = radios_json_int(run, "position_ms", 0);
        result.completed = completed && JSON_NODE_HOLDS_VALUE(completed) && json_node_get_boolean(completed);
        g_strlcpy(result.entry, radios_json_string(run, "entry_id", ""), sizeof result.entry);
    }
    if (snapshot) json_node_free(snapshot);
    return result;
}
/* The live row's runtime badge (programme_queue.c event_runs_apply). */
static gboolean live_row_badge(Window *window, const gchar *block, gchar **text)
{
    LiveRowButtons buttons; if (!live_row_find(window, block, &buttons)) return FALSE;
    GtkWidget *runtime = find(GTK_WIDGET(buttons.row), "programme-row-runtime");
    if (!GTK_IS_LABEL(runtime)) return FALSE;
    const gchar *value = gtk_label_get_text(GTK_LABEL(runtime));
    if (text) { g_free(*text); *text = g_strdup(value); }
    return gtk_widget_get_visible(runtime) && live_row_has_class(GTK_WIDGET(buttons.row), "is-event-running") &&
        g_str_has_prefix(value, "Yarı AutoDJ · ") && strstr(value, "Çalıyor") != NULL;
}
static void test_live_row_music_schema_pcm(void)
{
    const LiveRowSetup setup = {"music", "duration", 4000, 3000, 3000, TRUE, FALSE, FALSE};
    LiveRow f; live_row_open(&f, &setup); Window *w = &f.window;
    g_assert_true(g_uuid_string_is_valid(f.schema_id));
    gint64 bound = f.plan.c_planned_utc_us;
    wait_until(scope_workspace_ready, w->ctx->players, "player workspace ready for the event's own run");
    /* A ends; the flow waits at the music live step until its bound. */
    RadioSPlayoutSnapshot flow = live_row_flow(w); gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (flow.state == RADIOS_PLAYOUT_PLAYING && g_str_equal(flow.entry_id, f.plan.a) && g_get_monotonic_time() < deadline) { pump(); flow = live_row_flow(w); }
    g_assert_false(flow.state == RADIOS_PLAYOUT_PLAYING && g_str_equal(flow.entry_id, f.plan.a));
    g_assert_false(live_row_c_started(w, f.plan.c));
    RadioSPlayoutSnapshot waiting = flow; guint64 epoch = live_row_epoch(w); gint64 a_end = g_get_real_time();
    g_test_message("A ended %.3f s before C's accepted start; flow state %d index %u", (bound - a_end) / 1e6, waiting.state, waiting.index);
    LiveRowButtons row = live_row_get(w, f.plan.block);
    /* PDF s.15: R works on the event on air; a music step talks on the
     * studio microphone and its release never moves the flow. */
    LiveRowWanted talk_wanted = {w, f.plan.block};
    wait_until(live_row_talk_enabled, &talk_wanted, "R of the music step on air is pressable");
    row = live_row_get(w, f.plan.block); live_row_talk_event(row.talk, GDK_BUTTON_PRESS);
    g_assert_true(live_row_mic(w).ptt); run_for(150);
    live_row_talk_event(row.talk, GDK_BUTTON_RELEASE); g_assert_false(live_row_mic(w).ptt);
    g_assert_cmpint(live_row_flow(w).state, ==, waiting.state); g_assert_cmpuint(live_row_flow(w).index, ==, waiting.index);
    g_assert_false(live_row_event_run(w, f.plan.block).found);
    gtk_button_clicked(GTK_BUTTON(row.live)); gint64 pressed = g_get_real_time();
    /* L produces no transport command and opens no microphone. */
    RadioSPlayoutSnapshot after = live_row_flow(w);
    g_assert_cmpint(after.state, ==, waiting.state); g_assert_cmpstr(after.entry_id, ==, waiting.entry_id);
    g_assert_cmpuint(after.index, ==, waiting.index); g_assert_cmpuint(live_row_epoch(w), ==, epoch);
    LiveRowMic mic = live_row_mic(w); g_assert_false(mic.gate); g_assert_false(mic.latch);
    /* Within a few seconds: the row's Yarı AutoDJ badge and the schema's tone. */
    gchar *badge = NULL; gint64 c_start = 0; gboolean shown = FALSE;
    deadline = g_get_monotonic_time() + 8 * G_USEC_PER_SEC;
    while (!(shown = live_row_badge(w, f.plan.block, &badge)) && g_get_monotonic_time() < deadline) {
        pump(); if (!c_start && live_row_c_started(w, f.plan.c)) c_start = g_get_real_time();
    }
    gint64 shown_at = g_get_real_time(); LiveRowEventRun run = live_row_event_run(w, f.plan.block);
    if (!shown) {
        LiveRowButtons probe; gboolean present = live_row_find(w, f.plan.block, &probe);
        g_error("No Yarı AutoDJ badge on the live row %.3f s after L (badge '%s', row listed %d, event run found %d state %" G_GINT64_FORMAT
            " player %" G_GINT64_FORMAT ", C started %.3f s after L)", (shown_at - pressed) / 1e6, badge ? badge : "", present,
            run.found, run.state, run.source + 1, c_start ? (c_start - pressed) / 1e6 : -1.0);
    }
    g_test_message("L → Yarı AutoDJ badge '%s' after %.3f s (C's accepted start was %.3f s after L)", badge, (shown_at - pressed) / 1e6, (bound - pressed) / 1e6);
    /* The badge follows the display snapshot; the run itself settles to PLAYING. */
    for (gint64 settle = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; run.state != RADIOS_PLAYOUT_PLAYING && g_get_monotonic_time() < settle;) {
        pump(); run = live_row_event_run(w, f.plan.block);
    }
    g_assert_true(run.found); g_assert_false(run.completed); g_assert_cmpint(run.state, ==, RADIOS_PLAYOUT_PLAYING);
    g_assert_cmpint(run.source, >=, 0); g_assert_cmpint(run.source, <, 32);
    RadioSAudioRunSnapshot general = {0}; g_assert_true(radios_playout_ui_navigation_snapshot(w->ctx, &general));
    g_assert_false(general.owned[run.source]); /* the event's own player, not one of the flow */
    RadioSAudioUiSnapshot deck = {0}; g_assert_true(radios_audio_ui_get_snapshot(w->ctx, (guint)run.source, &deck));
    g_assert_cmpint(deck.state, ==, RADIOS_AUDIO_UI_PLAYING);
    g_assert_cmpstr(deck.media_id, ==, radios_json_string(json_object_get_object_member(json_node_get_object(f.s), "media"), "_id", ""));
    goffset pcm = capture_offset("MAIN"); run_for(400);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_S_HZ), >, .07);
    /* The flow is untouched by L: still at the step, or on with C at its own bound. */
    if (!c_start && live_row_c_started(w, f.plan.c)) c_start = g_get_real_time();
    flow = live_row_flow(w);
    g_assert_true((flow.state == waiting.state && g_str_equal(flow.entry_id, f.plan.a) && flow.index == waiting.index) ||
        (flow.state == RADIOS_PLAYOUT_PLAYING && g_str_equal(flow.entry_id, f.plan.c) && flow.index == waiting.index + 1));
    if (c_start) g_assert_cmpint(c_start, >=, bound - 150000);
    /* A second L press never cuts the event's run: same player, same media. */
    row = live_row_get(w, f.plan.block); gtk_button_clicked(GTK_BUTTON(row.live));
    pcm = capture_offset("MAIN"); run_for(600);
    LiveRowEventRun again = live_row_event_run(w, f.plan.block);
    g_assert_true(again.found); g_assert_cmpint(again.source, ==, run.source); g_assert_cmpint(again.state, ==, RADIOS_PLAYOUT_PLAYING);
    RadioSAudioUiSnapshot still = {0}; g_assert_true(radios_audio_ui_get_snapshot(w->ctx, (guint)run.source, &still));
    g_assert_cmpint(still.state, ==, RADIOS_AUDIO_UI_PLAYING); g_assert_cmpuint(still.generation, ==, deck.generation);
    g_assert_cmpuint(still.position_ms, >, deck.position_ms);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_S_HZ), >, .07);
    g_assert_true(live_row_badge(w, f.plan.block, &badge) || live_row_c_started(w, f.plan.c));
    /* PDF s.6: the step is complete when the outer order continues with C;
     * the step's own schema ends with it — C never shares MAIN with it. */
    RadioSPlayoutSnapshot following = live_row_flow_wait(w, RADIOS_PLAYOUT_PLAYING, f.plan.c, 200, 8, "C starts at the music step's bound");
    if (!c_start) c_start = g_get_real_time() - (gint64)following.position_ms * 1000;
    g_test_message("C observed %.3f s after its accepted start", (c_start - bound) / 1e6);
    g_assert_cmpint(c_start, >=, bound - 150000);
    LiveRowWanted ended = {w, f.plan.block};
    wait_until(live_row_run_over, &ended, "the music step's own schema ends when the outer order continues with C");
    RadioSAudioUiSnapshot stopped = {0}; g_assert_true(radios_audio_ui_get_snapshot(w->ctx, (guint)run.source, &stopped));
    g_assert_cmpint(stopped.state, !=, RADIOS_AUDIO_UI_PLAYING);
    /* Every deck that sounded the schema's track is silent again. */
    const gchar *s_media = radios_json_string(json_object_get_object_member(json_node_get_object(f.s), "media"), "_id", "");
    gint64 quiet_deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC; gint sounding_deck = -1;
    do {
        sounding_deck = -1;
        for (guint deck = 0; deck < 32 && sounding_deck < 0; ++deck) {
            RadioSAudioUiSnapshot any = {0};
            if (radios_audio_ui_get_snapshot(w->ctx, deck, &any) && any.state == RADIOS_AUDIO_UI_PLAYING && g_str_equal(any.media_id, s_media)) sounding_deck = (gint)deck;
        }
        if (sounding_deck >= 0) pump();
    } while (sounding_deck >= 0 && g_get_monotonic_time() < quiet_deadline);
    if (sounding_deck >= 0) g_error("The schema track still plays on player %d after the step ended", sounding_deck + 1);
    run_for(300); pcm = capture_offset("MAIN"); run_for(300);
    if (tone_energy("MAIN", pcm, LIVE_ROW_S_HZ) >= .01) {
        for (guint deck = 0; deck < 32; ++deck) {
            RadioSAudioUiSnapshot any = {0};
            if (radios_audio_ui_get_snapshot(w->ctx, deck, &any) && any.media_id[0])
                g_test_message("deck %u state %d media %s position %" G_GUINT64_FORMAT " (S %d)", deck + 1, any.state, any.media_id, any.position_ms, g_str_equal(any.media_id, s_media));
        }
        RadioSPlayoutSnapshot flow_now = live_row_flow(w); LiveRowEventRun run_now = live_row_event_run(w, f.plan.block);
        g_test_message("flow state %d entry %s title %s; event run found %d state %" G_GINT64_FORMAT " source %" G_GINT64_FORMAT,
            flow_now.state, flow_now.entry_id, flow_now.title, run_now.found, run_now.state, run_now.source + 1);
    }
    g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_C_HZ), >, .07); g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_S_HZ), <, .01);
    g_free(badge);
    live_row_close(&f);
}

/* ---- 5. talk label with a schema: L opens the talk channel and the special
 * live event schema (PDF s.8 "Konuşma"); L off at the manual step ends both
 * and the outer order continues with C. */
static gboolean live_row_run_over(gpointer value)
{
    LiveRowWanted *wanted = value; LiveRowEventRun run = live_row_event_run(wanted->window, wanted->block);
    return !run.found || run.completed || run.state != RADIOS_PLAYOUT_PLAYING;
}
static void test_live_row_talk_schema_pcm(void)
{
    const LiveRowSetup setup = {"talk", "manual", 0, 3000, 3000, TRUE, FALSE, FALSE};
    LiveRow f; live_row_open(&f, &setup); Window *w = &f.window;
    g_assert_true(g_uuid_string_is_valid(f.schema_id));
    wait_until(scope_workspace_ready, w->ctx->players, "player workspace ready for the event's own run");
    RadioSPlayoutSnapshot waiting = live_row_flow_wait(w, RADIOS_PLAYOUT_MANUAL_WAIT, f.plan.a, 0, 10, "A finished, the manual talk step holds the flow");
    guint64 epoch = live_row_epoch(w);
    LiveRowButtons row = live_row_get(w, f.plan.block);
    g_assert_false(live_row_event_run(w, f.plan.block).found);
    gtk_button_clicked(GTK_BUTTON(row.live)); gint64 pressed = g_get_real_time();
    LiveRowMic mic = live_row_mic(w); g_assert_true(mic.latch); g_assert_true(radios_audio_ui_microphone_active(w->ctx));
    g_assert_true(radios_audio_ui_channel_on_air(w->ctx, 3)); g_assert_true(radios_audio_ui_channel_on_air(w->ctx, 4));
    RadioSPlayoutSnapshot after = live_row_flow(w);
    g_assert_cmpint(after.state, ==, RADIOS_PLAYOUT_MANUAL_WAIT); g_assert_cmpstr(after.entry_id, ==, f.plan.a);
    g_assert_cmpuint(after.index, ==, waiting.index); g_assert_cmpuint(live_row_epoch(w), ==, epoch);
    gchar *badge = NULL; gboolean shown = FALSE; gint64 deadline = g_get_monotonic_time() + 8 * G_USEC_PER_SEC;
    while (!(shown = live_row_badge(w, f.plan.block, &badge)) && g_get_monotonic_time() < deadline) pump();
    LiveRowEventRun run = live_row_event_run(w, f.plan.block);
    if (!shown)
        g_error("No Yarı AutoDJ badge on the talk row %.3f s after L (badge '%s', event run found %d state %" G_GINT64_FORMAT ")",
            (g_get_real_time() - pressed) / 1e6, badge ? badge : "", run.found, run.state);
    g_assert_true(run.found); g_assert_false(run.completed); g_assert_cmpint(run.state, ==, RADIOS_PLAYOUT_PLAYING);
    g_assert_cmpint(run.source, >=, 0); g_assert_cmpint(run.source, <, 32);
    RadioSAudioRunSnapshot general = {0}; g_assert_true(radios_playout_ui_navigation_snapshot(w->ctx, &general));
    g_assert_false(general.owned[run.source]); /* the event's own player, not one of the flow */
    RadioSAudioUiSnapshot deck = {0}; g_assert_true(radios_audio_ui_get_snapshot(w->ctx, (guint)run.source, &deck));
    g_assert_cmpint(deck.state, ==, RADIOS_AUDIO_UI_PLAYING);
    g_assert_cmpstr(deck.media_id, ==, radios_json_string(json_object_get_object_member(json_node_get_object(f.s), "media"), "_id", ""));
    goffset pcm = capture_offset("MAIN"); run_for(400);
    g_test_message("Talk L → latch, strips 3/4 on air, badge '%s' after %.3f s on player %" G_GINT64_FORMAT "; schema tone at MAIN %.4f",
        badge, (g_get_real_time() - pressed) / 1e6, run.source + 1, tone_energy("MAIN", pcm, LIVE_ROW_S_HZ));
    after = live_row_flow(w);
    g_assert_cmpint(after.state, ==, RADIOS_PLAYOUT_MANUAL_WAIT); g_assert_cmpuint(after.index, ==, waiting.index);
    /* L off completes the manual step: latch and strips back, the step's own
     * schema ends with it and C follows. */
    row = live_row_get(w, f.plan.block); gtk_button_clicked(GTK_BUTTON(row.live));
    mic = live_row_mic(w); g_assert_false(mic.latch); g_assert_false(mic.gate);
    g_assert_false(radios_audio_ui_channel_on_air(w->ctx, 3)); g_assert_false(radios_audio_ui_channel_on_air(w->ctx, 4));
    RadioSPlayoutSnapshot following = live_row_flow_wait(w, RADIOS_PLAYOUT_PLAYING, f.plan.c, 200, 10, "L off completes the manual talk step and C follows");
    g_assert_cmpstr(following.title, ==, LIVE_ROW_C); g_assert_cmpuint(following.index, ==, waiting.index + 1);
    LiveRowWanted wanted = {w, f.plan.block};
    wait_until(live_row_run_over, &wanted, "the talk step's own schema run ends with the step");
    RadioSAudioUiSnapshot stopped = {0}; g_assert_true(radios_audio_ui_get_snapshot(w->ctx, (guint)run.source, &stopped));
    g_assert_cmpint(stopped.state, !=, RADIOS_AUDIO_UI_PLAYING);
    pcm = capture_offset("MAIN"); run_for(300);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_C_HZ), >, .07); g_assert_cmpfloat(tone_energy("MAIN", pcm, LIVE_ROW_S_HZ), <, .01);
    g_free(badge); live_row_close(&f);
}

/* ---- 6. header transport drives the broadcast event list ---------------- */
/* Playing state of an event row of the broadcast queue: 1 playing, 0 listed,
 * -1 not listed. */
static gint live_row_block_state(Window *window, const gchar *block)
{
    GtkWidget *queue = find(window->root, "programme-queue-table"); if (!queue) return -1;
    GtkListBox *list = g_object_get_data(G_OBJECT(queue), "list-box"); if (!list) return -1;
    for (gint index = 0;; ++index) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, index); if (!row) return -1;
        if (radios_programme_queue_row_level(row) == RADIOS_PROGRAMME_ROW_MAIN && !g_strcmp0(radios_programme_queue_row_id(row), block))
            return live_row_has_class(GTK_WIDGET(row), "is-playing") ? 1 : 0;
    }
}
typedef struct { Window *window; const gchar *block; gint state; } LiveRowBlockWanted;
static gboolean live_row_block_is(gpointer value)
{ LiveRowBlockWanted *wanted = value; return live_row_block_state(wanted->window, wanted->block) == wanted->state; }
static gboolean live_row_block_left(gpointer value)
{ LiveRowBlockWanted *wanted = value; return live_row_block_state(wanted->window, wanted->block) != 1; }
static void test_live_row_header_play_general_queue_pcm(void)
{
    const LiveRowSetup setup = {"talk", "manual", 0, 6000, 4000, FALSE, TRUE, FALSE};
    LiveRow f; live_row_open(&f, &setup); Window *w = &f.window;
    /* ▶ started the broadcast page's continuous general queue (A on MAIN,
     * driver armed), never the cluster's own flow. */
    g_assert_true(weekly_snapshot(f.driver).armed);
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER}; g_strlcpy(scope.cluster_id, f.cluster, sizeof scope.cluster_id);
    RadioSPlayerScopeExecutionSnapshot execution = {0};
    gboolean known = radios_player_scope_execution_snapshot(radios_player_scope_execution_get(w->ctx), &scope, &execution);
    g_test_message("Cluster flow after header Play: known %d schema_active %d state %d", known, execution.schema_active, execution.state);
    g_assert_true(!known || !execution.schema_active || execution.state == RADIOS_SCOPE_EXECUTION_IDLE);
    /* The event list shows A's event as the playing row. */
    LiveRowBlockWanted a_playing = {w, f.plan.a_block, 1};
    wait_until(live_row_block_is, &a_playing, "A's event row of the broadcast list is the playing row after header Play");
    /* ⏭ leaves A: its row is no longer the playing one, the flow moved on. */
    click(w->root, "header-next-track");
    LiveRowBlockWanted a_left = {w, f.plan.a_block, 1};
    wait_until(live_row_block_left, &a_left, "A's row stops being the playing row after header Next");
    RadioSPlayoutSnapshot moved = live_row_flow(w);
    g_test_message("After header Next: flow state %d entry %s; A row state %d, C row state %d", moved.state, moved.entry_id,
        live_row_block_state(w, f.plan.a_block), live_row_block_state(w, f.plan.c_block));
    g_assert_true(g_strcmp0(moved.entry_id, f.plan.a) || moved.state != RADIOS_PLAYOUT_PLAYING);
    if (moved.state == RADIOS_PLAYOUT_PLAYING && !g_strcmp0(moved.entry_id, f.plan.c)) {
        LiveRowBlockWanted c_playing = {w, f.plan.c_block, 1};
        wait_until(live_row_block_is, &c_playing, "C's event row is the playing row after header Next");
    }
    /* ■ stops the general queue: no row stays playing. */
    click(w->root, "header-stop"); run_for(400);
    g_assert_false(weekly_snapshot(f.driver).armed);
    g_assert_cmpint(live_row_block_state(w, f.plan.a_block), !=, 1); g_assert_cmpint(live_row_block_state(w, f.plan.c_block), !=, 1);
    /* ▶ again starts the same general queue, its list follows once more. */
    click(w->root, "header-play");
    gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
    while (!weekly_snapshot(f.driver).armed && g_get_monotonic_time() < deadline) pump();
    g_assert_true(weekly_snapshot(f.driver).armed);
    known = radios_player_scope_execution_snapshot(radios_player_scope_execution_get(w->ctx), &scope, &execution);
    g_assert_true(!known || !execution.schema_active || execution.state == RADIOS_SCOPE_EXECUTION_IDLE);
    live_row_close(&f);
}

/* ---- 7. the ending warning comes before the transition (PDF s.11) ------- */
static gboolean live_row_block_blinks(Window *window, const gchar *block, guint milliseconds)
{
    gint64 end = g_get_monotonic_time() + (gint64)milliseconds * 1000; gboolean seen = FALSE;
    while (g_get_monotonic_time() < end && !seen) {
        GtkWidget *queue = find(window->root, "programme-queue-table");
        GtkListBox *list = queue ? g_object_get_data(G_OBJECT(queue), "list-box") : NULL;
        for (gint index = 0; list; ++index) {
            GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, index); if (!row) break;
            if (radios_programme_queue_row_level(row) == RADIOS_PROGRAMME_ROW_MAIN && !g_strcmp0(radios_programme_queue_row_id(row), block))
                seen |= live_row_has_class(GTK_WIDGET(row), "is-ending");
        }
        pump();
    }
    return seen;
}
static void test_live_row_ending_before_transition_pcm(void)
{
    const LiveRowSetup setup = {"talk", "manual", 0, 14000, 3000, FALSE, FALSE, FALSE};
    LiveRow f; live_row_open(&f, &setup); Window *w = &f.window;
    /* Early in A (14 s): no warning, S on A's row. */
    RadioSPlayoutSnapshot flow = live_row_flow(w);
    g_assert_cmpint(flow.state, ==, RADIOS_PLAYOUT_PLAYING); g_assert_cmpuint(flow.position_ms, <, 2000);
    g_assert_false(live_row_block_blinks(w, f.plan.a_block, 1000));
    g_assert_cmpuint(live_row_flow(w).position_ms, <, 3800);
    g_assert_cmpint(live_row_block_state(w, f.plan.a_block), ==, 1);
    /* Inside the preparation region (last ten seconds before the boundary):
     * the row blinks while it still shows S, before any transition. */
    live_row_flow_wait(w, RADIOS_PLAYOUT_PLAYING, f.plan.a, 5000, 12, "A reaches its preparation region");
    g_assert_true(live_row_block_blinks(w, f.plan.a_block, 1500));
    flow = live_row_flow(w);
    g_test_message("Warning at A %" G_GUINT64_FORMAT " ms: state %d (no overlap yet), A row playing %d",
        flow.position_ms, flow.state, live_row_block_state(w, f.plan.a_block));
    g_assert_cmpint(flow.state, ==, RADIOS_PLAYOUT_PLAYING); g_assert_cmpint(live_row_block_state(w, f.plan.a_block), ==, 1);
    /* A ends at the manual step: the warning is cleared. */
    live_row_flow_wait(w, RADIOS_PLAYOUT_MANUAL_WAIT, f.plan.a, 0, 14, "A finished, the manual live step holds the flow");
    run_for(300); g_assert_false(live_row_block_blinks(w, f.plan.a_block, 1200));
    live_row_close(&f);
}

/* ---- 8. × on an unheard content of the running general flow (U7) ------- */
/* The n-th (0-based) plan row titled title: its entry and block ids. */
static gboolean live_row_entry_nth(Window *window, const gchar *title, guint n, gchar **entry, gchar **block)
{
    JsonNode *node = radios_playout_ui_plan_snapshot(window->ctx); gboolean found = FALSE;
    JsonArray *entries = node ? json_object_get_array_member(json_node_get_object(node), "entries") : NULL;
    for (guint i = 0, seen = 0; entries && i < json_array_get_length(entries) && !found; ++i) {
        JsonObject *row = json_array_get_object_element(entries, i);
        if (g_strcmp0(radios_json_string(row, "title", ""), title) || seen++ != n) continue;
        found = TRUE;
        if (entry) *entry = g_strdup(radios_json_string(row, "entry_id", ""));
        if (block) *block = g_strdup(radios_json_string(row, "block_id", ""));
    }
    if (node) json_node_free(node);
    return found;
}
static gboolean live_row_plan_has(Window *window, const gchar *entry_id)
{
    JsonNode *node = radios_playout_ui_plan_snapshot(window->ctx); gboolean found = FALSE;
    JsonArray *entries = node ? json_object_get_array_member(json_node_get_object(node), "entries") : NULL;
    for (guint i = 0; entries && i < json_array_get_length(entries) && !found; ++i)
        found = !g_strcmp0(radios_json_string(json_array_get_object_element(entries, i), "entry_id", ""), entry_id);
    if (node) json_node_free(node);
    return found;
}
/* The player of every plan row (one-based, as the queue shows it). */
static GArray *live_row_players(Window *window)
{
    JsonNode *node = radios_playout_ui_plan_snapshot(window->ctx); GArray *players = g_array_new(FALSE, FALSE, sizeof(gint64));
    JsonArray *entries = node ? json_object_get_array_member(json_node_get_object(node), "entries") : NULL;
    for (guint i = 0; entries && i < json_array_get_length(entries); ++i) {
        gint64 player = json_object_get_int_member(json_array_get_object_element(entries, i), "player");
        g_array_append_val(players, player);
    }
    if (node) json_node_free(node);
    return players;
}
static void test_live_row_remove_unheard_content_pcm(void)
{
    /* The schema takes one A track per round: A, step, C, A2, step, C, … */
    const LiveRowSetup setup = {"talk", "duration", 1500, 6000, 3000, FALSE, FALSE, TRUE};
    LiveRow f; live_row_open(&f, &setup); Window *w = &f.window;
    gchar *later = NULL, *later_block = NULL;
    g_assert_true(live_row_entry_nth(w, LIVE_ROW_C, 1, &later, &later_block));
    RadioSPlayoutSnapshot before = live_row_flow(w); guint64 epoch = live_row_epoch(w);
    g_assert_cmpint(before.state, ==, RADIOS_PLAYOUT_PLAYING); g_assert_cmpstr(before.entry_id, ==, f.plan.a);
    g_assert_cmpint(live_row_block_state(w, f.plan.c_block), >=, 0); /* C's event is listed */
    GArray *slots = live_row_players(w);
    RadioSProgrammeQueueHooks *hooks = live_row_hooks(w);
    /* × on the sounding A is refused: A keeps playing, its queue stays. */
    hooks->action(hooks->data, f.plan.a, RADIOS_PROGRAMME_ROW_REMOVE); pump();
    g_assert_true(live_row_plan_has(w, f.plan.c)); g_assert_cmpstr(live_row_flow(w).entry_id, ==, f.plan.a);
    /* × on a later unheard C: it leaves the executor's queue and the list. */
    goffset around = capture_offset("MAIN");
    hooks = live_row_hooks(w); hooks->action(hooks->data, later, RADIOS_PROGRAMME_ROW_REMOVE); run_for(300);
    g_assert_false(live_row_plan_has(w, later)); g_assert_cmpint(live_row_block_state(w, later_block), ==, -1);
    /* × on the very next C: the flow continues A → live step → A2. */
    hooks = live_row_hooks(w); hooks->action(hooks->data, f.plan.c, RADIOS_PROGRAMME_ROW_REMOVE); run_for(300);
    g_assert_false(live_row_plan_has(w, f.plan.c)); g_assert_cmpint(live_row_block_state(w, f.plan.c_block), ==, -1);
    RadioSPlayoutSnapshot after = live_row_flow(w);
    g_assert_cmpint(after.state, ==, RADIOS_PLAYOUT_PLAYING); g_assert_cmpstr(after.entry_id, ==, f.plan.a);
    g_assert_cmpuint(after.position_ms, >, before.position_ms); g_assert_cmpuint(live_row_epoch(w), ==, epoch);
    guint64 gap = 0; g_assert_cmpuint(capture_signal_span("MAIN", around, &gap), >, 0);
    g_test_message("Longest MAIN silence around the two removals: %.1f ms", gap / 48.0);
    g_assert_cmpfloat(gap / 48.0, <, 30);
    /* The shortened order keeps the pool alternation from the sounding A on:
     * each remaining row plays on the player of the slot it moved into, so
     * A2 does not land on A's player (no forced cut, both players loaded). */
    GArray *players = live_row_players(w);
    g_assert_cmpuint(players->len, >=, 2);
    for (guint i = 1; i < MIN(players->len, slots->len); ++i) {
        g_assert_cmpint(g_array_index(players, gint64, i), ==, g_array_index(slots, gint64, i));
        g_assert_cmpint(g_array_index(players, gint64, i), !=, g_array_index(players, gint64, i - 1));
    }
    g_array_unref(players); g_array_unref(slots);
    gchar *a2 = NULL; g_assert_true(live_row_entry_nth(w, LIVE_ROW_A2, 0, &a2, NULL));
    goffset span = capture_offset("MAIN");
    live_row_flow_wait(w, RADIOS_PLAYOUT_PLAYING, a2, 200, 15, "A2 plays after A and the live step; the removed C never does");
    g_assert_cmpfloat(tone_energy("MAIN", span, LIVE_ROW_C_HZ), <, .01);
    g_free(a2); g_free(later); g_free(later_block); live_row_close(&f);
}

static int live_row_run(void)
{
    g_test_add_func("/live-row/talk-manual-L-R-then-continue-PCM", test_live_row_talk_manual_pcm);
    g_test_add_func("/live-row/talk-duration-waits-then-continues-PCM", test_live_row_talk_duration_pcm);
    g_test_add_func("/live-row/R-release-never-skips-PCM", test_live_row_release_never_skips_pcm);
    g_test_add_func("/live-row/music-L-starts-event-schema-PCM", test_live_row_music_schema_pcm);
    g_test_add_func("/live-row/talk-L-opens-channel-and-live-schema-PCM", test_live_row_talk_schema_pcm);
    g_test_add_func("/live-row/header-transport-drives-general-queue-list-PCM", test_live_row_header_play_general_queue_pcm);
    g_test_add_func("/live-row/ending-warning-before-transition-PCM", test_live_row_ending_before_transition_pcm);
    g_test_add_func("/live-row/remove-unheard-content-from-running-general-queue-PCM", test_live_row_remove_unheard_content_pcm);
    return g_test_run();
}
int main(int argc, char **argv) { return radios_live_row_fixture_main(argc, argv); }
