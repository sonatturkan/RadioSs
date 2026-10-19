/* Reuse the authenticated private-store/window fixture; its existing tests
 * remain linked but this executable registers only the visible-panel case. */
#define main radios_workspace_fixture_main
#include "test_player_workspace_ui.c"
#undef main
#include "radios/automation_workspace_ui.h"
#include "radios/player_tree_ui.h"
#include "radios/media_jobs.h"
#include <glib/gstdio.h>
#include <math.h>

static GtkWidget *gui_find(GtkWidget *root,const gchar *name)
{if(!g_strcmp0(gtk_widget_get_name(root),name))return root;if(!GTK_IS_CONTAINER(root))return NULL;GList *children=gtk_container_get_children(GTK_CONTAINER(root));GtkWidget *found=NULL;for(GList *i=children;i&&!found;i=i->next)found=gui_find(i->data,name);g_list_free(children);return found;}
static void gui_click(GtkWidget *panel,const gchar *name)
{GtkWidget *b=gui_find(panel,name);g_assert_true(GTK_IS_BUTTON(b));g_assert_true(gtk_widget_get_sensitive(b));gtk_button_clicked(GTK_BUTTON(b));pump();}
static gboolean combo_has(GtkWidget *combo,const gchar *id)
{GtkTreeModel *model=gtk_combo_box_get_model(GTK_COMBO_BOX(combo));GtkTreeIter iter;gint column=gtk_combo_box_get_id_column(GTK_COMBO_BOX(combo));gboolean valid=gtk_tree_model_get_iter_first(model,&iter);while(valid){gchar *value=NULL;gtk_tree_model_get(model,&iter,column,&value,-1);gboolean same=!g_strcmp0(value,id);g_free(value);if(same)return TRUE;valid=gtk_tree_model_iter_next(model,&iter);}return FALSE;}
typedef struct{GtkWidget *combo;const gchar *id;} ComboWait;
static gboolean choice_ready(gpointer data){ComboWait *wait=data;return combo_has(wait->combo,wait->id);}
typedef struct{Window *window;RadioSAutomationScope scope;const gchar *id;} AssignmentWait;
static gboolean assignment_ready(gpointer data)
{AssignmentWait *wait=data;RadioSAutomationBinding binding;return radios_automation_scope_resolve(radios_player_workspace_topology(wait->window->ctx->players),&wait->scope,&binding,NULL)&&!g_strcmp0(binding.automation_id,wait->id);}
static void gui_select(GtkWidget *panel,const RadioSAutomationScope *scope)
{radios_automation_workspace_ui_refresh(panel);g_assert_true(radios_automation_workspace_ui_select(panel,scope));pump();}
static void save_ack(Window *window)
{JsonObject *expected=radios_player_topology_to_json(radios_player_workspace_topology(window->ctx->players));JsonNode *saved=wait_saved(expected);json_node_free(saved);json_object_unref(expected);run_for(150);}
typedef struct{Window *window;RadioSAutomationScope scope;} QueueWait;
static gboolean queue_loaded_ui(gpointer data)
{QueueWait *wait=data;JsonObject *queue=NULL;GError *error=NULL;gboolean ok=radios_player_workspace_get_programme(wait->window->ctx->players,&wait->scope,&queue,&error);g_clear_error(&error);JsonArray *events=queue&&json_object_has_member(queue,"events")?json_object_get_array_member(queue,"events"):NULL;ok=ok&&events&&json_array_get_length(events)>0;g_clear_pointer(&queue,json_object_unref);return ok;}
typedef struct{JsonNode *result;GError *error;gboolean done;} ImportFixture;
static void import_fixture_done(RadioSViewContext *ctx,GtkWidget *owner,JsonNode *result,const GError *error,gpointer data)
{(void)ctx;(void)owner;ImportFixture *fixture=data;fixture->result=result?radios_programme_clone(result):NULL;fixture->error=error?g_error_copy(error):NULL;}
static void import_fixture_destroy(gpointer data){((ImportFixture *)data)->done=TRUE;}
static gboolean import_fixture_finished(gpointer data){return ((ImportFixture *)data)->done;}
static void u16(guchar *p,guint value){p[0]=(guchar)value;p[1]=(guchar)(value>>8);}
static void u32(guchar *p,guint value){for(guint i=0;i<4;++i)p[i]=(guchar)(value>>(8*i));}
static JsonNode *playlist_fixture(Window *window)
{
    const guint frames=48000*3,bytes=frames*4;guchar *wav=g_malloc0(44+bytes);memcpy(wav,"RIFF",4);u32(wav+4,36+bytes);memcpy(wav+8,"WAVEfmt ",8);u32(wav+16,16);u16(wav+20,1);u16(wav+22,2);u32(wav+24,48000);u32(wav+28,48000*4);u16(wav+32,4);u16(wav+34,16);memcpy(wav+36,"data",4);u32(wav+40,bytes);
    for(guint i=0;i<frames;++i){gint16 sample=(gint16)lrint(sin(2*G_PI*440*i/48000)*8000);u16(wav+44+i*4,(guint16)sample);u16(wav+46+i*4,(guint16)sample);}
    gchar *path=g_build_filename(config.project_root,"scope-ui-fixture.wav",NULL);g_assert_true(g_file_set_contents(path,(const gchar *)wav,44+bytes,NULL));g_free(wav);
    ImportFixture fixture={0};GCancellable *cancel=radios_media_import_async(window->ctx,GTK_WIDGET(window->ctx->parent),path,"music","UI gerçek parça",import_fixture_done,&fixture,import_fixture_destroy);g_assert_nonnull(cancel);wait_until(import_fixture_finished,&fixture,"real private WAV import and SHA verification");g_assert_no_error(fixture.error);g_assert_nonnull(fixture.result);g_object_unref(cancel);g_assert_cmpint(g_unlink(path),==,0);g_free(path);
    JsonObject *media=json_object_get_object_member(json_node_get_object(fixture.result),"media");JsonNode *body=body_new();json_object_set_string_member(json_node_get_object(body),"media_id",radios_json_string(media,"_id",""));GError *error=NULL;JsonNode *detail=request("/v1/workflows/desktop-media/get",body,&error);g_assert_no_error(error);g_assert_nonnull(detail);json_node_free(body);json_node_free(fixture.result);
    body=body_new();JsonObject *o=json_node_get_object(body);gchar *key=g_uuid_string_random();json_object_set_string_member(o,"name","UI gerçek playlist");json_object_set_string_member(o,"idempotency_key",key);g_free(key);JsonNode *playlist=request("/v1/workflows/desktop-playlists/create",body,&error);g_assert_no_error(error);g_assert_nonnull(playlist);json_node_free(body);
    media=json_object_get_object_member(json_node_get_object(detail),"media");JsonObject *version=json_object_get_object_member(json_node_get_object(detail),"version"),*metadata=json_object_new();
    const gchar *names[]={"title","artist","album","genre"};for(guint i=0;i<G_N_ELEMENTS(names);++i)json_object_set_string_member(metadata,names[i],radios_json_string(media,names[i],""));json_object_set_string_member(metadata,"key","");json_object_set_string_member(metadata,"location","");json_object_set_string_member(metadata,"comment","");json_object_set_int_member(metadata,"duration_ms",radios_json_int(version,"duration_ms",0));json_object_set_double_member(metadata,"bpm",0);
    json_object_set_string_member(metadata,"media_id",radios_json_string(media,"_id",""));json_object_set_string_member(metadata,"version_id",radios_json_string(version,"_id",""));json_object_set_string_member(metadata,"media_file_id",radios_json_string(version,"media_file_id",""));json_object_set_string_member(metadata,"media_sha256",radios_json_string(version,"sha256",""));
    body=body_new();o=json_node_get_object(body);json_object_set_string_member(o,"playlist_id",radios_json_string(json_node_get_object(playlist),"_id",""));json_object_set_int_member(o,"expected_revision",radios_json_int(json_node_get_object(playlist),"_rev",0));json_object_set_object_member(o,"metadata",metadata);key=g_uuid_string_random();json_object_set_string_member(o,"idempotency_key",key);g_free(key);JsonNode *updated=request("/v1/workflows/desktop-playlists/entry-save",body,&error);g_assert_no_error(error);g_assert_nonnull(updated);json_node_free(body);json_node_free(playlist);json_node_free(detail);return updated;
}
static void assert_no_audio_start(Window *window)
{
    for (guint deck=0;deck<32;++deck) {
        RadioSAudioUiSnapshot audio={0};g_assert_true(radios_audio_ui_get_snapshot(window->ctx,deck,&audio));
        g_assert_cmpint(audio.state,!=,RADIOS_AUDIO_UI_PLAYING);g_assert_cmpint(audio.state,!=,RADIOS_AUDIO_UI_PAUSED);
        RadioSPlayoutSnapshot run={0};
        if(radios_player_workspace_snapshot(window->ctx->players,deck,&run))
            g_assert_true(run.state==RADIOS_PLAYOUT_IDLE||run.state==RADIOS_PLAYOUT_FINISHED);
    }
}
static void test_visible_scope_assignment_and_playlist(void)
{
    reset_private_definitions();JsonNode *shared=weekly("UI Küme şeması","UI küme event"),*individual=weekly("UI Tek şema","UI tek event");
    Window window=window_open();GtkWidget *panel=gui_find(GTK_WIDGET(window.ctx->parent),"automation-workspace");g_assert_nonnull(panel);
    RadioSPlayerTopology topology=*radios_player_workspace_topology(window.ctx->players);gchar cluster[37];GError *error=NULL;
    g_assert_true(radios_player_topology_add_cluster(&topology,"UI Müzik kümesi",cluster,&error));g_assert_true(radios_player_topology_set_cluster(&topology,0,cluster,&error));g_assert_true(radios_player_topology_set_cluster(&topology,2,cluster,&error));
    g_assert_true(radios_player_topology_set_autodj(&topology,0,RADIOS_AUTODJ_TRANSITIONS,2,&error));
    guint independent=0;g_assert_true(radios_player_topology_add_player(&topology,"",&independent,&error));g_assert_true(radios_player_workspace_replace(window.ctx->players,&topology,&error));g_assert_no_error(error);save_ack(&window);
    RadioSAutomationScope group={.kind=RADIOS_AUTOMATION_SCOPE_CLUSTER},player={.kind=RADIOS_AUTOMATION_SCOPE_PLAYER,.deck=0},aggregate={.kind=RADIOS_AUTOMATION_SCOPE_UNCLUSTERED};g_strlcpy(group.cluster_id,cluster,sizeof group.cluster_id);
    gui_select(panel,&group);gui_click(panel,"automation-workspace-reload");GtkWidget *schema=gui_find(panel,"automation-workspace-schema");
    const gchar *shared_id=radios_json_string(json_node_get_object(shared),"_id",""),*individual_id=radios_json_string(json_node_get_object(individual),"_id","");
    ComboWait shared_wait={schema,shared_id};wait_until(choice_ready,&shared_wait,"real saved weekly schema picker");g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(schema),shared_id));gui_click(panel,"automation-workspace-assign");
    AssignmentWait assigned={&window,group,shared_id};wait_until(assignment_ready,&assigned,"cluster schema assignment exact double read");save_ack(&window);
    gui_select(panel,&player);run_for(150);g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(gui_find(panel,"automation-workspace-schema-name"))),"Kümeden"));
    ComboWait override_wait={schema,individual_id};wait_until(choice_ready,&override_wait,"individual schema option");g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(schema),individual_id));gui_click(panel,"automation-workspace-assign");
    assigned.scope=player;assigned.id=individual_id;wait_until(assignment_ready,&assigned,"individual override assignment");save_ack(&window);gui_click(panel,"automation-workspace-unassign");assigned.id=shared_id;wait_until(assignment_ready,&assigned,"restore cluster inheritance");save_ack(&window);
    gui_select(panel,&group);GtkWidget *mode=gui_find(panel,"automation-workspace-autodj-mode"),*target=gui_find(panel,"automation-workspace-autodj-target");
    g_assert_false(gtk_widget_get_visible(gui_find(panel,"automation-workspace-independent-settings")));
    g_assert_cmpint(radios_player_workspace_topology(window.ctx->players)->players[0].autodj_mode,==,RADIOS_AUTODJ_TRANSITIONS);g_assert_cmpint(radios_player_workspace_topology(window.ctx->players)->players[0].autodj_target,==,2);
    JsonNode *fixture_playlist=playlist_fixture(&window);json_node_free(fixture_playlist);gui_click(panel,"automation-workspace-reload");
    JsonNode *body=body_new();JsonNode *listed=request("/v1/workflows/desktop-playlists/list",body,&error);json_node_free(body);g_assert_no_error(error);g_assert_nonnull(listed);
    JsonArray *playlists=json_object_get_array_member(json_node_get_object(listed),"playlists");g_assert_cmpuint(json_array_get_length(playlists),>,0);const gchar *playlist_id=radios_json_string(json_array_get_object_element(playlists,0),"_id","");
    GtkWidget *playlist=gui_find(panel,"automation-workspace-playlist");ComboWait playlist_wait={playlist,playlist_id};wait_until(choice_ready,&playlist_wait,"real authenticated playlist picker");g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(playlist),playlist_id));
    gui_click(panel,"automation-workspace-load");QueueWait queue_wait={&window,group};wait_until(queue_loaded_ui,&queue_wait,"cluster playlist persists formal event queue");
    g_assert_cmpint(radios_player_workspace_topology(window.ctx->players)->players[0].autodj_target,==,2);assert_no_audio_start(&window);
    gui_select(panel,&aggregate);g_assert_false(gtk_widget_get_sensitive(gui_find(panel,"automation-workspace-assign")));g_assert_false(gtk_widget_get_sensitive(gui_find(panel,"automation-workspace-edit")));
    g_assert_true(gtk_widget_get_visible(gui_find(panel,"automation-workspace-independent-settings")));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(mode),"2"));g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(target),"4"));save_ack(&window);
    g_assert_cmpint(radios_player_workspace_topology(window.ctx->players)->players[independent].autodj_mode,==,RADIOS_AUTODJ_TRANSITIONS);
    g_assert_cmpint(radios_player_workspace_topology(window.ctx->players)->players[independent].autodj_target,==,4);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(playlist),playlist_id));gui_click(panel,"automation-workspace-load");queue_wait.scope=aggregate;wait_until(queue_loaded_ui,&queue_wait,"Kümesizler owns its loaded playlist independently");assert_no_audio_start(&window);
    JsonObject *queue=NULL;g_assert_true(radios_player_workspace_get_programme(window.ctx->players,&aggregate,&queue,&error));g_assert_no_error(error);g_assert_false(json_object_has_member(queue,"autodj_players"));g_assert_false(json_object_has_member(queue,"queue_player"));json_object_unref(queue);
    gui_select(panel,&player);g_assert_true(gtk_widget_get_sensitive(gui_find(panel,"automation-workspace-edit")));window_close(&window);json_node_free(listed);json_node_free(shared);json_node_free(individual);
}
int main(int argc,char **argv)
{
    g_assert_cmpint(argc,>=,2);gchar *root=g_strdup(argv[1]);for(gint i=1;i<argc;++i)argv[i]=argv[i+1];--argc;g_test_init(&argc,&argv,NULL);gtk_init(&argc,&argv);
    const gchar *main_sink=g_getenv("RADIOS_TEST_MAIN_SINK"),*cue_sink=g_getenv("RADIOS_TEST_CUE_SINK");g_assert_nonnull(main_sink);g_assert_nonnull(cue_sink);g_assert_true(g_str_has_prefix(main_sink,"radios_gui_"));g_assert_true(g_str_has_prefix(cue_sink,"radios_gui_"));
    GError *error=NULL;g_assert_true(radios_config_load(root,NULL,&config,&error));g_assert_no_error(error);g_free(root);
    application=gtk_application_new("org.radios.AutomationWorkspaceAcceptance",G_APPLICATION_NON_UNIQUE);g_assert_true(g_application_register(G_APPLICATION(application),NULL,&error));g_assert_no_error(error);
    reader=radios_data_client_new(&config,&error);g_assert_no_error(error);g_assert_nonnull(reader);g_test_add_func("/automation-workspace/real-store-schema-inherit-playlist-separate",test_visible_scope_assignment_and_playlist);
    gint result=g_test_run();radios_data_client_unref(reader);g_object_unref(application);radios_config_clear(&config);return result;
}
