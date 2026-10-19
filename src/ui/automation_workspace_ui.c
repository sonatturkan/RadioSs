#include "radios/automation_workspace_ui.h"
#include "radios/player_tree_ui.h"
#include "radios/player_workspace.h"
#include "radios/player_scope_execution.h"
#include "radios/automation_scope_ui.h"
#include "radios/programme_model.h"
#include "radios/widgets.h"
#include <string.h>

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *root, *selector, *title, *summary, *scope_help, *status, *events, *orders, *calendar;
    GtkWidget *schema, *assign, *remove, *schema_name, *playlist, *load, *source, *source_row;
    GtkWidget *mode, *target, *play, *pause, *stop, *play_schema, *edit;
    GtkWidget *autodj_settings, *target_row, *schema_settings;
    RadioSAutomationWorkspaceSelected selected, edit_hook;
    gpointer selected_user, edit_user;
    RadioSAutomationScope scope;
    RadioSDataClient *client;
    JsonNode *projection, *schemas;
    GHashTable *records, *fetching, *failures;
    gchar *signature, *options_signature;
    guint generation, library_generation, timer;
    gboolean has_scope, disposed, updating, silent, library_busy;
} Workspace;
typedef struct {
    GWeakRef owner;
    RadioSAutomationScope scope;
    RadioSAutomationBinding binding;
    guint generation, library_generation;
    gchar id[37];
    gboolean assign;
} ScopeJob;

static JsonObject *object(JsonObject *parent, const gchar *key)
{ JsonNode *n = parent ? json_object_get_member(parent, key) : NULL; return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL; }
static JsonArray *array(JsonObject *parent, const gchar *key)
{ JsonNode *n = parent ? json_object_get_member(parent, key) : NULL; return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL; }
static JsonObject *at(JsonArray *items, guint index)
{ JsonNode *n = items && index < json_array_get_length(items) ? json_array_get_element(items, index) : NULL; return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL; }
static const gchar *text(JsonObject *parent, const gchar *key)
{ return radios_json_string(parent, key, ""); }
static Workspace *view_get(GtkWidget *owner)
{ return owner ? g_object_get_data(G_OBJECT(owner), "automation-workspace-view") : NULL; }
static GtkWidget *named(GtkWidget *widget, const gchar *name)
{ gtk_widget_set_name(widget, name); return widget; }
static void pack(GtkWidget *parent, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(parent), child, expand, expand, 0); }
static void status(Workspace *view, const gchar *message)
{ gtk_label_set_text(GTK_LABEL(view->status), message ? message : ""); }
static void button_label(GtkWidget *button, const gchar *label)
{
    if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(button)), label))
        gtk_button_set_label(GTK_BUTTON(button), label);
}
static gchar *player_name(const RadioSPlayerTopology *topology, guint deck)
{
    const RadioSPlayerDefinition *player = radios_player_topology_player_const(topology, deck);
    return player && *player->name ? g_strdup(player->name) : g_strdup_printf("Oynatıcı %u", deck + 1);
}
static void compact_combo(GtkWidget *combo)
{
    GList *cells=gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(combo));
    for(GList *item=cells;item;item=item->next)
        if(GTK_IS_CELL_RENDERER_TEXT(item->data))g_object_set(item->data,"ellipsize",PANGO_ELLIPSIZE_END,"max-width-chars",26,NULL);
    g_list_free(cells);gtk_widget_set_hexpand(combo,TRUE);
}
static void compact_style(void)
{
    GdkScreen *screen = gdk_screen_get_default();
    if (!screen || g_object_get_data(G_OBJECT(screen), "radios-automation-scope-style")) return;
    static const gchar css[] =
        "#automation-workspace.automation-scope-workspace { padding: 6px; margin: 0; }"
        "#automation-workspace button { min-height: 24px; padding: 4px 8px; font-size: 12px; }"
        "#automation-workspace .scope-autodj-controls, #automation-workspace .scope-playlist-controls { padding: 10px 12px; margin: 0; border: 1px solid #3b454e; border-radius: 6px; background-color: #202529; }"
        "#automation-workspace entry, #automation-workspace combobox button { background-color: #272d32; color: #e4eaed; }"
        "#automation-workspace-play, #automation-workspace-load { background-color: #176878; color: #ffffff; }"
        "#automation-workspace-schema-settings { padding: 6px; }"
        "#automation-workspace-tabs > header { padding: 0; margin: 0; min-height: 28px; }"
        "#automation-workspace-tabs > header tabs { padding: 0; margin: 0; min-height: 28px; }"
        "#automation-workspace-tabs > header tabs tab { min-width: 0; min-height: 24px; padding: 4px 10px; margin: 0; }"
        "#automation-workspace-tabs > header tabs tab label { min-width: 0; min-height: 0; padding: 0; margin: 0; font-size: 12px; }";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_set_data_full(G_OBJECT(screen), "radios-automation-scope-style", provider, g_object_unref);
}
static GtkWidget *tab_label(const gchar *caption, const gchar *description)
{
    GtkWidget *label = gtk_label_new(caption);
    gtk_label_set_line_wrap(GTK_LABEL(label), FALSE);
    gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
    gtk_widget_set_tooltip_text(label, description); return label;
}
static GtkTreeStore *store(GtkWidget *tree)
{ return GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(tree))); }
static GtkWidget *tree_new(const gchar *name)
{
    GtkTreeStore *model = gtk_tree_store_new(5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tree = named(gtk_tree_view_new_with_model(GTK_TREE_MODEL(model)), name); g_object_unref(model);
    const gchar *titles[] = {"Event / parça", "Oynatıcı", "Sanatçı", "Saat / süre", "Durum"};
    for (guint i = 0; i < G_N_ELEMENTS(titles); ++i) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new(); g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_expand(column, i == 0); gtk_tree_view_column_set_min_width(column, i == 0 ? 180 : 65);
        if(i){gtk_tree_view_column_set_sizing(column,GTK_TREE_VIEW_COLUMN_FIXED);gtk_tree_view_column_set_fixed_width(column,i==3?155:100);}
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
    }
    gtk_tree_view_set_reorderable(GTK_TREE_VIEW(tree), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tree), TRUE); return tree;
}
static GtkWidget *scroll(GtkWidget *tree)
{
    GtkWidget *widget = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(widget), tree); gtk_widget_set_vexpand(widget, TRUE); return widget;
}
static GtkTreeIter row_add(GtkWidget *tree, GtkTreeIter *parent, const gchar *title,
    const gchar *player, const gchar *artist, const gchar *time, const gchar *state)
{
    GtkTreeIter row; gtk_tree_store_append(store(tree), &row, parent);
    gtk_tree_store_set(store(tree), &row, 0, title, 1, player, 2, artist, 3, time, 4, state, -1); return row;
}
static gchar *duration(gint64 milliseconds)
{ return milliseconds > 0 ? g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, milliseconds / 60000, milliseconds / 1000 % 60) : g_strdup("—"); }
static void programme_render(GtkWidget *tree, JsonObject *programme, const gchar *name, const gchar *entry_id)
{
    if (!programme) return;
    GtkTreeIter parent = row_add(tree, NULL, text(programme, "title"), name, "", text(programme, "date"), "Yayın sırası");
    RadioSProgrammeTree *flow = radios_programme_tree_new(programme);
    for (guint i = 0; i < flow->blocks->len; ++i) {
        RadioSProgrammeBlock *block = g_ptr_array_index(flow->blocks, i);
        GtkTreeIter group = row_add(tree, &parent, text(block->block, "title"), name, "", text(block->block, "start_time"), "Event");
        for (guint j = 0; j < block->leaves->len; ++j) {
            JsonObject *item = g_ptr_array_index(block->leaves, j); gchar *length = duration(radios_json_int(item, "duration_ms", 0));
            row_add(tree, &group, text(item, "title"), name, text(item, "artist"), length,
                *entry_id && g_str_equal(entry_id, text(item, "id")) ? "Çalıyor" : "Sırada"); g_free(length);
        }
    }
    if (!flow->blocks->len) row_add(tree, &parent, "Henüz event eklenmedi", "", "", "", "");
    radios_programme_tree_free(flow);
}
static void planned_jingle_render(Workspace *view, JsonObject *jingle, GtkTreeIter *event,
    GtkTreeIter *order)
{
    if (!jingle || !*text(jingle, "title")) return;
    gboolean overlay = g_str_equal(text(jingle, "placement"), "overlay");
    gchar *title = g_strdup_printf("Jingle: %s · %s", text(jingle, "title"),
        overlay ? "Şarkının içinde" : "Şarkı sonrası");
    gint64 deck = radios_json_int(jingle, "player", 0);
    gchar *channel = deck >= 1 && deck <= 32 ?
        player_name(radios_player_workspace_topology(view->ctx->players), (guint)deck - 1) : g_strdup("");
    gchar *length = duration(radios_json_int(jingle, "duration_ms", 0));
    row_add(view->events, event, title, channel, text(jingle, "artist"), length, "Planlanan jingle");
    row_add(view->orders, order, title, channel, text(jingle, "artist"), length, "Planlanan jingle");
    g_free(length); g_free(channel); g_free(title);
}
static void planned_jingles_render(Workspace *view, JsonObject *entry, GtkTreeIter *event,
    GtkTreeIter *order)
{
    JsonArray *jingles = array(entry, "planned_jingles");
    if (jingles) {
        for (guint i = 0; i < json_array_get_length(jingles); ++i)
            planned_jingle_render(view, at(jingles, i), event, order);
    } else planned_jingle_render(view, object(entry, "planned_jingle"), event, order);
}
static gboolean order_active(JsonObject *order)
{
    gint64 state=radios_json_int(order,"state",RADIOS_PLAYOUT_IDLE);
    JsonArray *entries=array(order,"entries");
    return state>RADIOS_PLAYOUT_IDLE && state<RADIOS_PLAYOUT_FINISHED && entries && json_array_get_length(entries)>0;
}
/* Events and AutoDJ are two views of the same accepted live sequence. The
 * stored editable programme remains the idle view; it never supplies labels
 * or old rows for a running schema/refill. Consumed rows are not live events. */
static gboolean order_render(Workspace *view, JsonObject *order, GHashTable *seen)
{
    gint64 source=radios_json_int(order,"source",0);
    if (source<0 || source>32 || !order_active(order)) return FALSE;
    /* Zero identifies the global controller; it must not collide with a
     * scoped source in the de-duplication set. */
    gpointer owner_key=GINT_TO_POINTER((gint)source+1);
    if (g_hash_table_contains(seen,owner_key)) return TRUE;
    g_hash_table_add(seen,owner_key);
    gchar *name=source ? player_name(radios_player_workspace_topology(view->ctx->players),(guint)source-1) : g_strdup("Genel yayın");
    guint32 selected=0, owned=0;
    RadioSAutomationBinding binding;
    if (view->scope.kind==RADIOS_AUTOMATION_SCOPE_PLAYER) selected=(guint32)1<<view->scope.deck;
    else if (radios_automation_scope_resolve(radios_player_workspace_topology(view->ctx->players),&view->scope,&binding,NULL))
        for(guint i=0;i<binding.count;++i)selected|=(guint32)1<<binding.decks[i];
    JsonArray *pool=array(order,"players");
    for(guint i=0;pool && i<json_array_get_length(pool);++i){gint64 deck=json_array_get_int_element(pool,i);if(deck>=1 && deck<=32)owned|=(guint32)1<<(deck-1);}
    gboolean unresolved_visible=source==0 && owned && !(owned & ~selected);
    guint state=(guint)radios_json_int(order,"state",RADIOS_PLAYOUT_IDLE);
    const gchar *state_text=radios_playout_ui_state_name(state), *programme=text(order,"programme_title");
    GtkTreeIter order_parent=row_add(view->orders,NULL,"Çalışan AutoDJ sırası",name,"","",state_text);
    GtkTreeIter event_parent=row_add(view->events,NULL,*programme?programme:"Çalışan yayın sırası",name,"","",state_text);
    JsonArray *entries=array(order,"entries");
    guint index=(guint)MAX(radios_json_int(order,"index",0),0), shown=0;
    GHashTable *groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (guint i=index;i<json_array_get_length(entries);++i) {
        JsonObject *entry=at(entries,i); gint64 deck=radios_json_int(entry,"player",0);
        /* Unknown future routing is shown only when the entire actual
         * general pool belongs to this selection. Never invent a deck or
         * leak the other cluster's future rows into a child selection. */
        if (deck>=1 && deck<=32) { if (!(selected & ((guint32)1<<(deck-1)))) continue; }
        else if (!unresolved_visible) continue;
        gchar *channel=deck>=1 && deck<=32?player_name(radios_player_workspace_topology(view->ctx->players),(guint)deck-1):g_strdup(name);
        gint64 planned=radios_json_int(entry,"planned_utc_us",0);
        GDateTime *stamp=planned>0?g_date_time_new_from_unix_utc(planned/G_USEC_PER_SEC):NULL;
        gchar *time=stamp?g_date_time_format(stamp,"%H:%M:%S UTC"):g_strdup("");if(stamp)g_date_time_unref(stamp);
        const gchar *row_state=i==index?state_text:state==RADIOS_PLAYOUT_OVERLAP && i==(guint)radios_json_int(order,"next_index",index+1)?"Geçişte":"Sırada";
        const gchar *group_id = text(entry, "block_id");
        if (!*group_id) group_id = text(entry, "entry_id");
        const gchar *group = text(entry, "block_title");
        if (!*group) group = text(entry, "source_name");
        if (!*group) group = radios_programme_event_title(text(entry, "kind"));
        /* Names are captions, never identities: two distinct automation
         * blocks may both be called Müzik eventi. One block remains one
         * Event even when player filtering hides some of its songs. */
        GtkTreeIter *event_group = g_hash_table_lookup(groups, group_id);
        if (!event_group) {
            event_group = g_new(GtkTreeIter, 1);
            *event_group = row_add(view->events, &event_parent, group, name, "", "", "Event");
            g_hash_table_insert(groups, g_strdup(group_id), event_group);
        }
        GtkTreeIter order_row = row_add(view->orders,&order_parent,text(entry,"title"),channel,text(entry,"artist"),time,row_state);
        GtkTreeIter event_row = row_add(view->events,event_group,text(entry,"title"),channel,text(entry,"artist"),time,row_state);
        planned_jingles_render(view, entry, &event_row, &order_row);
        ++shown;g_free(channel);g_free(time);
    }
    if (!shown) {
        row_add(view->events,&event_parent,"Bu oynatıcıda sırada parça yok",name,"","",state_text);
        row_add(view->orders,&order_parent,"Bu oynatıcıda sırada parça yok",name,"","",state_text);
    }
    g_hash_table_unref(groups);g_free(name);return TRUE;
}
static void projection_clear(Workspace *view)
{
    gtk_tree_store_clear(store(view->events));gtk_tree_store_clear(store(view->orders));gtk_tree_store_clear(store(view->calendar));
    g_clear_pointer(&view->signature,g_free);g_clear_pointer(&view->projection,json_node_free);
}
static void calendar_render(Workspace *view, const gchar *id, const gchar *name)
{
    if (!id || !*id) return;
    JsonNode *saved = g_hash_table_lookup(view->records, id); JsonObject *record = saved ? json_node_get_object(saved) : NULL;
    GtkTreeIter parent = row_add(view->calendar, NULL, record ? text(record, "name") : g_hash_table_contains(view->failures, id) ? "Şema kullanılamıyor" : "Şema okunuyor…", name, "", "", "Otomasyon şeması");
    JsonArray *events = array(object(object(record, "config"), "calendar"), "events");
    for (guint i = 0; events && i < json_array_get_length(events); ++i) {
        JsonObject *event = at(events, i); gint64 begin = radios_json_int(event, "start_minute", 0), end = radios_json_int(event, "end_minute", 0);
        gchar *time = g_strdup_printf("%s · %02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT "–%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
            text(event, "date"), begin / 60, begin % 60, end / 60, end % 60);
        GtkTreeIter group = row_add(view->calendar, &parent, text(event, "title"), name, "", time, "Program eventi"); g_free(time);
        JsonArray *rotations = array(event, "rotations");
        for (guint j = 0; rotations && j < json_array_get_length(rotations); ++j) {
            JsonObject *rotation = at(rotations, j); const gchar *rotation_name = "Playlist rotasyonu";
            const gchar *rotation_id = text(rotation, "rotation_id");
            JsonArray *library = array(object(object(record, "config"), "calendar"), "rotation_library");
            for (guint k = 0; library && k < json_array_get_length(library); ++k)
                if (g_str_equal(text(at(library, k), "id"), rotation_id)) rotation_name = text(at(library, k), "name");
            row_add(view->calendar, &group, rotation_name, "", "", "", "Kaynak");
        }
    }
    if (record && (!events || !json_array_get_length(events))) row_add(view->calendar, &parent, "Bu şemada event yok", "", "", "", "");
}
static void view_refresh(Workspace *view);
static ScopeJob *job_new(Workspace *view)
{
    ScopeJob *job = g_new0(ScopeJob, 1); g_weak_ref_init(&job->owner, view->root); job->scope = view->scope; job->generation = view->generation; job->library_generation = view->library_generation; return job;
}
static void job_free(gpointer value)
{ ScopeJob *job = value; g_weak_ref_clear(&job->owner); g_free(job); }
static gboolean snapshot_definition_active(const RadioSPlayerScopeExecutionSnapshot *snapshot)
{
    return snapshot->schema_active || snapshot->preparing ||
        (snapshot->state == RADIOS_SCOPE_EXECUTION_PREPARING && !snapshot->loading_playlist) ||
        snapshot->state == RADIOS_SCOPE_EXECUTION_WAITING || snapshot->state == RADIOS_SCOPE_EXECUTION_PLAYING ||
        snapshot->state == RADIOS_SCOPE_EXECUTION_PAUSED;
}
static gboolean scope_active(Workspace *view, const RadioSAutomationScope *scope)
{
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(view->ctx);
    RadioSPlayerScopeExecutionSnapshot snapshot = {0};
    if (!execution || !radios_player_scope_execution_snapshot(execution, scope, &snapshot)) return FALSE;
    return snapshot_definition_active(&snapshot);
}
static gboolean definition_editable(Workspace *view, const RadioSAutomationScope *scope)
{
    if (!scope_active(view, scope)) return TRUE;
    status(view, "AutoDJ ve şema ayarlarını değiştirmek için önce bu yayın sırasını durdurun.");
    return FALSE;
}
static void schema_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer user)
{
    (void)ctx; ScopeJob *job = user; Workspace *view = view_get(owner); if (!view || view->disposed || job->library_generation != view->library_generation) return;
    g_hash_table_remove(view->fetching, job->id);
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        if (job->generation == view->generation) status(view, error ? error->message : "Otomasyon şeması okunamadı.");
        /* Retrying requires the explicit list refresh, avoiding repeated HTTP
         * requests for a removed or disabled assignment on every UI tick. */
        g_hash_table_add(view->failures, g_strdup(job->id));
        g_clear_pointer(&view->signature, g_free); view_refresh(view);
        return;
    }
    if (job->assign) {
        if (job->generation != view->generation || !definition_editable(view, &job->scope)) return;
        GError *failure = NULL;
        if (!radios_automation_scope_binding_current(radios_player_workspace_topology(view->ctx->players), &job->binding, &failure) ||
            !radios_player_workspace_assign(view->ctx->players, &job->scope, job->id, &failure)) {
            status(view, failure ? failure->message : "Şema atanamadı."); g_clear_error(&failure); return;
        }
        status(view, "Otomasyon şeması seçili kapsama atandı.");
    }
    JsonNode *copy = json_node_new(JSON_NODE_OBJECT); json_node_take_object(copy, radios_programme_clone_object(json_node_get_object(result)));
    g_hash_table_replace(view->records, g_strdup(job->id), copy); g_clear_pointer(&view->signature, g_free); view_refresh(view);
}
static void schema_fetch(Workspace *view, const gchar *id)
{
    if (!id || !*id || g_hash_table_contains(view->records, id) || g_hash_table_contains(view->fetching, id) || g_hash_table_contains(view->failures, id) || !radios_data_connected(view->ctx)) return;
    ScopeJob *job = job_new(view); g_strlcpy(job->id, id, sizeof job->id); g_hash_table_add(view->fetching, g_strdup(id));
    if (!radios_automation_scope_fetch(view->ctx, view->root, id, 0, schema_received, job, job_free)) g_hash_table_remove(view->fetching, id);
}
static RadioSAutomationScope execution_scope(Workspace *view)
{ return view->scope; }
static gboolean source_binding(Workspace *view, RadioSAutomationBinding *binding)
{
    RadioSAutomationScope scope = execution_scope(view);
    return view->has_scope && radios_automation_scope_resolve(radios_player_workspace_topology(view->ctx->players), &scope, binding, NULL) && binding->count;
}
static void choices_render(Workspace *view)
{
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(view->ctx->players); RadioSAutomationBinding selected;
    if (!topology || !radios_automation_scope_resolve(topology, &view->scope, &selected, NULL)) return;
    JsonObject *definition = radios_player_topology_to_json(topology); JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, definition);
    gchar *encoded = json_to_string(node, FALSE); json_node_free(node);
    gchar *signature = g_strdup_printf("%s:%u:%u:%s", encoded, (guint)view->scope.kind, view->scope.deck, view->scope.cluster_id); g_free(encoded);
    if (!g_strcmp0(signature, view->options_signature)) { g_free(signature); return; }
    g_free(view->options_signature); view->options_signature = signature; view->updating = TRUE;
    gchar *name = selected.count ? player_name(topology, selected.source) : g_strdup("Aynı kapsamda oynatıcı tanımlayın");
    gtk_label_set_text(GTK_LABEL(view->source), name); g_free(name);
    gtk_widget_set_visible(view->source_row, view->scope.kind != RADIOS_AUTOMATION_SCOPE_PLAYER);
    RadioSAutomationBinding source;
    gboolean valid = source_binding(view, &source); gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(view->target));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->target), "-1", "Sıradaki uygun oynatıcı");
    if (valid) {
        const RadioSPlayerDefinition *player = radios_player_topology_player_const(topology, source.source);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->mode), player->autodj_mode == RADIOS_AUTODJ_TRANSITIONS ? "2" : "1");
        guint peers[32]; guint count = radios_player_topology_members(topology, player->cluster_id, peers, G_N_ELEMENTS(peers));
        for (guint i = 0; i < count; ++i) if (peers[i] != source.source) {
            gchar *id = g_strdup_printf("%u", peers[i]), *name = player_name(topology, peers[i]);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->target), id, name); g_free(id); g_free(name);
        }
        gchar *id = g_strdup_printf("%d", player->autodj_target);
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->target), id)) gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->target), "-1");
        g_free(id);
    } else gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->target), "-1");
    view->updating = FALSE;
}
static void view_refresh(Workspace *view)
{
    if (view->disposed || !view->events) return;
    gboolean ready = view->ctx->players && radios_player_workspace_ready(view->ctx->players) && view->has_scope;
    const RadioSPlayerDefinition *selected_player = ready && view->scope.kind == RADIOS_AUTOMATION_SCOPE_PLAYER ?
        radios_player_topology_player_const(radios_player_workspace_topology(view->ctx->players), view->scope.deck) : NULL;
    gboolean independent = ready && (view->scope.kind == RADIOS_AUTOMATION_SCOPE_UNCLUSTERED || (selected_player && !*selected_player->cluster_id));
    gtk_widget_set_visible(view->autodj_settings, independent);
    gtk_widget_set_visible(view->schema_settings, ready && view->scope.kind != RADIOS_AUTOMATION_SCOPE_UNCLUSTERED);
    gtk_widget_set_sensitive(view->edit, ready && view->scope.kind == RADIOS_AUTOMATION_SCOPE_PLAYER && view->edit_hook != NULL);
    gtk_widget_set_sensitive(view->source, ready);
    GtkWidget *controls[] = { view->assign, view->load, view->play, view->pause, view->stop,
        view->mode, view->schema, view->remove, view->play_schema, view->target };
    if (!ready) {
        for (guint i = 0; i < G_N_ELEMENTS(controls); ++i) gtk_widget_set_sensitive(controls[i], FALSE);
        gtk_label_set_text(GTK_LABEL(view->summary), "Oynatıcı tanımları yükleniyor…"); projection_clear(view); return;
    }
    choices_render(view);
    gtk_widget_set_visible(view->target_row, !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->mode)), "2"));
    GError *error = NULL; JsonNode *projection = radios_player_workspace_projection(view->ctx->players, &view->scope, &error);
    if (!projection) {
        for (guint i = 0; i < G_N_ELEMENTS(controls); ++i) gtk_widget_set_sensitive(controls[i], FALSE);
        status(view, error ? error->message : "Seçili kapsam artık yok."); projection_clear(view); g_clear_error(&error); return;
    }
    JsonObject *root = json_node_get_object(projection); JsonArray *players = array(root, "players");
    const gchar *schema_id = text(root, "automation_id"); schema_fetch(view, schema_id);
    for (guint i = 0; players && i < json_array_get_length(players); ++i) schema_fetch(view, text(at(players, i), "automation_id"));
    JsonNode *saved = g_hash_table_lookup(view->records, schema_id); const gchar *schema_name = saved ? text(json_node_get_object(saved), "name") : g_hash_table_contains(view->failures, schema_id) ? "Şema kullanılamıyor · listeleri yenile" : *schema_id ? "Şema okunuyor…" : "Şema atanmadı";
    RadioSAutomationBinding binding = {0}; radios_automation_scope_resolve(radios_player_workspace_topology(view->ctx->players), &view->scope, &binding, NULL);
    gboolean playable = binding.count > 0;
    gtk_label_set_text(GTK_LABEL(view->scope_help), !playable ? "Önce Canlı oynatıcılar bölümünden bu alana bir oynatıcı ekleyin." : "");
    gchar *caption = binding.inherited ? g_strdup_printf("Kümeden: %s", schema_name) : g_strdup(schema_name);
    gtk_label_set_text(GTK_LABEL(view->schema_name), caption); g_free(caption);
    const RadioSPlayerDefinition *individual = view->scope.kind == RADIOS_AUTOMATION_SCOPE_PLAYER ? radios_player_topology_player_const(radios_player_workspace_topology(view->ctx->players), view->scope.deck) : NULL;
    button_label(view->remove, individual && *individual->cluster_id ? "Küme şemasını kullan" : "Atamayı kaldır");
    guint playing = 0; for (guint i = 0; players && i < json_array_get_length(players); ++i) {
        JsonObject *player = at(players, i); JsonNode *n = player ? json_object_get_member(player, "playing") : NULL;
        playing += n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_BOOLEAN && json_node_get_boolean(n);
    }
    gtk_label_set_text(GTK_LABEL(view->title), text(root, "name"));
    gchar *summary = g_strdup_printf("%u oynatıcı · %u çalıyor", players ? json_array_get_length(players) : 0, playing);
    RadioSAutomationScope executing = execution_scope(view);
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(view->ctx);
    RadioSPlayerScopeExecutionSnapshot runtime = {0};
    if (execution && radios_player_scope_execution_snapshot(execution, &executing, &runtime)) {
        static const gchar *const states[] = {"Beklemede", "Hazırlanıyor", "Program saatini bekliyor", "Çalıyor", "Duraklatıldı", "Hata"};
        const gchar *state = runtime.state <= RADIOS_SCOPE_EXECUTION_ERROR ? states[runtime.state] : "";
        gchar *live = g_strdup_printf("%s · %s%s%s", summary, state, *runtime.event_name ? " · " : "", runtime.event_name);
        gtk_label_set_text(GTK_LABEL(view->summary), live); g_free(live);
        button_label(view->play, runtime.state == RADIOS_SCOPE_EXECUTION_PAUSED ? "Devam et" : "Play");
    } else {
        gtk_label_set_text(GTK_LABEL(view->summary), summary); button_label(view->play, "Play");
    }
    g_free(summary);
    gboolean active = snapshot_definition_active(&runtime);
    gboolean has_target = gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(view->target)), NULL) > 1;
    gboolean transitions = !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->mode)), "2");
    gboolean missing_target = independent && playable && !active && !has_target && transitions;
    gboolean connected = radios_data_connected(view->ctx);
    gboolean schema_scope = view->scope.kind != RADIOS_AUTOMATION_SCOPE_UNCLUSTERED;
    /* Polls must apply only the final state: enabling a locked control first
     * restarts GTK state transitions even when the observed state is unchanged. */
    gtk_widget_set_sensitive(view->load, playable && connected);
    gtk_widget_set_sensitive(view->play, playable && !missing_target);
    gtk_widget_set_sensitive(view->pause, playable);
    gtk_widget_set_sensitive(view->stop, playable);
    gtk_widget_set_sensitive(view->play_schema, playable && !missing_target && schema_scope && *schema_id && connected);
    gtk_widget_set_sensitive(view->mode, playable && !active);
    gtk_widget_set_sensitive(view->target, playable && has_target && !active && transitions);
    gtk_widget_set_sensitive(view->assign, !active && schema_scope && connected);
    gtk_widget_set_sensitive(view->remove, !active && (individual ? *individual->automation_id != 0 : *schema_id != 0));
    gtk_widget_set_sensitive(view->schema, !active && schema_scope);
    if (missing_target) {
        gtk_label_set_text(GTK_LABEL(view->scope_help), "Geçiş için ikinci bir kümesiz oynatıcı ekleyin veya tek oynatıcıda devam etmeyi seçin.");
    }
    /* Future schema waits and cancellation can change without an accepted
     * batch. Include their observed state in the read-only render identity. */
    json_object_set_int_member(root,"execution_state",runtime.state);
    json_object_set_boolean_member(root,"execution_active",active);
    json_object_set_string_member(root,"execution_event",runtime.event_name);
    json_object_set_int_member(root,"execution_start",runtime.starts_at_utc_us);
    json_object_set_int_member(root,"execution_end",runtime.ends_at_utc_us);
    gchar *signature = json_to_string(projection, FALSE);
    if (!g_strcmp0(signature, view->signature)) { g_free(signature); json_node_free(projection); return; }
    g_free(view->signature); view->signature = signature; g_clear_pointer(&view->projection, json_node_free); view->projection = projection;
    gtk_tree_store_clear(store(view->events)); gtk_tree_store_clear(store(view->orders)); gtk_tree_store_clear(store(view->calendar));
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    JsonArray *orders = array(root, "autodj_orders");
    for (guint i = 0; orders && i < json_array_get_length(orders); ++i) order_render(view, at(orders, i), seen);
    for (guint i = 0; players && i < json_array_get_length(players); ++i)
        order_render(view,object(at(players,i),"autodj_order"),seen);
    gboolean live=g_hash_table_size(seen)>0;
    if (!live && !active && object(root,"programme")) programme_render(view->events,object(root,"programme"),text(root,"name"),"");
    if (!live && active) {
        const gchar *waiting=runtime.state==RADIOS_SCOPE_EXECUTION_WAITING?"Program saatini bekliyor":"Yayın sırası hazırlanıyor";
        row_add(view->events,NULL,*runtime.event_name?runtime.event_name:waiting,text(root,"name"),"","",waiting);
    }
    calendar_render(view, schema_id, text(root, "name"));
    for (guint i = 0; players && i < json_array_get_length(players); ++i) {
        JsonObject *player = at(players, i); guint deck = (guint)radios_json_int(player, "player", 1) - 1;
        gchar *name = player_name(radios_player_workspace_topology(view->ctx->players), deck);
        RadioSAutomationScope player_scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = deck};
        /* A live neighbour must not hide this independent player's queue.
         * Only that player's accepted owner or pending schema replaces its
         * stored rows; accepted sources are rendered once above. */
        if (!order_active(object(player,"autodj_order")) && !scope_active(view, &player_scope)) {
            if (object(player,"programme")) programme_render(view->events,object(player,"programme"),name,"");
            else row_add(view->events,NULL,"Playlist / event yüklenmedi",name,"","","Boş sıra");
        }
        if (g_strcmp0(schema_id, text(player, "automation_id"))) calendar_render(view, text(player, "automation_id"), name);
        g_free(name);
    }
    if (!live) row_add(view->orders, NULL, "Çalışan AutoDJ sırası yok", "", "", "", "");
    g_hash_table_unref(seen);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(view->events)); gtk_tree_view_expand_all(GTK_TREE_VIEW(view->orders)); gtk_tree_view_expand_all(GTK_TREE_VIEW(view->calendar));
}
static void selection_changed(const RadioSAutomationScope *scope, gpointer user)
{
    Workspace *view = user; if (!scope || view->disposed) return;
    view->scope = *scope; view->has_scope = TRUE; ++view->generation;
    g_clear_pointer(&view->signature, g_free); g_clear_pointer(&view->options_signature, g_free); view_refresh(view);
    if (!view->silent && view->selected) view->selected(scope, view->selected_user);
}
static gboolean client_same(Workspace *view)
{
    RadioSDataClient *current = radios_data_client_acquire(view->ctx); gboolean same = current && current == view->client;
    if (current) radios_data_client_unref(current);
    return same;
}
static void schemas_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer user)
{
    (void)ctx; ScopeJob *job = user; Workspace *view = view_get(owner);
    if (!view || view->disposed || job->generation != view->library_generation) return;
    view->library_busy = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result) || !client_same(view)) { status(view, error ? error->message : "Oturum değişti; listeleri yenileyin."); return; }
    g_clear_pointer(&view->schemas, json_node_free); view->schemas = radios_programme_clone(result);
    gchar *active = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->schema)));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(view->schema)); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->schema), "", "Kaydedilmiş şema seç");
    JsonArray *records = array(json_node_get_object(result), "records");
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = at(records, i); const gchar *id = text(record, "_id");
        if (radios_automation_scope_record_valid(record, radios_data_client_station_id(view->client), id, 0, NULL))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->schema), id, text(record, "name"));
    }
    if (!active || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->schema), active)) gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->schema), "");
    g_free(active);
    view_refresh(view);
}
static void playlists_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer user)
{
    (void)ctx; ScopeJob *job = user; Workspace *view = view_get(owner);
    if (!view || view->disposed || job->generation != view->library_generation) return;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result) || !client_same(view)) { status(view, error ? error->message : "Playlist listesi okunamadı."); return; }
    gchar *active = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->playlist)));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(view->playlist)); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->playlist), "", "Playlist seç");
    JsonArray *records = array(json_node_get_object(result), "playlists");
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = at(records, i); const gchar *id = text(record, "_id");
        if (g_uuid_string_is_valid(id) && g_str_equal(text(record, "station_id"), radios_data_client_station_id(view->client)) && g_str_equal(text(record, "status"), "active"))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->playlist), id, text(record, "name"));
    }
    if (!active || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->playlist), active)) gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->playlist), "");
    g_free(active);
}
static void libraries_reload(GtkButton *button, gpointer user)
{
    (void)button; Workspace *view = user;
    if (view->disposed || !radios_data_connected(view->ctx)) return;
    if (view->client) radios_data_client_unref(view->client);
    view->client = radios_data_client_acquire(view->ctx);
    ++view->library_generation; view->library_busy = TRUE;
    g_hash_table_remove_all(view->records); g_hash_table_remove_all(view->fetching); g_hash_table_remove_all(view->failures); g_clear_pointer(&view->signature, g_free);
    JsonNode *body = radios_data_body(view->ctx); json_object_set_string_member(json_node_get_object(body), "kind", "weekly_programme");
    ScopeJob *job = job_new(view); job->generation = view->library_generation;
    if (!radios_data_request(view->ctx, view->root, "POST", "/v1/workflows/desktop/list", body, schemas_received, job, job_free)) view->library_busy = FALSE;
    json_node_free(body); body = radios_data_body(view->ctx); job = job_new(view); job->generation = view->library_generation;
    radios_data_request(view->ctx, view->root, "POST", "/v1/workflows/desktop-playlists/list", body, playlists_received, job, job_free); json_node_free(body);
}
static void schema_assign(GtkButton *button, gpointer user)
{
    (void)button; Workspace *view = user; const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->schema));
    if (!definition_editable(view, &view->scope)) return;
    if (!view->has_scope || !id || !*id || view->scope.kind == RADIOS_AUTOMATION_SCOPE_UNCLUSTERED) { status(view, "Atamak için kaydedilmiş bir şema seçin."); return; }
    ScopeJob *job = job_new(view); job->assign = TRUE; g_strlcpy(job->id, id, sizeof job->id);
    GError *error = NULL;
    if (!radios_automation_scope_resolve(radios_player_workspace_topology(view->ctx->players), &view->scope, &job->binding, &error)) {
        status(view, error ? error->message : "Kapsam geçersiz."); g_clear_error(&error); job_free(job); return;
    }
    gint64 revision = 0; JsonArray *records = array(view->schemas ? json_node_get_object(view->schemas) : NULL, "records");
    for (guint i = 0; records && i < json_array_get_length(records); ++i) if (g_str_equal(text(at(records, i), "_id"), id)) revision = radios_json_int(at(records, i), "_rev", 0);
    status(view, "Şema yeniden doğrulanıyor…");
    if (!radios_automation_scope_fetch(view->ctx, view->root, id, revision, schema_received, job, job_free)) status(view, "Şema bağlantısı hazır değil.");
}
static void schema_remove(GtkButton *button, gpointer user)
{
    (void)button; Workspace *view = user; GError *error = NULL;
    if (!definition_editable(view, &view->scope)) return;
    if (!radios_player_workspace_assign(view->ctx->players, &view->scope, "", &error)) status(view, error ? error->message : "Atama kaldırılamadı.");
    else { status(view, "Ayrı şema ataması kaldırıldı."); g_clear_pointer(&view->signature, g_free); view_refresh(view); }
    g_clear_error(&error);
}
static gboolean options_save(Workspace *view, GError **error)
{
    RadioSAutomationBinding binding; if (!source_binding(view, &binding)) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Bir oynatıcı seçin."); return FALSE; }
    RadioSPlayerTopology next = *radios_player_workspace_topology(view->ctx->players);
    /* This form configures independent players only. Cluster playback must
     * not rewrite its source definition through a hidden control. */
    if (*next.players[binding.source].cluster_id) return TRUE;
    const gchar *mode_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->mode)), *target_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->target));
    RadioSAutoDJMode mode = !g_strcmp0(mode_id, "2") ? RADIOS_AUTODJ_TRANSITIONS : RADIOS_AUTODJ_SEMI;
    gint target = mode == RADIOS_AUTODJ_TRANSITIONS && target_id ? (gint)g_ascii_strtoll(target_id, NULL, 10) : -1;
    if (next.players[binding.source].autodj_mode == mode && next.players[binding.source].autodj_target == target) return TRUE;
    return radios_player_topology_set_autodj(&next, binding.source, mode, target, error) && radios_player_workspace_replace(view->ctx->players, &next, error);
}
static void options_changed(GtkComboBox *combo, gpointer user)
{
    (void)combo; Workspace *view = user; if (view->updating || view->disposed || !view->has_scope) return;
    if (!definition_editable(view, &view->scope)) {
        g_clear_pointer(&view->options_signature, g_free); view_refresh(view); return;
    }
    GError *error = NULL; if (!options_save(view, &error)) status(view, error ? error->message : "AutoDJ ayarı kaydedilemedi.");
    else status(view, "AutoDJ geçiş ayarı kaydedildi.");
    g_clear_error(&error); g_clear_pointer(&view->options_signature, g_free); view_refresh(view);
}
static void execution_done(RadioSViewContext *ctx, const RadioSAutomationScope *scope, guint source, const GError *error, gpointer user)
{
    (void)ctx; (void)scope; (void)source; ScopeJob *job = user; GtkWidget *owner = g_weak_ref_get(&job->owner);
    Workspace *view = view_get(owner);
    if (view && !view->disposed && job->generation == view->generation) {
        status(view, error ? error->message : "Yayın sırası hazır."); g_clear_pointer(&view->signature, g_free); view_refresh(view);
    }
    g_clear_object(&owner);
}
static void transport(GtkButton *button, gpointer user)
{
    Workspace *view = user; RadioSAutomationScope scope = execution_scope(view);
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(view->ctx); GError *error = NULL;
    if (!execution) { status(view, "Yayın motoru bağlantısı bekleniyor."); return; }
    if (GTK_WIDGET(button) == view->pause || GTK_WIDGET(button) == view->stop) {
        radios_player_scope_execution_action(execution, &scope, GTK_WIDGET(button) == view->stop ? RADIOS_PLAYOUT_STOP : RADIOS_PLAYOUT_PAUSE, &error);
        status(view, error ? error->message : GTK_WIDGET(button) == view->stop ? "Yayın sırası durduruldu." : "Yayın sırası duraklatıldı."); g_clear_error(&error); view_refresh(view); return;
    }
    RadioSPlayerScopeExecutionSnapshot runtime = {0};
    if (GTK_WIDGET(button) == view->play && radios_player_scope_execution_snapshot(execution, &scope, &runtime) && runtime.state == RADIOS_SCOPE_EXECUTION_PAUSED) {
        radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_RESUME, &error);
        status(view, error ? error->message : "Yayın sırası devam ediyor."); g_clear_error(&error); view_refresh(view); return;
    }
    if (!options_save(view, &error)) { status(view, error ? error->message : "AutoDJ ayarı geçersiz."); g_clear_error(&error); return; }
    const gchar *mode_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->mode)); RadioSAutoDJMode mode = !g_strcmp0(mode_id, "2") ? RADIOS_AUTODJ_TRANSITIONS : RADIOS_AUTODJ_SEMI;
    RadioSAutomationBinding selected_binding;
    if (source_binding(view, &selected_binding)) {
        const RadioSPlayerDefinition *source = radios_player_topology_player_const(radios_player_workspace_topology(view->ctx->players), selected_binding.source);
        /* A cluster scope plays together (resolved mode follows membership);
         * a clustered player node follows its own definition. */
        if (source && *source->cluster_id)
            mode = selected_binding.autodj_mode == RADIOS_AUTODJ_OFF ?
                (selected_binding.count > 1 ? RADIOS_AUTODJ_TRANSITIONS : RADIOS_AUTODJ_SEMI) : selected_binding.autodj_mode;
    }
    ScopeJob *job = job_new(view); gboolean accepted;
    JsonObject *stored = NULL; GError *queue_error = NULL;
    gboolean has_programme = radios_player_workspace_get_programme(view->ctx->players, &scope, &stored, &queue_error);
    g_clear_pointer(&stored, json_object_unref); g_clear_error(&queue_error);
    RadioSAutomationBinding binding;
    gboolean assigned = radios_automation_scope_resolve(radios_player_workspace_topology(view->ctx->players), &scope, &binding, NULL) && *binding.automation_id;
    if (GTK_WIDGET(button) == view->play_schema || (!has_programme && assigned))
        accepted = radios_player_scope_execution_play_schema(execution, &scope, mode, execution_done, job, job_free, &error);
    else accepted = radios_player_scope_execution_play_programme(execution, &scope, NULL, mode, execution_done, job, job_free, &error);
    status(view, accepted ? "Yayın sırası ve medya doğrulanıyor…" : error ? error->message : "Yayın sırası başlatılamadı."); g_clear_error(&error);
}
static void playlist_load(GtkButton *button, gpointer user)
{
    (void)button; Workspace *view = user; const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(view->playlist));
    if (!id || !*id) { status(view, "Yüklemek için bir playlist seçin."); return; }
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(view->ctx); if (!execution) { status(view, "Yayın motoru bağlantısı bekleniyor."); return; }
    RadioSAutomationScope scope = execution_scope(view); ScopeJob *job = job_new(view); GError *error = NULL;
    gboolean accepted = radios_player_scope_execution_load_playlist(execution, &scope, id, execution_done, job, job_free, &error);
    status(view, accepted ? "Playlist yükleniyor; ses Play ile başlar." : error ? error->message : "Playlist yüklenemedi."); g_clear_error(&error);
}
static void edit_clicked(GtkButton *button, gpointer user)
{ (void)button; Workspace *view = user; RadioSAutomationScope scope = execution_scope(view); if (view->edit_hook && scope.kind == RADIOS_AUTOMATION_SCOPE_PLAYER) view->edit_hook(&scope, view->edit_user); }
static gboolean tick(gpointer user)
{
    Workspace *view = user; if (view->disposed) return G_SOURCE_REMOVE;
    if (radios_data_connected(view->ctx) && (!view->client || !client_same(view)) && !view->library_busy) libraries_reload(NULL, view);
    view_refresh(view); return G_SOURCE_CONTINUE;
}
static void view_destroyed(GtkWidget *widget, gpointer user)
{ (void)widget; Workspace *view = user; view->disposed = TRUE; ++view->generation; ++view->library_generation; if (view->timer) { g_source_remove(view->timer); view->timer = 0; } }
static void view_free(gpointer user)
{
    Workspace *view = user; if (view->client) radios_data_client_unref(view->client);
    g_clear_pointer(&view->projection, json_node_free); g_clear_pointer(&view->schemas, json_node_free);
    g_hash_table_unref(view->records); g_hash_table_unref(view->fetching); g_hash_table_unref(view->failures); g_free(view->signature); g_free(view->options_signature); g_free(view);
}
GtkWidget *radios_automation_workspace_ui_new(RadioSViewContext *ctx, RadioSAutomationWorkspaceSelected selected, gpointer user)
{
    if (!ctx) return NULL;
    compact_style();
    Workspace *view = g_new0(Workspace, 1); view->ctx = ctx; view->selected = selected; view->selected_user = user;
    view->scope.kind = RADIOS_AUTOMATION_SCOPE_UNCLUSTERED; view->has_scope = TRUE;
    view->records = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_free);
    view->fetching = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    view->failures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    view->root = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "task-view automation-scope-workspace"), "automation-workspace");
    g_object_set_data_full(G_OBJECT(view->root), "automation-workspace-view", view, view_free);
    g_signal_connect(view->root, "destroy", G_CALLBACK(view_destroyed), view);
    GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    view->title = named(radios_ui_label("Kümesizler", "strong"), "automation-workspace-title"); pack(heading, view->title, TRUE);
    view->summary = named(radios_ui_label("Oynatıcı tanımları yükleniyor…", "small"), "automation-workspace-summary");
    gtk_label_set_ellipsize(GTK_LABEL(view->summary),PANGO_ELLIPSIZE_END);gtk_label_set_max_width_chars(GTK_LABEL(view->summary),42);
    gtk_label_set_ellipsize(GTK_LABEL(view->title),PANGO_ELLIPSIZE_END);gtk_label_set_max_width_chars(GTK_LABEL(view->title),26);
    pack(heading, view->summary, FALSE);
    GtkWidget *reload = named(radios_ui_button("Listeleri yenile"), "automation-workspace-reload"); pack(heading, reload, FALSE);
    g_signal_connect(reload, "clicked", G_CALLBACK(libraries_reload), view); pack(view->root, heading, FALSE);
    view->scope_help = named(radios_ui_label("", "small"), "automation-workspace-scope-help");
    gtk_label_set_line_wrap(GTK_LABEL(view->scope_help), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(view->scope_help), 100);
    pack(view->root, view->scope_help, FALSE);
    GtkWidget *pane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL); gtk_paned_set_position(GTK_PANED(pane), 205);
    GtkWidget *detail = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL);
    view->events = tree_new("automation-workspace-events"); view->orders = tree_new("automation-workspace-orders"); view->calendar = tree_new("automation-workspace-calendar");
    GtkWidget *tabs = named(gtk_notebook_new(), "automation-workspace-tabs");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(tabs), TRUE);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), scroll(view->events), tab_label("Yayın sırası", "Eventler ve yayın sırası"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), scroll(view->orders), tab_label("AutoDJ sırası", "Çalışan AutoDJ sırası"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), scroll(view->calendar), tab_label("Şema", "Atanan otomasyon şemaları ve haftalık eventler"));
    gtk_widget_set_size_request(tabs, -1, 140);
    view->schema_settings = named(gtk_expander_new("Otomasyon şeması"), "automation-workspace-schema-settings");
    GtkWidget *schema_body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, NULL);
    gtk_container_add(GTK_CONTAINER(view->schema_settings), schema_body);
    GtkWidget *schema_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
    pack(schema_row, radios_ui_label("Şema", NULL), FALSE); view->schema = named(gtk_combo_box_text_new(), "automation-workspace-schema");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->schema), "", "Kaydedilmiş şema seç"); gtk_combo_box_set_active(GTK_COMBO_BOX(view->schema), 0); pack(schema_row, view->schema, TRUE);
    view->assign = named(radios_ui_button("Şema ata"), "automation-workspace-assign"); view->remove = named(radios_ui_button("Atamayı kaldır"), "automation-workspace-unassign");
    pack(schema_row, view->assign, FALSE); pack(schema_row, view->remove, FALSE); pack(schema_body, schema_row, FALSE);
    g_signal_connect(view->assign, "clicked", G_CALLBACK(schema_assign), view); g_signal_connect(view->remove, "clicked", G_CALLBACK(schema_remove), view);
    view->schema_name = named(radios_ui_label("Şema atanmadı", "small"), "automation-workspace-schema-name"); pack(schema_body, view->schema_name, FALSE);
    GtkWidget *playlist_box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "scope-playlist-controls");
    pack(playlist_box, radios_ui_label("Playlist ve yayın", "strong"), FALSE);
    view->source_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL); pack(view->source_row, radios_ui_label("Başlangıç oynatıcısı", NULL), FALSE);
    view->source = named(radios_ui_label("—", "small"), "automation-workspace-source"); pack(view->source_row, view->source, TRUE); pack(playlist_box, view->source_row, FALSE);
    gtk_widget_show_all(view->source_row); gtk_widget_set_no_show_all(view->source_row, TRUE);
    GtkWidget *playlist_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL); view->playlist = named(gtk_combo_box_text_new(), "automation-workspace-playlist");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->playlist), "", "Playlist seç"); gtk_combo_box_set_active(GTK_COMBO_BOX(view->playlist), 0); pack(playlist_row, view->playlist, TRUE);
    view->load = named(radios_ui_button("Playlist yükle"), "automation-workspace-load"); pack(playlist_row, view->load, FALSE);
    view->edit = named(radios_ui_button("Listeyi düzenle"), "automation-workspace-edit"); pack(playlist_row, view->edit, FALSE); pack(playlist_box, playlist_row, FALSE);
    g_signal_connect(view->load, "clicked", G_CALLBACK(playlist_load), view); g_signal_connect(view->edit, "clicked", G_CALLBACK(edit_clicked), view);
    GtkWidget *autodj = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "scope-autodj-controls"), "automation-workspace-independent-settings");
    view->autodj_settings = autodj; pack(autodj, radios_ui_label("Kümesizler · AutoDJ", "strong"), FALSE);
    GtkWidget *options = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL); view->mode = named(gtk_combo_box_text_new(), "automation-workspace-autodj-mode");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->mode), "1", "Tek oynatıcıda devam et"); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->mode), "2", "Oynatıcılar arasında geçiş yap"); gtk_combo_box_set_active(GTK_COMBO_BOX(view->mode), 0);
    gtk_widget_set_tooltip_text(view->mode, "Yarı AutoDJ: playlist tek bağımsız oynatıcıda devam eder. AutoDJ: kümesiz oynatıcılar arasında geçiş yapar.");
    pack(options, radios_ui_label("Çalışma şekli", NULL), FALSE); pack(options, view->mode, TRUE); pack(autodj, options, FALSE);
    view->target_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
    pack(view->target_row, radios_ui_label("Devam oynatıcısı", NULL), FALSE);
    view->target = named(gtk_combo_box_text_new(), "automation-workspace-autodj-target"); pack(view->target_row, view->target, TRUE); pack(autodj, view->target_row, FALSE);
    gtk_widget_set_tooltip_text(view->target, "Sonraki parçanın hangi bağımsız oynatıcıdan devam edeceğini seçin. Geçiş türü (SYNC beatmatch dahil) ve olasılığı Rotasyon düzenleyici › “AutoDJ: geçiş ve kanallar” sekmesinden düzenlenir.");
    g_signal_connect(view->mode, "changed", G_CALLBACK(options_changed), view); g_signal_connect(view->target, "changed", G_CALLBACK(options_changed), view);
    GtkWidget *controls = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
    view->play = named(radios_ui_button("Play"), "automation-workspace-play"); view->play_schema = named(radios_ui_button("Şemayı çalıştır"), "automation-workspace-play-schema");
    view->pause = named(radios_ui_button("Duraklat"), "automation-workspace-pause"); view->stop = named(radios_ui_button("Durdur"), "automation-workspace-stop");
    GtkWidget *buttons[] = {view->play, view->pause, view->stop};
    for (guint i = 0; i < G_N_ELEMENTS(buttons); ++i) { pack(controls, buttons[i], FALSE); g_signal_connect(buttons[i], "clicked", G_CALLBACK(transport), view); }
    pack(playlist_box, controls, FALSE); pack(detail, playlist_box, FALSE);
    pack(detail, autodj, FALSE); pack(detail, tabs, TRUE);
    pack(schema_body, view->play_schema, FALSE); g_signal_connect(view->play_schema, "clicked", G_CALLBACK(transport), view);
    pack(detail, view->schema_settings, FALSE);
    gtk_widget_show_all(autodj); gtk_widget_set_no_show_all(autodj, TRUE);
    gtk_widget_set_no_show_all(view->target_row, TRUE);
    gtk_widget_show_all(view->schema_settings); gtk_widget_set_no_show_all(view->schema_settings, TRUE);
    view->status = named(radios_ui_label("", "application-status"), "automation-workspace-status"); gtk_label_set_line_wrap(GTK_LABEL(view->status), TRUE); pack(detail, view->status, FALSE);
    view->selector = named(radios_player_tree_ui_new(ctx, FALSE, FALSE, selection_changed, view), "automation-workspace-selector");
    GtkWidget *detail_scroll = named(gtk_scrolled_window_new(NULL, NULL), "automation-workspace-detail-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(detail_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(detail_scroll), FALSE);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(detail_scroll), FALSE);
    gtk_widget_set_vexpand(detail_scroll, TRUE); gtk_container_add(GTK_CONTAINER(detail_scroll), detail);
    gtk_paned_pack1(GTK_PANED(pane), view->selector, FALSE, FALSE); gtk_paned_pack2(GTK_PANED(pane), detail_scroll, TRUE, FALSE); pack(view->root, pane, TRUE);
    compact_combo(view->schema);compact_combo(view->playlist);compact_combo(view->mode);compact_combo(view->target);
    gtk_label_set_ellipsize(GTK_LABEL(view->schema_name),PANGO_ELLIPSIZE_END);gtk_label_set_max_width_chars(GTK_LABEL(view->schema_name),60);
    view->timer = g_timeout_add(500, tick, view); view_refresh(view); return view->root;
}
gboolean radios_automation_workspace_ui_select(GtkWidget *widget, const RadioSAutomationScope *scope)
{
    Workspace *view = view_get(widget); if (!view || view->disposed || !scope) return FALSE;
    RadioSAutomationScope requested = *scope;
    gboolean previous_silent = view->silent;
    g_object_ref(widget); view->silent = TRUE;
    radios_player_tree_ui_refresh(view->selector);
    gboolean selected = !view->disposed && radios_player_tree_ui_select(view->selector, &requested);
    if (!view->disposed) view->silent = previous_silent;
    g_object_unref(widget); return selected;
}
void radios_automation_workspace_ui_refresh(GtkWidget *widget)
{ Workspace *view = view_get(widget); if (view && !view->disposed) { radios_player_tree_ui_refresh(view->selector); view_refresh(view); } }
void radios_automation_workspace_ui_set_edit_hook(GtkWidget *widget, RadioSAutomationWorkspaceSelected edit, gpointer user)
{ Workspace *view = view_get(widget); if (view) { view->edit_hook = edit; view->edit_user = user; view_refresh(view); } }
