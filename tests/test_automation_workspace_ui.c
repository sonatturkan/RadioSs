#include "radios/automation_workspace_ui.h"
#include "radios/player_scope_execution.h"
#include "radios/player_tree_ui.h"
#include "radios/automation_scope_ui.h"
#include "radios/programme_model.h"

static RadioSPlayerTopology model;
static guint token, execution_token, writes, plays, loads, actions, selections, edits;
static gboolean connected;
static RadioSAutomationScope dispatched;
static RadioSAutoDJMode dispatched_mode;
static RadioSPlayoutAction last_action;
static RadioSPlayerScopeExecutionState execution_state;
static guint execution_source;
static gboolean execution_loading, execution_preparing;
static gboolean schema_fetch_accept;
static RadioSDataCallback schema_done;
static GtkWidget *schema_owner;
static gpointer schema_user;
static GDestroyNotify schema_destroy;
static RadioSPlayerScopeExecutionDone pending_done;
static gpointer pending_user;
static GDestroyNotify pending_destroy;
static RadioSViewContext *pending_ctx;

gboolean __wrap_radios_player_workspace_ready(const RadioSPlayerWorkspace *w)
{ g_assert_true(w == (RadioSPlayerWorkspace *)&token); return TRUE; }
const RadioSPlayerTopology *__wrap_radios_player_workspace_topology(const RadioSPlayerWorkspace *w)
{ g_assert_true(w == (RadioSPlayerWorkspace *)&token); return &model; }
gboolean __wrap_radios_player_workspace_replace(RadioSPlayerWorkspace *w, const RadioSPlayerTopology *next, GError **error)
{ (void)w; if (!radios_player_topology_validate(next, error)) return FALSE; model = *next; ++writes; return TRUE; }
void __wrap_radios_player_workspace_reload(RadioSPlayerWorkspace *w) { (void)w; }
gboolean __wrap_radios_player_workspace_assign(RadioSPlayerWorkspace *w, const RadioSAutomationScope *scope, const gchar *id, GError **error)
{ (void)w; return scope->kind == RADIOS_AUTOMATION_SCOPE_PLAYER ? radios_player_topology_set_automation(&model, scope->deck, id, error) : radios_player_topology_set_cluster_automation(&model, scope->cluster_id, id, error); }
static JsonObject *programme(const gchar *name)
{
    JsonObject *o=json_object_new(), *event=json_object_new(); JsonArray *events=json_array_new();
    json_object_set_string_member(o,"title",name); json_object_set_string_member(o,"date","2026-10-12"); json_object_set_string_member(o,"note","");
    json_object_set_string_member(event,"id","40000000-0000-4000-8000-000000000001"); json_object_set_string_member(event,"title",name);
    json_object_set_string_member(event,"kind","music"); json_object_set_int_member(event,"duration_ms",5000);
    json_array_add_object_element(events,event); json_object_set_array_member(o,"events",events); return o;
}
gboolean __wrap_radios_player_workspace_get_programme(RadioSPlayerWorkspace *w,const RadioSAutomationScope *scope,JsonObject **out,GError **error)
{ (void)w; (void)scope; (void)error; *out=programme("Stored event"); return TRUE; }
JsonNode *__wrap_radios_player_workspace_projection(RadioSPlayerWorkspace *w,const RadioSAutomationScope *scope,GError **error)
{
    (void)w; RadioSAutomationBinding binding; if(!radios_automation_scope_resolve(&model,scope,&binding,error))return NULL;
    JsonObject *o=json_object_new(); JsonArray *players=json_array_new(); json_object_set_array_member(o,"players",players);
    json_object_set_string_member(o,"name",scope->kind==RADIOS_AUTOMATION_SCOPE_PLAYER?model.players[scope->deck].name:scope->kind==RADIOS_AUTOMATION_SCOPE_CLUSTER?"Müzik":"Kümesizler");
    json_object_set_string_member(o,"automation_id",binding.automation_id);
    if(scope->kind!=RADIOS_AUTOMATION_SCOPE_PLAYER)json_object_set_object_member(o,"programme",programme(scope->kind==RADIOS_AUTOMATION_SCOPE_CLUSTER?"Cluster event":"Aggregate event"));
    guint count=scope->kind==RADIOS_AUTOMATION_SCOPE_PLAYER?1:binding.count;
    for(guint i=0;i<count;++i){ guint deck=binding.decks[i]; JsonObject *p=json_object_new();json_object_set_int_member(p,"player",deck+1);json_object_set_string_member(p,"name",model.players[deck].name);json_object_set_string_member(p,"automation_id","");json_object_set_object_member(p,"programme",programme(deck<2?"Cluster player event":"Independent player event"));json_array_add_object_element(players,p); }
    JsonNode *n=json_node_new(JSON_NODE_OBJECT); json_node_take_object(n,o);return n;
}
gboolean __wrap_radios_data_connected(RadioSViewContext *ctx){(void)ctx;return connected;}
RadioSDataClient *__wrap_radios_data_client_acquire(RadioSViewContext *ctx){(void)ctx;return NULL;}
JsonNode *__wrap_radios_data_body(RadioSViewContext *ctx){(void)ctx;JsonNode *n=json_node_new(JSON_NODE_OBJECT);json_node_take_object(n,json_object_new());return n;}
gboolean __wrap_radios_data_request(RadioSViewContext *ctx,GtkWidget *owner,const gchar *method,const gchar *path,JsonNode *body,RadioSDataCallback done,gpointer user,GDestroyNotify destroy)
{(void)ctx;(void)owner;(void)method;(void)path;(void)body;(void)done;if(destroy)destroy(user);return FALSE;}
gboolean __wrap_radios_automation_scope_fetch(RadioSViewContext *ctx,GtkWidget *owner,const gchar *id,gint64 revision,RadioSDataCallback done,gpointer user,GDestroyNotify destroy)
{
    (void)ctx;(void)revision;g_assert_true(g_uuid_string_is_valid(id));
    if(!schema_fetch_accept){if(destroy)destroy(user);return FALSE;}
    g_assert_null(schema_done);schema_done=done;schema_owner=owner;schema_user=user;schema_destroy=destroy;return TRUE;
}
RadioSPlayerScopeExecution *__wrap_radios_player_scope_execution_get(RadioSViewContext *ctx){(void)ctx;return (RadioSPlayerScopeExecution *)&execution_token;}
gboolean __wrap_radios_player_scope_execution_snapshot(RadioSPlayerScopeExecution *e,const RadioSAutomationScope *s,RadioSPlayerScopeExecutionSnapshot *out)
{
    (void)e; RadioSAutomationBinding binding;
    if (!radios_automation_scope_resolve(&model,s,&binding,NULL)) return FALSE;
    gboolean selected=s->kind==RADIOS_AUTOMATION_SCOPE_PLAYER?s->deck==execution_source:FALSE;
    if(s->kind!=RADIOS_AUTOMATION_SCOPE_PLAYER)for(guint i=0;i<binding.count;++i)selected|=binding.decks[i]==execution_source;
    *out=(RadioSPlayerScopeExecutionSnapshot){.state=selected?execution_state:RADIOS_SCOPE_EXECUTION_IDLE,.loading_playlist=selected&&execution_loading,.preparing=selected&&execution_preparing};return TRUE;
}
static gboolean dispatch(const RadioSAutomationScope *scope,RadioSPlayerScopeExecutionDone done,gpointer user,GDestroyNotify destroy)
{g_assert_null(pending_done);dispatched=*scope;pending_done=done;pending_user=user;pending_destroy=destroy;return TRUE;}
gboolean __wrap_radios_player_scope_execution_play_programme(RadioSPlayerScopeExecution *e,const RadioSAutomationScope *s,JsonObject *programme,RadioSAutoDJMode mode,RadioSPlayerScopeExecutionDone done,gpointer user,GDestroyNotify destroy,GError **error)
{(void)e;(void)programme;(void)error;++plays;dispatched_mode=mode;return dispatch(s,done,user,destroy);}
gboolean __wrap_radios_player_scope_execution_play_schema(RadioSPlayerScopeExecution *e,const RadioSAutomationScope *s,RadioSAutoDJMode mode,RadioSPlayerScopeExecutionDone done,gpointer user,GDestroyNotify destroy,GError **error)
{(void)e;(void)error;++plays;dispatched_mode=mode;return dispatch(s,done,user,destroy);}
gboolean __wrap_radios_player_scope_execution_load_playlist(RadioSPlayerScopeExecution *e,const RadioSAutomationScope *s,const gchar *id,RadioSPlayerScopeExecutionDone done,gpointer user,GDestroyNotify destroy,GError **error)
{(void)e;(void)error;g_assert_true(g_uuid_string_is_valid(id));++loads;return dispatch(s,done,user,destroy);}
gboolean __wrap_radios_player_scope_execution_action(RadioSPlayerScopeExecution *e,const RadioSAutomationScope *s,RadioSPlayoutAction action,GError **error)
{(void)e;(void)error;++actions;last_action=action;dispatched=*s;return TRUE;}
static void complete(void)
{g_assert_nonnull(pending_done);pending_done(pending_ctx,&dispatched,0,NULL,pending_user);if(pending_destroy)pending_destroy(pending_user);pending_done=NULL;pending_destroy=NULL;pending_user=NULL;}
static GtkWidget *find(GtkWidget *root,const gchar *name)
{if(!g_strcmp0(gtk_widget_get_name(root),name))return root;if(!GTK_IS_CONTAINER(root))return NULL;GList *children=gtk_container_get_children(GTK_CONTAINER(root));GtkWidget *found=NULL;for(GList *i=children;i&&!found;i=i->next)found=find(i->data,name);g_list_free(children);return found;}
static void pump(void){while(g_main_context_iteration(NULL,FALSE)){} }
static gboolean contains(GtkTreeModel *tree,GtkTreeIter *parent,const gchar *wanted)
{GtkTreeIter iter;gboolean valid=gtk_tree_model_iter_children(tree,&iter,parent);while(valid){gchar *name=NULL;gtk_tree_model_get(tree,&iter,0,&name,-1);gboolean same=!g_strcmp0(name,wanted);g_free(name);if(same||contains(tree,&iter,wanted))return TRUE;valid=gtk_tree_model_iter_next(tree,&iter);}return FALSE;}
static void selected(const RadioSAutomationScope *scope,gpointer user){(void)scope;(void)user;++selections;}
static void edit(const RadioSAutomationScope *scope,gpointer user){(void)user;g_assert_cmpint(scope->kind,==,RADIOS_AUTOMATION_SCOPE_PLAYER);++edits;}
typedef struct{GtkWidget *window,*panel;RadioSViewContext ctx;gchar cluster[37];} Harness;
static Harness *open_panel(void)
{
    Harness *h=g_new0(Harness,1);radios_player_topology_default(&model,4);g_assert_true(radios_player_topology_add_cluster(&model,"Müzik",h->cluster,NULL));
    g_assert_true(radios_player_topology_set_cluster(&model,0,h->cluster,NULL));g_assert_true(radios_player_topology_set_cluster(&model,1,h->cluster,NULL));
    g_strlcpy(model.players[0].name,"A",sizeof model.players[0].name);g_strlcpy(model.players[1].name,"B",sizeof model.players[1].name);
    g_strlcpy(model.players[2].name,"C",sizeof model.players[2].name);g_strlcpy(model.players[3].name,"D",sizeof model.players[3].name);
    connected=FALSE;writes=plays=loads=actions=selections=edits=0;execution_state=RADIOS_SCOPE_EXECUTION_IDLE;execution_source=0;execution_loading=execution_preparing=FALSE;schema_fetch_accept=FALSE;g_assert_null(schema_done);g_assert_null(pending_done);
    h->window=gtk_window_new(GTK_WINDOW_TOPLEVEL);g_object_ref_sink(h->window);h->ctx=(RadioSViewContext){.parent=GTK_WINDOW(h->window),.players=(RadioSPlayerWorkspace *)&token};pending_ctx=&h->ctx;
    h->panel=radios_automation_workspace_ui_new(&h->ctx,selected,NULL);radios_automation_workspace_ui_set_edit_hook(h->panel,edit,NULL);gtk_container_add(GTK_CONTAINER(h->window),h->panel);gtk_widget_show_all(h->window);pump();return h;
}
static void close_panel(Harness *h){gtk_widget_destroy(h->window);g_object_unref(h->window);pump();g_free(h);pending_ctx=NULL;}
static void click(Harness *h,const gchar *name){GtkWidget *b=find(h->panel,name);g_assert_true(GTK_IS_BUTTON(b));g_assert_true(gtk_widget_get_sensitive(b));gtk_button_clicked(GTK_BUTTON(b));pump();}
static void sensitivity_changed(GObject *object,GParamSpec *property,gpointer user)
{(void)object;(void)property;++*(guint *)user;}
static void assert_sensitivity_stays_quiet(Harness *h)
{
    const gchar *names[]={"automation-workspace-play","automation-workspace-pause","automation-workspace-stop",
        "automation-workspace-play-schema","automation-workspace-load","automation-workspace-autodj-mode",
        "automation-workspace-autodj-target","automation-workspace-assign","automation-workspace-unassign",
        "automation-workspace-schema"};
    GtkWidget *controls[G_N_ELEMENTS(names)];guint changes[G_N_ELEMENTS(names)]={0};gulong handlers[G_N_ELEMENTS(names)];
    for(guint i=0;i<G_N_ELEMENTS(names);++i){
        controls[i]=find(h->panel,names[i]);g_assert_nonnull(controls[i]);
        handlers[i]=g_signal_connect(controls[i],"notify::sensitive",G_CALLBACK(sensitivity_changed),&changes[i]);
    }
    const gchar *button_names[]={"automation-workspace-play","automation-workspace-unassign"};
    GtkWidget *buttons[G_N_ELEMENTS(button_names)],*children[G_N_ELEMENTS(button_names)];
    guint label_changes[G_N_ELEMENTS(button_names)]={0};gulong label_handlers[G_N_ELEMENTS(button_names)];
    for(guint i=0;i<G_N_ELEMENTS(button_names);++i){
        buttons[i]=find(h->panel,button_names[i]);g_assert_true(GTK_IS_BUTTON(buttons[i]));
        children[i]=gtk_bin_get_child(GTK_BIN(buttons[i]));g_assert_nonnull(children[i]);g_object_ref(children[i]);
        label_handlers[i]=g_signal_connect(buttons[i],"notify::label",G_CALLBACK(sensitivity_changed),&label_changes[i]);
    }
    guint before=writes;gint64 end=g_get_monotonic_time()+1100*G_TIME_SPAN_MILLISECOND;
    /* Exercise at least two real 500 ms workspace polls after settling. */
    while(g_get_monotonic_time()<end){pump();g_usleep(1000);}pump();
    for(guint i=0;i<G_N_ELEMENTS(names);++i){
        if(changes[i])g_test_message("Unchanged control emitted sensitivity notifications: %s",names[i]);
        g_assert_cmpuint(changes[i],==,0);g_signal_handler_disconnect(controls[i],handlers[i]);
    }
    for(guint i=0;i<G_N_ELEMENTS(button_names);++i){
        g_assert_true(gtk_bin_get_child(GTK_BIN(buttons[i]))==children[i]);
        g_assert_cmpuint(label_changes[i],==,0);g_signal_handler_disconnect(buttons[i],label_handlers[i]);g_object_unref(children[i]);
    }
    g_assert_cmpuint(writes,==,before);
}
static void test_scope_views_and_silent_mirror(void)
{
    Harness *h=open_panel();RadioSAutomationScope group={.kind=RADIOS_AUTOMATION_SCOPE_CLUSTER};g_strlcpy(group.cluster_id,h->cluster,sizeof group.cluster_id);
    guint before=selections;g_assert_true(radios_automation_workspace_ui_select(h->panel,&group));g_assert_cmpuint(selections,==,before);
    g_assert_false(gtk_widget_get_visible(find(h->panel,"automation-workspace-independent-settings")));
    GtkTreeModel *events=gtk_tree_view_get_model(GTK_TREE_VIEW(find(h->panel,"automation-workspace-events")));
    g_assert_true(contains(events,NULL,"Cluster event"));g_assert_true(contains(events,NULL,"Cluster player event"));g_assert_false(contains(events,NULL,"Independent player event"));
    g_assert_false(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-edit")));
    RadioSAutomationScope aggregate={.kind=RADIOS_AUTOMATION_SCOPE_UNCLUSTERED};g_assert_true(radios_automation_workspace_ui_select(h->panel,&aggregate));
    g_assert_true(gtk_widget_get_visible(find(h->panel,"automation-workspace-independent-settings")));
    g_assert_false(gtk_widget_get_visible(find(h->panel,"automation-workspace-schema-settings")));
    g_assert_true(contains(events,NULL,"Aggregate event"));g_assert_true(contains(events,NULL,"Independent player event"));g_assert_false(contains(events,NULL,"Cluster player event"));
    g_assert_false(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-assign")));g_assert_true(GTK_IS_LABEL(find(h->panel,"automation-workspace-source")));
    RadioSAutomationScope player={.kind=RADIOS_AUTOMATION_SCOPE_PLAYER,.deck=0};g_assert_true(radios_automation_workspace_ui_select(h->panel,&player));
    g_assert_true(contains(events,NULL,"Cluster player event"));g_assert_false(contains(events,NULL,"Cluster event"));g_assert_false(contains(events,NULL,"Independent player event"));click(h,"automation-workspace-edit");g_assert_cmpuint(edits,==,1);
    g_assert_cmpuint(writes,==,0);close_panel(h);
}
static void test_target_and_playlist_are_separate(void)
{
    Harness *h=open_panel();RadioSAutomationScope aggregate={.kind=RADIOS_AUTOMATION_SCOPE_UNCLUSTERED};g_assert_true(radios_automation_workspace_ui_select(h->panel,&aggregate));
    GtkWidget *mode=find(h->panel,"automation-workspace-autodj-mode"),*target=find(h->panel,"automation-workspace-autodj-target");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(mode),"2"));g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(target),"3"));
    g_assert_cmpint(model.players[2].autodj_mode,==,RADIOS_AUTODJ_TRANSITIONS);g_assert_cmpint(model.players[2].autodj_target,==,3);g_assert_cmpuint(loads,==,0);g_assert_cmpuint(plays,==,0);
    connected=TRUE;radios_automation_workspace_ui_refresh(h->panel);GtkWidget *playlist=find(h->panel,"automation-workspace-playlist");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(playlist),"50000000-0000-4000-8000-000000000001","Fixture playlist");g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(playlist),"50000000-0000-4000-8000-000000000001"));
    guint before=writes;click(h,"automation-workspace-load");g_assert_cmpint(dispatched.kind,==,RADIOS_AUTOMATION_SCOPE_UNCLUSTERED);g_assert_cmpuint(loads,==,1);g_assert_cmpuint(plays,==,0);g_assert_cmpuint(writes,==,before);g_assert_cmpint(model.players[2].autodj_target,==,3);complete();close_panel(h);
}
static void test_play_pause_and_late_callback_lifetime(void)
{
    Harness *h=open_panel();RadioSAutomationScope group={.kind=RADIOS_AUTOMATION_SCOPE_CLUSTER};g_strlcpy(group.cluster_id,h->cluster,sizeof group.cluster_id);g_assert_true(radios_automation_workspace_ui_select(h->panel,&group));
    guint before=writes;
    click(h,"automation-workspace-play");g_assert_cmpuint(plays,==,1);g_assert_cmpint(dispatched.kind,==,RADIOS_AUTOMATION_SCOPE_CLUSTER);g_assert_cmpuint(writes,==,before);complete();
    execution_state=RADIOS_SCOPE_EXECUTION_PAUSED;radios_automation_workspace_ui_refresh(h->panel);click(h,"automation-workspace-play");g_assert_cmpuint(plays,==,1);g_assert_cmpint(last_action,==,RADIOS_PLAYOUT_RESUME);
    execution_state=RADIOS_SCOPE_EXECUTION_IDLE;radios_automation_workspace_ui_refresh(h->panel);click(h,"automation-workspace-play");g_assert_cmpuint(plays,==,2);close_panel(h);complete();
}
static void test_active_definition_guards_are_scoped(void)
{
    Harness *h=open_panel(); RadioSAutomationScope group={.kind=RADIOS_AUTOMATION_SCOPE_CLUSTER};
    g_strlcpy(group.cluster_id,h->cluster,sizeof group.cluster_id);
    g_assert_true(radios_player_topology_set_autodj(&model,0,RADIOS_AUTODJ_TRANSITIONS,1,NULL));
    connected=TRUE;g_assert_true(radios_automation_workspace_ui_select(h->panel,&group));
    GtkWidget *mode=find(h->panel,"automation-workspace-autodj-mode"), *target=find(h->panel,"automation-workspace-autodj-target");
    const RadioSPlayerScopeExecutionState states[]={RADIOS_SCOPE_EXECUTION_PREPARING,RADIOS_SCOPE_EXECUTION_WAITING,RADIOS_SCOPE_EXECUTION_PLAYING,RADIOS_SCOPE_EXECUTION_PAUSED};
    for(guint i=0;i<G_N_ELEMENTS(states);++i){
        execution_state=states[i];radios_automation_workspace_ui_refresh(h->panel);
        g_assert_false(gtk_widget_get_sensitive(mode));g_assert_false(gtk_widget_get_sensitive(target));
        g_assert_false(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-schema")));
        g_assert_false(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-assign")));
        g_assert_true(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-load")));
        g_assert_true(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-stop")));
        g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(find(h->panel,"automation-workspace-play"))),==,
            states[i]==RADIOS_SCOPE_EXECUTION_PAUSED?"Devam et":"Play");
    }
    assert_sensitivity_stays_quiet(h);
    execution_state=RADIOS_SCOPE_EXECUTION_PLAYING;radios_automation_workspace_ui_refresh(h->panel);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(find(h->panel,"automation-workspace-play"))),==,"Play");
    execution_state=RADIOS_SCOPE_EXECUTION_PAUSED;radios_automation_workspace_ui_refresh(h->panel);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(find(h->panel,"automation-workspace-play"))),==,"Devam et");
    guint before=writes;
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(mode),"1"));
    g_assert_cmpuint(writes,==,before);g_assert_cmpint(model.players[0].autodj_mode,==,RADIOS_AUTODJ_TRANSITIONS);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(mode)),==,"2");
    RadioSAutomationScope unrelated={.kind=RADIOS_AUTOMATION_SCOPE_PLAYER,.deck=2};
    g_assert_true(radios_automation_workspace_ui_select(h->panel,&unrelated));
    g_assert_true(gtk_widget_get_sensitive(mode));g_assert_true(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-schema")));
    g_assert_true(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-assign")));
    g_assert_true(radios_automation_workspace_ui_select(h->panel,&group));guint mode_changes=0;
    gulong handler=g_signal_connect(mode,"notify::sensitive",G_CALLBACK(sensitivity_changed),&mode_changes);
    execution_state=RADIOS_SCOPE_EXECUTION_IDLE;
    radios_automation_workspace_ui_refresh(h->panel);g_assert_true(gtk_widget_get_sensitive(mode));g_assert_true(gtk_widget_get_sensitive(target));
    g_assert_cmpuint(mode_changes,==,1);radios_automation_workspace_ui_refresh(h->panel);g_assert_cmpuint(mode_changes,==,1);
    g_signal_handler_disconnect(mode,handler);
    close_panel(h);
}
static void test_async_assignment_cannot_change_started_scope(void)
{
    Harness *h=open_panel();RadioSAutomationScope player={.kind=RADIOS_AUTOMATION_SCOPE_PLAYER,.deck=0};
    connected=TRUE;g_assert_true(radios_automation_workspace_ui_select(h->panel,&player));
    GtkWidget *schema=find(h->panel,"automation-workspace-schema");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(schema),"60000000-0000-4000-8000-000000000001","Fixture weekly schema");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(schema),"60000000-0000-4000-8000-000000000001"));
    schema_fetch_accept=TRUE;click(h,"automation-workspace-assign");g_assert_nonnull(schema_done);
    execution_state=RADIOS_SCOPE_EXECUTION_PREPARING;
    JsonNode *record=json_node_new(JSON_NODE_OBJECT);json_node_take_object(record,json_object_new());
    schema_done(&h->ctx,schema_owner,record,NULL,schema_user);json_node_free(record);
    if(schema_destroy)schema_destroy(schema_user);
    schema_done=NULL;schema_owner=NULL;schema_user=NULL;schema_destroy=NULL;
    g_assert_cmpstr(model.players[0].automation_id,==,"");
    g_assert_nonnull(g_strstr_len(gtk_label_get_text(GTK_LABEL(find(h->panel,"automation-workspace-status"))),-1,"önce"));
    close_panel(h);
}
static void test_new_scope_mirror_refreshes_without_feedback(void)
{
    Harness *h=open_panel();gchar cluster[37];
    g_assert_true(radios_player_topology_add_cluster(&model,"Yeni küme",cluster,NULL));
    RadioSAutomationScope scope={.kind=RADIOS_AUTOMATION_SCOPE_CLUSTER};g_strlcpy(scope.cluster_id,cluster,sizeof scope.cluster_id);
    guint before=selections;g_assert_true(radios_automation_workspace_ui_select(h->panel,&scope));
    GtkWidget *selector=find(h->panel,"automation-workspace-selector");RadioSAutomationScope actual={0};gboolean general=TRUE;
    g_assert_true(radios_player_tree_ui_selection(selector,&actual,&general));g_assert_false(general);
    g_assert_cmpint(actual.kind,==,RADIOS_AUTOMATION_SCOPE_CLUSTER);g_assert_cmpstr(actual.cluster_id,==,cluster);g_assert_cmpuint(selections,==,before);
    guint deck=G_MAXUINT;g_assert_true(radios_player_topology_add_player(&model,cluster,&deck,NULL));
    scope=(RadioSAutomationScope){.kind=RADIOS_AUTOMATION_SCOPE_PLAYER,.deck=deck};
    g_assert_true(radios_automation_workspace_ui_select(h->panel,&scope));
    g_assert_true(radios_player_tree_ui_selection(selector,&actual,&general));
    g_assert_cmpint(actual.kind,==,RADIOS_AUTOMATION_SCOPE_PLAYER);g_assert_cmpuint(actual.deck,==,deck);g_assert_cmpuint(selections,==,before);
    radios_automation_workspace_ui_refresh(h->panel);g_assert_cmpuint(selections,==,before);
    g_assert_true(radios_player_tree_ui_selection(selector,&actual,&general));g_assert_cmpuint(actual.deck,==,deck);
    close_panel(h);
}
static void test_playlist_only_load_keeps_definition_controls_editable(void)
{
    Harness *h=open_panel();RadioSAutomationScope player={.kind=RADIOS_AUTOMATION_SCOPE_PLAYER,.deck=2};
    connected=TRUE;execution_source=2;g_assert_true(radios_player_topology_set_autodj(&model,2,RADIOS_AUTODJ_TRANSITIONS,3,NULL));
    execution_state=RADIOS_SCOPE_EXECUTION_PREPARING;execution_loading=TRUE;
    g_assert_true(radios_automation_workspace_ui_select(h->panel,&player));
    GtkWidget *mode=find(h->panel,"automation-workspace-autodj-mode"),*target=find(h->panel,"automation-workspace-autodj-target");
    g_assert_true(gtk_widget_get_sensitive(mode));g_assert_true(gtk_widget_get_sensitive(target));
    g_assert_true(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-schema")));
    g_assert_true(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-assign")));
    guint before=writes;g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(mode),"1"));
    g_assert_cmpuint(writes,==,before+1);g_assert_cmpint(model.players[2].autodj_mode,==,RADIOS_AUTODJ_SEMI);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(mode),"2"));g_assert_true(gtk_widget_get_sensitive(target));
    execution_preparing=TRUE;radios_automation_workspace_ui_refresh(h->panel);
    g_assert_false(gtk_widget_get_sensitive(mode));g_assert_false(gtk_widget_get_sensitive(target));
    g_assert_false(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-schema")));
    close_panel(h);
}
static void test_empty_scope_and_single_player_controls(void)
{
    Harness *h=open_panel();gchar empty[37];
    g_assert_true(radios_player_topology_add_cluster(&model,"Henüz boş",empty,NULL));
    RadioSAutomationScope scope={.kind=RADIOS_AUTOMATION_SCOPE_CLUSTER};g_strlcpy(scope.cluster_id,empty,sizeof scope.cluster_id);
    connected=TRUE;g_assert_true(radios_automation_workspace_ui_select(h->panel,&scope));
    const gchar *disabled[]={"automation-workspace-play","automation-workspace-play-schema","automation-workspace-load", "automation-workspace-autodj-mode","automation-workspace-autodj-target"};
    for(guint i=0;i<G_N_ELEMENTS(disabled);++i)g_assert_false(gtk_widget_get_sensitive(find(h->panel,disabled[i])));
    assert_sensitivity_stays_quiet(h);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(h->panel,"automation-workspace-scope-help"))),"oynatıcı ekleyin"));
    g_assert_true(radios_player_topology_set_cluster(&model,0,empty,NULL));
    g_assert_true(radios_player_topology_set_autodj(&model,0,RADIOS_AUTODJ_TRANSITIONS,-1,NULL));
    radios_automation_workspace_ui_refresh(h->panel);
    g_assert_true(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-play")));
    g_assert_false(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-autodj-target")));
    g_assert_true(radios_player_topology_set_cluster(&model,3,empty,NULL));
    g_assert_true(radios_player_topology_set_autodj(&model,2,RADIOS_AUTODJ_TRANSITIONS,-1,NULL));
    RadioSAutomationScope independent={.kind=RADIOS_AUTOMATION_SCOPE_PLAYER,.deck=2};
    g_assert_true(radios_automation_workspace_ui_select(h->panel,&independent));
    g_assert_false(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-play")));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(h->panel,"automation-workspace-scope-help"))),"ikinci bir kümesiz"));
    assert_sensitivity_stays_quiet(h);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(h->panel,"automation-workspace-autodj-mode")),"1"));
    g_assert_true(gtk_widget_get_sensitive(find(h->panel,"automation-workspace-play")));
    close_panel(h);
}
/* PDF §08 "kümeli çalışma": the cluster node starts its members together
 * even when one member's card was left on Yarı AutoDJ before it was moved
 * into the cluster; the member node itself keeps its own definition. */
static void test_cluster_play_uses_group_transitions(void)
{
    Harness *h=open_panel();RadioSAutomationScope group={.kind=RADIOS_AUTOMATION_SCOPE_CLUSTER};g_strlcpy(group.cluster_id,h->cluster,sizeof group.cluster_id);
    g_assert_true(radios_player_topology_set_autodj(&model,0,RADIOS_AUTODJ_SEMI,-1,NULL));
    g_assert_true(radios_player_topology_set_cluster_automation(&model,h->cluster,"60000000-0000-4000-8000-000000000001",NULL));
    connected=TRUE;g_assert_true(radios_automation_workspace_ui_select(h->panel,&group));
    guint before=writes;click(h,"automation-workspace-play");
    g_assert_cmpuint(plays,==,1);g_assert_cmpint(dispatched.kind,==,RADIOS_AUTOMATION_SCOPE_CLUSTER);g_assert_cmpint(dispatched_mode,==,RADIOS_AUTODJ_TRANSITIONS);
    g_assert_cmpuint(writes,==,before);g_assert_cmpint(model.players[0].autodj_mode,==,RADIOS_AUTODJ_SEMI);complete();
    click(h,"automation-workspace-play-schema");
    g_assert_cmpuint(plays,==,2);g_assert_cmpint(dispatched.kind,==,RADIOS_AUTOMATION_SCOPE_CLUSTER);g_assert_cmpint(dispatched_mode,==,RADIOS_AUTODJ_TRANSITIONS);complete();
    RadioSAutomationScope member={.kind=RADIOS_AUTOMATION_SCOPE_PLAYER,.deck=0};g_assert_true(radios_automation_workspace_ui_select(h->panel,&member));
    click(h,"automation-workspace-play");g_assert_cmpuint(plays,==,3);g_assert_cmpint(dispatched.kind,==,RADIOS_AUTOMATION_SCOPE_PLAYER);g_assert_cmpint(dispatched_mode,==,RADIOS_AUTODJ_SEMI);complete();
    /* A cluster left with one member plays alone. */
    g_assert_true(radios_player_topology_set_cluster(&model,1,"",NULL));g_assert_true(radios_automation_workspace_ui_select(h->panel,&group));
    click(h,"automation-workspace-play");g_assert_cmpuint(plays,==,4);g_assert_cmpint(dispatched_mode,==,RADIOS_AUTODJ_SEMI);complete();
    g_assert_cmpuint(writes,==,before);close_panel(h);
}
int main(int argc,char **argv)
{g_test_init(&argc,&argv,NULL);if(!gtk_init_check(&argc,&argv))return 77;g_test_add_func("/automation-workspace/scoped-events-silent-selection",test_scope_views_and_silent_mirror);g_test_add_func("/automation-workspace/target-playlist-independent",test_target_and_playlist_are_separate);g_test_add_func("/automation-workspace/transport-late-callback-lifetime",test_play_pause_and_late_callback_lifetime);g_test_add_func("/automation-workspace/active-definition-guards-scoped",test_active_definition_guards_are_scoped);g_test_add_func("/automation-workspace/late-assignment-start-guard",test_async_assignment_cannot_change_started_scope);g_test_add_func("/automation-workspace/new-scope-mirror-refresh",test_new_scope_mirror_refreshes_without_feedback);g_test_add_func("/automation-workspace/playlist-load-definition-controls",test_playlist_only_load_keeps_definition_controls_editable);g_test_add_func("/automation-workspace/empty-scope-and-single-target",test_empty_scope_and_single_player_controls);g_test_add_func("/automation-workspace/cluster-play-group-transitions",test_cluster_play_uses_group_transitions);return g_test_run();}
