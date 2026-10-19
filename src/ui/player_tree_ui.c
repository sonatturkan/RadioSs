#include "radios/player_tree_ui.h"
#include "radios/widgets.h"
#include <string.h>

enum { COL_NAME, COL_KIND, COL_DECK, COL_CLUSTER, COL_WEIGHT, COL_ICON, COL_COUNT };
#define GENERAL_KIND (-1)
#define CLUSTERED_KIND (-2)
typedef enum { EDIT_CLUSTER, EDIT_RENAME, EDIT_MOVE } EditKind;
typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *root, *tree, *status, *add_cluster, *add_player, *rename, *move, *remove, *reload, *dialog;
    GtkTreeStore *store;
    RadioSPlayerTreeSelection callback;
    gpointer user;
    gchar *signature;
    guint timer, initial, reveal;
    gboolean manage, include_general, rebuilding, constructing, destroyed;
} PlayerTree;
typedef struct {
    PlayerTree *ui;
    EditKind kind;
    GtkWidget *input, *error;
    RadioSAutomationScope scope;
    RadioSAutomationBinding binding;
} Edit;

static PlayerTree *state(GtkWidget *widget)
{ return GTK_IS_WIDGET(widget) ? g_object_get_data(G_OBJECT(widget), "radios-player-tree-ui") : NULL; }
static const RadioSPlayerTopology *topology(PlayerTree *ui)
{ return ui->ctx ? radios_player_workspace_topology(ui->ctx->players) : NULL; }
static gboolean ready(PlayerTree *ui)
{ return ui->ctx && radios_player_workspace_ready(ui->ctx->players); }
static void status(PlayerTree *ui, const gchar *message)
{
    gtk_label_set_text(GTK_LABEL(ui->status), message ? message : "");
    if (ui->manage) gtk_widget_set_visible(ui->status, message && *message);
}
static void notice(PlayerTree *ui, const gchar *message)
{ status(ui, ui->manage ? "" : message); }
static gboolean selected(PlayerTree *ui, RadioSAutomationScope *scope, gboolean *general)
{
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree)), NULL, &iter)) return FALSE;
    gint kind = GENERAL_KIND; guint deck = 0; gchar *cluster = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &iter, COL_KIND, &kind, COL_DECK, &deck, COL_CLUSTER, &cluster, -1);
    if (general) *general = kind == GENERAL_KIND;
    if (scope) {
        *scope = (RadioSAutomationScope){.kind = kind < 0 ? RADIOS_AUTOMATION_SCOPE_UNCLUSTERED : (RadioSAutomationScopeKind)kind, .deck = deck};
        g_strlcpy(scope->cluster_id, cluster ? cluster : "", sizeof scope->cluster_id);
    }
    g_free(cluster); return TRUE;
}
gboolean radios_player_tree_ui_selection(GtkWidget *widget, RadioSAutomationScope *scope, gboolean *general)
{
    PlayerTree *ui = state(widget); if (general) *general = FALSE;
    return ui && !ui->destroyed && selected(ui, scope, general);
}
static void controls(PlayerTree *ui)
{
    if (!ui->manage) return;
    RadioSAutomationScope scope = {0}; gboolean general = FALSE;
    gboolean have = selected(ui, &scope, &general), usable = ready(ui);
    gboolean editable = have && !general && scope.kind != RADIOS_AUTOMATION_SCOPE_UNCLUSTERED;
    gtk_widget_set_sensitive(ui->add_cluster, usable && topology(ui)->cluster_count < 32);
    gtk_widget_set_sensitive(ui->add_player, usable && radios_player_topology_count(topology(ui)) < 32);
    gtk_widget_set_sensitive(ui->rename, usable && editable);
    gtk_widget_set_sensitive(ui->move, usable && editable && scope.kind == RADIOS_AUTOMATION_SCOPE_PLAYER);
    gtk_widget_set_sensitive(ui->remove, usable && editable);
    gtk_widget_set_sensitive(ui->reload, ui->ctx && ui->ctx->players != NULL);
}
static void changed(GtkTreeSelection *selection, gpointer data)
{
    (void)selection; PlayerTree *ui = data;
    if (ui->rebuilding || ui->destroyed) return;
    controls(ui);
    RadioSAutomationScope scope; gboolean general = FALSE;
    if (!ui->constructing && ui->callback && selected(ui, &scope, &general)) ui->callback(general ? NULL : &scope, ui->user);
}
static gboolean selectable(GtkTreeSelection *selection, GtkTreeModel *model,
    GtkTreePath *path, gboolean currently_selected, gpointer data)
{
    (void)selection; (void)currently_selected; (void)data;
    GtkTreeIter iter; gint kind = CLUSTERED_KIND;
    if (gtk_tree_model_get_iter(model, &iter, path)) gtk_tree_model_get(model, &iter, COL_KIND, &kind, -1);
    return kind != CLUSTERED_KIND;
}
typedef struct { const RadioSAutomationScope *scope; GtkTreeIter iter; gboolean found; } Lookup;
static gboolean find_row(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
    (void)path; Lookup *lookup = data; gint kind = GENERAL_KIND; guint deck = 0; gchar *cluster = NULL;
    gtk_tree_model_get(model, iter, COL_KIND, &kind, COL_DECK, &deck, COL_CLUSTER, &cluster, -1);
    lookup->found = !lookup->scope ? kind == GENERAL_KIND : kind == (gint)lookup->scope->kind &&
        (kind == RADIOS_AUTOMATION_SCOPE_PLAYER ? deck == lookup->scope->deck :
         kind == RADIOS_AUTOMATION_SCOPE_CLUSTER ? g_strcmp0(cluster, lookup->scope->cluster_id) == 0 : TRUE);
    if (lookup->found) lookup->iter = *iter;
    g_free(cluster); return lookup->found;
}
static gboolean reveal_selection(gpointer data)
{
    PlayerTree *ui = data; ui->reveal = 0;
    if (ui->destroyed) return G_SOURCE_REMOVE;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree)), NULL, &iter)) {
        GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(ui->store), &iter);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(ui->tree), path, NULL, FALSE, 0, 0);
        gtk_tree_path_free(path);
    }
    return G_SOURCE_REMOVE;
}
static gboolean select_scope(PlayerTree *ui, const RadioSAutomationScope *scope, gboolean reveal)
{
    Lookup lookup = {.scope = scope}; gtk_tree_model_foreach(GTK_TREE_MODEL(ui->store), find_row, &lookup);
    if (!lookup.found) return FALSE;
    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(ui->store), &lookup.iter);
    if (reveal) gtk_tree_view_expand_to_path(GTK_TREE_VIEW(ui->tree), path);
    if (reveal) gtk_tree_view_set_cursor(GTK_TREE_VIEW(ui->tree), path, NULL, FALSE);
    else gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree)), &lookup.iter);
    gboolean root_selected = !scope || scope->kind != RADIOS_AUTOMATION_SCOPE_PLAYER;
    if (reveal) gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(ui->tree), path, NULL, root_selected, 0, 0);
    /* Selection callbacks can resize the player rack. Reveal once more after
     * that pending layout, without interfering with later manual scrolling. */
    if (reveal && !ui->reveal) ui->reveal = g_idle_add(reveal_selection, ui);
    gtk_tree_path_free(path); return TRUE;
}
gboolean radios_player_tree_ui_select(GtkWidget *widget, const RadioSAutomationScope *scope)
{
    PlayerTree *ui = state(widget); return ui && !ui->destroyed && select_scope(ui, scope, TRUE);
}
static gchar *signature(PlayerTree *ui)
{
    const RadioSPlayerTopology *model = topology(ui);
    if (!ready(ui) || !model) return g_strdup("waiting");
    JsonObject *object = radios_player_topology_to_json(model); JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, object); gchar *json = json_to_string(node, FALSE);
    gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, json, -1);
    g_free(json); json_node_free(node); return digest;
}
static void append_group(PlayerTree *ui, GtkTreeIter *iter, GtkTreeIter *parent,
    const gchar *name, gint kind, const gchar *cluster, guint count)
{
    gchar *label = count ? g_strdup_printf("%s  (%u)", name, count) : g_strdup(name);
    gtk_tree_store_append(ui->store, iter, parent);
    gtk_tree_store_set(ui->store, iter, COL_NAME, label, COL_KIND, kind, COL_DECK, 0u,
        COL_CLUSTER, cluster ? cluster : "", COL_WEIGHT, PANGO_WEIGHT_BOLD,
        COL_ICON, kind == GENERAL_KIND ? "audio-x-generic-symbolic" : "folder-symbolic", -1);
    g_free(label);
}
static void append_players(PlayerTree *ui, const RadioSPlayerTopology *model, GtkTreeIter *parent, const gchar *cluster)
{
    for (guint deck = 0; deck < 32; ++deck) {
        const RadioSPlayerDefinition *player = radios_player_topology_player_const(model, deck);
        if (!player || g_strcmp0(player->cluster_id, cluster)) continue;
        GtkTreeIter iter; gchar *label = *player->name ? g_strdup(player->name) : g_strdup_printf("Oynatıcı %u", deck + 1);
        gtk_tree_store_append(ui->store, &iter, parent);
        gtk_tree_store_set(ui->store, &iter, COL_NAME, label, COL_KIND, RADIOS_AUTOMATION_SCOPE_PLAYER,
            COL_DECK, deck, COL_CLUSTER, player->cluster_id, COL_WEIGHT, PANGO_WEIGHT_NORMAL, COL_ICON, "media-playback-start-symbolic", -1);
        g_free(label);
    }
}
typedef struct { PlayerTree *ui; GHashTable *known, *expanded; gboolean first; } Expansion;
static gchar *folder_key(GtkTreeModel *model, GtkTreeIter *iter)
{
    gint kind = GENERAL_KIND; gchar *cluster = NULL;
    gtk_tree_model_get(model, iter, COL_KIND, &kind, COL_CLUSTER, &cluster, -1);
    gchar *key = kind == RADIOS_AUTOMATION_SCOPE_PLAYER ? NULL : g_strdup_printf("%d:%s", kind, cluster ? cluster : "");
    g_free(cluster); return key;
}
static gboolean remember_expansion(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
    Expansion *state = data; gchar *key = folder_key(model, iter);
    if (key) {
        g_hash_table_add(state->known, g_strdup(key));
        if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(state->ui->tree), path)) g_hash_table_add(state->expanded, key);
        else g_free(key);
    }
    return FALSE;
}
static gboolean restore_expansion(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
    Expansion *state = data; gchar *key = folder_key(model, iter);
    if (key && (state->first || !g_hash_table_contains(state->known, key) || g_hash_table_contains(state->expanded, key)))
        gtk_tree_view_expand_row(GTK_TREE_VIEW(state->ui->tree), path, FALSE);
    g_free(key); return FALSE;
}
void radios_player_tree_ui_refresh(GtkWidget *widget)
{
    PlayerTree *ui = state(widget); if (!ui || ui->destroyed) return;
    gchar *next = signature(ui);
    if (!g_strcmp0(next, ui->signature)) { g_free(next); return; }
    RadioSAutomationScope scope = {0}; gboolean general = FALSE;
    gboolean have = selected(ui, &scope, &general), first = ui->signature == NULL || g_str_equal(ui->signature, "waiting");
    Expansion expansion = {ui, g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), first};
    gtk_tree_model_foreach(GTK_TREE_MODEL(ui->store), remember_expansion, &expansion);
    g_free(ui->signature); ui->signature = next; ui->rebuilding = TRUE;
    gtk_tree_store_clear(ui->store); GtkTreeIter parent, clustered;
    const RadioSPlayerTopology *model = ready(ui) ? topology(ui) : NULL;
    guint independent_count = model ? radios_player_topology_members(model, "", NULL, 0) : 0;
    if (ui->include_general) append_group(ui, &parent, NULL, "Genel yayın", GENERAL_KIND, "", 0);
    append_group(ui, &clustered, NULL, "Kümeli", CLUSTERED_KIND, "",
        model ? radios_player_topology_count(model) - independent_count : 0);
    for (guint i = 0; model && i < model->cluster_count; ++i) {
        const RadioSPlayerCluster *cluster = &model->clusters[i];
        append_group(ui, &parent, &clustered, cluster->name, RADIOS_AUTOMATION_SCOPE_CLUSTER, cluster->id,
            radios_player_topology_members(model, cluster->id, NULL, 0));
        append_players(ui, model, &parent, cluster->id);
    }
    append_group(ui, &parent, NULL, "Kümesizler", RADIOS_AUTOMATION_SCOPE_UNCLUSTERED, "", independent_count);
    if (model) append_players(ui, model, &parent, "");
    gtk_tree_model_foreach(GTK_TREE_MODEL(ui->store), restore_expansion, &expansion);
    g_hash_table_unref(expansion.expanded); g_hash_table_unref(expansion.known);
    gboolean restored = have && select_scope(ui, general ? NULL : &scope, FALSE);
    if (!restored) {
        RadioSAutomationScope independent = {.kind = RADIOS_AUTOMATION_SCOPE_UNCLUSTERED};
        select_scope(ui, ui->include_general ? NULL : &independent, TRUE);
    }
    ui->rebuilding = FALSE; controls(ui);
    if (model && ui->manage) status(ui, "");
    else if (model) {
        gchar *message = g_strdup_printf("%u oynatıcı · %u küme", radios_player_topology_count(model), model->cluster_count);
        status(ui, message); g_free(message);
    } else status(ui, "Oynatıcı tanımları yükleniyor…");
    changed(NULL, ui);
}
static gboolean save(PlayerTree *ui, RadioSPlayerTopology *model, const RadioSAutomationScope *scope, const gchar *message)
{
    GError *error = NULL;
    if (!radios_player_workspace_replace(ui->ctx->players, model, &error)) {
        status(ui, error ? error->message : "Oynatıcı tanımı kaydedilemedi."); g_clear_error(&error); return FALSE;
    }
    GtkWidget *root = g_object_ref(ui->root); radios_player_tree_ui_refresh(root);
    if (!ui->destroyed && scope) radios_player_tree_ui_select(root, scope);
    if (!ui->destroyed) notice(ui, message);
    g_object_unref(root); return TRUE;
}
static void dialog_destroyed(GtkWidget *dialog, gpointer data)
{ Edit *edit = data; if (edit->ui->dialog == dialog) edit->ui->dialog = NULL; }
static void edit_response(GtkDialog *dialog, gint response, gpointer data)
{
    Edit *edit = data; PlayerTree *ui = edit->ui;
    if (response != GTK_RESPONSE_ACCEPT || ui->destroyed) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    const RadioSPlayerTopology *current = topology(ui); GError *error = NULL;
    if (!ready(ui) || !current) { gtk_label_set_text(GTK_LABEL(edit->error), "Oynatıcı tanımları henüz hazır değil."); return; }
    RadioSPlayerTopology model = *current; RadioSAutomationScope next = edit->scope;
    gboolean accepted = TRUE;
    if (edit->kind != EDIT_CLUSTER) accepted = radios_automation_scope_binding_current(current, &edit->binding, &error);
    if (accepted && edit->kind == EDIT_CLUSTER) {
        gchar id[37]; accepted = radios_player_topology_add_cluster(&model, gtk_entry_get_text(GTK_ENTRY(edit->input)), id, &error);
        if (accepted) { next = (RadioSAutomationScope){.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER}; g_strlcpy(next.cluster_id, id, sizeof next.cluster_id); }
    } else if (accepted && edit->kind == EDIT_MOVE) {
        const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(edit->input));
        accepted = radios_player_topology_set_cluster(&model, edit->scope.deck, g_strcmp0(id, "unclustered") ? id : "", &error);
    } else if (accepted) {
        const gchar *name = gtk_entry_get_text(GTK_ENTRY(edit->input));
        if (edit->scope.kind == RADIOS_AUTOMATION_SCOPE_CLUSTER) {
            RadioSPlayerCluster *cluster = radios_player_topology_cluster(&model, edit->scope.cluster_id);
            if (cluster) g_strlcpy(cluster->name, name, sizeof cluster->name); else accepted = FALSE;
        } else {
            RadioSPlayerDefinition *player = radios_player_topology_player(&model, edit->scope.deck);
            if (player) g_strlcpy(player->name, name, sizeof player->name); else accepted = FALSE;
        }
        if (accepted) accepted = radios_player_topology_validate(&model, &error);
    }
    if (accepted) accepted = radios_player_workspace_replace(ui->ctx->players, &model, &error);
    if (!accepted) {
        gtk_label_set_text(GTK_LABEL(edit->error), error ? error->message : "Seçili oynatıcı veya küme artık mevcut değil.");
        g_clear_error(&error); return;
    }
    GtkWidget *root = g_object_ref(ui->root); gtk_widget_destroy(GTK_WIDGET(dialog));
    radios_player_tree_ui_refresh(root);
    if (!ui->destroyed) radios_player_tree_ui_select(root, &next);
    if (!ui->destroyed) notice(ui, "Tanım güncellendi.");
    g_object_unref(root);
}
static void edit_open(PlayerTree *ui, EditKind kind)
{
    if (!ready(ui) || ui->dialog) return;
    Edit *edit = g_new0(Edit, 1); edit->ui = ui; edit->kind = kind;
    gboolean general = FALSE;
    if (kind != EDIT_CLUSTER && (!selected(ui, &edit->scope, &general) || general ||
        edit->scope.kind == RADIOS_AUTOMATION_SCOPE_UNCLUSTERED ||
        !radios_automation_scope_resolve(topology(ui), &edit->scope, &edit->binding, NULL))) { g_free(edit); return; }
    const gchar *title = kind == EDIT_CLUSTER ? "Küme oluştur" : kind == EDIT_MOVE ? "Oynatıcıyı taşı" : "Adı değiştir";
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, ui->ctx->parent, GTK_DIALOG_DESTROY_WITH_PARENT,
        "Vazgeç", GTK_RESPONSE_CANCEL, kind == EDIT_CLUSTER ? "Oluştur" : "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    ui->dialog = dialog; gtk_widget_set_name(dialog, "player-tree-edit-dialog"); gtk_window_set_default_size(GTK_WINDOW(dialog), 360, -1);
    g_object_set_data_full(G_OBJECT(dialog), "radios-player-tree-edit", edit, g_free);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_container_set_border_width(GTK_CONTAINER(content), 14);
    gtk_box_set_spacing(GTK_BOX(content), 10);
    if (kind == EDIT_MOVE) {
        edit->input = gtk_combo_box_text_new(); gtk_widget_set_name(edit->input, "player-tree-move-target");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(edit->input), "unclustered", "Kümesizler");
        const RadioSPlayerTopology *model = topology(ui);
        for (guint i = 0; i < model->cluster_count; ++i)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(edit->input), model->clusters[i].id, model->clusters[i].name);
        const gchar *current_id = model->players[edit->scope.deck].cluster_id;
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(edit->input), *current_id ? current_id : "unclustered");
        gtk_box_pack_start(GTK_BOX(content), radios_ui_label("Hedef küme", "small"), FALSE, FALSE, 0);
    } else {
        edit->input = radios_ui_entry(kind == EDIT_CLUSTER ? "Küme adı" : "Oynatıcı veya küme adı");
        gtk_widget_set_name(edit->input, "player-tree-name-entry");
        gtk_entry_set_max_length(GTK_ENTRY(edit->input), kind == EDIT_CLUSTER || edit->scope.kind == RADIOS_AUTOMATION_SCOPE_CLUSTER ? 80 : 64);
        gtk_entry_set_activates_default(GTK_ENTRY(edit->input), TRUE);
        if (kind == EDIT_RENAME) {
            const RadioSPlayerTopology *model = topology(ui);
            const gchar *name = edit->scope.kind == RADIOS_AUTOMATION_SCOPE_CLUSTER ? radios_player_topology_cluster_const(model, edit->scope.cluster_id)->name : model->players[edit->scope.deck].name;
            gtk_entry_set_text(GTK_ENTRY(edit->input), name);
        }
    }
    gtk_box_pack_start(GTK_BOX(content), edit->input, FALSE, FALSE, 0);
    edit->error = radios_ui_label("", "application-status"); gtk_widget_set_name(edit->error, "player-tree-edit-status");
    gtk_box_pack_start(GTK_BOX(content), edit->error, FALSE, FALSE, 0);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_signal_connect(dialog, "destroy", G_CALLBACK(dialog_destroyed), edit);
    g_signal_connect(dialog, "response", G_CALLBACK(edit_response), edit);
    gtk_widget_show_all(dialog); gtk_widget_grab_focus(edit->input);
}
static void add_cluster_clicked(GtkButton *button, gpointer data) { (void)button; edit_open(data, EDIT_CLUSTER); }
static void rename_clicked(GtkButton *button, gpointer data) { (void)button; edit_open(data, EDIT_RENAME); }
static void move_clicked(GtkButton *button, gpointer data) { (void)button; edit_open(data, EDIT_MOVE); }
static void add_player_clicked(GtkButton *button, gpointer data)
{
    (void)button; PlayerTree *ui = data; if (!ready(ui)) return;
    RadioSPlayerTopology model = *topology(ui); RadioSAutomationScope selection; gboolean general = FALSE;
    const gchar *cluster = "";
    if (selected(ui, &selection, &general) && !general) {
        if (selection.kind == RADIOS_AUTOMATION_SCOPE_CLUSTER) cluster = selection.cluster_id;
        else if (selection.kind == RADIOS_AUTOMATION_SCOPE_PLAYER) cluster = model.players[selection.deck].cluster_id;
    }
    GError *error = NULL; guint deck = 32;
    if (!radios_player_topology_add_player(&model, cluster, &deck, &error)) { status(ui, error->message); g_clear_error(&error); return; }
    RadioSAutomationScope scope = {.kind = *cluster ? RADIOS_AUTOMATION_SCOPE_CLUSTER : RADIOS_AUTOMATION_SCOPE_UNCLUSTERED};
    g_strlcpy(scope.cluster_id, cluster, sizeof scope.cluster_id);
    save(ui, &model, &scope, "Oynatıcı eklendi.");
}
static void remove_clicked(GtkButton *button, gpointer data)
{
    (void)button; PlayerTree *ui = data; if (!ready(ui)) return;
    RadioSAutomationScope scope; gboolean general = FALSE;
    if (!selected(ui, &scope, &general) || general) return;
    RadioSPlayerTopology model = *topology(ui); GError *error = NULL; gboolean accepted = FALSE;
    if (scope.kind == RADIOS_AUTOMATION_SCOPE_CLUSTER) accepted = radios_player_topology_remove_cluster(&model, scope.cluster_id, &error);
    else if (scope.kind == RADIOS_AUTOMATION_SCOPE_PLAYER) accepted = radios_player_topology_remove_player(&model, scope.deck, &error);
    if (!accepted) { status(ui, error ? error->message : "Bir oynatıcı veya küme seçin."); g_clear_error(&error); return; }
    RadioSAutomationScope independent = {.kind = RADIOS_AUTOMATION_SCOPE_UNCLUSTERED};
    save(ui, &model, &independent, scope.kind == RADIOS_AUTOMATION_SCOPE_CLUSTER ? "Küme kaldırıldı; oynatıcılar Kümesizler içinde." : "Oynatıcı kaldırıldı.");
}
static void reload_clicked(GtkButton *button, gpointer data)
{
    (void)button; PlayerTree *ui = data;
    if (ui->ctx && ui->ctx->players) { radios_player_workspace_reload(ui->ctx->players); notice(ui, "Tanımlar için yenileme istendi."); }
}
static gboolean tick(gpointer data)
{
    PlayerTree *ui = data; GtkWidget *root = g_object_ref(ui->root);
    radios_player_tree_ui_refresh(root); gboolean keep = !ui->destroyed;
    g_object_unref(root); return keep ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
static gboolean initial_selection(gpointer data)
{
    PlayerTree *ui = data; ui->initial = 0; GtkWidget *root = g_object_ref(ui->root);
    changed(NULL, ui); g_object_unref(root); return G_SOURCE_REMOVE;
}
static void destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; PlayerTree *ui = data; ui->destroyed = TRUE;
    if (ui->timer) { g_source_remove(ui->timer); ui->timer = 0; }
    if (ui->initial) { g_source_remove(ui->initial); ui->initial = 0; }
    if (ui->reveal) { g_source_remove(ui->reveal); ui->reveal = 0; }
    if (ui->dialog) gtk_widget_destroy(ui->dialog);
}
static void free_tree(gpointer data)
{ PlayerTree *ui = data; g_clear_object(&ui->store); g_free(ui->signature); g_free(ui); }
static GtkWidget *control(PlayerTree *ui, GtkWidget *row, const gchar *label, const gchar *name, GCallback callback)
{
    GtkWidget *button = radios_ui_button(label); gtk_widget_set_name(button, name);
    gtk_box_pack_start(GTK_BOX(row), button, FALSE, FALSE, 0); g_signal_connect(button, "clicked", callback, ui); return button;
}
static void install_style(GtkWidget *widget)
{
    GdkScreen *screen = gtk_widget_get_screen(widget);
    if (!screen || g_object_get_data(G_OBJECT(screen), "radios-player-tree-theme")) return;
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
        ".player-tree-panel { padding: 9px; border: 1px solid #333; border-radius: 5px; background: #101010; }"
        ".player-tree-panel treeview { background: #101010; color: #dedede; font-size: 12px; }"
        ".player-tree-panel treeview:selected { background: #353535; color: #fff; }"
        ".player-tree-panel button { min-width: 0; padding: 4px 6px; font-size: 11px; }"
        ".player-tree-manager { padding: 6px; }"
        ".player-tree-manager button { min-width: 36px; min-height: 18px; padding: 2px 4px; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_set_data_full(G_OBJECT(screen), "radios-player-tree-theme", provider, g_object_unref);
}
GtkWidget *radios_player_tree_ui_new(RadioSViewContext *ctx, gboolean manage, gboolean include_general,
    RadioSPlayerTreeSelection callback, gpointer user)
{
    PlayerTree *ui = g_new0(PlayerTree, 1); ui->ctx = ctx; ui->manage = manage; ui->include_general = include_general;
    ui->callback = callback; ui->user = user; ui->constructing = TRUE;
    ui->root = radios_ui_box(GTK_ORIENTATION_VERTICAL, manage ? 4 : 8, "player-tree-panel"); gtk_widget_set_name(ui->root, "player-tree-selector");
    if (manage) gtk_style_context_add_class(gtk_widget_get_style_context(ui->root), "player-tree-manager");
    gtk_widget_set_size_request(ui->root, 210, -1); gtk_widget_set_hexpand(ui->root, FALSE); install_style(ui->root);
    g_object_set_data_full(G_OBJECT(ui->root), "radios-player-tree-ui", ui, free_tree);
    g_signal_connect(ui->root, "destroy", G_CALLBACK(destroyed), ui);
    GtkWidget *heading = radios_ui_label(manage ? "Oynatıcılar" : "Yayın kapsamı", "heading");
    gtk_label_set_line_wrap(GTK_LABEL(heading), FALSE); gtk_label_set_single_line_mode(GTK_LABEL(heading), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(heading), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(heading), 12);
    if (manage) {
        GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, NULL);
        gtk_box_pack_start(GTK_BOX(row), heading, TRUE, TRUE, 0);
        ui->add_cluster = control(ui, row, "+ Küme", "player-tree-add-cluster", G_CALLBACK(add_cluster_clicked));
        ui->add_player = control(ui, row, "+ Oynatıcı", "player-tree-add-player", G_CALLBACK(add_player_clicked));
        gtk_box_pack_start(GTK_BOX(ui->root), row, FALSE, FALSE, 0);
        row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, NULL);
        ui->rename = control(ui, row, "Ad", "player-tree-rename", G_CALLBACK(rename_clicked));
        ui->move = control(ui, row, "Taşı", "player-tree-move", G_CALLBACK(move_clicked));
        ui->remove = control(ui, row, "Sil", "player-tree-remove", G_CALLBACK(remove_clicked));
        ui->reload = control(ui, row, "Yenile", "player-tree-reload", G_CALLBACK(reload_clicked));
        gtk_widget_set_tooltip_text(ui->rename, "Seçili oynatıcı veya kümenin adını değiştir");
        gtk_widget_set_tooltip_text(ui->move, "Seçili oynatıcıyı bir kümeye veya Kümesizler içine taşı");
        gtk_widget_set_tooltip_text(ui->remove, "Seçili tanımı kaldır; kümenin oynatıcıları Kümesizler içinde kalır");
        gtk_widget_set_no_show_all(row, TRUE); gtk_box_pack_start(GTK_BOX(ui->root), row, FALSE, FALSE, 0);
    } else gtk_box_pack_start(GTK_BOX(ui->root), heading, FALSE, FALSE, 0);
    ui->store = gtk_tree_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_INT, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
    ui->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui->store)); gtk_widget_set_name(ui->tree, "player-tree");
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(ui->tree), FALSE); gtk_tree_view_set_enable_search(GTK_TREE_VIEW(ui->tree), TRUE);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(ui->tree), COL_NAME); gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(ui->tree), COL_NAME);
    atk_object_set_name(gtk_widget_get_accessible(ui->tree), "Oynatıcı ve küme seçimi");
    GtkTreeViewColumn *column = gtk_tree_view_column_new(); gtk_tree_view_column_set_expand(column, TRUE);
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new(); gtk_tree_view_column_pack_start(column, icon, FALSE);
    gtk_tree_view_column_add_attribute(column, icon, "icon-name", COL_ICON);
    GtkCellRenderer *text = gtk_cell_renderer_text_new(); g_object_set(text, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", manage ? 4 : 7, NULL);
    gtk_tree_view_column_pack_start(column, text, TRUE); gtk_tree_view_column_add_attribute(column, text, "text", COL_NAME);
    gtk_tree_view_column_add_attribute(column, text, "weight", COL_WEIGHT); gtk_tree_view_append_column(GTK_TREE_VIEW(ui->tree), column);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree)), GTK_SELECTION_SINGLE);
    gtk_tree_selection_set_select_function(gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree)), selectable, NULL, NULL);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree)), "changed", G_CALLBACK(changed), ui);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_widget_set_name(scroll, "player-tree-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, manage ? 130 : 190);
    if (manage) {
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 130);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 150);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    }
    gtk_container_add(GTK_CONTAINER(scroll), ui->tree);
    gtk_box_pack_start(GTK_BOX(ui->root), scroll, TRUE, TRUE, 0);
    ui->status = radios_ui_label("", "application-status"); gtk_widget_set_name(ui->status, "player-tree-status");
    gtk_widget_set_no_show_all(ui->status, manage);
    gtk_label_set_line_wrap(GTK_LABEL(ui->status), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(ui->status), 25);
    gtk_box_pack_start(GTK_BOX(ui->root), ui->status, FALSE, FALSE, 0);
    radios_player_tree_ui_refresh(ui->root); ui->constructing = FALSE;
    ui->timer = g_timeout_add(300, tick, ui); ui->initial = g_idle_add(initial_selection, ui); return ui->root;
}
