#include "radios/probability_editor.h"
#include "radios/automation_policy.h"
#include "radios/data.h"
#include "radios/station_time.h"
#include "radios/widgets.h"
#include "radios/platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Plain forms are the primary editor. The complete tables remain available
 * behind Advanced, including timing rules from older saved policies. Only
 * fields present in a form are changed; nested policies stay untouched. */
enum { CHOICE_ID, CHOICE_NAME, CHOICE_PERCENT, CHOICE_NOTE, CHOICE_REST, CHOICE_COLUMNS };
enum { TR_ID, TR_MODE, TR_PERCENT, TR_EFFECT, TR_SHAPED, TR_OVERLAP, TR_EARLY, TR_FREQUENCY, TR_INTERVAL, TR_EXCLUDED, TR_COLUMNS };
enum { JG_ID, JG_PLAYLIST, JG_PLAYER, JG_ANCHOR, JG_SECONDS, JG_PERCENT, JG_DUCK, JG_FREQUENCY, JG_INTERVAL, JG_EXCLUDED, JG_COLUMNS };
enum { OPTION_ID, OPTION_NAME, OPTION_COLUMNS };
static const gchar *const MODES[] = {"Parça sonunda", "Crossfade", "Cue", "SYNC beatmatch"};
static const gchar *const LEVELS[] = {"automation", "playlist", "track"};
static const gchar *const LEVEL_TITLES[] = {"Rotasyonlar", "Playlistler", "Şarkılar"};
typedef struct {
    RadioSViewContext *ctx;
    RadioSProbabilityEditorHooks hooks;
    GtkWidget *root, *status, *tabs, *advanced, *hint, *level, *choice_section, *choices, *choice_summary, *transitions, *transition_summary;
    GtkWidget *jingles, *jingle_summary, *jingle_playlist, *jingle_player, *jingle_add;
    GtkWidget *choice_list, *choice_percent, *choice_auto, *choice_save;
    GtkWidget *transition_list, *transition_modes[4], *transition_percent, *transition_auto;
    GtkWidget *transition_overlap, *transition_early, *transition_effect, *transition_save, *transition_remove;
    GtkWidget *jingle_list, *jingle_anchor, *jingle_seconds, *jingle_percent, *jingle_auto, *jingle_duck, *jingle_save, *jingle_remove;
    GtkWidget *choice_table_box, *transition_table_box, *jingle_table_box;
    GtkListStore *choice_store, *transition_store, *jingle_store, *modes, *anchors, *playlists, *players;
    GtkCssProvider *style;
    GdkScreen *screen;
    JsonNode *policy;
    JsonArray *catalogue;
    RadioSDataClient *opened_client;
    gchar *zone, *preferred_level, *focus, *choice_selected, *transition_selected, *jingle_selected;
    gboolean updating, closed;
} ProbabilityEditor;
static void render(ProbabilityEditor *editor);
static void simple_render(ProbabilityEditor *editor);
static void simple_load(ProbabilityEditor *editor);
static gboolean transition_form_apply(ProbabilityEditor *editor, JsonObject *rule, JsonArray *siblings);
static gboolean jingle_form_apply(ProbabilityEditor *editor, JsonObject *rule, JsonArray *siblings);
static JsonObject *obj(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *arr(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static guint length(JsonArray *array) { return array ? json_array_get_length(array) : 0; }
static JsonObject *item(JsonArray *array, guint index)
{
    JsonNode *node = array && index < json_array_get_length(array) ? json_array_get_element(array, index) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonNode *clone(JsonNode *node)
{
    if (!node) return NULL;
    gchar *text = json_to_string(node, FALSE); JsonParser *parser = json_parser_new();
    gboolean valid = json_parser_load_from_data(parser, text, -1, NULL); g_free(text);
    JsonNode *copy = valid ? json_parser_steal_root(parser) : NULL; g_object_unref(parser); return copy;
}
static JsonObject *root_policy(ProbabilityEditor *editor)
{ return editor->policy && JSON_NODE_HOLDS_OBJECT(editor->policy) ? json_node_get_object(editor->policy) : NULL; }
static void notice(ProbabilityEditor *editor, const gchar *message)
{ gtk_label_set_text(GTK_LABEL(editor->status), message ? message : ""); }
static gboolean identity_current(ProbabilityEditor *editor)
{
    if (!editor->ctx->data) return TRUE;
    RadioSDataClient *client = radios_data_client_acquire(editor->ctx);
    gboolean same = radios_data_client_same_identity(client, editor->opened_client);
    g_clear_pointer(&client, radios_data_client_unref);
    if (!same) notice(editor, "Veri oturumu değişti; olasılık penceresini kapatıp yeniden açın.");
    return same;
}
static gboolean probability_defined(JsonObject *object)
{
    JsonNode *node = object ? json_object_get_member(object, "probability") : NULL;
    return node && !JSON_NODE_HOLDS_NULL(node);
}
static gdouble numeric(JsonObject *object, const gchar *key, gdouble fallback)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && (json_node_get_value_type(node) == G_TYPE_INT64 || json_node_get_value_type(node) == G_TYPE_DOUBLE) ? json_node_get_double(node) : fallback;
}
static gchar *number_text(gdouble value)
{ return fabs(value - round(value)) < 1e-9 ? g_strdup_printf("%.0f", value) : g_strdup_printf("%.1f", value); }
/* Empty text means "not set". "%" and spaces are ignored; 12,5 and 12.5 both work. */
static gboolean value_parse(const gchar *text, gdouble low, gdouble high, gboolean integer, gboolean *empty, gdouble *value)
{
    gchar *clean = g_strdup(text ? text : ""); g_strstrip(clean);
    gsize size = strlen(clean);
    if (size && clean[size - 1] == '%') clean[size - 1] = 0;
    if (clean[0] == '%') memmove(clean, clean + 1, strlen(clean));
    g_strstrip(clean);
    for (gchar *p = clean; *p; ++p) if (*p == ',') *p = '.';
    *empty = !*clean; gchar *end = NULL; gdouble parsed = *empty ? 0 : g_ascii_strtod(clean, &end);
    gboolean okay = *empty || (end && !*end && isfinite(parsed) && parsed >= low && parsed <= high && (!integer || trunc(parsed) == parsed));
    g_free(clean); if (okay) *value = parsed; return okay;
}
static GtkWidget *named(GtkWidget *widget, const gchar *suffix)
{
    gchar *name = g_strconcat("probability-editor-", suffix, NULL); gtk_widget_set_name(widget, name); g_free(name); return widget;
}
static GtkWidget *wrapped(const gchar *text)
{
    GtkWidget *label = radios_ui_label(text, "re-caption"); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(label), 120); return label;
}
static GtkWidget *heading(const gchar *text)
{
    GtkWidget *label = gtk_label_new(NULL); gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup); g_free(markup); gtk_label_set_xalign(GTK_LABEL(label), 0); return label;
}
static void compact_combo(GtkWidget *combo)
{
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(combo));
    for (GList *cell = cells; cell; cell = cell->next)
        if (GTK_IS_CELL_RENDERER_TEXT(cell->data))
            g_object_set(cell->data, "ellipsize", PANGO_ELLIPSIZE_END, "max-width-chars", 20, NULL);
    g_list_free(cells);
    gtk_widget_set_hexpand(combo, TRUE);
}
/* Catalogue */
static JsonObject *target_by_id(ProbabilityEditor *editor, const gchar *id)
{
    for (guint i = 0; id && i < length(editor->catalogue); ++i) {
        JsonObject *target = item(editor->catalogue, i);
        if (target && !g_ascii_strcasecmp(radios_json_string(target, "id", ""), id)) return target;
    }
    return NULL;
}
static gboolean target_listed(JsonObject *target)
{
    JsonNode *node = json_object_get_member(target, "listed");
    return !node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN || json_node_get_boolean(node);
}
/* Groups of one level: one per id set; empty ids is the explicit "Kalanlar". */
static gboolean ids_contain(JsonArray *ids, const gchar *id)
{
    for (guint i = 0; id && i < length(ids); ++i) {
        JsonNode *node = json_array_get_element(ids, i);
        if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING && !g_ascii_strcasecmp(json_node_get_string(node), id)) return TRUE;
    }
    return FALSE;
}
static JsonObject *group_for(JsonObject *root, const gchar *scope, const gchar *id)
{
    JsonArray *children = arr(root, "children");
    for (guint i = 0; i < length(children); ++i) {
        JsonObject *group = item(children, i); if (!group || g_strcmp0(radios_json_string(group, "scope", ""), scope)) continue;
        JsonArray *ids = arr(group, "ids");
        if (id ? ids_contain(ids, id) : !length(ids)) return group;
    }
    return NULL;
}
static gboolean has_rules(JsonObject *policy)
{ return policy && (json_object_has_member(policy, "transitions") || json_object_has_member(policy, "jingles") || length(arr(policy, "children"))); }
static gdouble scope_total(JsonObject *root, const gchar *scope)
{
    gdouble total = 0; JsonArray *children = arr(root, "children");
    for (guint i = 0; i < length(children); ++i) {
        JsonObject *group = item(children, i);
        if (group && !g_strcmp0(radios_json_string(group, "scope", ""), scope) && probability_defined(group)) total += numeric(group, "probability", 0);
    }
    return total;
}
static gdouble rules_total(JsonArray *rules)
{
    gdouble total = 0;
    for (guint i = 0; i < length(rules); ++i) if (probability_defined(item(rules, i))) total += numeric(item(rules, i), "probability", 0);
    return total;
}
static gboolean policy_empty(JsonObject *root)
{ return !root || (!length(arr(root, "children")) && !json_object_has_member(root, "transitions") && !json_object_has_member(root, "jingles")); }
static const gchar *level_scope(ProbabilityEditor *editor)
{ return gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->level)); }
static gboolean commit(ProbabilityEditor *editor, JsonNode *candidate, const gchar *done)
{
    GError *error = NULL;
    if (editor->closed || !identity_current(editor) || !radios_automation_policy_validate(json_node_get_object(candidate), &error)) {
        if (error) notice(editor, error->message);
        g_clear_error(&error); json_node_free(candidate); render(editor); return FALSE;
    }
    json_node_free(editor->policy); editor->policy = candidate; render(editor);
    notice(editor, done ? done : "Değişiklik kaydediliyor.");
    if (editor->hooks.changed) {
        /* Nothing left at this level: the owner removes the policy, so legacy
         * weights and linear order apply again instead of an empty policy. */
        JsonNode *snapshot = policy_empty(root_policy(editor)) ? json_node_new(JSON_NODE_NULL) : clone(editor->policy);
        editor->hooks.changed(editor->root, snapshot, editor->hooks.user); json_node_free(snapshot);
    }
    return TRUE;
}
/* Station-local excluded times */
static gint zone_offset(const gchar *name)
{
    GTimeZone *zone = name && *name ? radios_time_zone_new(name) : NULL; if (!zone) zone = g_time_zone_new_utc();
    GDateTime *now = g_date_time_new_now(zone); gint offset = (gint)(g_date_time_get_utc_offset(now) / G_TIME_SPAN_MINUTE);
    g_date_time_unref(now); g_time_zone_unref(zone); return offset;
}
static gchar *exclusions_text(ProbabilityEditor *editor, JsonObject *rule)
{
    GString *text = g_string_new(NULL); JsonArray *times = arr(rule, "excluded_times");
    /* Rules saved before station time existed hold UTC windows. */
    gint shift = zone_offset(editor->zone) - zone_offset(radios_json_string(rule, "timezone", "UTC"));
    for (guint i = 0; i < length(times); ++i) {
        JsonObject *window = item(times, i); if (!window) continue;
        gint start = (((gint)radios_json_int(window, "start_minute", 0) + shift) % 1440 + 1440) % 1440;
        gint end = (((gint)radios_json_int(window, "end_minute", 0) + shift) % 1440 + 1440) % 1440;
        g_string_append_printf(text, "%s%02d:%02d-%02d:%02d", text->len ? ", " : "", start / 60, start % 60, end / 60, end % 60);
    }
    return g_string_free(text, FALSE);
}
static gboolean clock_parse(const gchar *text, gint *minute)
{
    gint hour = 0, min = 0; gchar extra = 0;
    if (sscanf(text, "%d:%d%c", &hour, &min, &extra) != 2 || hour < 0 || hour > 23 || min < 0 || min > 59 || !g_ascii_isdigit(text[0])) return FALSE;
    *minute = hour * 60 + min; return TRUE;
}
static JsonArray *exclusions_parse(const gchar *text)
{
    JsonArray *times = json_array_new(); gchar *clean = g_strstrip(g_strdup(text ? text : ""));
    if (!*clean) { g_free(clean); return times; }
    gchar **parts = g_strsplit(clean, ",", -1); gboolean valid = g_strv_length(parts) <= 16;
    for (guint i = 0; parts[i] && valid; ++i) {
        gchar **bounds = g_strsplit(g_strstrip(parts[i]), "-", -1); gint start = 0, end = 0;
        valid = g_strv_length(bounds) == 2 && clock_parse(g_strstrip(bounds[0]), &start) && clock_parse(g_strstrip(bounds[1]), &end);
        if (valid) {
            JsonObject *window = json_object_new(); json_object_set_int_member(window, "start_minute", start);
            json_object_set_int_member(window, "end_minute", end); json_array_add_object_element(times, window);
        }
        g_strfreev(bounds);
    }
    g_strfreev(parts); g_free(clean);
    if (!valid) { json_array_unref(times); return NULL; }
    return times;
}
static gboolean excluded_apply(ProbabilityEditor *editor, JsonObject *rule, const gchar *text)
{
    JsonArray *times = exclusions_parse(text);
    if (!times) { notice(editor, "İstenmeyen saatleri SS:DD-SS:DD biçiminde, virgülle ayırarak yazın (en çok 16 aralık)."); return FALSE; }
    json_object_set_array_member(rule, "excluded_times", times); json_object_set_string_member(rule, "timezone", editor->zone);
    return TRUE;
}
static gboolean field_apply(ProbabilityEditor *editor, JsonObject *rule, const gchar *key, const gchar *text,
    gdouble low, gdouble high, gboolean integer, gdouble scale, const gchar *message)
{
    gboolean empty = FALSE; gdouble value = 0;
    if (!value_parse(text, low, high, integer, &empty, &value) || empty) { notice(editor, message); return FALSE; }
    if (integer || scale != 1) json_object_set_int_member(rule, key, (gint64)llround(value * scale));
    else json_object_set_double_member(rule, key, value);
    return TRUE;
}
static gboolean percent_apply(ProbabilityEditor *editor, JsonObject *rule, const gchar *text, JsonArray *siblings)
{
    gboolean empty = FALSE; gdouble value = 0;
    if (!value_parse(text, 0, 100, FALSE, &empty, &value)) { notice(editor, "Olasılık 0 ile 100 arasında bir yüzde olmalı; boş bırakılan kalan paydan pay alır."); return FALSE; }
    if (empty) json_object_remove_member(rule, "probability"); else json_object_set_double_member(rule, "probability", value);
    gdouble total = rules_total(siblings);
    if (total > 100.000000001) {
        gchar *sum = number_text(total), *message = g_strdup_printf("Toplam %%%s olur; bir tabloda en çok %%100 verilebilir.", sum);
        notice(editor, message); g_free(message); g_free(sum); return FALSE;
    }
    return TRUE;
}
/* Rows */
static void select_id(GtkWidget *tree, const gchar *id)
{
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); GtkTreeIter iter;
    for (gboolean more = id && gtk_tree_model_get_iter_first(model, &iter); more; more = gtk_tree_model_iter_next(model, &iter)) {
        gchar *row = NULL; gtk_tree_model_get(model, &iter, 0, &row, -1); gboolean match = !g_strcmp0(row, id); g_free(row);
        if (match) { gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), &iter); return; }
    }
}
static gchar *selected_id(GtkWidget *tree)
{
    GtkTreeModel *model = NULL; GtkTreeIter iter; gchar *id = NULL;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), &model, &iter)) gtk_tree_model_get(model, &iter, 0, &id, -1);
    return id;
}
static JsonObject *row_rule(ProbabilityEditor *editor, GtkListStore *store, const gchar *key, const gchar *path, JsonNode **candidate)
{
    GtkTreeIter iter; gchar *id = NULL; *candidate = NULL;
    if (editor->updating || editor->closed || !identity_current(editor) || !gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &iter, path)) return NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, 0, &id, -1);
    *candidate = clone(editor->policy); JsonArray *rules = arr(json_node_get_object(*candidate), key); JsonObject *rule = NULL;
    for (guint i = 0; !rule && i < length(rules); ++i) if (item(rules, i) && !g_strcmp0(radios_json_string(item(rules, i), "id", ""), id)) rule = item(rules, i);
    g_free(editor->focus); editor->focus = id;
    if (!rule) g_clear_pointer(candidate, json_node_free);
    return rule;
}
/* 1 · Neyin çalacağı */
static void choice_row(ProbabilityEditor *editor, const gchar *id, const gchar *name, JsonObject *group)
{
    gboolean defined = probability_defined(group); guint members = length(arr(group, "ids"));
    gchar *percent = defined ? number_text(numeric(group, "probability", 0)) : g_strdup("");
    GString *note = g_string_new(NULL);
    if (!defined) g_string_append(note, "kalan pay");
    else if (numeric(group, "probability", 0) <= 0) g_string_append(note, "kapalı");
    if (members > 1) g_string_append_printf(note, "%s%u öğe birlikte", note->len ? " · " : "", members);
    if (has_rules(obj(group, "policy"))) g_string_append_printf(note, "%sözel kuralı var", note->len ? " · " : "");
    GtkTreeIter iter; gtk_list_store_insert_with_values(editor->choice_store, &iter, -1, CHOICE_ID, id, CHOICE_NAME, name,
        CHOICE_PERCENT, percent, CHOICE_NOTE, note->str, CHOICE_REST, FALSE, -1);
    g_free(percent); g_string_free(note, TRUE);
}
static void choices_render(ProbabilityEditor *editor)
{
    gtk_list_store_clear(editor->choice_store); const gchar *scope = level_scope(editor); JsonObject *root = root_policy(editor);
    if (!scope) { gtk_label_set_text(GTK_LABEL(editor->choice_summary), "Bu ayarda olasılık verilecek playlist ya da şarkı yok."); return; }
    GHashTable *shown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL); guint unclaimed = 0;
    for (guint i = 0; i < length(editor->catalogue); ++i) {
        JsonObject *target = item(editor->catalogue, i); const gchar *id = radios_json_string(target, "id", "");
        if (!target || g_strcmp0(radios_json_string(target, "scope", ""), scope) || !target_listed(target) || !g_uuid_string_is_valid(id)) continue;
        gchar *key = g_ascii_strdown(id, -1); if (g_hash_table_contains(shown, key)) { g_free(key); continue; }
        g_hash_table_add(shown, key); JsonObject *group = group_for(root, scope, id); if (!group) ++unclaimed;
        choice_row(editor, id, radios_json_string(target, "name", id), group);
    }
    JsonArray *children = arr(root, "children");
    for (guint i = 0; i < length(children); ++i) {
        JsonObject *group = item(children, i); if (!group || g_strcmp0(radios_json_string(group, "scope", ""), scope)) continue;
        JsonArray *ids = arr(group, "ids");
        for (guint j = 0; j < length(ids); ++j) {
            JsonNode *node = json_array_get_element(ids, j);
            if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) continue;
            const gchar *id = json_node_get_string(node); gchar *key = g_ascii_strdown(id, -1);
            if (g_hash_table_contains(shown, key)) { g_free(key); continue; }
            g_hash_table_add(shown, key); JsonObject *target = target_by_id(editor, id);
            choice_row(editor, id, target ? radios_json_string(target, "name", id) : "Kayıtlı öğe (bu listede yok)", group);
        }
    }
    g_hash_table_unref(shown);
    JsonObject *rest = group_for(root, scope, NULL); gboolean defined = probability_defined(rest);
    gchar *percent = defined ? number_text(numeric(rest, "probability", 0)) : g_strdup("");
    gchar *note = unclaimed ? g_strdup_printf("%u öğe, sırayla", unclaimed) : g_strdup("yüzdesi olmayan öğe yok");
    GtkTreeIter iter; gtk_list_store_insert_with_values(editor->choice_store, &iter, -1, CHOICE_ID, "", CHOICE_NAME, "Kalanlar (yüzde verilmeyenler)",
        CHOICE_PERCENT, percent, CHOICE_NOTE, note, CHOICE_REST, TRUE, -1);
    gdouble total = scope_total(root, scope); gchar *sum = number_text(total), *left = number_text(MAX(0, 100 - total));
    GString *summary = g_string_new(NULL); g_string_append_printf(summary, "Tanımlı toplam %%%s. ", sum);
    if (defined) g_string_append_printf(summary, "Kalanlar %%%s ile sırayla çalar.", percent);
    else if (total < 100 - 1e-9) g_string_append_printf(summary, "Kalan %%%s, yüzde verilmeyen %u öğe arasında sırayla paylaşılır.", left, unclaimed);
    else if (unclaimed) g_string_append(summary, "Toplam %100: yüzde verilmeyen öğeler yalnız diğerleri uygun değilken çalar.");
    gtk_label_set_text(GTK_LABEL(editor->choice_summary), summary->str);
    g_string_free(summary, TRUE); g_free(sum); g_free(left); g_free(percent); g_free(note);
}
static void choice_apply(JsonObject *root, const gchar *scope, const gchar *id, gboolean empty, gdouble value)
{
    JsonObject *group = group_for(root, scope, id); JsonArray *children = arr(root, "children");
    if (empty) {
        if (!group) return;
        JsonArray *ids = arr(group, "ids");
        if (id && length(ids) > 1) {
            for (guint k = 0; k < length(ids); ++k) {
                JsonNode *node = json_array_get_element(ids, k);
                if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING && !g_ascii_strcasecmp(json_node_get_string(node), id)) {
                    json_array_remove_element(ids, k); return;
                }
            }
            return;
        }
        /* A group carrying its own rules is kept; it only loses its share. */
        if (has_rules(obj(group, "policy"))) { json_object_remove_member(group, "probability"); return; }
        for (guint i = 0; i < length(children); ++i) if (item(children, i) == group) { json_array_remove_element(children, i); break; }
        if (!length(children)) json_object_remove_member(root, "children");
        return;
    }
    if (!group) {
        if (!children) { children = json_array_new(); json_object_set_array_member(root, "children", children); }
        group = json_object_new(); gchar *uuid = g_uuid_string_random(); json_object_set_string_member(group, "id", uuid); g_free(uuid);
        json_object_set_string_member(group, "scope", scope); JsonArray *ids = json_array_new();
        if (id) json_array_add_string_element(ids, id);
        json_object_set_array_member(group, "ids", ids); JsonObject *child = json_object_new(); json_object_set_int_member(child, "schema", 1);
        json_object_set_object_member(group, "policy", child); json_array_add_object_element(children, group);
    }
    json_object_set_double_member(group, "probability", value);
}
static void choice_edited(GtkCellRendererText *cell, gchar *path, gchar *text, gpointer data)
{
    (void)cell; ProbabilityEditor *editor = data; const gchar *scope = level_scope(editor);
    GtkTreeModel *model = GTK_TREE_MODEL(editor->choice_store); GtkTreeIter iter; gboolean empty = FALSE; gdouble value = 0;
    if (editor->updating || editor->closed || !scope || !identity_current(editor) || !gtk_tree_model_get_iter_from_string(model, &iter, path)) return;
    if (!value_parse(text, 0, 100, FALSE, &empty, &value)) { notice(editor, "Olasılık 0 ile 100 arasında bir yüzde olmalı; boş bırakılan kalan paydan pay alır."); return; }
    gchar *id = NULL; gboolean rest = FALSE; gtk_tree_model_get(model, &iter, CHOICE_ID, &id, CHOICE_REST, &rest, -1);
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free); GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->choices));
    /* Typing into one of several selected rows gives each of them that share. */
    if (!rest && gtk_tree_selection_iter_is_selected(selection, &iter) && gtk_tree_selection_count_selected_rows(selection) > 1) {
        GList *rows = gtk_tree_selection_get_selected_rows(selection, NULL);
        for (GList *p = rows; p; p = p->next) {
            GtkTreeIter row; gchar *row_id = NULL; gboolean row_rest = FALSE;
            if (!gtk_tree_model_get_iter(model, &row, p->data)) continue;
            gtk_tree_model_get(model, &row, CHOICE_ID, &row_id, CHOICE_REST, &row_rest, -1);
            if (!row_rest && row_id && *row_id) g_ptr_array_add(ids, row_id); else g_free(row_id);
        }
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    } else if (!rest) g_ptr_array_add(ids, g_strdup(id));
    JsonNode *candidate = clone(editor->policy); JsonObject *root = json_node_get_object(candidate);
    if (rest) choice_apply(root, scope, NULL, empty, value);
    for (guint i = 0; i < ids->len; ++i) choice_apply(root, scope, g_ptr_array_index(ids, i), empty, value);
    gdouble total = scope_total(root, scope);
    if (total > 100.000000001) {
        gchar *sum = number_text(total), *message = g_strdup_printf("Toplam %%%s olur; bir listede en çok %%100 verilebilir.", sum);
        notice(editor, message); g_free(message); g_free(sum); json_node_free(candidate);
    } else {
        JsonObject *group = rest || empty ? NULL : group_for(root, scope, id);
        gboolean joint = group && length(arr(group, "ids")) > 1;
        commit(editor, candidate, joint ? "Bu öğe birlikte seçilen bir grupta; yüzde grubun tamamına uygulandı." : NULL);
    }
    g_free(id); g_ptr_array_unref(ids);
}
/* 2 · Geçiş */
static void transitions_render(ProbabilityEditor *editor)
{
    gtk_list_store_clear(editor->transition_store); JsonObject *root = root_policy(editor); JsonArray *rules = arr(root, "transitions");
    guint undefined = 0;
    for (guint i = 0; i < length(rules); ++i) {
        JsonObject *rule = item(rules, i); if (!rule) continue;
        gint mode = (gint)CLAMP(radios_json_int(rule, "mode", 0), 0, 3); gboolean defined = probability_defined(rule);
        if (!defined) ++undefined;
        gchar *percent = defined ? number_text(numeric(rule, "probability", 0)) : g_strdup("");
        gchar *overlap = mode ? number_text(numeric(rule, "overlap_ms", 0) / 1000.0) : g_strdup("");
        gchar *early = number_text(numeric(rule, "early_seconds", 0));
        gchar *frequency = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(rule, "frequency", 1));
        gchar *interval = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(rule, "interval_minutes", 0)), *excluded = exclusions_text(editor, rule);
        GtkTreeIter iter; gtk_list_store_insert_with_values(editor->transition_store, &iter, -1, TR_ID, radios_json_string(rule, "id", ""),
            TR_MODE, MODES[mode], TR_PERCENT, percent, TR_EFFECT, !g_strcmp0(radios_json_string(rule, "effect", "none"), "lowpass"),
            TR_SHAPED, mode != 0, TR_OVERLAP, overlap, TR_EARLY, early, TR_FREQUENCY, frequency, TR_INTERVAL, interval, TR_EXCLUDED, excluded, -1);
        g_free(percent); g_free(overlap); g_free(early); g_free(frequency); g_free(interval); g_free(excluded);
    }
    gchar *summary = NULL;
    if (!json_object_has_member(root, "transitions"))
        summary = g_strdup("Burada geçiş tanımlı değil: üst düzeyde tanımlı geçiş olasılıkları, o da yoksa normal geçiş kuralları uygulanır.");
    else if (!length(rules)) summary = g_strdup("Bu düzeyde olasılıklı geçiş kapalı: normal geçiş kuralları uygulanır.");
    else {
        gdouble total = rules_total(rules); gchar *sum = number_text(total), *left = number_text(MAX(0, 100 - total));
        summary = g_strdup_printf("Tanımlı toplam %%%s. Kalan %%%s: %s", sum, left,
            undefined ? "yüzdesi boş geçişler sırayla uygulanır." : "normal geçiş kuralları uygulanır.");
        g_free(sum); g_free(left);
    }
    gtk_label_set_text(GTK_LABEL(editor->transition_summary), summary); g_free(summary);
}
static void transition_edited(GtkCellRendererText *cell, gchar *path, gchar *text, gpointer data)
{
    ProbabilityEditor *editor = data; gint column = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "column"));
    JsonNode *candidate = NULL; JsonObject *rule = row_rule(editor, editor->transition_store, "transitions", path, &candidate);
    if (!rule) return;
    gboolean okay = FALSE; gint mode = (gint)radios_json_int(rule, "mode", 0);
    if (column == TR_PERCENT) okay = percent_apply(editor, rule, text, arr(json_node_get_object(candidate), "transitions"));
    else if (column == TR_OVERLAP) {
        gboolean shaped = mode == 3 || !g_strcmp0(radios_json_string(rule, "effect", "none"), "lowpass");
        okay = mode && field_apply(editor, rule, "overlap_ms", text, shaped ? 0.1 : 0, 30, FALSE, 1000,
            shaped ? "SYNC beatmatch ve efektli inmede bindirme 0,1–30 sn olmalı." : "Bindirme 0–30 sn olmalı.");
    }
    else if (column == TR_EARLY) okay = field_apply(editor, rule, "early_seconds", text, 0, 3600, FALSE, 1, "Erken geçiş 0–3600 sn olmalı.");
    else if (column == TR_FREQUENCY) okay = field_apply(editor, rule, "frequency", text, 1, 1000, TRUE, 1, "Sıklık 1–1000 arasında tam sayı olmalı (1 = her geçişte).");
    else if (column == TR_INTERVAL) okay = field_apply(editor, rule, "interval_minutes", text, 0, 10080, TRUE, 1, "En az ara 0–10080 dk olmalı.");
    else if (column == TR_EXCLUDED) okay = excluded_apply(editor, rule, text);
    if (okay) commit(editor, candidate, NULL); else { json_node_free(candidate); render(editor); }
}
static void transition_mode_changed(GtkCellRendererCombo *combo, gchar *path, GtkTreeIter *option, gpointer data)
{
    (void)combo; ProbabilityEditor *editor = data; JsonNode *candidate = NULL;
    JsonObject *rule = row_rule(editor, editor->transition_store, "transitions", path, &candidate); if (!rule) return;
    gchar *id = NULL; gtk_tree_model_get(GTK_TREE_MODEL(editor->modes), option, OPTION_ID, &id, -1);
    gint mode = id ? CLAMP(atoi(id), 0, 3) : 0; g_free(id); json_object_set_int_member(rule, "mode", mode);
    if (!mode) { json_object_set_int_member(rule, "overlap_ms", 0); json_object_set_string_member(rule, "effect", "none"); }
    else if (radios_json_int(rule, "overlap_ms", 0) <= 0) json_object_set_int_member(rule, "overlap_ms", 3000);
    commit(editor, candidate, NULL);
}
static void transition_effect_toggled(GtkCellRendererToggle *toggle, gchar *path, gpointer data)
{
    (void)toggle; ProbabilityEditor *editor = data; JsonNode *candidate = NULL;
    JsonObject *rule = row_rule(editor, editor->transition_store, "transitions", path, &candidate); if (!rule) return;
    if (!radios_json_int(rule, "mode", 0)) { json_node_free(candidate); notice(editor, "Parça sonunda geçişte efekt kullanılmaz; önce geçiş türünü değiştirin."); return; }
    gboolean lowpass = g_strcmp0(radios_json_string(rule, "effect", "none"), "lowpass") != 0;
    json_object_set_string_member(rule, "effect", lowpass ? "lowpass" : "none");
    if (lowpass && radios_json_int(rule, "overlap_ms", 0) <= 0) json_object_set_int_member(rule, "overlap_ms", 3000);
    commit(editor, candidate, NULL);
}
static void rule_remove(ProbabilityEditor *editor, GtkWidget *tree, const gchar *key)
{
    if (editor->closed || !identity_current(editor)) return;
    gchar *id = selected_id(tree); if (!id) { notice(editor, "Silmek için listeden bir kural seçin."); return; }
    JsonNode *candidate = clone(editor->policy); JsonObject *root = json_node_get_object(candidate); JsonArray *rules = arr(root, key);
    for (guint i = 0; i < length(rules); ++i) if (item(rules, i) && !g_strcmp0(radios_json_string(item(rules, i), "id", ""), id)) { json_array_remove_element(rules, i); break; }
    /* The last row gone: this level inherits again instead of disabling. */
    if (!length(rules)) json_object_remove_member(root, key);
    g_free(id); commit(editor, candidate, NULL);
}
static void transition_add(GtkButton *button, gpointer data)
{
    (void)button; ProbabilityEditor *editor = data; if (editor->closed || !identity_current(editor)) return;
    JsonNode *candidate = clone(editor->policy); JsonObject *root = json_node_get_object(candidate); JsonArray *rules = arr(root, "transitions");
    if (!rules) { rules = json_array_new(); json_object_set_array_member(root, "transitions", rules); }
    if (length(rules) >= RADIOS_AUTOMATION_POLICY_MAX_CHOICES) { json_node_free(candidate); notice(editor, "Bir düzeyde en çok 16 geçiş tanımlanabilir."); return; }
    JsonObject *rule = json_object_new(); gchar *id = g_uuid_string_random(); json_object_set_string_member(rule, "id", id);
    json_object_set_int_member(rule, "mode", 1); json_object_set_int_member(rule, "overlap_ms", 3000); json_object_set_string_member(rule, "effect", "none");
    json_object_set_double_member(rule, "early_seconds", 0); json_object_set_int_member(rule, "frequency", 1); json_object_set_int_member(rule, "interval_minutes", 0);
    json_object_set_array_member(rule, "excluded_times", json_array_new()); json_array_add_object_element(rules, rule);
    if (!transition_form_apply(editor, rule, rules)) { g_free(id); json_node_free(candidate); return; }
    g_free(editor->transition_selected); editor->transition_selected = g_strdup(id);
    g_free(editor->focus); editor->focus = id;
    commit(editor, candidate, "Geçiş eklendi.");
}
static void transition_remove(GtkButton *button, gpointer data)
{ (void)button; ProbabilityEditor *editor = data; rule_remove(editor, editor->transitions, "transitions"); }
/* 3 · Şarkı içinde jingle */
static const gchar *option_name(GtkListStore *store, const gchar *id, gchar **owned)
{
    GtkTreeIter iter; *owned = NULL;
    for (gboolean more = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter); more; more = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter)) {
        gchar *row = NULL, *name = NULL; gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, OPTION_ID, &row, OPTION_NAME, &name, -1);
        gboolean match = !g_strcmp0(row, id); g_free(row);
        if (match) { *owned = name; return name; }
        g_free(name);
    }
    return id;
}
static gboolean option_known(GtkListStore *store, const gchar *id)
{ gchar *name = NULL; option_name(store, id, &name); gboolean known = name != NULL; g_free(name); return known; }
static void jingle_options_render(ProbabilityEditor *editor)
{
    JsonArray *rules = arr(root_policy(editor), "jingles");
    gchar *playlist = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->jingle_playlist)));
    gchar *player = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->jingle_player)));
    gtk_list_store_clear(editor->playlists); gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(editor->jingle_playlist));
    for (guint i = 0; i < length(editor->catalogue); ++i) {
        JsonObject *target = item(editor->catalogue, i); const gchar *id = radios_json_string(target, "id", ""), *purpose = radios_json_string(target, "purpose", "");
        if (!target || !g_uuid_string_is_valid(id) || g_strcmp0(radios_json_string(target, "scope", ""), "playlist") || (*purpose && g_strcmp0(purpose, "jingle"))) continue;
        if (option_known(editor->playlists, id)) continue;
        gtk_list_store_insert_with_values(editor->playlists, NULL, -1, OPTION_ID, id, OPTION_NAME, radios_json_string(target, "name", id), -1);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->jingle_playlist), id, radios_json_string(target, "name", id));
    }
    gtk_list_store_clear(editor->players); gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(editor->jingle_player));
    for (guint i = 0; editor->ctx->parent && i < 32; ++i) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", i); GtkWidget *card = g_object_get_data(G_OBJECT(editor->ctx->parent), key); g_free(key);
        if (!GTK_IS_WIDGET(card) || !gtk_widget_get_visible(card)) continue;
        gchar *id = g_strdup_printf("%u", i), *name = g_strdup_printf("Player %u", i + 1);
        gtk_list_store_insert_with_values(editor->players, NULL, -1, OPTION_ID, id, OPTION_NAME, name, -1);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->jingle_player), id, name); g_free(id); g_free(name);
    }
    /* Saved choices stay visible even when not available right now. */
    for (guint i = 0; i < length(rules); ++i) {
        JsonObject *rule = item(rules, i); const gchar *id = radios_json_string(rule, "playlist_id", "");
        if (rule && *id && !option_known(editor->playlists, id)) {
            gtk_list_store_insert_with_values(editor->playlists, NULL, -1, OPTION_ID, id, OPTION_NAME, "Kayıtlı playlist (bulunamadı)", -1);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->jingle_playlist), id, "Kayıtlı playlist (bulunamadı)");
        }
        gchar *deck = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(rule, "player", 0));
        if (rule && !option_known(editor->players, deck)) {
            gchar *name = g_strdup_printf("Player %" G_GINT64_FORMAT " (şu an görünmüyor)", radios_json_int(rule, "player", 0) + 1);
            gtk_list_store_insert_with_values(editor->players, NULL, -1, OPTION_ID, deck, OPTION_NAME, name, -1);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->jingle_player), deck, name); g_free(name);
        }
        g_free(deck);
    }
    if (playlist) gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->jingle_playlist), playlist);
    if (player) gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->jingle_player), player);
    g_free(playlist); g_free(player);
}
static void jingle_add_sensitivity(ProbabilityEditor *editor)
{
    gtk_widget_set_sensitive(editor->jingle_add, gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->jingle_playlist)) &&
        gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->jingle_player)));
    if (editor->jingle_save) gtk_widget_set_sensitive(editor->jingle_save, editor->jingle_selected &&
        gtk_widget_get_sensitive(editor->jingle_add));
}
static void jingles_render(ProbabilityEditor *editor)
{
    jingle_options_render(editor); gtk_list_store_clear(editor->jingle_store); JsonArray *rules = arr(root_policy(editor), "jingles"); guint undefined = 0;
    for (guint i = 0; i < length(rules); ++i) {
        JsonObject *rule = item(rules, i); if (!rule) continue;
        gboolean defined = probability_defined(rule); if (!defined) ++undefined;
        gchar *playlist_owned = NULL, *player_owned = NULL, *deck = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(rule, "player", 0));
        const gchar *playlist = option_name(editor->playlists, radios_json_string(rule, "playlist_id", ""), &playlist_owned), *player = option_name(editor->players, deck, &player_owned);
        gchar *seconds = number_text(numeric(rule, "seconds", 0)), *percent = defined ? number_text(numeric(rule, "probability", 0)) : g_strdup("");
        gchar *duck = number_text(numeric(rule, "duck_db", -12)), *frequency = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(rule, "frequency", 1));
        gchar *interval = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(rule, "interval_minutes", 0)), *excluded = exclusions_text(editor, rule);
        GtkTreeIter iter; gtk_list_store_insert_with_values(editor->jingle_store, &iter, -1, JG_ID, radios_json_string(rule, "id", ""),
            JG_PLAYLIST, playlist, JG_PLAYER, player, JG_ANCHOR, !g_strcmp0(radios_json_string(rule, "anchor", "after_start"), "before_end") ? "Bitişten önce" : "Başlangıçtan sonra",
            JG_SECONDS, seconds, JG_PERCENT, percent, JG_DUCK, duck, JG_FREQUENCY, frequency, JG_INTERVAL, interval, JG_EXCLUDED, excluded, -1);
        g_free(playlist_owned); g_free(player_owned); g_free(deck); g_free(seconds); g_free(percent); g_free(duck); g_free(frequency); g_free(interval); g_free(excluded);
    }
    gchar *summary = NULL;
    if (!length(rules)) summary = g_strdup("Şarkı içinde jingle tanımlı değil.");
    else {
        gdouble total = rules_total(rules); gchar *sum = number_text(total);
        summary = g_strdup_printf("Tanımlı toplam %%%s.%s Jingle, onaylı intro/outro alanına tam sığmazsa veya ilgili işaret yoksa o şarkıda atlanır; müzik çalan player seçilirse çalmaz.",
            sum, undefined ? " Yüzdesi boş jingle kalan payla her uygun şarkıda çalar." : "");
        g_free(sum);
    }
    gtk_label_set_text(GTK_LABEL(editor->jingle_summary), summary); g_free(summary); jingle_add_sensitivity(editor);
}
static void jingle_edited(GtkCellRendererText *cell, gchar *path, gchar *text, gpointer data)
{
    ProbabilityEditor *editor = data; gint column = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "column"));
    JsonNode *candidate = NULL; JsonObject *rule = row_rule(editor, editor->jingle_store, "jingles", path, &candidate);
    if (!rule) return;
    gboolean okay = FALSE;
    if (column == JG_PERCENT) okay = percent_apply(editor, rule, text, arr(json_node_get_object(candidate), "jingles"));
    else if (column == JG_SECONDS) okay = field_apply(editor, rule, "seconds", text, 0, 86400, FALSE, 1, "Saniye 0 ya da daha büyük olmalı.");
    else if (column == JG_DUCK) okay = field_apply(editor, rule, "duck_db", text, -60, 0, FALSE, 1, "Müzik kısma -60 ile 0 dB arasında olmalı.");
    else if (column == JG_FREQUENCY) okay = field_apply(editor, rule, "frequency", text, 1, 1000, TRUE, 1, "Sıklık 1–1000 arasında tam sayı olmalı (1 = her şarkıda).");
    else if (column == JG_INTERVAL) okay = field_apply(editor, rule, "interval_minutes", text, 0, 10080, TRUE, 1, "En az ara 0–10080 dk olmalı.");
    else if (column == JG_EXCLUDED) okay = excluded_apply(editor, rule, text);
    if (okay) commit(editor, candidate, NULL); else { json_node_free(candidate); render(editor); }
}
static void jingle_option_changed(GtkCellRendererCombo *combo, gchar *path, GtkTreeIter *option, gpointer data)
{
    ProbabilityEditor *editor = data; gint column = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(combo), "column"));
    JsonNode *candidate = NULL; JsonObject *rule = row_rule(editor, editor->jingle_store, "jingles", path, &candidate); if (!rule) return;
    GtkTreeModel *model = column == JG_PLAYLIST ? GTK_TREE_MODEL(editor->playlists) : column == JG_PLAYER ? GTK_TREE_MODEL(editor->players) : GTK_TREE_MODEL(editor->anchors);
    gchar *id = NULL; gtk_tree_model_get(model, option, OPTION_ID, &id, -1);
    if (!id) { json_node_free(candidate); return; }
    if (column == JG_PLAYLIST) json_object_set_string_member(rule, "playlist_id", id);
    else if (column == JG_PLAYER) json_object_set_int_member(rule, "player", g_ascii_strtoll(id, NULL, 10));
    else json_object_set_string_member(rule, "anchor", id);
    g_free(id); commit(editor, candidate, NULL);
}
static void jingle_add(GtkButton *button, gpointer data)
{
    (void)button; ProbabilityEditor *editor = data; if (editor->closed || !identity_current(editor)) return;
    /* Both are explicit: never borrow the first or an occupied player. */
    const gchar *playlist = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->jingle_playlist)), *player = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->jingle_player));
    if (!playlist || !g_uuid_string_is_valid(playlist) || !player) { notice(editor, "Jingle eklemek için önce jingle playlistini ve boş bir playerı seçin."); return; }
    JsonNode *candidate = clone(editor->policy); JsonObject *root = json_node_get_object(candidate); JsonArray *rules = arr(root, "jingles");
    if (!rules) { rules = json_array_new(); json_object_set_array_member(root, "jingles", rules); }
    if (length(rules) >= RADIOS_AUTOMATION_POLICY_MAX_CHOICES) { json_node_free(candidate); notice(editor, "Bir düzeyde en çok 16 jingle kuralı tanımlanabilir."); return; }
    JsonObject *rule = json_object_new(); gchar *id = g_uuid_string_random(); json_object_set_string_member(rule, "id", id);
    json_object_set_string_member(rule, "playlist_id", playlist); json_object_set_int_member(rule, "player", g_ascii_strtoll(player, NULL, 10));
    json_object_set_string_member(rule, "anchor", "after_start"); json_object_set_double_member(rule, "seconds", 10);
    json_object_set_double_member(rule, "duck_db", -12); json_object_set_double_member(rule, "gain_db", 0); json_object_set_int_member(rule, "fade_ms", 100);
    json_object_set_int_member(rule, "frequency", 1); json_object_set_int_member(rule, "interval_minutes", 0); json_object_set_array_member(rule, "excluded_times", json_array_new());
    json_array_add_object_element(rules, rule);
    if (!jingle_form_apply(editor, rule, rules)) { g_free(id); json_node_free(candidate); return; }
    g_free(editor->jingle_selected); editor->jingle_selected = g_strdup(id);
    g_free(editor->focus); editor->focus = id;
    commit(editor, candidate, "Jingle eklendi.");
}
static void jingle_remove(GtkButton *button, gpointer data)
{ (void)button; ProbabilityEditor *editor = data; rule_remove(editor, editor->jingles, "jingles"); }
static void jingle_choice_changed(GtkComboBox *combo, gpointer data)
{ (void)combo; ProbabilityEditor *editor = data; if (!editor->updating) jingle_add_sensitivity(editor); }
/* The normal view: select a plain-language row, then use ordinary controls.
 * Forms patch a cloned rule, preserving every advanced and nested field. */
static JsonObject *rule_by_id(JsonObject *root, const gchar *key, const gchar *id)
{
    JsonArray *rules = arr(root, key);
    for (guint i = 0; id && i < length(rules); ++i)
        if (item(rules, i) && !g_strcmp0(radios_json_string(item(rules, i), "id", ""), id)) return item(rules, i);
    return NULL;
}
static void percent_load(GtkWidget *spin, GtkWidget *automatic, JsonObject *rule)
{
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(automatic), !probability_defined(rule));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), numeric(rule, "probability", 100));
    gtk_widget_set_sensitive(spin, probability_defined(rule));
}
static gboolean form_percent(ProbabilityEditor *editor, JsonObject *rule, JsonArray *siblings, GtkWidget *spin, GtkWidget *automatic)
{
    gtk_spin_button_update(GTK_SPIN_BUTTON(spin));
    gchar *text = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(automatic)) ? g_strdup("") : number_text(gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin)));
    gboolean valid = percent_apply(editor, rule, text, siblings); g_free(text); return valid;
}
static gboolean transition_form_apply(ProbabilityEditor *editor, JsonObject *rule, JsonArray *siblings)
{
    gint mode = 0;
    for (gint i = 0; i < 4; ++i) if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->transition_modes[i]))) mode = i;
    gtk_spin_button_update(GTK_SPIN_BUTTON(editor->transition_overlap));
    gtk_spin_button_update(GTK_SPIN_BUTTON(editor->transition_early));
    gdouble overlap = mode ? gtk_spin_button_get_value(GTK_SPIN_BUTTON(editor->transition_overlap)) : 0;
    gboolean effect = mode && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->transition_effect));
    if ((mode == 3 || effect) && overlap < 0.1) { notice(editor, "SYNC beatmatch ve efektli inmede bindirme en az 0,1 saniye olmalı."); return FALSE; }
    json_object_set_int_member(rule, "mode", mode);
    json_object_set_int_member(rule, "overlap_ms", (gint64)llround(overlap * 1000));
    json_object_set_double_member(rule, "early_seconds", gtk_spin_button_get_value(GTK_SPIN_BUTTON(editor->transition_early)));
    json_object_set_string_member(rule, "effect", effect ? "lowpass" : "none");
    return form_percent(editor, rule, siblings, editor->transition_percent, editor->transition_auto);
}
static gboolean jingle_form_apply(ProbabilityEditor *editor, JsonObject *rule, JsonArray *siblings)
{
    const gchar *playlist = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->jingle_playlist));
    const gchar *player = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->jingle_player));
    const gchar *anchor = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->jingle_anchor));
    if (!playlist || !g_uuid_string_is_valid(playlist) || !player || !anchor) {
        notice(editor, "Jingle listesini, oynatıcıyı ve ne zaman çalacağını seçin."); return FALSE;
    }
    gtk_spin_button_update(GTK_SPIN_BUTTON(editor->jingle_seconds));
    gtk_spin_button_update(GTK_SPIN_BUTTON(editor->jingle_duck));
    json_object_set_string_member(rule, "playlist_id", playlist);
    json_object_set_int_member(rule, "player", g_ascii_strtoll(player, NULL, 10));
    json_object_set_string_member(rule, "anchor", anchor);
    json_object_set_double_member(rule, "seconds", gtk_spin_button_get_value(GTK_SPIN_BUTTON(editor->jingle_seconds)));
    json_object_set_double_member(rule, "duck_db", gtk_spin_button_get_value(GTK_SPIN_BUTTON(editor->jingle_duck)));
    return form_percent(editor, rule, siblings, editor->jingle_percent, editor->jingle_auto);
}
static void simple_save(GtkButton *button, gpointer data)
{
    ProbabilityEditor *editor = data;
    if (editor->closed || editor->updating || !identity_current(editor)) return;
    if (GTK_WIDGET(button) == editor->choice_save) {
        if (!editor->choice_selected) return;
        GtkTreeModel *model = GTK_TREE_MODEL(editor->choice_store); GtkTreeIter iter;
        for (gboolean more = gtk_tree_model_get_iter_first(model, &iter); more; more = gtk_tree_model_iter_next(model, &iter)) {
            gchar *id = NULL; gtk_tree_model_get(model, &iter, CHOICE_ID, &id, -1);
            gboolean match = !g_strcmp0(id, editor->choice_selected); g_free(id); if (!match) continue;
            gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->choices)));
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->choices)), &iter);
            GtkTreePath *path = gtk_tree_model_get_path(model, &iter); gchar *path_text = gtk_tree_path_to_string(path);
            gtk_spin_button_update(GTK_SPIN_BUTTON(editor->choice_percent));
            gchar *text = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->choice_auto)) ? g_strdup("") : number_text(gtk_spin_button_get_value(GTK_SPIN_BUTTON(editor->choice_percent)));
            choice_edited(NULL, path_text, text, editor); g_free(text); g_free(path_text); gtk_tree_path_free(path); return;
        }
        return;
    }
    gboolean transition = GTK_WIDGET(button) == editor->transition_save;
    const gchar *key = transition ? "transitions" : "jingles", *id = transition ? editor->transition_selected : editor->jingle_selected;
    JsonNode *candidate = clone(editor->policy); JsonObject *root = json_node_get_object(candidate), *rule = rule_by_id(root, key, id);
    if (!rule) { json_node_free(candidate); return; }
    gboolean valid = transition ? transition_form_apply(editor, rule, arr(root, key)) : jingle_form_apply(editor, rule, arr(root, key));
    if (valid) { g_free(editor->focus); editor->focus = g_strdup(id); commit(editor, candidate, "Kural güncellendi."); }
    else json_node_free(candidate);
}
static void automatic_toggled(GtkToggleButton *button, gpointer data)
{ gtk_widget_set_sensitive(GTK_WIDGET(data), !gtk_toggle_button_get_active(button)); }
static void mode_toggled(GtkToggleButton *button, gpointer data)
{
    (void)button; ProbabilityEditor *editor = data;
    gboolean shaped = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->transition_modes[0]));
    gtk_widget_set_sensitive(editor->transition_overlap, shaped); gtk_widget_set_sensitive(editor->transition_effect, shaped);
}
static void simple_selected(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    ProbabilityEditor *editor = data; if (editor->updating || editor->closed) return;
    const gchar *id = row ? g_object_get_data(G_OBJECT(row), "rule-id") : NULL;
    gchar **selected = GTK_WIDGET(list) == editor->choice_list ? &editor->choice_selected :
        GTK_WIDGET(list) == editor->transition_list ? &editor->transition_selected : &editor->jingle_selected;
    g_free(*selected); *selected = g_strdup(id);
    if (GTK_WIDGET(list) == editor->transition_list) select_id(editor->transitions, id);
    if (GTK_WIDGET(list) == editor->jingle_list) select_id(editor->jingles, id);
    simple_load(editor);
}
static void list_clear(GtkWidget *list)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *p = children; p; p = p->next) gtk_widget_destroy(p->data);
    g_list_free(children);
}
static void list_row(GtkWidget *list, const gchar *id, const gchar *title, const gchar *subtitle, const gchar *value, const gchar *selected)
{
    GtkWidget *row = gtk_list_box_row_new(), *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3), *name = gtk_label_new(title), *badge = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(name), 0); gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(labels), name, FALSE, FALSE, 0);
    if (subtitle && *subtitle) {
        GtkWidget *detail = wrapped(subtitle); gtk_label_set_line_wrap(GTK_LABEL(detail), FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END); gtk_box_pack_start(GTK_BOX(labels), detail, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(line), labels, TRUE, TRUE, 0); gtk_box_pack_end(GTK_BOX(line), badge, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row), line); gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
    g_object_set_data_full(G_OBJECT(row), "rule-id", g_strdup(id), g_free);
    gtk_style_context_add_class(gtk_widget_get_style_context(badge), "simple-badge");
    if (selected && !g_strcmp0(id, selected)) gtk_list_box_select_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(row));
}
static void simple_render(ProbabilityEditor *editor)
{
    list_clear(editor->choice_list); list_clear(editor->transition_list); list_clear(editor->jingle_list);
    GtkTreeIter iter; GtkTreeModel *model = GTK_TREE_MODEL(editor->choice_store);
    for (gboolean more = gtk_tree_model_get_iter_first(model, &iter); more; more = gtk_tree_model_iter_next(model, &iter)) {
        gchar *id = NULL, *name = NULL, *percent = NULL, *note = NULL;
        gtk_tree_model_get(model, &iter, CHOICE_ID, &id, CHOICE_NAME, &name, CHOICE_PERCENT, &percent, CHOICE_NOTE, &note, -1);
        gchar *value = percent && *percent ? g_strdup_printf("%%%s", percent) : g_strdup("Otomatik");
        list_row(editor->choice_list, id, name, note, value, editor->choice_selected);
        g_free(id); g_free(name); g_free(percent); g_free(note); g_free(value);
    }
    if (!gtk_list_box_get_selected_row(GTK_LIST_BOX(editor->choice_list))) {
        GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(editor->choice_list), 0);
        gtk_list_box_select_row(GTK_LIST_BOX(editor->choice_list), first); g_free(editor->choice_selected);
        editor->choice_selected = first ? g_strdup(g_object_get_data(G_OBJECT(first), "rule-id")) : NULL;
    }
    g_free(editor->transition_selected); editor->transition_selected = selected_id(editor->transitions);
    model = GTK_TREE_MODEL(editor->transition_store);
    for (gboolean more = gtk_tree_model_get_iter_first(model, &iter); more; more = gtk_tree_model_iter_next(model, &iter)) {
        gchar *id = NULL, *mode = NULL, *percent = NULL, *overlap = NULL, *early = NULL;
        gtk_tree_model_get(model, &iter, TR_ID, &id, TR_MODE, &mode, TR_PERCENT, &percent, TR_OVERLAP, &overlap, TR_EARLY, &early, -1);
        gchar *detail = overlap && *overlap ? g_strdup_printf("%s sn bindirme · %s sn erken", overlap, early) : g_strdup_printf("%s sn erken", early);
        gchar *value = percent && *percent ? g_strdup_printf("%%%s", percent) : g_strdup("Kalan pay");
        list_row(editor->transition_list, id, mode, detail, value, editor->transition_selected);
        g_free(id); g_free(mode); g_free(percent); g_free(overlap); g_free(early); g_free(detail); g_free(value);
    }
    g_free(editor->jingle_selected); editor->jingle_selected = selected_id(editor->jingles);
    model = GTK_TREE_MODEL(editor->jingle_store);
    for (gboolean more = gtk_tree_model_get_iter_first(model, &iter); more; more = gtk_tree_model_iter_next(model, &iter)) {
        gchar *id = NULL, *name = NULL, *player = NULL, *anchor = NULL, *seconds = NULL, *percent = NULL;
        gtk_tree_model_get(model, &iter, JG_ID, &id, JG_PLAYLIST, &name, JG_PLAYER, &player, JG_ANCHOR, &anchor, JG_SECONDS, &seconds, JG_PERCENT, &percent, -1);
        gchar *detail = g_strdup_printf("%s · %s %s sn", player, anchor, seconds);
        gchar *value = percent && *percent ? g_strdup_printf("%%%s", percent) : g_strdup("Kalan pay");
        list_row(editor->jingle_list, id, name, detail, value, editor->jingle_selected);
        g_free(id); g_free(name); g_free(player); g_free(anchor); g_free(seconds); g_free(percent); g_free(detail); g_free(value);
    }
    gtk_widget_show_all(editor->choice_list); gtk_widget_show_all(editor->transition_list); gtk_widget_show_all(editor->jingle_list);
}
static void simple_load(ProbabilityEditor *editor)
{
    JsonObject *root = root_policy(editor);
    JsonObject *choice = editor->choice_selected ? group_for(root, level_scope(editor), *editor->choice_selected ? editor->choice_selected : NULL) : NULL;
    percent_load(editor->choice_percent, editor->choice_auto, choice);
    gtk_widget_set_sensitive(editor->choice_save, editor->choice_selected != NULL);
    JsonObject *transition = rule_by_id(root, "transitions", editor->transition_selected);
    if (transition) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->transition_modes[CLAMP(radios_json_int(transition, "mode", 0), 0, 3)]), TRUE);
        percent_load(editor->transition_percent, editor->transition_auto, transition);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->transition_overlap), numeric(transition, "overlap_ms", 3000) / 1000.0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->transition_early), numeric(transition, "early_seconds", 0));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->transition_effect), !g_strcmp0(radios_json_string(transition, "effect", "none"), "lowpass"));
    }
    gtk_widget_set_sensitive(editor->transition_save, transition != NULL); gtk_widget_set_sensitive(editor->transition_remove, transition != NULL);
    mode_toggled(NULL, editor);
    JsonObject *jingle = rule_by_id(root, "jingles", editor->jingle_selected);
    if (jingle) {
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->jingle_playlist), radios_json_string(jingle, "playlist_id", ""));
        gchar *player = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(jingle, "player", 0));
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->jingle_player), player); g_free(player);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->jingle_anchor), radios_json_string(jingle, "anchor", "after_start"));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->jingle_seconds), numeric(jingle, "seconds", 10));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->jingle_duck), numeric(jingle, "duck_db", -12));
        percent_load(editor->jingle_percent, editor->jingle_auto, jingle);
    }
    gtk_widget_set_sensitive(editor->jingle_remove, jingle != NULL); jingle_add_sensitivity(editor);
}
/* Layout */
static void levels_render(ProbabilityEditor *editor)
{
    const gchar *active = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->level));
    gchar *keep = g_strdup(active ? active : editor->preferred_level); JsonObject *root = root_policy(editor);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(editor->level));
    for (guint level = 0; level < G_N_ELEMENTS(LEVELS); ++level) {
        gboolean available = FALSE; JsonArray *children = arr(root, "children");
        for (guint i = 0; !available && i < length(children); ++i) available = !g_strcmp0(radios_json_string(item(children, i), "scope", ""), LEVELS[level]);
        for (guint i = 0; !available && i < length(editor->catalogue); ++i) {
            JsonObject *target = item(editor->catalogue, i);
            available = target && target_listed(target) && !g_strcmp0(radios_json_string(target, "scope", ""), LEVELS[level]);
        }
        if (available) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->level), LEVELS[level], LEVEL_TITLES[level]);
    }
    if (!keep || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->level), keep)) gtk_combo_box_set_active(GTK_COMBO_BOX(editor->level), 0);
    g_free(keep);
}
static void render(ProbabilityEditor *editor)
{
    if (editor->closed) return;
    editor->updating = TRUE; levels_render(editor); choices_render(editor); transitions_render(editor); jingles_render(editor);
    /* A playlist's own window has nothing to share out: only its rules. */
    gboolean choices_were_visible = gtk_widget_get_visible(editor->choice_section);
    gtk_widget_set_visible(editor->choice_section, level_scope(editor) != NULL);
    if (level_scope(editor) && !choices_were_visible)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(editor->tabs), 0);
    if (!level_scope(editor) && gtk_notebook_get_current_page(GTK_NOTEBOOK(editor->tabs)) == 0)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(editor->tabs), 1);
    select_id(editor->transitions, editor->transition_selected);
    select_id(editor->jingles, editor->jingle_selected);
    select_id(editor->transitions, editor->focus); select_id(editor->jingles, editor->focus);
    simple_render(editor); simple_load(editor); editor->updating = FALSE;
}
static void level_changed(GtkComboBox *combo, gpointer data)
{
    (void)combo; ProbabilityEditor *editor = data; if (editor->updating || editor->closed) return;
    editor->updating = TRUE; g_clear_pointer(&editor->choice_selected, g_free);
    choices_render(editor); simple_render(editor); simple_load(editor); editor->updating = FALSE;
}
static GtkWidget *table(GtkListStore *store, const gchar *name, gint height, gboolean multiple, GtkWidget **scroll)
{
    GtkWidget *tree = named(gtk_tree_view_new_with_model(GTK_TREE_MODEL(store)), name);
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(tree), GTK_TREE_VIEW_GRID_LINES_BOTH);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), multiple ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);
    *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(*scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(*scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(*scroll), tree); gtk_widget_set_size_request(*scroll, -1, height); return tree;
}
static GtkCellRenderer *text_column(GtkWidget *tree, const gchar *title, gint column, gint editable, GCallback edited, gpointer data, gint width)
{
    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *view = gtk_tree_view_column_new_with_attributes(title, cell, "text", column, NULL);
    if (edited) {
        g_object_set(cell, "editable", TRUE, NULL); g_object_set_data(G_OBJECT(cell), "column", GINT_TO_POINTER(column));
        g_signal_connect(cell, "edited", edited, data);
        if (editable >= 0) { gtk_tree_view_column_add_attribute(view, cell, "editable", editable); gtk_tree_view_column_add_attribute(view, cell, "sensitive", editable); }
    }
    gtk_tree_view_column_set_resizable(view, TRUE); if (width > 0) gtk_tree_view_column_set_min_width(view, width);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), view); return cell;
}
static void combo_column(GtkWidget *tree, const gchar *title, gint column, GtkListStore *options, GCallback changed, gpointer data, gint width)
{
    GtkCellRenderer *cell = gtk_cell_renderer_combo_new();
    g_object_set(cell, "model", options, "text-column", OPTION_NAME, "has-entry", FALSE, "editable", TRUE, NULL);
    g_object_set_data(G_OBJECT(cell), "column", GINT_TO_POINTER(column)); g_signal_connect(cell, "changed", changed, data);
    GtkTreeViewColumn *view = gtk_tree_view_column_new_with_attributes(title, cell, "text", column, NULL);
    gtk_tree_view_column_set_resizable(view, TRUE); gtk_tree_view_column_set_min_width(view, width); gtk_tree_view_append_column(GTK_TREE_VIEW(tree), view);
}
static GtkListStore *options_new(const gchar *const *ids, const gchar *const *names, guint count)
{
    GtkListStore *store = gtk_list_store_new(OPTION_COLUMNS, G_TYPE_STRING, G_TYPE_STRING);
    for (guint i = 0; i < count; ++i) gtk_list_store_insert_with_values(store, NULL, -1, OPTION_ID, ids[i], OPTION_NAME, names[i], -1);
    return store;
}
static GtkWidget *button(const gchar *text, const gchar *name, GCallback callback, gpointer data)
{
    GtkWidget *widget = named(radios_ui_button(text), name); g_signal_connect(widget, "clicked", callback, data); return widget;
}
static void advanced_changed(GtkToggleButton *button, gpointer data)
{
    ProbabilityEditor *editor = data;
    gboolean show = gtk_toggle_button_get_active(button);
    for (gint column = 5; column <= 7; ++column)
        gtk_tree_view_column_set_visible(gtk_tree_view_get_column(GTK_TREE_VIEW(editor->transitions), column), show);
    for (gint column = 6; column <= 8; ++column)
        gtk_tree_view_column_set_visible(gtk_tree_view_get_column(GTK_TREE_VIEW(editor->jingles), column), show);
    gtk_widget_set_visible(editor->choice_table_box, show);
    gtk_widget_set_visible(editor->transition_table_box, show);
    gtk_widget_set_visible(editor->jingle_table_box, show);
}
static void tab_changed(GtkNotebook *tabs, GtkWidget *page, guint index, gpointer data)
{
    (void)tabs; (void)page; ProbabilityEditor *editor = data;
    gtk_widget_set_visible(editor->advanced, TRUE);
    (void)index;
}
static gboolean advanced_defined(JsonObject *policy)
{
    const gchar *collections[] = {"transitions", "jingles"};
    for (guint k = 0; k < G_N_ELEMENTS(collections); ++k) {
        JsonArray *rules = arr(policy, collections[k]);
        for (guint i = 0; i < length(rules); ++i) {
            JsonObject *rule = item(rules, i);
            if (numeric(rule, "frequency", 1) > 1 || numeric(rule, "interval_minutes", 0) > 0 ||
                length(arr(rule, "excluded_times"))) return TRUE;
        }
    }
    return FALSE;
}
static void destroyed(GtkWidget *widget, gpointer data)
{ (void)widget; ((ProbabilityEditor *)data)->closed = TRUE; }
static void editor_free(gpointer data)
{
    ProbabilityEditor *editor = data; g_clear_pointer(&editor->policy, json_node_free); g_clear_pointer(&editor->catalogue, json_array_unref);
    g_clear_pointer(&editor->opened_client, radios_data_client_unref);
    GtkListStore *stores[] = {editor->choice_store, editor->transition_store, editor->jingle_store, editor->modes, editor->anchors, editor->playlists, editor->players};
    for (guint i = 0; i < G_N_ELEMENTS(stores); ++i) g_clear_object(&stores[i]);
    if (editor->screen && editor->style) gtk_style_context_remove_provider_for_screen(editor->screen, GTK_STYLE_PROVIDER(editor->style));
    g_clear_object(&editor->style); g_clear_object(&editor->screen);
    g_free(editor->zone); g_free(editor->preferred_level); g_free(editor->focus);
    g_free(editor->choice_selected); g_free(editor->transition_selected); g_free(editor->jingle_selected);
    if (editor->hooks.destroy) editor->hooks.destroy(editor->hooks.user);
    g_free(editor);
}
static ProbabilityEditor *get(GtkWidget *widget)
{ return widget ? g_object_get_data(G_OBJECT(widget), "radios-probability-editor") : NULL; }
static GtkWidget *form_grid(void)
{
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 10); gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE); gtk_style_context_add_class(gtk_widget_get_style_context(grid), "simple-form");
    return grid;
}
static void form_field(GtkWidget *grid, gint column, gint row, const gchar *title, GtkWidget *control)
{
    GtkWidget *field = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5); gtk_box_pack_start(GTK_BOX(field), wrapped(title), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field), control, FALSE, FALSE, 0); gtk_widget_set_hexpand(control, TRUE);
    gtk_grid_attach(GTK_GRID(grid), field, column, row, 1, 1);
}
static GtkWidget *form_spin(const gchar *name, gdouble minimum, gdouble maximum, gdouble step, gdouble value)
{
    GtkWidget *spin = named(gtk_spin_button_new_with_range(minimum, maximum, step), name);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value); gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(spin), 6); return spin;
}
static GtkWidget *percent_field(const gchar *prefix, GtkWidget **spin, GtkWidget **automatic)
{
    gchar *spin_name = g_strconcat(prefix, "-percent", NULL), *auto_name = g_strconcat(prefix, "-auto", NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    *spin = form_spin(spin_name, 0, 100, 0.5, 100); *automatic = named(gtk_check_button_new_with_label("Kalan payı kullan"), auto_name);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(*automatic), TRUE); gtk_widget_set_sensitive(*spin, FALSE);
    gtk_box_pack_start(GTK_BOX(box), *spin, TRUE, TRUE, 0); gtk_box_pack_start(GTK_BOX(box), *automatic, FALSE, FALSE, 0);
    g_signal_connect(*automatic, "toggled", G_CALLBACK(automatic_toggled), *spin);
    g_free(spin_name); g_free(auto_name); return box;
}
static GtkWidget *simple_list(ProbabilityEditor *editor, const gchar *name, GtkWidget **list)
{
    *list = named(gtk_list_box_new(), name); gtk_list_box_set_selection_mode(GTK_LIST_BOX(*list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(*list), TRUE);
    GtkWidget *empty = wrapped("Henüz kural yok."); gtk_widget_set_margin_start(empty, 14);
    gtk_widget_show(empty); gtk_list_box_set_placeholder(GTK_LIST_BOX(*list), empty);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_widget_set_size_request(scroll, -1, 102);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), *list); gtk_style_context_add_class(gtk_widget_get_style_context(*list), "simple-rules");
    g_signal_connect(*list, "row-selected", G_CALLBACK(simple_selected), editor); return scroll;
}
static void legacy_table(GtkBox *section, GtkWidget *scroll, GtkWidget **box)
{
    *box = scroll; gtk_widget_show_all(scroll); gtk_widget_hide(scroll);
    gtk_widget_set_no_show_all(scroll, TRUE); gtk_box_pack_start(section, scroll, TRUE, TRUE, 0);
}
static void editor_style(ProbabilityEditor *editor)
{
    static const gchar *css =
        "#probability-editor-root { background: #202529; color: #edf3f5; }"
        "#probability-editor-root .simple-title { font-size: 20px; font-weight: bold; }"
        "#probability-editor-root notebook { border: 0; background: transparent; }"
        "#probability-editor-root notebook > header { background: transparent; border: 0; padding-bottom: 8px; }"
        "#probability-editor-root notebook > header tab { min-height: 34px; padding: 4px 22px; border: 0; border-bottom: 3px solid transparent; }"
        "#probability-editor-root notebook > header tab:checked { background: #253b46; border-bottom-color: #44cfcb; color: #ffffff; }"
        "#probability-editor-root notebook stack { background: transparent; }"
        "#probability-editor-root .simple-form { padding: 14px; background: #272d32; border-radius: 10px; }"
        "#probability-editor-root button { min-height: 30px; border-radius: 6px; padding: 4px 12px; }"
        "#probability-editor-root .simple-primary { background: #168c91; color: #ffffff; border-color: #25a4a7; font-weight: bold; }"
        "#probability-editor-root .simple-primary:hover { background: #20a1a5; }"
        "#probability-editor-root .simple-mode { min-height: 38px; background: #27343b; border: 1px solid #4a606d; }"
        "#probability-editor-root .simple-mode:checked { background: #156c74; color: #ffffff; border-color: #44cfcb; }"
        "#probability-editor-root spinbutton, #probability-editor-root combobox button { min-height: 32px; background: #172128; color: #edf3f5; border-radius: 6px; }"
        "#probability-editor-root .re-caption { color: #a8bec9; font-size: 12px; }"
        "#probability-editor-root .simple-rules { background: #272d32; border: 1px solid #3b454e; border-radius: 7px; }"
        "#probability-editor-root .simple-rules row { padding: 8px 12px; border-bottom: 1px solid #2c3e47; }"
        "#probability-editor-root .simple-rules row:selected { background: #274954; }"
        "#probability-editor-root .simple-badge { color: #79e2d8; font-weight: bold; padding-left: 10px; }";
    editor->screen = gdk_screen_get_default(); if (!editor->screen) return; g_object_ref(editor->screen);
    editor->style = gtk_css_provider_new(); gtk_css_provider_load_from_data(editor->style, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(editor->screen, GTK_STYLE_PROVIDER(editor->style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
}
GtkWidget *radios_probability_editor_new(RadioSViewContext *ctx, const RadioSProbabilityEditorHooks *hooks)
{
    g_return_val_if_fail(ctx != NULL, NULL); ProbabilityEditor *editor = g_new0(ProbabilityEditor, 1); editor->ctx = ctx;
    if (hooks) editor->hooks = *hooks;
    editor->opened_client = radios_data_client_acquire(ctx); editor->zone = g_strdup(radios_station_timezone(ctx));
    editor->root = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 10, NULL), "root"); gtk_container_set_border_width(GTK_CONTAINER(editor->root), 18);
    editor_style(editor);
    GtkBox *box = GTK_BOX(editor->root);
    GtkWidget *title = heading("Olasılık ayarları"); gtk_style_context_add_class(gtk_widget_get_style_context(title), "simple-title");
    gtk_box_pack_start(box, title, FALSE, FALSE, 0);
    editor->tabs = named(gtk_notebook_new(), "tabs");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(editor->tabs), TRUE);
    gtk_box_pack_start(box, editor->tabs, TRUE, TRUE, 0);
    /* 1 */
    editor->choice_section = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL), "choice-section");
    gtk_notebook_append_page(GTK_NOTEBOOK(editor->tabs), editor->choice_section, gtk_label_new("Ne çalsın?"));
    GtkBox *section = GTK_BOX(editor->choice_section); GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    gtk_box_pack_start(GTK_BOX(row), heading("Neyin çalacağı"), FALSE, FALSE, 0);
    editor->level = named(gtk_combo_box_text_new(), "level"); gtk_box_pack_end(GTK_BOX(row), editor->level, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), wrapped("Liste:"), FALSE, FALSE, 0); gtk_box_pack_start(section, row, FALSE, FALSE, 0);
    g_signal_connect(editor->level, "changed", G_CALLBACK(level_changed), editor);
    GtkWidget *grid = form_grid();
    form_field(grid, 0, 0, "Seçili öğenin çalma olasılığı (%)", percent_field("choice", &editor->choice_percent, &editor->choice_auto));
    editor->choice_save = button("Seçili öğeye uygula", "choice-save", G_CALLBACK(simple_save), editor);
    gtk_style_context_add_class(gtk_widget_get_style_context(editor->choice_save), "simple-primary");
    gtk_grid_attach(GTK_GRID(grid), editor->choice_save, 1, 0, 1, 1); gtk_widget_set_valign(editor->choice_save, GTK_ALIGN_END);
    gtk_box_pack_start(section, grid, FALSE, FALSE, 0);
    gtk_box_pack_start(section, simple_list(editor, "choice-list", &editor->choice_list), TRUE, TRUE, 0);
    editor->choice_store = gtk_list_store_new(CHOICE_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
    GtkWidget *scroll = NULL; GtkCellRenderer *cell = NULL; editor->choices = table(editor->choice_store, "choices", 190, TRUE, &scroll);
    cell = text_column(editor->choices, "Ad", CHOICE_NAME, -1, NULL, NULL, 360); g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_column_set_expand(gtk_tree_view_get_column(GTK_TREE_VIEW(editor->choices), 0), TRUE);
    cell = text_column(editor->choices, "Olasılık (%)", CHOICE_PERCENT, -1, G_CALLBACK(choice_edited), editor, 110);
    g_object_set(cell, "placeholder-text", "kalan", NULL);
    text_column(editor->choices, "Durum", CHOICE_NOTE, -1, NULL, NULL, 160);
    legacy_table(section, scroll, &editor->choice_table_box);
    editor->choice_summary = named(wrapped(""), "choices-summary"); gtk_box_pack_start(section, editor->choice_summary, FALSE, FALSE, 0);
    gtk_widget_show_all(editor->choice_section); gtk_widget_set_no_show_all(editor->choice_section, TRUE);
    /* 2 */
    GtkWidget *transition_page = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL), "transition-section");
    gtk_notebook_append_page(GTK_NOTEBOOK(editor->tabs), transition_page, gtk_label_new("Geçişler"));
    section = GTK_BOX(transition_page);
    row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL); gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
    for (gint mode = 0; mode < 4; ++mode) {
        gchar *name = g_strdup_printf("transition-mode-%d", mode);
        editor->transition_modes[mode] = named(gtk_radio_button_new_with_label_from_widget(mode ? GTK_RADIO_BUTTON(editor->transition_modes[0]) : NULL, MODES[mode]), name); g_free(name);
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(editor->transition_modes[mode]), FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(editor->transition_modes[mode]), "simple-mode");
        gtk_box_pack_start(GTK_BOX(row), editor->transition_modes[mode], TRUE, TRUE, 0);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->transition_modes[1]), TRUE);
    gtk_box_pack_start(section, row, FALSE, FALSE, 0);
    grid = form_grid();
    form_field(grid, 0, 0, "Bu geçişin olasılığı (%)", percent_field("transition", &editor->transition_percent, &editor->transition_auto));
    editor->transition_overlap = form_spin("transition-overlap", 0, 30, 0.1, 3);
    form_field(grid, 1, 0, "İki şarkı kaç saniye üst üste çalsın?", editor->transition_overlap);
    editor->transition_early = form_spin("transition-early", 0, 3600, 0.5, 0);
    form_field(grid, 0, 1, "Kaç saniye erken geçilsin?", editor->transition_early);
    editor->transition_effect = named(gtk_check_button_new_with_label("Geçişte efektli ses kısma"), "transition-effect");
    gtk_grid_attach(GTK_GRID(grid), editor->transition_effect, 1, 1, 1, 1); gtk_widget_set_valign(editor->transition_effect, GTK_ALIGN_END);
    gtk_box_pack_start(section, grid, FALSE, FALSE, 0);
    for (gint mode = 0; mode < 4; ++mode) g_signal_connect(editor->transition_modes[mode], "toggled", G_CALLBACK(mode_toggled), editor);
    row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    GtkWidget *add = button("+ Geçiş ekle", "transition-add", G_CALLBACK(transition_add), editor);
    gtk_style_context_add_class(gtk_widget_get_style_context(add), "simple-primary"); gtk_box_pack_start(GTK_BOX(row), add, FALSE, FALSE, 0);
    editor->transition_save = button("Seçileni güncelle", "transition-save", G_CALLBACK(simple_save), editor);
    editor->transition_remove = button("Sil", "transition-remove", G_CALLBACK(transition_remove), editor);
    gtk_box_pack_start(GTK_BOX(row), editor->transition_save, FALSE, FALSE, 0); gtk_box_pack_end(GTK_BOX(row), editor->transition_remove, FALSE, FALSE, 0);
    gtk_box_pack_start(section, row, FALSE, FALSE, 0);
    gtk_box_pack_start(section, simple_list(editor, "transition-list", &editor->transition_list), TRUE, TRUE, 0);
    editor->transition_store = gtk_list_store_new(TR_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    const gchar *mode_ids[] = {"0", "1", "2", "3"}; editor->modes = options_new(mode_ids, MODES, 4);
    editor->transitions = table(editor->transition_store, "transitions", 120, FALSE, &scroll);
    combo_column(editor->transitions, "Geçiş türü", TR_MODE, editor->modes, G_CALLBACK(transition_mode_changed), editor, 130);
    cell = text_column(editor->transitions, "Olasılık (%)", TR_PERCENT, -1, G_CALLBACK(transition_edited), editor, 90); g_object_set(cell, "placeholder-text", "kalan", NULL);
    GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new(); g_signal_connect(toggle, "toggled", G_CALLBACK(transition_effect_toggled), editor);
    gtk_tree_view_append_column(GTK_TREE_VIEW(editor->transitions), gtk_tree_view_column_new_with_attributes("Efektli inme", toggle,
        "active", TR_EFFECT, "activatable", TR_SHAPED, "sensitive", TR_SHAPED, NULL));
    text_column(editor->transitions, "Bindirme (sn)", TR_OVERLAP, TR_SHAPED, G_CALLBACK(transition_edited), editor, 90);
    text_column(editor->transitions, "Erken geçiş (sn)", TR_EARLY, -1, G_CALLBACK(transition_edited), editor, 100);
    text_column(editor->transitions, "Her N geçişte", TR_FREQUENCY, -1, G_CALLBACK(transition_edited), editor, 90);
    text_column(editor->transitions, "En az ara (dk)", TR_INTERVAL, -1, G_CALLBACK(transition_edited), editor, 90);
    text_column(editor->transitions, "İstenmeyen saatler", TR_EXCLUDED, -1, G_CALLBACK(transition_edited), editor, 160);
    legacy_table(section, scroll, &editor->transition_table_box);
    editor->transition_summary = named(wrapped(""), "transition-summary"); gtk_box_pack_start(section, editor->transition_summary, FALSE, FALSE, 0);
    /* 3 */
    GtkWidget *jingle_page = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL), "jingle-section");
    gtk_notebook_append_page(GTK_NOTEBOOK(editor->tabs), jingle_page, gtk_label_new("Jingle"));
    section = GTK_BOX(jingle_page);
    grid = form_grid();
    editor->jingle_playlist = named(gtk_combo_box_text_new(), "jingle-playlist"); editor->jingle_player = named(gtk_combo_box_text_new(), "jingle-player");
    compact_combo(editor->jingle_playlist); compact_combo(editor->jingle_player);
    form_field(grid, 0, 0, "Hangi jingle listesi?", editor->jingle_playlist); form_field(grid, 1, 0, "Hangi oynatıcıda?", editor->jingle_player);
    editor->jingle_anchor = named(gtk_combo_box_text_new(), "jingle-anchor");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->jingle_anchor), "after_start", "Şarkı başladıktan sonra");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->jingle_anchor), "before_end", "Şarkı bitmeden önce");
    gtk_combo_box_set_active(GTK_COMBO_BOX(editor->jingle_anchor), 0);
    form_field(grid, 0, 1, "Ne zaman?", editor->jingle_anchor);
    editor->jingle_seconds = form_spin("jingle-seconds", 0, 86400, 0.5, 10); form_field(grid, 1, 1, "Kaç saniye?", editor->jingle_seconds);
    form_field(grid, 0, 2, "Çalma olasılığı (%)", percent_field("jingle", &editor->jingle_percent, &editor->jingle_auto));
    editor->jingle_duck = form_spin("jingle-duck", -60, 0, 0.5, -12); form_field(grid, 1, 2, "Jingle çalarken müzik seviyesi (dB)", editor->jingle_duck);
    gtk_box_pack_start(section, grid, FALSE, FALSE, 0);
    row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    editor->jingle_add = button("+ Jingle ekle", "jingle-add", G_CALLBACK(jingle_add), editor);
    gtk_style_context_add_class(gtk_widget_get_style_context(editor->jingle_add), "simple-primary");
    editor->jingle_save = button("Seçileni güncelle", "jingle-save", G_CALLBACK(simple_save), editor);
    editor->jingle_remove = button("Sil", "jingle-remove", G_CALLBACK(jingle_remove), editor);
    gtk_box_pack_start(GTK_BOX(row), editor->jingle_add, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(row), editor->jingle_save, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), editor->jingle_remove, FALSE, FALSE, 0); gtk_box_pack_start(section, row, FALSE, FALSE, 0);
    gtk_box_pack_start(section, simple_list(editor, "jingle-list", &editor->jingle_list), TRUE, TRUE, 0);
    g_signal_connect(editor->jingle_playlist, "changed", G_CALLBACK(jingle_choice_changed), editor); g_signal_connect(editor->jingle_player, "changed", G_CALLBACK(jingle_choice_changed), editor);
    editor->jingle_store = gtk_list_store_new(JG_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    const gchar *anchor_ids[] = {"after_start", "before_end"}, *anchor_names[] = {"Başlangıçtan sonra", "Bitişten önce"};
    editor->anchors = options_new(anchor_ids, anchor_names, 2); editor->playlists = options_new(NULL, NULL, 0); editor->players = options_new(NULL, NULL, 0);
    editor->jingles = table(editor->jingle_store, "jingles", 100, FALSE, &scroll);
    combo_column(editor->jingles, "Jingle playlisti", JG_PLAYLIST, editor->playlists, G_CALLBACK(jingle_option_changed), editor, 150);
    combo_column(editor->jingles, "Player", JG_PLAYER, editor->players, G_CALLBACK(jingle_option_changed), editor, 90);
    combo_column(editor->jingles, "Ne zaman", JG_ANCHOR, editor->anchors, G_CALLBACK(jingle_option_changed), editor, 140);
    text_column(editor->jingles, "Saniye", JG_SECONDS, -1, G_CALLBACK(jingle_edited), editor, 60);
    cell = text_column(editor->jingles, "Olasılık (%)", JG_PERCENT, -1, G_CALLBACK(jingle_edited), editor, 90); g_object_set(cell, "placeholder-text", "kalan", NULL);
    text_column(editor->jingles, "Müzik kısma (dB)", JG_DUCK, -1, G_CALLBACK(jingle_edited), editor, 100);
    text_column(editor->jingles, "Her N şarkıda", JG_FREQUENCY, -1, G_CALLBACK(jingle_edited), editor, 90);
    text_column(editor->jingles, "En az ara (dk)", JG_INTERVAL, -1, G_CALLBACK(jingle_edited), editor, 90);
    text_column(editor->jingles, "İstenmeyen saatler", JG_EXCLUDED, -1, G_CALLBACK(jingle_edited), editor, 150);
    legacy_table(section, scroll, &editor->jingle_table_box);
    editor->jingle_summary = named(wrapped(""), "jingle-summary"); gtk_box_pack_start(section, editor->jingle_summary, FALSE, FALSE, 0);
    editor->advanced = named(gtk_check_button_new_with_label("Gelişmiş: sıklık, saatler ve ayrıntılı tablolar"), "advanced");
    gtk_box_pack_start(box, editor->advanced, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(editor->advanced, TRUE);
    g_signal_connect(editor->advanced, "toggled", G_CALLBACK(advanced_changed), editor);
    g_signal_connect(editor->tabs, "switch-page", G_CALLBACK(tab_changed), editor);
    editor->status = named(wrapped(""), "status"); gtk_box_pack_start(box, editor->status, FALSE, FALSE, 0);
    g_object_set_data_full(G_OBJECT(editor->root), "radios-probability-editor", editor, editor_free); g_signal_connect(editor->root, "destroy", G_CALLBACK(destroyed), editor);
    radios_probability_editor_set_policy(editor->root, NULL); gtk_widget_show_all(editor->root);
    tab_changed(GTK_NOTEBOOK(editor->tabs), NULL, (guint)MAX(0, gtk_notebook_get_current_page(GTK_NOTEBOOK(editor->tabs))), editor);
    return editor->root;
}
void radios_probability_editor_set_policy(GtkWidget *widget, JsonObject *policy)
{
    ProbabilityEditor *editor = get(widget); if (!editor || editor->closed) return;
    JsonNode *replacement = json_node_new(JSON_NODE_OBJECT);
    if (policy) { json_node_set_object(replacement, policy); JsonNode *copy = clone(replacement); json_node_free(replacement); replacement = copy; }
    else { JsonObject *empty = json_object_new(); json_object_set_int_member(empty, "schema", 1); json_node_take_object(replacement, empty); }
    g_clear_pointer(&editor->policy, json_node_free); editor->policy = replacement; g_clear_pointer(&editor->focus, g_free);
    GError *error = NULL;
    if (!radios_automation_policy_validate(root_policy(editor), &error)) notice(editor, error ? error->message : "Kayıtlı olasılık ayarları geçersiz.");
    g_clear_error(&error);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->advanced), FALSE);
    gtk_widget_set_tooltip_text(editor->advanced, advanced_defined(root_policy(editor)) ?
        "Kayıtlı sıklık ve saat kurallarınız korunur. Ayrıntıları görmek için açın." :
        "Sıklık, bekleme ve saat kurallarını ayrıntılı tablolarda düzenleyin.");
    advanced_changed(GTK_TOGGLE_BUTTON(editor->advanced), editor);
    render(editor);
}
void radios_probability_editor_set_targets(GtkWidget *widget, JsonArray *targets)
{
    ProbabilityEditor *editor = get(widget); if (!editor || editor->closed) return;
    JsonArray *replacement = NULL;
    if (targets) {
        JsonNode *node = json_node_new(JSON_NODE_ARRAY); json_node_set_array(node, targets); JsonNode *copy = clone(node); json_node_free(node);
        if (copy) { replacement = json_array_ref(json_node_get_array(copy)); json_node_free(copy); }
    }
    g_clear_pointer(&editor->catalogue, json_array_unref); editor->catalogue = replacement; render(editor);
}
void radios_probability_editor_show_scope(GtkWidget *widget, const gchar *scope)
{
    ProbabilityEditor *editor = get(widget); if (!editor || editor->closed || !scope) return;
    g_free(editor->preferred_level); editor->preferred_level = g_strdup(scope);
    editor->updating = TRUE;
    if (gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->level), scope))
        gtk_notebook_set_current_page(GTK_NOTEBOOK(editor->tabs), 0);
    choices_render(editor); simple_render(editor); simple_load(editor); editor->updating = FALSE;
}
JsonNode *radios_probability_editor_snapshot(GtkWidget *widget)
{ return radios_probability_editor_ready(widget) ? clone(get(widget)->policy) : NULL; }
gboolean radios_probability_editor_ready(GtkWidget *widget)
{
    ProbabilityEditor *editor = get(widget);
    return editor && !editor->closed && identity_current(editor) && radios_automation_policy_validate(root_policy(editor), NULL);
}
