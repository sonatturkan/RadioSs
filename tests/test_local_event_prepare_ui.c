#define _POSIX_C_SOURCE 200809L
#include <glib.h>
static int local_prepare_test_run(void);
#define main radios_local_prepare_fixture_main
#define g_test_run local_prepare_test_run
#include "test_playout_ui.c"
#undef g_test_run
#undef main
#include "radios/local_event_prepare.h"
#include "radios/header_navigation.h"

typedef struct { JsonNode *config; GError *error; guint calls, destroyed; } PreparedLocal;
static void prepared_local(RadioSViewContext *ctx,GtkWidget *owner,JsonObject *programme,const GError *error,gpointer value)
{
    (void)ctx;(void)owner;PreparedLocal *result=value;++result->calls;
    if(programme){result->config=json_node_new(JSON_NODE_OBJECT);json_node_take_object(result->config,radios_programme_clone_object(programme));}
    if(error)result->error=g_error_copy(error);
}
static void prepared_local_free(gpointer value){++((PreparedLocal *)value)->destroyed;}
static gboolean prepared_local_done(gpointer value){return ((PreparedLocal *)value)->destroyed>0;}
static JsonObject *local_recipe(const gchar *title)
{
    JsonObject *config=json_object_new();
    json_object_set_int_member(config,"schema",1);json_object_set_string_member(config,"title",title);
    json_object_set_string_member(config,"kind","program");json_object_set_array_member(config,"sources",json_array_new());
    return config;
}
static void local_recipe_add(JsonObject *config,const gchar *type,JsonNode *record)
{
    JsonObject *source=json_object_new();gchar *id=g_uuid_string_random();
    json_object_set_string_member(source,"id",id);g_free(id);
    json_object_set_string_member(source,"type",type);
    json_object_set_string_member(source,"source_id",radios_json_string(json_node_get_object(record),"_id",""));
    json_array_add_object_element(json_object_get_array_member(config,"sources"),source);
}
static void unchanged_document(const gchar *kind,const gchar *name,JsonNode *expected)
{
    JsonNode *actual=document_read(kind,name);g_assert_true(json_node_equal(actual,expected));json_node_free(actual);
}
static void test_readonly_materialization_sources_and_cancel(void)
{
    Window window=window_open();
    JsonNode *live=media_import(&window,"Çalmaya devam eden bağımsız sıra",700,.2,"music");
    JsonNode *schema_song=media_import(&window,"Yerel şema müziği",1700,.2,"music");
    JsonNode *local_song=media_import(&window,"Yerel playlist müziği",2200,.2,"music");
    seed_programme(&window,live,NULL);
    JsonNode *playlist=playlist_create("Yerel liste",local_song,NULL);
    JsonNode *schema_list=playlist_create("Şemanın bağımsız listesi",schema_song,NULL);
    gint64 future=(g_get_real_time()/G_TIME_SPAN_DAY+1)*G_TIME_SPAN_DAY+5*G_TIME_SPAN_MINUTE;
    JsonNode *weekly_config=weekly_pcm_configuration(schema_list,NULL,future,1,"Yerel şema kaynağı");
    JsonNode *weekly=document_save("weekly_programme","Kaynak haftalık",json_node_get_object(weekly_config));
    JsonObject *calendar_config=json_object_get_object_member(json_node_get_object(weekly_config),"calendar");
    JsonNode *calendar=document_save("automation_calendar","Kaynak takvim",calendar_config);
    JsonNode *before_doc=document_read("programme","Mevcut program");
    click(window.root,"programme-play");run_wait(&window,RADIOS_PLAYOUT_PLAYING,0,100);
    RadioSAudioRunSnapshot before={0};g_assert_true(radios_playout_ui_navigation_snapshot(window.ctx,&before));
    goffset at=capture_offset("MAIN");
    JsonObject *recipe=local_recipe("Bağımsız geniş Event");
    local_recipe_add(recipe,"playlist",playlist);local_recipe_add(recipe,"weekly_programme",weekly);
    local_recipe_add(recipe,"automation_calendar",calendar);local_recipe_add(recipe,"playlist",playlist);
    PreparedLocal result={0};
    GCancellable *cancel=radios_local_event_prepare_async(window.ctx,window.root,recipe,prepared_local,&result,prepared_local_free);
    g_assert_nonnull(cancel);g_object_unref(cancel);
    wait_until(prepared_local_done,&result,"all three reference types materialized without a live write");
    g_assert_no_error(result.error);g_assert_cmpuint(result.calls,==,1);g_assert_cmpuint(result.destroyed,==,1);g_assert_nonnull(result.config);
    RadioSProgrammeTree *tree=radios_programme_tree_new(json_node_get_object(result.config));
    g_assert_cmpuint(tree->blocks->len,==,1);guint count=radios_programme_tree_leaf_count(tree);g_assert_cmpuint(count,>=,4);
    g_assert_cmpstr(radios_json_string(radios_programme_tree_leaf_at(tree,0,NULL),"title",""),==,"Yerel playlist müziği");
    g_assert_cmpstr(radios_json_string(radios_programme_tree_leaf_at(tree,count-1,NULL),"title",""),==,"Yerel playlist müziği");
    GHashTable *ids=g_hash_table_new(g_str_hash,g_str_equal);
    for(guint i=0;i<count;++i){JsonObject *leaf=radios_programme_tree_leaf_at(tree,i,NULL);const gchar *id=radios_json_string(leaf,"id","");
        g_assert_true(g_uuid_string_is_valid(id));g_assert_false(g_hash_table_contains(ids,id));g_hash_table_add(ids,(gpointer)id);
        g_assert_cmpstr(radios_json_string(leaf,"start_time",""),==,"");}
    g_hash_table_unref(ids);radios_programme_tree_free(tree);
    JsonNode *body=body_new();json_object_set_string_member(json_node_get_object(body),"kind","programme");
    json_object_set_member(json_node_get_object(body),"config",deep_copy(result.config));
    JsonNode *verified=request("/v1/workflows/desktop-playout/prepare",body);json_node_free(body);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(verified),"tracks")),==,count);json_node_free(verified);
    unchanged_document("programme","Mevcut program",before_doc);
    unchanged_document("weekly_programme","Kaynak haftalık",weekly);unchanged_document("automation_calendar","Kaynak takvim",calendar);
    assert_saved_playlist_unchanged(playlist);assert_saved_playlist_unchanged(schema_list);
    RadioSAudioRunSnapshot after={0};g_assert_true(radios_playout_ui_navigation_snapshot(window.ctx,&after));
    g_assert_cmpuint(after.epoch,==,before.epoch);g_assert_cmpuint(after.count,==,before.count);
    g_assert_cmpuint(after.actual_current_frame,>,before.actual_current_frame);
    run_for(120);g_assert_cmpfloat(tone_energy("MAIN",at,700),>,.04);

    GtkWidget *owner=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);g_object_ref_sink(owner);PreparedLocal abandoned={0};
    cancel=radios_local_event_prepare_async(window.ctx,owner,recipe,prepared_local,&abandoned,prepared_local_free);
    g_assert_nonnull(cancel);gtk_widget_destroy(owner);g_object_unref(owner);g_object_unref(cancel);
    wait_until(prepared_local_done,&abandoned,"destroying the owning view cancels local source preparation");
    g_assert_cmpuint(abandoned.calls,==,0);g_assert_cmpuint(abandoned.destroyed,==,1);
    stop_run(&window);json_node_free(result.config);json_object_unref(recipe);
    json_node_free(before_doc);json_node_free(calendar);json_node_free(weekly);json_node_free(weekly_config);
    json_node_free(playlist);json_node_free(schema_list);json_node_free(live);json_node_free(schema_song);json_node_free(local_song);window_close(&window);
}
typedef struct {GtkWidget *dialog;const gchar *id;} InsertChoice;
static gboolean insert_choice_ready(gpointer value)
{
    InsertChoice *choice=value;
    return gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(choice->dialog,"local-event-source")),choice->id);
}
typedef struct {GtkWidget *dialog;const gchar *contains;} InsertStatus;
static gboolean insert_status_matches(gpointer value)
{
    InsertStatus *expected=value;GtkWidget *status=find(expected->dialog,"local-event-status");
    return status && strstr(gtk_label_get_text(GTK_LABEL(status)),expected->contains);
}
static void test_explicit_insert_fences_live_queue(void)
{
    Window window=window_open();
    JsonNode *live=media_import(&window,"Canlı ana sıra",810,.2,"music");
    JsonNode *other=media_import(&window,"Yerel eklenecek parça",1810,.2,"music");
    JsonNode *playlist=playlist_create("Yerel ekleme listesi",other,other);
    seed_programme(&window,live,NULL);click(window.root,"programme-play");run_wait(&window,RADIOS_PLAYOUT_PLAYING,0,100);
    RadioSAudioRunSnapshot before={0};g_assert_true(radios_playout_ui_navigation_snapshot(window.ctx,&before));
    click(window.root,"programme-local-event");GtkWidget *dialog=dialog_find("local-event-dialog");g_assert_nonnull(dialog);g_object_ref(dialog);
    gtk_entry_set_text(GTK_ENTRY(find(dialog,"local-event-name")),"Elle eklenen yerel Event");
    combo(dialog,"local-event-kind","sweeper");combo(dialog,"local-event-source-type","playlist");
    InsertChoice choice={dialog,radios_json_string(json_node_get_object(playlist),"_id","")};
    wait_until(insert_choice_ready,&choice,"integration button exposes local playlist");click(dialog,"local-event-add");
    click(dialog,"local-event-insert");InsertStatus done={dialog,"Yerel Event seçili yayın sırasına eklendi."};
    wait_until(insert_status_matches,&done,"explicit local insertion acknowledged in the same builder");
    JsonNode *saved=document_read("programme","Mevcut program");JsonObject *config_object=json_object_get_object_member(json_node_get_object(saved),"config");
    RadioSProgrammeTree *tree=radios_programme_tree_new(config_object);g_assert_cmpuint(tree->blocks->len,==,2);
    g_assert_cmpstr(radios_json_string(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks,1))->block,"title",""),==,"Elle eklenen yerel Event");
    g_assert_cmpstr(radios_json_string(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks,1))->block,"kind",""),==,"sweeper");
    g_assert_cmpuint(radios_programme_tree_leaf_count(tree),==,3);radios_programme_tree_free(tree);json_node_free(saved);
    RadioSAudioRunSnapshot after={0};g_assert_true(radios_playout_ui_navigation_snapshot(window.ctx,&after));
    g_assert_cmpuint(after.epoch,==,before.epoch);g_assert_cmpuint(after.count,==,before.count);g_assert_cmpuint(after.index,==,before.index);
    assert_saved_playlist_unchanged(playlist);
    /* The worker cannot finish on the GTK thread before this next operator
     * edit: a concurrent target change must reject the eventual append. */
    click(dialog,"local-event-insert");click(window.root,"programme-add-announcement");
    InsertStatus changed={dialog,"Seçili yayın sırası değişti"};
    wait_until(insert_status_matches,&changed,"a new operator edit fences a pending local Event append");
    saved=document_read("programme","Mevcut program");tree=radios_programme_tree_new(json_object_get_object_member(json_node_get_object(saved),"config"));
    g_assert_cmpuint(tree->blocks->len,==,3);g_assert_cmpuint(radios_programme_tree_leaf_count(tree),==,3);
    radios_programme_tree_free(tree);json_node_free(saved);
    g_assert_true(radios_playout_ui_navigation_snapshot(window.ctx,&after));g_assert_cmpuint(after.epoch,==,before.epoch);
    gtk_widget_destroy(dialog);g_object_unref(dialog);stop_run(&window);
    json_node_free(playlist);json_node_free(live);json_node_free(other);window_close(&window);
}
static void test_mixed_local_recipe_groups_until_station_break(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL);
    JsonNode *music = media_import(&window, "Yerel kesintisiz müzik", 900, .2, "music");
    JsonNode *advert = media_import(&window, "Yerel reklam arası", 1400, .2, "advertisement");
    JsonNode *sweeper = media_import(&window, "Yerel sweeper arası", 1900, .2, "sweeper");
    JsonNode *playlists[] = {playlist_create("Yerel müzik listesi A", music, music),
        playlist_create("Yerel müzik listesi B", music, music), playlist_create("Yerel reklam listesi", advert, NULL),
        playlist_create("Yerel sweeper listesi", sweeper, NULL)};
    JsonObject *recipe = local_recipe("Reklam ve sweeper sınırları olan yerel Event");
    const guint sources[] = {0, 1, 2, 0, 3, 1};
    for (guint i = 0; i < G_N_ELEMENTS(sources); ++i) local_recipe_add(recipe, "playlist", playlists[sources[i]]);
    JsonObject *playback = json_object_new(); json_object_set_int_member(playback, "player", 0);
    json_object_set_string_member(playback, "next_cluster_id", ""); json_object_set_object_member(recipe, "playback", playback);
    PreparedLocal result = {0};
    GCancellable *cancel = radios_local_event_prepare_async(window.ctx, window.root, recipe, prepared_local, &result, prepared_local_free);
    g_assert_nonnull(cancel); g_object_unref(cancel);
    wait_until(prepared_local_done, &result, "mixed local sources keep contiguous music until station breaks");
    g_assert_no_error(result.error); g_assert_nonnull(result.config);
    RadioSProgrammeTree *tree = radios_programme_tree_new(json_node_get_object(result.config));
    const guint owners[] = {0, 0, 0, 0, 1, 2, 2, 3, 4, 4};
    const gchar *kinds[] = {"music", "advertisement", "music", "sweeper", "music"};
    g_assert_cmpuint(tree->blocks->len, ==, G_N_ELEMENTS(kinds));
    g_assert_cmpuint(radios_programme_tree_leaf_count(tree), ==, G_N_ELEMENTS(owners));
    for (guint i = 0; i < G_N_ELEMENTS(owners); ++i) {
        guint owner = G_MAXUINT; JsonObject *leaf = radios_programme_tree_leaf_at(tree, i, &owner);
        g_assert_cmpuint(owner, ==, owners[i]);
        RadioSProgrammeBlock *block = g_ptr_array_index(tree->blocks, owner);
        g_assert_cmpstr(radios_json_string(block->block, "kind", ""), ==, kinds[owner]);
        g_assert_cmpstr(radios_json_string(leaf, "block_id", ""), ==, radios_json_string(block->block, "id", ""));
    }
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "kind", "programme");
    json_object_set_member(json_node_get_object(body), "config", deep_copy(result.config));
    JsonNode *verified = request("/v1/workflows/desktop-playout/prepare", body); json_node_free(body);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(verified), "tracks")), ==, G_N_ELEMENTS(owners));
    json_node_free(verified);
    click(window.root, "programme-local-event");
    GtkWidget *dialog = dialog_find("local-event-dialog"); g_assert_nonnull(dialog); g_object_ref(dialog);
    gtk_entry_set_text(GTK_ENTRY(find(dialog, "local-event-name")), "GUI üzerinden sınırları korunan yerel Event");
    combo(dialog, "local-event-kind", "program"); combo(dialog, "local-event-source-type", "playlist");
    for (guint i = 0; i < G_N_ELEMENTS(sources); ++i) {
        InsertChoice choice = {dialog, radios_json_string(json_node_get_object(playlists[sources[i]]), "_id", "")};
        wait_until(insert_choice_ready, &choice, "mixed recipe sources are offered in the real builder");
        click(dialog, "local-event-add");
    }
    click(dialog, "local-event-insert");
    InsertStatus inserted = {dialog, "Yerel Event seçili yayın sırasına eklendi."};
    wait_until(insert_status_matches, &inserted, "GUI insertion retains materialized typed event boundaries");
    JsonNode *saved = document_read("programme", "Mevcut program");
    RadioSProgrammeTree *stored = radios_programme_tree_new(json_object_get_object_member(json_node_get_object(saved), "config"));
    g_assert_cmpuint(stored->blocks->len, ==, G_N_ELEMENTS(kinds));
    g_assert_cmpuint(radios_programme_tree_leaf_count(stored), ==, G_N_ELEMENTS(owners));
    for (guint i = 0; i < G_N_ELEMENTS(owners); ++i) {
        guint owner = G_MAXUINT; radios_programme_tree_leaf_at(stored, i, &owner); g_assert_cmpuint(owner, ==, owners[i]);
        g_assert_cmpstr(radios_json_string(((RadioSProgrammeBlock *)g_ptr_array_index(stored->blocks, owner))->block, "kind", ""), ==, kinds[owner]);
    }
    radios_programme_tree_free(stored); json_node_free(saved); gtk_widget_destroy(dialog); g_object_unref(dialog);
    for (guint i = 0; i < G_N_ELEMENTS(playlists); ++i) { assert_saved_playlist_unchanged(playlists[i]); json_node_free(playlists[i]); }
    radios_programme_tree_free(tree); json_node_free(result.config); json_object_unref(recipe);
    json_node_free(music); json_node_free(advert); json_node_free(sweeper); window_close(&window);
}
static void test_mixed_flow_manual_wait_has_no_active_finished_event(void)
{
    Window window = window_open();
    JsonNode *music = media_import(&window, "Bekleme öncesi biten event", 750, .2, "music");
    JsonNode *sweeper = media_import(&window, "Elle geçilecek sonraki event", 1750, .2, "sweeper");
    seed_programme(&window, music, sweeper);
    combo_index(window.root, "programme-transition-mode", 3);
    GtkWidget *queue = find(window.root, "programme-queue-table"); GError *error = NULL;
    click(window.root, "programme-play");
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100);
    run_wait(&window, RADIOS_PLAYOUT_MANUAL_WAIT, 0, 0);
    GtkListBox *list = g_object_get_data(G_OBJECT(queue), "list-box");
    for (guint phase = 0; phase < 3; ++phase) {
        run_for(220); RadioSPlayoutSnapshot status = {0};
        g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &status));
        g_assert_cmpint(status.state, ==, RADIOS_PLAYOUT_MANUAL_WAIT);
        GtkStyleContext *style = gtk_widget_get_style_context(GTK_WIDGET(gtk_list_box_get_row_at_index(list, 0)));
        g_assert_false(gtk_style_context_has_class(style, "is-playing"));
        /* Manual wait retains its deck for Previous/replay. It has not
         * emitted a terminal receipt; explicit Next is INTERRUPTED. */
        g_assert_false(gtk_style_context_has_class(style, "is-event-completed"));
    }
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    require_call(radios_playout_ui_action(window.ctx, RADIOS_PLAYOUT_NEXT, &error), &error);
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 100); run_for(220);
    GtkStyleContext *finished = gtk_widget_get_style_context(GTK_WIDGET(gtk_list_box_get_row_at_index(list, 0)));
    g_assert_false(gtk_style_context_has_class(finished, "is-playing"));
    /* Explicit navigation is not a natural completion, even when the
     * retained manual source had reached its end. Do not fabricate one. */
    g_assert_false(gtk_style_context_has_class(finished, "is-event-completed"));
    GdkRGBA *initial = NULL;
    gtk_style_context_get(finished, gtk_style_context_get_state(finished), "background-color", &initial, NULL);
    g_assert_nonnull(initial);
    for (guint phase = 0; phase < 2; ++phase) {
        run_for(530); GdkRGBA *current = NULL;
        gtk_style_context_get(finished, gtk_style_context_get_state(finished), "background-color", &current, NULL);
        g_assert_nonnull(current);
        g_assert_false(gtk_style_context_has_class(finished, "is-playing"));
        g_assert_false(gtk_style_context_has_class(finished, "is-alert-white"));
        g_assert_false(gtk_style_context_has_class(finished, "is-event-completed"));
        g_assert_cmpfloat_with_epsilon(current->red, initial->red, .005);
        g_assert_cmpfloat_with_epsilon(current->green, initial->green, .005);
        g_assert_cmpfloat_with_epsilon(current->blue, initial->blue, .005);
        g_assert_cmpfloat_with_epsilon(current->alpha, initial->alpha, .005);
        gdk_rgba_free(current);
    }
    gdk_rgba_free(initial);
    stop_run(&window); json_node_free(music); json_node_free(sweeper); window_close(&window);
}
/* Kılavuz §06/§07 (akım B, A'nın L tuşu için): a live event's automation
 * schema prepared from now, read where the players' schema list reads it
 * (main calendar first, then active weekly programmes). Read-only, from the
 * schema's own source, owner-cancellable; non-schemas and bad ids refused. */
static gchar *utc_date(gint64 us)
{
    GDateTime *day = g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC); gchar *date = g_date_time_format(day, "%Y-%m-%d");
    g_date_time_unref(day); return date;
}
static JsonObject *schema_copy(JsonArray *library, const gchar *name, JsonNode *playlist)
{
    JsonNode *node = deep_copy(json_array_get_element(library, 0)); JsonObject *schema = json_node_get_object(node);
    gchar *id = g_uuid_string_random(), *source_id = g_uuid_string_random();
    json_object_set_string_member(schema, "id", id); json_object_set_string_member(schema, "name", name);
    json_object_set_boolean_member(json_object_get_object_member(schema, "settings"), "automation_schema", TRUE);
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(schema, "sources"), 0);
    json_object_set_string_member(source, "id", source_id); json_object_set_int_member(source, "interval_minutes", 0);
    json_object_set_string_member(source, "playlist_id", radios_json_string(json_node_get_object(playlist), "_id", ""));
    json_array_add_element(library, node); g_free(id); g_free(source_id); return schema;
}
static void schema_programme_check(PreparedLocal *result, const gchar *name, const gchar *song, gint64 before, gint64 after)
{
    g_assert_no_error(result->error); g_assert_cmpuint(result->calls, ==, 1); g_assert_cmpuint(result->destroyed, ==, 1);
    g_assert_nonnull(result->config); JsonObject *config = json_node_get_object(result->config);
    g_assert_cmpstr(radios_json_string(config, "title", ""), ==, name);
    gchar *note = g_strdup_printf("Live event şeması: %s", name);
    g_assert_cmpstr(radios_json_string(config, "note", ""), ==, note); g_free(note);
    gchar *first = utc_date(before), *last = utc_date(after); const gchar *date = radios_json_string(config, "date", "");
    g_assert_true(!strcmp(date, first) || !strcmp(date, last)); g_free(first); g_free(last);
    RadioSProgrammeTree *tree = radios_programme_tree_new(config); guint count = radios_programme_tree_leaf_count(tree);
    g_assert_cmpuint(tree->blocks->len, >=, 1); g_assert_cmpuint(count, >=, 1);
    GHashTable *ids = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < count; ++i) {
        guint owner = G_MAXUINT; JsonObject *leaf = radios_programme_tree_leaf_at(tree, i, &owner); const gchar *id = radios_json_string(leaf, "id", "");
        g_assert_true(g_uuid_string_is_valid(id)); g_assert_false(g_hash_table_contains(ids, id)); g_hash_table_add(ids, (gpointer)id);
        g_assert_cmpuint(owner, <, tree->blocks->len);
        g_assert_cmpstr(radios_json_string(leaf, "block_id", ""), ==, radios_json_string(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, owner))->block, "id", ""));
        g_assert_cmpstr(radios_json_string(leaf, "title", ""), ==, song);
    }
    g_test_message("%s: %u events in %u blocks on %s", name, count, tree->blocks->len, date);
    g_hash_table_unref(ids); radios_programme_tree_free(tree);
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "kind", "programme");
    json_object_set_member(json_node_get_object(body), "config", deep_copy(result->config));
    JsonNode *verified = request("/v1/workflows/desktop-playout/prepare", body); json_node_free(body);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(verified), "tracks")), ==, count);
    json_node_free(verified);
}
static void test_schema_window_from_now(void)
{
    Window window = window_open();
    JsonNode *schema_song = media_import(&window, "Live event şeması müziği", 1300, .2, "music");
    JsonNode *weekly_song = media_import(&window, "Haftalık live event şeması müziği", 1900, .2, "music");
    JsonNode *calendar_song = media_import(&window, "Takvimin kendi müziği", 2300, .2, "music");
    JsonNode *schema_list = playlist_create("Live event şeması listesi", schema_song, NULL);
    JsonNode *weekly_list = playlist_create("Haftalık şema listesi", weekly_song, NULL);
    JsonNode *calendar_list = playlist_create("Takvim listesi", calendar_song, NULL);
    gint64 tomorrow = (g_get_real_time() / G_TIME_SPAN_DAY + 1) * G_TIME_SPAN_DAY; gchar *date = utc_date(tomorrow);
    JsonNode *calendar = calendar_schedule_config(date, radios_json_string(json_node_get_object(calendar_list), "_id", ""), 0); g_free(date);
    JsonArray *library = json_object_get_array_member(json_node_get_object(calendar), "rotation_library");
    gchar *plain_id = g_strdup(radios_json_string(json_array_get_object_element(library, 0), "id", ""));
    gchar *schema_id = g_strdup(radios_json_string(schema_copy(library, "Akşam live event şeması", schema_list), "id", ""));
    JsonNode *saved = document_save("automation_calendar", "main", json_node_get_object(calendar));
    /* A schema saved only in an active weekly programme is found as well. */
    JsonNode *weekly_config = weekly_pcm_configuration(calendar_list, NULL, tomorrow + 5 * G_TIME_SPAN_MINUTE, 1, "Haftalık kaynak");
    JsonArray *weekly_library = json_object_get_array_member(json_object_get_object_member(json_node_get_object(weekly_config), "calendar"), "rotation_library");
    gchar *weekly_id = g_strdup(radios_json_string(schema_copy(weekly_library, "Haftalık live event şeması", weekly_list), "id", ""));
    JsonNode *weekly = document_save("weekly_programme", "Şemalı haftalık", json_node_get_object(weekly_config));
    RadioSPlayoutSnapshot idle = {0}; g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &idle));

    PreparedLocal result = {0}; gint64 before = g_get_real_time();
    GCancellable *cancel = radios_local_event_prepare_schema_async(window.ctx, window.root, schema_id, 2 * 60000, prepared_local, &result, prepared_local_free);
    g_assert_nonnull(cancel); g_object_unref(cancel);
    wait_until(prepared_local_done, &result, "the live event's main-calendar schema is prepared from now");
    schema_programme_check(&result, "Akşam live event şeması", "Live event şeması müziği", before, g_get_real_time());

    PreparedLocal from_weekly = {0}; before = g_get_real_time();
    cancel = radios_local_event_prepare_schema_async(window.ctx, window.root, weekly_id, 0, prepared_local, &from_weekly, prepared_local_free);
    g_assert_nonnull(cancel); g_object_unref(cancel);
    wait_until(prepared_local_done, &from_weekly, "a schema saved in an active weekly programme is prepared from now");
    schema_programme_check(&from_weekly, "Haftalık live event şeması", "Haftalık live event şeması müziği", before, g_get_real_time());

    /* An ordinary rotation is not a schema; a malformed id is refused at once. */
    PreparedLocal plain = {0};
    cancel = radios_local_event_prepare_schema_async(window.ctx, window.root, plain_id, 0, prepared_local, &plain, prepared_local_free);
    g_assert_nonnull(cancel); g_object_unref(cancel);
    wait_until(prepared_local_done, &plain, "a non-schema rotation is refused");
    g_assert_cmpuint(plain.calls, ==, 1); g_assert_null(plain.config); g_assert_nonnull(plain.error);
    g_assert_cmpstr(plain.error->message, ==, "Eventin otomasyon şeması bulunamadı ya da artık şema değil.");
    PreparedLocal malformed = {0};
    g_assert_null(radios_local_event_prepare_schema_async(window.ctx, window.root, "Not-a-schema", 0, prepared_local, &malformed, prepared_local_free));
    g_assert_cmpuint(malformed.calls, ==, 0); g_assert_cmpuint(malformed.destroyed, ==, 1);

    GtkWidget *owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(owner); PreparedLocal abandoned = {0};
    cancel = radios_local_event_prepare_schema_async(window.ctx, owner, schema_id, 0, prepared_local, &abandoned, prepared_local_free);
    g_assert_nonnull(cancel); gtk_widget_destroy(owner); g_object_unref(owner); g_object_unref(cancel);
    wait_until(prepared_local_done, &abandoned, "destroying the owning view cancels schema preparation");
    g_assert_cmpuint(abandoned.calls, ==, 0); g_assert_cmpuint(abandoned.destroyed, ==, 1);

    /* Nothing was saved or started. */
    unchanged_document("automation_calendar", "main", saved); unchanged_document("weekly_programme", "Şemalı haftalık", weekly);
    assert_saved_playlist_unchanged(schema_list); assert_saved_playlist_unchanged(weekly_list); assert_saved_playlist_unchanged(calendar_list);
    RadioSPlayoutSnapshot still = {0}; g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &still));
    g_assert_cmpint(still.state, ==, idle.state); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    json_node_free(result.config); json_node_free(from_weekly.config); g_clear_error(&plain.error);
    g_free(plain_id); g_free(schema_id); g_free(weekly_id);
    json_node_free(saved); json_node_free(calendar); json_node_free(weekly); json_node_free(weekly_config);
    json_node_free(schema_list); json_node_free(weekly_list); json_node_free(calendar_list);
    json_node_free(schema_song); json_node_free(weekly_song); json_node_free(calendar_song); window_close(&window);
}

static int local_prepare_test_run(void)
{
    g_test_add_func("/native-playout/local-event-materialize-readonly-and-cancel-PCM",test_readonly_materialization_sources_and_cancel);
    g_test_add_func("/native-playout/local-event-mixed-sources-station-breaks-PCM",test_mixed_local_recipe_groups_until_station_break);
    g_test_add_func("/native-playout/local-event-mixed-flow-manual-wait-stable-PCM",test_mixed_flow_manual_wait_has_no_active_finished_event);
    g_test_add_func("/native-playout/local-event-insert-fences-live-queue-PCM",test_explicit_insert_fences_live_queue);
    g_test_add_func("/native-playout/local-event-schema-from-now-readonly-PCM",test_schema_window_from_now);
    return g_test_run();
}
int main(int argc,char **argv){return radios_local_prepare_fixture_main(argc,argv);}
