#define _POSIX_C_SOURCE 200809L
#include <glib.h>
static int event_queue_run(void);
#define main radios_event_queue_fixture_main
#define g_test_run event_queue_run
#include "test_playout_ui.c"
#undef g_test_run
#undef main
#include "radios/player_binding.h"
#include "radios/player_loader.h"

static gboolean event_blocks_saved(gpointer value)
{
    Window *window=value;
    GtkWidget *count=find(window->root,"programme-queue-count");
    return count && !g_strcmp0(gtk_label_get_text(GTK_LABEL(count)),"3 event · 4 içerik") &&
        radios_data_document_revision(radios_broadcast_programme_document(window->ctx))>0;
}
static GtkWidget *event_queue_meta(Window *window, guint row)
{
    GtkWidget *queue=find(window->root,"programme-queue-table");
    GtkListBox *list=g_object_get_data(G_OBJECT(queue),"list-box");
    GtkWidget *item=GTK_WIDGET(gtk_list_box_get_row_at_index(list,(gint)row));
    return item?find(item,"programme-row-meta"):NULL;
}
static gboolean event_queue_has_label(GtkWidget *root, const gchar *text)
{
    if (GTK_IS_LABEL(root) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(root)),text)) return TRUE;
    if (!GTK_IS_CONTAINER(root)) return FALSE;
    GList *children=gtk_container_get_children(GTK_CONTAINER(root));gboolean found=FALSE;
    for (GList *item=children;item&&!found;item=item->next) found=event_queue_has_label(item->data,text);
    g_list_free(children);return found;
}
/* Kılavuz 18.10 s.13-14: the accepted jingle is no longer a prefix of the
 * music subtitle; it is the content's own 1.1 sub-row (name and relation),
 * directly below the song it plays after. Finished rows may leave the list,
 * so the song is found by its title. */
static GtkListBoxRow *event_queue_jingle_row(Window *window)
{
    GtkWidget *queue=find(window->root,"programme-queue-table");
    GtkListBox *list=g_object_get_data(G_OBJECT(queue),"list-box");
    for (gint index=0;;++index) {
        GtkListBoxRow *item=gtk_list_box_get_row_at_index(list,index);
        if (!item) return NULL;
        if (radios_programme_queue_row_level(item)!=RADIOS_PROGRAMME_ROW_MAIN || !event_queue_has_label(GTK_WIDGET(item),"Blok müzik B")) continue;
        GtkListBoxRow *sub=gtk_list_box_get_row_at_index(list,index+1);
        return sub && radios_programme_queue_row_level(sub)==RADIOS_PROGRAMME_ROW_SUB && event_queue_has_label(GTK_WIDGET(sub),"Blok istasyon ID") ? sub : NULL;
    }
}
static gboolean event_queue_jingle_hint(gpointer value)
{
    GtkListBoxRow *sub=event_queue_jingle_row(value);
    GtkWidget *meta=sub?find(GTK_WIDGET(sub),"programme-row-meta"):NULL;
    const gchar *number=sub?radios_programme_queue_row_number(sub):NULL;
    return GTK_IS_LABEL(meta) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(meta)),"Araya jingle") &&
        number && g_str_has_suffix(number,".1");
}
static void test_mixed_import_blocks_real_queue_hints_and_player_order(void)
{
    Window window=window_open();seed_programme(&window,NULL,NULL);
    JsonNode *media[4];
    const gchar *titles[]={"Blok müzik A","Blok müzik B","Blok istasyon ID","Blok müzik C"};
    const gchar *kinds[]={"music","music","jingle","music"};
    const gdouble tones[]={700,1400,2200,1700};
    JsonArray *tracks=json_array_new(),*inputs=json_array_new();
    for(guint i=0;i<4;++i){
        media[i]=media_import(&window,titles[i],tones[i],.2,kinds[i]);
        JsonObject *row=json_object_new();
        json_object_set_member(row,"detail",deep_copy(media[i]));
        json_object_set_object_member(row,"metadata",radios_programme_clone_object(json_object_get_object_member(json_node_get_object(media[i]),"media")));
        json_array_add_object_element(tracks,row);
        JsonObject *input=json_object_new();bind(input,json_node_get_object(media[i]));
        json_object_set_string_member(input,"title",titles[i]);json_object_set_string_member(input,"kind",kinds[i]);
        json_object_set_int_member(input,"duration_ms",radios_json_int(json_object_get_object_member(json_node_get_object(media[i]),"version"),"duration_ms",0));
        JsonObject *normalized=radios_player_input_new(input);g_assert_nonnull(normalized);
        g_assert_cmpstr(radios_json_string(normalized,"kind",""),==,kinds[i]);
        json_object_unref(input);json_array_add_object_element(inputs,normalized);
    }
    GError *error=NULL;
    require_call(radios_broadcast_append_event(window.ctx,tracks,NULL,&error),&error);
    wait_until(event_blocks_saved,&window,"mixed music/jingle import saved as three formal blocks");
    JsonNode *saved=document_read("programme","Mevcut program");
    JsonObject *cfg=json_object_get_object_member(json_node_get_object(saved),"config");
    RadioSProgrammeTree *tree=radios_programme_tree_new(cfg);
    g_assert_cmpuint(tree->blocks->len,==,3);
    g_assert_cmpuint(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks,0))->leaves->len,==,2);
    g_assert_cmpstr(radios_json_string(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks,1))->block,"kind",""),==,"jingle");
    gchar *music_block=g_strdup(radios_json_string(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks,0))->block,"id",""));
    GtkWidget *queue=find(window.root,"programme-queue-table");
    radios_programme_queue_set_expanded(queue,music_block,TRUE);
    click(window.root,"programme-play");
    run_wait(&window,RADIOS_PLAYOUT_PLAYING,0,100);
    wait_until(event_queue_jingle_hint,&window,"actual accepted next jingle appears as the 1.1 sub-row of its song");
    GtkWidget *first=event_queue_meta(&window,0);
    g_assert_true(GTK_IS_LABEL(first));g_assert_null(strstr(gtk_label_get_text(GTK_LABEL(first)),"jingle:"));
    goffset at=capture_offset("MAIN");run_for(180);g_assert_cmpfloat(tone_energy("MAIN",at,700),>,.03);
    click(window.root,"programme-stop");run_for(250);
    g_assert_null(strstr(gtk_label_get_text(GTK_LABEL(event_queue_meta(&window,1))),"jingle:"));
    g_assert_null(event_queue_jingle_row(&window));
    JsonNode *unchanged=document_read("programme","Mevcut program");
    g_assert_true(json_node_equal(saved,unchanged));json_node_free(unchanged);
    radios_programme_tree_free(tree);g_free(music_block);

    /* The same order created directly on a player retains the same three
     * formal event boundaries when accepted by Semi AutoDJ. */
    wait_until(scope_workspace_ready,window.ctx->players,"direct player event workspace");
    guint deck=99;
    require_call(radios_broadcast_player_register_playlist(window.ctx,inputs,"Elle kurulan blok sırası",0,0,&deck,&error),&error);
    g_assert_cmpuint(deck,==,0);
    combo(window.root,"deck-0-autodj-mode","1");
    ScopeReady ready={window.ctx->players,0};wait_until(scope_workspace_execution_ready,&ready,"Semi AutoDJ definitions acknowledged");
    click(window.root,"deck-0-autodj-start");
    RadioSAudioEngine *engine=radios_audio_ui_engine_acquire(window.ctx);
    player_run_wait(engine,0,RADIOS_AUDIO_RUN_PLAYING,0,4800);
    RadioSAutomationScope scope={.kind=RADIOS_AUTOMATION_SCOPE_PLAYER,.deck=0};
    JsonNode *projection=radios_player_workspace_projection(window.ctx->players,&scope,&error);g_assert_no_error(error);g_assert_nonnull(projection);
    JsonArray *orders=json_object_get_array_member(json_node_get_object(projection),"autodj_orders");
    g_assert_cmpuint(json_array_get_length(orders),==,1);
    JsonArray *entries=json_object_get_array_member(json_array_get_object_element(orders,0),"entries");
    g_assert_cmpuint(json_array_get_length(entries),==,4);
    const gchar *a=radios_json_string(json_array_get_object_element(entries,0),"block_id","");
    const gchar *b=radios_json_string(json_array_get_object_element(entries,1),"block_id","");
    const gchar *j=radios_json_string(json_array_get_object_element(entries,2),"block_id","");
    const gchar *c=radios_json_string(json_array_get_object_element(entries,3),"block_id","");
    g_assert_cmpstr(a,!=,"");g_assert_cmpstr(a,==,b);g_assert_cmpstr(a,!=,j);g_assert_cmpstr(j,!=,c);g_assert_cmpstr(a,!=,c);
    RadioSPlayerScopeExecution *execution=radios_player_scope_execution_get(window.ctx);
    require_call(radios_player_scope_execution_action(execution,&scope,RADIOS_PLAYOUT_STOP,&error),&error);
    json_node_free(projection);radios_audio_engine_unref(engine);json_node_free(saved);
    json_array_unref(inputs);json_array_unref(tracks);
    for(guint i=0;i<4;++i)json_node_free(media[i]);
    window_close(&window);
}
static int event_queue_run(void)
{
    g_test_add_func("/event-block-queue/mixed-import-player-order-accepted-jingle-PCM",test_mixed_import_blocks_real_queue_hints_and_player_order);
    return g_test_run();
}
int main(int argc,char **argv){return radios_event_queue_fixture_main(argc,argv);}
