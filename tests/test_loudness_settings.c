#include "radios/loudness_settings.h"
#include "radios/audio_ui.h"
#include "radios/player_workspace.h"
#include "radios/data.h"
#include <string.h>

/* Production GTK editor and core JSON validation, with a main-thread preference
 * adapter. No service, user database or transport command is involved. */
typedef struct {
    RadioSViewContext ctx;
    GtkWidget *window, *editor;
    RadioSAudioLoudnessPolicy policy;
    gboolean ready, fail_write;
    guint reads, ready_reads, writes;
} Fixture;

gboolean radios_audio_ui_loudness_policy_ready(RadioSViewContext *ctx)
{ Fixture *f = ctx->notify_data; ++f->ready_reads; return f->ready; }
gboolean radios_audio_ui_loudness_policy(RadioSViewContext *ctx, RadioSAudioLoudnessPolicy *out)
{ Fixture *f = ctx->notify_data; ++f->reads; *out = f->policy; return TRUE; }
gboolean radios_audio_ui_set_loudness_policy(RadioSViewContext *ctx, const RadioSAudioLoudnessPolicy *policy, GError **error)
{
    Fixture *f = ctx->notify_data; g_assert_true(f->ready);
    if (f->fail_write) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Private fixture save failure"); return FALSE; }
    if (!radios_audio_loudness_policy_validate(policy, error)) return FALSE;
    f->policy = *policy; ++f->writes; return TRUE;
}
const RadioSPlayerTopology *radios_player_workspace_topology(const RadioSPlayerWorkspace *workspace)
{ (void)workspace; g_assert_not_reached(); return NULL; }
RadioSDataClient *radios_data_client_acquire(RadioSViewContext *ctx)
{ (void)ctx; g_assert_not_reached(); return NULL; }
gboolean radios_data_request(RadioSViewContext *ctx, GtkWidget *owner, const gchar *method, const gchar *path,
    JsonNode *body, RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{
    (void)ctx; (void)owner; (void)method; (void)path; (void)body; (void)callback; (void)user; (void)destroy;
    g_assert_not_reached(); return FALSE;
}
const gchar *radios_json_string(JsonObject *object, const gchar *key, const gchar *fallback)
{
    JsonNode *n = object ? json_object_get_member(object, key) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : fallback;
}
gint64 radios_json_int(JsonObject *object, const gchar *key, gint64 fallback)
{
    JsonNode *n = object ? json_object_get_member(object, key) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_INT64 ? json_node_get_int(n) : fallback;
}
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *i = children; i && !found; i = i->next) found = find(i->data, name);
    g_list_free(children); return found;
}
static GtkWidget *field(Fixture *f, const gchar *suffix)
{
    gchar *name = g_strconcat("preferences-loudness-", suffix, NULL); GtkWidget *widget = find(f->editor, name);
    g_free(name); g_assert_nonnull(widget); return widget;
}
static void toggle(Fixture *f, const gchar *suffix, gboolean active)
{ gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(f, suffix)), active); }
static void choice(Fixture *f, const gchar *suffix, const gchar *id)
{ g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(f, suffix)), id)); }
static void click(Fixture *f, const gchar *suffix)
{ gtk_button_clicked(GTK_BUTTON(field(f, suffix))); }
static void text(Fixture *f, const gchar *suffix, const gchar *value)
{ gtk_entry_set_text(GTK_ENTRY(field(f, suffix)), value); }
static void select_row(Fixture *f, const gchar *suffix, guint index)
{
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)index, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(field(f, suffix))), path); gtk_tree_path_free(path);
}
static void pump(gint64 duration)
{
    gint64 end = g_get_monotonic_time() + duration;
    do { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); } while (g_get_monotonic_time() < end);
}
static void start(Fixture *f, gboolean ready)
{
    memset(f, 0, sizeof *f); f->ready = ready; radios_audio_loudness_policy_default(&f->policy);
    f->ctx.notify_data = f; f->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(f->window);
    f->ctx.parent = GTK_WINDOW(f->window); gtk_window_set_default_size(GTK_WINDOW(f->window), 740, 600);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_container_add(GTK_CONTAINER(f->window), scroll);
    f->editor = radios_loudness_settings_new(&f->ctx); gtk_expander_set_expanded(GTK_EXPANDER(f->editor), TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), f->editor); gtk_widget_show_all(f->window); pump(10000);
}
static void finish(Fixture *f)
{ gtk_widget_destroy(f->window); g_object_unref(f->window); f->window = f->editor = NULL; pump(1000); }
static void policy_equal(const RadioSAudioLoudnessPolicy *a, const RadioSAudioLoudnessPolicy *b)
{ g_assert_true(radios_audio_loudness_policy_equal(a, b)); }
static void players_context_phase_preserve_current_independent_fields(void)
{
    Fixture f; start(&f, TRUE); toggle(&f, "players-all", FALSE); g_assert_cmpuint(f.policy.players, ==, 0);
    toggle(&f, "player-31", TRUE); g_assert_cmpuint(f.policy.players, ==, ((guint32)1 << 31));
    toggle(&f, "player-0", TRUE); g_assert_cmpuint(f.policy.players, ==, (((guint32)1 << 31) | 1));
    toggle(&f, "players-all", TRUE); g_assert_cmpuint(f.policy.players, ==, G_MAXUINT32);
    toggle(&f, "player-31", FALSE); g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&f, "players-all"))));
    /* An independent update arrives before the next UI poll. Changing one
     * context must merge against that fresh policy rather than the shown copy. */
    f.policy.automation_mode = RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED;
    f.policy.automation_count = 1; g_strlcpy(f.policy.automation_ids[0], "abcdefab-cdef-4abc-8def-abcdefabcdef", 37);
    f.policy.pairs_mode = RADIOS_AUDIO_LOUDNESS_PAIRS_SELECTED; f.policy.pairs[31] = 1;
    f.policy.schedule_enabled = TRUE; g_strlcpy(f.policy.timezone, "Europe/Istanbul", 128);
    f.policy.window_count = 1; f.policy.windows[0] = (RadioSAudioLoudnessWindow){65, 1380, 60};
    RadioSAudioLoudnessPolicy expected = f.policy; expected.contexts &= ~RADIOS_AUDIO_LOUDNESS_MANUAL;
    toggle(&f, "context-manual", FALSE); policy_equal(&f.policy, &expected);
    expected.phase = RADIOS_AUDIO_LOUDNESS_PHASE_TRANSITION; choice(&f, "phase", "transition"); policy_equal(&f.policy, &expected);
    expected.phase = RADIOS_AUDIO_LOUDNESS_PHASE_STEADY; choice(&f, "phase", "steady"); policy_equal(&f.policy, &expected);
    finish(&f);
}
static void automation_modes_dormant_selection_and_independent_fields(void)
{
    Fixture f; start(&f, TRUE);
    f.policy.automation_count = 1; g_strlcpy(f.policy.automation_ids[0], "abcdefab-cdef-4abc-8def-abcdefabcdef", 37);
    choice(&f, "automations-mode", "selected");
    g_assert_cmpuint(f.policy.automation_count, ==, 1); g_assert_cmpuint(f.policy.automation_mode, ==, RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED);
    GtkTreeView *view = GTK_TREE_VIEW(field(&f, "automation-list")); GtkTreeModel *model = gtk_tree_view_get_model(view);
    g_assert_cmpint(gtk_tree_model_iter_n_children(model, NULL), ==, 1);
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(gtk_tree_view_get_column(view, 0)));
    g_assert_true(GTK_IS_CELL_RENDERER_TOGGLE(cells->data)); g_signal_emit_by_name(cells->data, "toggled", "0"); g_list_free(cells);
    g_assert_cmpuint(f.policy.automation_count, ==, 0);
    choice(&f, "automations-mode", "assigned"); g_assert_cmpuint(f.policy.automation_mode, ==, RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_ASSIGNED);
    choice(&f, "automations-mode", "all"); g_assert_cmpuint(f.policy.automation_mode, ==, RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_ALL);
    g_assert_cmpuint(f.policy.players, ==, G_MAXUINT32); g_assert_cmpuint(f.policy.contexts, ==, 7); finish(&f);
}
static void directed_pairs_both_remove_duplicate_and_self_guard(void)
{
    Fixture f; start(&f, TRUE); choice(&f, "pairs-mode", "selected");
    gtk_combo_box_set_active(GTK_COMBO_BOX(field(&f, "pair-source")), 31);
    gtk_combo_box_set_active(GTK_COMBO_BOX(field(&f, "pair-target")), 0); click(&f, "pair-add");
    g_assert_cmpuint(f.policy.pairs[31], ==, 1); g_assert_cmpuint(f.policy.pairs[0], ==, 0);
    toggle(&f, "pair-both", TRUE); click(&f, "pair-add");
    g_assert_cmpuint(f.policy.pairs[31], ==, 1); g_assert_cmpuint(f.policy.pairs[0], ==, ((guint32)1 << 31));
    click(&f, "pair-add"); GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(field(&f, "pair-list")));
    g_assert_cmpint(gtk_tree_model_iter_n_children(model, NULL), ==, 2);
    select_row(&f, "pair-list", 0); click(&f, "pair-remove");
    g_assert_cmpuint(f.policy.pairs[0], ==, 0); g_assert_cmpuint(f.policy.pairs[31], ==, 1);
    guint writes = f.writes; gtk_combo_box_set_active(GTK_COMBO_BOX(field(&f, "pair-target")), 31); click(&f, "pair-add");
    g_assert_cmpuint(f.writes, ==, writes); g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(field(&f, "scope-status"))), "farklı"));
    finish(&f);
}
static void overnight_windows_edit_remove_schedule_and_stale_selection(void)
{
    Fixture f; start(&f, TRUE); toggle(&f, "schedule-enabled", TRUE);
    text(&f, "timezone", "Europe/Istanbul"); click(&f, "timezone-apply"); g_assert_cmpstr(f.policy.timezone, ==, "Europe/Istanbul");
    for (guint i = 1; i <= 7; ++i) { gchar *name = g_strdup_printf("day-%u", i); toggle(&f, name, i == 1 || i == 7); g_free(name); }
    text(&f, "window-start", "23:10"); text(&f, "window-end", "01:20"); click(&f, "window-add");
    g_assert_cmpuint(f.policy.window_count, ==, 1); g_assert_cmpuint(f.policy.windows[0].days, ==, 65);
    g_assert_cmpuint(f.policy.windows[0].start_minute, ==, 1390); g_assert_cmpuint(f.policy.windows[0].end_minute, ==, 80);
    g_assert_true(radios_audio_loudness_policy_time_matches(&f.policy, 1, 30));
    g_assert_false(radios_audio_loudness_policy_time_matches(&f.policy, 1, 80));
    select_row(&f, "window-list", 0); text(&f, "window-start", "09:30"); text(&f, "window-end", "10:00"); click(&f, "window-update");
    g_assert_cmpuint(f.policy.window_count, ==, 1); g_assert_cmpuint(f.policy.windows[0].start_minute, ==, 570);
    g_assert_cmpuint(f.policy.windows[0].end_minute, ==, 600);
    select_row(&f, "window-list", 0); RadioSAudioLoudnessPolicy changed = f.policy;
    changed.windows[0].start_minute = 800; f.policy = changed; guint writes = f.writes;
    click(&f, "window-remove"); g_assert_cmpuint(f.writes, ==, writes); policy_equal(&f.policy, &changed);
    pump(550000); select_row(&f, "window-list", 0); click(&f, "window-remove"); g_assert_cmpuint(f.policy.window_count, ==, 0);
    toggle(&f, "schedule-enabled", FALSE); g_assert_false(f.policy.schedule_enabled); g_assert_cmpstr(f.policy.timezone, ==, "Europe/Istanbul"); finish(&f);
}
static void invalid_timezone_and_backend_failure_keep_policy(void)
{
    Fixture f; start(&f, TRUE); toggle(&f, "schedule-enabled", TRUE); RadioSAudioLoudnessPolicy expected = f.policy;
    guint writes = f.writes; text(&f, "timezone", "No/Such_Zone"); click(&f, "timezone-apply");
    policy_equal(&f.policy, &expected); g_assert_cmpuint(f.writes, ==, writes);
    g_assert_null(strstr(gtk_label_get_text(GTK_LABEL(field(&f, "scope-status"))), "uygulandı"));
    f.fail_write = TRUE; choice(&f, "phase", "steady"); policy_equal(&f.policy, &expected); g_assert_cmpuint(f.writes, ==, writes);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&f, "phase"))), ==, "always");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(field(&f, "scope-status"))), "Private fixture")); finish(&f);
}
static void incomplete_clock_draft_must_not_create_window(void)
{
    Fixture f; start(&f, TRUE); toggle(&f, "schedule-enabled", TRUE); RadioSAudioLoudnessPolicy expected = f.policy;
    guint writes = f.writes; text(&f, "window-start", "09:"); text(&f, "window-end", "10:00"); click(&f, "window-add");
    g_assert_cmpuint(f.writes, ==, writes); policy_equal(&f.policy, &expected); finish(&f);
}
static void waiting_baseline_external_refresh_and_destroy_stops_reads(void)
{
    Fixture f; start(&f, FALSE); g_assert_false(gtk_widget_is_sensitive(field(&f, "scope-controls")));
    toggle(&f, "player-31", FALSE); g_assert_cmpuint(f.writes, ==, 0);
    f.policy.players = ((guint32)1 << 31); f.policy.contexts = RADIOS_AUDIO_LOUDNESS_AUTOMATION;
    f.policy.phase = RADIOS_AUDIO_LOUDNESS_PHASE_STEADY; f.ready = TRUE; pump(550000);
    g_assert_true(gtk_widget_is_sensitive(field(&f, "scope-controls")));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&f, "player-31"))));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&f, "player-0"))));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&f, "players-all"))));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&f, "phase"))), ==, "steady");
    g_assert_cmpuint(f.writes, ==, 0); finish(&f); guint reads = f.reads, ready_reads = f.ready_reads;
    pump(550000); g_assert_cmpuint(f.reads, ==, reads); g_assert_cmpuint(f.ready_reads, ==, ready_reads); g_assert_cmpuint(f.writes, ==, 0);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); if (!gtk_init_check(&argc, &argv)) return 77;
    g_test_add_func("/loudness-settings/player32-all-context-phase-current-base", players_context_phase_preserve_current_independent_fields);
    g_test_add_func("/loudness-settings/automation-modes-dormant-selection", automation_modes_dormant_selection_and_independent_fields);
    g_test_add_func("/loudness-settings/directed-pairs-both-remove-self-guard", directed_pairs_both_remove_duplicate_and_self_guard);
    g_test_add_func("/loudness-settings/overnight-window-edit-remove-stale-guard", overnight_windows_edit_remove_schedule_and_stale_selection);
    g_test_add_func("/loudness-settings/invalid-timezone-save-failure-rollback", invalid_timezone_and_backend_failure_keep_policy);
    g_test_add_func("/loudness-settings/incomplete-clock-draft-no-save", incomplete_clock_draft_must_not_create_window);
    g_test_add_func("/loudness-settings/waiting-baseline-refresh-destroy", waiting_baseline_external_refresh_and_destroy_stops_reads);
    return g_test_run();
}
