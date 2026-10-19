#include "radios/loudness_settings.h"
#include "radios/audio_ui.h"
#include "radios/loudness_policy.h"
#include "radios/player_workspace.h"
#include "radios/station_time.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *owner, *controls, *status, *players_all, *players[32], *contexts[3];
    GtkWidget *phase, *automations_mode, *automations, *automation_box;
    GtkWidget *pairs_mode, *pair_box, *pair_source, *pair_target, *pair_both, *pairs;
    GtkWidget *schedule, *schedule_box, *timezone, *windows, *days[7], *start, *end;
    GtkListStore *automation_store, *pair_store, *window_store;
    GHashTable *names;
    RadioSDataClient *catalog_client;
    RadioSAudioLoudnessPolicy shown;
    guint timer, topology_hash, catalog_ticket;
    gboolean rendering, known, disposed, catalog_requested;
} LoudnessSettings;
typedef struct { RadioSDataClient *client; guint ticket; const gchar *kind; } CatalogRequest;
static const gchar *const days[] = {"Pzt", "Sal", "Çar", "Per", "Cum", "Cmt", "Paz"};
static void render(LoudnessSettings *s, const RadioSAudioLoudnessPolicy *policy);
static GtkWidget *named(GtkWidget *widget, const gchar *name)
{ gtk_widget_set_name(widget, name); return widget; }
static void pack(GtkWidget *box, GtkWidget *child)
{ gtk_box_pack_start(GTK_BOX(box), child, FALSE, FALSE, 0); }
static GtkWidget *label(const gchar *text)
{
    GtkWidget *widget = gtk_label_new(text); gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE); gtk_label_set_line_wrap_mode(GTK_LABEL(widget), PANGO_WRAP_WORD_CHAR);
    return widget;
}
static void note(LoudnessSettings *s, const gchar *text)
{ gtk_label_set_text(GTK_LABEL(s->status), text); }
static gchar *player_name(LoudnessSettings *s, guint deck)
{
    const RadioSPlayerTopology *topology = s->ctx->players ? radios_player_workspace_topology(s->ctx->players) : NULL;
    const RadioSPlayerDefinition *definition = topology ? radios_player_topology_player_const(topology, deck) : NULL;
    return definition && definition->name[0] ? g_strdup_printf("%u · %s", deck + 1, definition->name) : g_strdup_printf("Oynatıcı %u", deck + 1);
}
static guint player_hash(LoudnessSettings *s)
{
    const RadioSPlayerTopology *topology = s->ctx->players ? radios_player_workspace_topology(s->ctx->players) : NULL;
    guint hash = 5381;
    for (guint i = 0; topology && i < 32; ++i) hash = hash * 33 + g_str_hash(topology->players[i].name);
    return hash;
}
static void pair_names_refresh(LoudnessSettings *s)
{
    gint from = gtk_combo_box_get_active(GTK_COMBO_BOX(s->pair_source)), to = gtk_combo_box_get_active(GTK_COMBO_BOX(s->pair_target));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(s->pair_source)); gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(s->pair_target));
    for (guint i = 0; i < 32; ++i) { gchar *name = player_name(s, i); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->pair_source), name); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->pair_target), name); g_free(name); }
    gtk_combo_box_set_active(GTK_COMBO_BOX(s->pair_source), MAX(0, from)); gtk_combo_box_set_active(GTK_COMBO_BOX(s->pair_target), MAX(0, to));
}
static void choice_width(GtkWidget *widget)
{
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(widget));
    for (GList *item = cells; item; item = item->next) if (GTK_IS_CELL_RENDERER_TEXT(item->data))
        g_object_set(item->data, "ellipsize", PANGO_ELLIPSIZE_END, "max-width-chars", 20, NULL);
    g_list_free(cells);
}
static JsonObject *base(LoudnessSettings *s)
{
    RadioSAudioLoudnessPolicy current;
    if (s->disposed || !radios_audio_ui_loudness_policy_ready(s->ctx) || !radios_audio_ui_loudness_policy(s->ctx, &current)) {
        note(s, "Kayıtlı ses eşitleme tercihleri bekleniyor."); return NULL;
    }
    return radios_audio_loudness_policy_to_json(&current);
}
static gboolean commit(LoudnessSettings *s, JsonObject *object)
{
    RadioSAudioLoudnessPolicy policy; GError *error = NULL;
    gboolean ok = radios_audio_loudness_policy_from_json(&policy, object, &error) &&
        radios_audio_ui_set_loudness_policy(s->ctx, &policy, &error);
    json_object_unref(object);
    if (ok) { render(s, &policy); note(s, "Kapsam ve zamanlama uygulandı; veritabanına kaydediliyor."); }
    else {
        note(s, error ? error->message : "Tercih uygulanamadı.");
        RadioSAudioLoudnessPolicy current;
        if (radios_audio_ui_loudness_policy(s->ctx, &current)) render(s, &current);
    }
    g_clear_error(&error); return ok;
}
static void choice_changed(GtkComboBox *widget, gpointer data)
{
    LoudnessSettings *s = data; if (s->rendering) return;
    const gchar *key = g_object_get_data(G_OBJECT(widget), "policy-key"), *value = gtk_combo_box_get_active_id(widget);
    JsonObject *object = base(s); if (!object) return;
    if (!key || !value) { json_object_unref(object); return; }
    json_object_set_string_member(object, key, value); commit(s, object);
}
static void player_changed(GtkToggleButton *widget, gpointer data)
{
    LoudnessSettings *s = data; if (s->rendering) return;
    JsonObject *object = base(s); if (!object) return;
    guint deck = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), "player-index"));
    JsonArray *old = json_object_get_array_member(object, "players"), *next = json_array_new();
    gboolean active = gtk_toggle_button_get_active(widget);
    for (guint i = 1; i <= 32; ++i) {
        gboolean selected = FALSE;
        for (guint j = 0; j < json_array_get_length(old); ++j) selected |= json_array_get_int_element(old, j) == (gint64)i;
        if (i == deck + 1) selected = active;
        if (selected) json_array_add_int_element(next, i);
    }
    json_object_set_array_member(object, "players", next); commit(s, object);
}
static void all_players_changed(GtkToggleButton *widget, gpointer data)
{
    LoudnessSettings *s = data; if (s->rendering) return;
    JsonObject *object = base(s); if (!object) return;
    JsonArray *players = json_array_new();
    if (gtk_toggle_button_get_active(widget)) for (guint i = 1; i <= 32; ++i) json_array_add_int_element(players, i);
    json_object_set_array_member(object, "players", players); commit(s, object);
}
static void context_changed(GtkToggleButton *widget, gpointer data)
{
    LoudnessSettings *s = data; if (s->rendering) return;
    JsonObject *object = base(s); if (!object) return;
    const gchar *value = g_object_get_data(G_OBJECT(widget), "context-name");
    const gchar *names[] = {"manual", "automation", "autodj"};
    JsonArray *old = json_object_get_array_member(object, "contexts"), *next = json_array_new();
    for (guint i = 0; i < 3; ++i) {
        gboolean selected = FALSE;
        for (guint j = 0; j < json_array_get_length(old); ++j) selected |= g_str_equal(names[i], json_array_get_string_element(old, j));
        if (g_str_equal(names[i], value)) selected = gtk_toggle_button_get_active(widget);
        if (selected) json_array_add_string_element(next, names[i]);
    }
    json_object_set_array_member(object, "contexts", next); commit(s, object);
}
static void automation_toggled(GtkCellRendererToggle *cell, gchar *path, gpointer data)
{
    (void)cell; LoudnessSettings *s = data; GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(s->automation_store), &iter, path)) return;
    gchar *id = NULL; gboolean selected = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(s->automation_store), &iter, 0, &selected, 1, &id, -1);
    JsonObject *object = base(s); if (!object) { g_free(id); return; }
    JsonArray *old = json_object_get_array_member(object, "automation_ids"), *next = json_array_new();
    for (guint i = 0; i < json_array_get_length(old); ++i) {
        const gchar *value = json_array_get_string_element(old, i);
        if (!g_str_equal(value, id)) json_array_add_string_element(next, value);
    }
    if (!selected) json_array_add_string_element(next, id);
    json_object_set_array_member(object, "automation_ids", next); commit(s, object); g_free(id);
}
static void pair_add(GtkButton *button, gpointer data)
{
    (void)button; LoudnessSettings *s = data; JsonObject *object = base(s); if (!object) return;
    gint from = gtk_combo_box_get_active(GTK_COMBO_BOX(s->pair_source)) + 1, to = gtk_combo_box_get_active(GTK_COMBO_BOX(s->pair_target)) + 1;
    if (from == to || from < 1 || to < 1) { note(s, "İki farklı oynatıcı seçin."); json_object_unref(object); return; }
    JsonArray *pairs = json_object_get_array_member(object, "pairs");
    guint count = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->pair_both)) ? 2 : 1;
    for (guint direction = 0; direction < count; ++direction) {
        gint source = direction ? to : from, target = direction ? from : to; gboolean exists = FALSE;
        for (guint i = 0; i < json_array_get_length(pairs); ++i) {
            JsonObject *pair = json_array_get_object_element(pairs, i);
            exists |= radios_json_int(pair, "from", 0) == source && radios_json_int(pair, "to", 0) == target;
        }
        if (!exists) { JsonObject *pair = json_object_new(); json_object_set_int_member(pair, "from", source); json_object_set_int_member(pair, "to", target); json_array_add_object_element(pairs, pair); }
    }
    commit(s, object);
}
static void pair_remove(GtkButton *button, gpointer data)
{
    (void)button; LoudnessSettings *s = data; GtkTreeIter iter; GtkTreeModel *model = NULL;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(s->pairs)), &model, &iter)) return;
    guint from = 0, to = 0; gtk_tree_model_get(model, &iter, 0, &from, 1, &to, -1);
    JsonObject *object = base(s); if (!object) return;
    JsonArray *pairs = json_object_get_array_member(object, "pairs");
    for (guint i = 0; i < json_array_get_length(pairs); ++i) {
        JsonObject *pair = json_array_get_object_element(pairs, i);
        if (radios_json_int(pair, "from", 0) == from && radios_json_int(pair, "to", 0) == to) { json_array_remove_element(pairs, i); break; }
    }
    commit(s, object);
}
static void schedule_changed(GtkToggleButton *widget, gpointer data)
{
    LoudnessSettings *s = data; if (s->rendering) return;
    JsonObject *object = base(s); if (!object) return;
    json_object_set_boolean_member(object, "schedule_enabled", gtk_toggle_button_get_active(widget)); commit(s, object);
}
static void timezone_apply(GtkButton *button, gpointer data)
{
    (void)button; LoudnessSettings *s = data; JsonObject *object = base(s); if (!object) return;
    json_object_set_string_member(object, "timezone", gtk_entry_get_text(GTK_ENTRY(s->timezone))); commit(s, object);
}
static gboolean time_output(GtkSpinButton *widget, gpointer data)
{
    (void)data; guint minute = gtk_spin_button_get_value_as_int(widget);
    gchar text[6]; g_snprintf(text, sizeof text, "%02u:%02u", minute / 60, minute % 60);
    g_object_set_data(G_OBJECT(widget), "clock-rendering", GINT_TO_POINTER(TRUE));
    gtk_entry_set_text(GTK_ENTRY(widget), text); g_object_set_data(G_OBJECT(widget), "clock-rendering", NULL); return TRUE;
}
static gint time_input(GtkSpinButton *widget, gdouble *value, gpointer data)
{
    (void)data; guint hour = 0, minute = 0; gchar extra = 0;
    if (sscanf(gtk_entry_get_text(GTK_ENTRY(widget)), "%u:%u%c", &hour, &minute, &extra) != 2 || hour > 23 || minute > 59) {
        g_object_set_data(G_OBJECT(widget), "clock-invalid", GINT_TO_POINTER(TRUE)); return GTK_INPUT_ERROR;
    }
    *value = hour * 60 + minute; return TRUE;
}
static void time_edited(GtkEditable *widget, gpointer data)
{
    (void)data;
    if (!g_object_get_data(G_OBJECT(widget), "clock-rendering")) g_object_set_data(G_OBJECT(widget), "clock-invalid", NULL);
}
static GtkWidget *time_spin(const gchar *name)
{
    GtkWidget *widget = named(gtk_spin_button_new_with_range(0, 1439, 1), name);
    gtk_entry_set_width_chars(GTK_ENTRY(widget), 5); gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(widget), FALSE);
    g_signal_connect(widget, "output", G_CALLBACK(time_output), NULL); g_signal_connect(widget, "input", G_CALLBACK(time_input), NULL);
    g_signal_connect(widget, "changed", G_CALLBACK(time_edited), NULL); time_output(GTK_SPIN_BUTTON(widget), NULL);
    return widget;
}
static gint selected_window(LoudnessSettings *s)
{
    GtkTreeIter iter; GtkTreeModel *model = NULL; guint index = 0;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(s->windows)), &model, &iter)) return -1;
    gtk_tree_model_get(model, &iter, 0, &index, -1); return (gint)index;
}
static void window_selected(GtkTreeSelection *selection, gpointer data)
{
    (void)selection; LoudnessSettings *s = data; if (s->rendering) return;
    gint index = selected_window(s); if (index < 0 || (guint)index >= s->shown.window_count) return;
    const RadioSAudioLoudnessWindow *window = &s->shown.windows[index];
    for (guint day = 0; day < 7; ++day) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->days[day]), (window->days & (1u << day)) != 0);
    g_object_set_data(G_OBJECT(s->start), "clock-invalid", NULL); g_object_set_data(G_OBJECT(s->end), "clock-invalid", NULL);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->start), window->start_minute);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->end), window->end_minute);
}
static void window_write(GtkButton *button, gpointer data)
{
    LoudnessSettings *s = data; gboolean update = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "window-update"));
    JsonObject *object = base(s); if (!object) return;
    if (update) {
        RadioSAudioLoudnessPolicy latest;
        if (!radios_audio_loudness_policy_from_json(&latest, object, NULL) || !radios_audio_loudness_policy_equal(&latest, &s->shown)) {
            json_object_unref(object); note(s, "Tercihler değişti; düzenlemek istediğiniz aralığı yeniden seçin."); return;
        }
    }
    gint index = update ? selected_window(s) : -1;
    JsonArray *windows = json_object_get_array_member(object, "windows");
    if ((update && index < 0) || (!update && json_array_get_length(windows) >= RADIOS_AUDIO_LOUDNESS_MAX_WINDOWS)) {
        note(s, update ? "Düzenlemek için bir aralık seçin." : "En fazla 16 saat aralığı eklenebilir."); json_object_unref(object); return;
    }
    gdouble start = 0, end = 0;
    if (g_object_get_data(G_OBJECT(s->start), "clock-invalid") || g_object_get_data(G_OBJECT(s->end), "clock-invalid") ||
        time_input(GTK_SPIN_BUTTON(s->start), &start, NULL) == GTK_INPUT_ERROR || time_input(GTK_SPIN_BUTTON(s->end), &end, NULL) == GTK_INPUT_ERROR) {
        note(s, "Saatleri 00:00–23:59 aralığında SS:DD biçiminde girin."); json_object_unref(object); return;
    }
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->start), start); gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->end), end);
    JsonObject *window = json_object_new(); JsonArray *weekdays = json_array_new();
    for (guint day = 0; day < 7; ++day) if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->days[day]))) json_array_add_int_element(weekdays, day + 1);
    json_object_set_array_member(window, "days", weekdays);
    json_object_set_int_member(window, "start_minute", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s->start)));
    json_object_set_int_member(window, "end_minute", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s->end)));
    if (update) { JsonObject *existing = json_array_get_object_element(windows, index); JsonNode *n = json_node_new(JSON_NODE_OBJECT); json_node_take_object(n, window);
        const gchar *keys[] = {"days", "start_minute", "end_minute"};
        for (guint i = 0; i < 3; ++i) json_object_set_member(existing, keys[i], json_node_copy(json_object_get_member(json_node_get_object(n), keys[i])));
        json_node_free(n);
    } else json_array_add_object_element(windows, window);
    commit(s, object);
}
static void window_remove(GtkButton *button, gpointer data)
{
    (void)button; LoudnessSettings *s = data; gint index = selected_window(s); if (index < 0) return;
    JsonObject *object = base(s); if (!object) return;
    RadioSAudioLoudnessPolicy latest;
    if (!radios_audio_loudness_policy_from_json(&latest, object, NULL) || !radios_audio_loudness_policy_equal(&latest, &s->shown)) {
        json_object_unref(object); note(s, "Tercihler değişti; kaldırmak istediğiniz aralığı yeniden seçin."); return;
    }
    JsonArray *windows = json_object_get_array_member(object, "windows");
    if ((guint)index < json_array_get_length(windows)) json_array_remove_element(windows, index);
    commit(s, object);
}
static GtkWidget *choice(LoudnessSettings *s, const gchar *name, const gchar *key, const gchar *const *ids, const gchar *const *titles, guint count)
{
    GtkWidget *widget = named(gtk_combo_box_text_new(), name);
    for (guint i = 0; i < count; ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), ids[i], titles[i]);
    g_object_set_data(G_OBJECT(widget), "policy-key", (gpointer)key); g_signal_connect(widget, "changed", G_CALLBACK(choice_changed), s); return widget;
}
static GtkWidget *row(GtkWidget *body, const gchar *title, GtkWidget *widget)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); GtkWidget *caption = label(title);
    gtk_widget_set_size_request(caption, 125, -1); pack(box, caption); gtk_box_pack_start(GTK_BOX(box), widget, TRUE, TRUE, 0); pack(body, box); return box;
}
static GtkWidget *table(GtkListStore *store, const gchar *name, gint text_column)
{
    GtkWidget *view = named(gtk_tree_view_new_with_model(GTK_TREE_MODEL(store)), name); gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
    GtkCellRenderer *text = gtk_cell_renderer_text_new(); g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), gtk_tree_view_column_new_with_attributes("", text, "text", text_column, NULL)); return view;
}
static GtkWidget *scrolled(GtkWidget *widget, gint height)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, height); gtk_container_add(GTK_CONTAINER(scroll), widget); return scroll;
}
static void automation_render(LoudnessSettings *s, const RadioSAudioLoudnessPolicy *policy)
{
    gtk_list_store_clear(s->automation_store);
    GHashTable *ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTableIter it; gpointer key = NULL;
    g_hash_table_iter_init(&it, s->names); while (g_hash_table_iter_next(&it, &key, NULL)) g_hash_table_add(ids, g_strdup(key));
    for (guint i = 0; i < policy->automation_count; ++i) g_hash_table_add(ids, g_strdup(policy->automation_ids[i]));
    g_hash_table_iter_init(&it, ids);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        const gchar *name = g_hash_table_lookup(s->names, key); gboolean selected = FALSE;
        for (guint i = 0; i < policy->automation_count; ++i) selected |= g_str_equal(policy->automation_ids[i], key);
        GtkTreeIter row; gtk_list_store_append(s->automation_store, &row);
        gtk_list_store_set(s->automation_store, &row, 0, selected, 1, key, 2, name ? name : "Kayıtlı seçim · şu an listede yok", -1);
    }
    g_hash_table_unref(ids);
}
static void render(LoudnessSettings *s, const RadioSAudioLoudnessPolicy *p)
{
    s->rendering = TRUE; s->known = TRUE; s->shown = *p;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->players_all), p->players == G_MAXUINT32);
    for (guint i = 0; i < 32; ++i) { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->players[i]), (p->players & (1u << i)) != 0);
        gchar *name = player_name(s, i); gtk_button_set_label(GTK_BUTTON(s->players[i]), name); g_free(name);
        GtkWidget *text = gtk_bin_get_child(GTK_BIN(s->players[i])); gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(text), 24); }
    for (guint i = 0; i < 3; ++i) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->contexts[i]), (p->contexts & (1u << i)) != 0);
    const gchar *phases[] = {"always", "transition", "steady"}, *modes[] = {"all", "assigned", "selected"}, *pairs[] = {"all", "selected"};
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->phase), phases[p->phase]);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->automations_mode), modes[p->automation_mode]);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->pairs_mode), pairs[p->pairs_mode]);
    gtk_widget_set_sensitive(s->automation_box, p->automation_mode == RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED);
    gtk_widget_set_sensitive(s->pair_box, p->pairs_mode == RADIOS_AUDIO_LOUDNESS_PAIRS_SELECTED);
    automation_render(s, p); gtk_list_store_clear(s->pair_store);
    for (guint from = 0; from < 32; ++from) for (guint to = 0; to < 32; ++to) if (p->pairs[from] & (1u << to)) {
        gchar *a = player_name(s, from), *b = player_name(s, to), *text = g_strdup_printf("%s → %s", a, b); GtkTreeIter row;
        gtk_list_store_append(s->pair_store, &row); gtk_list_store_set(s->pair_store, &row, 0, from + 1, 1, to + 1, 2, text, -1); g_free(a); g_free(b); g_free(text);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->schedule), p->schedule_enabled); gtk_widget_set_sensitive(s->schedule_box, p->schedule_enabled);
    if (!gtk_widget_has_focus(s->timezone)) gtk_entry_set_text(GTK_ENTRY(s->timezone), p->timezone);
    gtk_list_store_clear(s->window_store);
    for (guint i = 0; i < p->window_count; ++i) {
        const RadioSAudioLoudnessWindow *w = &p->windows[i]; GString *text = g_string_new(NULL);
        for (guint day = 0; day < 7; ++day) if (w->days & (1u << day)) { if (text->len) g_string_append(text, ", "); g_string_append(text, days[day]); }
        if (!text->len) g_string_append(text, "Gün seçilmedi");
        if (w->start_minute == w->end_minute) g_string_append(text, " · tüm gün");
        else g_string_append_printf(text, " · %02u:%02u–%02u:%02u%s", w->start_minute / 60, w->start_minute % 60, w->end_minute / 60, w->end_minute % 60, w->end_minute < w->start_minute ? " (ertesi gün)" : "");
        GtkTreeIter row; gtk_list_store_append(s->window_store, &row); gtk_list_store_set(s->window_store, &row, 0, i, 1, text->str, -1); g_string_free(text, TRUE);
    }
    s->rendering = FALSE;
}
static void catalog_request_free(gpointer data)
{ CatalogRequest *request = data; radios_data_client_unref(request->client); g_free(request); }
static void catalog_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *error, gpointer data)
{
    CatalogRequest *request = data; LoudnessSettings *s = g_object_get_data(G_OBJECT(owner), "radios-loudness-settings");
    if (!s || s->disposed || request->ticket != s->catalog_ticket) return;
    RadioSDataClient *current = radios_data_client_acquire(ctx); gboolean same = radios_data_client_same_identity(current, request->client); g_clear_pointer(&current, radios_data_client_unref);
    if (!same) { s->catalog_requested = FALSE; return; }
    if (error || !node || !JSON_NODE_HOLDS_OBJECT(node)) { note(s, error ? error->message : "Otomasyon adları okunamadı."); return; }
    JsonObject *object = json_node_get_object(node); JsonNode *rows = json_object_get_member(object, "records");
    if (!rows || !JSON_NODE_HOLDS_ARRAY(rows)) { note(s, "Otomasyon listesi okunamadı."); return; }
    JsonArray *records = json_node_get_array(rows);
    for (guint i = 0; i < json_array_get_length(records); ++i) {
        JsonNode *entry = json_array_get_element(records, i); if (!JSON_NODE_HOLDS_OBJECT(entry)) continue;
        JsonObject *record = json_node_get_object(entry); const gchar *id = radios_json_string(record, "_id", "");
        JsonNode *configuration = json_object_get_member(record, "config");
        JsonObject *config = configuration && JSON_NODE_HOLDS_OBJECT(configuration) ? json_node_get_object(configuration) : NULL;
        const gchar *name = radios_json_string(config, "title", "");
        if (!*name) name = radios_json_string(record, "name", "Otomasyon");
        const gchar *category = g_str_equal(request->kind, "weekly_programme") ? "Program" :
            g_str_equal(request->kind, "automation_calendar") ? "Takvim / rotasyon" : "Otomasyon";
        if (g_uuid_string_is_valid(id)) g_hash_table_replace(s->names, g_strdup(id), g_strdup_printf("%s · %s", category, name));
    }
    if (s->known) automation_render(s, &s->shown);
}
static void catalog_refresh(GtkButton *button, gpointer data)
{
    (void)button; LoudnessSettings *s = data; if (s->disposed || !s->ctx->data) return;
    const gchar *kinds[] = {"weekly_programme", "automation", "automation_calendar"}; s->catalog_requested = TRUE; ++s->catalog_ticket;
    g_hash_table_remove_all(s->names);
    if (s->known) automation_render(s, &s->shown);
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        RadioSDataClient *client = radios_data_client_acquire(s->ctx); if (!client) { s->catalog_requested = FALSE; continue; }
        CatalogRequest *request = g_new0(CatalogRequest, 1); request->client = client; request->ticket = s->catalog_ticket; request->kind = kinds[i];
        JsonObject *body = json_object_new();
        json_object_set_string_member(body, "station_id", radios_data_client_station_id(client));
        json_object_set_string_member(body, "kind", kinds[i]); JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, body);
        radios_data_request(s->ctx, s->owner, "POST", "/v1/workflows/desktop/list", node, catalog_loaded, request, catalog_request_free);
        json_node_free(node);
    }
}
static gboolean tick(gpointer data)
{
    LoudnessSettings *s = data; if (s->disposed) return G_SOURCE_REMOVE;
    RadioSDataClient *client = s->ctx->data ? radios_data_client_acquire(s->ctx) : NULL;
    if (client && !radios_data_client_same_identity(client, s->catalog_client)) {
        g_clear_pointer(&s->catalog_client, radios_data_client_unref); s->catalog_client = radios_data_client_ref(client);
        g_hash_table_remove_all(s->names); s->catalog_requested = FALSE; ++s->catalog_ticket;
        if (s->known) automation_render(s, &s->shown);
    }
    g_clear_pointer(&client, radios_data_client_unref);
    gboolean ready = radios_audio_ui_loudness_policy_ready(s->ctx); gtk_widget_set_sensitive(s->controls, ready);
    RadioSAudioLoudnessPolicy current;
    if (ready && radios_audio_ui_loudness_policy(s->ctx, &current)) {
        guint topology = player_hash(s); gboolean first = !s->known;
        if (topology != s->topology_hash) { pair_names_refresh(s); s->topology_hash = topology; render(s, &current); }
        else if (!s->known || !radios_audio_loudness_policy_equal(&current, &s->shown)) render(s, &current);
        if (first) note(s, "Kapsam ve zamanlama hazır.");
        if (!s->catalog_requested) catalog_refresh(NULL, s);
    }
    return G_SOURCE_CONTINUE;
}
static void destroyed(GtkWidget *widget, gpointer data)
{ (void)widget; LoudnessSettings *s = data; s->disposed = TRUE; if (s->timer) { g_source_remove(s->timer); s->timer = 0; } }
static void state_free(gpointer data)
{
    LoudnessSettings *s = data; g_clear_pointer(&s->catalog_client, radios_data_client_unref); g_clear_object(&s->automation_store); g_clear_object(&s->pair_store); g_clear_object(&s->window_store); g_hash_table_unref(s->names); g_free(s);
}
GtkWidget *radios_loudness_settings_new(RadioSViewContext *ctx)
{
    LoudnessSettings *s = g_new0(LoudnessSettings, 1); s->ctx = ctx; s->names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    s->owner = named(gtk_expander_new("Nerede ve ne zaman uygulanacak"), "preferences-loudness-scope");
    g_object_set_data_full(G_OBJECT(s->owner), "radios-loudness-settings", s, state_free); g_signal_connect(s->owner, "destroy", G_CALLBACK(destroyed), s);
    s->controls = named(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8), "preferences-loudness-scope-controls"); gtk_container_add(GTK_CONTAINER(s->owner), s->controls);
    s->players_all = named(gtk_check_button_new_with_label("Tüm oynatıcılar"), "preferences-loudness-players-all"); pack(s->controls, s->players_all);
    g_signal_connect(s->players_all, "toggled", G_CALLBACK(all_players_changed), s);
    GtkWidget *players = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(players), 8);
    for (guint i = 0; i < 32; ++i) {
        gchar *name = g_strdup_printf("preferences-loudness-player-%u", i), *text = player_name(s, i);
        s->players[i] = named(gtk_check_button_new_with_label(text), name); g_free(name); g_free(text);
        GtkWidget *name_label = gtk_bin_get_child(GTK_BIN(s->players[i])); gtk_label_set_ellipsize(GTK_LABEL(name_label), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(name_label), 24);
        g_object_set_data(G_OBJECT(s->players[i]), "player-index", GUINT_TO_POINTER(i));
        g_signal_connect(s->players[i], "toggled", G_CALLBACK(player_changed), s); gtk_grid_attach(GTK_GRID(players), s->players[i], i % 2, i / 2, 1, 1);
    }
    pack(s->controls, scrolled(players, 125));
    const gchar *context_names[] = {"manual", "automation", "autodj"}, *context_titles[] = {"Manuel çalma ve playlist", "Otomasyon / program", "AutoDJ"};
    GtkWidget *contexts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    for (guint i = 0; i < 3; ++i) {
        gchar *name = g_strdup_printf("preferences-loudness-context-%s", context_names[i]); s->contexts[i] = named(gtk_check_button_new_with_label(context_titles[i]), name); g_free(name);
        g_object_set_data(G_OBJECT(s->contexts[i]), "context-name", (gpointer)context_names[i]); g_signal_connect(s->contexts[i], "toggled", G_CALLBACK(context_changed), s); pack(contexts, s->contexts[i]);
    }
    pack(s->controls, contexts);
    const gchar *phase_ids[] = {"always", "transition", "steady"}, *phase_titles[] = {"Sürekli / parça boyunca", "Yalnız geçiş sırasında", "Geçiş dışında"};
    s->phase = choice(s, "preferences-loudness-phase", "phase", phase_ids, phase_titles, 3); row(s->controls, "Uygulama zamanı", s->phase);
    const gchar *mode_ids[] = {"all", "assigned", "selected"}, *mode_titles[] = {"Tüm çalmalar", "Yalnız atanmış otomasyon", "Seçili otomasyonlar"};
    s->automations_mode = choice(s, "preferences-loudness-automations-mode", "automation_mode", mode_ids, mode_titles, 3); row(s->controls, "Otomasyon kapsamı", s->automations_mode);
    s->automation_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4); pack(s->controls, s->automation_box);
    s->automation_store = gtk_list_store_new(3, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING); s->automations = table(s->automation_store, "preferences-loudness-automation-list", 2);
    GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new(); g_signal_connect(toggle, "toggled", G_CALLBACK(automation_toggled), s);
    gtk_tree_view_insert_column(GTK_TREE_VIEW(s->automations), gtk_tree_view_column_new_with_attributes("", toggle, "active", 0, NULL), 0);
    pack(s->automation_box, scrolled(s->automations, 90));
    GtkWidget *refresh = named(gtk_button_new_with_label("Otomasyonları yenile"), "preferences-loudness-automation-refresh"); g_signal_connect(refresh, "clicked", G_CALLBACK(catalog_refresh), s); pack(s->automation_box, refresh);
    const gchar *pair_ids[] = {"all", "selected"}, *pair_titles[] = {"Tüm oynatıcı kombinasyonları", "Yalnız seçili geçiş çiftleri"};
    s->pairs_mode = choice(s, "preferences-loudness-pairs-mode", "pairs_mode", pair_ids, pair_titles, 2); row(s->controls, "Eşleşmeler", s->pairs_mode);
    s->pair_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4); pack(s->controls, s->pair_box);
    GtkWidget *pair_editor = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5); s->pair_source = named(gtk_combo_box_text_new(), "preferences-loudness-pair-source"); s->pair_target = named(gtk_combo_box_text_new(), "preferences-loudness-pair-target");
    for (guint i = 0; i < 32; ++i) { gchar *name = player_name(s, i); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->pair_source), name); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->pair_target), name); g_free(name); }
    choice_width(s->pair_source); choice_width(s->pair_target);
    gtk_combo_box_set_active(GTK_COMBO_BOX(s->pair_source), 0); gtk_combo_box_set_active(GTK_COMBO_BOX(s->pair_target), 1);
    pack(pair_editor, s->pair_source); pack(pair_editor, label("→")); pack(pair_editor, s->pair_target);
    s->pair_both = named(gtk_check_button_new_with_label("İki yönde"), "preferences-loudness-pair-both"); pack(pair_editor, s->pair_both);
    GtkWidget *add = named(gtk_button_new_with_label("Ekle"), "preferences-loudness-pair-add"); pack(pair_editor, add); g_signal_connect(add, "clicked", G_CALLBACK(pair_add), s); pack(s->pair_box, pair_editor);
    s->pair_store = gtk_list_store_new(3, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_STRING); s->pairs = table(s->pair_store, "preferences-loudness-pair-list", 2); pack(s->pair_box, scrolled(s->pairs, 75));
    GtkWidget *remove = named(gtk_button_new_with_label("Seçili çifti kaldır"), "preferences-loudness-pair-remove"); g_signal_connect(remove, "clicked", G_CALLBACK(pair_remove), s); pack(s->pair_box, remove);
    s->schedule = named(gtk_check_button_new_with_label("Gün ve saat aralıklarında uygula"), "preferences-loudness-schedule-enabled"); pack(s->controls, s->schedule); g_signal_connect(s->schedule, "toggled", G_CALLBACK(schedule_changed), s);
    s->schedule_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4); pack(s->controls, s->schedule_box);
    s->timezone = named(gtk_entry_new(), "preferences-loudness-timezone"); gtk_entry_set_max_length(GTK_ENTRY(s->timezone), 127);
    GtkWidget *zone_row = row(s->schedule_box, "Saat dilimi", s->timezone); GtkWidget *zone_apply = named(gtk_button_new_with_label("Uygula"), "preferences-loudness-timezone-apply"); pack(zone_row, zone_apply); g_signal_connect(zone_apply, "clicked", G_CALLBACK(timezone_apply), s);
    GtkWidget *weekdays = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    for (guint i = 0; i < 7; ++i) { gchar *name = g_strdup_printf("preferences-loudness-day-%u", i + 1); s->days[i] = named(gtk_check_button_new_with_label(days[i]), name); g_free(name); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->days[i]), TRUE); pack(weekdays, s->days[i]); }
    pack(s->schedule_box, weekdays);
    GtkWidget *time_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); s->start = time_spin("preferences-loudness-window-start"); s->end = time_spin("preferences-loudness-window-end"); pack(time_row, label("Başlangıç")); pack(time_row, s->start); pack(time_row, label("Bitiş")); pack(time_row, s->end);
    GtkWidget *window_add = named(gtk_button_new_with_label("Aralık ekle"), "preferences-loudness-window-add"), *window_update = named(gtk_button_new_with_label("Seçileni güncelle"), "preferences-loudness-window-update");
    pack(s->schedule_box, time_row);
    GtkWidget *window_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    pack(window_actions, window_add); pack(window_actions, window_update); pack(s->schedule_box, window_actions);
    g_object_set_data(G_OBJECT(window_update), "window-update", GINT_TO_POINTER(TRUE)); g_signal_connect(window_add, "clicked", G_CALLBACK(window_write), s); g_signal_connect(window_update, "clicked", G_CALLBACK(window_write), s);
    s->window_store = gtk_list_store_new(2, G_TYPE_UINT, G_TYPE_STRING); s->windows = table(s->window_store, "preferences-loudness-window-list", 1); pack(s->schedule_box, scrolled(s->windows, 90));
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(s->windows)), "changed", G_CALLBACK(window_selected), s);
    GtkWidget *window_remove_button = named(gtk_button_new_with_label("Seçili aralığı kaldır"), "preferences-loudness-window-remove"); g_signal_connect(window_remove_button, "clicked", G_CALLBACK(window_remove), s); pack(s->schedule_box, window_remove_button);
    s->status = named(label("Kayıtlı ses eşitleme tercihleri bekleniyor."), "preferences-loudness-scope-status"); pack(s->controls, s->status);
    RadioSAudioLoudnessPolicy policy; radios_audio_loudness_policy_default(&policy); render(s, &policy); s->known = FALSE;
    tick(s); s->timer = g_timeout_add(500, tick, s); return s->owner;
}
