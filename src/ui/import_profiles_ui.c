#define _POSIX_C_SOURCE 200809L
#include "radios/import_profiles_ui.h"
#include "radios/data.h"
#include <string.h>

/* Ayarlar › Import › Rekordbox içe aktarma profilleri (Kılavuz 18.10 R10.1):
 * the import profile API of the data service and the profile-based rekordbox
 * XML import. A profile keeps the stable library key of a DJ library, the cue
 * policy, source → local path rules and the allowed media roots; the XML
 * import previews with them and applies exactly the previewed hash. */
#define SECTION_KEY "radios-import-profiles"
#define XML_LIMIT (60u * 1024u * 1024u) /* the service accepts 86 MB of base64 */
#define PREVIEW 1

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *root, *list, *status, *add, *edit, *xml;
    GtkListStore *store;
    JsonNode *profiles; /* array as last read */
    guint generation;
} Profiles;

enum { COL_NAME, COL_KEY, COL_CUE, COL_RULES, COL_STATE, COL_ID, COL_COUNT };
static const gchar *const cue_ids[] = {"preserve_as_dj", "propose_radio_cues", "ignore"};
static const gchar *const cue_names[] = {"DJ bilgisi olarak sakla", "Radyo cue önerisi", "Aktarma"};

static void pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static GtkWidget *named(GtkWidget *widget, const gchar *name) { gtk_widget_set_name(widget, name); return widget; }
static GtkWidget *text(const gchar *value, const gchar *style)
{
    GtkWidget *label = gtk_label_new(value); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE); gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    if (style) gtk_style_context_add_class(gtk_widget_get_style_context(label), style);
    return label;
}
static JsonObject *object_of(JsonNode *node) { return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL; }
static JsonObject *member_object(JsonObject *parent, const gchar *name)
{ return parent && json_object_has_member(parent, name) ? object_of(json_object_get_member(parent, name)) : NULL; }
static JsonArray *member_array(JsonObject *parent, const gchar *name)
{
    JsonNode *node = parent && json_object_has_member(parent, name) ? json_object_get_member(parent, name) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static const gchar *cue_name(const gchar *id)
{
    for (guint i = 0; i < G_N_ELEMENTS(cue_ids); ++i) if (!g_strcmp0(cue_ids[i], id)) return cue_names[i];
    return cue_names[0];
}
static void status_set(Profiles *p, const gchar *message) { gtk_label_set_text(GTK_LABEL(p->status), message ? message : ""); }
static void profiles_free(gpointer data)
{ Profiles *p = data; if (p->profiles) json_node_free(p->profiles); g_free(p); }

static JsonObject *profile_by_id(Profiles *p, const gchar *id)
{
    JsonArray *rows = p->profiles && JSON_NODE_HOLDS_ARRAY(p->profiles) ? json_node_get_array(p->profiles) : NULL;
    for (guint i = 0; id && rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (row && g_str_equal(radios_json_string(row, "_id", ""), id)) return row;
    }
    return NULL;
}
static JsonObject *selected(Profiles *p)
{
    GtkTreeModel *model; GtkTreeIter iter; gchar *id = NULL;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->list)), &model, &iter)) return NULL;
    gtk_tree_model_get(model, &iter, COL_ID, &id, -1);
    JsonObject *row = profile_by_id(p, id); g_free(id); return row;
}
static void buttons_update(Profiles *p)
{
    JsonObject *row = selected(p); gboolean online = radios_data_connected(p->ctx);
    gtk_widget_set_sensitive(p->add, online);
    gtk_widget_set_sensitive(p->edit, online && row);
    gtk_widget_set_sensitive(p->xml, online && row && g_str_equal(radios_json_string(row, "status", ""), "active"));
}
static void selection_changed(GtkTreeSelection *selection, gpointer data) { (void)selection; buttons_update(data); }

static void render(Profiles *p, const gchar *keep)
{
    gtk_list_store_clear(p->store);
    JsonArray *rows = p->profiles && JSON_NODE_HOLDS_ARRAY(p->profiles) ? json_node_get_array(p->profiles) : NULL;
    GtkTreeIter iter, chosen; gboolean found = FALSE;
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i); if (!row) continue;
        JsonObject *config = member_object(row, "config"); JsonArray *rules = member_array(config, "path_rules");
        gchar *count = g_strdup_printf("%u", rules ? json_array_get_length(rules) : 0u);
        const gchar *id = radios_json_string(row, "_id", "");
        gtk_list_store_append(p->store, &iter);
        gtk_list_store_set(p->store, &iter, COL_NAME, radios_json_string(row, "name", ""),
            COL_KEY, radios_json_string(row, "library_key", ""),
            COL_CUE, cue_name(radios_json_string(config, "cue_policy", cue_ids[0])), COL_RULES, count,
            COL_STATE, g_str_equal(radios_json_string(row, "status", ""), "active") ? "Etkin" : "Devre dışı", COL_ID, id, -1);
        g_free(count);
        if (keep && g_str_equal(keep, id)) { chosen = iter; found = TRUE; }
    }
    if (found) gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->list)), &chosen);
    buttons_update(p);
}
static void listed(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer user)
{
    (void)ctx; Profiles *p = g_object_get_data(G_OBJECT(owner), SECTION_KEY);
    if (!p || GPOINTER_TO_UINT(user) != p->generation) return;
    if (error || !result || !JSON_NODE_HOLDS_ARRAY(result)) {
        status_set(p, error ? error->message : "İçe aktarma profilleri okunamadı."); return;
    }
    JsonObject *keep = selected(p); gchar *id = keep ? g_strdup(radios_json_string(keep, "_id", "")) : NULL;
    if (p->profiles) json_node_free(p->profiles);
    p->profiles = json_node_copy(result);
    render(p, id); g_free(id); status_set(p, NULL);
}
static void load(Profiles *p)
{
    buttons_update(p);
    const gchar *station = radios_data_station_id(p->ctx);
    if (!radios_data_connected(p->ctx) || !station || !*station) { status_set(p, "Veri servisi bağlı değil."); return; }
    gchar *escaped = g_uri_escape_string(station, NULL, FALSE), *path = g_strconcat("/v1/import-profiles?station_id=", escaped, NULL);
    ++p->generation;
    radios_data_request(p->ctx, p->root, "GET", path, NULL, listed, GUINT_TO_POINTER(p->generation), NULL);
    g_free(path); g_free(escaped);
}
static void refreshed(RadioSViewContext *ctx, GtkWidget *owner)
{ (void)ctx; Profiles *p = g_object_get_data(G_OBJECT(owner), SECTION_KEY); if (p) load(p); }
static void mapped(GtkWidget *widget, gpointer data) { (void)widget; load(data); }

/* --- profile editor ------------------------------------------------------ */
static GtkWidget *dialog_new(Profiles *p, const gchar *title, const gchar *name, GtkWidget **form)
{
    GtkWidget *toplevel = gtk_widget_get_toplevel(p->root);
    GtkWidget *dialog = named(gtk_dialog_new(), name);
    gtk_window_set_title(GTK_WINDOW(dialog), title); gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    if (GTK_IS_WINDOW(toplevel)) gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(toplevel));
    gtk_window_set_default_size(GTK_WINDOW(dialog), 620, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
    *form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_container_set_border_width(GTK_CONTAINER(*form), 18);
    pack(gtk_dialog_get_content_area(GTK_DIALOG(dialog)), *form, TRUE);
    g_object_set_data(G_OBJECT(dialog), SECTION_KEY, p);
    g_signal_connect_object(p->root, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    return dialog;
}
static GtkWidget *field(GtkWidget *form, const gchar *caption, GtkWidget *control)
{ pack(form, text(caption, NULL), FALSE); pack(form, control, FALSE); return control; }
static GtkWidget *lines_new(GtkWidget *dialog, GtkWidget *form, const gchar *caption, const gchar *name, const gchar *content)
{
    GtkWidget *view = named(gtk_text_view_new(), name);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_CHAR);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), content, -1);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 72);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    field(form, caption, scroll); g_object_set_data(G_OBJECT(dialog), name, view);
    return view;
}
static gchar *lines_text(GtkWidget *view)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)); GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end); return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}
/* "kaynak kök => yerel kök" per rule line; one absolute path per root line.
 * Returns the 1-based number of the first invalid line, 0 when all parse. */
static guint lines_parse(const gchar *content, gboolean rules, JsonArray **out)
{
    JsonArray *array = json_array_new(); gchar **lines = g_strsplit(content, "\n", -1); guint bad = 0;
    for (guint number = 1; lines[number - 1] && !bad; ++number) {
        gchar *value = g_strstrip(g_strdup(lines[number - 1]));
        if (*value && rules) {
            gchar *arrow = strstr(value, "=>");
            if (arrow) *arrow = '\0';
            gchar *source = g_strstrip(value), *target = arrow ? g_strstrip(arrow + 2) : NULL;
            if (!arrow || !*source || !*target || !g_path_is_absolute(target)) bad = number;
            else {
                JsonObject *rule = json_object_new();
                json_object_set_string_member(rule, "source_root", source); json_object_set_string_member(rule, "target_root", target);
                json_array_add_object_element(array, rule);
            }
        } else if (*value) {
            if (!g_path_is_absolute(value)) bad = number; else json_array_add_string_element(array, value);
        }
        g_free(value);
    }
    g_strfreev(lines);
    if (bad) json_array_unref(array); else *out = array;
    return bad;
}
static void editor_saved(RadioSViewContext *ctx, GtkWidget *dialog, JsonNode *result, const GError *error, gpointer user)
{
    (void)ctx; (void)user;
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "import-profile-status");
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, TRUE);
    JsonObject *saved = object_of(result);
    if (error || !saved) { gtk_label_set_text(GTK_LABEL(status), error ? error->message : "Profil kaydedilemedi."); return; }
    Profiles *p = g_object_get_data(G_OBJECT(dialog), SECTION_KEY);
    gchar *id = g_strdup(radios_json_string(saved, "_id", ""));
    gtk_widget_destroy(dialog);
    if (p) {
        /* Keep the saved profile selected once the list is read again. */
        if (!profile_by_id(p, id)) {
            JsonArray *rows = p->profiles && JSON_NODE_HOLDS_ARRAY(p->profiles) ? json_node_get_array(p->profiles) : NULL;
            if (!rows) { if (p->profiles) json_node_free(p->profiles); p->profiles = json_node_new(JSON_NODE_ARRAY); json_node_take_array(p->profiles, json_array_new()); rows = json_node_get_array(p->profiles); }
            json_array_add_object_element(rows, json_object_ref(saved));
        }
        render(p, id); load(p); status_set(p, "Profil kaydedildi.");
    }
    g_free(id);
}
static void editor_response(GtkDialog *dialog, gint response, gpointer data)
{
    Profiles *p = data;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "import-profile-status");
    gchar *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "import-profile-name")))));
    gchar *key = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "import-profile-library-key")))));
    gchar *rules_text = lines_text(g_object_get_data(G_OBJECT(dialog), "import-profile-path-rules"));
    gchar *roots_text = lines_text(g_object_get_data(G_OBJECT(dialog), "import-profile-roots"));
    JsonArray *rules = NULL, *roots = NULL; guint bad = 0;
    gchar *problem = NULL;
    if (!*name) problem = g_strdup("Profil adı boş bırakılamaz.");
    else if (!*key) problem = g_strdup("Kitaplık anahtarı boş bırakılamaz.");
    else if ((bad = lines_parse(rules_text, TRUE, &rules)))
        problem = g_strdup_printf("Yol kuralı %u: \"kaynak kök => yerel kök\" biçiminde, yerel kök mutlak yol olmalı.", bad);
    else if ((bad = lines_parse(roots_text, FALSE, &roots))) {
        problem = g_strdup_printf("İzinli kök %u mutlak bir yol olmalı.", bad); json_array_unref(rules); rules = NULL;
    }
    g_free(rules_text); g_free(roots_text);
    if (problem) { gtk_label_set_text(GTK_LABEL(status), problem); g_free(problem); g_free(name); g_free(key); return; }
    if (!radios_data_connected(p->ctx)) {
        gtk_label_set_text(GTK_LABEL(status), "Veri servisi bağlı değil; girdiler korunuyor.");
        json_array_unref(rules); json_array_unref(roots); g_free(name); g_free(key); return;
    }
    JsonObject *record = g_object_get_data(G_OBJECT(dialog), "import-profile-record");
    JsonNode *body = radios_data_body(p->ctx); JsonObject *request = json_node_get_object(body);
    json_object_set_string_member(request, "name", name); json_object_set_string_member(request, "library_key", key);
    g_free(name); g_free(key);
    /* Options this page does not edit (node/track selection, field
     * policies, category mapping) are kept as saved. */
    JsonObject *config = json_object_new(), *previous = member_object(record, "config");
    GList *members = previous ? json_object_get_members(previous) : NULL;
    for (GList *item = members; item; item = item->next)
        json_object_set_member(config, item->data, json_node_copy(json_object_get_member(previous, item->data)));
    g_list_free(members);
    GtkWidget *cue = g_object_get_data(G_OBJECT(dialog), "import-profile-cue");
    json_object_set_string_member(config, "cue_policy", cue_ids[CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(cue)), 0, 2)]);
    json_object_set_array_member(config, "path_rules", rules); json_object_set_array_member(config, "allowed_roots", roots);
    json_object_set_object_member(request, "config", config);
    GtkWidget *enabled = g_object_get_data(G_OBJECT(dialog), "import-profile-active");
    json_object_set_string_member(request, "status", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enabled)) ? "active" : "disabled");
    gchar *path;
    if (record) {
        path = g_strconcat("/v1/import-profiles/", radios_json_string(record, "_id", ""), NULL);
        json_object_set_int_member(request, "expected_revision", radios_json_int(record, "_rev", 0));
    } else path = g_strdup("/v1/import-profiles");
    if (radios_data_request(p->ctx, GTK_WIDGET(dialog), "POST", path, body, editor_saved, NULL, NULL)) {
        gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, FALSE);
        gtk_label_set_text(GTK_LABEL(status), "Profil kaydediliyor…");
    } else gtk_label_set_text(GTK_LABEL(status), "Veri servisi bağlı değil; girdiler korunuyor.");
    json_node_free(body); g_free(path);
}
static void editor_open(Profiles *p, JsonObject *record)
{
    GtkWidget *form, *dialog = dialog_new(p, record ? "İçe aktarma profilini düzenle" : "İçe aktarma profili ekle", "import-profile-dialog", &form);
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *name = named(gtk_entry_new(), "import-profile-name"); gtk_entry_set_max_length(GTK_ENTRY(name), 120);
    GtkWidget *key = named(gtk_entry_new(), "import-profile-library-key"); gtk_entry_set_max_length(GTK_ENTRY(key), 200);
    field(form, "Profil adı", name); field(form, "Kitaplık anahtarı", key);
    g_object_set_data(G_OBJECT(dialog), "import-profile-name", name); g_object_set_data(G_OBJECT(dialog), "import-profile-library-key", key);
    GtkWidget *cue = named(gtk_combo_box_text_new(), "import-profile-cue");
    for (guint i = 0; i < G_N_ELEMENTS(cue_ids); ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(cue), cue_ids[i], cue_names[i]);
    field(form, "Cue noktaları", cue); g_object_set_data(G_OBJECT(dialog), "import-profile-cue", cue);
    JsonObject *config = member_object(record, "config");
    GString *rules = g_string_new(""), *roots = g_string_new("");
    JsonArray *saved_rules = member_array(config, "path_rules"), *saved_roots = member_array(config, "allowed_roots");
    for (guint i = 0; saved_rules && i < json_array_get_length(saved_rules); ++i) {
        JsonObject *rule = json_array_get_object_element(saved_rules, i);
        g_string_append_printf(rules, "%s => %s\n", radios_json_string(rule, "source_root", ""), radios_json_string(rule, "target_root", ""));
    }
    for (guint i = 0; saved_roots && i < json_array_get_length(saved_roots); ++i)
        g_string_append_printf(roots, "%s\n", json_array_get_string_element(saved_roots, i));
    lines_new(dialog, form, "Yol kuralları · kaynak kök => yerel kök", "import-profile-path-rules", rules->str);
    lines_new(dialog, form, "İzinli medya kökleri", "import-profile-roots", roots->str);
    g_string_free(rules, TRUE); g_string_free(roots, TRUE);
    GtkWidget *enabled = named(gtk_check_button_new_with_label("Etkin"), "import-profile-active");
    pack(form, enabled, FALSE); g_object_set_data(G_OBJECT(dialog), "import-profile-active", enabled);
    GtkWidget *status = named(text("", "preferences-status"), "import-profile-status");
    pack(form, status, FALSE); g_object_set_data(G_OBJECT(dialog), "import-profile-status", status);
    gtk_entry_set_text(GTK_ENTRY(name), radios_json_string(record, "name", ""));
    gtk_entry_set_text(GTK_ENTRY(key), radios_json_string(record, "library_key", ""));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(cue), radios_json_string(config, "cue_policy", cue_ids[0]));
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(cue)) < 0) gtk_combo_box_set_active(GTK_COMBO_BOX(cue), 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enabled), !record || g_str_equal(radios_json_string(record, "status", "active"), "active"));
    if (record) g_object_set_data_full(G_OBJECT(dialog), "import-profile-record", json_object_ref(record), (GDestroyNotify)json_object_unref);
    g_signal_connect(dialog, "response", G_CALLBACK(editor_response), p);
    gtk_widget_show_all(dialog);
}
static void add_clicked(GtkButton *button, gpointer data) { (void)button; editor_open(data, NULL); }
static void edit_clicked(GtkButton *button, gpointer data)
{ (void)button; Profiles *p = data; JsonObject *row = selected(p); if (row) editor_open(p, row); }
static void row_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer data)
{ (void)view; (void)path; (void)column; edit_clicked(NULL, data); }

/* --- profile-based XML import ----------------------------------------- */
static void xml_summary(GtkWidget *dialog, const gchar *message)
{ gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(dialog), "import-profile-xml-summary")), message); }
static void xml_reset(GtkWidget *dialog)
{
    g_object_set_data(G_OBJECT(dialog), "import-xml-hash", NULL); g_object_set_data(G_OBJECT(dialog), "import-xml-key", NULL);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, FALSE);
}
static void xml_file_set(GtkFileChooserButton *chooser, gpointer dialog)
{ (void)chooser; g_object_set_data(G_OBJECT(dialog), "import-xml-base64", NULL); xml_reset(dialog); xml_summary(dialog, ""); }
static void xml_previewed(RadioSViewContext *ctx, GtkWidget *dialog, JsonNode *result, const GError *error, gpointer user)
{
    (void)ctx; (void)user;
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), PREVIEW, TRUE);
    JsonObject *root = object_of(result);
    if (error || !root || !*radios_json_string(root, "preview_hash", "")) {
        xml_summary(dialog, error ? error->message : "Önizleme alınamadı."); return;
    }
    JsonObject *counts = member_object(root, "counts"); JsonArray *entries = member_array(root, "entries");
    guint mapped = 0, unmapped = 0, invalid = 0;
    for (guint i = 0; entries && i < json_array_get_length(entries); ++i) {
        const gchar *state = radios_json_string(member_object(json_array_get_object_element(entries, i), "media_path"), "status", "");
        if (g_str_equal(state, "mapped")) ++mapped; else if (g_str_equal(state, "invalid")) ++invalid; else ++unmapped;
    }
    JsonArray *nodes = member_array(root, "playlist_nodes");
    gchar *summary = g_strdup_printf("Yeni %" G_GINT64_FORMAT " · Güncellenecek %" G_GINT64_FORMAT " · Korunacak %" G_GINT64_FORMAT
        " · Çakışma %" G_GINT64_FORMAT "\nDosya yolu: eşlendi %u · eşlenmedi %u · geçersiz %u · Playlist %u",
        radios_json_int(counts, "create", 0), radios_json_int(counts, "update", 0), radios_json_int(counts, "preserve", 0),
        radios_json_int(counts, "conflict", 0), mapped, unmapped, invalid, nodes ? json_array_get_length(nodes) : 0u);
    xml_summary(dialog, summary); g_free(summary);
    g_object_set_data_full(G_OBJECT(dialog), "import-xml-hash", g_strdup(radios_json_string(root, "preview_hash", "")), g_free);
    g_object_set_data_full(G_OBJECT(dialog), "import-xml-key", g_uuid_string_random(), g_free);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, TRUE);
}
static void xml_applied(RadioSViewContext *ctx, GtkWidget *dialog, JsonNode *result, const GError *error, gpointer user)
{
    (void)ctx; (void)user;
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), PREVIEW, TRUE);
    JsonObject *root = object_of(result);
    if (error || !root) {
        /* The same key and hash retry safely; a changed source needs a new preview. */
        gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, TRUE);
        xml_summary(dialog, error ? error->message : "İçe aktarma tamamlanamadı."); return;
    }
    JsonObject *counts = member_object(root, "counts");
    gchar *summary = g_strdup_printf("İçe aktarıldı: yeni parça %" G_GINT64_FORMAT " · güncellenen parça %" G_GINT64_FORMAT
        " · yeni sürüm %" G_GINT64_FORMAT " · playlist %" G_GINT64_FORMAT " · playlist satırı %" G_GINT64_FORMAT "%s",
        radios_json_int(counts, "items_created", 0), radios_json_int(counts, "items_updated", 0),
        radios_json_int(counts, "versions_created", 0), radios_json_int(counts, "playlist_nodes_written", 0),
        radios_json_int(counts, "playlist_entries_written", 0),
        json_object_has_member(root, "replayed") && json_object_get_boolean_member(root, "replayed") ? " · daha önce uygulanmıştı" : "");
    xml_summary(dialog, summary);
    Profiles *p = g_object_get_data(G_OBJECT(dialog), SECTION_KEY);
    if (p) { status_set(p, summary); radios_data_refresh(p->ctx); }
    g_free(summary);
    g_object_set_data(G_OBJECT(dialog), "import-xml-hash", NULL);
}
static void xml_response(GtkDialog *dialog, gint response, gpointer data)
{
    Profiles *p = data; GtkWidget *owner = GTK_WIDGET(dialog);
    const gchar *profile = g_object_get_data(G_OBJECT(dialog), "import-xml-profile");
    if (response == PREVIEW) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(g_object_get_data(G_OBJECT(dialog), "import-profile-xml-file")));
        gchar *contents = NULL; gsize length = 0; GError *error = NULL;
        xml_reset(owner);
        if (!path) { xml_summary(owner, "rekordbox XML dosyası seçilmedi."); return; }
        if (!g_file_get_contents(path, &contents, &length, &error)) { xml_summary(owner, error->message); g_error_free(error); g_free(path); return; }
        g_free(path);
        if (!length || length > XML_LIMIT) { xml_summary(owner, "XML dosyası boş ya da 60 MB'tan büyük."); g_free(contents); return; }
        gchar *encoded = g_base64_encode((const guchar *)contents, length); g_free(contents);
        g_object_set_data_full(G_OBJECT(dialog), "import-xml-base64", encoded, g_free);
        JsonNode *body = radios_data_body(p->ctx); JsonObject *request = json_node_get_object(body);
        json_object_set_string_member(request, "profile_id", profile); json_object_set_string_member(request, "xml_base64", encoded);
        if (radios_data_request(p->ctx, owner, "POST", "/v1/imports/rekordbox/preview", body, xml_previewed, NULL, NULL)) {
            gtk_dialog_set_response_sensitive(dialog, PREVIEW, FALSE); xml_summary(owner, "Önizleniyor…");
        } else xml_summary(owner, "Veri servisi bağlı değil.");
        json_node_free(body);
    } else if (response == GTK_RESPONSE_ACCEPT) {
        const gchar *encoded = g_object_get_data(G_OBJECT(dialog), "import-xml-base64");
        const gchar *hash = g_object_get_data(G_OBJECT(dialog), "import-xml-hash"), *key = g_object_get_data(G_OBJECT(dialog), "import-xml-key");
        if (!encoded || !hash || !key) return;
        JsonNode *body = radios_data_body(p->ctx); JsonObject *request = json_node_get_object(body);
        json_object_set_string_member(request, "profile_id", profile); json_object_set_string_member(request, "xml_base64", encoded);
        json_object_set_string_member(request, "expected_preview_hash", hash); json_object_set_string_member(request, "idempotency_key", key);
        if (radios_data_request(p->ctx, owner, "POST", "/v1/imports/rekordbox/apply", body, xml_applied, NULL, NULL)) {
            gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, FALSE); gtk_dialog_set_response_sensitive(dialog, PREVIEW, FALSE);
            xml_summary(owner, "İçe aktarılıyor…");
        } else xml_summary(owner, "Veri servisi bağlı değil.");
        json_node_free(body);
    } else gtk_widget_destroy(owner);
}
static void xml_clicked(GtkButton *button, gpointer data)
{
    (void)button; Profiles *p = data; JsonObject *row = selected(p); if (!row) return;
    GtkWidget *form, *dialog = dialog_new(p, "Profille rekordbox XML içe aktar", "import-profile-xml-dialog", &form);
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Kapat", GTK_RESPONSE_CLOSE, "Önizle", PREVIEW, "İçe aktar", GTK_RESPONSE_ACCEPT, NULL);
    g_object_set_data_full(G_OBJECT(dialog), "import-xml-profile", g_strdup(radios_json_string(row, "_id", "")), g_free);
    gchar *title = g_strdup_printf("Profil: %s · %s", radios_json_string(row, "name", ""), radios_json_string(row, "library_key", ""));
    pack(form, text(title, NULL), FALSE); g_free(title);
    GtkWidget *chooser = named(gtk_file_chooser_button_new("rekordbox XML", GTK_FILE_CHOOSER_ACTION_OPEN), "import-profile-xml-file");
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(chooser), TRUE);
    GtkFileFilter *filter = gtk_file_filter_new(); gtk_file_filter_set_name(filter, "rekordbox XML (*.xml)");
    gtk_file_filter_add_pattern(filter, "*.xml"); gtk_file_filter_add_pattern(filter, "*.XML");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
    field(form, "Dosya", chooser); g_object_set_data(G_OBJECT(dialog), "import-profile-xml-file", chooser);
    GtkWidget *summary = named(text("", "preferences-status"), "import-profile-xml-summary");
    gtk_label_set_selectable(GTK_LABEL(summary), TRUE);
    pack(form, summary, FALSE); g_object_set_data(G_OBJECT(dialog), "import-profile-xml-summary", summary);
    g_signal_connect(chooser, "file-set", G_CALLBACK(xml_file_set), dialog);
    g_signal_connect(dialog, "response", G_CALLBACK(xml_response), p);
    gtk_widget_show_all(dialog); xml_reset(dialog);
}

GtkWidget *radios_import_profiles_section_new(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx != NULL, NULL);
    Profiles *p = g_new0(Profiles, 1); p->ctx = ctx;
    GtkWidget *section = named(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8), "import-profiles");
    gtk_style_context_add_class(gtk_widget_get_style_context(section), "preferences-section");
    p->root = section; g_object_set_data_full(G_OBJECT(section), SECTION_KEY, p, profiles_free);
    pack(section, text("Rekordbox içe aktarma profilleri", "preferences-subtitle"), FALSE);
    p->store = gtk_list_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    p->list = named(gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->store)), "import-profiles-list"); g_object_unref(p->store);
    const gchar *titles[] = {"Profil", "Kitaplık anahtarı", "Cue", "Yol kuralı", "Durum"};
    for (guint i = 0; i < G_N_ELEMENTS(titles); ++i) {
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], gtk_cell_renderer_text_new(), "text", i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE); gtk_tree_view_column_set_expand(column, i < 2);
        gtk_tree_view_append_column(GTK_TREE_VIEW(p->list), column);
    }
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 110);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), p->list); pack(section, scroll, FALSE);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    p->add = named(gtk_button_new_with_label("Profil ekle"), "import-profiles-add");
    p->edit = named(gtk_button_new_with_label("Düzenle"), "import-profiles-edit");
    p->xml = named(gtk_button_new_with_label("XML içe aktar…"), "import-profiles-xml");
    pack(actions, p->add, FALSE); pack(actions, p->edit, FALSE); pack(actions, p->xml, FALSE); pack(section, actions, FALSE);
    p->status = named(text("", "preferences-status"), "import-profiles-status"); pack(section, p->status, FALSE);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->list)), "changed", G_CALLBACK(selection_changed), p);
    g_signal_connect(p->list, "row-activated", G_CALLBACK(row_activated), p);
    g_signal_connect(p->add, "clicked", G_CALLBACK(add_clicked), p);
    g_signal_connect(p->edit, "clicked", G_CALLBACK(edit_clicked), p);
    g_signal_connect(p->xml, "clicked", G_CALLBACK(xml_clicked), p);
    g_signal_connect(section, "map", G_CALLBACK(mapped), p);
    if (ctx->data) radios_data_register_refresh(ctx, section, refreshed);
    buttons_update(p);
    return section;
}
