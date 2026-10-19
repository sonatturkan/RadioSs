#include "radios/player_tree_ui.h"
#include <string.h>

static RadioSPlayerTopology model;
static gboolean loaded, reject_write;
static guint writes, reloads, selections;
static RadioSAutomationScope last_scope;
static gboolean last_general;
static guint workspace_token;

gboolean __wrap_radios_player_workspace_ready(const RadioSPlayerWorkspace *workspace)
{ g_assert_true(workspace == (RadioSPlayerWorkspace *)&workspace_token); return loaded; }
const RadioSPlayerTopology *__wrap_radios_player_workspace_topology(const RadioSPlayerWorkspace *workspace)
{ g_assert_true(workspace == (RadioSPlayerWorkspace *)&workspace_token); return &model; }
gboolean __wrap_radios_player_workspace_replace(RadioSPlayerWorkspace *workspace, const RadioSPlayerTopology *next, GError **error)
{
    g_assert_true(workspace == (RadioSPlayerWorkspace *)&workspace_token);
    if (reject_write) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Çalışan oynatıcı taşınamaz veya silinemez."); return FALSE; }
    if (!radios_player_topology_validate(next, error)) return FALSE;
    model = *next; ++writes; return TRUE;
}
void __wrap_radios_player_workspace_reload(RadioSPlayerWorkspace *workspace)
{ g_assert_true(workspace == (RadioSPlayerWorkspace *)&workspace_token); ++reloads; }
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} }
static void selection(const RadioSAutomationScope *scope, gpointer data)
{
    (void)data; ++selections; last_general = scope == NULL;
    if (scope) last_scope = *scope;
}
typedef struct { GtkWidget *window, *panel; RadioSViewContext context; gchar cluster[37]; } Harness;
static Harness *open_tree(gboolean manage, gboolean general)
{
    Harness *h = g_new0(Harness, 1); loaded = TRUE; reject_write = FALSE; writes = reloads = selections = 0;
    radios_player_topology_default(&model, 3); GError *error = NULL;
    g_assert_true(radios_player_topology_add_cluster(&model, "Müzik", h->cluster, &error));
    g_assert_true(radios_player_topology_set_cluster(&model, 0, h->cluster, &error));
    g_assert_true(radios_player_topology_set_cluster(&model, 1, h->cluster, &error)); g_assert_no_error(error);
    h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(h->window);
    h->context = (RadioSViewContext){.parent = GTK_WINDOW(h->window), .players = (RadioSPlayerWorkspace *)&workspace_token};
    h->panel = radios_player_tree_ui_new(&h->context, manage, general, selection, NULL);
    gtk_container_add(GTK_CONTAINER(h->window), h->panel); gtk_widget_show_all(h->window); pump(); return h;
}
static void close_tree(Harness *h)
{ gtk_widget_destroy(h->window); g_object_unref(h->window); pump(); g_free(h); }
static GtkWidget *edit_dialog(void)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), "player-tree-edit-dialog")) result = item->data;
    g_list_free(windows); return result;
}
static void click(Harness *h, const gchar *name)
{
    GtkWidget *button = find(h->panel, name); g_assert_true(GTK_IS_BUTTON(button));
    g_assert_true(gtk_widget_get_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button)); pump();
}
static void enter_name(const gchar *name)
{
    GtkWidget *dialog = edit_dialog(); g_assert_nonnull(dialog);
    GtkWidget *entry = find(dialog, "player-tree-name-entry"); g_assert_true(GTK_IS_ENTRY(entry));
    gtk_entry_set_text(GTK_ENTRY(entry), name); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); pump();
    g_assert_null(edit_dialog());
}
static void assert_folder_selected(Harness *h, const gchar *cluster_id)
{
    RadioSAutomationScope scope; gboolean general = TRUE;
    g_assert_true(radios_player_tree_ui_selection(h->panel, &scope, &general)); g_assert_false(general);
    g_assert_cmpint(scope.kind, ==, *cluster_id ? RADIOS_AUTOMATION_SCOPE_CLUSTER : RADIOS_AUTOMATION_SCOPE_UNCLUSTERED);
    g_assert_cmpstr(scope.cluster_id, ==, cluster_id);
    g_assert_false(last_general); g_assert_cmpint(last_scope.kind, ==, scope.kind);
    g_assert_cmpstr(last_scope.cluster_id, ==, cluster_id);
}
static void test_hierarchy_selection_refresh(void)
{
    Harness *h = open_tree(FALSE, TRUE); GtkWidget *tree = find(h->panel, "player-tree");
    GtkTreeModel *store = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); g_assert_cmpint(gtk_tree_model_iter_n_children(store, NULL), ==, 3);
    g_assert_true(last_general); g_assert_null(find(h->panel, "player-tree-add-cluster"));
    GtkTreeIter grouped, cluster; gchar *label = NULL;
    g_assert_true(gtk_tree_model_iter_nth_child(store, &grouped, NULL, 1));
    gtk_tree_model_get(store, &grouped, 0, &label, -1); g_assert_true(g_str_has_prefix(label, "Kümeli")); g_free(label);
    g_assert_cmpint(gtk_tree_model_iter_n_children(store, &grouped), ==, 1);
    g_assert_true(gtk_tree_model_iter_children(store, &cluster, &grouped));
    gtk_tree_model_get(store, &cluster, 0, &label, -1); g_assert_true(g_str_has_prefix(label, "Müzik")); g_free(label);
    g_assert_cmpint(gtk_tree_model_iter_n_children(store, &cluster), ==, 2);
    guint callbacks = selections; GtkTreePath *group_path = gtk_tree_path_new_from_indices(1, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), group_path);
    g_assert_cmpuint(selections, ==, callbacks); g_assert_true(last_general);
    gboolean general = FALSE; RadioSAutomationScope selected;
    g_assert_true(radios_player_tree_ui_selection(h->panel, &selected, &general)); g_assert_true(general);
    gtk_tree_path_free(group_path);
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    g_assert_true(radios_player_tree_ui_select(h->panel, &scope)); g_assert_cmpuint(last_scope.deck, ==, 0);
    guint previous = selections;
    for (guint i = 0; i < 5; ++i) radios_player_tree_ui_refresh(h->panel);
    g_assert_cmpuint(selections, ==, previous); g_assert_cmpuint(writes, ==, 0);
    GError *error = NULL; g_assert_true(radios_player_topology_set_cluster(&model, 0, "", &error)); g_assert_no_error(error);
    radios_player_tree_ui_refresh(h->panel); g_assert_cmpuint(selections, ==, previous + 1);
    general = TRUE;
    g_assert_true(radios_player_tree_ui_selection(h->panel, &selected, &general));
    g_assert_false(general); g_assert_cmpuint(selected.deck, ==, 0);
    GtkTreePath *selected_path = NULL; gtk_tree_view_get_cursor(GTK_TREE_VIEW(tree), &selected_path, NULL);
    if (selected_path) gtk_tree_path_free(selected_path);
    GtkTreeIter root; g_assert_true(gtk_tree_model_iter_nth_child(store, &root, NULL, 2));
    g_assert_cmpint(gtk_tree_model_iter_n_children(store, &root), ==, 2);
    GtkTreePath *cluster_path = gtk_tree_path_new_from_indices(1, 0, -1);
    gtk_tree_view_collapse_row(GTK_TREE_VIEW(tree), cluster_path);
    g_strlcpy(model.players[2].name, "Yeni bağımsız", sizeof model.players[2].name); radios_player_tree_ui_refresh(h->panel);
    g_assert_false(gtk_tree_view_row_expanded(GTK_TREE_VIEW(tree), cluster_path)); gtk_tree_path_free(cluster_path);
    g_assert_true(radios_player_tree_ui_select(h->panel, NULL)); g_assert_true(last_general); close_tree(h);
}
static void test_management_and_guard(void)
{
    Harness *h = open_tree(TRUE, FALSE); guint initial_count = radios_player_topology_count(&model);
    g_assert_true(gtk_widget_is_drawable(find(h->panel, "player-tree-scroll")));
    click(h, "player-tree-add-cluster"); enter_name("Haber"); g_assert_cmpuint(model.cluster_count, ==, 2);
    RadioSAutomationScope cluster; gboolean general;
    g_assert_true(radios_player_tree_ui_selection(h->panel, &cluster, &general));
    g_assert_cmpint(cluster.kind, ==, RADIOS_AUTOMATION_SCOPE_CLUSTER); g_assert_false(general);
    gchar added_id[37]; g_strlcpy(added_id, cluster.cluster_id, sizeof added_id);
    click(h, "player-tree-add-player"); g_assert_cmpuint(radios_player_topology_count(&model), ==, initial_count + 1);
    assert_folder_selected(h, added_id);
    RadioSAutomationScope player = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 3};
    g_assert_cmpstr(model.players[player.deck].cluster_id, ==, added_id);
    g_assert_true(radios_player_tree_ui_select(h->panel, &player));
    click(h, "player-tree-rename"); enter_name("Haber oynatıcı"); g_assert_cmpstr(model.players[player.deck].name, ==, "Haber oynatıcı");
    click(h, "player-tree-move"); GtkWidget *dialog = edit_dialog(); g_assert_nonnull(dialog);
    GtkWidget *target = find(dialog, "player-tree-move-target");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(target), "unclustered"));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); pump(); g_assert_null(edit_dialog());
    g_assert_cmpstr(model.players[player.deck].cluster_id, ==, "");
    guint before = writes; reject_write = TRUE; click(h, "player-tree-remove");
    g_assert_cmpuint(writes, ==, before); g_assert_true(model.players[player.deck].enabled);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(h->panel, "player-tree-status"))), "Çalışan"));
    reject_write = FALSE; click(h, "player-tree-remove"); g_assert_false(model.players[player.deck].enabled);
    g_assert_true(radios_player_tree_ui_select(h->panel, &cluster)); click(h, "player-tree-remove");
    g_assert_cmpuint(model.cluster_count, ==, 1); g_assert_cmpuint(radios_player_topology_count(&model), ==, initial_count);
    click(h, "player-tree-reload"); g_assert_cmpuint(reloads, ==, 1); close_tree(h);
}
static void test_add_keeps_current_folder(void)
{
    Harness *h = open_tree(TRUE, FALSE);
    RadioSAutomationScope cluster = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER};
    g_strlcpy(cluster.cluster_id, h->cluster, sizeof cluster.cluster_id);
    g_assert_true(radios_player_tree_ui_select(h->panel, &cluster));
    for (guint deck = 3; deck < 5; ++deck) {
        click(h, "player-tree-add-player");
        g_assert_true(model.players[deck].enabled); g_assert_cmpstr(model.players[deck].cluster_id, ==, h->cluster);
        assert_folder_selected(h, h->cluster);
    }
    RadioSAutomationScope player = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    g_assert_true(radios_player_tree_ui_select(h->panel, &player));
    click(h, "player-tree-add-player"); g_assert_cmpstr(model.players[5].cluster_id, ==, h->cluster);
    assert_folder_selected(h, h->cluster);
    RadioSAutomationScope independent = {.kind = RADIOS_AUTOMATION_SCOPE_UNCLUSTERED};
    g_assert_true(radios_player_tree_ui_select(h->panel, &independent));
    click(h, "player-tree-add-player"); g_assert_true(model.players[6].enabled);
    g_assert_cmpstr(model.players[6].cluster_id, ==, ""); assert_folder_selected(h, "");
    player.deck = 2; g_assert_true(radios_player_tree_ui_select(h->panel, &player));
    click(h, "player-tree-add-player"); g_assert_true(model.players[7].enabled);
    g_assert_cmpstr(model.players[7].cluster_id, ==, ""); assert_folder_selected(h, "");
    player.deck = 0; g_assert_true(radios_player_tree_ui_select(h->panel, &player));
    guint before = writes, callbacks = selections; reject_write = TRUE;
    click(h, "player-tree-add-player");
    g_assert_cmpuint(writes, ==, before); g_assert_cmpuint(radios_player_topology_count(&model), ==, 8);
    RadioSAutomationScope still_selected; gboolean general = TRUE;
    g_assert_true(radios_player_tree_ui_selection(h->panel, &still_selected, &general));
    g_assert_false(general); g_assert_cmpint(still_selected.kind, ==, RADIOS_AUTOMATION_SCOPE_PLAYER);
    g_assert_cmpuint(still_selected.deck, ==, 0); g_assert_cmpuint(selections, ==, callbacks);
    g_assert_true(gtk_widget_is_drawable(find(h->panel, "player-tree-status")));
    close_tree(h);
}
static void test_nested_folder_expansion(void)
{
    Harness *h = open_tree(FALSE, TRUE); GtkTreeView *tree = GTK_TREE_VIEW(find(h->panel, "player-tree"));
    GtkTreePath *grouped = gtk_tree_path_new_from_indices(1, -1);
    GtkTreePath *cluster = gtk_tree_path_new_from_indices(1, 0, -1);
    GtkTreePath *independent = gtk_tree_path_new_from_indices(2, -1);
    g_assert_true(gtk_tree_view_row_expanded(tree, grouped)); g_assert_true(gtk_tree_view_row_expanded(tree, cluster));
    gtk_tree_view_collapse_row(tree, cluster);
    g_strlcpy(model.players[2].name, "Bağımsız yeni ad", sizeof model.players[2].name);
    radios_player_tree_ui_refresh(h->panel);
    g_assert_true(gtk_tree_view_row_expanded(tree, grouped)); g_assert_false(gtk_tree_view_row_expanded(tree, cluster));
    g_assert_true(gtk_tree_view_row_expanded(tree, independent));
    gtk_tree_view_collapse_row(tree, grouped); gtk_tree_view_collapse_row(tree, independent);
    g_strlcpy(model.players[0].name, "Müzik yeni ad", sizeof model.players[0].name);
    radios_player_tree_ui_refresh(h->panel);
    g_assert_false(gtk_tree_view_row_expanded(tree, grouped)); g_assert_false(gtk_tree_view_row_expanded(tree, independent));
    gtk_tree_view_expand_row(tree, grouped, FALSE); g_assert_false(gtk_tree_view_row_expanded(tree, cluster));
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER};
    g_strlcpy(scope.cluster_id, h->cluster, sizeof scope.cluster_id);
    g_assert_true(radios_player_tree_ui_select(h->panel, &scope)); gtk_tree_view_collapse_row(tree, cluster);
    g_strlcpy(model.players[1].name, "İkinci müzik", sizeof model.players[1].name);
    radios_player_tree_ui_refresh(h->panel); assert_folder_selected(h, h->cluster);
    g_assert_false(gtk_tree_view_row_expanded(tree, cluster));
    gtk_tree_path_free(grouped); gtk_tree_path_free(cluster); gtk_tree_path_free(independent); close_tree(h);
}
static void test_empty_clustered_folder(void)
{
    Harness *h = open_tree(TRUE, FALSE); GError *error = NULL;
    g_assert_true(radios_player_topology_remove_cluster(&model, h->cluster, &error)); g_assert_no_error(error);
    radios_player_tree_ui_refresh(h->panel);
    GtkTreeModel *store = gtk_tree_view_get_model(GTK_TREE_VIEW(find(h->panel, "player-tree")));
    g_assert_cmpint(gtk_tree_model_iter_n_children(store, NULL), ==, 2);
    GtkTreeIter grouped; g_assert_true(gtk_tree_model_iter_nth_child(store, &grouped, NULL, 0));
    gchar *name = NULL; gtk_tree_model_get(store, &grouped, 0, &name, -1);
    g_assert_cmpstr(name, ==, "Kümeli"); g_free(name);
    g_assert_cmpint(gtk_tree_model_iter_n_children(store, &grouped), ==, 0);
    assert_folder_selected(h, ""); close_tree(h);
}
static void test_external_selection_cursor_after_resize(void)
{
    Harness *h = open_tree(TRUE, FALSE); GError *error = NULL;
    for (guint i = 0; i < 12; ++i) {
        guint deck = 32; g_assert_true(radios_player_topology_add_player(&model, "", &deck, &error));
    }
    g_assert_no_error(error); radios_player_tree_ui_refresh(h->panel);
    GtkTreeView *tree = GTK_TREE_VIEW(find(h->panel, "player-tree"));
    RadioSAutomationScope player = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 14};
    g_assert_true(radios_player_tree_ui_select(h->panel, &player)); pump();
    RadioSAutomationScope cluster = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER};
    g_strlcpy(cluster.cluster_id, h->cluster, sizeof cluster.cluster_id);
    g_assert_true(radios_player_tree_ui_select(h->panel, &cluster));
    gtk_window_resize(GTK_WINDOW(h->window), 500, 320);
    gint64 deadline = g_get_monotonic_time() + 200 * 1000;
    while (g_get_monotonic_time() < deadline) { pump(); g_usleep(1000); }
    assert_folder_selected(h, h->cluster);
    GtkTreePath *cursor = NULL, *expected = gtk_tree_path_new_from_indices(0, 0, -1);
    gtk_tree_view_get_cursor(tree, &cursor, NULL); g_assert_nonnull(cursor);
    g_assert_cmpint(gtk_tree_path_compare(cursor, expected), ==, 0);
    GtkTreePath *first = NULL, *last = NULL;
    g_assert_true(gtk_tree_view_get_visible_range(tree, &first, &last));
    gchar *first_text = gtk_tree_path_to_string(first), *last_text = gtk_tree_path_to_string(last);
    GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tree));
    g_test_message("Resize visible paths=%s..%s adjustment=%.2f upper=%.2f page=%.2f", first_text, last_text,
        gtk_adjustment_get_value(adjustment), gtk_adjustment_get_upper(adjustment), gtk_adjustment_get_page_size(adjustment));
    g_free(first_text); g_free(last_text);
    g_assert_cmpint(gtk_tree_path_compare(first, expected), <=, 0);
    g_assert_cmpint(gtk_tree_path_compare(last, expected), >=, 0);
    gtk_tree_path_free(first); gtk_tree_path_free(last); gtk_tree_path_free(cursor); gtk_tree_path_free(expected); close_tree(h);
}
static void test_ready_and_dialog_lifetime(void)
{
    Harness *h = open_tree(TRUE, FALSE); loaded = FALSE; radios_player_tree_ui_refresh(h->panel);
    g_assert_false(gtk_widget_get_sensitive(find(h->panel, "player-tree-add-player")));
    g_assert_cmpuint(writes, ==, 0); loaded = TRUE; radios_player_tree_ui_refresh(h->panel);
    GtkWidget *tree = find(h->panel, "player-tree"); GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1);
    g_assert_true(gtk_tree_view_row_expanded(GTK_TREE_VIEW(tree), path)); gtk_tree_path_free(path);
    click(h, "player-tree-add-cluster"); g_assert_nonnull(edit_dialog()); close_tree(h); g_assert_null(edit_dialog());
    for (guint i = 0; i < 4; ++i) { g_usleep(100000); pump(); }
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); if (!gtk_init_check(&argc, &argv)) return 77;
    g_test_add_func("/player-tree/hierarchy-selection-refresh", test_hierarchy_selection_refresh);
    g_test_add_func("/player-tree/management-and-guard", test_management_and_guard);
    g_test_add_func("/player-tree/add-keeps-current-folder", test_add_keeps_current_folder);
    g_test_add_func("/player-tree/nested-folder-expansion", test_nested_folder_expansion);
    g_test_add_func("/player-tree/empty-clustered-folder", test_empty_clustered_folder);
    g_test_add_func("/player-tree/external-selection-cursor-after-resize", test_external_selection_cursor_after_resize);
    g_test_add_func("/player-tree/ready-and-dialog-lifetime", test_ready_and_dialog_lifetime);
    return g_test_run();
}
