#include "radios/transition_distribution.h"
#include "radios/transition_preset.h"
#include "radios/widgets.h"
#include "radios/data.h"
#include <stdlib.h>
#include <string.h>

/* One table for every scope of settings.transitions (see the header): a row
 * per transition type and, for a kind, per designed preset; a share opens
 * the row's length and low-pass (normal form: a row without a share has no
 * further controls). The base table keeps the widget names of the earlier
 * "Geçiş dağılımı" grid ("rotation-editor-transition-weight-1", …). */
#define MODES 4
static const gchar *const mode_names[MODES] = {"Parça sonunda", "Crossfade", "Cue", "SYNC beatmatch"};
static const gint default_overlaps[MODES] = {0, 4000, 3000, 8000};

typedef struct {
    gchar *id, *name;
    gint duration;
    GtkWidget *label, *weight, *length;
} PresetRow;
typedef struct {
    RadioSTransitionDistributionHooks hooks;
    gchar *kind, *prefix;
    gboolean compact, touched, migrated, presets_enabled;
    guint updating;
    GtkWidget *root, *button, *summary, *grid, *order, *preset_caption, *rest_caption, *rest, *rest_open;
    GtkWidget *weights[MODES], *shares[MODES], *overlaps[MODES], *lowpass[MODES];
    GPtrArray *preset_rows;
    GHashTable *preset_weights; /* preset id → share kept across preset list changes */
} Distribution;

static Distribution *get(GtkWidget *widget)
{ return widget ? g_object_get_data(G_OBJECT(widget), "radios-transition-distribution") : NULL; }
static GtkWidget *named(Distribution *d, GtkWidget *widget, const gchar *part)
{
    gchar *name = g_strdup_printf("%s-%s", d->prefix, part); gtk_widget_set_name(widget, name); g_free(name); return widget;
}
static GtkWidget *caption(const gchar *text)
{
    GtkWidget *label = radios_ui_label(text, "re-caption");
    gtk_label_set_line_wrap(GTK_LABEL(label), FALSE); gtk_label_set_xalign(GTK_LABEL(label), 0); return label;
}
static void show(GtkWidget *widget, gboolean visible)
{
    if (!widget) return;
    gtk_widget_set_no_show_all(widget, !visible);
    if (visible) gtk_widget_show_all(widget); else gtk_widget_hide(widget);
}
static JsonObject *object_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static gboolean bool_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}
static gint spin_int(GtkWidget *spin) { return spin ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin)) : 0; }
/* The entry belongs to this scope: the base has no "kind", a kind its own. */
static gboolean in_scope(Distribution *d, JsonObject *entry)
{
    JsonNode *node = entry ? json_object_get_member(entry, "kind") : NULL;
    if (!d->kind) return !node;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING && !g_strcmp0(json_node_get_string(node), d->kind);
}
static gboolean preset_active(Distribution *d) { return d->kind && d->presets_enabled; }

/* ---- shares ------------------------------------------------------------ */
static gint builtin_total(Distribution *d)
{ gint total = 0; for (guint m = 0; m < MODES; ++m) total += spin_int(d->weights[m]); return total; }
static gint preset_total(Distribution *d)
{
    gint total = 0;
    for (guint i = 0; d->preset_rows && i < d->preset_rows->len; ++i) total += spin_int(((PresetRow *)g_ptr_array_index(d->preset_rows, i))->weight);
    return total;
}
static gchar *summary_text(Distribution *d)
{
    GString *text = g_string_new(NULL); gint used = 0;
    for (guint m = 0; m < MODES; ++m) {
        gint share = spin_int(d->weights[m]); if (!share) continue;
        g_string_append_printf(text, "%s%s %%%d", text->len ? " · " : "", mode_names[m], share); used += share;
    }
    for (guint i = 0; preset_active(d) && i < d->preset_rows->len; ++i) {
        PresetRow *row = g_ptr_array_index(d->preset_rows, i); gint share = spin_int(row->weight); if (!share) continue;
        g_string_append_printf(text, "%s%s %%%d", text->len ? " · " : "", row->name, share); used += share;
    }
    if (!text->len) g_string_append(text, "%100 Geçiş dağılımı");
    else if (used < 100) g_string_append_printf(text, " · kalan %%%d", 100 - used);
    return g_string_free(text, FALSE);
}
/* Normal form: a share brings its row's length and low-pass. The base also
 * shows each type's part of the whole; a kind shows its rest. */
static void dependents_render(Distribution *d)
{
    gint total = builtin_total(d), active = total + (preset_active(d) ? preset_total(d) : 0);
    for (guint m = 0; m < MODES; ++m) {
        gint share = spin_int(d->weights[m]);
        show(d->overlaps[m], m && share > 0);
        show(d->lowpass[m], m && share > 0);
        if (d->shares[m]) {
            gchar *text = share && total ? g_strdup_printf("%%%.0f", 100.0 * share / total) : g_strdup("—");
            gtk_label_set_text(GTK_LABEL(d->shares[m]), text); g_free(text);
        }
    }
    show(d->preset_caption, preset_active(d) && d->preset_rows->len > 0);
    for (guint i = 0; d->kind && i < d->preset_rows->len; ++i) {
        PresetRow *row = g_ptr_array_index(d->preset_rows, i);
        show(row->label, preset_active(d)); show(row->weight, preset_active(d));
        show(row->length, preset_active(d) && spin_int(row->weight) > 0);
    }
    if (d->rest) {
        gchar *text = active <= 100 ? g_strdup_printf("%%%d → Geçiş dağılımı", 100 - active) : g_strdup_printf("Toplam %%%d", active);
        gtk_label_set_text(GTK_LABEL(d->rest), text); g_free(text);
    }
    if (d->summary) {
        gchar *text = summary_text(d), *label = g_strdup_printf("%s ▾", text);
        gtk_label_set_text(GTK_LABEL(d->summary), label); gtk_widget_set_tooltip_text(d->button, text);
        g_free(label); g_free(text);
    }
}
static void edited(GtkWidget *widget, gpointer data)
{
    (void)widget; Distribution *d = data; if (d->updating) return;
    d->touched = TRUE;
    for (guint i = 0; d->kind && i < d->preset_rows->len; ++i) {
        PresetRow *row = g_ptr_array_index(d->preset_rows, i);
        g_hash_table_replace(d->preset_weights, g_strdup(row->id), GINT_TO_POINTER(spin_int(row->weight)));
    }
    dependents_render(d);
    if (d->hooks.changed) d->hooks.changed(d->root, d->hooks.user);
}
static void rest_clicked(GtkButton *button, gpointer data)
{ (void)button; Distribution *d = data; if (d->hooks.open_base) d->hooks.open_base(d->root, d->hooks.user); }

/* ---- presets ------------------------------------------------------------ */
static void preset_row_free(gpointer data)
{ PresetRow *row = data; g_free(row->id); g_free(row->name); g_free(row); }
static void presets_build(Distribution *d, JsonArray *presets)
{
    if (!d->kind) return;
    for (guint i = 0; i < d->preset_rows->len; ++i) {
        PresetRow *row = g_ptr_array_index(d->preset_rows, i);
        gtk_widget_destroy(row->label); gtk_widget_destroy(row->weight); gtk_widget_destroy(row->length);
    }
    g_ptr_array_set_size(d->preset_rows, 0);
    GHashTable *kept = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gint top = MODES + 2;
    for (guint i = 0; presets && i < json_array_get_length(presets); ++i) {
        JsonNode *node = json_array_get_element(presets, i);
        JsonObject *preset = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        const gchar *id = radios_json_string(preset, "id", "");
        if (!preset || !g_uuid_string_is_valid(id) || g_hash_table_contains(kept, id)) continue;
        PresetRow *row = g_new0(PresetRow, 1); row->id = g_strdup(id);
        row->name = g_strdup(radios_json_string(preset, "name", "Ön ayar")); row->duration = (gint)radios_json_int(preset, "duration_ms", 6000);
        row->label = caption(row->name); gtk_label_set_ellipsize(GTK_LABEL(row->label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(row->label), 22);
        gchar *part = g_strdup_printf("preset-%u", d->preset_rows->len);
        row->weight = named(d, gtk_spin_button_new_with_range(0, 100, 5), part); g_free(part);
        gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(row->weight), TRUE);
        gchar *length = g_strdup_printf("%d ms", row->duration); row->length = caption(length); g_free(length);
        gpointer saved = g_hash_table_lookup(d->preset_weights, id);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(row->weight), GPOINTER_TO_INT(saved));
        g_hash_table_add(kept, g_strdup(id));
        g_signal_connect(row->weight, "value-changed", G_CALLBACK(edited), d);
        gtk_grid_attach(GTK_GRID(d->grid), row->label, 0, top, 1, 1);
        gtk_grid_attach(GTK_GRID(d->grid), row->weight, 1, top, 1, 1);
        gtk_grid_attach(GTK_GRID(d->grid), row->length, 3, top, 1, 1);
        g_ptr_array_add(d->preset_rows, row); ++top;
    }
    /* Shares of presets that no longer exist are dropped. */
    GHashTableIter iter; gpointer key;
    g_hash_table_iter_init(&iter, d->preset_weights);
    while (g_hash_table_iter_next(&iter, &key, NULL)) if (!g_hash_table_contains(kept, key)) g_hash_table_iter_remove(&iter);
    g_hash_table_unref(kept);
    if (d->rest_caption) {
        gtk_container_child_set(GTK_CONTAINER(d->grid), d->rest_caption, "top-attach", top, NULL);
        gtk_container_child_set(GTK_CONTAINER(d->grid), d->rest, "top-attach", top, NULL);
        gtk_container_child_set(GTK_CONTAINER(d->grid), d->rest_open, "top-attach", top, NULL);
    }
}
void radios_transition_distribution_set_presets(GtkWidget *widget, JsonArray *presets, gboolean enabled)
{
    Distribution *d = get(widget); if (!d) return;
    ++d->updating; d->presets_enabled = enabled; presets_build(d, presets); dependents_render(d); --d->updating;
}

/* ---- load ---------------------------------------------------------------- */
/* A kind without entries shows its earlier rule converted: its designed
 * presets in equal shares while they act, else its AutoDJ rule (SYNC with
 * its probability, the rest crossfading over the same length). */
static const gchar *string_at(JsonArray *array, guint index)
{
    JsonNode *node = json_array_get_element(array, index);
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}
static void migrate(Distribution *d, JsonObject *settings, gint *weights, gint *overlaps)
{
    JsonObject *automation = object_member(settings, "autodj_automation");
    JsonArray *ids = array_member(object_member(object_member(automation, "kinds"), d->kind), "presets");
    GPtrArray *distinct = g_ptr_array_new();
    for (guint i = 0; ids && i < json_array_get_length(ids); ++i) {
        const gchar *id = string_at(ids, i); gboolean seen = FALSE;
        for (guint k = 0; id && k < distinct->len; ++k) seen |= !g_strcmp0(g_ptr_array_index(distinct, k), id);
        if (id && g_uuid_string_is_valid(id) && !seen) g_ptr_array_add(distinct, (gpointer)id);
    }
    if (distinct->len && bool_member(automation, "enabled")) {
        /* 100 split in the saved order: the first presets take the odd units. */
        for (guint i = 0; i < distinct->len; ++i) {
            gint share = 100 / (gint)distinct->len + ((gint)i < 100 % (gint)distinct->len ? 1 : 0);
            g_hash_table_replace(d->preset_weights, g_strdup(g_ptr_array_index(distinct, i)), GINT_TO_POINTER(share));
        }
        g_ptr_array_unref(distinct); d->migrated = TRUE; return;
    }
    g_ptr_array_unref(distinct);
    JsonObject *rule = object_member(object_member(settings, "kind_transitions"), d->kind);
    if (!rule) return;
    gint mode = (gint)CLAMP(radios_json_int(rule, "mode", 0), 0, MODES - 1), overlap = (gint)radios_json_int(rule, "overlap_ms", 0);
    gint sync = (gint)radios_json_int(rule, "sync_percent", 100);
    if (mode == 3 && sync < 100) {
        if (sync > 0) { weights[3] = sync; overlaps[3] = overlap; }
        weights[1] = 100 - sync; overlaps[1] = overlap;
    } else { weights[mode] = 100; if (mode) overlaps[mode] = overlap; }
    d->migrated = TRUE;
}
void radios_transition_distribution_render(GtkWidget *widget, JsonObject *settings)
{
    Distribution *d = get(widget); if (!d) return;
    ++d->updating;
    gint weights[MODES] = {0}, overlaps[MODES]; gboolean lowpass[MODES] = {FALSE}, found = FALSE;
    for (guint m = 0; m < MODES; ++m) overlaps[m] = default_overlaps[m];
    g_hash_table_remove_all(d->preset_weights); d->touched = d->migrated = FALSE;
    JsonArray *entries = array_member(settings, "transitions");
    for (guint i = 0; entries && i < json_array_get_length(entries); ++i) {
        JsonNode *node = json_array_get_element(entries, i);
        JsonObject *entry = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        if (!entry || !in_scope(d, entry)) continue;
        gint weight = (gint)CLAMP(radios_json_int(entry, "weight", 0), 0, 100);
        const gchar *preset = radios_json_string(entry, "preset_id", NULL);
        if (preset) { if (d->kind && g_uuid_string_is_valid(preset)) { g_hash_table_replace(d->preset_weights, g_strdup(preset), GINT_TO_POINTER(weight)); found = TRUE; } continue; }
        gint64 mode = radios_json_int(entry, "mode", -1); if (mode < 0 || mode >= MODES) continue;
        weights[mode] = weight; found = TRUE;
        if (mode) overlaps[mode] = (gint)CLAMP(radios_json_int(entry, "overlap_ms", default_overlaps[mode]), 0, 30000);
        lowpass[mode] = !g_strcmp0(radios_json_string(entry, "effect", "none"), "lowpass");
    }
    if (!d->kind && !found) weights[0] = 100;
    if (d->kind && !found) migrate(d, settings, weights, overlaps);
    for (guint m = 0; m < MODES; ++m) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->weights[m]), weights[m]);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->overlaps[m]), overlaps[m]);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->lowpass[m]), lowpass[m]);
    }
    if (d->order) gtk_combo_box_set_active_id(GTK_COMBO_BOX(d->order),
        !g_strcmp0(radios_json_string(settings, "transition_selection_mode", "weighted"), "linear") ? "linear" : "weighted");
    d->presets_enabled = bool_member(object_member(settings, "autodj_automation"), "enabled");
    presets_build(d, array_member(settings, "autodj_presets"));
    dependents_render(d);
    --d->updating;
}

/* ---- save ---------------------------------------------------------------- */
static JsonArray *scope_entries(Distribution *d)
{
    JsonArray *entries = json_array_new();
    for (guint m = 0; m < MODES; ++m) {
        gint weight = spin_int(d->weights[m]); if (!weight) continue;
        JsonObject *entry = json_object_new();
        if (d->kind) json_object_set_string_member(entry, "kind", d->kind);
        json_object_set_int_member(entry, "mode", m); json_object_set_int_member(entry, "weight", weight);
        json_object_set_int_member(entry, "overlap_ms", m ? spin_int(d->overlaps[m]) : 0);
        if (m && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->lowpass[m]))) json_object_set_string_member(entry, "effect", "lowpass");
        json_array_add_object_element(entries, entry);
    }
    for (guint i = 0; d->kind && i < d->preset_rows->len; ++i) {
        PresetRow *row = g_ptr_array_index(d->preset_rows, i); gint weight = spin_int(row->weight); if (!weight) continue;
        JsonObject *entry = json_object_new(); json_object_set_string_member(entry, "kind", d->kind);
        json_object_set_string_member(entry, "preset_id", row->id); json_object_set_int_member(entry, "mode", 1);
        json_object_set_int_member(entry, "overlap_ms", 0); json_object_set_int_member(entry, "weight", weight);
        json_array_add_object_element(entries, entry);
    }
    return entries;
}
void radios_transition_distribution_read(GtkWidget *widget, JsonObject *settings)
{
    Distribution *d = get(widget); if (!d || !settings) return;
    JsonArray *previous = array_member(settings, "transitions"), *next = json_array_new(), *mine = scope_entries(d);
    gboolean placed = FALSE;
    /* The base leads the array; a kind's entries take the place of its old ones. */
    if (!d->kind) { for (guint i = 0; i < json_array_get_length(mine); ++i) json_array_add_element(next, json_node_copy(json_array_get_element(mine, i))); placed = TRUE; }
    for (guint i = 0; previous && i < json_array_get_length(previous); ++i) {
        JsonNode *node = json_array_get_element(previous, i);
        JsonObject *entry = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        if (entry && in_scope(d, entry)) {
            if (!placed) { for (guint k = 0; k < json_array_get_length(mine); ++k) json_array_add_element(next, json_node_copy(json_array_get_element(mine, k))); placed = TRUE; }
            continue;
        }
        json_array_add_element(next, json_node_copy(node));
    }
    if (!placed) for (guint k = 0; k < json_array_get_length(mine); ++k) json_array_add_element(next, json_node_copy(json_array_get_element(mine, k)));
    json_array_unref(mine);
    json_object_set_array_member(settings, "transitions", next);
    if (d->order) {
        const gchar *order = gtk_combo_box_get_active_id(GTK_COMBO_BOX(d->order));
        json_object_set_string_member(settings, "transition_selection_mode", order && !strcmp(order, "linear") ? "linear" : "weighted");
    }
}
gboolean radios_transition_distribution_valid(GtkWidget *widget, gchar **message)
{
    Distribution *d = get(widget); if (message) *message = NULL;
    if (!d) return TRUE;
    gchar *problem = NULL; gint index = d->kind ? radios_transition_kind_index(d->kind) : -1;
    const gchar *owner = index >= 0 ? radios_transition_kind_labels[index] : "Rotasyon";
    gint total = builtin_total(d) + preset_total(d);
    for (guint m = 1; !problem && m < MODES; ++m) {
        if (!spin_int(d->weights[m]) || spin_int(d->overlaps[m])) continue;
        if (m == 3) problem = g_strdup_printf("%s: SYNC beatmatch için süre 0'dan büyük olmalı.", owner);
        else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->lowpass[m]))) problem = g_strdup_printf("%s: low-pass için süre 0'dan büyük olmalı.", owner);
    }
    if (!problem && d->kind && total > 100) problem = g_strdup_printf("%s geçişlerinin toplamı %%%d olur; en çok %%100 verilebilir.", owner, total);
    if (!problem && !d->kind && !builtin_total(d)) problem = g_strdup("Geçiş dağılımı: en az bir geçiş türünün olasılığı 0'dan büyük olmalı.");
    if (!problem) return TRUE;
    if (message) *message = problem; else g_free(problem);
    return FALSE;
}
gboolean radios_transition_distribution_pending(GtkWidget *widget)
{ Distribution *d = get(widget); return d && (d->touched || d->migrated); }
guint radios_transition_distribution_count(GtkWidget *widget)
{
    Distribution *d = get(widget); if (!d) return 0;
    JsonArray *entries = scope_entries(d); guint count = json_array_get_length(entries); json_array_unref(entries); return count;
}
gboolean radios_transition_distribution_uses_presets(GtkWidget *widget)
{ Distribution *d = get(widget); return d && preset_active(d) && preset_total(d) > 0; }

/* ---- construction --------------------------------------------------------- */
static void distribution_free(gpointer data)
{
    Distribution *d = data;
    g_ptr_array_unref(d->preset_rows); g_hash_table_unref(d->preset_weights);
    g_free(d->kind); g_free(d->prefix); g_free(d);
}
GtkWidget *radios_transition_distribution_new(const gchar *kind, gboolean compact, const RadioSTransitionDistributionHooks *hooks)
{
    Distribution *d = g_new0(Distribution, 1);
    if (hooks) d->hooks = *hooks;
    d->kind = kind && radios_transition_kind_index(kind) >= 0 ? g_strdup(kind) : NULL; d->compact = compact;
    d->prefix = d->kind ? g_strdup_printf("rotation-editor-distribution-%s", d->kind) : g_strdup("rotation-editor-transition");
    d->preset_rows = g_ptr_array_new_with_free_func(preset_row_free);
    d->preset_weights = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    d->grid = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(d->grid), 12); gtk_grid_set_row_spacing(GTK_GRID(d->grid), 6);
    const gchar *columns[] = {"Geçiş", d->kind ? "Olasılık (%)" : "Olasılık", d->kind ? "" : "Pay", "Süre (ms)", ""};
    for (guint c = 0; c < G_N_ELEMENTS(columns); ++c) if (*columns[c]) gtk_grid_attach(GTK_GRID(d->grid), caption(columns[c]), (gint)c, 0, 1, 1);
    for (guint m = 0; m < MODES; ++m) {
        gchar *part = g_strdup_printf("weight-%u", m);
        d->weights[m] = named(d, gtk_spin_button_new_with_range(0, 100, 5), part); g_free(part);
        part = g_strdup_printf("overlap-%u", m);
        d->overlaps[m] = named(d, gtk_spin_button_new_with_range(0, 30000, 250), part); g_free(part);
        part = g_strdup_printf("lowpass-%u", m);
        d->lowpass[m] = named(d, gtk_check_button_new_with_label("Low-pass"), part); g_free(part);
        gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(d->weights[m]), TRUE); gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(d->overlaps[m]), TRUE);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->overlaps[m]), default_overlaps[m]);
        gtk_grid_attach(GTK_GRID(d->grid), caption(mode_names[m]), 0, (gint)m + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(d->grid), d->weights[m], 1, (gint)m + 1, 1, 1);
        if (!d->kind) {
            part = g_strdup_printf("share-%u", m); d->shares[m] = named(d, caption("—"), part); g_free(part);
            gtk_grid_attach(GTK_GRID(d->grid), d->shares[m], 2, (gint)m + 1, 1, 1);
        }
        gtk_grid_attach(GTK_GRID(d->grid), d->overlaps[m], 3, (gint)m + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(d->grid), d->lowpass[m], 4, (gint)m + 1, 1, 1);
        g_signal_connect(d->weights[m], "value-changed", G_CALLBACK(edited), d);
        g_signal_connect(d->overlaps[m], "value-changed", G_CALLBACK(edited), d);
        g_signal_connect(d->lowpass[m], "toggled", G_CALLBACK(edited), d);
    }
    if (d->kind) {
        d->preset_caption = caption("Ön ayarlar"); gtk_grid_attach(GTK_GRID(d->grid), d->preset_caption, 0, MODES + 1, 2, 1);
        d->rest_caption = caption("Kalan pay"); gtk_grid_attach(GTK_GRID(d->grid), d->rest_caption, 0, MODES + 2, 1, 1);
        d->rest = named(d, caption(""), "rest"); gtk_grid_attach(GTK_GRID(d->grid), d->rest, 1, MODES + 2, 2, 1);
        d->rest_open = named(d, radios_ui_button("Geçiş dağılımı…"), "rest-open");
        g_signal_connect(d->rest_open, "clicked", G_CALLBACK(rest_clicked), d);
        gtk_grid_attach(GTK_GRID(d->grid), d->rest_open, 3, MODES + 2, 2, 1);
    }
    GtkWidget *table = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL);
    if (!d->kind) {
        GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 12, NULL);
        gtk_box_pack_start(GTK_BOX(heading), radios_ui_label("Geçiş dağılımı", "re-heading"), FALSE, FALSE, 0);
        d->order = named(d, gtk_combo_box_text_new(), "order");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(d->order), "weighted", "Olasılıkla seç");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(d->order), "linear", "Sırayla uygula");
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(d->order), "weighted");
        gtk_widget_set_tooltip_text(d->order, "Olasılıkla: her geçişte payına göre çekilir. Sırayla: olasılığı olan türler sırayla uygulanır.");
        g_signal_connect(d->order, "changed", G_CALLBACK(edited), d);
        gtk_box_pack_end(GTK_BOX(heading), d->order, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(table), heading, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(table), d->grid, FALSE, FALSE, 0);
    if (compact) {
        d->button = gtk_menu_button_new(); d->root = d->button;
        d->summary = gtk_label_new(""); gtk_label_set_ellipsize(GTK_LABEL(d->summary), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(d->summary), 34); gtk_label_set_xalign(GTK_LABEL(d->summary), 0);
        gtk_container_add(GTK_CONTAINER(d->button), d->summary);
        GtkWidget *popover = gtk_popover_new(d->button); gtk_menu_button_set_popover(GTK_MENU_BUTTON(d->button), popover);
        gtk_container_set_border_width(GTK_CONTAINER(table), 10); gtk_container_add(GTK_CONTAINER(popover), table);
        gtk_widget_show_all(table);
    } else d->root = table;
    gtk_widget_set_name(d->root, d->kind ? d->prefix : "rotation-editor-transitions");
    g_object_set_data_full(G_OBJECT(d->root), "radios-transition-distribution", d, distribution_free);
    ++d->updating; dependents_render(d); --d->updating;
    return d->root;
}
