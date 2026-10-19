#include "radios/schema_settings_ui.h"
#include "radios/automation_calendar_plan.h"
#include "radios/widgets.h"
#include <stdlib.h>
#include <string.h>

/* A schema carries ordered typed records and links; an automation is the
 * transition assigned to a record or link. This form edits the existing wire
 * contract, leaving media selection, player routing and preset design with
 * their respective editors. */
typedef struct SchemaEditor SchemaEditor;
typedef struct {
    SchemaEditor *editor;
    GtkWidget *root, *choice, *details, *mode, *overlap;
    gchar *kind; /* content kind of the boundary; NULL offers every preset */
} TransitionForm;
struct SchemaEditor {
    GtkWidget *root, *status, *tabs;
    GtkWidget *step_target, *step_routing, *steps, *step_order, *step_kind, *step_remove, *step_channels_enabled, *step_channels;
    GtkWidget *links, *link_from, *link_to, *link_remove;
    GtkWidget *entry_source, *entry_remove, *alternatives, *alternative_source, *alternative_weight, *alternative_remove, *entry_summary;
    GtkWidget *flow, *flow_players, *flow_detail, *flow_save, *flow_next;
    GtkListStore *step_store, *link_store, *alternative_store, *flow_store;
    TransitionForm step_transition, link_transition, entry_transition, alternative_transition, flow_transition;
    JsonNode *definition;
    JsonArray *playlists;
    JsonArray *players;  /* [{deck 1..32, name, cluster_id, cluster_name}] of the live topology */
    JsonArray *library;  /* existing automations of the station: presets + their typed kinds */
    JsonArray *channels; /* defined mixer strips [{number, name, label, source}] for the channel tree */
    RadioSSchemaSettingsChanged changed;
    gpointer user;
    void (*routing)(GtkWidget *, const gchar *, gpointer);
    gpointer routing_user;
    gint step_index, link_index, alternative_index, flow_index;
    gboolean updating;
};
static const gchar *const kinds[] = {"music", "talk", "dj", "advertisement", "jingle", "sweeper"};
static const gchar *const kind_titles[] = {"Müzik", "Konuşma", "DJ", "Reklam", "Jingle", "Sweeper"};
static const gchar *const modes[] = {"Parça sonunda", "Crossfade", "Cue", "SYNC beatmatch"};

static void render(SchemaEditor *editor);
static gint source_order(SchemaEditor *editor, GtkWidget *combo);
static JsonObject *flow_next(SchemaEditor *editor, guint index);
static guint32 deck_mask(JsonArray *decks);
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
static const gchar *str(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : "";
}
static gint number(JsonObject *object, const gchar *key, gint fallback)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 ? (gint)json_node_get_int(node) : fallback;
}
static guint count(JsonArray *array) { return array ? json_array_get_length(array) : 0; }
static JsonObject *at(JsonArray *array, guint index)
{
    JsonNode *node = array && index < count(array) ? json_array_get_element(array, index) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonNode *clone(JsonNode *node)
{
    if (!node) return NULL;
    gchar *text = json_to_string(node, FALSE); JsonParser *parser = json_parser_new();
    gboolean parsed = json_parser_load_from_data(parser, text, -1, NULL); g_free(text);
    JsonNode *copy = parsed ? json_parser_steal_root(parser) : NULL; g_object_unref(parser); return copy;
}
static JsonNode *clone_object(JsonObject *object)
{
    if (!object) return NULL;
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, object);
    JsonNode *copy = clone(node); json_node_free(node); return copy;
}
static JsonObject *definition(SchemaEditor *editor)
{ return editor->definition ? json_node_get_object(editor->definition) : NULL; }
static JsonObject *settings(SchemaEditor *editor) { return obj(definition(editor), "settings"); }
static GtkWidget *named(GtkWidget *widget, const gchar *suffix)
{
    gchar *name = g_strconcat("schema-settings-", suffix, NULL); gtk_widget_set_name(widget, name); g_free(name); return widget;
}
static GtkWidget *label(const gchar *text)
{
    GtkWidget *widget = gtk_label_new(text); gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE); return widget;
}
static void notice(SchemaEditor *editor, const gchar *message)
{ gtk_label_set_text(GTK_LABEL(editor->status), message); }
static void pack(GtkWidget *box, GtkWidget *widget, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), widget, expand, expand, 0); }
static GtkWidget *box(GtkOrientation orientation)
{ return gtk_box_new(orientation, 8); }
static GtkWidget *spin(const gchar *name, gint low, gint high, gint value)
{
    GtkWidget *widget = named(gtk_spin_button_new_with_range(low, high, 1), name);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), value); gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(widget), TRUE); return widget;
}
static gboolean spin_value(GtkWidget *widget, gint low, gint high, gint *value)
{
    gchar *text = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget))); g_strstrip(text);
    gchar *end = NULL; gint64 parsed = g_ascii_strtoll(text, &end, 10);
    gboolean valid = *text && end && !*end && parsed >= low && parsed <= high;
    if (valid) *value = (gint)parsed;
    g_free(text); return valid;
}
static void field(GtkWidget *grid, const gchar *caption, GtkWidget *widget, gint row)
{
    gtk_grid_attach(GTK_GRID(grid), label(caption), 0, row, 1, 1);
    gtk_widget_set_hexpand(widget, TRUE); gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
}
static GtkWidget *form(void)
{
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 7); gtk_grid_set_column_spacing(GTK_GRID(grid), 12); return grid;
}
static GtkWidget *button(SchemaEditor *editor, const gchar *caption, const gchar *name, GCallback callback)
{
    GtkWidget *widget = named(gtk_button_new_with_label(caption), name);
    g_signal_connect(widget, "clicked", callback, editor); return widget;
}
static GtkWidget *table(GtkListStore **store, const gchar *name, const gchar *first, const gchar *second, const gchar *third)
{
    *store = gtk_list_store_new(4, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tree = named(gtk_tree_view_new_with_model(GTK_TREE_MODEL(*store)), name);
    const gchar *titles[] = {first, second, third};
    for (guint i = 0; i < G_N_ELEMENTS(titles); ++i) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new(); g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", (gint)i + 1, NULL);
        gtk_tree_view_column_set_expand(column, TRUE); gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
    }
    return tree;
}
static GtkWidget *scroll(GtkWidget *child)
{
    GtkWidget *widget = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(widget), 110);
    gtk_container_add(GTK_CONTAINER(widget), child); return widget;
}
static JsonObject *own_preset(JsonObject *owner, const gchar *id)
{
    JsonArray *presets = arr(owner, "autodj_presets");
    for (guint i = 0; i < count(presets); ++i) if (!g_strcmp0(str(at(presets, i), "id"), id)) return at(presets, i);
    return NULL;
}
/* Kılavuz §02: "Şema hazırlanırken mevcut ... otomasyonlar kullanılmalıdır."
 * The schema's own presets first, then those of the station's other
 * rotations; a chosen one is copied into the schema on save (commit). */
static JsonObject *library_preset(SchemaEditor *editor, const gchar *id)
{
    for (guint i = 0; i < count(editor->library); ++i) if (!g_strcmp0(str(obj(at(editor->library, i), "preset"), "id"), id)) return at(editor->library, i);
    return NULL;
}
static JsonObject *preset(SchemaEditor *editor, const gchar *id)
{
    JsonObject *own = own_preset(settings(editor), id);
    return own ? own : obj(library_preset(editor, id), "preset");
}
static gchar *source_title(SchemaEditor *editor, JsonObject *source, guint index)
{
    const gchar *title = str(obj(source, "live"), "title");
    if (!*title && !g_strcmp0(str(source, "source_type"), "live")) title = "Live event";
    for (guint i = 0; !*title && i < count(editor->playlists); ++i)
        if (!g_strcmp0(str(at(editor->playlists, i), "_id"), str(source, "playlist_id"))) title = str(at(editor->playlists, i), "name");
    if (!*title) title = str(source, "name");
    return g_strdup_printf("%u · %s", index + 1, *title ? title : "Playlist");
}
static gchar *source_label(SchemaEditor *editor, const gchar *id)
{
    JsonArray *sources = arr(definition(editor), "sources");
    for (guint i = 0; i < count(sources); ++i) if (!g_strcmp0(str(at(sources, i), "id"), id)) return source_title(editor, at(sources, i), i);
    return g_strdup("Kaldırılmış yayın adımı");
}
static JsonObject *source_by_id(JsonObject *root, const gchar *id)
{
    JsonArray *sources = arr(root, "sources");
    for (guint i = 0; i < count(sources); ++i) if (!g_strcmp0(str(at(sources, i), "id"), id)) return at(sources, i);
    return NULL;
}
/* Kılavuz §02: "Playlist türü, hangi otomasyonun bu bağlantıda
 * kullanılabileceğini belirler." The boundary kind is the incoming step's
 * content kind: a playlist's purpose or a live step's label. */
static const gchar *source_kind(SchemaEditor *editor, JsonObject *source)
{
    if (!source) return NULL;
    if (!g_strcmp0(str(source, "source_type"), "live")) { const gchar *label = str(obj(source, "live"), "label"); return *label ? label : "talk"; }
    for (guint i = 0; i < count(editor->playlists); ++i) {
        JsonObject *playlist = at(editor->playlists, i); const gchar *purpose = str(playlist, "purpose");
        if (g_strcmp0(str(playlist, "_id"), str(source, "playlist_id"))) continue;
        if (!g_strcmp0(purpose, "advert") || !g_strcmp0(purpose, "advertisement")) return "advertisement";
        return !g_strcmp0(purpose, "jingle") || !g_strcmp0(purpose, "sweeper") ? purpose : "music";
    }
    return "music";
}
static const gchar *kind_title(const gchar *kind)
{
    for (guint i = 0; kind && i < G_N_ELEMENTS(kinds); ++i) if (!g_strcmp0(kinds[i], kind)) return kind_titles[i];
    return "Bu";
}
/* "Playlist türü, hangi otomasyonun bu bağlantıda kullanılabileceğini
 * belirler": a library preset keeps the kinds its own rotation typed it for. */
static gboolean preset_usable(TransitionForm *form, const gchar *id)
{
    if (own_preset(settings(form->editor), id) || !form->kind) return radios_automation_preset_kind_usable(settings(form->editor), id, form->kind);
    JsonArray *typed = arr(library_preset(form->editor, id), "kinds");
    for (guint i = 0; i < count(typed); ++i) if (!g_strcmp0(json_array_get_string_element(typed, i), form->kind)) return TRUE;
    return !count(typed);
}
static JsonObject *target_settings(SchemaEditor *editor, JsonObject *root, gboolean create)
{
    const gchar *target = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->step_target));
    JsonObject *owner = !g_strcmp0(target, "schema") ? root : source_by_id(root, target);
    if (!owner) return NULL;
    JsonObject *result = obj(owner, "settings");
    if (!result && create) { result = json_object_new(); json_object_set_object_member(owner, "settings", result); }
    return result;
}
static gchar *transition_label(SchemaEditor *editor, JsonObject *row)
{
    const gchar *id = str(row, "preset_id");
    if (*id) { JsonObject *value = preset(editor, id); return g_strdup(value ? str(value, "name") : "Kullanılamayan otomasyon"); }
    JsonObject *transition = obj(row, "transition");
    if (!transition) return g_strdup("Atanmadı · sonraki kurala geç");
    gint mode = CLAMP(number(transition, "mode", 0), 0, 3);
    return mode ? g_strdup_printf("%s · %d ms", modes[mode], number(transition, "overlap_ms", 0)) : g_strdup(modes[mode]);
}
static void transition_visibility(TransitionForm *form)
{
    gboolean simple = !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(form->choice)), "simple");
    gtk_widget_set_no_show_all(form->details, !simple); gtk_widget_set_visible(form->details, simple);
    gboolean duration = gtk_combo_box_get_active(GTK_COMBO_BOX(form->mode)) > 0;
    gtk_widget_set_sensitive(form->overlap, duration);
}
static void transition_changed(GtkComboBox *combo, gpointer data)
{ (void)combo; transition_visibility(data); }
static GtkWidget *transition_form(SchemaEditor *editor, TransitionForm *out, const gchar *prefix)
{
    out->editor = editor; out->root = box(GTK_ORIENTATION_VERTICAL);
    gchar *name = g_strconcat(prefix, "-automation", NULL); out->choice = named(gtk_combo_box_text_new(), name); g_free(name);
    gtk_widget_set_tooltip_text(out->choice, "AutoDJ otomasyonu bölümünde kaydedilen bir geçişi bu kayda ata.");
    pack(out->root, out->choice, FALSE); out->details = box(GTK_ORIENTATION_HORIZONTAL);
    name = g_strconcat(prefix, "-mode", NULL); out->mode = named(gtk_combo_box_text_new(), name); g_free(name);
    for (guint i = 0; i < G_N_ELEMENTS(modes); ++i) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(out->mode), modes[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(out->mode), 0);
    name = g_strconcat(prefix, "-overlap", NULL); out->overlap = spin(name, 0, 30000, 0); g_free(name);
    pack(out->details, out->mode, TRUE); pack(out->details, label("Süre (ms)"), FALSE); pack(out->details, out->overlap, FALSE);
    pack(out->root, out->details, FALSE);
    g_signal_connect(out->choice, "changed", G_CALLBACK(transition_changed), out);
    g_signal_connect(out->mode, "changed", G_CALLBACK(transition_changed), out); return out->root;
}
/* Offers only the presets this boundary's kind may use; a saved choice that
 * no longer fits stays visible (flagged) so it can be replaced, not lost. */
static void transition_choices(TransitionForm *form, const gchar *keep)
{
    gchar *wanted = g_strdup(keep ? keep : "inherit");
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(form->choice));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(form->choice), "inherit", "Atanmadı · sonraki kurala geç");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(form->choice), "simple", "Temel geçiş tanımla");
    JsonArray *presets = arr(settings(form->editor), "autodj_presets");
    for (guint i = 0; i < count(presets); ++i) {
        JsonObject *value = at(presets, i); const gchar *id = str(value, "id");
        if (g_uuid_string_is_valid(id) && preset_usable(form, id)) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(form->choice), id, str(value, "name"));
    }
    for (guint i = 0; i < count(form->editor->library); ++i) {
        JsonObject *item = at(form->editor->library, i), *value = obj(item, "preset"); const gchar *id = str(value, "id");
        if (!g_uuid_string_is_valid(id) || own_preset(settings(form->editor), id) || !preset_usable(form, id)) continue;
        gchar *caption = g_strdup_printf("%s · %s", str(value, "name"), str(item, "origin"));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(form->choice), id, caption); g_free(caption);
    }
    if (g_uuid_string_is_valid(wanted) && !preset(form->editor, wanted))
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(form->choice), wanted, "Kullanılamayan otomasyon · yeniden seç");
    else if (g_uuid_string_is_valid(wanted) && !preset_usable(form, wanted)) {
        gchar *caption = g_strdup_printf("%s · %s türüne atanmamış", str(preset(form->editor, wanted), "name"), kind_title(form->kind));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(form->choice), wanted, caption); g_free(caption);
    }
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(form->choice), wanted)) gtk_combo_box_set_active_id(GTK_COMBO_BOX(form->choice), "inherit");
    g_free(wanted);
}
static void form_kind(TransitionForm *form, const gchar *kind)
{ if (g_strcmp0(form->kind, kind)) { g_free(form->kind); form->kind = g_strdup(kind); } }
static void transition_load(TransitionForm *form, JsonObject *row)
{
    const gchar *id = str(row, "preset_id");
    JsonObject *simple = obj(row, "transition");
    transition_choices(form, *id ? id : simple ? "simple" : "inherit");
    gtk_combo_box_set_active(GTK_COMBO_BOX(form->mode), CLAMP(number(simple, "mode", 0), 0, 3));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(form->overlap), number(simple, "overlap_ms", 0)); transition_visibility(form);
}
static gboolean transition_read(TransitionForm *form, JsonObject *row)
{
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(form->choice));
    if (!id) { notice(form->editor, "Geçiş otomasyonunu seçin."); return FALSE; }
    json_object_remove_member(row, "preset_id"); json_object_remove_member(row, "transition");
    if (!g_strcmp0(id, "inherit")) return TRUE;
    if (!g_strcmp0(id, "simple")) {
        gint mode = gtk_combo_box_get_active(GTK_COMBO_BOX(form->mode)), overlap = 0;
        if (mode < 0 || mode > 3 || (mode && !spin_value(form->overlap, 0, 30000, &overlap))) {
            notice(form->editor, "Geçiş süresi 0–30000 ms arasında tam sayı olmalı."); return FALSE;
        }
        JsonObject *value = json_object_new(); json_object_set_int_member(value, "mode", mode);
        json_object_set_int_member(value, "overlap_ms", overlap); json_object_set_object_member(row, "transition", value); return TRUE;
    }
    if (!preset(form->editor, id)) { notice(form->editor, "Atanan otomasyon artık bulunamıyor; mevcut bir otomasyon seçin."); return FALSE; }
    if (!preset_usable(form, id)) {
        gchar *message = g_strdup_printf("Bu otomasyon %s türüne atanmamış; uygun bir otomasyon seçin.", kind_title(form->kind));
        notice(form->editor, message); g_free(message); return FALSE;
    }
    json_object_set_string_member(row, "preset_id", id); return TRUE;
}
static void array_put(JsonObject *owner, const gchar *key, gint index, JsonObject *row)
{
    JsonArray *old = arr(owner, key), *replacement = json_array_new();
    for (guint i = 0; i < count(old); ++i) {
        if ((gint)i == index) { if (row) json_array_add_object_element(replacement, json_object_ref(row)); }
        else json_array_add_element(replacement, clone(json_array_get_element(old, i)));
    }
    if (row && (index < 0 || (guint)index >= count(old))) json_array_add_object_element(replacement, json_object_ref(row));
    if (count(replacement)) json_object_set_array_member(owner, key, replacement);
    else { json_array_unref(replacement); json_object_remove_member(owner, key); }
}
/* Every preset a record of the candidate names must be the schema's own
 * (the planner and the store resolve only those): an existing automation of
 * another rotation chosen here is copied in. FALSE: the 32-preset limit. */
static void preset_reference(GPtrArray *ids, JsonObject *row)
{ if (*str(row, "preset_id")) g_ptr_array_add(ids, (gpointer)str(row, "preset_id")); }
static void preset_references(GPtrArray *ids, JsonObject *owner)
{
    JsonArray *lists[] = {arr(owner, "automation_steps"), arr(owner, "link_transitions")};
    for (guint l = 0; l < G_N_ELEMENTS(lists); ++l) for (guint i = 0; i < count(lists[l]); ++i) preset_reference(ids, at(lists[l], i));
    JsonObject *entry = obj(owner, "programme_entry"); preset_reference(ids, entry);
    for (guint i = 0; i < count(arr(entry, "alternatives")); ++i) preset_reference(ids, at(arr(entry, "alternatives"), i));
}
static gboolean presets_import(SchemaEditor *editor, JsonObject *root)
{
    JsonObject *owner = obj(root, "settings"); if (!owner) return TRUE;
    GPtrArray *ids = g_ptr_array_new(); preset_references(ids, owner);
    for (guint i = 0; i < count(arr(root, "sources")); ++i) preset_references(ids, obj(at(arr(root, "sources"), i), "settings"));
    gboolean fits = TRUE;
    for (guint i = 0; i < ids->len && fits; ++i) {
        const gchar *id = g_ptr_array_index(ids, i); JsonObject *value = obj(library_preset(editor, id), "preset");
        if (own_preset(owner, id) || !value) continue;
        JsonArray *presets = arr(owner, "autodj_presets");
        if (!presets) { presets = json_array_new(); json_object_set_array_member(owner, "autodj_presets", presets); }
        if (count(presets) >= 32) { fits = FALSE; break; }
        json_array_add_element(presets, clone_object(value));
    }
    g_ptr_array_free(ids, TRUE);
    if (!fits) notice(editor, "Bir şemada en fazla 32 otomasyon olabilir; kullanılmayanları AutoDJ otomasyonu bölümünden kaldırın.");
    return fits;
}
static void commit(SchemaEditor *editor, JsonNode *candidate)
{
    if (!presets_import(editor, json_node_get_object(candidate))) { json_node_free(candidate); return; }
    if (editor->definition && json_node_equal(editor->definition, candidate)) { json_node_free(candidate); notice(editor, "Ayarlar zaten kayıtlı."); return; }
    g_clear_pointer(&editor->definition, json_node_free); editor->definition = candidate;
    render(editor); notice(editor, "Şema ayarları kaydediliyor.");
    if (editor->changed) {
        JsonNode *snapshot = clone(editor->definition);
        editor->changed(editor->root, snapshot, editor->user); json_node_free(snapshot);
    }
}
static gint selected_index(GtkTreeSelection *selection)
{
    GtkTreeModel *model = NULL; GtkTreeIter row; gint index = -1;
    if (gtk_tree_selection_get_selected(selection, &model, &row)) gtk_tree_model_get(model, &row, 0, &index, -1);
    return index;
}
static void step_load(SchemaEditor *editor)
{
    JsonObject *row = at(arr(target_settings(editor, definition(editor), FALSE), "automation_steps"), editor->step_index);
    gboolean updating = editor->updating; editor->updating = TRUE;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->step_order), number(row, "order", 1));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->step_kind), row ? str(row, "kind") : "music");
    editor->updating = updating;
    form_kind(&editor->step_transition, gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->step_kind)));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->step_channels_enabled), row && json_object_has_member(row, "channels"));
    radios_ui_channel_picker_set_selected(editor->step_channels, arr(row, "channels"));
    transition_load(&editor->step_transition, row); gtk_widget_set_sensitive(editor->step_remove, row != NULL);
}
static void step_select(GtkTreeSelection *selection, gpointer data)
{ SchemaEditor *editor = data; if (!editor->updating) { editor->step_index = selected_index(selection); step_load(editor); } }
static void step_new(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data; editor->step_index = -1;
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->steps))); step_load(editor);
    JsonArray *steps = arr(target_settings(editor, definition(editor), FALSE), "automation_steps"); gint order = 1;
    for (; order <= 99; ++order) {
        gboolean found = FALSE; for (guint i = 0; i < count(steps); ++i) found |= number(at(steps, i), "order", 0) == order;
        if (!found) break;
    }
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->step_order), MIN(order, 99));
}
static void step_apply(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data; if (!editor->definition) return;
    gint order = 0; if (!spin_value(editor->step_order, 1, 99, &order)) { notice(editor, "İç sıra 1–99 arasında tam sayı olmalı."); return; }
    JsonArray *steps = arr(target_settings(editor, definition(editor), FALSE), "automation_steps");
    if (editor->step_index < 0 && count(steps) >= 32) { notice(editor, "Bir tanımda en fazla 32 iç sıra kaydı olabilir."); return; }
    for (guint i = 0; i < count(steps); ++i) if ((gint)i != editor->step_index && number(at(steps, i), "order", 0) == order) {
        notice(editor, "Bu iç sıra numarası zaten kullanılıyor; kaydı seçerek düzenleyin."); return;
    }
    const gchar *kind = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->step_kind)); if (!kind) return;
    JsonObject *row = json_object_new(); json_object_set_int_member(row, "order", order); json_object_set_string_member(row, "kind", kind);
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->step_channels_enabled))) {
        if (strcmp(kind, "music") && strcmp(kind, "talk") && strcmp(kind, "dj")) {
            notice(editor, "Mixer kanalları yalnız Müzik, Konuşma veya DJ kaydına atanabilir."); json_object_unref(row); return;
        }
        json_object_set_array_member(row, "channels", radios_ui_channel_picker_selected(editor->step_channels));
    }
    if (!transition_read(&editor->step_transition, row)) { json_object_unref(row); return; }
    JsonNode *candidate = clone(editor->definition); JsonObject *owner = target_settings(editor, json_node_get_object(candidate), TRUE);
    if (!owner) { json_object_unref(row); json_node_free(candidate); notice(editor, "Yayın adımı değişti; hedefi yeniden seçin."); return; }
    array_put(owner, "automation_steps", editor->step_index, row); json_object_unref(row);
    if (editor->step_index < 0) editor->step_index = (gint)count(arr(owner, "automation_steps")) - 1;
    commit(editor, candidate);
}
static void step_delete(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data; if (editor->step_index < 0 || !editor->definition) return;
    JsonNode *candidate = clone(editor->definition); JsonObject *owner = target_settings(editor, json_node_get_object(candidate), FALSE);
    if (!owner) { json_node_free(candidate); return; }
    array_put(owner, "automation_steps", editor->step_index, NULL); editor->step_index = -1; commit(editor, candidate);
}
static void step_target_changed(GtkComboBox *combo, gpointer data)
{ (void)combo; SchemaEditor *editor = data; if (!editor->updating) { editor->step_index = -1; render(editor); } }
static void open_routing(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data;
    const gchar *target = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->step_target));
    JsonObject *source = source_by_id(definition(editor), target);
    if (source && g_strcmp0(str(source, "source_type"), "live") && editor->routing) {
        gchar *id = g_strdup(str(source, "id")); editor->routing(editor->root, id, editor->routing_user); g_free(id);
    }
}
static const gchar *link_kind(SchemaEditor *editor)
{ return source_kind(editor, source_by_id(definition(editor), gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->link_to)))); }
static void link_load(SchemaEditor *editor)
{
    JsonObject *row = at(arr(settings(editor), "link_transitions"), editor->link_index);
    gboolean updating = editor->updating; editor->updating = TRUE;
    if (row) {
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->link_from), str(row, "from_source_id"))) gtk_combo_box_set_active(GTK_COMBO_BOX(editor->link_from), -1);
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->link_to), str(row, "to_source_id"))) gtk_combo_box_set_active(GTK_COMBO_BOX(editor->link_to), -1);
    }
    editor->updating = updating;
    form_kind(&editor->link_transition, link_kind(editor));
    transition_load(&editor->link_transition, row); gtk_widget_set_sensitive(editor->link_remove, row != NULL);
}
/* Changing the incoming step or the record's kind re-offers the presets
 * that kind may use, keeping the current choice when it still fits. */
static void link_to_changed(GtkComboBox *combo, gpointer data)
{
    (void)combo; SchemaEditor *editor = data; if (editor->updating) return;
    gchar *keep = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->link_transition.choice)));
    form_kind(&editor->link_transition, link_kind(editor)); transition_choices(&editor->link_transition, keep); g_free(keep);
    transition_visibility(&editor->link_transition);
}
static void step_kind_changed(GtkComboBox *combo, gpointer data)
{
    SchemaEditor *editor = data; if (editor->updating) return;
    gchar *keep = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->step_transition.choice)));
    form_kind(&editor->step_transition, gtk_combo_box_get_active_id(combo)); transition_choices(&editor->step_transition, keep); g_free(keep);
    transition_visibility(&editor->step_transition);
}
static void link_select(GtkTreeSelection *selection, gpointer data)
{ SchemaEditor *editor = data; if (!editor->updating) { editor->link_index = selected_index(selection); link_load(editor); } }
static void link_new(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data; editor->link_index = -1;
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->links))); link_load(editor);
    /* A new link proposes the step that follows its outgoing one. */
    gint from = source_order(editor, editor->link_from); JsonObject *next = from ? flow_next(editor, (guint)from - 1) : NULL;
    if (next) gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->link_to), str(next, "id"));
}
static void link_apply(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data; if (!editor->definition) return;
    const gchar *from = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->link_from));
    const gchar *to = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->link_to));
    if (!source_by_id(definition(editor), from) || !source_by_id(definition(editor), to)) { notice(editor, "Bağlantının çıkış ve giriş adımlarını seçin."); return; }
    JsonArray *links = arr(settings(editor), "link_transitions");
    if (editor->link_index < 0 && count(links) >= 50) { notice(editor, "Bir şemada en fazla 50 bağlantı olabilir."); return; }
    for (guint i = 0; i < count(links); ++i) if ((gint)i != editor->link_index && !g_strcmp0(str(at(links, i), "from_source_id"), from) && !g_strcmp0(str(at(links, i), "to_source_id"), to)) {
        notice(editor, "Bu bağlantı zaten var; kaydı seçerek düzenleyin."); return;
    }
    JsonObject *row = json_object_new(); json_object_set_string_member(row, "from_source_id", from); json_object_set_string_member(row, "to_source_id", to);
    if (!transition_read(&editor->link_transition, row)) { json_object_unref(row); return; }
    JsonNode *candidate = clone(editor->definition); JsonObject *owner = obj(json_node_get_object(candidate), "settings");
    if (!owner) { json_node_free(candidate); json_object_unref(row); return; }
    array_put(owner, "link_transitions", editor->link_index, row); json_object_unref(row);
    if (editor->link_index < 0) editor->link_index = (gint)count(arr(owner, "link_transitions")) - 1;
    commit(editor, candidate);
}
static void link_delete(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data; if (editor->link_index < 0 || !settings(editor)) return;
    JsonNode *candidate = clone(editor->definition); array_put(obj(json_node_get_object(candidate), "settings"), "link_transitions", editor->link_index, NULL);
    editor->link_index = -1; commit(editor, candidate);
}
static gint source_order(SchemaEditor *editor, GtkWidget *combo)
{
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)); JsonArray *sources = arr(definition(editor), "sources");
    for (guint i = 0; i < count(sources); ++i) if (!g_strcmp0(str(at(sources, i), "id"), id)) return (gint)i + 1;
    return 0;
}
static void select_order(SchemaEditor *editor, GtkWidget *combo, gint order)
{
    JsonObject *source = order > 0 ? at(arr(definition(editor), "sources"), (guint)order - 1) : NULL;
    if (!source || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), str(source, "id"))) gtk_combo_box_set_active(GTK_COMBO_BOX(combo), -1);
}
static void alternative_load(SchemaEditor *editor)
{
    JsonObject *row = at(arr(obj(settings(editor), "programme_entry"), "alternatives"), editor->alternative_index);
    select_order(editor, editor->alternative_source, number(row, "source_order", 1));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->alternative_weight), number(row, "weight", 0));
    transition_load(&editor->alternative_transition, row); gtk_widget_set_sensitive(editor->alternative_remove, row != NULL);
}
static void alternative_select(GtkTreeSelection *selection, gpointer data)
{ SchemaEditor *editor = data; if (!editor->updating) { editor->alternative_index = selected_index(selection); alternative_load(editor); } }
static void alternative_new(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data; editor->alternative_index = -1;
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->alternatives))); alternative_load(editor);
}
static void entry_apply(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data; if (!settings(editor)) return;
    gint order = source_order(editor, editor->entry_source); if (!order) { notice(editor, "Programa giriş için mevcut bir yayın adımı seçin."); return; }
    JsonNode *candidate = clone(editor->definition); JsonObject *owner = obj(json_node_get_object(candidate), "settings"), *entry = obj(owner, "programme_entry");
    if (!entry) { entry = json_object_new(); json_object_set_object_member(owner, "programme_entry", entry); }
    json_object_set_int_member(entry, "source_order", order);
    if (!transition_read(&editor->entry_transition, entry)) { json_node_free(candidate); return; }
    commit(editor, candidate);
}
static void entry_delete(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data; if (!obj(settings(editor), "programme_entry")) return;
    JsonNode *candidate = clone(editor->definition); json_object_remove_member(obj(json_node_get_object(candidate), "settings"), "programme_entry");
    editor->alternative_index = -1; commit(editor, candidate);
}
static void alternative_apply(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data;
    JsonObject *entry = obj(settings(editor), "programme_entry");
    if (!entry) { notice(editor, "Alternatif eklemeden önce varsayılan program girişini kaydedin."); return; }
    gint order = source_order(editor, editor->alternative_source), weight = 0;
    if (!order || !spin_value(editor->alternative_weight, 0, 100, &weight)) { notice(editor, "Alternatif adımı seçin; olasılık 0–100 arasında tam sayı olmalı."); return; }
    JsonArray *alternatives = arr(entry, "alternatives"); gint total = weight;
    if (editor->alternative_index < 0 && count(alternatives) >= 8) { notice(editor, "En fazla sekiz program giriş alternatifi eklenebilir."); return; }
    for (guint i = 0; i < count(alternatives); ++i) if ((gint)i != editor->alternative_index) total += number(at(alternatives, i), "weight", 0);
    if (total > 100) { notice(editor, "Alternatiflerin toplam olasılığı %100'ü aşamaz; kalan pay varsayılan girişindir."); return; }
    JsonObject *row = json_object_new(); json_object_set_int_member(row, "source_order", order); json_object_set_int_member(row, "weight", weight);
    if (!transition_read(&editor->alternative_transition, row)) { json_object_unref(row); return; }
    JsonNode *candidate = clone(editor->definition); JsonObject *owner = obj(obj(json_node_get_object(candidate), "settings"), "programme_entry");
    array_put(owner, "alternatives", editor->alternative_index, row); json_object_unref(row);
    if (editor->alternative_index < 0) editor->alternative_index = (gint)count(arr(owner, "alternatives")) - 1;
    commit(editor, candidate);
}
static void alternative_delete(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data; if (editor->alternative_index < 0 || !obj(settings(editor), "programme_entry")) return;
    JsonNode *candidate = clone(editor->definition); array_put(obj(obj(json_node_get_object(candidate), "settings"), "programme_entry"), "alternatives", editor->alternative_index, NULL);
    editor->alternative_index = -1; commit(editor, candidate);
}
static void sources_fill(SchemaEditor *editor, GtkWidget *combo, gboolean schema)
{
    gchar *selected = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo))); gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    if (schema) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "schema", "Aktif şemanın iç sırası");
    JsonArray *sources = arr(definition(editor), "sources");
    for (guint i = 0; i < count(sources); ++i) {
        JsonObject *source = at(sources, i); gchar *caption = source_title(editor, source, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), str(source, "id"), caption); g_free(caption);
    }
    if (!selected || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), selected)) gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    g_free(selected);
}
/* ---- Kılavuz §02–§04: "Yayın sırası ve geçişler" -------------------------
 * The schema's outer order as the guide's table: each step with its type,
 * the players (a cluster, or one unclustered player) that play it and the
 * existing automation assigned to the link into the next step (the last one
 * hands back to the first: a schema repeats). Writes sources[].autodj and
 * link_transitions, the same contract the planner reads. */
static const gchar *const pool_kinds[] = {"music", "advertisement", "jingle", "sweeper"};
static gboolean is_live(JsonObject *source) { return !g_strcmp0(str(source, "source_type"), "live"); }
static gchar *step_type(SchemaEditor *editor, JsonObject *source)
{
    if (!is_live(source)) {
        const gchar *kind = source_kind(editor, source);
        return g_strdup(!g_strcmp0(kind, "advertisement") ? "Reklam" : !g_strcmp0(kind, "jingle") ? "Jingle" : !g_strcmp0(kind, "sweeper") ? "Sweeper" : "Müzik");
    }
    JsonObject *live = obj(source, "live"); gint duration = number(live, "duration_ms", 0);
    gchar *rule = !g_strcmp0(str(live, "end_rule"), "duration") ? g_strdup_printf("%d dk", MAX(1, (duration + 59999) / 60000)) : g_strdup("elle");
    gchar *caption = g_strdup_printf("Live · %s · %s", kind_title(source_kind(editor, source)), rule); g_free(rule); return caption;
}
static JsonObject *player(SchemaEditor *editor, gint deck)
{
    for (guint i = 0; i < count(editor->players); ++i) if (number(at(editor->players, i), "deck", 0) == deck) return at(editor->players, i);
    return NULL;
}
static guint32 deck_mask(JsonArray *decks)
{
    guint32 mask = 0;
    for (guint i = 0; i < count(decks); ++i) {
        JsonNode *node = json_array_get_element(decks, i);
        gint64 deck = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 ? json_node_get_int(node) : 0;
        if (deck >= 1 && deck <= 32) mask |= (guint32)1 << (deck - 1);
    }
    return mask;
}
static guint32 cluster_mask(SchemaEditor *editor, const gchar *cluster)
{
    guint32 mask = 0;
    for (guint i = 0; i < count(editor->players); ++i) {
        JsonObject *row = at(editor->players, i); gint deck = number(row, "deck", 0);
        if (deck >= 1 && deck <= 32 && *cluster && !g_strcmp0(str(row, "cluster_id"), cluster)) mask |= (guint32)1 << (deck - 1);
    }
    return mask;
}
static gchar *mask_caption(SchemaEditor *editor, guint32 mask)
{
    GString *text = g_string_new(NULL), *numbers = g_string_new(NULL); gboolean plain = TRUE;
    for (gint deck = 1; deck <= 32; ++deck) if (mask & ((guint32)1 << (deck - 1))) {
        const gchar *name = str(player(editor, deck), "name"); gchar *standard = g_strdup_printf("Oynatıcı %d", deck);
        plain &= !*name || !g_strcmp0(name, standard);
        g_string_append_printf(text, "%s%s", text->len ? ", " : "", *name ? name : standard);
        g_string_append_printf(numbers, "%s%d", numbers->len ? ", " : "", deck); g_free(standard);
    }
    gchar *caption = plain && numbers->len ? g_strdup_printf("Oynatıcı %s", numbers->str) : g_strdup(text->str);
    g_string_free(text, TRUE); g_string_free(numbers, TRUE); return caption;
}
/* The step's player choice: "default" (no own pool: the schema's AutoDJ
 * players), "cluster:<id>", "player:<deck>" or "custom" (kind-specific). */
static gchar *players_choice(SchemaEditor *editor, JsonObject *source)
{
    JsonObject *autodj = obj(source, "autodj"); if (!autodj) return g_strdup("default");
    guint32 mask = deck_mask(arr(autodj, pool_kinds[0]));
    for (guint k = 1; k < G_N_ELEMENTS(pool_kinds); ++k) if (deck_mask(arr(autodj, pool_kinds[k])) != mask) return g_strdup("custom");
    if (!mask) return g_strdup("custom");
    for (guint i = 0; i < count(editor->players); ++i) {
        const gchar *cluster = str(at(editor->players, i), "cluster_id");
        if (*cluster && cluster_mask(editor, cluster) == mask) return g_strdup_printf("cluster:%s", cluster);
    }
    if (!(mask & (mask - 1))) {
        gint deck = g_bit_nth_lsf(mask, -1) + 1; JsonObject *row = player(editor, deck);
        if (row && !*str(row, "cluster_id")) return g_strdup_printf("player:%d", deck);
    }
    return g_strdup("custom");
}
static gchar *players_caption(SchemaEditor *editor, JsonObject *source)
{
    if (is_live(source)) return g_strdup("—");
    gchar *choice = players_choice(editor, source), *caption = NULL;
    if (!g_strcmp0(choice, "default")) caption = g_strdup("Şemanın AutoDJ oynatıcıları");
    else if (g_str_has_prefix(choice, "cluster:")) {
        const gchar *name = "";
        for (guint i = 0; i < count(editor->players) && !*name; ++i)
            if (!g_strcmp0(str(at(editor->players, i), "cluster_id"), choice + 8)) name = str(at(editor->players, i), "cluster_name");
        gchar *members = mask_caption(editor, cluster_mask(editor, choice + 8));
        caption = g_strdup_printf("Küme %s · %s", *name ? name : "", members); g_free(members);
    } else {
        JsonObject *autodj = obj(source, "autodj"); guint32 mask = 0;
        for (guint k = 0; k < G_N_ELEMENTS(pool_kinds); ++k) mask |= deck_mask(arr(autodj, pool_kinds[k]));
        gchar *members = mask_caption(editor, mask);
        caption = !g_strcmp0(choice, "custom") ? g_strdup_printf("Türe göre · %s", members) : g_strdup(members); g_free(members);
    }
    g_free(choice); return caption;
}
static gint link_index_for(JsonObject *owner, const gchar *from, const gchar *to)
{
    JsonArray *links = arr(owner, "link_transitions");
    for (guint i = 0; i < count(links); ++i)
        if (!g_strcmp0(str(at(links, i), "from_source_id"), from) && !g_strcmp0(str(at(links, i), "to_source_id"), to)) return (gint)i;
    return -1;
}
static JsonObject *flow_next(SchemaEditor *editor, guint index)
{
    JsonArray *sources = arr(definition(editor), "sources");
    return count(sources) > 1 ? at(sources, (index + 1) % count(sources)) : NULL;
}
static void flow_players_fill(SchemaEditor *editor, JsonObject *source)
{
    GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT(editor->flow_players); gtk_combo_box_text_remove_all(combo);
    gtk_combo_box_text_append(combo, "default", "Şemanın AutoDJ oynatıcıları");
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < count(editor->players); ++i) {
        JsonObject *row = at(editor->players, i); const gchar *cluster = str(row, "cluster_id");
        if (!*cluster || g_hash_table_contains(seen, cluster)) continue;
        g_hash_table_add(seen, (gpointer)cluster);
        gchar *id = g_strdup_printf("cluster:%s", cluster), *members = mask_caption(editor, cluster_mask(editor, cluster));
        gchar *caption = g_strdup_printf("Küme %s · %s", str(row, "cluster_name"), members);
        gtk_combo_box_text_append(combo, id, caption); g_free(id); g_free(members); g_free(caption);
    }
    g_hash_table_unref(seen);
    for (guint i = 0; i < count(editor->players); ++i) {
        JsonObject *row = at(editor->players, i); if (*str(row, "cluster_id")) continue;
        gchar *id = g_strdup_printf("player:%d", number(row, "deck", 0)), *caption = g_strdup_printf("Kümesiz · %s", str(row, "name"));
        gtk_combo_box_text_append(combo, id, caption); g_free(id); g_free(caption);
    }
    gchar *choice = source ? players_choice(editor, source) : g_strdup("default");
    if (!g_strcmp0(choice, "custom")) {
        gchar *caption = players_caption(editor, source); gtk_combo_box_text_append(combo, "custom", caption); g_free(caption);
    }
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), choice)) {
        /* A saved player that the live topology no longer has stays as it is. */
        gchar *caption = players_caption(editor, source); gtk_combo_box_text_append(combo, choice, caption); g_free(caption);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), choice);
    }
    g_free(choice);
}
static void flow_load(SchemaEditor *editor)
{
    JsonObject *source = at(arr(definition(editor), "sources"), editor->flow_index);
    gboolean updating = editor->updating; editor->updating = TRUE;
    flow_players_fill(editor, source);
    gboolean playable = source && !is_live(source);
    gtk_widget_set_sensitive(editor->flow_players, playable); gtk_widget_set_sensitive(editor->flow_detail, playable && editor->routing);
    JsonObject *next = source ? flow_next(editor, (guint)editor->flow_index) : NULL;
    gchar *caption = next ? source_title(editor, next, (guint)((editor->flow_index + 1) % (gint)count(arr(definition(editor), "sources")))) : g_strdup("—");
    gtk_label_set_text(GTK_LABEL(editor->flow_next), caption); g_free(caption);
    form_kind(&editor->flow_transition, next ? source_kind(editor, next) : NULL);
    gint index = source && next ? link_index_for(settings(editor), str(source, "id"), str(next, "id")) : -1;
    transition_load(&editor->flow_transition, at(arr(settings(editor), "link_transitions"), index));
    gtk_widget_set_sensitive(editor->flow_transition.root, next != NULL);
    gtk_widget_set_sensitive(editor->flow_save, source != NULL); editor->updating = updating;
}
static void flow_select(GtkTreeSelection *selection, gpointer data)
{ SchemaEditor *editor = data; if (!editor->updating) { editor->flow_index = selected_index(selection); flow_load(editor); } }
static void flow_detail(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data; JsonObject *source = at(arr(definition(editor), "sources"), editor->flow_index);
    if (source && !is_live(source) && editor->routing) { gchar *id = g_strdup(str(source, "id")); editor->routing(editor->root, id, editor->routing_user); g_free(id); }
}
static void flow_apply(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; SchemaEditor *editor = data;
    JsonObject *source = at(arr(definition(editor), "sources"), editor->flow_index);
    if (!source) { notice(editor, "Bir yayın adımı seçin."); return; }
    JsonNode *candidate = clone(editor->definition); JsonObject *root = json_node_get_object(candidate);
    JsonObject *target = at(arr(root, "sources"), editor->flow_index), *next = flow_next(editor, (guint)editor->flow_index);
    const gchar *choice = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->flow_players));
    if (!is_live(target) && choice && !g_str_equal(choice, "custom")) {
        guint32 mask = g_str_has_prefix(choice, "cluster:") ? cluster_mask(editor, choice + 8) :
            g_str_has_prefix(choice, "player:") ? (guint32)1 << CLAMP(atoi(choice + 7) - 1, 0, 31) : 0;
        if (g_str_equal(choice, "default")) json_object_remove_member(target, "autodj");
        else if (!mask) { json_node_free(candidate); notice(editor, "Seçilen kümede oynatıcı yok; oynatıcı ağacını denetleyin."); return; }
        else {
            JsonObject *autodj = obj(target, "autodj");
            if (!autodj) { autodj = json_object_new(); json_object_set_object_member(target, "autodj", autodj); }
            for (guint k = 0; k < G_N_ELEMENTS(pool_kinds); ++k) {
                JsonArray *decks = json_array_new();
                for (gint deck = 1; deck <= 32; ++deck) if (mask & ((guint32)1 << (deck - 1))) json_array_add_int_element(decks, deck);
                json_object_set_array_member(autodj, pool_kinds[k], decks);
            }
        }
    }
    if (next) {
        JsonObject *owner = obj(root, "settings");
        if (!owner) { owner = json_object_new(); json_object_set_object_member(root, "settings", owner); }
        JsonObject *row = json_object_new();
        json_object_set_string_member(row, "from_source_id", str(source, "id")); json_object_set_string_member(row, "to_source_id", str(next, "id"));
        if (!transition_read(&editor->flow_transition, row)) { json_object_unref(row); json_node_free(candidate); return; }
        gint index = link_index_for(owner, str(source, "id"), str(next, "id"));
        gboolean assigned = json_object_has_member(row, "preset_id") || json_object_has_member(row, "transition");
        if (assigned && index < 0 && count(arr(owner, "link_transitions")) >= 50) {
            json_object_unref(row); json_node_free(candidate); notice(editor, "Bir şemada en fazla 50 bağlantı olabilir."); return;
        }
        if (assigned || index >= 0) array_put(owner, "link_transitions", index, assigned ? row : NULL);
        json_object_unref(row);
    }
    commit(editor, candidate);
}
static void row_add(GtkListStore *store, GtkWidget *tree, gint index, gint selected, const gchar *first, const gchar *second, const gchar *third)
{
    GtkTreeIter row; gtk_list_store_append(store, &row); gtk_list_store_set(store, &row, 0, index, 1, first, 2, second, 3, third, -1);
    if (index == selected) gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), &row);
}
static void flow_render(SchemaEditor *editor)
{
    gtk_list_store_clear(editor->flow_store); JsonArray *sources = arr(definition(editor), "sources");
    if (editor->flow_index >= (gint)count(sources)) editor->flow_index = -1;
    if (editor->flow_index < 0 && count(sources)) editor->flow_index = 0;
    for (guint i = 0; i < count(sources); ++i) {
        JsonObject *source = at(sources, i), *next = flow_next(editor, i); GtkTreeIter row;
        gchar *order = g_strdup_printf("%u", i + 1), *title = source_title(editor, source, i), *type = step_type(editor, source);
        gchar *players = players_caption(editor, source), *transition = NULL;
        if (next) {
            JsonObject *link = at(arr(settings(editor), "link_transitions"), link_index_for(settings(editor), str(source, "id"), str(next, "id")));
            gchar *label_text = link ? transition_label(editor, link) : g_strdup("Atanmadı · varsayılan geçiş");
            transition = g_strdup_printf("→ %u · %s", (i + 1) % count(sources) + 1, label_text); g_free(label_text);
        } else transition = g_strdup("—");
        /* The step title without its order prefix ("1 · A playlist"). */
        const gchar *bare = strstr(title, " · "); bare = bare ? bare + strlen(" · ") : title;
        gtk_list_store_append(editor->flow_store, &row);
        gtk_list_store_set(editor->flow_store, &row, 0, (gint)i, 1, order, 2, bare, 3, type, 4, players, 5, transition, -1);
        if ((gint)i == editor->flow_index) gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->flow)), &row);
        g_free(order); g_free(title); g_free(type); g_free(players); g_free(transition);
    }
    flow_load(editor);
}
static void render(SchemaEditor *editor)
{
    editor->updating = TRUE;
    flow_render(editor);
    sources_fill(editor, editor->step_target, TRUE); sources_fill(editor, editor->link_from, FALSE); sources_fill(editor, editor->link_to, FALSE);
    sources_fill(editor, editor->entry_source, FALSE); sources_fill(editor, editor->alternative_source, FALSE);
    JsonObject *target = source_by_id(definition(editor), gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->step_target)));
    gtk_widget_set_sensitive(editor->step_routing, editor->routing && target && g_strcmp0(str(target, "source_type"), "live"));
    gtk_list_store_clear(editor->step_store);
    JsonArray *steps = arr(target_settings(editor, definition(editor), FALSE), "automation_steps");
    if (editor->step_index >= (gint)count(steps)) editor->step_index = -1;
    for (guint i = 0; i < count(steps); ++i) {
        JsonObject *row = at(steps, i); gchar *order = g_strdup_printf("%d", number(row, "order", 0)), *transition = transition_label(editor, row);
        const gchar *title = str(row, "kind");
        for (guint k = 0; k < G_N_ELEMENTS(kinds); ++k) if (!g_strcmp0(kinds[k], title)) { title = kind_titles[k]; break; }
        row_add(editor->step_store, editor->steps, (gint)i, editor->step_index, order, title, transition); g_free(order); g_free(transition);
    }
    step_load(editor); gtk_list_store_clear(editor->link_store);
    JsonArray *links = arr(settings(editor), "link_transitions"); if (editor->link_index >= (gint)count(links)) editor->link_index = -1;
    for (guint i = 0; i < count(links); ++i) {
        JsonObject *row = at(links, i); gchar *from = source_label(editor, str(row, "from_source_id")), *to = source_label(editor, str(row, "to_source_id")), *transition = transition_label(editor, row);
        row_add(editor->link_store, editor->links, (gint)i, editor->link_index, from, to, transition); g_free(from); g_free(to); g_free(transition);
    }
    link_load(editor);
    JsonObject *entry = obj(settings(editor), "programme_entry"); select_order(editor, editor->entry_source, number(entry, "source_order", 1));
    transition_load(&editor->entry_transition, entry); gtk_widget_set_sensitive(editor->entry_remove, entry != NULL);
    JsonArray *alternatives = arr(entry, "alternatives"); gint total = 0; gtk_list_store_clear(editor->alternative_store);
    if (editor->alternative_index >= (gint)count(alternatives)) editor->alternative_index = -1;
    for (guint i = 0; i < count(alternatives); ++i) {
        JsonObject *row = at(alternatives, i); gint order = number(row, "source_order", 1), weight = number(row, "weight", 0); total += weight;
        JsonObject *source = order > 0 ? at(arr(definition(editor), "sources"), (guint)order - 1) : NULL;
        gchar *caption = source ? source_title(editor, source, (guint)order - 1) : g_strdup("Kaldırılmış yayın adımı");
        gchar *percent = g_strdup_printf("%%%d", weight), *transition = transition_label(editor, row);
        row_add(editor->alternative_store, editor->alternatives, (gint)i, editor->alternative_index, caption, percent, transition);
        g_free(caption); g_free(percent); g_free(transition);
    }
    alternative_load(editor);
    gchar *summary = entry ? g_strdup_printf("Varsayılan giriş payı: %%%d", MAX(0, 100 - total)) : g_strdup("Giriş: ilk yayın adımı");
    gtk_label_set_text(GTK_LABEL(editor->entry_summary), summary); g_free(summary);
    gtk_widget_set_sensitive(editor->tabs, settings(editor) != NULL); editor->updating = FALSE;
}
static GtkWidget *actions(SchemaEditor *editor, const gchar *prefix, GCallback create, GCallback apply, GCallback remove, GtkWidget **remove_widget)
{
    GtkWidget *row = box(GTK_ORIENTATION_HORIZONTAL); gchar *name = g_strconcat(prefix, "-new", NULL);
    pack(row, button(editor, "Yeni kayıt", name, create), FALSE); g_free(name);
    name = g_strconcat(prefix, "-save", NULL); pack(row, button(editor, "Kaydı kaydet", name, apply), FALSE); g_free(name);
    name = g_strconcat(prefix, "-remove", NULL); *remove_widget = button(editor, "Kaydı kaldır", name, remove); g_free(name);
    pack(row, *remove_widget, FALSE); return row;
}
static void free_editor(gpointer data)
{
    SchemaEditor *editor = data; g_clear_pointer(&editor->definition, json_node_free); g_clear_pointer(&editor->playlists, json_array_unref);
    g_clear_pointer(&editor->players, json_array_unref); g_clear_pointer(&editor->library, json_array_unref); g_clear_object(&editor->flow_store);
    g_clear_pointer(&editor->channels, json_array_unref);
    TransitionForm *forms[] = {&editor->step_transition, &editor->link_transition, &editor->entry_transition, &editor->alternative_transition, &editor->flow_transition};
    for (guint i = 0; i < G_N_ELEMENTS(forms); ++i) g_clear_pointer(&forms[i]->kind, g_free);
    g_clear_object(&editor->step_store); g_clear_object(&editor->link_store); g_clear_object(&editor->alternative_store); g_free(editor);
}
GtkWidget *radios_schema_settings_new(JsonObject *initial, RadioSSchemaSettingsChanged changed, gpointer user)
{
    SchemaEditor *editor = g_new0(SchemaEditor, 1); editor->changed = changed; editor->user = user;
    editor->step_index = editor->link_index = editor->alternative_index = -1;
    editor->root = named(box(GTK_ORIENTATION_VERTICAL), "editor"); gtk_container_set_border_width(GTK_CONTAINER(editor->root), 12);
    editor->tabs = named(gtk_notebook_new(), "tabs"); gtk_notebook_set_scrollable(GTK_NOTEBOOK(editor->tabs), TRUE); pack(editor->root, editor->tabs, TRUE);

    GtkWidget *page = box(GTK_ORIENTATION_VERTICAL); gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    editor->flow_index = -1;
    editor->flow_store = gtk_list_store_new(6, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    editor->flow = named(gtk_tree_view_new_with_model(GTK_TREE_MODEL(editor->flow_store)), "flow");
    const gchar *flow_titles[] = {"Sıra", "Yayın adımı", "Tür", "Oynatıcı kümesi", "Sonraki adıma geçiş"};
    for (guint i = 0; i < G_N_ELEMENTS(flow_titles); ++i) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new(); g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(flow_titles[i], renderer, "text", (gint)i + 1, NULL);
        gtk_tree_view_column_set_expand(column, i > 0); gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(editor->flow), column);
    }
    pack(page, scroll(editor->flow), TRUE);
    GtkWidget *grid = form(); editor->flow_players = named(gtk_combo_box_text_new(), "flow-players");
    gtk_widget_set_tooltip_text(editor->flow_players, "Bu yayın adımını çalacak oynatıcı kümesi ya da kümesiz oynatıcı.");
    GtkWidget *players_row = box(GTK_ORIENTATION_HORIZONTAL); pack(players_row, editor->flow_players, TRUE);
    editor->flow_detail = button(editor, "Türe göre…", "flow-detail", G_CALLBACK(flow_detail));
    gtk_widget_set_tooltip_text(editor->flow_detail, "Müzik, reklam, jingle ve sweeper için ayrı oynatıcı seç.");
    pack(players_row, editor->flow_detail, FALSE); field(grid, "Oynatıcı kümesi", players_row, 0);
    editor->flow_next = named(label("—"), "flow-next"); field(grid, "Sonraki adım", editor->flow_next, 1);
    field(grid, "Sonraki adıma geçiş", transition_form(editor, &editor->flow_transition, "flow"), 2); pack(page, grid, FALSE);
    GtkWidget *flow_actions = box(GTK_ORIENTATION_HORIZONTAL);
    editor->flow_save = button(editor, "Adımı kaydet", "flow-save", G_CALLBACK(flow_apply)); pack(flow_actions, editor->flow_save, FALSE);
    pack(page, flow_actions, FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(editor->tabs), page, gtk_label_new("Yayın sırası ve geçişler"));

    page = box(GTK_ORIENTATION_VERTICAL); gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *target_row = box(GTK_ORIENTATION_HORIZONTAL);
    editor->step_target = named(gtk_combo_box_text_new(), "step-target"); pack(target_row, editor->step_target, TRUE);
    editor->step_routing = button(editor, "Oynatıcı kümesi…", "step-routing", G_CALLBACK(open_routing));
    gtk_widget_set_tooltip_text(editor->step_routing, "Bu yayın adımının içeriklerini çalacak mevcut oynatıcıları ve kümeyi seç.");
    pack(target_row, editor->step_routing, FALSE); pack(page, target_row, FALSE);
    editor->steps = table(&editor->step_store, "steps", "İç sıra", "Tür", "Atanan otomasyon"); pack(page, scroll(editor->steps), TRUE);
    gtk_widget_set_tooltip_text(editor->steps, "Şema, çıkış ve giriş playlistlerinde aynı numara ve uygun türdeki ilk kaydın otomasyonu uygulanır.");
    grid = form(); editor->step_order = spin("step-order", 1, 99, 1); editor->step_kind = named(gtk_combo_box_text_new(), "step-kind");
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->step_kind), kinds[i], kind_titles[i]);
    field(grid, "İç sıra", editor->step_order, 0); field(grid, "Beklenen tür", editor->step_kind, 1);
    editor->step_channels_enabled = named(gtk_check_button_new_with_label("Live event kanallarını bu kayıttan al"), "step-channels-enabled");
    /* Kullanıcı isteği 18.10 (U4): the defined channels in a folding tree. */
    editor->step_channels = radios_ui_channel_picker_new("schema-settings-step-channels");
    gtk_widget_set_halign(editor->step_channels, GTK_ALIGN_START);
    field(grid, "Mixer bağlantısı", editor->step_channels_enabled, 2); field(grid, "Mixer kanalları", editor->step_channels, 3);
    field(grid, "Atanan otomasyon", transition_form(editor, &editor->step_transition, "step"), 4); pack(page, grid, FALSE);
    pack(page, actions(editor, "step", G_CALLBACK(step_new), G_CALLBACK(step_apply), G_CALLBACK(step_delete), &editor->step_remove), FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(editor->tabs), page, gtk_label_new("İç sıra ve türler"));

    page = box(GTK_ORIENTATION_VERTICAL); gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    editor->links = table(&editor->link_store, "links", "Çıkış adımı", "Giriş adımı", "Varsayılan otomasyon"); pack(page, scroll(editor->links), TRUE);
    gtk_widget_set_tooltip_text(editor->links, "Ortak ve uygun iç sıra yoksa iki yayın adımı arasına atanan geçiş uygulanır; giriş adımının türü kullanılabilecek otomasyonu belirler.");
    grid = form(); editor->link_from = named(gtk_combo_box_text_new(), "link-from"); editor->link_to = named(gtk_combo_box_text_new(), "link-to");
    field(grid, "Çıkış", editor->link_from, 0); field(grid, "Giriş", editor->link_to, 1);
    field(grid, "Atanan otomasyon", transition_form(editor, &editor->link_transition, "link"), 2); pack(page, grid, FALSE);
    pack(page, actions(editor, "link", G_CALLBACK(link_new), G_CALLBACK(link_apply), G_CALLBACK(link_delete), &editor->link_remove), FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(editor->tabs), page, gtk_label_new("Adımlar arası geçiş"));

    page = box(GTK_ORIENTATION_VERTICAL); gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    grid = form(); editor->entry_source = named(gtk_combo_box_text_new(), "entry-source"); field(grid, "Varsayılan giriş", editor->entry_source, 0);
    gtk_widget_set_tooltip_text(editor->entry_source, "Bu şemayla çalışan programa girilecek yayın adımı.");
    field(grid, "Giriş otomasyonu", transition_form(editor, &editor->entry_transition, "entry"), 1); pack(page, grid, FALSE);
    GtkWidget *row = box(GTK_ORIENTATION_HORIZONTAL); pack(row, button(editor, "Girişi kaydet", "entry-save", G_CALLBACK(entry_apply)), FALSE);
    editor->entry_remove = button(editor, "Girişi ve alternatifleri kaldır", "entry-remove", G_CALLBACK(entry_delete)); pack(row, editor->entry_remove, FALSE); pack(page, row, FALSE);
    editor->entry_summary = named(label(""), "entry-summary"); pack(page, editor->entry_summary, FALSE);
    editor->alternatives = table(&editor->alternative_store, "alternatives", "Alternatif giriş", "Olasılık", "Giriş otomasyonu"); pack(page, scroll(editor->alternatives), TRUE);
    grid = form(); editor->alternative_source = named(gtk_combo_box_text_new(), "alternative-source"); editor->alternative_weight = spin("alternative-weight", 0, 100, 0);
    field(grid, "Alternatif adım", editor->alternative_source, 0); field(grid, "Olasılık (%)", editor->alternative_weight, 1);
    field(grid, "Giriş otomasyonu", transition_form(editor, &editor->alternative_transition, "alternative"), 2); pack(page, grid, FALSE);
    pack(page, actions(editor, "alternative", G_CALLBACK(alternative_new), G_CALLBACK(alternative_apply), G_CALLBACK(alternative_delete), &editor->alternative_remove), FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(editor->tabs), scroll(page), gtk_label_new("Programdan programa"));

    editor->status = named(label(""), "status"); pack(editor->root, editor->status, FALSE);
    g_signal_connect(editor->step_target, "changed", G_CALLBACK(step_target_changed), editor);
    g_signal_connect(editor->step_kind, "changed", G_CALLBACK(step_kind_changed), editor);
    g_signal_connect(editor->link_to, "changed", G_CALLBACK(link_to_changed), editor);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->flow)), "changed", G_CALLBACK(flow_select), editor);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->steps)), "changed", G_CALLBACK(step_select), editor);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->links)), "changed", G_CALLBACK(link_select), editor);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->alternatives)), "changed", G_CALLBACK(alternative_select), editor);
    g_object_set_data_full(G_OBJECT(editor->root), "radios-schema-settings", editor, free_editor);
    editor->definition = clone_object(initial); render(editor); gtk_widget_show_all(editor->root);
    return editor->root;
}
void radios_schema_settings_set_config(GtkWidget *widget, JsonObject *config)
{
    SchemaEditor *editor = widget ? g_object_get_data(G_OBJECT(widget), "radios-schema-settings") : NULL; if (!editor) return;
    JsonNode *copy = clone_object(config);
    if (copy && editor->definition && json_node_equal(copy, editor->definition)) { json_node_free(copy); return; }
    if (g_strcmp0(str(definition(editor), "id"), str(config, "id"))) editor->step_index = editor->link_index = editor->alternative_index = -1;
    g_clear_pointer(&editor->definition, json_node_free); editor->definition = copy; render(editor);
}
void radios_schema_settings_set_playlists(GtkWidget *widget, JsonArray *playlists)
{
    SchemaEditor *editor = widget ? g_object_get_data(G_OBJECT(widget), "radios-schema-settings") : NULL; if (!editor) return;
    if (!playlists && !editor->playlists) return;
    JsonNode *node = json_node_new(JSON_NODE_ARRAY), *previous = json_node_new(JSON_NODE_ARRAY);
    if (playlists) json_node_set_array(node, playlists);
    if (editor->playlists) json_node_set_array(previous, editor->playlists);
    gboolean same = playlists && editor->playlists && json_node_equal(node, previous); json_node_free(previous);
    JsonNode *copy = playlists && !same ? clone(node) : NULL; json_node_free(node);
    if (same) return;
    g_clear_pointer(&editor->playlists, json_array_unref);
    if (copy) { editor->playlists = json_array_ref(json_node_get_array(copy)); json_node_free(copy); }
    /* A catalogue notification changes captions, never the in-progress form
     * or its selected record. Reloading the complete form here discarded
     * unsaved orders/automation choices whenever playlist names refreshed. */
    gboolean updating = editor->updating; editor->updating = TRUE;
    GtkWidget *choices[] = {editor->step_target, editor->link_from, editor->link_to, editor->entry_source, editor->alternative_source};
    for (guint i = 0; i < G_N_ELEMENTS(choices); ++i) sources_fill(editor, choices[i], i == 0);
    GtkTreeIter iter;
    gboolean more = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(editor->link_store), &iter);
    while (more) {
        gint index = -1; gtk_tree_model_get(GTK_TREE_MODEL(editor->link_store), &iter, 0, &index, -1);
        JsonObject *row = at(arr(settings(editor), "link_transitions"), index);
        gchar *from = source_label(editor, str(row, "from_source_id")), *to = source_label(editor, str(row, "to_source_id"));
        gtk_list_store_set(editor->link_store, &iter, 1, from, 2, to, -1); g_free(from); g_free(to);
        more = gtk_tree_model_iter_next(GTK_TREE_MODEL(editor->link_store), &iter);
    }
    more = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(editor->alternative_store), &iter);
    while (more) {
        gint index = -1; gtk_tree_model_get(GTK_TREE_MODEL(editor->alternative_store), &iter, 0, &index, -1);
        JsonObject *row = at(arr(obj(settings(editor), "programme_entry"), "alternatives"), index);
        gint order = number(row, "source_order", 1);
        JsonObject *source = order > 0 ? at(arr(definition(editor), "sources"), (guint)order - 1) : NULL;
        gchar *caption = source ? source_title(editor, source, (guint)order - 1) : g_strdup("Kaldırılmış yayın adımı");
        gtk_list_store_set(editor->alternative_store, &iter, 1, caption, -1); g_free(caption);
        more = gtk_tree_model_iter_next(GTK_TREE_MODEL(editor->alternative_store), &iter);
    }
    editor->updating = updating;
}
/* The live player topology the "Oynatıcı kümesi" choices come from: one
 * object per player {deck 1..32, name, cluster_id, cluster_name}. */
void radios_schema_settings_set_players(GtkWidget *widget, JsonArray *players)
{
    SchemaEditor *editor = widget ? g_object_get_data(G_OBJECT(widget), "radios-schema-settings") : NULL; if (!editor) return;
    JsonNode *node = json_node_new(JSON_NODE_ARRAY); if (players) json_node_set_array(node, players);
    JsonNode *copy = players ? clone(node) : NULL; json_node_free(node);
    g_clear_pointer(&editor->players, json_array_unref);
    if (copy) { editor->players = json_array_ref(json_node_get_array(copy)); json_node_free(copy); }
    gboolean updating = editor->updating; editor->updating = TRUE; flow_render(editor); editor->updating = updating;
}
/* Kılavuz §02 "mevcut otomasyonlar": every preset of the station's saved
 * rotations, with the kinds its own rotation typed it for (empty = any). */
void radios_schema_settings_set_library(GtkWidget *widget, JsonArray *rotation_library)
{
    SchemaEditor *editor = widget ? g_object_get_data(G_OBJECT(widget), "radios-schema-settings") : NULL; if (!editor) return;
    JsonArray *items = json_array_new(); GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint d = 0; d < count(rotation_library); ++d) {
        JsonObject *rotation = at(rotation_library, d), *rotation_settings = obj(rotation, "settings");
        if (!g_strcmp0(str(rotation, "id"), str(definition(editor), "id"))) continue;
        JsonArray *presets = arr(rotation_settings, "autodj_presets");
        for (guint i = 0; i < count(presets); ++i) {
            JsonObject *value = at(presets, i); const gchar *id = str(value, "id");
            if (!g_uuid_string_is_valid(id) || g_hash_table_contains(seen, id)) continue;
            g_hash_table_add(seen, (gpointer)id);
            JsonObject *item = json_object_new(); JsonArray *typed = json_array_new(); gboolean everywhere = TRUE;
            for (guint k = 0; k < G_N_ELEMENTS(kinds); ++k) {
                if (radios_automation_preset_kind_usable(rotation_settings, id, kinds[k])) json_array_add_string_element(typed, kinds[k]);
                else everywhere = FALSE;
            }
            if (everywhere) { json_array_unref(typed); typed = json_array_new(); }
            json_object_set_member(item, "preset", clone_object(value)); json_object_set_array_member(item, "kinds", typed);
            json_object_set_string_member(item, "origin", *str(rotation, "name") ? str(rotation, "name") : "Rotasyon");
            json_array_add_object_element(items, item);
        }
    }
    g_hash_table_unref(seen); g_clear_pointer(&editor->library, json_array_unref); editor->library = items;
    /* New choices for the forms; the selected records and drafts stay. */
    TransitionForm *forms[] = {&editor->step_transition, &editor->link_transition, &editor->entry_transition, &editor->alternative_transition, &editor->flow_transition};
    gboolean updating = editor->updating; editor->updating = TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(forms); ++i) {
        gchar *keep = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(forms[i]->choice)));
        transition_choices(forms[i], keep); transition_visibility(forms[i]); g_free(keep);
    }
    flow_render(editor); editor->updating = updating;
}
void radios_schema_settings_set_channels(GtkWidget *widget, JsonArray *channels)
{
    SchemaEditor *editor = widget ? g_object_get_data(G_OBJECT(widget), "radios-schema-settings") : NULL; if (!editor) return;
    JsonNode *node = json_node_new(JSON_NODE_ARRAY); if (channels) json_node_set_array(node, channels);
    JsonNode *copy = channels ? clone(node) : NULL; json_node_free(node);
    g_clear_pointer(&editor->channels, json_array_unref);
    if (copy) { editor->channels = json_array_ref(json_node_get_array(copy)); json_node_free(copy); }
    radios_ui_channel_picker_set_channels(editor->step_channels, editor->channels);
}
JsonNode *radios_schema_settings_snapshot(GtkWidget *widget)
{
    SchemaEditor *editor = widget ? g_object_get_data(G_OBJECT(widget), "radios-schema-settings") : NULL; return editor ? clone(editor->definition) : NULL;
}
void radios_schema_settings_set_routing_handler(GtkWidget *widget,
    void (*handler)(GtkWidget *, const gchar *, gpointer), gpointer user)
{
    SchemaEditor *editor = widget ? g_object_get_data(G_OBJECT(widget), "radios-schema-settings") : NULL; if (!editor) return;
    editor->routing = handler; editor->routing_user = user;
    JsonObject *target = source_by_id(definition(editor), gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->step_target)));
    gtk_widget_set_sensitive(editor->step_routing, handler && target && g_strcmp0(str(target, "source_type"), "live"));
    JsonObject *step = at(arr(definition(editor), "sources"), editor->flow_index);
    gtk_widget_set_sensitive(editor->flow_detail, handler && step && !is_live(step));
}
