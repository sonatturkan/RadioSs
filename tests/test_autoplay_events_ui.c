#define _POSIX_C_SOURCE 200809L
/* Actual authenticated programme preparation and native runner observations;
 * the UI must follow accepted ownership without modifying stored queues. */
#include <glib.h>
static int autoplay_events_run(void);
#define main radios_autoplay_events_fixture_main
#define g_test_run autoplay_events_run
#include "test_playout_ui.c"
#undef g_test_run
#undef main
#include "radios/automation_workspace_ui.h"

static gboolean events_contains(GtkTreeModel *model, GtkTreeIter *parent, const gchar *wanted)
{
    GtkTreeIter row; gboolean valid = gtk_tree_model_iter_children(model, &row, parent);
    while (valid) {
        gchar *title = NULL; gtk_tree_model_get(model, &row, 0, &title, -1);
        gboolean found = !g_strcmp0(title, wanted); g_free(title);
        if (found || events_contains(model, &row, wanted)) return TRUE;
        valid = gtk_tree_model_iter_next(model, &row);
    }
    return FALSE;
}
static gboolean events_has(Window *window, const gchar *title)
{
    GtkWidget *tree = find(window->root, "automation-workspace-events"); g_assert_true(GTK_IS_TREE_VIEW(tree));
    return events_contains(gtk_tree_view_get_model(GTK_TREE_VIEW(tree)), NULL, title);
}
static gboolean events_state(GtkTreeModel *model, GtkTreeIter *parent, const gchar *wanted, const gchar *state)
{
    GtkTreeIter row; gboolean valid = gtk_tree_model_iter_children(model, &row, parent);
    while (valid) {
        gchar *title = NULL, *value = NULL; gtk_tree_model_get(model, &row, 0, &title, 4, &value, -1);
        gboolean found = !g_strcmp0(title, wanted) && !g_strcmp0(value, state); g_free(title); g_free(value);
        if (found || events_state(model, &row, wanted, state)) return TRUE;
        valid = gtk_tree_model_iter_next(model, &row);
    }
    return FALSE;
}
typedef struct { Window *window; const gchar *present, *absent, *state; } EventsWait;
static gboolean events_matches(gpointer data)
{
    EventsWait *expected = data;
    GtkWidget *tree = find(expected->window->root, "automation-workspace-events");
    return (!expected->present || events_has(expected->window, expected->present)) &&
        (!expected->absent || !events_has(expected->window, expected->absent)) &&
        (!expected->state || events_state(gtk_tree_view_get_model(GTK_TREE_VIEW(tree)), NULL, expected->present, expected->state));
}
static void events_wait(Window *window, const gchar *present, const gchar *absent, const gchar *state)
{
    EventsWait expected = {window, present, absent, state};
    g_test_message("Eventler wait present '%s', absent '%s', state '%s'",present?present:"",absent?absent:"",state?state:"");
    /* Do not call refresh: exercise the production automatic timer. */
    gint64 deadline=g_get_monotonic_time()+5*G_USEC_PER_SEC;
    while(!events_matches(&expected) && g_get_monotonic_time()<deadline)pump();
    if(!events_matches(&expected)) {
        GtkWidget *tree=find(window->root,"automation-workspace-events");GtkTreeModel *model=gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
        GtkTreeIter row;gboolean valid=gtk_tree_model_get_iter_first(model,&row);
        while(valid){gchar *title=NULL,*value=NULL;gtk_tree_model_get(model,&row,0,&title,4,&value,-1);g_test_message("Eventler root '%s' state '%s'",title,value);g_free(title);g_free(value);valid=gtk_tree_model_iter_next(model,&row);}
        g_error("Eventler did not automatically reach '%s' / '%s' without '%s'",present?present:"",state?state:"",absent?absent:"");
    }
}
static void events_readonly(Window *window)
{
    const gchar *names[] = {"automation-workspace-events", "automation-workspace-orders", "automation-workspace-calendar"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *tree = find(window->root, names[i]); g_assert_true(GTK_IS_TREE_VIEW(tree));
        g_assert_false(gtk_tree_view_get_reorderable(GTK_TREE_VIEW(tree)));
        g_assert_null(gtk_drag_source_get_target_list(tree)); g_assert_null(gtk_drag_dest_get_target_list(tree));
        GList *columns = gtk_tree_view_get_columns(GTK_TREE_VIEW(tree));
        for (GList *column = columns; column; column = column->next) {
            GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(column->data));
            for (GList *cell = cells; cell; cell = cell->next) {
                gboolean editable = TRUE; g_object_get(cell->data, "editable", &editable, NULL); g_assert_false(editable);
            }
            g_list_free(cells);
        }
        g_list_free(columns);
    }
}
static void events_select(Window *window, const RadioSAutomationScope *scope)
{
    GtkWidget *tree = find(window->root, "live-player-definitions"); g_assert_nonnull(tree);
    /* Use the actual user selection path so both visible selectors are
     * mirrored; the panel's public select API deliberately suppresses its
     * parent callback and is only the receiving side of that mirror. */
    radios_player_tree_ui_refresh(tree);g_assert_true(radios_player_tree_ui_select(tree,scope));
}
typedef struct { Window *window; RadioSAutomationScope scope; } EventsQueueReady;
static gboolean events_queue_ready(gpointer data)
{
    EventsQueueReady *ready=data; JsonObject *programme=NULL; GError *error=NULL;
    gboolean ok=radios_player_workspace_get_programme(ready->window->ctx->players,&ready->scope,&programme,&error);
    if(!ok && g_error_matches(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA) && !g_strcmp0(error->message,"Seçili kapsamın yayın sırası boş."))ok=TRUE;
    if(!ok)g_assert_error(error,G_IO_ERROR,G_IO_ERROR_PENDING);
    g_clear_error(&error);g_clear_pointer(&programme,json_object_unref);return ok;
}
static void events_store(Window *window, const RadioSAutomationScope *scope, const gchar *title, JsonNode *media)
{
    EventsQueueReady ready={window,*scope};wait_until(events_queue_ready,&ready,"stored queue baseline loaded before edit");
    JsonNode *config_node = programme_config(media, NULL); JsonObject *configuration = json_node_get_object(config_node);
    json_object_set_string_member(configuration, "title", title);
    GError *error = NULL; require_call(radios_player_workspace_set_programme(window->ctx->players, scope, configuration, &error), &error);
    json_node_free(config_node);
}
static JsonNode *events_prepare_three(JsonNode *a, JsonNode *b, JsonNode *c)
{
    JsonNode *configuration = programme_config(a, b), *third = programme_config(c, NULL);
    JsonObject *config_object = json_node_get_object(configuration);
    json_object_set_string_member(config_object, "title", "Gerçek yeni event akışı");
    JsonArray *entries = json_object_get_array_member(config_object, "events");
    JsonArray *last = json_object_get_array_member(json_node_get_object(third), "events");
    json_array_add_element(entries, deep_copy(json_array_get_element(last, 0)));
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "programme"); json_object_set_member(object, "config", deep_copy(configuration));
    JsonNode *prepared = request("/v1/workflows/desktop-playout/prepare", body);
    json_node_free(body); json_node_free(configuration); json_node_free(third); return prepared;
}
static void events_stop(Window *window, guint source)
{
    GError *error = NULL; require_call(radios_player_workspace_action(window->ctx->players, source, RADIOS_PLAYOUT_STOP, &error), &error);
}
static void events_reset_definitions(void)
{
    RadioSPlayerTopology topology;radios_player_topology_default(&topology,6);
    JsonObject *configuration=radios_player_topology_to_json(&topology);
    JsonNode *saved=document_save("live_players","main",configuration);
    json_node_free(saved);json_object_unref(configuration);
}
static void test_events_actual_next_epoch_stop_scope_and_readonly(void)
{
    events_reset_definitions();Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "Eventler player workspace ready");
    JsonNode *a = media_import(&window, "Event A ilk kabul", 600, .2, "music");
    JsonNode *b = media_import(&window, "Event B sıradaki", 1700, .2, "music");
    JsonNode *c = media_import(&window, "Event C yeni epoch", 2500, .2, "music");
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players); gchar cluster[37]; GError *error = NULL;
    require_call(radios_player_topology_add_cluster(&topology, "Eventler gerçek küme", cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 0, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 2, cluster, &error), &error);
    require_call(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_TRANSITIONS, 2, &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    RadioSAutomationScope group = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER}, source = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0}, target = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 2};
    g_strlcpy(group.cluster_id, cluster, sizeof group.cluster_id);
    events_store(&window, &group, "Eski küme taslağı", c); events_store(&window, &source, "Eski kaynak taslağı", c);
    events_select(&window, &group); events_wait(&window, "Eski küme taslağı", NULL, NULL); events_readonly(&window);
    ScopeReady ready = {window.ctx->players, 0}; wait_until(scope_workspace_execution_ready, &ready, "Eventler private scope history and saved definitions ready");
    RadioSAutomationBinding binding; require_call(radios_automation_scope_resolve(radios_player_workspace_topology(window.ctx->players), &group, &binding, &error), &error);
    JsonNode *prepared = events_prepare_three(a, b, c);
    require_call(radios_player_workspace_start_prepared(window.ctx->players, &binding, json_node_get_object(prepared), RADIOS_AUTODJ_TRANSITIONS, &error), &error);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    RadioSAudioRunSnapshot first = player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    events_wait(&window, "Event A ilk kabul", "Eski küme taslağı", "Çalıyor");
    g_assert_true(events_has(&window, "Event B sıradaki")); g_assert_true(events_has(&window, "Event C yeni epoch")); g_assert_false(events_has(&window, "Eski kaynak taslağı"));
    require_call(radios_player_workspace_action(window.ctx->players, 0, RADIOS_PLAYOUT_NEXT, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 1, 4800);
    events_wait(&window, "Event B sıradaki", "Event A ilk kabul", "Çalıyor");
    events_select(&window, &source); events_wait(&window, "Event C yeni epoch", "Event B sıradaki", "Sırada");
    g_assert_false(events_has(&window, "Event A ilk kabul")); g_assert_false(events_has(&window, "Eski kaynak taslağı"));
    events_select(&window, &target); events_wait(&window, "Event B sıradaki", "Event C yeni epoch", "Çalıyor");
    require_call(radios_player_workspace_action(window.ctx->players, 0, RADIOS_PLAYOUT_PAUSE, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PAUSED, 1, 0); events_wait(&window, "Event B sıradaki", NULL, "Duraklatıldı");
    events_stop(&window, 0); events_select(&window, &group); events_wait(&window, "Eski küme taslağı", "Event B sıradaki", NULL);
    JsonNode *replacement = scope_authenticated_programme(c, NULL);
    require_call(radios_player_workspace_start_prepared(window.ctx->players, &binding, json_node_get_object(replacement), RADIOS_AUTODJ_TRANSITIONS, &error), &error);
    RadioSAudioRunSnapshot second = player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800); g_assert_cmpuint(second.epoch, !=, first.epoch);
    events_wait(&window, "Event C yeni epoch", "Event B sıradaki", "Çalıyor"); g_assert_false(events_has(&window, "Event A ilk kabul"));
    events_stop(&window, 0); events_wait(&window, "Eski küme taslağı", "Gerçek yeni event akışı", NULL);
    JsonObject *stored = NULL; require_call(radios_player_workspace_get_programme(window.ctx->players, &group, &stored, &error), &error);
    g_assert_cmpstr(radios_json_string(stored, "title", ""), ==, "Eski küme taslağı"); json_object_unref(stored);
    events_readonly(&window); radios_audio_engine_unref(engine); json_node_free(replacement); json_node_free(prepared);
    json_node_free(a); json_node_free(b); json_node_free(c); window_close(&window);
}
static void test_events_aggregate_keeps_unrelated_queue_and_removes_live_traces(void)
{
    events_reset_definitions();Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "aggregate Eventler baseline");
    JsonNode *a = media_import(&window, "Kümesiz canlı A", 700, .2, "music");
    JsonNode *b = media_import(&window, "Kümesiz canlı B", 1800, .2, "music");
    RadioSAutomationScope aggregate = {.kind = RADIOS_AUTOMATION_SCOPE_UNCLUSTERED}, other = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 4};
    events_store(&window, &other, "Bağımsız diğer oynatıcının sırası", b);
    ScopeReady ready = {window.ctx->players, 0}; wait_until(scope_workspace_execution_ready, &ready, "aggregate scope history baseline");
    RadioSAutomationScope source = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0}; RadioSAutomationBinding binding; GError *error = NULL;
    require_call(radios_automation_scope_resolve(radios_player_workspace_topology(window.ctx->players), &source, &binding, &error), &error);
    JsonNode *prepared = scope_authenticated_programme(a, b);
    require_call(radios_player_workspace_start_prepared(window.ctx->players, &binding, json_node_get_object(prepared), RADIOS_AUTODJ_SEMI, &error), &error);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    events_select(&window, &aggregate); events_wait(&window, "Kümesiz canlı A", NULL, "Çalıyor");
    g_assert_true(events_has(&window, "Bağımsız diğer oynatıcının sırası"));
    events_select(&window, &other); events_wait(&window, "Bağımsız diğer oynatıcının sırası", "Kümesiz canlı A", NULL);
    events_select(&window, &aggregate); events_stop(&window, 0); events_wait(&window, "Bağımsız diğer oynatıcının sırası", "Kümesiz canlı A", NULL);
    events_readonly(&window); radios_audio_engine_unref(engine); json_node_free(prepared); json_node_free(a); json_node_free(b); window_close(&window);
}
static void test_events_general_owned_pool_filters_without_global_control(void)
{
    events_reset_definitions();Window window=window_open();wait_until(scope_workspace_ready,window.ctx->players,"general Eventler workspace baseline");
    JsonNode *a=media_import(&window,"Genel event A",600,.2,"music");
    JsonNode *b=media_import(&window,"Genel event B",1700,.2,"music");
    JsonNode *c=media_import(&window,"Genel event C yönü bekleniyor",2500,.2,"music");
    RadioSPlayerTopology topology=*radios_player_workspace_topology(window.ctx->players);gchar cluster[37];GError *error=NULL;
    require_call(radios_player_topology_add_cluster(&topology,"Genel birinci küme",cluster,&error),&error);
    require_call(radios_player_topology_set_cluster(&topology,0,cluster,&error),&error);
    require_call(radios_player_workspace_replace(window.ctx->players,&topology,&error),&error);
    RadioSAutomationScope group={.kind=RADIOS_AUTOMATION_SCOPE_CLUSTER},aggregate={.kind=RADIOS_AUTOMATION_SCOPE_UNCLUSTERED},child={.kind=RADIOS_AUTOMATION_SCOPE_PLAYER,.deck=0};
    g_strlcpy(group.cluster_id,cluster,sizeof group.cluster_id);
    events_store(&window,&child,"Genel sahibi varken eski iz gösterme",c);
    JsonNode *prepared=events_prepare_three(a,b,c);
    require_call(radios_playout_ui_start_prepared(window.ctx,window.root,json_node_get_object(prepared),0,&error),&error);
    RadioSPlayoutSnapshot first=run_wait(&window,RADIOS_PLAYOUT_PLAYING,0,100);g_assert_cmpint(first.deck,==,0);g_assert_cmpint(first.next_deck,==,1);
    events_select(&window,&group);events_wait(&window,"Genel event A","Genel event B","Çalıyor");
    g_assert_false(events_has(&window,"Genel event C yönü bekleniyor"));g_assert_false(events_has(&window,"Genel sahibi varken eski iz gösterme"));
    events_select(&window,&aggregate);events_wait(&window,"Genel event B","Genel event A","Sırada");
    g_assert_false(events_has(&window,"Genel event C yönü bekleniyor"));
    /* The projection's runtime_source must not promote the global owner to
     * a scoped lane. A scope Stop remains unable to stop general MAIN. */
    RadioSPlayerScopeExecution *execution=radios_player_scope_execution_get(window.ctx);
    require_call(radios_player_scope_execution_action(execution,&child,RADIOS_PLAYOUT_STOP,&error),&error);
    run_wait(&window,RADIOS_PLAYOUT_PLAYING,0,100);
    require_call(radios_playout_ui_action(window.ctx,RADIOS_PLAYOUT_NEXT,&error),&error);
    run_wait(&window,RADIOS_PLAYOUT_PLAYING,1,100);events_wait(&window,"Genel event B","Genel event A","Çalıyor");
    events_select(&window,&group);events_wait(&window,NULL,"Genel event B",NULL);
    require_call(radios_playout_ui_action(window.ctx,RADIOS_PLAYOUT_STOP,&error),&error);
    events_select(&window,&child);events_wait(&window,"Genel sahibi varken eski iz gösterme","Genel event B",NULL);
    events_readonly(&window);json_node_free(prepared);json_node_free(a);json_node_free(b);json_node_free(c);window_close(&window);
}
static void test_events_future_schema_wait_cancel_clears_events(void)
{
    events_reset_definitions();Window window=window_open();wait_until(scope_workspace_ready,window.ctx->players,"future Eventler workspace baseline");
    JsonNode *tone=media_import(&window,"Gelecekteki şemanın medyası",700,.2,"music");
    JsonNode *playlist=playlist_create("Gelecekteki event playlisti",tone,NULL);
    gint64 minute=60*G_USEC_PER_SEC,tomorrow=(g_get_real_time()/G_TIME_SPAN_DAY+1)*G_TIME_SPAN_DAY+5*minute;
    JsonNode *configuration=weekly_pcm_configuration(playlist,NULL,tomorrow,1,"Gelecekte beklenen yayın eventi");
    JsonNode *saved=document_save("weekly_programme","Eventler gelecek şema",json_node_get_object(configuration));
    RadioSAutomationScope scope={.kind=RADIOS_AUTOMATION_SCOPE_PLAYER,.deck=0};GError *error=NULL;
    require_call(radios_player_workspace_assign(window.ctx->players,&scope,radios_json_string(json_node_get_object(saved),"_id",""),&error),&error);
    events_store(&window,&scope,"Beklerken saklanan eski taslak",tone);events_select(&window,&scope);
    events_wait(&window,"Beklerken saklanan eski taslak",NULL,NULL);
    RadioSPlayerScopeExecution *execution=radios_player_scope_execution_get(window.ctx);ScopeExecutionResult armed={0};
    require_call(radios_player_scope_execution_play_schema(execution,&scope,RADIOS_AUTODJ_SEMI,execution_result,&armed,execution_result_destroy,&error),&error);
    execution_wait(&armed);events_wait(&window,"Gelecekte beklenen yayın eventi","Beklerken saklanan eski taslak","Program saatini bekliyor");
    g_assert_false(events_has(&window,"Gelecekteki şemanın medyası"));
    require_call(radios_player_scope_execution_action(execution,&scope,RADIOS_PLAYOUT_STOP,&error),&error);
    events_wait(&window,"Beklerken saklanan eski taslak","Gelecekte beklenen yayın eventi",NULL);
    events_readonly(&window);weekly_record_unchanged(saved);assert_saved_playlist_unchanged(playlist);
    json_node_free(saved);json_node_free(configuration);json_node_free(playlist);json_node_free(tone);window_close(&window);
}
static guint events_group_count(GtkTreeModel *model, GtkTreeIter *parent,
    const gchar *caption, gint children)
{
    guint count = 0; GtkTreeIter row;
    gboolean valid = gtk_tree_model_iter_children(model, &row, parent);
    while (valid) {
        gchar *title = NULL, *state = NULL;
        gtk_tree_model_get(model, &row, 0, &title, 4, &state, -1);
        if (!g_strcmp0(title, caption) && !g_strcmp0(state, "Event") &&
            (children < 0 || gtk_tree_model_iter_n_children(model, &row) == children)) ++count;
        g_free(title); g_free(state);
        count += events_group_count(model, &row, caption, children);
        valid = gtk_tree_model_iter_next(model, &row);
    }
    return count;
}
static JsonNode *events_prepare_distinct_blocks(JsonNode *a, JsonNode *b, JsonNode *jingle, JsonNode *c)
{
    JsonNode *configuration = programme_config(a, b), *additional = programme_config(jingle, c);
    JsonObject *config_object = json_node_get_object(configuration);
    json_object_set_string_member(config_object, "title", "Kabul edilen müzik ve jingle blokları");
    JsonArray *entries = json_object_get_array_member(config_object, "events");
    JsonArray *more = json_object_get_array_member(json_node_get_object(additional), "events");
    for (guint i = 0; i < json_array_get_length(more); ++i)
        json_array_add_element(entries, deep_copy(json_array_get_element(more, i)));
    JsonArray *blocks = json_array_new();
    const gchar *kinds[] = {"music", "jingle", "music"};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        gchar *id = g_uuid_string_random(); JsonObject *block = json_object_new();
        json_object_set_string_member(block, "id", id); json_object_set_string_member(block, "kind", kinds[i]);
        json_object_set_string_member(block, "title", i == 1 ? "Jingle bloğu" : "Aynı adlı müzik bloğu");
        json_object_set_string_member(block, "start_time", ""); json_object_set_string_member(block, "note", "");
        json_array_add_object_element(blocks, block);
        guint first = i == 0 ? 0 : i + 1, last = i == 0 ? 1 : i + 1;
        for (guint j = first; j <= last; ++j)
            json_object_set_string_member(json_array_get_object_element(entries, j), "block_id", id);
        g_free(id);
    }
    json_object_set_array_member(config_object, "blocks", blocks);
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "programme"); json_object_set_member(object, "config", deep_copy(configuration));
    JsonNode *prepared = request("/v1/workflows/desktop-playout/prepare", body);
    json_node_free(body); json_node_free(configuration); json_node_free(additional); return prepared;
}
static void test_events_distinct_block_identity_and_planned_jingle(void)
{
    events_reset_definitions(); Window window = window_open();
    wait_until(scope_workspace_ready, window.ctx->players, "block identity workspace ready");
    JsonNode *a = media_import(&window, "İlk blok A", 600, .2, "music");
    JsonNode *b = media_import(&window, "İlk blok B", 1700, .2, "music");
    JsonNode *jingle = media_import(&window, "Bloklar arasındaki jingle", 2500, .2, "jingle");
    JsonNode *c = media_import(&window, "İkinci blok C", 2900, .2, "music");
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    ScopeReady ready = {window.ctx->players, 0};
    wait_until(scope_workspace_execution_ready, &ready, "block identity accepted history baseline");
    RadioSAutomationBinding binding; GError *error = NULL;
    require_call(radios_automation_scope_resolve(radios_player_workspace_topology(window.ctx->players), &scope, &binding, &error), &error);
    JsonNode *prepared = events_prepare_distinct_blocks(a, b, jingle, c);
    require_call(radios_player_workspace_start_prepared(window.ctx->players, &binding, json_node_get_object(prepared), RADIOS_AUTODJ_SEMI, &error), &error);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    events_select(&window, &scope); events_wait(&window, "İlk blok A", NULL, "Çalıyor");
    GtkWidget *tree = find(window.root, "automation-workspace-events");
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
    g_assert_cmpuint(events_group_count(model, NULL, "Aynı adlı müzik bloğu", -1), ==, 2);
    g_assert_cmpuint(events_group_count(model, NULL, "Aynı adlı müzik bloğu", 2), ==, 1);
    g_assert_cmpuint(events_group_count(model, NULL, "Aynı adlı müzik bloğu", 1), ==, 1);
    g_assert_cmpuint(events_group_count(model, NULL, "Jingle bloğu", 1), ==, 1);
    const gchar *hint = "Jingle: Bloklar arasındaki jingle · Şarkı sonrası";
    g_assert_true(events_has(&window, hint));
    GtkWidget *orders = find(window.root, "automation-workspace-orders");
    g_assert_true(events_contains(gtk_tree_view_get_model(GTK_TREE_VIEW(orders)), NULL, hint));
    for (guint i = 1; i <= 2; ++i) {
        require_call(radios_player_workspace_action(window.ctx->players, 0, RADIOS_PLAYOUT_NEXT, &error), &error);
        player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, i, 0);
    }
    events_wait(&window, "Bloklar arasındaki jingle", "İlk blok B", "Çalıyor");
    g_assert_false(events_has(&window, hint));
    g_assert_cmpuint(events_group_count(model, NULL, "Aynı adlı müzik bloğu", -1), ==, 1);
    events_stop(&window, 0); events_wait(&window, NULL, "Bloklar arasındaki jingle", NULL);
    events_readonly(&window); radios_audio_engine_unref(engine); json_node_free(prepared);
    json_node_free(a); json_node_free(b); json_node_free(jingle); json_node_free(c); window_close(&window);
}
static int autoplay_events_run(void)
{
    g_test_add_func("/autoplay-events/distinct-block-identity-and-planned-jingle", test_events_distinct_block_identity_and_planned_jingle);
    g_test_add_func("/autoplay-events/actual-next-replacement-stop-scope-readonly", test_events_actual_next_epoch_stop_scope_and_readonly);
    g_test_add_func("/autoplay-events/aggregate-retains-independent-queue", test_events_aggregate_keeps_unrelated_queue_and_removes_live_traces);
    g_test_add_func("/autoplay-events/general-owned-pool-filter-readonly", test_events_general_owned_pool_filters_without_global_control);
    g_test_add_func("/autoplay-events/future-schema-wait-cancel-clears-events", test_events_future_schema_wait_cancel_clears_events);
    return g_test_run();
}
int main(int argc, char **argv) { return radios_autoplay_events_fixture_main(argc, argv); }
